// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

double sine(std::int64_t n, double hz, double a = 0.5) {
    return a * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}
// The gain at hz with the sweep standing still at Centre.
double still(Settings settings, double hz, double a = 0.5) {
    ModuleHarness h(stock("phaser"));
    h.set("depth", 0);
    h.set("feedback", 0);
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in_l", 1, [hz, a](int, std::int64_t n) { return sine(n, hz, a); });
    h.run(24000 + 9600);
    return amplitudeAt(slice(h.output("out_l"), 24000), hz, 48000) / a;
}
// Where n stages notch: each turns a quarter or three quarters of a half
// cycle (tan of pi/2n and 3pi/2n) times Centre, on the bilinear scale.
double notchHz(double centre, int n, int which) {
    const double t = std::tan(kPi * centre / 48000.0) * std::tan((2 * which + 1) * kPi / (2.0 * n));
    return std::atan(t) * 48000.0 / kPi;
}
}  // namespace

TEST_CASE("phaser: standing still, notches where the chain turns half a cycle") {
    for (int s = 0; s < 3; ++s) {
        const int n = 4 + 2 * s;
        CAPTURE(n);
        const double first = notchHz(700, n, 0);
        CHECK(still({{"stages", s}}, first) < 0.01);
        CHECK(still({{"stages", s}}, notchHz(700, n, 1)) < 0.01);
        // Between, at Centre for four stages a whole cycle: in step, full.
        if (n == 4) CHECK(still({{"stages", s}}, 700) == doctest::Approx(1.0).epsilon(1e-3));
    }
    // Mix 0 is the dry input.
    ModuleHarness h(stock("phaser"));
    h.set("mix", 0);
    h.input("in_l", 1, [](int, std::int64_t n) { return sine(n, 1234); });
    h.run(4800);
    for (int n = 0; n < 4800; n += 37)
        CHECK(h.output("out_l")[n] == doctest::Approx(sine(n, 1234)));
}

TEST_CASE("phaser: the LFO sweeps the notches; Spread sets the sides apart") {
    ModuleHarness h(stock("phaser"));
    h.set("rate", 1);
    h.set("depth", 100);
    h.set("feedback", 0);
    h.input("in_l", 1, [](int, std::int64_t n) { return sine(n, 700); });
    h.run(96000);
    double lo = 1e9, hi = 0.0;
    for (std::size_t n = 48000; n + 480 <= 96000; n += 480) {
        const double a = peak(slice(h.output("out_l"), n, n + 480)) / 0.5;
        lo = std::min(lo, a);
        hi = std::max(hi, a);
    }
    CHECK(lo < 0.2);
    CHECK(hi > 0.95);
    // Spread 0: both sides alike; 90: not.
    const auto& l = h.output("out_l");
    const auto& r = h.output("out_r");
    double apart = 0.0;
    for (std::size_t n = 48000; n < 96000; ++n) apart = std::max(apart, std::abs(l[n] - r[n]));
    CHECK(apart > 0.2);
    ModuleHarness m(stock("phaser"));
    m.set("spread", 0);
    m.input("in_l", 1, [](int, std::int64_t n) { return sine(n, 700); });
    m.run(24000);
    for (std::size_t n = 0; n < 24000; n += 41) CHECK(m.output("out_l")[n] == m.output("out_r")[n]);
}

TEST_CASE("phaser: Feedback builds peaks between the notches, and stays bounded") {
    // At Centre four stages turn a whole cycle: the loop adds in step.
    CHECK(still({{"stages", 0}, {"feedback", 50}}, 700, 0.05) ==
          doctest::Approx(0.5 + 0.5 / (1 - 0.5)).epsilon(0.02));
    CHECK(still({{"stages", 0}, {"feedback", -50}}, 700, 0.05) ==
          doctest::Approx(0.5 + 0.5 / (1 + 0.5)).epsilon(0.02));
    ModuleHarness h(stock("phaser"));
    h.set("feedback", 95);
    h.set("stages", 3);
    h.input("in_l", 1, [](int, std::int64_t n) {
        std::uint32_t x = static_cast<std::uint32_t>(n) * 2654435761u;
        return static_cast<double>(x >> 8) / 8388608.0 - 1.0;
    });
    h.run(96000);
    CHECK(peak(h.output("out_l")) < 8.0);
}

TEST_CASE("phaser: poly channels each their own, R follows L while unpatched") {
    ModuleHarness h(stock("phaser"));
    h.input("in_l", 2, [](int c, std::int64_t n) { return c == 0 ? sine(n, 300) : 0.0; });
    h.run(9600);
    CHECK(h.channels("out_l") == 2);
    CHECK(peak(h.output("out_l", 1)) == 0.0);
    CHECK(peak(h.output("out_r", 0)) > 0.1);
}
