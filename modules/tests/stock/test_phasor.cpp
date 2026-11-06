// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

TEST_CASE("phasor: a 0..1 ramp at the pitch, a trigger at each cycle") {
    ModuleHarness h(stock("phasor"));
    h.input("pitch", 1, [](int, std::int64_t) { return octaveOf(100); });
    h.run(48000);
    const auto& p = h.output("phase");
    CHECK(*std::min_element(p.begin(), p.end()) >= 0.0);
    CHECK(*std::max_element(p.begin(), p.end()) < 1.0);
    CHECK(p[240] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(mean(p) == doctest::Approx(0.5).epsilon(0.01));
    int rises = 0;
    const auto& w = h.output("wrap");
    for (std::size_t n = 1; n < w.size(); ++n) rises += w[n] > 0.5 && w[n - 1] < 0.5;
    CHECK(rises == doctest::Approx(100).epsilon(0.02));
    std::size_t high = 0;
    for (std::size_t n = 0; n < 480; ++n) high += w[n + 470] > 0.5;
    CHECK(high == 48);  // 1 ms
}

TEST_CASE("phasor: sync restarts, skew bends, offset shifts") {
    ModuleHarness h(stock("phasor"));
    h.input("pitch", 1, [](int, std::int64_t) { return octaveOf(100); });
    h.input("sync", 1, [](int, std::int64_t n) { return n >= 1000 && n < 1048 ? 1.0 : 0.0; });
    h.run(2000);
    CHECK(h.output("phase")[1000] == 0.0);
    CHECK(h.output("phase")[1001] == doctest::Approx(100.0 / 48000));

    ModuleHarness s(stock("phasor"));
    s.input("pitch", 1, [](int, std::int64_t) { return octaveOf(100); });
    s.set("skew", 50);
    s.run(48000);
    CHECK(mean(s.output("phase")) > 0.6);

    ModuleHarness o(stock("phasor"));
    o.input("pitch", 1, [](int, std::int64_t) { return octaveOf(100); });
    o.set("offset", 25);
    o.run(480);
    CHECK(o.output("phase")[0] == doctest::Approx(0.25 + 100.0 / 48000));
}
