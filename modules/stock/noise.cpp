// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Noise in every colour, poly: white (flat), pink (equal per octave), brown
// (deep, a random walk), blue (bright, rising 3 dB per octave), crackle
// (sparse random clicks, as many a second as Density says: vinyl, fire,
// rain on a roof) and digital noise clocked at Rate: a long shift register's
// hiss, a short one's metallic buzz, or random steps held between clock
// ticks. Each channel is a noise of its own; Channels sets how many, and a
// poly cable into a knob widens it further.

// Pink noise is near Gaussian: at its level a rare peak passes 1. Above 0.8
// it is eased in, to at most 1, which on noise nobody hears.
double eased(double x) {
    const double a = abs(x);
    return a <= 0.8 ? x : (x < 0.0 ? -1.0 : 1.0) * (0.8 + 0.2 * tanh((a - 0.8) / 0.2));
}

struct Gen {
    Noise white, pink, brown;
    Random random;
    double lastPink = 0.0;
    double click = 0.0;
    double clock = 0.0;  // the digital clock's phase
    int lfsr = 1;        // the shift register
    double held = 0.0;   // the digital output's value
};

struct NoiseModule : Module {
    static constexpr ModuleInfo info = {
        .name = "Noise",
        .id = "com.suppleaudio.rack.noise",
        .vendor = "Supple Audio",
        .description = "White, pink, brown and blue noise, crackle and clocked digital noise.",
        .tags = {"noise", "source", "poly"}};

    Param voices = integer("Channels", 1, 16, 1)
                       .withKey("channels")
                       .help("How many independent noises each output carries.");
    Param density =
        frequency("Density", 0.5, 5000, 30).help("The crackle's clicks per second, on average.");
    Param rate =
        frequency("Rate", 20, 48000, 6000)
            .help("The digital noise's clock: low for a crunchy hash, high for a thin hiss.");
    Param mode =
        choice("Mode", {"Long", "Metallic", "Steps"}, 0)
            .help(
                "Digital noise: a long shift register's hiss, a short one's pitched buzz, or "
                "random steps.");

    Output white = audioOut("White").help("Flat noise: every frequency equal.");
    Output pink = audioOut("Pink").help("Equal energy per octave: the natural, balanced one.");
    Output brown = audioOut("Brown").help("Deep rumbling noise, falling 6 dB per octave.");
    Output blue = audioOut("Blue").help("Bright noise, rising 3 dB per octave.");
    Output crackle = audioOut("Crackle").help("Random clicks, Density of them a second.");
    Output digital = audioOut("Digital").help("Noise clocked at Rate, as Mode makes it.");

    Array<Gen, 16> gen;

    void prepare() {
        for (int c = 0; c < 16; ++c) {
            gen[c].white.seed(seed() + 101 * c);
            gen[c].pink.seed(seed() + 101 * c + 17);
            gen[c].brown.seed(seed() + 101 * c + 33);
            gen[c].random.seed(seed() + 101 * c + 59);
            gen[c].lfsr = 1 + c * 977;
        }
    }

    void process(int frames) {
        const int n = max(voices.index(), channels());
        white.setChannels(n);
        pink.setChannels(n);
        brown.setChannels(n);
        blue.setChannels(n);
        crackle.setChannels(n);
        digital.setChannels(n);
        const double sr = sampleRate();
        const int m = mode.index();
        for (int c = 0; c < n; ++c) {
            Gen& g = gen[c];
            for (int i = 0; i < frames; ++i) {
                // Each scaled to about 0.3 RMS (-10 dBFS).
                white[c][i] = 0.52 * g.white.white();
                const double p = g.pink.pink();
                pink[c][i] = eased(1.53 * p);
                brown[c][i] = 0.51 * g.brown.brown();
                blue[c][i] = (p - g.lastPink) * 2.6;
                g.lastPink = p;

                const double chance = density.at(i, c) / sr;
                g.click *= 0.35;
                const double roll = g.random.next();
                if (roll < chance) {
                    const double size = 0.2 + 0.8 * g.random.next();
                    const double side = g.random.next();
                    g.click = side < 0.5 ? -size : size;
                }
                crackle[c][i] = g.click;

                g.clock += min(rate.at(i, c) / sr, 1.0);
                if (g.clock >= 1.0) {
                    g.clock -= 1.0;
                    if (m == 2) {
                        const double step = g.random.bipolar();
                        g.held = 0.52 * step;
                    } else {
                        // A 15-bit register; Metallic feeds back from bit 6,
                        // looping in a few dozen steps, a pitched buzz.
                        const int tap = m == 1 ? (g.lfsr >> 6) : (g.lfsr >> 1);
                        const int bit = (g.lfsr ^ tap) & 1;
                        g.lfsr = ((g.lfsr >> 1) | (bit << 14)) & 32767;
                        if (g.lfsr == 0) g.lfsr = 1;
                        g.held = (g.lfsr & 1) == 1 ? 0.3 : -0.3;
                    }
                }
                digital[c][i] = g.held;
            }
        }
    }
};
