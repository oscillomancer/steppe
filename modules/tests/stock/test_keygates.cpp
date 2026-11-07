// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cstdint>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;
using supple::rack::test::stock;

namespace {
// A MIDI key as a pitch: 0 at C4 (60), 1 per octave.
double key(int k) { return (k - 60) / 12.0; }
}  // namespace

TEST_CASE("keygates: each lane's gate follows its key, its velocity the note's") {
    ModuleHarness h(stock("keygates"));
    // Voice 0 plays C1 (36) from 100 to 300 at 0.5; voice 1 F#1 (42) from
    // 200 to 400 at 0.8; voice 2 E4 (64), on no lane, throughout.
    const int keys[] = {36, 42, 64};
    h.input("pitch", 3, [&](int c, std::int64_t) { return key(keys[c]); });
    h.input("gate", 3, [](int c, std::int64_t n) {
        if (c == 0) return n >= 100 && n < 300 ? 1.0 : 0.0;
        if (c == 1) return n >= 200 && n < 400 ? 1.0 : 0.0;
        return 1.0;
    });
    h.input("velocity", 3, [](int c, std::int64_t) { return c == 0 ? 0.5 : 0.8; });
    h.run(512);
    for (const char* k : {"out_1", "out_4", "vel_1"}) CHECK(h.channels(k) == 1);
    const auto& kick = h.output("out_1");
    const auto& hat = h.output("out_4");
    CHECK(kick[50] == 0.0);
    CHECK(kick[150] == 1.0);
    CHECK(kick[350] == 0.0);
    CHECK(hat[150] == 0.0);
    CHECK(hat[250] == 1.0);
    CHECK(hat[450] == 0.0);
    CHECK(h.output("vel_1")[150] == doctest::Approx(0.5));
    CHECK(h.output("vel_4")[250] == doctest::Approx(0.8));
    // Held after the note ends, for a drum's accent read late.
    CHECK(h.output("vel_1")[450] == doctest::Approx(0.5));
    for (const char* k : {"out_2", "out_3", "out_5", "out_6", "out_7", "out_8"}) {
        CAPTURE(k);
        for (double v : h.output(k)) CHECK(v == 0.0);
    }
}

TEST_CASE("keygates: a second note on a held key hits again; Key moves a lane") {
    ModuleHarness h(stock("keygates"));
    h.input("pitch", 2, [](int, std::int64_t) { return key(38); });
    h.input("gate", 2, [](int c, std::int64_t n) {
        return c == 0 ? (n >= 100 && n < 400 ? 1.0 : 0.0) : (n >= 200 && n < 300 ? 1.0 : 0.0);
    });
    h.run(512);
    const auto& snare = h.output("out_2");
    CHECK(snare[150] == 1.0);
    CHECK(snare[200] == 0.0);  // the frame voice 1 starts
    CHECK(snare[201] == 1.0);
    CHECK(snare[350] == 1.0);  // voice 0 still holds it
    CHECK(snare[450] == 0.0);
    int rises = 0;
    for (std::size_t n = 1; n < snare.size(); ++n) rises += snare[n] > 0.5 && snare[n - 1] < 0.5;
    CHECK(rises == 2);

    ModuleHarness moved(stock("keygates"));
    moved.set("key_1", 60);
    moved.input("pitch", 1, [](int, std::int64_t) { return 0.0; });
    moved.input("gate", 1, [](int, std::int64_t n) { return n >= 10 ? 1.0 : 0.0; });
    moved.run(64);
    CHECK(moved.output("out_1")[32] == 1.0);
    // Unconnected, the velocity is full.
    CHECK(moved.output("vel_1")[32] == 1.0);
}

TEST_CASE("keygates: a key handed from one voice to another on the same frame hits again") {
    // One voice's gate falls on the frame the other's rises, on the same key,
    // in either order of the voices.
    for (int first : {0, 1}) {
        CAPTURE(first);
        ModuleHarness h(stock("keygates"));
        h.input("pitch", 2, [](int, std::int64_t) { return key(36); });
        h.input("gate", 2, [first](int c, std::int64_t n) {
            const bool early = n >= 100 && n < 200;
            const bool late = n >= 200 && n < 300;
            return (c == first ? early : late) ? 1.0 : 0.0;
        });
        h.input("velocity", 2, [first](int c, std::int64_t) { return c == first ? 0.5 : 0.9; });
        h.run(400);
        const auto& kick = h.output("out_1");
        CHECK(kick[200] == 0.0);
        CHECK(kick[201] == 1.0);
        CHECK(h.output("vel_1")[250] == doctest::Approx(0.9));
        int rises = 0;
        for (std::size_t n = 1; n < kick.size(); ++n) rises += kick[n] > 0.5 && kick[n - 1] < 0.5;
        CHECK(rises == 2);
    }
}

TEST_CASE("keygates: a voice the cable stops carrying forgets its note") {
    // Voice 1 holds the kick; the cable drops to one channel and comes back
    // with voice 1 already high, now on the snare: that is a new note.
    ModuleHarness h(stock("keygates"));
    h.input("pitch", 2, [](int, std::int64_t) { return key(36); });
    h.input("gate", 2, [](int c, std::int64_t n) { return c == 1 && n >= 32 ? 1.0 : 0.0; });
    h.input("velocity", 2, [](int, std::int64_t) { return 0.4; });
    h.run(128);
    CHECK(h.output("out_1")[100] == 1.0);
    h.input("gate", 1, [](int, std::int64_t) { return 0.0; });
    h.run(128);
    CHECK(h.output("out_1")[200] == 0.0);
    h.input("pitch", 2, [](int, std::int64_t) { return key(38); });
    h.input("gate", 2, [](int c, std::int64_t) { return c == 1 ? 1.0 : 0.0; });
    h.input("velocity", 2, [](int, std::int64_t) { return 0.7; });
    h.run(128);
    CHECK(h.output("out_1")[300] == 0.0);
    CHECK(h.output("out_2")[300] == 1.0);
    CHECK(h.output("vel_2")[300] == doctest::Approx(0.7));
}
