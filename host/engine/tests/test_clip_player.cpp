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
#include <tuple>

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
// the master's output, its latency and the engine's own launch log. With
// `loop` (timeline seconds) the transport loops there.
using Loop = std::optional<std::pair<double, double>>;
// Round 66's looped variant: bars 1.5-2.5. A clip launched on bar 2 plays
// into the wrap, after which the song is before its anchor, and a launch
// decided after bar 2 finds its next bar past the loop end: the song wraps
// before that anchor too, and it lands on bar 2 of the next pass.
const Loop kWrapBeforeAnchor = std::pair{1.0, 3.0};
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
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("looped " << bool(loop) << ", block " << block << ", anticipative "
                               << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(96000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live =
                    playLive(s, p.app->projectDir(), options, block, frames, steps, loop);
                REQUIRE(live.log.size() == 4);
                CHECK(live.log[0].anchorSample == 96000);  // bar 2
                CHECK(live.log[0].runningFrame == 96000);
                CHECK(live.log[1].anchorSample == 96000);  // one target for the scene
                // Bar 3, or bar 2 again after the wrap at 3 s.
                CHECK(live.log[2].anchorSample == (loop ? 96000 : 192000));
                CHECK(live.log[2].runningFrame == 192000);
                InstancePool pool = makePool();
                OfflineSettings settings;
                settings.endSeconds = 5.0;
                settings.projectDir = p.app->projectDir();
                settings.launches = live.log;
                settings.loop = loop;
                auto offline = renderOffline(s, pool, settings);
                REQUIRE(offline);
                CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 5) ==
                      0.0);
                // The launches changed the output: equality says something.
                settings.launches.clear();
                auto plain = renderOffline(s, pool, settings);
                REQUIRE(plain);
                CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 5) > 0.0);
            }
}

TEST_CASE("a replay from a play start past 0 places the launches as live, looped or not" *
          doctest::description("rt")) {
    // Round 74 F1: the log's anchors are timeline samples, and the replay's
    // plan counts from its start. Played from 2 s, with the loop at 1-3 s
    // (the start after the loop's start) and without it.
    Project p;
    twoTracks(p);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 4 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("looped " << bool(loop) << ", anticipative " << options.anticipative);
            constexpr std::uint32_t block = 128;
            Engine::LaunchRequest stop = backTo("tr_2");
            stop.action = LaunchLogEntry::Action::Stop;
            const std::vector<Step> steps{{20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                                          {(96000 + 4000) / block, {backTo("tr_1"), stop}}};
            const Live live =
                playLive(s, p.app->projectDir(), options, block, frames, steps, loop, 2.0);
            REQUIRE(live.log.size() == 4);
            // Bar 2 after the wrap at 3 s, or bar 3 (4 s) without the loop.
            CHECK(live.log[0].anchorSample == (loop ? 96000 : 192000));
            CHECK(live.log[0].runningFrame == 96000);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.startSeconds = 2.0;
            settings.endSeconds = 6.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            settings.loop = loop;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 4) == 0.0);
            settings.launches.clear();
            auto plain = renderOffline(s, pool, settings);
            REQUIRE(plain);
            CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 4) > 0.0);
        }
}

TEST_CASE("live launches through the bridge play as the offline replay" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    Project p;
    twoTracks(p);
    p.run("fx.set", R"({"fx": "fx_1", "isolation": "shared_bridge"})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 4 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("looped " << bool(loop) << ", anticipative " << options.anticipative);
            const std::vector<Step> steps{{10, {request("tr_1", "cl_1")}}};
            const Live live =
                playLive(s, p.app->projectDir(), test::patient(options), 128, frames, steps, loop);
            REQUIRE(live.log.size() == 1);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 4.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            settings.loop = loop;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 4) == 0.0);
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

TEST_CASE("removing the playing clip, or freezing its track, crossfades to the arrangement" *
          doctest::description("rt")) {
    // A ramp clip on a track whose arrangement is silent: from the block the
    // edit is adopted, the clip fades out over the 5 ms switch fade, by the
    // fade weight, instead of stopping dead.
    for (const char* edit : {"clip.remove", "track.freeze"})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO(std::string(edit) << ", anticipative " << options.anticipative);
            Project p;
            // An item far past the test, so the track can freeze.
            auto item = j(R"({"track": "tr_1", "bar": 20})");
            item["audio_file"] = p.ramp("late.wav", 4800).string();
            p.runArgs("item.add", item);
            auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
            args["audio_file"] = p.ramp("ramp.wav", 48000).string();
            p.runArgs("clip.add", args);
            p.run("clip.set", R"({"clip": "cl_1", "length": 0.25})");  // 12000 frames
            const model::Session s = p.app->head();
            p.run(edit, std::string(edit) == "clip.remove" ? R"({"clip": "cl_1"})"
                                                           : R"({"track": "tr_1"})");
            const model::Session edited = p.app->head();
            const std::int64_t frames = 48000 * 4;
            const std::int64_t at = (96000 + 20000) / 128;  // the edit's cycle
            const Live live = playLive(s, p.app->projectDir(), options, 128, frames,
                                       {{20, {request("tr_1", "cl_1")}}, {at, {}, {}, &edited}});
            const auto out = [&](std::int64_t t) {
                return double(live.left[std::size_t(t + std::int64_t(live.latency))]);
            };
            const auto ratio = [&](std::int64_t t) {
                return out(t) / double(rampAt((t - 96000) % 12000));
            };
            // The fade's end: the first silent frame after the edit.
            std::int64_t z = at * 128;
            while (z < frames - 48000 && out(z) != 0.0) ++z;
            REQUIRE(z < frames - 48000);
            CHECK(ratio(z - 241) == doctest::Approx(1.0).epsilon(1e-4));
            CHECK(ratio(z - 120) == doctest::Approx(0.5).epsilon(0.01));
            CHECK(ratio(z - 1) == doctest::Approx(1.0 / 240).epsilon(0.05));
            for (std::int64_t t = z; t < z + 24000; ++t) REQUIRE(out(t) == 0.0);
        }
}

TEST_CASE(
    "right after a pause, the position, a stopped launch's anchor and a capture's start "
    "are where it paused" *
    doctest::description("rt")) {
    // Review round 70 P3: in the anticipative executor, until the renderer
    // relocated for the pause's epoch, transport() gave that epoch's
    // predecessor's start (0 here), so a launch or a capture armed in the
    // same main-thread turn as the pause was anchored there.
    Project p;
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("ramp.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("clip.set", R"({"clip": "cl_1", "length": 0.25})");
    const model::Session s = p.app->head();
    constexpr std::uint32_t kBlock = 128;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.play();
        for (int done = 0; done < 104; done += 8) {
            if (options.anticipative)
                runAheadOnState(engine, backend, 8, kBlock);
            else
                backend.run(8);
            engine.service();
        }
        const std::int64_t heard = engine.transport().sample;
        REQUIRE(heard > 0);
        engine.pause();  // and at once, in the same turn:
        CHECK(engine.transport().sample == heard);
        REQUIRE(engine.captureLaunches(true));
        const Engine::LaunchRequest r = request("tr_1", "cl_1");
        auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar);
        REQUIRE(placed);
        CHECK(placed->anchorSample == heard);
        const auto capture = engine.launchCapture();
        REQUIRE(capture);
        CHECK(capture->startSample == heard);
        engine.stop();
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

TEST_CASE("an audio clip's launch is refused while the master playrate keeps the pitch") {
    // S3 step 1: launched audio plays by tape only. A MIDI clip launches.
    Project p;
    twoTracks(p);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    p.run("transport.playrate.set", R"({"rate": 0.5, "preserve_pitch": true})");
    REQUIRE(p.app->engine()->awaitPlan());
    auto r = p.bus->execute({"launch.clip", j(R"({"clip": "cl_2"})"), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.playrate");
    CHECK(r.error().message.find("cl_2") != std::string::npos);
    CHECK(p.bus->execute({"launch.clip", j(R"({"clip": "cl_1"})"), kOrigin}));
    // Without the pitch kept it launches.
    p.run("transport.playrate.set", R"({"rate": 0.5, "preserve_pitch": false})");
    REQUIRE(p.app->engine()->awaitPlan());
    backend.run(40);  // (playing: the new plan's clock reads)
    CHECK(p.bus->execute({"launch.clip", j(R"({"clip": "cl_2"})"), kOrigin}));
    p.app->stopEngine();
}

TEST_CASE("launching a clip that follows the context warns launch.unfollowed, once per launch") {
    // ADR-0098: a launched clip plays as written until X4, so live and the timeline differ for
    // a clip whose follow is not none; the answer says so.
    Project p;
    twoTracks(p);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    const auto unfollowed = [](const json::Value& done) {
        int n = 0;
        if (const json::Value* w = done.find("warnings"))
            for (const json::Value& e : w->asArray())
                n += e.find("code")->asString() == "launch.unfollowed";
        return n;
    };
    CHECK(unfollowed(p.run("launch.clip", R"({"clip": "cl_1"})")) == 0);  // follow none
    p.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w", "follow": "key"})");
    json::Value one = p.run("launch.clip", R"({"clip": "cl_1"})");
    REQUIRE(unfollowed(one) == 1);
    CHECK(one["warnings"].at(0).find("message")->asString().find("cl_1") != std::string::npos);
    json::Value scene = p.run("launch.scene", R"({"scene": "sc_1"})");
    CHECK(unfollowed(scene) == 1);  // one for the scene, not one per clip
    const std::string said = scene["warnings"].at(0).find("message")->asString();
    CHECK(said.find("the clip cl_1 follows") != std::string::npos);  // one clip: singular
    p.app->stopEngine();
}

TEST_CASE("a looped replay is refused under a master playrate") {
    // The replay wraps as the live executors do only on the unity path.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.5})");
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = 2.0;
    settings.projectDir = p.app->projectDir();
    settings.loop = std::pair{0.5, 1.5};
    auto r = renderOffline(p.app->head(), pool, settings);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "render.loop");
    settings.applyMasterPlayrate = false;  // the unity path loops
    CHECK(renderOffline(p.app->head(), pool, settings));
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
        for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
            for (const std::uint32_t block : {128u, 512u})
                for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                    INFO("looped " << bool(loop) << ", device " << device << ", block " << block
                                   << ", anticipative " << options.anticipative);
                    Engine::LaunchRequest stop = backTo("tr_1");
                    stop.action = LaunchLogEntry::Action::Stop;
                    const std::vector<Step> steps{{20, {request("tr_1", "cl_1")}},
                                                  {(96000 + 4000) / block, {stop}},
                                                  {(192000 + 4000) / block, {backTo("tr_1")}}};
                    const Live live =
                        playLive(s, p.app->projectDir(), options, block, frames, steps, loop);
                    REQUIRE(live.log.size() == 3);
                    // Looped, each later launch wraps first and lands on bar 2.
                    CHECK(live.log[0].anchorSample == 96000);
                    CHECK(live.log[1].anchorSample == (loop ? 96000 : 192000));
                    CHECK(live.log[2].anchorSample == (loop ? 96000 : 288000));
                    CHECK(live.log[2].runningFrame == 288000);
                    InstancePool pool = makePool();
                    OfflineSettings settings;
                    settings.endSeconds = 7.0;
                    settings.projectDir = p.app->projectDir();
                    settings.launches = live.log;
                    settings.loop = loop;
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
    // again at once by launch() (ADR-0097, revision 2026-11-21), it
    // switches the track and the rack together on the next bar.
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
                bool done = false;  // the first attempt stalls, not the second
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
                    if (st.done) return;
                    st.done = true;
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
            CHECK(placed->anchorSample == 192000);  // missed on bar 2: placed again, on bar 3
            engine.service();
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
        REQUIRE(engine.captureLaunches(true));
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

// --- A clip's take chain (ADR-0097, amended 2026-11-20) -----------------------

namespace {

std::string takeOf(Project& p, const char* clip) {
    return p.app->head().clips.find(id(clip))->activeTake->str();
}

// tr_1's audio clip cl_1 (the ramp, 0.25 s) whose take has the gain fixture
// at -6 dB with 64 samples of latency.
void latentClip(Project& p) {
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
// The ramp's frame k through that chain.
double chained(std::int64_t k) { return double(rampAt(k)) * std::pow(10.0, -6.0 / 20.0); }

}  // namespace

TEST_CASE("a clip with a take chain launched while stopped plays from the play start" *
          doctest::description("rt")) {
    // Review round 69: placed while stopped, the target is the play start
    // and the chain's early half falls before the epoch's first frame. It
    // applies at that first frame, as the replay applies it, the chain
    // warming up through its first frames; it used to count as passed, and
    // the launch was placed again a bar later.
    Project p;
    latentClip(p);
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
        backend.run(8);  // stopped cycles
        const Engine::LaunchRequest r = request("tr_1", "cl_1");
        auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar);
        REQUIRE(placed);
        CHECK(placed->stream == 0);
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
        std::vector<float> live = backend.capture().channels[0];
        live.erase(live.begin(), live.begin() + 8 * block);
        CHECK(double(live[latency + 5000]) == doctest::Approx(chained(5000)).epsilon(1e-5));
        OfflineSettings settings;
        settings.endSeconds = 1.5;
        settings.projectDir = p.app->projectDir();
        settings.launches = engine.launchLog();
        auto offline = renderOffline(s, pool, settings);
        REQUIRE(offline);
        CHECK(maxDiff(live, latency, offline->audio.channels[0], 72000) == 0.0);
    }
}

TEST_CASE("a clip plays through its take's chain, heard at its boundary, not its latency later") {
    Project p;
    latentClip(p);
    const std::int64_t anchor = 2 * kBeat;
    const auto out = p.render(1.5, {launch(anchor, "cl_1")});
    for (std::int64_t t = 0; t < anchor; ++t) REQUIRE(out[std::size_t(t)] == 0.0f);
    // Through its gain; the 64-sample chain ran 64 samples ahead, so its
    // content is on time.
    for (std::int64_t t = anchor + kFade; t < 72000; ++t)
        REQUIRE_MESSAGE(double(out[std::size_t(t)]) ==
                            doctest::Approx(chained((t - anchor) % 12000)).epsilon(1e-6),
                        "frame " << t);
    CHECK(double(out[std::size_t(anchor + kFade / 2)]) ==
          doctest::Approx(0.5 * chained(kFade / 2)).epsilon(1e-5));
}

TEST_CASE(
    "a clip chain's tail is cut at the switch away, and it starts afresh at its next launch") {
    Project p;
    latentClip(p);
    auto args = j(R"({"track": "tr_1", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("other.wav", 48000).string();
    p.runArgs("clip.add", args);  // cl_2: no chain
    const std::vector<LaunchLogEntry> log{launch(kBeat, "cl_1"), launch(2 * kBeat, "cl_2"),
                                          launch(3 * kBeat, "cl_1")};
    const auto out = p.render(2.0, log);
    // cl_2 alone once the crossfade is over: cl_1's chain does not ring on.
    for (std::int64_t t = 2 * kBeat + kFade; t < 3 * kBeat; ++t)
        REQUIRE_MESSAGE(out[std::size_t(t)] == rampAt(t - 2 * kBeat), "frame " << t);
    // cl_1 again from its start, nothing of the last pass in its delay.
    for (std::int64_t t = 3 * kBeat + kFade; t < 3 * kBeat + 12000; ++t)
        REQUIRE_MESSAGE(
            double(out[std::size_t(t)]) == doctest::Approx(chained(t - 3 * kBeat)).epsilon(1e-6),
            "frame " << t);
}

TEST_CASE("a clip's take envelopes are in clip time: the same at any song position and pass") {
    Project p;
    latentClip(p);
    auto env = j(R"({"param": "volume", "points": [{"seconds": 0, "value": 0},
                                                   {"seconds": 0.25, "value": "-12 dB"}]})");
    env["take"] = takeOf(p, "cl_1");
    p.runArgs("env.add", env);
    const std::int64_t a = kBeat, b = 5 * kBeat + 1234;
    const auto x = p.render(4.0, {launch(a, "cl_1")});
    const auto y = p.render(4.0, {launch(b, "cl_1")});
    // Each pass of the 12000-frame period ramps down from 0 dB.
    for (std::int64_t k = kFade; k < 3 * 12000; ++k)
        REQUIRE_MESSAGE(x[std::size_t(a + k)] == y[std::size_t(b + k)], "frame " << k);
    CHECK(double(x[std::size_t(a + 12000 + 6000)]) ==
          doctest::Approx(chained(6000) * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-5));
    // 0 dB again at each pass's start.
    CHECK(double(x[std::size_t(a + 2 * 12000)]) == doctest::Approx(chained(0)).epsilon(1e-6));
    CHECK(double(x[std::size_t(a + 2 * 12000 - 1)]) < chained(11999) * 0.6);
}

TEST_CASE("a MIDI clip plays through an instrument in its take's chain, ending at the switch") {
    Project p;
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:w"})");
    auto fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");
    fx["take"] = takeOf(p, "cl_1");
    p.runArgs("fx.add", fx);
    auto gain = j(R"({"plugin": "com.suppleaudio.test.gain"})");
    gain["take"] = takeOf(p, "cl_1");
    const std::string fxId = p.runArgs("fx.add", gain)["fx"].asString();
    auto patch = j(R"({"patch": {"params": {"latency": "short"}}})");
    patch["fx"] = fxId;
    p.runArgs("fx.patch.merge", patch);
    const std::int64_t at = kBeat, away = 3 * kBeat;
    const auto out =
        p.render(2.0, {launch(at, "cl_1"), back(away, LaunchLogEntry::Action::Arrangement)});
    // Silent before the launch (the track has no instrument of its own) and
    // after the switch away; the note sounds from the boundary on.
    for (std::int64_t t = 0; t < at; ++t) REQUIRE(out[std::size_t(t)] == 0.0f);
    double during = 0;
    for (std::int64_t t = at; t < at + 2000; ++t)
        during = std::max(during, std::fabs(double(out[std::size_t(t)])));
    CHECK(during > 0.01);
    CHECK(out[std::size_t(at - 1)] == 0.0f);
    for (std::int64_t t = away + kFade; t < 96000; ++t)
        REQUIRE_MESSAGE(out[std::size_t(t)] == 0.0f, "frame " << t);
}

TEST_CASE("an envelope on a clip take's FX plays in clip time, pass after pass (C2)") {
    Project p;
    latentClip(p);
    p.run("env.add", R"({"fx": "fx_1", "param": "gain", "points": [{"seconds": 0, "value": 0},
                                                                  {"seconds": 0.25, "value": -24}]})");
    // Anchors on the same phase of the absolute 32-sample automation grid.
    const std::int64_t a = kBeat, b = 5 * kBeat + 39 * 32;
    const auto x = p.render(4.0, {launch(a, "cl_1")});
    const auto y = p.render(4.0, {launch(b, "cl_1")});
    for (std::int64_t k = kFade; k < 3 * 12000; ++k)
        REQUIRE_MESSAGE(x[std::size_t(a + k)] == y[std::size_t(b + k)], "frame " << k);
    // Each pass starts near 0 dB and ends near -24 dB, not where the
    // timeline would put the curve.
    const auto ratio = [&](std::int64_t k) {
        return double(x[std::size_t(a + k)]) / double(rampAt(k % 12000));
    };
    CHECK(ratio(12000 + 600) > 0.7);
    CHECK(ratio(12000 + 11500) < 0.12);
    CHECK(ratio(24000 + 600) > 0.7);
}

TEST_CASE("a MIDI clip's take envelope is in beats from the clip's start, wherever it plays") {
    Project p;
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C3:w"})");
    auto fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");
    fx["take"] = takeOf(p, "cl_1");
    p.runArgs("fx.add", fx);
    auto env = j(R"({"param": "volume", "points": [{"beat": 0, "value": 0},
                                                   {"beat": 2, "value": "-24 dB"}]})");
    env["take"] = takeOf(p, "cl_1");
    p.runArgs("env.add", env);
    const std::int64_t a = kBeat, b = 4 * kBeat;
    const auto x = p.render(4.0, {launch(a, "cl_1")});
    const auto y = p.render(4.0, {launch(b, "cl_1")});
    const auto peak = [](const std::vector<float>& v, std::int64_t from, std::int64_t to) {
        double m = 0;
        for (std::int64_t t = from; t < to; ++t)
            m = std::max(m, std::fabs(double(v[std::size_t(t)])));
        return m;
    };
    for (std::int64_t k = kFade; k < 3 * kBeat; ++k)
        REQUIRE_MESSAGE(x[std::size_t(a + k)] == y[std::size_t(b + k)], "frame " << k);
    // Down 24 dB over two beats: the second beat is quieter than the first.
    CHECK(peak(x, a, a + kBeat / 4) > 0.05);
    CHECK(peak(x, a + 2 * kBeat, a + 2 * kBeat + kBeat / 4) < 0.1 * peak(x, a, a + kBeat / 4));
}

TEST_CASE("a clip chain through the bridge plays as the offline replay" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    Project p;
    latentClip(p);
    p.run("fx.set", R"({"fx": "fx_1", "isolation": "shared_bridge"})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 4 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("looped " << bool(loop) << ", anticipative " << options.anticipative);
            const std::vector<Step> steps{{10, {request("tr_1", "cl_1")}}};
            const Live live =
                playLive(s, p.app->projectDir(), test::patient(options), 128, frames, steps, loop);
            REQUIRE(live.log.size() == 1);
            InstancePool pool = makePool();
            OfflineSettings settings;
            settings.endSeconds = 4.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = live.log;
            settings.loop = loop;
            auto offline = renderOffline(s, pool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 4) == 0.0);
            // The clip plays from bar 2, through its chain: a ramp at -6 dB.
            CHECK(double(live.left[live.latency + 96000 + 5000]) ==
                  doctest::Approx(chained(5000)).epsilon(1e-5));
        }
}

TEST_CASE("live launches of clips with take chains play as the offline replay" *
          doctest::description("rt")) {
    // An audio clip through a latent chain on tr_1, a MIDI clip through an
    // instrument and a latent gain on tr_2; launched, relaunched, swapped,
    // back to the arrangement and stopped.
    Project p;
    latentClip(p);
    p.run("track.add", R"({"name": "Keys"})");  // tr_2
    p.run("clip.add",
          R"({"track": "tr_2", "scene": "sc_1", "notation": "voice bass; 1: C3:q E3:q G3:h"})");
    auto fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");
    fx["take"] = takeOf(p, "cl_2");
    p.runArgs("fx.add", fx);
    auto gain = j(R"({"plugin": "com.suppleaudio.test.gain"})");
    gain["take"] = takeOf(p, "cl_2");
    const std::string fxId = p.runArgs("fx.add", gain)["fx"].asString();
    auto patch = j(R"({"patch": {"params": {"latency": "short"}}})");
    patch["fx"] = fxId;
    p.runArgs("fx.patch.merge", patch);
    auto args = j(R"({"track": "tr_1", "bar": 1})");
    args["audio_file"] = p.ramp("item.wav", 240000).string();
    p.runArgs("item.add", args);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5 + 8192;
    for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("looped " << bool(loop) << ", block " << block << ", anticipative "
                               << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                // Looped, the last two are decided after the wrap at 3 s (song
                // 1.08 s), before their anchor on bar 2 of the second pass.
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(96000 + 4000) / block,
                     {request("tr_1", "cl_1")},
                     model::LaunchQuantize::Beat},
                    {(144000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live =
                    playLive(s, p.app->projectDir(), options, block, frames, steps, loop);
                REQUIRE(live.log.size() == 5);
                if (loop) {
                    CHECK(live.log[3].anchorSample == 96000);
                    CHECK(live.log[3].runningFrame == 192000);
                }
                InstancePool pool = makePool();
                OfflineSettings settings;
                settings.endSeconds = 5.0;
                settings.projectDir = p.app->projectDir();
                settings.launches = live.log;
                settings.loop = loop;
                auto offline = renderOffline(s, pool, settings);
                REQUIRE(offline);
                CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], 48000 * 5) ==
                      0.0);
                settings.launches.clear();
                auto plain = renderOffline(s, pool, settings);
                REQUIRE(plain);
                CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], 48000 * 5) > 0.0);
            }
}

TEST_CASE("a clip through its take's chain keeps its phase when the song wraps before its anchor" *
          doctest::description("rt")) {
    // Review round 66's suggestion: a transport loop over bars 1-2 and the
    // latent chain clip, with a clip-time envelope on its FX, launched on
    // bar 2. After the wrap the song is before the anchor; the chain's
    // input folds there and its envelope reads the clip position, so each
    // frame sounds as the same phase did on the first pass.
    Project p;
    latentClip(p);
    p.run("env.add", R"({"fx": "fx_1", "param": "gain", "points": [{"seconds": 0, "value": 0},
                                                                  {"seconds": 0.25, "value": -24}]})");
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
        const auto at = [&](std::int64_t k) { return double(out[std::size_t(k + latency)]); };
        // Running frame k after the wrap (song k - 192000), against the first
        // pass at the same phase; away from the period's and the loop's edges.
        int checked = 0;
        for (std::int64_t k = 192000 + 300; k < 240000; k += 97) {
            const std::int64_t phase = ((k - 192000 - 96000) % 12000 + 12000) % 12000;
            if (phase < 300 || phase > 11700) continue;
            INFO("frame " << k << ", phase " << phase);
            REQUIRE(at(96000 + 12000 + phase) != 0.0);
            CHECK(at(k) == doctest::Approx(at(96000 + 12000 + phase)).epsilon(1e-6));
            ++checked;
        }
        CHECK(checked > 400);
    }
}

TEST_CASE("a launch onto a track with a clip chain waits the chain's latency longer") {
    // ADR-0097, amended 2026-11-20: the input side takes the launch the
    // lead early, so the floor is that much later.
    for (const bool latent : {false, true}) {
        CAPTURE(latent);
        Project p;
        latentClip(p);
        if (!latent)
            p.run("fx.patch.merge", R"({"fx": "fx_1", "patch": {"params": {"latency": "none"}}})");
        const model::Session s = p.app->head();
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(s, p.app->projectDir()));
        engine.play();
        backend.run(10);
        const Engine::LaunchRequest r = request("tr_1", "cl_1");
        auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::None);
        REQUIRE(placed);
        CHECK(placed->stream == 11 * 128 + (latent ? 64 : 0));
        backend.run(20);
        engine.service();
        REQUIRE(engine.launchLog().size() == 1);
        CHECK(engine.launchLog()[0].runningFrame == placed->stream);
        engine.stop();
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

TEST_CASE("the launch log and a performance say which launches were a scene's" *
          doctest::description("rt")) {
    // A commit writes a section per scene launched whole; it must not guess
    // one from clips that started together (a launch.clip of a scene's only
    // clip would look the same). Each logged launch names its scene, or none.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Project p;
        twoTracks(p);
        AppLive live(p, options, 128, 48000 * 5 + 16384);
        p.run("launch.capture", R"({"on": true})");
        REQUIRE(p.app->transport("play"));
        live.until(20 * 128);
        p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2, both tracks
        live.until(96000 + 4000);
        p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 3, tr_1 alone
        live.until(192000 + 4000);
        const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
        REQUIRE(log.size() == 3);
        CHECK(log[0].scene == id("sc_1"));
        CHECK(log[1].scene == id("sc_1"));
        CHECK_FALSE(log[2].scene);
        CHECK(log[2].clip == id("cl_1"));
        p.run("launch.capture", R"({"on": false})");
        p.run("launch.commit", "{}");
        json::Value history = p.run("log.list", R"({"limit": 1})");
        const json::Value& last = history["entries"].at(history["entries"].size() - 1);
        REQUIRE(last.find("command")->asString() == "launch.commit");
        const json::Value* launches = last.find("args")->find("performance")->find("launches");
        REQUIRE(launches);
        REQUIRE(launches->size() == 3);
        for (std::size_t k : {0u, 1u}) {
            const json::Value* scene = launches->at(k).find("scene");
            REQUIRE(scene);
            CHECK(scene->asString() == "sc_1");
        }
        CHECK_FALSE(launches->at(2).find("scene"));
        live.stop();
    }
}

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
        REQUIRE(engine.captureLaunches(true));
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
        REQUIRE(engine.captureLaunches(true));
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

TEST_CASE("a clip take's FX envelope commits as it was heard, pass after pass (C2)" *
          doctest::description("rt")) {
    // A period that is its content's pass commits as one looped item, its
    // envelope repeated pass after pass; a shorter one as an item per
    // period. The FX's 64-sample latency hears each next pass early, as live.
    for (const double length : {1.0, 0.25})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("length " << length << ", anticipative " << options.anticipative);
            Project p;
            latentClip(p);
            auto set = j(R"({"clip": "cl_1"})");
            set["length"] = length;  // of the ramp's 48000 frames
            p.runArgs("clip.set", set);
            auto env = j(R"({"fx": "fx_1", "param": "gain",
                         "points": [{"seconds": 0, "value": 0}, {"seconds": 1, "value": -24}]})");
            env["points"].asArray()[1]["seconds"] = length;
            p.runArgs("env.add", env);
            const std::int64_t frames = 48000 * 5 + 16384;
            AppLive live(p, options, 128, frames);
            p.run("launch.capture", R"({"on": true})");
            REQUIRE(p.app->transport("play"));
            live.until(20 * 128);
            p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 2
            live.until(48000 * 5);
            json::Value done = p.run("launch.commit", "{}");
            const std::vector<float> heard = live.stop();
            const std::int64_t end = std::min(std::int64_t(done["to"].asDouble() * double(kBeat)),
                                              live.cycles * 128 - std::int64_t(live.latency));
            CHECK(end > 96000 + 2 * 48000);  // two passes and more
            const std::vector<float> committed = arrangement(p, 5.0);
            const auto [worst, first] = compare(heard, live.latency, committed, 0, end, 1e-6);
            CHECK_MESSAGE(first < 0, "first difference at frame " << first << ", " << worst);
            // The second pass starts near 0 dB again.
            const auto P = std::int64_t(length * 48000);
            CHECK(double(committed[std::size_t(96000 + P + 600)]) > 0.7 * double(rampAt(600)));
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

TEST_CASE("launch.commit writes a section for each scene launched whole, in the same undo step") {
    Project p;
    twoTracks(p);  // tr_1 cl_1 and tr_2 cl_2 in sc_1 "A"
    // sc_2 "B" has a clip on tr_1 only; a third scene "C" has both again.
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "notation": "voice b; 1: E3:w"})");
    p.run("scene.add", R"({"name": "C", "color": "#ff8800"})");  // sc_3
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_3", "notation": "voice b; 1: G3:w"})");
    p.run("clip.add", R"({"track": "tr_2", "scene": "sc_3", "notation": "voice b; 1: G2:w"})");
    // The clips: cl_1 (tr_1, A), cl_2 (tr_2, A), cl_3 (tr_1, B), cl_4 (tr_1, C), cl_5 (tr_2, C).
    // A launch of a clip; `scene` is the id launch.scene logs with it ("" for a clip launched
    // on its own).
    const auto launch = [](std::int64_t frame, const char* track, const char* clip, double beat,
                           const char* scene = "") {
        return std::string(R"({"frame": )") + std::to_string(frame) + R"(, "track": ")" + track +
               R"(", "action": "clip", "clip": ")" + clip + R"(", )" +
               (*scene ? std::string(R"("scene": ")") + scene + R"(", )" : std::string()) +
               R"("anchor": )" + std::to_string(frame) + R"(, "anchor_beat": )" +
               std::to_string(beat) + "}";
    };
    const auto commit = [&](const std::string& launches, std::int64_t frames) {
        json::Value args = json::Value::object();
        args["performance"] = j(R"({"sample_rate": 48000, "start": 0, "frames": )" +
                                std::to_string(frames) + R"(, "launches": [)" + launches + "]}");
        return p.runArgs("launch.commit", args);
    };
    const auto regions = [&] {
        std::vector<std::tuple<std::string, double, double>> out;
        for (const auto& e : p.app->head().regions)
            out.push_back({e.value->name, e.value->start, e.value->end});
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });
        return out;
    };
    const std::size_t before = p.app->head().regions.size();
    const std::size_t itemsBefore = p.app->head().items.size();

    SUBCASE(
        "two scenes in turn: a section each, named after the scene, meeting where one gave way") {
        // A on bar 1 (frame 0), C on bar 2 (96000): beats 0 and 4; the capture ends at bar 4.
        const std::string log = launch(0, "tr_1", "cl_1", 0, "sc_1") + "," +
                                launch(0, "tr_2", "cl_2", 0, "sc_1") + "," +
                                launch(96000, "tr_1", "cl_4", 4, "sc_3") + "," +
                                launch(96000, "tr_2", "cl_5", 4, "sc_3");
        json::Value done = commit(log, 288000);
        REQUIRE(done["sections"].size() == 2);
        const auto r = regions();
        REQUIRE(r.size() == before + 2);
        CHECK(std::get<0>(r[before]) == "A");
        CHECK(std::get<1>(r[before]) == 0.0);
        CHECK(std::get<2>(r[before]) == 4.0);
        CHECK(std::get<0>(r[before + 1]) == "C");
        CHECK(std::get<1>(r[before + 1]) == 4.0);
        CHECK(std::get<2>(r[before + 1]) == doctest::Approx(12));  // the capture's end
        CHECK(done["sections"].at(0).find("scene")->asString() == "sc_1");
        CHECK(done["sections"].at(1).find("scene")->asString() == "sc_3");
        const model::Session s = p.app->head();
        const model::Region& second =
            *s.regions.find(id(done["sections"].at(1).find("region")->asString().c_str()));
        CHECK(second.section);
        CHECK(second.color == "#ff8800");
        // context.at names the section over the committed performance.
        auto at = p.run("context.at", R"({"bar": 2})");
        CHECK(at["section"]["name"].asString() == "C");
        // One undo step takes the sections and the items away together.
        p.run("history.undo", "{}");
        CHECK(p.app->head().regions.size() == before);
        CHECK(p.app->head().items.size() == itemsBefore);
        p.run("history.redo", "{}");
        CHECK(p.app->head().regions.size() == before + 2);
    }
    SUBCASE("under a transport loop the sections land where the items do, past the wrap") {
        // The loop runs bars 1-2 (frames 0-192000); A is launched whole at running frame 0,
        // C at running frame 288000, which the song heard as frame 96000 (beat 4, bar 2). The
        // items lie on the timeline by running time, C from bar 4 (beat 12), A before it, and
        // so must the sections: not at the folded song position.
        const auto at = [](std::int64_t frame, std::int64_t anchor, const char* track,
                           const char* clip, const char* scene, double beat) {
            return std::string(R"({"frame": )") + std::to_string(frame) + R"(, "track": ")" +
                   track + R"(", "action": "clip", "clip": ")" + clip + R"(", "scene": ")" + scene +
                   R"(", "anchor": )" + std::to_string(anchor) + R"(, "anchor_beat": )" +
                   std::to_string(beat) + "}";
        };
        const std::string log = at(0, 0, "tr_1", "cl_1", "sc_1", 0) + "," +
                                at(0, 0, "tr_2", "cl_2", "sc_1", 0) + "," +
                                at(288000, 96000, "tr_1", "cl_4", "sc_3", 4) + "," +
                                at(288000, 96000, "tr_2", "cl_5", "sc_3", 4);
        json::Value args = json::Value::object();
        args["performance"] = j(
            R"({"sample_rate": 48000, "start": 0, "frames": 384000, "loop": {"start": 0, "end": 192000},
                "launches": [)" +
            log + "]}");
        json::Value done = p.runArgs("launch.commit", args);
        REQUIRE(done["sections"].size() == 2);
        const auto r = regions();
        REQUIRE(r.size() == before + 2);
        CHECK(std::get<0>(r[before]) == "A");
        CHECK(std::get<1>(r[before]) == doctest::Approx(0));
        CHECK(std::get<2>(r[before]) == doctest::Approx(12));
        CHECK(std::get<0>(r[before + 1]) == "C");
        CHECK(std::get<1>(r[before + 1]) == doctest::Approx(12));
        CHECK(std::get<2>(r[before + 1]) == doctest::Approx(16));  // the capture's end
        // The section names what plays under it: C's items start where its section does.
        CHECK(p.run("context.at", R"({"bar": 3})")["section"]["name"].asString() == "A");
        CHECK(p.run("context.at", R"({"bar": 4})")["section"]["name"].asString() == "C");
    }
    SUBCASE("committing again writes the same performance over itself: the sections stay") {
        const std::string log =
            launch(0, "tr_1", "cl_1", 0, "sc_1") + "," + launch(0, "tr_2", "cl_2", 0, "sc_1");
        json::Value first = commit(log, 192000);
        const std::size_t items = p.app->head().items.size();
        json::Value again = commit(log, 192000);
        CHECK(p.app->head().regions.size() == before + 1);
        CHECK(p.app->head().items.size() == items);
        CHECK(again["sections"].at(0).find("region")->asString() ==
              first["sections"].at(0).find("region")->asString());
    }
    SUBCASE("a clip launched alone, or only some of a scene's clips, writes none") {
        // A holds cl_1 and cl_2: launching cl_1 alone is not launching the scene.
        json::Value done = commit(launch(0, "tr_1", "cl_1", 0), 192000);
        CHECK(done["sections"].size() == 0);
        CHECK(p.app->head().regions.size() == before);
        CHECK(done["items"].size() > 0);
    }
    SUBCASE("every clip of a scene launched one by one on one frame writes none") {
        // The same clips at the same frame as launch.scene would start them, but launched on
        // their own: no scene was launched, so no section.
        const std::string log = launch(0, "tr_1", "cl_1", 0) + "," + launch(0, "tr_2", "cl_2", 0);
        json::Value done = commit(log, 192000);
        CHECK(done["sections"].size() == 0);
        CHECK(p.app->head().regions.size() == before);
        CHECK(done["items"].size() > 0);
    }
    SUBCASE("a scene launch one of whose tracks is taken over on the same frame writes none") {
        // launch.scene A, then tr_2 launched alone on A's frame with C's clip: A is not whole.
        const std::string log = launch(0, "tr_1", "cl_1", 0, "sc_1") + "," +
                                launch(0, "tr_2", "cl_2", 0, "sc_1") + "," +
                                launch(0, "tr_2", "cl_5", 0);
        json::Value done = commit(log, 192000);
        CHECK(done["sections"].size() == 0);
        CHECK(p.app->head().regions.size() == before);
    }
    SUBCASE("a scene's section ends when one of its tracks is launched again") {
        // A at bar 1; tr_2 leaves for the arrangement at bar 3 (frame 192000): A ends there.
        const std::string log = launch(0, "tr_1", "cl_1", 0, "sc_1") + "," +
                                launch(0, "tr_2", "cl_2", 0, "sc_1") +
                                R"(,{"frame": 192000, "track": "tr_2", "action": "arrangement",
                                    "anchor": 192000, "anchor_beat": 8})";
        json::Value done = commit(log, 288000);
        REQUIRE(done["sections"].size() == 1);
        CHECK(done["sections"].at(0).find("end")->asDouble() == doctest::Approx(8));
    }
    SUBCASE(
        "a scene launched twice writes a section each time; the last of one frame's launches "
        "holds") {
        // B (one clip on tr_1) at bar 1, then A over it at the same frame: A holds there.
        const std::string log = launch(0, "tr_1", "cl_3", 0, "sc_2") + "," +
                                launch(0, "tr_1", "cl_1", 0, "sc_1") + "," +
                                launch(0, "tr_2", "cl_2", 0, "sc_1") + "," +
                                launch(96000, "tr_1", "cl_1", 4, "sc_1") + "," +
                                launch(96000, "tr_2", "cl_2", 4, "sc_1");
        json::Value done = commit(log, 192000);
        REQUIRE(done["sections"].size() == 2);
        CHECK(done["sections"].at(0).find("scene")->asString() == "sc_1");
        CHECK(done["sections"].at(1).find("scene")->asString() == "sc_1");
        CHECK(done["sections"].at(0).find("end")->asDouble() == doctest::Approx(4));
        CHECK(done["sections"].at(1).find("start")->asDouble() == doctest::Approx(4));
    }
}

TEST_CASE("launch.commit names the items after their clips") {
    Project p;
    twoTracks(p);
    p.run("clip.set", R"({"clip": "cl_1", "name": "Riff"})");
    json::Value args = json::Value::object();
    args["performance"] = j(R"({"sample_rate": 48000, "start": 0, "frames": 192000,
        "launches": [{"frame": 0, "track": "tr_1", "action": "clip", "clip": "cl_1",
                      "anchor": 0, "anchor_beat": 0}]})");
    json::Value done = p.runArgs("launch.commit", args);
    REQUIRE(done["items"].size() > 0);
    const model::Session s = p.app->head();
    for (const auto& item : done["items"].asArray()) {
        const model::Item& it = *s.items.find(id(item.asString().c_str()));
        CHECK(s.takes.find(*it.activeTake)->name == "Riff");
    }
}

TEST_CASE("launch.commit copies a launched clip and warns when its key is not the project's") {
    Project p;
    twoTracks(p);
    p.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    p.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w"})");
    json::Value args = json::Value::object();
    args["performance"] = j(R"({"sample_rate": 48000, "start": 0, "frames": 192000,
        "launches": [{"frame": 0, "track": "tr_1", "action": "clip", "clip": "cl_1",
                      "anchor": 0, "anchor_beat": 0}]})");
    json::Value done = p.runArgs("launch.commit", args);
    REQUIRE(done["items"].size() > 0);
    const json::Value* warnings = done.find("warnings");
    REQUIRE(warnings);
    bool mismatch = false;
    for (const auto& w : warnings->asArray())
        mismatch |= w.find("code")->asString() == "context.mismatch";
    CHECK(mismatch);
    // A clip laid one item per period (its length twice its content) says it once per stretch,
    // not once per period.
    p.run("clip.set", R"({"clip": "cl_1", "length": 8})");
    json::Value periods = j(R"({"performance": {"sample_rate": 48000, "start": 0, "frames": 768000,
        "launches": [{"frame": 0, "track": "tr_1", "action": "clip", "clip": "cl_1",
                      "anchor": 0, "anchor_beat": 0}]}})");
    json::Value laid = p.runArgs("launch.commit", periods);
    CHECK(laid["items"].size() > 1);
    int mismatches = 0;
    for (const auto& w : laid.find("warnings")->asArray())
        mismatches += w.find("code")->asString() == "context.mismatch";
    CHECK(mismatches == 1);
    p.run("clip.set", R"({"clip": "cl_1", "length": 4})");
    // The notes are as they were played: nothing was mapped.
    p.run("clip.set", R"({"clip": "cl_1", "context": "1: key C minor ~:w"})");
    json::Value again = p.runArgs("launch.commit", args);
    CHECK_FALSE(again.find("warnings"));
}

// --- Launches under a master playrate (ADR-0097, dated revision 2026-11-21) ---

namespace {

// The master playrate the S3 tests play under, the pitch not kept: a
// constant 0.75, or an envelope from 1 down to 0.6 over two bars.
void underRate(Project& p, bool envelope) {
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    if (envelope)
        p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                         R"({"beat": 0, "value": 1}, {"beat": 8, "value": 0.6}]})");
}

// The live run's output against the offline replay of its log, over the
// first `frames` device frames; and the same without the launches, which
// must differ for the equality to say something.
void liveIsReplay(const model::Session& s, const fs::path& dir, const Live& live, double seconds,
                  std::size_t frames) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.projectDir = dir;
    settings.launches = live.log;
    auto offline = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(offline, (offline ? std::string() : offline.error().toString()));
    REQUIRE(offline->audio.channels[0].size() >= frames);
    CHECK(maxDiff(live.left, live.latency, offline->audio.channels[0], frames) == 0.0);
    settings.launches.clear();
    auto plain = renderOffline(s, pool, settings);
    REQUIRE(plain);
    CHECK(maxDiff(live.left, live.latency, plain->audio.channels[0], frames) > 0.0);
}

}  // namespace

TEST_CASE("launches under a master playrate play as the offline replay, in both executors" *
          doctest::description("rt")) {
    // twoTracks' scene launch, then tr_1 back to the arrangement and tr_2
    // stopped, under a constant 0.75 and under an envelope. At 0.75 bar 2
    // (song 96000) is heard at running frame 128000, bar 3 at 256000.
    for (const bool envelope : {false, true}) {
        Project p;
        twoTracks(p);
        underRate(p, envelope);
        const model::Session s = p.app->head();
        const std::int64_t frames = 48000 * 8 + 8192;
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(128000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
                REQUIRE(live.log.size() == 4);
                CHECK(live.log[0].anchorSample == 96000);  // bar 2
                CHECK(live.log[1].anchorSample == 96000);  // one target for the scene
                CHECK(live.log[2].anchorSample == 192000);
                if (!envelope) {
                    CHECK(live.log[0].runningFrame == 128000);
                    CHECK(live.log[2].runningFrame == 256000);
                }
                liveIsReplay(s, p.app->projectDir(), live, 5.0, 300000);
            }
    }
}

TEST_CASE("a launch under a master playrate switches where the song reaches its boundary") {
    // The anchor is the first frame whose song position reaches the bar:
    // on the rate timeline the frame before is short of it. And it moves
    // with a scalar edit made while it waits.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.6, "preserve_pitch": false})");
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 5;
    // 0.6: bar 2 (song 96000) at device 160000 exactly.
    const Live plain = playLive(s, p.app->projectDir(), kInCallback, 128, frames,
                                {{20, {request("tr_1", "cl_1")}}});
    REQUIRE(plain.log.size() == 1);
    CHECK(plain.log[0].anchorSample == 96000);
    CHECK(plain.log[0].runningFrame == 160000);
    // A scalar edit to 1.2 well before then: the launch is placed again on
    // the new trajectory, still on bar 2, much earlier.
    p.run("transport.playrate.set", R"({"rate": 1.2, "preserve_pitch": false})");
    const model::Session faster = p.app->head();
    const Live edited = playLive(s, p.app->projectDir(), kInCallback, 128, frames,
                                 {{20, {request("tr_1", "cl_1")}},
                                  {200, {}, {}, &faster},
                                  {210, {}}});  // (a service, which places it again)
    REQUIRE(edited.log.size() == 1);
    CHECK(edited.log[0].anchorSample == 96000);
    CHECK(edited.log[0].runningFrame < 140000);
    CHECK(edited.log[0].runningFrame > 200 * 128);
}

TEST_CASE("a launch while a playrate change is in flight says so, and lands once it is not") {
    // Review round 78 K1: right after a rate edit reaches the engine, and
    // before the executors take it, placement answers launch.later with the
    // rate's own message, not the relocation one.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.6, "preserve_pitch": false})");
    const model::Session slow = p.app->head();
    // A rate envelope changes the trajectory through a plan handover (a
    // scalar alone is published to the running plan at once).
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 1}, {"beat": 8, "value": 0.6}]})");
    const model::Session fast = p.app->head();
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(slow, p.app->projectDir()));
    engine.play();
    backend.run(20);
    engine.service();
    REQUIRE(engine.load(fast, p.app->projectDir()));
    const std::vector<Engine::LaunchRequest> one{request("tr_1", "cl_1")};
    auto inFlight = engine.launch(one, model::LaunchQuantize::Bar);
    REQUIRE_FALSE(inFlight);
    CHECK(inFlight.error().code == "launch.later");
    CHECK(inFlight.error().message == "the master playrate is changing");
    CHECK(inFlight.error().hint == "launch again in a moment");
    // Once the executors take the new plan, it is placed.
    auto placed = engine.launch(one, model::LaunchQuantize::Bar);
    for (int cycle = 0; cycle < 2000 && !placed; ++cycle) {
        CHECK(placed.error().code == "launch.later");
        backend.run(1);
        engine.service();
        placed = engine.launch(one, model::LaunchQuantize::Bar);
    }
    REQUIRE_MESSAGE(placed, (placed ? std::string() : placed.error().toString()));
    engine.stop();
}

TEST_CASE("launches of clips with take chains under a master playrate play as the replay" *
          doctest::description("rt")) {
    // As the unity test: an audio clip through a latent chain on tr_1, a
    // MIDI clip through an instrument and a latent gain on tr_2.
    for (const bool envelope : {false, true}) {
        Project p;
        latentClip(p);
        p.run("track.add", R"({"name": "Keys"})");  // tr_2
        p.run("clip.add",
              R"({"track": "tr_2", "scene": "sc_1", "notation": "voice bass; 1: C3:q E3:q G3:h"})");
        auto fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");
        fx["take"] = takeOf(p, "cl_2");
        p.runArgs("fx.add", fx);
        auto gain = j(R"({"plugin": "com.suppleaudio.test.gain"})");
        gain["take"] = takeOf(p, "cl_2");
        const std::string fxId = p.runArgs("fx.add", gain)["fx"].asString();
        auto patch = j(R"({"patch": {"params": {"latency": "short"}}})");
        patch["fx"] = fxId;
        p.runArgs("fx.patch.merge", patch);
        auto args = j(R"({"track": "tr_1", "bar": 1})");
        args["audio_file"] = p.ramp("item.wav", 240000).string();
        p.runArgs("item.add", args);
        underRate(p, envelope);
        const model::Session s = p.app->head();
        const std::int64_t frames = 48000 * 8 + 8192;
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Engine::LaunchRequest stop = backTo("tr_2");
                stop.action = LaunchLogEntry::Action::Stop;
                const std::vector<Step> steps{
                    {20, {request("tr_1", "cl_1"), request("tr_2", "cl_2")}},
                    {(128000 + 4000) / block,
                     {request("tr_1", "cl_1")},
                     model::LaunchQuantize::Beat},
                    {(192000 + 4000) / block, {backTo("tr_1"), stop}}};
                const Live live = playLive(s, p.app->projectDir(), options, block, frames, steps);
                REQUIRE(live.log.size() == 5);
                liveIsReplay(s, p.app->projectDir(), live, 5.0, 300000);
            }
    }
}

TEST_CASE("a rack's Notes module follows launches under a master playrate as the replay" *
          doctest::description("rt")) {
    Project p;
    rackFollows(p, false);
    underRate(p, false);
    const model::Session s = p.app->head();
    const std::int64_t frames = 48000 * 8 + 8192;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        Engine::LaunchRequest stop = backTo("tr_1");
        stop.action = LaunchLogEntry::Action::Stop;
        const std::vector<Step> steps{{20, {request("tr_1", "cl_1")}},
                                      {(128000 + 4000) / 128, {stop}},
                                      {(256000 + 4000) / 128, {backTo("tr_1")}}};
        const Live live = playLive(s, p.app->projectDir(), options, 128, frames, steps);
        REQUIRE(live.log.size() == 3);
        CHECK(live.log[2].anchorSample == 288000);
        CHECK(live.log[2].runningFrame == 384000);
        liveIsReplay(s, p.app->projectDir(), live, 6.5, 380000);
        // The gate follows the clip, at song times over 0.75 in device time.
        const auto g = gates(p, 6.5, live.log), plain = gates(p, 6.5, {});
        const auto at = [](const std::vector<float>& v, double song) {
            return v[std::size_t(song / 0.75 * 48000)];
        };
        CHECK(at(g, 2.1) == 1.0f);   // the clip's first eighth note
        CHECK(at(g, 2.35) == 0.0f);  // its rest: the arrangement's A3 passes unplayed
        CHECK(at(plain, 2.35) == 1.0f);
        CHECK(at(g, 2.6) == 1.0f);  // its second eighth
        CHECK(at(g, 4.5) == 0.0f);  // stopped
        CHECK(at(g, 6.3) == 1.0f);  // the arrangement again
    }
}

TEST_CASE("launch.capture is refused under a master playrate, and one becoming active ends it") {
    Project p;
    twoTracks(p);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(p.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}, kInCallback));
    // Armed at unity, running: a playrate turned on ends it.
    p.run("launch.capture", R"({"on": true})");
    p.run("transport.play", "{}");
    backend.run(40);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    REQUIRE(p.app->engine()->awaitPlan());
    backend.run(40);
    p.app->engine()->service();
    auto capture = p.app->engine()->launchCapture();
    REQUIRE(capture);
    CHECK_FALSE(capture->on);
    CHECK(capture->frames > 0);
    // Now refused at arming.
    auto r = p.bus->execute({"launch.capture", j(R"({"on": true})"), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "launch.playrate");
    CHECK(r.error().hint.find("then capture") != std::string::npos);
    p.app->stopEngine();
}

TEST_CASE("under a master playrate a switch crossfades over 5 ms as heard, in device frames") {
    // At 0.75 bar 2 (song 96000) is device frame 128000. The crossfade runs
    // over the next 240 device frames (180 song samples), the arrangement's
    // ramp out and the clip's in, each read through the tape converter.
    Project p;
    auto args = j(R"({"track": "tr_1", "bar": 1})");
    args["audio_file"] = p.ramp("item.wav", 240000).string();
    p.runArgs("item.add", args);
    args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = p.ramp("clip.wav", 48000).string();
    p.runArgs("clip.add", args);
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    LaunchLogEntry e = launch(128000, "cl_1");
    e.anchorSample = 96000;
    e.anchorBeat = 4;
    const auto out = p.render(2.5, {e});
    const auto plain = p.render(2.5, {});
    REQUIRE(out.size() > 130000);
    for (std::int64_t d = 120000; d < 128000; d += 97)
        REQUIRE(out[std::size_t(d)] == plain[std::size_t(d)]);
    for (std::int64_t k = 60; k < kFade; k += 30) {
        const double x = double(k) / double(kFade);
        const double clip = double(rampAt(static_cast<std::int64_t>(0.75 * double(k))));
        const double want = double(plain[std::size_t(128000 + k)]) * (1 - x) + clip * x;
        REQUIRE_MESSAGE(double(out[std::size_t(128000 + k)]) == doctest::Approx(want).epsilon(0.01),
                        "frame " << 128000 + k);
    }
    // Past it, the clip alone, its phase the song's from the anchor.
    CHECK(double(out[128000 + 400]) == doctest::Approx(double(rampAt(300))).epsilon(0.01));
}

TEST_CASE("under a slow master playrate a launch's first frame plays the note on its anchor") {
    // At 0.25 the frame before a switch can already be past the anchor (an
    // unquantized launch's anchor is the floor's song sample, rounded down):
    // the switch's first frame reaches back to it, so the clip's first note
    // sounds. Song 96000 + 0.5 at device frame 384002.
    Project p;
    twoTracks(p);
    p.run("transport.playrate.set", R"({"rate": 0.25, "preserve_pitch": false})");
    LaunchLogEntry e = launch(384002, "cl_1");
    e.anchorSample = 96000;
    e.anchorBeat = 4;
    const auto out = p.render(2.5, {e});
    REQUIRE(out.size() > 384002 + 40000);
    // Half a second of C3 (131 Hz: about 131 zero crossings; the
    // arrangement's A3 there would give 220), before E3 an eighth (48000
    // frames) later.
    int crossings = 0;
    double peak = 0;
    for (std::size_t i = 384002 + 500; i < 384002 + 24500; ++i) {
        peak = std::max(peak, std::fabs(double(out[i])));
        if ((out[i] < 0) != (out[i + 1] < 0)) ++crossings;
    }
    CHECK(peak > 0.05);
    CHECK(crossings > 110);
    CHECK(crossings < 150);
}

TEST_CASE("under a master playrate a clip take's FX envelope plays in clip time (C2)") {
    // At 0.75 each 12000-sample pass takes 16000 device frames. The gain
    // envelope runs 0 to -24 dB over the pass, by the clip position heard,
    // never by the rate timeline.
    Project p;
    latentClip(p);
    p.run("env.add", R"({"fx": "fx_1", "param": "gain", "points": [{"seconds": 0, "value": 0},
                                                                  {"seconds": 0.25, "value": -24}]})");
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
    LaunchLogEntry e = launch(128000, "cl_1");
    e.anchorSample = 96000;
    e.anchorBeat = 4;
    const auto x = p.render(3.0, {e});
    REQUIRE(x.size() > 128000 + 32000);
    const auto ratio = [&](std::int64_t k) {
        const auto c = static_cast<std::int64_t>(0.75 * double(k)) % 12000;
        return double(x[std::size_t(128000 + k)]) / double(rampAt(c));
    };
    CHECK(ratio(16000 + 800) > 0.7);    // clip position 600: about -1 dB
    CHECK(ratio(16000 + 15333) < 0.1);  // 11500: about -23 dB
    CHECK(ratio(16000 + 15333) > 0.05);
}
