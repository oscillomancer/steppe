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
constexpr std::int64_t kBeat = kBar / 4;

void song(ModuleHarness& h, double beats = 0.0) {
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 480;
    t.beats = beats;
    h.transport(t);
}

// A gate on every beat into all four parts, high for half of it.
void beats(ModuleHarness& h) {
    for (const char* k : {"gate_1", "gate_2", "gate_3", "gate_4"})
        h.input(k, 1, [](int, std::int64_t n) { return n % kBeat < kBeat / 2 ? 1.0 : 0.0; });
}

int rises(const std::vector<double>& y, std::int64_t from, std::int64_t to) {
    int count = 0;
    for (auto n = from; n < to; ++n) {
        const auto i = static_cast<std::size_t>(n);
        if (y[i] >= 0.5 && (n == 0 || y[i - 1] < 0.5)) ++count;
    }
    return count;
}

// Which parts play in bar b, as "1234" with '-' for a rest.
std::string parts(ModuleHarness& h, std::int64_t b) {
    std::string out;
    const char* ons[] = {"on_1", "on_2", "on_3", "on_4"};
    for (int k = 0; k < 4; ++k)
        out += h.output(ons[k])[static_cast<std::size_t>(b * kBar + kBar / 2)] == 1.0
                   ? static_cast<char>('1' + k)
                   : '-';
    return out;
}

}  // namespace

TEST_CASE("layers: parts enter one by one as Level rises, at the bar") {
    ModuleHarness h(supple::rack::test::stock("layers"));
    song(h);
    beats(h);
    // Level: 0, 0.3, 0.6, 0.9 a bar, changing a beat into each bar; then 0.1.
    h.input("level", 1, [](int, std::int64_t n) {
        const auto b = (n + kBar - kBeat) / kBar - 1;
        return b < 4 ? 0.3 * static_cast<double>(b < 0 ? 0 : b) : 0.1;
    });
    h.run(kBar * 6);
    // Bar 0 reads 0; bar 1 still 0 (it rises a beat in); bar 2 0.3 ...
    CHECK(parts(h, 0) == "1---");
    CHECK(parts(h, 1) == "1---");
    CHECK(parts(h, 2) == "12--");
    CHECK(parts(h, 3) == "123-");
    CHECK(parts(h, 4) == "1234");
    CHECK(parts(h, 5) == "1---");
    CHECK(rises(h.output("out_1"), 0, kBar * 6) == 24);
    CHECK(rises(h.output("out_2"), 0, kBar * 6) == 12);
    CHECK(rises(h.output("out_4"), 0, kBar * 6) == 4);
    CHECK(rises(h.output("out_4"), 4 * kBar, 5 * kBar) == 4);
    CHECK(h.output("count")[static_cast<std::size_t>(4 * kBar + 10)] == 1.0);
    CHECK(h.output("count")[static_cast<std::size_t>(3 * kBar + 10)] == doctest::Approx(0.75));
    // With nothing in Level, every part plays.
    ModuleHarness all(supple::rack::test::stock("layers"));
    song(all);
    beats(all);
    all.run(kBar);
    CHECK(parts(all, 0) == "1234");
}

TEST_CASE("layers: a note that began plays to its end; one that began muted stays so") {
    ModuleHarness h(supple::rack::test::stock("layers"));
    song(h);
    // Level 1 in bar 0, 0 after; part 2 holds a note from a beat before bar 1 to past it,
    // and starts another in bar 1.
    h.input("level", 1, [](int, std::int64_t n) { return n < kBar ? 1.0 : 0.0; });
    h.input("gate_2", 1, [](int, std::int64_t n) {
        return (n >= 3 * kBeat && n < 5 * kBeat) || (n >= 7 * kBeat && n < 9 * kBeat) ? 1.0 : 0.0;
    });
    h.run(kBar * 2);
    const auto& two = h.output("out_2");
    CHECK(two[static_cast<std::size_t>(4 * kBeat + 100)] == 1.0);  // past the bar line
    CHECK(two[static_cast<std::size_t>(5 * kBeat + 100)] == 0.0);
    CHECK(two[static_cast<std::size_t>(7 * kBeat + 100)] == 0.0);  // began after it went
    CHECK(h.output("on_2")[static_cast<std::size_t>(4 * kBeat + 100)] == 0.0);
}

TEST_CASE("layers: At sets the span; Bar counts an outside clock") {
    ModuleHarness h(supple::rack::test::stock("layers"));
    h.set("at", 0);  // each beat
    song(h);
    h.input("level", 1, [](int, std::int64_t n) { return (n / kBeat) % 2 == 1 ? 1.0 : 0.0; });
    h.run(kBar);
    CHECK(h.output("on_4")[static_cast<std::size_t>(kBeat / 2)] == 0.0);
    CHECK(h.output("on_4")[static_cast<std::size_t>(kBeat + kBeat / 2)] == 1.0);
    CHECK(h.output("on_4")[static_cast<std::size_t>(2 * kBeat + kBeat / 2)] == 0.0);

    ModuleHarness four(supple::rack::test::stock("layers"));
    four.set("at", 3);  // every 4 bars
    song(four);
    four.input("level", 1, [](int, std::int64_t n) { return n < kBar ? 0.0 : 1.0; });
    four.run(kBar * 5);
    CHECK(parts(four, 3) == "1---");
    CHECK(parts(four, 4) == "1234");

    // An outside clock of 1000-frame bars.
    ModuleHarness ext(supple::rack::test::stock("layers"));
    ext.input("bar", 1, [](int, std::int64_t n) { return n % 1000 < 10 ? 1.0 : 0.0; });
    ext.input("level", 1, [](int, std::int64_t n) { return n < 1500 ? 0.0 : 1.0; });
    ext.run(3000);
    CHECK(ext.output("on_3")[1500] == 0.0);
    CHECK(ext.output("on_3")[2500] == 1.0);
}

TEST_CASE("layers: Rest sits parts out by span, the same from any start, for a seed") {
    auto play = [](int seed, double restPct, double from) {
        ModuleHarness h(supple::rack::test::stock("layers"), 48000.0, seed);
        h.set("rest", restPct);
        song(h, from * 4.0);
        h.run(kBar * 16);
        std::string out;
        for (int b = 0; b < 16; ++b) out += parts(h, b) + " ";
        return out;
    };
    const std::string none = play(1, 100, 0);
    CHECK(none.find('2') == std::string::npos);
    CHECK(none.find('1') != std::string::npos);
    const std::string some = play(1, 50, 0);
    CAPTURE(some);
    CHECK(some != none);
    const bool three = some.find("123") != std::string::npos;
    CHECK(three);
    CHECK(play(1, 50, 0) == some);
    CHECK(play(2, 50, 0) != some);
    // Started at bar 8, bars 8 to 15 are as they were.
    const std::string later = play(1, 50, 8);
    CHECK(later.substr(0, 8 * 5) == some.substr(8 * 5));
}
