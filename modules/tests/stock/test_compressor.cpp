// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

double sine(std::int64_t n, double amplitude, double hz = 1000.0) {
    return amplitude * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}
double db(double x) { return 20.0 * std::log10(x); }
double fromDb(double x) { return std::pow(10.0, x / 20.0); }

// The output's peak in dB once settled, for a 1 kHz sine `levelDb` peak.
double settled(Settings settings, double levelDb) {
    ModuleHarness h(stock("compressor"));
    h.set("attack", 1);
    h.set("release", 1000);
    for (const auto& [key, value] : settings) h.set(key, value);
    const double a = fromDb(levelDb);
    h.input("in", 1, [a](int, std::int64_t n) { return sine(n, a); });
    h.run(48000);
    return db(peak(slice(h.output("out"), 36000)));
}
}  // namespace

TEST_CASE("compressor: below the threshold untouched, above it reduced by the ratio") {
    CHECK(settled({{"knee", 0}}, -30) == doctest::Approx(-30).epsilon(1e-6));
    // Hard knee, -18 dB, 4:1: 12 dB over comes out 3 dB over.
    CHECK(settled({{"knee", 0}}, -6) == doctest::Approx(-15).epsilon(0.02));
    CHECK(settled({{"knee", 0}, {"ratio", 2}}, -6) == doctest::Approx(-12).epsilon(0.02));
    CHECK(settled({{"knee", 0}, {"ratio", 20}, {"threshold", -12}}, 0) ==
          doctest::Approx(-12 + 12.0 / 20).epsilon(0.03));
    // The knee eases in: at the threshold, a quarter of the knee's slope.
    const double at = settled({{"knee", 12}}, -18);
    CHECK(at == doctest::Approx(-18 + (0.25 - 1) * 36.0 / 24.0).epsilon(0.02));
    // Makeup adds after.
    CHECK(settled({{"knee", 0}, {"makeup", 6}}, -6) == doctest::Approx(-9).epsilon(0.03));
}

TEST_CASE("compressor: Attack and Release set how fast it turns down and lets go") {
    // A step from -40 to -6 dB at 0.5 s, back at 1 s: 9 dB of reduction.
    ModuleHarness h(stock("compressor"));
    h.set("knee", 0);
    h.set("attack", 20);
    h.set("release", 200);
    h.input("in", 1, [](int, std::int64_t n) { return n >= 24000 && n < 48000 ? 1.0 : 0.01; });
    h.run(96000);
    const auto& g = h.output("gain");
    // 0 dB in: 18 over, 13.5 reduced.
    CHECK(g[24000 - 1] == doctest::Approx(1.0));
    CHECK(db(g[48000 - 1]) == doctest::Approx(-13.5).epsilon(0.01));
    // After one Attack, 63 % of the way in dB; after one Release, 63 % back.
    CHECK(db(g[24000 + 960]) == doctest::Approx(-13.5 * (1 - std::exp(-1.0))).epsilon(0.03));
    CHECK(db(g[48000 + 9600]) == doctest::Approx(-13.5 * std::exp(-1.0)).epsilon(0.03));
}

TEST_CASE("compressor: Sidechain ducks the input; SC Filter keeps the bass out") {
    const auto duck = [](double hz, double filter) {
        ModuleHarness h(stock("compressor"));
        h.set("threshold", -30);
        h.set("ratio", 10);
        h.set("sc_filter", filter);
        h.input("in", 1, [](int, std::int64_t n) { return sine(n, 0.5, 300.0); });
        h.input("sidechain", 1,
                [hz](int, std::int64_t n) { return n >= 24000 ? sine(n, 0.9, hz) : 0.0; });
        h.run(48000);
        return std::make_pair(peak(slice(h.output("out"), 12000, 24000)),
                              peak(slice(h.output("out"), 36000)));
    };
    const auto [before, during] = duck(1000, 20);
    CHECK(before == doctest::Approx(0.5).epsilon(1e-3));  // the input's own level is not heard
    CHECK(db(during / before) < -20);
    const auto [b2, bass] = duck(40, 500);
    CHECK(db(bass / b2) > db(during / before) + 15);
}

TEST_CASE("compressor: Link turns every channel down by the loudest; unlinked each alone") {
    const auto run = [](bool linked) {
        ModuleHarness h(stock("compressor"));
        h.set("knee", 0);
        h.set("link", linked ? 1 : 0);
        h.input("in", 2, [](int c, std::int64_t n) { return sine(n, c == 0 ? 1.0 : 0.05); });
        h.run(24000);
        return std::make_pair(peak(slice(h.output("out", 0), 12000)),
                              peak(slice(h.output("out", 1), 12000)));
    };
    const auto [loudL, quietL] = run(true);
    CHECK(db(quietL / 0.05) == doctest::Approx(db(loudL)).epsilon(0.02));
    const auto [loudU, quietU] = run(false);
    CHECK(quietU == doctest::Approx(0.05).epsilon(1e-3));
    CHECK(db(loudU) == doctest::Approx(db(loudL)).epsilon(0.02));
}

TEST_CASE("compressor: Mix blends the dry for parallel compression") {
    ModuleHarness h(stock("compressor"));
    h.set("knee", 0);
    h.set("mix", 50);
    h.set("attack", 1);
    h.set("release", 1000);
    h.input("in", 1, [](int, std::int64_t n) { return sine(n, 1.0); });
    h.run(48000);
    CHECK(peak(slice(h.output("out"), 36000)) ==
          doctest::Approx(0.5 + 0.5 * fromDb(-13.5)).epsilon(0.01));
}
