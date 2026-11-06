// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

constexpr int kStep = 4800;

ModuleHarness::Signal clockOf(int period) {
    return [period](int, std::int64_t n) { return (n % period) < 100 ? 1.0 : 0.0; };
}

// The row after `step` clocks, read from the Cells channels as bits.
int rowAt(ModuleHarness& h, int step) {
    int r = 0;
    for (int k = 0; k < h.channels("cells"); ++k)
        if (h.output("cells", k)[static_cast<std::size_t>(step * kStep + 10)] == 1.0) r |= 1 << k;
    return r;
}

int bits(int r) {
    int n = 0;
    for (; r != 0; r &= r - 1) ++n;
    return n;
}

}  // namespace

TEST_CASE("cellular: rule 90 grows a triangle from the middle cell") {
    ModuleHarness h(supple::rack::test::stock("cellular"));
    h.set("rule", 90);
    h.set("width", 16);
    h.set("wrap", 0);
    h.input("clock", 1, clockOf(kStep));
    h.run(kStep * 5);
    CHECK(h.channels("cells") == 16);
    CHECK(rowAt(h, 0) == ((1 << 7) | (1 << 9)));
    CHECK(rowAt(h, 1) == ((1 << 6) | (1 << 10)));
    CHECK(rowAt(h, 2) == ((1 << 5) | (1 << 7) | (1 << 9) | (1 << 11)));
    // High with the clock only.
    CHECK(h.output("cells", 7)[200] == 0.0);
}

TEST_CASE("cellular: rule 184 keeps its cars; the taps follow cells 1, 3, 5 and 7") {
    ModuleHarness h(supple::rack::test::stock("cellular"));
    h.set("rule", 184);
    h.set("start", 1);
    h.set("revive", 0);
    h.input("clock", 1, clockOf(kStep));
    h.run(kStep * 24);
    const int cars = bits(rowAt(h, 0));
    CHECK(cars > 0);
    bool moved = false;
    for (int s = 1; s < 24; ++s) {
        CHECK(bits(rowAt(h, s)) == cars);
        if (rowAt(h, s) != rowAt(h, s - 1)) moved = true;
        const std::size_t at = static_cast<std::size_t>(s * kStep + 10);
        CHECK(h.output("alive")[at] == doctest::Approx(cars / 8.0));
        for (int k = 0; k < 4; ++k)
            CHECK(h.output("tap_" + std::to_string(k + 1))[at] == h.output("cells", 2 * k + 1)[at]);
    }
    CHECK(moved);
}

TEST_CASE("cellular: Value reads the row; Revive sows again; Mutate flips a cell") {
    SUBCASE("value") {
        ModuleHarness h(supple::rack::test::stock("cellular"));
        h.set("rule", 204);  // each cell stays as it is
        h.set("revive", 0);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 2);
        // Cell 4 of 8: 1/32 of the full scale's 255/256.
        CHECK(h.output("value")[kStep + 10] == doctest::Approx((1.0 / 32.0) / (255.0 / 256.0)));
    }
    SUBCASE("dying without revive") {
        ModuleHarness h(supple::rack::test::stock("cellular"));
        h.set("rule", 0);
        h.set("revive", 0);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 3);
        CHECK(rowAt(h, 1) == 0);
        CHECK(h.output("alive")[kStep + 10] == 0.0);
    }
    SUBCASE("revived") {
        ModuleHarness h(supple::rack::test::stock("cellular"));
        h.set("rule", 0);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 3);
        CHECK(rowAt(h, 1) == 1 << 4);
        CHECK(rowAt(h, 2) == 1 << 4);
    }
    SUBCASE("mutate") {
        ModuleHarness h(supple::rack::test::stock("cellular"));
        h.set("rule", 204);
        h.set("mutate", 100);
        h.set("revive", 0);
        h.set("start", 1);
        h.input("clock", 1, clockOf(kStep));
        h.run(kStep * 20);
        for (int s = 1; s < 20; ++s) CHECK(bits(rowAt(h, s) ^ rowAt(h, s - 1)) == 1);
    }
}

TEST_CASE("cellular: the song's sixteenths; Reset grows the same rows again") {
    ModuleHarness h(supple::rack::test::stock("cellular"));
    h.set("rule", 30);
    h.set("start", 1);
    h.set("mutate", 20);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 6000 frames a sixteenth
    h.transport(t);
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 95950 && n < 96000 ? 1.0 : 0.0; });
    h.run(192000);
    bool any = false;
    for (int s = 0; s < 16; ++s) {
        for (int k = 0; k < 8; ++k) {
            const auto& y = h.output("cells", k);
            const std::size_t at = static_cast<std::size_t>(s * 6000 + 10);
            CHECK(y[at + 3100] == 0.0);  // half a sixteenth
            CHECK(y[96000 + at] == y[at]);
            if (y[at] == 1.0) any = true;
        }
    }
    CHECK(any);
}
