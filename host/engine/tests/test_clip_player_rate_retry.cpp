// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A trajectory change prepared again (S3 step D4, review round 166): an
// attempt the renderer overtakes prepares again with its hand, so its
// switch S, later; three fail, and the plan is prepared once more as a seek
// at the consumed position. The late attempts are forced (a test hook: no
// clock, no load). Launched audio crosses the change by the miss path, so
// what plays depends on S alone: two runs with the same attempts late play
// the same, and each late attempt moves S. Its own ctest entry.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

namespace {
struct RetryRun {
    std::vector<float> heard;
    std::uint32_t latency = 0;
    std::int64_t S = INT64_MIN;  // the change's switch as prepared (none: a seek)
    std::uint64_t seeks = 0;     // epochs begun from the edit on (review round 167 S2)
};

// The first frame where two runs differ, or -1; first that their change
// switched at the same frame, without which they differ for that alone.
std::int64_t firstDifference(const RetryRun& a, const RetryRun& b) {
    INFO("switches at " << a.S << " and " << b.S);
    REQUIRE(a.S == b.S);
    if (a.latency != b.latency || a.heard.size() != b.heard.size()) return 0;
    for (std::size_t k = 0; k < a.heard.size(); ++k)
        if (a.heard[k] != b.heard[k]) return std::int64_t(k);
    return -1;
}

// cl_2 plays, cl_3 is queued for bar 3 (running frame 256000), and the
// envelope is added so that, with no attempt late, S lands `d` frames
// before it; `late` of its attempts are late. The renderer settles first,
// so nothing but the hook makes an attempt late.
RetryRun queuedRun(const EngineOptions& options, std::int64_t d, int late) {
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
    const std::uint64_t epoch = epochForTests(*p.app->engine());
    lateAttemptsForTests(*p.app->engine(), late);
    p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                     R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
    lateAttemptsForTests(*p.app->engine(), 0);
    RetryRun out;
    out.S = preparedSwitchForTests(*p.app->engine());
    for (std::int64_t f = 256000 - lag - d + 1024; f < 420000; f += 1024) {
        live.until(f);
        p.app->engine()->service();  // as the app's tick
    }
    CHECK(p.app->engine()->processErrors() == 0);
    out.seeks = epochForTests(*p.app->engine()) - epoch;
    out.heard = live.stop();
    out.latency = live.latency;
    return out;
}
}  // namespace

TEST_CASE("late preparation attempts move a change's switch later; three end in a seek" *
          doctest::description("rt")) {
    // One late attempt puts the hand, so S, at least a quantum later, and
    // what plays moves with it; two runs with that one attempt late play
    // the same, sample for sample; a second, later again. Three: the third
    // fails (playrate.prepare_late), and the plan is prepared again as a
    // seek at the consumed position, with no trajectory change (S none),
    // the same in two runs. The callback's case and the renderer's (a
    // warm-up switching after S).
    struct Case {
        bool anticipative;
        std::int64_t d;
    };
    for (const Case c : {Case{false, -300}, Case{true, 200}}) {
        INFO("anticipative " << c.anticipative << ", d " << c.d);
        const EngineOptions options = c.anticipative ? EngineOptions{.workers = 0} : kInCallback;
        RetryRun previous = queuedRun(options, c.d, 0);
        REQUIRE(previous.S != INT64_MIN);
        CHECK(previous.seeks == 0);
        for (const int late : {1, 2}) {
            INFO(late << " late");
            const RetryRun run = queuedRun(options, c.d, late);
            INFO("switches at " << previous.S << " (one fewer late) and " << run.S);
            CHECK(run.S >= previous.S + detail::kRateQuantum);
            CHECK(run.seeks == 0);                           // a retry is no seek
            const bool moved = run.heard != previous.heard;  // S moved: what plays moved
            CHECK(moved);
            if (late == 1) CHECK(firstDifference(run, queuedRun(options, c.d, late)) == -1);
            previous = run;
        }
        const RetryRun sought = queuedRun(options, c.d, 3);
        CHECK(sought.S == INT64_MIN);
        CHECK(sought.seeks == 1);  // the recovery seek, a new epoch
        CHECK(firstDifference(sought, queuedRun(options, c.d, 3)) == -1);
    }
}
