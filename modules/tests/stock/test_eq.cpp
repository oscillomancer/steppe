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

// The EQ's gain in dB at hz, from a sine through it.
double gainDb(Settings settings, double hz) {
    ModuleHarness h(stock("eq"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [hz](int, std::int64_t n) {
        return 0.5 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
    });
    h.run(24000 + 9600);
    return 20.0 * std::log10(amplitudeAt(slice(h.output("out"), 24000), hz, 48000) / 0.5);
}
}  // namespace

TEST_CASE("eq: flat at its defaults, the input exactly") {
    ModuleHarness h(stock("eq"));
    const auto x = [](std::int64_t n) {
        return 0.4 * std::sin(0.01 * n) + 0.3 * std::sin(1.7 * n) + 0.2 * std::sin(2.9 * n);
    };
    h.input("in", 1, [x](int, std::int64_t n) { return x(n); });
    h.run(9600);
    for (int n = 0; n < 9600; n += 37)
        CHECK(h.output("out")[n] == doctest::Approx(x(n)).epsilon(1e-9));
}

TEST_CASE("eq: the bells boost and cut at their centres, Q sets their width") {
    CHECK(gainDb({{"mid1_gain", 12}}, 400) == doctest::Approx(12).epsilon(0.01));
    CHECK(std::abs(gainDb({{"mid1_gain", 12}}, 8000)) < 0.3);
    CHECK(gainDb({{"mid2_gain", -9}, {"mid2_freq", 3000}}, 3000) ==
          doctest::Approx(-9).epsilon(0.01));
    const double wide = gainDb({{"mid1_gain", 12}, {"mid1_q", 0.5}}, 800);
    const double narrow = gainDb({{"mid1_gain", 12}, {"mid1_q", 4}}, 800);
    CHECK(wide > 6);
    CHECK(narrow < 3);
}

TEST_CASE("eq: shelves lift or lower an end; cuts clear it") {
    CHECK(gainDb({{"low_gain", 6}}, 30) == doctest::Approx(6).epsilon(0.05));
    CHECK(std::abs(gainDb({{"low_gain", 6}}, 3000)) < 0.2);
    CHECK(gainDb({{"high_gain", -6}}, 18000) == doctest::Approx(-6).epsilon(0.05));
    CHECK(std::abs(gainDb({{"high_gain", -6}}, 300)) < 0.2);
    // 12 dB an octave: two octaves under a 200 Hz cut, about 24 dB down.
    CHECK(gainDb({{"low_cut", 200}}, 50) < -22);
    CHECK(std::abs(gainDb({{"low_cut", 200}}, 3000)) < 0.1);
    CHECK(gainDb({{"high_cut", 2000}}, 8000) < -22);
    CHECK(std::abs(gainDb({{"high_cut", 2000}}, 200)) < 0.1);
    CHECK(gainDb({{"level", -6}}, 1000) == doctest::Approx(-6).epsilon(0.01));
}

TEST_CASE("eq: poly channels alike, each its own") {
    ModuleHarness h(stock("eq"));
    h.set("mid1_gain", -18);
    h.set("mid1_freq", 1000);
    h.set("mid1_q", 2);
    h.input("in", 2, [](int c, std::int64_t n) {
        return 0.5 *
               std::sin(2 * kPi * (c == 0 ? 1000.0 : 100.0) * static_cast<double>(n) / 48000.0);
    });
    h.run(24000);
    CHECK(h.channels("out") == 2);
    CHECK(peak(slice(h.output("out", 0), 12000)) < 0.5 * std::pow(10.0, -17.0 / 20));
    CHECK(peak(slice(h.output("out", 1), 12000)) == doctest::Approx(0.5).epsilon(0.02));
}
