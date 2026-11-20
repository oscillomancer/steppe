// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Hornbeam: a guitar amplifier and the cabinet it plays into, every part a
// model of the physics, with no recordings of either.
//
// Hornbeam is a hardwood hard enough to be called ironwood, long turned into
// piano actions and mallets. Each sample takes this road:
//
//   Preamp     two to four triode stages by the voice (clean, chime, crunch,
//              lead, bass): the coupling capacitors' low cuts between them,
//              the cathodes' bypass shelves, each valve's own curve (Child's
//              law, cut off on one side and held by grid current on the
//              other) and the charge grid current leaves on its coupling
//              capacitor; the gain pot with its bright capacitor.
//   Tone       the passive three-knob network, solved from its circuit, so
//              bass, mid and treble lean on each other as real ones do.
//   Power      presence, a push-pull pair whose supply sags under the
//              current it draws, and an output transformer whose core
//              saturates the bass first. Master drives it; its ceiling is
//              fixed, so a cranked amp grows denser, not louder.
//   Speaker    a motor and a cone of masses on springs: the coil, the
//              cap and three rings of paper out to the surround. Driven
//              through the amp's output impedance, it rings at its
//              resonance in the box and breaks up into its modes above a
//              kilohertz; at high power its suspension stiffens and its
//              motor weakens with the excursion.
//   Microphone each ring reaches the mic by its own paths, near and far
//              across the cone (which is a cone: the cap sits centimetres
//              behind the rim), with the mic's pattern and the proximity
//              its gradient hears; the other speakers, the box's edges or
//              open back, the floor and the wall behind the mic arrive
//              later. Then the capsule's own response.
//
// The preamp, tone and power amp run oversampled; the cabinet, linear but
// for its speaker's excursion, at the session's rate. Its response as drawn
// is computed from the same model, filter for filter.

constexpr double kC = 343.0;                    // the speed of sound, m/s
constexpr double kRho = 1.2;                    // air, kg/m3
constexpr double kRef = 0.12589254117941673;    // -18 dBFS RMS
constexpr double kIn = 1.3333333333333333;      // 0 dBFS is 2 V at the jack; a grid's bias 1.5 V
constexpr double kGrid = 0.8;                   // where grid current starts, in bias units
constexpr double kCut = 1.7;                    // where the valve cuts off
constexpr double kCutPow = 2.2165264084143254;  // kCut^1.5
constexpr double kSlope = 0.5113031862366004;   // 1 / (1.5 sqrt(kCut))
constexpr double kPotOhms = 1.0e6;              // the gain pot
constexpr double kCabVolts = 5.0;               // cab only: 0 dBFS is 5 V, about 3 W into 8 ohms
constexpr double kOutFull = 0.3;                // the amp's ceiling at the output
constexpr double kOutAmp = 0.48;                // and alone, its load's voltage (as loud)
constexpr double kPaRef = 0.1;      // the preamp's RMS into the power amp for a -18 dBFS RMS input
constexpr double kCapsule = 0.008;  // a capsule's radius, m
constexpr double kShiftMax = 1.5;   // the most a stage's bias shifts, in bias units
constexpr int kStagesMax = 5;
constexpr int kModes = 5;  // the coil, the apex (cap and inner cone), two rings, the rim
constexpr int kRings = 4;  // the four that radiate
constexpr int kBins = 3;   // taps per ring
constexpr int kFarTaps = 16;
constexpr int kSources = 8;
constexpr int kTable = 65;  // the preamp's level, gain 0..100% in 64 steps
constexpr int kNodes = 48;  // points of the Gaussian it is measured over
constexpr int kNormPoints = 16;
constexpr int kLine = 8192;  // each ring's history, samples (a power of two)

// --- complex numbers, for responses -------------------------------------------------------

struct Cx {
    double re = 0.0;
    double im = 0.0;
    void set(double r, double i) {
        re = r;
        im = i;
    }
    void add(const Cx& b) {
        re += b.re;
        im += b.im;
    }
    void addScaled(const Cx& b, double k) {
        re += b.re * k;
        im += b.im * k;
    }
    void mul(const Cx& b) {
        const double r = re * b.re - im * b.im;
        im = re * b.im + im * b.re;
        re = r;
    }
    void scale(double k) {
        re *= k;
        im *= k;
    }
    void div(const Cx& b) {
        const double d = b.re * b.re + b.im * b.im;
        if (d < 1e-300) {
            re = 0.0;
            im = 0.0;
            return;
        }
        const double r = (re * b.re + im * b.im) / d;
        im = (im * b.re - re * b.im) / d;
        re = r;
    }
    void expj(double ph) {
        re = cos(ph);
        im = sin(ph);
    }
    double norm() const { return re * re + im * im; }
};

// Gains often hold still for many frames. Cache only exact repeats, so a
// parameter glide still gets every distinct gain it asks for.
struct GainCache {
    double at = 1e100, value = 1.0;
    double get(double db) {
        if (db != at) {
            at = db;
            value = dbToGain(db);
        }
        return value;
    }
};

// --- filters whose responses the editor can draw exactly ----------------------------------

// A first-order section, the bilinear transform of (n0 + n1 s) / (d0 + d1 s)
// with s = k (1 - 1/z) / (1 + 1/z); k is twice the rate, or prewarped.
struct Fo {
    double b0 = 1.0, b1 = 0.0, a1 = 0.0, z = 0.0;
    void analog(double n0, double n1, double d0, double d1, double k) {
        const double a0 = d0 + d1 * k;
        b0 = (n0 + n1 * k) / a0;
        b1 = (n0 - n1 * k) / a0;
        a1 = (d0 - d1 * k) / a0;
    }
    double process(double x) {
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }
    void clear() { z = 0.0; }
    // At w = 2 pi f / rate.
    void response(double w, Cx& out) const {
        const double c = cos(w), s = sin(w);
        Cx d;
        out.set(b0 + b1 * c, -b1 * s);
        d.set(1.0 + a1 * c, -a1 * s);
        out.div(d);
    }
};

// k for a corner at hz, so the corner lands where it should after the warp.
double warpK(double hz, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate);
    return w / tan(0.5 * w / rate);
}

void lowpass1(Fo& f, double hz, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate);
    f.analog(w, 0.0, w, 1.0, warpK(hz, rate));
}
void highpass1(Fo& f, double hz, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate);
    f.analog(0.0, 1.0, w, 1.0, warpK(hz, rate));
}
// Lows down by db below hz (db <= 0), highs untouched: (s + wz) / (s + wp).
void lowShelf1(Fo& f, double hz, double db, double rate) {
    const double wp = twoPi * min(hz, 0.45 * rate);
    f.analog(wp * dbToGain(db), 1.0, wp, 1.0, warpK(hz, rate));
}
// Highs up by db above hz: (s G + w) / (s + w), G = the gain at the top.
void highShelf1(Fo& f, double hz, double db, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate);
    f.analog(w, dbToGain(db), w, 1.0, warpK(hz, rate));
}

// A second-order section (the cookbook's designs), with its response.
struct Bq {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
    void set(double n0, double n1, double n2, double d0, double d1, double d2) {
        b0 = n0 / d0;
        b1 = n1 / d0;
        b2 = n2 / d0;
        a1 = d1 / d0;
        a2 = d2 / d0;
    }
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() {
        z1 = 0.0;
        z2 = 0.0;
    }
    void response(double w, Cx& out) const {
        const double c1 = cos(w), s1 = sin(w), c2 = cos(2.0 * w), s2 = sin(2.0 * w);
        Cx d;
        out.set(b0 + b1 * c1 + b2 * c2, -b1 * s1 - b2 * s2);
        d.set(1.0 + a1 * c1 + a2 * c2, -a1 * s1 - a2 * s2);
        out.div(d);
    }
};

void bqLowpass(Bq& f, double hz, double q, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate) / rate, c = cos(w), al = sin(w) / (2.0 * q);
    f.set(0.5 * (1.0 - c), 1.0 - c, 0.5 * (1.0 - c), 1.0 + al, -2.0 * c, 1.0 - al);
}
void bqHighpass(Bq& f, double hz, double q, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate) / rate, c = cos(w), al = sin(w) / (2.0 * q);
    f.set(0.5 * (1.0 + c), -(1.0 + c), 0.5 * (1.0 + c), 1.0 + al, -2.0 * c, 1.0 - al);
}
void bqPeak(Bq& f, double hz, double q, double db, double rate) {
    const double w = twoPi * min(hz, 0.45 * rate) / rate, c = cos(w), al = sin(w) / (2.0 * q);
    const double a = pow(10.0, db / 40.0);
    f.set(1.0 + al * a, -2.0 * c, 1.0 - al * a, 1.0 + al / a, -2.0 * c, 1.0 - al / a);
}

// --- the amplifier --------------------------------------------------------------------------

// Pots: an audio taper reads 10% at half travel.
double audioTaper(double x) { return (pow(81.0, clamp(x, 0.0, 1.0)) - 1.0) / 80.0; }

// A triode's grid against its plate current, in units of its bias: Child's
// law, (v + cut)^1.5, cut off below -cut; above kGrid grid current holds
// the grid back. Normalized to a slope of 1 at rest; the plate's swing is
// its negative (each stage inverts).
double triode(double u) {
    double v = u;
    if (u > kGrid) {
        const double e = u - kGrid;
        v = kGrid + e / (1.0 + 3.0 * e);
    }
    const double w = v + kCut;
    if (w <= 0.0) return -kCutPow * kSlope;
    return (w * sqrt(w) - kCutPow) * kSlope;
}

// A cathode follower: unity until grid current at the top, cut off far below.
double follower(double u) {
    if (u > 1.0) {
        const double e = u - 1.0;
        return 1.0 + e / (1.0 + 2.0 * e);
    }
    if (u < -2.5) {
        const double e = -2.5 - u;
        return -2.5 - e / (1.0 + e);
    }
    return u;
}

// The push-pull pair: u / (1 + 2(1-h) u^2 + u^4)^(1/4), soft (h = 0, the
// same as u / sqrt(1 + u^2)) to hard (h = 1, a sharper knee at the same
// ceiling).
double pushPull(double u, double hard) {
    const double u2 = u * u;
    return u / sqrt(sqrt(1.0 + 2.0 * (1.0 - hard) * u2 + u2 * u2));
}

// The output transformer's core: never quite flat.
double core(double v) { return 0.4 * v + 0.6 * v / sqrt(1.0 + v * v); }

// A voice: its stages, the pot, the tone stack's parts and the power amp.
struct Voice {
    int stages = 2;
    Array<int, 5> kind;  // 0 a triode stage, 1 a cathode follower
    Array<double, 5> gainDb, hpHz, shelfHz, shelfDb, lpHz, bias, charge;
    int potAfter = 0, stackAfter = 0;
    double brightPf = 0.0;
    double r1 = 250e3, r2 = 1e6, r3 = 25e3, r4 = 56e3, c1 = 250e-12, c2 = 20e-9, c3 = 20e-9;
    double rs = 38e3, rl = 1e6;
    double hard = 0.5, asym = 0.04, sagMs = 80.0, sagDepth = 0.3, otHz = 30.0, otDrive = 0.5;
    double zo = 4.0, watts = 50.0, presenceHz = 3500.0, presenceDb = 8.0;
    // What the level table misses (the filters between the stages, the
    // grid's charge), fitted on a guitar's DI: dB at gain 0, 25 .. 100%.
    Array<double, 5> trim;
};

void stage(Voice& v, int k, int kind, double gainDb, double hpHz, double shelfHz, double shelfDb,
           double lpHz, double bias, double charge) {
    v.kind[k] = kind;
    v.gainDb[k] = gainDb;
    v.hpHz[k] = hpHz;
    v.shelfHz[k] = shelfHz;
    v.shelfDb[k] = shelfDb;
    v.lpHz[k] = lpHz;
    v.bias[k] = bias;
    v.charge[k] = charge;
}

void stack(Voice& v, double r1, double r2, double r3, double r4, double c1, double c2, double c3,
           double rs) {
    v.r1 = r1;
    v.r2 = r2;
    v.r3 = r3;
    v.r4 = r4;
    v.c1 = c1;
    v.c2 = c2;
    v.c3 = c3;
    v.rs = rs;
}

void power(Voice& v, double hard, double asym, double sagMs, double sagDepth, double otHz,
           double otDrive, double zo, double watts, double presenceHz) {
    v.hard = hard;
    v.asym = asym;
    v.sagMs = sagMs;
    v.sagDepth = sagDepth;
    v.otHz = otHz;
    v.otDrive = otDrive;
    v.zo = zo;
    v.watts = watts;
    v.presenceHz = presenceHz;
}

void loadVoice(int which, Voice& v) {
    // 2.5 dB into the first stage: 2 V at the jack against a 1.5 V bias.
    switch (which) {
        case 0:  // clean: a stage, the tone stack on its plate, the volume, a second stage
            v.stages = 2;
            stage(v, 0, 0, 2.5, 20, 10, 0, 22000, 0, 0.3);
            stage(v, 1, 0, 36.0, 12, 10, 0, 14000, 0, 0.3);
            v.stackAfter = 0;
            v.potAfter = 0;
            v.brightPf = 120.0;
            stack(v, 250e3, 250e3, 10e3, 100e3, 250e-12, 100e-9, 47e-9, 38e3);
            power(v, 0.8, 0.03, 60, 0.22, 25, 0.35, 3.0, 40, 3500);
            v.trim = {-2.0, -2.0, -1.2, 2.4, 7.0};
            break;
        case 1:  // chime: two stages, then the tone stack; class A, no feedback
            v.stages = 2;
            stage(v, 0, 0, 2.5, 20, 10, 0, 18000, 0, 0.5);
            stage(v, 1, 0, 34.0, 30, 220, -4, 12000, 0, 0.5);
            v.potAfter = 0;
            v.stackAfter = 1;
            v.brightPf = 0.0;
            stack(v, 250e3, 1e6, 25e3, 56e3, 470e-12, 22e-9, 47e-9, 38e3);
            power(v, 0.0, 0.08, 120, 0.5, 40, 0.8, 10.0, 30, 3000);
            v.trim = {11.1, 11.1, 10.9, 10.3, 10.0};
            break;
        case 2:  // crunch: two stages and a follower into the tone stack
            v.stages = 3;
            stage(v, 0, 0, 2.5, 20, 10, 0, 18000, 0, 0.5);
            stage(v, 1, 0, 28.0, 25, 250, -5, 10000, 0, 0.6);
            stage(v, 2, 1, 0.0, 10, 10, 0, 20000, 0, 0);
            v.potAfter = 0;
            v.stackAfter = 2;
            v.brightPf = 470.0;
            stack(v, 220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9, 1.5e3);
            power(v, 0.4, 0.05, 90, 0.35, 30, 0.6, 5.0, 50, 3500);
            v.trim = {-4.7, -4.7, 3.1, 8.9, 10.7};
            break;
        case 3:  // lead: four tight stages (one cold), a follower, the tone stack
            v.stages = 5;
            stage(v, 0, 0, 2.5, 30, 400, -6, 16000, 0, 0.5);
            stage(v, 1, 0, 26.0, 120, 300, -3, 12000, -0.6, 0.8);
            stage(v, 2, 0, 24.0, 80, 10, 0, 9000, 0, 0.7);
            stage(v, 3, 0, 16.0, 40, 10, 0, 8000, 0, 0.6);
            stage(v, 4, 1, 0.0, 10, 10, 0, 20000, 0, 0);
            v.potAfter = 0;
            v.stackAfter = 4;
            v.brightPf = 120.0;
            stack(v, 250e3, 1e6, 25e3, 47e3, 250e-12, 22e-9, 22e-9, 1.5e3);
            power(v, 0.7, 0.04, 70, 0.3, 30, 0.5, 4.0, 100, 4000);
            v.trim = {9.9, 9.9, 10.3, 10.3, 10.5};
            break;
        default:  // bass: as clean, with the stack's lows and big iron
            v.stages = 2;
            stage(v, 0, 0, 2.5, 8, 10, 0, 22000, 0, 0.3);
            stage(v, 1, 0, 34.0, 8, 10, 0, 15000, 0, 0.3);
            v.stackAfter = 0;
            v.potAfter = 0;
            v.brightPf = 0.0;
            stack(v, 250e3, 1e6, 25e3, 56e3, 250e-12, 47e-9, 47e-9, 38e3);
            power(v, 0.9, 0.02, 50, 0.2, 20, 0.5, 1.5, 300, 3000);
            v.trim = {7.3, 7.3, 7.3, 7.3, 7.0};
            break;
    }
}

// The tone stack's analog coefficients, from its circuit: the treble pot's
// wiper is the output, its bottom meets the bass cap and the bass pot, which
// runs to the mid pot and the mid cap; the slope resistor feeds both caps.
// t, l, m are the pots' resistances as shares (treble, bass, mid); the source
// and the next grid's leak load it. out: b1 b2 b3 (b0 is 0), a0 a1 a2 a3.
void stackAnalog(const Voice& v, double t, double l, double m, Array<double, 7>& out) {
    const double R1 = v.r1, R2 = v.r2, R3 = v.r3, R4 = v.r4, C1 = v.c1, C2 = v.c2, C3 = v.c3;
    const double Rs = v.rs, RL = v.rl;
    const double k0 = R1 * t, k1 = C1 * k0, k2 = R2 * l, k3 = C1 * k2, k4 = R3 * m, k5 = C1 * k4;
    const double k6 = C2 * k2, k7 = C2 * k4, k8 = C3 * k4, k9 = R1 * k3, k10 = R1 * k5;
    const double k11 = C2 * R4, k12 = C3 * k10, k13 = C3 * R4, k14 = k13 * k3, k15 = C2 * RL;
    const double k16 = C1 * RL, k17 = Rs * k16, k18 = R4 * k15, k19 = Rs * k15, k20 = C3 * RL;
    const double k21 = t * t, k22 = C1 * R1 * R1, k23 = C1 * R3, k24 = R1 * k23, k25 = RL * k23;
    const double k26 = Rs * k23, k27 = C2 * R3, k28 = C3 * R3, k29 = C1 * R2, k30 = R1 * k29;
    const double k31 = RL * k29, k32 = Rs * k29, k33 = C2 * R2, k34 = R4 * k33, k35 = RL * k33;
    const double k36 = Rs * k33, k37 = C3 * R2, k38 = R4 * k37, k39 = Rs * k37, k40 = R1 * Rs;
    const double k41 = C1 * k40, k42 = R1 * R4, k43 = C2 * k42, k44 = C2 * k40, k45 = C3 * k42;
    const double k46 = C3 * k40, k47 = R1 * k6, k48 = R1 * k8, k49 = C3 * Rs, k50 = C2 * k22;
    const double k51 = C3 * k22, k52 = R4 * k51, k53 = Rs * k51, k54 = k22 * k6;
    const double k55 = R4 * k50 + Rs * k50 + k22 * k7 + k22 * k8 + k52 + k53 + k54;
    const double k56 = Rs * k30, k57 = k11 * k31, k58 = k20 * k34, k59 = RL * k37;
    const double k60 = k3 * k45, k61 = k52 * k6 + k53 * k6 + k54 * k8, k62 = C3 * k30;
    out[0] = RL * (k1 + k3 + k5 + k6 + k7 + k8);
    out[1] = RL * (C2 * k10 + C2 * k9 + k1 * k11 + k1 * k13 + k11 * k3 + k11 * k5 + k12 + k13 * k5 +
                   k14 + k3 * k8 + k6 * k8);
    out[2] = C3 * k15 * k3 * (R1 * k4 + R4 * k0 + R4 * k4);
    out[3] = RL + k0 + k2 + k4;
    out[4] =
        R1 * k16 + R4 * k20 + Rs * k20 + k17 + k18 + k19 - k21 * k22 +
        l * (R2 * k8 + k30 + k31 + k32 + k34 + k35 + k36 + k38 + k39) +
        m * (R4 * k27 + R4 * k28 + RL * k27 + RL * k28 + Rs * k27 + Rs * k28 + k24 + k25 + k26) +
        t * (R1 * k7 - k10 + k22 + k41 + k43 + k44 + k45 + k46 + k47 + k48 - k9);
    out[5] = k11 * k17 + k13 * k17 + k15 * k41 + k16 * k43 + k16 * k45 + k20 * k41 - k21 * k55 +
             l * (C2 * k56 + C3 * k56 + k10 * k37 + k11 * k30 + k11 * k32 + k13 * k30 + k13 * k31 +
                  k13 * k32 + k15 * k30 + k31 * k49 + k35 * k49 + k38 * k7 + k39 * k5 + k39 * k7 +
                  k5 * k59 + k57 + k58 + k59 * k7) +
             m * (C2 * Rs * k24 + k11 * k24 + k11 * k25 + k11 * k26 + k13 * k24 + k13 * k25 +
                  k13 * k26 + k15 * k24 + k20 * k24 + k24 * k49) +
             t * (k11 * k41 + k13 * k41 - k3 * k43 - k3 * k46 - k3 * k48 - k43 * k5 - k45 * k5 +
                  k45 * k6 + k46 * k6 + k47 * k8 + k55 - k60);
    out[6] = -k21 * k61 +
             l * (k12 * k34 + k12 * k35 + k12 * k36 + k18 * k62 + k19 * k62 + k34 * k49 * k5 +
                  k49 * k57 + k5 * k58) +
             t * (k14 * k44 - k60 * k7 + k61);
}

// The knobs' travel to the pots' shares: treble and bass audio taper, mid linear.
double trebleShare(double x) { return audioTaper(x); }
double bassShare(double x) { return audioTaper(x); }
double midShare(double x) { return clamp(x, 0.0, 1.0); }

// The tone stack's analog gain at hz.
double stackGain(const Array<double, 7>& a, double hz) {
    const double w = twoPi * hz;
    // s = jw: num = b1 s + b2 s^2 + b3 s^3, den = a0 + a1 s + a2 s^2 + a3 s^3
    Cx n, d;
    n.set(-a[1] * w * w, a[0] * w - a[2] * w * w * w);
    d.set(a[3] - a[5] * w * w, a[4] * w - a[6] * w * w * w);
    n.div(d);
    return sqrt(n.norm());
}

// The tone stack, third order, by the bilinear transform at k = 2 rate.
struct Stack3 {
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double s1 = 0.0, s2 = 0.0, s3 = 0.0;
    void design(const Array<double, 7>& a, double k) {
        const double B1 = a[0] * k, B2 = a[1] * k * k, B3 = a[2] * k * k * k;
        const double A0 = a[3], A1 = a[4] * k, A2 = a[5] * k * k, A3 = a[6] * k * k * k;
        const double d0 = A0 + A1 + A2 + A3;
        b0 = (B1 + B2 + B3) / d0;
        b1 = (B1 - B2 - 3.0 * B3) / d0;
        b2 = (-B1 - B2 + 3.0 * B3) / d0;
        b3 = (-B1 + B2 - B3) / d0;
        a1 = (3.0 * A0 + A1 - A2 - 3.0 * A3) / d0;
        a2 = (3.0 * A0 - A1 - A2 + 3.0 * A3) / d0;
        a3 = (A0 - A1 + A2 - A3) / d0;
    }
    double process(double x) {
        const double y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y + s3;
        s3 = b3 * x - a3 * y;
        return y;
    }
    void clear() {
        s1 = 0.0;
        s2 = 0.0;
        s3 = 0.0;
    }
    void response(double w, Cx& out) const {
        Cx d;
        out.set(b0 + b1 * cos(w) + b2 * cos(2.0 * w) + b3 * cos(3.0 * w),
                -b1 * sin(w) - b2 * sin(2.0 * w) - b3 * sin(3.0 * w));
        d.set(1.0 + a1 * cos(w) + a2 * cos(2.0 * w) + a3 * cos(3.0 * w),
              -a1 * sin(w) - a2 * sin(2.0 * w) - a3 * sin(3.0 * w));
        out.div(d);
    }
};

// One stage's state and filters.
struct Stage {
    Fo hp, shelf, lp;
    double gain = 1.0, rest = 0.0, q = 0.0, keep = 0.999, bias = 0.0, charge = 0.0;
    bool hpOn = true, shelfOn = true, isFollower = false;
    void clear() {
        hp.clear();
        shelf.clear();
        lp.clear();
        q = 0.0;
    }
};

// The amp's state for one channel, at the oversampled rate.
struct AmpLane {
    Array<Stage, 5> st;
    Fo inHp, inShelf;              // the first stage's, at the session's rate (before any curve)
    double flux = 0.0, err = 0.0;  // the output transformer's, at the session's rate
    Fo bright;
    Stack3 tone;
    Fo presence;
    double env = 0.0;
    Array<double, 6> cin, cout;  // each curve's input and output (the power amp's last)
    void clear() {
        for (int k = 0; k < kStagesMax; ++k) st[k].clear();
        bright.clear();
        tone.clear();
        presence.clear();
        inHp.clear();
        inShelf.clear();
        env = 0.0;
        flux = 0.0;
        err = 0.0;
    }
};

// --- the cabinet ---------------------------------------------------------------------------

// A speaker design at 12 inches: resonance, its mechanical and electrical
// Q, the coil's resistance and inductance (with the eddy currents' loss
// resistance across it), the cone's paper (kg/m2), the coil's mass, the
// links between the masses (the coil to the apex, then outward, Hz), each
// breakup mode's loss factor, the cone's depth, its rated power and the
// excursion its suspension stiffens over (mm).
struct SpeakerSpec {
    double fs = 75.0, qms = 5.0, qes = 0.8, re = 6.4, le = 0.6e-3, r2 = 10.0, sigma = 0.24;
    double coil = 0.005, depth = 0.046, watts = 25.0, xlim = 3.0;
    Array<double, 4> links, etas;
};

void loadSpeaker(int which, SpeakerSpec& s) {
    switch (which) {
        case 0:  // alnico: a light cone and a weak magnet; chime, early breakup
            s.fs = 75;
            s.qms = 4.0;
            s.qes = 1.3;
            s.re = 6.4;
            s.le = 0.45e-3;
            s.r2 = 9.0;
            s.sigma = 0.2;
            s.coil = 0.004;
            s.depth = 0.042;
            s.watts = 15;
            s.xlim = 2.5;
            s.links = {6000.0, 4800.0, 3000.0, 1600.0};
            s.etas = {0.34, 0.28, 0.26, 0.6};
            break;
        case 1:  // vintage: ceramic, 25 W; warm, smooth mids
            s.fs = 75;
            s.qms = 5.0;
            s.qes = 0.85;
            s.re = 6.4;
            s.le = 0.65e-3;
            s.r2 = 12.0;
            s.sigma = 0.24;
            s.coil = 0.005;
            s.depth = 0.046;
            s.watts = 25;
            s.xlim = 3.0;
            s.links = {4000.0, 3000.0, 1900.0, 1100.0};
            s.etas = {0.55, 0.45, 0.4, 0.9};
            break;
        case 2:  // modern: ceramic, 60 W; forward upper mids, tight lows
            s.fs = 85;
            s.qms = 6.0;
            s.qes = 0.6;
            s.re = 6.4;
            s.le = 0.7e-3;
            s.r2 = 14.0;
            s.sigma = 0.26;
            s.coil = 0.006;
            s.depth = 0.048;
            s.watts = 60;
            s.xlim = 3.5;
            s.links = {5200.0, 4600.0, 2800.0, 1700.0};
            s.etas = {0.32, 0.24, 0.2, 0.55};
            break;
        default:  // heavy: a stiff cone and a big magnet; flat and clean, 200 W
            s.fs = 62;
            s.qms = 7.0;
            s.qes = 0.38;
            s.re = 6.0;
            s.le = 0.9e-3;
            s.r2 = 10.0;
            s.sigma = 0.36;
            s.coil = 0.009;
            s.depth = 0.05;
            s.watts = 200;
            s.xlim = 5.0;
            s.links = {7000.0, 5600.0, 3600.0, 2200.0};
            s.etas = {0.6, 0.5, 0.45, 0.9};
            break;
    }
}

// A cabinet: its speakers' size and count (the miked one at 0, 0; the
// others relative to it, m), open or closed (the box's volume, litres),
// depth, width and the miked speaker's height above the floor.
struct CabSpec {
    double inches = 12.0, litres = 0.0, depth = 0.25, width = 0.56, height = 0.36;
    bool open = true;
    int count = 1;
    Array<double, 3> dx, dy;
};

void loadCab(int which, CabSpec& c) {
    c.dx = {0.0, 0.0, 0.0};
    c.dy = {0.0, 0.0, 0.0};
    switch (which) {
        case 0:  // 1x12, open back (a combo)
            c.inches = 12;
            c.count = 1;
            c.open = true;
            c.litres = 0;
            c.depth = 0.24;
            c.width = 0.56;
            c.height = 0.36;
            break;
        case 1:  // 2x12, open back
            c.inches = 12;
            c.count = 2;
            c.open = true;
            c.litres = 0;
            c.depth = 0.26;
            c.width = 0.7;
            c.height = 0.38;
            c.dx = {0.33, 0.0, 0.0};
            break;
        case 2:  // 2x12, closed back
            c.inches = 12;
            c.count = 2;
            c.open = false;
            c.litres = 55;
            c.depth = 0.28;
            c.width = 0.74;
            c.height = 0.36;
            c.dx = {0.33, 0.0, 0.0};
            break;
        case 3:  // 4x12, closed back: the top left speaker is miked
            c.inches = 12;
            c.count = 4;
            c.open = false;
            c.litres = 110;
            c.depth = 0.36;
            c.width = 0.76;
            c.height = 0.56;
            c.dx = {0.33, 0.0, 0.33};
            c.dy = {0.0, -0.33, -0.33};
            break;
        case 4:  // 4x10, closed back
            c.inches = 10;
            c.count = 4;
            c.open = false;
            c.litres = 80;
            c.depth = 0.33;
            c.width = 0.6;
            c.height = 0.52;
            c.dx = {0.28, 0.0, 0.28};
            c.dy = {0.0, -0.28, -0.28};
            break;
        default:  // 1x15, closed back
            c.inches = 15;
            c.count = 1;
            c.open = false;
            c.litres = 110;
            c.depth = 0.42;
            c.width = 0.6;
            c.height = 0.4;
            break;
    }
}

// A microphone: how much it hears pressure (1 omni, 0.5 cardioid, 0 figure
// eight), and its capsule's response.
struct MicSpec {
    double alpha = 0.5, hpHz = 100.0, hpQ = 0.6, lpHz = 12000.0, lpQ = 0.9;
    double p1Hz = 5000.0, p1Q = 1.0, p1Db = 0.0, p2Hz = 2000.0, p2Q = 1.0, p2Db = 0.0;
};

void loadMic(int which, MicSpec& m) {
    switch (which) {
        case 0:  // dynamic: cardioid, a presence rise, a top that falls past 10 kHz
            m.alpha = 0.5;
            m.hpHz = 115;
            m.hpQ = 0.6;
            m.p1Hz = 5200;
            m.p1Q = 1.4;
            m.p1Db = 5.0;
            m.p2Hz = 2000;
            m.p2Q = 1.0;
            m.p2Db = -1.0;
            m.lpHz = 12500;
            m.lpQ = 0.9;
            break;
        case 1:  // condenser: cardioid, flat and open, a little air
            m.alpha = 0.5;
            m.hpHz = 55;
            m.hpQ = 0.6;
            m.p1Hz = 11000;
            m.p1Q = 1.0;
            m.p1Db = 2.5;
            m.p2Hz = 2000;
            m.p2Q = 1.0;
            m.p2Db = 0.0;
            m.lpHz = 20000;
            m.lpQ = 0.7;
            break;
        default:  // ribbon: figure eight, a soft top
            m.alpha = 0.0;
            m.hpHz = 20;
            m.hpQ = 0.7;
            m.p1Hz = 200;
            m.p1Q = 0.8;
            m.p2Db = 0.0;
            m.p1Db = 1.0;
            m.p2Hz = 2000;
            m.p2Q = 1.0;
            m.lpHz = 7500;
            m.lpQ = 0.55;
            break;
    }
}

// The speaker as built for a cabinet: the cone's rings, their masses and
// the links between them, solved into modes.
struct Model {
    double radius = 0.127, depth = 0.046, cap = 0.025;
    Array<double, 6> edges;    // ring edges from the centre: cap, apex, two rings, rim
    Array<double, 4> areas;    // what each radiating mass covers, m2
    Array<double, 5> w, zeta;  // the modes: rad/s and damping
    Array<double, 25> phi;     // mass-normalized shapes: phi[i * 5 + m], mass i in mode m
    double bl = 10.0, re = 6.4, le = 0.6e-3, r2 = 10.0, zo = 0.0;
    double mc = 0.02, ma = 0.01, rr = 20.0;  // the cone's mass, the air load's, its resistance
    double ksus = 4000.0, xlim = 0.003;
    double volts = 20.0;  // the drive at full scale
    double sd = 0.05;
};

// The symmetric 5x5 eigenproblem, by Jacobi rotations: a in, its
// eigenvalues on the diagonal and eigenvectors in v's columns out.
void jacobi(Array<double, 25>& a, Array<double, 25>& v) {
    for (int i = 0; i < 25; ++i) v[i] = 0.0;
    for (int i = 0; i < 5; ++i) v[i * 5 + i] = 1.0;
    for (int sweep = 0; sweep < 30; ++sweep) {
        double off = 0.0, diag = 0.0;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) {
                if (i != j)
                    off += a[i * 5 + j] * a[i * 5 + j];
                else
                    diag += a[i * 5 + j] * a[i * 5 + j];
            }
        if (off <= 1e-24 * diag) break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 5; ++q) {
                const double apq = a[p * 5 + q];
                if (abs(apq) < 1e-300) continue;
                const double theta = (a[q * 5 + q] - a[p * 5 + p]) / (2.0 * apq);
                const double t = sign(theta) / (abs(theta) + sqrt(theta * theta + 1.0)) +
                                 (theta == 0.0 ? 1.0 : 0.0);
                const double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 5; ++k) {
                    const double akp = a[k * 5 + p], akq = a[k * 5 + q];
                    a[k * 5 + p] = c * akp - s * akq;
                    a[k * 5 + q] = s * akp + c * akq;
                }
                for (int k = 0; k < 5; ++k) {
                    const double apk = a[p * 5 + k], aqk = a[q * 5 + k];
                    a[p * 5 + k] = c * apk - s * aqk;
                    a[q * 5 + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 5; ++k) {
                    const double vkp = v[k * 5 + p], vkq = v[k * 5 + q];
                    v[k * 5 + p] = c * vkp - s * vkq;
                    v[k * 5 + q] = s * vkp + c * vkq;
                }
            }
    }
}

// Builds the speaker for a cabinet, driven through zo (the amp's output
// impedance; 0 from a voltage source) at volts for full scale.
void buildModel(int speaker, int cabinet, double zo, double volts, Model& md) {
    SpeakerSpec sp;
    loadSpeaker(speaker, sp);
    CabSpec cb;
    loadCab(cabinet, cb);
    const double size = cb.inches / 12.0;
    md.radius = 0.127 * size;
    md.depth = sp.depth * size;
    // The rings: the cap to 0.2 R, the apex to 0.38, rings to 0.62, 0.84, the rim to R.
    md.edges = {0.0,      0.2 * md.radius, 0.38 * md.radius, 0.62 * md.radius, 0.84 * md.radius,
                md.radius};
    md.cap = md.edges[1];
    md.areas[0] = pi * md.edges[2] * md.edges[2];
    for (int k = 1; k < 4; ++k)
        md.areas[k] = pi * (md.edges[k + 2] * md.edges[k + 2] - md.edges[k + 1] * md.edges[k + 1]);
    md.sd = md.areas[0] + md.areas[1] + md.areas[2] + md.areas[3];
    // The air load (both sides of the cone, spread by area): it falls away
    // above ka ~ 1, which the shelf on the force undoes (Model.ma, rr).
    const double air = 2.0 * 8.0 / 3.0 * kRho * md.radius * md.radius * md.radius;
    Array<double, 5> m;
    m[0] = sp.coil * pow(size, 1.5);
    for (int k = 0; k < 4; ++k) m[k + 1] = sp.sigma * md.areas[k] + air * md.areas[k] / md.sd;
    double mms = 0.0;
    for (int k = 0; k < 5; ++k) mms += m[k];
    md.mc = mms - air;
    md.ma = air;
    md.rr = kRho * kC * md.sd;
    const double fs = sp.fs * pow(size, -0.6);
    const double ws = twoPi * fs;
    Array<double, 25> kk;
    for (int i = 0; i < 25; ++i) kk[i] = 0.0;
    for (int j = 0; j < 4; ++j) {
        const double wl = twoPi * sp.links[j] / size;
        const double mu = m[j] * m[j + 1] / (m[j] + m[j + 1]);
        const double k = mu * wl * wl;
        kk[j * 5 + j] += k;
        kk[(j + 1) * 5 + j + 1] += k;
        kk[j * 5 + j + 1] -= k;
        kk[(j + 1) * 5 + j] -= k;
    }
    md.ksus = ws * ws * mms;
    kk[0] += 0.6 * md.ksus;   // the spider, at the coil
    kk[24] += 0.4 * md.ksus;  // the surround, at the rim
    if (!cb.open && cb.litres > 0.0) {
        // The box's air, shared by its speakers, on every ring by its area
        // (the volume doubled: lining and leaks).
        const double kb = kRho * kC * kC / (2.0 * cb.litres * 0.001 / cb.count);
        for (int i = 1; i < 5; ++i)
            for (int j = 1; j < 5; ++j) kk[i * 5 + j] += kb * md.areas[i - 1] * md.areas[j - 1];
    }
    Array<double, 25> a, vec;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) a[i * 5 + j] = kk[i * 5 + j] / sqrt(m[i] * m[j]);
    jacobi(a, vec);
    // Sort the modes by frequency, the shapes mass-normalized.
    Array<int, 5> order = {0, 1, 2, 3, 4};
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j)
            if (a[order[j] * 5 + order[j]] < a[order[i] * 5 + order[i]]) {
                const int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
    for (int mm = 0; mm < 5; ++mm) {
        const int o = order[mm];
        md.w[mm] = sqrt(max(a[o * 5 + o], 1.0));
        // A shape's sign is free: make the coil's part positive.
        const double sgn = vec[o] < 0.0 ? -1.0 : 1.0;
        for (int i = 0; i < 5; ++i) md.phi[i * 5 + mm] = sgn * vec[i * 5 + o] / sqrt(m[i]);
    }
    md.zeta[0] = 0.5 / sp.qms;
    for (int k = 1; k < 5; ++k) md.zeta[k] = 0.5 * sp.etas[k - 1];
    md.re = sp.re;
    md.le = sp.le * size;
    md.r2 = sp.r2;
    md.zo = zo;
    md.bl = sqrt(ws * mms * sp.re / sp.qes);
    md.volts = volts / sqrt(cb.count);
    // The excursion the suspension stiffens over grows with the cone.
    md.xlim = sp.xlim * 0.001 * size;
}

// The paths from the cone and the cabinet to the mic, as taps.
struct Taps {
    Array<double, 16> delay, wp, wg;  // ring k, bin b at k * kBins + b: samples, pressure, gradient
    Array<int, 16> whole, farWhole;   // the delays' whole samples
    Array<double, 16> part, farPart;  // and the fractions past them
    int farCount = 0;
    Array<double, 16> farDelay, farW;
    Array<int, 16> farSrc;
    int sources = 0;
    Array<double, 8> srcP, srcG, srcHz;
    double norm = 1.0;  // to unity over the guitar's band
    double alpha = 0.5;
};

// One far source: its taps' paths (m), weights and arrival (cos of the
// angle off the mic's axis), heard through a low pass at hz.
void farSource(Taps& t, double path, double spread, double weight, double cosang, double hz,
               double alpha, double rate) {
    if (t.sources >= kSources || t.farCount + 2 > kFarTaps) return;
    const int s = t.sources;
    t.sources += 1;
    t.srcP[s] = weight / path * (alpha + (1.0 - alpha) * cosang);
    t.srcG[s] = weight / path * (1.0 - alpha) * cosang * kC / path;
    t.srcHz[s] = hz;
    if (spread > 0.0) {
        // Its near and far halves, half each.
        for (int k = 0; k < 2; ++k) {
            const int f = t.farCount;
            t.farCount += 1;
            t.farDelay[f] = (path + (k == 0 ? -spread : spread)) / kC * rate;
            t.farW[f] = 0.5;
            t.farSrc[f] = s;
        }
    } else {
        const int f = t.farCount;
        t.farCount += 1;
        t.farDelay[f] = path / kC * rate;
        t.farW[f] = 1.0;
        t.farSrc[f] = s;
    }
}

// pos: the mic's offset from the cap's centre as a share of the radius;
// dist: its distance from the front of the cone (the plane of the rim), m.
void buildTaps(const Model& md, int cabinet, int mic, double pos, double dist, double room,
               double rate, Taps& t) {
    CabSpec cb;
    loadCab(cabinet, cb);
    MicSpec ms;
    loadMic(mic, ms);
    const double alpha = ms.alpha;
    t.alpha = alpha;
    const double rho = clamp(pos, 0.0, 1.0) * md.radius;
    // Each ring's surface in 4 radial steps and 12 across the half the mic
    // faces (the other half mirrors it), binned by distance into 4 taps.
    for (int k = 0; k < kRings; ++k) {
        const double r0 = md.edges[k == 0 ? 0 : k + 1], r1 = md.edges[k + 2];
        double nearest = 1e9, farthest = 0.0;
        for (int pass = 0; pass < 2; ++pass) {
            Array<double, 4> sw, swr, swc, swp;
            sw.fill(0.0);
            swr.fill(0.0);
            swc.fill(0.0);
            swp.fill(0.0);
            for (int q = 0; q < 4; ++q) {
                const double ra = r0 + (r1 - r0) * (q + 0.5) * 0.25;
                const double dr = (r1 - r0) * 0.25;
                double z = 0.0;
                if (ra < md.cap) {
                    const double f = ra / md.cap;
                    z = md.depth * (0.85 + 0.15 * f * f);
                } else {
                    z = md.depth * (md.radius - ra) / (md.radius - md.cap);
                }
                const double dz = dist + z;
                for (int p = 0; p < 12; ++p) {
                    const double th = pi * (p + 0.5) / 12.0;
                    const double dA = ra * dr * pi / 12.0 * 2.0;
                    const double dx = ra * cos(th) - rho, dy = ra * sin(th);
                    // (The capsule's own size: no path is shorter than its radius.)
                    const double r = sqrt(dx * dx + dy * dy + dz * dz + kCapsule * kCapsule);
                    if (pass == 0) {
                        nearest = min(nearest, r);
                        farthest = max(farthest, r);
                        continue;
                    }
                    const double w = dA / (twoPi * r);
                    const double c = dz / r;
                    const int b =
                        clamp(floorToInt((r - nearest) / (farthest - nearest + 1e-9) * kBins), 0,
                              kBins - 1);
                    sw[b] += w;
                    swr[b] += w * r;
                    swc[b] += w * c;
                    swp[b] += w * c / r;
                }
            }
            if (pass == 0) continue;
            for (int b = 0; b < kBins; ++b) {
                const int i = k * kBins + b;
                if (sw[b] <= 0.0) {
                    t.delay[i] = nearest / kC * rate;
                    t.wp[i] = 0.0;
                    t.wg[i] = 0.0;
                    continue;
                }
                t.delay[i] = swr[b] / sw[b] / kC * rate;
                t.wp[i] = alpha * sw[b] + (1.0 - alpha) * swc[b];
                t.wg[i] = (1.0 - alpha) * kC * swp[b];
            }
        }
    }
    // The far sources, from the cone's far field: the other speakers, the
    // box's edge or open back, the floor, the wall behind the mic.
    t.farCount = 0;
    t.sources = 0;
    const double d = dist + 0.6 * md.depth;
    for (int j = 0; j < cb.count - 1; ++j) {
        const double lat = sqrt((cb.dx[j] - rho) * (cb.dx[j] - rho) + cb.dy[j] * cb.dy[j]);
        const double path = sqrt(lat * lat + d * d);
        const double sinth = lat / path;
        // Its directivity, a low pass where ka sin(theta) ~ 1.6.
        const double hz = 1.6 * kC / (twoPi * md.radius * max(sinth, 0.001));
        farSource(t, path, 0.6 * md.radius * sinth, 1.0, d / path, hz, alpha, rate);
    }
    const double edge = 0.5 * cb.width - rho;
    if (cb.open) {
        // The rear wave, out of the open back and around the nearest side.
        const double side = sqrt(edge * edge + d * d);
        farSource(t, 2.0 * cb.depth + side, 0.0, -0.5 * (1.0 + room), d / side, 700, alpha, rate);
    } else {
        // The baffle's nearest edge diffracts a weakened, inverted wave.
        const double side = sqrt(edge * edge + d * d);
        farSource(t, edge + side, 0.0, -0.35, d / side, 600, alpha, rate);
    }
    const double floorPath = sqrt(d * d + 4.0 * cb.height * cb.height);
    if (room > 0.0) farSource(t, floorPath, 0.0, 0.85 * room, d / floorPath, 3000, alpha, rate);
    // The wall behind the mic, which only a figure eight's back lobe hears.
    if (room > 0.0 && alpha < 0.25) farSource(t, d + 3.0, 0.0, 0.7 * room, -1.0, 2500, alpha, rate);
    // Nothing arrives before the nearest path.
    double first = 1e9;
    for (int i = 0; i < kRings * kBins; ++i) first = min(first, t.delay[i]);
    for (int i = 0; i < kRings * kBins; ++i) t.delay[i] -= first;
    for (int f = 0; f < t.farCount; ++f)
        t.farDelay[f] = clamp(t.farDelay[f] - first, 0.0, kLine - 4.0);
    for (int i = 0; i < kRings * kBins; ++i) {
        t.whole[i] = floorToInt(t.delay[i]);
        t.part[i] = t.delay[i] - t.whole[i];
    }
    for (int f = 0; f < t.farCount; ++f) {
        t.farWhole[f] = floorToInt(t.farDelay[f]);
        t.farPart[f] = t.farDelay[f] - t.farWhole[f];
    }
}

// The cabinet's coefficients, shared by the channels: the coil's
// admittance, the air load's shelf, the gradient's integrator, the far
// sources' low passes, the capsule, and each mode's state-variable filter.
struct CabDesign {
    Fo elec, air, integ;
    Array<Fo, 8> src;
    Array<Bq, 4> mic;
    Array<bool, 4> micOn;  // a peak of 0 dB is none
    Array<double, 5> g, h, k2, wq;
};

void designCab(const Model& md, const Taps& t, int mic, double rate, CabDesign& d) {
    const double k = 2.0 * rate;
    const double re = md.re + md.zo;
    // The coil: Re (and the amp's output impedance) with Le, its eddy
    // currents' resistance R2 across it.
    d.elec.analog(md.r2, md.le, re * md.r2, md.le * (re + md.r2), k);
    const double g = (md.mc + md.ma) / md.mc;
    d.air.analog(g * md.rr, g * md.ma, md.rr * (1.0 + md.ma / md.mc), md.ma, k);
    d.integ.analog(1.0, 0.0, twoPi * 8.0, 1.0, k);
    for (int m = 0; m < kModes; ++m) {
        const double gm = tan(pi * min(md.w[m] / twoPi, 0.49 * rate) / rate);
        d.g[m] = gm;
        d.k2[m] = 2.0 * md.zeta[m] + gm;
        d.h[m] = 1.0 / (1.0 + 2.0 * md.zeta[m] * gm + gm * gm);
        d.wq[m] = k * gm;
    }
    for (int s = 0; s < kSources; ++s)
        lowpass1(d.src[s], s < t.sources ? t.srcHz[s] : 1000.0, rate);
    MicSpec ms;
    loadMic(mic, ms);
    bqHighpass(d.mic[0], ms.hpHz, ms.hpQ, rate);
    bqPeak(d.mic[1], ms.p1Hz, ms.p1Q, ms.p1Db, rate);
    bqPeak(d.mic[2], ms.p2Hz, ms.p2Q, ms.p2Db, rate);
    bqLowpass(d.mic[3], ms.lpHz, ms.lpQ, rate);
    d.micOn[0] = true;
    d.micOn[1] = ms.p1Db != 0.0;
    d.micOn[2] = ms.p2Db != 0.0;
    d.micOn[3] = true;
}

// A tap's delay with linear interpolation, at w.
void tapAt(double delay, double w, Cx& out) {
    const int n = floorToInt(delay);
    const double fr = delay - n;
    Cx e;
    out.set(1.0 - fr + fr * cos(w), -fr * sin(w));
    e.expj(-w * n);
    out.mul(e);
}

// The cabinet's response at hz, per volt, small signal: exactly what its
// filters do at this rate (mic: with the capsule). Its power, |H|^2.
double cabAt(const Model& md, const Taps& t, const CabDesign& d, double hz, double rate, bool mic) {
    const double w = twoPi * min(hz, 0.4999 * rate) / rate;
    const double sj = 2.0 * rate * tan(0.5 * w);  // where the bilinear maps w
    Cx y, air, mob, tmp, den;
    d.elec.response(w, y);
    d.air.response(w, air);
    Array<double, 8> ar, ai;  // the rings' accelerations per newton at the coil
    ar.fill(0.0);
    ai.fill(0.0);
    for (int m = 0; m < kModes; ++m) {
        const double sn = sj / d.wq[m];
        den.set(1.0 - sn * sn, 2.0 * md.zeta[m] * sn);
        Cx hp, bp;
        hp.set(-sn * sn, 0.0);
        hp.div(den);
        bp.set(0.0, sn);
        bp.div(den);
        const double p0 = md.phi[m];
        mob.addScaled(bp, p0 * p0 / d.wq[m]);
        for (int k = 0; k < kRings; ++k) {
            const double pk = md.phi[(k + 1) * 5 + m] * p0;
            ar[k] += hp.re * pk;
            ai[k] += hp.im * pk;
        }
    }
    // The coil's current through the back EMF's loop (a sample late, as
    // the audio has it), then the force.
    Cx loop = y, i = y, f;
    loop.mul(air);
    loop.mul(mob);
    tmp.expj(-w);
    loop.mul(tmp);
    loop.scale(md.bl * md.bl);
    loop.re += 1.0;
    i.div(loop);
    f = air;
    f.mul(i);
    f.scale(md.bl);
    Cx integ;
    d.integ.response(w, integ);
    Cx total, field;
    for (int k = 0; k < kRings; ++k) {
        Cx a;
        a.set(ar[k], ai[k]);
        a.mul(f);
        field.addScaled(a, md.areas[k] / twoPi);
        Cx heard;
        for (int b = 0; b < kBins; ++b) {
            const int j = k * kBins + b;
            if (t.wp[j] == 0.0 && t.wg[j] == 0.0) continue;
            Cx weight, tap;
            weight.set(t.wp[j] + integ.re * t.wg[j], integ.im * t.wg[j]);
            tapAt(t.delay[j], w, tap);
            tap.mul(weight);
            heard.add(tap);
        }
        heard.mul(a);
        total.add(heard);
    }
    for (int s = 0; s < t.sources; ++s) {
        Cx sum;
        for (int j = 0; j < t.farCount; ++j)
            if (t.farSrc[j] == s) {
                Cx tap;
                tapAt(t.farDelay[j], w, tap);
                sum.addScaled(tap, t.farW[j]);
            }
        Cx lp, weight;
        d.src[s].response(w, lp);
        weight.set(t.srcP[s] + integ.re * t.srcG[s], integ.im * t.srcG[s]);
        sum.mul(lp);
        sum.mul(weight);
        sum.mul(field);
        total.add(sum);
    }
    if (mic)
        for (int b = 0; b < 4; ++b) {
            if (!d.micOn[b]) continue;
            d.mic[b].response(w, tmp);
            total.mul(tmp);
        }
    return total.norm();
}

// The amp alone: the voltage across its speaker load per volt it makes,
// 1 - zo i (the coil's current through the same loop as cabAt's). Its power.
double loadAt(const Model& md, const CabDesign& d, double hz, double rate) {
    const double w = twoPi * min(hz, 0.4999 * rate) / rate;
    const double sj = 2.0 * rate * tan(0.5 * w);
    Cx y, air, mob, tmp, den;
    d.elec.response(w, y);
    d.air.response(w, air);
    for (int m = 0; m < kModes; ++m) {
        const double sn = sj / d.wq[m];
        den.set(1.0 - sn * sn, 2.0 * md.zeta[m] * sn);
        Cx bp;
        bp.set(0.0, sn);
        bp.div(den);
        mob.addScaled(bp, md.phi[m] * md.phi[m] / d.wq[m]);
    }
    Cx loop = y, i = y;
    loop.mul(air);
    loop.mul(mob);
    tmp.expj(-w);
    loop.mul(tmp);
    loop.scale(md.bl * md.bl);
    loop.re += 1.0;
    i.div(loop);
    i.scale(-md.zo);
    i.re += 1.0;
    return i.norm();
}

// The gain that brings the cabinet to unity over the guitar's band (100 Hz
// to 5 kHz, equal weight per octave).
double cabNorm(const Model& md, const Taps& t, const CabDesign& d, double rate) {
    double sum = 0.0;
    for (int k = 0; k < kNormPoints; ++k) {
        const double hz = 100.0 * pow(50.0, (k + 0.5) / kNormPoints);
        sum += cabAt(md, t, d, hz, rate, true);
    }
    const double rms = sqrt(sum / kNormPoints);
    return rms > 1e-30 ? 1.0 / rms : 1.0;
}

// The cabinet's state for one channel.
struct CabLane {
    double ez = 0.0, az = 0.0, iz = 0.0;
    Array<double, 8> srcZ;
    Array<double, 4> m1, m2;
    Array<double, 5> s1, s2;
    double v0 = 0.0, x0 = 0.0, cur = 0.0;
    Array<double, 4> acc;
    Buffer<double> line;  // the four rings and the far field, kLine each
    int at = 0;
    void clear() {
        ez = 0.0;
        az = 0.0;
        iz = 0.0;
        srcZ.fill(0.0);
        m1.fill(0.0);
        m2.fill(0.0);
        s1.fill(0.0);
        s2.fill(0.0);
        v0 = 0.0;
        x0 = 0.0;
        cur = 0.0;
        acc.fill(0.0);
        line.fill(0.0);
        at = 0;
    }
};

// The speaker's motor and cone, one sample: the voltage in, the rings'
// accelerations (acc) and the coil's current (cur) out. Bl falls and the
// suspension stiffens with the excursion; both read last sample's.
void front(const Model& md, const CabDesign& d, CabLane& c, double volts) {
    const double xr = c.x0 / md.xlim;
    const double xr2 = xr * xr;
    const double bl = md.bl / (1.0 + 0.4 * xr2);
    const double e = volts - bl * c.v0;
    const double i = d.elec.b0 * e + c.ez;
    c.ez = d.elec.b1 * e - d.elec.a1 * i;
    c.cur = i;
    const double force = bl * i - md.ksus * c.x0 * xr2;
    const double f = d.air.b0 * force + c.az;
    c.az = d.air.b1 * force - d.air.a1 * f;
    double x0 = 0.0, v0 = 0.0, a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    for (int m = 0; m < kModes; ++m) {
        const double p0 = md.phi[m];
        const double g = d.g[m];
        const double hp = (p0 * f - d.k2[m] * c.s1[m] - c.s2[m]) * d.h[m];
        const double bp = g * hp + c.s1[m];
        c.s1[m] = g * hp + bp;
        const double lp = g * bp + c.s2[m];
        c.s2[m] = g * bp + lp;
        const double wq = d.wq[m];
        x0 += p0 * lp / (wq * wq);
        v0 += p0 * bp / wq;
        a0 += md.phi[5 + m] * hp;
        a1 += md.phi[10 + m] * hp;
        a2 += md.phi[15 + m] * hp;
        a3 += md.phi[20 + m] * hp;
    }
    c.x0 = x0;
    c.v0 = v0;
    c.acc[0] = a0;
    c.acc[1] = a1;
    c.acc[2] = a2;
    c.acc[3] = a3;
}

// A line's sample n + fr samples ago, linearly between samples.
double readLine(const CabLane& c, int line, int n, double fr) {
    const int base = line * kLine;
    const double a = c.line[base + ((c.at - n) & (kLine - 1))];
    const double b = c.line[base + ((c.at - n - 1) & (kLine - 1))];
    return a + fr * (b - a);
}

// The mic's signal, one sample, after front().
double hear(const Model& md, const Taps& t, const CabDesign& d, CabLane& c) {
    double field = 0.0;
    for (int k = 0; k < kRings; ++k) {
        c.line[k * kLine + c.at] = c.acc[k];
        field += md.areas[k] * c.acc[k];
    }
    c.line[4 * kLine + c.at] = field / twoPi;
    double yp = 0.0, yg = 0.0;
    for (int k = 0; k < kRings; ++k)
        for (int b = 0; b < kBins; ++b) {
            const int j = k * kBins + b;
            if (t.wp[j] == 0.0 && t.wg[j] == 0.0) continue;
            const double v = readLine(c, k, t.whole[j], t.part[j]);
            yp += t.wp[j] * v;
            yg += t.wg[j] * v;
        }
    // The far sources (one with no weight, as the floor in a dead room, is
    // skipped).
    Array<double, 8> sum;
    sum.fill(0.0);
    for (int j = 0; j < t.farCount; ++j) {
        const int s = t.farSrc[j];
        if (t.srcP[s] == 0.0 && t.srcG[s] == 0.0) continue;
        sum[s] += t.farW[j] * readLine(c, 4, t.farWhole[j], t.farPart[j]);
    }
    for (int s = 0; s < t.sources; ++s) {
        if (t.srcP[s] == 0.0 && t.srcG[s] == 0.0) continue;
        const Fo& lp = d.src[s];
        const double v = lp.b0 * sum[s] + c.srcZ[s];
        c.srcZ[s] = lp.b1 * sum[s] - lp.a1 * v;
        yp += t.srcP[s] * v;
        yg += t.srcG[s] * v;
    }
    const double ig = d.integ.b0 * yg + c.iz;
    c.iz = d.integ.b1 * yg - d.integ.a1 * ig;
    double y = yp + ig;
    for (int b = 0; b < 4; ++b) {
        if (!d.micOn[b]) continue;
        const Bq& q = d.mic[b];
        const double o = q.b0 * y + c.m1[b];
        c.m1[b] = q.b1 * y - q.a1 * o + c.m2[b];
        c.m2[b] = q.b2 * y - q.a2 * o;
        y = o;
    }
    c.at = (c.at + 1) & (kLine - 1);
    return y;
}

// --- the look --------------------------------------------------------------------------------

constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;
constexpr int kGraphBottom = 0x130F0C;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;

struct Hornbeam : Effect {
    static constexpr Info info = {
        .name = "Hornbeam",
        .id = "com.suppleaudio.hornbeam",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A guitar amplifier and its cabinet, modelled from the circuit and the speaker's "
            "physics with no impulse responses: five voices, a real tone stack, sag, six "
            "cabinets, four speakers and three mics placed anywhere on the cone.",
        .features = {"distortion", "amp-simulator"}};

    Param voice = choice("Voice", {"Clean", "Chime", "Crunch", "Lead", "Bass"}, 1)
                      .in("Amp")
                      .help(
                          "The amp's circuit: clean (two stages, a scooped stack, firm power), "
                          "chime (class A, no feedback), crunch, lead (four tight stages), "
                          "bass.");
    Param gain = percent("Gain", 45)
                     .modulated()
                     .smoothed(30)
                     .in("Amp")
                     .help("The preamp's gain pot: clean, through edge of breakup, to saturated.");
    Param bass =
        percent("Bass", 50)
            .modulated()
            .smoothed(30)
            .in("Amp")
            .help(
                "The tone stack's bass pot; it leans on the mid and treble as a real stack does.");
    Param mid = percent("Mid", 55).modulated().smoothed(30).in("Amp").help(
        "The tone stack's mid pot: at 0 the stack's deepest scoop.");
    Param treble = percent("Treble", 60)
                       .modulated()
                       .smoothed(30)
                       .in("Amp")
                       .help("The tone stack's treble pot.");
    Param presence = percent("Presence", 40)
                         .modulated()
                         .smoothed(30)
                         .in("Amp")
                         .help("The power amp's top end: less negative feedback above 3 kHz.");
    Param sag = percent("Sag", 40).modulated().smoothed(30).in("Amp").help(
        "How far the supply droops under the power amp's draw: a stiff supply at 0, a "
        "compressing, blooming valve rectifier at 100.");
    Param master = percent("Master", 55)
                       .modulated()
                       .smoothed(30)
                       .in("Amp")
                       .help(
                           "The power amp's drive: below half way it stays clean at the same "
                           "level; above, it breaks up, denser and a little louder up to its "
                           "ceiling.");

    Param cabinet =
        choice(
            "Cabinet",
            {"1x12 Open", "2x12 Open", "2x12 Closed", "4x12 Closed", "4x10 Closed", "1x15 Closed"},
            1)
            .keys({"open1x12", "open2x12", "closed2x12", "closed4x12", "closed4x10", "closed1x15"})
            .in("Cabinet")
            .help(
                "The cabinet: its speakers' size and number, and an open back (airy, lighter "
                "lows) or a closed one (a resonance and tight lows).");
    Param speaker = choice("Speaker", {"Alnico", "Vintage", "Modern", "Heavy"}, 0)
                        .in("Cabinet")
                        .help(
                            "The speaker: alnico (light, chiming), vintage (warm), modern "
                            "(forward upper mids), heavy (flat and clean, high power).");
    Param mic =
        choice("Mic", {"Dynamic", "Condenser", "Ribbon"}, 0)
            .in("Cabinet")
            .help("The microphone: dynamic and condenser are cardioid, ribbon a figure eight.");
    Param position = percent("Position", 30)
                         .modulated()
                         .smoothed(50)
                         .in("Cabinet")
                         .help(
                             "Where the mic points on the cone: the cap's centre (bright) at 0, "
                             "the edge of the cone (dark) at 100.");
    Param distance = continuous("Distance", 0.01, 1.0, 0.04, Unit::Meters)
                         .modulated()
                         .curved(Curve::Log)
                         .smoothed(50)
                         .in("Cabinet")
                         .help("The mic's distance from the cone's front, 1 cm to 1 m.");
    Param room =
        percent("Room", 20)
            .modulated()
            .smoothed(50)
            .in("Cabinet")
            .help(
                "How hard the floor and the walls are: their reflections, more audible as the mic "
                "moves back.");

    Param chain = choice("Chain", {"Amp and Cab", "Amp Only", "Cab Only"}, 0)
                      .keys({"full", "amp", "cab"})
                      .in("Output")
                      .help(
                          "Both, the amp alone (its voltage across a speaker's load, for an "
                          "impulse response elsewhere), or the cabinet alone (a clean drive, "
                          "for a preamp or pedal's signal).");
    Param channelMode =
        choice("Channels", {"Mono", "Stereo"}, 0)
            .withKey("channels")
            .in("Output")
            .help(
                "Mono: the left input (as a pedal's L/MONO jack takes it) through one amp and "
                "cabinet, heard on both sides; stereo: an amp and cabinet for each side, at "
                "twice the cost.");
    Param mix =
        percent("Mix", 100)
            .modulated()
            .smoothed(20)
            .in("Output")
            .help("The dry signal and the amp blended (latency aligned): a DI under a bass amp.");
    Param output =
        decibels("Output", -24, 24, 0).modulated().smoothed(20).in("Output").help("The level out.");
    Param quality = choice("Quality", {"1x", "2x", "4x", "8x", "16x"}, 2)
                        .keys({"x1", "x2", "x4", "x8", "x16"})
                        .restarts()
                        .in("Output")
                        .activeUnless(chain, {"Cab Only"})
                        .help(
                            "The amp's oversampling: higher keeps harmonics above hearing from "
                            "folding back (up to 768 kHz inside); adds a few samples of "
                            "latency.");

    Param inputLevel = decibels("Input Level", -96, 24, -96)
                           .meter()
                           .help("The input's peak, dBFS, falling 20 dB a second.");
    Param outputLevel = decibels("Output Level", -96, 24, -96)
                            .meter()
                            .help("The output's peak, dBFS, falling 20 dB a second.");
    Param distortion =
        percent("Distortion", 0)
            .meter()
            .help(
                "What the preamp adds: the share of its output (RMS) that is not a scaled copy "
                "of its input, over the last 300 ms.");
    Param sagNow = decibels("Sag Now", -12, 0, 0)
                       .meter()
                       .help("How far the power amp's supply has drooped this moment, dB.");

    void factoryPatches(Patches& p) {
        p.add("Edge of Breakup");
        p.add("Glass Clean")
            .set(voice, "clean")
            .set(gain, 35)
            .set(bass, 55)
            .set(mid, 40)
            .set(treble, 65)
            .set(presence, 45)
            .set(sag, 20)
            .set(master, 40)
            .set(speaker, "heavy")
            .set(mic, "condenser")
            .set(position, 40)
            .set(distance, 0.08)
            .set(room, 25)
            .set(output, 5);
        p.add("Sparkle Clean")
            .set(voice, "clean")
            .set(gain, 55)
            .set(bass, 45)
            .set(mid, 35)
            .set(treble, 70)
            .set(presence, 55)
            .set(sag, 25)
            .set(master, 45)
            .set(cabinet, "open1x12")
            .set(position, 20)
            .set(distance, 0.03)
            .set(output, -2);
        p.add("Chime")
            .set(gain, 62)
            .set(bass, 40)
            .set(mid, 60)
            .set(treble, 72)
            .set(presence, 55)
            .set(sag, 50)
            .set(master, 62)
            .set(position, 15)
            .set(distance, 0.03)
            .set(output, -3);
        p.add("Classic Crunch")
            .set(voice, "crunch")
            .set(gain, 55)
            .set(bass, 55)
            .set(mid, 65)
            .set(treble, 60)
            .set(presence, 50)
            .set(master, 60)
            .set(cabinet, "closed4x12")
            .set(speaker, "vintage")
            .set(position, 35)
            .set(distance, 0.025)
            .set(room, 15)
            .set(output, 2);
        p.add("Singing Lead")
            .set(voice, "crunch")
            .set(gain, 85)
            .set(bass, 50)
            .set(mid, 70)
            .set(treble, 55)
            .set(presence, 45)
            .set(sag, 70)
            .set(master, 75)
            .set(cabinet, "closed4x12")
            .set(speaker, "vintage")
            .set(position, 40)
            .set(distance, 0.03)
            .set(room, 25)
            .set(output, 2);
        p.add("Hot Rod")
            .set(voice, "lead")
            .set(gain, 40)
            .set(bass, 55)
            .set(mid, 60)
            .set(treble, 60)
            .set(presence, 50)
            .set(sag, 35)
            .set(cabinet, "closed4x12")
            .set(speaker, "modern")
            .set(distance, 0.025)
            .set(room, 15)
            .set(output, 2);
        p.add("High Gain Rhythm")
            .set(voice, "lead")
            .set(gain, 70)
            .set(bass, 50)
            .set(mid, 50)
            .set(treble, 60)
            .set(presence, 55)
            .set(sag, 20)
            .set(master, 50)
            .set(cabinet, "closed4x12")
            .set(speaker, "modern")
            .set(position, 20)
            .set(distance, 0.02)
            .set(room, 10)
            .set(output, 2);
        p.add("Scooped Metal")
            .set(voice, "lead")
            .set(gain, 85)
            .set(bass, 70)
            .set(mid, 20)
            .set(treble, 70)
            .set(presence, 60)
            .set(sag, 15)
            .set(master, 50)
            .set(cabinet, "closed4x12")
            .set(speaker, "modern")
            .set(position, 25)
            .set(distance, 0.02)
            .set(room, 10)
            .set(output, 3);
        p.add("Ribbon Room")
            .set(voice, "crunch")
            .set(gain, 45)
            .set(bass, 50)
            .set(mid, 60)
            .set(treble, 55)
            .set(presence, 45)
            .set(sag, 45)
            .set(master, 65)
            .set(cabinet, "closed2x12")
            .set(speaker, "vintage")
            .set(mic, "ribbon")
            .set(position, 50)
            .set(distance, 0.6)
            .set(room, 60)
            .set(output, 5);
        p.add("Small Combo Squash")
            .set(voice, "clean")
            .set(gain, 85)
            .set(bass, 40)
            .set(mid, 60)
            .set(treble, 55)
            .set(presence, 45)
            .set(sag, 85)
            .set(master, 80)
            .set(cabinet, "open1x12")
            .set(position, 45)
            .set(distance, 0.05)
            .set(room, 30)
            .set(output, 2);
        p.add("Bass Amp")
            .set(voice, "bass")
            .set(gain, 40)
            .set(bass, 60)
            .set(mid, 50)
            .set(treble, 45)
            .set(presence, 35)
            .set(sag, 20)
            .set(master, 45)
            .set(cabinet, "closed4x10")
            .set(speaker, "heavy")
            .set(position, 40)
            .set(distance, 0.05)
            .set(room, 15)
            .set(output, 6);
        p.add("Bass Grit")
            .set(voice, "bass")
            .set(gain, 85)
            .set(bass, 60)
            .set(mid, 60)
            .set(treble, 50)
            .set(presence, 40)
            .set(master, 75)
            .set(cabinet, "closed1x15")
            .set(speaker, "heavy")
            .set(position, 50)
            .set(distance, 0.08)
            .set(room, 15)
            .set(mix, 60)
            .set(output, 5);
        p.add("Cab Only 4x12")
            .set(chain, "cab")
            .set(cabinet, "closed4x12")
            .set(speaker, "vintage")
            .set(distance, 0.025)
            .set(room, 10)
            .set(output, 3);
        p.add("Cab Only 1x12 Open")
            .set(chain, "cab")
            .set(cabinet, "open1x12")
            .set(mic, "condenser")
            .set(position, 40)
            .set(distance, 0.1)
            .set(room, 25)
            .set(output, 1);
        p.add("Amp Only")
            .set(chain, "amp")
            .set(voice, "crunch")
            .set(gain, 55)
            .set(master, 60)
            .set(output, -2);
    }

    // --- audio ------------------------------------------------------------------------------

    double rate = 48000.0;
    int factor = 1;
    int latency = 0;
    double hi = 48000.0;
    Array<Oversampler, 2> over;
    Array<AmpLane, 2> amp;
    Array<CabLane, 2> cab;
    Array<CabLane, 2> load;  // the amp alone: its speaker load's coil
    Array<DelayLine, 2> dry;
    GainCache masterGain, makeupGain, outputGain, trimGain;

    // What is set up: the voice, the cabinet, its model and taps.
    Voice vc;
    Model md, ld;
    Taps taps;
    CabDesign cd, lc;
    int voiceNow = -1, cabNow = -1, speakerNow = -1, micNow = -1, chainNow = -1, monoNow = -1;
    double duck = 1.0, duckStep = 0.01;
    bool unplayed = true;

    // The preamp's level by gain, for the voice.
    Array<double, 65> level;
    Array<double, 48> nodes, weights;
    double weightSum = 1.0;
    double stackMid = 1.0;

    // The glides' last designs.
    double atGain = -1.0, atBass = -1.0, atMid = -1.0, atTreble = -1.0, atPresence = -1.0;
    double atPos = -1.0, atDist = -1.0, atRoom = -1.0;
    int sinceTune = 0, sinceGeometry = 0;
    double sagUp = 0.01, sagDown = 0.001;
    double beta = 0.001, leak = 0.9999, chargeStep = 0.005, stageIn = 1.0;
    double restPa = 0.0;

    // Meters and the distortion's running moments.
    Array<double, 2> peakIn, peakOut;
    double fall = 0.999;
    // What the editor shows.
    Shared<double> sampleRateNow, hiNow, inLeft, inRight, outLeft, outRight;
    Shared<int> latencyShown;
    Array<double, 6> sx, sy, sxx, sxy, syy;
    double sw = 0.0, momentKeep = 0.999;
    int sinceMoments = 0;
    double sagShownDb = 0.0;

    void prepare() {
        rate = sampleRate();
        const int q = quality.index();
        factor = q == 0 ? 1 : (q == 1 ? 2 : (q == 2 ? 4 : (q == 3 ? 8 : 16)));
        while (factor > 1 && rate * factor > 770000.0) factor = factor / 2;
        hi = rate * factor;
        for (int c = 0; c < 2; ++c) {
            over[c].prepare(factor);
            over[c].clear();
            amp[c].clear();
            cab[c].line.resize(5 * kLine);
            cab[c].clear();
            load[c].line.resize(4);
            load[c].clear();
            dry[c].prepare(256);
            dry[c].clear();
        }
        latency = over[0].latency();
        setLatency(latency);
        duckStep = 1.0 / (0.005 * rate);
        duck = 1.0;
        fall = dbToGain(-20.0 / rate);
        momentKeep = exp(-1.0 / (0.3 * rate));
        leak = 1.0 - twoPi * 4.0 / rate;
        chargeStep = 1.0 / (0.001 * hi);
        peakIn.fill(0.0);
        peakOut.fill(0.0);
        sx.fill(0.0);
        sy.fill(0.0);
        sxx.fill(0.0);
        sxy.fill(0.0);
        syy.fill(0.0);
        sw = 0.0;
        sinceMoments = 0;
        sinceTune = 0;
        sinceGeometry = 0;
        unplayed = true;
        weightSum = 0.0;
        const double dz = 10.0 / kNodes;
        for (int j = 0; j < kNodes; ++j) {
            const double z = -5.0 + (j + 0.5) * dz;
            nodes[j] = z;
            weights[j] = exp(-0.5 * z * z);
            weightSum += weights[j];
        }
        voiceNow = -1;
        apply();
    }

    // Sets up the voice, the cabinet and the chain as the parameters say,
    // every state cleared (the output is silent while it happens).
    void apply() {
        voiceNow = voice.index();
        cabNow = cabinet.index();
        speakerNow = speaker.index();
        micNow = mic.index();
        chainNow = chain.index();
        monoNow = channelMode.index() == 0 ? 1 : 0;
        Voice fresh;
        vc = fresh;
        loadVoice(voiceNow, vc);
        sagUp = 1.0 - exp(-1.0 / (0.004 * rate));
        sagDown = 1.0 - exp(-1.0 / (0.001 * vc.sagMs * rate));
        beta = twoPi * vc.otHz / rate;
        stageIn = kIn * dbToGain(vc.gainDb[0]);
        restPa = pushPull(vc.asym, vc.hard);
        const double zo = chainNow == 2 ? 0.0 : vc.zo;
        const double volts = chainNow == 2 ? kCabVolts : sqrt(2.0 * vc.watts * 8.0);
        buildModel(speakerNow, cabNow, zo, volts, md);
        // The amp alone drives a vintage speaker in a closed 4x12 as its load.
        buildModel(1, 3, vc.zo, sqrt(2.0 * vc.watts * 8.0), ld);
        Taps none;
        designCab(ld, none, 0, rate, lc);
        for (int c = 0; c < 2; ++c) {
            amp[c].clear();
            over[c].clear();
            cab[c].clear();
            load[c].clear();
        }
        buildLevel();
        tuneAmp(true);
        tuneCab(true);
    }

    // The preamp's level against gain: a Gaussian at -18 dBFS RMS through its
    // stages as curves (filters at their mid-band gain), and the gain that
    // brings it to kPaRef into the power amp.
    void buildLevel() {
        Array<double, 7> a;
        stackAnalog(vc, trebleShare(0.5), bassShare(0.5), midShare(0.5), a);
        double power = 0.0;
        for (int k = 0; k < 16; ++k) {
            const double s = stackGain(a, 100.0 * pow(30.0, (k + 0.5) / 16.0));
            power += s * s;
        }
        stackMid = sqrt(power / 16.0);
        for (int g = 0; g < kTable; ++g) {
            const double pot = max(audioTaper(g / (kTable - 1.0)), 1e-4);
            double m1 = 0.0, m2 = 0.0;
            for (int j = 0; j < kNodes; ++j) {
                double x = kRef * nodes[j];
                for (int k = 0; k < vc.stages; ++k) {
                    const double u = x * dbToGain(vc.gainDb[k]);
                    if (vc.kind[k] == 1)
                        x = follower(u);
                    else
                        x = -(triode(u + vc.bias[k]) - triode(vc.bias[k]));
                    if (k == vc.potAfter) x *= pot;
                    if (k == vc.stackAfter) x *= stackMid;
                }
                m1 += weights[j] * x;
                m2 += weights[j] * x * x;
            }
            m1 /= weightSum;
            m2 /= weightSum;
            const double rms = sqrt(max(m2 - m1 * m1, 1e-30));
            level[g] = min(kPaRef / rms, 1e4);
        }
        // Below a fifth of the travel the level is no longer made up: the
        // pot fades the amp toward silence, as a real one does.
        const int floorAt = (kTable - 1) / 5;
        for (int g = 0; g < floorAt; ++g) level[g] = level[floorAt];
    }

    double trimAt(double g) const {
        const double at = clamp(g, 0.0, 1.0) * 4.0;
        const int k = min(floorToInt(at), 3);
        return vc.trim[k] + (at - k) * (vc.trim[k + 1] - vc.trim[k]);
    }

    double levelAt(double g) const {
        const double at = clamp(g, 0.0, 1.0) * (kTable - 1);
        const int k = min(floorToInt(at), kTable - 2);
        return level[k] + (at - k) * (level[k + 1] - level[k]);
    }

    // The amp's filters for the glides at frame i (or the values).
    void tuneAmp(bool all) {
        retuneAmp(all, gain.value(), bass.value(), mid.value(), treble.value(), presence.value());
    }

    void retuneAmp(bool all, double g, double b, double m, double t, double p) {
        if (all) {
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < vc.stages; ++k) {
                    Stage& s = amp[c].st[k];
                    highpass1(s.hp, vc.hpHz[k], hi);
                    lowShelf1(s.shelf, vc.shelfHz[k], vc.shelfDb[k], hi);
                    lowpass1(s.lp, vc.lpHz[k], hi);
                    s.gain = dbToGain(vc.gainDb[k]);
                    s.rest = triode(vc.bias[k]);
                    s.keep = exp(-twoPi * vc.hpHz[k] / hi);
                    // A follower is coupled directly; a shelf of 0 dB is none;
                    // the first stage's filters run before the oversampler.
                    s.hpOn = k > 0 && vc.kind[k] == 0;
                    s.isFollower = vc.kind[k] == 1;
                    s.bias = vc.bias[k];
                    s.charge = vc.charge[k];
                    s.shelfOn = k > 0 && vc.shelfDb[k] != 0.0;
                    if (k == 0) {
                        highpass1(amp[c].inHp, vc.hpHz[k], rate);
                        lowShelf1(amp[c].inShelf, vc.shelfHz[k], vc.shelfDb[k], rate);
                    }
                }
        }
        if (all || abs(g - atGain) > 1e-4) {
            atGain = g;
            // The pot with its bright cap across the upper part:
            // g (1 + s (1-g) R C) / (1 + s g (1-g) R C).
            const double pot = max(audioTaper(g), 1e-4);
            const double rc = (1.0 - pot) * kPotOhms * vc.brightPf * 1e-12;
            for (int c = 0; c < 2; ++c) {
                if (rc > 1e-9)
                    amp[c].bright.analog(pot, pot * rc, 1.0, pot * rc, 2.0 * hi);
                else
                    amp[c].bright.analog(pot, 0.0, 1.0, 0.0, 2.0 * hi);
            }
        }
        if (all || abs(b - atBass) > 1e-4 || abs(m - atMid) > 1e-4 || abs(t - atTreble) > 1e-4) {
            atBass = b;
            atMid = m;
            atTreble = t;
            Array<double, 7> a;
            stackAnalog(vc, trebleShare(t), bassShare(b), midShare(m), a);
            // Kept at the stack's mid-band level at noon, which the level
            // table counts.
            for (int c = 0; c < 2; ++c) amp[c].tone.design(a, 2.0 * hi);
        }
        if (all || abs(p - atPresence) > 1e-4) {
            atPresence = p;
            for (int c = 0; c < 2; ++c)
                highShelf1(amp[c].presence, vc.presenceHz, vc.presenceDb * p, hi);
        }
    }

    // The cabinet's paths for the glides (or the values).
    void tuneCab(bool all) { retuneCab(all, position.value(), distance.value(), room.value()); }

    void retuneCab(bool all, double pos, double dist, double rm) {
        if (!all && abs(pos - atPos) < 1e-4 && abs(dist - atDist) < 1e-5 * dist &&
            abs(rm - atRoom) < 1e-4)
            return;
        atPos = pos;
        atDist = dist;
        atRoom = rm;
        buildTaps(md, cabNow, micNow, pos, dist, rm, rate, taps);
        designCab(md, taps, micNow, rate, cd);
        taps.norm = cabNorm(md, taps, cd, rate);
    }

    void update() {
        if (!unplayed) return;
        apply();
    }

    // The amp, one oversampled sample of one channel: its input in volts
    // over the bias, its output at the power amp's ceiling (1).
    double runAmp(AmpLane& a, double x, double comp, double drive, double supply, bool watch) {
        double v = x;
        for (int k = 0; k < vc.stages; ++k) {
            Stage& s = a.st[k];
            double u = v;
            if (k > 0) {
                u *= s.gain;
                if (s.hpOn) u = s.hp.process(u);
                if (s.shelfOn) u = s.shelf.process(u);
            }
            double y = 0.0;
            if (s.isFollower) {
                y = follower(u);
                v = s.lp.process(y);
            } else {
                // Grid current charges the coupling cap (in about a
                // millisecond) by the peaks' excess, biasing the stage colder
                // until it leaks away through the grid resistor (the
                // coupling's own time); the shift saturates as the grid
                // stopper starves the charging.
                u = u + s.bias - s.charge * s.q / (1.0 + s.q * (1.0 / kShiftMax));
                s.q = s.q * s.keep + chargeStep * max(0.0, u - kGrid);
                y = triode(u);
                v = -s.lp.process(y - s.rest);
            }
            if (watch) {
                a.cin[k] = u;
                a.cout[k] = y;
            }
            if (k == vc.potAfter) v = a.bright.process(v);
            if (k == vc.stackAfter) v = a.tone.process(v);
        }
        v *= comp;
        // The power amp: presence and the pair under a sagging supply (the
        // supply's slow follower once a frame).
        const double pu = a.presence.process(v) * drive + vc.asym;
        const double py = pushPull(pu, vc.hard);
        if (watch) {
            a.cin[5] = pu;
            a.cout[5] = py;
        }
        const double y = supply * (py - restPa);
        if (watch) {
            const double mag = abs(y);
            a.env += (mag > a.env ? sagUp : sagDown) * (mag - a.env);
        }
        return y;
    }

    // The output transformer, at the session's rate: its core's flux is the
    // signal integrated, so it saturates the bass first (its harmonics are
    // the bass's, low enough not to need oversampling).
    double transformer(AmpLane& a, double y) {
        a.flux = leak * a.flux + beta * y * vc.otDrive;
        const double e = core(a.flux) - a.flux;
        const double out = y + (e - leak * a.err) / (beta * vc.otDrive);
        a.err = e;
        return out;
    }

    void process(Block& io) {
        const bool first = unplayed;
        unplayed = false;
        const bool stereo = io.channels > 1;
        const int channels = stereo ? 2 : 1;
        if (first) apply();
        for (int i = 0; i < io.frames; ++i) {
            // A switch of voice, cabinet, speaker, mic, chain or channels:
            // down 5 ms, set up afresh in the silence, up 5 ms.
            const bool want = voice.index() != voiceNow || cabinet.index() != cabNow ||
                              speaker.index() != speakerNow || mic.index() != micNow ||
                              chain.index() != chainNow ||
                              (channelMode.index() == 0 ? 1 : 0) != monoNow;
            if (want) {
                duck -= duckStep;
                if (duck <= 0.0) {
                    duck = 0.0;
                    apply();
                }
            } else if (duck < 1.0) {
                duck = min(1.0, duck + duckStep);
            }
            sinceTune += 1;
            if (sinceTune >= 16) {
                sinceTune = 0;
                retuneAmp(false, gain.at(i), bass.at(i), mid.at(i), treble.at(i), presence.at(i));
            }
            sinceGeometry += 1;
            if (sinceGeometry >= 128) {
                sinceGeometry = 0;
                retuneCab(false, position.at(i), distance.at(i), room.at(i));
            }
            const int mode = chainNow;
            const double trim = trimGain.get(trimAt(gain.at(i)));
            const double comp = levelAt(gain.at(i)) * trim;
            const double driveDb = (master.at(i) - 0.5) * 40.0;
            const double drive = masterGain.get(driveDb);
            // Made up below half way; above it, most of the drive is.
            const double makeup = makeupGain.get(driveDb < 0.0 ? -driveDb : -0.6 * driveDb);
            const double sagAmount = sag.at(i);
            const double wet = mix.at(i);
            const double gainOut = outputGain.get(output.at(i)) * duck;
            // The dry sides, delayed as the amp delays; in mono one amp hears
            // the left (a mono source on two channels has both alike).
            Array<double, 2> in, late, wetOut;
            for (int ch = 0; ch < channels; ++ch) {
                double x = io[ch][i];
                if (!isFinite(x)) x = 0.0;
                peakIn[ch] = max(abs(x), peakIn[ch] * fall);
                dry[ch].push(x);
                in[ch] = x;
                late[ch] = dry[ch].read(latency);
            }
            const bool one = channels == 1 || monoNow == 1;
            const int lanes = one ? 1 : 2;
            for (int ln = 0; ln < lanes; ++ln) {
                const double x = in[ln];
                const double d = late[ln];
                double y = 0.0;
                if (mode == 2) {
                    front(md, cd, cab[ln], d * md.volts);
                    y = hear(md, taps, cd, cab[ln]) * taps.norm / md.volts;
                } else {
                    Oversampler& o = over[ln];
                    AmpLane& al = amp[ln];
                    o.up(al.inShelf.process(al.inHp.process(x * stageIn)));
                    // The supply this frame: the drive over it, the ceiling under it.
                    const double supply = max(0.35, 1.0 - sagAmount * vc.sagDepth * al.env);
                    const double into = drive / supply;
                    for (int k = 0; k < factor; ++k)
                        o.set(k, runAmp(al, o.sample(k), comp, into, supply, k == 0));
                    const double a = transformer(al, o.down()) * makeup;
                    if (mode == 1) {
                        front(ld, lc, load[ln], a * ld.volts);
                        y = (a * ld.volts - vc.zo * load[ln].cur) / ld.volts * kOutAmp;
                    } else {
                        front(md, cd, cab[ln], a * md.volts);
                        y = hear(md, taps, cd, cab[ln]) * taps.norm / md.volts * kOutFull;
                    }
                    if (ln == 0) moments(al);
                }
                wetOut[ln] = y;
            }
            for (int ch = 0; ch < channels; ++ch) {
                const double d = late[ch];
                const double y = wetOut[one ? 0 : ch];
                const double out = (d + wet * (y - d)) * gainOut;
                io[ch][i] = out;
                peakOut[ch] = max(abs(out), peakOut[ch] * fall);
            }
        }
        const double inR = stereo ? peakIn[1] : peakIn[0], outR = stereo ? peakOut[1] : peakOut[0];
        inputLevel.set(max(-96.0, gainToDb(max(peakIn[0], inR))));
        outputLevel.set(max(-96.0, gainToDb(max(peakOut[0], outR))));
        inLeft.set(gainToDb(peakIn[0]));
        inRight.set(gainToDb(inR));
        outLeft.set(gainToDb(peakOut[0]));
        outRight.set(gainToDb(outR));
        sampleRateNow.set(rate);
        hiNow.set(hi);
        latencyShown.set(latency);
        if (chainNow == 2) distortion.set(0.0);
        const double supply = max(0.35, 1.0 - sag.value() * vc.sagDepth * amp[0].env);
        sagNow.set(chainNow == 2 ? 0.0 : max(-12.0, gainToDb(supply)));
    }

    // Each curve's input and output, once a frame: the share of its output
    // that is not a scaled copy of its input; the amp's, the curves'
    // shares of power together.
    void moments(const AmpLane& a) {
        for (int k = 0; k < 6; ++k) {
            if (k >= vc.stages && k < 5) continue;
            const double u = a.cin[k], y = a.cout[k];
            sx[k] = momentKeep * sx[k] + u;
            sy[k] = momentKeep * sy[k] + y;
            sxx[k] = momentKeep * sxx[k] + u * u;
            sxy[k] = momentKeep * sxy[k] + u * y;
            syy[k] = momentKeep * syy[k] + y * y;
        }
        sw = momentKeep * sw + 1.0;
        sinceMoments += 1;
        if (sinceMoments < 2400) return;
        sinceMoments = 0;
        double clean = 1.0;
        for (int k = 0; k < 6; ++k) {
            if (k >= vc.stages && k < 5) continue;
            const double mx = sx[k] / sw, my = sy[k] / sw;
            const double vx = sxx[k] / sw - mx * mx, vy = syy[k] / sw - my * my;
            const double cxy = sxy[k] / sw - mx * my;
            if (vx > 1e-12 && vy > 1e-12) clean *= clamp(cxy * cxy / (vx * vy), 0.0, 1.0);
        }
        distortion.set(sqrt(1.0 - clean));
    }

    // --- the editor ----------------------------------------------------------------------------

    static constexpr double kW = 1080;
    static constexpr double kH = 768;
    static constexpr double kDialY = 214;  // the amp's dials
    static constexpr double kDialR = 27;
    static constexpr double tgx = 716;  // the tone graph
    static constexpr double tgy = 98;
    static constexpr double tgw = 332;
    static constexpr double tgh = 150;
    static constexpr double ccx = 158;  // the cone
    static constexpr double ccy = 520;
    static constexpr double cr = 104;
    static constexpr double svx = 300;  // the side view
    static constexpr double svy = 372;
    static constexpr double svw = 164;
    static constexpr double svh = 284;
    static constexpr double rx = 484;  // the response
    static constexpr double ry = 348;
    static constexpr double rw = 564;
    static constexpr double rh = 176;
    static constexpr double kRowY = 540;  // the cabinet's choices
    static constexpr double kFootY = 680;
    static constexpr double kSideScale = 0.6;  // the side view's cone depth, px per mm

    // The editor's own.
    int dragging = 0;  // 1 a dial, 2 the mic across the cone, 3 the mic's distance
    int dragDial = -1;
    double dialPos = 0.0;
    int hotDial = -1;
    double hoverX = -1.0, hoverY = -1.0;

    // The editor's copy of the model, and the curves drawn from it.
    Model em;
    Taps et;
    CabDesign ed;
    Array<double, 241> resp, tone;
    Array<double, 4> ringShare;
    bool have = false;
    int eVoice = -1, eCab = -1, eSpeaker = -1, eMic = -1, eChain = -1;
    double ePos = -1.0, eDist = -1.0, eRoom = -1.0, eRate = -1.0, eHi = -1.0;
    double eGain = -1.0, eBass = -1.0, eMid = -1.0, eTreble = -1.0, ePresence = -1.0;

    void editor(Editor& e) { e.size(kW, kH); }

    double rateNow() const { return sampleRateNow.get() > 1000.0 ? sampleRateNow.get() : 48000.0; }
    double hiRate() const { return hiNow.get() > 1000.0 ? hiNow.get() : 4.0 * rateNow(); }

    double plotHz(int k) const { return 20.0 * pow(1000.0, k / 240.0); }

    // The cabinet as the audio has it: the same model, paths and filters,
    // at the same rate.
    void refreshCab() {
        const double r = rateNow();
        const int v = voice.index(), cb = cabinet.index(), sp = speaker.index();
        const int mc = mic.index(), ch = chain.index();
        const double pos = position.value(), dist = distance.value(), rm = room.value();
        if (have && v == eVoice && cb == eCab && sp == eSpeaker && mc == eMic && ch == eChain &&
            pos == ePos && dist == eDist && rm == eRoom && r == eRate)
            return;
        have = true;
        eVoice = v;
        eCab = cb;
        eSpeaker = sp;
        eMic = mc;
        eChain = ch;
        ePos = pos;
        eDist = dist;
        eRoom = rm;
        eRate = r;
        Voice vv;
        loadVoice(v, vv);
        if (ch == 1) {
            buildModel(1, 3, vv.zo, 1.0, em);
            Taps none;
            designCab(em, none, 0, r, ed);
            for (int k = 0; k <= 240; ++k)
                resp[k] = 10.0 * log10(loadAt(em, ed, plotHz(k), r) + 1e-30);
            ringShare.fill(0.0);
            return;
        }
        buildModel(sp, cb, ch == 2 ? 0.0 : vv.zo, 1.0, em);
        buildTaps(em, cb, mc, pos, dist, rm, r, et);
        designCab(em, et, mc, r, ed);
        const double norm = gainToDb(cabNorm(em, et, ed, r));
        for (int k = 0; k <= 240; ++k)
            resp[k] = 10.0 * log10(cabAt(em, et, ed, plotHz(k), r, true) + 1e-30) + norm;
        // How much of what the mic hears comes from each ring, at its taps'
        // pressure weights and the rings' share of the cone's motion at 1 kHz.
        double most = 0.0;
        for (int k = 0; k < kRings; ++k) {
            double s = 0.0;
            for (int b = 0; b < kBins; ++b) s += abs(et.wp[k * kBins + b]);
            ringShare[k] = s;
            most = max(most, s);
        }
        for (int k = 0; k < kRings; ++k) ringShare[k] = most > 0.0 ? ringShare[k] / most : 0.0;
    }

    // The tone stack, presence and the bright cap as the amp has them, at
    // its oversampled rate, against the stack's level at noon.
    void refreshTone() {
        const double h = hiRate();
        const int v = voice.index();
        const double g = gain.value(), b = bass.value(), m = mid.value(), t = treble.value();
        const double pr = presence.value();
        if (v == eVoice && h == eHi && g == eGain && b == eBass && m == eMid && t == eTreble &&
            pr == ePresence && tone[0] != 0.0)
            return;
        eHi = h;
        eGain = g;
        eBass = b;
        eMid = m;
        eTreble = t;
        ePresence = pr;
        Voice vv;
        loadVoice(v, vv);
        Array<double, 7> a;
        stackAnalog(vv, trebleShare(0.5), bassShare(0.5), midShare(0.5), a);
        double power = 0.0;
        for (int k = 0; k < 16; ++k) {
            const double s = stackGain(a, 100.0 * pow(30.0, (k + 0.5) / 16.0));
            power += s * s;
        }
        const double ref = gainToDb(sqrt(power / 16.0));
        stackAnalog(vv, trebleShare(t), bassShare(b), midShare(m), a);
        Stack3 st;
        st.design(a, 2.0 * h);
        Fo pres, bright;
        highShelf1(pres, vv.presenceHz, vv.presenceDb * pr, h);
        const double pot = max(audioTaper(g), 1e-4);
        const double rc = (1.0 - pot) * kPotOhms * vv.brightPf * 1e-12;
        if (rc > 1e-9)
            bright.analog(pot, pot * rc, 1.0, pot * rc, 2.0 * h);
        else
            bright.analog(pot, 0.0, 1.0, 0.0, 2.0 * h);
        const double potDb = gainToDb(pot);
        for (int k = 0; k <= 240; ++k) {
            const double w = twoPi * plotHz(k) / h;
            Cx a1, a2, a3;
            st.response(w, a1);
            pres.response(w, a2);
            bright.response(w, a3);
            tone[k] = 10.0 * log10(a1.norm() * a2.norm() * a3.norm() + 1e-30) - ref - potDb;
        }
    }

    // --- drawing helpers ----------------------------------------------------------------------

    void panel(Canvas& g, double x, double y, double w, double h) {
        g.linearGradient(0, y, 0, y + h, Theme::surface, kGroundBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 10);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
        g.strokeStyle(Theme::lineLit);
        g.globalAlpha(0.5);
        g.beginPath();
        g.moveTo(x + 10, y + 0.5);
        g.lineTo(x + w - 10, y + 0.5);
        g.stroke();
        g.globalAlpha(1.0);
    }

    void well(Canvas& g, double x, double y, double w, double h) {
        g.linearGradient(0, y, 0, y + h, kGraphTop, kGraphBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 6);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
    }

    void heading(Canvas& g, const char* text, double x, double y) {
        g.fillStyle(Theme::text);
        g.serifFont(15);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void aside(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(11);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void label(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    // The editor's words, by group and index (drawn where they are made).
    void words(Canvas& g, int group, int k, double x, double y) {
        if (group == 0) {
            switch (k) {
                case 0:
                    g.fillText("CLEAN", x, y);
                    break;
                case 1:
                    g.fillText("CHIME", x, y);
                    break;
                case 2:
                    g.fillText("CRUNCH", x, y);
                    break;
                case 3:
                    g.fillText("LEAD", x, y);
                    break;
                default:
                    g.fillText("BASS", x, y);
                    break;
            }
        } else if (group == 1) {
            switch (k) {
                case 0:
                    g.fillText("1x12 OPEN", x, y);
                    break;
                case 1:
                    g.fillText("2x12 OPEN", x, y);
                    break;
                case 2:
                    g.fillText("2x12 CLOSED", x, y);
                    break;
                case 3:
                    g.fillText("4x12 CLOSED", x, y);
                    break;
                case 4:
                    g.fillText("4x10 CLOSED", x, y);
                    break;
                default:
                    g.fillText("1x15 CLOSED", x, y);
                    break;
            }
        } else if (group == 2) {
            switch (k) {
                case 0:
                    g.fillText("ALNICO", x, y);
                    break;
                case 1:
                    g.fillText("VINTAGE", x, y);
                    break;
                case 2:
                    g.fillText("MODERN", x, y);
                    break;
                default:
                    g.fillText("HEAVY", x, y);
                    break;
            }
        } else if (group == 3) {
            switch (k) {
                case 0:
                    g.fillText("DYNAMIC", x, y);
                    break;
                case 1:
                    g.fillText("CONDENSER", x, y);
                    break;
                default:
                    g.fillText("RIBBON", x, y);
                    break;
            }
        } else if (group == 4) {
            switch (k) {
                case 0:
                    g.fillText("AMP AND CAB", x, y);
                    break;
                case 1:
                    g.fillText("AMP ONLY", x, y);
                    break;
                default:
                    g.fillText("CAB ONLY", x, y);
                    break;
            }
        } else if (group == 5) {
            switch (k) {
                case 0:
                    g.fillText("1x", x, y);
                    break;
                case 1:
                    g.fillText("2x", x, y);
                    break;
                case 2:
                    g.fillText("4x", x, y);
                    break;
                case 3:
                    g.fillText("8x", x, y);
                    break;
                default:
                    g.fillText("16x", x, y);
                    break;
            }
        } else if (group == 6) {
            switch (k) {
                case 0:
                    g.fillText("GAIN", x, y);
                    break;
                case 1:
                    g.fillText("BASS", x, y);
                    break;
                case 2:
                    g.fillText("MID", x, y);
                    break;
                case 3:
                    g.fillText("TREBLE", x, y);
                    break;
                case 4:
                    g.fillText("PRESENCE", x, y);
                    break;
                case 5:
                    g.fillText("SAG", x, y);
                    break;
                case 6:
                    g.fillText("MASTER", x, y);
                    break;
                case 7:
                    g.fillText("ROOM", x, y);
                    break;
                case 8:
                    g.fillText("MIX", x, y);
                    break;
                default:
                    g.fillText("OUTPUT", x, y);
                    break;
            }
        } else if (group == 9) {
            switch (k) {
                case 0:
                    g.fillText("MONO", x, y);
                    break;
                default:
                    g.fillText("STEREO", x, y);
                    break;
            }
        } else if (group == 7) {
            switch (k) {
                case 0:
                    g.fillText("two stages, a scooped stack, firm power", x, y);
                    break;
                case 1:
                    g.fillText("class A, no feedback: chime and bloom", x, y);
                    break;
                case 2:
                    g.fillText("two stages and a follower into the stack", x, y);
                    break;
                case 3:
                    g.fillText("four tight stages, one biased cold", x, y);
                    break;
                default:
                    g.fillText("deep iron and headroom", x, y);
                    break;
            }
        } else {
            switch (k) {
                case 0:
                    g.fillText("1", x, y);
                    break;
                case 1:
                    g.fillText("3", x, y);
                    break;
                case 2:
                    g.fillText("10", x, y);
                    break;
                case 3:
                    g.fillText("30", x, y);
                    break;
                default:
                    g.fillText("100", x, y);
                    break;
            }
        }
    }

    // A pill whose words are words(group, k).
    void choicePill(Canvas& g, double x, double y, double w, double h, int group, int k, bool lit,
                    int colour) {
        g.fillStyle(lit ? colour : (hovered(x, y, w, h) ? 0x3A2F27 : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, h * 0.5);
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(lit ? kInk : Theme::textMuted);
        g.boldFont(9.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        words(g, group, k, x + w * 0.5, y + h * 0.5 + 0.5);
    }

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value.
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool hot,
              int name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        g.strokeStyle(colour);
        g.lineWidth(3.5);
        if (at - start > 0.001) {
            g.beginPath();
            g.arc(x, y, r, start, at);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 4, hot ? 0x5E4E41 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 7);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 16), y + sin(at) * (r - 16));
        g.lineTo(x + cos(at) * (r - 9), y + sin(at) * (r - 9));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        words(g, 6, name, x, y - r - 12);
    }

    void valueStyle(Canvas& g) {
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 4, TextAlign::Right);
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double my0 = y + c * 6;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x, my0, w, 4, 2);
            g.fill();
            const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(x, my0, w * t, 4, 2);
                g.fill();
            }
        }
    }

    // A hornbeam leaf: ovate, double-toothed, its veins straight and parallel.
    void drawMark(Canvas& g, double x, double y) {
        g.save();
        g.translate(x, y);
        g.rotate(-0.5);
        g.linearGradient(0, -15, 0, 15, 0x9AA66A, 0x5F6B40);
        g.beginPath();
        g.moveTo(0, -15);
        for (int k = 0; k <= 12; ++k) {
            const double t = k / 12.0;
            const double yy = -15 + 30 * t;
            const double half = 9.0 * sin(pi * pow(t, 0.8)) + (imod(k, 2) == 0 ? 0.0 : 0.9);
            g.lineTo(half, yy);
        }
        for (int k = 12; k >= 0; --k) {
            const double t = k / 12.0;
            const double yy = -15 + 30 * t;
            const double half = 9.0 * sin(pi * pow(t, 0.8)) + (imod(k, 2) == 0 ? 0.0 : 0.9);
            g.lineTo(-half, yy);
        }
        g.closePath();
        g.fill();
        g.strokeStyle(0xC9CF9E);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(0, -14);
        g.lineTo(0, 18);
        g.stroke();
        g.lineWidth(0.7);
        for (int k = 0; k < 7; ++k) {
            const double yy = -10 + k * 3.6;
            const double t = (yy + 15) / 30;
            const double half = 8.0 * sin(pi * pow(clamp(t, 0.0, 1.0), 0.8));
            g.beginPath();
            g.moveTo(0, yy + 2.5);
            g.lineTo(half, yy - 1);
            g.moveTo(0, yy + 2.5);
            g.lineTo(-half, yy - 1);
            g.stroke();
        }
        g.restore();
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 30, 29);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Hornbeam", 54, 37);
        const double nameWidth = g.measureText("Hornbeam");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("a valve amp and its cabinet", 64 + nameWidth, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 680, 23, 150, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 898, 23, 150, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the amp ------------------------------------------------------------------------------

    double dialX(int k) const {
        if (k < 7) return 66 + k * 92;
        if (k == 7) return 960;
        return k == 8 ? 548 : 640;
    }
    double dialY(int k) const {
        if (k < 7) return kDialY;
        if (k == 7) return kRowY + 106;
        return kFootY + 44;
    }
    double dialR(int k) const { return k < 7 ? kDialR : 21.0; }

    void drawAmp(Canvas& g) {
        const bool on = chain.index() != 2;
        panel(g, 16, 64, 1048, 232);
        heading(g, "Amp", 32, 86);
        if (on) {
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(11);
            g.textAlign(TextAlign::Left);
            g.textBaseline(TextBaseline::Middle);
            words(g, 7, voice.index(), 72, 87);
        } else {
            aside(g, "out of the chain: the cabinet alone", 72, 87, TextAlign::Left);
        }
        g.globalAlpha(on ? 1.0 : 0.45);
        for (int k = 0; k < 5; ++k)
            choicePill(g, 32 + k * 120, 112, 112, 24, 0, k, voice.index() == k, Theme::accent);
        for (int k = 0; k < 7; ++k) {
            const Param& p =
                k == 0
                    ? gain
                    : (k == 1
                           ? bass
                           : (k == 2 ? mid
                                     : (k == 3 ? treble
                                               : (k == 4 ? presence : (k == 5 ? sag : master)))));
            dial(g, dialX(k), dialY(k), dialR(k), p.normalized(),
                 k == 0 || k == 6 ? Theme::accent : Theme::accent2,
                 hotDial == k || (dragging == 1 && dragDial == k), k);
            valueStyle(g);
            g.fillText(format("%.0f%%", p.value() * 100.0), dialX(k), dialY(k) + kDialR + 12);
        }
        // The tone: the stack, presence and the bright cap, as the amp has them.
        refreshTone();
        well(g, tgx, tgy, tgw, tgh);
        g.save();
        g.clip(tgx, tgy, tgw, tgh);
        g.frequencyGrid(tgx, tgy, tgw, tgh, 20, 20000);
        g.levelGrid(tgx, tgy, tgw, tgh, -30, 18, 6);
        g.strokeStyle(Theme::accent2);
        g.lineWidth(2);
        g.beginPath();
        for (int k = 0; k <= 240; ++k) {
            const double x = hzToX(plotHz(k), 20, 20000, tgx, tgw);
            const double y = dbToY(clamp(tone[k], -30.0, 18.0), -30, 18, tgy, tgh);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.restore();
        scales(g, tgx, tgy, tgw, tgh, -30, 18, 6);
        aside(g, "the tone stack, presence and bright cap", tgx, tgy + tgh + 24, TextAlign::Left);
        label(g, "DISTORTION", tgx + 200, tgy + tgh + 24, TextAlign::Left);
        valueStyle(g);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%.0f%%", distortion.value() * 100.0), tgx + 264, tgy + tgh + 24);
        label(g, "SAG", tgx + 200, tgy + tgh + 40, TextAlign::Left);
        g.textAlign(TextAlign::Left);
        const double sagDb = sagNow.value();
        if (sagDb > -0.05)
            g.fillText("0.0 dB", tgx + 264, tgy + tgh + 40);
        else
            g.fillText(format("%.1f dB", sagDb), tgx + 264, tgy + tgh + 40);
        g.globalAlpha(1.0);
    }

    // --- the cabinet --------------------------------------------------------------------------

    // The cabinet small, its speakers placed as the model has them, the
    // miked one lit.
    void drawThumb(Canvas& g, double x, double y) {
        CabSpec cb;
        loadCab(cabinet.index(), cb);
        const double s = 70.0;  // px per metre... of a 0.8 m box in 56 px
        const double bw = cb.width * s, bh = (cb.count == 4 ? 0.74 : 0.5) * s;
        g.fillStyle(0x2E241D);
        g.beginPath();
        g.roundRect(x, y, bw, bh, 3);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
        const double r = 0.127 * cb.inches / 12.0 * s;
        const double mx = x + bw * 0.5 - (cb.count > 1 ? 0.5 * cb.dx[0] * s : 0.0);
        const double my = y + bh * 0.5 - (cb.count == 4 ? -0.5 * cb.dy[1] * s : 0.0);
        for (int j = -1; j < cb.count - 1; ++j) {
            const double sx = mx + (j < 0 ? 0.0 : cb.dx[j] * s);
            const double sy = my - (j < 0 ? 0.0 : cb.dy[j] * s);
            g.fillStyle(j < 0 ? 0x6B5440 : 0x3A2F27);
            g.beginPath();
            g.circle(sx, sy, r);
            g.fill();
        }
        g.fillStyle(Theme::accent);
        g.beginPath();
        g.circle(mx + position.value() * r, my, 2.2);
        g.fill();
    }

    void drawCone(Canvas& g) {
        // The baffle, then the cone's rings out from the cap, each lit by its
        // share of what the mic hears.
        g.fillStyle(0x221B16);
        g.beginPath();
        g.roundRect(ccx - cr - 16, ccy - cr - 16, 2 * cr + 32, 2 * cr + 32, 8);
        g.fill();
        g.radialGradient(ccx, ccy - 20, 10, cr, 0x5A4636, 0x2F251D);
        g.beginPath();
        g.circle(ccx, ccy, cr);
        g.fill();
        Array<double, 6> edges;
        edges[0] = 0.0;
        edges[1] = 0.2;
        edges[2] = 0.38;
        edges[3] = 0.62;
        edges[4] = 0.84;
        edges[5] = 1.0;
        for (int k = 0; k < kRings; ++k) {
            const double r0 = edges[k == 0 ? 0 : k + 1] * cr, r1 = edges[k + 2] * cr;
            const double share = ringShare[k];
            if (share <= 0.0) continue;
            g.strokeStyle(Theme::accent);
            g.globalAlpha(0.08 + 0.42 * share);
            g.lineWidth(r1 - r0);
            g.beginPath();
            g.circle(ccx, ccy, 0.5 * (r0 + r1));
            g.stroke();
        }
        g.globalAlpha(1.0);
        g.strokeStyle(0x1F1813);
        g.lineWidth(1);
        for (int k = 2; k < 6; ++k) {
            g.beginPath();
            g.circle(ccx, ccy, edges[k] * cr);
            g.stroke();
        }
        // The surround, the cap.
        g.strokeStyle(0x4A3B2F);
        g.lineWidth(6);
        g.beginPath();
        g.circle(ccx, ccy, cr - 3);
        g.stroke();
        g.radialGradient(ccx - 6, ccy - 8, 2, 0.2 * cr, 0x6E5A48, 0x3A2E24);
        g.beginPath();
        g.circle(ccx, ccy, 0.2 * cr);
        g.fill();
        // The mic, seen from the front, where it points.
        const double mx = ccx + position.value() * cr;
        g.fillStyle(0x000000);
        g.globalAlpha(0.35);
        g.beginPath();
        g.circle(mx + 2, ccy + 3, 11);
        g.fill();
        g.globalAlpha(1.0);
        g.radialGradient(mx - 3, ccy - 3, 1, 11, 0xD9CDB8, 0x8C8170);
        g.beginPath();
        g.circle(mx, ccy, 10);
        g.fill();
        g.strokeStyle(0x5A5146);
        g.lineWidth(0.8);
        for (int k = -2; k <= 2; ++k) {
            g.beginPath();
            g.moveTo(mx + k * 3.2, ccy - sqrt(max(0.0, 100.0 - k * k * 10.24)));
            g.lineTo(mx + k * 3.2, ccy + sqrt(max(0.0, 100.0 - k * k * 10.24)));
            g.stroke();
        }
        valueStyle(g);
        g.fillText(format("%.0f%% across the cone", position.value() * 100.0), ccx, ccy + cr + 28);
    }

    // The side view: the cone's back at the left, its front (where distance
    // starts) a cone's depth to the right, then the mic on a log ruler.
    double sideFront() const { return svx + 14 + 46.0 * kSideScale; }
    double distX(double d) const {
        const double x0 = sideFront() + 8, x1 = svx + svw - 30;
        return x0 + (x1 - x0) * log(clamp(d, 0.01, 1.0) / 0.01) / log(100.0);
    }
    double distAt(double x) const {
        const double x0 = sideFront() + 8, x1 = svx + svw - 30;
        return 0.01 * pow(100.0, clamp((x - x0) / (x1 - x0), 0.0, 1.0));
    }

    void drawSide(Canvas& g) {
        // The speaker from the side, the cone opening to the right, and the
        // mic on a log ruler of its distance from the cone's front.
        SpeakerSpec sp;
        loadSpeaker(speaker.index(), sp);
        CabSpec cb;
        loadCab(cabinet.index(), cb);
        const double bx = sideFront();
        const double half = 0.38 * svh * cb.inches / 15.0;
        const double cy = svy + 0.46 * svh;
        const double depthPx = sp.depth * 1000.0 * kSideScale * cb.inches / 12.0;
        g.fillStyle(0x221B16);
        g.fillRect(bx - 3, svy + 24, 4, svh - 64);
        g.strokeStyle(0x6B5440);
        g.lineWidth(3);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        g.moveTo(bx, cy - half);
        g.lineTo(bx - depthPx, cy - 0.2 * half);
        g.lineTo(bx - depthPx, cy + 0.2 * half);
        g.lineTo(bx, cy + half);
        g.stroke();
        g.fillStyle(0x3A2E24);
        g.fillRect(bx - depthPx - 9, cy - 0.14 * half, 9, 0.28 * half);
        // The ruler.
        const double ry0 = svy + svh - 30;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(distX(0.01), ry0);
        g.lineTo(distX(1.0), ry0);
        g.stroke();
        for (int k = 0; k < 5; ++k) {
            const double x = distX(0.01 * pow(10.0, 0.5 * k));
            g.beginPath();
            g.moveTo(x, ry0 - 3);
            g.lineTo(x, ry0 + 3);
            g.stroke();
            g.fillStyle(Theme::textFaint);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            words(g, 8, k, x, ry0 + 12);
        }
        label(g, "CM", distX(1.0) + 14, ry0 + 12, TextAlign::Left);
        // The mic: its capsule faces the cone, at the position's height.
        const double mx = distX(distance.value());
        const double my = cy - position.value() * half;
        g.strokeStyle(Theme::accent);
        g.globalAlpha(0.35);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(bx, my);
        g.lineTo(mx, my);
        g.stroke();
        g.globalAlpha(1.0);
        g.fillStyle(0x8C8170);
        g.beginPath();
        g.roundRect(mx, my - 5, 26, 10, 4);
        g.fill();
        g.radialGradient(mx, my - 2, 1, 8, 0xD9CDB8, 0x8C8170);
        g.beginPath();
        g.circle(mx, my, 7);
        g.fill();
        valueStyle(g);
        const double d = distance.value();
        if (d < 0.1)
            g.fillText(format("%.1f cm away", d * 100.0), svx + 0.5 * svw, svy + 8);
        else
            g.fillText(format("%.0f cm away", d * 100.0), svx + 0.5 * svw, svy + 8);
    }

    // The graphs' scales: hertz along the bottom, decibels down the left.
    void scales(Canvas& g, double x, double y, double w, double h, double low, double high,
                double step) {
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textBaseline(TextBaseline::Top);
        g.textAlign(TextAlign::Center);
        g.fillText("100", hzToX(100, 20, 20000, x, w), y + h + 3);
        g.fillText("1k", hzToX(1000, 20, 20000, x, w), y + h + 3);
        g.fillText("10k", hzToX(10000, 20, 20000, x, w), y + h + 3);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        for (double db = low + step; db < high; db += step)
            g.fillText(format("%.0f", db), x - 4, dbToY(db, low, high, y, h));
    }

    void drawResponse(Canvas& g) {
        well(g, rx, ry, rw, rh);
        g.save();
        g.clip(rx, ry, rw, rh);
        g.frequencyGrid(rx, ry, rw, rh, 20, 20000);
        g.levelGrid(rx, ry, rw, rh, -36, 12, 6);
        g.globalAlpha(0.18);
        g.fillStyle(Theme::accent);
        g.beginPath();
        g.moveTo(rx, ry + rh);
        for (int k = 0; k <= 240; ++k)
            g.lineTo(hzToX(plotHz(k), 20, 20000, rx, rw),
                     dbToY(clamp(resp[k], -40.0, 14.0), -36, 12, ry, rh));
        g.lineTo(rx + rw, ry + rh);
        g.closePath();
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(Theme::accent);
        g.lineWidth(2);
        g.beginPath();
        for (int k = 0; k <= 240; ++k) {
            const double x = hzToX(plotHz(k), 20, 20000, rx, rw);
            const double y = dbToY(clamp(resp[k], -40.0, 14.0), -36, 12, ry, rh);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.restore();
        scales(g, rx, ry, rw, rh, -36, 12, 12);
        const int ch = chain.index();
        aside(g,
              ch == 1 ? "the speaker load the amp drives (the cabinet is out)"
                      : (ch == 2 ? "the cabinet and mic, driven clean"
                                 : "the cabinet and mic, driven by the amp"),
              rx, ry + rh + 24, TextAlign::Left);
    }

    void drawCab(Canvas& g) {
        refreshCab();
        const bool on = chain.index() != 1;
        panel(g, 16, 308, 1048, 364);
        heading(g, "Cabinet", 32, 330);
        aside(g, on ? "a speaker of masses on springs, heard by its mic" : "out of the chain", 96,
              331, TextAlign::Left);
        g.globalAlpha(on ? 1.0 : 0.45);
        drawCone(g);
        drawThumb(g, 36, 346);
        drawSide(g);
        g.globalAlpha(1.0);
        drawResponse(g);
        g.globalAlpha(on ? 1.0 : 0.45);
        label(g, "CABINET", rx, kRowY + 12 + 32, TextAlign::Left);
        for (int k = 0; k < 6; ++k)
            choicePill(g, rx + 66 + k * 82, kRowY + 32, 78, 24, 1, k, cabinet.index() == k,
                       Theme::accent);
        label(g, "SPEAKER", rx, kRowY + 12 + 64, TextAlign::Left);
        for (int k = 0; k < 4; ++k)
            choicePill(g, rx + 66 + k * 82, kRowY + 64, 78, 24, 2, k, speaker.index() == k,
                       Theme::accent2);
        label(g, "MIC", rx, kRowY + 12 + 96, TextAlign::Left);
        for (int k = 0; k < 3; ++k)
            choicePill(g, rx + 66 + k * 82, kRowY + 96, 78, 24, 3, k, mic.index() == k,
                       Theme::accent2);
        dial(g, dialX(7), dialY(7), dialR(7), room.normalized(), Theme::accent2,
             hotDial == 7 || (dragging == 1 && dragDial == 7), 7);
        valueStyle(g);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%.0f%%", room.value() * 100.0), dialX(7) + 27, dialY(7));
        g.globalAlpha(1.0);
    }

    void drawFooter(Canvas& g) {
        panel(g, 16, kFootY, 1048, 76);
        label(g, "CHAIN", 32, kFootY + 22, TextAlign::Left);
        for (int k = 0; k < 3; ++k)
            choicePill(g, 32 + k * 100, kFootY + 34, 96, 24, 4, k, chain.index() == k,
                       Theme::accent);
        label(g, "CHANNELS", 344, kFootY + 22, TextAlign::Left);
        for (int k = 0; k < 2; ++k)
            choicePill(g, 344 + k * 68, kFootY + 34, 64, 24, 9, k, channelMode.index() == k,
                       Theme::accent2);
        dial(g, dialX(8), dialY(8), dialR(8), mix.normalized(), Theme::accent2,
             hotDial == 8 || (dragging == 1 && dragDial == 8), 8);
        dial(g, dialX(9), dialY(9), dialR(9), output.normalized(), Theme::accent2,
             hotDial == 9 || (dragging == 1 && dragDial == 9), 9);
        valueStyle(g);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%.0f%%", mix.value() * 100.0), dialX(8) + 26, dialY(8));
        if (abs(output.value()) < 0.05)
            g.fillText("0.0 dB", dialX(9) + 26, dialY(9));
        else
            g.fillText(format("%+.1f dB", output.value()), dialX(9) + 26, dialY(9));
        const bool amp = chain.index() != 2;
        g.globalAlpha(amp ? 1.0 : 0.45);
        label(g, "OVERSAMPLING", 730, kFootY + 22, TextAlign::Left);
        for (int k = 0; k < 5; ++k)
            choicePill(g, 730 + k * 48, kFootY + 34, 44, 24, 5, k, quality.index() == k,
                       Theme::accent2);
        g.globalAlpha(1.0);
        valueStyle(g);
        g.textAlign(TextAlign::Right);
        const int lat = latencyShown.get();
        g.fillText(format("%d samples", lat), 1048, kFootY + 38);
        g.fillText(format("%.2f ms", lat * 1000.0 / rateNow()), 1048, kFootY + 54);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawAmp(g);
        drawCab(g);
        drawFooter(g);
    }

    // --- the pointer ---------------------------------------------------------------------------

    void setDial(int k, int how, double v) {
        // how: 0 a value in its unit, 1 a knob position (0..1), 2 the default.
        const Param& p =
            k == 0
                ? gain
                : (k == 1
                       ? bass
                       : (k == 2
                              ? mid
                              : (k == 3
                                     ? treble
                                     : (k == 4
                                            ? presence
                                            : (k == 5
                                                   ? sag
                                                   : (k == 6 ? master
                                                             : (k == 7 ? room
                                                                       : (k == 8 ? mix
                                                                                 : output))))))));
        if (how == 0)
            p.edit(v);
        else if (how == 1)
            p.editNormalized(v);
        else
            p.reset();
    }

    double dialValue(int k) const {
        const Param& p =
            k == 0
                ? gain
                : (k == 1
                       ? bass
                       : (k == 2
                              ? mid
                              : (k == 3
                                     ? treble
                                     : (k == 4
                                            ? presence
                                            : (k == 5
                                                   ? sag
                                                   : (k == 6 ? master
                                                             : (k == 7 ? room
                                                                       : (k == 8 ? mix
                                                                                 : output))))))));
        return p.normalized();
    }

    int dialAt(double x, double y) const {
        for (int k = 0; k < 10; ++k) {
            const double dx = x - dialX(k), dy = y - dialY(k), r = dialR(k) + 6;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }

    bool overCone(const Pointer& p) const { return p.within(ccx, ccy, cr + 10); }
    bool overSide(const Pointer& p) const { return p.over(svx, svy, svw, svh); }

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotDial = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotDial = dialAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p);
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                dialPos = clamp(dialPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setDial(dragDial, 1, dialPos);
            } else if (dragging == 2) {
                position.edit(clamp(abs(p.x() - ccx) / cr, 0.0, 1.0));
            } else if (dragging == 3) {
                distance.edit(distAt(p.x()));
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotDial = dialAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            if (overCone(p) || overSide(p)) {
                distance.edit(distance * pow(2.0, notches * 0.125 * fine));
                return;
            }
            const int k = dialAt(p.x(), p.y());
            if (k >= 0) setDial(k, 1, clamp(dialValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        const int k = dialAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                setDial(k, 2, 0);
                return;
            }
            dragging = 1;
            dragDial = k;
            dialPos = dialValue(k);
            return;
        }
        if (overCone(p)) {
            if (p.doubleClick()) {
                position.reset();
                return;
            }
            dragging = 2;
            position.edit(clamp(abs(x - ccx) / cr, 0.0, 1.0));
            return;
        }
        if (overSide(p)) {
            if (p.doubleClick()) {
                distance.reset();
                return;
            }
            dragging = 3;
            distance.edit(distAt(x));
            return;
        }
        for (int c = 0; c < 5; ++c)
            if (p.over(32 + c * 120, 112, 112, 24)) {
                voice.edit(c);
                return;
            }
        for (int c = 0; c < 6; ++c)
            if (p.over(rx + 66 + c * 82, kRowY + 32, 78, 24)) {
                cabinet.edit(c);
                return;
            }
        for (int c = 0; c < 4; ++c)
            if (p.over(rx + 66 + c * 82, kRowY + 64, 78, 24)) {
                speaker.edit(c);
                return;
            }
        for (int c = 0; c < 3; ++c)
            if (p.over(rx + 66 + c * 82, kRowY + 96, 78, 24)) {
                mic.edit(c);
                return;
            }
        for (int c = 0; c < 3; ++c)
            if (p.over(32 + c * 100, kFootY + 34, 96, 24)) {
                chain.edit(c);
                return;
            }
        for (int c = 0; c < 2; ++c)
            if (p.over(344 + c * 68, kFootY + 34, 64, 24)) {
                channelMode.edit(c);
                return;
            }
        for (int c = 0; c < 5; ++c)
            if (p.over(730 + c * 48, kFootY + 34, 44, 24)) {
                quality.edit(c);
                return;
            }
    }
};
