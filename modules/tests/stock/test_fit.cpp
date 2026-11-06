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

int semis(double pitch) { return static_cast<int>(std::lround(pitch * 12.0)); }

// C major (C E G) until frame `at`, then A minor (A C E), two octaves up:
// only the pitch classes count.
void chords(ModuleHarness& h, std::int64_t at = 1 << 30) {
    h.input("chord", 3, [at](int c, std::int64_t n) {
        static const int major[] = {0, 4, 7};
        static const int minor[] = {9, 12, 16};
        return 2.0 + (n < at ? major[c] : minor[c]) / 12.0;
    });
}

// Pitch input k semitones above C4 for frames [100k, 100k + 100).
void sweep(ModuleHarness& h, int from) {
    h.input("pitch", 1, [from](int, std::int64_t n) { return (from + n / 100) / 12.0; });
}

int at(ModuleHarness& h, int step) {
    return semis(h.output("out")[static_cast<std::size_t>(step * 100 + 50)]);
}

}  // namespace

TEST_CASE("fit: each pitch goes to the nearest tone of the chord, in any octave") {
    ModuleHarness h(supple::rack::test::stock("fit"));
    chords(h);
    sweep(h, -2);  // from A#3
    h.run(100 * 16);
    // A#3, B3, C4, C#4, D4 (between C and E: down), D#4, E4, F4, F#4, G4,
    // G#4, A4 (nearer G), A#4, B4, C5, C#5.
    const std::vector<int> want = {0, 0, 0, 0, 0, 4, 4, 4, 7, 7, 7, 7, 12, 12, 12, 12};
    for (int k = 0; k < 16; ++k) {
        CAPTURE(k);
        CHECK(at(h, k) == want[static_cast<std::size_t>(k)]);
    }
}

TEST_CASE("fit: Snap rounds down or up; Degree and Walk move along the chord") {
    auto one = [](double pitch, int snap, int degree, double walk) {
        ModuleHarness h(supple::rack::test::stock("fit"));
        chords(h);
        h.set("snap", snap);
        h.set("degree", degree);
        h.input("pitch", 1, [pitch](int, std::int64_t) { return pitch; });
        h.input("walk", 1, [walk](int, std::int64_t) { return walk; });
        h.run(256);
        return semis(h.output("out")[200]);
    };
    CHECK(one(2.0 / 12, 1, 0, 0) == 0);      // D down: C
    CHECK(one(2.0 / 12, 2, 0, 0) == 4);      // D up: E
    CHECK(one(5.0 / 12, 2, 0, 0) == 7);      // F up: G
    CHECK(one(0.0, 0, 1, 0) == 4);           // C, a tone up: E
    CHECK(one(0.0, 0, -1, 0) == -5);         // a tone down: G3
    CHECK(one(0.0, 0, 0, 1.0) == 12);        // Walk 1: three tones, an octave
    CHECK(one(4.0 / 12, 0, 1, -1.0) == -5);  // E, up one and down three: G3
}

TEST_CASE("fit: a note moves with the chord; Changed fires as it does") {
    ModuleHarness h(supple::rack::test::stock("fit"));
    chords(h, 1000);
    h.input("pitch", 1, [](int, std::int64_t) { return 7.0 / 12; });  // G4
    h.run(2000);
    const auto& out = h.output("out");
    const auto& changed = h.output("changed");
    CHECK(semis(out[999]) == 7);
    CHECK(semis(out[1000]) == 9);  // A minor's nearest to G: A
    for (std::size_t n = 10; n < 1000; ++n) CHECK(changed[n] == 0.0);
    CHECK(changed[1000] == 1.0);
    CHECK(changed[1047] == 1.0);
    CHECK(changed[1048] == 0.0);
}

TEST_CASE("fit: with a clock it holds between edges; Follow moves a held note to a new chord") {
    // The notes at frames 500, 1400, 1600 and 2100.
    auto run = [](bool follow) {
        ModuleHarness h(supple::rack::test::stock("fit"));
        chords(h, 1500);
        h.set("follow", follow ? 1 : 0);
        sweep(h, 0);  // rising a semitone every 100 frames
        h.input("clock", 1, [](int, std::int64_t n) { return n % 1000 < 10 ? 1.0 : 0.0; });
        h.run(3000);
        std::vector<int> got;
        for (std::size_t n : {500, 1400, 1600, 2100}) got.push_back(semis(h.output("out")[n]));
        return got;
    };
    // Sampled at 0 (C) and held, though the input climbs; at 1000, A#4
    // goes to C5. At 1500 the chord turns A minor: Follow fits the held A#4
    // again, to A4; without, C5 stays. At 2000, G#5 in A minor goes to A5.
    CHECK(run(true) == std::vector<int>{0, 12, 9, 21});
    CHECK(run(false) == std::vector<int>{0, 12, 12, 21});

    // A held G moves to A as the chord changes, with Follow; without, it stays.
    auto g = [](bool follow) {
        ModuleHarness h(supple::rack::test::stock("fit"));
        chords(h, 1500);
        h.set("follow", follow ? 1 : 0);
        h.input("pitch", 1, [](int, std::int64_t) { return 7.0 / 12; });
        h.input("clock", 1, [](int, std::int64_t n) { return n < 10 ? 1.0 : 0.0; });
        h.run(2000);
        return semis(h.output("out")[1800]);
    };
    CHECK(g(true) == 9);
    CHECK(g(false) == 7);
}

TEST_CASE("fit: with nothing in Chord it rounds to semitones, poly") {
    ModuleHarness h(supple::rack::test::stock("fit"));
    h.input("pitch", 2, [](int c, std::int64_t) { return c == 0 ? 0.3 / 12 : 6.7 / 12; });
    h.run(256);
    CHECK(h.channels("out") == 2);
    CHECK(semis(h.output("out", 0)[100]) == 0);
    CHECK(h.output("out", 0)[100] == 0.0);
    CHECK(h.output("out", 1)[100] == doctest::Approx(7.0 / 12));
}
