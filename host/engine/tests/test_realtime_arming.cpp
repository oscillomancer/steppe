// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Real time equals offline when a track is armed or monitored mid-song.
#include <doctest/doctest.h>

#include "realtime_support.h"

// Arming, a monitor change or disarming while the song plays moves the
// instrument between the anticipative engine's renderer and its live region;
// the timeline note it holds keeps sounding, exactly as offline, at unity and
// under a master playrate, and a key held on the keyboard ends when the
// keyboard stops being heard.
TEST_CASE("arming or monitoring mid-song keeps an instrument's held timeline note" *
          doctest::description("rt")) {
    constexpr std::int64_t kSegment = 80 * 256;  // device frames between session changes
    // A change takes effect within this (the anticipative engine's at its render horizon).
    constexpr std::int64_t kSettle = 48 * 256;
    struct Step {
        bool armed;
        model::Monitor monitor;
    };
    const std::array<Step, 5> steps = {{{false, model::Monitor::On},
                                        {true, model::Monitor::On},
                                        {true, model::Monitor::Auto},
                                        {true, model::Monitor::On},
                                        {false, model::Monitor::On}}};
    constexpr std::int64_t kFrames = 5 * kSegment;
    // Keys held on the keyboard, while monitored: one ended by Auto, one by disarming.
    constexpr std::int64_t kFirst = kSegment + kSettle + 512,
                           kSecond = 3 * kSegment + kSettle + 512;
    struct Rate {
        double rate;
        bool preserve;
    };
    // A CLAP and a VST3 instrument: their reset and processing calls differ.
    auto scanned = host::scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(scanned);
    const auto vst3 = std::find_if(scanned->begin(), scanned->end(), [](const host::PluginInfo& p) {
        return p.name == "Test VST3 Sine";
    });
    REQUIRE(vst3 != scanned->end());
    for (const std::string& plugin : {std::string("com.suppleaudio.test.sine"), vst3->id})
        for (const Rate r : {Rate{1.0, false}, Rate{1.25, false}, Rate{1.25, true}}) {
            auto pool = makeVstPool();
            // Offline output is in device frames: 3 s of timeline is 2.4 s at 1.25.
            const auto offline = renderOffline(heldNoteSession(false, model::Monitor::On, plugin,
                                                               r.rate, r.preserve),
                                               pool, {.startSeconds = 0, .endSeconds = 3.0})
                                     .value();
            const auto& want = offline.audio.channels[0];
            REQUIRE(want.size() >= static_cast<std::size_t>(kFrames + 1024));
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO(plugin << ", rate " << r.rate << ", preserve " << r.preserve << ", block "
                                << block << ", anticipative " << options.anticipative
                                << ", workers " << options.workers);
                    auto rtPool = makeVstPool();
                    MediaCache media;
                    DummyBackend backend(keyboard({{static_cast<int>(kFirst), 0x90, 76, 100},
                                                   {static_cast<int>(kSecond), 0x90, 72, 100}},
                                                  kFrames + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    const auto session = [&](const Step& s) {
                        return heldNoteSession(s.armed, s.monitor, plugin, r.rate, r.preserve);
                    };
                    REQUIRE(engine.load(session(steps[0])));
                    REQUIRE(engine.awaitPlan());
                    engine.play();
                    for (std::size_t i = 0; i < steps.size(); ++i) {
                        if (i) {
                            REQUIRE(engine.load(session(steps[i])));
                            // At unity, while everything is live, the renderer keeps a
                            // render-ahead past what the callback has played when it
                            // next runs, and hands over there. Waiting keeps that within
                            // 200 ms (below kSettle) of the change, however late a busy
                            // machine runs the renderer thread; a device's clock would
                            // pace it. A rate plan's handoff is fixed when it is loaded,
                            // and adopted only once played up to, so it is not awaited.
                            if (r.rate == 1.0) REQUIRE(engine.awaitPlan());
                        }
                        runFrames(engine, backend, options, kSegment, block);
                        engine.service();
                    }
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    REQUIRE(engine.outputLatency() == 0);
                    const auto& L = backend.capture().channels[0];
                    // The output equals the offline render from `from` to `to`.
                    const auto offlineFrom = [&](std::int64_t from, std::int64_t to) {
                        const auto a = static_cast<std::size_t>(from);
                        return maxDiff(
                            L, a,
                            std::vector<float>(want.begin() + static_cast<std::ptrdiff_t>(from),
                                               want.end()),
                            static_cast<std::size_t>(to - from));
                    };
                    const auto keyHeard = [&](std::int64_t at) {
                        const auto i = static_cast<std::size_t>(at + 100);
                        return std::fabs(double(L[i]) - double(want[i])) > 0.0;
                    };
                    // Arming keeps the timeline note; the first key only adds to it.
                    CHECK(offlineFrom(0, kFirst) == 0.0);
                    CHECK(keyHeard(kFirst));
                    // Auto does not hear the keyboard while playing: its key ends, the
                    // note plays on through Auto and On again until the second key.
                    CHECK(offlineFrom(2 * kSegment + kSettle, kSecond) == 0.0);
                    CHECK(keyHeard(kSecond));
                    // Disarmed: the second key ends, the timeline note still sounds.
                    CHECK(offlineFrom(4 * kSegment + kSettle, kFrames) == 0.0);
                    CHECK(std::fabs(double(L[static_cast<std::size_t>(kFrames - 100)])) > 0.0);
                }
        }
}

// An effect's tail crosses into the live region with its strip: a short note
// ends before the strip is armed (or starts monitoring), and its echo, still
// inside the effect's delay line at the handoff, is heard as offline. Alone
// the strip opens the live region; beside an armed track (`pads`) it joins
// the region that exists (adoptLive's continuing branch).
TEST_CASE("arming or monitoring mid-song keeps an effect's tail" * doctest::description("rt")) {
    constexpr std::int64_t kEdit = 12800, kFrames = 64000;
    struct Change {
        const char* name;
        bool armedBefore;
        model::Monitor before;
    };
    for (const bool pads : {false, true}) {
        auto pool = makePool();
        const auto offline = renderOffline(echoSession(false, model::Monitor::On, pads), pool,
                                           {.startSeconds = 0, .endSeconds = 1.5})
                                 .value();
        const auto& want = offline.audio.channels[0];
        REQUIRE(want.size() >= static_cast<std::size_t>(kFrames));
        for (const Change change : {Change{"arming", false, model::Monitor::On},
                                    Change{"monitoring on", true, model::Monitor::Off}})
            for (const std::uint32_t block : {256u, 37u})
                for (const EngineOptions& options : kBothExecutors) {
                    INFO(std::string(change.name)
                         << ", pads " << pads << ", block " << block << ", anticipative "
                         << options.anticipative << ", workers " << options.workers);
                    auto rtPool = makePool();
                    MediaCache media;
                    DummyBackend backend(keyboard({}, kFrames + 4096));
                    Engine engine(rtPool, media, options);
                    REQUIRE(
                        engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                    REQUIRE(engine.load(echoSession(change.armedBefore, change.before, pads)));
                    REQUIRE(engine.awaitPlan());
                    engine.play();
                    runFrames(engine, backend, options, kEdit, block);
                    REQUIRE(engine.load(echoSession(true, model::Monitor::On, pads)));
                    runFrames(engine, backend, options, kFrames - kEdit, block);
                    engine.service();
                    CHECK(engine.processErrors() == 0);
                    engine.stop();
                    REQUIRE(engine.outputLatency() == 0);
                    const auto& L = backend.capture().channels[0];
                    CHECK(maxDiff(L, 0, want, static_cast<std::size_t>(kFrames)) == 0.0);
                    double echo = 0;
                    for (std::size_t i = 48000; i < 54000; ++i)
                        echo = std::max(echo, std::fabs(double(L[i])));
                    CHECK(echo > 0.05);
                }
    }
}

// Beside an armed track, a strip whose handoff the callback reaches late
// (the renderer stalled, the callback played on past it) starts afresh in
// the live region: its delay line is dropped, so the echo the renderer
// carried is not heard, and from the handoff on the output is what a fresh
// start after the note gives (offline from there: nothing to echo).
TEST_CASE("a late handoff beside an armed track drops an effect's tail" *
          doctest::description("rt")) {
    constexpr std::int64_t kFrom = 36000, kFrames = 96000;
    auto pool = makePool();
    const auto fresh = renderOffline(echoSession(true, model::Monitor::On, true), pool,
                                     {.startSeconds = double(kFrom) / 48000.0, .endSeconds = 2.0})
                           .value();
    REQUIRE(fresh.audio.channels[0].size() >= static_cast<std::size_t>(kFrames - kFrom));
    for (const std::uint32_t block : {256u, 37u})
        for (const int workers : {0, 2}) {
            INFO("block " << block << ", workers " << workers);
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(keyboard({}, kFrames + 4096));
            Engine engine(rtPool, media, EngineOptions{.workers = workers});
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                    .ok());
            REQUIRE(engine.load(echoSession(false, model::Monitor::On, true)));
            REQUIRE(engine.awaitPlan());
            engine.play();
            std::int64_t played = 0;
            auto playTo = [&](std::int64_t to, bool ahead) {
                const std::int64_t cycles = (to - played + block - 1) / block;
                if (cycles <= 0) return;
                if (ahead)
                    runAhead(engine, backend, cycles, block);
                else
                    backend.run(cycles);
                played += cycles * block;
            };
            playTo(12000, true);  // the note (to 6000) is in the echo's delay line
            engine.holdRenderer(true);
            REQUIRE(engine.awaitRendererHeld());
            playTo(28000, false);
            REQUIRE(engine.load(echoSession(true, model::Monitor::On, true)));
            REQUIRE(engine.awaitPlan());  // adopted by the renderer where it stalled
            playTo(32000, false);         // the callback takes it at once, past the handoff
            engine.holdRenderer(false);
            playTo(kFrames, true);
            engine.service();
            CHECK(engine.processErrors() == 0);
            engine.stop();
            const auto& L = backend.capture().channels[0];
            REQUIRE(L.size() >= static_cast<std::size_t>(kFrames));
            // (The exact handoff, in the test above, hears the echo at 48000.)
            CHECK(maxDiff(L, static_cast<std::size_t>(kFrom), fresh.audio.channels[0],
                          static_cast<std::size_t>(kFrames - kFrom)) == 0.0);
        }
}

// A handoff the callback reaches late (here past the renderer's position,
// after the renderer stalled and the frontier ran dry) cannot continue the
// carried node: what the renderer never rendered includes the note's end.
// The strip starts afresh, its instrument reset, so the note does not hang:
// at unity and under a master playrate.
TEST_CASE("a late handoff starts the newly live strip afresh: no note hangs" *
          doctest::description("rt")) {
    struct Rate {
        double rate;
        bool preserve;
    };
    for (const Rate r : {Rate{1.0, false}, Rate{1.25, false}, Rate{1.25, true}})
        for (const std::uint32_t block : {256u, 37u})
            for (const int workers : {0, 2}) {
                INFO("rate " << r.rate << ", preserve " << r.preserve << ", block " << block
                             << ", workers " << workers);
                const EngineOptions options{.workers = workers};
                auto pool = makePool();
                MediaCache media;
                DummyBackend backend(keyboard({}, 96000 + 4096));
                Engine engine(pool, media, options);
                REQUIRE(engine
                            .start(backend,
                                   {.sampleRate = 48000, .blockSize = block, .midiInputs = true})
                            .ok());
                REQUIRE(engine.load(twoKeysSession(false, r.rate, r.preserve)));
                REQUIRE(engine.awaitPlan());
                engine.play();
                std::int64_t played = 0;
                // Plays device callbacks up to frame `to`, the renderer kept ahead or not.
                auto playTo = [&](std::int64_t to, bool ahead) {
                    const std::int64_t cycles = (to - played + block - 1) / block;
                    if (cycles <= 0) return;
                    if (ahead)
                        runAhead(engine, backend, cycles, block);
                    else
                        backend.run(cycles);
                    played += cycles * block;
                };
                playTo(24000, true);
                // The renderer stops before the note's end (at 48000, or 38400 at
                // 1.25); the callback plays on past it, missing the frontier.
                engine.holdRenderer(true);
                REQUIRE(engine.awaitRendererHeld());
                host::Instance* const keys = pool.find(keysInstrument(twoKeysSession(false)));
                REQUIRE(keys);
                const std::uint32_t resetsBefore = keys->resets();
                if (r.rate == 1.0) {
                    playTo(60000, false);
                    // The renderer adopts the plan at its stalled position; the
                    // callback, past that handoff, takes it at once.
                    REQUIRE(engine.load(twoKeysSession(true, r.rate, r.preserve)));
                    REQUIRE(engine.awaitPlan());
                    playTo(68000, false);
                } else {
                    // A rate plan's handoff is its prepared boundary, a render-ahead
                    // span past the callback (before the note's end, edited here):
                    // the renderer reaches it once released, after the callback
                    // has played past it and past the note's end.
                    REQUIRE(engine.load(twoKeysSession(true, r.rate, r.preserve)));
                    playTo(68000, false);
                }
                engine.holdRenderer(false);
                playTo(96000, true);
                engine.service();
                // The callback, not continuing the instrument, reset it.
                CHECK(keys->resets() > resetsBefore);
                CHECK(engine.processErrors() == 0);
                engine.stop();
                const auto& L = backend.capture().channels[0];
                REQUIRE(L.size() >= 96000);
                double before = 0, after = 0;
                for (std::size_t i = 1000; i < 20000; ++i)
                    before = std::max(before, std::fabs(double(L[i])));
                for (std::size_t i = 72000; i < 96000; ++i)
                    after = std::max(after, std::fabs(double(L[i])));
                CHECK(before > 0.05);
                CHECK(after == 0.0);
            }
}
