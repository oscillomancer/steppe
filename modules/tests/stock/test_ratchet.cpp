// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

// Gates 100 frames long every `period` frames.
ModuleHarness::Signal clockOf(int period) {
    return [period](int, std::int64_t n) { return (n % period) < 100 ? 1.0 : 0.0; };
}

std::vector<std::int64_t> rises(const std::vector<double>& y, std::int64_t from, std::int64_t to) {
    std::vector<std::int64_t> at;
    for (std::int64_t n = from; n < to; ++n)
        if (y[static_cast<std::size_t>(n)] >= 0.5 &&
            (n == 0 || y[static_cast<std::size_t>(n - 1)] < 0.5))
            at.push_back(n);
    return at;
}

}  // namespace

TEST_CASE("ratchet: a gate becomes Count even pulses over the time to the next") {
    ModuleHarness h(supple::rack::test::stock("ratchet"));
    h.set("count", 4);
    h.input("gate", 1, clockOf(4800));
    h.run(4800 * 3);
    const auto& y = h.output("out");
    // The second gate knows the period: pulses 1200 apart, 600 long (the
    // first a frame late, rising again out of the first burst's last pulse).
    CHECK(rises(y, 4800, 9600) == std::vector<std::int64_t>{4801, 6000, 7200, 8400});
    CHECK(y[6000 + 599] == 1.0);
    CHECK(y[6000 + 600] == 0.0);
    CHECK(h.output("level")[7300] == 1.0);
}

TEST_CASE("ratchet: Every and Chance choose the gates; the rest pass as they came") {
    SUBCASE("every second gate") {
        ModuleHarness h(supple::rack::test::stock("ratchet"));
        h.set("count", 2);
        h.set("every", 2);
        h.input("gate", 1, clockOf(4800));
        h.run(4800 * 4);
        const auto& y = h.output("out");
        CHECK(rises(y, 4800, 9600).size() == 1);  // the second gate passes
        CHECK(y[4800 + 99] == 1.0);               // as long as it came
        CHECK(y[4800 + 100] == 0.0);
        CHECK(rises(y, 9600, 14400).size() == 2);  // the third bursts
    }
    SUBCASE("no chance: every gate passes") {
        ModuleHarness h(supple::rack::test::stock("ratchet"));
        h.set("chance", 0);
        h.input("gate", 1, clockOf(4800));
        h.run(4800 * 4);
        const auto& y = h.output("out");
        const auto& g = h.output("level");
        for (std::size_t n = 0; n < y.size(); ++n) CHECK(y[n] == ((n % 4800) < 100 ? 1.0 : 0.0));
        CHECK(g[200] == 1.0);
    }
}

TEST_CASE("ratchet: Curve speeds the pulses up; Decay fades their level") {
    ModuleHarness h(supple::rack::test::stock("ratchet"));
    h.set("count", 4);
    h.set("curve", 80);
    h.set("decay", 50);
    h.input("gate", 1, clockOf(4800));
    h.run(4800 * 2);
    const auto r = rises(h.output("out"), 4800, 9600);
    REQUIRE(r.size() == 4);
    CHECK(r[1] - r[0] > r[2] - r[1]);
    CHECK(r[2] - r[1] > r[3] - r[2]);
    const auto& g = h.output("level");
    for (int k = 0; k < 4; ++k)
        CHECK(g[static_cast<std::size_t>(r[static_cast<std::size_t>(k)])] ==
              doctest::Approx(1.0 / (1 << k)));
}

TEST_CASE("ratchet: poly channels toss their own coins; Reset repeats them") {
    ModuleHarness h(supple::rack::test::stock("ratchet"));
    h.set("chance", 50);
    h.set("count", 2);
    h.input("gate", 4, clockOf(2400));
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 48000 && n < 48050 ? 1.0 : 0.0; });
    h.run(96000);
    CHECK(h.channels("out") == 4);
    bool differ = false;
    for (int c = 1; c < 4; ++c)
        for (std::size_t n = 0; n < 48000; ++n)
            if (h.output("out", c)[n] != h.output("out", 0)[n]) differ = true;
    CHECK(differ);
    // After the reset the same tosses again (the period is known by then, so
    // compare from the second gate on).
    for (int c = 0; c < 4; ++c)
        for (std::size_t n = 2400; n < 48000; n += 50)
            CHECK(h.output("out", c)[48000 + n] == h.output("out", c)[n]);
}
