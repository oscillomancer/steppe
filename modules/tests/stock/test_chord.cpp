// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "module_harness.h"
#include "stock_lookup.h"

using supple::rack::test::ModuleHarness;

namespace {

// The chord's notes in semitones for a note in semitones.
std::vector<int> chord(ModuleHarness& h, double note) {
    h.input("pitch", 1, [note](int, std::int64_t) { return note / 12.0; });
    h.run(32);
    std::vector<int> v;
    const auto last = static_cast<std::size_t>(h.frames() - 16);
    for (int c = 0; c < h.channels("out"); ++c)
        v.push_back(static_cast<int>(std::lround(h.output("out", c)[last] * 12.0)));
    return v;
}

}  // namespace

TEST_CASE("chord: types, voices past the chord, inversions") {
    ModuleHarness h(supple::rack::test::stock("chord"));
    CHECK(chord(h, 0) == std::vector<int>{0, 4, 7, 12});  // major at 4 voices
    h.set("voices", 3);
    h.set("type", 7);  // minor 7, 3 voices
    CHECK(chord(h, 2) == std::vector<int>{2, 5, 9});
    h.set("voices", 4);
    h.set("inversion", 1);
    CHECK(chord(h, 2) == std::vector<int>{5, 9, 12, 14});
    h.set("inversion", -2);
    CHECK(chord(h, 2) == std::vector<int>{-3, 0, 2, 5});
}

TEST_CASE("chord: voicings spread the notes") {
    ModuleHarness h(supple::rack::test::stock("chord"));
    h.set("type", 6);     // major 7
    h.set("voicing", 1);  // drop 2
    CHECK(chord(h, 0) == std::vector<int>{-5, 0, 4, 11});
    h.set("voicing", 2);  // open
    CHECK(chord(h, 0) == std::vector<int>{0, 7, 16, 23});
    h.set("voicing", 3);  // wide
    CHECK(chord(h, 0) == std::vector<int>{0, 16, 19, 35});
}

TEST_CASE("chord: in a key, the key's thirds on the note") {
    ModuleHarness h(supple::rack::test::stock("chord"));
    h.set("type", 22);  // in major
    h.set("voices", 3);
    CHECK(chord(h, 2) == std::vector<int>{2, 5, 9});    // D minor in C
    CHECK(chord(h, 7) == std::vector<int>{7, 11, 14});  // G major
    CHECK(chord(h, -1) == std::vector<int>{-1, 2, 5});  // B diminished, below C4
    CHECK(chord(h, 6) == std::vector<int>{5, 9, 12});   // F# taken down to F
    h.set("voices", 4);
    CHECK(chord(h, 7) == std::vector<int>{7, 11, 14, 17});  // G7
    h.set("type", 23);                                      // in minor
    h.set("key", 9);                                        // A minor
    h.set("voices", 3);
    CHECK(chord(h, 4) == std::vector<int>{4, 7, 11});  // E minor
}

TEST_CASE("chord: strum staggers the gate across the voices") {
    ModuleHarness h(supple::rack::test::stock("chord"));
    h.set("strum", 10);  // 480 frames
    h.input("gate", 1, [](int, std::int64_t n) { return n >= 100 && n < 4000 ? 1.0 : 0.0; });
    h.run(6000);
    for (int c = 0; c < 4; ++c) {
        CAPTURE(c);
        CHECK(h.output("gates", c)[100] == 1.0);
        CHECK(h.output("strummed", c)[100 + 480 * c - 1] == 0.0);
        CHECK(h.output("strummed", c)[100 + 480 * c] == 1.0);
        CHECK(h.output("strummed", c)[4000 + 480 * c] == 0.0);
    }
    ModuleHarness d(supple::rack::test::stock("chord"));
    d.set("strum", 10);
    d.set("direction", 1);
    d.input("gate", 1, [](int, std::int64_t n) { return n >= 100 && n < 4000 ? 1.0 : 0.0; });
    d.run(2000);
    CHECK(d.output("strummed", 3)[100] == 1.0);
    CHECK(d.output("strummed", 0)[100 + 3 * 480 - 1] == 0.0);
}
