// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A transient shaper, poly: turns up or down the start of each hit and its
// tail, apart, whatever its level, with no threshold to set. Attack above 0
// sharpens a drum's crack or a pluck's pick, below 0 softens it; Sustain
// above 0 lengthens the ring and the room, below 0 tightens them. Length is
// how long counts as the start: short for clicks, long for a snare's body.
//
// It measures the level over 3 ms and follows it two more ways: smoothed
// over Length, and holding its peaks and falling slowly. Where the level is
// ahead of the smoothed one, a hit is starting; where it is behind it and
// the held one is above it, the hit is dying away. The gain follows their
// differences in dB, by Attack and Sustain: up to 12 dB either way at 100,
// and in proportion below, so a hit out of silence is not boosted the most
// at any setting. A steady sound is left alone, and at 0 and 0 everything passes untouched.

constexpr double kMostDb = 12.0;
constexpr double kLevelMs = 3.0;
constexpr double kHoldMs = 400.0;

struct Follow {
    double power = 0.0, slow = 0.0, hold = 0.0;
};

struct Transient : Module {
    static constexpr ModuleInfo info = {
        .name = "Transient",
        .id = "com.suppleaudio.rack.transient",
        .vendor = "Supple Audio",
        .description =
            "A transient shaper: each hit's attack and sustain up or down apart, at any level.",
        .tags = {"dynamics", "effect", "poly"}};

    Param attack = percent("Attack", 0, -100, 100)
                       .help("The start of each hit: above 0 sharper, below 0 softer.");
    Param sustain = percent("Sustain", 0, -100, 100)
                        .help("Each hit's tail: above 0 longer and fuller, below 0 tighter.");
    Param length =
        milliseconds("Length", 5, 100, 20)
            .help("How long counts as the start: short for clicks, long for a drum's body.");
    Param level = decibels("Level", -18, 18, 0).smoothed(20).help("The gain after.");

    Input in = audioIn("In").help("Drums, plucks, anything with hits. Poly: each channel alone.");
    Output out = audioOut("Out").help("The shaped sound.");

    Array<Follow, 16> follow;

    void process(int frames) {
        const double sr = sampleRate();
        const double now = 1.0 - exp(-1.0 / (kLevelMs * 0.001 * sr));
        const double span = 1.0 - exp(-1.0 / (max(length.value(), 5.0) * 0.001 * sr));
        const double hold = 1.0 - exp(-1.0 / (kHoldMs * 0.001 * sr));
        for (int c = 0; c < channels(); ++c) {
            Follow& f = follow[c];
            for (int i = 0; i < frames; ++i) {
                const double x = in[c][i];
                f.power += now * (x * x - f.power);
                const double a = sqrt(f.power);
                f.slow += span * (a - f.slow);
                f.hold = a > f.hold ? a : f.hold + hold * (a - f.hold);
                const double q = 20.0 * log10(a + 1e-6);
                const double rising = q - 20.0 * log10(f.slow + 1e-6);
                // Only while the level is falling, below its smoothed self:
                // a hit starting after a gap is not taken for a tail.
                const double falling = a < f.slow ? 20.0 * log10(f.hold + 1e-6) - q : 0.0;
                const double gainDb = clamp(attack.at(i, c) * clamp(rising, 0.0, kMostDb) +
                                                sustain.at(i, c) * clamp(falling, 0.0, kMostDb),
                                            -kMostDb, kMostDb);
                out[c][i] = x * pow(10.0, (gainDb + level.at(i, c)) / 20.0);
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Punch").set(attack, 60).set(sustain, -10).set(level, -3);
        p.add("Tighten").set(attack, 10).set(sustain, -60).set(length, 15);
        p.add("Soften").set(attack, -60).set(length, 30);
        p.add("Room").set(attack, 0).set(sustain, 70).set(level, -3);
        p.add("Snap").set(attack, 100).set(length, 6).set(level, -6);
    }
};
