// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr std::int64_t kBar = 24000;  // 4/4 at 480 beats a minute

void song(ModuleHarness& h, double beats = 0.0) {
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 480;
    t.beats = beats;
    h.transport(t);
}

// Which gate is high at frame n: "a", "b", "c", "edge", or "" for none.
std::string role(ModuleHarness& h, std::int64_t n) {
    const auto at = static_cast<std::size_t>(n);
    std::string out;
    for (const char* k : {"a", "b", "c", "edge"})
        if (h.output(k)[at] == 1.0) out += k;
    return out;
}

std::vector<std::int64_t> rises(const std::vector<double>& y) {
    std::vector<std::int64_t> at;
    for (std::size_t n = 0; n < y.size(); ++n)
        if (y[n] >= 0.5 && (n == 0 || y[n - 1] < 0.5)) at.push_back(static_cast<std::int64_t>(n));
    return at;
}

}  // namespace

TEST_CASE("form: verse, chorus in the song's bars, intro and outro half as long") {
    ModuleHarness h(supple::rack::test::stock("form"));
    h.set("bars", 0);  // 4 bars: intro 2, six sections of 4, outro 2 = 28 bars
    song(h);
    h.run(kBar * 30);
    const std::vector<std::string> want = {"edge", "a", "b", "a", "b", "c", "b", "edge"};
    const std::vector<int> starts = {0, 2, 6, 10, 14, 18, 22, 26};
    for (std::size_t s = 0; s < want.size(); ++s) {
        CAPTURE(s);
        CHECK(role(h, starts[s] * kBar + kBar / 2) == want[s]);
    }
    CHECK(role(h, 28 * kBar + 100) == "edge");  // round again
    std::vector<std::int64_t> changes = rises(h.output("change"));
    REQUIRE(changes.size() == 9);
    for (std::size_t s = 0; s < starts.size(); ++s) CHECK(changes[s] == starts[s] * kBar);
    CHECK(changes[8] == 28 * kBar);
    CHECK(rises(h.output("end")) == std::vector<std::int64_t>{28 * kBar});
    // Energy: a verse 0.5, a chorus 0.85; the intro rising, the outro falling.
    const auto& e = h.output("energy");
    CHECK(e[static_cast<std::size_t>(3 * kBar)] == doctest::Approx(0.5));
    CHECK(e[static_cast<std::size_t>(7 * kBar)] == doctest::Approx(0.85));
    CHECK(e[100] < e[static_cast<std::size_t>(kBar + 100)]);
    CHECK(e[static_cast<std::size_t>(26 * kBar + 100)] >
          e[static_cast<std::size_t>(27 * kBar + 100)]);
    // Progress halfway through the first verse; Fill in its last bar only.
    CHECK(h.output("progress")[static_cast<std::size_t>(4 * kBar)] ==
          doctest::Approx(0.5).epsilon(1e-4));
    const auto& f = h.output("fill");
    CHECK(f[static_cast<std::size_t>(5 * kBar - 100)] == 0.0);
    CHECK(f[static_cast<std::size_t>(5 * kBar + 100)] == 1.0);
    CHECK(f[static_cast<std::size_t>(6 * kBar + 100)] == 0.0);
}

TEST_CASE("form: Fill's length; AABA; a build's energy rises to the drop") {
    ModuleHarness h(supple::rack::test::stock("form"));
    h.set("bars", 0);
    h.set("fill_length", 0);  // a beat
    h.set("plan", 1);
    song(h);
    h.run(kBar * 16);
    const std::vector<std::string> want = {"a", "a", "c", "a"};
    for (std::size_t s = 0; s < 4; ++s)
        CHECK(role(h, static_cast<std::int64_t>(s) * 4 * kBar + 100) == want[s]);
    const auto& f = h.output("fill");
    CHECK(f[static_cast<std::size_t>(4 * kBar - kBar / 4 - 100)] == 0.0);
    CHECK(f[static_cast<std::size_t>(4 * kBar - kBar / 4 + 100)] == 1.0);

    ModuleHarness d(supple::rack::test::stock("form"));
    d.set("bars", 0);
    d.set("plan", 2);  // intro 2, break 4, build 4, drop 4
    song(d);
    d.run(kBar * 16);
    const auto& e = d.output("energy");
    CHECK(role(d, 6 * kBar + 100) == "c");
    CHECK(e[static_cast<std::size_t>(6 * kBar + 100)] == doctest::Approx(0.3).epsilon(0.01));
    CHECK(e[static_cast<std::size_t>(10 * kBar - 100)] > 0.99);
    CHECK(role(d, 10 * kBar + 100) == "b");
    CHECK(e[static_cast<std::size_t>(10 * kBar + 100)] == 1.0);
    CHECK(e[static_cast<std::size_t>(3 * kBar)] == doctest::Approx(0.25));  // the break
}

TEST_CASE("form: from the middle of the song it is where the song is; Loop off rests") {
    ModuleHarness h(supple::rack::test::stock("form"));
    h.set("bars", 0);
    h.set("loop", 0);
    song(h, 4.0 * 19);  // bar 19: in the bridge
    h.run(kBar * 12);
    CHECK(role(h, 100) == "c");
    CHECK(h.output("change")[0] == 1.0);
    CHECK(role(h, 3 * kBar + 100) == "b");     // bar 22
    CHECK(role(h, 8 * kBar + 100) == "edge");  // bar 27, the outro
    // Bar 28: the end, then rest.
    CHECK(rises(h.output("end")) == std::vector<std::int64_t>{9 * kBar});
    CHECK(role(h, 9 * kBar + 100) == "");
    CHECK(h.output("energy")[static_cast<std::size_t>(11 * kBar)] == 0.0);
    CHECK(rises(h.output("change")).size() == 3);  // 19 (joined), 22, 26
}

TEST_CASE("form: Bar counts an outside clock; Next jumps on; Reset starts the form now") {
    ModuleHarness h(supple::rack::test::stock("form"));
    h.set("bars", 0);
    h.input("bar", 1, [](int, std::int64_t n) { return n % 1000 < 10 ? 1.0 : 0.0; });
    // Next in the first verse's second bar (bar 3); Reset at bar 12.
    h.input("next", 1, [](int, std::int64_t n) { return n >= 3500 && n < 3600 ? 1.0 : 0.0; });
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 12000 && n < 12005 ? 1.0 : 0.0; });
    h.run(1000 * 20);
    CHECK(role(h, 1500) == "edge");
    CHECK(role(h, 2500) == "a");
    // Progress halfway through bar 2 (the verse's first): an eighth of the verse.
    CHECK(h.output("progress")[2500] == doctest::Approx(0.125).epsilon(0.01));
    CHECK(role(h, 3700) == "b");  // jumped to the chorus
    CHECK(role(h, 7400) == "b");
    CHECK(role(h, 7600) == "a");  // the chorus' four bars from 3.5
    // After the reset, the next edge is the intro's first bar.
    CHECK(role(h, 12500) == "edge");
    CHECK(role(h, 14500) == "edge");
    CHECK(role(h, 15500) == "a");
}

TEST_CASE("form: Wander walks from an intro, never ends, the same for a seed") {
    auto walk = [](int seed) {
        ModuleHarness h(supple::rack::test::stock("form"), 48000.0, seed);
        h.set("bars", 0);
        h.set("plan", 3);
        h.input("bar", 1, [](int, std::int64_t n) { return n % 500 < 10 ? 1.0 : 0.0; });
        h.run(500 * (2 + 4 * 31) + 250);
        std::string out;
        for (int s = 0; s < 32; ++s) {
            const std::string r = role(h, (s == 0 ? 0 : 2 + 4 * (s - 1)) * 500 + 250);
            out += r == "edge" ? "E" : (r == "a" ? "A" : (r == "b" ? "B" : (r == "c" ? "C" : "?")));
        }
        CHECK(rises(h.output("end")).empty());
        return out;
    };
    const std::string one = walk(1);
    CAPTURE(one);
    CHECK(one.substr(0, 2) == "EA");
    CHECK(one.find('E', 1) == std::string::npos);
    CHECK(one.find('?') == std::string::npos);
    CHECK(one.find('B') != std::string::npos);
    CHECK(one.find('C') != std::string::npos);
    CHECK(walk(1) == one);
    CHECK(walk(2) != one);
}
