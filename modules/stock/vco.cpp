// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// The workhorse oscillator: sine, triangle, saw, pulse and a sub-octave square
// at once, all anti-aliased, poly. Pitch is 1 per octave from C4, plus the
// coarse and fine knobs. Exp FM bends the pitch (vibrato, sweeps); Lin FM
// moves the frequency through zero by its index times the pitch, so an FM
// timbre stays the same up the keyboard; Sync restarts the cycle each time
// it rises above 0 (another oscillator there is hard sync). A cable into
// Pulse Width is PWM.
//
// Every edge the waveforms make (a wrap, the pulse's fall, the triangle's
// corners, a sync reset, the sub's flips) is smoothed where it fell between
// the samples, in either direction, by a four-sample step (or ramp): a cubic
// B-spline's integral, which keeps aliases some 10 dB under a two-sample
// polyBLEP's. So the outputs run two samples behind the phase.

constexpr double kC4 = 261.6255653005986;
constexpr int kSine = 0;
constexpr int kTri = 1;
constexpr int kSaw = 2;
constexpr int kPulse = 3;
constexpr int kSub = 4;

double sineAt(double p) { return sin(twoPi * p); }
double triAt(double p) { return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p; }
double sawAt(double p) { return 2.0 * p - 1.0; }
double pulseAt(double p, double pw) { return p < pw ? 1.0 : -1.0; }
double subAt(int flip) { return flip == 0 ? 1.0 : -1.0; }

// The band-limited step's residual (the B-spline's integral minus the step)
// within a sample of the edge, a in [-1, 1); beyond it, d^4 / 24 before and
// -(1 - d)^4 / 24 after.
double stepNear(double a) {
    const double a3 = a * a * a, a4 = a3 * a;
    return a < 0.0 ? 0.5 + (4.0 * a - 2.0 * a3 - 0.75 * a4) / 6.0
                   : -0.5 + (4.0 * a - 2.0 * a3 + 0.75 * a4) / 6.0;
}
// The band-limited ramp's residual (its integral) within a sample, |a| <= 1;
// beyond it, (2 - |a|)^5 / 120.
double rampNear(double a) {
    const double b = abs(a), b2 = b * b, b4 = b2 * b2;
    return 7.0 / 30.0 - 0.5 * b + (2.0 * b2 - 0.5 * b4 + 0.15 * b4 * b) / 6.0;
}

// When a phase moving from p to q (unwrapped, |q - p| < 1) crosses level x
// (0..1), as a time in the sample (t0 at p, at the rate inc); -1 if it does
// not.
double crossing(double p, double q, double x, double inc, double t0) {
    if (q > p) {
        if (p < x && x <= q) return t0 + (x - p) / inc;
        if (p < x + 1.0 && x + 1.0 <= q) return t0 + (x + 1.0 - p) / inc;
    } else if (q < p) {
        if (q < x && x <= p) return t0 + (x - p) / inc;
        if (q < x - 1.0 && x - 1.0 <= p) return t0 + (x - 1.0 - p) / inc;
    }
    return -1.0;
}

struct Voice {
    double phase = 0.0;
    int flip = 0;  // the sub's half
    bool armed = false;
    double lastSync = 0.0;
    double drift = 0.0;
    Random random;
    // Each output's last two samples, still open to corrections, and the
    // corrections this sample's edges make to them (back2, back1), to this
    // sample (now) and to the next (ahead); carry is the last sample's ahead.
    Array<double, 5> late2, late1, carry;
    Array<double, 5> back2, back1, now, ahead;

    // A step of h at d samples (0..1) before this sample.
    void step(int w, double h, double d) {
        const double e = 1.0 - d;
        back2[w] += h * d * d * d * d / 24.0;
        back1[w] += h * stepNear(d - 1.0);
        now[w] += h * stepNear(d);
        ahead[w] -= h * e * e * e * e / 24.0;
    }
    // A change of slope (per sample) of k there.
    void kink(int w, double k, double d) {
        const double e = 1.0 - d;
        back2[w] += k * d * d * d * d * d / 120.0;
        back1[w] += k * rampNear(e);
        now[w] += k * rampNear(d);
        ahead[w] += k * e * e * e * e * e / 120.0;
    }

    // The phase over [t0, t1] of the sample at rate inc, and its edges.
    void run(double inc, double pw, double t0, double t1) {
        const double p = phase;
        const double q = p + inc * (t1 - t0);
        const double dt = abs(inc);
        const double s = inc > 0.0 ? 1.0 : -1.0;
        const double wrap = crossing(p, q, 0.0, inc, t0);
        if (wrap >= 0.0) {
            const double d = 1.0 - wrap;
            step(kSaw, -2.0 * s, d);
            step(kPulse, 2.0 * s, d);
            kink(kTri, 8.0 * dt, d);
            step(kSub, -2.0 * subAt(flip), d);
            flip = 1 - flip;
        }
        const double fall = crossing(p, q, pw, inc, t0);
        if (fall >= 0.0) step(kPulse, -2.0 * s, 1.0 - fall);
        const double top = crossing(p, q, 0.5, inc, t0);
        if (top >= 0.0) kink(kTri, -8.0 * dt, 1.0 - top);
        phase = q - floor(q);
        if (phase >= 1.0) phase = 0.0;
    }

    // A hard sync at time a of the sample: back to the cycle's start.
    void reset(double inc, double pw, double a) {
        const double d = 1.0 - a;
        const double p = phase;
        step(kSine, -sineAt(p), d);
        kink(kSine, twoPi * inc * (1.0 - cos(twoPi * p)), d);
        step(kTri, -1.0 - triAt(p), d);
        kink(kTri, inc * (4.0 - (p < 0.5 ? 4.0 : -4.0)), d);
        step(kSaw, -1.0 - sawAt(p), d);
        step(kPulse, 1.0 - pulseAt(p, pw), d);
        step(kSub, 1.0 - subAt(flip), d);
        phase = 0.0;
        flip = 0;
    }

    // One sample: the outputs, two samples late, into y.
    void tick(double inc, double pw, double sync, Array<double, 5>& y) {
        back2.fill(0.0);
        back1.fill(0.0);
        now.fill(0.0);
        ahead.fill(0.0);
        double at = -1.0;  // where in the sample a sync fell
        if (sync <= 0.0) armed = true;
        if (armed && sync > 0.0) {
            at = clamp(-lastSync / (sync - lastSync), 0.0, 1.0);
            armed = false;
        }
        lastSync = sync;
        if (at >= 0.0) {
            run(inc, pw, 0.0, at);
            reset(inc, pw, at);
            run(inc, pw, at, 1.0);
        } else {
            run(inc, pw, 0.0, 1.0);
        }
        now[kSine] += sineAt(phase);
        now[kTri] += triAt(phase);
        now[kSaw] += sawAt(phase);
        now[kPulse] += pulseAt(phase, pw);
        now[kSub] += subAt(flip);
        for (int w = 0; w < 5; ++w) {
            y[w] = late2[w] + back2[w];
            late2[w] = late1[w] + back1[w];
            late1[w] = now[w] + carry[w];
            carry[w] = ahead[w];
        }
    }
};

struct Vco : Module {
    static constexpr ModuleInfo info = {
        .name = "VCO",
        .id = "com.suppleaudio.rack.vco",
        .vendor = "Supple Audio",
        .description = "An analog-style oscillator: sine, triangle, saw, pulse and sub at once.",
        .tags = {"oscillator", "poly"}};

    Param coarse = semitones("Coarse", -48, 48, 0).help("Transposes the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).help("Detunes the pitch in cents.");
    Param pulseWidth =
        percent("Pulse Width", 50, 2, 98)
            .help("The pulse's high share of each cycle; 50 is square, a cable into it is PWM.");
    Param fmDepth = octaves("FM Depth", -4, 4, 1)
                        .help("How far the Exp FM input bends the pitch, in octaves at a CV of 1.");
    Param fmIndex =
        percent("FM Index", 100, 0, 1000)
            .help(
                "The Lin FM input's index: the frequency swings by this share of itself at 1, "
                "through zero past 100.");
    Param drift =
        percent("Drift", 0)
            .help("A slow random wander of each voice's pitch, up to 10 cents: analog life.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: one voice per channel.");
    Input expFm = cvIn("Exp FM").help(
        "Bends the pitch exponentially by FM Depth's octaves: vibrato, sweeps.");
    Input linFm = audioIn("Lin FM").help(
        "Moves the frequency linearly and through zero: patch another VCO for FM tones.");
    Input sync = audioIn("Sync").help(
        "Restarts the cycle each time it rises above 0: a gate, or a VCO for hard sync.");

    Output sine = audioOut("Sine").help("The sine wave.");
    Output triangle = audioOut("Triangle").help("The triangle wave: soft, odd harmonics.");
    Output saw = audioOut("Saw").help("The rising saw: bright, every harmonic.");
    Output pulse = audioOut("Pulse").help("The pulse wave at the Pulse Width.");
    Output sub = audioOut("Sub").help("A square an octave below.");

    Array<Voice, 16> voice;
    Array<double, 5> y;

    void prepare() {
        for (int c = 0; c < 16; ++c) voice[c].random.seed(seed() + 7919 * c);
    }

    void process(int frames) {
        const double sr = sampleRate();
        // The drift: a slow random walk per voice, once a block (~0.5 s memory).
        const double k = frames / (0.5 * sr);
        const double spread = sqrt(2.0 * k);
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voice[c];
            const double kick = v.random.gaussian();
            v.drift += -v.drift * k + spread * kick;
            const double wander =
                clamp(v.drift, -3.0, 3.0) * drift.at(0, c) * (10.0 / 1200.0 / 3.0);
            for (int i = 0; i < frames; ++i) {
                const double oct = pitch[c][i] + coarse.at(i, c) / 12.0 + fine.at(i, c) / 1200.0 +
                                   fmDepth.at(i, c) * expFm[c][i] + wander;
                const double hz =
                    kC4 * exp2(clamp(oct, -14.0, 7.0)) * (1.0 + fmIndex.at(i, c) * linFm[c][i]);
                const double inc = clamp(hz / sr, -0.45, 0.45);
                const double pw = clamp(pulseWidth.at(i, c), 0.02, 0.98);
                v.tick(inc, pw, sync[c][i], y);
                sine[c][i] = y[kSine];
                triangle[c][i] = y[kTri];
                saw[c][i] = y[kSaw];
                pulse[c][i] = y[kPulse];
                sub[c][i] = y[kSub];
            }
        }
    }
};
