// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("slew: a straight rise and fall at their rates") {
    ModuleHarness h(supple::rack::test::stock("slew"));
    h.set("rise", 10);   // a unit in 480 frames
    h.set("fall", 100);  // in 4800
    h.input("in", 1, [](int, std::int64_t n) { return n >= 32 && n < 2400 ? 1.0 : 0.0; });
    h.run(4800);
    const auto& y = h.output("out");
    CHECK(y[32 + 239] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(y[1000] == 1.0);
    CHECK(y[2400 + 479] == doctest::Approx(0.9).epsilon(0.01));
}

TEST_CASE("slew: exponential settles in about its time; poly channels apart") {
    ModuleHarness h(supple::rack::test::stock("slew"));
    h.set("rise", 10);
    h.set("shape", 100);
    h.input("in", 2, [](int c, std::int64_t n) { return n >= 32 ? 0.5 * (c + 1) : 0.0; });
    h.run(2000);
    CHECK(h.output("out", 0)[32 + 120] > 0.25);  // fast first: past half by a quarter
    CHECK(h.output("out", 1)[32 + 480] == doctest::Approx(1.0).epsilon(0.03));
}

TEST_CASE("slew: the glide limit leaves leaps alone") {
    ModuleHarness h(supple::rack::test::stock("slew"));
    h.set("rise", 100);
    h.set("glide_limit", 3);
    h.input("in", 1, [](int, std::int64_t n) {
        return n < 1000 ? 0.0 : (n < 20000 ? 2.0 / 12.0 : 9.0 / 12.0);
    });
    h.run(24000);
    const auto& y = h.output("out");
    CHECK(y[1100] > 0.0);
    CHECK(y[1100] < 2.0 / 12.0);                     // a tone glides
    CHECK(y[20000] == doctest::Approx(9.0 / 12.0));  // a sixth leaps
}
