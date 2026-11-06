// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("macro: knobs out of their jacks, unipolar or bipolar, glided") {
    ModuleHarness h(supple::rack::test::stock("macro"));
    h.set("macro_3", 25);
    h.set("macro_8", 100);
    h.run(32);
    CHECK(h.output("out_3")[10] == doctest::Approx(0.25));
    CHECK(h.output("out_8")[10] == doctest::Approx(1.0));
    CHECK(h.output("out_1")[10] == 0.0);
    h.set("range", 1);
    h.set("smooth", 10);
    h.run(4800);
    CHECK(h.output("out_3")[40] > -0.5);  // gliding towards -0.5
    CHECK(h.output("out_3")[4800] == doctest::Approx(-0.5).epsilon(1e-3));
    CHECK(h.output("out_1")[4800] == doctest::Approx(-1.0).epsilon(1e-3));
}
