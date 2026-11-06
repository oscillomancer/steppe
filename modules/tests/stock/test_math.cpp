// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("math: the arithmetic of A and B") {
    ModuleHarness h(supple::rack::test::stock("math"));
    h.input("a", 1, [](int, std::int64_t) { return -0.5; });
    h.input("b", 1, [](int, std::int64_t) { return 0.25; });
    h.set("blend", 25);
    h.run(32);
    CHECK(h.output("sum")[10] == doctest::Approx(-0.25));
    CHECK(h.output("difference")[10] == doctest::Approx(-0.75));
    CHECK(h.output("product")[10] == doctest::Approx(-0.125));
    CHECK(h.output("minimum")[10] == doctest::Approx(-0.5));
    CHECK(h.output("maximum")[10] == doctest::Approx(0.25));
    CHECK(h.output("absolute")[10] == doctest::Approx(0.5));
    CHECK(h.output("mix")[10] == doctest::Approx(-0.3125));
    CHECK(h.output("greater")[10] == 0.0);
}

TEST_CASE("math: the comparator with hysteresis") {
    ModuleHarness h(supple::rack::test::stock("math"));
    h.set("hysteresis", 0.2);
    // A sine with noise on it, against 0.
    h.input("a", 1, [](int, std::int64_t n) {
        return std::sin(n * 2 * M_PI / 4800) + 0.05 * std::sin(n * 1.7);
    });
    h.run(48000);
    int rises = 0;
    const auto& g = h.output("greater");
    for (std::size_t n = 1; n < g.size(); ++n) rises += g[n] > 0.5 && g[n - 1] < 0.5;
    CHECK(rises == 10);
    CHECK(g[1200] == 1.0);
    CHECK(g[3600] == 0.0);
}
