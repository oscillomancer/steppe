// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kStep = 4800;

ModuleHarness::Signal clockOf(int period) {
    return [period](int, std::int64_t n) { return (n % period) < 100 ? 1.0 : 0.0; };
}

// The column played at clock `k` (from 0), as eight bits, top row first.
int columnAt(ModuleHarness& h, int k) {
    int c = 0;
    for (int y = 0; y < 8; ++y)
        if (h.output("cells", y)[static_cast<std::size_t>(k * kStep + 10)] == 1.0) c |= 1 << y;
    return c;
}

std::vector<std::int64_t> rises(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 1; n < y.size(); ++n)
        if (y[n] >= 0.5 && y[n - 1] < 0.5) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

}  // namespace

TEST_CASE("life: a full grid plays full columns for a pass, then dies by the classic rule") {
    ModuleHarness h(supple::rack::test::stock("life"));
    h.set("density", 100);
    h.set("mutate", 0);
    h.set("revive", 0);
    h.input("clock", 1, clockOf(kStep));
    h.run(kStep * 12);
    CHECK(h.channels("cells") == 8);
    for (int k = 0; k < 8; ++k) CHECK(columnAt(h, k) == 255);
    CHECK(h.output("value")[10] == doctest::Approx(1.0));
    CHECK(h.output("count")[10] == 1.0);
    CHECK(h.output("alive")[10] == 1.0);
    // Every cell has eight neighbours on the torus: overcrowded, all die.
    for (int k = 8; k < 12; ++k) CHECK(columnAt(h, k) == 0);
    CHECK(h.output("alive")[8 * kStep + 10] == 0.0);
    const auto g = rises(h.output("gen"));
    REQUIRE(g.size() == 1);
    CHECK(g[0] == 8 * kStep);
    CHECK(h.output("cells", 0)[200] == 0.0);  // high with the clock only
}

TEST_CASE("life: Rule and Evolve") {
    SUBCASE("day and night keeps a full grid") {
        ModuleHarness h(supple::rack::test::stock("life"));
        h.set("rule", 3);
        h.set("density", 100);
        h.set("mutate", 0);
        h.set("revive", 0);
        h.set("evolve", 1);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 6);
        for (int k = 0; k < 6; ++k) CHECK(columnAt(h, k) == 255);
        CHECK(rises(h.output("gen")).size() == 5);  // a generation a clock
    }
    SUBCASE("seeds: nothing survives") {
        ModuleHarness h(supple::rack::test::stock("life"));
        h.set("rule", 2);
        h.set("density", 100);
        h.set("mutate", 0);
        h.set("revive", 0);
        h.set("evolve", 1);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 3);
        CHECK(columnAt(h, 0) == 255);
        CHECK(columnAt(h, 1) == 0);
    }
    SUBCASE("revive sows again") {
        ModuleHarness h(supple::rack::test::stock("life"));
        h.set("density", 100);
        h.set("mutate", 0);
        h.set("evolve", 1);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 3);
        CHECK(columnAt(h, 1) == 255);  // died, and sown full again
    }
}

TEST_CASE("life: a random grid lives on; Reset sows the same one again") {
    ModuleHarness h(supple::rack::test::stock("life"));
    h.input("clock", 1, clockOf(kStep));
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 64 * kStep - 1000 && n < 64 * kStep - 900 ? 1.0 : 0.0;
    });
    h.run(kStep * 128);
    bool changed = false;
    for (int k = 0; k < 64; ++k) {
        CHECK(columnAt(h, 64 + k) == columnAt(h, k));
        if (k >= 8 && columnAt(h, k) != columnAt(h, k - 8)) changed = true;
    }
    CHECK(changed);
    const double a = h.output("alive")[10];
    CHECK(a > 0.1);
    CHECK(a < 0.7);
}
