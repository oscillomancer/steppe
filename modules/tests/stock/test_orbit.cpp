// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

double tone(std::int64_t n, double hz = 1000.0) {
    return 0.5 * std::sin(2 * kPi * hz * static_cast<double>(n) / 48000.0);
}
std::string speaker(int k) { return "out" + std::to_string(k); }
std::unique_ptr<ModuleHarness> orbit(Settings settings, int frames, double hz = 1000.0) {
    auto h = std::make_unique<ModuleHarness>(stock("orbit"));
    h->set("doppler", 0);
    for (const auto& [key, value] : settings) h->set(key, value);
    h->input("in", 1, [hz](int, std::int64_t n) { return tone(n, hz); });
    h->run(frames);
    return h;
}
// Speaker k's gain for the tone over [from, from + 4800).
double gain(const ModuleHarness& h, int k, std::size_t from = 9600) {
    return amplitudeAt(slice(h.output(speaker(k).c_str()), from, from + 4800), 1000.0, 48000) / 0.5;
}
}  // namespace

TEST_CASE("orbit: standing still, the sound is at a speaker or shared by the two beside it") {
    const double half = 1.0 / std::sqrt(2.0);
    struct Case {
        double angle;
        int speakers;
        std::array<double, 8> gains;
    };
    for (const Case& c :
         {Case{0, 0, {1, 0, 0, 0, 0, 0, 0, 0}}, Case{90, 0, {0, 1, 0, 0, 0, 0, 0, 0}},
          Case{45, 0, {half, half, 0, 0, 0, 0, 0, 0}}, Case{-90, 0, {0, 0, 0, 1, 0, 0, 0, 0}},
          Case{45, 1, {0, 1, 0, 0, 0, 0, 0, 0}}, Case{-22.5, 1, {half, 0, 0, 0, 0, 0, 0, half}}}) {
        CAPTURE(c.angle);
        CAPTURE(c.speakers);
        const auto h = orbit({{"rate", 0}, {"angle", c.angle}, {"speakers", c.speakers}}, 14400);
        for (int k = 1; k <= 8; ++k) {
            CAPTURE(k);
            CHECK(gain(*h, k) == doctest::Approx(c.gains[k - 1]).epsilon(1e-3));
        }
    }
}

TEST_CASE("orbit: Rate turns it clockwise, keeping the power, the other way below 0") {
    // A turn a second of four speakers: each loudest a quarter second after
    // the one before; the power over them all always the input's.
    for (double r : {1.0, -1.0}) {
        CAPTURE(r);
        ModuleHarness h(stock("orbit"));
        h.set("doppler", 0);
        h.set("rate", r);
        h.input("in", 1, [](int, std::int64_t) { return 1.0; });
        h.run(48000);
        std::array<std::size_t, 4> loudest{};
        for (int k = 0; k < 4; ++k) {
            const auto& y = h.output(speaker(k + 1).c_str());
            // Over three quarters of the turn: a whole one comes back to 1.
            for (std::size_t n = 0; n < 36000 + 200; ++n)
                if (y[n] > y[loudest[k]]) loudest[k] = n;
        }
        CHECK(loudest[0] < 200);
        const std::size_t quarter = 12000;
        CHECK(static_cast<double>(loudest[r > 0 ? 1 : 3]) ==
              doctest::Approx(quarter).epsilon(0.01));
        CHECK(static_cast<double>(loudest[2]) == doctest::Approx(2 * quarter).epsilon(0.01));
        for (std::size_t n = 0; n < 48000; n += 101) {
            double power = 0.0;
            for (int k = 1; k <= 8; ++k) power += std::pow(h.output(speaker(k).c_str())[n], 2);
            CHECK(power == doctest::Approx(1.0).epsilon(1e-9));
        }
    }
}

TEST_CASE("orbit: Width spreads it over every speaker; L and R fold the ring to stereo") {
    const auto wide = orbit({{"rate", 0}, {"width", 100}, {"speakers", 1}}, 14400);
    for (int k = 1; k <= 8; ++k)
        CHECK(gain(*wide, k) == doctest::Approx(1.0 / std::sqrt(8.0)).epsilon(1e-3));
    const auto half = orbit({{"rate", 0}, {"width", 50}}, 14400);
    CHECK(gain(*half, 1) == doctest::Approx(std::sqrt(0.5 + 0.5 / 4)).epsilon(1e-3));
    CHECK(gain(*half, 3) == doctest::Approx(std::sqrt(0.5 / 4)).epsilon(1e-3));

    const auto at = [](const ModuleHarness& h, const char* key) {
        return amplitudeAt(slice(h.output(key), 9600, 14400), 1000.0, 48000) / 0.5;
    };
    const auto leftOnly = orbit({{"rate", 0}, {"angle", -90}}, 14400);
    CHECK(at(*leftOnly, "left") == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(at(*leftOnly, "right") < 1e-6);
    const auto front = orbit({{"rate", 0}}, 14400);
    CHECK(at(*front, "left") == doctest::Approx(1.0 / std::sqrt(2.0)).epsilon(1e-3));
    CHECK(at(*front, "right") == doctest::Approx(1.0 / std::sqrt(2.0)).epsilon(1e-3));
}

TEST_CASE("orbit: Doppler bends the pitch as it turns; none without it") {
    // Two turns a second in the largest room: up to 18 % either way, highest
    // as it nears speaker 1, lowest as it leaves.
    const auto bent = [](double amount) {
        ModuleHarness h(stock("orbit"));
        h.set("rate", 2);
        h.set("doppler", amount);
        h.input("in", 1, [](int, std::int64_t n) { return tone(n); });
        h.run(48000 + 24000);
        double lo = 1e9, hi = 0.0;
        const auto& y = h.output("out1");
        // Only where speaker 1 is loud enough to measure.
        for (std::size_t n = 48000; n + 480 < y.size(); n += 240) {
            const auto part = slice(y, n, n + 480);
            if (rms(part) < 0.1) continue;
            const double f = frequency(part, 48000);
            lo = std::min(lo, f);
            hi = std::max(hi, f);
        }
        return std::make_pair(lo, hi);
    };
    const auto [lo, hi] = bent(100);
    CHECK(hi > 1100.0);
    CHECK(hi < 1250.0);
    CHECK(lo < 900.0);
    CHECK(lo > 800.0);
    const auto [flatLo, flatHi] = bent(0);
    CHECK(flatLo == doctest::Approx(1000.0).epsilon(0.01));
    CHECK(flatHi == doctest::Approx(1000.0).epsilon(0.01));
}

TEST_CASE("orbit: poly voices sit Spread apart; Reset turns back to the front") {
    ModuleHarness h(stock("orbit"));
    h.set("doppler", 0);
    h.set("rate", 0);
    h.input("in", 2, [](int c, std::int64_t n) { return tone(n, c == 0 ? 1000.0 : 1500.0); });
    h.run(14400);
    const auto out = [&h](int k, double hz) {
        return amplitudeAt(slice(h.output(speaker(k).c_str()), 9600, 14400), hz, 48000) / 0.5;
    };
    CHECK(out(1, 1000) == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(out(3, 1500) == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(out(1, 1500) < 1e-3);
    CHECK(h.channels("out1") == 1);

    ModuleHarness r(stock("orbit"));
    r.set("doppler", 0);
    r.set("rate", 1);
    r.input("in", 1, [](int, std::int64_t) { return 1.0; });
    r.input("reset", 1, [](int, std::int64_t n) { return n >= 30000 && n < 30048 ? 1.0 : 0.0; });
    r.run(31000);
    const auto& front = r.output("out1");
    CHECK(front[29990] < 0.1);  // at 0.625 of a turn: between 3 and 4
    CHECK(front[30001] == doctest::Approx(1.0).epsilon(1e-3));
}
