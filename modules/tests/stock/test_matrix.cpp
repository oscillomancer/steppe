// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("matrix: each output sums every input times its knob") {
    ModuleHarness h(supple::rack::test::stock("matrix"));
    h.set("a_to_1", 100);
    h.set("b_to_1", -50);
    h.set("c_to_2", 25);
    h.set("d_to_4", 100);  // D unpatched: an offset of 1
    h.input("a", 1, [](int, std::int64_t) { return 0.8; });
    h.input("b", 1, [](int, std::int64_t) { return 0.4; });
    h.input("c", 1, [](int, std::int64_t) { return -1.0; });
    h.run(2000);
    CHECK(h.output("out_1")[1500] == doctest::Approx(0.8 - 0.2));
    CHECK(h.output("out_2")[1500] == doctest::Approx(-0.25));
    CHECK(h.output("out_3")[1500] == 0.0);
    CHECK(h.output("out_4")[1500] == doctest::Approx(1.0));
}

TEST_CASE("matrix: poly inputs keep their channels; a mono one reaches them all") {
    ModuleHarness h(supple::rack::test::stock("matrix"));
    h.set("a_to_1", 100);
    h.set("b_to_1", 100);
    h.input("a", 3, [](int c, std::int64_t) { return 0.1 * (c + 1); });
    h.input("b", 1, [](int, std::int64_t) { return 1.0; });
    h.run(2000);
    CHECK(h.channels("out_1") == 3);
    for (int c = 0; c < 3; ++c)
        CHECK(h.output("out_1", c)[1500] == doctest::Approx(1.0 + 0.1 * (c + 1)));
}

TEST_CASE("matrix: a knob turned glides; a cable into it is followed at once") {
    ModuleHarness h(supple::rack::test::stock("matrix"));
    h.input("a", 1, [](int, std::int64_t) { return 1.0; });
    h.modulate("a_to_3", 1, [](int, std::int64_t n) { return n < 1000 ? 0.0 : 1.0; }, 50.0);
    h.run(2000);
    h.set("a_to_2", 100);
    h.run(2000);
    const auto& cable = h.output("out_3");
    CHECK(cable[999] == 0.0);
    CHECK(cable[1000] == doctest::Approx(0.5));
    const auto& knob = h.output("out_2");
    CHECK(knob[1999] == 0.0);
    CHECK(knob[3900] == doctest::Approx(1.0).epsilon(0.01));
    for (std::size_t n = 1; n < knob.size(); ++n) CHECK(std::abs(knob[n] - knob[n - 1]) < 0.05);
}
