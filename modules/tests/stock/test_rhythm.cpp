// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kPeriod = 2400;

void clocked(ModuleHarness& h, int count) {
    h.input("clock", 1, [](int, std::int64_t n) { return n % kPeriod < 100 ? 1.0 : 0.0; });
    h.run(static_cast<std::int64_t>(count) * kPeriod);
}

std::string pattern(const ModuleHarness& h, const char* key, int from = 0, int count = 16) {
    std::string s;
    for (int k = from; k < from + count; ++k)
        s += h.output(key)[static_cast<std::size_t>(k * kPeriod + 10)] > 0.5 ? 'x' : '.';
    return s;
}

}  // namespace

TEST_CASE("rhythm: the corners of the map are their grooves") {
    ModuleHarness h(supple::rack::test::stock("rhythm"));
    h.set("x", 0);
    h.set("y", 0);
    h.set("chaos", 0);
    clocked(h, 16);
    CHECK(pattern(h, "kick") == "x...x...x...x...");
    CHECK(pattern(h, "snare") == "................");
    CHECK(pattern(h, "hat") == "..x...x...x...x.");
    CHECK(pattern(h, "accent") == "x...x...x...x...");

    ModuleHarness b(supple::rack::test::stock("rhythm"));
    b.set("x", 50);
    b.set("y", 0);
    b.set("chaos", 0);
    clocked(b, 16);
    CHECK(pattern(b, "kick") == "x.......x.......");
    CHECK(pattern(b, "snare") == "....x.......x...");
    CHECK(pattern(b, "hat") == "x...x...x...x...");
}

TEST_CASE("rhythm: density picks the weights; X morphs between grooves") {
    ModuleHarness h(supple::rack::test::stock("rhythm"));
    h.set("x", 0);
    h.set("y", 100);
    h.set("chaos", 0);
    h.set("kick_density", 100);
    h.set("hat_density", 0);
    clocked(h, 16);
    CHECK(pattern(h, "kick") == "x...x...x...x..x");
    CHECK(pattern(h, "hat") == "................");

    // A quarter of the way to the backbeat: the kick on 2 is weighed 4.5,
    // so it plays at a density of 60 % and not at 50 %.
    ModuleHarness m(supple::rack::test::stock("rhythm"));
    m.set("x", 25);
    m.set("y", 0);
    m.set("chaos", 0);
    clocked(m, 16);
    CHECK(pattern(m, "kick") == "x.......x.......");
    ModuleHarness n(supple::rack::test::stock("rhythm"));
    n.set("x", 25);
    n.set("y", 0);
    n.set("chaos", 0);
    n.set("kick_density", 60);
    clocked(n, 16);
    CHECK(pattern(n, "kick") == "x...x...x...x...");
}

TEST_CASE("rhythm: chaos changes bars, the same again after a reset") {
    ModuleHarness h(supple::rack::test::stock("rhythm"));
    h.set("chaos", 60);
    h.input("clock", 1, [](int, std::int64_t n) { return n % kPeriod < 100 ? 1.0 : 0.0; });
    h.input("reset", 1, [](int, std::int64_t n) {
        return n >= 64 * kPeriod - 500 && n < 64 * kPeriod - 400 ? 1.0 : 0.0;
    });
    h.run(128 * kPeriod);
    bool varies = false;
    for (int bar = 1; bar < 4; ++bar)
        varies = varies || pattern(h, "hat", 16 * bar) != pattern(h, "hat");
    CHECK(varies);
    for (const char* key : {"kick", "snare", "hat", "perc", "accent"})
        CHECK(pattern(h, key, 64, 64) == pattern(h, key, 0, 64));
}

TEST_CASE("rhythm: with no clock it follows the song's bar") {
    ModuleHarness h(supple::rack::test::stock("rhythm"));
    h.set("chaos", 0);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 6000 frames a sixteenth
    t.beats = 1.0;  // the fifth sixteenth of the bar
    h.transport(t);
    h.run(6000 * 4);
    CHECK(h.output("kick")[100] == 1.0);   // step 4: a kick
    CHECK(h.output("kick")[6100] == 0.0);  // step 5
    CHECK(h.output("kick")[3100] == 0.0);  // half a sixteenth high
}
