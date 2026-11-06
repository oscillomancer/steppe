// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal hitAt(std::int64_t at) {
    return [at](int, std::int64_t n) { return n >= at && n < at + 48 ? 1.0 : 0.0; };
}
std::vector<double> play(int model, double decay = 50, double accent = 0, double tune = 0) {
    ModuleHarness h(stock("drum"));
    h.set("model", model);
    h.set("decay", decay);
    h.set("tune", tune);
    h.input("trigger", 1, hitAt(4800));
    h.input("accent", 1, [accent](int, std::int64_t) { return accent; });
    h.run(48000 * 2);
    return h.output("out");
}
}  // namespace

TEST_CASE("drum: every model is silent until hit, then sounds and dies away") {
    for (int m = 0; m < 8; ++m) {
        CAPTURE(m);
        const auto y = play(m);
        CHECK(peak(slice(y, 0, 4800)) < 1e-9);
        const double p = peak(slice(y, 4800, 4800 + 4800));
        CHECK(p > 0.15);
        CHECK(p < 1.6);
        // Gone within a second at the shortest Decay; longer with more.
        const auto short_ = play(m, 0);
        CHECK(rms(slice(short_, 4800 + 48000)) <
              0.01 * rms(slice(short_, 4800, 4800 + 2400)) + 1e-6);
        const auto long_ = play(m, 100);
        CHECK(rms(slice(long_, 4800 + 9600, 4800 + 19200)) >
              rms(slice(y, 4800 + 9600, 4800 + 19200)));
    }
}

TEST_CASE("drum: a kick settles to its pitch, Tune moves it") {
    const auto low = slice(play(0, 100), 4800 + 9600, 4800 + 33600);
    const auto high = slice(play(0, 100, 0, 12), 4800 + 9600, 4800 + 33600);
    const double f = frequency(low, 48000);
    CHECK(f == doctest::Approx(42).epsilon(0.05));
    CHECK(frequency(high, 48000) == doctest::Approx(2 * f).epsilon(0.03));
}

TEST_CASE("drum: hats are high, kicks low, accent is louder") {
    const auto hat = slice(play(3), 4800, 4800 + 2400);
    CHECK(bandPower(hat, 9000, 48000) > 100 * bandPower(hat, 300, 48000));
    const auto kick = slice(play(0), 4800, 4800 + 9600);
    CHECK(bandPower(kick, 60, 48000) > 100 * bandPower(kick, 5000, 48000));
    for (int m : {0, 1, 5}) {
        CAPTURE(m);
        CHECK(peak(play(m, 50, 1)) > 1.6 * peak(play(m, 50, 0)));
    }
}

TEST_CASE("drum: env follows the hit, poly hits each channel") {
    ModuleHarness h(stock("drum"));
    h.set("decay", 30);
    h.input("trigger", 2, [](int c, std::int64_t n) { return c == 1 && n < 48 ? 1.0 : 0.0; });
    h.run(48000);
    CHECK(peak(h.output("out", 0)) < 1e-9);
    CHECK(peak(h.output("out", 1)) > 0.3);
    CHECK(h.output("env", 1)[10] > 0.95);
    CHECK(h.output("env", 1)[40000] < 0.05);
}
