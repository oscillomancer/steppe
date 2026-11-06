// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;
using Wave = std::function<double(std::int64_t)>;

double sine(std::int64_t n, double hz, double a = 0.5) {
    return a * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}
// A full-scale saw, rich in harmonics: a synth carrier.
double saw(std::int64_t n, double hz) {
    const double t = hz * static_cast<double>(n) / 48000.0;
    return 2.0 * (t - std::floor(t)) - 1.0;
}
// White noise from a hash of n, -1..1.
double hash(std::int64_t n) {
    std::uint32_t x = static_cast<std::uint32_t>(n) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return static_cast<double>(x >> 8) / 8388608.0 - 1.0;
}
// Something like speech: a 140 Hz buzz through two vowel formants, words of
// 250 ms with gaps of 100 ms between.
double speech(std::int64_t n) {
    const std::int64_t k = n % 16800;
    if (k >= 12000) return 0.0;
    const double env = std::sin(kPi * static_cast<double>(k) / 12000.0);
    return 0.5 * env *
           (0.6 * sine(n, 700, 1) * (0.5 + 0.5 * saw(n, 140)) +
            0.4 * sine(n, 1200, 1) * (0.5 + 0.5 * saw(n, 140)));
}

// The power of y within a tenth either side of hz: a Hann-windowed spectrum
// sampled across the band, for noisy sounds; times hz, so that bands a
// tenth wide compare by their energy.
double around(const std::vector<double>& y, double hz) {
    double sum = 0.0;
    for (int k = -20; k <= 20; ++k) {
        const double a = amplitudeAt(y, hz * (1.0 + 0.005 * k), 48000);
        sum += a * a;
    }
    return hz * sum / 41.0;
}
double dbOver(double a, double b) { return 10.0 * std::log10(a / b); }

std::vector<double> vocode(Settings settings, Wave v, Wave c, int frames = 48000) {
    ModuleHarness h(stock("vocoder"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("voice", 1, [v](int, std::int64_t n) { return v(n); });
    h.input("carrier", 1, [c](int, std::int64_t n) { return c(n); });
    h.run(frames);
    return h.output("out");
}
}  // namespace

TEST_CASE("vocoder: the voice's bands open the carrier's, where the voice is") {
    const auto y =
        slice(vocode({{"noise", 0}}, [](std::int64_t n) { return sine(n, 1000); }, hash), 12000);
    const double at = around(y, 1000), below = around(y, 300), above = around(y, 4000);
    // Each band two band passes, falling 12 dB an octave: two octaves away
    // well down, and further at Width 0.
    CHECK(dbOver(at, below) > 24);
    CHECK(dbOver(at, above) > 20);
    const auto narrow =
        slice(vocode(
                  {{"noise", 0}, {"width", 0}}, [](std::int64_t n) { return sine(n, 1000); }, hash),
              12000);
    CHECK(dbOver(around(narrow, 1000), around(narrow, 300)) > dbOver(at, below) + 6);
    CHECK(dbOver(around(narrow, 1000), around(narrow, 4000)) > dbOver(at, above) + 6);
    // Shift +12 moves the carrier's bands an octave up against the voice's.
    const auto up = slice(
        vocode(
            {{"noise", 0}, {"shift", 12}}, [](std::int64_t n) { return sine(n, 1000); }, hash),
        12000);
    CHECK(dbOver(around(up, 2000), around(up, 1000)) > 15);
}

TEST_CASE("vocoder: the carrier's pitch comes through, shaped by the voice") {
    const auto y = slice(vocode({}, hash, [](std::int64_t n) { return saw(n, 220); }), 12000);
    CHECK(peakHz(slice(y, 0, 9600), 150, 300, 48000, 1) == doctest::Approx(220).epsilon(0.01));
    // Its harmonics, up to the top band, all there.
    CHECK(amplitudeAt(y, 220 * 10, 48000) > 0.1 * amplitudeAt(y, 220, 48000));
}

TEST_CASE("vocoder: silent without a voice; Noise feeds only the upper bands") {
    CHECK(peak(vocode(
              {{"noise", 100}}, [](std::int64_t) { return 0.0; },
              [](std::int64_t n) { return saw(n, 110); })) == 0.0);
    CHECK(peak(vocode({{"noise", 0}}, hash, [](std::int64_t) { return 0.0; })) == 0.0);
    const auto y = slice(vocode({{"noise", 100}}, hash, [](std::int64_t) { return 0.0; }), 12000);
    CHECK(rms(y) > 0.01);
    CHECK(dbOver(around(y, 5000), around(y, 300)) > 30);
}

TEST_CASE("vocoder: Release lets the bands ring on after the voice stops") {
    const auto burst = [](std::int64_t n) { return n < 24000 ? sine(n, 800) : 0.0; };
    const auto noisy = [](std::int64_t n) { return 0.5 * hash(n); };
    const auto shortTail = vocode({{"noise", 0}, {"release", 20}}, burst, noisy);
    const auto longTail = vocode({{"noise", 0}, {"release", 400}}, burst, noisy);
    // 100 to 200 ms after: the short one gone, the long one still sounding.
    CHECK(rms(slice(shortTail, 28800, 33600)) < 0.01 * rms(slice(shortTail, 19200, 24000)));
    CHECK(rms(slice(longTail, 28800, 33600)) > 0.3 * rms(slice(longTail, 19200, 24000)));
}

TEST_CASE("vocoder: levels at its defaults; Level after") {
    // A voice peaking at -6 dB and a full-scale saw: out at about the
    // voice's own level, whatever the bands.
    for (int bands : {4, 16, 32}) {
        CAPTURE(bands);
        const auto y =
            vocode({{"bands", bands}}, speech, [](std::int64_t n) { return saw(n, 110); }, 96000);
        const double p = peak(y), db = 20 * std::log10(rms(slice(y, 4800)));
        CAPTURE(p);
        CAPTURE(db);
        CHECK(p < 1.0);
        CHECK(db > -24);
        CHECK(db < -16);
    }
    const auto loud = vocode({}, speech, [](std::int64_t n) { return saw(n, 110); });
    // Width, too, leaves the level about where it is.
    for (double w : {0.0, 100.0}) {
        CAPTURE(w);
        const auto y = vocode({{"width", w}}, speech, [](std::int64_t n) { return saw(n, 110); });
        CHECK(std::abs(20 * std::log10(rms(slice(y, 4800)) / rms(slice(loud, 4800)))) < 3);
    }
    // A pure tone, all in one band, is the loudest voice: still in bounds.
    const auto tone = vocode(
        {}, [](std::int64_t n) { return sine(n, 1000); },
        [](std::int64_t n) { return saw(n, 110); });
    CHECK(peak(tone) < 2.0);
    const auto soft = vocode({{"level", -12}}, speech, [](std::int64_t n) { return saw(n, 110); });
    CHECK(20 * std::log10(rms(slice(soft, 4800)) / rms(slice(loud, 4800))) ==
          doctest::Approx(-12).epsilon(0.01));
}

TEST_CASE("vocoder: a poly carrier is summed into one output") {
    ModuleHarness h(stock("vocoder"));
    h.set("noise", 0);
    h.input("voice", 1, [](int, std::int64_t n) { return hash(n); });
    h.input("carrier", 3, [](int c, std::int64_t n) { return c == 1 ? saw(n, 220) : 0.0; });
    h.run(24000);
    CHECK(h.channels("out") == 1);
    const auto mono =
        vocode({{"noise", 0}}, hash, [](std::int64_t n) { return saw(n, 220); }, 24000);
    for (std::size_t n = 0; n < 24000; n += 37)
        CHECK(h.output("out")[n] == doctest::Approx(mono[n]));
}
