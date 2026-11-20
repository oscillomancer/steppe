// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's players (ADR-0097, engine.md §1.4.2), offline: a
// launch log replayed by renderOffline. A clip's phase is a pure function of
// the song position and its anchor; a switch ends the notes left sounding
// and crossfades audio over 5 ms.
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>

#include <chrono>
#include <cmath>
#include <filesystem>

#include "bridge_patience.h"
#include "plan.h"
#include "realtime_common.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace supple::engine {
// test_launch_access.cpp (EngineTestAccess): runs `hook` in launch() between
// publishing a launch and deciding it.
void launchCommitHookForTests(Engine& e, void (*hook)(void*), void* context);
// The slots the executors still act on (Engine::launchesQueued_).
std::uint32_t launchesQueuedForTests(const Engine& e);
}  // namespace supple::engine

namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "clip player", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }
model::Id id(const char* text) { return *model::Id::parse(text); }

// A project at 120 BPM (a beat is 24000 samples) with channel tr_1 and
// scenes sc_1 and sc_2, no Console 9.
struct Project {
    fs::path root = fs::temp_directory_path() /
                    ("supple-player-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;

    Project() {
        fs::remove_all(root);
        fs::create_directories(root);
        AppOptions options;
        options.workdir = root;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto made = EngineApp::create(options, {.console9 = false});
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name": "Bass"})");
        run("scene.add", R"({"name": "A"})");
        run("scene.add", R"({"name": "B"})");
    }
    ~Project() { fs::remove_all(root); }

    json::Value run(std::string name, std::string_view args) {
        auto result = bus->execute({std::move(name), j(args), kOrigin});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    }
    json::Value runArgs(std::string name, const json::Value& args) {
        auto result = bus->execute({std::move(name), args, kOrigin});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    }
    // A mono file whose frame k holds (k + 1) / 2^20: each frame says where
    // in the source it is.
    fs::path ramp(const char* name, std::size_t frames) {
        audio::AudioData a;
        a.sampleRate = 48000;
        a.channels.assign(1, std::vector<float>(frames));
        for (std::size_t k = 0; k < frames; ++k) a.channels[0][k] = float(k + 1) / float(1 << 20);
        const auto path = root / name;
        REQUIRE(audio::writeWav(path, a));
        return path;
    }
    // tr_1 before its fader, from 0 for `seconds`, replaying `log`.
    std::vector<float> render(double seconds, std::vector<LaunchLogEntry> log,
                              std::uint32_t block = 512) {
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = seconds;
        settings.blockSize = block;
        settings.projectDir = app->projectDir();
        settings.track = id("tr_1");
        settings.tap = model::Tap::PreFader;
        settings.launches = std::move(log);
        auto r = renderOffline(app->head(), pool, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return r->audio.channels[0];
    }
};

constexpr std::int64_t kBeat = 24000;
constexpr std::int64_t kFade = 240;  // 5 ms at 48 kHz

LaunchLogEntry launch(std::int64_t frame, const char* clip) {
    LaunchLogEntry e;
    e.runningFrame = frame;
    e.track = id("tr_1");
    e.action = LaunchLogEntry::Action::Clip;
    e.clip = id(clip);
    e.anchorSample = frame;
    e.anchorBeat = double(frame) / double(kBeat);
    return e;
}
LaunchLogEntry back(std::int64_t frame, LaunchLogEntry::Action action) {
    LaunchLogEntry e = launch(frame, "cl_1");
    e.action = action;
    e.clip = {};
    return e;
}

float rampAt(std::int64_t k) { return float(k + 1) / float(1 << 20); }

// A live run from 0: launches made through Engine::launch at given cycles,
// the master's output, its latency and the engine's own launch log.
struct Step {
    std::int64_t cycle;
    std::vector<Engine::LaunchRequest> requests;
    model::LaunchQuantize quantize = model::LaunchQuantize::Bar;
    const model::Session* load = nullptr;  // instead: an edit reaches the engine
};
struct Live {
    std::vector<float> left;
    std::uint32_t latency = 0;
    std::vector<LaunchLogEntry> log;
};
Live playLive(const model::Session& s, const fs::path& dir, const EngineOptions& options,
              std::uint32_t block, std::int64_t frames, const std::vector<Step>& steps) {
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
    REQUIRE(engine.load(s, dir));
    engine.play();
    const auto run = [&](std::int64_t cycles) {
        if (cycles <= 0) return;
        if (options.anticipative)
            runAheadOnState(engine, backend, cycles, block);
        else
            backend.run(cycles);
    };
    std::int64_t done = 0;
    for (const Step& step : steps) {
        run(step.cycle - done);
        done = std::max(done, step.cycle);
        engine.service();
        if (step.load) {
            REQUIRE(engine.load(*step.load, dir));
            continue;
        }
        auto placed = engine.launch(step.requests, step.quantize);
        REQUIRE_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
    }
    run(frames / block - done);
    engine.service();
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.bridgeLateCycles() == 0);  // a late live block is silent by design
    engine.stop();
    CHECK(engine.processErrors() == 0);
    return {backend.capture().channels[0], engine.outputLatency(), engine.launchLog()};
}

Engine::LaunchRequest request(const char* track, const char* clip) {
    Engine::LaunchRequest r;
    r.track = id(track);
    r.clip = id(clip);
    return r;
}
Engine::LaunchRequest backTo(const char* track) {
    Engine::LaunchRequest r;
    r.track = id(track);
    r.action = LaunchLogEntry::Action::Arrangement;
    return r;
}

// A MIDI clip on tr_1 (the sine), an audio clip on tr_2, arrangement on
// both; tr_2's latent gain shifts tr_1 (its node switches later in the
// running frame than tr_2's).
void twoTracks(Project& p) {
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    p.run("item.add",
          R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: G3:w | A3:w | B3:w"})");
    p.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:e E3:e G3:q r:h"})");
    p.run("track.add", R"({"name": "Loop"})");  // tr_2
    auto args = j(R"({"track": "tr_2", "bar": 1})");
    args["audio_file"] = p.ramp("item.wav", 240000).string();
    p.runArgs("item.add", args);
    args = j(R"({"track": "tr_2", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("clip.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("clip.set", R"({"clip": "cl_2", "length": 0.75})");
    p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.gain"})");
    p.run("fx.patch.merge", R"({"fx": "fx_2", "patch": {"params": {"latency": "short"}}})");
}

}  // namespace

TEST_CASE("an audio clip plays from its anchor, its phase the song position's") {
    Project p;
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("ramp.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("clip.set", R"({"clip": "cl_1", "length": 0.25})");  // 12000 frames
    const std::int64_t anchor = 2 * kBeat;
    const auto out = p.render(2.0, {launch(anchor, "cl_1")});
    // Silent before the launch: the arrangement is empty.
    for (std::int64_t t = 0; t < anchor; ++t) REQUIRE(out[std::size_t(t)] == 0.0f);
    // A 5 ms fade in, then the content by phase, period after period.
    CHECK(out[std::size_t(anchor)] == 0.0f);
    for (std::int64_t t = anchor + kFade; t < 96000; ++t) {
        const std::int64_t phase = (t - anchor) % 12000;
        REQUIRE_MESSAGE(out[std::size_t(t)] == rampAt(phase), "frame " << t);
    }
    CHECK(out[std::size_t(anchor + kFade / 2)] ==
          doctest::Approx(0.5 * double(rampAt(kFade / 2))).epsilon(1e-6));
}

TEST_CASE("a once clip plays its period and falls silent; a loop window repeats its content") {
    Project p;
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("ramp.wav", 48000).string();
    p.runArgs("clip.add", args);
    SUBCASE("once") {
        p.run("clip.set", R"({"clip": "cl_1", "length": 0.25, "mode": "once"})");
        const auto out = p.render(1.5, {launch(kBeat, "cl_1")});
        CHECK(out[std::size_t(kBeat + 11999)] == rampAt(11999));
        for (std::int64_t t = kBeat + 12000; t < 72000; ++t) REQUIRE(out[std::size_t(t)] == 0.0f);
    }
    SUBCASE("a loop window") {
        // 0.5 s period; the window 0.1 s to 0.15 s (frames 4800 to 7200) repeats.
        p.run(
            "clip.set",
            R"({"clip": "cl_1", "length": 0.5, "loop": {"on": true, "start": 0.1, "length": 0.05}})");
        const auto out = p.render(1.5, {launch(kBeat, "cl_1")});
        for (std::int64_t t = kBeat + kFade; t < 72000; ++t) {
            const std::int64_t phase = (t - kBeat) % 24000;
            REQUIRE_MESSAGE(out[std::size_t(t)] == rampAt(4800 + phase % 2400), "frame " << t);
        }
    }
}

TEST_CASE("a loop clip keeps its phase when the song wraps back before its anchor" *
          doctest::description("rt")) {
    // A transport loop over bars 1-2 (0 to 4 s) and an audio clip of 0.75 s
    // launched on bar 2: at the wrap the song returns to 0, before the
    // anchor, and the clip plays on at (song - anchor) mod period (L-c).
    Project p;
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("clip.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("clip.set", R"({"clip": "cl_1", "length": 0.75})");
    const model::Session s = p.app->head();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 6});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.setLoop(0.0, 4.0, true);
        engine.play();
        const auto run = [&](std::int64_t cycles) {
            if (options.anticipative)
                runAheadOnState(engine, backend, cycles, 128);
            else
                backend.run(cycles);
        };
        run(10);
        const Engine::LaunchRequest r = request("tr_1", "cl_1");
        REQUIRE(engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));  // bar 2
        run(48000 * 5 / 128 - 10);
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        const std::uint32_t latency = engine.outputLatency();
        engine.stop();
        const std::vector<float>& out = backend.capture().channels[0];
        // The gain from the clip to the output, where its phase is known.
        const double gain = double(out[96000 + 1000 + latency]) / double(rampAt(1000));
        REQUIRE(gain > 0.1);
        for (std::int64_t k = 192000 + 97; k < 240000; k += 997) {
            const std::int64_t phase = ((k - 192000 - 96000) % 36000 + 36000) % 36000;
            INFO("frame " << k << ", phase " << phase);
            CHECK(double(out[std::size_t(k + latency)]) ==
                  doctest::Approx(double(rampAt(phase)) * gain).epsilon(1e-5));
        }
    }
}

TEST_CASE("a switch crossfades the arrangement out and the clip in over 5 ms") {
    Project p;
    auto args = j(R"({"track": "tr_1", "bar": 1})");
    args["audio_file"] = p.ramp("item.wav", 96000).string();
    p.runArgs("item.add", args);
    args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("clip.wav", 48000).string();
    p.runArgs("clip.add", args);
    const std::int64_t at = kBeat;
    const auto out =
        p.render(1.5, {launch(at, "cl_1"), back(48000, LaunchLogEntry::Action::Arrangement)});
    for (std::int64_t t = 0; t < at; ++t) REQUIRE(out[std::size_t(t)] == rampAt(t));
    for (std::int64_t k = 0; k < kFade; k += 7) {
        const double x = double(k) / double(kFade);
        const double want = double(rampAt(at + k)) * (1 - x) + double(rampAt(k)) * x;
        REQUIRE_MESSAGE(double(out[std::size_t(at + k)]) == doctest::Approx(want).epsilon(1e-5),
                        "frame " << at + k);
    }
    CHECK(out[std::size_t(at + kFade + 10)] == rampAt(kFade + 10));
    // Back to the arrangement at 1 s: it continues where the song is.
    CHECK(out[std::size_t(48000 + kFade + 10)] == rampAt(48000 + kFade + 10));
}

TEST_CASE("a MIDI clip plays its notes by beat from its anchor; a switch ends held notes") {
    // The sine fixture starts each note at phase 0 (a zero sample at its
    // start) and stops at its note-off: the output shows each note's span.
    Project p;
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    p.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:q r:*"})");
    p.run("item.add", R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: G3:w"})");
    // The arrangement's whole note sounds until the launch at beat 2, which
    // ends it there; the clip's quarter plays at beats 2 and 6 (its period
    // is a bar), and stops the track at beat 8.
    const auto out =
        p.render(5.0, {launch(2 * kBeat, "cl_1"), back(8 * kBeat, LaunchLogEntry::Action::Stop)});
    CHECK(out[1] != 0.0f);
    CHECK(out[std::size_t(2 * kBeat - 1)] != 0.0f);
    CHECK(out[std::size_t(2 * kBeat)] == 0.0f);  // G3 ended, C3 starts at phase 0
    CHECK(out[std::size_t(2 * kBeat + 1)] != 0.0f);
    CHECK(out[std::size_t(3 * kBeat - 1)] != 0.0f);
    for (std::int64_t t = 3 * kBeat; t < 6 * kBeat; ++t) REQUIRE(out[std::size_t(t)] == 0.0f);
    CHECK(out[std::size_t(6 * kBeat + 1)] != 0.0f);
    for (std::int64_t t = 7 * kBeat; t < 10 * kBeat; ++t) REQUIRE(out[std::size_t(t)] == 0.0f);
}

TEST_CASE("the replay does not depend on the block size") {
    Project p;
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    p.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:e E3:e G3:q r:h"})");
    p.run("item.add", R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: G3:w | A3:w"})");
    const std::vector<LaunchLogEntry> log{
        launch(kBeat + 3, "cl_1"), back(5 * kBeat + 17, LaunchLogEntry::Action::Arrangement)};
    const auto a = p.render(4.0, log, 512), b = p.render(4.0, log, 96);
    REQUIRE(a.size() == b.size());
    std::size_t first = a.size();
    for (std::size_t k = 0; k < a.size() && first == a.size(); ++k)
        if (a[k] != b[k]) first = k;
    CHECK_MESSAGE(first == a.size(), "first difference at frame " << first);
}

TEST_CASE("a channel delayed by compensation switches and plays its clip where its song is") {
    // tr_2's latent gain delays tr_1 by the output latency (its timeline
    // shift). The mix, with the latency taken off as renders do, is the same
    // as without tr_2: the switch and the phase follow tr_1's song position.
    // Audio crossfades by song position, so it holds even with an early
    // switch; MIDI ends its notes at the switch, so it shows one.
    Project p;
    SUBCASE("audio") {
        auto args = j(R"({"track": "tr_1", "bar": 1})");
        args["audio_file"] = p.ramp("item.wav", 96000).string();
        p.runArgs("item.add", args);
        args = j(R"({"track": "tr_1", "scene": "sc_1"})");
        args["audio_file"] = p.ramp("clip.wav", 48000).string();
        p.runArgs("clip.add", args);
        p.run("clip.set", R"({"clip": "cl_1", "length": 0.25})");
    }
    SUBCASE("MIDI") {
        p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
        p.run("item.add", R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: G3:w"})");
        p.run("clip.add",
              R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:q r:*"})");
    }
    const std::string latentFx = p.app->head().fx.size() == 0 ? "fx_1" : "fx_2";
    const std::vector<LaunchLogEntry> log{launch(kBeat, "cl_1"),
                                          back(48000 + 77, LaunchLogEntry::Action::Arrangement)};
    const auto mix = [&] {
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 1.5;
        settings.projectDir = p.app->projectDir();
        settings.launches = log;
        auto r = renderOffline(p.app->head(), pool, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::pair{r->audio.channels[0], r->outputLatency};
    };
    const auto [plain, none] = mix();
    CHECK(none == 0);
    p.run("track.add", R"({"name": "Latent"})");
    p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.gain"})");
    auto merge = j(R"({"patch": {"params": {"latency": "short"}}})");
    merge["fx"] = latentFx;
    p.runArgs("fx.patch.merge", merge);
    const auto [shifted, latency] = mix();
    CHECK(latency > 0);
    REQUIRE(plain.size() == shifted.size());
    std::size_t first = plain.size();
    for (std::size_t k = 0; k < plain.size() && first == plain.size(); ++k)
        if (plain[k] != shifted[k]) first = k;
    CHECK_MESSAGE(first == plain.size(), "first difference at frame " << first);
    CHECK(plain[std::size_t(kBeat + kFade + 5)] != 0.0f);
}

TEST_CASE("live launches play as the offline replay of their log, in both executors" *
          doctest::description("rt")) {
    // A scene launch (both tracks, one target) on bar 2, then tr_1 back to
    // the arrangement and tr_2 stopped on bar 3.
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const std::uint32_t block : {128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("block " << block << ", anticipative " << options.anticipative);
            Engine::LaunchRequest stop = backTo("tr_2");
            stop.action = LaunchLogEntry::Action::Stop;
            const std::vector<Step> steps{{20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                                          {(96000 + 4000) / block, {backTo("tr_1"), stop}}};
            const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
            REQUIRE(live.log.size() == 4);
            CHECK(live.log[0].anchorSample == 96000);  // bar 2
            CHECK(live.log[0].runningFrame == 96000);
            CHECK(live.log[1].anchorSample == 96000);   // one target for the scene
            CHECK(live.log[2].anchorSample == 192000);  // bar 3
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 5.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 5) == 0.0);
            // The launches changed the output: equality says something.
            settings.launches.clear();
            auto plain = renderOffline(s, pool, settings);
            REQUIRE(plain);
            CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 5) > 0.0);
        }
}

TEST_CASE("live launches through the bridge play as the offline replay" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    Project p;
    twoTracks(p);
    p.run("fx.set", R"({"fx": "fx_1", "isolation": "shared_bridge"})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 3 + 8192;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const std::vector<Step> steps{{10, {request("tr_1", "cl_1")}}};
        const Live live =
            playLive(s, p.app->projectDir(), test::patient(options), 128, frames, steps);
        REQUIRE(live.log.size() == 1);
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 3.0;
        settings.projectDir = p.app->projectDir();
        settings.launches = live.log;
        auto offline = renderOffline(s, pool, settings);
        REQUIRE(offline);
        CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 3) == 0.0);
    }
}

TEST_CASE("a launch waits for its boundary, and one waiting at a relocation is placed again") {
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000 * 3});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(s, p.app->projectDir()));
    engine.play();
    backend.run(10);
    const Engine::LaunchRequest r = request("tr_1", "cl_1");
    SUBCASE("at the floor") {
        auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::None);
        REQUIRE(placed);
        // One device block past the next: the callback may already be in it.
        CHECK(placed->stream == 11 * 128);
        CHECK(placed->anchorSample == 11 * 128);
        backend.run(1);
        engine.service();
        CHECK(engine.launchLog().empty());
        CHECK(engine.launchesPending().size() == 1);
        backend.run(1);
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        CHECK(engine.launchLog()[0].runningFrame == 11 * 128);
        CHECK(engine.launchesPending().empty());
    }
    SUBCASE("a seek before its bar") {
        auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar);
        REQUIRE(placed);
        CHECK(placed->anchorSample == 96000);
        engine.seek(1.5);  // song 72000: the new epoch's stream starts at 0
        engine.service();  // not placed yet: the callback has not relocated
        backend.run(1);
        engine.service();  // placed again: bar 2 is 24000 frames into the epoch
        const auto pending = engine.launchesPending();
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].placed.anchorSample == 96000);
        CHECK(pending[0].placed.stream == 24000);
        backend.run(96000 / 128);
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);  // applied once
        CHECK(engine.launchLog()[0].runningFrame == 24000);
        CHECK(engine.launchLog()[0].anchorSample == 96000);
    }
    engine.stop();
}

TEST_CASE("a plan swap keeps what a track plays, and a clip removed returns it to the arrangement" *
          doctest::description("rt")) {
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    p.run("track.add", R"({"name": "Spare"})");  // an edit that changes no sound
    const model::Session spare = p.app->head();
    // tr_2's playing clip goes; another stays, so its node keeps players.
    auto other = j(R"({"track": "tr_2", "scene": "sc_2"})");
    other["audio_file"] = p.ramp("other.wav", 4800).string();
    p.runArgs("clip.add", other);
    p.run("clip.remove", R"({"clip": "cl_2"})");
    const model::Session removed = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    const std::int64_t seconds = 48000;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const Step scene{20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}};
        InstancePool pool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 5.0;
        settings.projectDir = p.app->projectDir();
        {
            const Live live = playLive(s, p.app->projectDir(), options, 128, frames,
                                       {scene, {(96000 + 20000) / 128, {}, {}, &spare}});
            REQUIRE(live.log.size() == 2);
            settings.launches = live.log;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 5 * seconds) == 0.0);
        }
        {
            const Live live = playLive(s, p.app->projectDir(), options, 128, frames,
                                       {scene, {(96000 + 20000) / 128, {}, {}, &removed}});
            // After the swap (well before 3.5 s): tr_1 plays its clip, tr_2
            // its arrangement, as a render of the edit with tr_1's launch.
            settings.launches.clear();
            for (const LaunchLogEntry& e : live.log)
                if (e.track == id("tr_1")) settings.launches.push_back(e);
            REQUIRE(settings.launches.size() == 1);
            auto offline = renderOffline(removed, pool, settings);
            REQUIRE(offline);
            const auto from = std::size_t(3.5 * seconds);
            const std::vector<float> tail(offline->audio.channels[0].begin() + std::ptrdiff_t(from),
                                          offline->audio.channels[0].end());
            CHECK(maxDiff(live.left, live.latency + from, tail, std::size_t(1.5 * seconds)) == 0.0);
            // ... and not the replay of both launches: tr_2 did change.
            settings.launches = live.log;
            auto both = renderOffline(s, pool, settings);
            REQUIRE(both);
            CHECK(maxDiff(live.left, live.latency, both->audio.channels[0], 5 * seconds) > 0.0);
        }
    }
}

TEST_CASE("a launch while stopped applies at the play start" * doctest::description("rt")) {
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    const std::uint32_t block = 128;
    const std::int64_t frames = 48000 * 2 + 8192;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        backend.run(8);  // stopped cycles: nothing plays, the stream holds
        const Engine::LaunchRequest r = request("tr_2", "cl_2");
        auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar);
        REQUIRE(placed);
        CHECK(placed->anchorSample == 0);
        engine.play();
        if (options.anticipative)
            runAheadOnState(engine, backend, frames / block - 8, block);
        else
            backend.run(frames / block - 8);
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        CHECK(engine.launchLog()[0].runningFrame == 0);
        const std::uint32_t latency = engine.outputLatency();
        engine.stop();
        // The capture holds the stopped cycles first.
        std::vector<float> live = backend.capture().channels[0];
        live.erase(live.begin(), live.begin() + 8 * block);
        OfflineSettings settings;
        settings.endSeconds = 1.5;
        settings.projectDir = p.app->projectDir();
        settings.launches = engine.launchLog();
        auto offline = renderOffline(s, pool, settings);
        REQUIRE(offline);
        CHECK(maxDiff(live, latency, offline->audio.channels[0], 72000) == 0.0);
    }
}

TEST_CASE("a queued launch whose clip an edit removes is dropped, not played later") {
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    auto other = j(R"({"track": "tr_2", "scene": "sc_2"})");
    other["audio_file"] = p.ramp("other.wav", 4800).string();
    p.runArgs("clip.add", other);
    p.run("clip.remove", R"({"clip": "cl_2"})");
    const model::Session removed = p.app->head();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000 * 3});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.play();
        const auto run = [&](std::int64_t cycles) {
            if (options.anticipative)
                runAheadOnState(engine, backend, cycles, 128);
            else
                backend.run(cycles);
        };
        run(10);
        const Engine::LaunchRequest r = request("tr_2", "cl_2");
        REQUIRE(engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar));  // bar 2
        REQUIRE(engine.load(removed, p.app->projectDir()));
        engine.service();
        CHECK(engine.launchesPending().empty());
        run(96000 * 2 / 128);
        engine.service();
        CHECK(engine.launchLog().empty());
        engine.stop();
    }
}

TEST_CASE("the launch commands: state, stop, refusals and a scene that skips a frozen track") {
    Project p;
    twoTracks(p);
    p.run("track.add", R"({"name": "Pad"})");  // tr_3: no clips
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    const auto fails = [&](const char* name, std::string_view args) {
        auto r = p.bus->execute({name, j(args), kOrigin});
        REQUIRE_FALSE(r);
        return r.error().code;
    };
    const auto plays = [&](const json::Value& state, const char* track) {
        for (const json::Value& t : state.find("tracks")->asArray())
            if (t.find("track")->asString() == track) return t;
        FAIL("no " << track);
        return json::Value();
    };
    json::Value state = p.run("launch.state", "{}");
    CHECK(state["playing"].asBool() == false);
    CHECK(state["tracks"].size() == 2);  // tr_3 has no clips
    CHECK(plays(state, "tr_1")["plays"].asString() == "arrangement");

    CHECK(fails("launch.stop", R"({"track": "tr_3"})") == "launch.track");
    CHECK(p.run("launch.arrangement", R"({"track": "tr_3"})")["tracks"].size() == 0);

    json::Value done = p.run("launch.clip", R"({"clip": "cl_1"})");  // stopped: starts
    CHECK(done["started"].asBool());
    CHECK(done["beat"].asDouble() == 0);
    backend.run(4);
    p.app->tick();
    state = p.run("launch.state", "{}");
    CHECK(state["playing"].asBool());
    CHECK(plays(state, "tr_1")["plays"].asString() == "clip");
    CHECK(plays(state, "tr_1")["clip"].asString() == "cl_1");
    CHECK(plays(state, "tr_1")["position"].asDouble() >= 0);

    done = p.run("launch.stop", "{}");  // every track a clip holds: tr_1
    REQUIRE(done["tracks"].size() == 1);
    CHECK(done["tracks"].at(0).find("track")->asString() == "tr_1");
    CHECK(done["beat"].asDouble() == 4);  // bar 2
    CHECK(done["started"].asBool() == false);
    state = p.run("launch.state", "{}");
    CHECK(plays(state, "tr_1")["queued"]["action"].asString() == "stop");
    backend.run(96000 / 128 + 2);
    p.app->tick();
    state = p.run("launch.state", "{}");
    CHECK(plays(state, "tr_1")["plays"].asString() == "stopped");
    CHECK(plays(state, "tr_1").find("queued") == nullptr);

    p.run("track.freeze", R"({"track": "tr_2"})");
    CHECK(fails("launch.clip", R"({"clip": "cl_2"})") == "track.frozen");
    done = p.run("launch.scene", R"({"scene": "sc_1"})");
    REQUIRE(done["tracks"].size() == 1);
    CHECK(done["tracks"].at(0).find("track")->asString() == "tr_1");
    REQUIRE(done.find("warnings"));
    CHECK(done["warnings"].at(0).find("code")->asString() == "track.frozen");
    CHECK(p.run("launch.stop", R"({"track": "tr_2"})")["tracks"].size() == 0);  // frozen
    p.app->stopEngine();
}

TEST_CASE("a render without a launch log plays the arrangement, with no clip warnings") {
    // twoTracks' audio clip is 1.5 beats long: a live plan warns
    // launch.tempo; a render never compiles clips, so it neither warns nor
    // refuses to freeze for it.
    Project p;
    twoTracks(p);
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = 1.0;
    settings.projectDir = p.app->projectDir();
    auto plain = renderOffline(p.app->head(), pool, settings);
    REQUIRE(plain);
    for (const std::string& w : plain->warnings) CHECK(w.find("launch.") == std::string::npos);
    settings.launches = {launch(0, "cl_2")};
    settings.launches[0].track = id("tr_2");
    auto replayed = renderOffline(p.app->head(), pool, settings);
    REQUIRE(replayed);
    CHECK(std::any_of(
        replayed->warnings.begin(), replayed->warnings.end(),
        [](const std::string& w) { return w.find("launch.tempo") != std::string::npos; }));
}

TEST_CASE("launches are refused while a master playrate is active") {
    Project p;
    twoTracks(p);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    p.run("transport.playrate.set", R"({"rate": 0.5})");
    auto r = p.bus->execute({"launch.clip", j(R"({"clip": "cl_1"})"), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.playrate");
    p.app->stopEngine();
}

TEST_CASE("a MIDI clip plays through the track's MIDI send") {
    // tr_1 has no instrument and sends its MIDI to tr_2's sine: a launched
    // clip sounds there, as an item's notes do.
    Project p;
    p.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:h E3:h"})");
    p.run("track.add", R"({"name": "Synth"})");
    p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.sine"})");
    p.run("route.add", R"({"from": "tr_1", "to": "tr_2", "midi": {}})");
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = 2.0;
    settings.projectDir = p.app->projectDir();
    settings.track = id("tr_2");
    settings.tap = model::Tap::PreFader;
    settings.launches = {launch(kBeat, "cl_1")};
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    double peak = 0;
    for (std::size_t k = std::size_t(kBeat); k < r->audio.channels[0].size(); ++k)
        peak = std::max(peak, std::fabs(double(r->audio.channels[0][k])));
    CHECK(peak > 0.01);
    double before = 0;
    for (std::size_t k = 0; k < std::size_t(kBeat); ++k)
        before = std::max(before, std::fabs(double(r->audio.channels[0][k])));
    CHECK(before == 0.0);
    // Live, in both executors, as the offline replay of the engine's log.
    const model::Session s = p.app->head();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        const Live live = playLive(s, p.app->projectDir(), options, 128, 48000 * 2 + 8192,
                                   {{10, {request("tr_1", "cl_1")}}});
        REQUIRE(live.log.size() == 1);
        OfflineSettings mix;
        mix.endSeconds = 2.0;
        mix.projectDir = p.app->projectDir();
        mix.launches = live.log;
        auto offline = renderOffline(s, pool, mix);
        REQUIRE(offline);
        CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 2) == 0.0);
        double heard = 0;
        for (float x : live.left) heard = std::max(heard, std::fabs(double(x)));
        CHECK(heard > 0.01);
    }
}

// --- A rack's Notes module follows its source track (ADR-0097, 2026-11-20) -----

// tr_1 plays the sine: four bars of whole notes, and a clip of two eighth
// notes a bar. tr_2's rack turns tr_1's notes into its gate: a Notes module
// sourcing tr_1 (with a device, which makes the rack live, so the anticipative
// engine runs it in the callback while tr_1 stays with the renderer).
void rackFollows(Project& p, bool device) {
    p.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    p.run("item.add",
          R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: G3:w | A3:w | B3:w | C4:w"})");
    p.run(
        "clip.add",
        R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:e r:e C3:e r:e r:h"})");
    p.run("track.add", R"({"name": "Gates"})");  // tr_2
    p.run("rack.add", R"({"track": "tr_2"})");   // rk_1, its Out mo_1
    p.run("module.add",
          device ? R"({"rack": "rk_1", "type": "notes", "source": "tr_1", "device": "controller"})"
                 : R"({"rack": "rk_1", "type": "notes", "source": "tr_1"})");  // mo_2
    p.run("cable.add", R"({"from": "mo_2.gate", "to": "mo_1.l"})");
}

// tr_2 (the rack's gate) before its fader, offline, replaying `log`.
std::vector<float> gates(Project& p, double seconds, std::vector<LaunchLogEntry> log) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.projectDir = p.app->projectDir();
    settings.track = id("tr_2");
    settings.tap = model::Tap::PreFader;
    settings.launches = std::move(log);
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

TEST_CASE("a rack's Notes module follows its source track's launches, offline") {
    Project p;
    rackFollows(p, false);
    const std::int64_t bar = 4 * kBeat;
    const std::vector<LaunchLogEntry> log{launch(bar, "cl_1"),
                                          back(2 * bar, LaunchLogEntry::Action::Stop),
                                          back(3 * bar, LaunchLogEntry::Action::Arrangement)};
    const auto g = gates(p, 8.0, log);
    const auto at = [&](double seconds) { return g[std::size_t(seconds * 48000)]; };
    CHECK(at(1.0) == 1.0f);   // the arrangement's G3
    CHECK(at(2.1) == 1.0f);   // the clip's first eighth note
    CHECK(at(2.35) == 0.0f);  // its rest: the arrangement's A3 passes unplayed
    CHECK(at(2.6) == 1.0f);   // its second eighth
    CHECK(at(3.5) == 0.0f);
    CHECK(at(4.5) == 0.0f);  // stopped: B3 passes unplayed
    CHECK(at(6.5) == 1.0f);  // the arrangement again, from C4 at the switch
    // Without the launches the rack plays the arrangement throughout.
    const auto plain = gates(p, 8.0, {});
    CHECK(plain[std::size_t(2.35 * 48000)] == 1.0f);
    CHECK(plain[std::size_t(4.5 * 48000)] == 1.0f);
}

TEST_CASE(
    "a rack's Notes module follows launches live as offline, in both executors, also "
    "with the rack and its source track on different executors" *
    doctest::description("rt")) {
    for (const bool device : {false, true}) {
        Project p;
        rackFollows(p, device);
        const model::Session s = p.app->head();
        {
            // With the device the rack is live and tr_1 is not: in the
            // anticipative engine their launches switch on two executors.
            InstancePool pool = makePool();
            auto plan = detail::compile(s, pool, 0, 128);
            REQUIRE_MESSAGE(plan, (plan ? std::string() : plan.error().toString()));
            for (const detail::Node& n : plan->nodes) {
                if (n.rack) CHECK(n.live == device);
                if (n.id == id("tr_1")) CHECK_FALSE(n.live);
            }
        }
        const std::int64_t frames = 48000 * 7 + 8192;
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("device " << device << ", block " << block << ", anticipative "
                               << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_1");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{{20, {request("tr_1", "cl_1")}},
                                              {(96000 + 4000) / block, {stop}},
                                              {(192000 + 4000) / block, {backTo("tr_1")}}};
                const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
                REQUIRE(live.log.size() == 3);
                CHECK(live.log[0].anchorSample == 96000);
                CHECK(live.log[1].anchorSample == 192000);
                CHECK(live.log[2].anchorSample == 288000);
                InstancePool pool = makePool();
                OfflineSettings settings;
                settings.endSeconds = 7.0;
                settings.projectDir = p.app->projectDir();
                settings.launches = live.log;
                auto offline = renderOffline(s, pool, settings);
                REQUIRE(offline);
                CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 7) ==
                      0.0);
                // The rack's switches changed the mix: equality says something.
                const auto heard = gates(p, 7.0, live.log), plain = gates(p, 7.0, {});
                CHECK(heard != plain);
            }
    }
}

TEST_CASE("an applied launch frees its slot once every executor is done with it" *
          doctest::description("rt")) {
    // Each executor class marks an applied slot done, even one that runs
    // none of its targets (the callback of a plan with nothing live), so the
    // queue empties and its slots come back (else launch.queue, at last).
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    constexpr std::uint32_t kBlock = 512;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 4});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.play();
        const auto run = [&](std::int64_t cycles) {
            if (options.anticipative)
                runAheadOnState(engine, backend, cycles, kBlock);
            else
                backend.run(cycles);
            engine.service();
        };
        run(10);
        const std::vector<Engine::LaunchRequest> scene{request("tr_1", "cl_1"),
                                                       request("tr_2", "cl_2")};
        REQUIRE(engine.launch(scene, model::LaunchQuantize::Beat));
        run(48000 / kBlock);
        REQUIRE(engine.launchLog().size() == 2);
        CHECK(launchesQueuedForTests(engine) == 0);
        engine.stop();
    }
}

TEST_CASE("a launch missed before it is decided lands on neither the track nor its rack" *
          doctest::description("rt")) {
    // The main thread stalls between publishing a launch and deciding it,
    // while the executors play past its bar. The executor that reaches a
    // frame of it undecided marks it missed: nothing switches there. Placed
    // again, it switches the track and the rack together on the next bar.
    for (const bool device : {false, true}) {
        Project p;
        rackFollows(p, device);
        const model::Session s = p.app->head();
        constexpr std::uint32_t kBlock = 128;
        const std::int64_t frames = 48000 * 7 + 8192;
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("device " << device << ", anticipative " << options.anticipative);
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
            REQUIRE(engine.load(s, p.app->projectDir()));
            engine.play();
            struct Stall {
                Engine* engine;
                DummyBackend* backend;
                bool anticipative;
                std::int64_t cycles;
            } stall{&engine, &backend, options.anticipative, (96000 + 8192) / kBlock - 20};
            const auto run = [](Stall& st, std::int64_t cycles) {
                if (st.anticipative)
                    runAheadOnState(*st.engine, *st.backend, cycles, kBlock);
                else
                    st.backend->run(cycles);
            };
            run(stall, 20);
            launchCommitHookForTests(
                engine,
                [](void* context) {
                    auto& st = *static_cast<Stall*>(context);
                    if (st.anticipative)
                        runAheadOnState(*st.engine, *st.backend, st.cycles, kBlock);
                    else
                        st.backend->run(st.cycles);
                },
                &stall);
            const Engine::LaunchRequest r = request("tr_1", "cl_1");
            auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar);
            launchCommitHookForTests(engine, nullptr, nullptr);
            REQUIRE(placed);
            CHECK(placed->anchorSample == 96000);
            engine.service();  // missed: placed again, on bar 3
            const auto pending = engine.launchesPending();
            REQUIRE(pending.size() == 1);
            CHECK(pending[0].placed.anchorSample == 192000);
            run(stall, frames / kBlock - 20 - stall.cycles);
            engine.service();
            engine.stop();
            CHECK(engine.processErrors() == 0);
            CHECK(engine.anticipationMisses() == 0);
            const auto log = engine.launchLog();
            REQUIRE(log.size() == 1);  // applied once, on bar 3
            CHECK(log[0].anchorSample == 192000);
            OfflineSettings settings;
            settings.endSeconds = 7.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = log;
            InstancePool offlinePool = makePool();
            auto offline = renderOffline(s, offlinePool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(backend.capture().channels[0], engine.outputLatency(),
                          offline->audio.channels[0], 48000 * 7) == 0.0);
        }
    }
}

TEST_CASE(
    "a launch and a capture armed while stopped begin where the song starts after a "
    "count-in" *
    doctest::description("rt")) {
    // The count holds the transport (heard as stopped, with its clicks), so
    // the stream is still at the play position when the song starts: a
    // launch placed while stopped switches there, and a capture armed then
    // begins there, the count-in not counted.
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    constexpr std::uint32_t kBlock = 333;  // a cycle splits where the count ends
    constexpr std::int64_t kBar = 96000, kSong = 48000 * 3;
    const std::int64_t cycles = (kBar + kSong + 8192) / kBlock;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = cycles * kBlock});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.setMetronome(false, 0.0);  // the count clicks regardless
        engine.captureLaunches(true);
        const std::vector<Engine::LaunchRequest> scene{request("tr_1", "cl_1"),
                                                       request("tr_2", "cl_2")};
        auto placed = engine.launch(scene, model::LaunchQuantize::Bar);
        REQUIRE(placed);
        CHECK(placed->stream == 0);  // stopped: at the play position, unquantized
        CHECK(placed->anchorSample == 0);
        engine.playCountIn(kBar, kBar / 4, 4);
        if (options.anticipative)
            runAheadOnState(engine, backend, cycles, kBlock);
        else
            backend.run(cycles);
        engine.service();
        const auto log = engine.launchLog();
        REQUIRE(log.size() == 2);
        for (const LaunchLogEntry& e : log) {
            CHECK(e.runningFrame == 0);
            CHECK(e.anchorSample == 0);
        }
        auto capture = engine.launchCapture();
        REQUIRE(capture);
        CHECK(capture->on);
        CHECK(capture->startSample == 0);
        CHECK(capture->frames > kSong - 8192);
        CHECK(capture->frames <= cycles * kBlock - kBar);
        REQUIRE(capture->launches.size() == 2);
        for (const LaunchLogEntry& e : capture->launches) {
            CHECK(e.runningFrame == 0);
            CHECK(e.action == LaunchLogEntry::Action::Clip);
        }
        engine.stop();
        CHECK(engine.processErrors() == 0);
        // After the count the song plays as the offline replay of the log.
        InstancePool offlinePool = makePool();
        OfflineSettings settings;
        settings.endSeconds = 3.0;
        settings.projectDir = p.app->projectDir();
        settings.launches = log;
        auto offline = renderOffline(s, offlinePool, settings);
        REQUIRE(offline);
        CHECK(maxDiff(backend.capture().channels[0], engine.outputLatency() + kBar,
                      offline->audio.channels[0], kSong) == 0.0);
        settings.launches.clear();
        auto plain = renderOffline(s, offlinePool, settings);
        REQUIRE(plain);
        CHECK(maxDiff(backend.capture().channels[0], engine.outputLatency() + kBar,
                      plain->audio.channels[0], kSong) > 0.0);
    }
}

// --- Capturing a performance and committing it (ADR-0097 L4) ------------------

namespace {

// The app's own engine on a manual backend, driven cycle by cycle in
// either executor, its service run after each stretch.
struct AppLive {
    Project& p;
    EngineOptions options;
    std::uint32_t block;
    DummyBackend backend;
    std::int64_t cycles = 0;
    AppLive(Project& project, const EngineOptions& o, std::uint32_t b, std::int64_t frames)
        : p(project),
          options(o),
          block(b),
          backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames}) {
        REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = block}, options));
    }
    // Runs until `frame` of the stream has played.
    void until(std::int64_t frame) {
        const std::int64_t n = frame / block - cycles;
        if (n > 0) {
            if (options.anticipative)
                runAheadOnState(*p.app->engine(), backend, n, block);
            else
                backend.run(n);
            cycles += n;
        }
        p.app->tick();
    }
    std::vector<float> stop() {
        CHECK(p.app->engine()->anticipationMisses() == 0);
        latency = p.app->engine()->outputLatency();
        p.app->stopEngine();
        return backend.capture().channels[0];
    }
    std::uint32_t latency = 0;
};

// The project's arrangement alone (no launches), from 0 for `seconds`.
std::vector<float> arrangement(Project& p, double seconds, std::uint32_t block = 512) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.blockSize = block;
    settings.projectDir = p.app->projectDir();
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// The largest difference between live (its latency taken off) and
// offline over [from, to), and the first frame past `tolerance`.
std::pair<double, std::int64_t> compare(const std::vector<float>& live, std::uint32_t latency,
                                        const std::vector<float>& offline, std::int64_t from,
                                        std::int64_t to, double tolerance = 0) {
    double worst = 0;
    std::int64_t first = -1;
    for (std::int64_t k = from; k < to; ++k) {
        const double d =
            std::fabs(double(live[std::size_t(k + latency)]) - double(offline[std::size_t(k)]));
        if (d > tolerance && first < 0) first = k;
        worst = std::max(worst, d);
    }
    return {worst, first};
}

std::vector<std::string> itemsOn(const model::Session& s, const char* track) {
    std::vector<std::string> out;
    for (const auto& e : s.items)
        if (e.value->track == id(track)) out.push_back(e.id.str());
    return out;
}

}  // namespace

TEST_CASE("a captured performance commits as items that play as it was heard" *
          doctest::description("rt")) {
    // The scene on bar 2 (both tracks, from the arrangement), tr_1 back to
    // the arrangement and tr_2 stopped on bar 3, tr_2's clip again on bar 4:
    // the committed arrangement, rendered offline, is what played live. MIDI
    // is exact; audio is exact outside the 5 ms switch fades, which item
    // fades reproduce to rounding.
    for (const std::uint32_t block : {128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("block " << block << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            const std::int64_t frames = 48000 * 5 + 16384;
            AppLive live(p, options, block, frames);
            CHECK(p.run("launch.capture", R"({"on": true})")["from"].asDouble() == 0);
            REQUIRE(p.app->transport("play"));
            live.until(20 * block);
            p.run("launch.scene", R"({"scene": "sc_1"})");
            live.until(96000 + 4000);
            p.run("launch.arrangement", R"({"track": "tr_1"})");
            p.run("launch.stop", R"({"track": "tr_2"})");
            live.until(192000 + 4000);
            p.run("launch.clip", R"({"clip": "cl_2"})");
            live.until(48000 * 5);
            CHECK(p.run("launch.state", "{}")["capturing"].asBool());
            json::Value done = p.run("launch.commit", "{}");
            CHECK_FALSE(p.run("launch.state", "{}")["capturing"].asBool());
            REQUIRE(done["tracks"].size() == 2);
            CHECK(done["from"].asDouble() == 0);
            CHECK(done["to"].asDouble() >= 9.9);  // about 5 s at 120 BPM
            const std::vector<float> heard = live.stop();
            // To the capture's end, or the last song frame the output holds.
            const std::int64_t end = std::min(std::int64_t(done["to"].asDouble() * double(kBeat)),
                                              live.cycles * block - std::int64_t(live.latency));
            const std::vector<float> committed = arrangement(p, 5.0, block);
            const auto [worst, first] = compare(heard, live.latency, committed, 0, end, 1e-6);
            CHECK_MESSAGE(first < 0, "first difference at frame " << first << ", " << worst);
            // Away from the switches it is exact.
            for (const std::int64_t s :
                 {std::int64_t(0), 96000 + kFade, 192000 + kFade, 288000 + kFade})
                CHECK(
                    compare(heard, live.latency, committed, s, std::min(end, s + 96000 - 2 * kFade))
                        .first == 0.0);
            // The committed arrangement differs from the original one there.
            Project original;
            twoTracks(original);
            CHECK(compare(heard, live.latency, arrangement(original, 5.0, block), 0, end).first >
                  0.0);
        }
}

TEST_CASE("a capture over a loop wrap lays out by running time, each clip at its heard phase" *
          doctest::description("rt")) {
    // Clips only, no arrangement: tr_1 an audio clip of 1.5 beats, tr_2 a
    // MIDI clip of 3 beats, launched on bar 2 under a loop over bars 1-2.
    // At the wrap the song returns to 0 and each clip's phase jumps; the
    // commit lays the performance on past the loop, as heard.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
        args["audio_file"] = p.ramp("clip.wav", 48000).string();
        p.runArgs("clip.add", args);
        p.run("clip.set", R"({"clip": "cl_1", "length": 0.75})");
        p.run("track.add", R"({"name": "Keys"})");
        p.run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.sine"})");
        p.run(
            "clip.add",
            R"({"track": "tr_2", "scene": "sc_1", "notation": "voice k; 1: C4:q E4:q G4:q r:q"})");
        p.run("clip.set", R"({"clip": "cl_2", "length": 3})");
        const std::int64_t frames = 48000 * 7 + 16384;
        AppLive live(p, options, 128, frames);
        p.run("transport.loop", R"({"start_seconds": 0, "end_seconds": 4, "enabled": true})");
        p.run("launch.capture", R"({"on": true})");
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2
        live.until(48000 * 7);
        json::Value done = p.run("launch.commit", "{}");
        const std::vector<float> heard = live.stop();
        p.run("transport.loop", R"({"enabled": false})");
        const std::int64_t end = std::min(std::int64_t(done["to"].asDouble() * double(kBeat)),
                                          live.cycles * 128 - std::int64_t(live.latency));
        CHECK(end > 192000 + 96000);  // past the loop's end: laid out linearly
        const std::vector<float> committed = arrangement(p, 7.0);
        const auto [worst, first] = compare(heard, live.latency, committed, 0, end, 1e-6);
        CHECK_MESSAGE(first < 0, "first difference at frame " << first << ", " << worst);
        CHECK(compare(heard, live.latency, committed, 96000 + kFade, end).first == 0.0);
        // tr_1's piece after the wrap starts where the song's phase put it:
        // (0 - 96000) mod 36000 = 12000 frames into the clip.
        const model::Session s = p.app->head();
        bool found = false;
        for (const auto& e : s.items) {
            if (e.value->track != id("tr_1")) continue;
            const model::ItemSpan span = model::itemSpan(s, *e.value);
            if (std::llround(span.startSec * 48000) != 192000) continue;
            found = true;
            CHECK(s.takes.find(*e.value->activeTake)->startOffset ==
                  doctest::Approx(12000.0 / 48000));
        }
        CHECK(found);
    }
}

TEST_CASE("a capture keeps what plays as it begins and ends at a relocation" *
          doctest::description("rt")) {
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 6});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        std::int64_t played = 0;
        bool stopped = false;
        const auto until = [&](std::int64_t frame) {
            const std::int64_t n = frame / 128 - played;
            if (options.anticipative && !stopped)
                runAheadOnState(engine, backend, n, 128);
            else
                backend.run(n);
            played += n;
            engine.service();
        };
        CHECK_FALSE(engine.launchCapture());
        engine.play();
        until(1280);
        const std::vector<Engine::LaunchRequest> scene{request("tr_1", "cl_1"),
                                                       request("tr_2", "cl_2")};
        REQUIRE(engine.launch(scene, model::LaunchQuantize::Bar));  // bar 2
        until(110000);
        engine.captureLaunches(true);
        auto capture = engine.launchCapture();
        REQUIRE(capture);
        CHECK(capture->on);
        const std::int64_t start = capture->startSample;
        CHECK(start >= 110000 - 4096);
        CHECK(start <= 110000);
        // What each track played as it began, with its own anchor.
        REQUIRE(capture->launches.size() == 2);
        for (const LaunchLogEntry& e : capture->launches) {
            CHECK(e.runningFrame == 0);
            CHECK(e.anchorSample == 96000);
            CHECK(e.action == LaunchLogEntry::Action::Clip);
        }
        const Engine::LaunchRequest back = backTo("tr_1");
        REQUIRE(engine.launch(std::span(&back, 1), model::LaunchQuantize::Bar));  // bar 3
        until(200000);
        capture = engine.launchCapture();
        REQUIRE(capture->launches.size() == 3);
        CHECK(capture->launches[2].runningFrame == 192000 - start);
        CHECK(capture->launches[2].action == LaunchLogEntry::Action::Arrangement);
        CHECK(capture->frames >= 200000 - 4096 - start);
        // A seek ends a capture that has run, where it was heard.
        const std::int64_t frames = capture->frames;
        engine.seek(1.0);
        capture = engine.launchCapture();
        CHECK_FALSE(capture->on);
        CHECK(capture->frames >= frames);
        CHECK(capture->frames <= frames + 4096);
        until(210000);
        CHECK(engine.launchCapture()->frames == capture->frames);
        // One that has not run moves with a relocation: armed while stopped,
        // it begins where playing begins.
        engine.pause();
        stopped = true;
        until(220000);
        engine.captureLaunches(true);
        engine.seek(3.0);
        engine.awaitTransport();  // as transport.seek does: the executor has placed it
        until(230000);
        engine.play();
        stopped = false;
        until(240000);
        capture = engine.launchCapture();
        CHECK(capture->on);
        CHECK(capture->startSample == 144000);
        engine.stop();
    }
}

TEST_CASE("launch.commit: history, undo, pooling and refusals") {
    Project p;
    twoTracks(p);
    const auto fails = [&](const char* name, const json::Value& args) {
        auto r = p.bus->execute({name, args, kOrigin});
        REQUIRE_FALSE(r);
        return r.error();
    };
    CHECK(fails("launch.commit", j("{}")).code == "engine.stopped");
    CHECK(fails("launch.capture", j(R"({"on": true})")).code == "engine.stopped");
    // tr_1 plays cl_1 from bar 2 for a bar: a performance given outright,
    // as history records it.
    const json::Value perf = j(R"({"sample_rate": 48000, "start": 0, "frames": 192000,
        "launches": [{"frame": 96000, "track": "tr_1", "action": "clip", "clip": "cl_1",
                      "anchor": 96000, "anchor_beat": 4}]})");
    json::Value args = json::Value::object();
    args["performance"] = perf;
    const std::size_t before = p.app->head().items.size();
    json::Value done = p.runArgs("launch.commit", args);
    CHECK(done["tracks"].size() == 1);
    CHECK(done["from"].asDouble() == 0);
    CHECK(done["to"].asDouble() == 8);
    // The arrangement item is cut at bar 2, and the clip lies from there.
    const model::Session after = p.app->head();
    std::vector<std::pair<double, double>> spans;
    for (const auto& e : after.items)
        if (e.value->track == id("tr_1")) {
            const model::ItemSpan span = model::itemSpan(after, *e.value);
            spans.push_back({span.startBeat, model::itemEndBeat(after, *e.value)});
        }
    std::sort(spans.begin(), spans.end());
    REQUIRE(spans.size() == 3);  // bar 1, the clip's bar, bars 3 on
    CHECK(spans[0].first == 0);
    CHECK(spans[0].second == doctest::Approx(4));
    CHECK(spans[1].first == doctest::Approx(4));
    CHECK(spans[1].second == doctest::Approx(8));
    CHECK(spans[2].first == doctest::Approx(8));
    // Copied, not pooled: its own MIDI.
    const auto sourceOf = [&](const model::Session& s, const std::string& item) {
        return *s.takes.find(*s.items.find(*model::Id::parse(item))->activeTake)->source;
    };
    const model::Clip& clip = *after.clips.find(id("cl_1"));
    const model::Id clipSource = *after.takes.find(*clip.activeTake)->source;
    CHECK(sourceOf(after, done["items"].at(0).asString()) != clipSource);
    // History records the performance, so a replay needs no engine.
    json::Value log = p.run("log.list", R"({"limit": 1})");
    const json::Value& last = log["entries"].at(log["entries"].size() - 1);
    CHECK(last.find("command")->asString() == "launch.commit");
    const json::Value* logged = last.find("args")->find("performance");
    REQUIRE(logged);
    CHECK(logged->find("frames")->asInt() == 192000);
    CHECK(logged->find("launches")->size() == 1);
    // One undo step.
    p.run("history.undo", "{}");
    CHECK(p.app->head().items.size() == before);
    // Pooled: the item shares the clip's MIDI.
    args["pooled"] = true;
    json::Value pooled = p.runArgs("launch.commit", args);
    CHECK(sourceOf(p.app->head(), pooled["items"].at(0).asString()) == clipSource);
    p.run("history.undo", "{}");
    // A locked item in the way is refused, not cut.
    const std::string arrangementItem = itemsOn(p.app->head(), "tr_1").at(0);
    json::Value lock = j(R"({"locked": true})");
    lock["item"] = arrangementItem;
    p.runArgs("item.set", lock);
    CHECK(fails("launch.commit", args).code == "item.locked");
    CHECK(p.app->head().items.size() == before);
    lock["locked"] = false;
    p.runArgs("item.set", lock);
    // A clip removed since the capture.
    p.run("clip.remove", R"({"clip": "cl_1"})");
    const Error stale = fails("launch.commit", args);
    CHECK(stale.code == "launch.commit");
    CHECK(stale.path == "/args/performance/launches/0/clip");
    // With the engine running and nothing captured.
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    CHECK(fails("launch.commit", j("{}")).code == "launch.capture");
    p.app->stopEngine();
}
