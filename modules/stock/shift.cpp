// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A frequency shifter, poly: every frequency in the input moves by the same
// number of Hz, up at Up and down at Down. Unlike a pitch shift, that pulls
// harmonics out of their ratios: a few Hz gives a slow phasing swirl, tens
// a metallic, bell-like clang, hundreds a radio's sideband garble. Shift
// passes through zero, so a CV can sweep it from down through still to up;
// Feedback sends Up back into the input, so each pass shifts again: a
// spiralling, barber-pole climb (or fall, with Shift below 0).
//
// Ring is an old-fashioned ring modulator beside it: the input times a sine
// at Carrier, or times whatever is patched into Carrier In, both sidebands
// at once.
//
// The shift comes from a pair of allpass chains whose outputs stay 90
// degrees apart from 16 Hz to about 20 kHz (at 44.1 kHz and up), within 0.16
// degrees: the other sideband is 57 dB down. Each chain is eight first-order
// sections; their corners came from a minimax fit of the phase difference
// over a 5000:1 band, scaled here to the sample rate.

// The chains' corners over the band's lowest frequency, in the bilinear
// transform's warped frequency: in-phase chain, then the one 90 degrees late.
double cornerOf(int chain, int k) {
    if (chain == 0) {
        switch (k) {
            case 0:
                return 0.88983251;
            case 1:
                return 3.35565898;
            case 2:
                return 11.494439;
            case 3:
                return 40.9550729;
            case 4:
                return 146.807588;
            case 5:
                return 519.929631;
            case 6:
                return 1966.9943;
            default:
                return 14768.0649;
        }
    }
    switch (k) {
        case 0:
            return 0.268675861;
        case 1:
            return 1.80343145;
        case 2:
            return 6.18290846;
        case 3:
            return 21.6124627;
        case 4:
            return 77.7405607;
        case 5:
            return 275.846741;
        case 6:
            return 994.61768;
        default:
            return 4264.24353;
    }
}

// One chain of eight first-order allpasses, (a - z^-1) / (1 - a z^-1) each.
struct Chain {
    Array<double, 8> a, x1, y1;

    void tune(int chain, double sr) {
        const double lowest = tan(pi * 16.0 / sr);
        for (int k = 0; k < 8; ++k) {
            const double p = cornerOf(chain, k) * lowest;
            a[k] = (1.0 - p) / (1.0 + p);
        }
    }
    double process(double x) {
        double v = x;
        for (int k = 0; k < 8; ++k) {
            const double y = a[k] * v - x1[k] + a[k] * y1[k];
            x1[k] = v;
            y1[k] = y;
            v = y;
        }
        return v;
    }
};

struct Shifter {
    Chain real, imag;
    double phase = 0.0;      // the shift's
    double ringPhase = 0.0;  // the ring's carrier
    double back = 0.0;       // the last Up, for feedback
};

struct Shift : Module {
    static constexpr ModuleInfo info = {
        .name = "Shift",
        .id = "com.suppleaudio.rack.shift",
        .vendor = "Supple Audio",
        .description =
            "A frequency shifter, through zero, up and down at once, with feedback; "
            "and a ring modulator.",
        .tags = {"effect", "frequency-shifter", "ring-modulator", "poly"}};

    // In Hz, but not a frequency's octave cables: it passes through 0.
    Param shift = continuous("Shift", -2000, 2000, 50)
                      .in("Shift")
                      .help("How many Hz every frequency moves up at Up (and down at Down).");
    Param fine = continuous("Fine", -20, 20, 0)
                     .in("Shift")
                     .help("Adds up to 20 Hz either way: slow swirls near zero.");
    Param cvDepth = continuous("CV Depth", -2000, 2000, 100)
                        .in("Shift")
                        .help("The Hz the CV input adds at a CV of 1, either way.");
    Param feedback = percent("Feedback", 0, 0, 95)
                         .in("Shift")
                         .help("Sends Up back into the input: each pass shifts again, a spiral.");
    Param carrier =
        frequency("Carrier", 1, 5000, 300).in("Ring").help("The ring modulator's sine.");

    Input in = audioIn("In").help("What to shift. Poly: each channel alone.");
    Input cv = cvIn("CV").help("Adds CV Depth's Hz per unit: sweeps through zero.");
    Input carrierIn =
        audioIn("Carrier In").help("While patched, the ring multiplies by this instead.");

    Output up = audioOut("Up").help("The input shifted up by Shift's Hz.");
    Output down = audioOut("Down").help("The input shifted down by Shift's Hz.");
    Output ring = audioOut("Ring").help("The input times the carrier: both sidebands.");

    Array<Shifter, 16> shifter;

    void prepare() {
        for (int c = 0; c < 16; ++c) {
            shifter[c].real.tune(0, sampleRate());
            shifter[c].imag.tune(1, sampleRate());
        }
    }

    void process(int frames) {
        const double sr = sampleRate();
        const bool external = carrierIn.connected();
        for (int c = 0; c < channels(); ++c) {
            Shifter& s = shifter[c];
            for (int i = 0; i < frames; ++i) {
                const double x = in[c][i];
                const double fb = clamp(feedback.at(i, c), 0.0, 0.95);
                const double fed = x + fb * s.back;
                const double re = s.real.process(fed);
                const double im = s.imag.process(fed);
                const double hz = shift.at(i, c) + fine.at(i, c) + cvDepth.at(i, c) * cv[c][i];
                s.phase += clamp(hz / sr, -0.5, 0.5);
                s.phase -= floor(s.phase);
                const double co = cos(twoPi * s.phase), si = sin(twoPi * s.phase);
                const double u = re * co - im * si;
                up[c][i] = u;
                down[c][i] = re * co + im * si;
                s.back = 3.0 * tanh(u / 3.0);
                double m = 0.0;
                if (external) {
                    m = carrierIn[c][i];
                } else {
                    s.ringPhase += min(carrier.at(i, c) / sr, 0.5);
                    s.ringPhase -= floor(s.ringPhase);
                    m = sin(twoPi * s.ringPhase);
                }
                ring[c][i] = x * m;
            }
        }
    }
};
