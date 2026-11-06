// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
ModuleHarness::Signal pluckAt(std::int64_t at) {
    return [at](int, std::int64_t n) { return n >= at && n < at + 48 ? 1.0 : 0.0; };
}
ModuleHarness::Signal constant(double v) {
    return [v](int, std::int64_t) { return v; };
}
double dbOf(double x) { return 20 * std::log10(std::max(x, 1e-30)); }
}  // namespace

TEST_CASE("string: rings at its pitch") {
    for (double hz : {82.41, 261.63, 1046.5}) {
        for (double stiff : {0.0, 60.0}) {
            CAPTURE(hz);
            CAPTURE(stiff);
            ModuleHarness h(stock("string"));
            h.set("stiffness", stiff);
            h.set("damping", 50);
            h.input("pitch", 1, constant(octaveOf(hz)));
            h.input("pluck", 1, pluckAt(0));
            h.run(48000);
            const auto y = slice(h.output("out"), 4800, 4800 + 24000);
            CHECK(peakHz(y, hz * 0.97, hz * 1.03, 48000, hz / 2000) ==
                  doctest::Approx(hz).epsilon(0.002));
        }
    }
}

TEST_CASE("string: decays over Decay, its highs faster with Damping") {
    ModuleHarness h(stock("string"));
    h.set("decay", 2000);
    h.set("damping", 0);
    h.input("pitch", 1, constant(octaveOf(220)));
    h.input("pluck", 1, pluckAt(0));
    h.run(48000 * 2);
    const auto& y = h.output("out");
    const double drop = dbOf(amplitudeAt(slice(y, 9600, 19200), 220, 48000)) -
                        dbOf(amplitudeAt(slice(y, 57600, 67200), 220, 48000));
    CHECK(drop == doctest::Approx(30).epsilon(0.15));  // 1 s of a 2 s T60

    ModuleHarness d(stock("string"));
    d.set("damping", 90);
    d.input("pitch", 1, constant(octaveOf(220)));
    d.input("pluck", 1, pluckAt(0));
    d.run(48000);
    const auto e = slice(d.output("out"), 24000, 33600);
    const auto f = slice(h.output("out"), 24000, 33600);
    CHECK(amplitudeAt(e, 1320, 48000) / amplitudeAt(e, 220, 48000) <
          0.2 * amplitudeAt(f, 1320, 48000) / amplitudeAt(f, 220, 48000));
}

TEST_CASE("string: stiffness stretches the overtones, the middle has no even ones") {
    ModuleHarness h(stock("string"));
    h.set("stiffness", 80);
    h.set("damping", 10);
    h.input("pitch", 1, constant(octaveOf(220)));
    h.input("pluck", 1, pluckAt(0));
    h.run(48000);
    const auto y = slice(h.output("out"), 2400, 26400);
    const double f1 = peakHz(y, 210, 230, 48000, 0.1);
    const double f4 = peakHz(y, 4 * f1, 4.6 * f1, 48000, 0.5);
    CHECK(f4 / f1 > 4.04);

    ModuleHarness m(stock("string"));
    m.set("position", 50);
    m.set("damping", 10);
    m.input("pitch", 1, constant(octaveOf(220)));
    m.input("pluck", 1, pluckAt(0));
    m.run(24000);
    const auto z = slice(m.output("out"), 2400);
    CHECK(amplitudeAt(z, 440, 48000) < 0.1 * amplitudeAt(z, 220, 48000));
    CHECK(amplitudeAt(z, 660, 48000) > 0.1 * amplitudeAt(z, 220, 48000));
}

TEST_CASE("string: velocity, an exciter, a string per channel") {
    ModuleHarness soft(stock("string")), hard(stock("string"));
    for (ModuleHarness* h : {&soft, &hard}) {
        h->input("pluck", 1, pluckAt(0));
        h->input("velocity", 1, constant(h == &soft ? 0.25 : 1.0));
        h->run(9600);
    }
    CHECK(peak(soft.output("out")) < 0.5 * peak(hard.output("out")));
    CHECK(peak(hard.output("out")) > 0.2);
    CHECK(peak(hard.output("out")) < 1.5);

    ModuleHarness bowed(stock("string"));
    bowed.input("pitch", 1, constant(octaveOf(330)));
    bowed.input("exciter", 1, [](int, std::int64_t n) {
        std::uint32_t x = static_cast<std::uint32_t>(n) * 2654435761u;
        x ^= x >> 13;
        return (static_cast<double>(x & 0xffff) / 32768.0 - 1.0) * 0.3;
    });
    bowed.run(48000);
    const auto b = slice(bowed.output("out"), 24000);
    CHECK(amplitudeAt(b, 330, 48000) > 4 * amplitudeAt(b, 290, 48000));

    ModuleHarness poly(stock("string"));
    poly.input("pitch", 2, [](int c, std::int64_t) { return c == 0 ? 0.0 : 7.0 / 12.0; });
    poly.input("pluck", 2, pluckAt(0));
    poly.run(24000);
    CHECK(peakHz(slice(poly.output("out", 1), 2400), 380, 400, 48000, 0.1) ==
          doctest::Approx(kC4 * std::exp2(7.0 / 12.0)).epsilon(0.002));
}
