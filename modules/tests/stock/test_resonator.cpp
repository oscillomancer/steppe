// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal strikeAt(std::int64_t at) {
    return [at](int, std::int64_t n) { return n >= at && n < at + 48 ? 1.0 : 0.0; };
}
ModuleHarness::Signal constant(double v) {
    return [v](int, std::int64_t) { return v; };
}
ModuleHarness::Signal noise() {
    return [](int, std::int64_t n) {
        std::uint32_t h = static_cast<std::uint32_t>(n) * 2654435761u + 99u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        return (static_cast<double>(h & 0xffff) / 32768.0 - 1.0) * 0.3;
    };
}
double dbOf(double x) { return 20 * std::log10(std::max(x, 1e-30)); }
std::vector<double> struck(double structure, double pitch, const char* out = "out_l",
                           double position = 25, double spread = 0) {
    ModuleHarness h(stock("resonator"));
    h.set("structure", structure);
    h.set("position", position);
    h.set("spread", spread);
    h.input("pitch", 1, constant(pitch));
    h.input("strike", 1, strikeAt(0));
    h.run(48000);
    return h.output(out);
}
}  // namespace

TEST_CASE("resonator: a string's modes are harmonic, a bell has its hum") {
    const auto s = slice(struck(0, 0), 0, 24000);
    CHECK(peakHz(s, 250, 272, 48000, 0.1) == doctest::Approx(kC4).epsilon(0.002));
    CHECK(peakHz(s, 510, 535, 48000, 0.1) == doctest::Approx(2 * kC4).epsilon(0.002));
    const auto b = slice(struck(75, 0), 0, 24000);
    CHECK(peakHz(b, 120, 140, 48000, 0.1) == doctest::Approx(kC4 / 2).epsilon(0.002));
    CHECK(amplitudeAt(b, kC4 * 1.2, 48000) > 0.1 * amplitudeAt(b, kC4, 48000));
    CHECK(amplitudeAt(s, kC4 * 1.2, 48000) < 0.02 * amplitudeAt(s, kC4, 48000));
    // A plate's second mode at 2.5.
    const auto p = slice(struck(100, -1), 0, 24000);
    CHECK(amplitudeAt(p, kC4 / 2 * 2.5, 48000) > 0.1 * amplitudeAt(p, kC4 / 2, 48000));
}

TEST_CASE("resonator: the lowest mode rings for Decay") {
    ModuleHarness h(stock("resonator"));
    h.set("decay", 1000);
    h.input("strike", 1, strikeAt(0));
    h.input("pitch", 1, constant(-1));
    h.run(48000);
    const auto& y = h.output("out_l");
    const double hz = kC4 / 2;
    const double drop = dbOf(amplitudeAt(slice(y, 4800, 14400), hz, 48000)) -
                        dbOf(amplitudeAt(slice(y, 28800, 38400), hz, 48000));
    CHECK(drop == doctest::Approx(30).epsilon(0.1));  // half a second of a 1 s T60
}

TEST_CASE("resonator: position, spread, input") {
    const auto mid = slice(struck(0, 0, "out_l", 50), 0, 24000);
    CHECK(amplitudeAt(mid, 2 * kC4, 48000) < 0.01 * amplitudeAt(mid, kC4, 48000));
    CHECK(struck(0, 0, "out_l", 25, 0) == struck(0, 0, "out_r", 25, 0));
    CHECK(struck(0, 0, "out_l", 25, 100) != struck(0, 0, "out_r", 25, 100));

    ModuleHarness h(stock("resonator"));
    h.input("in", 1, noise());
    h.input("pitch", 1, constant(octaveOf(440)));
    h.run(48000);
    const auto y = slice(h.output("out_l"), 24000);
    CHECK(amplitudeAt(y, 440, 48000) > 5 * amplitudeAt(y, 400, 48000));
    CHECK(rms(y) > 0.05);
    CHECK(rms(y) < 1.0);
    CHECK(peak(struck(0, 0)) < 1.5);
    CHECK(peak(struck(0, 0)) > 0.2);
}
