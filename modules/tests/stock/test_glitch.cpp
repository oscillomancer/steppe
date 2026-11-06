// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

#include "measure_sound.h"
#include "module_harness.h"
#include "stock_lookup.h"

using namespace supple::rack::test;

namespace {
using Settings = std::initializer_list<std::pair<const char*, double>>;

// At 120 bpm with the transport stopped: a quarter-note step is 24000
// samples, a sixteenth-note slice 6000.
constexpr std::int64_t kStep = 24000;
constexpr std::int64_t kSlice = 6000;

// A signal every sample of which can be told apart.
double marked(std::int64_t n) {
    std::uint32_t h = static_cast<std::uint32_t>(n) * 2654435761u;
    h ^= h >> 15;
    return static_cast<double>(h & 0xFFFFFFu) / 8388608.0 - 1.0;
}
double tone(std::int64_t n) {
    return 0.5 * std::sin(2 * kPi * 480.0 * static_cast<double>(n) / 48000.0);
}

// Only `kind` among the four, every step, the slice kept.
std::unique_ptr<ModuleHarness> glitch(const char* kind, Settings more, bool sine,
                                      std::int64_t frames) {
    auto h = std::make_unique<ModuleHarness>(stock("glitch"));
    h->set("chance", 100);
    h->set("vary", 0);
    for (const char* k : {"stutter", "reverse", "drop", "stop"}) h->set(k, 0);
    if (kind != nullptr) h->set(kind, 100);
    for (const auto& [key, value] : more) h->set(key, value);
    h->input("in", 1, [sine](int, std::int64_t n) { return sine ? tone(n) : marked(n); });
    h->run(frames);
    return h;
}
}  // namespace

TEST_CASE("glitch: at Chance 0 the sound passes untouched") {
    const auto h = glitch("stutter", {{"chance", 0}}, false, 4 * kStep);
    const auto& y = h->output("out");
    for (std::int64_t n = 0; n < 4 * kStep; n += 7) CHECK(y[n] == marked(n));
    CHECK(peak(h->output("active")) == 0.0);
}

TEST_CASE("glitch: Stutter repeats the slice just played, Reverse plays it backwards") {
    const std::int64_t s = kStep;  // the second step
    const auto st = glitch("stutter", {}, false, 3 * kStep);
    const auto rv = glitch("reverse", {}, false, 3 * kStep);
    for (std::int64_t t = 300; t < kStep - 300; t += 37) {
        const std::int64_t into = t % kSlice;
        if (into < 200 || into > kSlice - 200) continue;  // the repeat's edges fade
        CAPTURE(t);
        CHECK(st->output("out")[s + t] == doctest::Approx(marked(s - kSlice + into)));
        CHECK(rv->output("out")[s + t] == doctest::Approx(marked(s - 1 - into)));
    }
    CHECK(st->output("active")[s + 100] == 1.0);
    // Length sets the slice: an eighth note repeats every 12000 samples.
    const auto eighth = glitch("stutter", {{"length", 3}}, false, 3 * kStep);
    CHECK(eighth->output("out")[s + 12000 + 5000] == doctest::Approx(marked(s - 12000 + 5000)));
}

TEST_CASE("glitch: Drop repeats the slice a fourth, a fifth or an octave down") {
    const auto h = glitch("drop", {{"length", 4}}, true, 6 * kStep);  // quarter-note slices
    int fourths = 0, fifths = 0, octaves = 0;
    for (std::int64_t step = 1; step < 6; ++step) {
        const auto part = slice(h->output("out"), step * kStep + 500, step * kStep + 7500);
        const double f = frequency(part, 48000);
        CAPTURE(f);
        if (std::abs(f - 360.0) < 2.0) ++fourths;
        if (std::abs(f - 320.0) < 2.0) ++fifths;
        if (std::abs(f - 240.0) < 2.0) ++octaves;
    }
    CHECK(fourths + fifths + octaves == 5);
}

TEST_CASE("glitch: Stop winds the sound down over the step") {
    const auto h = glitch("stop", {}, true, 3 * kStep);
    const auto& y = h->output("out");
    const std::size_t s = kStep;
    // Its speed falls from 1 to 0 across the step: the pitch with it.
    CHECK(frequency(slice(y, s + 200, s + 2600), 48000) ==
          doctest::Approx(480.0 * 0.95).epsilon(0.02));
    CHECK(frequency(slice(y, s + 12000, s + 14400), 48000) ==
          doctest::Approx(480.0 * 0.45).epsilon(0.03));
    CHECK(rms(slice(y, s + kStep - 600, s + kStep)) < 0.02);
}

TEST_CASE("glitch: the same choices every render; Reset starts them again") {
    Settings mixed = {{"chance", 50}, {"stutter", 100}, {"reverse", 60},
                      {"drop", 40},   {"stop", 30},     {"vary", 50}};
    const auto a = glitch(nullptr, mixed, false, 10 * kStep);
    const auto b = glitch(nullptr, mixed, false, 10 * kStep);
    CHECK(a->output("out") == b->output("out"));
    // Some steps glitch and some do not.
    int glitched = 0;
    for (std::int64_t step = 0; step < 10; ++step)
        glitched += a->output("active")[step * kStep + 10] > 0.5;
    CHECK(glitched > 1);
    CHECK(glitched < 9);

    ModuleHarness c(stock("glitch"));
    for (const auto& [key, value] : mixed) c.set(key, value);
    c.input("in", 1, [](int, std::int64_t n) { return marked(n); });
    c.input("reset", 1,
            [](int, std::int64_t n) { return n >= 5 * kStep && n < 5 * kStep + 48 ? 1.0 : 0.0; });
    c.run(10 * kStep);
    for (std::int64_t n = 0; n < 4 * kStep; n += 101)
        CHECK(c.output("active")[5 * kStep + n] == a->output("active")[n]);
}

TEST_CASE("glitch: on Clock, steps follow its rises; poly voices glitch together") {
    ModuleHarness h(stock("glitch"));
    h.set("chance", 50);
    h.input("in", 2, [](int c, std::int64_t n) { return c == 0 ? marked(n) : marked(n + 777); });
    h.input("clock", 1, [](int, std::int64_t n) { return n % 10000 < 100 ? 1.0 : 0.0; });
    h.run(20 * 10000);
    const auto& on = h.output("active");
    int changes = 0;
    for (std::size_t n = 1; n < on.size(); ++n)
        if (on[n] != on[n - 1]) {
            CAPTURE(n);
            CHECK(n % 10000 == 0);
            ++changes;
        }
    CHECK(changes > 2);
    // The second voice is glitched just as the first: the same reads of
    // its own input.
    CHECK(h.channels("out") == 2);
    const auto& y0 = h.output("out", 0);
    const auto& y1 = h.output("out", 1);
    int same = 0, compared = 0;
    for (std::int64_t n = 1000; n < 200000; n += 113) {
        if (on[n] < 0.5) continue;
        // Where voice 0 reads marked(m) exactly, voice 1 reads marked(m + 777).
        for (std::int64_t back = 0; back < 30000; ++back)
            if (y0[n] == marked(n - back)) {
                ++compared;
                same += y1[n] == marked(n - back + 777);
                break;
            }
    }
    CHECK(compared > 10);
    CHECK(same == compared);
}

TEST_CASE("glitch: while the song plays, steps fall on its beats") {
    // Playing from half a beat in: the quarter-note steps begin 12000
    // samples on, then every 24000.
    ModuleHarness h(stock("glitch"));
    h.set("chance", 50);
    ModuleHarness::Transport t;
    t.playing = true;
    t.tempo = 120;
    t.beats = 0.5;
    h.transport(t);
    h.input("in", 1, [](int, std::int64_t n) { return marked(n); });
    h.run(20 * kStep);
    const auto& on = h.output("active");
    int changes = 0;
    for (std::size_t n = 1; n < on.size(); ++n)
        if (on[n] != on[n - 1]) {
            CAPTURE(n);
            CHECK(n % kStep == 12000);
            ++changes;
        }
    CHECK(changes > 2);
}
