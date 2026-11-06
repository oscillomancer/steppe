// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kBar = 96000;  // 4/4 at 120 beats a minute
constexpr int kStep = 1000;  // the test clock

int semis(double pitch) { return static_cast<int>(std::lround(pitch * 12.0)); }
int pc(int s) { return ((s % 12) + 12) % 12; }

// The chord in step k of a clocked run, in semitones from C4.
std::vector<int> chordAt(ModuleHarness& h, int k, int notes) {
    std::vector<int> c;
    for (int v = 0; v < notes; ++v)
        c.push_back(semis(h.output("chord", v)[static_cast<std::size_t>(k * kStep + 500)]));
    return c;
}

void clocked(ModuleHarness& h) {
    h.input("clock", 1, [](int, std::int64_t n) { return n % kStep < 10 ? 1.0 : 0.0; });
}

}  // namespace

TEST_CASE("changes: a chord a bar from the home chord; locked, the loop comes round the same") {
    ModuleHarness h(supple::rack::test::stock("changes"));
    h.set("change", 0);
    h.set("lead", 0);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
    h.run(kBar * 8);
    // C major's home chord in root position around C4.
    CHECK(semis(h.output("chord", 0)[100]) == 0);
    CHECK(semis(h.output("chord", 1)[100]) == 4);
    CHECK(semis(h.output("chord", 2)[100]) == 7);
    CHECK(h.output("root")[100] == doctest::Approx(-1.0));  // C3
    CHECK(h.output("degree")[100] == 0.0);
    // The gate opens 2 ms in, and again at each chord.
    CHECK(h.output("gate", 0)[50] == 0.0);
    CHECK(h.output("gate", 0)[200] == 1.0);
    CHECK(h.output("gate", 0)[kBar + 50] == 0.0);
    std::vector<std::int64_t> nexts;
    const auto& next = h.output("next");
    for (std::size_t n = 1; n < next.size(); ++n)
        if (next[n] >= 0.5 && next[n - 1] < 0.5) nexts.push_back(static_cast<std::int64_t>(n));
    if (next[0] >= 0.5) nexts.insert(nexts.begin(), 0);
    CHECK(nexts.size() == 8);
    CHECK(nexts[1] == kBar);
    // Four chords, then the same four.
    for (int b = 0; b < 4; ++b)
        for (int v = 0; v < 3; ++v)
            CHECK(h.output("chord", v)[static_cast<std::size_t>(b * kBar + 5000)] ==
                  h.output("chord", v)[static_cast<std::size_t>((b + 4) * kBar + 5000)]);
}

TEST_CASE("changes: every chord is built from the mode's own thirds") {
    struct Case {
        int mode;  // its index: major, minor, harmonic minor
        std::set<int> scale;
    };
    const std::vector<Case> cases = {
        {0, {0, 2, 4, 5, 7, 9, 11}}, {1, {0, 2, 3, 5, 7, 8, 10}}, {6, {0, 2, 3, 5, 7, 8, 11}}};
    for (const auto& c : cases) {
        CAPTURE(c.mode);
        ModuleHarness h(supple::rack::test::stock("changes"));
        h.set("mode", c.mode);
        h.set("change", 100);
        h.set("loop", 8);
        h.set("home", 0);
        clocked(h);
        h.run(kStep * 200);
        std::set<int> degrees;
        for (int k = 0; k < 200; ++k) {
            const auto chord = chordAt(h, k, 3);
            for (int s : chord) CHECK(c.scale.count(pc(s)) == 1);
            const int d = static_cast<int>(
                std::lround(h.output("degree")[static_cast<std::size_t>(k * kStep + 500)] * 6));
            degrees.insert(d);
            // The root, an octave below, is the chord's root.
            const int r = semis(h.output("root")[static_cast<std::size_t>(k * kStep + 500)]);
            bool hasRoot = false;
            for (int s : chord) hasRoot = hasRoot || pc(s) == pc(r);
            CHECK(hasRoot);
            // The fifth chord is minor in the natural minor, major in the
            // harmonic: its third is B flat or B.
            if (d == 4) {
                std::set<int> pcs;
                for (int s : chord) pcs.insert(pc(s));
                if (c.mode != 0) CHECK(pcs.count(c.mode == 6 ? 11 : 10) == 1);
            }
        }
        CHECK(degrees.size() >= 5);  // it travels round the key
    }
}

TEST_CASE("changes: Lead voices each chord near the last; Notes stacks sevenths") {
    auto movement = [](bool lead) {
        ModuleHarness h(supple::rack::test::stock("changes"));
        h.set("lead", lead ? 1 : 0);
        h.set("change", 100);
        h.set("loop", 16);
        clocked(h);
        h.run(kStep * 100);
        int total = 0;
        for (int k = 1; k < 100; ++k) {
            const auto a = chordAt(h, k - 1, 3);
            const auto b = chordAt(h, k, 3);
            for (int v = 0; v < 3; ++v) {
                total += std::abs(b[v] - a[v]);
                CHECK(b[v] >= -12);
                CHECK(b[v] <= 24);
            }
        }
        return total;
    };
    const int led = movement(true);
    const int plain = movement(false);
    CHECK(led * 2 < plain);
    CHECK(led < 99 * 6);  // about two semitones a voice a chord at most

    ModuleHarness h(supple::rack::test::stock("changes"));
    h.set("notes", 4);
    h.set("lead", 0);
    clocked(h);
    h.run(kStep * 2);
    CHECK(h.channels("chord") == 4);
    CHECK(chordAt(h, 0, 4) == std::vector<int>{0, 4, 7, 11});  // C major seventh
}

TEST_CASE("changes: Reset starts the same progression over") {
    ModuleHarness h(supple::rack::test::stock("changes"));
    h.set("change", 60);
    h.set("loop", 6);
    clocked(h);
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 50 * kStep + 500 && n < 50 * kStep + 600 ? 1.0 : 0.0;
    });
    h.run(kStep * 100);
    // The reset lands mid-step 50: step 51 is the new run's first chord.
    for (int k = 0; k < 40; ++k) CHECK(chordAt(h, k, 3) == chordAt(h, 51 + k, 3));
    CHECK(chordAt(h, 51, 3) == std::vector<int>{0, 4, 7});
}

TEST_CASE("changes: the walk moves down a fifth most, and seldom to the diminished chord") {
    ModuleHarness h(supple::rack::test::stock("changes"));
    h.set("loop", 16);
    h.set("change", 100);
    h.set("home", 0);
    clocked(h);
    h.run(kStep * 2000);
    std::vector<int> moves(7, 0);
    int diminished = 0;
    int last = -1;
    for (int k = 0; k < 2000; ++k) {
        const int d = static_cast<int>(
            std::lround(h.output("degree")[static_cast<std::size_t>(k * kStep + 500)] * 6));
        if (d == 6) ++diminished;
        if (last >= 0) ++moves[static_cast<std::size_t>(((d - last) % 7 + 7) % 7)];
        last = d;
    }
    // Down a fifth (up three degrees) the most; down as well as up.
    for (int i = 0; i < 7; ++i)
        if (i != 3) CHECK(moves[3] > moves[static_cast<std::size_t>(i)]);
    CHECK(moves[5] > 100);  // down a third
    CHECK(moves[6] > 50);   // down a step
    CHECK(diminished < 2000 / 10);
}
