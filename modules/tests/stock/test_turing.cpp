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

std::vector<double> values(ModuleHarness& h, int count) {
    h.input("clock", 1, [](int, std::int64_t n) { return n % kPeriod < 100 ? 1.0 : 0.0; });
    h.run(static_cast<std::int64_t>(count) * kPeriod);
    std::vector<double> v;
    for (int k = 0; k < count; ++k)
        v.push_back(h.output("out")[static_cast<std::size_t>(k * kPeriod + 10)]);
    return v;
}

}  // namespace

TEST_CASE("turing: locked, the loop repeats every length; 100 % every two lengths") {
    ModuleHarness h(supple::rack::test::stock("turing"));
    h.set("flip", 0);
    h.set("length", 5);
    const auto v = values(h, 40);
    for (int k = 13; k < 40; ++k) CHECK(v[k] == v[k - 5]);  // once the first 8 bits are the loop's

    ModuleHarness d(supple::rack::test::stock("turing"));
    d.set("flip", 100);
    d.set("length", 5);
    const auto w = values(d, 60);
    for (int k = 20; k < 60; ++k) CHECK(w[k] == w[k - 10]);
    bool differs = false;
    for (int k = 20; k < 60; ++k) differs = differs || w[k] != w[k - 5];
    CHECK(differs);
}

TEST_CASE("turing: write fills it with ones, clear with zeros; scale and offset place it") {
    ModuleHarness h(supple::rack::test::stock("turing"));
    h.set("scale", 2);
    h.set("offset", -1);
    h.input("write", 1, [](int, std::int64_t) { return 1.0; });
    const auto v = values(h, 10);
    CHECK(v[9] == doctest::Approx(1.0));  // all on: -1 + 2
    ModuleHarness c(supple::rack::test::stock("turing"));
    c.input("clear", 1, [](int, std::int64_t) { return 1.0; });
    const auto w = values(c, 10);
    CHECK(w[9] == 0.0);
    CHECK(c.output("bit_1")[9 * kPeriod + 10] == 0.0);
    CHECK(h.output("bit_8")[9 * kPeriod + 10] == 1.0);
    CHECK(h.output("bit_8")[9 * kPeriod + 200] == 0.0);  // with the clock
}

TEST_CASE("turing: chaos wanders; reset starts the same register over") {
    ModuleHarness h(supple::rack::test::stock("turing"));
    h.set("flip", 50);
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 30 * kPeriod - 500 && n < 30 * kPeriod - 400 ? 1.0 : 0.0;
    });
    const auto v = values(h, 60);
    CHECK(std::set<double>(v.begin(), v.end()).size() > 20);
    for (int k = 0; k < 30; ++k) CHECK(v[30 + k] == v[k]);
}
