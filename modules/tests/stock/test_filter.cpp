// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
// The filter's gain at hz on an output, for a quiet sine (linear range).
double response(double hz, const char* out, double resonance, double pitch = 0.0) {
    ModuleHarness h(stock("filter"));
    h.set("cutoff", 1000);
    h.set("resonance", resonance);
    h.input("in", 1,
            [hz](int, std::int64_t n) { return 0.1 * std::sin(2 * kPi * hz * n / 48000.0); });
    h.input("pitch", 1, [pitch](int, std::int64_t) { return pitch; });
    h.run(24000);
    return gainAt(h.output(out), hz, 48000, 4800) / 0.1;
}
double db(double g) { return 20 * std::log10(g); }
}  // namespace

TEST_CASE("filter: each output's response around the cutoff") {
    // Resonance 0 is a Q of 0.5: -6 dB at the cutoff for low, band and high.
    CHECK(db(response(1000, "low", 0)) == doctest::Approx(-6.02).epsilon(0.02));
    CHECK(db(response(1000, "high", 0)) == doctest::Approx(-6.02).epsilon(0.02));
    CHECK(db(response(1000, "band", 0)) == doctest::Approx(-6.02).epsilon(0.02));
    CHECK(db(response(1000, "notch", 0)) < -40);
    CHECK(db(response(100, "low", 0)) == doctest::Approx(0).epsilon(0.1));
    CHECK(db(response(10000, "high", 0)) == doctest::Approx(0).epsilon(0.1));
    // Slopes: an octave apart far above the cutoff, 12 and 24 dB.
    const double low12 = db(response(8000, "low", 30)) - db(response(4000, "low", 30));
    const double low24 = db(response(8000, "low24", 30)) - db(response(4000, "low24", 30));
    CHECK(low12 == doctest::Approx(-12).epsilon(0.15));
    CHECK(low24 == doctest::Approx(-24).epsilon(0.15));
    // Resonance raises the peak.
    CHECK(db(response(1000, "low", 90)) > 12);
}

TEST_CASE("filter: the pitch input tracks 1 per octave") {
    CHECK(db(response(2000, "low", 0, 1.0)) == doctest::Approx(-6.02).epsilon(0.02));
    CHECK(db(response(250, "band", 0, -2.0)) == doctest::Approx(-6.02).epsilon(0.02));
}

TEST_CASE("filter: full resonance sings at the cutoff, steadily") {
    ModuleHarness h(stock("filter"));
    h.set("cutoff", 440);
    h.set("resonance", 100);
    h.input("in", 1, [](int, std::int64_t n) { return n < 10 ? 0.5 : 0.0; });
    h.run(96000);
    const auto tail = slice(h.output("low"), 48000);
    CHECK(frequency(tail, 48000) == doctest::Approx(440).epsilon(0.03));
    const double a = peak(slice(tail, 0, 24000)), b = peak(slice(tail, 24000));
    CHECK(a > 0.3);
    CHECK(a < 3.0);
    CHECK(b == doctest::Approx(a).epsilon(0.02));
}

TEST_CASE("filter: stable under audio-rate cutoff modulation") {
    ModuleHarness h(stock("filter"));
    h.set("resonance", 95);
    h.set("fm_depth", 6);
    h.input("in", 1, [](int, std::int64_t n) { return std::sin(n * 0.031) > 0 ? 1.0 : -1.0; });
    h.input("fm", 1, [](int, std::int64_t n) { return std::sin(2 * kPi * 1500 * n / 48000.0); });
    h.set("drive", 100);
    h.run(48000);
    for (const char* out : {"low", "band", "high", "notch", "low24"}) {
        CAPTURE(out);
        const double p = peak(h.output(out));
        CHECK(std::isfinite(p));
        CHECK(p < 8.0);
    }
}

TEST_CASE("filter: drive is clean at 0 and saturates at 100") {
    for (double drive : {0.0, 100.0}) {
        ModuleHarness h(stock("filter"));
        h.set("cutoff", 20000);
        h.set("resonance", 0);
        h.set("drive", drive);
        h.input("in", 1,
                [](int, std::int64_t n) { return 0.5 * std::sin(2 * kPi * 100 * n / 48000.0); });
        h.run(9600);
        const auto y = slice(h.output("low"), 4800);
        const double third = amplitudeAt(y, 300, 48000) / amplitudeAt(y, 100, 48000);
        if (drive == 0)
            CHECK(third < 0.01);
        else
            CHECK(third > 0.2);
    }
}

TEST_CASE("filter: poly, a cutoff per channel") {
    ModuleHarness h(stock("filter"));
    h.set("resonance", 100);
    h.input("in", 1, [](int, std::int64_t n) { return n < 10 ? 0.5 : 0.0; });
    h.input("pitch", 2, [](int c, std::int64_t) { return c == 0 ? -1.0 : 0.0; });
    h.run(48000);
    CHECK(h.channels("low") == 2);
    CHECK(frequency(slice(h.output("low", 0), 24000), 48000) == doctest::Approx(500).epsilon(0.03));
    CHECK(frequency(slice(h.output("low", 1), 24000), 48000) ==
          doctest::Approx(1000).epsilon(0.03));
}
