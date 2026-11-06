// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

int firstHigh(const std::vector<double>& x, std::size_t from = 0) {
    for (std::size_t n = from; n < x.size(); ++n)
        if (x[n] > 0.5) return static_cast<int>(n);
    return -1;
}

}  // namespace

TEST_CASE("function: a trigger rises and falls in their times") {
    ModuleHarness h(supple::rack::test::stock("function"));
    h.set("curve", 0);
    h.set("rise", 10);  // 480 frames
    h.set("fall", 20);  // 960
    h.input("trigger", 1, [](int, std::int64_t n) { return n < 48 ? 1.0 : 0.0; });
    h.run(4800);
    const auto& y = h.output("out");
    CHECK(y[239] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(y[479] == doctest::Approx(1.0));
    CHECK(y[480 + 479] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(y[1500] == 0.0);
    CHECK(std::abs(firstHigh(h.output("end_of_rise")) - 479) <= 1);
    CHECK(firstHigh(h.output("end_of_fall")) == doctest::Approx(1439).epsilon(0.002));
}

TEST_CASE("function: a trigger while falling rises again from there; speed halves times") {
    ModuleHarness h(supple::rack::test::stock("function"));
    h.set("curve", 0);
    h.set("rise", 10);
    h.set("fall", 10);
    h.input("trigger", 1,
            [](int, std::int64_t n) { return n < 48 || (n >= 720 && n < 768) ? 1.0 : 0.0; });
    h.run(2000);
    const auto& y = h.output("out");
    CHECK(y[719] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(y[720 + 230] == doctest::Approx(1.0).epsilon(0.02));  // half the rise left

    ModuleHarness f(supple::rack::test::stock("function"));
    f.set("curve", 0);
    f.set("rise", 10);
    f.set("speed", 1);
    f.input("trigger", 1, [](int, std::int64_t n) { return n < 48 ? 1.0 : 0.0; });
    f.run(1000);
    CHECK(f.output("out")[239] == doctest::Approx(1.0));
}

TEST_CASE("function: cycling is an LFO of rise plus fall") {
    ModuleHarness h(supple::rack::test::stock("function"));
    h.set("cycle", 1);
    h.set("rise", 10);
    h.set("fall", 15);
    h.run(48000);
    int ends = 0;
    const auto& e = h.output("end_of_fall");
    for (std::size_t n = 1; n < e.size(); ++n) ends += e[n] > 0.5 && e[n - 1] < 0.5;
    CHECK(ends == doctest::Approx(40).epsilon(0.05));
}

TEST_CASE("function: a slew limits the rate; rectified it follows a level") {
    ModuleHarness h(supple::rack::test::stock("function"));
    h.set("curve", 0);
    h.set("rise", 10);
    h.set("fall", 100);
    h.input("in", 1, [](int, std::int64_t n) { return n < 2400 ? 1.0 : 0.0; });
    h.run(4800);
    const auto& y = h.output("out");
    CHECK(y[239] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(y[1000] == 1.0);
    CHECK(y[2400 + 479] == doctest::Approx(0.9).epsilon(0.01));  // 10 % of a unit in 10 ms
    CHECK(firstHigh(h.output("end_of_rise")) == doctest::Approx(480).epsilon(0.01));

    ModuleHarness f(supple::rack::test::stock("function"));
    f.set("rectify", 1);
    f.set("rise", 1);
    f.set("fall", 300);
    f.set("curve", 100);
    f.input("in", 1, [](int, std::int64_t n) { return 0.5 * std::sin(n * 0.05); });
    f.run(9600);
    CHECK(f.output("out")[9000] == doctest::Approx(0.5).epsilon(0.05));
}
