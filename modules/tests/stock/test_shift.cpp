// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

// A second of output, after half a second, for a sine of amp at hz.
std::vector<double> shifted(Settings settings, double hz, const char* out = "up",
                            double amp = 0.5) {
    ModuleHarness h(stock("shift"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [hz, amp](int, std::int64_t n) {
        return amp * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
    });
    h.run(24000 + 48000);
    return slice(h.output(out), 24000);
}
double at(const std::vector<double>& y, double hz) { return amplitudeAt(y, hz, 48000); }
}  // namespace

TEST_CASE("shift: Up and Down move every frequency by Shift's Hz") {
    const auto upward = shifted({{"shift", 100}}, 1000);
    CHECK(at(upward, 1100) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(at(upward, 900) < 0.5 * 0.002);
    const auto downward = shifted({{"shift", 100}}, 1000, "down");
    CHECK(at(downward, 900) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(at(downward, 1100) < 0.5 * 0.002);
}

TEST_CASE("shift: the other sideband is 50 dB down across the band") {
    for (double hz : {30.0, 100.0, 1000.0, 5000.0, 12000.0, 18000.0}) {
        CAPTURE(hz);
        const auto y = shifted({{"shift", 10}}, hz);
        CHECK(at(y, hz + 10) == doctest::Approx(0.5).epsilon(0.01));
        CHECK(20 * std::log10(at(y, hz - 10) / at(y, hz + 10)) < -50);
    }
}

TEST_CASE("shift: through zero, by the knob and by CV") {
    const auto below = shifted({{"shift", -100}}, 1000);
    CHECK(at(below, 900) == doctest::Approx(0.5).epsilon(0.01));
    const auto still = shifted({{"shift", 0}}, 1000);
    CHECK(at(still, 1000) == doctest::Approx(0.5).epsilon(0.01));
    const auto fine = shifted({{"shift", 0}, {"fine", 5}}, 1000);
    CHECK(at(fine, 1005) == doctest::Approx(0.5).epsilon(0.01));

    ModuleHarness h(stock("shift"));
    h.set("shift", 0);
    h.set("cv_depth", -200);
    h.input("in", 1,
            [](int, std::int64_t n) { return 0.5 * std::sin(2 * kPi * 1000 * n / 48000.0); });
    h.input("cv", 1, [](int, std::int64_t) { return 0.5; });
    h.run(72000);
    CHECK(at(slice(h.output("up"), 24000), 900) == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("shift: Feedback shifts again on each pass") {
    const auto spiral = shifted({{"shift", 100}, {"feedback", 50}}, 1000, "up", 0.05);
    CHECK(at(spiral, 1100) == doctest::Approx(0.05).epsilon(0.01));
    CHECK(at(spiral, 1200) == doctest::Approx(0.025).epsilon(0.01));
    CHECK(at(spiral, 1300) == doctest::Approx(0.0125).epsilon(0.01));
}

TEST_CASE("shift: the ring modulator, its own carrier or a patched one") {
    const auto rung = shifted({{"carrier", 300}}, 1000, "ring");
    CHECK(at(rung, 700) == doctest::Approx(0.25).epsilon(0.01));
    CHECK(at(rung, 1300) == doctest::Approx(0.25).epsilon(0.01));
    CHECK(at(rung, 1000) < 1e-3);

    ModuleHarness h(stock("shift"));
    h.input("in", 1, [](int, std::int64_t) { return 0.5; });
    h.input("carrier_in", 1, [](int, std::int64_t) { return -0.5; });
    h.run(480);
    CHECK(h.output("ring")[100] == doctest::Approx(-0.25));
}

TEST_CASE("shift: poly, each channel alone") {
    ModuleHarness h(stock("shift"));
    h.input("in", 2, [](int c, std::int64_t n) { return c * std::sin(n * 0.1); });
    h.run(4800);
    REQUIRE(h.channels("up") == 2);
    CHECK(peak(h.output("up", 0)) == 0.0);
    CHECK(peak(h.output("up", 1)) > 0.5);
}
