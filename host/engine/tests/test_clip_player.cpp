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
