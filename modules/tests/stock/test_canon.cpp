// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kStep = 1000;  // the clock: high for the first half of each step

void lead(ModuleHarness& h, bool everyStep) {
    h.input("clock", 1, [](int, std::int64_t n) { return n % kStep < kStep / 2 ? 1.0 : 0.0; });
    // Step k's note is k semitones above C4.
    h.input("in", 1, [](int, std::int64_t n) { return static_cast<double>(n / kStep) / 12.0; });
    // The lead's gate: the first 300 frames of a step.
    h.input("in_gate", 1, [everyStep](int, std::int64_t n) {
        return n % kStep < 300 && (everyStep || (n / kStep) % 2 == 0) ? 1.0 : 0.0;
    });
}

// Whether a follower's gate sounds in step k, read mid-step.
bool sounds(const std::vector<double>& g, int k) { return g[k * kStep + 100] == 1.0; }

}  // namespace

TEST_CASE("canon: each follower plays the lead Delay steps later, Shift away") {
    ModuleHarness h(supple::rack::test::stock("canon"));
    lead(h, true);
    h.run(kStep * 40);
    const auto& p1 = h.output("pitch_1");
    const auto& p2 = h.output("pitch_2");
    const auto& p3 = h.output("pitch_3");
    for (int k : {12, 20, 33}) {
        const std::size_t at = static_cast<std::size_t>(k * kStep + 700);
        CHECK(p1[at] == doctest::Approx((k - 4) / 12.0));        // 4 steps, unison
        CHECK(p2[at] == doctest::Approx((k - 8 + 7) / 12.0));    // 8 steps, a fifth up
        CHECK(p3[at] == doctest::Approx((k - 12 + 12) / 12.0));  // 12 steps, an octave up
    }
    const auto& g1 = h.output("gate_1");
    CHECK_FALSE(sounds(g1, 3));  // nothing yet four steps back
    CHECK(sounds(g1, 4));
    // As long as the lead's gate was: 300 frames.
    CHECK(g1[4 * kStep + 299] == 1.0);
    CHECK(g1[4 * kStep + 300] == 0.0);
    CHECK_FALSE(sounds(h.output("gate_3"), 11));
    CHECK(sounds(h.output("gate_3"), 12));
}

TEST_CASE("canon: a follower sounds where the lead did; All carries them on one cable") {
    ModuleHarness h(supple::rack::test::stock("canon"));
    lead(h, false);  // the lead sounds on even steps only
    h.set("voices", 2);
    h.set("delay_1", 3);
    h.run(kStep * 20);
    const auto& g1 = h.output("gate_1");
    for (int k = 3; k < 20; ++k) CHECK(sounds(g1, k) == ((k - 3) % 2 == 0));
    // A silent step holds the follower's last note: step 8 would play step
    // 5, which was silent, so step 4's note stays.
    CHECK(h.output("pitch_1")[8 * kStep + 100] == doctest::Approx(4 / 12.0));
    CHECK(h.channels("all") == 3);
    CHECK(h.channels("all_gates") == 3);
    CHECK(h.output("all", 0)[15500] == doctest::Approx(15 / 12.0));
    CHECK(h.output("all", 1)[15500] == h.output("pitch_1")[15500]);
    CHECK(h.output("all_gates", 2)[15100] == h.output("gate_2")[15100]);
    CHECK(h.output("gate_3")[15100] == 0.0);  // past Voices: silent
}

TEST_CASE("canon: Thin leaves notes out, the same way after Reset") {
    ModuleHarness h(supple::rack::test::stock("canon"));
    lead(h, true);
    h.set("thin", 50);
    h.set("delay_1", 1);
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 100 * kStep + 600 && n < 100 * kStep + 700 ? 1.0 : 0.0;
    });
    h.run(kStep * 200);
    const auto& g1 = h.output("gate_1");
    int played = 0;
    for (int k = 1; k < 100; ++k) played += sounds(g1, k) ? 1 : 0;
    CHECK(played > 30);
    CHECK(played < 70);
    // The reset comes just after step 100's clock, so step 101 is the new
    // line's first step: from there it plays and thins as from step 0.
    for (int k = 0; k < 99; ++k) CHECK(sounds(g1, k) == sounds(g1, 101 + k));
}

TEST_CASE("canon: with nothing in Clock it steps on the song's sixteenths") {
    ModuleHarness h(supple::rack::test::stock("canon"));
    h.set("voices", 1);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // a sixteenth is 6000 frames
    h.transport(t);
    h.input("in", 1, [](int, std::int64_t n) { return n < 6000 ? 0.5 : 0.0; });
    h.input("in_gate", 1, [](int, std::int64_t n) { return n < 6000 ? 1.0 : 0.0; });
    h.run(48000);
    const auto& g1 = h.output("gate_1");
    // The first sixteenth's note comes back four sixteenths (a beat) later,
    // as long as it was.
    CHECK(g1[24000 + 100] == 1.0);
    CHECK(g1[24000 + 5900] == 1.0);
    CHECK(g1[30000 + 100] == 0.0);
    CHECK(h.output("pitch_1")[24100] == doctest::Approx(0.5));
}

TEST_CASE("canon: a note the lead holds over several steps ties on in its followers") {
    ModuleHarness h(supple::rack::test::stock("canon"));
    h.set("voices", 1);
    h.set("delay_1", 2);
    h.input("clock", 1,
            [](int, std::int64_t n) { return n % kStep < 10 ? 1.0 : 0.0; });  // triggers
    h.input("in", 1, [](int, std::int64_t) { return 0.25; });
    // Held from step 0 through step 2, then silent.
    h.input("in_gate", 1, [](int, std::int64_t n) { return n < 3 * kStep ? 1.0 : 0.0; });
    h.run(kStep * 8);
    const auto& g1 = h.output("gate_1");
    for (std::int64_t n = 2 * kStep; n < 5 * kStep; n += 50)
        CHECK(g1[static_cast<std::size_t>(n)] == 1.0);
    CHECK(g1[5 * kStep + 10] == 0.0);
    CHECK(g1[2 * kStep - 1] == 0.0);
}
