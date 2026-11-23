// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A switch on a clip's period wrap: the clip going out reads on in its pass,
// live and committed, at rate 1 and under a master playrate (ADR-0097, note
// 2026-11-22, review round 113). Split from test_clip_player_rate.cpp so no
// ctest entry runs much past a minute (CI 45118821222 timed the file out at
// 600 s on Linux).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a switch on a period wrap: the clip going out reads on in its pass, live and committed" *
          doctest::description("rt")) {
    // Review round 113: tr_1's cl_1 (0.25 s of the ramp, laid one item per
    // period) launched on bar 2 wraps every 12000 song samples, so tr_1's
    // return to its audio arrangement on bar 3 (song 192000) lands on a
    // wrap. Going out, the clip reads on in the pass it was in through the
    // fade-out (240 song samples), as its committed piece does, rather than
    // jumping to the period's start mid-fade (which differed by 0.011). At
    // rate 1, under 0.75 and under the envelope, blocks 128 and 512, both
    // executors: live equals the replay of its log, and the commit meets
    // the standard to the capture's last sample (at rate 1 exactly at the
    // edge; under a rate within the kernel's reach of it, bounded), except
    // under a rate just before the return (below).
    for (const int rated : {0, 1, 2})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("rate " << (rated == 0   ? "1"
                                 : rated == 1 ? "0.75"
                                              : "envelope")
                             << ", block " << block << ", anticipative " << options.anticipative);
                Project p;
                auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
                args["audio_file"] = p.ramp("ramp.wav", 48000).string();
                p.runArgs("clip.add", args);
                p.run("clip.set", R"({"clip": "cl_1", "length": 0.25})");
                auto item = j(R"({"track": "tr_1", "bar": 1})");
                item["audio_file"] = p.ramp("item.wav", 240000).string();
                p.runArgs("item.add", item);
                if (rated) underRate(p, rated == 2);
                AppLive live(p, options, block, 48000 * 12 + 16384);
                const auto logged = [&](std::size_t n) {
                    while (p.app->engine()->launchLog().size() < n &&
                           live.cycles * block < 48000 * 10)
                        live.until((live.cycles + 16) * block);
                    REQUIRE(p.app->engine()->launchLog().size() == n);
                };
                CHECK(p.run("launch.capture", R"({"on": true})")["from"].asDouble() == 0);
                REQUIRE(p.app->transport("play"));
                live.until(20 * block);
                p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 2
                logged(1);
                p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3: on a wrap
                logged(2);
                p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 4
                logged(3);
                // 200 blocks past the last launch as played: the anticipative
                // renderer logs it ahead of the device, by however far it ran.
                live.until(p.app->engine()->launchLog().back().runningFrame +
                           200 * std::int64_t(block));
                const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
                for (std::size_t k = 0; k < 3; ++k)
                    CHECK(log[k].songFrame == std::int64_t(96000 * (k + 1)));
                const std::vector<float> replayed = replay(p, 12.0, log, block);
                json::Value done = p.run("launch.commit", "{}");
                live.until((live.cycles + 20) * block);  // past the capture's end
                const std::vector<float> heard = live.stop();
                const std::vector<float> committed = arrangement(p, 12.0, block);
                const double rate = rated == 0 ? 1.0 : rated == 1 ? 0.75 : 0.6;
                const std::int64_t end =
                    log.back().runningFrame +
                    std::llround((done["to"].asDouble() * double(kBeat) - 288000.0) / rate);
                REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
                REQUIRE(end <= std::int64_t(committed.size()));
                REQUIRE(end > log.back().runningFrame + 20000);
                // Live is the replay of its log, through the return on the wrap.
                const auto [differs, at] = compare(heard, live.latency, replayed, 0, end);
                CHECK_MESSAGE(differs == 0.0,
                              "live differs from the replay by " << differs << " at frame " << at);
                // Under a rate, within the tape kernel's reach before the
                // return (128 song samples), live still read the pass's end
                // gated at the wrap (the switch reaches the node at its frame),
                // where the commit's piece reads on: a stated limit, bounded at
                // its measure. Everywhere else the standard holds.
                const std::int64_t rf = log[1].runningFrame;
                const std::int64_t reach = rated ? std::llround(128 / rate) + 64 : 0;
                double before = 0, fading = 0, away = 0, edges = 0;
                for (std::int64_t k = 0; k < end; ++k) {
                    const double d = std::fabs(double(heard[std::size_t(k + live.latency)]) -
                                               double(committed[std::size_t(k)]));
                    bool near = false;
                    for (const LaunchLogEntry& e : log)
                        near = near || (k >= e.runningFrame - 256 && k < e.runningFrame + 768);
                    double& worst = k >= rf - reach && k < rf ? before
                                    : near                    ? fading
                                    : k >= end - reach        ? edges
                                                              : away;
                    worst = std::max(worst, d);
                }
                MESSAGE("before the return " << before << ", within the crossfades " << fading
                                             << ", at the capture's end " << edges);
                CHECK(away == 0.0);
                CHECK(fading <= 1e-6);
                CHECK(before <= (rated == 0 ? 0.0 : rated == 1 ? 0.0028 : 0.011));
                CHECK(edges <= 0.03);
            }
}
