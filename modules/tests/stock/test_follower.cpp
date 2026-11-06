// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

// A 1 kHz sine at `amplitude` from 0.5 s to 1 s, silence around it.
ModuleHarness::Signal burst(double amplitude) {
    return [amplitude](int, std::int64_t n) {
        return n >= 24000 && n < 48000 ? amplitude * std::sin(n * 2 * M_PI * 1000 / 48000) : 0.0;
    };
}

}  // namespace

TEST_CASE("follower: the level of a sine, peak and RMS") {
    ModuleHarness h(supple::rack::test::stock("follower"));
    h.input("in", 1, burst(0.5));
    h.run(72000);
    CHECK(h.output("envelope")[40000] == doctest::Approx(0.5).epsilon(0.05));
    CHECK(h.output("envelope")[23000] == 0.0);
    CHECK(h.output("envelope")[71000] < 0.02);  // released: 150 ms a time constant

    ModuleHarness r(supple::rack::test::stock("follower"));
    r.set("detector", 1);
    r.set("attack", 30);
    r.set("release", 300);
    r.input("in", 1, burst(0.5));
    r.run(48000);
    CHECK(r.output("envelope")[47000] == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.03));
}

TEST_CASE("follower: a gate above the threshold, an onset as it opens") {
    ModuleHarness h(supple::rack::test::stock("follower"));
    h.set("threshold", -12);
    h.input("in", 1, burst(0.5));  // -6 dB
    h.run(72000);
    const auto& g = h.output("gate");
    CHECK(g[23000] == 0.0);
    CHECK(g[24500] == 1.0);
    CHECK(g[47000] == 1.0);
    CHECK(g[70000] == 0.0);
    int onsets = 0;
    const auto& o = h.output("onset");
    for (std::size_t n = 1; n < o.size(); ++n) onsets += o[n] > 0.5 && o[n - 1] < 0.5;
    CHECK(onsets == 1);

    ModuleHarness q(supple::rack::test::stock("follower"));
    q.set("threshold", -3);
    q.input("in", 1, burst(0.5));
    q.run(72000);
    for (double x : q.output("gate")) REQUIRE(x == 0.0);
}
