// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A pitch shifter, poly: moves whatever comes in up or down by up to two
// octaves, at once, without changing its speed. Harmonies from a single
// line, octave-down bass from a lead, chipmunks and giants; with Feedback,
// each pass goes through again, so a shift of 12 climbs in octaves into a
// shimmering cloud and a shift of -1 spirals down.
//
// The input runs into a delay read by two taps half a Window apart, each
// gliding through the window at the speed that changes the pitch and
// fading in and out so one is silent when it jumps back. Where it lands
// is moved by up to 15 ms to where the input best matches what the other
// tap is reading, so the two cross-fade in step: a held note keeps its
// pitch and its tone instead of beating. Short windows follow the input
// tightly but buzz on low notes; long ones are smooth but smear and echo:
// 30 to 60 ms suits most material. The shifted sound is late by about
// half a Window.

constexpr double kMaxWindowMs = 200.0;
constexpr double kSearchMs = 15.0;

struct Shifter {
    DelayLine line;
    double phase = 0.0;             // where tap A is in the window, 0..1; tap B is half a window on
    double offA = 0.0, offB = 0.0;  // each tap's landing moved, in samples
    double back = 0.0;              // the last shifted sample, for Feedback
    double hold = 0.0;              // 1 while the pitch stands still: one tap alone
};

struct PitchShift : Module {
    static constexpr ModuleInfo info = {
        .name = "Pitch Shift",
        .id = "com.suppleaudio.rack.pitchshift",
        .vendor = "Supple Audio",
        .description =
            "A pitch shifter: up to two octaves either way, window size, feedback "
            "for shimmer.",
        .tags = {"effect", "pitch", "poly"}};

    Param shift = semitones("Shift", -24, 24, 12).help("Moves the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).help("Moves the pitch in cents, for detuning.");
    Param window = milliseconds("Window", 10, kMaxWindowMs, 40)
                       .help(
                           "The taps' span: short follows tightly, long is smoother but "
                           "smears.");
    Param feedback = percent("Feedback", 0, 0, 95)
                         .help("Sends the shifted sound back in, so it shifts again and again.");
    Param mix = percent("Mix", 100).smoothed(20).help("The shifted share, the rest dry.");

    Input in = audioIn("In").help("What to shift. Poly: each channel alone.");
    Output out = audioOut("Out").help("The shifted sound, and the dry input by Mix.");

    Array<Shifter, 16> shifter;

    void prepare() {
        const int size = toInt((kMaxWindowMs + kSearchMs) * 0.001 * sampleRate()) + 160;
        for (int c = 0; c < 16; ++c) shifter[c].line.prepare(size);
    }

    // How alike the input is at delays `at` and `partner`, over 128
    // samples back from each: -1 to 1.
    double match(const Shifter& s, double at, double partner) const {
        double xy = 0.0, xx = 1e-12, yy = 1e-12;
        for (int j = 0; j < 32; ++j) {
            const double a = s.line.read(at + 4.0 * j);
            const double b = s.line.read(partner + 4.0 * j);
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }
        return xy / sqrt(xx * yy);
    }

    // How far, 0 to span samples, to move a tap landing at delay `from` so
    // it reads in step with the other, at `partner`.
    double align(const Shifter& s, double from, double partner, int span) const {
        int best = 0;
        double score = -2.0;
        for (int d = 0; d <= span; d += 2) {
            const double v = match(s, from + d, partner);
            if (v > score) {
                score = v;
                best = d;
            }
        }
        const double below = best > 0 ? match(s, from + best - 1, partner) : -2.0;
        const double above = match(s, from + best + 1, partner);
        if (below > score && below >= above) return best - 1.0;
        if (above > score) return best + 1.0;
        return best;
    }

    void process(int frames) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Shifter& s = shifter[c];
            for (int i = 0; i < frames; ++i) {
                const double ratio = exp2(shift.at(i, c) / 12.0 + fine.at(i, c) / 1200.0);
                const double w = clamp(window.at(i, c), 10.0, kMaxWindowMs) * 0.001 * sr;
                const double fb = clamp(feedback.at(i, c), 0.0, 0.95);
                const double x = in[c][i];
                // The fed-back sound is limited softly, so the loop stays bounded.
                s.line.push(x + fb * 3.0 * tanh(s.back / 3.0));
                // Each tap's delay, two samples clear of the newest, and its
                // gain: a Hann window over its trip, the two summing to 1.
                // While the pitch stands still the taps do too, and the one
                // nearer the window's middle takes over, so there is one
                // copy of the input, not two a comb apart.
                const double pa = s.phase;
                const double pb = pa < 0.5 ? pa + 0.5 : pa - 0.5;
                const double sa = sin(pi * pa);
                const double still = abs(ratio - 1.0) < 1e-6 ? 1.0 : -1.0;
                s.hold = clamp(s.hold + still / (0.05 * sr), 0.0, 1.0);
                const double ga = lerp(sa * sa, sa * sa >= 0.5 ? 1.0 : 0.0, s.hold);
                const double ya = s.line.read(2.0 + pa * w + s.offA);
                const double yb = s.line.read(2.0 + pb * w + s.offB);
                const double y = ga * ya + (1.0 - ga) * yb;
                s.back = y;
                // A tap reading ratio samples of the past each sample moves
                // its delay by 1 - ratio. One that jumps back (A across 0,
                // B across a half) lands in step with the other.
                const double next = s.phase + (1.0 - ratio) / w;
                const double na = next - floor(next);
                const double nb = na < 0.5 ? na + 0.5 : na - 0.5;
                const bool jumpA = next >= 1.0 || next < 0.0;
                const bool jumpB = !jumpA && ((pa < 0.5) != (na < 0.5));
                s.phase = na;
                const int span = toInt(min(0.5 * w, kSearchMs * 0.001 * sr));
                if (jumpA) s.offA = align(s, 2.0 + na * w, 2.0 + nb * w + s.offB, span);
                if (jumpB) s.offB = align(s, 2.0 + nb * w, 2.0 + na * w + s.offA, span);
                const double m = clamp(mix.at(i, c), 0.0, 1.0);
                out[c][i] = m * y + (1.0 - m) * x;
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Octave Up").set(shift, 12);
        p.add("Octave Down").set(shift, -12).set(window, 60);
        p.add("Fifth Harmony").set(shift, 7).set(mix, 50);
        p.add("Shimmer").set(shift, 12).set(window, 80).set(feedback, 60).set(mix, 40);
        p.add("Doubler").set(shift, 0).set(fine, 9).set(window, 30).set(mix, 50);
    }
};
