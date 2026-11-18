// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A file audition in the device callback (ADR-0094): outputs 1/2, after the
// project's output and outside it, in both executors, stopped or playing.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "realtime_common.h"

using namespace supple;
using namespace supple::engine;

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
