// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

std::vector<std::int64_t> rises(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 0; n < y.size(); ++n)
        if (y[n] >= 0.5 && (n == 0 || y[n - 1] < 0.5)) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

}  // namespace

TEST_CASE("bounce: each flight keeps Bounce of the last, each hit softer") {
    ModuleHarness h(supple::rack::test::stock("bounce"));
    h.run(48000 * 4);  // 48 kHz: the first fall is 24000 frames
    const auto hits = rises(h.output("hit"));
    REQUIRE(hits.size() > 10);
    // 0.5 s down, then flights of 0.75 s, 0.5625 s, 0.421875 s ...
    const std::vector<double> want = {24000, 60000, 87000, 107250, 122437.5};
    for (std::size_t k = 0; k < want.size(); ++k)
        CHECK(std::llabs(hits[k] - static_cast<std::int64_t>(want[k])) <= 1);
    const auto& s = h.output("strength");
    CHECK(s[24100] == doctest::Approx(1.0));
    CHECK(s[60100] == doctest::Approx(0.75));
    CHECK(s[87100] == doctest::Approx(0.5625));
    // The arc after the first hit peaks at 0.75 squared, 0.375 s on.
    const auto& height = h.output("height");
    CHECK(height[0] == doctest::Approx(1.0).epsilon(0.001));
    CHECK(height[24000 + 18000] == doctest::Approx(0.5625).epsilon(0.001));
    // It settles at its last hit, short of 0.5 s x (1 + 2 x 0.75 / 0.25) =
    // 3.5 s, and Loop drops it again: the next hit is a fall later.
    const auto ends = rises(h.output("end"));
    REQUIRE(ends.size() == 1);
    CHECK(ends[0] > 167000);
    CHECK(ends[0] < 168000);
    bool next = false;
    for (auto at : hits)
        if (std::llabs(at - (ends[0] + 24000)) <= 1) next = true;
    CHECK(next);
    CHECK(h.output("air")[ends[0] - 10] == 1.0);
}

TEST_CASE("bounce: with Loop off it waits for Drop; poly balls fall apart") {
    ModuleHarness h(supple::rack::test::stock("bounce"));
    h.set("loop", 0);
    h.set("fall", 100);  // 4800 frames
    h.set("bounce", 50);
    h.input("drop", 2, [](int c, std::int64_t n) {
        const std::int64_t at = c == 0 ? 1000 : 3000;
        return n >= at && n < at + 100 ? 1.0 : 0.0;
    });
    h.run(48000);
    CHECK(h.channels("hit") == 2);
    const auto a = rises(h.output("hit", 0));
    const auto b = rises(h.output("hit", 1));
    REQUIRE(a.size() >= 3);
    REQUIRE(b.size() >= 3);
    CHECK(std::llabs(a[0] - 5800) <= 1);
    CHECK(std::llabs(a[1] - 10600) <= 1);  // 2 x 0.5 x 4800 later
    CHECK(std::llabs(b[0] - 7800) <= 1);
    CHECK(rises(h.output("end", 0)).size() == 1);
    CHECK(h.output("air", 0)[500] == 0.0);    // nothing until Drop
    CHECK(h.output("air", 0)[47000] == 0.0);  // and settled, not dropped again
}

TEST_CASE("bounce: Rough varies the bounces; Reset brings back the same floor") {
    ModuleHarness h(supple::rack::test::stock("bounce"));
    h.set("rough", 80);
    h.set("fall", 100);  // 4800 frames
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 48000 && n < 48100 ? 1.0 : 0.0; });
    h.run(96000);
    const auto hits = rises(h.output("hit"));
    std::vector<std::int64_t> before;
    std::vector<std::int64_t> after;
    for (auto at : hits) {
        if (at < 48000)
            before.push_back(at);
        else
            after.push_back(at - 48000);
    }
    REQUIRE(before.size() >= 8);
    REQUIRE(after.size() >= 8);
    for (std::size_t k = 0; k < 8; ++k) CHECK(std::llabs(before[k] - after[k]) <= 1);
    // Not the smooth floor's times: 4800, then 0.75 x 9600 later.
    CHECK(std::llabs(before[1] - before[0] - 7200) > 50);
}
