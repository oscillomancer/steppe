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

// The frames where an output rises.
std::vector<int> rises(const ModuleHarness& h, const std::string& key) {
    std::vector<int> at;
    const auto& x = h.output(key);
    for (std::size_t n = 0; n < x.size(); ++n)
        if (x[n] > 0.5 && (n == 0 || x[n - 1] < 0.5)) at.push_back(static_cast<int>(n));
    return at;
}

}  // namespace

TEST_CASE("clock: following the song, ratios of its beats") {
    ModuleHarness h(supple::rack::test::stock("clock"));
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;  // 24000 frames a beat
    h.transport(t);
    h.run(96000);
    CHECK(rises(h, "out_1") == std::vector<int>{0, 6000, 12000, 18000, 24000, 30000, 36000, 42000,
                                                48000, 54000, 60000, 66000, 72000, 78000, 84000,
                                                90000});                   // x4
    CHECK(rises(h, "out_3") == std::vector<int>{0, 24000, 48000, 72000});  // x1
    CHECK(rises(h, "out_5") == std::vector<int>{0});                       // /4
    CHECK(rises(h, "reset_out") == std::vector<int>{0});
    // Half a tick high.
    CHECK(h.output("out_3")[11990] == 1.0);
    CHECK(h.output("out_3")[12010] == 0.0);
}

TEST_CASE("clock: following the song from the middle lands on its beats") {
    ModuleHarness h(supple::rack::test::stock("clock"));
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    t.beats = 2.5;
    h.transport(t);
    h.run(48000);
    CHECK(rises(h, "out_3") == std::vector<int>{12000, 36000});
    CHECK(rises(h, "out_4") == std::vector<int>{36000});  // /2: on beat 4
}

TEST_CASE("clock: free tempo with swing; run stops it") {
    ModuleHarness h(supple::rack::test::stock("clock"));
    h.set("follow_song", 0);
    h.set("tempo", 120);
    h.set("swing", 50);
    h.input("run", 1, [](int, std::int64_t n) { return n < 48000 ? 1.0 : 0.0; });
    h.run(96000);
    // x2: ticks every 12000, every second one 6000 late.
    CHECK(rises(h, "out_2") == std::vector<int>{0, 18000, 24000, 42000});
    CHECK(rises(h, "out_3") == std::vector<int>{0, 24000});  // swing leaves x1 alone
}

TEST_CASE("clock: an external clock divided and multiplied; reset restarts it") {
    ModuleHarness h(supple::rack::test::stock("clock"));
    h.input("clock", 1, [](int, std::int64_t n) { return n % 4800 < 48 ? 1.0 : 0.0; });
    h.input("reset", 1, [](int, std::int64_t n) { return n >= 26000 && n < 26048 ? 1.0 : 0.0; });
    h.run(48000);
    const auto twice = rises(h, "out_2");  // x2
    CHECK(twice[0] == 0);
    CHECK(twice[1] == 4800);
    CHECK(twice[2] == 7200);  // the period known: halfway between pulses
    CHECK(twice[3] == 9600);
    const auto half = rises(h, "out_4");                            // /2
    CHECK(half == std::vector<int>{0, 9600, 19200, 28800, 38400});  // afresh after the reset
    CHECK(rises(h, "reset_out") == std::vector<int>{0, 26000});
}
