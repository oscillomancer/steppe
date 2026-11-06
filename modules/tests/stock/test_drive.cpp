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
enum { kSoft, kTube, kHard, kFold, kRectify, kBitcrush, kDownsample };

// A sine of `amp` at `hz` through the drive; a whole number of cycles in 9600
// frames when hz is a multiple of 5.
std::vector<double> driven(std::initializer_list<std::pair<const char*, double>> settings,
                           double hz = 1000.0, double amp = 0.5) {
    ModuleHarness h(stock("drive"));
    h.set("level", 0);  // the curves' own levels
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [hz, amp](int, std::int64_t n) {
        return amp * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
    });
    h.run(9600 + 9600);
    return slice(h.output("out"), 9600);
}
// Harmonic n's amplitude of a 1000 Hz tone (200 cycles).
double harmonic(const std::vector<double>& y, int n) {
    return 2.0 * std::sqrt(binPower(y, 200 * n)) / static_cast<double>(y.size());
}
}  // namespace

TEST_CASE("drive: gentle at low drive, saturating at high") {
    const auto clean = driven({{"mode", kSoft}, {"drive", 0}}, 1000.0, 0.05);
    CHECK(harmonic(clean, 1) == doctest::Approx(0.05).epsilon(0.01));
    CHECK(harmonic(clean, 3) < 1e-3 * harmonic(clean, 1));
    const auto hot = driven({{"mode", kSoft}, {"drive", 30}});
    CHECK(peak(hot) < 1.05);
    CHECK(harmonic(hot, 3) > 0.2 * harmonic(hot, 1));
    // Symmetric: no even harmonics, until Bias tips it.
    CHECK(harmonic(hot, 2) < 1e-3 * harmonic(hot, 1));
    const auto tipped = driven({{"mode", kSoft}, {"drive", 12}, {"bias", 30}});
    CHECK(harmonic(tipped, 2) > 0.05 * harmonic(tipped, 1));
    CHECK(std::abs(mean(tipped)) < 1e-3);  // its DC filtered out
}

TEST_CASE("drive: each curve's character") {
    const auto tube = driven({{"mode", kTube}, {"drive", 12}});
    CHECK(harmonic(tube, 2) > 0.1 * harmonic(tube, 1));
    const auto hard = driven({{"mode", kHard}, {"drive", 20}});
    CHECK(harmonic(hard, 3) > 0.25 * harmonic(hard, 1));
    CHECK(harmonic(hard, 2) < 1e-3 * harmonic(hard, 1));
    const auto fold = driven({{"mode", kFold}, {"drive", 18}});
    CHECK(harmonic(fold, 7) > 0.1 * harmonic(fold, 1));
    const auto rect = driven({{"mode", kRectify}, {"drive", 0}});
    CHECK(harmonic(rect, 2) > 5 * harmonic(rect, 1));
}

TEST_CASE("drive: the curves are oversampled") {
    // 3 kHz pushed hard: 600 cycles; aliases land off its harmonics.
    for (int m : {kSoft, kTube, kHard, kFold, kRectify}) {
        CAPTURE(m);
        CHECK(inharmonicDb(driven({{"mode", m}, {"drive", 24}}, 3000.0), 600) < -40);
    }
}

TEST_CASE("drive: Bitcrush steps and Downsample aliases") {
    const auto fine = driven({{"mode", kBitcrush}, {"drive", 0}, {"bits", 16}});
    const auto coarse = driven({{"mode", kBitcrush}, {"drive", 0}, {"bits", 3}});
    const auto distortion = [](const std::vector<double>& y) {
        double off = 0;
        for (int n = 2; n < 24; ++n) off += harmonic(y, n) * harmonic(y, n);
        return std::sqrt(off) / harmonic(y, 1);
    };
    CHECK(distortion(fine) < 1e-3);
    CHECK(distortion(coarse) > 0.05);
    // Held at 6 kHz, 1 kHz has an image at 5 kHz.
    const auto held = driven({{"mode", kDownsample}, {"drive", 0}, {"rate", 6000}});
    CHECK(amplitudeAt(held, 5000, 48000) > 0.05);
    CHECK(amplitudeAt(held, 1000, 48000) > 0.3);
}

TEST_CASE("drive: Tone tilts, Mix blends with the dry input") {
    const auto dark = driven({{"mode", kHard}, {"drive", 20}, {"tone", -100}});
    const auto bright = driven({{"mode", kHard}, {"drive", 20}, {"tone", 100}});
    CHECK(harmonic(dark, 9) / harmonic(dark, 1) < 0.3 * harmonic(bright, 9) / harmonic(bright, 1));
    const auto dry = driven({{"mode", kHard}, {"drive", 30}, {"mix", 0}});
    CHECK(harmonic(dry, 1) == doctest::Approx(0.5).epsilon(1e-3));
    CHECK(harmonic(dry, 3) < 1e-6);
    const auto quiet = driven({{"mode", kSoft}, {"drive", 0}, {"level", -6}}, 1000.0, 0.05);
    CHECK(harmonic(quiet, 1) == doctest::Approx(0.05 * std::pow(10.0, -6.0 / 20)).epsilon(0.01));
}

TEST_CASE("drive: poly, each channel alone") {
    ModuleHarness h(stock("drive"));
    h.input("in", 3, [](int c, std::int64_t) { return 0.1 * c; });
    h.run(480);
    CHECK(h.channels("out") == 3);
}

TEST_CASE("drive: the dry part waits for the oversampled one") {
    // Nearly linear, so half wet and half dry adds up to the input at 12 kHz too.
    const auto blend = driven({{"mode", kSoft}, {"drive", 0}, {"mix", 50}}, 12000.0, 0.01);
    CHECK(amplitudeAt(blend, 12000, 48000) == doctest::Approx(0.01).epsilon(0.01));
    ModuleHarness h(stock("drive"));
    h.set("mix", 0);
    h.set("level", 0);
    h.input("in", 1, [](int, std::int64_t n) { return n == 0 ? 1.0 : 0.0; });
    h.run(480);
    const auto& y = h.output("out");
    CHECK(y[72] == doctest::Approx(1.0));  // 1.5 ms at 48 kHz
}
