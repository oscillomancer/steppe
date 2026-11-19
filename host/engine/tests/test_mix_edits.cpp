// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mix-value edits on the running plan (ADR-0060) and automation write holds
// (ADR-0061): dummy devices and the in-process fixture instrument only.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
#include "supple/model/session.h"

#include "../../clap-host/src/platform.h"

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
    src.sequence.length = 16 * 960;  // 8 s at 120 BPM: longer than any test plays
    for (int i = 0; i < 16; ++i)
        src.sequence.notes.push_back(
            {i * 960, 900, 0, static_cast<std::uint8_t>(key + i % 8), 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 16.0;  // beats
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
    // A send's encoding is structural: a send to 0 dB drops its encoder, and
    // a send that starts following its source's pan gains one. (Mute and solo
    // are values since ADR-0093; see below.)
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

// scalarOnly lists every Session collection; one that is left out lets an edit
// ride along with a playrate move without ever compiling.
TEST_CASE("a playrate edit together with a rack edit compiles (not scalar-only)") {
    Mix m = mixSession();
    model::Id rack;
    {
        model::EditTxn t(m.session, 2);
        rack = t.create(model::Rack{m.a, "Before"});
        auto project = t.project();
        project.playrate.rate = 1.25;
        t.setProject(project);
        m.session = t.commit().value();
    }
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(m.session));
    engine.play();
    backend.run(8);
    model::EditTxn both(m.session, 3);
    auto project = both.project();
    project.playrate.rate = 1.5;
    both.setProject(project);
    both.modify<model::Rack>(rack, [](auto& r) { r.name = "After"; });
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(both.commit().value()));
    CHECK(engine.planGeneration() > generation);  // the rack edit reaches the engine
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

namespace {
// An app with one track whose volume has an envelope in `mode`, playing.
struct WriteHost {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("supple-mix-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    DummyBackend backend{DummyBackend::Options{.mode = DummyBackend::Mode::Manual}};
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    explicit WriteHost(const char* mode) {
        std::filesystem::create_directories(root / "plugins");
        AppOptions options;
        options.workdir = root;
        options.pluginPath = {root / "plugins"};
        auto created = EngineApp::create(options);
        REQUIRE_MESSAGE(created, (created ? "" : created.error().toString()));
        app = std::move(*created);
        app->setFixedClock(10'000);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add");
        run("env.add", R"({"track":"tr_1","param":"volume","points":[{"beat":0,"value":0}]})");
        run("env.set", std::string(R"({"env":"en_1","mode":")") + mode + "\"}");
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                                 {.anticipative = false, .workers = 0}));
        run("transport.play");
        backend.run(4);
        app->tick();
    }
    ~WriteHost() {
        bus.reset();
        app.reset();
        std::filesystem::remove_all(root);
    }
    json::Value run(const char* command, std::string_view args = "{}", std::string gesture = {}) {
        auto parsed = json::parse(args);
        REQUIRE(parsed);
        auto r = bus->execute({command, *parsed, {cmd::Origin::Type::Test, "mix", {}, gesture}});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
    void fader(double db) {
        run("track.set", R"({"track":"tr_1","volume_db":)" + std::to_string(db) + "}", "fader:1");
        backend.run(2);
    }
    Engine& engine() { return *app->engine(); }
};
const model::EnvTarget kHeld{{model::Kind::Track, 1}, "volume"};
}  // namespace

TEST_CASE("a latch pass holds what it writes without compiling, until the pass ends") {
    WriteHost h("latch");
    const auto generation = h.engine().planGeneration();
    for (double db : {-3.0, -6.0, -9.0}) h.fader(db);
    CHECK(h.engine().writeHolds() == std::vector<model::EnvTarget>{kHeld});
    CHECK(h.engine().planGeneration() == generation);
    CHECK(h.engine().mixEdits() == 3);
    CHECK(h.app->head().envelopes.begin()->value->points.size() > 1);  // still written
    h.app->setFixedClock(20'000);  // latch never lets go by itself
    h.app->tick();
    CHECK(h.engine().writeHolds().size() == 1);
    h.run("transport.pause");
    CHECK(h.engine().writeHolds().empty());
    CHECK(h.engine().planGeneration() > generation);  // the written curve plays now
    // A mute is not held: its write compiles.
    h.run("env.add", R"({"track":"tr_1","param":"mute","points":[{"beat":0,"value":0}]})");
    h.run("env.set", R"({"env":"en_2","mode":"latch"})");
    h.run("transport.play");
    h.backend.run(4);
    const auto before = h.engine().planGeneration();
    h.run("track.set", R"({"track":"tr_1","mute":true})");
    CHECK(h.engine().planGeneration() > before);
}

TEST_CASE("automation a pass wrote on a branch reaches main through a replayed merge") {
    // Out of scope of item A, found then: the pass's writes logged no step,
    // so a merge that replays the branch dropped them.
    WriteHost h("latch");
    h.run("transport.pause");
    REQUIRE(h.app->versions().createBranch("agent/claude/ride", "main"));
    REQUIRE(h.app->switchBranch("agent/claude/ride"));
    h.run("transport.play");
    h.backend.run(4);
    for (double db : {-3.0, -6.0, -9.0}) h.fader(db);
    h.run("transport.pause");  // "Automation written": held to the stop
    const auto written = h.app->head().envelopes.begin()->value->points;
    REQUIRE(written.size() > 2);
    REQUIRE(h.app->switchBranch("main"));
    h.run("track.add");  // main moves on: the merge replays the branch
    auto merged = h.run("branch.merge", R"({"from": "agent/claude/ride"})");
    CHECK_FALSE(merged["fast_forward"].asBool());
    const auto* env = h.app->head().envelopes.find(model::Id{model::Kind::Envelope, 1});
    REQUIRE(env);
    CHECK(env->points == written);
}

TEST_CASE("a merge while main plays replays a pass's writes without capturing new ones") {
    // Round-3 review, finding 3: the replayed track.set steps also wrote at
    // main's live position, and joined main's own write pass.
    WriteHost h("latch");
    h.run("transport.pause");
    REQUIRE(h.app->versions().createBranch("agent/claude/ride", "main"));
    REQUIRE(h.app->switchBranch("agent/claude/ride"));
    h.run("transport.play");
    h.backend.run(4);
    for (double db : {-3.0, -6.0, -9.0}) h.fader(db);
    h.run("transport.pause");
    const auto written = h.app->head().envelopes.begin()->value->points;
    REQUIRE(written.size() > 2);
    REQUIRE(h.app->switchBranch("main"));
    h.run("track.add");                            // main moves on: the merge replays the branch
    h.run("transport.seek", R"({"seconds": 4})");  // farther ahead than the pass
    h.run("transport.play");
    h.backend.run(4);
    auto merged = h.run("branch.merge", R"({"from": "agent/claude/ride"})");
    CHECK_FALSE(merged["fast_forward"].asBool());
    const model::Id en1{model::Kind::Envelope, 1};
    REQUIRE(h.app->head().envelopes.find(en1));
    CHECK(h.app->head().envelopes.find(en1)->points == written);
    h.run("transport.pause");  // main's own pass wrote nothing: nothing to hold
    CHECK(h.app->head().envelopes.find(en1)->points == written);
}

TEST_CASE("a touch pass lets go of a level half a second after its last write") {
    WriteHost h("touch");
    const auto generation = h.engine().planGeneration();
    h.fader(-3);
    h.app->setFixedClock(10'300);
    h.fader(-6);
    CHECK(h.engine().planGeneration() == generation);
    REQUIRE(h.app->nextServiceDeadline());  // the release is scheduled
    h.app->setFixedClock(10'799);
    h.app->tick();
    CHECK(h.engine().writeHolds().size() == 1);
    h.app->setFixedClock(10'800);
    REQUIRE(h.app->nextServiceDeadline());  // overdue work must still wake service
    h.app->tick();
    CHECK(h.engine().writeHolds().empty());
    CHECK(h.engine().planGeneration() == generation + 1);  // its curve compiles
    h.app->tick();
    CHECK(h.engine().planGeneration() == generation + 1);
    CHECK_FALSE(h.app->nextServiceDeadline());
}

TEST_CASE("a touch release compiles its stale curve when another strip moves") {
    WriteHost h("touch");
    h.run("track.add");
    h.fader(-6);
    const auto generation = h.engine().planGeneration();
    h.app->setFixedClock(10'500);
    h.run("track.set", R"({"track":"tr_2","volume_db":-3})");
    CHECK(h.engine().writeHolds().empty());
    CHECK(h.engine().planGeneration() == generation + 1);
    h.app->tick();
    CHECK(h.engine().planGeneration() == generation + 1);
}

TEST_CASE("a released touch deadline does not keep servicing an unrelated latch hold") {
    WriteHost h("touch");
    h.run("track.add");
    h.run("env.add", R"({"track":"tr_2","param":"volume","points":[{"beat":0,"value":0}]})");
    h.run("env.set", R"({"env":"en_2","mode":"latch"})");
    h.fader(-6);
    h.run("track.set", R"({"track":"tr_2","volume_db":-3})");
    REQUIRE(h.engine().writeHolds().size() == 2);
    h.app->setFixedClock(10'500);
    REQUIRE(h.app->nextServiceDeadline());
    h.app->tick();
    CHECK(h.engine().writeHolds() ==
          std::vector<model::EnvTarget>{{{model::Kind::Track, 2}, "volume"}});
    CHECK_FALSE(h.app->nextServiceDeadline());
}

TEST_CASE("undo mid-pass plays the restored envelope and leaves no latch tail") {
    WriteHost h("latch");
    h.fader(-6);
    REQUIRE(h.engine().writeHolds().size() == 1);
    h.run("history.undo");
    CHECK(h.engine().writeHolds().empty());
    const auto& points = h.app->head().envelopes.begin()->value->points;
    REQUIRE(points.size() == 1);
    CHECK(points[0].value == 0);
    h.backend.run(4);
    h.run("transport.pause");  // the undone write gets no latch tail
    CHECK(h.app->head().envelopes.begin()->value->points.size() == 1);
}

TEST_CASE("an engine started again after a halt holds nothing from the old pass") {
    WriteHost h("latch");
    h.fader(-6);
    REQUIRE(h.engine().writeHolds().size() == 1);
    const auto written = h.app->head().envelopes.begin()->value->points.size();
    h.app->stopEngine();
    REQUIRE(h.app->startEngine(h.backend, {.sampleRate = 48000, .blockSize = 128},
                               {.anticipative = false, .workers = 0}));
    h.backend.run(2);
    h.app->tick();
    CHECK_FALSE(h.engine().transport().playing);
    CHECK(h.engine().writeHolds().empty());
    // Recording starts no new pass: the halted one must not come back either.
    h.run("track.set", R"({"track":"tr_0","armed":true})");
    h.run("transport.record");
    h.backend.run(4);
    h.app->tick();
    CHECK(h.engine().writeHolds().empty());
    h.run("transport.stop");
    // What the halted pass wrote stays, with no latch tail from it.
    CHECK(h.app->head().envelopes.begin()->value->points.size() == written);
}

TEST_CASE("a scalar playrate edit carries hold changes and compiles a released stale curve" *
          doctest::description("rt")) {
    Mix m = mixSession();
    const model::EnvTarget target{m.a, "volume"};
    auto rated = [](const model::Session& s, double rate, std::uint64_t version) {
        model::EditTxn t(s, version);
        auto project = t.project();
        project.playrate.rate = rate;
        t.setProject(project);
        return t.commit().value();
    };
    {
        model::EditTxn t(m.session, 2);
        auto project = t.project();
        project.console9 = false;
        project.playrate.rate = 1.1;
        t.setProject(project);
        t.modify<model::Track>(m.b, [](auto& x) { x.volumeDb = model::kSilenceDb; });
        t.modify<model::Route>(m.sendA, [](auto& r) { r.mute = true; });
        t.modify<model::Route>(m.sendB, [](auto& r) { r.mute = true; });
        t.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->volumeDb = 0; });
        model::Envelope env;
        env.target = target;
        env.mode = model::EnvMode::Latch;
        env.timebase = model::Timebase::Time;
        env.points = {{0, 0}};  // the curve plays 0 dB; the static level is silence
        t.create(env);
        t.modify<model::Track>(m.a, [](auto& x) { x.volumeDb = model::kSilenceDb; });
        m.session = t.commit().value();
    }
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 600 * 256});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}));
    REQUIRE(engine.load(m.session));
    engine.play();
    backend.run(200);  // past a rate plan's silent lead-in
    // A clean release: the plan's curve is current, the hold flag must go.
    CHECK_FALSE(engine.setWriteHolds({target}));
    backend.run(60);
    CHECK_FALSE(engine.setWriteHolds({}, false));  // the next load carries it
    auto generation = engine.planGeneration();
    REQUIRE(engine.load(rated(m.session, 1.15, 3)));
    CHECK(engine.planGeneration() == generation);  // the scalar shortcut
    backend.run(80);
    // A stale release: written points never compiled, so the load compiles.
    CHECK_FALSE(engine.setWriteHolds({target}));
    model::EditTxn t(rated(m.session, 1.15, 4), 5);
    t.modify<model::Envelope>({model::Kind::Envelope, 1},
                              [](auto& e) { e.points.push_back({.5, 0}); });
    const auto written = t.commit().value();
    REQUIRE(engine.load(written));
    CHECK(engine.planGeneration() == generation);
    backend.run(60);
    CHECK(engine.setWriteHolds({}, false));
    REQUIRE(engine.load(rated(written, 1.2, 6)));
    CHECK(engine.planGeneration() > generation);
    backend.run(120);
    engine.stop();
    const auto& L = backend.capture().channels[0];
    CHECK(peakOf(L, 150 * 256, 200 * 256) > 0.01);  // the curve
    CHECK(peakOf(L, 204 * 256, 260 * 256) == 0.0);  // held at silence
    CHECK(peakOf(L, 280 * 256, 340 * 256) > 0.01);  // released by the rate edit
    CHECK(peakOf(L, 344 * 256, 400 * 256) == 0.0);  // held again
    CHECK(peakOf(L, 440 * 256, 520 * 256) > 0.01);  // the compiled curve
}

TEST_CASE("mix edits reach live and rendered nodes alike, across the frontier" *
          doctest::description("rt")) {
    // A monitored track makes the buss it sends to, and the master, live; the
    // instruments stay with the renderer and reach them across the frontier
    // (their sends encoded at the source). The VCA spans both classes.
    Mix m = mixSession();
    Id vocal;
    {
        model::EditTxn t(m.session, 2);
        model::Track tr;
        tr.parent = kMaster;
        tr.pan = 30;
        vocal = t.create(tr);
        t.modify<model::Track>(kMaster, [&](auto& x) { x.children.push_back(vocal); });
        model::Route in;
        in.from.hwChannels = std::vector<int>{1, 2};
        in.to.track = vocal;
        const Id input = t.create(in);
        t.modify<model::Track>(vocal, [&](auto& x) {
            x.record.input = input;
            x.record.monitor = model::Monitor::On;
            x.record.armed = true;
        });
        model::Route send;
        send.from.track = vocal;
        send.to.track = m.bus;
        send.volumeDb = -4;
        t.create(send);
        t.modify<model::TrackGroup>(m.vca, [&](auto& g) { g.members.push_back(vocal); });
        m.session = t.commit().value();
    }
    model::Session next = moved(m, 3);
    {
        model::EditTxn t(next, 4);
        t.modify<model::Track>(vocal, [](auto& x) {
            x.volumeDb = -7;
            x.pan = -60;
        });
        t.modify<model::Route>({model::Kind::Route, 4}, [](auto& r) { r.volumeDb = -11; });
        next = t.commit().value();
    }
    constexpr std::int64_t kCycles = 300;
    std::vector<std::vector<float>> reference;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 1}})
        for (bool compiled : {true, false}) {
            INFO("anticipative " << options.anticipative << ", compiled " << compiled);
            auto pool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual,
                 .captureFrames = kCycles * 256,
                 .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                             std::int64_t frame) {
                     for (std::uint32_t i = 0; i < n; ++i) {
                         const float v = static_cast<float>((frame + i) % 4800) / 4800.0f - 0.5f;
                         in[0][i] = v;
                         if (channels > 1) in[1][i] = -v;
                     }
                 }});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
            REQUIRE(engine.load(m.session));
            const auto generation = engine.planGeneration();
            REQUIRE(engine.load(compiled ? withMarker(next, 5) : next));
            CHECK(engine.planGeneration() == generation + (compiled ? 1 : 0));
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, kCycles, 256);
            else
                backend.run(kCycles);
            CHECK(engine.anticipationMisses() == 0);
            CHECK(engine.processErrors() == 0);
            engine.stop();
            const auto& cap = backend.capture().channels;
            CHECK(peakOf(cap[0], 20000, 40000) > 0.01);
            if (reference.empty())
                reference = cap;
            else
                for (std::size_t c = 0; c < 2; ++c) CHECK(cap[c] == reference[c]);
        }
}

namespace {
// mixSession() with a gain at -12 dB after each instrument, and a monitored
// vocal with its own gain: the instruments' gains run in the renderer, the
// vocal's in the callback (ADR-0093 edits reach both).
struct Wet {
    Mix m;
    Id vocal, gainA, gainB, gainVocal;
};
Wet wetSession() {
    Wet w;
    w.m = mixSession();
    model::EditTxn t(w.m.session, 2);
    auto gain = [&](Id track) {
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.gain";
        fx.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12}})").value();
        const Id id = t.create(fx);
        t.modify<model::Track>(track, [&](auto& x) { x.chain.push_back(id); });
        return id;
    };
    w.gainA = gain(w.m.a);
    w.gainB = gain(w.m.b);
    model::Track tr;
    tr.parent = kMaster;
    w.vocal = t.create(tr);
    t.modify<model::Track>(kMaster, [&](auto& x) { x.children.push_back(w.vocal); });
    model::Route in;
    in.from.hwChannels = std::vector<int>{1, 2};
    in.to.track = w.vocal;
    const Id input = t.create(in);
    t.modify<model::Track>(w.vocal, [&](auto& x) {
        x.record.input = input;
        x.record.monitor = model::Monitor::On;
        x.record.armed = true;
    });
    w.gainVocal = gain(w.vocal);
    w.m.session = t.commit().value();
    return w;
}
DummyBackend::Options sawInput(std::int64_t frames) {
    return {
        .mode = DummyBackend::Mode::Manual,
        .captureFrames = frames,
        .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t frame) {
            for (std::uint32_t i = 0; i < n; ++i) {
                const float v = static_cast<float>((frame + i) % 4800) / 4800.0f - 0.5f;
                in[0][i] = v;
                if (channels > 1) in[1][i] = -v;
            }
        }};
}
// Plays `base`, takes `next` at cycle 100 by `run` (0: value edit, 1:
// compiled reload, 2: not at all) and returns channel 0 of 300 cycles. The
// anticipative renderer is full when the edit is made: it has rendered its
// whole render-ahead and waits for the callback, so every run takes the
// edit from the same renderer position.
constexpr std::size_t kEditFrame = 100 * 256, kEditFrames = 300 * 256;
std::vector<float> playEdit(const model::Session& base, const model::Session& next,
                            const EngineOptions& options, int run, bool valueEdit = true) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(sawInput(kEditFrames));
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    REQUIRE(engine.load(base));
    engine.play();
    const auto cycles = [&](int n) {
        if (options.anticipative)
            runAhead(engine, backend, n, 256);
        else
            backend.run(n);
    };
    cycles(100);
    if (options.anticipative)
        REQUIRE(engine.awaitRendered(std::numeric_limits<std::int64_t>::max()));
    const auto generation = engine.planGeneration();
    if (run == 0) REQUIRE(engine.load(next));
    if (run == 1) REQUIRE(engine.load(withMarker(next, next.version + 1)));
    const bool fast = run == 0 && valueEdit;
    CHECK(engine.planGeneration() == generation + (run == 2 || fast ? 0 : 1));
    CHECK(engine.mixEdits() == (fast ? 1u : 0u));
    cycles(200);
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.processErrors() == 0);
    engine.stop();
    return backend.capture().channels[0];
}

// Where a full renderer resumes: the first render block that ends past the
// playhead plus the render-ahead target (EngineOptions::renderAheadMs).
std::size_t rendererResumesAt(const EngineOptions& options, std::size_t playhead) {
    const std::size_t block = options.renderBlock;
    const std::size_t ahead = std::max<std::size_t>(
        2 * block, static_cast<std::size_t>(options.renderAheadMs / 1000.0 * 48000));
    return (playhead + ahead + block - 1) / block * block;
}

// Which executor's values an edit changes in the anticipative executor:
// only the renderer's, only the callback's (a live node's), or both.
enum class Path { Rendered, Live, Split };

// The value edit against the compiled reload and the unedited run (ADR-0093).
// In the callback executor they are equal. In the anticipative one, with the
// renderer full at the edit, a compiled reload is heard from where the
// renderer resumes. A value edit heard only on rendered tracks is too, and
// is then equal to the compiled reload sample for sample. One heard on a
// live track plays there from the next block, and equals the compiled
// reload once that reload's ramp (Plan::smoothFrames, 480) is done. One
// that changes both executors' values compiles there, so both switch at the
// handover: the callback and the renderer take values at different stream
// positions, and between them the output would be neither session's.
void checkLikeCompiled(const model::Session& base, const model::Session& next, Path path) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 1}}) {
        CAPTURE(options.anticipative);
        const bool valueEdit = !options.anticipative || path != Path::Split;
        const auto fast = playEdit(base, next, options, 0, valueEdit);
        const auto compiled = playEdit(base, next, options, 1);
        const auto unedited = playEdit(base, next, options, 2);
        auto heard = [&](const std::vector<float>& x) {
            for (std::size_t i = 0; i < kEditFrames; ++i)
                if (x[i] != unedited[i]) return i;
            return kEditFrames;
        };
        CHECK(peakOf(unedited, 0, kEditFrames) > 0.01);
        REQUIRE(heard(fast) < kEditFrames);  // the edit is heard
        if (!options.anticipative) {
            CHECK(heard(fast) >= kEditFrame);
            CHECK(maxDiff(fast, 0, compiled, kEditFrames) == 0.0);
            continue;
        }
        // From frame `at`: a stage's wet ramp starts at the old level, so its
        // first frame may still sound unchanged.
        auto from = [](std::size_t heardAt, std::size_t at) {
            return heardAt == at || heardAt == at + 1;
        };
        const std::size_t resumes = rendererResumesAt(options, kEditFrame);
        // Should this fail, where each run was heard says which run moved.
        std::size_t apart = 0;
        while (apart < kEditFrames && fast[apart] == compiled[apart]) ++apart;
        INFO("heard: fast " << heard(fast) << ", compiled " << heard(compiled) << "; resumes "
                            << resumes << "; fast and compiled first differ at " << apart);
        CHECK(from(heard(compiled), resumes));
        if (path == Path::Live) {
            CHECK(from(heard(fast), kEditFrame));
            const std::size_t ramped = resumes + 480;
            CHECK(std::equal(fast.begin() + ramped, fast.begin() + kEditFrames,
                             compiled.begin() + ramped));
        } else {
            CHECK(maxDiff(fast, 0, compiled, kEditFrames) == 0.0);
        }
    }
}
}  // namespace

namespace {
// Two edits while playing: `first` at cycle 100 and `second` `gap` cycles
// later, as value edits where the engine allows (compiled: both reloads
// forced). The renderer is full before each, so every run takes them from
// the same positions. Returns channel 0 of 300 cycles and the counts after
// each edit.
struct TwoEdits {
    std::vector<float> out;
    std::uint64_t mixEdits[2] = {}, plans[2] = {};
};
TwoEdits playTwo(const model::Session& base, const model::Session& first,
                 const model::Session& second, const EngineOptions& options, bool compiled,
                 int gap) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(sawInput(kEditFrames));
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
    REQUIRE(engine.load(base));
    engine.play();
    const auto cycles = [&](int n) {
        if (options.anticipative)
            runAhead(engine, backend, n, 256);
        else
            backend.run(n);
        if (options.anticipative)
            REQUIRE(engine.awaitRendered(std::numeric_limits<std::int64_t>::max()));
    };
    cycles(100);
    TwoEdits r;
    const auto generation = engine.planGeneration();
    const model::Session* edits[2] = {&first, &second};
    for (int i = 0; i < 2; ++i) {
        if (i == 1) cycles(gap);
        const model::Session& s = *edits[i];
        REQUIRE(engine.load(compiled ? withMarker(s, s.version + 1) : s));
        r.mixEdits[i] = engine.mixEdits();
        r.plans[i] = engine.planGeneration() - generation;
    }
    cycles(200 - gap);
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.processErrors() == 0);
    engine.stop();
    r.out = backend.capture().channels[0];
    return r;
}
}  // namespace

TEST_CASE(
    "a rendered edit and then a live one switch together while the first is ahead "
    "(ADR-0093)" *
    doctest::description("rt")) {
    // Round 42b: a and b muted, the vocal playing. One edit unmutes a (the
    // renderer's values), the next mutes the vocal (the callback's). The
    // callback takes the second at its next block, the renderer took the
    // first where it was rendering, up to the render-ahead later in the
    // stream; in between all three would be silent, which neither session
    // is. So while the first is still ahead of what the callback played, the
    // second compiles, and the two switch together as two compiled reloads
    // do. That holds after the renderer has taken the first, too (gap 8).
    const Wet w = wetSession();
    const Mix& m = w.m;
    auto edit = [](const model::Session& s, auto change) {
        model::EditTxn t(s, s.version + 1);
        change(t);
        return t.commit().value();
    };
    const model::Session base = edit(m.session, [&](model::EditTxn& t) {
        for (Id id : {m.a, m.b}) t.modify<model::Track>(id, [](auto& x) { x.mute = true; });
    });
    const model::Session unmuted = edit(base, [&](model::EditTxn& t) {
        t.modify<model::Track>(m.a, [](auto& x) { x.mute = false; });
    });
    const model::Session vocalMuted = edit(unmuted, [&](model::EditTxn& t) {
        t.modify<model::Track>(w.vocal, [](auto& x) { x.mute = true; });
    });
    // The render-ahead is 10240 frames here, 40 cycles: at gap 60 the
    // callback has played past where the renderer took the first edit.
    for (const int gap : {0, 8, 60})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 1}}) {
            CAPTURE(gap);
            CAPTURE(options.anticipative);
            const auto fast = playTwo(base, unmuted, vocalMuted, options, false, gap);
            const auto compiled = playTwo(base, unmuted, vocalMuted, options, true, gap);
            CHECK(peakOf(compiled.out, 0, kEditFrames) > 0.01);
            CHECK(fast.mixEdits[0] == 1);
            CHECK(fast.plans[0] == 0);
            const bool together = options.anticipative && gap < 60;
            CHECK(fast.mixEdits[1] == (together ? 1u : 2u));
            CHECK(fast.plans[1] == (together ? 1u : 0u));
            if (!options.anticipative || together) {
                CHECK(maxDiff(fast.out, 0, compiled.out, kEditFrames) == 0.0);
            } else {
                // A live-only edit after the first was played: the vocal is
                // muted from its next block, the compiled reload from where
                // the renderer resumes; equal before and after that.
                const std::size_t at = kEditFrame + std::size_t(gap) * 256;
                const std::size_t ramped = rendererResumesAt(options, at) + 480;
                CHECK(std::equal(fast.out.begin(), fast.out.begin() + at, compiled.out.begin()));
                CHECK(std::equal(fast.out.begin() + ramped, fast.out.end(),
                                 compiled.out.begin() + ramped));
            }
        }
}

namespace {
// The fixture library's process hold: while set, the crash fixture's
// process() waits inside it. Opened as the host opens it, so this is the
// library the instance pool loaded and the flag its instances read.
struct ProcessHold {
    void (*hold)(int) = nullptr;
    int (*held)() = nullptr;
    ProcessHold() {
        auto library = host::platform::openLibrary(std::filesystem::weakly_canonical(kFixture));
        REQUIRE(library);
        hold = reinterpret_cast<void (*)(int)>(
            host::platform::symbol(*library, "supple_fixture_hold_process"));
        held = reinterpret_cast<int (*)()>(
            host::platform::symbol(*library, "supple_fixture_held_process"));
        REQUIRE(hold);
        REQUIRE(held);
    }
    ~ProcessHold() {
        if (hold) hold(0);
    }
    ProcessHold(const ProcessHold&) = delete;
    ProcessHold& operator=(const ProcessHold&) = delete;
};
}  // namespace

TEST_CASE(
    "a rendered edit taken while the renderer is inside a block still gates a live one "
    "(ADR-0093)" *
    doctest::description("rt")) {
    // Round 42 suggestion 1. A rendered edit published while the renderer is
    // inside a block (it took the values at the block's start) is heard from
    // that block's end, renderedStream_ + renderBlock, not from
    // renderedStream_. A live edit with the callback between the two must
    // still compile. The crash fixture on b (rendered, muted) holds the
    // renderer there; blocks of 4096 make that window 16 cycles wide.
    //
    // Renderer full after 100 cycles (25600 played): rendered to 36864, the
    // first multiple of 4096 at least 9600 ahead. It starts its next block
    // once 27264 is played: held there 8 cycles on (27648), the first edit
    // is published, and 40 cycles later (37888, inside 36864..40960) the
    // second.
    const Wet w = wetSession();
    const Mix& m = w.m;
    auto edit = [](const model::Session& s, auto change) {
        model::EditTxn t(s, s.version + 1);
        change(t);
        return t.commit().value();
    };
    const model::Session base = edit(m.session, [&](model::EditTxn& t) {
        for (Id id : {m.a, m.b}) t.modify<model::Track>(id, [](auto& x) { x.mute = true; });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.crash";  // passes audio through
        const Id hold = t.create(fx);
        t.modify<model::Track>(m.b, [&](auto& x) { x.chain.push_back(hold); });
    });
    const model::Session unmuted = edit(base, [&](model::EditTxn& t) {
        t.modify<model::Track>(m.a, [](auto& x) { x.mute = false; });
    });
    const model::Session vocalMuted = edit(unmuted, [&](model::EditTxn& t) {
        t.modify<model::Track>(w.vocal, [](auto& x) { x.mute = true; });
    });
    ProcessHold processHold;
    const EngineOptions options{.renderBlock = 4096, .workers = 1};
    auto play = [&](bool compiled) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(sawInput(kEditFrames));
        Engine engine(pool, media, options);
        // Released on every way out, before the engine joins its renderer: a
        // REQUIRE failing while the hold is set would otherwise leave the
        // renderer waiting in it, and ~Engine waiting for the renderer.
        struct Release {
            ProcessHold& h;
            ~Release() { h.hold(0); }
        } release{processHold};
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}));
        REQUIRE(engine.load(base));
        engine.play();
        auto full = [&] {
            REQUIRE(engine.awaitRendered(std::numeric_limits<std::int64_t>::max()));
        };
        runAhead(engine, backend, 100, 256);
        full();
        const auto generation = engine.planGeneration();
        TwoEdits r;
        // Into the next block, held inside it.
        const int entered = processHold.held();
        processHold.hold(1);
        backend.run(8);
        while (processHold.held() == entered) std::this_thread::yield();
        REQUIRE(engine.load(compiled ? withMarker(unmuted, unmuted.version + 1) : unmuted));
        r.mixEdits[0] = engine.mixEdits();
        r.plans[0] = engine.planGeneration() - generation;
        processHold.hold(0);
        runAhead(engine, backend, 40, 256);
        full();
        REQUIRE(
            engine.load(compiled ? withMarker(vocalMuted, vocalMuted.version + 1) : vocalMuted));
        r.mixEdits[1] = engine.mixEdits();
        r.plans[1] = engine.planGeneration() - generation;
        runAhead(engine, backend, 300 - 148, 256);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(engine.processErrors() == 0);
        engine.stop();
        r.out = backend.capture().channels[0];
        return r;
    };
    const auto fast = play(false);
    const auto compiled = play(true);
    CHECK(peakOf(compiled.out, 0, kEditFrames) > 0.01);
    CHECK(fast.mixEdits[0] == 1);
    CHECK(fast.plans[0] == 0);
    // Still ahead of the callback: the second compiles, and both switch as
    // two compiled reloads do.
    CHECK(fast.mixEdits[1] == 1);
    CHECK(fast.plans[1] == 1);
    std::size_t apart = 0;
    while (apart < kEditFrames && fast.out[apart] == compiled.out[apart]) ++apart;
    CAPTURE(apart);  // where they first differ
    CHECK(maxDiff(fast.out, 0, compiled.out, kEditFrames) == 0.0);
}

TEST_CASE("FX wet and bypass edits while playing sound exactly like a compiled reload" *
          doctest::description("rt")) {
    // ADR-0093: no plan; the stage's own ramp crossfades, as after a compile.
    // The instruments' gains run in the renderer, the vocal's live.
    const Wet w = wetSession();
    for (const auto& [name, fxs, bypass, path] :
         std::vector<std::tuple<std::string, std::vector<Id>, bool, Path>>{
             {"wet, rendered", {w.gainA}, false, Path::Rendered},
             {"wet, live", {w.gainVocal}, false, Path::Live},
             {"wet, rendered and live", {w.gainA, w.gainVocal}, false, Path::Split},
             {"bypass, rendered", {w.gainB}, true, Path::Rendered},
             {"bypass, live", {w.gainVocal}, true, Path::Live},
             {"bypass, rendered and live", {w.gainB, w.gainVocal}, true, Path::Split}}) {
        CAPTURE(name);
        model::EditTxn t(w.m.session, 3);
        for (Id fx : fxs)
            t.modify<model::FxInstance>(fx, [&](auto& f) {
                if (bypass)
                    f.bypass = true;
                else
                    f.wet = 40;
            });
        checkLikeCompiled(w.m.session, t.commit().value(), path);
    }
}

TEST_CASE("wet and bypass edits on input and take chains sound like a compiled reload" *
          doctest::description("rt")) {
    // Round 42: stages other than a track's chain have mix slots too. A take
    // chain on a rendered audio item and an input chain on the monitored
    // vocal, each a -12 dB gain.
    const Wet w = wetSession();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-stages-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    audio::AudioData saw;
    saw.sampleRate = 48000;
    saw.channels.assign(2, std::vector<float>(96000));
    for (std::size_t i = 0; i < 96000; ++i)
        saw.channels[0][i] = saw.channels[1][i] = static_cast<float>(i % 300) / 600.0f - 0.25f;
    REQUIRE(audio::writeWav(dir / "saw.wav", saw));
    Id takeGain, inputGain;
    const model::Session base = [&] {
        model::EditTxn t(w.m.session, 3);
        auto gain = [&] {
            model::FxInstance fx;
            fx.plugin.id = "com.suppleaudio.test.gain";
            fx.patch =
                json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12}})")
                    .value();
            return t.create(fx);
        };
        model::Track tr;
        tr.parent = kMaster;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](auto& x) { x.children.push_back(track); });
        model::Take take;
        take.source =
            t.create(model::MediaSource{(dir / "saw.wav").string(), "", "wav", 48000, 2, 96000});
        takeGain = gain();
        take.chain = {takeGain};
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = 4.0;  // beats: 2 s at 120 BPM
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        inputGain = gain();
        t.modify<model::Track>(w.vocal, [&](auto& x) { x.inputChain.push_back(inputGain); });
        return t.commit().value();
    }();
    auto edited = [&](std::vector<Id> fxs, bool bypass) {
        model::EditTxn t(base, base.version + 1);
        for (Id fx : fxs)
            t.modify<model::FxInstance>(fx, [&](auto& f) {
                if (bypass)
                    f.bypass = true;
                else
                    f.wet = 40;
            });
        return t.commit().value();
    };
    for (const auto& [name, fxs, bypass, path] :
         std::vector<std::tuple<std::string, std::vector<Id>, bool, Path>>{
             {"take chain wet", {takeGain}, false, Path::Rendered},
             {"take chain bypass", {takeGain}, true, Path::Rendered},
             {"input chain wet", {inputGain}, false, Path::Live},
             {"input chain bypass", {inputGain}, true, Path::Live},
             {"take and input chain bypass", {takeGain, inputGain}, true, Path::Split}}) {
        CAPTURE(name);
        checkLikeCompiled(base, edited(fxs, bypass), path);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("an FX wet or bypass edit compiles with anything else, or when the FX goes offline") {
    const Wet w = wetSession();
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}));
    REQUIRE(engine.load(w.m.session));
    std::uint64_t version = 3;
    auto compiles = [&](auto change) {
        model::EditTxn t(w.m.session, version++);
        change(t);
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(t.commit().value()));
        const bool compiled = engine.planGeneration() == generation + 1;
        REQUIRE(engine.load(w.m.session));
        return compiled;
    };
    CHECK_FALSE(compiles([&](auto& t) {
        t.template modify<model::FxInstance>(w.gainA, [](auto& f) { f.wet = 50; });
    }));
    CHECK_FALSE(compiles([&](auto& t) {
        t.template modify<model::FxInstance>(w.gainA, [](auto& f) { f.bypass = true; });
    }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::FxInstance>(w.gainA, [](auto& f) { f.offline = true; });
    }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::FxInstance>(w.gainA, [](auto& f) {
            f.wet = 50;
            f.pdc = model::PdcMode::Ignore;  // with anything else, the whole edit compiles
        });
    }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::FxInstance>(w.gainA, [](auto& f) {
            f.bypass = true;
            f.patch = json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-3}})")
                          .value();
        });
    }));
    engine.stop();
}

TEST_CASE("mute and solo edits while playing sound exactly like a compiled reload" *
          doctest::description("rt")) {
    // ADR-0093: track, VCA and route mutes, track and VCA solo, solo-defeat,
    // on rendered tracks (a, b, the buss; the VCA holds a and b) and a live
    // one (the vocal).
    const Wet w = wetSession();
    const Mix& m = w.m;
    auto soloed = [&](Id track) {
        model::EditTxn t(m.session, 3);
        t.modify<model::Track>(track, [](auto& x) { x.solo = model::Solo::On; });
        return t.commit().value();
    };
    const model::Session vocalSoloed = soloed(w.vocal), bSoloed = soloed(m.b);
    using Edit = std::function<void(model::EditTxn&)>;
    auto mute = [](std::vector<Id> ids) -> Edit {
        return [ids](model::EditTxn& t) {
            for (Id id : ids) t.modify<model::Track>(id, [](auto& x) { x.mute = true; });
        };
    };
    auto solo = [](std::vector<std::pair<Id, model::Solo>> to) -> Edit {
        return [to](model::EditTxn& t) {
            for (const auto& [id, s] : to) t.modify<model::Track>(id, [s](auto& x) { x.solo = s; });
        };
    };
    struct Case {
        std::string name;
        const model::Session& base;
        Edit edit;
        Path path;
    };
    const std::vector<Case> cases = {
        {"track mute, rendered", m.session, mute({m.a}), Path::Rendered},
        {"track mute, live", m.session, mute({w.vocal}), Path::Live},
        {"track mute, rendered and live", m.session, mute({m.a, w.vocal}), Path::Split},
        // Silences the vocal and a.
        {"solo a rendered track", m.session, solo({{m.b, model::Solo::On}}), Path::Split},
        // Silences b alone.
        {"solo the live track, defeat another", m.session,
         solo({{w.vocal, model::Solo::InPlace}, {m.a, model::Solo::Defeat}}), Path::Rendered},
        // Silences the vocal and lets b play, or the reverse (round 42).
        {"solo moves from the live track to a rendered one", vocalSoloed,
         solo({{w.vocal, model::Solo::Off}, {m.b, model::Solo::On}}), Path::Split},
        {"solo moves from a rendered track to the live one", bSoloed,
         solo({{m.b, model::Solo::Off}, {w.vocal, model::Solo::On}}), Path::Split},
        {"VCA mute", m.session,
         [&](model::EditTxn& t) {
             t.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->mute = true; });
         },
         Path::Rendered},
        // Silences the vocal alone: the buss is fed by a and b.
        {"VCA solo", m.session,
         [&](model::EditTxn& t) {
             t.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->solo = true; });
         },
         Path::Live},
        {"an encoded send's mute", m.session,
         [&](model::EditTxn& t) {
             t.modify<model::Route>(m.sendA, [](auto& r) { r.mute = true; });
             t.modify<model::Route>(m.sendB, [](auto& r) { r.mute = true; });
         },
         Path::Rendered},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        model::EditTxn t(c.base, c.base.version + 1);
        c.edit(t);
        checkLikeCompiled(c.base, t.commit().value(), c.path);
    }
}

TEST_CASE("an edit is split wherever a value it changes is read (ADR-0093)" *
          doctest::description("rt")) {
    // Each split case changes values in both executors through one branch of
    // changedExecutors alone: a VCA mute read by a live member, a send whose
    // owner is live, a hardware MIDI route read by its live source. Each
    // compiles in the anticipative executor and equals the compiled reload.
    // A key into the live track is not one: it carries its source's output,
    // after the fader and VCAs the renderer applies there.
    const Wet w = wetSession();
    const Mix& m = w.m;
    auto with = [&](auto change) {
        model::EditTxn t(m.session, 3);
        change(t);
        return t.commit().value();
    };
    // A second VCA over a rendered track and the live vocal.
    Id mixedVca;
    const model::Session vcaLive = with([&](model::EditTxn& t) {
        model::TrackGroup group;
        group.members = {m.b, w.vocal};
        group.vca = model::Vca{0, false, false, {}, {}};
        mixedVca = t.create(group);
    });
    // The vocal's own encoded send to a buss of its own (live with it).
    Id vocalSend;
    const model::Session sendLive = with([&](model::EditTxn& t) {
        model::Track buss;
        buss.type = model::StripType::Buss;
        buss.parent = kMaster;
        const Id id = t.create(buss);
        t.modify<model::Track>(kMaster, [&](auto& x) { x.children.push_back(id); });
        model::Route r;
        r.from.track = w.vocal;
        r.to.track = id;
        r.volumeDb = -6;  // encoded, so its mute is a value
        vocalSend = t.create(r);
    });
    // a keys a key mix on the vocal.
    const model::Session keyLive = with([&](model::EditTxn& t) {
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.test.keymix";
        const Id keyMix = t.create(fx);
        t.modify<model::Track>(w.vocal, [&](auto& x) { x.chain.push_back(keyMix); });
        model::Route r;
        r.from.track = m.a;
        r.to.fx = keyMix;
        r.volumeDb = -6;
        t.create(r);
    });
    // b plays to a hardware MIDI port as well, which makes b live.
    Id hardware;
    const model::Session hardwareLive = with([&](model::EditTxn& t) {
        model::Route r;
        r.from.track = m.b;
        r.to.midiDevice = "No such port";
        r.midi = model::MidiRouting{};
        hardware = t.create(r);
    });
    using Edit = std::function<void(model::EditTxn&)>;
    struct Case {
        std::string name;
        const model::Session& base;
        Edit edit;
        Path path;
    };
    auto muteA = [&](model::EditTxn& t) {
        t.modify<model::Track>(m.a, [](auto& x) { x.mute = true; });
    };
    const std::vector<Case> cases = {
        {"a VCA mute with a live member", vcaLive,
         [&](model::EditTxn& t) {
             t.modify<model::TrackGroup>(mixedVca, [](auto& g) { g.vca->mute = true; });
         },
         Path::Split},
        {"the live track's send muted with a rendered track", sendLive,
         [&](model::EditTxn& t) {
             t.modify<model::Route>(vocalSend, [](auto& r) { r.mute = true; });
             muteA(t);
         },
         Path::Split},
        {"a VCA mute over a track that keys the live one", keyLive,
         [&](model::EditTxn& t) {
             t.modify<model::TrackGroup>(m.vca, [](auto& g) { g.vca->mute = true; });
         },
         Path::Rendered},
        {"a hardware MIDI route muted with a rendered track", hardwareLive,
         [&](model::EditTxn& t) {
             t.modify<model::Route>(hardware, [](auto& r) { r.mute = true; });
             muteA(t);
         },
         Path::Split},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        model::EditTxn t(c.base, c.base.version + 1);
        c.edit(t);
        checkLikeCompiled(c.base, t.commit().value(), c.path);
    }
}

TEST_CASE("a mute or solo edit compiles where it changes what a plan holds") {
    // A route mute decides an unencoded send's encoding; item mute decides
    // which events a plan holds.
    Mix m = mixSession();
    Id plain;
    {
        model::EditTxn t(m.session, 2);
        model::Route r;  // post-fader, 0 dB, following its source's pan: unencoded
        r.from.track = m.b;
        r.to.track = m.bus;
        plain = t.create(r);
        m.session = t.commit().value();
    }
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(m.session));
    std::uint64_t version = 3;
    auto compiles = [&](auto change) {
        model::EditTxn t(m.session, version++);
        change(t);
        const auto generation = engine.planGeneration();
        const auto edits = engine.mixEdits();
        REQUIRE(engine.load(t.commit().value()));
        const bool compiled = engine.planGeneration() == generation + 1;
        CHECK(engine.mixEdits() == edits + (compiled ? 0 : 1));
        REQUIRE(engine.load(m.session));
        return compiled;
    };
    CHECK_FALSE(compiles(
        [&](auto& t) { t.template modify<model::Track>(m.a, [](auto& x) { x.mute = true; }); }));
    CHECK_FALSE(compiles([&](auto& t) {
        t.template modify<model::Track>(m.a, [](auto& x) { x.solo = model::Solo::Defeat; });
    }));
    CHECK_FALSE(compiles([&](auto& t) {
        t.template modify<model::Route>(m.sendA, [](auto& r) { r.mute = true; });
    }));
    CHECK(compiles(
        [&](auto& t) { t.template modify<model::Route>(plain, [](auto& r) { r.mute = true; }); }));
    CHECK(compiles([&](auto& t) {
        for (const auto& item : m.session.items)
            if (item.value->track == m.a)
                t.template modify<model::Item>(item.id, [](auto& x) { x.mute = true; });
    }));
    CHECK(compiles([&](auto& t) {
        t.template modify<model::Track>(m.a, [](auto& x) {
            x.mute = true;
            x.name = "renamed";  // with anything else, the whole edit compiles
        });
    }));
    engine.stop();
}

TEST_CASE("pan controls: held writing width edits skip compilation and release the stale curve" *
          doctest::description("rt")) {
    Mix m = mixSession();
    model::EditTxn initial(m.session, 2);
    initial.modify<model::Track>(m.a, [](auto& tr) { tr.panMode = model::PanMode::Width; });
    model::Envelope env;
    env.target = {m.a, "width"};
    env.mode = model::EnvMode::Latch;
    env.points = {{0, -100}, {4, 100}};
    const Id lane = initial.create(env);
    const auto base = initial.commit().value();
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(base));
    engine.play();
    backend.run(4);
    const auto generation = engine.planGeneration();
    CHECK_FALSE(engine.setWriteHolds({env.target}));
    model::EditTxn move(base, 3);
    move.modify<model::Track>(m.a, [](auto& tr) { tr.width = 35; });
    move.modify<model::Envelope>(lane, [](auto& e) { e.points.push_back({8, 35}); });
    const auto written = move.commit().value();
    REQUIRE(engine.load(written));
    CHECK(engine.planGeneration() == generation);
    CHECK(engine.mixEdits() == 1);
    backend.run(4);
    CHECK(engine.setWriteHolds({}));
    REQUIRE(engine.load(written));
    CHECK(engine.planGeneration() == generation + 1);
    engine.stop();
}

TEST_CASE("pan controls: touch latch and write record positions and effective compensation") {
    for (const char* mode : {"touch", "latch", "latch_preview", "write"})
        for (const std::string param : {"width", "pan_left", "pan_right", "pan_boost"}) {
            CAPTURE(mode);
            CAPTURE(param);
            WriteHost h(mode);
            h.run("project.set", R"({"pan_law":{"db":-3}})");
            h.run("env.add",
                  R"({"track":"tr_1","param":")" + param + R"(","points":[{"beat":0,"value":0}]})");
            h.run("env.set", R"({"env":"en_2","mode":")" + std::string(mode) + "\"}");
            const auto generation = h.engine().planGeneration();
            h.run("track.set", param == "pan_boost"
                                   ? R"({"track":"tr_1","pan_law":{"db":-3,"boost":true}})"
                                   : R"({"track":"tr_1",")" + param + R"(":25})");
            const model::EnvTarget target{{model::Kind::Track, 1}, param};
            CHECK(h.engine().writeHolds() == std::vector<model::EnvTarget>{target});
            if (param != "pan_boost") CHECK(h.engine().planGeneration() == generation);
            const auto* env = h.app->head().envelopes.find({model::Kind::Envelope, 2});
            REQUIRE(env);
            CHECK(env->points.back().value == (param == "pan_boost" ? 1 : 25));
            if (param == "pan_boost") {
                h.run("track.set", R"({"track":"tr_1","pan_law":null})");
                CHECK(
                    h.app->head().envelopes.find({model::Kind::Envelope, 2})->points.back().value ==
                    0);
            }
            h.backend.run(2);
            if (std::string_view(mode) == "touch") {
                h.app->setFixedClock(10'501);
                h.app->tick();
                CHECK(h.engine().writeHolds().empty());
            }
            h.run("transport.pause");
            CHECK(h.engine().writeHolds().empty());
        }
}
