// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A west-coast wavefolder, poly: a sine or triangle in, a bright, vocal,
// shifting tone out. Fold drives the wave past the folder's edges, where it
// turns back on itself; each turn adds harmonics, so sweeping Fold (a cable
// from an envelope or an LFO into it) makes the tone sing and talk. Stages
// sets how many times it can turn before it just clips; Symmetry tilts the
// folds for even harmonics, a hollow, reedy colour. Soft is the same input
// at the same drive through a plain soft clip, for contrast or blending.
//
// Each fold is a cell that bends the wave back at an edge (2, 4, 6 ... past
// the first): a reflection with a rounded corner, so the folds stay smooth.
// It runs at four times the sample rate, which keeps the new harmonics from
// folding back down as aliases.

// A smooth max(a, 0): a corner rounded over about `w`.
double ramp(double a, double w) { return 0.5 * (a + sqrt(a * a + w * w)); }

// The folds of x, `stages` of them, odd about 0.
double folds(double x, int stages) {
    const double a = abs(x);
    double y = a;
    double sign = -2.0;
    for (int j = 0; j < stages; ++j) {
        const double edge = 2.0 * j + 1.0;
        y += sign * (ramp(a - edge, 0.15) - ramp(-edge, 0.15));
        sign = -sign;
    }
    // Past the last fold, a gentle clip (nothing below 1).
    if (y > 1.0) y = 1.0 + 0.3 * tanh((y - 1.0) / 0.3);
    return x < 0.0 ? -y : y;
}

struct Cell {
    Oversampler fold, soft;
    OnePole dcFold, dcSoft;
};

struct Fold : Module {
    static constexpr ModuleInfo info = {
        .name = "Fold",
        .id = "com.suppleaudio.rack.fold",
        .vendor = "Supple Audio",
        .description =
            "A west-coast wavefolder: fold, symmetry and stages, oversampled; and a "
            "soft clip.",
        .tags = {"waveshaper", "distortion", "poly"}};

    Param fold =
        percent("Fold", 35)
            .help("How far the wave is driven past the edges: each turn adds harmonics. Sweep it.");
    Param symmetry = percent("Symmetry", 0, -100, 100)
                         .help("Tilts the folds for even harmonics: 0 folds both halves alike.");
    Param stages =
        integer("Stages", 1, 6, 4).help("How many times the wave can turn back before it clips.");
    Param level = decibels("Level", -24, 6, 0).smoothed(10).help("The output level.");

    Input in = audioIn("In").help("What it folds: a sine or triangle sings best.");

    Output out = audioOut("Out").help("The folded wave.");
    Output soft = audioOut("Soft").help("The input at the same drive through a soft clip.");

    Array<Cell, 16> cell;

    void prepare() {
        for (int c = 0; c < 16; ++c) {
            cell[c].fold.prepare(4);
            cell[c].soft.prepare(4);
            cell[c].dcFold.setCutoff(8.0);
            cell[c].dcSoft.setCutoff(8.0);
        }
    }

    void process(int frames) {
        const int n = clamp(stages.index(), 1, 6);
        for (int c = 0; c < channels(); ++c) {
            Cell& s = cell[c];
            for (int i = 0; i < frames; ++i) {
                const double gain = exp2(3.6 * clamp(fold.at(i, c), 0.0, 1.0));
                const double bias = clamp(symmetry.at(i, c), -1.0, 1.0);
                const double x = in[c][i];
                s.fold.up(x);
                s.soft.up(x);
                for (int k = 0; k < 4; ++k) {
                    const double driven = gain * s.fold.sample(k) + bias;
                    s.fold.set(k, folds(driven, n));
                    s.soft.set(k, tanh(gain * s.soft.sample(k) + bias));
                }
                // 0.8: the folds' sharp turns overshoot a little coming down.
                const double g = 0.8 * dbToGain(level.at(i, c));
                out[c][i] = g * s.dcFold.highpass(s.fold.down());
                soft[c][i] = g * s.dcSoft.highpass(s.soft.down());
            }
        }
    }
};
