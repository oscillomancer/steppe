// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A morphing oscillator, poly: one Shape knob sweeps continuously through a
// family of waves, sine (0) to triangle (20) to saw (40) to square (60) to
// a pulse narrowing to a buzz (80) to formant bursts (100), a windowed
// sine whose overtone climbs like a vowel. Warp bends the phase, squeezing
// each cycle's first half against its second: sharper, brassier, or
// hollower versions of every shape. Patch an LFO or an envelope into Shape
// and Warp: this is the oscillator to modulate. Sub is a square an octave
// down.
//
// Its edges are anti-aliased as the VCO's are: wherever a step or a corner
// of the current wave falls between two samples, a four-sample B-spline
// step or ramp of the right size smooths it, measured from the wave itself,
// so every shape and warp stays clean. The outputs run two samples late.

constexpr double kC4 = 261.6255653005986;

// The band-limited step's and ramp's residuals within a sample of the edge
// (the VCO's).
double stepNear(double a) {
    const double a3 = a * a * a, a4 = a3 * a;
    return a < 0.0 ? 0.5 + (4.0 * a - 2.0 * a3 - 0.75 * a4) / 6.0
                   : -0.5 + (4.0 * a - 2.0 * a3 + 0.75 * a4) / 6.0;
}
double rampNear(double a) {
    const double b = abs(a), b2 = b * b, b4 = b2 * b2;
    return 7.0 / 30.0 - 0.5 * b + (2.0 * b2 - 0.5 * b4 + 0.15 * b4 * b) / 6.0;
}

// The warp: the phase bent so its slope is a at the start and 1 / a at the
// end, and back.
double warpOf(double p, double a) { return p * a / (1.0 + p * (a - 1.0)); }
double unwarp(double q, double a) { return q / (a - q * (a - 1.0)); }

// The shapes, each in phase with the sine (rising through 0 at the start).
double triOf(double q) { return q < 0.25 ? 4.0 * q : (q < 0.75 ? 2.0 - 4.0 * q : 4.0 * q - 4.0); }
double sawOf(double q) { return q < 0.5 ? 2.0 * q : 2.0 * q - 2.0; }
double squareOf(double q) { return q < 0.5 ? 1.0 : -1.0; }
// A pulse of width w with no DC, its high at 1.
double pulseOf(double q, double w) { return ((q < w ? 1.0 : 0.0) - w) / (1.0 - w); }
double burstOf(double q, double k) { return sin(twoPi * k * q) * 0.5 * (1.0 - cos(twoPi * q)); }

struct Wave {
    double s = 0.0;  // the shape, 0..5
    double a = 1.0;  // the warp's slope
    double k = 1.0;  // the bursts' overtone

    double width() const { return s < 3.0 ? 0.5 : (s < 4.0 ? 0.5 - 0.45 * (s - 3.0) : 0.05); }
    // The wave at phase p (0..1).
    double at(double p) const {
        const double q = warpOf(p, a);
        if (s < 1.0) return lerp(sin(twoPi * q), triOf(q), s);
        if (s < 2.0) return lerp(triOf(q), sawOf(q), s - 1.0);
        if (s < 3.0) return lerp(sawOf(q), squareOf(q), s - 2.0);
        if (s < 4.0) return pulseOf(q, width());
        return lerp(pulseOf(q, 0.05), burstOf(q, k), s - 4.0);
    }
    // Where (in phase) the wave may step or turn: the wrap, the triangle's
    // corners, the saw's and square's middle, the pulse's fall.
    double edge(int e) const {
        if (e == 0) return 0.0;
        if (e == 1) return unwarp(0.25, a);
        if (e == 2) return unwarp(0.5, a);
        if (e == 3) return unwarp(0.75, a);
        return unwarp(width(), a);
    }
};

struct Voice {
    double phase = 0.0;
    int flip = 0;
    Array<double, 2> late2, late1, carry;
    Array<double, 2> back2, back1, now, ahead;

    void step(int w, double h, double d) {
        const double e = 1.0 - d;
        back2[w] += h * d * d * d * d / 24.0;
        back1[w] += h * stepNear(d - 1.0);
        now[w] += h * stepNear(d);
        ahead[w] -= h * e * e * e * e / 24.0;
    }
    void kink(int w, double k, double d) {
        const double e = 1.0 - d;
        back2[w] += k * d * d * d * d * d / 120.0;
        back1[w] += k * rampNear(e);
        now[w] += k * rampNear(d);
        ahead[w] += k * e * e * e * e * e / 120.0;
    }
};

struct Morph : Module {
    static constexpr ModuleInfo info = {
        .name = "Morph",
        .id = "com.suppleaudio.rack.morph",
        .vendor = "Supple Audio",
        .description =
            "A morphing oscillator: one knob from sine through saw and pulse to formant "
            "bursts, and a warp.",
        .tags = {"oscillator", "poly"}};

    Param shape = percent("Shape", 40)
                      .help(
                          "The wave: sine 0, triangle 20, saw 40, square 60, narrow pulse 80, "
                          "formant bursts 100.");
    Param warp = percent("Warp", 0, -100, 100)
                     .help(
                         "Bends each cycle: above 0 its start races and its end drags, below 0 the "
                         "reverse.");
    Param coarse = semitones("Coarse", -48, 48, 0).help("Transposes the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).help("Detunes the pitch in cents.");
    Param fmDepth = octaves("FM Depth", -4, 4, 1)
                        .help("How far the FM input bends the pitch, in octaves at a CV of 1.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: one voice per channel.");
    Input fm = cvIn("FM").help("Bends the pitch by FM Depth's octaves: vibrato, sweeps.");

    Output out = audioOut("Out").help("The morphing wave.");
    Output sub = audioOut("Sub").help("A square an octave below.");

    Array<Voice, 16> voice;
    Wave wave;

    void process(int frames) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voice[c];
            for (int i = 0; i < frames; ++i) {
                const double oct = pitch[c][i] + coarse.at(i, c) / 12.0 + fine.at(i, c) / 1200.0 +
                                   fmDepth.at(i, c) * fm[c][i];
                const double hz = kC4 * exp2(clamp(oct, -14.0, 7.0));
                const double inc = min(hz / sr, 0.45);
                wave.s = 5.0 * clamp(shape.at(i, c), 0.0, 1.0);
                wave.a = exp2(3.0 * clamp(warp.at(i, c), -1.0, 1.0));
                // The bursts' overtone climbs from 1 to 7, kept under Nyquist.
                wave.k = min(1.0 + 6.0 * clamp(wave.s - 4.0, 0.0, 1.0), 0.4 / inc);
                v.back2.fill(0.0);
                v.back1.fill(0.0);
                v.now.fill(0.0);
                v.ahead.fill(0.0);
                const double p = v.phase;
                const double q = p + inc;
                for (int e = 0; e < 5; ++e) {
                    const double x = wave.edge(e);
                    // A square's fall is the saw's middle: count it once.
                    if (e == 4 && abs(x - wave.edge(2)) < 1e-12) continue;
                    double at = -1.0;
                    if (p < x && x <= q) at = (x - p) / inc;
                    if (p < x + 1.0 && x + 1.0 <= q) at = (x + 1.0 - p) / inc;
                    if (at < 0.0) continue;
                    const double d = 1.0 - at;
                    // The step and the turn there, measured from the wave.
                    const double tiny = 1e-9, h = 1e-6;
                    const double lo = x <= 0.0 ? 1.0 : x;
                    const double before = wave.at(lo - tiny), after = wave.at(x + tiny);
                    const double slopeBefore = (before - wave.at(lo - tiny - h)) / h;
                    const double slopeAfter = (wave.at(x + tiny + h) - after) / h;
                    v.step(0, after - before, d);
                    v.kink(0, (slopeAfter - slopeBefore) * inc, d);
                    if (e == 0) {
                        v.step(1, v.flip == 0 ? -2.0 : 2.0, d);
                        v.flip = 1 - v.flip;
                    }
                }
                v.phase = q - floor(q);
                v.now[0] += wave.at(v.phase);
                v.now[1] += v.flip == 0 ? 1.0 : -1.0;
                for (int w = 0; w < 2; ++w) {
                    const double y = v.late2[w] + v.back2[w];
                    v.late2[w] = v.late1[w] + v.back1[w];
                    v.late1[w] = v.now[w] + v.carry[w];
                    v.carry[w] = v.ahead[w];
                    if (w == 0)
                        out[c][i] = y;
                    else
                        sub[c][i] = y;
                }
            }
        }
    }
};
