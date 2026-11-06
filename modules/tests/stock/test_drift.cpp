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

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    double ma = 0, mb = 0;
    for (std::size_t n = 0; n < a.size(); ++n) {
        ma += a[n];
        mb += b[n];
    }
    ma /= static_cast<double>(a.size());
    mb /= static_cast<double>(b.size());
    double ab = 0, aa = 0, bb = 0;
    for (std::size_t n = 0; n < a.size(); ++n) {
        ab += (a[n] - ma) * (b[n] - mb);
        aa += (a[n] - ma) * (a[n] - ma);
        bb += (b[n] - mb) * (b[n] - mb);
    }
    return ab / std::sqrt(aa * bb);
}

int crossings(const std::vector<double>& y) {
    int k = 0;
    for (std::size_t n = 1; n < y.size(); ++n)
        if ((y[n - 1] < 0.0) != (y[n] < 0.0)) ++k;
    return k;
}

}  // namespace

TEST_CASE("drift: smooth, bounded motion, a channel a voice") {
    ModuleHarness h(supple::rack::test::stock("drift"));
    h.set("rate", 2);
    h.run(48000 * 4);
    CHECK(h.channels("out") == 4);
    const auto& y = h.output("out", 0);
    double biggestStep = 0.0;
    double lo = 1.0, hi = -1.0;
    for (std::size_t n = 1; n < y.size(); ++n) {
        biggestStep = std::max(biggestStep, std::abs(y[n] - y[n - 1]));
        lo = std::min(lo, y[n]);
        hi = std::max(hi, y[n]);
    }
    CHECK(biggestStep < 0.002);  // no steps
    CHECK(lo >= -1.0);
    CHECK(hi <= 1.0);
    CHECK(hi - lo > 0.5);  // it does move
}

TEST_CASE("drift: Together from apart to as one") {
    ModuleHarness apart(supple::rack::test::stock("drift"));
    apart.set("rate", 2);
    apart.set("together", 0);
    apart.set("spread", 0);
    apart.run(48000 * 10);
    CHECK(std::abs(correlation(apart.output("out", 0), apart.output("out", 1))) < 0.5);

    ModuleHarness one(supple::rack::test::stock("drift"));
    one.set("rate", 2);
    one.set("together", 100);
    one.run(48000);
    for (int c = 1; c < 4; ++c)
        CHECK(one.output("out", c)[30000] == doctest::Approx(one.output("out", 0)[30000]));
    // The shared path at full depth is the voices' own at 100 %.
    CHECK(one.output("shared")[30000] == doctest::Approx(one.output("out", 0)[30000]));
}

TEST_CASE("drift: Rate sets the pace") {
    ModuleHarness slow(supple::rack::test::stock("drift"));
    slow.set("rate", 0.5);
    slow.set("together", 0);
    slow.set("spread", 0);
    slow.run(48000 * 20);
    ModuleHarness fast(supple::rack::test::stock("drift"));
    fast.set("rate", 4);
    fast.set("together", 0);
    fast.set("spread", 0);
    fast.run(48000 * 20);
    CHECK(crossings(fast.output("out", 0)) > 3 * crossings(slow.output("out", 0)));
}

TEST_CASE("drift: Reset starts the same motion over; unipolar sits in 0..1") {
    ModuleHarness h(supple::rack::test::stock("drift"));
    h.set("polarity", 1);
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 24000 && n < 24100 ? 1.0 : 0.0; });
    h.run(48000);
    const auto& y = h.output("out", 2);
    for (int k = 0; k < 20000; k += 500) CHECK(y[24000 + k] == doctest::Approx(y[k]));
    for (double v : y) {
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
    }
}
