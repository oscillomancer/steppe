// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
double db(double g) { return 20 * std::log10(g); }
double response(double hz, const char* out, double resonance, double compensation = 0,
                double pitch = 0) {
    ModuleHarness h(stock("ladder"));
    h.set("cutoff", 1000);
    h.set("drive", 0);
    h.set("resonance", resonance);
    h.set("compensation", compensation);
    h.input("in", 1,
            [hz](int, std::int64_t n) { return 0.05 * std::sin(2 * kPi * hz * n / 48000.0); });
    h.input("pitch", 1, [pitch](int, std::int64_t) { return pitch; });
    h.run(24000);
    return gainAt(h.output(out), hz, 48000, 4800) / 0.05;
}
}  // namespace

TEST_CASE("ladder: four poles, -12 dB at the cutoff, 24 dB per octave") {
    CHECK(db(response(50, "out", 0)) == doctest::Approx(0).epsilon(0.1));
    CHECK(db(response(1000, "out", 0)) == doctest::Approx(-12.04).epsilon(0.03));
    CHECK(db(response(1000, "low12", 0)) == doctest::Approx(-6.02).epsilon(0.03));
    const double slope = db(response(8000, "out", 0)) - db(response(4000, "out", 0));
    CHECK(slope == doctest::Approx(-24).epsilon(0.1));
    CHECK(db(response(100, "high", 0)) < -35);
    CHECK(db(response(15000, "high", 0)) == doctest::Approx(0).epsilon(0.5));
    CHECK(db(response(2000, "out", 0, 0, 1.0)) == doctest::Approx(-12.04).epsilon(0.03));
}

TEST_CASE("ladder: resonance thins the bass, compensation restores it") {
    const double k = 4.4 * 0.8;
    CHECK(response(50, "out", 80, 0) == doctest::Approx(1 / (1 + k)).epsilon(0.03));
    CHECK(response(50, "out", 80, 100) == doctest::Approx(1).epsilon(0.03));
    CHECK(db(response(1000, "out", 80)) > db(response(1000, "out", 0)) + 10);
}

TEST_CASE("ladder: full resonance sings at the cutoff") {
    for (double hz : {110.0, 880.0, 5000.0}) {
        CAPTURE(hz);
        ModuleHarness h(stock("ladder"));
        h.set("cutoff", hz);
        h.set("resonance", 100);
        h.input("in", 1, [](int, std::int64_t n) { return n < 10 ? 0.5 : 0.0; });
        h.run(96000);
        const auto tail = slice(h.output("out"), 48000);
        CHECK(frequency(tail, 48000) == doctest::Approx(hz).epsilon(0.03));
        CHECK(peak(tail) > 0.2);
        CHECK(peak(tail) < 2.0);
    }
}

TEST_CASE("ladder: poly, a cutoff per channel") {
    ModuleHarness h(stock("ladder"));
    h.set("resonance", 100);
    h.input("in", 1, [](int, std::int64_t n) { return n < 10 ? 0.5 : 0.0; });
    h.input("pitch", 2, [](int c, std::int64_t) { return c == 0 ? -1.0 : 0.0; });
    h.run(48000);
    CHECK(frequency(slice(h.output("out", 0), 24000), 48000) == doctest::Approx(400).epsilon(0.03));
    CHECK(frequency(slice(h.output("out", 1), 24000), 48000) == doctest::Approx(800).epsilon(0.03));
}
