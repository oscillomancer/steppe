// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("pitch: alone, a constant pitch from C4") {
    ModuleHarness h(supple::rack::test::stock("pitch"));
    h.run(64);
    CHECK(h.channels("out") == 1);
    CHECK(h.output("out")[10] == 0.0);

    ModuleHarness g(supple::rack::test::stock("pitch"));
    g.set("octave", -1);
    g.set("semitone", 9);
    g.set("fine", 50);
    g.run(64);
    CHECK(g.output("out")[63] == doctest::Approx(-1.0 + 9.0 / 12.0 + 0.5 / 12.0));
}

TEST_CASE("pitch: a poly cable transposed channel by channel") {
    ModuleHarness h(supple::rack::test::stock("pitch"));
    h.set("semitone", 7);
    h.input("in", 3, [](int c, std::int64_t) { return c / 12.0; });
    h.run(64);
    CHECK(h.channels("out") == 3);
    for (int c = 0; c < 3; ++c) CHECK(h.output("out", c)[40] == doctest::Approx((c + 7) / 12.0));
}

TEST_CASE("pitch: a cable into Semitone moves in whole steps") {
    ModuleHarness h(supple::rack::test::stock("pitch"));
    h.set("semitone", 0);
    h.modulate("semitone", 1, [](int, std::int64_t) { return 0.26; }, 12.0);  // 3.12 semitones
    h.run(64);
    CHECK(h.output("out")[63] == doctest::Approx(3.0 / 12.0));
}
