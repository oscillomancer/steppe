// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

TEST_CASE("lfo: a free sine at its rate, from the bottom") {
    ModuleHarness h(supple::rack::test::stock("lfo"));
    h.set("rate", 1);
    h.run(48000);
    const auto& y = h.output("out");
    CHECK(y[0] == doctest::Approx(-1.0));
    CHECK(y[12000] == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(y[24000] == doctest::Approx(1.0));
    CHECK(h.output("phase")[36000] == doctest::Approx(0.75));
}

TEST_CASE("lfo: synced, the phase follows the song position") {
    ModuleHarness h(supple::rack::test::stock("lfo"));
    h.set("sync", 1);
    h.set("division", 6);  // 1/4: a cycle a beat
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 24000 frames a beat
    t.beats = 2.25;
    h.transport(t);
    h.run(24000);
    const auto& ph = h.output("phase");
    CHECK(ph[0] == doctest::Approx(0.25));
    CHECK(ph[12000] == doctest::Approx(0.75).epsilon(1e-3));
    // A beat (and so a cycle) begins at frame 18000: the trigger fires.
    CHECK(h.output("trigger")[18000 + 10] == 1.0);
    CHECK(h.output("trigger")[17000] == 0.0);
}

TEST_CASE("lfo: voices spread over a cycle") {
    ModuleHarness h(supple::rack::test::stock("lfo"));
    h.set("voices", 4);
    h.set("spread", 100);
    h.set("rate", 0.01);
    h.run(32);
    CHECK(h.channels("out") == 4);
    for (int c = 0; c < 4; ++c) CHECK(h.output("phase", c)[0] == doctest::Approx(0.25 * c));
    CHECK(h.output("out", 2)[0] == doctest::Approx(1.0));  // half a cycle on: the top
}

TEST_CASE("lfo: a square's width, unipolar and level") {
    ModuleHarness h(supple::rack::test::stock("lfo"));
    h.set("shape", 4);
    h.set("width", 25);
    h.set("polarity", 1);
    h.set("level", 50);
    h.set("rate", 10);
    h.run(48000);
    int high = 0;
    for (double x : h.output("out")) {
        CHECK((x == doctest::Approx(0.0) || x == doctest::Approx(0.5)));
        if (x > 0.25) ++high;
    }
    CHECK(high == doctest::Approx(12000).epsilon(0.01));
}

TEST_CASE("lfo: random steps repeat after a reset, and differ by cycle") {
    ModuleHarness h(supple::rack::test::stock("lfo"));
    h.set("shape", 5);
    h.set("rate", 10);  // 4800 frames a step
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 24000 && n < 24048 ? 1.0 : 0.0; });
    h.run(48000);
    const auto& y = h.output("out");
    CHECK(y[100] != y[4900]);
    CHECK(y[100] == y[200]);
    for (int k = 0; k < 4; ++k) CHECK(y[24000 + 100 + 4800 * k] == y[100 + 4800 * k]);
}

TEST_CASE("lfo: a phase input drives it") {
    ModuleHarness h(supple::rack::test::stock("lfo"));
    h.set("shape", 3);  // ramp
    h.input("phase_in", 1, [](int, std::int64_t n) { return std::fmod(n / 1000.0, 1.0); });
    h.run(3000);
    CHECK(h.output("out")[500] == doctest::Approx(0.0));
    CHECK(h.output("out")[1250] == doctest::Approx(-0.5));
    CHECK(h.output("trigger")[2005] == 1.0);
}
