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

constexpr int kPeriod = 2400;

// Holds C, E and G (on channels in the order G, C, E, a block apart) and
// returns `count` notes in semitones.
std::vector<int> arp(ModuleHarness& h, int count, std::int64_t release = 1 << 30) {
    const double notes[3] = {7, 0, 4};
    h.input("pitch", 3, [notes](int c, std::int64_t) { return notes[c] / 12.0; });
    h.input("gate", 3,
            [release](int c, std::int64_t n) { return n >= 32 * c && n < release ? 1.0 : 0.0; });
    h.input("clock", 1,
            [](int, std::int64_t n) { return n >= 200 && (n - 200) % kPeriod < 100 ? 1.0 : 0.0; });
    h.run(200 + static_cast<std::int64_t>(count) * kPeriod);
    std::vector<int> v;
    for (int k = 0; k < count; ++k)
        v.push_back(static_cast<int>(
            std::lround(h.output("out")[static_cast<std::size_t>(200 + k * kPeriod + 10)] * 12.0)));
    return v;
}

}  // namespace

TEST_CASE("arp: the orders") {
    auto with = [](int mode, int octaves, int count) {
        ModuleHarness h(supple::rack::test::stock("arp"));
        h.set("mode", mode);
        h.set("octaves", octaves);
        return arp(h, count);
    };
    CHECK(with(0, 1, 6) == std::vector<int>{0, 4, 7, 0, 4, 7});
    CHECK(with(1, 1, 4) == std::vector<int>{7, 4, 0, 7});
    CHECK(with(2, 1, 6) == std::vector<int>{0, 4, 7, 4, 0, 4});
    CHECK(with(3, 1, 4) == std::vector<int>{7, 0, 4, 7});
    CHECK(with(5, 1, 4) == std::vector<int>{0, 7, 4, 0});
    CHECK(with(0, 2, 7) == std::vector<int>{0, 4, 7, 12, 16, 19, 0});
    for (int x : with(4, 2, 32))
        CHECK((x == 0 || x == 4 || x == 7 || x == 12 || x == 16 || x == 19));
}

TEST_CASE("arp: gates by length; released keys stop it unless latched") {
    ModuleHarness h(supple::rack::test::stock("arp"));
    arp(h, 8, 200 + 4 * kPeriod);
    const auto& g = h.output("out_gate");
    CHECK(g[200 + 2 * kPeriod + 10] == 1.0);
    CHECK(g[200 + 2 * kPeriod + kPeriod / 2 + 10] == 0.0);
    CHECK(g[200 + 6 * kPeriod + 10] == 0.0);  // keys up

    ModuleHarness l(supple::rack::test::stock("arp"));
    l.set("latch", 1);
    const auto v = arp(l, 8, 200 + 4 * kPeriod);
    CHECK(l.output("out_gate")[200 + 6 * kPeriod + 10] == 1.0);
    CHECK(v[6] == 0);
}
