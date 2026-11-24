// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Clips with their own fades captured under a master playrate (S3 step C):
// the commit and the replay against what played. Split from
// test_clip_player_rate_commit.cpp so no ctest entry runs much past a minute
// in dev (the two cases together ran past 600 s under ThreadSanitizer).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

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
