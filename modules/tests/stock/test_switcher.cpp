// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("switcher: steps through the inputs and deals Route out") {
    ModuleHarness h(supple::rack::test::stock("switcher"));
    h.set("fade", 0);
    h.set("positions", 3);
    h.input("in_1", 1, [](int, std::int64_t) { return 1.0; });
    h.input("in_2", 1, [](int, std::int64_t) { return 2.0; });
    h.input("in_3", 1, [](int, std::int64_t) { return 3.0; });
    h.input("route", 1, [](int, std::int64_t) { return 0.5; });
    h.input("step", 1, [](int, std::int64_t n) { return n % 1000 >= 900 ? 1.0 : 0.0; });
    h.run(4000);
    CHECK(h.output("out")[500] == 1.0);
    CHECK(h.output("out")[1500] == 2.0);
    CHECK(h.output("out")[2500] == 3.0);
    CHECK(h.output("out")[3500] == 1.0);  // round again after three
    CHECK(h.output("out_2")[1500] == 0.5);
    CHECK(h.output("out_1")[1500] == 0.0);
}

TEST_CASE("switcher: select chooses, fade crossfades, reset returns") {
    ModuleHarness h(supple::rack::test::stock("switcher"));
    h.set("fade", 10);  // 480 frames
    h.input("in_1", 1, [](int, std::int64_t) { return 0.0; });
    h.input("in_4", 1, [](int, std::int64_t) { return 1.0; });
    h.set("select", 0);
    h.run(32);
    h.set("select", 80);  // position 4
    h.run(960);
    CHECK(h.output("out")[32 + 240] == doctest::Approx(0.5).epsilon(0.02));
    CHECK(h.output("out")[900] == 1.0);

    ModuleHarness r(supple::rack::test::stock("switcher"));
    r.set("fade", 0);
    r.input("in_2", 1, [](int, std::int64_t) { return 2.0; });
    r.input("step", 1, [](int, std::int64_t n) { return n >= 100 && n < 200 ? 1.0 : 0.0; });
    r.input("reset", 1, [](int, std::int64_t n) { return n >= 1000 && n < 1100 ? 1.0 : 0.0; });
    r.run(2000);
    CHECK(r.output("out")[500] == 2.0);
    CHECK(r.output("out")[1500] == 0.0);
}
