// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The mirror (S3 step D4, phase 2) in a looped song and with launches queued
// across a trajectory change: its reading is keyed by the switch's loop
// cycle from the launch log, a loop changed since is a miss, and whatever it
// finds or misses, what plays is the miss path's bit for bit. Its own ctest
// entry.
#include <optional>
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
struct MirrorRun {
    std::vector<float> heard;
    std::uint32_t latency = 0;
    std::vector<std::pair<std::int64_t, std::uint64_t>> around;  // primes per run about S
};

std::uint64_t primes(const MirrorRun& r) {
    std::uint64_t n = 0;
    for (const auto& [frame, count] : r.around) n += count;
    return n;
}

std::int64_t firstDifference(const MirrorRun& a, const MirrorRun& b) {
    if (a.latency != b.latency || a.heard.size() != b.heard.size()) return 0;
    for (std::size_t k = 0; k < a.heard.size(); ++k)
        if (a.heard[k] != b.heard[k]) return std::int64_t(k);
    return -1;
}

void addEnvelope(Project& p) {
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
}

// The song loops over bars 1-2 (song 0-192000, 256000 frames a cycle at
// 0.75), and cl_2 switches on bar 2 (running frame 128000 in cycle 0, 384000
// in cycle 1); the envelope is added 16000 frames after a frame `change`
// (S about 9600 later).
enum class LoopCase {
    LaterCycle,    // launched in cycle 1, the change in cycle 1
    Moved,         // as LaterCycle, the loop's end moved to bar 6 (song 480000) after the switch
    WrappedSince,  // launched in cycle 0, a wrap between its anchor and S (A6)
};
MirrorRun loopRun(const EngineOptions& options, LoopCase loop, bool mirror) {
    std::optional<MirrorOff> off;
    if (!mirror) off.emplace();
    Project p;
    twoTracks(p);
    p.run("transport.loop", R"({"start_seconds": 0, "end_seconds": 4, "enabled": true})");
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    constexpr std::uint32_t block = 128;
    AppLive live(p, options, block, 48000 * 11);
    REQUIRE(p.app->transport("play"));
    const bool later = loop != LoopCase::WrappedSince;
    live.until(later ? 260096 : 20 * block);
    p.run("launch.clip", R"({"clip": "cl_2"})");
    const std::int64_t switched = later ? 384000 : 128000;
    live.until(switched + 1024);
    if (loop == LoopCase::Moved)
        p.run("transport.loop", R"({"start_seconds": 0, "end_seconds": 10, "enabled": true})");
    // The change: 16000 after the switch in cycle 1, or after the wrap.
    const std::int64_t change = later ? 400000 : 272000;
    live.until(change - 128);
    MirrorRun out;
    out.around = primesPerRun(live, change - 128, change + 80000, block, [&](std::int64_t f) {
        if (f == change) addEnvelope(p);
    });
    CHECK(p.app->engine()->processErrors() == 0);
    const auto log = p.app->engine()->launchLog();
    REQUIRE(log.size() == 1);
    CHECK(log[0].runningFrame == switched);
    out.heard = live.stop();
    out.latency = live.latency;
    return out;
}
}  // namespace

TEST_CASE("a looped launch finds the mirror's reading by its switch's cycle; a loop moved misses" *
          doctest::description("rt")) {
    // Reader 0's key takes the switch's loop cycle from the log's song frame
    // (its whole loop passes, as no ring holds a frame long past): 1 for a
    // launch in cycle 1, and 0 for one in cycle 0 with a wrap since (a wrap
    // unanchors the launch, not the reading): the executor's, so the run
    // holding S primes nothing. With the loop's end moved to bar 6 after the
    // switch, its passes (192000 song samples) make 0 cycles of the new loop
    // (480000): a miss, which the executor primes (45). Each plays what the
    // miss path plays.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}})
        for (const LoopCase loop :
             {LoopCase::LaterCycle, LoopCase::Moved, LoopCase::WrappedSince}) {
            INFO("anticipative " << options.anticipative << ", loop case " << int(loop));
            const MirrorRun with = loopRun(options, loop, true);
            const MirrorRun without = loopRun(options, loop, false);
            // All of them about S (the renderer's can fall into two of its
            // slices as they are counted here).
            CHECK(primes(without) == 45u);
            CHECK(primes(with) == (loop == LoopCase::Moved ? 45u : 0u));
            CHECK(firstDifference(with, without) == -1);
        }
}

TEST_CASE("launches queued across a trajectory change play the same with the mirror" *
          doctest::description("rt")) {
    // "a launch queued across a trajectory change" (test_clip_player_rate_
    // trajectory_queued.cpp): cl_2 plays, cl_3 is queued for bar 3 (running
    // frame 256000), and the envelope's switch S lands `d` frames before it
    // (after it for d < 0): a warm-up switching before S, one under way at
    // the adoption switching after it. A warm-up under way when the change
    // is prepared is found: switching after S (reader 2 for its switch; the
    // executors prime 4 there instead of 94), in [handAt, S] (reader 2 for S:
    // the renderer's d -30), or switched by the adoption (reader 0 for S,
    // round 163 S1: the renderer's d -300). One that begins between the
    // mirror's read and the callback's adoption (every callback case here:
    // the race case) is missed and primed by the executor. Whatever it finds
    // or misses, what plays is the miss path's.
    struct Case {
        bool anticipative;
        std::int64_t d;
    };
    for (const Case c : {Case{false, -2000}, Case{false, -300}, Case{true, -300}, Case{true, -30},
                         Case{true, 200}, Case{true, 2500}}) {
        INFO("anticipative " << c.anticipative << ", d " << c.d);
        MirrorRun runs[2];
        std::uint64_t primed[2] = {0, 0};  // by the executors over the run, with and without
        for (const bool mirror : {true, false}) {
            std::optional<MirrorOff> off;
            if (!mirror) off.emplace();
            Project p;
            twoTracks(p);
            auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
            args["audio_file"] = p.ramp("second.wav", 48000).string();
            p.runArgs("clip.add", args);  // cl_3
            p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
            constexpr std::uint32_t block = 128;
            const EngineOptions options =
                c.anticipative ? EngineOptions{.workers = 0} : kInCallback;
            AppLive live(p, options, block, 48000 * 9 + 16384);
            REQUIRE(p.app->transport("play"));
            live.until(20 * block);
            p.run("launch.clip", R"({"clip": "cl_2"})");
            live.until(200000);
            p.run("launch.clip", R"({"clip": "cl_3"})");  // bar 3
            const std::int64_t lag = c.anticipative ? 9793 : 9601;
            live.until(256000 - lag - c.d);
            addEnvelope(p);
            std::uint64_t executed = 0;  // primed by the executors, not the main thread
            for (std::int64_t f = 256000 - lag - c.d + 1024; f < 420000; f += 1024) {
                const std::uint64_t before = detail::stretchPrimes();
                live.until(f);
                executed += detail::stretchPrimes() - before;
                p.app->engine()->service();  // as the app's tick
            }
            primed[mirror ? 0 : 1] = executed;
            CHECK(p.app->engine()->processErrors() == 0);
            REQUIRE(p.app->engine()->launchLog().size() == 2);
            MirrorRun& out = runs[mirror ? 0 : 1];
            out.heard = live.stop();
            out.latency = live.latency;
        }
        CHECK(firstDifference(runs[0], runs[1]) == -1);
        INFO("the executors primed " << primed[0] << " with the mirror, " << primed[1]
                                     << " without");
        if (c.anticipative)                      // the renderer adopts long after its read
            CHECK(primed[0] + 44 <= primed[1]);  // the warm-up under way, or switched, found
        else
            CHECK(primed[0] == primed[1]);  // begun after the read: a miss (the race case)
    }
}
