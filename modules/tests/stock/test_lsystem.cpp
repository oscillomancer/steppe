// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kStep = 4800;

ModuleHarness::Signal clockOf(int period) {
    return [period](int, std::int64_t n) { return (n % period) < 100 ? 1.0 : 0.0; };
}

double at(ModuleHarness& h, const char* key, int step) {
    return h.output(key)[static_cast<std::size_t>(step * kStep + 10)];
}

}  // namespace

TEST_CASE("lsystem: A to AB, B to A grows the Fibonacci word; the moves walk it") {
    ModuleHarness h(supple::rack::test::stock("lsystem"));
    h.set("rule_a", 4);  // AB
    h.set("rule_b", 0);  // A
    h.set("depth", 4);   // ABAABABA
    h.set("scale", 0);   // chromatic
    h.set("move_a", 1);
    h.set("move_b", -1);
    h.input("clock", 1, clockOf(kStep));
    h.run(kStep * 16);
    const std::vector<double> symbols{0, 0.5, 0, 0, 0.5, 0, 0.5, 0};
    const std::vector<int> notes{0, 1, 0, 1, 2, 1, 2, 1};
    for (int k = 0; k < 16; ++k) {
        CHECK(at(h, "symbol", k) == symbols[static_cast<std::size_t>(k % 8)]);
        CHECK(at(h, "pitch", k) == doctest::Approx(notes[static_cast<std::size_t>(k % 8)] / 12.0));
        CHECK(at(h, "gate", k) == 1.0);
    }
    CHECK(h.output("start")[10] == 1.0);
    CHECK(h.output("start")[8 * kStep + 10] == 1.0);
    CHECK(h.output("start")[4 * kStep + 10] == 0.0);
}

TEST_CASE("lsystem: Rest, Loop, folding into Range, and the scale's degrees") {
    SUBCASE("rest b, loop 3") {
        ModuleHarness h(supple::rack::test::stock("lsystem"));
        h.set("rule_a", 4);
        h.set("rule_b", 0);
        h.set("depth", 4);
        h.set("rest", 2);
        h.set("loop", 3);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 6);
        const std::vector<double> gates{1, 0, 1, 1, 0, 1};
        for (int k = 0; k < 6; ++k) CHECK(at(h, "gate", k) == gates[static_cast<std::size_t>(k)]);
    }
    SUBCASE("folded by octaves into one octave") {
        ModuleHarness h(supple::rack::test::stock("lsystem"));
        h.set("rule_a", 3);  // AA
        h.set("depth", 4);   // sixteen As
        h.set("scale", 0);
        h.set("range", 1);
        h.set("move_a", 1);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 8);
        for (int k = 0; k < 6; ++k) CHECK(at(h, "pitch", k) == doctest::Approx(k / 12.0));
        CHECK(at(h, "pitch", 6) == doctest::Approx(-6 / 12.0));
        CHECK(at(h, "pitch", 7) == doctest::Approx(-5 / 12.0));
    }
    SUBCASE("degrees of minor pentatonic in D") {
        ModuleHarness h(supple::rack::test::stock("lsystem"));
        h.set("rule_a", 3);
        h.set("depth", 4);
        h.set("root", 2);
        h.set("move_a", 1);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 5);
        const std::vector<int> semis{2, 5, 7, 9, 12};
        for (int k = 0; k < 5; ++k)
            CHECK(at(h, "pitch", k) == doctest::Approx(semis[static_cast<std::size_t>(k)] / 12.0));
    }
}

TEST_CASE("lsystem: the song's eighths; Reset goes back to the start") {
    ModuleHarness h(supple::rack::test::stock("lsystem"));
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 12000 frames an eighth
    h.transport(t);
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 59000 && n < 59100 ? 1.0 : 0.0; });
    h.run(120000);
    const auto& p = h.output("pitch");
    const auto& g = h.output("gate");
    for (int k = 0; k < 5; ++k) {
        CHECK(g[static_cast<std::size_t>(k * 12000 + 10)] == 1.0);
        CHECK(g[static_cast<std::size_t>(k * 12000 + 6100)] == 0.0);
        CHECK(p[static_cast<std::size_t>(60000 + k * 12000 + 10)] ==
              p[static_cast<std::size_t>(k * 12000 + 10)]);
    }
}
