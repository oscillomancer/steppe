// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A capture armed while launched clips play, under a master playrate and at
// rate 1 (ADR-0097, review round 113): from its first sample, and with a
// held note struck again at its start. Split from test_clip_player_rate.cpp so no ctest
// entry runs much past a minute (CI 45118821222 timed the file out at 600 s
// on Linux).
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a capture armed while clips play under a master playrate commits from its first sample" *
          doctest::description("rt")) {
    // Round 113 T1: armed at once while twoTracks' scene plays (launched on
    // bar 2, song 96000), under a constant 0.75, the capture starts in the
    // middle of cl_2's second pass (song 132000 to 168000) and in tr_1's
    // clip's rest (beats 2 to 4 of its bar); tr_1 returns to the arrangement
    // on bar 3 (song 192000). From the capture's first device frame to its
    // last, the commit meets the standard, except within the kernel's reach
    // of each edge, where the committed items begin or end while the
    // performance went on (a stated limit, bounded at its measure).
    for (const std::uint32_t block : {128u, 512u})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("block " << block << ", anticipative " << options.anticipative);
            Project p;
            twoTracks(p);
            underRate(p, false);
            AppLive live(p, options, block, 48000 * 9 + 16384);
            const auto logged = [&](std::size_t n) {
                while (p.app->engine()->launchLog().size() < n && live.cycles * block < 48000 * 8)
                    live.until((live.cycles + 16) * block);
                REQUIRE(p.app->engine()->launchLog().size() == n);
            };
            REQUIRE(p.app->transport("play"));
            live.until(20 * block);
            p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2: device 128000
            logged(2);
            live.until(200000);  // song 150000
            const double from = p.run("launch.capture", R"({"on": true})")["from"].asDouble();
            CHECK(from * double(kBeat) > 144000);
            CHECK(from * double(kBeat) < 168000);
            p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3
            logged(3);
            // 200 blocks past the last launch as played: the anticipative
            // renderer logs it ahead of the device, by however far it ran.
            live.until(p.app->engine()->launchLog().back().runningFrame +
                       200 * std::int64_t(block));
            const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
            json::Value done = p.run("launch.commit", "{}");
            CHECK(done["from"].asDouble() == from);
            live.until((live.cycles + 20) * block);  // past the capture's end
            const std::vector<float> heard = live.stop();
            const std::vector<float> committed = arrangement(p, 9.0, block);
            // Song s plays at device frame s / 0.75 (the rate is constant).
            const std::int64_t start = std::llround(from * double(kBeat) / 0.75);
            const std::int64_t end = std::llround(done["to"].asDouble() * double(kBeat) / 0.75);
            REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
            REQUIRE(end <= std::int64_t(committed.size()));
            REQUIRE(end > 192000 / 0.75 + 4000);
            // The capture's one switch is tr_1's at bar 3.
            const std::vector<LaunchLogEntry> switches(log.begin() + 2, log.end());
            const CommitResidual r = commitResidual(heard, live.latency, committed, switches, start,
                                                    end, std::llround(128 / 0.75) + 64);
            MESSAGE("worst within the crossfade " << r.fading << ", at the capture's edges "
                                                  << r.edges);
            CHECK(r.away == 0.0);
            CHECK(r.fading <= 1e-6);
            CHECK(r.edges > 0.0);     // the cut is there, and compared
            CHECK(r.edges <= 0.007);  // measured 0.0068 (the cut audio's level)
            // What it committed differs from the arrangement it replaced.
            Project original;
            twoTracks(original);
            underRate(original, false);
            CHECK(
                compare(heard, live.latency, arrangement(original, 9.0, block), start, end).first >=
                0);
        }
}

TEST_CASE("a capture armed while a launched clip's note sounds commits it struck again" *
          doctest::description("rt")) {
    // Round 113, a stated limit at any rate: armed at song 112500, while
    // tr_1's clip holds E3 (beats 4.5 to 5, song 108000 to 120000), the
    // commit's MIDI piece starts at the capture's start, inside the note,
    // and the arrangement strikes a note held into an item's start again
    // there (plan.cpp's chase). So the note sounds through its rest, at the
    // level heard, but from a new attack; after it the commit meets the
    // standard, and armed in a rest it is exact (the case above). It is the
    // same at rate 1 and under a rate.
    for (const double rate : {1.0, 0.75})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("rate " << rate << ", anticipative " << options.anticipative);
            constexpr std::uint32_t block = 128;
            Project p;
            twoTracks(p);
            if (rate != 1.0) underRate(p, false);
            AppLive live(p, options, block, 48000 * 9 + 16384);
            const auto logged = [&](std::size_t n) {
                while (p.app->engine()->launchLog().size() < n && live.cycles * block < 48000 * 8)
                    live.until((live.cycles + 16) * block);
                REQUIRE(p.app->engine()->launchLog().size() == n);
            };
            REQUIRE(p.app->transport("play"));
            live.until(20 * block);
            p.run("launch.scene", R"({"scene": "sc_1"})");  // bar 2
            logged(2);
            live.until(std::llround(112500 / rate));
            const double from = p.run("launch.capture", R"({"on": true})")["from"].asDouble();
            CHECK(from * double(kBeat) > 108000);
            CHECK(from * double(kBeat) < 120000);
            p.run("launch.arrangement", R"({"track": "tr_1"})");  // bar 3
            logged(3);
            // 200 blocks past the last launch as played: the anticipative
            // renderer logs it ahead of the device, by however far it ran.
            live.until(p.app->engine()->launchLog().back().runningFrame +
                       200 * std::int64_t(block));
            const std::vector<LaunchLogEntry> log = p.app->engine()->launchLog();
            json::Value done = p.run("launch.commit", "{}");
            live.until((live.cycles + 20) * block);
            const std::vector<float> heard = live.stop();
            const std::vector<float> committed = arrangement(p, 9.0, block);
            const std::int64_t start = std::llround(from * double(kBeat) / rate);
            const std::int64_t noteEnd = std::llround(120000 / rate);
            const std::int64_t end = std::llround(done["to"].asDouble() * double(kBeat) / rate);
            REQUIRE(end + std::int64_t(live.latency) <= std::int64_t(heard.size()));
            REQUIRE(end <= std::int64_t(committed.size()));
            const std::int64_t reach = rate == 1.0 ? 0 : std::llround(128 / rate) + 64;
            // The note is struck again at the capture's start: the commit
            // differs from there to the note's end...
            const auto [gap, firstGap] =
                compare(heard, live.latency, committed, start, noteEnd + reach);
            MESSAGE("the held note's rest: worst " << gap << " over " << (noteEnd - start)
                                                   << " frames");
            CHECK(firstGap == start);
            CHECK(gap > 0.05);
            // ...but it sounds there at the level heard, with no gap: without
            // the note the commit would hold only tr_2 there.
            const auto rms = [](const std::vector<float>& v, std::int64_t off, std::int64_t a,
                                std::int64_t b) {
                double e = 0;
                for (std::int64_t k = a; k < b; ++k) {
                    const double x = double(v[std::size_t(k + off)]);
                    e += x * x;
                }
                return std::sqrt(e / double(b - a));
            };
            const double heardRest = rms(heard, std::int64_t(live.latency), start, noteEnd);
            const double committedRest = rms(committed, 0, start, noteEnd);
            MESSAGE("the held note's rest: rms heard " << heardRest << ", committed "
                                                       << committedRest);
            CHECK(committedRest == doctest::Approx(heardRest).epsilon(0.02));
            // The same pitch, which a level alone does not show (round 128):
            // as many zero crossings after the new attack's first 2000 frames.
            const auto crossings = [](const std::vector<float>& v, std::int64_t off, std::int64_t a,
                                      std::int64_t b) {
                int n = 0;
                for (std::int64_t k = a + 1; k < b; ++k)
                    if ((v[std::size_t(k - 1 + off)] < 0) != (v[std::size_t(k + off)] < 0)) ++n;
                return n;
            };
            const int heardCrossings =
                crossings(heard, std::int64_t(live.latency), start + 2000, noteEnd);
            const int committedCrossings = crossings(committed, 0, start + 2000, noteEnd);
            MESSAGE("the held note's rest: zero crossings heard "
                    << heardCrossings << ", committed " << committedCrossings);
            CHECK(std::abs(committedCrossings - heardCrossings) <= 2);
            // ...and after it the commit meets the standard.
            const std::vector<LaunchLogEntry> switches(log.begin() + 2, log.end());
            const CommitResidual r = commitResidual(heard, live.latency, committed, switches,
                                                    noteEnd + reach, end, reach);
            CHECK(r.away == 0.0);
            CHECK(r.fading <= 1e-6);
            CHECK(r.edges <= 0.03);
        }
}
