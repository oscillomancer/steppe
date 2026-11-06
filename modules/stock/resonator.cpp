// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A modal resonator, poly: twenty-four tuned modes that ring when something
// excites them, so noise, drums, plucks or a trigger's click become bells,
// bowls, bars and strings at the pitch you play. Structure morphs the modes'
// ratios smoothly from a string (harmonic) through a free beam and a
// clamped bar (sparse, glassy) to a bell (a hum an octave down, a minor
// third) and a plate (dense, clangy). Brightness tilts the modes' levels,
// Decay is how long the lowest rings, Damping how much sooner the high ones
// die, Position where the body is struck (a node silences the modes that
// have one there), and Spread pans alternate modes apart.
//
// Each mode is a complex one-pole turning at its frequency: its decay and
// frequency can move without clicks. They are retuned at the start of each
// block of the rack's grid.

constexpr int kModes = 24;
constexpr double kC4 = 261.6255653005986;

// Mode n's frequency over the fundamental for each structure: 0 a string, 1
// a free-free beam, 2 a clamped-free bar, 3 a bell, 4 a square plate.
double stringRatio(int n) { return n + 1.0; }
double beamRatio(int n) {
    const double b = (2.0 * n + 3.0) / 3.0;
    return b * b;
}
double barRatio(int n) {
    if (n == 0) return 1.0;
    const double b = (2.0 * n + 1.0) * pi * 0.5 / 1.8751;
    return b * b;
}
double bellRatio(int n) {
    switch (n) {
        case 0:
            return 0.5;
        case 1:
            return 1.0;
        case 2:
            return 1.2;
        case 3:
            return 1.5;
        case 4:
            return 2.0;
        case 5:
            return 2.5;
        case 6:
            return 2.67;
        case 7:
            return 3.0;
        case 8:
            return 4.0;
        case 9:
            return 5.33;
        case 10:
            return 6.0;
        default:
            return 6.0 * pow(1.26, n - 10.0);
    }
}
// Distinct m^2 + n^2 for m, n >= 1, over 2.
double plateRatio(int n) {
    switch (n) {
        case 0:
            return 1.0;
        case 1:
            return 2.5;
        case 2:
            return 4.0;
        case 3:
            return 5.0;
        case 4:
            return 6.5;
        case 5:
            return 8.5;
        case 6:
            return 9.0;
        case 7:
            return 10.0;
        case 8:
            return 12.5;
        case 9:
            return 13.0;
        case 10:
            return 14.5;
        case 11:
            return 16.0;
        case 12:
            return 17.0;
        case 13:
            return 18.5;
        case 14:
            return 20.0;
        case 15:
            return 20.5;
        case 16:
            return 22.5;
        case 17:
            return 25.0;
        case 18:
            return 26.0;
        case 19:
            return 26.5;
        case 20:
            return 29.0;
        case 21:
            return 30.5;
        case 22:
            return 32.5;
        default:
            return 34.0;
    }
}
double familyRatio(int family, int n) {
    switch (family) {
        case 0:
            return stringRatio(n);
        case 1:
            return beamRatio(n);
        case 2:
            return barRatio(n);
        case 3:
            return bellRatio(n);
        default:
            return plateRatio(n);
    }
}
// Between the families, gliding in log frequency.
double ratioAt(double structure, int n) {
    const double s = clamp(structure, 0.0, 1.0) * 4.0;
    const int f = min(floorToInt(s), 3);
    const double t = s - f;
    return exp(lerp(log(familyRatio(f, n)), log(familyRatio(f + 1, n)), t));
}

struct Mode {
    double re = 0.0, im = 0.0;  // its state
    double cr = 1.0, ci = 0.0;  // its turn each sample, times its decay
    double drive = 0.0;         // what of the input it takes
    double hit = 0.0;           // what a strike gives it
    double left = 0.0, right = 0.0;
};

struct Body {
    Array<Mode, kModes> mode;
    bool high = false;
};

struct Resonator : Module {
    static constexpr ModuleInfo info = {
        .name = "Resonator",
        .id = "com.suppleaudio.rack.resonator",
        .vendor = "Supple Audio",
        .description =
            "A modal resonator: noise and drums become tuned strings, bars, bells "
            "and plates.",
        .tags = {"physical-model", "filter", "poly"}};

    Param structure =
        percent("Structure", 0)
            .help(
                "The modes' ratios: string at 0, beam at 25, bar at 50, bell at 75, plate at 100.");
    Param brightness = percent("Brightness", 55)
                           .help("The high modes' share: dark wood at 0, bright metal at 100.");
    Param decay = milliseconds("Decay", 30, 30000, 2500)
                      .help("How long the lowest mode rings, to 60 dB down.");
    Param damping =
        percent("Damping", 40)
            .help("How much sooner the high modes die than the low: 0 rings them all alike.");
    Param position = percent("Position", 25, 1, 50)
                         .help("Where it is struck: 50, the middle, silences every second mode.");
    Param spread =
        percent("Spread", 50).help("Pans alternate modes apart, left and right: 0 is mono.");
    Param level = decibels("Level", -24, 12, 0).smoothed(10).help("The output level.");

    Input in = audioIn("In").help("What excites the modes: noise, a drum, a pluck, a voice.");
    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: a body per channel.");
    Input strike = gateIn("Strike").help("A click into every mode as it rises: a mallet.");

    Output outL = audioOut("Out L").help("The modes panned left by Spread.");
    Output outR = audioOut("Out R").help("The modes panned right by Spread.");

    Array<Body, 16> body;

    // Retune channel c's modes from the values at frame 0.
    void tune(int c) {
        Body& b = body[c];
        const double sr = sampleRate();
        const double f0 = kC4 * exp2(clamp(pitch[c][0], -6.0, 5.0));
        const double st = structure.at(0, c);
        const double tilt = 1.6 * (1.0 - clamp(brightness.at(0, c), 0.0, 1.0));
        const double t60 = max(decay.at(0, c), 1.0) * 0.001;
        const double damp = clamp(damping.at(0, c), 0.0, 1.0);
        const double pos = clamp(position.at(0, c), 0.01, 0.5);
        const double sp = clamp(spread.at(0, c), 0.0, 1.0);
        double total = 0.0;
        for (int n = 0; n < kModes; ++n) {
            Mode& m = b.mode[n];
            const double ratio = ratioAt(st, n);
            const double hz = f0 * ratio;
            double amp = 0.0;
            if (hz < 0.45 * sr) amp = pow(ratio, -tilt) * abs(sin(pi * (n + 1.0) * pos));
            const double t = t60 / (1.0 + damp * 3.0 * (ratio - 1.0) / sqrt(ratio));
            const double r = pow(10.0, -3.0 / (max(t, 0.002) * sr));
            const double w = twoPi * hz / sr;
            m.cr = r * cos(w);
            m.ci = r * sin(w);
            m.hit = amp;
            m.drive = amp * sqrt(8.0 * (1.0 - r));
            const double pan = n % 2 == 0 ? -sp : sp;
            m.left = sqrt(0.5 * (1.0 - pan));
            m.right = sqrt(0.5 * (1.0 + pan));
            total += amp * amp;
        }
        const double norm = 1.0 / sqrt(max(total, 1e-6));
        for (int n = 0; n < kModes; ++n) {
            b.mode[n].hit *= norm;
            b.mode[n].drive *= norm;
        }
    }

    void process(int frames) {
        for (int c = 0; c < channels(); ++c) {
            tune(c);
            Body& b = body[c];
            for (int i = 0; i < frames; ++i) {
                const double g = strike[c][i];
                double click = 0.0;
                if (!b.high && g >= 0.5) {
                    b.high = true;
                    click = 0.7;
                } else if (b.high && g < 0.25) {
                    b.high = false;
                }
                const double x = in[c][i];
                double l = 0.0, r = 0.0;
                for (int n = 0; n < kModes; ++n) {
                    Mode& m = b.mode[n];
                    const double e = x * m.drive + click * m.hit;
                    const double re = m.cr * m.re - m.ci * m.im + e;
                    const double im = m.ci * m.re + m.cr * m.im;
                    m.re = re;
                    m.im = im;
                    l += im * m.left;
                    r += im * m.right;
                }
                const double out = dbToGain(level.at(i, c));
                outL[c][i] = l * out;
                outR[c][i] = r * out;
            }
        }
    }
};
