// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

// A 110 Hz saw (band-limited, 0.5 peak-ish) through the filter.
double saw(std::int64_t n) {
    double s = 0;
    for (int k = 1; k * 110 < 20000; ++k)
        s += std::sin(2 * kPi * k * 110.0 * static_cast<double>(n) / 48000.0) / k;
    return 0.3 * s;
}
std::vector<double> voiced(Settings settings) {
    ModuleHarness h(stock("vowel"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [](int, std::int64_t n) { return saw(n); });
    h.run(4800 + 48000);
    return slice(h.output("out"), 4800);
}
// The gain to a sine at hz, less the bands' 12 dB.
double response(Settings settings, double hz) {
    ModuleHarness h(stock("vowel"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [hz](int, std::int64_t n) {
        return 0.1 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
    });
    h.run(9600 + 9600);
    return amplitudeAt(slice(h.output("out"), 9600), hz, 48000) / 0.4;
}
}  // namespace

TEST_CASE("vowel: each vowel's formants") {
    const auto a = voiced({{"vowel", 0}}), i = voiced({{"vowel", 2}}), u = voiced({{"vowel", 4}});
    // a: high first formant, i: low first and high second, u: both low.
    CHECK(bandPower(a, 750, 48000, 4) > 3 * bandPower(i, 750, 48000, 4));
    const auto tilt = [](const std::vector<double>& y, double hi, double lo) {
        return bandPower(y, hi, 48000, 4) / bandPower(y, lo, 48000, 4);
    };
    CHECK(tilt(i, 2250, 750) > 3 * tilt(a, 2250, 750));
    CHECK(bandPower(i, 300, 48000, 4) > 3 * bandPower(a, 300, 48000, 4));
    CHECK(bandPower(u, 750, 48000, 4) > 3 * bandPower(i, 750, 48000, 4));
    CHECK(tilt(a, 1200, 330) > 3 * tilt(u, 1200, 330));
}

TEST_CASE("vowel: the bands peak at the formants") {
    const double at = response({{"vowel", 0}}, 750);
    CHECK(at == doctest::Approx(1.0).epsilon(0.15));
    CHECK(response({{"vowel", 0}}, 600) < 0.6 * at);
    CHECK(response({{"vowel", 0}}, 950) < 0.6 * at);
    // Halfway from a to e the first formant is between theirs, in log.
    const double mid = std::sqrt(750.0 * 480.0);
    CHECK(response({{"vowel", 0.5}}, mid) > 0.85 * response({{"vowel", 0.5}}, mid * 1.15));
    CHECK(response({{"vowel", 0.5}}, mid) > 0.85 * response({{"vowel", 0.5}}, mid / 1.15));
}

TEST_CASE("vowel: Size moves the formants, Resonance narrows them") {
    CHECK(response({{"vowel", 0}, {"size", 12}}, 375) ==
          doctest::Approx(response({{"vowel", 0}}, 750)).epsilon(0.1));
    CHECK(response({{"vowel", 0}, {"size", -12}}, 1500) ==
          doctest::Approx(response({{"vowel", 0}}, 750)).epsilon(0.1));
    CHECK(response({{"vowel", 0}, {"resonance", 100}}, 900) <
          0.5 * response({{"vowel", 0}, {"resonance", 0}}, 900));
}

TEST_CASE("vowel: a saw comes out about as loud, and Mix blends") {
    const double in = 0.3 * std::sqrt(kPi * kPi / 6 / 2);
    for (double v : {0.0, 1.0, 2.0, 3.0, 4.0}) {
        CAPTURE(v);
        const double out = rms(voiced({{"vowel", v}}));
        CHECK(out > 0.5 * in);
        CHECK(out < 2.0 * in);
    }
    const auto dry = voiced({{"mix", 0}});
    CHECK(dry[1000] == doctest::Approx(saw(4800 + 1000)));
}
