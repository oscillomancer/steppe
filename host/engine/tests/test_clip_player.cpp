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
#include "realtime_common.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

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
