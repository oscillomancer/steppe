// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
constexpr double kRate = 48000.0;
double db(double v) { return std::pow(10.0, v / 20.0); }
double sine(std::int64_t n, double hz) { return std::sin(2 * kPi * hz * n / kRate); }
double ms(double t) { return t * 0.001 * kRate; }
// The peak of an output between two times in ms.
double peakBetween(const std::vector<double>& y, double from, double to) {
    return peak(slice(y, static_cast<std::size_t>(ms(from)), static_cast<std::size_t>(ms(to))));
}
}  // namespace

TEST_CASE("noisegate: open over Thresh, closed below by Range, after Hold") {
    ModuleHarness h(stock("noisegate"));
    h.set("threshold", -30);
    h.set("hold", 20);
    h.set("release", 5);
    h.set("range", -40);
    // A 1 kHz tone at -12 dB for 100 ms, then at -50 (below the close at -36).
    h.input("in", 1,
            [](int, std::int64_t n) { return (n < ms(100) ? db(-12) : db(-50)) * sine(n, 1000); });
    h.run(static_cast<std::int64_t>(ms(300)));
    const auto& out = h.output("out");
    const auto& open = h.output("open");
    CHECK(peakBetween(out, 20, 100) == doctest::Approx(db(-12)).epsilon(0.01));
    // The follower falls 1 dB a millisecond, so it is below the close (-36)
    // 24 ms after the drop, and held open 20 ms more: closed at about 144.
    CHECK(open[static_cast<std::size_t>(ms(115))] == 1.0);
    CHECK(open[static_cast<std::size_t>(ms(138))] == 1.0);
    CHECK(peakBetween(out, 101, 112) == doctest::Approx(db(-50)).epsilon(0.02));
    CHECK(open[static_cast<std::size_t>(ms(150))] == 0.0);
    // Then down by Range: -50 - 40.
    CHECK(peakBetween(out, 200, 300) == doctest::Approx(db(-90)).epsilon(0.02));
    CHECK(h.output("gain")[static_cast<std::size_t>(ms(250))] == doctest::Approx(db(-40)));
    int opens = 0;
    for (std::size_t n = 1; n < open.size(); ++n) opens += open[n] > 0.5 && open[n - 1] < 0.5;
    CHECK(opens == 1);
}

TEST_CASE("noisegate: hysteresis keeps it from chattering between the thresholds") {
    // A level wobbling between -33 and -27 around a Thresh of -30.
    auto wobbling = [](int, std::int64_t n) {
        const double level = -30 + 3 * sine(n, 5);
        return db(level) * sine(n, 1000);
    };
    auto opensWith = [&](double hysteresis) {
        ModuleHarness h(stock("noisegate"));
        h.set("threshold", -30);
        h.set("hysteresis", hysteresis);
        h.set("hold", 0);
        h.input("in", 1, wobbling);
        h.run(48000);
        const auto& open = h.output("open");
        int opens = 0;
        for (std::size_t n = 1; n < open.size(); ++n) opens += open[n] > 0.5 && open[n - 1] < 0.5;
        return opens;
    };
    // Without it, it opens on each of the five swells a second; with 6 dB,
    // once, and stays open.
    CHECK(opensWith(0) >= 5);
    CHECK(opensWith(6) == 1);
}

TEST_CASE("noisegate: a range of -80 is silence, 0 leaves the sound alone") {
    ModuleHarness shut(stock("noisegate"));
    shut.input("in", 1, [](int, std::int64_t n) { return db(-70) * sine(n, 500); });
    shut.run(4800);
    CHECK(peak(shut.output("out")) == 0.0);

    ModuleHarness through(stock("noisegate"));
    through.set("range", 0);
    through.input("in", 1, [](int, std::int64_t n) { return db(-70) * sine(n, 500); });
    through.run(4800);
    CHECK(peak(slice(through.output("out"), 2400, 4800)) == doctest::Approx(db(-70)).epsilon(0.01));
}

TEST_CASE("noisegate: Key opens it instead of In, through its filters") {
    // A held pad at -20 dB, keyed by a kick-like 60 Hz burst every 250 ms,
    // with a 4 kHz hat at the same level between.
    auto pad = [](int, std::int64_t n) { return db(-20) * sine(n, 300); };
    auto key = [](int, std::int64_t n) {
        const std::int64_t t = n % static_cast<std::int64_t>(ms(250));
        if (t < ms(30)) return 0.8 * sine(n, 60);
        if (t >= ms(125) && t < ms(155)) return 0.8 * sine(n, 4000);
        return 0.0;
    };
    ModuleHarness h(stock("noisegate"));
    h.set("threshold", -30);
    h.set("hold", 10);
    h.set("release", 5);
    h.set("key_lp", 200);
    h.input("in", 1, pad);
    h.input("sidechain", 1, key);
    h.run(static_cast<std::int64_t>(ms(500)));
    const auto& out = h.output("out");
    // Open through the kick (after its first quarter cycle at 60 Hz).
    CHECK(peakBetween(out, 260, 280) == doctest::Approx(db(-20)).epsilon(0.02));
    // Shut through the hat: the low pass keeps it from being heard.
    CHECK(peakBetween(out, 380, 405) < db(-80));

    // Without the low pass the hat opens it too.
    ModuleHarness open(stock("noisegate"));
    open.set("threshold", -30);
    open.set("hold", 10);
    open.set("release", 5);
    open.input("in", 1, pad);
    open.input("sidechain", 1, key);
    open.run(static_cast<std::int64_t>(ms(500)));
    CHECK(peakBetween(open.output("out"), 380, 405) == doctest::Approx(db(-20)).epsilon(0.02));
}

TEST_CASE("noisegate: linked, the channels open together; unlinked, each alone") {
    auto loudAndQuiet = [](int c, std::int64_t n) {
        return (c == 0 ? 0.5 : db(-70)) * sine(n, 700);
    };
    ModuleHarness linked(stock("noisegate"));
    linked.input("in", 2, loudAndQuiet);
    linked.run(4800);
    CHECK(linked.channels("open") == 1);
    CHECK(peak(slice(linked.output("out", 1), 2400, 4800)) ==
          doctest::Approx(db(-70)).epsilon(0.01));

    ModuleHarness alone(stock("noisegate"));
    alone.set("link", 0);
    alone.input("in", 2, loudAndQuiet);
    alone.run(4800);
    CHECK(alone.channels("open") == 2);
    CHECK(alone.output("open", 0)[4000] == 1.0);
    CHECK(alone.output("open", 1)[4000] == 0.0);
    CHECK(peak(alone.output("out", 1)) == 0.0);
}
