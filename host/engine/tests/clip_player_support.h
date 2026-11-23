// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// What the clip launcher's player tests share (test_clip_player.cpp and
// test_clip_player_rate.cpp, which runs as its own ctest entry beside it).
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "../../../tests/file_platform.h"
#include "bridge_patience.h"
#include "plan.h"
#include "realtime_common.h"

namespace supple::engine {
// test_launch_access.cpp (EngineTestAccess): runs `hook` in launch() between
// publishing a launch and deciding it.
void launchCommitHookForTests(Engine& e, void (*hook)(void*), void* context);
// The slots the executors still act on (Engine::launchesQueued_).
std::uint32_t launchesQueuedForTests(const Engine& e);
}  // namespace supple::engine

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
    fs::path root = supple::test::uniqueTempDir("supple-player-");
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
    // For a failure's message: each step's request frame and where the engine
    // placed it, then the engine's counters as the run ended.
    std::string report;
};
// A launch log, one entry per line, for a failure's message.
inline std::string describe(const std::vector<LaunchLogEntry>& log) {
    std::string out;
    for (const LaunchLogEntry& e : log)
        out += "\n  action " + std::to_string(int(e.action)) + " " + e.track.str() + " " +
               e.clip.str() + ": running frame " + std::to_string(e.runningFrame) +
               ", anchor sample " + std::to_string(e.anchorSample) + ", anchor beat " +
               std::to_string(e.anchorBeat) + ", epoch " + std::to_string(e.epoch);
    return out;
}
// Each of the session's media files and whether it is on disk now, for a
// failure's message (another process's cleanup once removed a fixture's media).
inline std::string mediaPresence(const model::Session& s, const fs::path& projectDir) {
    std::string out;
    for (const auto& m : s.media) {
        fs::path path(m.value->path);
        if (path.is_relative()) {
            const fs::path inMedia = projectDir / s.project->mediaDir / path;
            path = fs::exists(inMedia) ? inMedia : projectDir / path;
        }
        out += "\n  " + path.string() + (fs::exists(path) ? ": present" : ": missing");
    }
    return out.empty() ? std::string("\n  (no media)") : out;
}
inline std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += "\n  " + line;
    return out.empty() ? std::string("\n  (none)") : out;
}
// Where `a` from `aFrom` and `b` differ over `n` frames: the first such frame
// with both values, and how many differ.
inline std::string differences(const std::vector<float>& a, std::size_t aFrom,
                               const std::vector<float>& b, std::size_t n) {
    std::size_t count = 0, first = n;
    for (std::size_t i = 0; i < n; ++i)
        if (a[aFrom + i] != b[i]) {
            if (!count) first = i;
            ++count;
        }
    if (!count) return "no frame differs";
    return "first differing frame " + std::to_string(first) + " (live " +
           std::to_string(a[aFrom + first]) + ", replay " + std::to_string(b[first]) + "), " +
           std::to_string(count) + " of " + std::to_string(n) + " frames differ";
}
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
    std::string report;
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
        report += "\n  step at frame " + std::to_string(step.cycle * block) + ": placed epoch " +
                  std::to_string(placed->epoch) + ", stream " + std::to_string(placed->stream) +
                  ", anchor sample " + std::to_string(placed->anchorSample) + ", anchor beat " +
                  std::to_string(placed->anchorBeat);
    }
    run(frames / block - done);
    engine.service();
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.bridgeLateCycles() == 0);  // a late live block is silent by design
    engine.stop();
    CHECK(engine.processErrors() == 0);
    report += "\n  anticipation misses " + std::to_string(engine.anticipationMisses()) +
              ", bridge late cycles " + std::to_string(engine.bridgeLateCycles()) +
              ", process errors " + std::to_string(engine.processErrors()) +
              ", plug-in output drops " + std::to_string(engine.pluginOutputDrops()) +
              ", control MIDI drops " + std::to_string(engine.controlMidiDrops()) +
              ", modulation deferred " + std::to_string(engine.modulationDeferred()) + ", cycles " +
              std::to_string(engine.cycles());
    return {backend.capture().channels[0], engine.outputLatency(), engine.launchLog(),
            std::move(report)};
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
// The ramp's frame k through latentClip's chain.
inline double chained(std::int64_t k) { return double(rampAt(k)) * std::pow(10.0, -6.0 / 20.0); }
// --- Under a master playrate (test_clip_player_rate*.cpp) ---

// The master playrate the S3 tests play under, the pitch not kept: a
// constant 0.75, or an envelope from 1 down to 0.6 over two bars.
inline void underRate(Project& p, bool envelope) {
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    if (envelope)
        p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                         R"({"beat": 0, "value": 1}, {"beat": 8, "value": 0.6}]})");
}

// One session's offline renders, kept across its live runs: a render is a
// pure function of the session and the log (the offline invariant), so a
// log rendered before need not be rendered again, and the render without
// launches is the same for every run. They took most of these cases' time.
struct Replays {
    std::vector<std::pair<std::vector<LaunchLogEntry>, std::vector<float>>> rendered;
    std::optional<std::vector<float>> plain;
};

// The live run's output against the offline replay of its log, over the
// first `frames` device frames; and the same without the launches, which
// must differ for the equality to say something.
inline void liveIsReplay(const model::Session& s, const fs::path& dir, const Live& live,
                         double seconds, std::size_t frames, Replays& replays) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.projectDir = dir;
    const auto render = [&](const std::vector<LaunchLogEntry>& log) {
        settings.launches = log;
        auto offline = renderOffline(s, pool, settings);
        REQUIRE_MESSAGE(offline, (offline ? std::string() : offline.error().toString()));
        REQUIRE(offline->audio.channels[0].size() >= frames);
        return offline->audio.channels[0];
    };
    auto found = std::find_if(replays.rendered.begin(), replays.rendered.end(),
                              [&](const auto& r) { return r.first == live.log; });
    if (found == replays.rendered.end())
        found = replays.rendered.insert(replays.rendered.end(), {live.log, render(live.log)});
    CHECK(maxDiff(live.left, live.latency, found->second, frames) == 0.0);
    if (!replays.plain) replays.plain = render({});
    CHECK(maxDiff(live.left, live.latency, *replays.plain, frames) > 0.0);
}

// --- Capturing a performance and committing it (ADR-0097 L4) ---

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
inline std::vector<float> arrangement(Project& p, double seconds, std::uint32_t block = 512) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.blockSize = block;
    settings.projectDir = p.app->projectDir();
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// The project's master offline, replaying `log`.
inline std::vector<float> replay(Project& p, double seconds, std::vector<LaunchLogEntry> log,
                                 std::uint32_t block = 512) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.blockSize = block;
    settings.projectDir = p.app->projectDir();
    settings.launches = std::move(log);
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return r->audio.channels[0];
}

// The largest difference between live (its latency taken off) and
// offline over [from, to), and the first frame past `tolerance`.
inline std::pair<double, std::int64_t> compare(const std::vector<float>& live,
                                               std::uint32_t latency,
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

// A capture's commit against what played, over the capture's device frames
// [start, end): the worst within a switch's crossfade (240 song samples,
// with the latent gain's shift, from each launch's running frame), within
// `reach` frames after the start and before the end, where the committed
// items are cut while the performance went on (the tape converter's kernel
// reaches 128 song samples across a cut), and away from both.
struct CommitResidual {
    double fading = 0, edges = 0, away = 0;
};
inline CommitResidual commitResidual(const std::vector<float>& heard, std::uint32_t latency,
                                     const std::vector<float>& committed,
                                     const std::vector<LaunchLogEntry>& log, std::int64_t start,
                                     std::int64_t end, std::int64_t reach) {
    CommitResidual r;
    for (std::int64_t k = start; k < end; ++k) {
        const double d =
            std::fabs(double(heard[std::size_t(k + latency)]) - double(committed[std::size_t(k)]));
        bool fading = false;
        for (const LaunchLogEntry& e : log)
            fading = fading || (k >= e.runningFrame - 256 && k < e.runningFrame + 768);
        double& worst = fading                                  ? r.fading
                        : k < start + reach || k >= end - reach ? r.edges
                                                                : r.away;
        worst = std::max(worst, d);
    }
    return r;
}

}  // namespace clip_player_test
