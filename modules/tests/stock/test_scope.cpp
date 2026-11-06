// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

// The frames where Sweep rises: where each sweep started.
std::vector<std::int64_t> starts(const ModuleHarness& h) {
    std::vector<std::int64_t> at;
    const auto& y = h.output("sweep");
    for (std::size_t n = 0; n < y.size(); ++n)
        if (y[n] >= 0.5 && (n == 0 || y[n - 1] < 0.5)) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

double sine(std::int64_t n, double period) {
    return std::sin(2.0 * std::numbers::pi * static_cast<double>(n) / period);
}

}  // namespace

TEST_CASE("scope: a rising crossing of Level starts each sweep, so a wave stands still") {
    ModuleHarness h(supple::rack::test::stock("scope"));
    h.set("time_base", 2);  // 96 frames, shorter than the wave's 480
    h.input("a", 1, [](int, std::int64_t n) { return sine(n, 480); });
    h.run(2400);
    const auto s = starts(h);
    REQUIRE(s.size() == 4);
    // At each upward crossing (a frame late where the sine lands a hair below zero).
    for (std::size_t k = 0; k < s.size(); ++k)
        CHECK(std::llabs(s[k] - 480 * static_cast<std::int64_t>(k + 1)) <= 1);
    // A pulse of 1 ms.
    CHECK(h.output("sweep")[s[0] + 47] == 1.0);
    CHECK(h.output("sweep")[s[0] + 48] == 0.0);
}

TEST_CASE("scope: Level and Slope move the trigger point; B as the source") {
    SUBCASE("a level of 0.5, rising: a twelfth of the way in") {
        ModuleHarness h(supple::rack::test::stock("scope"));
        h.set("time_base", 2);
        h.set("level", 0.5);
        h.input("a", 1, [](int, std::int64_t n) { return sine(n, 480); });
        h.run(1000);
        const auto s = starts(h);
        REQUIRE(!s.empty());
        CHECK(std::llabs(s[0] - 40) <= 1);
    }
    SUBCASE("falling, on B") {
        ModuleHarness h(supple::rack::test::stock("scope"));
        h.set("time_base", 2);
        h.set("source", 2);  // B
        h.set("slope", 1);
        h.input("a", 1, [](int, std::int64_t) { return 0.0; });
        h.input("b", 1, [](int, std::int64_t n) { return sine(n, 480); });
        h.run(1000);
        const auto s = starts(h);
        REQUIRE(s.size() == 2);
        CHECK(std::llabs(s[0] - 240) <= 1);
        CHECK(std::llabs(s[1] - 720) <= 1);
    }
}

TEST_CASE("scope: with no crossing it sweeps anyway; Free never waits") {
    SUBCASE("auto after a quarter second") {
        ModuleHarness h(supple::rack::test::stock("scope"));
        h.set("time_base", 100);  // 4800 frames
        h.run(40000);
        const auto s = starts(h);
        REQUIRE(s.size() >= 2);
        CHECK(s[0] == 12000);
        CHECK(s[1] == 12000 + 4800 + 12000);
    }
    SUBCASE("free: sweep after sweep") {
        ModuleHarness h(supple::rack::test::stock("scope"));
        h.set("time_base", 10);  // 480 frames
        h.set("source", 0);
        h.run(2000);
        const auto s = starts(h);
        REQUIRE(s.size() == 5);
        for (std::size_t k = 0; k < s.size(); ++k)
            CHECK(s[k] == 480 * static_cast<std::int64_t>(k));
    }
    SUBCASE("free at 1 ms: the trigger drops a frame between sweeps") {
        ModuleHarness h(supple::rack::test::stock("scope"));
        h.set("time_base", 1);  // 48 frames, as long as the pulse
        h.set("source", 0);
        h.run(480);
        const auto s = starts(h);
        REQUIRE(s.size() == 10);
        CHECK(s[1] == 49);  // the first pulse's low frame delays the second
    }
}

TEST_CASE("scope: Trigger In starts a sweep only between sweeps") {
    ModuleHarness h(supple::rack::test::stock("scope"));
    h.set("time_base", 5);  // 240 frames
    h.set("source", 3);
    h.input("trigger", 1, [](int, std::int64_t n) { return (n % 100) >= 50 && (n % 100) < 60; });
    h.run(1000);
    const auto s = starts(h);
    REQUIRE(s.size() == 4);
    CHECK(s[0] == 50);
    CHECK(s[1] == 350);  // the edges at 150 and 250 fall inside the first sweep
    CHECK(s[2] == 650);
    CHECK(s[3] == 950);
}

TEST_CASE("scope: synced, a sweep a span of the song, on the beat") {
    ModuleHarness h(supple::rack::test::stock("scope"));
    h.set("sync", 1);
    h.set("span", 2);  // 1/4: a beat, 24000 frames at 120
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    t.beats = 3.5;
    h.transport(t);
    h.run(60000);
    const auto s = starts(h);
    REQUIRE(s.size() == 3);
    CHECK(s[0] == 0);  // the first frame begins a sweep partway through the beat
    CHECK(s[1] == 12000);
    CHECK(s[2] == 36000);
}
