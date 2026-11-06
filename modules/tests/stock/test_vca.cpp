// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("vca: the CV opens it, the level scales it") {
    const auto& code = supple::rack::test::stock("vca");
    ModuleHarness h(code);
    h.input("in", 1, [](int, std::int64_t) { return 0.5; });
    h.input("cv", 1, [](int, std::int64_t n) { return n < 480 ? 1.0 : 0.0; });
    h.set("response", 1);  // linear
    h.set("level", -6);
    h.run(960);
    CHECK(h.output("out")[100] == doctest::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-9));
    CHECK(h.output("out")[700] == 0.0);
}

TEST_CASE("vca: open fully with nothing in CV, poly follows the input") {
    const auto& code = supple::rack::test::stock("vca");
    ModuleHarness h(code);
    h.input("in", 3, [](int c, std::int64_t) { return 0.1 * (c + 1); });
    h.run(64);
    CHECK(h.channels("out") == 3);
    CHECK(h.output("out", 2)[10] == doctest::Approx(0.3));
}

TEST_CASE("vca: a cable into the level moves it in dB") {
    const auto& code = supple::rack::test::stock("vca");
    ModuleHarness h(code);
    h.input("in", 1, [](int, std::int64_t) { return 1.0; });
    h.modulate("level", 1, [](int, std::int64_t) { return -1.0; }, 12.0);  // -12 dB
    h.run(64);
    CHECK(h.output("out")[20] == doctest::Approx(std::pow(10.0, -12.0 / 20.0)));
}
