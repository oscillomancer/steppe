// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The mirror (S3 step D4, phase 2) when a trajectory change is prepared
// again (review round 166): an attempt the renderer overtakes prepares again
// with its hand, so its switch S, later; three fail, and the plan is
// prepared once more as a seek at the consumed position. The late attempts
// are forced here (a test hook: no clock, no load), in both runs alike:
// whatever S the change gets, the mirror plays what the miss path plays, and
// no reader keeps a mark an earlier attempt left. Its own ctest entry.
#include <optional>
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
struct RetryRun {
    std::vector<float> heard;
    std::uint32_t latency = 0;
    std::int64_t S = INT64_MIN;  // the change's switch as prepared (none: a seek)
    int marked = -1;             // readers the mirror marked once prepared (in-callback)
};

// The first frame where two runs differ, or -1.
std::int64_t firstDifference(const RetryRun& a, const RetryRun& b) {
    if (a.latency != b.latency || a.heard.size() != b.heard.size()) return 0;
    for (std::size_t k = 0; k < a.heard.size(); ++k)
        if (a.heard[k] != b.heard[k]) return std::int64_t(k);
    return -1;
}

// "launches queued across a trajectory change play the same with the
// mirror" (test_clip_player_rate_mirror_queued.cpp): cl_2 plays, cl_3 is
// queued for bar 3 (running frame 256000), and the envelope is added so
// that, with no attempt late, S lands `d` frames before it; `late` of its
// attempts are late.
RetryRun queuedRun(const EngineOptions& options, std::int64_t d, bool mirror, int late) {
    std::optional<MirrorOff> off;
    if (!mirror) off.emplace();
    Project p;
    twoTracks(p);
    auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
    args["audio_file"] = p.ramp("second.wav", 48000).string();
    p.runArgs("clip.add", args);  // cl_3
    p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
    constexpr std::uint32_t block = 128;
    AppLive live(p, options, block, 48000 * 9 + 16384);
    REQUIRE(p.app->transport("play"));
    live.until(20 * block);
    p.run("launch.clip", R"({"clip": "cl_2"})");
    live.until(200000);
    p.run("launch.clip", R"({"clip": "cl_3"})");  // bar 3
    const std::int64_t lag = options.anticipative ? 9793 : 9601;
    live.until(256000 - lag - d);
    live.settle();
    lateAttemptsForTests(*p.app->engine(), late);
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    lateAttemptsForTests(*p.app->engine(), 0);
    RetryRun out;
    out.S = preparedSwitchForTests(*p.app->engine());
    if (!options.anticipative) out.marked = mirrorMarkedForTests(*p.app->engine());
    for (std::int64_t f = 256000 - lag - d + 1024; f < 420000; f += 1024) {
        live.until(f);
        p.app->engine()->service();  // as the app's tick
    }
    CHECK(p.app->engine()->processErrors() == 0);
    out.heard = live.stop();
    out.latency = live.latency;
    return out;
}

struct Case {
    bool anticipative;
    std::int64_t d;
};
}  // namespace

TEST_CASE("a change prepared again switches later, and the mirror plays the miss path there" *
          doctest::description("rt")) {
    // One late attempt puts the hand, so S, at least a quantum later; a
    // second, later again: what plays changes with S alone (the mirror off
    // in both runs). With the mirror on and the same attempts late, S is
    // the same and so is every sample: an earlier attempt's readings were
    // for an S that is not this one's. The callback case: a reading for S
    // marked once prepared (reader 0, cl_2's); the renderer's: the warm-up
    // under way at S (d -30) and switching after it (d 200).
    for (const Case c : {Case{false, -300}, Case{true, -30}, Case{true, 200}}) {
        INFO("anticipative " << c.anticipative << ", d " << c.d);
        const EngineOptions options = c.anticipative ? EngineOptions{.workers = 0} : kInCallback;
        RetryRun previous = queuedRun(options, c.d, false, 0);
        REQUIRE(previous.S != INT64_MIN);
        for (const int late : {1, 2}) {
            INFO(late << " late");
            const RetryRun with = queuedRun(options, c.d, true, late);
            const RetryRun without = queuedRun(options, c.d, false, late);
            INFO("switches at " << previous.S << " (one fewer late), " << with.S << " and "
                                << without.S);
            CHECK(without.S >= previous.S + detail::kRateQuantum);
            CHECK(firstDifference(without, previous) != -1);  // S moved: what plays moved
            REQUIRE(with.S == without.S);
            CHECK(firstDifference(with, without) == -1);
            if (!c.anticipative) {
                CHECK(with.marked > 0);
                CHECK(without.marked == 0);
            }
            previous = without;
        }
    }
}

TEST_CASE("a change whose preparation fails leaves no mirror mark for the seek after it" *
          doctest::description("rt")) {
    // Three late attempts: the third fails (playrate.prepare_late) after it
    // primed, and the plan is prepared again as a seek at the consumed
    // position, with no trajectory change (S none). Before review round 166
    // its readers kept the third attempt's marks; now none, and the seek
    // plays the same with the mirror as without.
    for (const Case c : {Case{false, -300}, Case{true, 200}}) {
        INFO("anticipative " << c.anticipative << ", d " << c.d);
        const EngineOptions options = c.anticipative ? EngineOptions{.workers = 0} : kInCallback;
        const RetryRun with = queuedRun(options, c.d, true, 3);
        const RetryRun without = queuedRun(options, c.d, false, 3);
        CHECK(with.S == INT64_MIN);
        CHECK(without.S == INT64_MIN);
        if (!c.anticipative) CHECK(with.marked == 0);
        CHECK(firstDifference(with, without) == -1);
    }
}
