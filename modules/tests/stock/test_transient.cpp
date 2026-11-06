// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

// A hit every half second: a 200 Hz tone dying away over about 60 ms.
double hit(std::int64_t n) {
    const std::int64_t k = n % 24000;
    const double t = static_cast<double>(k) / 48000.0;
    return 0.5 * std::exp(-t / 0.06) * std::sin(2 * kPi * 200.0 * t);
}
std::vector<double> shaped(Settings settings) {
    ModuleHarness h(stock("transient"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [](int, std::int64_t n) { return hit(n); });
    h.run(48000 * 2);
    return h.output("out");
}
// Over the third hit: the peak of its first 10 ms, and its level from 100
// to 160 ms, in dB against the input's.
std::pair<double, double> startAndTail(const std::vector<double>& y) {
    std::vector<double> x(y.size());
    for (std::size_t n = 0; n < x.size(); ++n) x[n] = hit(static_cast<std::int64_t>(n));
    const std::size_t at = 48000;
    const auto db = [](double a, double b) { return 20.0 * std::log10(a / b); };
    return {db(peak(slice(y, at, at + 480)), peak(slice(x, at, at + 480))),
            db(rms(slice(y, at + 4800, at + 7680)), rms(slice(x, at + 4800, at + 7680)))};
}
}  // namespace

TEST_CASE("transient: at 0 and 0 the sound passes untouched; a steady one stays") {
    const auto y = shaped({});
    for (std::int64_t n = 0; n < 96000; n += 37) CHECK(y[n] == doctest::Approx(hit(n)));
    ModuleHarness h(stock("transient"));
    h.set("attack", 100);
    h.set("sustain", 100);
    h.input("in", 1,
            [](int, std::int64_t n) { return 0.5 * std::sin(2 * kPi * 500.0 * n / 48000.0); });
    h.run(48000);
    CHECK(std::abs(20 * std::log10(peak(slice(h.output("out"), 24000)) / 0.5)) < 0.5);
}

TEST_CASE("transient: Attack turns each hit's start up or down, not its tail") {
    const auto [upStart, upTail] = startAndTail(shaped({{"attack", 100}}));
    CHECK(upStart > 6);
    CHECK(upStart < 12.5);
    CHECK(std::abs(upTail) < 1.5);
    // Out of silence, the most it boosts goes with Attack: 12 dB at 100,
    // about 7 at 60.
    const auto [someStart, someTail] = startAndTail(shaped({{"attack", 60}}));
    CHECK(someStart > 3);
    CHECK(someStart < 7.5);
    const auto [downStart, downTail] = startAndTail(shaped({{"attack", -100}}));
    CHECK(downStart < -6);
    CHECK(std::abs(downTail) < 1.5);
}

TEST_CASE("transient: Sustain turns each hit's tail up or down, not its start") {
    const auto [upStart, upTail] = startAndTail(shaped({{"sustain", 100}}));
    CHECK(upTail > 6);
    CHECK(std::abs(upStart) < 1.5);
    const auto [downStart, downTail] = startAndTail(shaped({{"sustain", -100}}));
    CHECK(downTail < -6);
    CHECK(std::abs(downStart) < 1.5);
}

TEST_CASE("transient: Level after; poly channels each their own") {
    ModuleHarness h(stock("transient"));
    h.set("level", -6);
    h.set("attack", 100);
    h.input("in", 2, [](int c, std::int64_t n) { return c == 0 ? hit(n) : 0.0; });
    h.run(48000);
    CHECK(h.channels("out") == 2);
    CHECK(peak(h.output("out", 1)) == 0.0);
    CHECK(peak(h.output("out", 0)) > 0.5);
}
