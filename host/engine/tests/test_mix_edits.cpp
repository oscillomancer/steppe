// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mix-value edits on the running plan (ADR-0060) and automation write holds
// (ADR-0061): dummy devices and the in-process fixture instrument only.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
#include "supple/model/session.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

const EngineOptions kInCallback{.anticipative = false, .workers = 0};

Id synthTrack(model::EditTxn& t, Id parent, std::uint8_t key) {
    model::Track tr;
    tr.name = "Synth";
    tr.parent = parent;
    const Id track = t.create(tr);
    t.modify<model::Track>(parent, [&](model::Track& p) { p.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 8 * 960;
    for (int i = 0; i < 8; ++i)
        src.sequence.notes.push_back({i * 960, 900, 0, static_cast<std::uint8_t>(key + i), 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return track;
}

// Two instruments feeding the master and, through a post-fader send with its
// own pan and a pre-fader send following its source's pan, a buss; both
// under a VCA. Console 9 on, so every level runs through the encoders.
struct Mix {
    model::Session session;
    Id a, b, bus, sendA, sendB, vca;
};
Mix mixSession() {
    Mix m;
    model::EditTxn t(model::Session::create({.console9 = true, .seed = 97531}), 1);
    m.a = synthTrack(t, kMaster, 60);
    m.b = synthTrack(t, kMaster, 67);
    model::Track bus;
    bus.type = model::StripType::Buss;
    bus.parent = kMaster;
    m.bus = t.create(bus);
    t.modify<model::Track>(kMaster, [&](model::Track& p) { p.children.push_back(m.bus); });
    model::Route route;
    route.from.track = m.a;
    route.to.track = m.bus;
    route.volumeDb = -6;
    route.pan = 30;
    m.sendA = t.create(route);
    route.from.track = m.b;
    route.tap = model::Tap::PreFader;
    route.volumeDb = -3;
    route.pan = {};
    m.sendB = t.create(route);
    model::TrackGroup group;
    group.members = {m.a, m.b};
    group.vca = model::Vca{-2, false, false, {}, {}};
    m.vca = t.create(group);
    m.session = t.commit().value();
    return m;
}

// Every value the fast path takes, changed at once (send levels keep their encoders).
model::Session moved(const Mix& m, std::uint64_t version = 2) {
    model::EditTxn t(m.session, version);
    t.modify<model::Track>(m.a, [](auto& x) {
        x.volumeDb = -4.5;
        x.pan = -40;
    });
    t.modify<model::Track>(m.b, [](auto& x) {
        x.volumeDb = 2;
        x.pan = 25;
    });
    t.modify<model::Route>(m.sendA, [](auto& r) {
        r.volumeDb = -9;
        r.pan = -20;
    });
    t.modify<model::Route>(m.sendB, [](auto& r) { r.volumeDb = -1.5; });
    t.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->volumeDb = -5; });
    return t.commit().value();
}

// The same session with an audible no-op that needs a compile (a marker).
model::Session withMarker(const model::Session& s, std::uint64_t version) {
    model::EditTxn t(s, version);
    t.create(model::Marker{});
    return t.commit().value();
}

double maxDiff(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b,
               std::size_t n) {
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(double(a[aFrom + i]) - double(b[i])));
    return worst;
}

void runAhead(Engine& engine, DummyBackend& backend, std::int64_t cycles, std::uint32_t block) {
    while (cycles > 0) {
        REQUIRE(engine.awaitRendered(4096));
        const std::int64_t n =
            std::min<std::int64_t>(cycles, std::max<std::int64_t>(1, 4096 / block));
        backend.run(n);
        cycles -= n;
    }
}

double peakOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(x[i])));
    return m;
}

}  // namespace

TEST_CASE("a mix-only edit updates the running plan and plays as the compiled session" *
          doctest::description("rt")) {
    const Mix m = mixSession();
    const model::Session next = moved(m);
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
        REQUIRE(engine.load(m.session));
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(next));
        CHECK(engine.planGeneration() == generation);  // nothing compiled
        CHECK(engine.mixEdits() == 1);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 + 4096) / 128, 128);
        else
            backend.run((48000 + 4096) / 128);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        const auto& cap = backend.capture();
        CHECK(maxDiff(cap.channels[0], 0, offline.audio.channels[0], 48000) == 0.0);
        CHECK(maxDiff(cap.channels[1], 0, offline.audio.channels[1], 48000) == 0.0);
    }
}

TEST_CASE("a mix-only edit while playing sounds exactly like a compiled reload" *
          doctest::description("rt")) {
    const Mix m = mixSession();
    const model::Session next = moved(m);
    std::vector<std::vector<float>> outputs;
    for (bool compiled : {false, true}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(m.session));
        engine.play();
        backend.run(100);
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(compiled ? withMarker(next, 3) : next));
        CHECK(engine.planGeneration() == generation + (compiled ? 1 : 0));
        backend.run(200);
        engine.stop();
        outputs.push_back(backend.capture().channels[0]);
    }
    CHECK(peakOf(outputs[0], 0, 300 * 128) > 0.01);
    CHECK(maxDiff(outputs[0], 0, outputs[1], 300 * 128) == 0.0);
}

TEST_CASE("a fader move ramps over 10 ms without compiling, after a playrate change too" *
          doctest::description("rt")) {
    for (bool rate : {false, true}) {
        INFO("rate scheduling " << rate);
        Mix m = mixSession();
        {
            model::EditTxn t(m.session, 2);
            auto project = t.project();
            project.console9 = false;
            t.setProject(project);
            m.session = t.commit().value();
        }
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
        REQUIRE(engine.load(m.session));
        engine.play();
        model::Session s = m.session;
        std::uint64_t version = 3;
        if (rate) {  // the playrate knob, away from 1.0 and back: rate plans stay
            for (double r : {1.1, 1.0}) {
                model::EditTxn t(s, version++);
                auto project = t.project();
                project.playrate.rate = r;
                t.setProject(project);
                s = t.commit().value();
                REQUIRE(engine.load(s));
                backend.run(4);
            }
        }
        backend.run(rate ? 200 : 20);  // a rate plan's lead-in is silent
        const auto start = static_cast<std::size_t>(engine.cycles()) * 256;
        const auto generation = engine.planGeneration();
        model::EditTxn t(s, version++);
        for (Id track : {m.a, m.b})
            t.modify<model::Track>(track, [](auto& x) { x.volumeDb = model::kSilenceDb; });
        // Send B taps before the fader: its own level silences it.
        t.modify<model::Route>(m.sendB, [](auto& r) { r.volumeDb = model::kSilenceDb; });
        REQUIRE(engine.load(t.commit().value()));
        CHECK(engine.planGeneration() == generation);
        CHECK(engine.mixEdits() == 1);
        backend.run(20);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        CHECK(peakOf(L, start - 2048, start) > 0.01);
        CHECK(peakOf(L, start, start + 128) > 0);          // still fading
        CHECK(peakOf(L, start + 480, start + 4096) == 0);  // silent after 10 ms
        double jump = 0;  // no sample-to-sample step bigger than the sine's own slope
        for (std::size_t i = start - 256; i < start + 512; ++i)
            jump = std::max(jump, std::fabs(double(L[i]) - double(L[i - 1])));
        CHECK(jump < 0.1);
    }
}

TEST_CASE("what a running plan cannot take still compiles") {
    const Mix m = mixSession();
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(m.session));
    std::uint64_t version = 2;
    auto compiles = [&](auto change) {
        model::EditTxn t(m.session, version++);
        change(t);
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(t.commit().value()));
        const bool compiled = engine.planGeneration() == generation + 1;
        REQUIRE(engine.load(m.session));  // back, for the next case
        return compiled;
    };
    // Mute, solo and a send's encoding are structural; a send to 0 dB drops
    // its encoder, and a send that starts following its source's pan gains one.
    CHECK(compiles(
        [&](auto& t) { t.template modify<model::Track>(m.a, [](auto& x) { x.mute = true; }); }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::Track>(m.a, [](auto& x) { x.solo = model::Solo::On; });
    }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::Route>(m.sendA, [](auto& r) { r.volumeDb = 0; });
    }));
    CHECK(compiles(
        [&](auto& t) { t.template modify<model::Route>(m.sendA, [](auto& r) { r.pan = {}; }); }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::Track>(m.a, [](auto& x) {
            x.volumeDb = -1;
            x.name = "renamed";  // with anything else, the whole edit compiles
        });
    }));
    // A curve on a level, unless a write pass holds it (ADR-0061).
    CHECK(compiles([&](auto& t) {
        model::Envelope env;
        env.target = {m.a, "volume"};
        env.points = {{0, -3}};
        t.create(env);
    }));
    // The same session again is a maintenance reload, never skipped.
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(m.session));
    CHECK(engine.planGeneration() == generation + 1);
    // While stopped too, a level edit does not compile.
    model::EditTxn t(m.session, version++);
    t.modify<model::Track>(m.a, [](auto& x) { x.volumeDb = -1; });
    const auto before = engine.planGeneration();
    REQUIRE(engine.load(t.commit().value()));
    CHECK(engine.planGeneration() == before);
    engine.stop();
}

TEST_CASE("a playrate edit together with a VCA edit compiles (not scalar-only)") {
    const Mix m = mixSession();
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    model::EditTxn rate(m.session, 2);
    auto project = rate.project();
    project.playrate.rate = 1.25;
    rate.setProject(project);
    const auto s = rate.commit().value();
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(8);
    model::EditTxn both(s, 3);
    project.playrate.rate = 1.5;
    both.setProject(project);
    both.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->volumeDb = -20; });
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(both.commit().value()));
    CHECK(engine.planGeneration() > generation);  // the VCA level reaches the engine
    engine.stop();
}

TEST_CASE("a write hold plays the static level over the curve until it is released" *
          doctest::description("rt")) {
    Mix m = mixSession();
    const model::EnvTarget target{m.a, "volume"};
    {
        model::EditTxn t(m.session, 2);
        auto project = t.project();
        project.console9 = false;
        t.setProject(project);
        t.modify<model::Track>(m.b, [](auto& x) { x.volumeDb = model::kSilenceDb; });
        t.modify<model::Route>(m.sendA, [](auto& r) { r.mute = true; });
        t.modify<model::Route>(m.sendB, [](auto& r) { r.mute = true; });  // pre fader
        t.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->volumeDb = 0; });
        model::Envelope env;
        env.target = target;
        env.mode = model::EnvMode::Latch;
        env.timebase = model::Timebase::Time;
        env.points = {{0, 0}};  // the curve plays 0 dB
        t.create(env);
        t.modify<model::Track>(m.a, [](auto& x) { x.volumeDb = model::kSilenceDb; });
        m.session = t.commit().value();
    }
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(m.session));
    engine.play();
    backend.run(20);
    const auto generation = engine.planGeneration();
    // Holding needs no compile: the static level (silence) replaces the curve.
    CHECK_FALSE(engine.setWriteHolds({target}));
    backend.run(20);
    // The pass writes points into the held envelope: still no compile.
    model::EditTxn t(m.session, 3);
    const Id env{model::Kind::Envelope, 1};
    t.modify<model::Envelope>(env, [](auto& e) { e.points.push_back({.3, 0}); });
    const auto written = t.commit().value();
    REQUIRE(engine.load(written));
    CHECK(engine.planGeneration() == generation);
    CHECK(engine.mixEdits() == 1);
    backend.run(20);
    // Released, the curve the plan holds is stale: the session compiles again.
    CHECK(engine.setWriteHolds({}));
    REQUIRE(engine.load(written));
    CHECK(engine.planGeneration() == generation + 1);
    backend.run(20);
    engine.stop();
    const auto& L = backend.capture().channels[0];
    CHECK(peakOf(L, 2560, 5120) > 0.01);          // the curve
    CHECK(peakOf(L, 5120 + 512, 15360) == 0.0);   // held at silence
    CHECK(peakOf(L, 15360 + 512, 20480) > 0.01);  // the curve again
}
