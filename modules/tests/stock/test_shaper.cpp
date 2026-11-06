// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
// 770 Hz: 154 cycles in 9600 frames.
std::vector<double> shaped(int curve, double skew = 0, double fold = 0, double width = 50) {
    ModuleHarness h(stock("shaper"));
    h.set("curve", curve);
    h.set("skew", skew);
    h.set("fold", fold);
    h.set("width", width);
    h.input("phase", 1, [](int, std::int64_t n) {
        const double p = n * 770.0 / 48000.0;
        return p - std::floor(p);
    });
    h.run(4800 + 9600);
    return slice(h.output("out"), 4800);
}
double harmonic(const std::vector<double>& y, int n) { return std::sqrt(binPower(y, 154 * n)); }
}  // namespace

TEST_CASE("shaper: each curve from the phase") {
    const auto sine = shaped(0);
    CHECK(harmonic(sine, 2) < 1e-6 * harmonic(sine, 1));
    CHECK(peak(sine) == doctest::Approx(1).epsilon(1e-3));
    const auto tri = shaped(1);
    CHECK(harmonic(tri, 3) / harmonic(tri, 1) == doctest::Approx(1.0 / 9).epsilon(0.05));
    const auto saw = shaped(2);
    CHECK(harmonic(saw, 2) / harmonic(saw, 1) == doctest::Approx(0.5).epsilon(0.03));
    const auto pulse = shaped(3, 0, 0, 25);
    CHECK(mean(pulse) == doctest::Approx(-0.5).epsilon(0.02));
    const auto step = shaped(4);
    CHECK(harmonic(step, 2) < 1e-6 * harmonic(step, 1));
    CHECK(harmonic(step, 3) > 0.05 * harmonic(step, 1));
    const auto window = shaped(5, 0, 0, 20);  // 1 + round(15 * 0.2) = 4 overtones up
    CHECK(harmonic(window, 4) > harmonic(window, 1));
}

TEST_CASE("shaper: skew is phase distortion, fold adds overtones") {
    const auto bent = shaped(0, 60);
    CHECK(harmonic(bent, 2) > 0.08 * harmonic(bent, 1));
    const auto folded = shaped(0, 0, 60);
    CHECK(harmonic(folded, 3) > 0.05 * harmonic(folded, 1));
}

TEST_CASE("shaper: steps are smoothed") {
    CHECK(inharmonicDb(shaped(2), 154) < -30);
    CHECK(inharmonicDb(shaped(3, 30, 0, 30), 154) < -30);
}
