// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kBar = 96000;  // 4/4 at 120 beats a minute

ModuleHarness::Transport playing() {
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    t.beats = 0.0;
    return t;
}

std::vector<std::int64_t> rises(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 0; n < y.size(); ++n)
        if (y[n] >= 0.5 && (n == 0 || y[n - 1] < 0.5)) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

}  // namespace

TEST_CASE("looper: a bar recorded while Rec is high plays back round and round") {
    ModuleHarness h(supple::rack::test::stock("looper"));
    h.set("punch", 0);  // free
    h.transport(playing());
    // A ramp and a gate on each beat in the first bar; something else after.
    h.input("in", 1,
            [](int, std::int64_t n) { return n < kBar ? static_cast<double>(n) / kBar : -0.5; });
    h.input("gate", 1,
            [](int, std::int64_t n) { return n < kBar && (n % 24000) < 6000 ? 1.0 : 0.0; });
    h.input("rec", 1, [](int, std::int64_t n) { return n < kBar ? 1.0 : 0.0; });
    h.run(kBar * 3);
    const auto& y = h.output("out");
    const auto& g = h.output("out_gate");
    for (int k = 0; k < kBar; k += 997) {
        const double want = static_cast<double>(k) / kBar;
        CHECK(y[static_cast<std::size_t>(k)] == doctest::Approx(want));  // recording: In through
        CHECK(y[static_cast<std::size_t>(kBar + k)] == doctest::Approx(want).epsilon(0.002));
        CHECK(y[static_cast<std::size_t>(2 * kBar + k)] == doctest::Approx(want).epsilon(0.002));
    }
    CHECK(g[kBar + 100] == 1.0);
    CHECK(g[kBar + 6100] == 0.0);
    CHECK(g[kBar + 24100] == 1.0);
    CHECK(g[2 * kBar + 72100] == 1.0);
    CHECK(g[2 * kBar + 80000] == 0.0);
}

TEST_CASE("looper: at Punch Loop a trigger at Rec records the whole next loop") {
    ModuleHarness h(supple::rack::test::stock("looper"));
    h.transport(playing());
    // In counts the bars: 0, 1, 2, ...
    h.input("in", 1, [](int, std::int64_t n) { return static_cast<double>(n / kBar); });
    h.input("rec", 1, [](int, std::int64_t n) { return n >= 30000 && n < 30100 ? 1.0 : 0.0; });
    h.run(kBar * 4);
    const auto& y = h.output("out");
    CHECK(y[50000] == 0.0);             // empty: In through
    CHECK(y[kBar + 50000] == 1.0);      // recording the second bar
    CHECK(y[2 * kBar + 10] == 1.0);     // and playing it after
    CHECK(y[3 * kBar + 90000] == 1.0);  // In is 3 by now
    // Loop fires a 1 ms trigger at each bar.
    const auto& l = h.output("loop");
    CHECK(rises(l) == std::vector<std::int64_t>{0, kBar, 2 * kBar, 3 * kBar});
    CHECK(l[47] == 1.0);
    CHECK(l[48] == 0.0);
}

TEST_CASE("looper: Depth and Offset; Clear goes back to In; the song stopped") {
    SUBCASE("depth, offset and clear") {
        ModuleHarness h(supple::rack::test::stock("looper"));
        h.set("depth", -50);
        h.set("offset", 0.25);
        h.transport(playing());
        h.input("in", 1, [](int, std::int64_t n) { return n < kBar ? 1.0 : 0.0; });
        h.input("rec", 1, [](int, std::int64_t n) { return n < 100 ? 1.0 : 0.0; });
        h.input("clear", 1, [](int, std::int64_t n) {
            return n >= 2 * kBar + 1000 && n < 2 * kBar + 1100 ? 1.0 : 0.0;
        });
        h.run(kBar * 3);
        const auto& y = h.output("out");
        CHECK(y[kBar + 5000] == doctest::Approx(1.0 * -0.5 + 0.25));
        CHECK(y[2 * kBar + 500] == doctest::Approx(-0.25));
        CHECK(y[2 * kBar + 5000] == 0.0);  // cleared: In, as it is
    }
    SUBCASE("stopped: free at the tempo, from Reset") {
        ModuleHarness h(supple::rack::test::stock("looper"));
        h.set("length", 0);  // a beat: 24000 frames
        h.input("in", 1, [](int, std::int64_t n) {
            return n < 48000 ? static_cast<double>(n % 24000) / 24000 : 0.0;
        });
        h.input("rec", 1, [](int, std::int64_t n) { return n >= 10 && n < 100 ? 1.0 : 0.0; });
        h.input("reset", 1,
                [](int, std::int64_t n) { return n >= 60000 && n < 60100 ? 1.0 : 0.0; });
        h.run(100000);
        const auto& y = h.output("out");
        // Armed by the trigger, it records the second beat and plays it after.
        CHECK(y[48000 + 6000] == doctest::Approx(0.25).epsilon(0.002));
        CHECK(y[60000 + 6000] == doctest::Approx(0.25).epsilon(0.002));
        CHECK(rises(h.output("loop")) == std::vector<std::int64_t>{0, 24000, 48000, 60000, 84000});
    }
}
