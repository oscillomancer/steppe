// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A capture's commit under a master playrate (ADR-0097, S3 B and review
// round 113): the positions and the equality to what played. Split from
// test_clip_player_rate.cpp so no ctest entry runs much past a minute (CI
// 45118821222 timed the file out at 600 s on Linux).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a capture under a master playrate commits by the song positions heard" *
          doctest::description("rt")) {
    // S3 B: the capture's running time is the song's, in samples. twoTracks'
    // scene on bar 2, tr_1 back to the arrangement and tr_2 stopped on bar 3,
    // cl_2 again on bar 4, under a constant 0.75 and under the envelope
    // (tape): the performance holds the launches at song samples 96000,
    // 192000 and 288000, and the committed arrangement, rendered offline at
    // the same rate, is what played live to the rate-1 commit's standard:
    // exact away from the switches' crossfades, through cl_2's period wraps
    // (0.75 s of a 1 s ramp, one item per period, at song 132000 and
    // 168000), and within 1e-6 through them, the float32 rounding of the
    // switch gain (the live gain is a float, an item's fade a double). To
    // the capture's last sample: within the kernel's reach of it, where the
    // committed items end while the performance went on, the commit differs
    // by up to the level cut there (a stated limit, bounded at its measure).
    for (const bool envelope : {false, true})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Project p;
                twoTracks(p);
                underRate(p, envelope);
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
                p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2
                logged(2);
                p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3
                p.run("launch.stop", R"({"track": "tr_2"})");
                logged(4);
                p.run("launch.clip", R"({"clip": "cl_2"})");  // bar 4
                logged(5);
                // 200 blocks past the last launch as played: the anticipative
                // renderer logs it ahead of the device, by however far it ran.
                live.until(p.app->engine()->launchLog().back().runningFrame +
                           200 * std::int64_t(block));
                const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
                const std::vector<float> plain = arrangement(p, 12.0, block);
                json::Value done = p.run("launch.commit", "{}");
                CHECK(done["from"].asDouble() == 0);
                json::Value history = p.run("log.list", R"({"limit": 1})");
                const json::Value& last = history["entries"].at(history["entries"].size() - 1);
                REQUIRE(last.find("command")->asString() == "launch.commit");
                const json::Value& launches =
                    *last.find("args")->find("performance")->find("launches");
                REQUIRE(launches.size() == 5);
                for (std::size_t k = 0; k < 5; ++k) {
                    const std::int64_t want = k < 2 ? 96000 : k < 4 ? 192000 : 288000;
                    CHECK(launches.at(k).find("frame")->asInt() == want);
                    CHECK(launches.at(k).find("anchor")->asInt() == want);
                }
                live.until((live.cycles + 20) * block);  // past the capture's end
                const std::vector<float> heard = live.stop();
                const std::vector<float> committed = arrangement(p, 12.0, block);
                // The capture's last device frame: after the last launch (song
                // 288000) the rate is constant, 0.75, or 0.6 past the envelope.
                const double rate = envelope ? 0.6 : 0.75;
                const std::int64_t end =
                    log.back().runningFrame +
                    std::llround((done["to"].asDouble() * double(kBeat) - 288000.0) / rate);
                REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
                REQUIRE(end <= std::int64_t(committed.size()));
                // Past cl_2's second wrap (song 168000: device 224000 at
                // 0.75, about 206800 under the envelope).
                REQUIRE(end > 230000);
                const CommitResidual r = commitResidual(heard, live.latency, committed, log, 0, end,
                                                        std::llround(128 / rate) + 64);
                MESSAGE("worst within the crossfades " << r.fading << ", at the capture's end "
                                                       << r.edges);
                CHECK(r.away == 0.0);
                CHECK(r.fading <= 1e-6);  // the float32 rounding of the switch gain
                CHECK(r.edges > 0.0);     // the cut is there, and compared
                CHECK(r.edges <= 0.03);   // measured 0.028 (the cut audio's level)
                CHECK(compare(heard, live.latency, plain, 0, end).first >= 0);
            }
}

TEST_CASE("a capture of clips with fades under a playrate commits as heard" *
          doctest::description("rt")) {
    // S3 step C: fadingPerformance under a constant 0.75 and under the
    // envelope (tape), which is constant (0.6) from song 192000: cl_1's fades
    // apply on the converter's output at each frame's song position, as an
    // item's do, so the commit is exact away from the crossfades, its
    // stand-in included, and within 1e-6 through them.
    for (const bool envelope : {false, true})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Project p;
                fadingClips(p);
                underRate(p, envelope);
                const double rate = envelope ? 0.6 : 0.75;  // from song 192000
                AppLive live(p, options, block, 48000 * 14 + 16384);
                p.run("launch.capture", R"({"on": true})");
                REQUIRE(p.app->transport("play"));
                live.until(20 * block);
                const auto logged = [&](int n) {
                    if (n < 0) {  // until song sample -n, after the third launch
                        const LaunchLogEntry& third = p.app->engine()->launchLog().at(2);
                        live.until(third.runningFrame +
                                   std::llround(double(-n - third.anchorSample) / rate));
                        return;
                    }
                    while (p.app->engine()->launchLog().size() < std::size_t(n) &&
                           live.cycles * block < 48000 * 13)
                        live.until((live.cycles + 16) * block);
                    REQUIRE(p.app->engine()->launchLog().size() == std::size_t(n));
                };
                fadingPerformance(p, logged);
                const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
                REQUIRE(log.size() == 4);
                CHECK(log[1].anchorSample == 192000);
                CHECK(log[3].anchorSample == 360000);
                live.until(log.back().runningFrame + 200 * std::int64_t(block));
                const std::vector<float> replayed = replay(p, 14.0, log, block);
                json::Value done = p.run("launch.commit", "{}");
                live.until((live.cycles + 20) * block);  // past the capture's end
                const std::vector<float> heard = live.stop();
                const std::vector<float> committed = arrangement(p, 14.0, block);
                const std::int64_t end =
                    log.back().runningFrame +
                    std::llround((done["to"].asDouble() * double(kBeat) - 360000.0) / rate);
                REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
                REQUIRE(end <= std::int64_t(committed.size()));
                // Live is the replay of its log, bit for bit.
                CHECK(compare(heard, live.latency, replayed, 0, end).first == 0.0);
                const CommitResidual r = commitResidual(heard, live.latency, committed, log, 0, end,
                                                        std::llround(128 / rate) + 64);
                MESSAGE("worst within the crossfades " << r.fading << ", at the capture's end "
                                                       << r.edges);
                CHECK(r.away == 0.0);
                CHECK(r.fading <= 1e-6);
                CHECK(r.edges <= 0.03);
            }
}
