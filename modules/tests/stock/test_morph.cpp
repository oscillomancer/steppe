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
std::vector<double> tone(double shape, double warp, const char* out = "out") {
    ModuleHarness h(stock("morph"));
    h.set("shape", shape);
    h.set("warp", warp);
    h.input("pitch", 1, [](int, std::int64_t) { return octaveOf(770.0); });
    h.run(4800 + 9600);
    return slice(h.output(out), 4800);
}
double harmonic(const std::vector<double>& y, int n) { return std::sqrt(binPower(y, 154 * n)); }
}  // namespace

TEST_CASE("morph: its pitch, and the sub an octave down") {
    ModuleHarness h(stock("morph"));
    h.run(48000);
    CHECK(frequency(h.output("out"), 48000) == doctest::Approx(kC4).epsilon(1e-3));
    CHECK(frequency(h.output("sub"), 48000) == doctest::Approx(kC4 / 2).epsilon(1e-3));
}

TEST_CASE("morph: the shapes along the way") {
    const auto sine = tone(0, 0);
    CHECK(harmonic(sine, 3) < 1e-3 * harmonic(sine, 1));
    CHECK(peak(sine) == doctest::Approx(1).epsilon(0.01));
    const auto tri = tone(20, 0);
    CHECK(harmonic(tri, 3) / harmonic(tri, 1) == doctest::Approx(1.0 / 9).epsilon(0.05));
    CHECK(harmonic(tri, 2) < 1e-3 * harmonic(tri, 1));
    const auto saw = tone(40, 0);
    CHECK(harmonic(saw, 2) / harmonic(saw, 1) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(harmonic(saw, 3) / harmonic(saw, 1) == doctest::Approx(1.0 / 3).epsilon(0.03));
    const auto square = tone(60, 0);
    CHECK(harmonic(square, 2) < 1e-3 * harmonic(square, 1));
    CHECK(harmonic(square, 3) / harmonic(square, 1) == doctest::Approx(1.0 / 3).epsilon(0.03));
    // Narrow pulses and bursts have strong upper harmonics; warp bends a sine.
    const auto pulse = tone(80, 0), burst = tone(100, 0), bent = tone(0, 60);
    CHECK(harmonic(pulse, 5) > 0.5 * harmonic(pulse, 1));
    CHECK(harmonic(burst, 7) > harmonic(burst, 1));
    CHECK(harmonic(bent, 2) > 0.1 * harmonic(bent, 1));
}

TEST_CASE("morph: anti-aliased at every shape and warp") {
    for (double shape : {20.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0})
        for (double warp : {0.0, 50.0, -70.0}) {
            CAPTURE(shape);
            CAPTURE(warp);
            // A pulse narrowed to 5 % keeps its spectrum flat far up, so its
            // weak fundamental leaves its aliases relatively louder.
            CHECK(inharmonicDb(tone(shape, warp), 154) < (shape >= 80 ? -25 : -40));
        }
    CHECK(inharmonicDb(tone(0, 0, "sub"), 77) < -40);
}

TEST_CASE("morph: a sweep of Shape stays in bounds") {
    ModuleHarness h(stock("morph"));
    h.input("pitch", 1, [](int, std::int64_t) { return -1.0; });
    h.modulate(
        "shape", 1, [](int, std::int64_t n) { return std::sin(2 * kPi * 3 * n / 48000.0); }, 50);
    h.modulate(
        "warp", 1, [](int, std::int64_t n) { return std::sin(2 * kPi * 0.7 * n / 48000.0); }, 100);
    h.set("shape", 50);
    h.run(48000);
    CHECK(peak(h.output("out")) < 1.35);
    CHECK(rms(h.output("out")) > 0.3);
}
