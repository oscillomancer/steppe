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

double tone(std::int64_t n, double hz = 440.0) {
    return 0.5 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}
std::vector<double> shifted(Settings settings, double hz = 440.0) {
    ModuleHarness h(stock("pitchshift"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [hz](int, std::int64_t n) { return tone(n, hz); });
    h.run(9600 + 48000);
    return slice(h.output("out"), 9600);
}
}  // namespace

TEST_CASE("pitchshift: moves a tone up and down by the shift") {
    for (double st : {12.0, 7.0, -12.0, 24.0, -24.0, 3.0}) {
        CAPTURE(st);
        const auto y = shifted({{"shift", st}});
        const double to = 440.0 * std::exp2(st / 12.0);
        CHECK(frequency(y, 48000) == doctest::Approx(to).epsilon(0.002));
        CHECK(amplitudeAt(y, to, 48000) > 0.4);
        CHECK(rms(y) > 0.5 * 0.5 / std::sqrt(2.0));
        CHECK(rms(y) < 1.2 * 0.5 / std::sqrt(2.0));
        CHECK(bandPower(y, to, 48000, 4) > 30 * bandPower(y, to > 440 ? to / 2 : to * 2, 48000, 4));
    }
    // Fine adds cents.
    const auto up = shifted({{"shift", 0}, {"fine", 100}});
    CHECK(frequency(up, 48000) == doctest::Approx(440 * std::exp2(1.0 / 12)).epsilon(0.002));
}

TEST_CASE("pitchshift: at no shift, one copy of the input half a window late; Mix blends") {
    // From the start: the taps never moved, so the copy is half a window
    // late (and two samples).
    ModuleHarness h(stock("pitchshift"));
    h.set("shift", 0);
    h.set("window", 50);
    h.input("in", 1, [](int, std::int64_t n) { return tone(n, 300); });
    h.run(48000);
    const auto y = slice(h.output("out"), 24000);
    CHECK(amplitudeAt(y, 300, 48000) == doctest::Approx(0.5).epsilon(1e-3));
    // After a shift that ends at 0: still one copy, at full level.
    const auto back = shifted({{"shift", 0}, {"window", 50}}, 300);
    CHECK(amplitudeAt(slice(back, 24000), 300, 48000) == doctest::Approx(0.5).epsilon(1e-3));

    const auto dry = shifted({{"mix", 0}});
    for (int n = 0; n < 2000; n += 97) CHECK(dry[n] == doctest::Approx(tone(9600 + n)));
}

TEST_CASE("pitchshift: Feedback shifts again and again") {
    // A burst at 440 Hz up an octave: the first pass at 880, the second at
    // 1760 only with Feedback.
    const auto run = [](double fb) {
        ModuleHarness h(stock("pitchshift"));
        h.set("shift", 12);
        h.set("feedback", fb);
        h.input("in", 1, [](int, std::int64_t n) { return n < 9600 ? tone(n) : 0.0; });
        h.run(9600);
        return slice(h.output("out"), 4800);
    };
    const auto none = run(0), some = run(70);
    CHECK(amplitudeAt(none, 1760, 48000) < 1e-3);
    CHECK(amplitudeAt(some, 1760, 48000) > 0.05);
    // It stays bounded at the most.
    ModuleHarness h(stock("pitchshift"));
    h.set("shift", 0);
    h.set("feedback", 95);
    h.input("in", 1, [](int, std::int64_t n) { return 2.0 * tone(n); });
    h.run(96000);
    CHECK(peak(h.output("out")) < 4.0);
}

TEST_CASE("pitchshift: poly channels shift on their own") {
    ModuleHarness h(stock("pitchshift"));
    h.set("shift", -12);
    h.input("in", 2, [](int c, std::int64_t n) { return tone(n, c == 0 ? 400.0 : 1000.0); });
    h.run(9600 + 24000);
    CHECK(h.channels("out") == 2);
    const auto a = slice(h.output("out", 0), 9600), b = slice(h.output("out", 1), 9600);
    CHECK(amplitudeAt(a, 200, 48000) > 0.3);
    CHECK(amplitudeAt(a, 500, 48000) < 0.01);
    CHECK(amplitudeAt(b, 500, 48000) > 0.3);
    CHECK(amplitudeAt(b, 200, 48000) < 0.01);
}
