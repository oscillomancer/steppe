// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

double tone(std::int64_t n, double hz = 440.0) {
    return 0.5 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}
// A rich, steady test signal: three partials that are not harmonics.
double chord(std::int64_t n) {
    return 0.3 * std::sin(2 * kPi * 311.0 * n / 48000.0) +
           0.2 * std::sin(2 * kPi * 1237.0 * n / 48000.0 + 1.0) +
           0.1 * std::sin(2 * kPi * 5003.0 * n / 48000.0 + 2.0);
}
// The delay from the input to the output: where a click comes out.
std::size_t latency() {
    ModuleHarness h(stock("freeze"));
    h.input("in", 1, [](int, std::int64_t n) { return n == 0 ? 1.0 : 0.0; });
    h.run(24000);
    const auto& y = h.output("out");
    std::size_t at = 0;
    for (std::size_t n = 0; n < y.size(); ++n)
        if (std::abs(y[n]) > std::abs(y[at])) at = n;
    return at;
}
// A 440 Hz tone for the first second, then silence; the gate high from
// `on` to `off` samples.
std::vector<double> held(Settings settings, std::int64_t on, std::int64_t off, int frames) {
    ModuleHarness h(stock("freeze"));
    for (const auto& [key, value] : settings) h.set(key, value);
    h.input("in", 1, [](int, std::int64_t n) { return n < 48000 ? tone(n) : 0.0; });
    h.input("freeze_gate", 1,
            [on, off](int, std::int64_t n) { return n >= on && n < off ? 1.0 : 0.0; });
    h.run(frames);
    return h.output("out");
}
}  // namespace

TEST_CASE("freeze: left alone, the sound comes back as it went in, about 85 ms late") {
    const std::size_t late = latency();
    CHECK(late > 3000);
    CHECK(late < 4500);
    ModuleHarness h(stock("freeze"));
    h.input("in", 1, [](int, std::int64_t n) { return chord(n); });
    h.run(48000);
    const auto& y = h.output("out");
    double worst = 0.0;
    for (std::size_t n = 8192; n < 48000; ++n)
        worst = std::max(worst, std::abs(y[n] - chord(static_cast<std::int64_t>(n - late))));
    CHECK(worst < 1e-6);
}

TEST_CASE("freeze: the gate holds the spectrum, a steady tone at its pitch and level") {
    // Frozen half a second in, long after the tone stops it plays on.
    const auto y = held({}, 24000, 1 << 30, 48000 * 3);
    const auto late = slice(y, 48000 * 2);
    CHECK(frequency(late, 48000) == doctest::Approx(440.0).epsilon(0.001));
    CHECK(amplitudeAt(late, 440.0, 48000) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(rms(slice(late, 0, 12000)) ==
          doctest::Approx(rms(slice(late, 12000, 24000))).epsilon(0.01));
    // Not frozen: silence once the tone has gone through.
    const auto none = held({}, 1 << 30, 1 << 30, 48000 * 2);
    CHECK(peak(slice(none, 48000 + 9000)) < 1e-6);
    // Let go: the hold stops.
    const auto freed = held({}, 24000, 72000, 48000 * 3);
    CHECK(peak(slice(freed, 72000 + 9000)) < 1e-6);
    // A chord keeps every partial at its level.
    ModuleHarness c(stock("freeze"));
    c.input("in", 1, [](int, std::int64_t n) { return n < 48000 ? chord(n) : 0.0; });
    c.input("freeze_gate", 1, [](int, std::int64_t n) { return n >= 24000 ? 1.0 : 0.0; });
    c.run(48000 * 3);
    const auto pad = slice(c.output("out"), 48000 * 2);
    CHECK(amplitudeAt(pad, 311.0, 48000) == doctest::Approx(0.3).epsilon(0.05));
    CHECK(amplitudeAt(pad, 1237.0, 48000) == doctest::Approx(0.2).epsilon(0.05));
    CHECK(amplitudeAt(pad, 5003.0, 48000) == doctest::Approx(0.1).epsilon(0.05));
    // The Freeze switch does as the gate does.
    ModuleHarness h(stock("freeze"));
    h.input("in", 1, [](int, std::int64_t n) { return n < 48000 ? tone(n) : 0.0; });
    h.run(24000);
    h.set("freeze", 1);
    h.run(48000 * 2);
    CHECK(amplitudeAt(slice(h.output("out"), 48000 * 2), 440.0, 48000) ==
          doctest::Approx(0.5).epsilon(0.05));
}

TEST_CASE("freeze: Shift moves the partials, live and frozen") {
    for (double st : {12.0, -12.0, 7.0}) {
        CAPTURE(st);
        const double to = 440.0 * std::exp2(st / 12.0);
        const auto live = held({{"shift", st}}, 1 << 30, 1 << 30, 48000);
        const auto mid = slice(live, 12000, 24000);
        CHECK(frequency(mid, 48000) == doctest::Approx(to).epsilon(0.002));
        CHECK(amplitudeAt(mid, to, 48000) == doctest::Approx(0.5).epsilon(0.05));
        CHECK(amplitudeAt(mid, 440.0, 48000) < 0.02);
        const auto frozen = held({{"shift", st}}, 24000, 1 << 30, 48000 * 3);
        const auto late = slice(frozen, 48000 * 2);
        CHECK(frequency(late, 48000) == doctest::Approx(to).epsilon(0.002));
        CHECK(amplitudeAt(late, to, 48000) == doctest::Approx(0.5).epsilon(0.05));
    }
}

TEST_CASE("freeze: Blur smears in time; Tilt sets the balance by octaves") {
    // Blurred, the tone fades out over seconds rather than stopping.
    const auto smeared = held({{"blur", 60}}, 1 << 30, 1 << 30, 48000 * 2);
    CHECK(rms(slice(smeared, 48000 + 9000, 48000 + 21000)) > 0.05);
    CHECK(rms(slice(smeared, 48000 + 9000, 48000 + 21000)) < 0.5 / std::sqrt(2.0));
    // And it comes in slowly too.
    CHECK(rms(slice(smeared, 4096, 8896)) < 0.5 * rms(slice(smeared, 40000, 44800)));

    // +6 dB per octave about 1 kHz: 250 Hz 12 dB down, 4 kHz 12 dB up.
    ModuleHarness h(stock("freeze"));
    h.set("tilt", 6);
    h.input("in", 1, [](int, std::int64_t n) { return tone(n, 250.0) + tone(n, 4000.0); });
    h.run(48000);
    const auto y = slice(h.output("out"), 24000);
    CHECK(amplitudeAt(y, 250.0, 48000) == doctest::Approx(0.5 / 3.981).epsilon(0.05));
    CHECK(amplitudeAt(y, 4000.0, 48000) == doctest::Approx(0.5 * 3.981).epsilon(0.05));
}

TEST_CASE("freeze: Mix blends the dry undelayed; poly channels freeze on their own") {
    ModuleHarness d(stock("freeze"));
    d.set("mix", 0);
    d.input("in", 1, [](int, std::int64_t n) { return chord(n); });
    d.run(4800);
    for (int n = 0; n < 4800; n += 97) CHECK(d.output("out")[n] == doctest::Approx(chord(n)));

    ModuleHarness h(stock("freeze"));
    h.input("in", 2, [](int c, std::int64_t n) {
        return n < 48000 ? tone(n, c == 0 ? 440.0 : 660.0) : 0.0;
    });
    h.input("freeze_gate", 2,
            [](int c, std::int64_t n) { return c == 0 && n >= 24000 ? 1.0 : 0.0; });
    h.run(48000 * 2 + 12000);
    CHECK(h.channels("out") == 2);
    CHECK(amplitudeAt(slice(h.output("out", 0), 48000 * 2), 440.0, 48000) > 0.4);
    CHECK(peak(slice(h.output("out", 1), 48000 * 2)) < 1e-6);
}

TEST_CASE("freeze: within its budget at any rate, sixteen channels, a frame at a time") {
    // In a feedback loop a module runs one frame per call, and every
    // channel's spectrum comes due on the same frame.
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        CAPTURE(sr);
        ModuleHarness h(stock("freeze"), sr);
        h.set("shift", 7);
        h.set("blur", 40);
        h.input("in", 16, [sr](int c, std::int64_t n) {
            return std::sin(2 * kPi * (200.0 + 50 * c) * static_cast<double>(n) / sr);
        });
        h.input("freeze_gate", 16,
                [](int c, std::int64_t n) { return c % 2 == 0 && n > 9000 ? 1.0 : 0.0; });
        for (int n = 0; n < 20000; ++n) h.run(1);
        CHECK(h.status().budgetExceeded.load() == 0);
        CHECK(peak(slice(h.output("out", 15), 12000)) > 0.3);
    }
}
