// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Launches around a trajectory change (S3 step D4, phase 1): one queued
// across an envelope edit, one switching just before a change of preserve
// pitch, and one in a later loop cycle when preserve pitch is turned on.
// Split from test_clip_player_rate_trajectory.cpp to keep each entry well
// inside its limit under ThreadSanitizer.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a launch queued across a trajectory change" * doctest::description("rt")) {
    // cl_2 plays; cl_3 is queued for bar 3 (running frame 256000), and a rate
    // envelope is added so that the change's switch S lands `d` frames
    // before bar 3 (the change reaches S about 9600 frames after the edit
    // here, a little later in the anticipative executor).
    // - d <= 0: cl_3 switches first, warmed on the old trajectory, then
    //   crossfades into the new one at S.
    // - d > 0 in the anticipative executor: its warm-up is under way when
    //   the change is adopted, so it starts again on the new trajectory and
    //   still switches at bar 3.
    // - d > 0 in the callback: the change voids the launch, not yet applied,
    //   and placement keeps it clear of the pending change: it lands a bar
    //   later (the app's service places it again).
    struct Case {
        bool anticipative;
        std::int64_t d;
        double beat;  // where it lands: bar 3, or bar 4
    };
    for (const Case c :
         {Case{false, -2000, 8}, Case{false, -300, 8}, Case{true, -2000, 8}, Case{true, -300, 8},
          Case{true, 200, 8}, Case{true, 2500, 8}, Case{false, 2500, 12}}) {
        INFO("anticipative " << c.anticipative << ", d " << c.d);
        Project p;
        twoTracks(p);
        auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
        args["audio_file"] = p.ramp("second.wav", 48000).string();
        p.runArgs("clip.add", args);  // cl_3
        p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
        constexpr std::uint32_t block = 128;
        const EngineOptions options = c.anticipative ? EngineOptions{.workers = 0} : kInCallback;
        AppLive live(p, options, block, 48000 * 9 + 16384);
        REQUIRE(p.app->transport("play"));
        live.until(20 * block);
        p.run("launch.clip", R"({"clip": "cl_2"})");
        live.until(200000);
        p.run("launch.clip", R"({"clip": "cl_3"})");  // bar 3
        const std::int64_t lag = c.anticipative ? 9793 : 9601;
        live.until(256000 - lag - c.d);
        p.run("env.add", R"({"track": "tr_0", "param": "playrate", "points": [)"
                         R"({"beat": 0, "value": 0.75}, {"beat": 40, "value": 0.95}]})");
        for (std::int64_t f = 256000 - lag - c.d + 1024; f < 420000; f += 1024) {
            live.until(f);
            p.app->engine()->service();  // as the app's tick
        }
        const auto log = p.app->engine()->launchLog();
        CHECK(p.app->engine()->processErrors() == 0);
        ChangeRun run;
        run.heard = live.stop();
        run.latency = live.latency;
        REQUIRE(log.size() == 2);
        CHECK(log[1].clip == id("cl_3"));
        CHECK(log[1].anchorBeat == c.beat);
        if (c.beat == 8) CHECK(log[1].runningFrame == 256000);  // not moved
        const std::int64_t lands = log[1].runningFrame;
        CHECK(loudest(run, lands + 3000, lands + 30000) > 0.01);  // cl_3 plays
        if (c.d <= 0) {
            const std::int64_t S = 256000 - c.d;
            CHECK(largestStep(run, S - 300, S + 3000) <=
                  1.5 * largestStep(run, S + 20000, S + 40000));
        }
    }
}

TEST_CASE("a launch switching just before a change of preserve pitch" *
          doctest::description("rt")) {
    // Round 152 N2: cl_3 is queued for bar 3 (running frame 256000) and
    // preserve pitch is toggled so that the change's switch S lands about 300
    // frames later. Stretch to tape: cl_3 plays on tape from its own switch
    // (a tape reader on reader 2 holds no warm-up to rotate in), not on its
    // stretched warm-up until S. Tape to stretch: it plays on the new
    // trajectory from its switch (a tape plan warms nothing). Neither adds a
    // click to the switch's own (cl_3 starts with a step of its own): its
    // launch crossfade covers its start, and nothing is crossfaded at S.
    for (const bool toTape : {true, false})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("to tape " << toTape << ", anticipative " << options.anticipative);
            const auto play = [&](bool toggle) {
                Project p;
                twoTracks(p);
                auto args = j(R"({"track": "tr_2", "scene": "sc_2"})");
                args["audio_file"] = p.ramp("second.wav", 48000).string();
                p.runArgs("clip.add", args);  // cl_3
                const bool stretched = toTape;
                p.run("transport.playrate.set", stretched
                                                    ? R"({"rate": 0.75, "preserve_pitch": true})"
                                                    : R"({"rate": 0.75, "preserve_pitch": false})");
                constexpr std::uint32_t block = 128;
                AppLive live(p, options, block, 48000 * 8 + 16384);
                REQUIRE(p.app->transport("play"));
                live.until(20 * block);
                p.run("launch.clip", R"({"clip": "cl_2"})");
                live.until(200000);
                p.run("launch.clip", R"({"clip": "cl_3"})");  // bar 3
                const std::int64_t edit = 256000 + 300 - 9601;
                live.until(edit);
                if (toggle)
                    p.run("transport.playrate.set",
                          stretched ? R"({"rate": 0.75, "preserve_pitch": false})"
                                    : R"({"rate": 0.75, "preserve_pitch": true})");
                for (std::int64_t f = edit + 1024; f < 340000; f += 1024) {
                    live.until(f);
                    p.app->engine()->service();
                }
                const auto log = p.app->engine()->launchLog();
                CHECK(p.app->engine()->processErrors() == 0);
                ChangeRun run;
                run.heard = live.stop();
                run.latency = live.latency;
                REQUIRE(log.size() == 2);
                CHECK(log[1].clip == id("cl_3"));
                CHECK(log[1].runningFrame == 256000);
                return run;
            };
            const ChangeRun same = play(false);
            const ChangeRun moved = play(true);
            CHECK(largestStep(moved, 256000 - 300, 256000 + 3300) <=
                  1.5 * largestStep(same, 256000 - 300, 256000 + 3300));
            CHECK(loudest(moved, 259000, 290000) > 0.01);  // cl_3 plays
        }
}

TEST_CASE("turning preserve pitch on just after a launch in a later loop cycle" *
          doctest::description("rt")) {
    // Round 152 F2: the new stretched reading of a clip a tape reader played
    // takes the loop cycle of its switch, recorded at the switch
    // (launchSwitchCycle), not the tape reader's (which keeps none). With a
    // transport loop over song 0-4 s (256000 frames at 0.75), cl_2 launched
    // for bar 2 of the first or the second cycle, and preserve pitch turned on
    // so that S lands one quantum or 448 frames after its switch, the two
    // play the same, a loop apart. A pin of that result only: the converter
    // reads from S, after the anchor, so the cycle is not heard here, and
    // at one quantum the switch may land in the adoption's own slice and
    // not reach the carry's tape branch at all. The carried cycle itself is
    // checked at plan level (test_clip_player_rate_trajectory.cpp).
    for (const std::int64_t after : {115, 500})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("S about " << after << " after the switch, anticipative " << options.anticipative);
            ChangeRun runs[2];
            for (const int cycle : {0, 1}) {
                Project p;
                twoTracks(p);
                p.run("track.set", R"({"track": "tr_1", "mute": true})");  // not periodic
                p.run("transport.loop",
                      R"({"start_seconds": 0, "end_seconds": 4, "enabled": true})");
                p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": false})");
                AppLive live(p, options, 128, 48000 * 10 + 16384);
                REQUIRE(p.app->transport("play"));
                const std::int64_t offset = cycle * 256000;
                live.until(offset + 20 * 128);
                p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 2 of this cycle
                const std::int64_t edit = offset + 128000 + after - 9601;
                live.until(edit);
                p.run("transport.playrate.set", R"({"rate": 0.75, "preserve_pitch": true})");
                for (std::int64_t f = edit + 1024; f < offset + 200000; f += 1024) {
                    live.until(f);
                    p.app->engine()->service();
                }
                const auto log = p.app->engine()->launchLog();
                CHECK(p.app->engine()->processErrors() == 0);
                runs[cycle].heard = live.stop();
                runs[cycle].latency = live.latency;
                REQUIRE_FALSE(log.empty());
                CHECK(log.back().runningFrame == offset + 128000);
            }
            REQUIRE(runs[0].latency == runs[1].latency);
            double worst = 0;
            for (std::int64_t k = 120000; k < 190000; ++k) {
                const auto i0 = static_cast<std::size_t>(k + runs[0].latency);
                worst = std::max(worst, std::fabs(double(runs[0].heard[i0]) -
                                                  double(runs[1].heard[i0 + 256000])));
            }
            CHECK(worst == 0.0);
            CHECK(loudest(runs[1], 256000 + 131000, 256000 + 160000) > 0.01);
        }
}
