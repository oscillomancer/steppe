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

constexpr int kBar = 96000;  // 4/4 at 120 beats a minute

std::vector<std::int64_t> rises(const std::vector<double>& y, std::int64_t to) {
    std::vector<std::int64_t> at;
    for (std::int64_t n = 0; n < to; ++n)
        if (y[static_cast<std::size_t>(n)] >= 0.5 &&
            (n == 0 || y[static_cast<std::size_t>(n - 1)] < 0.5))
            at.push_back(n);
    return at;
}

bool near(const std::vector<std::int64_t>& got, const std::vector<std::int64_t>& want) {
    if (got.size() != want.size()) return false;
    for (std::size_t k = 0; k < got.size(); ++k)
        if (std::llabs(got[k] - want[k]) > 1) return false;
    return true;
}

}  // namespace

TEST_CASE("polyrhythm: 4 against 3 against 5 against 7 in a bar of the song") {
    ModuleHarness h(supple::rack::test::stock("polyrhythm"));
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
    h.run(kBar * 2);
    CHECK(near(rises(h.output("gate_1"), kBar), {0, 24000, 48000, 72000}));
    CHECK(near(rises(h.output("gate_2"), kBar), {0, 32000, 64000}));
    CHECK(near(rises(h.output("gate_4"), kBar), {0, 13714, 27429, 41143, 54857, 68571, 82286}));
    // Each lane meets again on the next downbeat.
    for (int k = 1; k <= 4; ++k) CHECK(h.output("gate_" + std::to_string(k))[kBar + 10] == 1.0);
    // Ramps rise over each beat; gates are high for Width (25 %).
    CHECK(h.output("ramp_1")[12000] == doctest::Approx(0.5).epsilon(0.001));
    CHECK(h.output("ramp_2")[16000] == doctest::Approx(0.5).epsilon(0.001));
    CHECK(h.output("gate_1")[5900] == 1.0);
    CHECK(h.output("gate_1")[6100] == 0.0);
}

TEST_CASE("polyrhythm: Any fires at every start; Hits counts the lanes that meet") {
    ModuleHarness h(supple::rack::test::stock("polyrhythm"));
    h.set("count_1", 4);
    h.set("count_2", 2);
    h.set("count_3", 1);
    h.set("count_4", 3);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
    h.run(kBar);
    // Starts at 0 (all four), 24000 (1), 32000 (4), 48000 (1 and 2),
    // 64000 (4), 72000 (1).
    CHECK(near(rises(h.output("any"), kBar), {0, 24000, 32000, 48000, 64000, 72000}));
    const auto& hits = h.output("hits");
    CHECK(hits[100] == 1.0);
    CHECK(hits[24100] == 0.25);
    CHECK(hits[48100] == 0.5);
    CHECK(h.output("any")[47] == 1.0);  // 1 ms
    CHECK(h.output("any")[48] == 0.0);
}

TEST_CASE("polyrhythm: stopped, it runs at the tempo; Reset starts the span over") {
    ModuleHarness h(supple::rack::test::stock("polyrhythm"));
    h.set("span", 0);  // a beat: 24000 frames
    h.set("count_1", 2);
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 30000 && n < 30100 ? 1.0 : 0.0; });
    h.run(60000);
    CHECK(near(rises(h.output("gate_1"), 60000), {0, 12000, 24000, 30000, 42000, 54000}));
}
