// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

// Classic's waves: sine, triangle, tri/saw, saw, saw/square, square, pulses.
constexpr double kSaw = 300.0 / 7.0;

std::vector<double> play(Settings settings, double octave = 0.0, int frames = 48000) {
    ModuleHarness h(stock("wavetable"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("pitch", 1, [octave](int, std::int64_t) { return octave; });
    h.run(4800 + frames);
    return slice(h.output("out"), 4800);
}
// Harmonic k's amplitude at the pitch hz.
double harmonic(const std::vector<double>& y, double hz, int k) {
    return amplitudeAt(y, hz * k, 48000);
}
}  // namespace

TEST_CASE("wavetable: plays at the pitch, a sine at Classic's start peaking at 1") {
    const auto y = play({{"position", 0}});
    CHECK(frequency(y, 48000) == doctest::Approx(kC4).epsilon(0.002));
    CHECK(peak(y) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(harmonic(y, kC4, 2) < 1e-3);
    CHECK(harmonic(y, kC4, 3) < 1e-3);
    const auto a = play({{"position", 0}}, 1.0);
    CHECK(frequency(a, 48000) == doctest::Approx(2 * kC4).epsilon(0.002));
    const auto b = play({{"position", 0}, {"coarse", -12}, {"fine", 50}});
    CHECK(frequency(b, 48000) == doctest::Approx(kC4 / 2 * std::exp2(50.0 / 1200)).epsilon(0.002));
}

TEST_CASE("wavetable: Position sweeps from sine to saw to square") {
    const auto saw = play({{"position", kSaw}});
    CHECK(harmonic(saw, kC4, 2) / harmonic(saw, kC4, 1) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(harmonic(saw, kC4, 5) / harmonic(saw, kC4, 1) == doctest::Approx(0.2).epsilon(0.03));
    const auto square = play({{"position", 500.0 / 7.0}});
    CHECK(harmonic(square, kC4, 2) < 1e-3);
    CHECK(harmonic(square, kC4, 3) / harmonic(square, kC4, 1) ==
          doctest::Approx(1.0 / 3).epsilon(0.03));
    // Halfway between two waves, a cross-fade of the two.
    const auto sine = play({{"position", 0}});
    const auto tri = play({{"position", 100.0 / 7.0}});
    const auto mid = play({{"position", 50.0 / 7.0}});
    for (int n = 0; n < 2000; n += 37)
        CHECK(mid[n] == doctest::Approx(0.5 * (sine[n] + tri[n])).epsilon(1e-3));
}

TEST_CASE("wavetable: no aliasing at any pitch") {
    // Notes of c times 10 Hz: c whole cycles in 4800 samples, harmonics on
    // bins c, 2c, ... Since c does not divide 4800, what folds over half
    // the sample rate lands between them. (Formant's last wave has nothing
    // left at the highest notes: all its harmonics are above half the
    // sample rate.)
    for (int t : {0, 1, 2, 3, 6}) {
        for (int c : {29, 97, 301, 503, 811, 1201, 1999}) {
            const double hz = 10.0 * c;
            CAPTURE(t);
            CAPTURE(hz);
            ModuleHarness h(stock("wavetable"));
            h.set("table", t);
            h.set("position", 100);
            h.input("pitch", 1, [hz](int, std::int64_t) { return octaveOf(hz); });
            h.run(4800 + 4800);
            const auto y = slice(h.output("out"), 4800);
            const double db = inharmonicDb(y, static_cast<std::size_t>(c));
            CAPTURE(db);
            CHECK((db < -50 || rms(y) < 1e-6));
        }
    }
}

TEST_CASE("wavetable: the tables have their characters") {
    // Sync's last wave rings near its 7th harmonic, Formant's near its 15th.
    const auto sync = play({{"table", 1}, {"position", 100}});
    CHECK(harmonic(sync, kC4, 7) > 2 * harmonic(sync, kC4, 2));
    const auto formant = play({{"table", 3}, {"position", 100}});
    CHECK(harmonic(formant, kC4, 15) > 4 * harmonic(formant, kC4, 3));
    // Organ's first wave is a sine, its last has the 16th harmonic.
    const auto first = play({{"table", 5}, {"position", 0}});
    CHECK(harmonic(first, kC4, 2) < 1e-3);
    const auto last = play({{"table", 5}, {"position", 100}});
    CHECK(harmonic(last, kC4, 16) > 0.05);
    CHECK(harmonic(last, kC4, 7) < 1e-3);
    // Vowel at A2: a's first formant (750 Hz) against i's (300 Hz).
    const double a2 = 110.0;
    const auto va = play({{"table", 4}, {"position", 0}}, octaveOf(a2));
    const auto vi = play({{"table", 4}, {"position", 50}}, octaveOf(a2));
    CHECK(harmonic(va, a2, 7) / harmonic(va, a2, 3) >
          3 * harmonic(vi, a2, 7) / harmonic(vi, a2, 3));
    // Digital's last wave is three levels, -1, 0 and 1, a third of the
    // cycle each: no third harmonic, a strong fifth.
    const auto digital = play({{"table", 6}, {"position", 100}}, octaveOf(100));
    CHECK(harmonic(digital, 100, 5) > 0.1);
    CHECK(harmonic(digital, 100, 3) < 2e-3);
    // Fold grows brighter along the table.
    const auto soft = play({{"table", 2}, {"position", 0}});
    const auto hard = play({{"table", 2}, {"position", 100}});
    CHECK(bandPower(hard, 4000, 48000) > 10 * bandPower(soft, 4000, 48000));
}

TEST_CASE("wavetable: every wave peaks near 1") {
    for (int t = 0; t < 7; ++t)
        for (double p : {0.0, 30.0, 60.0, 100.0}) {
            CAPTURE(t);
            CAPTURE(p);
            const auto y = play({{"table", t}, {"position", p}}, -1.0, 9600);
            CHECK(peak(y) > 0.8);
            CHECK(peak(y) < 1.1);
        }
}

TEST_CASE("wavetable: Unison spreads copies over Detune; FM and poly") {
    const auto y = play({{"position", 0}, {"unison", 3}, {"detune", 30}}, 0.0, 96000);
    const double up = kC4 * std::exp2(30.0 / 1200), down = kC4 / std::exp2(30.0 / 1200);
    const double each = std::pow(3.0, -0.8);
    CHECK(amplitudeAt(y, up, 48000) == doctest::Approx(each).epsilon(0.05));
    CHECK(amplitudeAt(y, down, 48000) == doctest::Approx(each).epsilon(0.05));
    CHECK(amplitudeAt(y, kC4, 48000) == doctest::Approx(each).epsilon(0.05));

    ModuleHarness h(stock("wavetable"));
    h.set("position", 0);
    h.set("fm_depth", 2);
    h.input("pitch", 2, [](int c, std::int64_t) { return c == 0 ? 0.0 : -1.0; });
    h.input("fm", 2, [](int, std::int64_t) { return 0.5; });
    h.run(4800 + 48000);
    CHECK(h.channels("out") == 2);
    CHECK(frequency(slice(h.output("out", 0), 4800), 48000) ==
          doctest::Approx(2 * kC4).epsilon(0.002));
    CHECK(frequency(slice(h.output("out", 1), 4800), 48000) == doctest::Approx(kC4).epsilon(0.002));
}
