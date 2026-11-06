// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kStep = 1000;  // a lead note a step

// A lead melody in C major: a walk by steps and thirds around C4, from a
// fixed sequence, ending each eight notes on C.
std::vector<int> melody(int count) {
    static const int scale[] = {0, 2, 4, 5, 7, 9, 11};
    std::vector<int> out;
    int d = 7;  // C4 as a degree from C3
    unsigned x = 12345;
    for (int k = 0; k < count; ++k) {
        x = x * 1103515245u + 12345u;
        const int r = static_cast<int>((x >> 16) % 6);
        const int by[] = {-2, -1, -1, 1, 1, 2};
        d += by[r];
        if (d < 3) d += 2;
        if (d > 11) d -= 2;
        if (k % 8 == 7) d = 7;
        out.push_back(12 * (d / 7 - 1) + scale[d % 7]);
    }
    return out;
}

// Plays the melody, a note a step; `legato` closes the gate for 2 ms
// between notes instead of 400 frames.
void play(ModuleHarness& h, const std::vector<int>& m, bool legato = false) {
    h.input("lead", 1, [m](int, std::int64_t n) {
        return m[static_cast<std::size_t>(std::min<std::int64_t>(n / kStep, m.size() - 1))] / 12.0;
    });
    h.input("lead_gate", 1, [legato](int, std::int64_t n) {
        return n % kStep < (legato ? kStep - 96 : 600) ? 1.0 : 0.0;
    });
}

int semis(double p) { return static_cast<int>(std::lround(p * 12)); }
int pc(int s) { return ((s % 12) + 12) % 12; }

}  // namespace

TEST_CASE("counter: consonant notes of the key below the lead, never parallel fifths or octaves") {
    const auto m = melody(64);
    ModuleHarness h(supple::rack::test::stock("counter"));
    play(h, m);
    h.run(kStep * 64);
    const std::set<int> cMajor = {0, 2, 4, 5, 7, 9, 11};
    const std::set<int> consonant = {0, 3, 4, 7, 8, 9};
    int contrary = 0, similar = 0, thirdsSixths = 0;
    int lastC = 0, lastIc = -1;
    for (int k = 0; k < 64; ++k) {
        CAPTURE(k);
        const auto at = static_cast<std::size_t>(k * kStep + 300);
        const int c = semis(h.output("pitch")[at]);
        const int l = m[static_cast<std::size_t>(k)];
        CHECK(h.output("gate")[at] == 1.0);
        CHECK(cMajor.count(pc(c)) == 1);
        CHECK(l - c >= 3);
        CHECK(l - c <= 16);
        const int ic = (l - c) % 12;
        CHECK(consonant.count(ic) == 1);
        CHECK(h.output("interval")[at] == doctest::Approx((c - l) / 12.0));
        thirdsSixths += ic == 3 || ic == 4 || ic == 8 || ic == 9 ? 1 : 0;
        if (k > 0) {
            const int ours = c - lastC;
            const int theirs = l - m[static_cast<std::size_t>(k - 1)];
            if ((ic == 0 || ic == 7) && ic == lastIc && ours != 0 && theirs != 0) FAIL("parallel");
            if (ours != 0 && theirs != 0) ((ours > 0) != (theirs > 0) ? contrary : similar)++;
            CHECK(std::abs(ours) <= 12);
        }
        lastC = c;
        lastIc = ic;
    }
    CHECK(contrary > 2 * similar);
    CHECK(thirdsSixths > 32);
    // The gate follows the lead's, 5 ms late.
    CHECK(h.output("gate")[600 + 230] == 1.0);
    CHECK(h.output("gate")[600 + 250] == 0.0);
}

TEST_CASE("counter: on the key's first note it meets the lead at the octave") {
    const auto m = melody(256);
    ModuleHarness h(supple::rack::test::stock("counter"));
    play(h, m);
    h.run(kStep * 256);
    int tonic = 0, octaves = 0, other = 0, otherOctaves = 0;
    for (int k = 1; k < 256; ++k) {
        const int c = semis(h.output("pitch")[static_cast<std::size_t>(k * kStep + 300)]);
        const int l = m[static_cast<std::size_t>(k)];
        const bool octave = (l - c) % 12 == 0;
        if (pc(l) == 0) {
            ++tonic;
            octaves += octave ? 1 : 0;
        } else {
            ++other;
            otherOctaves += octave ? 1 : 0;
        }
    }
    REQUIRE(tonic > 20);
    CHECK(octaves * 3 > tonic);  // over a third of the time; seldom elsewhere
    CHECK(otherOctaves * 5 < other);
}

TEST_CASE("counter: Hold keeps a note through the lead's next and ties it; Above sits above") {
    const auto m = melody(64);
    auto moves = [&m](double holdPct) {
        ModuleHarness h(supple::rack::test::stock("counter"));
        h.set("hold", holdPct);
        play(h, m, true);
        h.run(kStep * 64);
        const auto& g = h.output("gate");
        int moved = 0, ties = 0;
        for (int k = 1; k < 64; ++k) {
            const auto at = static_cast<std::size_t>(k * kStep);
            const bool same = h.output("pitch")[at + 300] == h.output("pitch")[at - 300];
            moved += same ? 0 : 1;
            // A held note's gate stays high over the lead's 2 ms gap.
            bool dipped = false;
            for (std::size_t n = at - 100; n < at + 200; ++n) dipped = dipped || g[n] == 0.0;
            if (same) ties += dipped ? 0 : 1;
            if (!same) CHECK(dipped);
        }
        CHECK(ties == 63 - moved);
        return moved;
    };
    CHECK(moves(100) < moves(0) - 10);

    ModuleHarness h(supple::rack::test::stock("counter"));
    h.set("place", 1);
    play(h, m);
    h.run(kStep * 64);
    for (int k = 0; k < 64; ++k) {
        const int c = semis(h.output("pitch")[static_cast<std::size_t>(k * kStep + 300)]);
        const int l = m[static_cast<std::size_t>(k)];
        CHECK(c - l >= 3);
        CHECK(c - l <= 16);
    }
}

TEST_CASE("counter: Vary takes other notes from the seed; Reset starts them over") {
    const auto m = melody(32);
    ModuleHarness h(supple::rack::test::stock("counter"));
    h.set("vary", 80);
    h.input("lead", 1, [m](int, std::int64_t n) {
        return m[static_cast<std::size_t>((n / kStep) % 32)] / 12.0;
    });
    h.input("lead_gate", 1, [](int, std::int64_t n) { return n % kStep < 600 ? 1.0 : 0.0; });
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 32 * kStep - 200 && n < 32 * kStep - 100 ? 1.0 : 0.0;
    });
    h.run(kStep * 64);
    for (int k = 0; k < 32; ++k)
        CHECK(h.output("pitch")[static_cast<std::size_t>(k * kStep + 300)] ==
              h.output("pitch")[static_cast<std::size_t>((k + 32) * kStep + 300)]);
    ModuleHarness plain(supple::rack::test::stock("counter"));
    plain.set("vary", 0);
    play(plain, m);
    plain.run(kStep * 32);
    int differ = 0;
    for (int k = 0; k < 32; ++k)
        differ += h.output("pitch")[static_cast<std::size_t>(k * kStep + 300)] !=
                          plain.output("pitch")[static_cast<std::size_t>(k * kStep + 300)]
                      ? 1
                      : 0;
    CHECK(differ > 3);
}

TEST_CASE("counter: with nothing in Gate each new pitch is a note") {
    ModuleHarness h(supple::rack::test::stock("counter"));
    h.input("lead", 1, [](int, std::int64_t n) { return n < 1000 ? 0.0 : 2.0 / 12; });
    h.run(2000);
    CHECK(h.output("gate")[500] == 1.0);
    const int a = semis(h.output("pitch")[500]);
    const int b = semis(h.output("pitch")[1500]);
    CHECK(-a >= 3);
    CHECK(b != a);
    CHECK(h.output("moved")[1000] == 1.0);
}
