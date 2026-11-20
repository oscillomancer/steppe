// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Yarrow: an eight-band equaliser with zero latency.
//
// Each band is a bell, a shelf, a cut, a notch or a tilt, on both channels,
// the mid, the side, or one channel alone. Bells and shelves can be dynamic:
// they act only while their part of the spectrum is louder than a threshold
// (a de-esser, a resonance tamer). The filters are matched to their analog
// prototypes, so a curve keeps its shape up to the top octave instead of
// bunching up near half the sample rate.
//
// The editor draws the spectrum of the output as a single smooth line: a
// 32768-point transform (bass first: 1.5 Hz apart at 48 kHz), smoothed to a
// sixteenth of an octave and tilted 4.5 dB per octave so that a balanced mix
// reads level. Drag a node to move a band, turn the wheel over it for its Q,
// hold Alt to hear only that band, Ctrl to land on a note, Shift for fine
// moves; double-click empty space to add a band, a node to switch it off.

// --- filters ------------------------------------------------------------------
//
// A second-order section, run in transposed direct form II, and the
// designs: poles placed exactly where the analog prototype's are (impulse
// invariance), zeros fitted to the prototype's magnitude, exact at DC and at
// the band's frequency and as close as least squares gets them above it, so
// a curve keeps its analog shape up to the top octave.

struct Sec {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct Mem {
    double z1 = 0.0, z2 = 0.0;
};

double run(const Sec& s, Mem& m, double x) {
    const double y = s.b0 * x + m.z1;
    m.z1 = s.b1 * x - s.a1 * y + m.z2;
    m.z2 = s.b2 * x - s.a2 * y;
    return y;
}

void identity(Sec& s) {
    s.b0 = 1.0;
    s.b1 = 0.0;
    s.b2 = 0.0;
    s.a1 = 0.0;
    s.a2 = 0.0;
}

// The poles of s^2 + 2 zeta wn s + wn^2, wn in radians per sample.
void poles(Sec& s, double wn, double zeta) {
    const double e = exp(-zeta * wn);
    s.a2 = e * e;
    if (zeta < 1.0)
        s.a1 = -2.0 * e * cos(wn * sqrt(1.0 - zeta * zeta));
    else
        s.a1 = -2.0 * e * cosh(wn * sqrt(zeta * zeta - 1.0));
}

// An analog prototype: k (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), with s
// relative to the band's frequency.
struct Prototype {
    double k = 1.0;
    double n2 = 1.0, n1 = 0.0, n0 = 1.0;
    double d2 = 1.0, d1 = 0.0, d0 = 1.0;
};

// Its power gain |H(jw)|^2, w relative to the band's frequency.
double analog(const Prototype& h, double w) {
    const double nr = h.n0 - h.n2 * w * w, ni = h.n1 * w;
    const double dr = h.d0 - h.d2 * w * w, di = h.d1 * w;
    return h.k * (nr * nr + ni * ni) / (dr * dr + di * di);
}

// Zeros for the poles already in s, matching the prototype h (band frequency
// w0, radians per sample). A section's power gain is (c0 p0 + c1 p1 + c2 p2)
// over the same in its poles, with p1 = sin^2(w/2), p0 = 1 - p1, p2 = 4 p0 p1;
// c0 holds DC, c2 follows from matching at wc, and c1 (Nyquist) is chosen by
// least squares of the relative error at 16 frequencies from wc/4 up.
void fit(Sec& s, const Prototype& h, double w0, double wc) {
    const double a0 = (1.0 + s.a1 + s.a2) * (1.0 + s.a1 + s.a2);
    const double a1 = (1.0 - s.a1 + s.a2) * (1.0 - s.a1 + s.a2);
    const double a2 = -4.0 * s.a2;
    const double sc = sin(0.5 * wc);
    const double pc1 = sc * sc;
    const double pc0 = 1.0 - pc1;
    const double pc2 = 4.0 * pc0 * pc1;
    const double c0 = analog(h, 0.0) * a0;
    // c2 = base + slope * c1 keeps the match at wc.
    const double base = (analog(h, wc / w0) * (a0 * pc0 + a1 * pc1 + a2 * pc2) - c0 * pc0) / pc2;
    const double slope = -pc1 / pc2;
    double ab = 0.0, bb = 0.0;
    for (int i = 0; i < 16; ++i) {
        const double w = exp(log(0.25 * wc) + (log(0.999 * pi) - log(0.25 * wc)) * i / 15.0);
        const double sn = sin(0.5 * w);
        const double p1 = sn * sn;
        const double p0 = 1.0 - p1;
        const double p2 = 4.0 * p0 * p1;
        const double want = analog(h, w / w0) * (a0 * p0 + a1 * p1 + a2 * p2);
        const double a = (c0 * p0 + base * p2) / want - 1.0;
        const double b = (p1 + slope * p2) / want;
        ab += a * b;
        bb += b * b;
    }
    const double c1 = max(0.0, -ab / max(bb, 1e-300));
    const double c2 = base + slope * c1;
    const double r0 = sqrt(c0), r1 = sqrt(c1);
    const double w = 0.5 * (r0 + r1);
    const double d = sqrt(max(0.0, w * w + c2));
    s.b0 = 0.5 * (w + d);
    s.b1 = 0.5 * (r0 - r1);
    s.b2 = 0.5 * (w - d);
}

// The section's inverse: poles become zeros and zeros poles.
void invert(Sec& s) {
    const double b0 = s.b0, b1 = s.b1, b2 = s.b2, a1 = s.a1, a2 = s.a2;
    s.b0 = 1.0 / b0;
    s.b1 = a1 / b0;
    s.b2 = a2 / b0;
    s.a1 = b1 / b0;
    s.a2 = b2 / b0;
}

// A bell: (s^2 + s A/Q + 1) / (s^2 + s/(AQ) + 1). A cut is the boost inverted,
// so the poles always sit at the centre.
void bell(Sec& s, double w0, double q, double db) {
    const double a = pow(10.0, abs(db) / 40.0);
    poles(s, w0, 0.5 / (a * q));
    Prototype h;
    h.n1 = a / q;
    h.d1 = 1.0 / (a * q);
    fit(s, h, w0, w0);
    if (db < 0.0) invert(s);
}

// Shelves: A (s^2 + sqrt(A)/Q s + A) / (A s^2 + sqrt(A)/Q s + 1) for the low
// one, its mirror for the high. Each is designed in the direction whose
// poles lie below its corner, and inverted for the other; it is matched at
// its zeros' frequency, where its curve turns, unless that is near Nyquist.
void lowShelf(Sec& s, double w0, double q, double db) {
    const double a = pow(10.0, abs(db) / 40.0), r = sqrt(a);
    poles(s, w0 / r, 0.5 / q);
    double wz = w0 * r;
    if (wz > 0.6 * pi) wz = w0;
    Prototype h;
    h.k = a * a;
    h.n1 = r / q;
    h.n0 = a;
    h.d2 = a;
    h.d1 = r / q;
    fit(s, h, w0, wz);
    if (db < 0.0) invert(s);
}

void highShelf(Sec& s, double w0, double q, double db) {
    const double a = pow(10.0, -abs(db) / 40.0), r = sqrt(a);
    poles(s, w0 * r, 0.5 / q);
    double wz = w0 / r;
    if (wz > 0.6 * pi) wz = w0;
    Prototype h;
    h.k = a * a;
    h.n2 = a;
    h.n1 = r / q;
    h.d1 = r / q;
    h.d0 = a;
    fit(s, h, w0, wz);
    if (db > 0.0) invert(s);
}

// Second-order low- and high-pass sections, matched at DC and at the corner.
void lowPass(Sec& s, double w0, double q) {
    poles(s, w0, 0.5 / q);
    const double a0 = (1.0 + s.a1 + s.a2) * (1.0 + s.a1 + s.a2);
    const double a1 = (1.0 - s.a1 + s.a2) * (1.0 - s.a1 + s.a2);
    const double a2 = -4.0 * s.a2;
    const double sn = sin(0.5 * w0);
    const double p1 = sn * sn;
    const double p0 = 1.0 - p1;
    const double p2 = 4.0 * p0 * p1;
    const double c1 = max(0.0, ((a0 * p0 + a1 * p1 + a2 * p2) * q * q - a0 * p0) / p1);
    s.b0 = 0.5 * (sqrt(a0) + sqrt(c1));
    s.b1 = sqrt(a0) - s.b0;
    s.b2 = 0.0;
}

void highPass(Sec& s, double w0, double q) {
    poles(s, w0, 0.5 / q);
    const double a0 = (1.0 + s.a1 + s.a2) * (1.0 + s.a1 + s.a2);
    const double a1 = (1.0 - s.a1 + s.a2) * (1.0 - s.a1 + s.a2);
    const double a2 = -4.0 * s.a2;
    const double sn = sin(0.5 * w0);
    const double p1 = sn * sn;
    const double p0 = 1.0 - p1;
    const double p2 = 4.0 * p0 * p1;
    s.b0 = q * sqrt(a0 * p0 + a1 * p1 + a2 * p2) / (4.0 * p1);
    s.b1 = -2.0 * s.b0;
    s.b2 = s.b0;
}

// First-order sections, matched at DC and at Nyquist.
void lowPass1(Sec& s, double w0) {
    const double p = exp(-w0);
    const double ratio = pi / w0;
    const double nyquist = sqrt(1.0 / (1.0 + ratio * ratio));
    s.a1 = -p;
    s.a2 = 0.0;
    const double sum = 1.0 - p, diff = (1.0 + p) * nyquist;
    s.b0 = 0.5 * (sum + diff);
    s.b1 = 0.5 * (sum - diff);
    s.b2 = 0.0;
}

void highPass1(Sec& s, double w0) {
    const double p = exp(-w0);
    const double ratio = pi / w0;
    const double nyquist = ratio / sqrt(1.0 + ratio * ratio);
    s.a1 = -p;
    s.a2 = 0.0;
    s.b0 = 0.5 * (1.0 + p) * nyquist;
    s.b1 = -s.b0;
    s.b2 = 0.0;
}

void notch(Sec& s, double w0, double q) {
    poles(s, w0, 0.5 / q);
    const double c = cos(w0);
    const double k = (1.0 + s.a1 + s.a2) / (2.0 - 2.0 * c);
    s.b0 = k;
    s.b1 = -2.0 * c * k;
    s.b2 = k;
}

// The band types, in the order of the Type parameter's choices.
constexpr int kBell = 0;
constexpr int kLowShelf = 1;
constexpr int kHighShelf = 2;
constexpr int kLowCut = 3;
constexpr int kHighCut = 4;
constexpr int kNotch = 5;
constexpr int kTilt = 6;

bool hasGain(int kind) {
    return kind == kBell || kind == kLowShelf || kind == kHighShelf || kind == kTilt;
}
bool isCut(int kind) { return kind == kLowCut || kind == kHighCut; }
bool canBeDynamic(int kind) { return kind == kBell || kind == kLowShelf || kind == kHighShelf; }

// A cut's order for each choice of Slope (6, 12, 18, 24, 36 and 48 dB per octave).
int orderOf(int slope) {
    if (slope <= 3) return slope + 1;
    return slope == 4 ? 6 : 8;
}

// A band as sections: their number, which is fixed by its type and order
// (a band at 0 dB is sections that pass the signal unchanged), so changing
// its frequency, gain or Q never changes how many there are.
int design(Array<Sec, 4>& out, int kind, double hz, double db, double q, int order, double rate) {
    const double w0 = twoPi * clamp(hz, 10.0, 0.45 * rate) / rate;
    const bool flat = abs(db) < 1e-6;
    if (kind == kBell) {
        if (flat)
            identity(out[0]);
        else
            bell(out[0], w0, q, db);
        return 1;
    }
    if (kind == kLowShelf || kind == kHighShelf) {
        const double sq = clamp(q, 0.25, 2.0);
        if (flat)
            identity(out[0]);
        else if (kind == kLowShelf)
            lowShelf(out[0], w0, sq, db);
        else
            highShelf(out[0], w0, sq, db);
        return 1;
    }
    if (kind == kTilt) {
        const double sq = clamp(q, 0.25, 1.0) * 0.5;
        if (flat) {
            identity(out[0]);
            identity(out[1]);
        } else {
            lowShelf(out[0], w0, sq, -0.5 * db);
            highShelf(out[1], w0, sq, 0.5 * db);
        }
        return 2;
    }
    if (kind == kNotch) {
        notch(out[0], w0, q);
        return 1;
    }
    // Cuts: Butterworth sections, the most resonant one scaled by Q.
    const bool low = kind == kLowCut;
    const int pairs = idiv(order, 2);
    int n = 0;
    for (int k = 0; k < pairs; ++k) {
        double sq = 0.5 / cos((2.0 * k + 1.0) * pi / (2.0 * order));
        if (k == pairs - 1) sq *= clamp(q, 0.1, 30.0) / 0.70710678;
        if (low)
            highPass(out[n], w0, sq);
        else
            lowPass(out[n], w0, sq);
        ++n;
    }
    if (imod(order, 2) == 1) {
        if (low)
            highPass1(out[n], w0);
        else
            lowPass1(out[n], w0);
        ++n;
    }
    return n;
}

// A section's power gain at one frequency, given p1 = sin^2(w/2).
double power(const Sec& s, double p1) {
    const double p0 = 1.0 - p1;
    const double p2 = 4.0 * p0 * p1;
    const double n0 = (s.b0 + s.b1 + s.b2) * (s.b0 + s.b1 + s.b2);
    const double n1 = (s.b0 - s.b1 + s.b2) * (s.b0 - s.b1 + s.b2);
    const double d0 = (1.0 + s.a1 + s.a2) * (1.0 + s.a1 + s.a2);
    const double d1 = (1.0 - s.a1 + s.a2) * (1.0 - s.a1 + s.a2);
    const double num = n0 * p0 + n1 * p1 - 4.0 * s.b0 * s.b2 * p2;
    const double den = d0 * p0 + d1 * p1 - 4.0 * s.a2 * p2;
    return max(num, 0.0) / max(den, 1e-30);
}

// A soft knee: 0 well below zero, x well above, a parabola between.
double knee(double x, double width) {
    if (x <= -0.5 * width) return 0.0;
    if (x >= 0.5 * width) return x;
    const double t = x + 0.5 * width;
    return t * t / (2.0 * width);
}

// What a band is set to: the parameters, read once into numbers.
struct Setting {
    bool on = false;
    int kind = 0;
    double hz = 1000.0;
    double db = 0.0;
    double q = 1.0;
    int order = 4;
    int place = 0;
    bool dynamic = false;
    double threshold = -30.0;
};

// Channel placement, in the order of the Channel parameter's choices.
constexpr int kStereo = 0;
constexpr int kMid = 1;
constexpr int kSide = 2;
constexpr int kLeft = 3;
constexpr int kRight = 4;

// A band as heard: its structure, its values gliding toward the settings,
// its sections and their memory for each channel, and its detector.
struct Band {
    bool on = false;
    int kind = 0;
    int order = 4;
    int place = 0;
    double hz = 1000.0;
    double db = 0.0;  // the set gain, scaled, gliding
    double q = 1.0;
    double now = 0.0;  // the gain designed into the sections: db, or less while dynamic
    double designedHz = 0.0, designedDb = 1e9, designedQ = 0.0;
    double wet = 0.0;
    double fade = 0.0;  // +1 fading in, -1 out
    int count = 0;
    Array<Sec, 4> sec;
    Array<Mem, 4> m0, m1;
    Biquad det0, det1;
    EnvelopeFollower env;
    double detHz = 0.0, detQ = 0.0;
    int detKind = -1;
    double level = -120.0;  // the detector's level, dB
};

// A band's sections as the editor draws them.
struct Shape {
    Array<Sec, 4> sec;
    int count = 0;
};

double chain(const Array<Sec, 4>& sec, int count, Array<Mem, 4>& m, double x) {
    double y = x;
    for (int k = 0; k < count; ++k) y = run(sec[k], m[k], y);
    return y;
}

// Colours: the bands' (earth and meadow: clay, marigold, ochre, moss, sage,
// dusk, heather, madder), and the editor's own beside the palette.
int bandColour(int b) {
    switch (b) {
        case 0:
            return 0xD9775A;
        case 1:
            return 0xE29A52;
        case 2:
            return 0xD5BA62;
        case 3:
            return 0x9DB36E;
        case 4:
            return 0x72AE9B;
        case 5:
            return 0x82A3C3;
        case 6:
            return 0xAB91C6;
        default:
            return 0xCF8099;
    }
}

constexpr int kInk = 0x1A1411;        // text on a lit colour, unlit wells
constexpr int kGroundTop = 0x2B221B;  // the walnut ground, lighter at the top
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;  // the graph's soil
constexpr int kGraphBottom = 0x130F0C;
constexpr int kGrid = 0x2B231D;
constexpr int kGridLit = 0x3E332A;
constexpr int kSpectrum = 0xE3D5BE;  // the analyser's line: unbleached linen
constexpr int kCurve = 0xF7EEDF;
constexpr int kTip = 0x2C231C;
constexpr int kMeterLow = 0x6E7A4B;  // moss to dry grass
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kIdle = 0x76675A;

// Fields of a band, for editing one from the editor.
constexpr int fOn = 0;
constexpr int fType = 1;
constexpr int fFreq = 2;
constexpr int fGain = 3;
constexpr int fQ = 4;
constexpr int fSlope = 5;
constexpr int fChannel = 6;
constexpr int fDynamic = 7;
constexpr int fThreshold = 8;

// --- the analyser's transform ------------------------------------------------------

constexpr int kN = 32768;         // samples per transform
constexpr int kM = 16384;         // complex points: the real transform packed in half
constexpr int kPoints = 420;      // across the graph, 20 Hz to 20 kHz
constexpr double kSmooth = 16.0;  // a sixteenth of an octave
constexpr double kSlopeDb = 4.5;  // dB per octave, about 1 kHz
constexpr double kAverage = 0.5;  // seconds of power averaging

struct Yarrow : Effect {
    static constexpr Info info = {.name = "Yarrow",
                                  .id = "com.suppleaudio.yarrow",
                                  .vendor = "Supple Audio",
                                  .version = "1.0.0",
                                  .description =
                                      "Eight-band zero-latency equaliser with dynamic bands, "
                                      "mid/side placement and a high-resolution spectrum.",
                                  .features = {"equalizer"}};

    Param b1On = toggle("Band 1 On", false)
                     .in("Band 1")
                     .withKey("band1.enabled")
                     .help("Whether band 1 shapes the sound; off, it passes everything unchanged.");
    Param b1Type =
        choice("Band 1 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 3)
            .in("Band 1")
            .activeWhen(b1On)
            .withKey("band1.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b1Freq = frequency("Band 1 Frequency", 20, 20000, 30)
                       .modulated()
                       .in("Band 1")
                       .activeWhen(b1On)
                       .withKey("band1.freq")
                       .help("Its centre, corner or pivot.");
    Param b1Gain =
        decibels("Band 1 Gain", -30, 30, 0)
            .modulated()
            .in("Band 1")
            .activeUnless(b1Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band1.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b1Q = continuous("Band 1 Q", 0.1, 30, 0.71, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 1")
                    .activeWhen(b1On)
                    .withKey("band1.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b1Slope =
        choice("Band 1 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 1")
            .activeWhen(b1Type, {"Low Cut", "High Cut"})
            .withKey("band1.slope")
            .help("How steeply a cut falls away.");
    Param b1Channel = choice("Band 1 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 1")
                          .activeWhen(b1On)
                          .withKey("band1.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b1Dynamic = toggle("Band 1 Dynamic", false)
                          .in("Band 1")
                          .activeWhen(b1Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band1.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b1Threshold =
        decibels("Band 1 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 1")
            .activeWhen(b1Dynamic)
            .withKey("band1.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b2On = toggle("Band 2 On", false)
                     .in("Band 2")
                     .withKey("band2.enabled")
                     .help("Whether band 2 shapes the sound; off, it passes everything unchanged.");
    Param b2Type =
        choice("Band 2 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 1)
            .in("Band 2")
            .activeWhen(b2On)
            .withKey("band2.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b2Freq = frequency("Band 2 Frequency", 20, 20000, 100)
                       .modulated()
                       .in("Band 2")
                       .activeWhen(b2On)
                       .withKey("band2.freq")
                       .help("Its centre, corner or pivot.");
    Param b2Gain =
        decibels("Band 2 Gain", -30, 30, 0)
            .modulated()
            .in("Band 2")
            .activeUnless(b2Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band2.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b2Q = continuous("Band 2 Q", 0.1, 30, 0.71, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 2")
                    .activeWhen(b2On)
                    .withKey("band2.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b2Slope =
        choice("Band 2 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 2")
            .activeWhen(b2Type, {"Low Cut", "High Cut"})
            .withKey("band2.slope")
            .help("How steeply a cut falls away.");
    Param b2Channel = choice("Band 2 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 2")
                          .activeWhen(b2On)
                          .withKey("band2.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b2Dynamic = toggle("Band 2 Dynamic", false)
                          .in("Band 2")
                          .activeWhen(b2Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band2.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b2Threshold =
        decibels("Band 2 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 2")
            .activeWhen(b2Dynamic)
            .withKey("band2.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b3On = toggle("Band 3 On", false)
                     .in("Band 3")
                     .withKey("band3.enabled")
                     .help("Whether band 3 shapes the sound; off, it passes everything unchanged.");
    Param b3Type =
        choice("Band 3 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 0)
            .in("Band 3")
            .activeWhen(b3On)
            .withKey("band3.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b3Freq = frequency("Band 3 Frequency", 20, 20000, 250)
                       .modulated()
                       .in("Band 3")
                       .activeWhen(b3On)
                       .withKey("band3.freq")
                       .help("Its centre, corner or pivot.");
    Param b3Gain =
        decibels("Band 3 Gain", -30, 30, 0)
            .modulated()
            .in("Band 3")
            .activeUnless(b3Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band3.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b3Q = continuous("Band 3 Q", 0.1, 30, 1.0, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 3")
                    .activeWhen(b3On)
                    .withKey("band3.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b3Slope =
        choice("Band 3 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 3")
            .activeWhen(b3Type, {"Low Cut", "High Cut"})
            .withKey("band3.slope")
            .help("How steeply a cut falls away.");
    Param b3Channel = choice("Band 3 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 3")
                          .activeWhen(b3On)
                          .withKey("band3.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b3Dynamic = toggle("Band 3 Dynamic", false)
                          .in("Band 3")
                          .activeWhen(b3Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band3.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b3Threshold =
        decibels("Band 3 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 3")
            .activeWhen(b3Dynamic)
            .withKey("band3.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b4On = toggle("Band 4 On", false)
                     .in("Band 4")
                     .withKey("band4.enabled")
                     .help("Whether band 4 shapes the sound; off, it passes everything unchanged.");
    Param b4Type =
        choice("Band 4 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 0)
            .in("Band 4")
            .activeWhen(b4On)
            .withKey("band4.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b4Freq = frequency("Band 4 Frequency", 20, 20000, 800)
                       .modulated()
                       .in("Band 4")
                       .activeWhen(b4On)
                       .withKey("band4.freq")
                       .help("Its centre, corner or pivot.");
    Param b4Gain =
        decibels("Band 4 Gain", -30, 30, 0)
            .modulated()
            .in("Band 4")
            .activeUnless(b4Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band4.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b4Q = continuous("Band 4 Q", 0.1, 30, 1.0, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 4")
                    .activeWhen(b4On)
                    .withKey("band4.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b4Slope =
        choice("Band 4 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 4")
            .activeWhen(b4Type, {"Low Cut", "High Cut"})
            .withKey("band4.slope")
            .help("How steeply a cut falls away.");
    Param b4Channel = choice("Band 4 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 4")
                          .activeWhen(b4On)
                          .withKey("band4.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b4Dynamic = toggle("Band 4 Dynamic", false)
                          .in("Band 4")
                          .activeWhen(b4Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band4.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b4Threshold =
        decibels("Band 4 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 4")
            .activeWhen(b4Dynamic)
            .withKey("band4.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b5On = toggle("Band 5 On", false)
                     .in("Band 5")
                     .withKey("band5.enabled")
                     .help("Whether band 5 shapes the sound; off, it passes everything unchanged.");
    Param b5Type =
        choice("Band 5 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 0)
            .in("Band 5")
            .activeWhen(b5On)
            .withKey("band5.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b5Freq = frequency("Band 5 Frequency", 20, 20000, 2500)
                       .modulated()
                       .in("Band 5")
                       .activeWhen(b5On)
                       .withKey("band5.freq")
                       .help("Its centre, corner or pivot.");
    Param b5Gain =
        decibels("Band 5 Gain", -30, 30, 0)
            .modulated()
            .in("Band 5")
            .activeUnless(b5Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band5.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b5Q = continuous("Band 5 Q", 0.1, 30, 1.0, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 5")
                    .activeWhen(b5On)
                    .withKey("band5.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b5Slope =
        choice("Band 5 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 5")
            .activeWhen(b5Type, {"Low Cut", "High Cut"})
            .withKey("band5.slope")
            .help("How steeply a cut falls away.");
    Param b5Channel = choice("Band 5 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 5")
                          .activeWhen(b5On)
                          .withKey("band5.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b5Dynamic = toggle("Band 5 Dynamic", false)
                          .in("Band 5")
                          .activeWhen(b5Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band5.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b5Threshold =
        decibels("Band 5 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 5")
            .activeWhen(b5Dynamic)
            .withKey("band5.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b6On = toggle("Band 6 On", false)
                     .in("Band 6")
                     .withKey("band6.enabled")
                     .help("Whether band 6 shapes the sound; off, it passes everything unchanged.");
    Param b6Type =
        choice("Band 6 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 0)
            .in("Band 6")
            .activeWhen(b6On)
            .withKey("band6.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b6Freq = frequency("Band 6 Frequency", 20, 20000, 6000)
                       .modulated()
                       .in("Band 6")
                       .activeWhen(b6On)
                       .withKey("band6.freq")
                       .help("Its centre, corner or pivot.");
    Param b6Gain =
        decibels("Band 6 Gain", -30, 30, 0)
            .modulated()
            .in("Band 6")
            .activeUnless(b6Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band6.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b6Q = continuous("Band 6 Q", 0.1, 30, 1.0, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 6")
                    .activeWhen(b6On)
                    .withKey("band6.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b6Slope =
        choice("Band 6 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 6")
            .activeWhen(b6Type, {"Low Cut", "High Cut"})
            .withKey("band6.slope")
            .help("How steeply a cut falls away.");
    Param b6Channel = choice("Band 6 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 6")
                          .activeWhen(b6On)
                          .withKey("band6.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b6Dynamic = toggle("Band 6 Dynamic", false)
                          .in("Band 6")
                          .activeWhen(b6Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band6.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b6Threshold =
        decibels("Band 6 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 6")
            .activeWhen(b6Dynamic)
            .withKey("band6.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b7On = toggle("Band 7 On", false)
                     .in("Band 7")
                     .withKey("band7.enabled")
                     .help("Whether band 7 shapes the sound; off, it passes everything unchanged.");
    Param b7Type =
        choice("Band 7 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 2)
            .in("Band 7")
            .activeWhen(b7On)
            .withKey("band7.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b7Freq = frequency("Band 7 Frequency", 20, 20000, 10000)
                       .modulated()
                       .in("Band 7")
                       .activeWhen(b7On)
                       .withKey("band7.freq")
                       .help("Its centre, corner or pivot.");
    Param b7Gain =
        decibels("Band 7 Gain", -30, 30, 0)
            .modulated()
            .in("Band 7")
            .activeUnless(b7Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band7.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b7Q = continuous("Band 7 Q", 0.1, 30, 0.71, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 7")
                    .activeWhen(b7On)
                    .withKey("band7.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b7Slope =
        choice("Band 7 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 7")
            .activeWhen(b7Type, {"Low Cut", "High Cut"})
            .withKey("band7.slope")
            .help("How steeply a cut falls away.");
    Param b7Channel = choice("Band 7 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 7")
                          .activeWhen(b7On)
                          .withKey("band7.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b7Dynamic = toggle("Band 7 Dynamic", false)
                          .in("Band 7")
                          .activeWhen(b7Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band7.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b7Threshold =
        decibels("Band 7 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 7")
            .activeWhen(b7Dynamic)
            .withKey("band7.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");
    Param b8On = toggle("Band 8 On", false)
                     .in("Band 8")
                     .withKey("band8.enabled")
                     .help("Whether band 8 shapes the sound; off, it passes everything unchanged.");
    Param b8Type =
        choice("Band 8 Type",
               {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Tilt"}, 4)
            .in("Band 8")
            .activeWhen(b8On)
            .withKey("band8.type")
            .help("Its shape: a bell, a shelf, a cut, a notch, or a tilt about its frequency.");
    Param b8Freq = frequency("Band 8 Frequency", 20, 20000, 18000)
                       .modulated()
                       .in("Band 8")
                       .activeWhen(b8On)
                       .withKey("band8.freq")
                       .help("Its centre, corner or pivot.");
    Param b8Gain =
        decibels("Band 8 Gain", -30, 30, 0)
            .modulated()
            .in("Band 8")
            .activeUnless(b8Type, {"Low Cut", "High Cut", "Notch"})
            .withKey("band8.gain")
            .help("Boost or cut, for bells, shelves and tilts (cuts and notches ignore it).");
    Param b8Q = continuous("Band 8 Q", 0.1, 30, 0.71, Unit::None)
                    .modulated()
                    .curved(Curve::Log)
                    .in("Band 8")
                    .activeWhen(b8On)
                    .withKey("band8.q")
                    .help("Width: higher is narrower; resonance for shelves (up to 2) and cuts.");
    Param b8Slope =
        choice("Band 8 Slope",
               {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}, 3)
            .keys({"db6", "db12", "db18", "db24", "db36", "db48"})
            .in("Band 8")
            .activeWhen(b8Type, {"Low Cut", "High Cut"})
            .withKey("band8.slope")
            .help("How steeply a cut falls away.");
    Param b8Channel = choice("Band 8 Channel", {"Stereo", "Mid", "Side", "Left", "Right"}, 0)
                          .in("Band 8")
                          .activeWhen(b8On)
                          .withKey("band8.channel")
                          .help(
                              "What it works on: both channels, the mid (centre), the side "
                              "(width), or one channel.");
    Param b8Dynamic = toggle("Band 8 Dynamic", false)
                          .in("Band 8")
                          .activeWhen(b8Type, {"Bell", "Low Shelf", "High Shelf"})
                          .withKey("band8.dynamic")
                          .help(
                              "A bell or shelf that moves toward its gain only while its region is "
                              "louder than the threshold.");
    Param b8Threshold =
        decibels("Band 8 Threshold", -60, 0, -30)
            .modulated()
            .in("Band 8")
            .activeWhen(b8Dynamic)
            .withKey("band8.threshold")
            .help("For a dynamic band: the level of its region above which it starts to act.");

    Param scale =
        percent("Gain Scale", 100, 0, 200)
            .modulated()
            .help("Scales every bell, shelf and tilt gain: 50% halves the curve, 200% doubles it.");
    Param autoGain = toggle("Auto Gain", false)
                         .help(
                             "Offsets the output by the curve's average effect, so a boost does "
                             "not just sound louder.");
    Param output = decibels("Output", -24, 24, 0)
                       .modulated()
                       .smoothed(20)
                       .help("The level out, after every band.");
    Param listen = integer("Listen", 0, 8, 0)
                       .notAutomatable()
                       .help(
                           "Plays only the part of the spectrum one band works on (1 to 8); 0 "
                           "plays everything.");

    void factoryPatches(Patches& p) {
        p.add("Flat");
        p.add("Clean Up")
            .set(b1On, true)
            .set(b1Freq, 35)
            .set(b1Slope, "db24")
            .set(b3On, true)
            .set(b3Freq, 320)
            .set(b3Gain, -2)
            .set(b3Q, 0.9);
        p.add("Vocal Polish")
            .set(b1On, true)
            .set(b1Freq, 90)
            .set(b1Slope, "db18")
            .set(b3On, true)
            .set(b3Freq, 260)
            .set(b3Gain, -2.5)
            .set(b3Q, 1.2)
            .set(b5On, true)
            .set(b5Freq, 3200)
            .set(b5Gain, 2)
            .set(b5Q, 0.8)
            .set(b6On, true)
            .set(b6Freq, 6800)
            .set(b6Gain, -6)
            .set(b6Q, 2.5)
            .set(b6Dynamic, true)
            .set(b6Threshold, -32)
            .set(b7On, true)
            .set(b7Freq, 11000)
            .set(b7Gain, 2.5)
            .set(autoGain, true);
        p.add("De-Ess")
            .set(b6On, true)
            .set(b6Freq, 6500)
            .set(b6Gain, -8)
            .set(b6Q, 2)
            .set(b6Dynamic, true)
            .set(b6Threshold, -34);
        p.add("Air and Weight")
            .set(b2On, true)
            .set(b2Freq, 85)
            .set(b2Gain, 1.5)
            .set(b7On, true)
            .set(b7Freq, 12000)
            .set(b7Gain, 2)
            .set(b7Q, 0.6)
            .set(autoGain, true);
        p.add("Mono Bass")
            .set(b1On, true)
            .set(b1Freq, 140)
            .set(b1Slope, "db12")
            .set(b1Channel, "side")
            .set(b7On, true)
            .set(b7Freq, 9000)
            .set(b7Gain, 1.5)
            .set(b7Channel, "side");
        p.add("Telephone")
            .set(b1On, true)
            .set(b1Freq, 400)
            .set(b1Slope, "db24")
            .set(b5On, true)
            .set(b5Freq, 1800)
            .set(b5Gain, 4)
            .set(b5Q, 0.7)
            .set(b8On, true)
            .set(b8Freq, 3400)
            .set(b8Slope, "db24")
            .set(output, -3);
    }

    // --- audio ------------------------------------------------------------------------

    Array<Setting, 8> want;
    Array<Band, 8> band;
    double rate = 48000.0;
    double gainScale = 1.0;
    double autoTarget = 0.0, autoNow = 0.0;
    double gainDb = 1e9, gainLinear = 1.0;
    double wetStep = 0.001, glide = 0.05, quick = 0.2, slow = 0.01, fall = 0.999;
    double attackMs = 3.0;
    int tick = 0;
    bool unplayed = true;  // no audio since prepare() (or a reset): changes snap
    int listenBand = 0;    // 1..8, 0 for none
    double listenMix = 0.0;
    double listenHz = 0.0, listenQ = 0.0;
    int listenKind = -1;
    Biquad hear0, hear1;
    bool hearDirty = false;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0;
    int pos = 0;
    int total = 0;

    // What the editor sees.
    SharedArray<float, 32768> ring;  // the output, mono, the newest before `head`
    Shared<int> head;
    Shared<int> written;
    Shared<double> sampleRateNow;
    Shared<double> inLeft, inRight, outLeft, outRight;  // peaks, dB
    SharedArray<float, 8> liveDb;                       // each band's gain as heard
    SharedArray<float, 8> levelDb;                      // each dynamic band's detector
    Shared<double> autoDb;

    void prepare() {
        rate = sampleRate();
        wetStep = 1.0 / (0.012 * rate);
        glide = 1.0 - exp(-16.0 / (0.025 * rate));
        quick = 1.0 - exp(-16.0 / (0.004 * rate));
        slow = 1.0 - exp(-16.0 / (0.15 * rate));
        fall = exp(-1.0 / (0.3 * rate));
        unplayed = true;
        tick = 0;
    }

    void update() {
        loadAll(want);
        gainScale = scale;
        listenBand = listen.index();
        autoTarget = autoGain.on() ? compensation() : 0.0;
        // Until audio has played there is nothing to glide or fade from, so
        // what the next block plays depends only on the values now.
        if (unplayed) {
            for (int b = 0; b < 8; ++b) adopt(band[b], want[b], true);
            autoNow = autoTarget;
            listenMix = listenBand > 0 ? 1.0 : 0.0;
        }
    }

    // The gain that cancels the curve's average effect on a typical mix:
    // its power gain averaged across the spectrum, weighted toward the
    // middle where loudness is judged. Dynamic bands count as resting.
    double compensation() {
        Array<Shape, 8> curve;
        for (int b = 0; b < 8; ++b) {
            const Setting& s = want[b];
            if (!s.on) continue;
            double db = hasGain(s.kind) ? s.db * gainScale : 0.0;
            if (s.dynamic && canBeDynamic(s.kind)) db = 0.0;
            const int count = design(curve[b].sec, s.kind, s.hz, db, s.q, s.order, rate);
            curve[b].count = count;
        }
        double weight = 0.0, sum = 0.0;
        for (int k = 0; k < 40; ++k) {
            const double hz = 30.0 * pow(16000.0 / 30.0, k / 39.0);
            const double w =
                1.0 / ((1.0 + (90.0 / hz) * (90.0 / hz)) * (1.0 + (hz / 7000.0) * (hz / 7000.0)));
            const double sn = sin(pi * hz / rate);
            const double p1 = sn * sn;
            double gain = 1.0;
            for (int b = 0; b < 8; ++b) {
                for (int j = 0; j < curve[b].count; ++j) gain *= power(curve[b].sec[j], p1);
            }
            weight += w;
            sum += w * gain;
        }
        return clamp(-10.0 * log10(max(sum / weight, 1e-12)), -18.0, 18.0);
    }

    // Takes a band's structure from its setting: at once (on the first
    // update, or once it has faded out), its memory cleared.
    void adopt(Band& d, const Setting& s, bool snap) {
        d.on = s.on;
        d.kind = s.kind;
        d.order = s.order;
        d.place = s.place;
        d.hz = s.hz;
        d.q = s.q;
        d.db = hasGain(s.kind) ? s.db * gainScale : 0.0;
        d.now = s.dynamic && canBeDynamic(s.kind) ? 0.0 : d.db;
        d.designedDb = 1e9;
        for (int k = 0; k < 4; ++k) {
            d.m0[k].z1 = 0.0;
            d.m0[k].z2 = 0.0;
            d.m1[k].z1 = 0.0;
            d.m1[k].z2 = 0.0;
        }
        d.det0.clear();
        d.det1.clear();
        d.env.clear();
        d.detKind = -1;
        d.level = -120.0;
        if (snap) d.wet = d.on ? 1.0 : 0.0;
        d.fade = d.on ? 1.0 : -1.0;
        const int count = design(d.sec, d.kind, d.hz, d.now, d.q, d.order, rate);
        d.count = count;
        d.designedHz = d.hz;
        d.designedDb = d.now;
        d.designedQ = d.q;
    }

    // Every 16 samples: structures, glides, dynamics and coefficients.
    void control() {
        for (int b = 0; b < 8; ++b) {
            Band& d = band[b];
            const Setting& s = want[b];
            const bool changed =
                s.on != d.on || s.kind != d.kind || s.order != d.order || s.place != d.place;
            if (changed) {
                if (d.wet <= 0.0) {
                    adopt(d, s, false);
                } else {
                    d.fade = -1.0;
                    continue;
                }
            }
            d.fade = d.on ? 1.0 : -1.0;
            if (!d.on && d.wet <= 0.0) continue;

            d.hz = exp(log(d.hz) + (log(s.hz) - log(d.hz)) * glide);
            d.q = exp(log(d.q) + (log(s.q) - log(d.q)) * glide);
            const double target = hasGain(d.kind) ? s.db * gainScale : 0.0;
            d.db += (target - d.db) * glide;
            if (abs(d.db - target) < 1e-4) d.db = target;

            const bool dynamic = s.dynamic && canBeDynamic(d.kind);
            double goal = d.db;
            if (dynamic) {
                listenDetector(d);
                d.level = gainToDb(d.env.level());
                const double move = min(abs(d.db), 0.75 * knee(d.level - s.threshold, 6.0));
                goal = d.db < 0.0 ? -move : move;
            }
            d.now += (goal - d.now) * (dynamic ? quick : glide);
            if (abs(goal - d.now) < 1e-4) d.now = goal;

            if (abs(d.hz - d.designedHz) > 1e-6 * d.hz || abs(d.now - d.designedDb) > 1e-5 ||
                abs(d.q - d.designedQ) > 1e-6 * d.q) {
                const int count = design(d.sec, d.kind, d.hz, d.now, d.q, d.order, rate);
                d.count = count;
                d.designedHz = d.hz;
                d.designedDb = d.now;
                d.designedQ = d.q;
            }
            liveDb[b] = static_cast<float>(d.now);
            levelDb[b] = static_cast<float>(dynamic ? d.level : -120.0);
        }
        autoNow += (autoTarget - autoNow) * slow;

        // The band being listened to: a band-pass over its region.
        if (listenBand > 0) {
            const Band& d = band[listenBand - 1];
            if (d.kind != listenKind || abs(d.hz - listenHz) > 1e-6 * d.hz ||
                abs(d.q - listenQ) > 1e-6) {
                const double hz = clamp(d.hz, 20.0, 0.45 * rate);
                if (d.kind == kLowShelf || d.kind == kLowCut) {
                    hear0.lowpass(hz, 0.707);
                    hear1.lowpass(hz, 0.707);
                } else if (d.kind == kHighShelf || d.kind == kHighCut) {
                    hear0.highpass(hz, 0.707);
                    hear1.highpass(hz, 0.707);
                } else {
                    hear0.bandpass(hz, clamp(d.q, 0.7, 30.0));
                    hear1.bandpass(hz, clamp(d.q, 0.7, 30.0));
                }
                listenKind = d.kind;
                listenHz = d.hz;
                listenQ = d.q;
            }
        }
    }

    // A dynamic band's detector follows the band: the region a bell
    // touches, or everything a shelf lifts or lowers.
    void listenDetector(Band& d) {
        if (d.detKind == d.kind && abs(d.detHz - d.hz) < 1e-3 * d.hz &&
            abs(d.detQ - d.q) < 1e-3 * d.q)
            return;
        const double hz = clamp(d.hz, 20.0, 0.45 * rate);
        if (d.kind == kLowShelf) {
            d.det0.lowpass(hz, 0.707);
            d.det1.lowpass(hz, 0.707);
        } else if (d.kind == kHighShelf) {
            d.det0.highpass(hz, 0.707);
            d.det1.highpass(hz, 0.707);
        } else {
            d.det0.bandpass(hz, clamp(d.q, 0.5, 20.0));
            d.det1.bandpass(hz, clamp(d.q, 0.5, 20.0));
        }
        const double release = 90.0 * max(1.0, 200.0 / hz);
        d.env.set(attackMs * max(1.0, 100.0 / hz), release);
        d.detKind = d.kind;
        d.detHz = d.hz;
        d.detQ = d.q;
    }

    void runBand(Band& d, bool dynamic, double& l, double& r, bool stereo) {
        d.wet = clamp(d.wet + d.fade * wetStep, 0.0, 1.0);
        if (d.wet <= 0.0) return;
        const double wet = d.wet;
        if (!stereo) {
            if (d.place == kSide) return;
            if (dynamic && d.detKind >= 0) d.env.process(d.det0.process(l));
            l += wet * (chain(d.sec, d.count, d.m0, l) - l);
            return;
        }
        if (d.place == kStereo) {
            if (dynamic && d.detKind >= 0) {
                const double kl = abs(d.det0.process(l));
                const double kr = abs(d.det1.process(r));
                d.env.process(max(kl, kr));
            }
            l += wet * (chain(d.sec, d.count, d.m0, l) - l);
            r += wet * (chain(d.sec, d.count, d.m1, r) - r);
        } else if (d.place == kMid || d.place == kSide) {
            const double m = 0.5 * (l + r), s = 0.5 * (l - r);
            if (d.place == kMid) {
                if (dynamic && d.detKind >= 0) d.env.process(d.det0.process(m));
                const double m2 = m + wet * (chain(d.sec, d.count, d.m0, m) - m);
                l = m2 + s;
                r = m2 - s;
            } else {
                if (dynamic && d.detKind >= 0) d.env.process(d.det0.process(s));
                const double s2 = s + wet * (chain(d.sec, d.count, d.m0, s) - s);
                l = m + s2;
                r = m - s2;
            }
        } else if (d.place == kLeft) {
            if (dynamic && d.detKind >= 0) d.env.process(d.det0.process(l));
            l += wet * (chain(d.sec, d.count, d.m0, l) - l);
        } else {
            if (dynamic && d.detKind >= 0) d.env.process(d.det0.process(r));
            r += wet * (chain(d.sec, d.count, d.m1, r) - r);
        }
    }

    void process(Block& io) {
        unplayed = false;
        const bool stereo = io.channels > 1;
        for (int i = 0; i < io.frames; ++i) {
            if (tick == 0) control();
            tick = tick == 15 ? 0 : tick + 1;
            double l = io[0][i];
            double r = stereo ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            for (int b = 0; b < 8; ++b) {
                Band& d = band[b];
                if (d.wet <= 0.0 && d.fade < 0.0) continue;
                runBand(d, want[b].dynamic && canBeDynamic(d.kind), l, r, stereo);
            }
            listenMix = clamp(listenMix + (listenBand > 0 ? wetStep : -wetStep), 0.0, 1.0);
            if (listenMix > 0.0) {
                l += listenMix * (hear0.process(l) - l);
                r += listenMix * (hear1.process(r) - r);
                hearDirty = true;
            } else if (hearDirty && listenBand == 0) {
                hear0.clear();
                hear1.clear();
                hearDirty = false;
            }
            const double db = output.at(i) + autoNow;
            if (db != gainDb) {
                gainLinear = dbToGain(db);
                gainDb = db;
            }
            l *= gainLinear;
            r *= gainLinear;
            io[0][i] = l;
            if (stereo) io[1][i] = r;
            peakOut0 = max(abs(l), peakOut0 * fall);
            peakOut1 = max(abs(r), peakOut1 * fall);
            ring[pos] = static_cast<float>(stereo ? 0.5 * (l + r) : l);
            pos = pos == kN - 1 ? 0 : pos + 1;
        }
        total += io.frames;
        head.set(pos);
        written.set(total);
        sampleRateNow.set(rate);
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(stereo ? peakIn1 : peakIn0));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(stereo ? peakOut1 : peakOut0));
        autoDb.set(autoNow);
    }

    // --- parameters by band -----------------------------------------------------------

    void load(Setting& s, const Param& on, const Param& type, const Param& freq, const Param& gain,
              const Param& q, const Param& slope, const Param& channel, const Param& dynamic,
              const Param& threshold) {
        s.on = on.on();
        s.kind = type.index();
        s.hz = freq;
        s.db = gain;
        s.q = q;
        s.order = orderOf(slope.index());
        s.place = channel.index();
        s.dynamic = dynamic.on();
        s.threshold = threshold;
    }

    void loadAll(Array<Setting, 8>& out) {
        load(out[0], b1On, b1Type, b1Freq, b1Gain, b1Q, b1Slope, b1Channel, b1Dynamic, b1Threshold);
        load(out[1], b2On, b2Type, b2Freq, b2Gain, b2Q, b2Slope, b2Channel, b2Dynamic, b2Threshold);
        load(out[2], b3On, b3Type, b3Freq, b3Gain, b3Q, b3Slope, b3Channel, b3Dynamic, b3Threshold);
        load(out[3], b4On, b4Type, b4Freq, b4Gain, b4Q, b4Slope, b4Channel, b4Dynamic, b4Threshold);
        load(out[4], b5On, b5Type, b5Freq, b5Gain, b5Q, b5Slope, b5Channel, b5Dynamic, b5Threshold);
        load(out[5], b6On, b6Type, b6Freq, b6Gain, b6Q, b6Slope, b6Channel, b6Dynamic, b6Threshold);
        load(out[6], b7On, b7Type, b7Freq, b7Gain, b7Q, b7Slope, b7Channel, b7Dynamic, b7Threshold);
        load(out[7], b8On, b8Type, b8Freq, b8Gain, b8Q, b8Slope, b8Channel, b8Dynamic, b8Threshold);
    }

    // how: 0 sets a value in its unit, 1 a knob position (0..1), 2 the default.
    void act(int how, const Param& p, double v) {
        if (how == 0)
            p.edit(v);
        else if (how == 1)
            p.editNormalized(v);
        else
            p.reset();
    }

    void actOn(int how, int field, double v, const Param& on, const Param& type, const Param& freq,
               const Param& gain, const Param& q, const Param& slope, const Param& channel,
               const Param& dynamic, const Param& threshold) {
        switch (field) {
            case fOn:
                act(how, on, v);
                break;
            case fType:
                act(how, type, v);
                break;
            case fFreq:
                act(how, freq, v);
                break;
            case fGain:
                act(how, gain, v);
                break;
            case fQ:
                act(how, q, v);
                break;
            case fSlope:
                act(how, slope, v);
                break;
            case fChannel:
                act(how, channel, v);
                break;
            case fDynamic:
                act(how, dynamic, v);
                break;
            default:
                act(how, threshold, v);
                break;
        }
    }

    double knobOf(int field, const Param& freq, const Param& gain, const Param& q,
                  const Param& threshold) const {
        if (field == fFreq) return freq.normalized();
        if (field == fGain) return gain.normalized();
        if (field == fQ) return q.normalized();
        return threshold.normalized();
    }

    void change(int how, int b, int field, double v) {
        switch (b) {
            case 0:
                actOn(how, field, v, b1On, b1Type, b1Freq, b1Gain, b1Q, b1Slope, b1Channel,
                      b1Dynamic, b1Threshold);
                break;
            case 1:
                actOn(how, field, v, b2On, b2Type, b2Freq, b2Gain, b2Q, b2Slope, b2Channel,
                      b2Dynamic, b2Threshold);
                break;
            case 2:
                actOn(how, field, v, b3On, b3Type, b3Freq, b3Gain, b3Q, b3Slope, b3Channel,
                      b3Dynamic, b3Threshold);
                break;
            case 3:
                actOn(how, field, v, b4On, b4Type, b4Freq, b4Gain, b4Q, b4Slope, b4Channel,
                      b4Dynamic, b4Threshold);
                break;
            case 4:
                actOn(how, field, v, b5On, b5Type, b5Freq, b5Gain, b5Q, b5Slope, b5Channel,
                      b5Dynamic, b5Threshold);
                break;
            case 5:
                actOn(how, field, v, b6On, b6Type, b6Freq, b6Gain, b6Q, b6Slope, b6Channel,
                      b6Dynamic, b6Threshold);
                break;
            case 6:
                actOn(how, field, v, b7On, b7Type, b7Freq, b7Gain, b7Q, b7Slope, b7Channel,
                      b7Dynamic, b7Threshold);
                break;
            default:
                actOn(how, field, v, b8On, b8Type, b8Freq, b8Gain, b8Q, b8Slope, b8Channel,
                      b8Dynamic, b8Threshold);
                break;
        }
    }

    double normOf(int b, int field) const {
        switch (b) {
            case 0:
                return knobOf(field, b1Freq, b1Gain, b1Q, b1Threshold);
            case 1:
                return knobOf(field, b2Freq, b2Gain, b2Q, b2Threshold);
            case 2:
                return knobOf(field, b3Freq, b3Gain, b3Q, b3Threshold);
            case 3:
                return knobOf(field, b4Freq, b4Gain, b4Q, b4Threshold);
            case 4:
                return knobOf(field, b5Freq, b5Gain, b5Q, b5Threshold);
            case 5:
                return knobOf(field, b6Freq, b6Gain, b6Q, b6Threshold);
            case 6:
                return knobOf(field, b7Freq, b7Gain, b7Q, b7Threshold);
            default:
                return knobOf(field, b8Freq, b8Gain, b8Q, b8Threshold);
        }
    }

    // --- the editor -------------------------------------------------------------------

    // Layout, in design units.
    static constexpr double kW = 1000;
    static constexpr double kH = 624;
    static constexpr double gx = 46;
    static constexpr double gy = 62;
    static constexpr double gw = 904;
    static constexpr double gh = 372;
    static constexpr double bx = 16;  // the band panel
    static constexpr double by = 448;
    static constexpr double bw = 664;
    static constexpr double bh = 160;
    static constexpr double ox = 692;  // the output panel
    static constexpr double ow = 292;

    // The editor's own.
    Array<Setting, 8> ui;
    Array<float, 8> uiLive;
    Array<Shape, 8> shapes;
    int selected = 0;
    int hot = -1;      // the node under the pointer
    int dragging = 0;  // 0 nothing, 1 a node, 2 a knob
    int dragBand = 0;
    int dragKnob = 0;  // which knob (see knobAt)
    double vx = 0.0, vy = 0.0, knobPos = 0.0;
    bool listening = false;
    bool hovering = false;
    double hoverX = 0.0, hoverY = 0.0;
    int range = 1;  // the curve's scale: 6, 12, 24 or 30 dB
    bool analyser = true;

    // The analyser's.
    Array<double, 16384> fr, fi;
    Array<double, 8192> twr, twi;
    Array<double, 16385> spr, spi;
    Array<int, 16384> rev;
    Array<float, 32768> window;
    Array<double, 16385> bins;
    Array<double, 16386> cum;
    Array<double, 420> average, target, shown;
    bool tables = false;
    bool primed = false;
    bool haveSpectrum = false;
    int lastWritten = 0;
    int sinceTransform = 1000000;

    double eqRange() const {
        if (range == 0) return 6.0;
        if (range == 1) return 12.0;
        if (range == 2) return 24.0;
        return 30.0;
    }
    double rateNow() const { return sampleRateNow.get() > 1000.0 ? sampleRateNow.get() : 48000.0; }

    void refresh() {
        loadAll(ui);
        for (int b = 0; b < 8; ++b) {
            uiLive[b] = liveDb[b];
            const Setting& s = ui[b];
            const int n =
                design(shapes[b].sec, s.kind, s.hz, shownGain(b), s.q, s.order, rateNow());
            shapes[b].count = n;
        }
    }

    // sin^2(w/2) at hz, which power() takes.
    double halfSin2(double hz) const {
        const double sn = sin(pi * min(hz, 0.4999 * rateNow()) / rateNow());
        return sn * sn;
    }

    // The gain a band is drawn with: as heard when it is dynamic.
    double shownGain(int b) const {
        const Setting& s = ui[b];
        if (!hasGain(s.kind)) return 0.0;
        if (s.dynamic && canBeDynamic(s.kind)) return uiLive[b];
        return s.db * scale;
    }

    // One band's response in dB at hz.
    double bandDb(int b, double hz) const { return shapeDb(b, halfSin2(hz)); }
    double shapeDb(int b, double p1) const {
        double g = 1.0;
        for (int k = 0; k < shapes[b].count; ++k) g *= power(shapes[b].sec[k], p1);
        return 10.0 * log10(max(g, 1e-30));
    }

    double nodeX(int b) const { return hzToX(ui[b].hz, 20, 20000, gx, gw); }
    double nodeY(int b) const {
        const Setting& s = ui[b];
        double db = 0.0;
        if (hasGain(s.kind))
            db = s.db * scale;
        else if (isCut(s.kind))
            db = bandDb(b, s.hz);
        return dbToY(clamp(db, -eqRange(), eqRange()), -eqRange(), eqRange(), gy, gh);
    }

    void editor(Editor& e) { e.size(kW, kH); }

    // --- the analyser ---------------------------------------------------------------

    void buildTables() {
        for (int j = 0; j < 8192; ++j) {
            twr[j] = cos(twoPi * j / kM);
            twi[j] = -sin(twoPi * j / kM);
        }
        for (int k = 0; k <= kM; ++k) {
            spr[k] = cos(twoPi * k / kN);
            spi[k] = -sin(twoPi * k / kN);
        }
        for (int n = 0; n < kM; ++n) {
            int r = 0;
            int v = n;
            for (int b = 0; b < 14; ++b) {
                r = r * 2 + imod(v, 2);
                v = idiv(v, 2);
            }
            rev[n] = r;
        }
        for (int n = 0; n < kN; ++n)
            window[n] = static_cast<float>(0.5 - 0.5 * cos(twoPi * n / kN));
        tables = true;
    }

    // The newest kN samples, windowed, through the transform: each bin's power.
    void transform() {
        const int h = head.get();
        for (int n = 0; n < kM; ++n) {
            int a = h + 2 * n;
            if (a >= kN) a -= kN;
            int b = a + 1;
            if (b >= kN) b -= kN;
            const int at = rev[n];
            fr[at] = ring[a] * window[2 * n];
            fi[at] = ring[b] * window[2 * n + 1];
        }
        int step = kM / 2;
        for (int size = 2; size <= kM; size *= 2) {
            const int half = size / 2;
            for (int start = 0; start < kM; start += size) {
                int t = 0;
                for (int j = 0; j < half; ++j) {
                    const int a = start + j, b = a + half;
                    const double wr = twr[t], wi = twi[t];
                    const double xr = wr * fr[b] - wi * fi[b];
                    const double xi = wr * fi[b] + wi * fr[b];
                    fr[b] = fr[a] - xr;
                    fi[b] = fi[a] - xi;
                    fr[a] += xr;
                    fi[a] += xi;
                    t += step;
                }
            }
            step = step / 2;
        }
        // Unpack the real transform; amplitude 1 at a bin's centre reads 1.
        const double norm = 16.0 / (static_cast<double>(kN) * kN);
        cum[0] = 0.0;
        for (int k = 0; k <= kM; ++k) {
            const int a = k == kM ? 0 : k;
            const int c = k == 0 ? 0 : kM - k;
            const double zr = fr[a], zi = fi[a];
            const double cr = fr[c], ci = -fi[c];
            const double er = 0.5 * (zr + cr), ei = 0.5 * (zi + ci);
            const double dr = 0.5 * (zr - cr), di = 0.5 * (zi - ci);
            // odd half: -i (d), then by the twiddle
            const double or_ = di, oi = -dr;
            const double xr = er + spr[k] * or_ - spi[k] * oi;
            const double xi = ei + spr[k] * oi + spi[k] * or_;
            bins[k] = (xr * xr + xi * xi) * norm;
            cum[k + 1] = cum[k] + bins[k];
        }
    }

    double binDb(int k) const { return 10.0 * log10(bins[clamp(k, 0, kM)] + 1e-24); }

    // The summed power below a fractional bin position.
    double below(double x) const {
        const double c = clamp(x, 0.0, static_cast<double>(kM));
        const int k = floorToInt(c);
        return cum[k] + (c - k) * bins[min(k, kM)];
    }

    // The spectrum at kPoints frequencies: smoothed over a fraction of an
    // octave where bins are dense, interpolated smoothly where they are
    // sparse (the bass), averaged over the last half second or so (as power,
    // which steadies the bass without blurring it), and tilted.
    void measure(int elapsed) {
        const double binHz = rateNow() / kN;
        const double spread = pow(2.0, 0.5 / kSmooth) - 1.0;
        const double keep = haveSpectrum ? exp(-elapsed / (kAverage * rateNow())) : 0.0;
        for (int i = 0; i < kPoints; ++i) {
            const double hz = 20.0 * pow(1000.0, i / (kPoints - 1.0));
            const double at = hz / binHz;
            const double half = at * spread;
            double smooth = 0.0;
            if (half > 0.0)
                smooth = (below(at + half + 0.5) - below(at - half + 0.5)) / (2.0 * half);
            const int k = floorToInt(at);
            const double t = at - k;
            const double y0 = binDb(k - 1), y1 = binDb(k), y2 = binDb(k + 1), y3 = binDb(k + 2);
            const double curve =
                y1 +
                0.5 * t *
                    (y2 - y0 +
                     t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0)));
            const double mixAmount = clamp(2.0 * half - 1.0, 0.0, 1.0);
            const double now = lerp(dbToGain(2.0 * curve), smooth, mixAmount);
            average[i] = keep * average[i] + (1.0 - keep) * now;
        }
        // Rounded peaks: a short binomial kernel across neighbouring points.
        for (int i = 0; i < kPoints; ++i) {
            const double a = average[max(i - 2, 0)], b = average[max(i - 1, 0)], c = average[i];
            const double d = average[min(i + 1, kPoints - 1)], e = average[min(i + 2, kPoints - 1)];
            const double hz = 20.0 * pow(1000.0, i / (kPoints - 1.0));
            target[i] = 10.0 * log10((a + 4.0 * b + 6.0 * c + 4.0 * d + e) / 16.0 + 1e-24) +
                        kSlopeDb * log2(hz / 1000.0);
        }
        haveSpectrum = true;
    }

    // Each repaint: a new transform once enough audio has arrived, and the
    // drawn line easing toward it (rising faster than it falls).
    void analyse() {
        if (!tables) buildTables();
        const int now = written.get();
        int fresh2 = now - lastWritten;
        lastWritten = now;
        if (fresh2 < 0 || fresh2 > 10 * kN) fresh2 = kN;
        sinceTransform += fresh2;
        const int hop = max(1024, roundToInt(rateNow() * 0.04));
        if (sinceTransform >= hop && (fresh2 > 0 || !haveSpectrum)) {
            transform();
            measure(sinceTransform);
            sinceTransform = 0;
        }
        if (!haveSpectrum) return;
        const double dt = fresh2 / rateNow();
        const double up = 1.0 - exp(-dt / 0.05);
        const double down = 1.0 - exp(-dt / 0.12);
        for (int i = 0; i < kPoints; ++i) {
            if (!primed)
                shown[i] = target[i];
            else
                shown[i] += (target[i] - shown[i]) * (target[i] > shown[i] ? up : down);
        }
        primed = true;
    }

    // --- drawing --------------------------------------------------------------------

    static constexpr double kTopDb = 0.0;
    static constexpr double kFloorDb = -96.0;

    double spectrumY(int i) const {
        return dbToY(clamp(shown[i], kFloorDb, kTopDb), kFloorDb, kTopDb, gy + 6, gh - 6);
    }

    void drawSpectrum(Canvas& g) {
        if (!haveSpectrum || !analyser) return;
        g.save();
        g.clip(gx, gy, gw, gh);
        g.lineJoin(LineJoin::Round);
        g.lineCap(LineCap::Round);
        g.strokeStyle(kSpectrum);
        g.globalAlpha(0.82);
        g.lineWidth(1.6);
        g.beginPath();
        const double floorY = gy + gh;
        double x0 = 0.0, y0 = 0.0, x1 = gx, y1 = spectrumY(0), x2 = 0.0, y2 = 0.0, x3 = 0.0,
               y3 = 0.0;
        x0 = x1;
        y0 = y1;
        g.moveTo(x1, y1);
        for (int i = 0; i < kPoints - 1; ++i) {
            x2 = gx + gw * (i + 1) / (kPoints - 1.0);
            y2 = spectrumY(i + 1);
            const int j = min(i + 2, kPoints - 1);
            x3 = gx + gw * j / (kPoints - 1.0);
            y3 = spectrumY(j);
            // Catmull-Rom through the points, as Bezier segments, kept off the floor.
            const double c1x = x1 + (x2 - x0) / 6.0, c1y = min(y1 + (y2 - y0) / 6.0, floorY);
            const double c2x = x2 - (x3 - x1) / 6.0, c2y = min(y2 - (y3 - y1) / 6.0, floorY);
            g.bezierCurveTo(c1x, c1y, c2x, c2y, x2, y2);
            x0 = x1;
            y0 = y1;
            x1 = x2;
            y1 = y2;
        }
        g.stroke();
        g.restore();
    }

    void drawGrid(Canvas& g) {
        g.linearGradient(0, gy, 0, gy + gh, kGraphTop, kGraphBottom);
        g.beginPath();
        g.roundRect(gx, gy, gw, gh, 6);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
        g.save();
        g.clip(gx, gy, gw, gh);
        g.lineWidth(1);
        // Decades and their 2s and 5s.
        for (int d = 1; d <= 4; ++d) {
            const double decade = pow(10.0, d);
            for (int m = 1; m < 10; ++m) {
                const double hz = decade * m;
                if (hz < 20.0 || hz > 20000.0) continue;
                const double x = hzToX(hz, 20, 20000, gx, gw);
                g.strokeStyle(m == 1 ? kGridLit : kGrid);
                g.globalAlpha(m == 1 || m == 2 || m == 5 ? 1.0 : 0.45);
                g.beginPath();
                g.moveTo(x, gy);
                g.lineTo(x, gy + gh);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        const double r = eqRange();
        const double step = r <= 6.0 ? 2.0 : (r <= 12.0 ? 3.0 : 6.0);
        for (double db = -r; db <= r + 0.001; db += step) {
            const double y = dbToY(db, -r, r, gy, gh);
            g.strokeStyle(abs(db) < 0.001 ? kGridLit : kGrid);
            g.beginPath();
            g.moveTo(gx, y);
            g.lineTo(gx + gw, y);
            g.stroke();
        }
        g.restore();
        // Labels: frequencies along the foot, the curve's dB down the right.
        g.monoFont(9.5);
        g.fillStyle(Theme::textFaint);
        g.textBaseline(TextBaseline::Bottom);
        g.textAlign(TextAlign::Center);
        drawHzLabel(g, 50, "50");
        drawHzLabel(g, 100, "100");
        drawHzLabel(g, 200, "200");
        drawHzLabel(g, 500, "500");
        drawHzLabel(g, 1000, "1k");
        drawHzLabel(g, 2000, "2k");
        drawHzLabel(g, 5000, "5k");
        drawHzLabel(g, 10000, "10k");
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        for (double db = -r + step; db <= r - step + 0.001; db += step) {
            const double y = dbToY(db, -r, r, gy, gh);
            g.fillText(db > 0.001 ? format("+%.0f", db) : format("%.0f", db), gx + gw - 6, y - 6);
        }
    }

    void drawHzLabel(Canvas& g, double hz, const char* text) {
        g.fillText(text, hzToX(hz, 20, 20000, gx, gw), gy + gh - 4);
    }

    // The summed curve, and the selected band's own, shaded.
    void drawCurves(Canvas& g) {
        const double r = eqRange();
        const double zero = dbToY(0, -r, r, gy, gh);
        const int steps = 300;
        g.save();
        g.clip(gx, gy, gw, gh);
        // The selected band's region.
        const Setting& sel = ui[selected];
        if (sel.on) {
            // Shaded where it does something (a sliver along 0 dB would draw as a line).
            Array<double, 301> bdb;
            int first = -1, last = -1;
            for (int k = 0; k <= steps; ++k) {
                bdb[k] = bandDb(selected, xToHz(gx + gw * k / steps, 20, 20000, gx, gw));
                if (abs(bdb[k]) >= 0.05) {
                    if (first < 0) first = k;
                    last = k;
                }
            }
            if (first >= 0) {
                g.fillStyle(bandColour(selected));
                g.globalAlpha(0.16);
                g.beginPath();
                g.moveTo(gx + gw * first / steps, zero);
                for (int k = first; k <= last; ++k)
                    g.lineTo(gx + gw * k / steps,
                             dbToY(clamp(bdb[k], -r - 2, r + 2), -r, r, gy, gh));
                g.lineTo(gx + gw * last / steps, zero);
                g.closePath();
                g.fill();
            }
            g.globalAlpha(0.55);
            g.strokeStyle(bandColour(selected));
            g.lineWidth(1.2);
            g.beginPath();
            for (int k = 0; k <= steps; ++k) {
                const double x = gx + gw * k / steps;
                const double db = bandDb(selected, xToHz(x, 20, 20000, gx, gw));
                const double y = dbToY(clamp(db, -r - 2, r + 2), -r, r, gy, gh);
                if (k == 0 || abs(db) < 0.05)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        // The sum of every band that is on.
        Array<double, 301> ys;
        bool any = false;
        for (int b = 0; b < 8; ++b) any = any || ui[b].on;
        for (int k = 0; k <= steps; ++k) {
            const double x = gx + gw * k / steps;
            const double p1 = halfSin2(xToHz(x, 20, 20000, gx, gw));
            double db = 0.0;
            for (int b = 0; b < 8; ++b)
                if (ui[b].on) db += shapeDb(b, p1);
            ys[k] = dbToY(clamp(db + output, -r - 3, r + 3), -r, r, gy, gh);
        }
        g.globalAlpha(any ? 0.10 : 0.0);
        g.linearGradient(0, gy, 0, gy + gh, kCurve, kCurve);
        g.beginPath();
        g.moveTo(gx, zero);
        for (int k = 0; k <= steps; ++k) g.lineTo(gx + gw * k / steps, ys[k]);
        g.lineTo(gx + gw, zero);
        g.closePath();
        g.fill();
        g.lineJoin(LineJoin::Round);
        g.strokeStyle(Theme::accent);
        g.globalAlpha(0.18);
        g.lineWidth(6);
        g.beginPath();
        for (int k = 0; k <= steps; ++k) {
            if (k == 0)
                g.moveTo(gx, ys[k]);
            else
                g.lineTo(gx + gw * k / steps, ys[k]);
        }
        g.stroke();
        g.globalAlpha(1.0);
        g.strokeStyle(kCurve);
        g.lineWidth(2);
        g.stroke();
        g.restore();
    }

    void drawNodes(Canvas& g) {
        for (int pass = 0; pass < 2; ++pass) {
            for (int b = 0; b < 8; ++b) {
                const Setting& s = ui[b];
                if ((pass == 0) != !s.on) continue;  // switched-off nodes first, beneath
                const double x = nodeX(b), y = nodeY(b);
                const bool lit = b == hot || (dragging == 1 && dragBand == b);
                const double radius = lit ? 9.5 : 8.0;
                if (s.on && s.dynamic && canBeDynamic(s.kind)) {
                    // How far it may reach (the band at its full gain), and
                    // a ring where its gain is now.
                    const double r = eqRange();
                    g.save();
                    g.clip(gx, gy, gw, gh);
                    g.strokeStyle(bandColour(b));
                    g.globalAlpha(0.45);
                    g.lineWidth(1.2);
                    g.beginPath();
                    Array<Sec, 4> full;
                    const int n = design(full, s.kind, s.hz, s.db * scale, s.q, s.order, rateNow());
                    for (int k = 0; k <= 160; ++k) {
                        const double px = gx + gw * k / 160.0;
                        const double sn = sin(
                            pi * min(xToHz(px, 20, 20000, gx, gw), 0.4999 * rateNow()) / rateNow());
                        double p = 1.0;
                        for (int j = 0; j < n; ++j) p *= power(full[j], sn * sn);
                        const double db = 10.0 * log10(max(p, 1e-30));
                        const double py = dbToY(clamp(db, -r - 2, r + 2), -r, r, gy, gh);
                        if (k == 0 || abs(db) < 0.05)
                            g.moveTo(px, py);
                        else
                            g.lineTo(px, py);
                    }
                    g.stroke();
                    g.restore();
                    const double yNow =
                        dbToY(clamp(static_cast<double>(uiLive[b]), -r, r), -r, r, gy, gh);
                    g.globalAlpha(1.0);
                    g.strokeStyle(bandColour(b));
                    g.lineWidth(2);
                    g.beginPath();
                    g.circle(x, yNow, 4.5);
                    g.stroke();
                }
                if (b == selected) {
                    g.strokeStyle(Theme::text);
                    g.globalAlpha(s.on ? 0.9 : 0.5);
                    g.lineWidth(1.5);
                    g.beginPath();
                    g.circle(x, y, radius + 4);
                    g.stroke();
                }
                g.globalAlpha(s.on ? 1.0 : 0.55);
                g.fillStyle(s.on ? bandColour(b) : kInk);
                g.strokeStyle(bandColour(b));
                g.lineWidth(1.6);
                g.beginPath();
                g.circle(x, y, radius);
                g.fill();
                g.stroke();
                g.fillStyle(s.on ? kInk : bandColour(b));
                g.boldFont(10);
                g.textAlign(TextAlign::Center);
                g.textBaseline(TextBaseline::Middle);
                g.fillText(format("%.0f", b + 1.0), x, y + 0.5);
                // Where it works, when that is not both channels.
                if (s.place != kStereo) {
                    g.fillStyle(bandColour(b));
                    g.boldFont(8.5);
                    const double ly = y + radius + 8;
                    if (s.place == kMid) g.fillText("MID", x, ly);
                    if (s.place == kSide) g.fillText("SIDE", x, ly);
                    if (s.place == kLeft) g.fillText("L", x, ly);
                    if (s.place == kRight) g.fillText("R", x, ly);
                }
                g.globalAlpha(1.0);
            }
        }
    }

    // A note's name and octave (A4 = 440 Hz), left-aligned at x; its width.
    double drawNote(Canvas& g, int key, double x, double y) {
        g.textAlign(TextAlign::Left);
        double w = 0.0;
        switch (imod(key, 12)) {
            case 0:
                g.fillText("C", x, y);
                w = g.measureText("C");
                break;
            case 1:
                g.fillText("C#", x, y);
                w = g.measureText("C#");
                break;
            case 2:
                g.fillText("D", x, y);
                w = g.measureText("D");
                break;
            case 3:
                g.fillText("D#", x, y);
                w = g.measureText("D#");
                break;
            case 4:
                g.fillText("E", x, y);
                w = g.measureText("E");
                break;
            case 5:
                g.fillText("F", x, y);
                w = g.measureText("F");
                break;
            case 6:
                g.fillText("F#", x, y);
                w = g.measureText("F#");
                break;
            case 7:
                g.fillText("G", x, y);
                w = g.measureText("G");
                break;
            case 8:
                g.fillText("G#", x, y);
                w = g.measureText("G#");
                break;
            case 9:
                g.fillText("A", x, y);
                w = g.measureText("A");
                break;
            case 10:
                g.fillText("A#", x, y);
                w = g.measureText("A#");
                break;
            default:
                g.fillText("B", x, y);
                w = g.measureText("B");
                break;
        }
        const char* octave = format("%.0f", floor(key / 12.0) - 1.0);
        g.fillText(octave, x + w, y);
        return w + g.measureText(octave);
    }

    void drawHz(Canvas& g, double hz, double x, double y) {
        if (hz < 100.0)
            g.fillText(format("%.1f Hz", hz), x, y);
        else if (hz < 1000.0)
            g.fillText(format("%.0f Hz", hz), x, y);
        else if (hz < 10000.0)
            g.fillText(format("%.2f kHz", hz / 1000.0), x, y);
        else
            g.fillText(format("%.1f kHz", hz / 1000.0), x, y);
    }

    // The frequency and note under the pointer, and what the analyser reads there.
    void drawHover(Canvas& g) {
        if (!hovering || dragging != 0 || hot >= 0) return;
        if (!(hoverX >= gx && hoverX < gx + gw && hoverY >= gy && hoverY < gy + gh)) return;
        const double hz = xToHz(hoverX, 20, 20000, gx, gw);
        g.strokeStyle(Theme::text);
        g.globalAlpha(0.22);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(hoverX, gy);
        g.lineTo(hoverX, gy + gh);
        g.stroke();
        g.globalAlpha(1.0);
        const double midi = 69.0 + 12.0 * log2(hz / 440.0);
        const int key = roundToInt(midi);
        const double cents = (midi - key) * 100.0;
        g.monoFont(11);
        g.textBaseline(TextBaseline::Middle);
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::text);
        drawHz(g, hz, gx + 12, gy + 16);
        g.fillStyle(Theme::textMuted);
        const double w = drawNote(g, key, gx + 94, gy + 16);
        g.fillText(format("%+.0f ct", cents), gx + 102 + w, gy + 16);
    }

    // A tooltip for the node under the pointer or in hand.
    void drawNodeTip(Canvas& g) {
        int b = hot;
        if (dragging == 1) b = dragBand;
        if (b < 0) return;
        const Setting& s = ui[b];
        const double x = nodeX(b), y = nodeY(b);
        const double w = 132, h = hasGain(s.kind) || isCut(s.kind) || s.kind == kNotch ? 46 : 32;
        double tx = x + 16, ty = y - h - 10;
        if (tx + w > gx + gw - 4) tx = x - 16 - w;
        if (ty < gy + 4) ty = y + 14;
        g.fillStyle(kTip);
        g.globalAlpha(0.94);
        g.beginPath();
        g.roundRect(tx, ty, w, h, 5);
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(bandColour(b));
        g.lineWidth(1);
        g.stroke();
        g.monoFont(10.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillStyle(Theme::text);
        drawHz(g, s.hz, tx + 9, ty + 12);
        g.fillStyle(Theme::textMuted);
        g.textAlign(TextAlign::Right);
        const double midi = 69.0 + 12.0 * log2(s.hz / 440.0);
        const int key = roundToInt(midi);
        drawNote(g, key, tx + w - 36, ty + 12);
        g.textAlign(TextAlign::Left);
        if (hasGain(s.kind))
            g.fillText(format("%+.1f dB   Q %.2f", s.db * scale, s.q), tx + 9, ty + 30);
        else if (isCut(s.kind))
            g.fillText(format("%.0f dB/oct   Q %.2f", s.order * 6.0, s.q), tx + 9, ty + 30);
        else if (s.kind == kNotch)
            g.fillText(format("Q %.2f", s.q), tx + 9, ty + 30);
        if (listening && dragging == 1) {
            g.fillStyle(bandColour(b));
            g.fillText("listening", tx + 9, ty + h - 8);
        }
    }

    // A slim stereo peak meter, -60 to +6 dB.
    void drawMeter(Canvas& g, double x, double left, double right, const char* label) {
        const double top = gy, h = gh - 18;
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double mx = x + c * 7;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(mx, top, 5, h, 2.5);
            g.fill();
            const double t = clamp((db + 60.0) / 66.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(0, top + h, 0, top, kMeterLow,
                                 db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(mx, top + h * (1.0 - t), 5, h * t, 2.5);
                g.fill();
            }
        }
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(label, x + 6, gy + gh - 6);
    }

    // --- controls -------------------------------------------------------------------

    // A raised panel: lit a little from above, with a hairline edge.
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

    // The mark: a yarrow's flat-topped head, clusters of florets on stems
    // that branch from one point.
    void drawMark(Canvas& g, double cx, double cy) {
        g.strokeStyle(Theme::accent2);
        g.lineWidth(1.1);
        g.lineCap(LineCap::Round);
        g.beginPath();
        g.moveTo(cx, cy + 15);
        g.lineTo(cx, cy + 6);
        g.stroke();
        for (int k = 0; k < 5; ++k) {
            const double hx = cx + (k - 2) * 5.0, hy = cy - 4 + abs(k - 2) * abs(k - 2) * 0.7;
            g.beginPath();
            g.moveTo(cx, cy + 6);
            g.lineTo(hx, hy + 1.5);
            g.stroke();
        }
        for (int k = 0; k < 5; ++k) {
            const double hx = cx + (k - 2) * 5.0, hy = cy - 4 + abs(k - 2) * abs(k - 2) * 0.7;
            for (int j = 0; j < 4; ++j) {
                g.fillStyle(Theme::text);
                g.beginPath();
                g.circle(hx + (j - 1.5) * 1.9, hy - (j == 1 || j == 2 ? 1.7 : 0.4), 1.25);
                g.fill();
            }
        }
        g.fillStyle(Theme::accent);
        g.beginPath();
        g.circle(cx, cy - 6.3, 1.25);
        g.fill();
    }

    void pill(Canvas& g, double x, double y, double w, double h, const char* text, bool lit,
              int colour, bool enabled) {
        g.globalAlpha(enabled ? 1.0 : 0.35);
        g.fillStyle(lit ? colour : Theme::raised);
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
        g.fillText(text, x + w * 0.5, y + h * 0.5 + 0.5);
        g.globalAlpha(1.0);
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value; bipolar ones
    // light from the top.
    void dial(Canvas& g, double cx, double cy, double norm, int colour, bool bipolar, bool enabled,
              const char* label) {
        const double r = 21;
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.globalAlpha(enabled ? 1.0 : 0.35);
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(5);
        g.beginPath();
        g.arc(cx, cy, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(3.5);
        g.beginPath();
        if (at >= from)
            g.arc(cx, cy, r, from, at);
        else
            g.arc(cx, cy, r, at, from);
        g.stroke();
        g.radialGradient(cx, cy - 6, 2, r - 4, kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(cx, cy, r - 6);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(cx + cos(at) * (r - 14), cy + sin(at) * (r - 14));
        g.lineTo(cx + cos(at) * (r - 8), cy + sin(at) * (r - 8));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(label, cx, cy - r - 11);
        g.globalAlpha(1.0);
    }

    // A small picture of each band type, on a button.
    void typeIcon(Canvas& g, int kind, double x, double y, double w, double h) {
        const double l = x + 6, r = x + w - 6, m = y + h * 0.5, t = y + 5, b = y + h - 5;
        const double c = 0.5 * (l + r);
        g.beginPath();
        if (kind == kBell) {
            g.moveTo(l, m + 2);
            g.bezierCurveTo(c - 4, m + 2, c - 3, t, c, t);
            g.bezierCurveTo(c + 3, t, c + 4, m + 2, r, m + 2);
        } else if (kind == kLowShelf) {
            g.moveTo(l, t + 1);
            g.lineTo(c - 4, t + 1);
            g.bezierCurveTo(c, t + 1, c, b - 1, c + 4, b - 1);
            g.lineTo(r, b - 1);
        } else if (kind == kHighShelf) {
            g.moveTo(l, b - 1);
            g.lineTo(c - 4, b - 1);
            g.bezierCurveTo(c, b - 1, c, t + 1, c + 4, t + 1);
            g.lineTo(r, t + 1);
        } else if (kind == kLowCut) {
            g.moveTo(l + 3, b);
            g.bezierCurveTo(c - 3, t, c, t + 1, r, t + 1);
        } else if (kind == kHighCut) {
            g.moveTo(l, t + 1);
            g.bezierCurveTo(c, t + 1, c + 3, t, r - 3, b);
        } else if (kind == kNotch) {
            g.moveTo(l, t + 2);
            g.lineTo(c - 3, t + 2);
            g.lineTo(c, b);
            g.lineTo(c + 3, t + 2);
            g.lineTo(r, t + 2);
        } else {
            g.moveTo(l, b - 2);
            g.lineTo(r, t + 2);
        }
        g.stroke();
    }

    // Where the band panel's controls are.
    static constexpr double rowY = by + 12;
    static constexpr double typeX = bx + 236;
    static constexpr double chanX = bx + 494;
    static constexpr double knobY = by + 104;
    static constexpr double slopeX = bx + 262;
    static constexpr double dynX = bx + 412;

    double knobX(int k) const {
        // 0 frequency, 1 gain, 2 Q, 3 threshold, 4 gain scale, 5 output
        if (k == 0) return bx + 50;
        if (k == 1) return bx + 128;
        if (k == 2) return bx + 206;
        if (k == 3) return bx + 612;
        if (k == 4) return ox + 52;
        return ox + 132;
    }

    void drawBandPanel(Canvas& g) {
        const Setting& s = ui[selected];
        const int colour = bandColour(selected);
        panel(g, bx, by, bw, bh);

        // The band: its number, name and switch.
        g.fillStyle(s.on ? colour : kInk);
        g.strokeStyle(colour);
        g.lineWidth(1.6);
        g.beginPath();
        g.circle(bx + 26, rowY + 13, 13);
        g.fill();
        g.stroke();
        g.fillStyle(s.on ? kInk : colour);
        g.boldFont(13);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.0f", selected + 1.0), bx + 26, rowY + 13.5);
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::text);
        g.serifFont(14);
        g.fillText(format("Band %.0f", selected + 1.0), bx + 48, rowY + 6);
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(11.5);
        drawKindName(g, s.kind, bx + 48, rowY + 21);
        pill(g, bx + 118, rowY + 2, 42, 22, s.on ? "ON" : "OFF", s.on, colour, true);
        pill(g, bx + 166, rowY + 2, 54, 22, "LISTEN", listening && dragging == 3, colour, s.on);

        // Type.
        for (int k = 0; k < 7; ++k) {
            const double x = typeX + k * 35;
            const bool lit = s.kind == k;
            g.fillStyle(lit ? Theme::raised : kInk);
            g.beginPath();
            g.roundRect(x, rowY + 1, 31, 24, 5);
            g.fill();
            if (lit) {
                g.strokeStyle(colour);
                g.lineWidth(1);
                g.stroke();
            }
            g.strokeStyle(lit ? colour : Theme::textFaint);
            g.lineWidth(1.6);
            g.lineCap(LineCap::Round);
            g.lineJoin(LineJoin::Round);
            typeIcon(g, k, x, rowY + 1, 31, 24);
        }

        // Channel.
        for (int k = 0; k < 5; ++k) {
            const double x = chanX + k * 31;
            const bool lit = s.place == k;
            g.fillStyle(lit ? Theme::raised : kInk);
            g.beginPath();
            g.roundRect(x, rowY + 1, 28, 24, 5);
            g.fill();
            if (lit) {
                g.strokeStyle(colour);
                g.lineWidth(1);
                g.stroke();
            }
            g.fillStyle(lit ? colour : Theme::textFaint);
            g.boldFont(9.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            if (k == kStereo) g.fillText("ST", x + 14, rowY + 13.5);
            if (k == kMid) g.fillText("M", x + 14, rowY + 13.5);
            if (k == kSide) g.fillText("S", x + 14, rowY + 13.5);
            if (k == kLeft) g.fillText("L", x + 14, rowY + 13.5);
            if (k == kRight) g.fillText("R", x + 14, rowY + 13.5);
        }

        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(bx + 14, by + 46);
        g.lineTo(bx + bw - 14, by + 46);
        g.stroke();

        // Frequency, gain, Q.
        dial(g, knobX(0), knobY, normOf(selected, fFreq), colour, false, true, "FREQUENCY");
        dial(g, knobX(1), knobY, normOf(selected, fGain), colour, true, hasGain(s.kind), "GAIN");
        dial(g, knobX(2), knobY, normOf(selected, fQ), colour, false, true,
             s.kind == kTilt ? "SLOPE" : "Q");
        g.monoFont(10.5);
        g.fillStyle(Theme::text);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        drawHz(g, s.hz, knobX(0), knobY + 34);
        g.globalAlpha(hasGain(s.kind) ? 1.0 : 0.35);
        g.fillText(format("%+.1f dB", s.db), knobX(1), knobY + 34);
        g.globalAlpha(1.0);
        g.fillText(format("%.2f", s.q), knobX(2), knobY + 34);

        // Slope, for the cuts.
        const bool cut = isCut(s.kind);
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.fillText("SLOPE", slopeX + 56, knobY - 32);
        for (int k = 0; k < 6; ++k) {
            const double x = slopeX + imod(k, 3) * 38;
            const double y = knobY - 20 + idiv(k, 3) * 26;
            const int order = orderOf(k);
            pill(g, x, y, 34, 20,
                 order == 1
                     ? "6"
                     : (order == 2
                            ? "12"
                            : (order == 3 ? "18"
                                          : (order == 4 ? "24" : (order == 6 ? "36" : "48")))),
                 cut && s.order == order, colour, cut);
        }

        g.strokeStyle(Theme::line);
        g.beginPath();
        g.moveTo(dynX - 14, by + 60);
        g.lineTo(dynX - 14, by + bh - 14);
        g.stroke();

        // Dynamics.
        const bool can = canBeDynamic(s.kind);
        const bool dyn = s.dynamic && can;
        pill(g, dynX, knobY - 34, 84, 22, "DYNAMIC", dyn, colour, can);
        const double mx = dynX, my = knobY + 2, mw = 150;
        g.globalAlpha(dyn ? 1.0 : 0.35);
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(mx, my, mw, 8, 4);
        g.fill();
        const double level = levelDb[selected];
        if (dyn && level > -60.0) {
            g.fillStyle(level > s.threshold ? colour : kIdle);
            g.beginPath();
            g.roundRect(mx, my, mw * clamp((level + 60.0) / 60.0, 0.0, 1.0), 8, 4);
            g.fill();
        }
        const double tx = mx + mw * clamp((s.threshold + 60.0) / 60.0, 0.0, 1.0);
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(tx, my - 4);
        g.lineTo(tx, my + 12);
        g.stroke();
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::textMuted);
        g.fillText("now", mx, my + 26);
        g.fillStyle(dyn ? colour : Theme::textFaint);
        g.fillText(dyn ? format("%+.1f dB", static_cast<double>(uiLive[selected])) : "static",
                   mx + 32, my + 26);
        g.globalAlpha(1.0);
        dial(g, knobX(3), knobY, normOf(selected, fThreshold), colour, false, can, "THRESHOLD");
        g.monoFont(10.5);
        g.fillStyle(Theme::text);
        g.textAlign(TextAlign::Center);
        g.globalAlpha(can ? 1.0 : 0.35);
        g.fillText(format("%.1f dB", s.threshold), knobX(3), knobY + 34);
        g.globalAlpha(1.0);
    }

    void drawKindName(Canvas& g, int kind, double x, double y) {
        switch (kind) {
            case kBell:
                g.fillText("Bell", x, y);
                break;
            case kLowShelf:
                g.fillText("Low shelf", x, y);
                break;
            case kHighShelf:
                g.fillText("High shelf", x, y);
                break;
            case kLowCut:
                g.fillText("Low cut", x, y);
                break;
            case kHighCut:
                g.fillText("High cut", x, y);
                break;
            case kNotch:
                g.fillText("Notch", x, y);
                break;
            default:
                g.fillText("Tilt", x, y);
                break;
        }
    }

    void drawOutputPanel(Canvas& g) {
        panel(g, ox, by, ow, bh);
        g.fillStyle(Theme::text);
        g.serifFont(14);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("Output", ox + 16, rowY + 13);
        pill(g, ox + ow - 104, rowY + 2, 88, 22, "AUTO GAIN", autoGain.on(), Theme::accent, true);
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(ox + 14, by + 46);
        g.lineTo(ox + ow - 14, by + 46);
        g.stroke();
        dial(g, knobX(4), knobY, scale.normalized(), Theme::accent, false, true, "GAIN SCALE");
        dial(g, knobX(5), knobY, output.normalized(), Theme::accent, true, true, "OUTPUT");
        g.monoFont(10.5);
        g.fillStyle(Theme::text);
        g.textAlign(TextAlign::Center);
        g.fillText(format("%.0f%%", scale * 100.0), knobX(4), knobY + 34);
        g.fillText(format("%+.1f dB", output.value()), knobX(5), knobY + 34);
        // What auto gain adds, and the output's peak.
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.fillText("AUTO", ox + 196, knobY - 18);
        g.fillText("PEAK", ox + 196, knobY + 18);
        g.monoFont(11);
        g.fillStyle(autoGain.on() ? Theme::text : Theme::textFaint);
        g.fillText(format("%+.1f dB", autoGain.on() ? autoDb.get() : 0.0), ox + 196, knobY - 2);
        const double peak = max(outLeft.get(), outRight.get());
        g.fillStyle(peak > -0.1 ? Theme::alert : Theme::text);
        g.fillText(peak < -90.0 ? "-inf" : format("%.1f dB", peak), ox + 196, knobY + 34);
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 30, 29);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Yarrow", 50, 37);
        const double tw = g.measureText("Yarrow");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("equaliser", 58 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        // The bands, to pick one and see which are on.
        for (int b = 0; b < 8; ++b) {
            const double x = 336 + b * 42, y = 17;
            const Setting& s = ui[b];
            g.fillStyle(s.on ? bandColour(b) : kInk);
            g.globalAlpha(s.on ? 1.0 : 0.8);
            g.beginPath();
            g.roundRect(x, y, 36, 26, 6);
            g.fill();
            g.strokeStyle(b == selected ? Theme::text : bandColour(b));
            g.globalAlpha(b == selected ? 1.0 : (s.on ? 0.0 : 0.55));
            g.lineWidth(b == selected ? 1.6 : 1.0);
            g.stroke();
            g.globalAlpha(1.0);
            g.fillStyle(s.on ? kInk : bandColour(b));
            g.boldFont(11);
            g.textAlign(TextAlign::Center);
            g.fillText(format("%.0f", b + 1.0), x + 18, y + 13.5);
        }
        pill(g, 780, 18, 84, 24, "ANALYSER", analyser, Theme::accent, true);
        pill(g, 872, 18, 78, 24,
             range == 0 ? "±6 dB" : (range == 1 ? "±12 dB" : (range == 2 ? "±24 dB" : "±30 dB")),
             false, Theme::accent, true);
    }

    void draw(Canvas& g) {
        refresh();
        analyse();
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawGrid(g);
        drawSpectrum(g);
        drawCurves(g);
        drawNodes(g);
        drawHover(g);
        drawNodeTip(g);
        drawMeter(g, 18, inLeft.get(), inRight.get(), "IN");
        drawMeter(g, gx + gw + 18, outLeft.get(), outRight.get(), "OUT");
        drawBandPanel(g);
        drawOutputPanel(g);
    }

    // --- the pointer ----------------------------------------------------------------

    int nodeAt(double x, double y) const {
        int best = -1;
        double bestD = 13.0 * 13.0;
        for (int b = 0; b < 8; ++b) {
            const double dx = x - nodeX(b), dy = y - nodeY(b);
            double d = dx * dx + dy * dy;
            if (!ui[b].on) d += 20.0;  // a band that is on wins a tie
            if (b == selected) d -= 10.0;
            if (d < bestD) {
                bestD = d;
                best = b;
            }
        }
        return best;
    }

    // The knob under the pointer, if it does something for the selected band
    // (gain is a bell's, shelf's or tilt's; threshold a dynamic band's).
    int knobAt(double x, double y) const {
        const int kind = ui[selected].kind;
        for (int k = 0; k < 6; ++k) {
            if ((k == 1 && !hasGain(kind)) || (k == 3 && !canBeDynamic(kind))) continue;
            if ((x - knobX(k)) * (x - knobX(k)) + (y - knobY) * (y - knobY) <= 26.0 * 26.0)
                return k;
        }
        return -1;
    }

    double knobValue(int k) const {
        if (k == 4) return scale.normalized();
        if (k == 5) return output.normalized();
        return normOf(selected, k == 0 ? fFreq : (k == 1 ? fGain : (k == 2 ? fQ : fThreshold)));
    }

    void setKnob(int k, int how, double v) {
        if (k == 4)
            act(how, scale, v);
        else if (k == 5)
            act(how, output, v);
        else
            change(how, selected, k == 0 ? fFreq : (k == 1 ? fGain : (k == 2 ? fQ : fThreshold)),
                   v);
    }

    // A band moved to where the pointer is: across for frequency, up and
    // down for gain (or for Q, on a band without gain).
    void place(int b, double x, double y, double dy, bool snap, double fine) {
        const Setting& s = ui[b];
        double hz = xToHz(clamp(x, gx, gx + gw), 20, 20000, gx, gw);
        if (snap) hz = 440.0 * pow(2.0, round(12.0 * log2(hz / 440.0)) / 12.0);
        change(0, b, fFreq, hz);
        if (hasGain(s.kind)) {
            const double r = eqRange();
            const double db = yToDb(clamp(y, gy, gy + gh), -r, r, gy, gh);
            change(0, b, fGain, scale > 0.01 ? db / scale : db);
        } else {
            change(0, b, fQ, s.q * exp(-dy * fine * 0.015));
        }
    }

    // A band added where a double click lands: a cut at either end, a bell between.
    void addBand(double x, double y) {
        int free = -1;
        for (int b = 7; b >= 0; --b)
            if (!ui[b].on) free = b;
        if (free < 0) return;
        const double hz = xToHz(x, 20, 20000, gx, gw);
        const double r = eqRange();
        const double db = yToDb(y, -r, r, gy, gh);
        int kind = kBell;
        if (hz < 32.0) kind = kLowCut;
        if (hz > 15000.0) kind = kHighCut;
        change(0, free, fType, kind);
        change(0, free, fFreq, hz);
        if (kind == kBell) {
            change(0, free, fGain, scale > 0.01 ? db / scale : db);
            change(0, free, fQ, 1.0);
        }
        change(0, free, fOn, 1);
        selected = free;
        ui[free].on = true;
        ui[free].kind = kind;
        ui[free].hz = hz;
    }

    void pointer(const Pointer& p) {
        refresh();
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hovering = false;
            hot = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        hovering = true;
        const bool inGraph = p.over(gx, gy, gw, gh);
        if (a == PointerAction::Move) {
            hot = inGraph ? nodeAt(p.x(), p.y()) : -1;
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p, inGraph);
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                vx += p.dx() * fine;
                vy += p.dy() * fine;
                place(dragBand, vx, vy, p.dy(), p.ctrl(), fine);
            } else if (dragging == 2) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            }
            return;
        }
        if (a == PointerAction::Release) {
            if (listening) act(0, listen, 0);
            listening = false;
            dragging = 0;
            hot = inGraph ? nodeAt(p.x(), p.y()) : -1;
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            if (inGraph) {
                int b = nodeAt(p.x(), p.y());
                if (b < 0) b = selected;
                const Setting& s = ui[b];
                if (isCut(s.kind) && p.alt()) {
                    int slope = 0;
                    for (int k = 0; k < 6; ++k)
                        if (orderOf(k) == s.order) slope = k;
                    change(0, b, fSlope, clamp(slope + (notches > 0 ? 1 : -1), 0, 5));
                } else {
                    change(0, b, fQ, s.q * pow(2.0, notches * 0.125 * fine));
                }
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void pressed(const Pointer& p, bool inGraph) {
        const double x = p.x(), y = p.y();
        // The header: a band to pick (a double click switches it), the analyser, the scale.
        if (y < 50) {
            for (int b = 0; b < 8; ++b) {
                if (p.over(336 + b * 42, 17, 36, 26)) {
                    selected = b;
                    if (p.doubleClick()) change(0, b, fOn, ui[b].on ? 0 : 1);
                }
            }
            if (p.over(780, 18, 84, 24)) analyser = !analyser;
            if (p.over(872, 18, 78, 24)) range = range == 3 ? 0 : range + 1;
            return;
        }
        if (inGraph) {
            const int b = nodeAt(x, y);
            if (b >= 0) {
                selected = b;
                if (p.doubleClick()) {
                    change(0, b, fOn, ui[b].on ? 0 : 1);
                    return;
                }
                if (!ui[b].on) change(0, b, fOn, 1);
                if (p.alt()) {
                    act(0, listen, b + 1);
                    listening = true;
                }
                dragging = 1;
                dragBand = b;
                vx = nodeX(b);
                vy = nodeY(b);
                return;
            }
            if (p.doubleClick()) {
                addBand(x, y);
                dragging = 1;
                dragBand = selected;
                vx = x;
                vy = y;
            }
            return;
        }
        // The knobs: drag up and down; a double click returns one to its default.
        const int k = knobAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                setKnob(k, 2, 0);
                return;
            }
            dragging = 2;
            dragKnob = k;
            knobPos = knobValue(k);
            return;
        }
        const Setting& s = ui[selected];
        if (p.over(bx + 118, rowY + 2, 42, 22)) change(0, selected, fOn, s.on ? 0 : 1);
        if (p.over(bx + 166, rowY + 2, 54, 22) && s.on) {
            act(0, listen, selected + 1);
            listening = true;
            dragging = 3;
        }
        for (int t = 0; t < 7; ++t)
            if (p.over(typeX + t * 35, rowY + 1, 31, 24)) change(0, selected, fType, t);
        for (int c = 0; c < 5; ++c)
            if (p.over(chanX + c * 31, rowY + 1, 28, 24)) change(0, selected, fChannel, c);
        if (isCut(s.kind))
            for (int k2 = 0; k2 < 6; ++k2)
                if (p.over(slopeX + imod(k2, 3) * 38, knobY - 20 + idiv(k2, 3) * 26, 34, 20))
                    change(0, selected, fSlope, k2);
        if (canBeDynamic(s.kind) && p.over(dynX, knobY - 34, 84, 22))
            change(0, selected, fDynamic, s.dynamic ? 0 : 1);
        if (p.over(ox + ow - 104, rowY + 2, 88, 22)) act(0, autoGain, autoGain.on() ? 0 : 1);
    }
};
