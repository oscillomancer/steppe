// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A stereo chorus and flanger, poly. The input is delayed and the delay
// swept by an LFO, so the copy's pitch wavers; mixed with the dry input it
// thickens and shimmers.
//
//   Chorus   up to six voices, delays of 6 to 22 ms swept by sines spread
//            evenly through the cycle, with a quicker, smaller wobble on
//            top: a string ensemble's lush, moving width.
//   Flanger  a delay of 0.25 ms up to 8 swept by a triangle (in octaves of
//            delay, so the sweep sounds even): the jet's whoosh of a comb
//            of notches moving up and down. Feedback deepens it to a
//            metallic ring, positive or negative (hollower).
//
// Rate is the LFO's speed, Depth how far it sweeps, Spread how far apart in
// the cycle the left and right sides run (a quarter cycle at 100: wide),
// Mix the share of the swept copies (50 gives a flanger its deepest
// notches). In R follows In L while nothing is patched into it.

double triangle01(double p) { return p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p; }

struct Lines {
    DelayLine left, right;
};

struct Chorus : Module {
    static constexpr ModuleInfo info = {
        .name = "Chorus",
        .id = "com.suppleaudio.rack.chorus",
        .vendor = "Supple Audio",
        .description =
            "A stereo chorus ensemble with a flanger mode: rate, depth, voices, "
            "spread, feedback.",
        .tags = {"modulation", "effect", "stereo", "poly"}};

    Param mode = choice("Mode", {"Chorus", "Flanger"}, 0)
                     .help("Chorus: longer delays, several voices. Flanger: a short swept comb.");
    Param rate = frequency("Rate", 0.02, 10, 0.5).help("The LFO's speed.");
    Param depth = percent("Depth", 50).help("How far the LFO sweeps the delay.");
    Param voices = integer("Voices", 1, 6, 3)
                       .help("How many swept copies, spread evenly through the LFO's cycle.");
    Param spread =
        percent("Spread", 70).help("How far apart the left and right sweeps run: 0 is mono.");
    Param feedback = percent("Feedback", 0, -95, 95)
                         .help("Feeds the copies back into the delay: a flanger's ring.");
    Param mix = percent("Mix", 50).smoothed(20).help("The swept copies' share; 50 is classic.");

    Input inL = audioIn("In L").help("The left input, or mono. Poly: each channel alone.");
    Input inR = audioIn("In R").help("The right input; follows In L while unpatched.");

    Output outL = audioOut("Out L").help("The left side.");
    Output outR = audioOut("Out R").help("The right side.");

    Array<Lines, 16> lines;
    double phase = 0.0;   // the LFO's
    double wobble = 0.0;  // the chorus's quicker one

    void prepare() {
        const int longest = toInt(0.03 * sampleRate()) + 8;
        for (int c = 0; c < 16; ++c) {
            lines[c].left.prepare(longest);
            lines[c].right.prepare(longest);
        }
    }

    // A voice's delay in samples at LFO phase p (0..1) and quick phase q.
    double delayAt(bool flanger, double p, double q, double d, double sr) const {
        if (flanger) {
            const double top = 0.25 + 7.75 * d;
            return 0.00025 * sr * exp(log(top / 0.25) * triangle01(p));
        }
        const double slow = 0.5 + 0.5 * sin(twoPi * p);
        const double quick = 0.5 + 0.5 * sin(twoPi * q);
        return (0.006 + d * (0.014 * slow + 0.002 * quick)) * sr;
    }

    void process(int frames) {
        const double sr = sampleRate();
        const bool flanger = mode.index() == 1;
        const bool stereoIn = inR.connected();
        const int n = clamp(roundToInt(voices.at(0, 0)), 1, 6);
        const double count = n;
        for (int i = 0; i < frames; ++i) {
            const double hz = clamp(rate.at(i, 0), 0.001, 20.0);
            const double d = clamp(depth.at(i, 0), 0.0, 1.0);
            const double offset = 0.25 * clamp(spread.at(i, 0), 0.0, 1.0);
            const double fb = clamp(feedback.at(i, 0), -0.95, 0.95);
            const double w = clamp(mix.at(i, 0), 0.0, 1.0);
            for (int c = 0; c < channels(); ++c) {
                Lines& l = lines[c];
                const double xl = inL[c][i];
                const double xr = stereoIn ? inR[c][i] : xl;
                // Read before writing this frame's sample: a delay of D frames is
                // read(D - 1), and the feedback's loop is D long too.
                double sumL = 0.0, sumR = 0.0;
                for (int k = 0; k < n; ++k) {
                    const double p = phase + k / count;
                    const double q = wobble + k / count;
                    const double pr = p + offset;
                    const double dl = delayAt(flanger, p - floor(p), q - floor(q), d, sr);
                    const double dr = delayAt(flanger, pr - floor(pr), q - floor(q), d, sr);
                    sumL += l.left.read(dl - 1.0);
                    sumR += l.right.read(dr - 1.0);
                }
                const double feedL = xl + fb * sumL / count;
                const double feedR = xr + fb * sumR / count;
                l.left.push(3.0 * tanh(feedL / 3.0));
                l.right.push(3.0 * tanh(feedR / 3.0));
                const double gain = 1.0 / sqrt(count);
                outL[c][i] = (1.0 - w) * xl + w * sumL * gain;
                outR[c][i] = (1.0 - w) * xr + w * sumR * gain;
            }
            phase += hz / sr;
            phase -= floor(phase);
            wobble += 7.3 * hz / sr;
            wobble -= floor(wobble);
        }
    }
};
