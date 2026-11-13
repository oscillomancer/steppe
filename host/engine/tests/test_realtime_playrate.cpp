// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Real time equals offline when arming under a master playrate.
#include <doctest/doctest.h>

#include "realtime_support.h"

// With another track already armed the live region exists, so a strip armed
// mid-note joins it (adoptLive's continuing branch). It continues exactly,
// at unity (also with a loop wrap between the edit and the handoff) and
// under a master playrate (also looping), with a handoff inside an
// odd-sized callback too: the same engine never armed plays the same, and
// nothing hangs.
TEST_CASE("arming beside an armed track mid-note: exact at unity and under a playrate" *
          doctest::description("rt")) {
    constexpr std::int64_t kEdit = 12800, kFrames = 96000;
    auto pool = makePool();
    const auto offline =
        renderOffline(twoKeysSession(false), pool, {.startSeconds = 0, .endSeconds = 2.5}).value();
    REQUIRE(offline.audio.channels[0].size() >= static_cast<std::size_t>(kFrames));
    struct Rate {
        double rate;
        bool preserve;
        bool envelope = false;  // a playrate envelope, 1 to 1.5
    };
    for (const Rate r :
         {Rate{1.0, false}, Rate{1.25, false}, Rate{1.25, true}, Rate{1.0, false, true}})
        for (const bool loop : {false, true})
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    if (r.envelope && loop) continue;
                    const double rate = r.envelope ? 0.0 : r.rate;  // 0: the envelope
                    INFO("rate " << rate << ", preserve " << r.preserve << ", loop " << loop
                                 << ", block " << block << ", anticipative " << options.anticipative
                                 << ", workers " << options.workers);
                    // `arm`: arm Keys at kEdit; else the reference, never armed.
                    // `resets`: how often the Keys instrument was reset from the
                    // edit to the end.
                    std::uint32_t resets = 0;
                    auto run = [&](bool arm) {
                        auto rtPool = makePool();
                        MediaCache media;
                        DummyBackend backend(keyboard({}, kFrames + 4096));
                        Engine engine(rtPool, media, options);
                        REQUIRE(engine
                                    .start(backend, {.sampleRate = 48000,
                                                     .blockSize = block,
                                                     .midiInputs = true})
                                    .ok());
                        REQUIRE(engine.load(twoKeysSession(false, r.rate, r.preserve, r.envelope)));
                        REQUIRE(engine.awaitPlan());
                        // The loop wraps 2000 frames after the edit, before its handoff.
                        if (loop) engine.setLoop(0.0, double(kEdit + 2000) / 48000.0, true);
                        engine.play();
                        runFrames(engine, backend, options, kEdit, block);
                        if (loop && rate == 1.0 && options.anticipative) {
                            // A unity handoff is where the renderer is when it adopts
                            // the plan: rendered at least 4096 frames ahead of the
                            // playhead (under kEdit + block) at the edit, so past the
                            // wrap at kEdit + 2000, which the playhead has not reached.
                            REQUIRE(options.renderAheadMs * 48.0 >= 4096.0);
                            REQUIRE(engine.awaitRendered(4096));
                            REQUIRE((kEdit + block - 1) / block * block < kEdit + 2000);
                        }
                        host::Instance* const keys =
                            rtPool.find(keysInstrument(twoKeysSession(false)));
                        REQUIRE(keys);
                        const std::uint32_t resetsBefore = keys->resets();
                        if (arm)
                            REQUIRE(
                                engine.load(twoKeysSession(true, r.rate, r.preserve, r.envelope)));
                        runFrames(engine, backend, options, kFrames - kEdit, block);
                        engine.service();
                        resets = keys->resets() - resetsBefore;
                        CHECK(engine.processErrors() == 0);
                        engine.stop();
                        REQUIRE(engine.outputLatency() == 0);
                        return backend.capture().channels[0];
                    };
                    const auto reference = run(false);
                    const auto armed = run(true);
                    // An exact handoff continues the instrument: never reset.
                    CHECK(resets == 0);
                    // Arming changes nothing: the never-armed run of this engine.
                    CHECK(maxDiff(armed, 0, reference, static_cast<std::size_t>(kFrames)) == 0.0);
                    if (rate == 1.0) {
                        if (!loop)
                            CHECK(maxDiff(armed, 0, offline.audio.channels[0],
                                          static_cast<std::size_t>(kFrames)) == 0.0);
                    } else if (!loop && !r.envelope) {
                        // The note ends at 1 s of timeline, 38400 device frames
                        // at 1.25: silence after it.
                        double after = 0;
                        for (std::size_t i = 40000; i < static_cast<std::size_t>(kFrames); ++i)
                            after = std::max(after, std::fabs(double(armed[i])));
                        CHECK(after == 0.0);
                        double during = 0;
                        for (std::size_t i = 1000; i < 12000; ++i)
                            during = std::max(during, std::fabs(double(armed[i])));
                        CHECK(during > 0.05);
                    }
                }
}

// One change that both turns a playrate on (unity to 1.25) and arms a strip
// mid-note cannot continue it: the first rate plan places its handoff in a
// new device coordinate. The strip starts afresh, its instrument reset, and
// nothing sounds after the note's end, also after a loop wrap.
TEST_CASE("turning a playrate on and arming in one change restarts the strip: no note hangs" *
          doctest::description("rt")) {
    for (const bool loop : {false, true})
        for (const std::uint32_t block : {256u, 37u})
            for (const EngineOptions& options : kBothExecutors) {
                INFO("loop " << loop << ", block " << block << ", anticipative "
                             << options.anticipative << ", workers " << options.workers);
                // Looping: [0, 1.5 s) wraps at 72000 before the edit at 80000
                // (timeline 8000, mid-note); the note ends at timeline 48000,
                // under 1.25 by device 116000, and the next wrap comes after
                // 132000, when the note starts again.
                const std::int64_t edit = loop ? 80000 : 12800, frames = loop ? 150000 : 96000;
                auto pool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, frames + 4096));
                Engine engine(pool, media, options);
                REQUIRE(engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                REQUIRE(engine.load(twoKeysSession(false)));
                REQUIRE(engine.awaitPlan());
                if (loop) engine.setLoop(0.0, 1.5, true);
                engine.play();
                runFrames(engine, backend, options, edit, block);
                host::Instance* const keys = pool.find(keysInstrument(twoKeysSession(false)));
                REQUIRE(keys);
                const std::uint32_t resetsBefore = keys->resets();
                REQUIRE(engine.load(twoKeysSession(true, 1.25)));
                runFrames(engine, backend, options, frames - edit, block);
                engine.service();
                if (options.anticipative) CHECK(keys->resets() > resetsBefore);
                CHECK(engine.effectivePlayrate() == doctest::Approx(1.25).epsilon(1e-3));
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& L = backend.capture().channels[0];
                REQUIRE(L.size() >= static_cast<std::size_t>(frames));
                const auto peak = [&](std::size_t from, std::size_t to) {
                    double x = 0;
                    for (std::size_t i = from; i < to; ++i)
                        x = std::max(x, std::fabs(double(L[i])));
                    return x;
                };
                CHECK(peak(1000, 12000) > 0.05);
                if (loop) {
                    CHECK(peak(118000, 132000) == 0.0);
                    CHECK(peak(136000, 146000) > 0.05);  // the next pass, the strip now live
                } else {
                    CHECK(peak(46000, 96000) == 0.0);
                }
            }
}

// Arming at the boundary where the playrate also changes (1.25 to 1.5):
// the strip continues from the renderer's old timeline into the new plan's,
// whose history up to the handoff is the old one's, and plays exactly as the
// never-armed run of the same engine. Both wait, before the edit, until the
// renderer is full, so the prepared boundary is the same in both runs.
TEST_CASE("arming while the playrate changes: exact" * doctest::description("rt")) {
    constexpr std::int64_t kEdit = 12800, kFrames = 96000;
    for (const bool preserve : {false, true})
        for (const std::uint32_t block : {256u, 37u})
            for (const EngineOptions& options : kBothExecutors) {
                INFO("preserve " << preserve << ", block " << block << ", anticipative "
                                 << options.anticipative << ", workers " << options.workers);
                std::uint32_t resets = 0;
                auto run = [&](bool arm) {
                    auto pool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, kFrames + 4096));
                    Engine engine(pool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(twoKeysSession(false, 1.25, preserve)));
                    REQUIRE(engine.awaitPlan());
                    engine.play();
                    runFrames(engine, backend, options, kEdit, block);
                    if (options.anticipative)
                        REQUIRE(engine.awaitRendered(std::numeric_limits<std::int64_t>::max()));
                    host::Instance* const keys = pool.find(keysInstrument(twoKeysSession(false)));
                    REQUIRE(keys);
                    const std::uint32_t before = keys->resets();
                    REQUIRE(engine.load(twoKeysSession(arm, 1.5, preserve)));
                    runFrames(engine, backend, options, kFrames - kEdit, block);
                    engine.service();
                    resets = keys->resets() - before;
                    CHECK(engine.effectivePlayrate() == 1.5);
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    return backend.capture().channels[0];
                };
                const auto reference = run(false);
                const auto armed = run(true);
                CHECK(resets == 0);
                CHECK(maxDiff(armed, 0, reference, static_cast<std::size_t>(kFrames)) == 0.0);
                double during = 0;
                for (std::size_t i = 1000; i < 12000; ++i)
                    during = std::max(during, std::fabs(double(armed[i])));
                CHECK(during > 0.05);
            }
}
