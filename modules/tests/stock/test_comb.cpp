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

// The gain to a small sine at hz.
double response(Settings settings, double hz, double octave = 0.0) {
    ModuleHarness h(stock("comb"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("pitch", 1, [octave](int, std::int64_t) { return octave; });
    h.input("in", 1, [hz](int, std::int64_t n) {
        return 0.01 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
    });
    h.run(48000 + 48000);
    return amplitudeAt(slice(h.output("out"), 48000), hz, 48000) / 0.01;
}
// Deterministic white noise, about 0.3 RMS.
double noise(std::int64_t n) {
    std::uint64_t z = static_cast<std::uint64_t>(n) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 31)) * 0xBF58476D1CE4E5B9ull;
    z ^= z >> 29;
    return 0.3 * std::sqrt(3.0) * (static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0);
}
}  // namespace

TEST_CASE("comb: rings at the pitch and its harmonics") {
    const Settings s = {{"feedback", 90}, {"damping", 0}};
    const double at = response(s, kC4), second = response(s, 2 * kC4);
    const double between = response(s, 1.5 * kC4);
    CHECK(at > 10 * between);
    CHECK(second > 10 * between);
    // A pitch of 1 is an octave up.
    CHECK(response(s, 2 * kC4, 1.0) > 10 * response(s, 3 * kC4, 1.0));
}

TEST_CASE("comb: negative feedback rings only the odd harmonics, at the same pitch") {
    const Settings s = {{"feedback", -90}, {"damping", 0}};
    const double first = response(s, kC4), third = response(s, 3 * kC4);
    const double second = response(s, 2 * kC4);
    CHECK(first > 10 * second);
    CHECK(third > 10 * second);
}

TEST_CASE("comb: in tune with damping") {
    for (double damping : {0.0, 30.0, 50.0}) {
        CAPTURE(damping);
        ModuleHarness h(stock("comb"));
        h.set("feedback", 98);
        h.set("damping", damping);
        h.input("in", 1, [](int, std::int64_t n) { return n == 0 ? 1.0 : 0.0; });
        h.run(96000);
        CHECK(peakHz(slice(h.output("out"), 0, 48000), 250, 275, 48000, 0.05) ==
              doctest::Approx(kC4).epsilon(2e-3));
    }
}

TEST_CASE("comb: damping darkens the high harmonics") {
    const double brightRatio = response({{"feedback", 90}, {"damping", 0}}, 8 * kC4) /
                               response({{"feedback", 90}, {"damping", 0}}, kC4);
    const double darkRatio = response({{"feedback", 90}, {"damping", 60}}, 8 * kC4) /
                             response({{"feedback", 90}, {"damping", 60}}, kC4);
    CHECK(darkRatio < 0.3 * brightRatio);
}

TEST_CASE("comb: noise keeps its level at any feedback; Mix 0 is dry") {
    for (double fb : {0.0, 50.0, 90.0, -90.0, 95.0}) {
        CAPTURE(fb);
        ModuleHarness h(stock("comb"));
        h.set("feedback", fb);
        h.set("damping", 0);
        h.input("in", 1, [](int, std::int64_t n) { return noise(n); });
        h.run(96000);
        const double level = rms(slice(h.output("out"), 48000));
        CHECK(level > 0.15);  // within 6 dB of the input
        CHECK(level < 0.45);
    }
    ModuleHarness h(stock("comb"));
    h.set("mix", 0);
    h.input("in", 1, [](int, std::int64_t n) { return noise(n); });
    h.run(480);
    CHECK(h.output("out")[300] == doctest::Approx(noise(300)));
}
