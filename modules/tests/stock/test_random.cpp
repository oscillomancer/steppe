// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kPeriod = 2400;

ModuleHarness::Signal clockEvery(int period) {
    return [period](int, std::int64_t n) { return n % period < 1200 ? 1.0 : 0.0; };
}

std::vector<double> steps(const ModuleHarness& h, int count, int c = 0) {
    std::vector<double> v;
    for (int k = 0; k < count; ++k)
        v.push_back(h.output("stepped", c)[static_cast<std::size_t>(k * kPeriod + 10)]);
    return v;
}

}  // namespace

TEST_CASE("random: a locked loop repeats; full change does not") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("loop", 4);
    h.set("change", 0);
    h.set("steps", 0);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(16 * kPeriod);
    const auto v = steps(h, 16);
    for (int k = 4; k < 16; ++k) CHECK(v[k] == v[k - 4]);
    CHECK(std::set<double>(v.begin(), v.end()).size() == 4);

    ModuleHarness f(supple::rack::test::stock("random"));
    f.set("loop", 4);
    f.set("change", 100);
    f.set("steps", 0);
    f.input("clock", 1, clockEvery(kPeriod));
    f.run(16 * kPeriod);
    const auto w = steps(f, 16);
    CHECK(std::set<double>(w.begin(), w.end()).size() == 16);
}

TEST_CASE("random: a little change mutates the phrase slowly") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("loop", 8);
    h.set("change", 10);
    h.set("steps", 0);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(80 * kPeriod);
    const auto v = steps(h, 80);
    int kept = 0;
    for (int k = 8; k < 80; ++k) kept += v[k] == v[k - 8] ? 1 : 0;
    CHECK(kept > 55);  // about 90 % of 72
    CHECK(kept < 72);
}

TEST_CASE("random: steps pull values onto the scale, in its range") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("change", 100);
    h.set("scale", 5);  // minor pentatonic
    h.set("root", 2);   // D
    h.set("range", 2);
    h.set("spread", 100);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(64 * kPeriod);
    const std::set<int> degrees = {0, 3, 5, 7, 10};
    for (double x : steps(h, 64)) {
        const double semis = x * 12.0;
        CHECK(semis == doctest::Approx(std::round(semis)));
        const int degree = ((static_cast<int>(std::lround(semis)) - 2) % 12 + 12) % 12;
        CHECK(degrees.count(degree) == 1);
        CHECK(std::abs(semis) <= 12.5);
    }
}

TEST_CASE("random: bias and spread place the values") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("spread", 0);
    h.set("bias", 0);
    h.set("steps", 0);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(4 * kPeriod);
    for (double x : steps(h, 4)) CHECK(x == doctest::Approx(-1.0));  // the bottom of 2 octaves
}

TEST_CASE("random: a reset plays the same sequence again") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("change", 50);
    h.input("clock", 1, clockEvery(kPeriod));
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 20 * kPeriod - 600 && n < 20 * kPeriod - 500 ? 1.0 : 0.0;
    });
    h.run(40 * kPeriod);
    const auto v = steps(h, 40);
    for (int k = 0; k < 20; ++k) CHECK(v[20 + k] == v[k]);
}

TEST_CASE("random: one gate at a time, by chance and bias") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("change", 100);
    h.set("gate_chance", 100);
    h.set("gate_bias", -100);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(64 * kPeriod);
    int a = 0, b = 0, c = 0;
    for (int k = 0; k < 64; ++k) {
        const std::size_t n = static_cast<std::size_t>(k * kPeriod + 10);
        const double ga = h.output("gate_a")[n], gb = h.output("gate_b")[n],
                     gc = h.output("gate_c")[n];
        CHECK(ga + gb + gc == 1.0);
        a += ga > 0.5;
        b += gb > 0.5;
        c += gc > 0.5;
        CHECK(h.output("gate_a")[n + 1500] + h.output("gate_b")[n + 1500] == 0.0);  // clock low
    }
    CHECK(c == 0);
    CHECK(a > b);

    ModuleHarness z(supple::rack::test::stock("random"));
    z.set("gate_chance", 0);
    z.input("clock", 1, clockEvery(kPeriod));
    z.run(16 * kPeriod);
    for (double x : z.output("gate_b")) REQUIRE(x == 0.0);
}

TEST_CASE("random: voices are chords of their own; smooth glides to each value") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("voices", 3);
    h.set("change", 100);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(8 * kPeriod);
    CHECK(h.channels("stepped") == 3);
    CHECK(steps(h, 8, 0) != steps(h, 8, 1));
    const auto& s = h.output("smooth", 2);
    const auto& st = h.output("stepped", 2);
    CHECK(s[5 * kPeriod - 2] == doctest::Approx(st[5 * kPeriod - 2]).epsilon(1e-3));
    CHECK(std::abs(s[4 * kPeriod + kPeriod / 2] - st[4 * kPeriod + kPeriod / 2]) <=
          std::abs(st[4 * kPeriod + 10] - st[3 * kPeriod + 10]) * 0.51 + 1e-9);
}

TEST_CASE("random: with no clock it steps on the song's eighths") {
    ModuleHarness h(supple::rack::test::stock("random"));
    h.set("change", 100);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 12000 frames an eighth
    h.transport(t);
    h.run(48000);
    const auto& v = h.output("stepped");
    CHECK(v[100] != v[12100]);
    CHECK(v[100] == v[11900]);
}
