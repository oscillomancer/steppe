// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A phaser, stereo and poly: the input runs through a chain of all-pass
// stages that shift its phase more and more with frequency, and mixed back
// with itself it loses the frequencies the chain turns half a cycle: a
// comb of soft notches, which an LFO sweeps up and down around Centre for
// the swirl. More stages make more notches (two for every four). Feedback
// sends the chain's output round again, sharpening the notches and the
// peaks between them into a vocal, resonant sweep; below 0 the peaks and
// notches swap. Spread offsets the right side's LFO for a wide sweep.
// Mix 50 makes the deepest notches.

struct Side {
    Array<double, 12> state;  // each all-pass stage's one state
    double back = 0.0;        // the chain's last output, for Feedback
};

struct Phaser : Module {
    static constexpr ModuleInfo info = {
        .name = "Phaser",
        .id = "com.suppleaudio.rack.phaser",
        .vendor = "Supple Audio",
        .description =
            "A phaser: 4 to 12 all-pass stages swept by an LFO, centre, feedback either way, "
            "stereo spread.",
        .tags = {"effect", "modulation", "stereo", "poly"}};

    Param stages = choice("Stages", {"4", "6", "8", "12"}, 1)
                       .keys({"four", "six", "eight", "twelve"})
                       .help("The all-pass stages: two notches for every four.");
    Param rate = frequency("Rate", 0.02, 10, 0.3).in("Sweep").help("The LFO's speed.");
    Param depth = percent("Depth", 70)
                      .in("Sweep")
                      .help("How far the LFO sweeps: two octaves either way of Centre at 100.");
    Param centre = frequency("Centre", 100, 5000, 700)
                       .in("Sweep")
                       .help("Where the sweep is centred: the stages' turning point.");
    Param spread = degrees("Spread", 0, 180, 90)
                       .in("Sweep")
                       .help("The right side's LFO this far ahead of the left's: 0 is mono.");
    Param feedback = percent("Feedback", 40, -95, 95)
                         .in("Tone")
                         .help(
                             "Sends the chain round again: sharper, resonant notches; below 0 they "
                             "swap with the peaks.");
    Param mix = percent("Mix", 50).smoothed(20).in("Tone").help(
        "The phased share: 50 makes the deepest notches, 100 only a vibrato-like shift.");

    Input inL = audioIn("In L").help("The left input, or mono. Poly: each channel alone.");
    Input inR = audioIn("In R").help("The right input; follows In L while unpatched.");
    Output outL = audioOut("Out L").help("The left side.");
    Output outR = audioOut("Out R").help("The right side.");

    Array<Side, 32> side;  // channel c's left at 2c, right at 2c + 1
    double lfo = 0.0;      // the LFO's phase, 0..1

    // One sample of a side through n stages swept to hz.
    double chain(Side& s, double x, int n, double hz, double fb) {
        const double t = tan(pi * hz / sampleRate());
        const double a = (t - 1.0) / (t + 1.0);
        double v = x + 3.0 * tanh(fb * s.back / 3.0);
        for (int k = 0; k < n; ++k) {
            // A first-order all-pass in transposed form: one state each.
            const double y = a * v + s.state[k];
            s.state[k] = v - a * y;
            v = y;
        }
        s.back = v;
        return v;
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int n = stages.index() == 3 ? 12 : 4 + 2 * stages.index();
        for (int i = 0; i < frames; ++i) {
            lfo += rate.at(i, 0) / sr;
            lfo -= floor(lfo);
            const double reach = 2.0 * clamp(depth.at(i, 0), 0.0, 1.0);
            const double top = 0.45 * sr;
            const double hzL = min(centre.at(i, 0) * exp2(reach * sin(twoPi * lfo)), top);
            const double hzR = min(
                centre.at(i, 0) * exp2(reach * sin(twoPi * (lfo + spread.at(i, 0) / 360.0))), top);
            const double fb = clamp(feedback.at(i, 0), -0.95, 0.95);
            const double m = clamp(mix.at(i, 0), 0.0, 1.0);
            for (int c = 0; c < channels(); ++c) {
                const double l = inL[c][i];
                const double r = inR.connected() ? inR[c][i] : l;
                const double wl = chain(side[2 * c], l, n, hzL, fb);
                const double wr = chain(side[2 * c + 1], r, n, hzR, fb);
                outL[c][i] = (1.0 - m) * l + m * wl;
                outR[c][i] = (1.0 - m) * r + m * wr;
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Classic").set(stages, "four").set(rate, 0.4).set(depth, 70).set(feedback, 30);
        p.add("Slow Swirl").set(rate, 0.08).set(depth, 90).set(centre, 900).set(spread, 120);
        p.add("Jet").set(stages, "twelve").set(rate, 0.15).set(feedback, 75).set(depth, 80);
        p.add("Vibe").set(stages, "four").set(rate, 4).set(depth, 35).set(feedback, 0).set(mix, 50);
        p.add("Hollow").set(stages, "eight").set(rate, 0.25).set(feedback, -60);
    }
};
