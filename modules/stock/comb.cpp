// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A tuned comb filter, poly: the input is fed back through a delay one
// cycle of the pitch long, so it rings at the pitch and its harmonics, a
// string or a tube. Noise becomes a pitched breath, a drum a tuned clang, a
// chord a drone at the note you play. Feedback sets how long it rings: above
// 0 every harmonic (a string's buzz), below 0 only the odd ones (the delay
// halves to stay at the pitch: a hollow, clarinet-like tube). Damping darkens
// each pass, so the high harmonics die sooner, from glassy to woody.
//
// Its tuning allows for the damping's delay, so it stays at the pitch. The
// ringing is scaled so a noise comes out about as loud at any Feedback, until
// near 100 the loop's soft limit holds it back.

constexpr double kC4 = 261.6255653005986;

struct Loop {
    DelayLine line;
    double low = 0.0;  // the damping's one-pole
};

struct Comb : Module {
    static constexpr ModuleInfo info = {
        .name = "Comb",
        .id = "com.suppleaudio.rack.comb",
        .vendor = "Supple Audio",
        .description =
            "A tuned comb filter: rings at the pitch, positive or negative feedback, "
            "damping.",
        .tags = {"filter", "resonator", "poly"}};

    Param tune = semitones("Tune", -48, 48, 0).help("Moves the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).help("Detunes the pitch in cents.");
    Param feedback =
        percent("Feedback", 85, -99, 99)
            .help("How long it rings: above 0 every harmonic, below 0 only the odd ones.");
    Param damping = percent("Damping", 20)
                        .help("Darkens each pass, so the high harmonics die sooner: 0 is none.");
    Param mix = percent("Mix", 100).smoothed(20).help("The comb's share, the rest dry.");

    Input in = audioIn("In").help("What to ring: noise, drums, anything. Poly: each alone.");
    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4: the note it rings at.");

    Output out = audioOut("Out").help("The ringing comb.");

    Array<Loop, 16> loop;

    void prepare() {
        // Down to 16 Hz.
        for (int c = 0; c < 16; ++c) loop[c].line.prepare(toInt(0.07 * sampleRate()) + 8);
    }

    void process(int frames) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Loop& l = loop[c];
            for (int i = 0; i < frames; ++i) {
                const double oct = pitch[c][i] + tune.at(i, c) / 12.0 + fine.at(i, c) / 1200.0;
                const double f0 = kC4 * exp2(clamp(oct, -4.0, 4.5));
                const double fb = clamp(feedback.at(i, c), -0.99, 0.99);
                // Negative feedback rings at odd multiples of half the loop's
                // frequency: a loop half as long keeps the pitch.
                const double cycle = fb < 0.0 ? sr / (2.0 * f0) : sr / f0;
                // The damping's one-pole: none at 0, about 50 Hz at 48 kHz at 100.
                const double a = exp(-5.0 * clamp(damping.at(i, c), 0.0, 1.0));
                // The one-pole's phase delay at the loop's frequency, taken
                // off the line.
                const double w = twoPi / cycle;
                const double b = 1.0 - a;
                const double lag = atan2(b * sin(w), 1.0 - b * cos(w)) / w;
                const double d = max(cycle - lag - 1.0, 1.0);
                const double x = in[c][i];
                const double back = l.line.read(d);
                l.low += a * (back - l.low);
                const double v = x + fb * l.low;
                l.line.push(3.0 * tanh(v / 3.0));
                const double m = clamp(mix.at(i, c), 0.0, 1.0);
                out[c][i] = m * v * sqrt(1.0 - fb * fb) + (1.0 - m) * x;
            }
        }
    }
};
