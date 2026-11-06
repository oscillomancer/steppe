// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

double tone(std::int64_t n, double hz = 500.0) {
    return 0.5 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}

struct Stereo {
    std::vector<double> l, r;
};
Stereo grains(Settings settings, int frames) {
    ModuleHarness h(stock("granular"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [](int, std::int64_t n) { return tone(n); });
    h.run(frames);
    return {h.output("out_l"), h.output("out_r")};
}
}  // namespace

TEST_CASE("granular: half-overlapping grains give back the input, delayed") {
    // 100 ms grains, 20 a second, 250 ms back, no spread.
    const auto y = grains({{"position", 250}, {"spread", 0}}, 48000);
    double worst = 0;
    for (int n = 24000; n < 48000; ++n) worst = std::max(worst, std::abs(y.l[n] - tone(n - 12000)));
    CHECK(worst < 1e-3);
    CHECK(y.l == y.r);
}

TEST_CASE("granular: Pitch transposes the grains") {
    const auto up = grains({{"pitch", 12}, {"spread", 0}}, 48000);
    const auto tail = slice(up.l, 24000);
    CHECK(amplitudeAt(tail, 1000, 48000) > 4 * amplitudeAt(tail, 500, 48000));
    const auto down = grains({{"pitch", -12}, {"spread", 0}, {"position", 200}}, 48000);
    const auto low = slice(down.l, 24000);
    CHECK(amplitudeAt(low, 250, 48000) > 4 * amplitudeAt(low, 500, 48000));
}

TEST_CASE("granular: Scatter and Spray smear, Spread widens") {
    const auto plain = grains({{"spread", 0}}, 48000);
    const auto scattered = grains({{"spread", 0}, {"scatter", 7}}, 48000);
    CHECK(amplitudeAt(slice(scattered.l, 24000), 500, 48000) <
          0.7 * amplitudeAt(slice(plain.l, 24000), 500, 48000));
    const auto wide = grains({{"spread", 100}, {"spray", 300}}, 48000);
    double diff = 0;
    for (int n = 24000; n < 48000; ++n) diff = std::max(diff, std::abs(wide.l[n] - wide.r[n]));
    CHECK(diff > 0.1);
}

TEST_CASE("granular: Reverse plays grains backwards") {
    // One 100 ms grain every 200 ms on a rising ramp: a quarter and three
    // quarters into a grain its window is the same, so the difference is the
    // ramp's direction as read.
    const auto direction = [](double reverse) {
        ModuleHarness h(stock("granular"));
        h.set("reverse", reverse);
        h.set("spread", 0);
        h.set("density", 5);
        h.set("position", 300);
        h.input("in", 1, [](int, std::int64_t n) { return static_cast<double>(n) / 48000.0; });
        h.run(48000);
        const auto& y = h.output("out_l");
        const int start = 2 * 9600;
        return y[start + 3600] - y[start + 1200];
    };
    CHECK(direction(0) > 0.01);
    CHECK(direction(100) < -0.01);
}

TEST_CASE("granular: a frozen buffer keeps playing what it held") {
    ModuleHarness h(stock("granular"));
    h.set("spread", 0);
    h.set("position", 200);
    h.input("in", 1, [](int, std::int64_t n) { return n < 24000 ? tone(n) : 0.0; });
    h.input("freeze_gate", 1, [](int, std::int64_t n) { return n >= 20000 ? 1.0 : 0.0; });
    h.run(96000);
    const auto late = slice(h.output("out_l"), 72000);
    CHECK(amplitudeAt(late, 500, 48000) > 0.3);

    ModuleHarness live(stock("granular"));
    live.input("in", 1, [](int, std::int64_t n) { return n < 24000 ? tone(n) : 0.0; });
    live.run(96000);
    CHECK(peak(slice(live.output("out_l"), 72000)) < 1e-9);
}

TEST_CASE("granular: Mix, and the seed makes it repeatable") {
    const auto dry = grains({{"mix", 0}}, 4800);
    for (int n = 0; n < 4800; n += 97) CHECK(dry.l[n] == doctest::Approx(tone(n)));
    const auto a = grains({{"spray", 500}, {"scatter", 5}, {"reverse", 50}}, 24000);
    const auto b = grains({{"spray", 500}, {"scatter", 5}, {"reverse", 50}}, 24000);
    CHECK(a.l == b.l);
}
