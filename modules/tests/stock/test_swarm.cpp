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

int rises(const std::vector<double>& y, std::size_t from = 0) {
    int n = 0;
    for (std::size_t k = std::max<std::size_t>(from, 1); k < y.size(); ++k)
        if (y[k] >= 0.5 && y[k - 1] < 0.5) ++n;
    return n;
}

}  // namespace

TEST_CASE("swarm: uncoupled, each voice keeps its own rate across Spread") {
    ModuleHarness h(supple::rack::test::stock("swarm"));
    h.set("coupling", 0);
    h.set("rate", 2);
    h.set("spread", 100);  // half an octave either side: 1.41, 1.78, 2.24 and 2.83 Hz
    h.run(48000 * 10);
    CHECK(h.channels("out") == 4);
    const double want[] = {14.14, 17.82, 22.45, 28.28};
    for (int k = 0; k < 4; ++k) CHECK(std::abs(rises(h.output("trigger", k)) - want[k]) <= 1.0);
    // Out is each phase's sine.
    const auto& ph = h.output("phase", 2);
    const auto& out = h.output("out", 2);
    CHECK(out[12345] == doctest::Approx(std::sin(2.0 * 3.141592653589793 * ph[12345])));
}

TEST_CASE("swarm: coupled they lock in step; pushed apart they share the cycle") {
    ModuleHarness locked(supple::rack::test::stock("swarm"));
    locked.set("coupling", 80);
    locked.set("rate", 2);
    locked.run(48000 * 10);
    const auto& sync = locked.output("sync");
    CHECK(sync[48000 * 9] > 0.97);
    // In step they share one rate: every voice the same number of cycles.
    std::vector<int> counts;
    for (int k = 0; k < 4; ++k) counts.push_back(rises(locked.output("trigger", k), 48000 * 5));
    for (int k = 1; k < 4; ++k) CHECK(std::abs(counts[k] - counts[0]) <= 1);
    // Mean swings nearly a full wave when they are together.
    double top = 0.0;
    const auto& mean = locked.output("mean");
    for (std::size_t n = 48000 * 9; n < mean.size(); ++n) top = std::max(top, mean[n]);
    CHECK(top > 0.95);

    ModuleHarness apart(supple::rack::test::stock("swarm"));
    apart.set("coupling", -80);
    apart.set("spread", 0);
    apart.set("rate", 2);
    apart.run(48000 * 10);
    CHECK(apart.output("sync")[48000 * 9] < 0.05);
}

TEST_CASE("swarm: Noise jostles the same way each time; Reset starts the swarm over") {
    auto run = [](bool withReset) {
        ModuleHarness h(supple::rack::test::stock("swarm"));
        h.set("noise", 60);
        h.set("coupling", 40);
        if (withReset)
            h.input("reset", 1,
                    [](int, std::int64_t n) { return n >= 48000 && n < 48100 ? 1.0 : 0.0; });
        h.run(96000);
        return h.output("out", 1);
    };
    const auto a = run(false);
    const auto b = run(false);
    const auto r = run(true);
    CHECK(a[90000] == b[90000]);
    // After the reset (its edge at frame 48000) the swarm replays its start.
    for (std::size_t n : {std::size_t{5000}, std::size_t{20000}, std::size_t{40000}})
        CHECK(r[48000 + n] == doctest::Approx(a[n]));
}

TEST_CASE("swarm: Scatter throws the phases anew") {
    ModuleHarness h(supple::rack::test::stock("swarm"));
    h.set("coupling", 80);
    h.input("scatter", 1,
            [](int, std::int64_t n) { return n >= 240000 && n < 240100 ? 1.0 : 0.0; });
    h.run(250000);
    const auto& sync = h.output("sync");
    CHECK(sync[239000] > 0.97);
    CHECK(sync[240010] < 0.9);
}
