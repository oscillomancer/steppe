// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
double dbOf(double x) { return 20 * std::log10(std::max(x, 1e-30)); }
// A short burst of noise, as a clap.
ModuleHarness::Signal burst() {
    return [](int, std::int64_t n) {
        if (n >= 480) return 0.0;
        std::uint32_t h = static_cast<std::uint32_t>(n) * 2654435761u;
        h ^= h >> 13;
        return (static_cast<double>(h & 0xffff) / 32768.0 - 1.0) * 0.5;
    };
}
ModuleHarness::Signal noise() {
    return [](int, std::int64_t n) {
        std::uint32_t h = static_cast<std::uint32_t>(n) * 2654435761u + 12345u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        return (static_cast<double>(h & 0xffff) / 32768.0 - 1.0) * 0.3;
    };
}
}  // namespace

TEST_CASE("reverb: the tail falls 60 dB in the decay time") {
    for (double decay : {1000.0, 4000.0}) {
        CAPTURE(decay);
        ModuleHarness h(stock("reverb"));
        h.set("decay", decay);
        h.set("damping", 0);
        h.set("modulation", 0);
        h.set("mix", 100);
        h.input("in_l", 1, burst());
        h.run(static_cast<std::int64_t>(48 * decay));
        const auto& y = h.output("out_l");
        // Level from 20 % to 60 % of the decay time.
        const std::size_t a = static_cast<std::size_t>(48 * decay * 0.2),
                          b = static_cast<std::size_t>(48 * decay * 0.6), w = 2400;
        const double drop = dbOf(rms(slice(y, a, a + w))) - dbOf(rms(slice(y, b, b + w)));
        CHECK(drop / 0.4 == doctest::Approx(60).epsilon(0.15));
    }
}

TEST_CASE("reverb: pre-delay keeps the start clear, stereo is wide") {
    ModuleHarness h(stock("reverb"));
    h.set("pre_delay", 100);
    h.set("mix", 100);
    h.input("in_l", 1, [](int, std::int64_t n) { return n == 0 ? 1.0 : 0.0; });
    h.run(48000);
    CHECK(peak(slice(h.output("out_l"), 0, 4800)) < 1e-9);
    CHECK(peak(slice(h.output("out_l"), 4800, 24000)) > 0.01);

    ModuleHarness s(stock("reverb"));
    s.set("mix", 100);
    s.input("in_l", 1, noise());
    s.run(96000);
    const auto l = slice(s.output("out_l"), 48000), r = slice(s.output("out_r"), 48000);
    double lr = 0, ll = 0, rr = 0;
    for (std::size_t n = 0; n < l.size(); ++n) {
        lr += l[n] * r[n];
        ll += l[n] * l[n];
        rr += r[n] * r[n];
    }
    CHECK(std::abs(lr / std::sqrt(ll * rr)) < 0.3);
    // White noise is mostly highs, which Damping takes: it comes back some
    // 8 dB under what goes in.
    const double level = dbOf(rms(l)) - dbOf(0.3 / std::sqrt(3.0));
    CHECK(level > -12);
    CHECK(level < -4);
    // A held tone with a saw's harmonics, more like music, about as loud.
    ModuleHarness t(stock("reverb"));
    t.set("mix", 100);
    const auto held = [](int, std::int64_t n) {
        double sum = 0.0;
        for (int k = 1; k <= 40; ++k)
            sum += std::sin(2 * kPi * k * 110.0 * static_cast<double>(n) / 48000.0 + k * k) / k;
        return 0.3 * sum;
    };
    t.input("in_l", 1, held);
    t.run(96000);
    std::vector<double> x(48000);
    for (std::size_t n = 0; n < x.size(); ++n) x[n] = held(0, static_cast<std::int64_t>(n));
    CHECK(std::abs(dbOf(rms(slice(t.output("out_l"), 48000))) - dbOf(rms(x))) < 3);
}

TEST_CASE("reverb: freeze holds the tail and lets nothing in") {
    ModuleHarness h(stock("reverb"));
    h.set("mix", 100);
    h.input("in_l", 1, noise());
    h.input("freeze_gate", 1, [](int, std::int64_t n) { return n >= 48000 ? 1.0 : 0.0; });
    h.run(48000 * 6);
    const auto& y = h.output("out_l");
    const double a = rms(slice(y, 48000 * 2, 48000 * 3)), b = rms(slice(y, 48000 * 5));
    CHECK(a > 0.05);
    CHECK(dbOf(b) - dbOf(a) == doctest::Approx(0).epsilon(1.5));
}

TEST_CASE("reverb: shimmer adds the octave above") {
    double octave[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        ModuleHarness h(stock("reverb"));
        h.set("mix", 100);
        h.set("shimmer", k == 0 ? 0 : 100);
        h.set("damping", 10);
        h.input("in_l", 1,
                [](int, std::int64_t n) { return 0.3 * std::sin(2 * kPi * 300 * n / 48000.0); });
        h.run(96000);
        const auto y = slice(h.output("out_l"), 48000);
        octave[k] = amplitudeAt(y, 600, 48000) / amplitudeAt(y, 300, 48000);
    }
    CHECK(octave[1] > 4 * octave[0]);
    CHECK(octave[1] > 0.1);
}

TEST_CASE("reverb: bounded at its extremes") {
    ModuleHarness h(stock("reverb"));
    h.set("decay", 30000);
    h.set("shimmer", 100);
    h.set("size", 100);
    h.set("diffusion", 100);
    h.set("modulation", 100);
    h.set("damping", 0);
    h.set("mix", 100);
    h.input("in_l", 1, [](int, std::int64_t n) { return std::sin(n * 0.05) > 0 ? 1.0 : -1.0; });
    h.run(48000 * 20);
    CHECK(peak(h.output("out_l")) < 15.0);
}
