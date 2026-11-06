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

constexpr int kPeriod = 4800;  // a clock every 100 ms
const int kPattern[16] = {0, 12, 7, 0, 3, 12, 10, 7, 0, 12, 7, 0, 5, 3, 10, 12};

ModuleHarness::Signal clockEvery(int period) {
    return [period](int, std::int64_t n) { return n % period < 100 ? 1.0 : 0.0; };
}

// The step's pitch in semitones, read a little after clock k.
double semitonesAt(const ModuleHarness& h, int k) {
    return h.output("pitch")[static_cast<std::size_t>(k * kPeriod + 10)] * 12.0;
}

}  // namespace

TEST_CASE("sequencer: each clock plays the next step of the pattern") {
    ModuleHarness h(supple::rack::test::stock("sequencer"));
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(20 * kPeriod);
    for (int k = 0; k < 20; ++k) {
        CAPTURE(k);
        CHECK(semitonesAt(h, k) == doctest::Approx(kPattern[k % 16]));
    }
    // Half a step long, from the second clock on (the first has no period yet).
    const auto& g = h.output("gate");
    CHECK(g[5 * kPeriod + 10] == 1.0);
    CHECK(g[5 * kPeriod + kPeriod / 2 - 10] == 1.0);
    CHECK(g[5 * kPeriod + kPeriod / 2 + 10] == 0.0);
    // The end trigger as the pattern comes round.
    CHECK(h.output("end")[16 * kPeriod + 10] == 1.0);
    CHECK(h.output("end")[15 * kPeriod + 10] == 0.0);
    CHECK(h.output("step")[3 * kPeriod + 10] == 1.0);
}

TEST_CASE("sequencer: steps and directions") {
    auto steps = [](int direction, int count) {
        ModuleHarness h(supple::rack::test::stock("sequencer"));
        h.set("steps", 4);
        h.set("direction", direction);
        for (int k = 1; k <= 4; ++k) h.set("pitch_" + std::to_string(k), k);
        h.input("clock", 1, clockEvery(kPeriod));
        h.run(static_cast<std::int64_t>(count) * kPeriod);
        std::vector<int> out;
        for (int k = 0; k < count; ++k)
            out.push_back(static_cast<int>(std::lround(semitonesAt(h, k))));
        return out;
    };
    CHECK(steps(0, 6) == std::vector<int>{1, 2, 3, 4, 1, 2});
    CHECK(steps(1, 6) == std::vector<int>{4, 3, 2, 1, 4, 3});
    CHECK(steps(2, 8) == std::vector<int>{1, 2, 3, 4, 3, 2, 1, 2});
    // Random and drunk stay in the pattern; drunk moves one step at a time.
    for (int s : steps(3, 32)) CHECK((s >= 1 && s <= 4));
    const std::vector<int> drunk = steps(4, 32);
    for (std::size_t k = 1; k < drunk.size(); ++k) {
        const int d = std::abs(drunk[k] - drunk[k - 1]);
        CHECK((d == 1 || d == 3));
    }
}

TEST_CASE("sequencer: a reset makes the next clock play the first step, the same again") {
    ModuleHarness h(supple::rack::test::stock("sequencer"));
    h.set("direction", 3);  // random: from the count, so it repeats after the reset
    h.input("clock", 1, clockEvery(kPeriod));
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 10 * kPeriod - 2000 && n < 10 * kPeriod - 1900 ? 1.0 : 0.0;
    });
    h.run(20 * kPeriod);
    for (int k = 0; k < 10; ++k) {
        CAPTURE(k);
        CHECK(semitonesAt(h, 10 + k) == doctest::Approx(semitonesAt(h, k)));
    }
}

TEST_CASE("sequencer: with no clock it follows the song's sixteenths") {
    ModuleHarness h(supple::rack::test::stock("sequencer"));
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 6000 frames a sixteenth
    t.beats = 1.0;  // the fifth sixteenth
    h.transport(t);
    h.run(6000 * 3);
    CHECK(h.output("pitch")[10] * 12.0 == doctest::Approx(kPattern[4]));
    CHECK(h.output("pitch")[6010] * 12.0 == doctest::Approx(kPattern[5]));
    CHECK(h.output("pitch")[12010] * 12.0 == doctest::Approx(kPattern[6]));
}

TEST_CASE("sequencer: ties hold the gate, chance 0 and gate off rest") {
    ModuleHarness h(supple::rack::test::stock("sequencer"));
    h.set("length_1", 99);   // still high at the next clock
    h.set("length_2", 100);  // tied into step 3
    h.set("gate_4", 0);
    h.set("chance_5", 0);
    h.input("clock", 1, clockEvery(kPeriod));
    h.run(6 * kPeriod);
    const auto& g = h.output("gate");
    // Step 1 is still high: it retriggers into step 2, one frame low at the clock.
    CHECK(g[kPeriod] == 0.0);
    CHECK(g[kPeriod + 1] == 1.0);
    // Steps 2 and 3 are one gate.
    for (int n = kPeriod + 1; n < 2 * kPeriod + kPeriod / 2 - 10; ++n) REQUIRE(g[n] == 1.0);
    CHECK(g[3 * kPeriod + 100] == 0.0);  // gate off
    CHECK(g[4 * kPeriod + 100] == 0.0);  // chance 0
    CHECK(g[5 * kPeriod + 100] == 1.0);
    // A resting step keeps the last note's pitch, for its release.
    CHECK(h.output("pitch")[3 * kPeriod + 100] * 12.0 == doctest::Approx(kPattern[2]));
}

TEST_CASE("sequencer: a poly transpose plays the pattern in every voice") {
    ModuleHarness h(supple::rack::test::stock("sequencer"));
    h.input("clock", 1, clockEvery(kPeriod));
    h.input("transpose", 3, [](int c, std::int64_t) { return c * 4 / 12.0; });
    h.run(2 * kPeriod);
    CHECK(h.channels("pitch") == 3);
    CHECK(h.channels("gate") == 1);
    CHECK(h.output("pitch", 2)[kPeriod + 10] * 12.0 == doctest::Approx(kPattern[1] + 8));
}
