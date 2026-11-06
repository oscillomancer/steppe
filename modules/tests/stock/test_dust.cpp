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
    for (std::size_t n = 0; n < y.size(); ++n)
        if (y[n] >= 0.5 && (n == 0 || y[n - 1] < 0.5)) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

// The spread of the gaps between events: about 1 for evenly random ones,
// more when they bunch.
double variation(const std::vector<std::int64_t>& at) {
    std::vector<double> gaps;
    for (std::size_t k = 1; k < at.size(); ++k)
        gaps.push_back(static_cast<double>(at[k] - at[k - 1]));
    double mean = 0.0;
    for (double g : gaps) mean += g;
    mean /= static_cast<double>(gaps.size());
    double var = 0.0;
    for (double g : gaps) var += (g - mean) * (g - mean);
    var /= static_cast<double>(gaps.size());
    return std::sqrt(var) / mean;
}

}  // namespace

TEST_CASE("dust: Density events a second, at random moments") {
    ModuleHarness h(supple::rack::test::stock("dust"));
    h.set("density", 10);
    h.run(48000 * 20);
    const auto at = rises(h.output("trigger"));
    CHECK(at.size() > 160);  // 200 expected
    CHECK(at.size() < 240);
    CHECK(variation(at) > 0.8);
    CHECK(variation(at) < 1.2);
    // A 1 ms trigger; Velocity between 1 - Vary and 1, Value within ±1, held.
    CHECK(h.output("trigger")[static_cast<std::size_t>(at[3] + 47)] == 1.0);
    CHECK(h.output("trigger")[static_cast<std::size_t>(at[3] + 48)] == 0.0);
    const auto& v = h.output("velocity");
    const auto& x = h.output("value");
    for (std::size_t k = 1; k + 1 < at.size(); ++k) {
        const auto mid = static_cast<std::size_t>((at[k] + at[k + 1]) / 2);
        CHECK(v[mid] >= 0.5);
        CHECK(v[mid] <= 1.0);
        CHECK(std::abs(x[mid]) <= 1.0);
        CHECK(x[mid] == x[static_cast<std::size_t>(at[k])]);
    }
}

TEST_CASE("dust: Cluster bunches the events into bursts") {
    ModuleHarness h(supple::rack::test::stock("dust"));
    h.set("density", 3);
    h.set("cluster", 90);
    h.run(48000 * 60);
    const auto at = rises(h.output("trigger"));
    // Each event brings 0.81 more on average: about 3 / 0.19 a second.
    CHECK(at.size() > 60 * 3 * 2);
    CHECK(variation(at) > 1.5);
}

TEST_CASE("dust: Grid holds each event to the song's next sixteenth") {
    ModuleHarness h(supple::rack::test::stock("dust"));
    h.set("density", 6);
    h.set("grid", 2);  // 1/16: 6000 frames at 120 beats a minute
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
    h.run(48000 * 10);
    const auto at = rises(h.output("trigger"));
    CHECK(at.size() > 30);
    for (auto n : at) CHECK(n % 6000 == 0);
}

TEST_CASE("dust: Reset starts the same events over; a stream keeps its rain as Voices grows") {
    ModuleHarness h(supple::rack::test::stock("dust"));
    h.set("density", 8);
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 96000 && n < 96100 ? 1.0 : 0.0; });
    h.run(48000 * 4);
    std::vector<std::int64_t> before, after;
    for (auto n : rises(h.output("trigger"))) {
        if (n < 96000)
            before.push_back(n);
        else
            after.push_back(n - 96000);
    }
    REQUIRE(before.size() > 5);
    REQUIRE(after.size() >= before.size());
    for (std::size_t k = 0; k < before.size(); ++k) CHECK(before[k] == after[k]);

    ModuleHarness one(supple::rack::test::stock("dust"));
    ModuleHarness four(supple::rack::test::stock("dust"));
    four.set("voices", 4);
    one.run(48000 * 2);
    four.run(48000 * 2);
    CHECK(four.channels("trigger") == 4);
    CHECK(rises(one.output("trigger")) == rises(four.output("trigger", 0)));
    CHECK(rises(four.output("trigger", 1)) != rises(four.output("trigger", 0)));
}
