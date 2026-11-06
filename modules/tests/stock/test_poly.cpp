// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("poly: merge up to the last input, split, sum") {
    ModuleHarness h(supple::rack::test::stock("poly"));
    h.input("merge_1", 1, [](int, std::int64_t) { return 0.1; });
    h.input("merge_3", 1, [](int, std::int64_t) { return 0.3; });
    h.input("poly_in", 4, [](int c, std::int64_t) { return c + 1.0; });
    h.run(32);
    CHECK(h.channels("merged") == 3);
    CHECK(h.output("merged", 0)[10] == doctest::Approx(0.1));
    CHECK(h.output("merged", 1)[10] == 0.0);
    CHECK(h.output("merged", 2)[10] == doctest::Approx(0.3));
    CHECK(h.output("split_4")[10] == 4.0);
    CHECK(h.output("split_5")[10] == 0.0);
    CHECK(h.output("sum")[10] == 10.0);
    h.set("average", 1);
    h.set("channels", 5);
    h.run(32);
    CHECK(h.output("sum")[40] == 2.5);
    CHECK(h.channels("merged") == 5);
}

TEST_CASE("poly: spread pans the first left and the last right") {
    ModuleHarness h(supple::rack::test::stock("poly"));
    h.input("poly_in", 2, [](int c, std::int64_t) { return c == 0 ? 1.0 : 0.0; });
    h.run(32);
    CHECK(h.output("left")[10] == doctest::Approx(1.0));
    CHECK(std::abs(h.output("right")[10]) < 1e-12);
}
