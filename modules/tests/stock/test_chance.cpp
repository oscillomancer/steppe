// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kPeriod = 4800;

void clocked(ModuleHarness& h, int count, int channels = 1) {
    h.input("gate", channels, [](int, std::int64_t n) { return n % kPeriod < 1200 ? 1.0 : 0.0; });
    h.run(static_cast<std::int64_t>(count) * kPeriod);
}

int rises(const ModuleHarness& h, const std::string& key, int c = 0) {
    int count = 0;
    const auto& x = h.output(key, c);
    for (std::size_t n = 0; n < x.size(); ++n) count += x[n] > 0.5 && (n == 0 || x[n - 1] < 0.5);
    return count;
}

}  // namespace

TEST_CASE("chance: every gate goes to A or B, by the chance") {
    ModuleHarness h(supple::rack::test::stock("chance"));
    clocked(h, 200);
    const int a = rises(h, "a"), b = rises(h, "b");
    CHECK(a + b == 200);
    CHECK(a == doctest::Approx(100).epsilon(0.2));
    for (int k = 0; k < 200; ++k) {
        const std::size_t n = static_cast<std::size_t>(k * kPeriod + 600);
        CHECK(h.output("a")[n] + h.output("b")[n] == 1.0);
        CHECK(h.output("a")[n + 1200] + h.output("b")[n + 1200] == 0.0);  // follows the gate
    }
    ModuleHarness all(supple::rack::test::stock("chance"));
    all.set("chance", 100);
    clocked(all, 20);
    CHECK(rises(all, "a") == 20);
}

TEST_CASE("chance: latched, one side is always high; channels toss apart") {
    ModuleHarness h(supple::rack::test::stock("chance"));
    h.set("latch", 1);
    clocked(h, 50, 2);
    for (std::size_t n = 0; n < h.output("a").size(); ++n)
        REQUIRE(h.output("a")[n] + h.output("b")[n] == 1.0);
    CHECK(h.channels("a") == 2);
    CHECK(h.output("a", 0) != h.output("a", 1));
}

TEST_CASE("chance: ratchets burst over the time to the next gate") {
    ModuleHarness h(supple::rack::test::stock("chance"));
    h.set("chance", 100);
    h.set("ratchet_chance", 100);
    h.set("ratchets", 4);
    clocked(h, 10);
    CHECK(rises(h, "a") == doctest::Approx(40).epsilon(0.05));
    // From the second gate on: a pulse every 1200 frames.
    const auto& x = h.output("a");
    CHECK(x[5 * kPeriod + 100] == 1.0);
    CHECK(x[5 * kPeriod + 700] == 0.0);
    CHECK(x[5 * kPeriod + 1300] == 1.0);
    CHECK(x[5 * kPeriod + 3700] == 1.0);
}
