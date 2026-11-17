// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>
#include <algorithm>
#include <array>
#include "bridge_patience.h"

using namespace supple;
using namespace supple::engine;

TEST_CASE("engine bridge failures retain scope, structured errors and restart ownership") {
    host::setBridgeExecutable(BRIDGE_PATH);
    // Mode 2 hangs: the product's deadline kills it (ADR-0013).
    const test::BridgeDeadline productDefault(2000);
    for (double mode : {1.0, 2.0}) {
        CAPTURE(mode);
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        model::Track track;
        track.name = "Bridge fault";
        track.parent = model::kMaster;
        const auto trackId = edit.create(track);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(trackId); });
        std::array<model::Id, 3> ids;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            model::FxInstance fx;
            fx.plugin.id = i == 1 ? "com.suppleaudio.test.crash" : "com.suppleaudio.test.gain";
            fx.isolation = i == 2 ? model::Isolation::Dedicated : model::Isolation::SharedBridge;
            ids[i] = edit.create(fx);
            edit.modify<model::Track>(trackId, [&](auto& item) { item.chain.push_back(ids[i]); });
        }
        auto committed = edit.commit();
        REQUIRE(committed);
        const auto& session = *committed;
        InstancePool pool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
            const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
            host::ScanOptions options;
            options.forceInProcess = true;
            auto plugins = host::scan(std::span(&fixture, 1), options);
            if (!plugins) return plugins.error();
            for (const auto& plugin : *plugins)
                if (plugin.id == ref.id) return plugin;
            return Error("engine.plugin_missing", "bridge fixture not found");
        });
        std::array<host::Instance*, 3> instances{};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto loaded = pool.get(ids[i], *session.fx.find(ids[i]), session);
            REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
            instances[i] = *loaded;
            REQUIRE(instances[i]->isolated());
            REQUIRE(instances[i]->activate(48000, 1, 64));
            REQUIRE(instances[i]->startProcessing());
        }
        const auto sharedPid = instances[0]->bridgePid();
        const auto dedicatedPid = instances[2]->bridgePid();
        CHECK(sharedPid == instances[1]->bridgePid());
        CHECK(sharedPid != dedicatedPid);
        std::array<float, 64> input{}, left{}, right{};
        input.fill(0.25f);
        const float* inputs[]{input.data(), input.data()};
        float* outputs[]{left.data(), right.data()};
        host::ProcessBlock block;
        block.inputs = inputs;
        block.outputs = outputs;
        block.inputChannels = block.outputChannels = 2;
        block.frames = 64;
        REQUIRE(instances[1]->setParam(1, mode));
        REQUIRE(instances[1]->process(block));
        CHECK(instances[0]->failed());
        CHECK(instances[1]->failed());
        CHECK_FALSE(instances[2]->failed());
        CHECK(std::all_of(left.begin(), left.end(), [](float value) { return value == 0; }));
        if (mode == 2)
            CHECK(instances[1]->failure().find("stopped responding") != std::string::npos);
        const auto failures = pool.failedErrors();
        REQUIRE(failures.size() == 2);
        for (const auto& [id, error] : failures) {
            CHECK((id == ids[0] || id == ids[1]));
            CHECK(error.code == "engine.bridge_failed");
            CHECK(error.path == "/fx/" + id.str());
            CHECK_FALSE(error.hint.empty());
        }
        REQUIRE(instances[2]->process(block));
        CHECK(left == input);
        for (auto* instance : instances) {
            instance->stopProcessing();
            instance->deactivate();
        }
        for (std::size_t i = 0; i < 2; ++i) pool.restart(ids[i]);
        for (std::size_t i = 0; i < 2; ++i) {
            auto restarted = pool.get(ids[i], *session.fx.find(ids[i]), session);
            REQUIRE(restarted);
            CHECK_FALSE((*restarted)->failed());
            CHECK((*restarted)->bridgePid() != sharedPid);
        }
        CHECK(pool.find(ids[0])->bridgePid() == pool.find(ids[1])->bridgePid());
        CHECK(pool.find(ids[2])->bridgePid() == dedicatedPid);
        CHECK(pool.failedErrors().empty());
    }
}

TEST_CASE("inherit resolves third-party plug-ins to the default isolation, stock ones in process") {
    // ADR-0081 commit 7: the plug-in isolation preference. An fx that names
    // its isolation keeps it; inherit takes the default as its instance is
    // made, except a stock plug-in (stockPluginDir()), which stays in process.
    host::setBridgeExecutable(BRIDGE_PATH);
    model::EditTxn edit(model::Session::create({.console9 = false}), 1);
    model::Track track;
    track.name = "Default isolation";
    track.parent = model::kMaster;
    const auto trackId = edit.create(track);
    edit.modify<model::Track>(model::kMaster,
                              [&](auto& master) { master.children.push_back(trackId); });
    model::FxInstance inherit;
    inherit.plugin.id = "com.suppleaudio.test.gain";
    const model::Id inheritId = edit.create(inherit);
    model::FxInstance named = inherit;
    named.isolation = model::Isolation::InProcess;
    const model::Id namedId = edit.create(named);
    edit.modify<model::Track>(trackId, [&](auto& item) {
        item.chain.push_back(inheritId);
        item.chain.push_back(namedId);
    });
    auto committed = edit.commit();
    REQUIRE(committed);
    const auto& session = *committed;
    const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    const std::filesystem::path elsewhere = std::filesystem::path(STOCK_CLAP_DIR);
    auto pool = [&](model::Isolation byDefault, const std::filesystem::path& stock) {
        auto made = std::make_unique<InstancePool>(
            [fixture](const model::PluginRef& ref) -> Result<host::PluginInfo> {
                host::ScanOptions options;
                options.forceInProcess = true;
                auto plugins = host::scan(std::span(&fixture, 1), options);
                if (!plugins) return plugins.error();
                for (const auto& plugin : *plugins)
                    if (plugin.id == ref.id) return plugin;
                return Error("engine.plugin_missing", "fixture not found");
            });
        made->setStockDir(stock);
        made->setDefaultIsolation(byDefault);
        return made;
    };
    auto load = [&](InstancePool& p, model::Id id) {
        auto loaded = p.get(id, *session.fx.find(id), session);
        REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
        return *loaded;
    };
    SUBCASE("the shipped default: in process") {
        InstancePool plain([](const model::PluginRef&) -> Result<host::PluginInfo> {
            return Error("engine.plugin_missing", "unused");
        });
        CHECK(plain.defaultIsolation() == model::Isolation::InProcess);
        auto p = pool(model::Isolation::InProcess, elsewhere);
        CHECK_FALSE(load(*p, inheritId)->isolated());
        CHECK(p->effectiveIsolation(inheritId, *session.fx.find(inheritId)) ==
              model::Isolation::InProcess);
    }
    const std::array<model::Isolation, 2> bridged{model::Isolation::SharedBridge,
                                                  model::Isolation::Dedicated};
    SUBCASE("a third-party plug-in takes the bridged default; a named one keeps its own") {
        for (const auto isolation : bridged) {
            CAPTURE(static_cast<int>(isolation));
            auto p = pool(isolation, elsewhere);
            host::Instance* instance = load(*p, inheritId);
            CHECK(instance->isolated());
            CHECK(p->effectiveIsolation(inheritId, *session.fx.find(inheritId)) == isolation);
            CHECK_FALSE(load(*p, namedId)->isolated());
            CHECK(p->effectiveIsolation(namedId, *session.fx.find(namedId)) ==
                  model::Isolation::InProcess);
            // A new default applies as instances are made: this one stays.
            p->setDefaultIsolation(model::Isolation::InProcess);
            CHECK(load(*p, inheritId) == instance);
            CHECK(p->effectiveIsolation(inheritId, *session.fx.find(inheritId)) == isolation);
            p->restart(inheritId);
            CHECK_FALSE(load(*p, inheritId)->isolated());
            CHECK(p->isolationFallbacks().empty());
        }
    }
    SUBCASE("a stock plug-in stays in process") {
        for (const auto isolation : bridged) {
            auto p = pool(isolation, fixture.parent_path());
            CHECK_FALSE(load(*p, inheritId)->isolated());
            CHECK(p->effectiveIsolation(inheritId, *session.fx.find(inheritId)) ==
                  model::Isolation::InProcess);
            CHECK(p->isolationFallbacks().empty());
        }
    }
    SUBCASE("without a bridge: in process, and said") {
        struct Restore {
            ~Restore() { host::setBridgeExecutable(BRIDGE_PATH); }
        } restore;
        host::setBridgeExecutable({});
        for (const auto isolation : bridged) {
            auto p = pool(isolation, elsewhere);
            CHECK_FALSE(load(*p, inheritId)->isolated());
            CHECK(p->effectiveIsolation(inheritId, *session.fx.find(inheritId)) ==
                  model::Isolation::InProcess);
            REQUIRE(p->isolationFallbacks().size() == 1);
            CHECK(p->isolationFallbacks().front() == inheritId);
        }
    }
}

TEST_CASE("the engine says when the callback processes a bridged plug-in") {
    // ADR-0086: the idle-latency request follows this. The in-callback
    // executor processes every node; with the renderer, the callback has only
    // the live (monitored) ones.
    host::setBridgeExecutable(BRIDGE_PATH);
    auto session = [](model::Isolation isolation, bool monitored) {
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        model::Track track;
        track.name = "Bridged";
        track.parent = model::kMaster;
        const auto trackId = edit.create(track);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(trackId); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.gain";
        fx.isolation = isolation;
        const auto fxId = edit.create(fx);
        edit.modify<model::Track>(trackId, [&](auto& item) { item.chain.push_back(fxId); });
        if (monitored) {
            model::Route r;
            r.from.hwChannels = std::vector<int>{1, 2};
            r.to.track = trackId;
            const auto in = edit.create(r);
            edit.modify<model::Track>(trackId, [&](model::Track& x) {
                x.record.input = in;
                x.record.monitor = model::Monitor::On;
                x.record.armed = true;
            });
        }
        return edit.commit().value();
    };
    struct Case {
        model::Isolation isolation;
        bool anticipative, monitored, live;
    };
    for (const Case c : {Case{model::Isolation::SharedBridge, false, false, true},
                         Case{model::Isolation::SharedBridge, true, false, false},
                         Case{model::Isolation::SharedBridge, true, true, true},
                         Case{model::Isolation::Dedicated, true, true, true},
                         Case{model::Isolation::InProcess, false, false, false},
                         Case{model::Isolation::InProcess, true, true, false}}) {
        CAPTURE(static_cast<int>(c.isolation));
        CAPTURE(c.anticipative);
        CAPTURE(c.monitored);
        InstancePool pool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
            const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
            host::ScanOptions options;
            options.forceInProcess = true;
            auto plugins = host::scan(std::span(&fixture, 1), options);
            if (!plugins) return plugins.error();
            for (const auto& plugin : *plugins)
                if (plugin.id == ref.id) return plugin;
            return Error("engine.plugin_missing", "bridge fixture not found");
        });
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, {.anticipative = c.anticipative, .workers = 0});
        CHECK_FALSE(engine.bridgedLive());
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
        REQUIRE(engine.load(session(c.isolation, c.monitored)));
        CHECK(engine.bridgedLive() == c.live);
        // The same session without the plug-in's bridge: never.
        REQUIRE(engine.load(session(model::Isolation::InProcess, c.monitored)));
        CHECK_FALSE(engine.bridgedLive());
        REQUIRE(engine.load(session(c.isolation, c.monitored)));
        CHECK(engine.bridgedLive() == c.live);
        engine.stop();
        CHECK_FALSE(engine.bridgedLive());
    }
}
