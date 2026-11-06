// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

ModuleHarness::Signal gateFor(std::int64_t from, std::int64_t to) {
    return [from, to](int, std::int64_t n) { return n >= from && n < to ? 1.0 : 0.0; };
}

void straight(ModuleHarness& h) {
    h.set("attack_curve", 0);
    h.set("decay_curve", 0);
    h.set("release_curve", 0);
}

}  // namespace

TEST_CASE("adsr: stages take their times, gated") {
    ModuleHarness h(supple::rack::test::stock("adsr"));
    straight(h);
    h.set("attack", 10);  // 480 frames
    h.set("decay", 20);   // 960
    h.set("sustain", 50);
    h.set("release", 40);  // 1920
    h.input("gate", 1, gateFor(0, 4800));
    h.run(9600);
    const auto& e = h.output("envelope");
    CHECK(e[239] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(e[479] == doctest::Approx(1.0).epsilon(0.01));
    CHECK(e[480 + 480] == doctest::Approx(0.75).epsilon(0.01));  // halfway down to 0.5
    CHECK(e[3000] == doctest::Approx(0.5));
    CHECK(e[4800 + 960] == doctest::Approx(0.25).epsilon(0.01));  // halfway released
    CHECK(e[4800 + 1925] == 0.0);
    CHECK(h.output("inverted")[3000] == doctest::Approx(0.5));
    // The end trigger: 1 ms as the release reaches 0, and only then.
    const auto& end = h.output("end_of_cycle");
    int high = 0;
    std::int64_t first = -1;
    for (std::size_t n = 0; n < end.size(); ++n)
        if (end[n] > 0.5) {
            ++high;
            if (first < 0) first = static_cast<std::int64_t>(n);
        }
    CHECK(high == 48);
    CHECK(first == doctest::Approx(4800 + 1920).epsilon(0.001));
}

TEST_CASE("adsr: curves bend the stages") {
    ModuleHarness h(supple::rack::test::stock("adsr"));
    straight(h);
    h.set("attack", 10);
    h.set("attack_curve", 80);
    h.input("gate", 1, gateFor(0, 48000));
    h.run(480);
    CHECK(h.output("envelope")[239] > 0.8);  // fast first

    ModuleHarness k(supple::rack::test::stock("adsr"));
    straight(k);
    k.set("attack", 10);
    k.set("attack_curve", -80);
    k.input("gate", 1, gateFor(0, 48000));
    k.run(480);
    CHECK(k.output("envelope")[239] < 0.2);  // slow first
}

TEST_CASE("adsr: triggered plays the whole shape from a pulse; velocity scales it") {
    ModuleHarness h(supple::rack::test::stock("adsr"));
    straight(h);
    h.set("mode", 1);
    h.set("attack", 10);
    h.set("decay", 10);
    h.set("sustain", 50);
    h.set("release", 10);
    h.input("gate", 1, gateFor(0, 48));  // 1 ms
    h.input("velocity", 1, [](int, std::int64_t) { return 0.5; });
    h.run(4800);
    const auto& e = h.output("envelope");
    CHECK(e[479] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(e[959] == doctest::Approx(0.25).epsilon(0.01));
    CHECK(e[1200] == doctest::Approx(0.125).epsilon(0.02));
    CHECK(e[1500] == 0.0);
}

TEST_CASE("adsr: looping cycles with nothing in the gate; poly gates are apart") {
    ModuleHarness h(supple::rack::test::stock("adsr"));
    straight(h);
    h.set("mode", 2);
    h.set("attack", 10);
    h.set("decay", 10);
    h.set("sustain", 0);
    h.set("release", 1);
    h.run(48000);
    int ends = 0;
    for (double x : h.output("end_of_cycle"))
        if (x > 0.5) ++ends;
    // A cycle is 10 + 10 + 1 ms, each end 48 frames high.
    CHECK(ends / 48 == doctest::Approx(1000.0 / 21.0).epsilon(0.05));

    ModuleHarness p(supple::rack::test::stock("adsr"));
    straight(p);
    p.set("attack", 10);
    p.input("gate", 2, [](int c, std::int64_t n) { return c == 0 || n >= 2400 ? 1.0 : 0.0; });
    p.run(4800);
    CHECK(p.channels("envelope") == 2);
    CHECK(p.output("envelope", 0)[2000] > 0.5);
    CHECK(p.output("envelope", 1)[2000] == 0.0);
    CHECK(p.output("envelope", 1)[2400 + 239] == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("adsr: a retrigger rises again from where it is") {
    ModuleHarness h(supple::rack::test::stock("adsr"));
    straight(h);
    h.set("attack", 10);
    h.set("decay", 10);
    h.set("sustain", 20);
    h.input("gate", 1, gateFor(0, 48000));
    h.input("retrigger", 1, gateFor(2400, 2448));
    h.run(4800);
    const auto& e = h.output("envelope");
    CHECK(e[2399] == doctest::Approx(0.2));
    CHECK(std::abs(e[2400] - e[2399]) < 0.01);                   // no jump
    CHECK(e[2400 + 384] == doctest::Approx(1.0).epsilon(0.01));  // 80 % of the attack left
}
