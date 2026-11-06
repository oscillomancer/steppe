// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("scale: input times gain plus offset; alone, the offset") {
    ModuleHarness h(supple::rack::test::stock("scale"));
    h.input("in_1", 2, [](int c, std::int64_t) { return 0.5 + c; });
    h.set("gain_1", -50);
    h.set("offset_1", 0.25);
    h.set("offset_3", 1.5);
    h.run(32);
    CHECK(h.output("out_1", 0)[10] == doctest::Approx(0.0));
    CHECK(h.output("out_1", 1)[10] == doctest::Approx(-0.5));
    CHECK(h.output("out_3")[10] == doctest::Approx(1.5));
    CHECK(h.output("out_2")[10] == 0.0);
}
