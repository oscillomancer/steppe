// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal constant(double v) {
    return [v](int, std::int64_t) { return v; };
}
}  // namespace

TEST_CASE("vco: pitch is 1 per octave from C4, with coarse and fine") {
    const auto& code = stock("vco");
    {
        ModuleHarness h(code);
        h.run(48000);
        CHECK(frequency(h.output("sine"), 48000) == doctest::Approx(kC4).epsilon(1e-4));
        CHECK(frequency(h.output("saw"), 48000) == doctest::Approx(kC4).epsilon(1e-3));
    }
    {
        ModuleHarness h(code);
        h.input("pitch", 1, constant(1.0));
        h.set("coarse", -7);
        h.set("fine", 50);
        h.run(48000);
        const double hz = kC4 * 2.0 * std::exp2((-7.0 + 0.5) / 12.0);
        CHECK(frequency(h.output("sine"), 48000) == doctest::Approx(hz).epsilon(1e-4));
        CHECK(frequency(h.output("sub"), 48000) == doctest::Approx(hz / 2).epsilon(1e-3));
    }
}

TEST_CASE("vco: every output is full scale, the pulse follows its width") {
    ModuleHarness h(stock("vco"));
    h.set("pulse_width", 25);
    h.run(24000);
    for (const char* out : {"sine", "triangle", "saw", "pulse", "sub"}) {
        CAPTURE(out);
        CHECK(peak(slice(h.output(out), 1000)) == doctest::Approx(1.0).epsilon(0.05));
    }
    CHECK(mean(slice(h.output("pulse"), 1000, 1000 + 183 * 100)) ==
          doctest::Approx(-0.5).epsilon(0.02));
    CHECK(std::abs(mean(slice(h.output("saw"), 1000, 1000 + 183 * 100))) < 0.01);
}

TEST_CASE("vco: the edges are anti-aliased") {
    // 770 Hz: exactly 154 cycles in 9600 frames (the sub 77), so aliases land
    // between the harmonics' bins.
    ModuleHarness h(stock("vco"));
    h.input("pitch", 1, constant(octaveOf(770.0)));
    h.set("pulse_width", 30);
    h.run(4800 + 9600);
    for (const char* out : {"saw", "pulse", "triangle", "sub"}) {
        CAPTURE(out);
        // A naive saw is -17 dB here, a two-sample polyBLEP's -33 dB; the
        // triangle's aliases are far lower.
        const std::size_t cycles = std::string(out) == "sub" ? 77 : 154;
        CHECK(inharmonicDb(slice(h.output(out), 4800), cycles) <
              (std::string(out) == "triangle" ? -70.0 : -40.0));
    }
}

TEST_CASE("vco: hard sync locks the cycle to the sync input") {
    ModuleHarness h(stock("vco"));
    h.input("pitch", 1, constant(octaveOf(337.0)));
    h.input("sync", 1, [](int, std::int64_t n) { return (n % 480) < 240 ? 1.0 : -1.0; });  // 100 Hz
    h.run(9600);
    const auto& y = h.output("saw");
    double worst = 0;
    for (std::size_t n = 2000; n < 9000; ++n) worst = std::max(worst, std::abs(y[n] - y[n + 480]));
    CHECK(worst < 1e-6);
    CHECK(frequency(slice(h.output("sine"), 2000), 48000) > 300.0);  // still its own pitch inside
}

TEST_CASE("vco: linear FM goes through zero") {
    ModuleHarness h(stock("vco"));
    h.input("lin_fm", 1, constant(-2.0));  // index 100 %: the frequency is -C4
    h.run(4800);
    const auto& y = h.output("saw");
    int falling = 0;
    for (std::size_t n = 100; n < 4800; ++n) falling += y[n] < y[n - 1] ? 1 : 0;
    CHECK(falling > 4000);  // the saw runs backwards
    CHECK(frequency(h.output("sine"), 48000) == doctest::Approx(kC4).epsilon(1e-3));
}

TEST_CASE("vco: exp FM in octaves, poly by pitch") {
    ModuleHarness h(stock("vco"));
    h.input("pitch", 3, [](int c, std::int64_t) { return c * 1.0; });
    h.input("exp_fm", 1, constant(0.5));
    h.set("fm_depth", -2);  // -1 octave
    h.run(48000);
    CHECK(h.channels("sine") == 3);
    for (int c = 0; c < 3; ++c)
        CHECK(frequency(h.output("sine", c), 48000) ==
              doctest::Approx(kC4 * std::exp2(c - 1.0)).epsilon(1e-3));
}

TEST_CASE("vco: drift wanders a little and the same every time") {
    ModuleHarness a(stock("vco")), b(stock("vco"));
    a.set("drift", 100);
    b.set("drift", 100);
    a.run(48000);
    b.run(48000);
    CHECK(a.output("saw") == b.output("saw"));
    const double hz = frequency(a.output("sine"), 48000);
    CHECK(hz != doctest::Approx(kC4).epsilon(1e-6));
    CHECK(hz == doctest::Approx(kC4).epsilon(0.01));
}
