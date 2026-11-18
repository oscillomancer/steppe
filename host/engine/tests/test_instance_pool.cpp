// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "supple/engine/offline.h"

using namespace supple;
using namespace supple::engine;

TEST_CASE("undo to an FX without saved settings restores its factory state") {
    const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    const auto plugins = host::scan(std::span(&fixture, 1));
    REQUIRE(plugins);
    InstancePool pool([&](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("engine.plugin_missing", ref.id);
    });
    const auto session = model::Session::create();
    const model::Id id{model::Kind::Fx, 1};
    model::FxInstance original;

    SUBCASE("semantic patch") {
        original.plugin.id = "com.suppleaudio.test.gain";
        auto initial = pool.get(id, original, session);
        REQUIRE(initial);
        const auto factory = (*initial)->paramValue(0);
        REQUIRE(factory);
        auto changed = original;
        changed.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12}})").value();
        auto edited = pool.get(id, changed, session);
        REQUIRE(edited);
        CHECK((*edited)->paramValue(0).value() != *factory);
        auto restored = pool.get(id, original, session);
        REQUIRE(restored);
        CHECK((*restored)->paramValue(0).value() == *factory);
        CHECK(pool.get(id, original, session).value() == *restored);
    }

    SUBCASE("opaque state while the old instance is processing") {
        original.plugin.id = "com.suppleaudio.test.edge";
        auto initial = pool.get(id, original, session);
        REQUIRE(initial);
        const auto factory = (*initial)->tail();
        auto changed = original;
        changed.state = "sha256:changed";
        auto saved = session;
        saved.blobs[*changed.state] =
            std::make_shared<const model::Blob>(model::Blob{{0, 7, 0, 0}});
        auto edited = pool.get(id, changed, saved);
        REQUIRE(edited);
        CHECK((*edited)->tail() == 7);
        REQUIRE((*edited)->activate(48000, 1, 256));
        REQUIRE((*edited)->startProcessing());
        auto restored = pool.get(id, original, session);
        REQUIRE(restored);
        CHECK((*restored)->tail() == factory);
        CHECK(*restored != *edited);
        CHECK(pool.retired() == 1);
        CHECK((*edited)->processing());
        CHECK((*edited)->tail() == 7);
        (*edited)->stopProcessing();
    }
}

TEST_CASE("an unchanged FX costs the pool no patch serialization") {
    // Route E stage 2: get() serialized every FX's patch on every compile,
    // to find out whether it had changed. The model's storage says so.
    const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    const auto plugins = host::scan(std::span(&fixture, 1));
    REQUIRE(plugins);
    InstancePool pool([&](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("engine.plugin_missing", ref.id);
    });
    const auto patch = [](int gain) {
        return json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":)" +
                           std::to_string(gain) + "}}")
            .value();
    };
    model::EditTxn t(model::Session::create(), 1);
    std::vector<model::Id> ids;
    for (int i = 0; i < 8; ++i) {
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.gain";
        fx.patch = patch(-i);
        ids.push_back(t.create(fx));
        t.modify<model::Track>(model::kMaster, [&](auto& m) { m.chain.push_back(ids.back()); });
    }
    auto committed = t.commit();
    REQUIRE_MESSAGE(committed, (committed ? "" : committed.error().toString()));
    const auto first = *committed;
    const auto getAll = [&](const model::Session& s) {
        for (const auto id : ids) REQUIRE(pool.get(id, *s.fx.find(id), s));
    };
    getAll(first);
    const auto settled = pool.patchSerializations();
    getAll(first);  // a reload: nothing changed
    CHECK(pool.patchSerializations() == settled);
    const auto before = pool.find(ids[3])->paramValue(0).value();
    model::EditTxn edit(first, 2);
    edit.modify<model::FxInstance>(ids[3], [&](auto& fx) { fx.patch = patch(-20); });
    model::Track renamed = *first.tracks.find(model::kMaster);
    renamed.name = "Renamed";
    edit.modify<model::Track>(model::kMaster, [&](auto& m) { m = renamed; });
    auto edited = edit.commit();
    REQUIRE_MESSAGE(edited, (edited ? "" : edited.error().toString()));
    const auto second = *edited;
    getAll(second);  // the edited FX alone, once
    CHECK(pool.patchSerializations() == settled + 1);
    CHECK(pool.find(ids[3])->paramValue(0).value() != before);
}

TEST_CASE("an FX the pool remembers is compared by text once it may not describe the instance") {
    // Route E stage 2 skips the comparison only when the instance was last
    // configured from this very session object, and nothing changed it since.
    const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    const auto plugins = host::scan(std::span(&fixture, 1));
    REQUIRE(plugins);
    InstancePool pool([&](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("engine.plugin_missing", ref.id);
    });
    const auto patch = [](int gain) {
        return json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":)" +
                           std::to_string(gain) + "}}")
            .value();
    };
    model::EditTxn t(model::Session::create(), 1);
    model::FxInstance gain;
    gain.plugin.id = "com.suppleaudio.test.gain";
    gain.patch = patch(-6);
    const model::Id id = t.create(gain);
    t.modify<model::Track>(model::kMaster, [&](auto& m) { m.chain.push_back(id); });
    auto committed = t.commit();
    REQUIRE_MESSAGE(committed, (committed ? "" : committed.error().toString()));
    const auto session = *committed;
    const model::FxInstance& own = *session.fx.find(id);
    auto first = pool.get(id, own, session);
    REQUIRE(first);
    const auto applied = (*first)->paramValue(0).value();

    SUBCASE("an acknowledged output patch whose commit did not happen") {
        // The plug-in moved its own parameter and the app acknowledged that
        // patch, but the session kept the object from before.
        const auto moved = json::writeCompact(patch(-20));
        REQUIRE((*first)->setPatch(moved, CLAP_SUPPLE_SET_REPLACE));
        pool.acknowledgeOutputPatch(id, patch(-20));
        REQUIRE((*first)->paramValue(0).value() != applied);
        auto again = pool.get(id, own, session);
        REQUIRE(again);
        CHECK((*again)->paramValue(0).value() == applied);
    }

    SUBCASE("an acknowledged output state whose commit did not happen") {
        // The same through the plug-in's state: it changed itself and the app
        // acknowledged the state it published, but the session kept the
        // patch object from before.
        const auto moved = json::writeCompact(patch(-20));
        REQUIRE((*first)->setPatch(moved, CLAP_SUPPLE_SET_REPLACE));
        pool.acknowledgeOutputState(id, "sha256:published");
        REQUIRE((*first)->paramValue(0).value() != applied);
        auto again = pool.get(id, own, session);
        REQUIRE(again);
        CHECK((*again)->paramValue(0).value() == applied);
    }

    SUBCASE("a recreated instance whose state failed to load") {
        // A state edit recreates a processing instance; its blob is missing,
        // so the fresh instance keeps its defaults. Undo returns the object
        // the old instance was configured from: the fresh one needs its patch.
        REQUIRE((*first)->activate(48000, 1, 256));
        REQUIRE((*first)->startProcessing());
        auto stateful = gain;
        stateful.patch.reset();
        stateful.state = "sha256:missing";
        CHECK_FALSE(pool.get(id, stateful, session));
        auto undone = pool.get(id, own, session);
        REQUIRE(undone);
        CHECK(*undone != *first);
        CHECK((*undone)->paramValue(0).value() == applied);
        (*first)->stopProcessing();
    }

    SUBCASE("a copy with another patch than the session's") {
        auto copy = own;
        copy.patch = patch(-20);
        auto edited = pool.get(id, copy, session);
        REQUIRE(edited);
        CHECK((*edited)->paramValue(0).value() != applied);
        auto back = pool.get(id, own, session);
        REQUIRE(back);
        CHECK((*back)->paramValue(0).value() == applied);
    }
}
