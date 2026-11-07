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
double sine(std::int64_t n, double hz) { return std::sin(2 * kPi * hz * n / kRate); }
// A pair: a 220 Hz tone in both, a 330 Hz one only on the left.
double left(std::int64_t n) { return 0.4 * sine(n, 220) + 0.4 * sine(n, 330); }
double right(std::int64_t n) { return 0.4 * sine(n, 220); }
void feed(ModuleHarness& h) {
    h.input("in_l", 1, [](int, std::int64_t n) { return left(n); });
    h.input("in_r", 1, [](int, std::int64_t n) { return right(n); });
}
}  // namespace

TEST_CASE("width: 100 passes the pair, 0 folds it to mono, 200 doubles the side") {
    for (double w : {0.0, 100.0, 200.0}) {
        CAPTURE(w);
        ModuleHarness h(stock("width"));
        h.set("width", w);
        feed(h);
        h.run(4096);
        const auto& l = h.output("out_l");
        const auto& r = h.output("out_r");
        const auto& m = h.output("mid");
        const auto& s = h.output("side");
        // After the 20 ms smoothing has settled.
        for (std::int64_t n = 2048; n < 4096; n += 37) {
            const double mid = 0.5 * (left(n) + right(n));
            const double side = 0.5 * (left(n) - right(n)) * w / 100.0;
            CHECK(m[n] == doctest::Approx(mid));
            CHECK(s[n] == doctest::Approx(side));
            CHECK(l[n] == doctest::Approx(mid + side));
            CHECK(r[n] == doctest::Approx(mid - side));
        }
    }
}

TEST_CASE("width: a mono input has no side and passes at any width") {
    ModuleHarness h(stock("width"));
    h.set("width", 200);
    h.input("in_l", 1, [](int, std::int64_t n) { return 0.5 * sine(n, 440); });
    h.run(2048);
    for (std::int64_t n = 0; n < 2048; n += 13) {
        CHECK(h.output("out_l")[n] == doctest::Approx(0.5 * sine(n, 440)));
        CHECK(h.output("out_r")[n] == doctest::Approx(0.5 * sine(n, 440)));
        CHECK(h.output("side")[n] == doctest::Approx(0.0));
    }
}

TEST_CASE("width: Mono Below takes the bass out of the side, not the treble") {
    ModuleHarness h(stock("width"));
    h.set("mono_below", 200);
    // Opposite on the two sides: all side, no mid. 50 Hz below, 3 kHz above.
    auto difference = [](std::int64_t n) { return 0.3 * sine(n, 50) + 0.3 * sine(n, 3000); };
    h.input("in_l", 1, [&](int, std::int64_t n) { return difference(n); });
    h.input("in_r", 1, [&](int, std::int64_t n) { return -difference(n); });
    h.run(48000);
    const auto tail = slice(h.output("side"), 24000, 48000);
    CHECK(amplitudeAt(tail, 50, kRate) < 0.3 * 0.1);     // more than 20 dB down
    CHECK(amplitudeAt(tail, 3000, kRate) > 0.3 * 0.95);  // within half a dB
    // The mid is untouched: still none.
    CHECK(peak(slice(h.output("mid"), 24000, 48000)) < 1e-12);
}

TEST_CASE("width: a second Width in Mid/Side rebuilds the pair from Mid and Side") {
    ModuleHarness split(stock("width"));
    feed(split);
    split.run(2048);
    ModuleHarness join(stock("width"));
    join.set("mode", 1);
    const auto& m = split.output("mid");
    const auto& s = split.output("side");
    join.input("in_l", 1, [&](int, std::int64_t n) { return m[n]; });
    join.input("in_r", 1, [&](int, std::int64_t n) { return s[n]; });
    join.run(2048);
    for (std::int64_t n = 0; n < 2048; n += 11) {
        CHECK(join.output("out_l")[n] == doctest::Approx(left(n)));
        CHECK(join.output("out_r")[n] == doctest::Approx(right(n)));
    }
    // With nothing in In R it has no side: both outputs are the mid.
    ModuleHarness alone(stock("width"));
    alone.set("mode", 1);
    alone.input("in_l", 1, [](int, std::int64_t n) { return 0.5 * sine(n, 440); });
    alone.run(512);
    CHECK(alone.output("out_l")[300] == doctest::Approx(alone.output("out_r")[300]));
    CHECK(alone.output("out_l")[300] == doctest::Approx(0.5 * sine(300, 440)));
}

TEST_CASE("width: poly channels are each their own pair") {
    ModuleHarness h(stock("width"));
    h.set("width", 0);
    h.input("in_l", 2, [](int c, std::int64_t) { return c == 0 ? 0.6 : 0.2; });
    h.input("in_r", 2, [](int c, std::int64_t) { return c == 0 ? 0.2 : -0.2; });
    h.run(256);
    CHECK(h.channels("out_l") == 2);
    CHECK(h.output("out_l", 0)[200] == doctest::Approx(0.4));
    CHECK(h.output("out_r", 0)[200] == doctest::Approx(0.4));
    CHECK(h.output("out_l", 1)[200] == doctest::Approx(0.0));
}
