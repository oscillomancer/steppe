// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Width: a stereo pair as mid and side, poly. The mid is what the two sides
// share, (L + R) / 2; the side is how they differ, (L - R) / 2. Width
// scales the side: 0 folds the pair to mono, 100 leaves it as it came, 200
// doubles the difference (wider, and the centre seems further back). Mono
// Below takes the side out under its frequency, so the bass stays in the
// centre where it hits hardest (a 12 dB/oct high pass on the side; off at
// 20 Hz).
//
// Mid and Side are the two halves as signals, Side after Width and Mono
// Below: process one (a reverb on the side, a compressor on the mid) and
// rebuild the pair with a second Width set to Mid/Side, which reads In L as
// the mid and In R as the side. Mono into In L alone (In R follows it) has
// no side, so it passes unchanged at any width.

struct SideFilter {
    Biquad high;
};

struct Width : Module {
    static constexpr ModuleInfo info = {
        .name = "Width",
        .id = "com.suppleaudio.rack.width",
        .vendor = "Supple Audio",
        .description =
            "Stereo width by mid and side: narrow to mono or widen, keep the bass in the "
            "centre, split and rebuild a pair as mid and side.",
        .tags = {"stereo", "utility", "effect", "poly"}};

    Param mode = choice("Input", {"L/R", "Mid/Side"}, 0)
                     .help(
                         "L/R: a stereo pair. Mid/Side: In L is the mid and In R the side, "
                         "to rebuild a pair split by another Width.");
    Param width = percent("Width", 100, 0, 200)
                      .smoothed(20)
                      .help("The side's level: 0 is mono, 100 as it came, 200 twice as wide.");
    Param monoBelow = frequency("Mono Below", 20, 500, 20)
                          .help(
                              "Takes the side out under this frequency, so the bass stays centred; "
                              "off at 20 Hz.");
    Param level = decibels("Level", -24, 12, 0).smoothed(20).help("The gain after, on both sides.");

    Input inL = audioIn("In L").help("The left input, or mono; the mid in Mid/Side.");
    Input inR = audioIn("In R").help(
        "The right input; follows In L while unpatched. The side in Mid/Side.");

    Output outL = audioOut("Out L").help("The left side.");
    Output outR = audioOut("Out R").help("The right side.");
    Output mid = audioOut("Mid").help("What the sides share, (L + R) / 2, after Level.");
    Output side = audioOut("Side").help(
        "How they differ, (L - R) / 2, after Width, Mono Below "
        "and Level.");

    Array<SideFilter, 16> filters;

    void prepare() {
        for (int c = 0; c < 16; ++c) filters[c].high.clear();
    }

    void process(int frames) {
        const bool msIn = mode.index() == 1;
        const bool stereoIn = inR.connected();
        const double hz = monoBelow.value();
        const bool bassMono = hz > 20.5;
        const int n = max(channels(), 1);
        for (int c = 0; c < n; ++c) filters[c].high.highpass(max(hz, 20.0), 0.7071);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < n; ++c) {
                const double a = inL[c][i];
                double m = 0.0, s = 0.0;
                if (msIn) {
                    m = a;
                    s = inR[c][i];  // no side while unpatched
                } else {
                    const double b = stereoIn ? inR[c][i] : a;
                    m = 0.5 * (a + b);
                    s = 0.5 * (a - b);
                }
                const double w = clamp(width.at(i, c), 0.0, 2.0);
                if (bassMono) s = filters[c].high.process(s);
                const double g = pow(10.0, level.at(i, c) / 20.0);
                m *= g;
                s *= w * g;
                outL[c][i] = m + s;
                outR[c][i] = m - s;
                mid[c][i] = m;
                side[c][i] = s;
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Mono").set(width, 0);
        p.add("Narrow").set(width, 50);
        p.add("Wide Bass Centred").set(width, 150).set(monoBelow, 120);
        p.add("Extra Wide").set(width, 200).set(monoBelow, 150).set(level, -3);
        p.add("Mid/Side Decode").set(mode, 1);
    }
};
