// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr std::int64_t kStep = 6000;  // a sixteenth at 120 beats a minute

void song(ModuleHarness& h) {
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    h.transport(t);
}

// A gate on every sixteenth, high for `length` frames of it.
void sixteenths(ModuleHarness& h, std::int64_t length = kStep / 2) {
    h.input("gate", 1, [length](int, std::int64_t n) { return n % kStep < length ? 1.0 : 0.0; });
}

std::vector<std::int64_t> rises(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 0; n < y.size(); ++n)
        if (y[n] >= 0.5 && (n == 0 || y[n - 1] < 0.5)) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

std::vector<std::int64_t> falls(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 1; n < y.size(); ++n)
        if (y[n] < 0.5 && y[n - 1] >= 0.5) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

}  // namespace

TEST_CASE("groove: by default the gates pass on time, the downbeats loudest") {
    ModuleHarness h(supple::rack::test::stock("groove"));
    song(h);
    sixteenths(h);
    h.run(kStep * 16);
    const auto r = rises(h.output("out"));
    REQUIRE(r.size() == 16);
    for (std::size_t s = 0; s < 16; ++s) CHECK(r[s] == static_cast<std::int64_t>(s) * kStep);
    const auto& v = h.output("velocity");
    auto at = [&](int s) { return v[static_cast<std::size_t>(s * kStep + 10)]; };
    CHECK(at(0) == doctest::Approx(0.85));
    CHECK(at(8) == doctest::Approx(0.85 * 0.9));
    CHECK(at(4) == doctest::Approx(0.85 * 0.8));
    CHECK(at(2) == doctest::Approx(0.85 * 0.7));
    CHECK(at(1) == doctest::Approx(0.85 * 0.6));
    CHECK(h.output("weight")[static_cast<std::size_t>(4 * kStep + 10)] == doctest::Approx(0.6));
    // Velocity holds through the note.
    CHECK(v[static_cast<std::size_t>(kStep - 10)] == doctest::Approx(0.85));
}

TEST_CASE("groove: Backbeat and Offbeats move the accents; Depth 0 makes them even") {
    auto velocities = [](int feelIndex, double depthPct) {
        ModuleHarness h(supple::rack::test::stock("groove"));
        h.set("feel", feelIndex);
        h.set("depth", depthPct);
        song(h);
        sixteenths(h);
        h.run(kStep * 16);
        std::vector<double> out;
        for (int s = 0; s < 16; ++s)
            out.push_back(h.output("velocity")[static_cast<std::size_t>(s * kStep + 10)]);
        return out;
    };
    const auto back = velocities(2, 100);
    CHECK(back[4] == doctest::Approx(0.85));
    CHECK(back[12] == doctest::Approx(0.85));
    CHECK(back[0] == doctest::Approx(0.85 * 0.7));
    CHECK(back[1] == doctest::Approx(0.85 * 0.2));
    const auto off = velocities(3, 100);
    CHECK(off[2] == doctest::Approx(0.85));
    CHECK(off[0] == doctest::Approx(0.85 * 0.5));
    for (double x : velocities(1, 0)) CHECK(x == doctest::Approx(0.85));
}

TEST_CASE("groove: Swing makes every second step late and keeps lengths; Lay Back all") {
    ModuleHarness h(supple::rack::test::stock("groove"));
    h.set("swing", 50);
    song(h);
    sixteenths(h, kStep / 4);
    h.run(kStep * 8);
    const auto r = rises(h.output("out"));
    const auto f = falls(h.output("out"));
    REQUIRE(r.size() == 8);
    REQUIRE(f.size() == 8);
    for (std::size_t s = 0; s < 8; ++s) {
        CAPTURE(s);
        const std::int64_t want =
            static_cast<std::int64_t>(s) * kStep + (s % 2 == 1 ? kStep / 2 : 0);
        CHECK(r[s] == want);
        CHECK(f[s] - r[s] == kStep / 4);
    }

    ModuleHarness e(supple::rack::test::stock("groove"));
    e.set("swing", 50);
    e.set("swing_on", 1);  // eighths: the offbeat a whole sixteenth late, its neighbours half
    e.set("lay_back", 10);
    song(e);
    sixteenths(e, kStep / 4);
    e.run(kStep * 8);
    const auto er = rises(e.output("out"));
    REQUIRE(er.size() == 8);
    const std::int64_t bent[] = {0, kStep / 2, kStep, kStep / 2};
    for (std::size_t s = 0; s < 8; ++s) {
        CAPTURE(s);
        CHECK(er[s] == static_cast<std::int64_t>(s) * kStep + 480 + bent[s % 4]);
    }
}

TEST_CASE("groove: a tied note into a swung one stays in order, dropping for a frame") {
    ModuleHarness h(supple::rack::test::stock("groove"));
    h.set("swing", 60);
    song(h);
    // Step 1 (swung) is held until a frame before step 2 (not swung) rises.
    h.input("gate", 1, [](int, std::int64_t n) {
        return (n >= kStep && n < 2 * kStep - 1) || (n >= 2 * kStep && n < 3 * kStep) ? 1.0 : 0.0;
    });
    h.run(kStep * 4);
    const auto& g = h.output("out");
    const auto r = rises(g);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == kStep + 3600);
    // The swung note's fall is due after step 2's rise: the rise waits for it.
    CHECK(r[1] > r[0]);
    CHECK(g[static_cast<std::size_t>(r[1] - 1)] == 0.0);
    CHECK(g[static_cast<std::size_t>(3 * kStep - 1)] == 1.0);
}

TEST_CASE("groove: Human varies time and level from the seed; Accent; a clock and Reset") {
    auto play = [](int seed, std::vector<double>& vel) {
        ModuleHarness h(supple::rack::test::stock("groove"), 48000.0, seed);
        h.set("human", 100);
        h.set("depth", 0);
        song(h);
        sixteenths(h);
        h.run(kStep * 16);
        const auto r = rises(h.output("out"));
        REQUIRE(r.size() == 16);
        std::vector<std::int64_t> late;
        for (std::size_t s = 0; s < 16; ++s) {
            late.push_back(r[s] - static_cast<std::int64_t>(s) * kStep);
            vel.push_back(h.output("velocity")[static_cast<std::size_t>(r[s])]);
        }
        return late;
    };
    std::vector<double> v1, v1b, v2;
    const auto a = play(1, v1);
    CHECK(play(1, v1b) == a);
    CHECK(v1b == v1);
    CHECK(play(2, v2) != a);
    std::set<std::int64_t> distinct(a.begin(), a.end());
    CHECK(distinct.size() > 8);
    for (auto d : a) {
        CHECK(d >= 0);
        CHECK(d <= 576);  // 12 ms
    }
    for (double x : v1) {
        CHECK(x >= 0.85 - 0.12 - 1e-9);
        CHECK(x <= 0.85 + 0.12 + 1e-9);
    }

    // A clock of 1000 frames: steps from its edges; Reset makes the next the first.
    ModuleHarness c(supple::rack::test::stock("groove"));
    c.set("feel", 2);  // backbeat
    c.set("depth", 100);
    c.input("clock", 1, [](int, std::int64_t n) { return n % 1000 < 10 ? 1.0 : 0.0; });
    c.input("gate", 2, [](int ch, std::int64_t n) {
        return ch == 0 ? (n % 1000 < 400 ? 1.0 : 0.0) : (n % 2000 < 400 ? 1.0 : 0.0);
    });
    c.input("accent", 1, [](int, std::int64_t n) { return n >= 1000 && n < 1100 ? 1.0 : 0.0; });
    c.input("reset", 1, [](int, std::int64_t n) { return n >= 5500 && n < 5600 ? 1.0 : 0.0; });
    c.run(1000 * 12);
    const auto& v = c.output("velocity", 0);
    CHECK(v[4010] == doctest::Approx(0.85));        // step 4: beat 2
    CHECK(v[1010] == doctest::Approx(0.85));        // step 1, accented
    CHECK(v[2010] == doctest::Approx(0.85 * 0.4));  // step 2
    CHECK(v[6010] == doctest::Approx(0.85 * 0.7));  // after the reset: step 0
    CHECK(v[10010] == doctest::Approx(0.85));       // and step 4 again
    // The second channel keeps its own notes: every other step.
    CHECK(rises(c.output("out", 1)).size() == 6);
    CHECK(c.output("velocity", 1)[2010] == doctest::Approx(0.85 * 0.4));
}
