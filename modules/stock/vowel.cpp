// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A formant filter, poly: four resonant bands at the formants of a sung
// vowel, so a bright source (a saw, a pulse, noise, a drum loop) seems to
// speak. Vowel morphs smoothly through a (0), e (1), i (2), o (3) and u
// (4): patch an LFO or an envelope into it for wah-wah and yowls. Size
// moves every formant together, the length of the throat: up a semitone
// count is a bigger, darker voice, down a smaller, childlike one. Resonance
// narrows the bands, from a soft breath to a ringing whistle.
//
// The formants are rounded averages for an adult voice; between vowels
// they glide in log frequency. The bands peak 12 dB up, so a saw comes out
// about as loud as it went in. Each band is a state-variable filter tuned
// every sample, so the vowel can move at audio rate without zipper noise.

// Formant f (0..3) of vowel v (0..4) in Hz.
double formantOf(int v, int f) {
    if (f == 3) return 3500.0;
    if (v == 0) return f == 0 ? 750.0 : (f == 1 ? 1200.0 : 2600.0);  // a
    if (v == 1) return f == 0 ? 480.0 : (f == 1 ? 1850.0 : 2550.0);  // e
    if (v == 2) return f == 0 ? 300.0 : (f == 1 ? 2250.0 : 3000.0);  // i
    if (v == 3) return f == 0 ? 470.0 : (f == 1 ? 850.0 : 2500.0);   // o
    return f == 0 ? 330.0 : (f == 1 ? 750.0 : 2400.0);               // u
}
double widthOf(int f) { return f == 0 ? 80.0 : (f == 1 ? 100.0 : (f == 2 ? 130.0 : 170.0)); }
double levelOf(int f) { return f == 0 ? 1.0 : (f == 1 ? 0.6 : (f == 2 ? 0.3 : 0.15)); }

struct Band {
    double s1 = 0.0, s2 = 0.0;

    // A trapezoidal state-variable band pass with a peak gain of 1.
    double run(double x, double g, double k) {
        const double hp = (x - (k + g) * s1 - s2) / (1.0 + g * (k + g));
        const double bp = g * hp + s1;
        s1 = g * hp + bp;
        const double lp = g * bp + s2;
        s2 = g * bp + lp;
        return k * bp;
    }
};

struct Throat {
    Array<Band, 4> band;
};

struct Vowel : Module {
    static constexpr ModuleInfo info = {
        .name = "Vowel",
        .id = "com.suppleaudio.rack.vowel",
        .vendor = "Supple Audio",
        .description =
            "A formant filter that morphs through a, e, i, o, u, with size and "
            "resonance.",
        .tags = {"filter", "formant", "poly"}};

    Param vowel =
        continuous("Vowel", 0, 4, 0).help("Morphs through a at 0, e at 1, i at 2, o at 3, u at 4.");
    Param size = semitones("Size", -12, 12, 0)
                     .help("Moves the formants down (bigger, above 0) or up (smaller, below 0).");
    Param resonance =
        percent("Resonance", 50).help("Narrows the bands: breathy at 0, ringing at 100.");
    Param mix = percent("Mix", 100).smoothed(20).help("The filtered share, the rest dry.");
    Param level = decibels("Level", -24, 24, 0).smoothed(20).help("The output level.");

    Input in = audioIn("In").help("A bright source to shape. Poly: each channel alone.");
    Output out = audioOut("Out").help("The input, voiced.");

    Array<Throat, 16> throat;

    void process(int frames) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Throat& t = throat[c];
            for (int i = 0; i < frames; ++i) {
                const double v = clamp(vowel.at(i, c), 0.0, 4.0);
                const int lo = min(floorToInt(v), 3);
                const double frac = v - lo;
                const double scale = exp2(-size.at(i, c) / 12.0);
                const double narrow = exp2(2.0 - 4.0 * clamp(resonance.at(i, c), 0.0, 1.0));
                const double x = in[c][i];
                double y = 0.0;
                for (int f = 0; f < 4; ++f) {
                    const double a = log(formantOf(lo, f)), b = log(formantOf(lo + 1, f));
                    const double hz = min(exp(lerp(a, b, frac)) * scale, 0.45 * sr);
                    const double g = tan(pi * hz / sr);
                    const double k = min(widthOf(f) * narrow / hz, 2.0);
                    y += levelOf(f) * t.band[f].run(x, g, k);
                }
                const double m = clamp(mix.at(i, c), 0.0, 1.0);
                out[c][i] = dbToGain(level.at(i, c)) * (m * 4.0 * y + (1.0 - m) * x);
            }
        }
    }
};
