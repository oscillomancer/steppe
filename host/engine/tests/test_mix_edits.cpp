// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Mix-value edits on the running plan (ADR-0060) and automation write holds
// (ADR-0061): dummy devices and the in-process fixture instrument only.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

#include "supple/engine/app.h"
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
