// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A phase shaper, poly: a 0..1 phase in (a Phasor's, an Operator's), a wave
// out, drawn by Curve: sine, triangle, saw, pulse (of Width), a smooth step
// (a square with rounded shoulders) or a window (a sine Width overtones up,
// faded in and out each cycle: a resonant, vocal tone). Skew bends the phase
// before the curve, which is phase distortion: a skewed sine grows brassy;
// Fold folds the result back on itself for more overtones. Two shapers on
// one phasor make two waves locked together.
//
// Its steps are smoothed (a two-sample polynomial step) at the rate the
// phase moves, measured from the input.

double warpOf(double p, double a) { return p * a / (1.0 + p * (a - 1.0)); }
double unwarp(double q, double a) { return q / (a - q * (a - 1.0)); }
double triOf(double q) { return q < 0.25 ? 4.0 * q : (q < 0.75 ? 2.0 - 4.0 * q : 4.0 * q - 4.0); }
double smooth(double u) { return u * u * (3.0 - 2.0 * u); }
// Folds x back at +-1, as often as it takes.
double foldOf(double x) {
    const double t = (x + 1.0) * 0.25;
    return 4.0 * abs(t - floor(t) - 0.5) - 1.0;
}
// The two-sample step's residual at t (phase since the edge, wrapped) for a
// phase rate dt, per unit of step.
double blep(double t, double dt) {
    if (t < dt) {
        const double x = t / dt;
        return -0.5 * (1.0 - x) * (1.0 - x);
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt;
        return 0.5 * (1.0 + x) * (1.0 + x);
    }
    return 0.0;
}

struct Drawing {
    int curve = 0;
    double a = 1.0, width = 0.5, gain = 1.0;

    double at(double p) const {
        const double q = warpOf(p - floor(p), a);
        double y = 0.0;
        if (curve == 0) {
            y = sin(twoPi * q);
        } else if (curve == 1) {
            y = triOf(q);
        } else if (curve == 2) {
            y = 2.0 * q - 1.0;
        } else if (curve == 3) {
            y = q < width ? 1.0 : -1.0;
        } else if (curve == 4) {
            const double u = q < 0.5 ? 2.0 * q : 2.0 - 2.0 * q;
            y = 2.0 * smooth(smooth(u)) - 1.0;
        } else {
            const double k = 1.0 + roundToInt(15.0 * width);
            y = sin(twoPi * k * q) * 0.5 * (1.0 - cos(twoPi * q));
        }
        return gain > 1.0 ? foldOf(y * gain) : y;
    }
};

struct Track {
    double last = 0.0;
};

struct Shaper : Module {
    static constexpr ModuleInfo info = {
        .name = "Shaper",
        .id = "com.suppleaudio.rack.shaper",
        .vendor = "Supple Audio",
        .description =
            "A phase shaper: phase in, wave out by a curve; skew and fold for phase "
            "distortion.",
        .tags = {"waveshaper", "phase", "poly"}};

    Param curve = choice("Curve", {"Sine", "Triangle", "Saw", "Pulse", "Smooth Step", "Window"}, 0)
                      .help("The wave drawn from the phase.");
    Param skew =
        percent("Skew", 0, -100, 100)
            .help("Bends the phase before the curve: phase distortion, a sine grows brassy.");
    Param width =
        percent("Width", 50, 1, 99)
            .help("The pulse's high share; for Window, how many overtones up its sine rings.");
    Param fold = percent("Fold", 0).help("Folds the wave back on itself for more overtones.");

    Input phase = phaseIn("Phase").help("A 0..1 phase: a Phasor's or an Operator's.");

    Output out = audioOut("Out").help("The wave.");

    Array<Track, 16> track;
    Drawing shape;

    void process(int frames) {
        shape.curve = curve.index();
        for (int c = 0; c < channels(); ++c) {
            Track& t = track[c];
            for (int i = 0; i < frames; ++i) {
                const double p = phase[c][i];
                double moved = p - t.last;
                moved -= floor(moved + 0.5);
                t.last = p;
                const double dt = min(abs(moved), 0.5);
                shape.a = exp2(3.0 * clamp(skew.at(i, c), -1.0, 1.0));
                shape.width = clamp(width.at(i, c), 0.01, 0.99);
                shape.gain = 1.0 + 5.0 * clamp(fold.at(i, c), 0.0, 1.0);
                double y = shape.at(p);
                // Steps: the saw's and pulse's wrap, the pulse's fall.
                if (dt > 0.0 && (shape.curve == 2 || shape.curve == 3)) {
                    for (int e = 0; e < 2; ++e) {
                        if (e == 1 && shape.curve != 3) continue;
                        const double x = e == 0 ? 0.0 : unwarp(shape.width, shape.a);
                        const double lo = x <= 0.0 ? 1.0 : x;
                        const double h = shape.at(x + 1e-9) - shape.at(lo - 1e-9);
                        double since = p - x;
                        since -= floor(since);
                        y += h * blep(since, dt);
                    }
                }
                out[c][i] = y;
            }
        }
    }
};
