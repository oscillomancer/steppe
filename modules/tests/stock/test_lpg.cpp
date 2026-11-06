// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal sine(double hz, double a = 1.0) {
    return [hz, a](int, std::int64_t n) { return a * std::sin(2 * kPi * hz * n / 48000.0); };
}
ModuleHarness::Signal trigger(std::int64_t at) {
    return [at](int, std::int64_t n) { return n >= at && n < at + 48 ? 1.0 : 0.0; };
}
}  // namespace

TEST_CASE("lpg: shut at rest, a ping opens it and it dies away over Decay") {
    for (double decay : {150.0, 600.0}) {
        CAPTURE(decay);
        ModuleHarness h(stock("lpg"));
        h.set("decay", decay);
        h.input("in", 1, sine(220));
        h.input("ping", 1, trigger(4800));
        h.run(48000 * 3);
        const auto& y = h.output("out");
        const auto& lv = h.output("level");
        CHECK(peak(slice(y, 0, 4800)) < 1e-3);
        const double open = peak(slice(y, 4800, 4800 + 960));
        CHECK(open > 0.4);
        // One Decay later it has fallen most of the way; the tail lingers.
        const std::size_t later = 4800 + static_cast<std::size_t>(48 * decay);
        CHECK(lv[later] < 0.45);
        CHECK(lv[later] > 0.05);
        CHECK(peak(slice(lv, 4800, 4800 + 480)) > 0.9);
    }
}

TEST_CASE("lpg: it closes from the top down") {
    ModuleHarness h(stock("lpg"));
    h.set("mode", 1);  // the filter alone keeps the level, shows the colour
    h.set("resonance", 0);
    h.input("in", 1, [](int, std::int64_t n) {
        return 0.5 * std::sin(2 * kPi * 220 * n / 48000.0) +
               0.5 * std::sin(2 * kPi * 3520 * n / 48000.0);
    });
    h.input("ping", 1, trigger(0));
    h.run(48000);
    const auto early = slice(h.output("out"), 480, 2400),
               late = slice(h.output("out"), 14400, 16800);
    const double brightEarly = amplitudeAt(early, 3520, 48000) / amplitudeAt(early, 220, 48000);
    const double brightLate = amplitudeAt(late, 3520, 48000) / amplitudeAt(late, 220, 48000);
    CHECK(brightEarly > 0.5);
    CHECK(brightLate < 0.5 * brightEarly);
}

TEST_CASE("lpg: CV and a held gate hold it open; VCA mode is clean") {
    ModuleHarness h(stock("lpg"));
    h.set("mode", 2);
    h.input("in", 1, sine(220, 0.5));
    h.input("cv", 1, [](int, std::int64_t) { return 1.0; });
    h.run(9600);
    CHECK(peak(slice(h.output("out"), 4800)) == doctest::Approx(0.5).epsilon(0.01));

    ModuleHarness g(stock("lpg"));
    g.input("in", 1, sine(220, 0.5));
    g.input("ping", 1, [](int, std::int64_t n) { return n < 24000 ? 1.0 : 0.0; });
    g.run(48000);
    CHECK(g.output("level")[23000] > 0.99);
    CHECK(g.output("level")[47000] < 0.5);
}

TEST_CASE("lpg: poly, each channel pinged alone") {
    ModuleHarness h(stock("lpg"));
    h.input("in", 1, sine(220));
    h.input("ping", 2, [](int c, std::int64_t n) { return c == 1 && n < 48 ? 1.0 : 0.0; });
    h.run(4800);
    CHECK(peak(h.output("out", 0)) < 1e-3);
    CHECK(peak(h.output("out", 1)) > 0.4);
}
