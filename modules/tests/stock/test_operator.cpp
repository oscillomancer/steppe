// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal constant(double v) {
    return [v](int, std::int64_t) { return v; };
}
}  // namespace

TEST_CASE("operator: a sine at the pitch times the ratio, plus the offset") {
    ModuleHarness h(stock("operator"));
    h.set("ratio", 3);
    h.set("offset", 5);
    h.input("pitch", 1, constant(-1));
    h.run(48000);
    CHECK(frequency(h.output("sine"), 48000) == doctest::Approx(kC4 * 1.5 + 5).epsilon(1e-4));
    CHECK(peak(h.output("sine")) == doctest::Approx(1).epsilon(1e-3));
    const auto& p = h.output("phase");
    CHECK(*std::min_element(p.begin(), p.end()) >= 0.0);
    CHECK(*std::max_element(p.begin(), p.end()) < 1.0);
}

TEST_CASE("operator: phase modulation makes Bessel sidebands") {
    // A 300 Hz carrier modulated by 300 Hz at index 1: the second harmonic at
    // J1(1) / J0(1) of the first's ... measured against the spectrum of
    // sin(wt + sin(wt)).
    ModuleHarness h(stock("operator"));
    h.input("pitch", 1, constant(octaveOf(300)));
    h.input("pm", 1, [](int, std::int64_t n) { return std::sin(2 * kPi * 300 * n / 48000.0); });
    h.set("index", 2);
    h.run(9600);
    std::vector<double> ref(9600);
    for (int n = 0; n < 9600; ++n) {
        const double w = 2 * kPi * 300 * n / 48000.0;
        ref[n] = std::sin(w + 2 * std::sin(w));
    }
    const auto& y = h.output("sine");
    for (double hz : {300.0, 600.0, 900.0, 1200.0})
        CHECK(amplitudeAt(y, hz, 48000) ==
              doctest::Approx(amplitudeAt(ref, hz, 48000)).epsilon(0.02));
}

TEST_CASE("operator: feedback brightens, level scales, In Phase locks") {
    ModuleHarness a(stock("operator")), b(stock("operator"));
    b.set("feedback", 60);
    b.set("level", 50);
    a.run(24000);
    b.run(24000);
    const auto ya = slice(a.output("sine"), 4800), yb = slice(b.output("sine"), 4800);
    CHECK(amplitudeAt(yb, 2 * kC4, 48000) > 0.1 * amplitudeAt(yb, kC4, 48000));
    CHECK(amplitudeAt(ya, 2 * kC4, 48000) < 1e-3);
    CHECK(peak(yb) < 0.51);

    ModuleHarness f(stock("operator"));
    f.set("ratio", 2);
    f.input("in_phase", 1, [](int, std::int64_t n) {
        const double p = n / 160.0;  // 300 Hz
        return p - std::floor(p);
    });
    f.run(9600);
    CHECK(frequency(f.output("sine"), 48000) == doctest::Approx(600).epsilon(1e-3));
}
