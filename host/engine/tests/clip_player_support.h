// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// What the clip launcher's player tests share (test_clip_player.cpp and
// test_clip_player_rate.cpp, which runs as its own ctest entry beside it).
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "bridge_patience.h"
#include "plan.h"
#include "realtime_common.h"

namespace clip_player_test {
using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

inline const cmd::Origin kOrigin{cmd::Origin::Type::Test, "clip player", ""};
inline json::Value j(std::string_view text) { return json::parse(text).value(); }
inline model::Id id(const char* text) { return *model::Id::parse(text); }

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

inline LaunchLogEntry launch(std::int64_t frame, const char* clip) {
    LaunchLogEntry e;
    e.runningFrame = frame;
    e.track = id("tr_1");
    e.action = LaunchLogEntry::Action::Clip;
    e.clip = id(clip);
    e.anchorSample = frame;
    e.anchorBeat = double(frame) / double(kBeat);
    return e;
}
inline LaunchLogEntry back(std::int64_t frame, LaunchLogEntry::Action action) {
    LaunchLogEntry e = launch(frame, "cl_1");
    e.action = action;
    e.clip = {};
    return e;
}

inline float rampAt(std::int64_t k) { return float(k + 1) / float(1 << 20); }

// A live run from 0: launches made through Engine::launch at given cycles,
// the master's output, its latency and the engine's own launch log. With
// `loop` (timeline seconds) the transport loops there.
using Loop = std::optional<std::pair<double, double>>;
// Round 66's looped variant: bars 1.5-2.5. A clip launched on bar 2 plays
// into the wrap, after which the song is before its anchor, and a launch
// decided after bar 2 finds its next bar past the loop end: the song wraps
// before that anchor too, and it lands on bar 2 of the next pass.
inline const Loop kWrapBeforeAnchor = std::pair{1.0, 3.0};
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
inline Live playLive(const model::Session& s, const fs::path& dir, const EngineOptions& options,
                     std::uint32_t block, std::int64_t frames, const std::vector<Step>& steps,
                     const Loop& loop = {}, double start = 0.0) {
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
    REQUIRE(engine.load(s, dir));
    if (loop) engine.setLoop(loop->first, loop->second, true);
    if (start > 0.0) engine.seek(start);
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

inline Engine::LaunchRequest request(const char* track, const char* clip) {
    Engine::LaunchRequest r;
    r.track = id(track);
    r.clip = id(clip);
    return r;
}
inline Engine::LaunchRequest backTo(const char* track) {
    Engine::LaunchRequest r;
    r.track = id(track);
    r.action = LaunchLogEntry::Action::Arrangement;
    return r;
}

// A MIDI clip on tr_1 (the sine), an audio clip on tr_2, arrangement on
// both; tr_2's latent gain shifts tr_1 (its node switches later in the
// running frame than tr_2's).
inline void twoTracks(Project& p) {
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

// tr_1 plays the sine: four bars of whole notes, and a clip of two eighth
// notes a bar. tr_2's rack turns tr_1's notes into its gate: a Notes module
// sourcing tr_1 (with a device, which makes the rack live, so the anticipative
// engine runs it in the callback while tr_1 stays with the renderer).
inline void rackFollows(Project& p, bool device) {
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
inline std::vector<float> gates(Project& p, double seconds, std::vector<LaunchLogEntry> log) {
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

inline std::string takeOf(Project& p, const char* clip) {
    return p.app->head().clips.find(id(clip))->activeTake->str();
}

// tr_1's audio clip cl_1 (the ramp, 0.25 s) whose take has the gain fixture
// at -6 dB with 64 samples of latency.
inline void latentClip(Project& p) {
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("ramp.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("clip.set", R"({"clip": "cl_1", "length": 0.25})");  // 12000 frames
    auto fx = j(R"({"plugin": "com.suppleaudio.test.gain"})");
    fx["take"] = takeOf(p, "cl_1");
    const std::string fxId = p.runArgs("fx.add", fx)["fx"].asString();
    auto patch = j(R"({"patch": {"params": {"latency": "short", "gain": -6}}})");
    patch["fx"] = fxId;
    p.runArgs("fx.patch.merge", patch);
}
}  // namespace clip_player_test
