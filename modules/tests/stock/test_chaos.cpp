// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

int crossings(const std::vector<double>& v) {
    int n = 0;
    for (std::size_t k = 1; k < v.size(); ++k) n += (v[k] >= 0) != (v[k - 1] >= 0);
    return n;
}

}  // namespace

TEST_CASE("chaos: both attractors move within -1..1, flipping sides") {
    for (int a = 0; a < 2; ++a) {
        CAPTURE(a);
        ModuleHarness h(supple::rack::test::stock("chaos"));
        h.set("attractor", a);
        h.set("rate", 2);
        h.run(48000 * 10);
        for (const char* key : {"x", "y", "z"}) {
            CAPTURE(key);
            double lo = 1, hi = -1;
            for (double v : h.output(key)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            CHECK(lo >= -1.0);
            CHECK(hi <= 1.0);
            CHECK(hi - lo > 0.5);
        }
        CHECK(crossings(h.output("x")) > 4);
    }
}

TEST_CASE("chaos: rate speeds it; voices drift apart; reset starts over") {
    ModuleHarness slow(supple::rack::test::stock("chaos")),
        fast(supple::rack::test::stock("chaos"));
    slow.set("rate", 0.5);
    fast.set("rate", 4);
    slow.run(48000 * 8);
    fast.run(48000 * 8);
    CHECK(crossings(fast.output("x")) > 3 * crossings(slow.output("x")));

    ModuleHarness h(supple::rack::test::stock("chaos"));
    h.set("voices", 2);
    h.set("rate", 2);
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 480000 && n < 480048 ? 1.0 : 0.0; });
    h.run(48000 * 20);
    CHECK(h.channels("x") == 2);
    CHECK(std::abs(h.output("x", 0)[100] - h.output("x", 1)[100]) < 0.01);
    double apart = 0;
    for (int n = 300000; n < 480000; ++n)
        apart = std::max(apart, std::abs(h.output("x", 0)[n] - h.output("x", 1)[n]));
    CHECK(apart > 0.5);
    for (int n = 0; n < 48000; n += 997)
        CHECK(h.output("y", 0)[480000 + n] == doctest::Approx(h.output("y", 0)[n]).epsilon(1e-9));
}
