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

constexpr int kStep = 4800;

ModuleHarness::Signal clockOf(int period) {
    return [period](int, std::int64_t n) { return (n % period) < 100 ? 1.0 : 0.0; };
}

int semis(double octaves) { return static_cast<int>(std::lround(octaves * 12.0)); }
int pitchClass(double octaves) { return ((semis(octaves) % 12) + 12) % 12; }

// The pitch class of each step, sampled just after its clock.
std::vector<int> classes(ModuleHarness& h, int from, int steps) {
    std::vector<int> pcs;
    for (int k = from; k < from + steps; ++k)
        pcs.push_back(pitchClass(h.output("pitch")[static_cast<std::size_t>(k * kStep + 10)]));
    return pcs;
}

}  // namespace

TEST_CASE("markov: notes of the scale, within Range, gated for Length of a step") {
    ModuleHarness h(supple::rack::test::stock("markov"));
    h.set("scale", 1);  // C major
    h.set("rest", 0);
    h.input("clock", 1, clockOf(kStep));
    h.run(kStep * 64);
    const auto& p = h.output("pitch");
    const auto& g = h.output("gate");
    std::set<int> heard;
    for (int k = 1; k < 64; ++k) {
        const std::size_t at = static_cast<std::size_t>(k * kStep + 10);
        const int pc = pitchClass(p[at]);
        CHECK((pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11));
        heard.insert(pc);
        CHECK(semis(p[at]) >= -6);  // two octaves around C4
        CHECK(semis(p[at]) < 18);
        CHECK(g[at] == 1.0);
        CHECK(g[at + kStep / 2 + 10] == 0.0);  // half a step
    }
    CHECK(heard.size() >= 5);
}

TEST_CASE("markov: it plays what it learned; Clear forgets it") {
    ModuleHarness h(supple::rack::test::stock("markov"));
    h.set("scale", 0);  // chromatic, so nothing it hears is left out
    h.set("rest", 0);
    h.set("surprise", 0);
    // Teach C, E, G, C, ... during the first second, then play.
    h.input("in", 1, [](int, std::int64_t n) {
        const int k = static_cast<int>(n / 1000);
        return (k % 3 == 0 ? 0 : (k % 3 == 1 ? 4 : 7)) / 12.0;
    });
    h.input("learn", 1, [](int, std::int64_t n) {
        return n < 48000 && (n % 1000) >= 500 && (n % 1000) < 600 ? 1.0 : 0.0;
    });
    h.input("clock", 1,
            [](int, std::int64_t n) { return n >= 48000 && (n % kStep) < 100 ? 1.0 : 0.0; });
    h.input("clear", 1, [](int, std::int64_t n) { return n >= 240000 && n < 240100 ? 1.0 : 0.0; });
    h.run(240000 + kStep * 40);
    // From C (the root it starts on): E, G, C, E, G, ...
    const auto learned = classes(h, 10, 30);
    for (std::size_t k = 0; k < learned.size(); ++k)
        CHECK(learned[k] == (k % 3 == 0 ? 4 : (k % 3 == 1 ? 7 : 0)));
    // Forgotten, the style's own odds wander off the cycle.
    const auto after = classes(h, 51, 38);
    bool offCycle = false;
    for (std::size_t k = 1; k < after.size(); ++k) {
        const int a = after[k - 1];
        const int b = after[k];
        if (!((a == 0 && b == 4) || (a == 4 && b == 7) || (a == 7 && b == 0))) offCycle = true;
    }
    CHECK(offCycle);
}

TEST_CASE("markov: Surprise from the likeliest note to almost any") {
    auto meanOdds = [](double surprise) {
        ModuleHarness h(supple::rack::test::stock("markov"));
        h.set("surprise", surprise);
        h.set("scale", 0);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 200);
        double sum = 0.0;
        for (int k = 1; k < 200; ++k)
            sum += h.output("odds")[static_cast<std::size_t>(k * kStep + 10)];
        return sum / 199.0;
    };
    const double sure = meanOdds(0);
    const double plain = meanOdds(50);
    const double wild = meanOdds(100);
    CHECK(sure > plain);
    CHECK(plain > wild);
    CHECK(wild < 0.15);  // near 1/12 each
}

TEST_CASE("markov: Rest silences steps; the song's eighths; Reset repeats") {
    SUBCASE("rest") {
        ModuleHarness h(supple::rack::test::stock("markov"));
        h.set("rest", 100);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 16);
        for (double v : h.output("gate")) CHECK(v == 0.0);
    }
    SUBCASE("eighths and reset") {
        ModuleHarness h(supple::rack::test::stock("markov"));
        h.set("rest", 0);
        h.set("length", 25);
        ModuleHarness::Transport t;
        t.playing = true;
        t.tempo = 120;  // 12000 frames an eighth
        t.beats = 0.0;
        h.transport(t);
        h.input("reset", 1,
                [](int, std::int64_t n) { return n >= 120000 - 50 && n < 120000 ? 1.0 : 0.0; });
        h.run(240000);
        const auto& g = h.output("gate");
        const auto& p = h.output("pitch");
        for (int k = 0; k < 10; ++k) {
            CHECK(g[static_cast<std::size_t>(k * 12000 + 10)] == 1.0);
            CHECK(g[static_cast<std::size_t>(k * 12000 + 3100)] == 0.0);
            CHECK(p[static_cast<std::size_t>(120000 + k * 12000 + 10)] ==
                  p[static_cast<std::size_t>(k * 12000 + 10)]);
        }
    }
}
