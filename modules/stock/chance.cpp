// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A coin for every gate: each gate that arrives goes out of A, with the
// chance Chance, or else of B, poly, each channel tossing its own coin. A
// cable into Chance tilts the coin. Latched, A or B stays high from one
// toss to the next, a random switch that a clock flips. Ratchets turn some
// gates into bursts: with Ratchet Chance, a gate becomes Ratchets quick
// pulses spread over the time to the next gate, a drum roll from a steady
// clock. Reset starts the same tosses over.

struct Chance : Module {
    static constexpr ModuleInfo info = {
        .name = "Chance",
        .id = "com.suppleaudio.rack.chance",
        .vendor = "Supple Audio",
        .description = "A coin toss per gate: to A or B, latched, or as a burst of ratchets.",
        .tags = {"random", "logic", "generative", "poly"}};

    Param chance = percent("Chance", 50).help("How likely each gate is to go to A, not B.");
    Param latch =
        toggle("Latch", false)
            .help("Hold A or B high from one toss to the next, instead of passing each gate.");
    Param ratchetChance = percent("Ratchet", 0)
                              .activeUnless(latch)
                              .help("How likely a gate is to become a burst of pulses.");
    Param ratchets =
        integer("Ratchets", 2, 8, 3)
            .activeUnless(latch)
            .help("How many pulses a burst has, spread over the time to the next gate.");

    Input gate = gateIn("Gate").help("The gates to toss (rises at 0.5, falls below 0.25); poly.");
    Input reset = gateIn("Reset").help("A rising edge starts the same tosses over.");

    Output a = gateOut("A").help("The gates that came up A.");
    Output b = gateOut("B").help("The gates that came up B.");

    struct Voice {
        Random rng;
        bool high = false;
        bool toA = false;  // the last toss
        bool tossed = false;
        int burst = 0;      // pulses in the burst; 0 none
        int since = 0;      // frames since the gate rose
        int period = 0;     // frames between the last two rises; 0 unknown
        int length = 4800;  // frames of each burst pulse's cycle
    };
    Array<Voice, 16> voices;
    bool resetHigh = false;
    int salt = 0;

    void factoryPatches(Patches& p) {
        p.add("Coin");
        p.add("Mostly A").set(chance, 80);
        p.add("Random Switch").set(latch, true);
        p.add("Drum Rolls").set(chance, 100).set(ratchetChance, 25).set(ratchets, 3);
    }

    void startOver() {
        for (int c = 0; c < 16; ++c) {
            voices[c].rng.seed(salt + c * 7919);
            voices[c].burst = 0;
            voices[c].tossed = false;
        }
    }

    void prepare() {
        salt = seed();
        startOver();
    }

    void process(int frames) {
        const bool held = latch.on();
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                startOver();
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            for (int c = 0; c < channels(); ++c) {
                Voice& v = voices[c];
                const double g = gate[c][i];
                ++v.since;
                if (!v.high && g >= 0.5) {
                    v.high = true;
                    if (v.since > 1 && v.since < 1000000) v.period = v.since;
                    v.since = 0;
                    // Two draws a toss, whatever the settings.
                    const double coin = v.rng.next();
                    const double roll = v.rng.next();
                    v.toA = coin < chance.at(i, c);
                    v.tossed = true;
                    v.burst = !held && roll < ratchetChance.at(i, c)
                                  ? clamp(roundToInt(ratchets.at(i, c)), 2, 8)
                                  : 0;
                    const int span = v.period > 0 ? v.period : roundToInt(0.25 * sampleRate());
                    v.length = max(2, idiv(span, max(1, v.burst)));
                } else if (v.high && g < 0.25) {
                    v.high = false;
                }
                bool on = false;
                if (held) {
                    on = v.tossed;
                } else if (v.burst > 0) {
                    // Pulses of half a cycle each, the burst's span long.
                    const int pulse = idiv(v.since, v.length);
                    on = pulse < v.burst && imod(v.since, v.length) < v.length / 2;
                    if (pulse >= v.burst) v.burst = 0;
                } else {
                    on = v.high;
                }
                a[c][i] = on && v.toA ? 1.0 : 0.0;
                b[c][i] = on && !v.toA ? 1.0 : 0.0;
            }
        }
    }
};
