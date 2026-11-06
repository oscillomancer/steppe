// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <set>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("hold: samples at each rising edge") {
    ModuleHarness h(supple::rack::test::stock("hold"));
    h.input("in", 1, [](int, std::int64_t n) { return n / 1000.0; });
    h.input("trigger", 1, [](int, std::int64_t n) { return n % 1000 < 100 ? 1.0 : 0.0; });
    h.run(3000);
    CHECK(h.output("out")[500] == 0.0);
    CHECK(h.output("out")[1500] == doctest::Approx(1.0));
    CHECK(h.output("out")[2999] == doctest::Approx(2.0));
}

TEST_CASE("hold: tracks while high, holds while low") {
    ModuleHarness h(supple::rack::test::stock("hold"));
    h.set("mode", 1);
    h.input("in", 1, [](int, std::int64_t n) { return n / 1000.0; });
    h.input("trigger", 1, [](int, std::int64_t n) { return n < 500 ? 1.0 : 0.0; });
    h.run(1000);
    CHECK(h.output("out")[400] == doctest::Approx(0.4));
    CHECK(h.output("out")[900] == doctest::Approx(0.499));
}

TEST_CASE("hold: noise alone, a value per voice per trigger") {
    ModuleHarness h(supple::rack::test::stock("hold"));
    h.set("voices", 3);
    h.input("trigger", 1, [](int, std::int64_t n) { return n % 1000 < 100 ? 1.0 : 0.0; });
    h.run(20000);
    CHECK(h.channels("out") == 3);
    std::set<double> seen;
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 20; ++k) {
            const double x = h.output("out", c)[static_cast<std::size_t>(k * 1000 + 50)];
            CHECK(x >= -1.0);
            CHECK(x < 1.0);
            CHECK(h.output("out", c)[static_cast<std::size_t>(k * 1000 + 900)] == x);
            seen.insert(x);
        }
    CHECK(seen.size() == 60);
}
