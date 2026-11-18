// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A file audition in the device callback (ADR-0094): outputs 1/2, after the
// project's output and outside it, in both executors, stopped or playing.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "audition_loader.h"
#include "realtime_common.h"
#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
// A voice of `frames` that no two frames of share a value: left a rising
// ramp, right a falling one (or none: mono).
std::shared_ptr<AuditionVoice> rampVoice(std::size_t frames, bool stereo = true) {
    auto v = std::make_shared<AuditionVoice>();
    for (std::size_t i = 0; i < frames; ++i) {
        v->left.push_back(0.25f + 0.5f * static_cast<float>(i) / static_cast<float>(frames));
        if (stereo)
            v->right.push_back(-0.25f - 0.5f * static_cast<float>(i) / static_cast<float>(frames));
    }
    return v;
}

constexpr std::uint32_t kBlock = 256;
constexpr std::int64_t kCycles = 120;
constexpr std::uint32_t kFade = 240;  // 5 ms at 48 kHz

// `cycles` device cycles of `session` (played or not) on four outputs. At
// each cycle `step(engine, cycle)` runs first, so what it publishes is taken
// at that cycle's first frame. Returns the capture.
template <class Step>
std::vector<std::vector<float>> run(const model::Session& session, const EngineOptions& options,
                                    bool play, Step step) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = kCycles * kBlock});
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock, .outputs = 4}));
    REQUIRE(engine.load(session));
    if (play) engine.play();
    for (std::int64_t c = 0; c < kCycles; ++c) {
        if (options.anticipative && play) REQUIRE(engine.awaitRendered((c + 1) * kBlock));
        step(engine, c);
        backend.run(1);
    }
    CHECK(engine.processErrors() == 0);
    engine.stop();
    return backend.capture().channels;
}
}  // namespace

TEST_CASE("audition: a file plays on outputs 1 and 2 over the project, in both executors" *
          doctest::description("rt")) {
    const model::Session silent = model::Session::create({.console9 = false});
    const model::Session song = synthSession();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}})
        for (const bool playing : {false, true})
            for (const bool stereo : {true, false}) {
                CAPTURE(options.anticipative);
                CAPTURE(playing);
                CAPTURE(stereo);
                const model::Session& session = playing ? song : silent;
                const auto voice = rampVoice(7000, stereo);  // 27.3 cycles
                const std::int64_t at = 10;                  // the cycle it starts at
                std::vector<AuditionState> states;
                const auto without = run(session, options, playing, [](Engine&, std::int64_t) {});
                const auto with = run(session, options, playing, [&](Engine& e, std::int64_t c) {
                    if (c == at) CHECK(e.audition(voice) != 0);
                    states.push_back(e.auditionState());
                });
                if (playing) CHECK(*std::max_element(without[0].begin(), without[0].end()) > 0.01f);
                const std::size_t from = at * kBlock, to = from + voice->frames();
                const auto& right = stereo ? voice->right : voice->left;
                // The file's samples added to the project's, sample for sample
                // (a gain of exactly 1), from the cycle it was published at.
                for (std::size_t c = 0; c < 4; ++c) {
                    CAPTURE(c);
                    std::size_t wrong = with[c].size();
                    for (std::size_t i = 0; i < with[c].size() && wrong == with[c].size(); ++i) {
                        float expected = without[c][i];
                        if (c < 2 && i >= from && i < to)
                            expected += (c == 0 ? voice->left : right)[i - from];
                        if (with[c][i] != expected) wrong = i;
                    }
                    CHECK(wrong == with[c].size());  // the first frame that differs
                }
                if (!playing) CHECK(*std::max_element(without[0].begin(), without[0].end()) == 0);
                // The state seen before each cycle: none, then the voice
                // from where the callback is, then none once it has ended.
                REQUIRE(states.size() == kCycles);
                CHECK_FALSE(states[at].playing);  // published, not yet taken
                for (std::int64_t c = at + 1; c < kCycles; ++c) {
                    CAPTURE(c);
                    const auto played = static_cast<std::size_t>(c - at) * kBlock;
                    CHECK(states[static_cast<std::size_t>(c)].playing ==
                          (played < voice->frames()));
                    if (played < voice->frames())
                        CHECK(states[static_cast<std::size_t>(c)].position ==
                              static_cast<std::int64_t>(played));
                }
            }
}

TEST_CASE("audition: a stop fades out over 5 ms; a new file starts while the old fades" *
          doctest::description("rt")) {
    const model::Session silent = model::Session::create({.console9 = false});
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        CAPTURE(options.anticipative);
        const auto first = rampVoice(20000), second = rampVoice(3000, false);
        const std::int64_t at = 4, change = 12;
        const std::size_t played = (change - at) * kBlock;  // where the first is
        auto gain = [](std::uint32_t i) {
            return 1.0f - static_cast<float>(i + 1) / static_cast<float>(kFade);
        };
        SUBCASE("stop") {
            const auto out = run(silent, options, false, [&](Engine& e, std::int64_t c) {
                if (c == at) e.audition(first);
                if (c == change) CHECK(e.audition(nullptr) == 0);
            });
            const std::size_t from = change * kBlock;
            for (std::uint32_t i = 0; i < kFade; ++i) {
                CAPTURE(i);
                CHECK(out[0][from + i] == gain(i) * first->left[played + i]);
                CHECK(out[1][from + i] == gain(i) * first->right[played + i]);
            }
            CHECK(out[0][from + kFade - 1] == 0.0f);  // the last frame of the fade
            for (std::size_t i = from + kFade; i < out[0].size(); ++i) {
                CAPTURE(i);
                REQUIRE(out[0][i] == 0.0f);
                REQUIRE(out[1][i] == 0.0f);
            }
        }
        SUBCASE("replace") {
            const auto out = run(silent, options, false, [&](Engine& e, std::int64_t c) {
                if (c == at) e.audition(first);
                if (c == change) e.audition(second);
            });
            const std::size_t from = change * kBlock;
            for (std::uint32_t i = 0; i < second->frames(); ++i) {
                CAPTURE(i);
                // The old fades (added first), the new starts at its first sample.
                const float fading = i < kFade ? gain(i) * first->left[played + i] : 0.0f;
                REQUIRE(out[0][from + i] == (0.0f + fading) + second->left[i]);
            }
            for (std::size_t i = from + second->frames(); i < out[0].size(); ++i)
                REQUIRE(out[0][i] == 0.0f);
        }
    }
}

TEST_CASE("audition: the engine frees a voice once the callback no longer reads it" *
          doctest::description("rt")) {
    const model::Session silent = model::Session::create({.console9 = false});
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        CAPTURE(options.anticipative);
        auto pool = makePool();
        MediaCache media;
        // Blocks of 128: the 240-frame fade spans two cycles.
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4096});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(silent));
        std::weak_ptr<AuditionVoice> first, second;
        {
            auto a = rampVoice(1000), b = rampVoice(100000);
            first = a;
            second = b;
            engine.audition(std::move(a));
            backend.run(1);
            engine.service();
            CHECK_FALSE(first.expired());  // playing
            engine.audition(std::move(b));
        }
        engine.service();
        CHECK_FALSE(first.expired());  // not yet seen to be replaced
        backend.run(1);                // takes the second; the first fades
        engine.service();
        CHECK_FALSE(first.expired());  // 128 of its 240 fade frames
        backend.run(1);                // the fade is done
        engine.service();
        CHECK(first.expired());
        CHECK_FALSE(second.expired());
        engine.audition(nullptr);
        backend.run(2);
        engine.service();
        CHECK(second.expired());
        CHECK_FALSE(engine.auditionState().playing);
        // Stopping the engine drops a voice still playing.
        auto c = rampVoice(100000);
        std::weak_ptr<AuditionVoice> third = c;
        engine.audition(std::move(c));
        backend.run(1);
        CHECK(engine.auditionState().playing);
        engine.stop();
        CHECK(third.expired());
        CHECK_FALSE(engine.auditionState().playing);
    }
}

TEST_CASE("audition: a file's start plays at the device's rate on outputs 1/2, then ends") {
    // ADR-0094 through the commands. The file is read and resampled off the
    // model thread: audition.play answers at once (decoding), tick() hands
    // the voice to the engine when it is ready. The cap is in frames at the
    // higher of the file's and the device's rates: a 44.1 kHz file plays the
    // cap's length in device frames, a 96 kHz one decodes no more than the
    // cap.
    const fs::path root =
        fs::temp_directory_path() /
        ("supple-audition-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { fs::remove_all(dir); }
    } cleanup{root};
    cmd::Registry registry;
    registerEngineCommands(registry);
    AppOptions o;
    o.workdir = root;
    o.chainDir = root / "chains";
    o.presetDir = root / "presets";
    o.templateDir = root / "templates";
    o.projectTemplateDir = root / "project-templates";
    o.pluginPath = {kFixture};
    o.auditionMaxFrames = 12000;  // short: 0.25 s at 48 kHz, 0.125 s at 96 kHz
    auto made = EngineApp::create(o, {.console9 = false});
    REQUIRE(made);
    auto& app = *made;
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, const json::Value& args) {
        return d.execute({std::move(command), args, {cmd::Origin::Type::Test, "unit", ""}});
    };
    // Half a second of stereo, every frame distinct, at 44.1 and 96 kHz.
    fs::create_directories(app->mediaDir());
    auto writeFile = [&](const char* name, double rate) {
        audio::AudioData file;
        file.sampleRate = rate;
        const auto n = static_cast<std::size_t>(rate / 2);
        file.channels.assign(2, std::vector<float>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i);
            file.channels[0][i] = 0.5f * static_cast<float>(std::sin(0.01 * x));
            file.channels[1][i] = 0.25f * static_cast<float>(std::cos(0.02 * x));
        }
        const fs::path at = app->mediaDir() / name;
        REQUIRE(audio::writeWav(at, file));
        return at;
    };
    const fs::path wav = writeFile("take.wav", 44100);
    const fs::path fast = writeFile("fast.wav", 96000);
    auto args = json::Value::object();
    args["path"] = wav.string();

    // A stopped engine refuses.
    auto refused = call("audition.play", args);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "engine.stopped");

    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 128 * 256});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .outputs = 4},
                             {.anticipative = false})
                .ok());
    // Files that are not there or not audio.
    for (const char* bad : {"missing.wav", "notes.txt"}) {
        CAPTURE(bad);
        if (std::string(bad) == "notes.txt") std::ofstream(app->mediaDir() / bad) << "text";
        auto badArgs = json::Value::object();
        badArgs["path"] = bad;
        auto r = call("audition.play", badArgs);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "audition.file");
    }
    backend.run(4);  // the project plays nothing: silence before

    auto played = call("audition.play", args);
    REQUIRE(played);
    CHECK(played->find("seconds")->asDouble() == doctest::Approx(0.25));
    CHECK(played->find("sample_rate")->asDouble() == 44100.0);
    CHECK(played->find("channels")->asInt() == 2);
    CHECK(played->find("truncated")->asBool());
    CHECK(app->auditionStatus().playing);
    // A relative name is in the media folder: the same file.
    auto relative = json::Value::object();
    relative["path"] = "take.wav";
    auto again = call("audition.play", relative);
    REQUIRE(again);
    CHECK(fs::equivalent(again->find("path")->asString(), wav));
    // Decoded off this thread: wait on the state, not the clock.
    auto decode = [&] {
        while (app->auditionStatus().decoding) {
            app->tick();
            std::this_thread::yield();
        }
    };
    // Plays the file's first `head` frames resampled to 48 kHz on outputs
    // 1/2, nothing on 3/4, and ends within the cycle that plays its last
    // frame. Before it, outputs 1/2 are silent from `from` on.
    auto playsHead = [&](const fs::path& file, std::int64_t head, std::int64_t from) {
        decode();
        auto got = call("audition.get", json::Value::object());
        REQUIRE(got);
        CHECK(got->find("playing")->asBool());  // published, taken at the next cycle
        CHECK_FALSE(got->find("decoding")->asBool());
        const std::int64_t start = backend.cycles() * 256;
        auto read = audio::readAudioHead(file, {.maxFrames = head});
        REQUIRE(read);
        auto expected = audio::resample(*read, 48000);
        REQUIRE(expected);
        const std::size_t frames = expected->frames();
        CHECK(frames <= 12000);  // the cap holds for the voice too
        while (app->auditionStatus().playing) {
            backend.run(1);
            app->tick();
            REQUIRE(backend.cycles() < 128);
        }
        CHECK(backend.cycles() * 256 - start - static_cast<std::int64_t>(frames) < 256);
        const auto& out = backend.capture().channels;
        for (std::size_t c = 0; c < 4; ++c) {
            CAPTURE(c);
            std::size_t wrong = out[c].size();
            for (auto i = static_cast<std::size_t>(from);
                 i < static_cast<std::size_t>(backend.cycles() * 256); ++i) {
                const std::size_t at = i - static_cast<std::size_t>(start);
                const bool voice = c < 2 && i >= static_cast<std::size_t>(start) && at < frames;
                const float want = voice ? expected->channels[c][at] : 0.0f;
                if (out[c][i] != want) {
                    wrong = i;
                    break;
                }
            }
            CHECK(wrong == out[c].size());
        }
    };
    playsHead(wav, 11025, 0);  // 12000 frames at 48 kHz are 11025 at 44.1 kHz

    // Above the device's rate the cap counts the file's frames: 12000 of
    // them, 0.125 s, played as 6000 at 48 kHz.
    auto fastArgs = json::Value::object();
    fastArgs["path"] = "fast.wav";
    const std::int64_t before = backend.cycles() * 256;
    auto fastPlayed = call("audition.play", fastArgs);
    REQUIRE(fastPlayed);
    CHECK(fastPlayed->find("seconds")->asDouble() == doctest::Approx(0.125));
    CHECK(fastPlayed->find("sample_rate")->asDouble() == 96000.0);
    CHECK(fastPlayed->find("truncated")->asBool());
    playsHead(fast, 12000, before);

    // audition.stop ends one that plays; a new project does too.
    REQUIRE(call("audition.play", args));
    auto stopped = call("audition.stop", json::Value::object());
    REQUIRE(stopped);
    CHECK_FALSE(stopped->find("playing")->asBool());
    CHECK_FALSE(app->auditionStatus().playing);
    REQUIRE(call("audition.play", args));
    decode();
    CHECK(app->auditionStatus().playing);
    REQUIRE(call("project.new", json::Value::object()));
    CHECK_FALSE(app->auditionStatus().playing);
    app->stopEngine();
}

TEST_CASE("audition: a cancel or a newer request ends the file's decode between chunks") {
    // A read asks whether to stop before each chunk (16384 frames); this
    // file has three. The cancel, or the newer request, is made from the
    // read's first ask, so it lands mid-read whatever the timing. The read
    // must end at that ask and its result be dropped. One that read on would
    // ask three times and hand over the whole file.
    const fs::path root =
        fs::temp_directory_path() /
        ("supple-audition-cancel-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { fs::remove_all(dir); }
    } cleanup{root};
    audio::AudioData file;
    file.sampleRate = 48000;
    file.channels.assign(2, std::vector<float>(3 * 16384, 0.25f));
    const fs::path wav = root / "long.wav";
    REQUIRE(audio::writeWav(wav, file));
    for (const bool newer : {false, true}) {
        CAPTURE(newer);
        std::atomic<int> asks{0}, woken{0};
        AuditionLoader* self = nullptr;
        AuditionLoader loader([&] { ++woken; },
                              [&](std::uint64_t generation) {
                                  if (generation != 1 || asks++ != 0) return;  // the first read
                                  if (newer)
                                      self->request({root / "missing.wav", 1 << 20, 48000});
                                  else
                                      self->cancel();
                              });
        self = &loader;
        REQUIRE(loader.request({wav, 1 << 20, 48000}) == 1);
        // Decided by the loader's state: the first read dropped, or handed over.
        while (loader.discarded() == 0 && (newer || woken == 0)) std::this_thread::yield();
        CHECK(asks == 1);
        CHECK(loader.discarded() == 1);
        if (newer) {
            // The newer request is answered: the missing file's error.
            while (woken == 0) std::this_thread::yield();
            auto done = loader.take();
            REQUIRE(done);
            CHECK(done->generation == 2);
            REQUIRE_FALSE(done->voice);
            CHECK(done->voice.error().code == "audio.open");
        } else {
            CHECK(woken == 0);
            CHECK_FALSE(loader.take());
        }
    }
}

TEST_CASE("audition: a stopping engine is silent in a cycle the device runs after the request" *
          doctest::description("rt")) {
    // Engine::stop asks the callback for silence, then closes the device; a
    // device may run another cycle in between. This one runs one in stop().
    struct LateCycle : DummyBackend {
        using DummyBackend::DummyBackend;
        bool late = true;
        void stop() override {
            if (std::exchange(late, false)) run(1);
            DummyBackend::stop();
        }
    };
    const model::Session silent = model::Session::create({.console9 = false});
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        CAPTURE(options.anticipative);
        auto pool = makePool();
        MediaCache media;
        LateCycle backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4 * kBlock});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        REQUIRE(engine.load(silent));
        const auto voice = rampVoice(100000);
        REQUIRE(engine.audition(voice) != 0);
        backend.run(2);
        CHECK(engine.auditionState().playing);
        engine.stop();
        REQUIRE(backend.cycles() == 3);  // the late cycle ran
        const auto& out = backend.capture().channels;
        for (std::size_t c = 0; c < 2; ++c) {
            CAPTURE(c);
            // Heard up to the request, at unity...
            CHECK(out[c][2 * kBlock - 1] == (c == 0 ? voice->left : voice->right)[2 * kBlock - 1]);
            // ... and not at all in the cycle after it.
            CHECK(std::all_of(out[c].begin() + 2 * kBlock, out[c].begin() + 3 * kBlock,
                              [](float v) { return v == 0.0f; }));
        }
    }
}
