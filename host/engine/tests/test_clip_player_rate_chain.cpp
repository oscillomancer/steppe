// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A capture's commit under a master playrate for a clip played through its
// take's FX chain (ADR-0097, review round 113 T3). Split from
// test_clip_player_rate.cpp so no ctest entry runs much past a minute (CI
// 45118821222 timed the file out at 600 s on Linux).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a capture of a clip with a take chain under a master playrate commits as heard" *
          doctest::description("rt")) {
    // Round 113 T3: tr_1's clip cl_1 (the ramp, 0.25 s) plays through its
    // take's chain (the gain fixture, -6 dB, 64 samples of latency) over an
    // arrangement of audio, its period 0.3 s. Launched on bar 2, back to the arrangement on
    // bar 3, launched again on bar 4, under a constant 0.75 and under the
    // envelope: the commit meets the standard to the capture's last sample,
    // except within the kernel's reach of it (bounded at its measure).
    for (const bool envelope : {false, true})
        for (const std::uint32_t block : {128u, 512u})
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("envelope " << envelope << ", block " << block << ", anticipative "
                                 << options.anticipative);
                Project p;
                latentClip(p);
                // 0.3 s, so no period wrap falls in a switch's fade-out.
                p.run("clip.set", R"({"clip": "cl_1", "length": 0.3})");
                auto item = j(R"({"track": "tr_1", "bar": 1})");
                item["audio_file"] = p.ramp("item.wav", 240000).string();
                p.runArgs("item.add", item);
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
                p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 2
                logged(1);
                p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3
                logged(2);
                p.run("launch.clip", R"({"clip": "cl_1"})");  // bar 4
                logged(3);
                live.until((live.cycles + 200) * block);
                const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
                const std::vector<float> plain = arrangement(p, 12.0, block);
                json::Value done = p.run("launch.commit", "{}");
                for (std::size_t k = 0; k < 3; ++k)
                    CHECK(log[k].songFrame == std::int64_t(96000 * (k + 1)));
                live.until((live.cycles + 20) * block);  // past the capture's end
                const std::vector<float> heard = live.stop();
                const std::vector<float> committed = arrangement(p, 12.0, block);
                const double rate = envelope ? 0.6 : 0.75;
                const std::int64_t end =
                    log.back().runningFrame +
                    std::llround((done["to"].asDouble() * double(kBeat) - 288000.0) / rate);
                REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
                REQUIRE(end <= std::int64_t(committed.size()));
                REQUIRE(end > log.back().runningFrame + 20000);
                const CommitResidual r = commitResidual(heard, live.latency, committed, log, 0, end,
                                                        std::llround(128 / rate) + 64);
                MESSAGE("worst within the crossfades " << r.fading << ", at the capture's end "
                                                       << r.edges);
                CHECK(r.away == 0.0);
                CHECK(r.fading <= 1e-6);
                CHECK(r.edges > 0.0);
                CHECK(r.edges <= 0.004);  // measured 0.0037 (the cut audio's level)
                CHECK(compare(heard, live.latency, plain, 0, end).first >= 0);
            }
}
