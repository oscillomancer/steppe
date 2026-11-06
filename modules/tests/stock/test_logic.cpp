// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("logic: the truth tables") {
    ModuleHarness h(supple::rack::test::stock("logic"));
    // A: 0 0 1 1, B: 0 1 0 1, a block each.
    h.input("a", 1, [](int, std::int64_t n) { return n >= 64 ? 1.0 : 0.0; });
    h.input("b", 1, [](int, std::int64_t n) { return (n / 32) % 2 == 1 ? 1.0 : 0.0; });
    h.run(128);
    auto at = [&](const char* key, int block) {
        return h.output(key)[static_cast<std::size_t>(block * 32 + 16)];
    };
    for (int k = 0; k < 4; ++k) {
        const bool a = k >= 2, b = k % 2 == 1;
        CAPTURE(k);
        CHECK(at("and", k) == (a && b ? 1.0 : 0.0));
        CHECK(at("or", k) == (a || b ? 1.0 : 0.0));
        CHECK(at("xor", k) == (a != b ? 1.0 : 0.0));
        CHECK(at("not_a", k) == (a ? 0.0 : 1.0));
    }
}

TEST_CASE("logic: the flip-flop halves a clock; change fires at both edges") {
    ModuleHarness h(supple::rack::test::stock("logic"));
    h.input("a", 1, [](int, std::int64_t n) { return n % 1000 < 500 ? 1.0 : 0.0; });
    h.run(4000);
    const auto& f = h.output("flip_flop");
    CHECK(f[100] == 1.0);
    CHECK(f[1100] == 0.0);
    CHECK(f[2100] == 1.0);
    int pulses = 0;
    const auto& c = h.output("change");
    for (std::size_t n = 1; n < c.size(); ++n) pulses += c[n] > 0.5 && c[n - 1] < 0.5;
    CHECK(pulses == 7);  // the first rise is at frame 0
    CHECK(c[0] == 1.0);
}
