// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

std::vector<std::int64_t> rises(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 1; n < y.size(); ++n)
        if (y[n] >= 0.5 && y[n - 1] < 0.5) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

}  // namespace

TEST_CASE("stages: Gated holds at the Hold stage, then goes on as the gate falls") {
    ModuleHarness h(supple::rack::test::stock("stages"));
    // The defaults: to 1 in 10 ms, to 0.6 in 200, at 0.6 for 300 (held),
    // to 0 in 500.
    h.input("gate", 1, [](int, std::int64_t n) { return n < 48000 ? 1.0 : 0.0; });
    h.run(96000);
    const auto& y = h.output("out");
    CHECK(y[480] == doctest::Approx(1.0).epsilon(0.01));
    CHECK(y[10600] == doctest::Approx(0.6));
    CHECK(y[40000] == doctest::Approx(0.6));
    CHECK(h.output("stage_3")[40000] == 1.0);
    CHECK(h.output("stage_4")[50000] == 1.0);
    CHECK(y[50000] < 0.6);
    CHECK(y[48000 + 24000 + 10] == 0.0);
    const auto e = rises(h.output("end"));
    REQUIRE(e.size() == 1);
    CHECK(std::abs(e[0] - 72000) <= 2);
    // Never a jump: each frame's move is small.
    for (std::size_t n = 1; n < y.size(); ++n) CHECK(std::abs(y[n] - y[n - 1]) < 0.02);
}

TEST_CASE("stages: Once runs every stage; Loop cycles; Speed scales the times") {
    SUBCASE("once") {
        ModuleHarness h(supple::rack::test::stock("stages"));
        h.set("mode", 0);
        h.input("gate", 1, [](int, std::int64_t n) { return n < 100 ? 1.0 : 0.0; });
        h.run(60000);
        // 10 + 200 + 300 + 500 ms, whatever the gate does.
        const auto e = rises(h.output("end"));
        REQUIRE(e.size() == 1);
        CHECK(std::abs(e[0] - 48480) <= 4);
        CHECK(h.output("out")[40000] > 0.0);
        CHECK(h.output("out")[55000] == 0.0);
    }
    SUBCASE("loop, at Speed 1 and 2") {
        for (double speed : {1.0, 2.0}) {
            ModuleHarness h(supple::rack::test::stock("stages"));
            h.set("mode", 1);
            h.set("speed", speed);
            h.run(48480 * 4);
            const auto e = rises(h.output("end"));
            REQUIRE(e.size() >= 3);
            const double period = 48480.0 / speed;
            CHECK(std::abs(static_cast<double>(e[2] - e[1]) - period) <= 4.0);
        }
    }
}

TEST_CASE("stages: Curve bends a stage; a retrigger starts from where it is") {
    auto midway = [](double curve) {
        ModuleHarness h(supple::rack::test::stock("stages"));
        h.set("mode", 0);
        h.set("time_1", 1000);
        h.set("curve_1", curve);
        h.input("gate", 1, [](int, std::int64_t n) { return n < 100 ? 1.0 : 0.0; });
        h.run(30000);
        return h.output("out")[24000];
    };
    CHECK(midway(0) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(midway(100) > 0.9);
    CHECK(midway(-100) < 0.1);

    ModuleHarness h(supple::rack::test::stock("stages"));
    h.set("mode", 0);
    h.input("gate", 2, [](int c, std::int64_t n) {
        const std::int64_t at = c == 0 ? 0 : 6000;
        return (n >= at && n < at + 100) || (c == 0 && n >= 9000 && n < 9100) ? 1.0 : 0.0;
    });
    h.run(20000);
    const auto& y = h.output("out", 0);
    for (std::size_t n = 1; n < y.size(); ++n) CHECK(std::abs(y[n] - y[n - 1]) < 0.02);
    CHECK(h.channels("out") == 2);
    CHECK(h.output("out", 1)[3000] == 0.0);  // not started yet
    CHECK(y[3000] != 0.0);
}
