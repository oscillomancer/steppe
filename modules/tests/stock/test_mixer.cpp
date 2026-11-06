// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("mixer: levels sum, pans place, constant power") {
    ModuleHarness h(supple::rack::test::stock("mixer"));
    h.input("in_1", 1, [](int, std::int64_t) { return 0.5; });
    h.input("in_2", 1, [](int, std::int64_t) { return 0.25; });
    h.set("level_2", -6);
    h.set("pan_1", -100);
    h.run(64);
    const double two = 0.25 * std::pow(10.0, -6.0 / 20.0);
    CHECK(h.output("mono")[40] == doctest::Approx(0.5 + two));
    CHECK(h.output("left")[40] == doctest::Approx(0.5 + two * std::sqrt(0.5)));
    CHECK(h.output("right")[40] == doctest::Approx(two * std::sqrt(0.5)).epsilon(1e-9));
}

TEST_CASE("mixer: mutes fade out, poly is summed, master and off") {
    ModuleHarness h(supple::rack::test::stock("mixer"));
    h.input("in_3", 3, [](int c, std::int64_t) { return 0.1 * (c + 1); });
    h.set("master", -60);
    h.run(32);
    CHECK(h.output("mono")[20] == 0.0);
    h.set("master", 0);
    h.run(4800);  // the master glides up
    CHECK(h.output("mono")[4800] == doctest::Approx(0.6).epsilon(1e-3));
    h.set("mute_3", 1);
    h.run(4800);
    CHECK(h.output("mono")[4832 + 10] > 0.3);  // fading, not cut
    CHECK(std::abs(h.output("mono")[9600]) < 1e-6);
}
