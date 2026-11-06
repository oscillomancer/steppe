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

struct Stereo {
    std::vector<double> l, r;
};
Stereo sine(Settings settings, double hz, double amp, int frames) {
    ModuleHarness h(stock("chorus"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in_l", 1, [hz, amp](int, std::int64_t n) {
        return amp * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
    });
    h.run(frames);
    return {h.output("out_l"), h.output("out_r")};
}
int peakAt(const std::vector<double>& y) {
    return static_cast<int>(std::max_element(y.begin(), y.end()) - y.begin());
}
}  // namespace

TEST_CASE("chorus: Mix 0 is the dry input") {
    const auto h = sine({{"mix", 0}}, 440, 0.5, 4800);
    double worst = 0;
    for (int n = 0; n < 4800; ++n)
        worst = std::max(worst, std::abs(h.l[n] - 0.5 * std::sin(2 * kPi * 440 * n / 48000.0)));
    CHECK(worst < 1e-12);
    CHECK(h.r == h.l);
}

TEST_CASE("chorus: the delays, chorus and flanger") {
    for (const auto& [mode, frames] : {std::pair{0, 288}, std::pair{1, 12}}) {
        CAPTURE(mode);
        ModuleHarness h(stock("chorus"));
        h.set("mode", mode);
        h.set("depth", 0);
        h.set("voices", 1);
        h.set("mix", 100);
        h.input("in_l", 1, [](int, std::int64_t n) { return n == 0 ? 1.0 : 0.0; });
        h.run(2400);
        CHECK(peakAt(h.output("out_l")) == frames);  // 6 ms and 0.25 ms
    }
}

TEST_CASE("chorus: the flanger's comb, and feedback rings it") {
    // 0.25 ms: notches at 2, 6, 10 kHz, peaks at 4, 8 kHz.
    const Settings flat = {{"mode", 1}, {"depth", 0}, {"voices", 1}, {"mix", 50}};
    // Small, so the loop's limiter stays linear.
    const auto gain = [](Settings s, double hz, double fb) {
        ModuleHarness h(stock("chorus"));
        for (const auto& [key, value] : s) h.set(key, value);
        h.set("feedback", fb);
        h.input("in_l", 1, [hz](int, std::int64_t n) {
            return 0.01 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
        });
        h.run(9600);
        return amplitudeAt(slice(h.output("out_l"), 4800), hz, 48000) / 0.01;
    };
    CHECK(gain(flat, 2000, 0) < 1e-3);
    CHECK(gain(flat, 4000, 0) == doctest::Approx(1).epsilon(1e-3));
    CHECK(gain(flat, 4000, 80) == doctest::Approx(3).epsilon(0.01));
    CHECK(gain(flat, 4000, -80) == doctest::Approx(0.5 + 0.5 / 1.8).epsilon(0.01));
    CHECK(gain(flat, 2000, -80) == doctest::Approx(2).epsilon(0.01));
}

TEST_CASE("chorus: the sweep bends the pitch") {
    const auto h =
        sine({{"voices", 1}, {"mix", 100}, {"depth", 50}, {"rate", 0.5}}, 1000, 0.5, 96000);
    double lo = 1e9, hi = 0;
    for (int k = 0; k < 19; ++k) {
        const double f = frequency(slice(h.l, 4800 * k, 4800 * (k + 1)), 48000);
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    CHECK(hi / lo > 1.01);
    CHECK(hi / lo < 1.05);
}

TEST_CASE("chorus: Spread widens a mono input; In R takes a stereo one") {
    const auto narrow = sine({{"spread", 0}}, 440, 0.5, 9600);
    CHECK(narrow.l == narrow.r);
    const auto wide = sine({{"spread", 100}}, 440, 0.5, 9600);
    double diff = 0;
    for (int n = 0; n < 9600; ++n) diff = std::max(diff, std::abs(wide.l[n] - wide.r[n]));
    CHECK(diff > 0.05);

    ModuleHarness h(stock("chorus"));
    h.set("mix", 0);
    h.input("in_l", 1, [](int, std::int64_t) { return 0.25; });
    h.input("in_r", 1, [](int, std::int64_t) { return -0.5; });
    h.run(480);
    CHECK(h.output("out_l")[100] == doctest::Approx(0.25));
    CHECK(h.output("out_r")[100] == doctest::Approx(-0.5));
}

TEST_CASE("chorus: more voices thicken, and the level holds") {
    for (int v : {1, 3, 6}) {
        CAPTURE(v);
        const auto h = sine({{"voices", v}, {"mix", 100}}, 440, 0.5, 48000);
        const double level = rms(slice(h.l, 4800));
        CHECK(level > 0.25);
        CHECK(level < 0.7);
    }
}
