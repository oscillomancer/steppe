// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
// 500 Hz: 100 cycles in 9600 frames.
std::vector<double> tone(std::initializer_list<std::pair<const char*, double>> settings,
                         const char* out = "out", double hz = 500.0) {
    ModuleHarness h(stock("harmonic"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("pitch", 1, [hz](int, std::int64_t) { return octaveOf(hz); });
    h.run(4800 + 9600);
    return slice(h.output(out), 4800);
}
double partial(const std::vector<double>& y, int n) { return std::sqrt(binPower(y, 100 * n)); }
}  // namespace

TEST_CASE("harmonic: its pitch, and a sine from one partial") {
    ModuleHarness h(stock("harmonic"));
    h.set("partials", 1);  // many partials cross zero many times a cycle
    h.run(48000);
    CHECK(frequency(h.output("out"), 48000) == doctest::Approx(kC4).epsilon(1e-3));
    const auto sine = tone({{"partials", 1}});
    CHECK(partial(sine, 2) < 1e-6 * partial(sine, 1));
    CHECK(peak(sine) == doctest::Approx(0.7).epsilon(1e-3));
}

TEST_CASE("harmonic: Partials counts them, the last fading in") {
    const auto eight = tone({{"partials", 8}});
    CHECK(partial(eight, 8) > 0.05 * partial(eight, 1));
    CHECK(partial(eight, 9) < 1e-6 * partial(eight, 1));
    const auto half = tone({{"partials", 8.5}});
    CHECK(partial(half, 9) / partial(half, 1) == doctest::Approx(0.5 / 9).epsilon(0.01));
}

TEST_CASE("harmonic: Brightness tilts the levels, which keep their power") {
    for (const auto& [bright, ratio] :
         {std::pair{0.0, 0.25}, std::pair{50.0, 0.5}, std::pair{100.0, 1.0}}) {
        CAPTURE(bright);
        const auto y = tone({{"brightness", bright}});
        CHECK(partial(y, 2) / partial(y, 1) == doctest::Approx(ratio).epsilon(0.01));
        CHECK(partial(y, 4) / partial(y, 1) == doctest::Approx(ratio * ratio).epsilon(0.01));
        CHECK(rms(y) == doctest::Approx(0.7 / std::sqrt(2.0)).epsilon(0.01));
        CHECK(peak(y) < 1.4);
    }
}

TEST_CASE("harmonic: Odd/Even, and the odd and even outputs") {
    const auto hollow = tone({{"odd_even", -100}});
    CHECK(partial(hollow, 2) < 1e-6 * partial(hollow, 1));
    CHECK(partial(hollow, 3) / partial(hollow, 1) == doctest::Approx(1.0 / 3).epsilon(0.01));
    const auto octaves = tone({{"odd_even", 100}});
    CHECK(partial(octaves, 3) < 1e-6 * partial(octaves, 1));
    CHECK(partial(octaves, 4) / partial(octaves, 1) == doctest::Approx(0.25).epsilon(0.01));

    ModuleHarness h(stock("harmonic"));
    h.run(4800);
    const auto out = h.output("out"), odd = h.output("odd"), even = h.output("even");
    double worst = 0;
    for (std::size_t n = 0; n < out.size(); ++n)
        worst = std::max(worst, std::abs(out[n] - odd[n] - even[n]));
    CHECK(worst < 1e-12);
    const auto o = tone({}, "odd"), e = tone({}, "even");
    CHECK(partial(o, 2) < 1e-6 * partial(o, 1));
    CHECK(partial(e, 1) < 1e-6 * partial(e, 2));
    CHECK(partial(e, 2) > 0.4 * partial(o, 1));
}

TEST_CASE("harmonic: Stretch moves the partials off the harmonics") {
    const auto wide = tone({{"stretch", 100}});
    const double f2 = 500.0 * std::pow(2.0, 1.3);
    CHECK(amplitudeAt(wide, f2, 48000) > 0.2);
    CHECK(amplitudeAt(wide, 1000.0, 48000) < 0.05);
    const auto narrow = tone({{"stretch", -100}});
    CHECK(amplitudeAt(narrow, 500.0 * std::pow(2.0, 0.7), 48000) > 0.2);
}

TEST_CASE("harmonic: the comb's notches, and Shift slides them") {
    const auto combed = tone({{"comb", 100}, {"comb_spacing", 4}});
    CHECK(partial(combed, 3) < 1e-6 * partial(combed, 1));
    CHECK(partial(combed, 7) < 1e-6 * partial(combed, 1));
    CHECK(partial(combed, 5) > 0.1 * partial(combed, 1));
    const auto slid = tone({{"comb", 100}, {"comb_spacing", 4}, {"comb_shift", 25}});
    CHECK(partial(slid, 4) < 1e-6 * partial(slid, 1));
    CHECK(partial(slid, 3) > 0.1 * partial(slid, 1));
    // A whole spacing on is where it started.
    const auto round = tone({{"comb", 100}, {"comb_spacing", 4}, {"comb_shift", 100}});
    CHECK(partial(round, 3) < 1e-6 * partial(round, 1));
}

TEST_CASE("harmonic: partials past half the sample rate are left out") {
    // 8500 Hz: only its second partial fits; the third would alias to 22500 Hz.
    const auto high = tone({{"partials", 32}, {"brightness", 100}}, "out", 8500.0);
    CHECK(inharmonicDb(high, 1700) < -120);
    CHECK(std::sqrt(binPower(high, 1700)) > 0.1 * 9600);
}

TEST_CASE("harmonic: poly, a voice per channel") {
    ModuleHarness h(stock("harmonic"));
    h.set("partials", 1);
    h.input("pitch", 2, [](int c, std::int64_t) { return c == 0 ? 0.0 : 1.0; });
    h.run(48000);
    REQUIRE(h.channels("out") == 2);
    CHECK(frequency(h.output("out", 1), 48000) == doctest::Approx(2 * kC4).epsilon(1e-3));
}
