// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
struct Spectrum {
    double h1, h2, h3, upper, inharmonic;
};
// A 1.5 kHz sine through the folder: 300 cycles in 9600 frames.
Spectrum fold(double amount, double symmetry, int stages, const char* out = "out") {
    ModuleHarness h(stock("fold"));
    h.set("fold", amount);
    h.set("symmetry", symmetry);
    h.set("stages", stages);
    h.input("in", 1,
            [](int, std::int64_t n) { return 0.8 * std::sin(2 * kPi * 1500 * n / 48000.0); });
    h.run(9600 * 2);
    const auto y = slice(h.output(out), 9600);
    Spectrum s{};
    s.h1 = std::sqrt(binPower(y, 300));
    s.h2 = std::sqrt(binPower(y, 600));
    s.h3 = std::sqrt(binPower(y, 900));
    double up = 0;
    for (std::size_t k = 1200; k < 4800; k += 300) up += binPower(y, k);
    s.upper = std::sqrt(up);
    s.inharmonic = inharmonicDb(y, 300);
    return s;
}
}  // namespace

TEST_CASE("fold: clean at no fold, brighter as it folds") {
    const Spectrum clean = fold(0, 0, 4);
    CHECK(clean.h3 / clean.h1 < 0.02);
    const Spectrum some = fold(40, 0, 4), lots = fold(100, 0, 4);
    CHECK(some.upper / some.h1 > 0.3);
    CHECK(lots.upper / lots.h1 > some.upper / some.h1);
    CHECK(lots.h2 / lots.h1 < 0.01);  // symmetric: odd harmonics only
}

TEST_CASE("fold: symmetry brings even harmonics, stages more folds") {
    const Spectrum tilted = fold(40, 40, 4);
    CHECK(tilted.h2 / tilted.h1 > 0.1);
    const Spectrum one = fold(100, 0, 1), six = fold(100, 0, 6);
    CHECK(six.upper / six.h1 > one.upper / one.h1);
}

TEST_CASE("fold: oversampling keeps aliases down") {
    CHECK(fold(100, 0, 6).inharmonic < -40);
    CHECK(fold(100, 30, 6).inharmonic < -40);
}

TEST_CASE("fold: the soft output clips and keeps its level") {
    const Spectrum s = fold(100, 0, 4, "soft");
    CHECK(s.h3 / s.h1 > 0.1);
    ModuleHarness h(stock("fold"));
    h.input("in", 1,
            [](int, std::int64_t n) { return 0.8 * std::sin(2 * kPi * 1500 * n / 48000.0); });
    h.set("fold", 100);
    h.run(9600);
    CHECK(peak(slice(h.output("out"), 4800)) < 1.6);
    CHECK(peak(slice(h.output("soft"), 4800)) < 1.1);
    CHECK(rms(slice(h.output("out"), 4800)) > 0.25);
}
