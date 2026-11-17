// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Patch-only FX edits reach the running instances without a plan (route E
// stage 3, ADR-0092): dummy devices and the fixture plug-ins only.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "realtime_common.h"
#include "supple/engine/offline.h"
#include "supple/host/clap_host.h"
#include "supple/model/session.h"

namespace {

json::Value gainPatch(int gain, int latency = 0) {
    return json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":)" +
                       std::to_string(gain) + R"(,"latency":)" + std::to_string(latency) + "}}")
        .value();
}

Id patchedGain(model::EditTxn& t, Id track, int gain,
               model::Isolation isolation = model::Isolation::Inherit) {
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    fx.patch = gainPatch(gain);
    fx.isolation = isolation;
    const Id id = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(id); });
    return id;
}

// A sine track with a patched gain, into the master, which has one too.
// `live`: a monitored input track makes the master live, so the track's gain
// runs ahead in the renderer and the master's in the callback.
struct Patched {
    model::Session session;
    Id track, trackGain, masterGain;
};
Patched patchedSession(bool live) {
    Patched p;
    const auto base = synthSession(-6.0);
    model::EditTxn t(base, 2);
    p.track = base.tracks.find(kMaster)->children.front();
    p.trackGain = patchedGain(t, p.track, -3);
    p.masterGain = patchedGain(t, kMaster, -2);
    if (live) {
        model::Track vocal;
        vocal.parent = kMaster;
        const Id track = t.create(vocal);
        t.modify<model::Track>(kMaster, [&](auto& m) { m.children.push_back(track); });
        model::Route in;
        in.from.hwChannels = std::vector<int>{1, 2};
        in.to.track = track;
        const Id input = t.create(in);
        t.modify<model::Track>(track, [&](auto& x) {
            x.record.input = input;
            x.record.monitor = model::Monitor::On;
            x.record.armed = true;
        });
    }
    p.session = t.commit().value();
    return p;
}

// Both gains moved, nothing else.
model::Session repatched(const Patched& p, std::uint64_t version) {
    model::EditTxn t(p.session, version);
    t.modify<model::FxInstance>(p.trackGain, [](auto& fx) { fx.patch = gainPatch(-14); });
    t.modify<model::FxInstance>(p.masterGain, [](auto& fx) { fx.patch = gainPatch(-9); });
    return t.commit().value();
}

// The same session with an audible no-op that needs a compile (a marker).
model::Session withMarker(const model::Session& s, std::uint64_t version) {
    model::EditTxn t(s, version);
    t.create(model::Marker{});
    return t.commit().value();
}

double peakOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(x[i])));
    return m;
}

}  // namespace

TEST_CASE("a patch-only edit compiles no plan and plays as the compiled session" *
          doctest::description("rt")) {
    const Patched p = patchedSession(false);
    const model::Session next = repatched(p, 3);
    auto offlinePool = makePool();
    const auto offline =
        renderOffline(next, offlinePool, {.startSeconds = 0, .endSeconds = 1.0}).value();
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 + 4096});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(p.session));
        const auto generation = engine.planGeneration();
        const auto serialized = pool.patchSerializations();
        REQUIRE(engine.load(next));
        CHECK(engine.planGeneration() == generation);  // nothing compiled
        CHECK(engine.patchEdits() == 1);
        CHECK(pool.patchSerializations() == serialized + 2);  // the two edited FX
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 + 4096) / 128, 128);
        else
            backend.run((48000 + 4096) / 128);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        const auto& cap = backend.capture();
        CHECK(peakOf(cap.channels[0], 0, 48000) > 0.001);
        CHECK(maxDiff(cap.channels[0], 0, offline.audio.channels[0], 48000) == 0.0);
        CHECK(maxDiff(cap.channels[1], 0, offline.audio.channels[1], 48000) == 0.0);
    }
}

TEST_CASE("a patch-only edit while playing sounds exactly like a compiled reload" *
          doctest::description("rt")) {
    // When the new patch is heard does not change: get() applied it on the
    // main thread inside the compile, and applies it there now. A live node
    // plays it from the next callback; a node the renderer runs plays it
    // after the chunks it had already rendered, plan swap or not.
    for (bool live : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 1}}) {
            if (live && !options.anticipative) continue;  // no renderer: nothing to split
            INFO("live master " << live << ", anticipative " << options.anticipative);
            const Patched p = patchedSession(live);
            const model::Session next = repatched(p, 3);
            std::vector<std::vector<float>> outputs;
            for (int run = 0; run < 3; ++run) {  // patch-only, compiled, unedited
                auto pool = makePool();
                MediaCache media;
                DummyBackend backend(
                    {.mode = DummyBackend::Mode::Manual, .captureFrames = 300 * 128});
                Engine engine(pool, media, options);
                REQUIRE(
                    engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
                REQUIRE(engine.load(p.session));
                engine.play();
                const auto cycles = [&](int n) {
                    if (options.anticipative)
                        runAhead(engine, backend, n, 128);
                    else
                        backend.run(n);
                };
                cycles(100);
                const auto generation = engine.planGeneration();
                if (run == 0) REQUIRE(engine.load(next));
                if (run == 1) REQUIRE(engine.load(withMarker(next, 4)));
                CHECK(engine.planGeneration() == generation + (run == 1 ? 1 : 0));
                CHECK(engine.patchEdits() == (run == 0 ? 1u : 0u));
                cycles(200);
                engine.stop();
                CHECK(engine.anticipationMisses() == 0);
                outputs.push_back(backend.capture().channels[0]);
            }
            CHECK(peakOf(outputs[0], 0, 300 * 128) > 0.001);
            CHECK(maxDiff(outputs[0], 0, outputs[1], 300 * 128) == 0.0);
            // The edit is heard: the unedited run differs after it.
            CHECK(maxDiff(outputs[0], 200 * 128, outputs[2], 100 * 128) > 0.0);
        }
}

TEST_CASE("a patch edit a running instance cannot take compiles") {
    const Patched p = patchedSession(false);
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(p.session));
    engine.play();
    backend.run(4);
    std::uint64_t version = 3;
    auto compiles = [&](auto change) {
        model::EditTxn t(p.session, version++);
        change(t);
        const auto generation = engine.planGeneration();
        const auto edits = engine.patchEdits();
        REQUIRE(engine.load(t.commit().value()));
        backend.run(4);
        const bool compiled = engine.planGeneration() > generation;
        CHECK(engine.patchEdits() == edits + (compiled ? 0 : 1));
        REQUIRE(engine.load(p.session));  // back, for the next case
        backend.run(4);
        for (int i = 0; i < 3; ++i) engine.service();
        return compiled;
    };
    const auto patch = [&](int gain, int latency = 0) {
        return [=, &p](auto& t) {
            t.template modify<model::FxInstance>(
                p.trackGain, [&](auto& fx) { fx.patch = gainPatch(gain, latency); });
        };
    };
    CHECK_FALSE(compiles(patch(-12)));
    // With anything else, the whole edit compiles.
    CHECK(compiles([&](auto& t) {
        patch(-12)(t);
        t.template modify<model::Track>(p.track, [](auto& x) { x.name = "renamed"; });
    }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::FxInstance>(p.trackGain, [](auto& fx) {
            fx.patch = gainPatch(-12);
            fx.wet = 50;
        });
    }));
    // A patch that moves the plug-in's latency: it asks for a restart.
    CHECK(compiles(patch(-12, 1)));
    // An FX with automation: a compile resends its automated values.
    {
        model::EditTxn t(p.session, version++);
        model::Envelope env;
        env.target = {p.trackGain, "gain"};
        env.points = {{0, -6}};
        t.create(env);
        const auto automated = t.commit().value();
        REQUIRE(engine.load(automated));
        model::EditTxn edit(automated, version++);
        edit.modify<model::FxInstance>(p.trackGain, [](auto& fx) { fx.patch = gainPatch(-12); });
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(edit.commit().value()));
        CHECK(engine.planGeneration() == generation + 1);
        REQUIRE(engine.load(p.session));
    }
    // An FX given its first patch: its instance was configured without one.
    {
        model::EditTxn t(p.session, version++);
        model::FxInstance bare;
        bare.plugin.id = "com.suppleaudio.test.gain";
        const Id id = t.create(bare);
        t.modify<model::Track>(p.track, [&](auto& x) { x.chain.push_back(id); });
        const auto withBare = t.commit().value();
        REQUIRE(engine.load(withBare));
        model::EditTxn edit(withBare, version++);
        edit.modify<model::FxInstance>(id, [](auto& fx) { fx.patch = gainPatch(-12); });
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(edit.commit().value()));
        CHECK(engine.planGeneration() == generation + 1);
    }
    // The same session again is a maintenance reload, never skipped.
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(p.session));
    CHECK(engine.planGeneration() == generation + 1);
    engine.stop();
}

namespace {
// The edge fixture's test controls (fixture_plugins.cpp, kEdgeControl).
struct EdgeControl {
    void (*requestProcess)(const clap_plugin_t*);
    void (*rescan)(const clap_plugin_t*, bool);
    void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
    void (*restart)(const clap_plugin_t*);
};
}  // namespace

TEST_CASE("a patch edit compiles while any plug-in's automation is cleared") {
    // A compile lets automation a plug-in cleared play again
    // (resetAutomation); a patch edit then still compiles, as it did.
    Patched p = patchedSession(false);
    Id edge;
    {
        model::EditTxn t(p.session, 3);
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.edge";
        fx.state = "sha256:patch-edits-edge";
        edge = t.create(fx);
        t.modify<model::Track>(p.track, [&](auto& x) { x.chain.push_back(edge); });
        t.putBlob(*fx.state, std::make_shared<const model::Blob>(model::Blob{{0, 0, 0, 0}}));
        p.session = t.commit().value();
    }
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
    REQUIRE(engine.load(p.session));
    backend.run(1);
    auto* instance = pool.find(edge);
    REQUIRE(instance);
    const auto* control = static_cast<const EdgeControl*>(
        instance->raw()->get_extension(instance->raw(), "supple.test.edge-control"));
    REQUIRE(control);
    control->clear(instance->raw(), 7, CLAP_PARAM_CLEAR_ALL);
    engine.service();  // a notification reload: the clear stays
    backend.run(1);
    REQUIRE(pool.automationCleared(edge, 7));
    std::uint64_t version = 4;
    const auto edit = [&](int gain) {
        model::EditTxn t(*engine.latestSession(), version++);
        t.modify<model::FxInstance>(p.trackGain, [&](auto& fx) { fx.patch = gainPatch(gain); });
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(t.commit().value()));
        return engine.planGeneration() > generation;
    };
    CHECK(edit(-12));
    CHECK_FALSE(pool.automationCleared(edge, 7));
    CHECK_FALSE(edit(-13));  // nothing cleared now
    engine.stop();
}

TEST_CASE("a patch-only edit while stopped compiles no plan either") {
    const Patched p = patchedSession(false);
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(p.session));
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(repatched(p, 3)));
    CHECK(engine.planGeneration() == generation);
    // The gain's value is normalized over -60..12 dB (ADR-0010).
    CHECK(pool.find(p.trackGain)->paramValue(0).value() == doctest::Approx((-14 + 60) / 72.0));
    engine.stop();
}

TEST_CASE("a bridged FX's patch-only edit costs no more bridge requests than its compile") {
    host::setBridgeExecutable(BRIDGE_PATH);
    auto requestsFor = [](bool compiled) {
        const Patched p = [] {
            Patched made;
            const auto base = synthSession(-6.0);
            model::EditTxn t(base, 2);
            made.track = base.tracks.find(kMaster)->children.front();
            made.trackGain = patchedGain(t, made.track, -3, model::Isolation::SharedBridge);
            made.masterGain = patchedGain(t, kMaster, -2, model::Isolation::SharedBridge);
            made.session = t.commit().value();
            return made;
        }();
        auto pool = makePool();
        MainThreadWake wake;  // as the app has: service() then polls nothing
        pool.setMainThreadWake(wake);
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, {.anticipative = false, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}));
        REQUIRE(engine.load(p.session));
        engine.play();
        backend.run(2);
        for (int i = 0; i < 3; ++i) engine.service();
        model::EditTxn t(p.session, 3);
        t.modify<model::FxInstance>(p.trackGain, [](auto& fx) { fx.patch = gainPatch(-14); });
        const auto next = t.commit().value();
        const auto before = host::bridgeRequests();
        REQUIRE(engine.load(compiled ? withMarker(next, 4) : next));
        backend.run(2);
        for (int i = 0; i < 3; ++i) engine.service();
        const auto used = host::bridgeRequests() - before;
        CHECK(engine.patchEdits() == (compiled ? 0u : 1u));
        engine.stop();
        return used;
    };
    const auto patchOnly = requestsFor(false);
    const auto compiled = requestsFor(true);
    CAPTURE(patchOnly);
    CAPTURE(compiled);
    CHECK(patchOnly <= compiled);
}
