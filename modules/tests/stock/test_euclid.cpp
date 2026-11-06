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

// What an output did on each of `count` clocks, as x (high) and . (low).
std::string pattern(ModuleHarness& h, const char* key, int count) {
    std::string s;
    for (int k = 0; k < count; ++k)
        s += h.output(key)[static_cast<std::size_t>(k * kPeriod + 10)] > 0.5 ? 'x' : '.';
    return s;
}

void clocked(ModuleHarness& h, int count) {
    h.input("clock", 1, [](int, std::int64_t n) { return n % kPeriod < 100 ? 1.0 : 0.0; });
    h.run(static_cast<std::int64_t>(count) * kPeriod);
}

}  // namespace

TEST_CASE("euclid: hits spread evenly, rotated") {
    ModuleHarness h(supple::rack::test::stock("euclid"));
    h.set("steps", 8);
    h.set("pulses", 3);
    clocked(h, 16);
    CHECK(pattern(h, "gate", 16) == "x..x..x.x..x..x.");
    CHECK(pattern(h, "off", 8) == ".xx.xx.x");

    ModuleHarness r(supple::rack::test::stock("euclid"));
    r.set("steps", 8);
    r.set("pulses", 3);
    r.set("rotation", 2);
    clocked(r, 8);
    CHECK(pattern(r, "gate", 8) == "x.x..x..");
}

TEST_CASE("euclid: accents fall evenly on hits") {
    ModuleHarness h(supple::rack::test::stock("euclid"));
    h.set("steps", 16);
    h.set("pulses", 8);
    h.set("accents", 2);
    clocked(h, 16);
    CHECK(pattern(h, "gate", 16) == "x.x.x.x.x.x.x.x.");
    CHECK(pattern(h, "accent", 16) == "x.......x.......");
    ModuleHarness s(supple::rack::test::stock("euclid"));
    s.set("steps", 16);
    s.set("pulses", 8);
    s.set("accents", 2);
    s.set("accent_shift", 1);
    clocked(s, 16);
    CHECK(pattern(s, "accent", 16) == "..x.......x.....");
}

TEST_CASE("euclid: the gate follows the clock; no clock follows the song") {
    ModuleHarness h(supple::rack::test::stock("euclid"));
    h.set("pulses", 16);
    clocked(h, 2);
    CHECK(h.output("gate")[50] == 1.0);
    CHECK(h.output("gate")[150] == 0.0);

    ModuleHarness s(supple::rack::test::stock("euclid"));
    s.set("steps", 4);
    s.set("pulses", 1);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;   // 6000 frames a sixteenth
    t.beats = 0.75;  // the fourth sixteenth: step 3
    s.transport(t);
    s.run(12000);
    CHECK(s.output("gate")[100] == 0.0);
    CHECK(s.output("gate")[6100] == 1.0);  // step 0 of the next beat
    CHECK(s.output("gate")[9100] == 0.0);  // half a sixteenth high
}
