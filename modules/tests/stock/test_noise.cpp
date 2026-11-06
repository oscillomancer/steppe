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
double db(double power) { return 10 * std::log10(power); }
// How the band power changes from 500 Hz to 2 kHz, two octaves, in dB.
double tilt(const std::vector<double>& y) {
    return db(bandPower(y, 2000, 48000)) - db(bandPower(y, 500, 48000));
}
}  // namespace

TEST_CASE("noise: every colour at a usable level, with its slope") {
    ModuleHarness h(stock("noise"));
    h.run(96000);
    for (const char* out : {"white", "pink", "brown", "blue", "digital"}) {
        CAPTURE(out);
        const double r = rms(h.output(out));
        CHECK(r == doctest::Approx(0.3).epsilon(0.08));
        CHECK(peak(h.output(out)) <= 1.5);
    }
    // Band powers of constant-Q bands two octaves apart: white rises 6 dB,
    // pink stays, brown falls 6, blue rises 12.
    CHECK(tilt(h.output("white")) == doctest::Approx(6).epsilon(0.25));
    CHECK(std::abs(tilt(h.output("pink"))) < 1.5);
    CHECK(tilt(h.output("brown")) == doctest::Approx(-6).epsilon(0.25));
    CHECK(tilt(h.output("blue")) == doctest::Approx(12).epsilon(0.15));
}

TEST_CASE("noise: crackle clicks Density times a second") {
    ModuleHarness h(stock("noise"));
    h.set("density", 200);
    h.run(48000 * 4);
    int clicks = 0;
    const auto& y = h.output("crackle");
    for (std::size_t n = 1; n < y.size(); ++n)
        clicks += std::abs(y[n]) > 0.19 && std::abs(y[n]) > 2 * std::abs(y[n - 1]);
    CHECK(clicks == doctest::Approx(800).epsilon(0.1));
}

TEST_CASE("noise: digital noise holds between clock ticks") {
    for (int mode : {0, 1, 2}) {
        CAPTURE(mode);
        ModuleHarness h(stock("noise"));
        h.set("rate", 1000);
        h.set("mode", mode);
        h.run(48000);
        const auto& y = h.output("digital");
        int changes = 0;
        for (std::size_t n = 1; n < y.size(); ++n) changes += y[n] != y[n - 1];
        CHECK(changes <= 1000);
        CHECK(changes > (mode == 2 ? 990 : 300));
    }
    // Metallic loops: its pattern repeats.
    ModuleHarness h(stock("noise"));
    h.set("rate", 48000);
    h.set("mode", 1);
    h.run(4800);
    const auto& y = h.output("digital");
    int period = 0;
    for (int p = 2; p < 2000 && period == 0; ++p) {
        bool same = true;
        for (int n = 1000; n < 2000 && same; ++n) same = y[n] == y[n + p];
        if (same) period = p;
    }
    CHECK(period > 10);
    CHECK(period < 200);
}

TEST_CASE("noise: Channels sets independent channels") {
    ModuleHarness h(stock("noise"));
    h.set("channels", 4);
    h.run(4800);
    CHECK(h.channels("white") == 4);
    CHECK(h.channels("crackle") == 4);
    CHECK(h.output("pink", 0) != h.output("pink", 3));
    CHECK(rms(h.output("brown", 3)) > 0.1);
}
