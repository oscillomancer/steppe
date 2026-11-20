// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Teasel: saturation and distortion, from a trace of warmth on a master to a
// folded, crushed, resonating mess.
//
// Fullers once raised the nap on woollen cloth with dried teasel heads:
// texture drawn out of a smooth surface. Each sample takes the same road:
//
//   Band      an optional slice of the spectrum to work on (Linkwitz-Riley
//             crossovers); below and above it pass clean.
//   Emphasis  a tilt before the curve and its exact inverse after, so highs
//             (or lows) are driven harder without changing the clean tone.
//   Drive     into one of twelve curves in three families: warm (soft, tape,
//             tube, transformer), biting (transistor, diode, clip, fuzz) and
//             wild (fold, rectify, crush, alias). Character changes each one
//             in its own way; bias tilts it for even harmonics. Tape,
//             tube, transformer and alias have memory: a hysteresis loop, a
//             bias that sags after loud peaks, a core that saturates the bass
//             first, a sample-and-hold.
//   Feedback  the curve's output, a pitch's period later, back into it: a
//             tuned, distorted resonance.
//   Movement  Dynamics drives the attacks harder than the sustain, or the
//             other way about; Motion, an LFO free or in time with the song,
//             moves the drive, the character, the bias or the pitch.
//   Level     auto gain holds the level of a -18 dBFS RMS signal whatever the
//             drive, at every moment (motion and dynamics included), so
//             comparisons are fair; then cuts after the curve, mix, output.
//
// All of it runs oversampled (up to 16x), the clean bands and the dry signal
// too, so mixing and band splitting never comb.
//
// It measures itself: a 100 Hz sine at -18 dBFS RMS through the curve as
// set gives the THD and the even harmonics' share (the readouts an agent
// sets a colour by), and the music's own distortion is the part of the
// curve's output that is not a scaled copy of its input.

constexpr int kStyles = 12;
constexpr double kRef = 0.12589254117941673;  // -18 dBFS RMS
constexpr double kCompLow = -36.0;            // the auto gain table: drive -36 .. 84 dB
constexpr int kCompSteps = 121;
constexpr int kNodes = 64;  // points of the Gaussian the table is measured over
constexpr int kHarm = 16;   // harmonics the probe measures
constexpr double kProbeHz = 100.0;
constexpr int kProbeCycles = 20;
constexpr int kRing = 16384;       // the spectra's rings
constexpr int kTrace = 1024;       // the transfer plot's recent samples
constexpr int kLevelBins = 48;     // and its histogram of levels, -72 to 0 dBFS
constexpr double kPivot = 1000.0;  // where the emphasis tilt turns

// Gains often hold still for many frames. Cache only exact repeats, so a
// parameter glide or modulation still gets every distinct gain it asks for.
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

// Equivalent exp form, saturated where double tanh already rounds to 1.
double equivalentTanh(double x) {
    const double a = abs(x);
    if (a >= 20.0) return x < 0.0 ? -1.0 : 1.0;
    const double e = exp(-2.0 * a);
    const double y = (1.0 - e) / (1.0 + e);
    return x < 0.0 ? -y : y;
}

// --- the curves ---------------------------------------------------------------------------
//
// Each takes the driven signal (bias added) and gives one near +-1 at full
// tilt; each has a slope of about 1 at rest, except fuzz, which has gain.

// x / (1 + |x|^p)^(1/p): a knee that sharpens as p grows.
double algebraic(double x, double p) { return x / pow(1.0 + pow(abs(x), p), 1.0 / p); }

// The gentlest: a cubic (only the third harmonic, until it flattens at 1),
// or with character a quintic that stays clean longer and then turns over.
double soft(double x, double c) {
    const double a = abs(x);
    const double cubic = a < 1.5 ? a - a * a * a * 4.0 / 27.0 : 1.0;
    const double quintic = a < 1.25 ? a - a * a * a * a * a * 0.08192 : 1.0;
    return sign(x) * lerp(cubic, quintic, c);
}

// A triode: the grid side (positive) compresses early and softly, the
// cutoff side later and harder; second harmonic first.
double triode(double u) {
    if (u >= 0.0) return u / sqrt(1.0 + u * u);
    return algebraic(u / 1.4, 4.0) * 1.4;
}

// A transformer core's curve: never quite flat, so the bass thins rather
// than vanishes when it saturates.
double core(double v) { return 0.35 * v + 0.65 * algebraic(v, 2.5); }

// Triangle folding: slope 1, turning back at +-1.
double triangle(double u) {
    const double t = u + 1.0;
    const double m = t - 4.0 * floor(t * 0.25);
    return m < 2.0 ? m - 1.0 : 3.0 - m;
}

// Crush's bit depth for a character: 16 bits to 1.
double crushBits(double c) { return 1.0 + 15.0 * pow(1.0 - clamp(c, 0.0, 1.0), 1.5); }

// Alias's sample rate for a character: 20 kHz down to 250 Hz.
double aliasHz(double c) { return 20000.0 * pow(0.0125, clamp(c, 0.0, 1.0)); }

// Transformer's core corner for a character: 40 Hz to 400 Hz.
double coreHz(double c) { return 40.0 * pow(10.0, clamp(c, 0.0, 1.0)); }

double transistor(double u, double p) {
    return u >= 0.0 ? algebraic(u, p) : 0.92 * algebraic(u / 0.92, p);
}

double crush(double u, double levels) { return round(tanh(u) * levels) / levels; }

// The curve without memory: the static ones as they are; tape, tube and
// alias without theirs; transformer as its core at the corner frequency.
double curve(int style, double u, double c) {
    switch (style) {
        case 0:
            return soft(u, c);
        case 1:
            return tanh(u);
        case 2:
            return triode(u);
        case 3:
            return core(u);
        case 4: {
            const double p = 2.0 + 10.0 * pow(clamp(c, 0.0, 1.0), 1.5);
            return transistor(u, p);
        }
        case 5: {
            const double t = 1.0 + 2.0 * c;
            return u >= 0.0 ? 1.0 - exp(-u) : -t * (1.0 - exp(u / t));
        }
        case 6: {
            const double k = 0.5 * clamp(c, 0.0, 1.0), a = abs(u);
            if (a <= 1.0 - k) return u;
            if (a >= 1.0 + k) return sign(u);
            const double d = a - (1.0 - k);
            return sign(u) * (a - d * d / (4.0 * k));
        }
        case 7: {
            const double d = 0.04 * c;
            const double g = sign(u) * max(0.0, abs(u) - d);
            return g >= 0.0 ? tanh(4.0 * g) : 0.85 * tanh(4.0 * g / 0.85);
        }
        case 8:
            return lerp(sin(u), triangle(u), c);
        case 9: {
            const double v = u >= 0.0 ? u : -c * u;
            return 1.5 * tanh(v / 1.5);
        }
        case 10: {
            const double levels = exp2(crushBits(c) - 1.0);
            return crush(u, levels);
        }
        default:
            return tanh(u);
    }
}

// A curve with its memory, one per channel (and one for the probe), at the
// rate it runs.
struct Shaper {
    double rate = 48000.0;
    double dirStep = 0.1;  // tape: how fast the direction it remembers turns
    double dir = 0.0, last = 0.0;
    bool fresh = true;
    double chargeKeep = 0.999;  // tube: the grid's charge, and how it leaks away
    double charge = 0.0;
    double leak = 0.9999;  // transformer: the core's flux, and its error a sample back
    double flux = 0.0, err = 0.0;
    double phase = 1.0, held = 0.0;  // alias: the sample held
    double atCore = -1.0, atBias = -1.0, atAlias = -1.0;
    double beta = 1.0, coreBias = 0.0, holdStep = 1.0;
    double atHardness = -1.0, atBits = -1.0, hardness = 2.0, levels = 1.0;

    void prepare(double r) {
        rate = r;
        dirStep = 1.0 - exp(-1.0 / (0.0002 * r));
        chargeKeep = exp(-1.0 / (0.05 * r));
        leak = 1.0 - twoPi * 4.0 / r;
        clear();
    }
    void clear() {
        dir = 0.0;
        last = 0.0;
        fresh = true;
        charge = 0.0;
        flux = 0.0;
        err = 0.0;
        phase = 1.0;
        held = 0.0;
        atCore = -1.0;
        atBias = -1.0;
        atAlias = -1.0;
        atHardness = -1.0;
        atBits = -1.0;
    }

    double process(int style, double x, double c, double b) {
        if (style == 1) {
            // Tape: the curve shifts against the direction the signal moves,
            // near saturation only, so a loud signal draws a loop.
            const double u = x + b;
            if (fresh) last = u;
            fresh = false;
            const double s = u > last ? 1.0 : (u < last ? -1.0 : 0.0);
            last = u;
            dir += dirStep * (s - dir);
            const double t = equivalentTanh(u);
            return equivalentTanh(u - 0.35 * c * dir * t * t * t * t);
        }
        if (style == 2) {
            // Tube: peaks past the grid charge it, and the charge biases the
            // tube colder until it leaks away (sag, blocking at the extreme).
            const double u = x + b - 2.0 * c * charge;
            charge = charge * chargeKeep + (1.0 - chargeKeep) * 4.0 * max(0.0, u - 0.3);
            return triode(u);
        }
        if (style == 3) {
            // Transformer: the core's flux is the signal integrated, so the
            // bass saturates first; the output is the input less the
            // derivative of what saturation took from the flux. Bias is a
            // standing flux (direct current through the winding).
            // These depend on the controls, not on the signal. A glide or
            // motion still updates them at every distinct character/bias.
            if (c != atCore) {
                atCore = c;
                beta = twoPi * coreHz(c) / rate;
            }
            if (b != atBias) {
                atBias = b;
                coreBias = core(b);
            }
            flux = leak * flux + beta * x;
            const double e = core(flux + b) - flux - coreBias;
            const double y = x + (e - leak * err) / beta;
            err = e;
            return y;
        }
        if (style == 11) {
            // Alias: held at a lower rate, with no filter before it.
            if (c != atAlias) {
                atAlias = c;
                holdStep = aliasHz(c) / rate;
            }
            phase += holdStep;
            if (phase >= 1.0) {
                phase -= floor(phase);
                held = tanh(x + b);
            }
            return held;
        }
        if (style == 4) {
            if (c != atHardness) {
                atHardness = c;
                hardness = 2.0 + 10.0 * pow(clamp(c, 0.0, 1.0), 1.5);
            }
            return transistor(x + b, hardness);
        }
        if (style == 10) {
            if (c != atBits) {
                atBits = c;
                levels = exp2(crushBits(c) - 1.0);
            }
            return crush(x + b, levels);
        }
        return curve(style, x + b, c);
    }
};

// The emphasis: a first-order tilt about the pivot, lows down and highs up
// by half the tilt each, or its exact inverse.
struct Tilt {
    double b0 = 1.0, b1 = 0.0, a1 = 0.0, z = 0.0;

    void design(double db, double hz, double rate, bool inverse) {
        const double t = tan(pi * min(hz, 0.45 * rate) / rate);
        double r = sqrt(dbToGain(db));
        if (inverse) r = 1.0 / r;
        const double nz = r / t, np = 1.0 / (r * t);
        const double g = 1.0 / r;
        b0 = g * (nz + 1.0) / (np + 1.0);
        b1 = g * (1.0 - nz) / (np + 1.0);
        a1 = (1.0 - np) / (np + 1.0);
    }
    double process(double x) {
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }
    void clear() { z = 0.0; }
};

// The oversampled path of one channel.
struct Lane {
    Biquad lowA, lowB, restA, restB;  // the low crossover: low-pass, high-pass, twice each
    Biquad midA, midB, highA, highB;  // the high one
    Biquad lowAllpass;                // the low band's phase, matched to the high crossover
    Tilt pre, post;
    OnePole dc;
    Biquad lowCut, highCut;
    OnePole loopLow, loopDc;  // the feedback loop's filters
    Shaper now, was;          // the curve, and the one before a change of style
    void clear() {
        lowA.clear();
        lowB.clear();
        restA.clear();
        restB.clear();
        midA.clear();
        midB.clear();
        highA.clear();
        highB.clear();
        lowAllpass.clear();
        pre.clear();
        post.clear();
        dc.clear();
        lowCut.clear();
        highCut.clear();
        loopLow.clear();
        loopDc.clear();
        now.clear();
        was.clear();
    }
};

// A smooth random value per cycle, the same for a cycle wherever it plays:
// squares modulo a prime, exact in double.
double hashUnit(int n) {
    double x = wrap(n, 65521) + 17.0;
    for (int k = 0; k < 4; ++k) x = fmod(x * x + 12345.0 + k, 65521.0);
    return x / 32760.0 - 1.0;
}

// The LFO's shapes, from its phase (0..1) and the cycle it is in: -1..1.
double lfoShape(int shape, double ph, int cycle) {
    switch (shape) {
        case 0:
            return sin(twoPi * ph);
        case 1:
            return ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0);
        case 2:
            return 2.0 * ph - 1.0;
        case 3:
            return 1.0 - 2.0 * ph;
        case 4: {
            // A square with its edges rounded over 2% of the cycle.
            const double e = 0.02;
            if (ph < e) return sin(0.5 * pi * ph / e);
            if (ph < 0.5 - e) return 1.0;
            if (ph < 0.5 + e) return cos(0.5 * pi * (ph - 0.5 + e) / e);
            if (ph < 1.0 - e) return -1.0;
            return -cos(0.5 * pi * (ph - 1.0 + e) / e);
        }
        case 5:
            return hashUnit(cycle);
        default: {
            const double a = hashUnit(cycle), b = hashUnit(cycle + 1);
            return a + (b - a) * (0.5 - 0.5 * cos(pi * ph));
        }
    }
}

// Quarter notes per cycle of each synced rate (0: free).
double syncBeats(int k, double barBeats) {
    switch (k) {
        case 1:
            return 4.0 * barBeats;
        case 2:
            return 2.0 * barBeats;
        case 3:
            return barBeats;
        case 4:
            return 2.0;
        case 5:
            return 1.0;
        case 6:
            return 0.5;
        case 7:
            return 0.25;
        case 8:
            return 0.125;
        case 9:
            return 2.0 / 3.0;
        case 10:
            return 1.0 / 3.0;
        case 11:
            return 1.0 / 6.0;
        case 12:
            return 1.5;
        case 13:
            return 0.75;
        default:
            return 1.0;
    }
}

// --- the look --------------------------------------------------------------------------

constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;
constexpr int kGraphBottom = 0x130F0C;
constexpr int kGrid = 0x2B231D;
constexpr int kGridLit = 0x3E332A;
constexpr int kLinen = 0xE3D5BE;
constexpr int kCurve = 0xF7EEDF;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kWarm = 0xE2B866;  // the warm curves: straw
constexpr int kBite = 0xE0805A;  // the biting ones: embers
constexpr int kWild = 0xB9A0DA;  // the wild ones: the teasel's own lilac
constexpr int kHead = 0x8E6E4A;  // a dried head
constexpr int kStem = 0x7E8A5A;

// The analyser: a 16384-point transform of the input and of the output.
constexpr int kN = 16384;
constexpr int kM = 8192;
constexpr int kPoints = 360;
constexpr double kSmooth = 12.0;   // a twelfth of an octave
constexpr double kSlopeDb = 4.5;   // dB per octave, about 1 kHz
constexpr double kAverage = 0.35;  // seconds of power averaging

struct Teasel : Effect {
    static constexpr Info info = {
        .name = "Teasel",
        .id = "com.suppleaudio.teasel",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Saturation and distortion: twelve curves from subtle warmth to folding and "
            "crushing, band focus, tuned feedback, dynamics and an LFO on the drive, auto gain, "
            "and readouts of the distortion it adds.",
        .features = {"distortion"},
        // A ring lets go a second into silence: 1.25 s at 192 kHz.
        .tail = 240000};

    Param style = choice("Style",
                         {"Soft", "Tape", "Tube", "Transformer", "Transistor", "Diode", "Clip",
                          "Fuzz", "Fold", "Rectify", "Crush", "Alias"},
                         1)
                      .in("Drive")
                      .help(
                          "The curve: soft, tape, tube, transformer (warm); transistor, diode, "
                          "clip, fuzz (biting); fold, rectify, crush, alias (wild).");
    Param drive = decibels("Drive", -12, 48, 6)
                      .modulated()
                      .smoothed(20)
                      .in("Drive")
                      .help("Gain into the curve; with auto gain the level out stays put.");
    Param character =
        percent("Character", 30)
            .modulated()
            .smoothed(20)
            .in("Drive")
            .help(
                "Each curve's own: soft's knee, tape's wear, tube's sag, transformer's core "
                "(40 to 400 Hz), hardness, diode asymmetry, clip's knee, fuzz starve, fold's "
                "edge, rectify's full-wave share, crush's bits, alias's rate.");
    Param bias = percent("Bias", 0, -100, 100)
                     .modulated()
                     .smoothed(20)
                     .in("Drive")
                     .help("Tilts the curve off centre for even harmonics (the second first).");
    Param autoGain = toggle("Auto Gain", true)
                         .in("Drive")
                         .help(
                             "Holds the level of a -18 dBFS RMS signal whatever the drive, so "
                             "louder never passes for better.");
    Param dynamics =
        percent("Dynamics", 0, -100, 100)
            .modulated()
            .smoothed(20)
            .in("Movement")
            .help(
                "Positive drives the attacks harder than the sustain; negative the sustain and "
                "tails, so attacks stay clean. 100% moves the drive a dB per dB.");
    Param motion = percent("Motion", 0)
                       .modulated()
                       .smoothed(20)
                       .in("Movement")
                       .help(
                           "The LFO's depth: at 100%, 24 dB of drive, half the character, all "
                           "the bias or two octaves of pitch.");
    Param motionSync =
        choice("Motion Sync",
               {"Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T",
                "1/8 T", "1/16 T", "1/4 D", "1/8 D"},
               0)
            .keys({"free", "bars4", "bars2", "bar1", "n2", "n4", "n8", "n16", "n32", "n4t", "n8t",
                   "n16t", "n4d", "n8d"})
            .in("Movement")
            .activeAbove(motion, 0)
            .help("The LFO's cycle in note values, locked to the song while it plays.");
    Param motionRate = frequency("Motion Rate", 0.05, 20, 1)
                           .modulated()
                           .in("Movement")
                           .activeWhen(motionSync, {"Free"})
                           .help("The LFO's rate in Hz, when its sync is free.");
    Param motionShape =
        choice("Motion Shape",
               {"Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "Random", "Drift"}, 0)
            .in("Movement")
            .activeAbove(motion, 0)
            .help("The LFO's wave; random holds a value each cycle, drift glides between them.");
    Param motionTarget = choice("Motion Target", {"Drive", "Character", "Bias", "Pitch"}, 0)
                             .in("Movement")
                             .activeAbove(motion, 0)
                             .help("What the LFO moves; pitch is the feedback's.");
    Param feedback =
        percent("Feedback", 0, -95, 95)
            .modulated()
            .smoothed(20)
            .in("Feedback")
            .help(
                "The curve's output fed back into it a period of the pitch later: a tuned, "
                "distorted ring; negative rings an octave lower, odd harmonics only.");
    Param feedbackPitch = frequency("Feedback Pitch", 30, 4000, 220)
                              .modulated()
                              .smoothed(20)
                              .in("Feedback")
                              .help("The note the feedback rings at.");
    Param bandLow = frequency("Band Low", 20, 20000, 20)
                        .modulated()
                        .smoothed(30)
                        .in("Band")
                        .help("Below this passes clean (20 Hz: nothing does).");
    Param bandHigh = frequency("Band High", 20, 20000, 20000)
                         .modulated()
                         .smoothed(30)
                         .in("Band")
                         .help("Above this passes clean (20 kHz: nothing does).");
    Param emphasis =
        decibels("Emphasis", -12, 12, 0)
            .modulated()
            .smoothed(30)
            .in("Tone")
            .help(
                "Tilts what the curve hears about 1 kHz and tilts back after it: positive drives "
                "the highs harder, negative the lows, the clean tone unchanged.");
    Param lowCut = frequency("Low Cut", 20, 2000, 20)
                       .modulated()
                       .smoothed(30)
                       .in("Tone")
                       .help("A high-pass on what the curve gives (20 Hz is off).");
    Param highCut = frequency("High Cut", 1000, 20000, 20000)
                        .modulated()
                        .smoothed(30)
                        .in("Tone")
                        .help("A low-pass on what the curve gives, to tame fizz (20 kHz is off).");
    Param mix =
        percent("Mix", 100).modulated().smoothed(20).in("Output").help("Dry and driven, blended.");
    Param output =
        decibels("Output", -24, 24, 0).modulated().smoothed(20).in("Output").help("The level out.");
    Param channelMode =
        choice("Channels", {"Stereo", "Mid", "Side"}, 0)
            .withKey("channels")
            .in("Output")
            .help("Drives both channels, or only the mid (centre) or the side (width).");
    Param quality =
        choice("Quality", {"1x", "2x", "4x", "8x", "16x"}, 2)
            .keys({"x1", "x2", "x4", "x8", "x16"})
            .restarts()
            .in("Output")
            .help(
                "Oversampling: higher keeps harmonics above hearing from folding back down "
                "(up to 768 kHz inside); adds a few samples of latency.");
    Param phase = choice("Phase", {"Minimum", "Linear"}, 0)
                      .restarts()
                      .in("Output")
                      .activeUnless(quality, {"1x"})
                      .help(
                          "The oversampling filters: minimum adds 3 to 5 samples of latency and "
                          "turns the top octave's phase a little; linear turns no phase at all, "
                          "for about 70 samples of latency.");
    Param listen = choice("Listen", {"Off", "Band", "Delta"}, 0)
                       .notAutomatable()
                       .in("Output")
                       .help("Plays the driven band alone, or only what the drive changes.");

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
                "What the curve adds to the music now: the share of its output (RMS) that is "
                "not a scaled copy of its input, over the last 300 ms.");
    Param thd = percent("THD", 0).withKey("thd").meter().help(
        "Harmonics and noise as a share of the output (RMS), for a 100 Hz sine at "
        "-18 dBFS RMS through the curve as set.");
    Param evenShare = percent("Even Harmonics", 0)
                          .withKey("even")
                          .meter()
                          .help("The even harmonics' share of that sine's harmonic power.");
    Param driveNow = decibels("Drive Now", -36, 84, 0)
                         .meter()
                         .help("The drive this moment, with dynamics and motion.");

    void factoryPatches(Patches& p) {
        p.add("Warm Start");
        p.add("Bus Tape").set(style, "tape").set(drive, 4).set(character, 25);
        p.add("Master Sheen")
            .set(style, "soft")
            .set(drive, 3)
            .set(character, 40)
            .set(emphasis, 4)
            .set(quality, "x8")
            .set(phase, "linear");
        p.add("Mid Warmth")
            .set(style, "tube")
            .set(drive, 5)
            .set(bias, 15)
            .set(character, 20)
            .set(channelMode, "mid");
        p.add("Side Air")
            .set(style, "transistor")
            .set(drive, 10)
            .set(character, 20)
            .set(channelMode, "side")
            .set(bandLow, 3000)
            .set(mix, 60);
        p.add("Iron Bass").set(style, "transformer").set(drive, 9).set(character, 55);
        p.add("Bass Growl")
            .set(style, "tube")
            .set(drive, 18)
            .set(character, 40)
            .set(bias, 25)
            .set(bandLow, 160)
            .set(highCut, 7000);
        p.add("Drum Crunch")
            .set(style, "diode")
            .set(drive, 15)
            .set(character, 50)
            .set(dynamics, 50)
            .set(mix, 50);
        p.add("Sustain Fuzz")
            .set(style, "fuzz")
            .set(drive, 18)
            .set(character, 30)
            .set(dynamics, -70)
            .set(highCut, 6000);
        p.add("Master Clipper")
            .set(style, "clip")
            .set(drive, 3)
            .set(character, 15)
            .set(autoGain, false)
            .set(quality, "x8")
            .set(phase, "linear");
        p.add("Lo-Fi Sampler")
            .set(style, "alias")
            .set(drive, 3)
            .set(character, 55)
            .set(highCut, 9000);
        p.add("Bitcrushed").set(style, "crush").set(drive, 0).set(character, 70).set(mix, 70);
        p.add("Octave Up").set(style, "rectify").set(drive, 6).set(character, 100).set(mix, 50);
        p.add("Folded Pulse")
            .set(style, "fold")
            .set(drive, 16)
            .set(character, 30)
            .set(motion, 60)
            .set(motionSync, "n8")
            .set(motionShape, "triangle")
            .set(quality, "x8");
        p.add("Ringing Throat")
            .set(style, "transistor")
            .set(drive, 20)
            .set(character, 30)
            .set(feedback, 70)
            .set(feedbackPitch, 220)
            .set(highCut, 8000);
        p.add("Broken Radio")
            .set(style, "alias")
            .set(drive, 12)
            .set(character, 70)
            .set(bandLow, 400)
            .set(bandHigh, 3500)
            .set(motion, 40)
            .set(motionShape, "drift")
            .set(motionTarget, "character")
            .set(motionRate, 0.3)
            .set(mix, 85);
    }

    // --- audio ------------------------------------------------------------------------------

    double rate = 48000.0;
    int factor = 1;
    int latency = 0;
    double hi = 48000.0;  // the oversampled rate
    GainCache driveGain, levelGain, previousLevelGain, outputGain;
    Array<Oversampler, 2> over;
    Array<Lane, 2> lane;
    Array<DelayLine, 2> loop;
    int loopMax = 4;

    // The auto gain: dB to apply after the curve, for each dB of drive, for
    // the style now and the one before a change.
    Array<double, 121> compNow, compWas;
    Array<double, 64> compNodes, compWeights;
    Array<double, 121> compDrives;
    double compWeight = 1.0;
    double compGain = 1.0, compWasGain = 1.0, compStep = 0.002;
    int styleNow = 1, styleWas = 1;
    double fade = 1.0, fadeStep = 0.001;

    // The filters' glides: designed for these values.
    double atLow = -1.0, atHigh = -1.0, atEmph = -100.0, atLowCut = -1.0, atHighCut = -1.0;
    bool splitLow = false, splitHigh = false, cutLow = false, cutHigh = false;
    int sinceTune = 0;

    // Dynamics and motion.
    double fastEnv = 0.0, slowEnv = 0.0;
    double fastUp = 0.1, fastDown = 0.01, slowUp = 0.01, slowDown = 0.001;
    double lfoPhase = 0.0;
    int lfoCycle = 0;
    double lfoNow = 0.0;
    bool randomFresh = true;
    int randomCycle = 0;
    double randomAt = 0.0, randomNext = 0.0;

    // The probe: a sine through its own curve, measured each 20 cycles.
    Shaper probe;
    double probePhase = 0.0, probeStep = 0.002, probeGain = 1.0;
    double probeCos = 1.0, probeSin = 0.0, stepCos = 1.0, stepSin = 0.0;
    Array<double, 16> probeRe, probeIm;
    double probeSum = 0.0, probeSq = 0.0;
    int probeCount = 0, probeCycles = 0;

    // The music's distortion: running moments of the curve's input and output.
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0, sw = 0.0, momentKeep = 0.999;
    int sinceMoments = 0;

    int listenMode = 0;
    double loopScale = 1.0, loopOpen = 1.0, loopStep = 0.001;
    int quiet = 0, quietLimit = 48000;
    int modeNow = 0;
    double duck = 1.0, duckStep = 0.01;
    int listenLength = 480, listenAt = 480;
    double listenBand = 0.0, listenDelta = 0.0;
    double listenFromBand = 0.0, listenFromDelta = 0.0;
    bool unplayed = true;

    // Meters, the spectra's rings and the transfer plot's samples.
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0, fall = 0.999;
    int ringAt = 0, ringCount = 0, traceAt = 0;
    double lastDrive = 0.0;

    SharedArray<float, 16384> ringIn, ringOut;
    Shared<int> ringHead, ringWritten;
    Shared<double> sampleRateNow;
    SharedArray<float, 1024> traceX, traceY;
    Shared<int> traceHead;
    Array<double, 48> levelHist, levelCount;
    SharedArray<float, 48> levels;     // where the curve's input has been: 1.5 dB bins up to 0 dBFS
    SharedArray<float, 16> harmonics;  // H1..H16 of the probe, dB of the output's RMS
    Shared<double> inLeft, inRight, outLeft, outRight, lfoShown, lfoPhaseShown, driveShown,
        distShown;
    Shared<int> lfoCycleShown, latencyShown;
    Shared<double> compShown, compSetShown;  // the auto gain now, and at the drive as set

    void prepare() {
        rate = sampleRate();
        const int q = quality.index();
        factor = q == 0 ? 1 : (q == 1 ? 2 : (q == 2 ? 4 : (q == 3 ? 8 : 16)));
        // No further than 768 kHz: 16x at 48 kHz, 8x at 96 kHz.
        while (factor > 1 && rate * factor > 770000.0) factor = factor / 2;
        hi = rate * factor;
        loopMax = ceilToInt(hi / 18.0) + 8;
        const Oversampler::Phase filters =
            phase.index() == 1 ? Oversampler::Phase::Linear : Oversampler::Phase::Minimum;
        for (int c = 0; c < 2; ++c) {
            over[c].prepare(factor, filters);
            over[c].clear();
            lane[c].clear();
            lane[c].now.prepare(hi);
            lane[c].was.prepare(hi);
            loop[c].prepare(loopMax);
            loop[c].clear();
        }
        latency = over[0].latency();
        setLatency(latency);
        probe.prepare(rate);
        probeStep = kProbeHz / rate;
        stepCos = cos(twoPi * probeStep);
        stepSin = sin(twoPi * probeStep);
        fadeStep = 1.0 / (0.02 * rate);
        compStep = 1.0 - exp(-1.0 / (0.01 * rate));
        listenLength = max(1, roundToInt(0.01 * rate));
        listenMode = listen.index();
        snapListen();
        duckStep = 1.0 / (0.003 * rate);
        duck = 1.0;
        loopOpen = 1.0;
        loopStep = 1.0 / (0.2 * rate);
        quiet = 0;
        quietLimit = roundToInt(rate);
        modeNow = channelMode.index();
        fastUp = 1.0 - exp(-1.0 / (0.0003 * rate));
        fastDown = 1.0 - exp(-1.0 / (0.015 * rate));
        slowUp = 1.0 - exp(-1.0 / (0.025 * rate));
        slowDown = 1.0 - exp(-1.0 / (0.25 * rate));
        momentKeep = exp(-1.0 / (0.3 * rate));
        fall = dbToGain(-20.0 / rate);
        // A restart prepares an instance that has played: start it afresh.
        fastEnv = 0.0;
        slowEnv = 0.0;
        lfoPhase = 0.0;
        lfoCycle = 0;
        lfoNow = 0.0;
        randomFresh = true;
        probePhase = 0.0;
        probeCos = 1.0;
        probeSin = 0.0;
        clearProbe();
        sx = 0.0;
        sy = 0.0;
        sxx = 0.0;
        sxy = 0.0;
        syy = 0.0;
        sw = 0.0;
        sinceMoments = 0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        ringAt = 0;
        ringCount = 0;
        levelHist.fill(0.0);
        levelCount.fill(0.0);
        traceAt = 0;
        fade = 1.0;
        atLow = -1.0;
        atHigh = -1.0;
        atEmph = -100.0;
        atLowCut = -1.0;
        atHighCut = -1.0;
        sinceTune = 0;
        unplayed = true;
        styleNow = style.index();
        styleWas = styleNow;
        // The quadrature and drive grid are independent of the curve.
        // Parameter changes only need to measure it on this same grid.
        compWeight = 0.0;
        const double dz = 10.0 / kNodes;
        for (int j = 0; j < kNodes; ++j) {
            const double z = -5.0 + (j + 0.5) * dz;
            compNodes[j] = z;
            compWeights[j] = exp(-0.5 * z * z);
            compWeight += compWeights[j];
        }
        for (int d = 0; d < kCompSteps; ++d) compDrives[d] = dbToGain(kCompLow + d);
        buildComp(compNow, styleNow);
        buildComp(compWas, styleNow);
        loopScale = 1.0 / max(1.0, steepest(styleNow));
        compGain = 1.0;
        compWasGain = 1.0;
        tune(true);
    }

    void clearProbe() {
        probe.clear();
        probeRe.fill(0.0);
        probeIm.fill(0.0);
        probeSum = 0.0;
        probeSq = 0.0;
        probeCount = 0;
        probeCycles = 0;
    }

    // The auto gain for a style: over drives -36 .. 84 dB, the gain that
    // brings a -18 dBFS RMS Gaussian signal through the curve (as set, bias
    // and its offset removed) back to -18 dBFS RMS.
    void buildComp(Array<double, 121>& table, int st) {
        const double c = character.value(), b = 0.5 * bias.value();
        for (int d = 0; d < kCompSteps; ++d) {
            const double g = compDrives[d];
            double m = 0.0, m2 = 0.0;
            for (int j = 0; j < kNodes; ++j) {
                const double z = compNodes[j], w = compWeights[j];
                const double u = g * kRef * z;
                const double y = st == 3 ? u : curve(st, u + b, c);
                m += w * y;
                m2 += w * y * y;
            }
            m /= compWeight;
            m2 /= compWeight;
            const double rms = sqrt(max(m2 - m * m, 0.0));
            table[d] = rms > 1e-9 ? clamp(gainToDb(kRef / rms), -48.0, 24.0) : 24.0;
        }
    }

    // The curve's steepest slope near where it rests, over +-1 about the bias.
    double steepest(int st) const {
        if (st == 3 || st == 10 || st == 11)
            return 1.0;  // crush steps: its quantising slope is tanh's
        const double c = character.value(), b = 0.5 * bias.value();
        double most = 0.0;
        for (int k = 0; k <= 40; ++k) {
            const double u = b - 1.0 + k * 0.05;
            most = max(most, abs(curve(st, u + 0.001, c) - curve(st, u - 0.001, c)) / 0.002);
        }
        return most;
    }

    double compAt(const Array<double, 121>& table, double db) const {
        const double at = clamp(db - kCompLow, 0.0, kCompSteps - 1.0);
        const int k = min(floorToInt(at), kCompSteps - 2);
        return table[k] + (at - k) * (table[k + 1] - table[k]);
    }

    // The filters, designed for the parameters' glides at frame i (or their
    // values), at the oversampled rate. Only what moved is redesigned.
    void tune(bool all) {
        const double low = bandLow.value(), high = max(bandHigh.value(), low * 1.05);
        const double e = emphasis.value(), lc = lowCut.value(), hc = highCut.value();
        retune(all, low, high, e, lc, hc);
    }

    void retune(bool all, double low, double high, double e, double lc, double hc) {
        const double ratio = 1.0 / factor;
        if (all || abs(low - atLow) > 1e-3 * low) {
            atLow = low;
            splitLow = low > 20.5;
            for (int c = 0; c < 2; ++c) {
                lane[c].lowA.lowpass(low * ratio, 0.7071067811865476);
                lane[c].lowB.lowpass(low * ratio, 0.7071067811865476);
                lane[c].restA.highpass(low * ratio, 0.7071067811865476);
                lane[c].restB.highpass(low * ratio, 0.7071067811865476);
            }
        }
        if (all || abs(high - atHigh) > 1e-3 * high) {
            atHigh = high;
            splitHigh = high < 19500.0;
            const double f = min(high, 0.45 * rate) * ratio;
            for (int c = 0; c < 2; ++c) {
                lane[c].midA.lowpass(f, 0.7071067811865476);
                lane[c].midB.lowpass(f, 0.7071067811865476);
                lane[c].highA.highpass(f, 0.7071067811865476);
                lane[c].highB.highpass(f, 0.7071067811865476);
                lane[c].lowAllpass.allpass(f, 0.7071067811865476);
            }
        }
        if (all || abs(e - atEmph) > 0.01) {
            atEmph = e;
            for (int c = 0; c < 2; ++c) {
                lane[c].pre.design(e, kPivot, hi, false);
                lane[c].post.design(e, kPivot, hi, true);
            }
        }
        if (all || abs(lc - atLowCut) > 1e-3 * lc) {
            atLowCut = lc;
            cutLow = lc > 20.5;
            for (int c = 0; c < 2; ++c) lane[c].lowCut.highpass(lc * ratio, 0.7071067811865476);
        }
        if (all || abs(hc - atHighCut) > 1e-3 * hc) {
            atHighCut = hc;
            cutHigh = hc < 19500.0;
            const double f = min(hc, 0.45 * rate) * ratio;
            for (int c = 0; c < 2; ++c) lane[c].highCut.lowpass(f, 0.7071067811865476);
        }
        if (all) {
            for (int c = 0; c < 2; ++c) {
                lane[c].dc.setCutoff(6.0 * ratio);
                lane[c].loopLow.setCutoff(min(6000.0, 0.45 * rate) * ratio);
                lane[c].loopDc.setCutoff(20.0 * ratio);
            }
        }
    }

    void snapListen() {
        listenBand = listenMode == 1 ? 1.0 : 0.0;
        listenDelta = listenMode == 2 ? 1.0 : 0.0;
        listenFromBand = listenBand;
        listenFromDelta = listenDelta;
        listenAt = listenLength;
    }

    void update() {
        const int nextListen = listen.index();
        if (nextListen != listenMode) {
            listenFromBand = listenBand;
            listenFromDelta = listenDelta;
            listenAt = 0;
            listenMode = nextListen;
        }
        probeGain = dbToGain(drive.value());
        const int st = style.index();
        if (st != styleNow) {
            if (unplayed) {
                styleNow = st;
                styleWas = st;
                fade = 1.0;
                for (int c = 0; c < 2; ++c) lane[c].now.clear();
            } else {
                // Crossfade from the curve as it is to the new one, which
                // starts afresh.
                styleWas = styleNow;
                styleNow = st;
                for (int k = 0; k < kCompSteps; ++k) compWas[k] = compNow[k];
                compWasGain = compGain;
                for (int c = 0; c < 2; ++c) {
                    lane[c].was = lane[c].now;
                    lane[c].now.clear();
                }
                fade = 0.0;
            }
            clearProbe();
        }
        if (style.changed() || character.changed() || bias.changed()) {
            buildComp(compNow, styleNow);
            loopScale = 1.0 / max(1.0, steepest(styleNow));
        }
        if (!unplayed) return;
        snapListen();
        tune(true);
    }

    // The LFO at frame i: -1..1.
    double lfoAt(const Block& io, int i) {
        const int sync = motionSync.index();
        const Transport& t = io.transport;
        if (sync > 0 && t.playing && t.tempo > 0.0) {
            const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
            const double cycleBeats = syncBeats(sync, barBeats);
            const double at = (t.beats + i * t.tempo / (60.0 * rate)) / cycleBeats;
            const int cycle = floorToInt(at);
            lfoPhase = at - cycle;
            lfoCycle = cycle;
        } else {
            double hz = motionRate.value();
            if (sync > 0) {
                const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
                hz = (t.tempo > 0.0 ? t.tempo : 120.0) / 60.0 / syncBeats(sync, barBeats);
            }
            lfoPhase += hz / rate;
            if (lfoPhase >= 1.0) {
                lfoPhase -= floor(lfoPhase);
                lfoCycle += 1;
            }
        }
        const int shape = motionShape.index();
        if (shape < 5) return lfoShape(shape, lfoPhase, lfoCycle);
        // Random and drift's hash values depend on the cycle, not its phase.
        if (randomFresh || randomCycle != lfoCycle) {
            randomFresh = false;
            randomCycle = lfoCycle;
            randomAt = hashUnit(lfoCycle);
            randomNext = hashUnit(lfoCycle + 1);
        }
        if (shape == 5) return randomAt;
        return randomAt + (randomNext - randomAt) * (0.5 - 0.5 * cos(pi * lfoPhase));
    }

    void process(Block& io) {
        const bool first = unplayed;
        const bool stereo = io.channels > 1;
        const int wantMode = stereo ? channelMode.index() : 0;
        if (first) {
            modeNow = wantMode;
            snapListen();
        }
        const int target = motionTarget.index();
        double lastDriveDb = drive.value();
        unplayed = false;
        for (int i = 0; i < io.frames; ++i) {
            // The filters follow their glides every 16 frames.
            sinceTune += 1;
            if (sinceTune >= 16) {
                sinceTune = 0;
                const double low = bandLow.at(i);
                retune(false, low, max(bandHigh.at(i), low * 1.05), emphasis.at(i), lowCut.at(i),
                       highCut.at(i));
            }
            // Decided each frame, never each block, so a render does not
            // depend on how the host cuts it up.
            const bool runLow = splitLow || bandLow.value() > 20.5;
            const bool runHigh = splitHigh || bandHigh.value() < 19500.0;
            const bool runLowCut = cutLow || lowCut.value() > 20.5;
            const bool runHighCut = cutHigh || highCut.value() < 19500.0;
            double l = io[0][i];
            double r = stereo ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;

            // Dynamics: the drive follows the attacks or the sustain.
            const double a = max(abs(l), abs(r));
            fastEnv += (a > fastEnv ? fastUp : fastDown) * (a - fastEnv);
            slowEnv += (a > slowEnv ? slowUp : slowDown) * (a - slowEnv);
            // Keep both followers warm for a later edit, but their dB
            // difference is unused while the glided depth is exactly zero.
            double dynDb = 0.0;
            const double amount = dynamics.at(i);
            if (amount != 0.0) {
                const double slowDb = gainToDb(slowEnv);
                const double gate = clamp((slowDb + 70.0) / 10.0, 0.0, 1.0);
                dynDb = amount * clamp(gainToDb(fastEnv) - slowDb, -24.0, 24.0) * gate;
            }

            // Motion.
            const double depth = motion.at(i);
            lfoNow = depth > 0.0 ? lfoAt(io, i) : 0.0;
            const double m = depth * lfoNow;
            double driveDb = drive.at(i) + dynDb + (target == 0 ? 24.0 * m : 0.0);
            driveDb = clamp(driveDb, -36.0, 84.0);
            lastDriveDb = driveDb;
            const double c = clamp(character.at(i) + (target == 1 ? 0.5 * m : 0.0), 0.0, 1.0);
            const double b = 0.5 * clamp(bias.at(i) + (target == 2 ? m : 0.0), -1.0, 1.0);
            const double pitch =
                clamp(feedbackPitch.at(i) * exp2(target == 3 ? 2.0 * m : 0.0), 20.0, 16000.0);
            const double period = max(2.0, hi / pitch);
            // The feedback, scaled by the curve's steepest slope so the
            // loop's gain stays under 1, and let go a second into digital
            // silence so no ring outlasts the music.
            quiet = a < 1e-5 ? quiet + 1 : 0;
            loopOpen += (quiet > quietLimit ? -1.0 : 1.0) * loopStep;
            loopOpen = clamp(loopOpen, 0.0, 1.0);
            const double fb = feedback.at(i) * loopScale * loopOpen;
            const bool fbOn = fb != 0.0;
            const double g = driveGain.get(driveDb);
            // Where each curve rests with no signal: its offset, taken away
            // at once (the bias would otherwise step and ring).
            const double restNow = styleNow == 3 ? 0.0 : curve(styleNow, b, c);
            const bool level = autoGain.on();
            const double compTo = level ? levelGain.get(compAt(compNow, driveDb)) : 1.0;
            if (first && i == 0) compGain = compTo;
            compGain += compStep * (compTo - compGain);
            double restWas = 0.0, t = 1.0;
            // Once the old curve is inaudible, none of its control values
            // is needed. The next style change copies compGain afresh.
            if (fade < 1.0) {
                restWas = styleWas == 3 ? 0.0 : curve(styleWas, b, c);
                const double compWasTo =
                    level ? previousLevelGain.get(compAt(compWas, driveDb)) : 1.0;
                compWasGain += compStep * (compWasTo - compWasGain);
                fade = min(1.0, fade + fadeStep);
                t = 0.5 - 0.5 * cos(pi * fade);
            }
            const double wet = mix.at(i);
            if (listenAt < listenLength) {
                listenAt += 1;
                const double progress = static_cast<double>(listenAt) / listenLength;
                const double toBand = listenMode == 1 ? 1.0 : 0.0;
                const double toDelta = listenMode == 2 ? 1.0 : 0.0;
                listenBand =
                    listenAt == listenLength ? toBand : lerp(listenFromBand, toBand, progress);
                listenDelta =
                    listenAt == listenLength ? toDelta : lerp(listenFromDelta, toDelta, progress);
            }

            // Into the channels: left and right, or mid and side. A change
            // between them dips the output for 3 ms each way.
            if (wantMode != modeNow) {
                duck -= duckStep;
                if (duck <= 0.0) {
                    duck = 0.0;
                    modeNow = wantMode;
                }
            } else if (duck < 1.0) {
                duck = min(1.0, duck + duckStep);
            }
            const int mode = modeNow;
            double v0 = l, v1 = r;
            if (mode > 0) {
                v0 = 0.5 * (l + r);
                v1 = 0.5 * (l - r);
            }
            for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
                const bool driven = mode == 0 || (mode == 1 && ch == 0) || (mode == 2 && ch == 1);
                Oversampler& o = over[ch];
                Lane& s = lane[ch];
                const double in = ch == 0 ? v0 : v1;
                o.up(in);
                for (int k = 0; k < factor; ++k) {
                    const double x = o.sample(k);
                    // The band: low and high pass clean around it.
                    double low = 0.0, band = x, high = 0.0;
                    // (Filters about to come in run beforehand, so none
                    // starts cold.)
                    if (runLow) {
                        const double lo = s.lowB.process(s.lowA.process(x));
                        const double rest = s.restB.process(s.restA.process(x));
                        if (splitLow) {
                            low = lo;
                            band = rest;
                        }
                    }
                    if (runHigh) {
                        const double top = s.highB.process(s.highA.process(band));
                        const double inner = s.midB.process(s.midA.process(band));
                        const double turned = s.lowAllpass.process(low);
                        if (splitHigh) {
                            high = top;
                            band = inner;
                            low = turned;
                        }
                    }
                    double y = band;
                    if (driven) {
                        const double e = s.pre.process(band);
                        double u = g * e;
                        if (fbOn) {
                            const double back =
                                s.loopDc.highpass(s.loopLow.lowpass(loop[ch].read(period - 1.0)));
                            u += fb * tanh(back);
                        }
                        double shaped = s.now.process(styleNow, u, c, b) - restNow;
                        loop[ch].push(shaped);
                        if (ch == (mode == 2 ? 1 : 0) && k == 0)
                            moments(u, shaped, e, shaped * compGain);
                        shaped *= compGain;
                        if (t < 1.0) {
                            const double old =
                                (s.was.process(styleWas, u, c, b) - restWas) * compWasGain;
                            shaped = old + t * (shaped - old);
                        }
                        y = s.post.process(s.dc.highpass(shaped));
                        if (runLowCut) {
                            const double cut = s.lowCut.process(y);
                            if (cutLow) y = cut;
                        }
                        if (runHighCut) {
                            const double cut = s.highCut.process(y);
                            if (cutHigh) y = cut;
                        }
                    }
                    const double normal = low + band + high + wet * (y - band);
                    double out = normal;
                    if (listenBand > 0.0) out += listenBand * (y - normal);
                    if (listenDelta > 0.0) out += listenDelta * (wet * (y - band) - normal);
                    o.set(k, out);
                }
                const double d = o.down();
                if (ch == 0)
                    v0 = d;
                else
                    v1 = d;
            }
            double ol = v0, orr = v1;
            if (mode > 0) {
                ol = v0 + v1;
                orr = v0 - v1;
            }
            const double gainOut = outputGain.get(output.at(i)) * duck;
            ol *= gainOut;
            orr *= gainOut;
            io[0][i] = ol;
            if (stereo) io[1][i] = orr;

            runProbe();
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);
            ringIn[ringAt] = static_cast<float>(stereo ? 0.5 * (l + r) : l);
            ringOut[ringAt] = static_cast<float>(stereo ? 0.5 * (ol + orr) : ol);
            ringAt = ringAt + 1 == kRing ? 0 : ringAt + 1;
            ringCount += 1;
        }
        lastDrive = lastDriveDb;
        // Where the curve's input has been, about the last half second.
        const double keep = exp(-io.frames / (0.5 * rate));
        double most = 0.0;
        for (int b = 0; b < kLevelBins; ++b) {
            levelHist[b] = keep * levelHist[b] + levelCount[b];
            levelCount[b] = 0.0;
            most = max(most, levelHist[b]);
        }
        for (int b = 0; b < kLevelBins; ++b)
            levels[b] = static_cast<float>(most > 0.0 ? levelHist[b] / most : 0.0);
        ringHead.set(ringAt);
        ringWritten.set(ringCount);
        sampleRateNow.set(rate);
        traceHead.set(traceAt);
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(stereo ? peakIn1 : peakIn0));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(stereo ? peakOut1 : peakOut0));
        inputLevel.set(max(-96.0, gainToDb(max(peakIn0, peakIn1))));
        outputLevel.set(max(-96.0, gainToDb(max(peakOut0, peakOut1))));
        lfoShown.set(lfoNow);
        lfoPhaseShown.set(lfoPhase);
        lfoCycleShown.set(lfoCycle);
        latencyShown.set(latency);
        driveShown.set(lastDriveDb);
        compShown.set(compGain);
        compSetShown.set(autoGain.on() ? dbToGain(compAt(compNow, drive.value())) : 1.0);
        driveNow.set(lastDriveDb);
    }

    // The curve's input and output, once a frame: their running moments
    // give the share of the output that is not a scaled copy of the input;
    // and the transfer plot's samples (the band before the drive, the curve's
    // output).
    void moments(double u, double y, double before, double after) {
        sx = momentKeep * sx + u;
        sy = momentKeep * sy + y;
        sxx = momentKeep * sxx + u * u;
        sxy = momentKeep * sxy + u * y;
        syy = momentKeep * syy + y * y;
        sw = momentKeep * sw + 1.0;
        const double a = abs(before);
        if (a > 0.00025) {
            const int bin = clamp(floorToInt((gainToDb(a) + 72.0) / 1.5), 0, kLevelBins - 1);
            levelCount[bin] += 1.0;
        }
        traceX[traceAt] = static_cast<float>(before);
        traceY[traceAt] = static_cast<float>(after);
        traceAt = traceAt + 1 == kTrace ? 0 : traceAt + 1;
        sinceMoments += 1;
        if (sinceMoments < 2400) return;
        sinceMoments = 0;
        const double mx = sx / sw, my = sy / sw;
        const double vx = sxx / sw - mx * mx, vy = syy / sw - my * my, cxy = sxy / sw - mx * my;
        double share = 0.0;
        if (vx > 1e-12 && vy > 1e-12) share = sqrt(clamp(1.0 - cxy * cxy / (vx * vy), 0.0, 1.0));
        distortion.set(share);
        distShown.set(share);
    }

    // The probe: a 100 Hz sine at -18 dBFS RMS through its own copy of the
    // curve, as set (motion and dynamics aside); every 20 cycles its
    // harmonics, THD and even share.
    void runProbe() {
        const double c = character.value(), b = 0.5 * bias.value();
        const double c1 = probeCos, s1 = probeSin;
        const double x = kRef * sqrt(2.0) * s1;
        const double y = probe.process(styleNow, probeGain * x, c, b);
        double ch = 1.0, sh = 0.0;
        for (int h = 0; h < kHarm; ++h) {
            const double nc = ch * c1 - sh * s1;
            sh = sh * c1 + ch * s1;
            ch = nc;
            probeRe[h] += y * ch;
            probeIm[h] += y * sh;
        }
        probeSum += y;
        probeSq += y * y;
        probeCount += 1;
        probePhase += probeStep;
        const double nc = probeCos * stepCos - probeSin * stepSin;
        probeSin = probeSin * stepCos + probeCos * stepSin;
        probeCos = nc;
        if (probePhase < 1.0) return;
        probePhase -= 1.0;
        probeCos = cos(twoPi * probePhase);
        probeSin = sin(twoPi * probePhase);
        probeCycles += 1;
        if (probeCycles < kProbeCycles) return;
        const double n = probeCount;
        const double mean = probeSum / n;
        const double total = max(probeSq / n - mean * mean, 1e-30);
        double even = 0.0, all = 0.0, first = 0.0;
        for (int h = 0; h < kHarm; ++h) {
            const double re = 2.0 * probeRe[h] / n, im = 2.0 * probeIm[h] / n;
            const double power = 0.5 * (re * re + im * im);
            harmonics[h] = static_cast<float>(10.0 * log10(power / total + 1e-20));
            if (h == 0)
                first = power;
            else {
                all += power;
                if (imod(h, 2) == 1) even += power;
            }
        }
        thd.set(sqrt(clamp(1.0 - first / total, 0.0, 1.0)));
        evenShare.set(all > 1e-24 ? even / all : 0.0);
        probeRe.fill(0.0);
        probeIm.fill(0.0);
        probeSum = 0.0;
        probeSq = 0.0;
        probeCount = 0;
        probeCycles = 0;
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1080;
    static constexpr double kH = 780;
    static constexpr double gx = 28;  // the spectrum
    static constexpr double gy = 96;
    static constexpr double gw = 712;
    static constexpr double gh = 204;
    static constexpr double py = 344;  // the lower panels
    static constexpr double ph = 420;
    static constexpr double ax = 16;  // the curve panel
    static constexpr double aw = 520;
    static constexpr double mvx = 548;  // the movement panel
    static constexpr double mvw = 268;
    static constexpr double ox = 828;  // the output panel
    static constexpr double ow = 236;
    static constexpr double kChannelsY = 182;  // its rows of pills, down from py
    static constexpr double kOverY = 230;
    static constexpr double kPhaseY = 278;
    static constexpr double kListenY = 326;
    static constexpr double tx = 32;  // the transfer plot
    static constexpr double ty = 524;
    static constexpr double ts = 168;
    static constexpr double hx = 212;  // the harmonics
    static constexpr double hw = 150;
    static constexpr double wx = 564;  // the LFO's wave
    static constexpr double wy = 512;
    static constexpr double ww = 236;
    static constexpr double wh = 56;
    static constexpr double kTopDb = 0.0;  // the spectrum's scale
    static constexpr double kFloorDb = -96.0;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the band's low edge, 3 its high edge, 4 the band
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double pressX = 0.0, pressLow = 20.0, pressHigh = 20000.0;
    double zoom = 0.25;
    bool zoomed = false;

    // The analyser's.
    Array<double, 8192> fr, fi;
    Array<double, 4096> twr, twi;
    Array<double, 8193> spr, spi;
    Array<int, 8192> rev;
    Array<float, 16384> window;
    Array<double, 8193> bins;
    Array<double, 8194> cum;
    Array<double, 360> avgIn, avgOut, tgtIn, tgtOut, showIn, showOut;
    bool tables = false, primed = false, haveSpectrum = false;
    int lastWritten = 0, sinceTransform = 1000000;

    void editor(Editor& e) { e.size(kW, kH); }

    int family() const { return style.index() < 4 ? 0 : (style.index() < 8 ? 1 : 2); }
    int familyColour(int st) const { return st < 4 ? kWarm : (st < 8 ? kBite : kWild); }
    double rateNow() const { return sampleRateNow.get() > 1000.0 ? sampleRateNow.get() : 48000.0; }

    // --- the analyser -------------------------------------------------------------------------

    void buildTables() {
        for (int j = 0; j < 4096; ++j) {
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
            for (int b = 0; b < 13; ++b) {
                r = r * 2 + imod(v, 2);
                v = idiv(v, 2);
            }
            rev[n] = r;
        }
        for (int n = 0; n < kN; ++n)
            window[n] = static_cast<float>(0.5 - 0.5 * cos(twoPi * n / kN));
        tables = true;
    }

    // The newest kN samples of a ring, windowed, through the transform:
    // each bin's power.
    void transform(const SharedArray<float, 16384>& ring) {
        const int h = ringHead.get();
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
            const double or_ = di, oi = -dr;
            const double xr = er + spr[k] * or_ - spi[k] * oi;
            const double xi = ei + spr[k] * oi + spi[k] * or_;
            bins[k] = (xr * xr + xi * xi) * norm;
            cum[k + 1] = cum[k] + bins[k];
        }
    }

    double binDb(int k) const { return 10.0 * log10(bins[clamp(k, 0, kM)] + 1e-24); }

    double below(double x) const {
        const double c = clamp(x, 0.0, static_cast<double>(kM));
        const int k = floorToInt(c);
        return cum[k] + (c - k) * bins[min(k, kM)];
    }

    // The spectrum at kPoints frequencies: smoothed over a fraction of an
    // octave where bins are dense, interpolated where they are sparse,
    // averaged as power, and tilted 4.5 dB an octave about 1 kHz.
    void measure(int elapsed, Array<double, 360>& average, Array<double, 360>& target) {
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
            const double curveDb =
                y1 +
                0.5 * t *
                    (y2 - y0 +
                     t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0)));
            const double mixAmount = clamp(2.0 * half - 1.0, 0.0, 1.0);
            const double now = lerp(dbToGain(2.0 * curveDb), smooth, mixAmount);
            average[i] = keep * average[i] + (1.0 - keep) * now;
        }
        for (int i = 0; i < kPoints; ++i) {
            const double a = average[max(i - 2, 0)], b = average[max(i - 1, 0)], c = average[i];
            const double d = average[min(i + 1, kPoints - 1)], e = average[min(i + 2, kPoints - 1)];
            const double hz = 20.0 * pow(1000.0, i / (kPoints - 1.0));
            target[i] = 10.0 * log10((a + 4.0 * b + 6.0 * c + 4.0 * d + e) / 16.0 + 1e-24) +
                        kSlopeDb * log2(hz / 1000.0);
        }
    }

    // Each repaint: new transforms once enough audio has arrived, and the
    // drawn lines easing toward them.
    void analyse() {
        if (!tables) buildTables();
        const int now = ringWritten.get();
        int fresh = now - lastWritten;
        lastWritten = now;
        if (fresh < 0 || fresh > 10 * kN) fresh = kN;
        sinceTransform += fresh;
        const int hop = max(1024, roundToInt(rateNow() * 0.04));
        if (sinceTransform >= hop && (fresh > 0 || !haveSpectrum) && now > 0) {
            transform(ringIn);
            measure(sinceTransform, avgIn, tgtIn);
            transform(ringOut);
            measure(sinceTransform, avgOut, tgtOut);
            haveSpectrum = true;
            sinceTransform = 0;
        }
        if (!haveSpectrum) return;
        const double dt = fresh / rateNow();
        const double up = 1.0 - exp(-dt / 0.05);
        const double down = 1.0 - exp(-dt / 0.12);
        for (int i = 0; i < kPoints; ++i) {
            if (!primed) {
                showIn[i] = tgtIn[i];
                showOut[i] = tgtOut[i];
            } else {
                showIn[i] += (tgtIn[i] - showIn[i]) * (tgtIn[i] > showIn[i] ? up : down);
                showOut[i] += (tgtOut[i] - showOut[i]) * (tgtOut[i] > showOut[i] ? up : down);
            }
        }
        primed = true;
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
        g.serifFont(14);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void aside(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(10.5);
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

    void pill(Canvas& g, double x, double y, double w, double h, const char* text, bool lit,
              int colour, bool hot) {
        g.fillStyle(lit ? colour : (hot ? 0x3A2F27 : Theme::raised));
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
    }

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value; a bipolar one
    // lights from the top.
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool bipolar,
              bool hot, const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 34 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(r > 34 ? 4.5 : 3.5);
        if (abs(at - from) > 0.001) {
            g.beginPath();
            if (at >= from)
                g.arc(x, y, r, from, at);
            else
                g.arc(x, y, r, at, from);
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
        g.fillText(name, x, y - r - 12);
    }

    void valueStyle(Canvas& g) {
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void fillHz(Canvas& g, double hz, double x, double y) {
        if (hz < 100.0)
            g.fillText(format("%.1f Hz", hz), x, y);
        else if (hz < 1000.0)
            g.fillText(format("%.0f Hz", hz), x, y);
        else
            g.fillText(format("%.1f kHz", hz * 0.001), x, y);
    }

    // A signed level; one that rounds to nothing reads 0.0.
    void fillDb(Canvas& g, double db, double x, double y) {
        if (abs(db) < 0.05)
            g.fillText("0.0 dB", x, y);
        else
            g.fillText(format("%+.1f dB", db), x, y);
    }

    void fillPercent(Canvas& g, double v, double x, double y) {
        if (v < 0.0995)
            g.fillText(format("%.2f%%", v * 100.0), x, y);
        else
            g.fillText(format("%.1f%%", v * 100.0), x, y);
    }

    // A pitch's note: A4 is 440 Hz.
    void fillNote(Canvas& g, double hz, double x, double y) {
        const int m = roundToInt(ftom(hz));
        const int octave = idiv(m - imod(m, 12), 12) - 1;
        switch (imod(m, 12)) {
            case 0:
                g.fillText(format("C%d", octave), x, y);
                break;
            case 1:
                g.fillText(format("C#%d", octave), x, y);
                break;
            case 2:
                g.fillText(format("D%d", octave), x, y);
                break;
            case 3:
                g.fillText(format("D#%d", octave), x, y);
                break;
            case 4:
                g.fillText(format("E%d", octave), x, y);
                break;
            case 5:
                g.fillText(format("F%d", octave), x, y);
                break;
            case 6:
                g.fillText(format("F#%d", octave), x, y);
                break;
            case 7:
                g.fillText(format("G%d", octave), x, y);
                break;
            case 8:
                g.fillText(format("G#%d", octave), x, y);
                break;
            case 9:
                g.fillText(format("A%d", octave), x, y);
                break;
            case 10:
                g.fillText(format("A#%d", octave), x, y);
                break;
            default:
                g.fillText(format("B%d", octave), x, y);
        }
    }

    void fillStyleName(Canvas& g, int st, double x, double y) {
        switch (st) {
            case 0:
                g.fillText("Soft", x, y);
                break;
            case 1:
                g.fillText("Tape", x, y);
                break;
            case 2:
                g.fillText("Tube", x, y);
                break;
            case 3:
                g.fillText("Transformer", x, y);
                break;
            case 4:
                g.fillText("Transistor", x, y);
                break;
            case 5:
                g.fillText("Diode", x, y);
                break;
            case 6:
                g.fillText("Clip", x, y);
                break;
            case 7:
                g.fillText("Fuzz", x, y);
                break;
            case 8:
                g.fillText("Fold", x, y);
                break;
            case 9:
                g.fillText("Rectify", x, y);
                break;
            case 10:
                g.fillText("Crush", x, y);
                break;
            default:
                g.fillText("Alias", x, y);
        }
    }

    // What character does to the style now, in its own words.
    void fillCharacter(Canvas& g, double x, double y) {
        const double c = character.value();
        switch (style.index()) {
            case 0:
                g.fillText(c < 0.5 ? "gentle from the start" : "clean, then turns", x, y);
                break;
            case 1:
                g.fillText(format("wear %.0f%%", c * 100.0), x, y);
                break;
            case 2:
                g.fillText(format("sag %.0f%%", c * 100.0), x, y);
                break;
            case 3:
                g.fillText(format("core %.0f Hz", coreHz(c)), x, y);
                break;
            case 4:
                g.fillText(format("hardness %.0f%%", c * 100.0), x, y);
                break;
            case 5:
                g.fillText(format("asymmetry %.0f%%", c * 100.0), x, y);
                break;
            case 6:
                g.fillText(format("knee %.0f%%", c * 100.0), x, y);
                break;
            case 7:
                g.fillText(format("starve %.0f%%", c * 100.0), x, y);
                break;
            case 8:
                g.fillText(c < 0.5 ? "rounded folds" : "sharp folds", x, y);
                break;
            case 9:
                g.fillText(format("full-wave %.0f%%", c * 100.0), x, y);
                break;
            case 10:
                g.fillText(format("%.1f bits", crushBits(c)), x, y);
                break;
            default:
                if (aliasHz(c) < 1000.0)
                    g.fillText(format("held at %.0f Hz", aliasHz(c)), x, y);
                else
                    g.fillText(format("held at %.1f kHz", aliasHz(c) * 0.001), x, y);
        }
    }

    void fillSync(Canvas& g, int k, double x, double y) {
        switch (k) {
            case 0:
                g.fillText("FREE", x, y);
                break;
            case 1:
                g.fillText("4 BARS", x, y);
                break;
            case 2:
                g.fillText("2 BARS", x, y);
                break;
            case 3:
                g.fillText("1 BAR", x, y);
                break;
            case 4:
                g.fillText("1/2", x, y);
                break;
            case 5:
                g.fillText("1/4", x, y);
                break;
            case 6:
                g.fillText("1/8", x, y);
                break;
            case 7:
                g.fillText("1/16", x, y);
                break;
            case 8:
                g.fillText("1/32", x, y);
                break;
            case 9:
                g.fillText("1/4 T", x, y);
                break;
            case 10:
                g.fillText("1/8 T", x, y);
                break;
            case 11:
                g.fillText("1/16 T", x, y);
                break;
            case 12:
                g.fillText("1/4 D", x, y);
                break;
            default:
                g.fillText("1/8 D", x, y);
        }
    }

    // --- the mark -----------------------------------------------------------------------------

    // A teasel head: an egg of hooked bracts in rows, its long spines curving
    // up around it; the head warms with what the curve adds, the spines
    // reach further the harder it is driven.
    void drawMark(Canvas& g, double x, double y) {
        const double heat = clamp(sqrt(distShown.get()) * 1.6, 0.0, 1.0);
        const double reach = 0.65 + 0.35 * clamp((drive.value() + 12.0) / 60.0, 0.0, 1.0);
        const int warm = familyColour(style.index());
        g.lineCap(LineCap::Round);
        g.strokeStyle(kStem);
        g.lineWidth(1.8);
        g.beginPath();
        g.moveTo(x, y + 10);
        g.quadraticCurveTo(x + 1.5, y + 18, x - 1, y + 26);
        g.stroke();
        // The spines from the base, curving up past the head.
        g.strokeStyle(0xB49A6E);
        g.lineWidth(1.2);
        for (int k = 0; k < 4; ++k) {
            const double side = k < 2 ? -1.0 : 1.0;
            const double spread = (imod(k, 2) == 0 ? 12.0 : 8.5) * reach;
            const double rise = (imod(k, 2) == 0 ? 15.0 : 24.0) * reach;
            g.beginPath();
            g.moveTo(x + side * 2.0, y + 9);
            g.quadraticCurveTo(x + side * (spread + 3.0), y + 8, x + side * spread, y + 9 - rise);
            g.stroke();
        }
        // The head.
        g.radialGradient(x - 2, y - 5, 1, 13, 0xC8A878, kHead);
        g.beginPath();
        g.ellipse(x, y - 1, 7, 12);
        g.fill();
        g.fillStyle(warm);
        g.globalAlpha(0.15 + 0.75 * heat);
        g.beginPath();
        g.ellipse(x, y - 1, 7, 12);
        g.fill();
        g.globalAlpha(1.0);
        // Its rows of hooked bracts.
        g.strokeStyle(0x5E4630);
        g.lineWidth(0.9);
        for (int row = 0; row < 7; ++row) {
            const double ry = y - 10.5 + row * 3.3;
            const double half = 6.6 * sqrt(max(0.0, 1.0 - pow((ry - y + 1.0) / 12.0, 2.0)));
            for (int k = -3; k <= 3; ++k) {
                const double bx = x + k * 2.4 + (imod(row, 2) == 0 ? 0.0 : 1.2);
                if (abs(bx - x) > half - 0.8) continue;
                g.beginPath();
                g.moveTo(bx - 0.7, ry + 0.8);
                g.lineTo(bx + 0.5, ry - 0.9);
                g.stroke();
            }
        }
    }

    // --- the header ---------------------------------------------------------------------------

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

    void drawHeader(Canvas& g) {
        drawMark(g, 30, 27);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Teasel", 54, 37);
        const double nameWidth = g.measureText("Teasel");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        const int f = family();
        g.fillText(f == 0 ? "warm saturation" : (f == 1 ? "biting distortion" : "wild distortion"),
                   62 + nameWidth, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 680, 23, 150, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 898, 23, 150, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the spectrum -------------------------------------------------------------------------

    double specY(double db) const {
        return dbToY(clamp(db, kFloorDb, kTopDb), kFloorDb, kTopDb, gy + 6, gh - 6);
    }
    double bandX(double hz) const { return hzToX(hz, 20, 20000, gx, gw); }
    bool bandOn() const { return bandLow > 20.5 || bandHigh < 19500.0; }

    // A spectrum as a smooth line (Catmull-Rom through its points); the
    // path is left open for the caller to stroke or close.
    void specPath(Canvas& g, const Array<double, 360>& v, bool start) {
        double x0 = gx, y0 = specY(v[0]), x1 = gx, y1 = specY(v[0]);
        if (start)
            g.moveTo(x1, y1);
        else
            g.lineTo(x1, y1);
        for (int i = 0; i < kPoints - 1; ++i) {
            const double x2 = gx + gw * (i + 1) / (kPoints - 1.0);
            const double y2 = specY(v[i + 1]);
            const int j = min(i + 2, kPoints - 1);
            const double x3 = gx + gw * j / (kPoints - 1.0);
            const double y3 = specY(v[j]);
            g.bezierCurveTo(x1 + (x2 - x0) / 6.0, y1 + (y2 - y0) / 6.0, x2 - (x3 - x1) / 6.0,
                            y2 - (y3 - y1) / 6.0, x2, y2);
            x0 = x1;
            y0 = y1;
            x1 = x2;
            y1 = y2;
        }
    }

    void drawSpectrum(Canvas& g) {
        well(g, gx, gy, gw, gh);
        g.save();
        g.clip(gx, gy, gw, gh);
        g.lineWidth(1);
        for (int d = 1; d <= 4; ++d) {
            const double decade = pow(10.0, d);
            for (int m = 1; m < 10; ++m) {
                const double hz = decade * m;
                if (hz < 20.0 || hz > 20000.0) continue;
                const double x = bandX(hz);
                g.strokeStyle(m == 1 ? kGridLit : kGrid);
                g.globalAlpha(m == 1 || m == 2 || m == 5 ? 1.0 : 0.45);
                g.beginPath();
                g.moveTo(x + 0.5, gy);
                g.lineTo(x + 0.5, gy + gh);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        for (int k = 1; k < 8; ++k) {
            const double y = specY(-12.0 * k);
            g.strokeStyle(kGrid);
            g.beginPath();
            g.moveTo(gx, y + 0.5);
            g.lineTo(gx + gw, y + 0.5);
            g.stroke();
        }
        const int colour = familyColour(style.index());
        if (haveSpectrum) {
            // What the curve adds: where the output stands above the input.
            g.beginPath();
            for (int i = 0; i < kPoints; ++i) {
                const double x = gx + gw * i / (kPoints - 1.0);
                const double y = specY(max(showOut[i], showIn[i]));
                if (i == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            for (int i = kPoints - 1; i >= 0; --i)
                g.lineTo(gx + gw * i / (kPoints - 1.0), specY(showIn[i]));
            g.closePath();
            g.linearGradient(0, gy, 0, gy + gh, colour, 0x6A4A30);
            g.globalAlpha(0.85);
            g.fill();
            g.globalAlpha(1.0);
            g.lineJoin(LineJoin::Round);
            g.lineCap(LineCap::Round);
            g.beginPath();
            specPath(g, showIn, true);
            g.strokeStyle(kLinen);
            g.globalAlpha(0.45);
            g.lineWidth(1.2);
            g.stroke();
            g.globalAlpha(1.0);
            g.beginPath();
            specPath(g, showOut, true);
            g.strokeStyle(kCurve);
            g.lineWidth(1.6);
            g.stroke();
        }
        // The band: outside it dimmed, its edges as handles.
        const double lx = bandX(bandLow), hxp = bandX(max(bandHigh.value(), bandLow * 1.05));
        g.fillStyle(kInk);
        g.globalAlpha(0.55);
        if (bandLow > 20.5) g.fillRect(gx, gy, lx - gx, gh);
        if (bandHigh < 19500.0) g.fillRect(hxp, gy, gx + gw - hxp, gh);
        g.globalAlpha(1.0);
        g.restore();
        bandEdge(g, lx, bandLow > 20.5, dragging == 2 || nearEdge() == 1);
        bandEdge(g, hxp, bandHigh < 19500.0, dragging == 3 || nearEdge() == 2);
        // The scale.
        g.monoFont(8.5);
        g.textBaseline(TextBaseline::Middle);
        g.fillStyle(Theme::textFaint);
        g.textAlign(TextAlign::Center);
        g.fillText("100", bandX(100), gy + gh + 9);
        g.fillText("1k", bandX(1000), gy + gh + 9);
        g.fillText("10k", bandX(10000), gy + gh + 9);
    }

    // A band edge: a line with a grip, lit when it is set.
    void bandEdge(Canvas& g, double x, bool set, bool hot) {
        const int colour = familyColour(style.index());
        g.strokeStyle(set ? colour : Theme::textFaint);
        g.globalAlpha(set ? (hot ? 1.0 : 0.8) : (hot ? 0.8 : 0.35));
        g.lineWidth(hot ? 1.8 : 1.2);
        g.beginPath();
        g.moveTo(x + 0.5, gy + 4);
        g.lineTo(x + 0.5, gy + gh - 4);
        g.stroke();
        g.fillStyle(set ? colour : Theme::textFaint);
        g.beginPath();
        g.roundRect(x - 3.5, gy + 10, 8, 22, 4);
        g.fill();
        g.globalAlpha(1.0);
    }

    // 1 near the band's low edge, 2 its high edge, 0 neither.
    int nearEdge() const {
        if (dragging != 0 || hoverY < gy || hoverY > gy + gh) return 0;
        const double lx = bandX(bandLow), hxp = bandX(max(bandHigh.value(), bandLow * 1.05));
        const double dl = abs(hoverX - lx), dh = abs(hoverX - hxp);
        if (dl < 8 && dl <= dh) return 1;
        if (dh < 8) return 2;
        return 0;
    }

    void drawTop(Canvas& g) {
        panel(g, 16, 60, 1048, 272);
        heading(g, "Spectrum", 32, 80);
        aside(g, "in, out, and what the curve adds", gx + gw, 80, TextAlign::Right);
        drawSpectrum(g);
        const int colour = familyColour(style.index());
        const double lgY = 320;
        g.fillStyle(kLinen);
        g.globalAlpha(0.5);
        g.fillRect(gx + 2, lgY - 0.5, 12, 1);
        g.globalAlpha(1.0);
        label(g, "IN", gx + 19, lgY, TextAlign::Left);
        g.fillStyle(kCurve);
        g.fillRect(gx + 42, lgY - 1, 12, 2);
        label(g, "OUT", gx + 59, lgY, TextAlign::Left);
        g.fillStyle(colour);
        g.globalAlpha(0.6);
        g.fillRect(gx + 88, lgY - 3, 10, 6);
        g.globalAlpha(1.0);
        label(g, "ADDED", gx + 103, lgY, TextAlign::Left);
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Left);
        if (bandOn()) {
            label(g, "BAND", gx + 160, lgY, TextAlign::Left);
            valueStyle(g);
            g.textAlign(TextAlign::Left);
            if (bandLow > 20.5)
                fillHz(g, bandLow, gx + 192, lgY);
            else
                g.fillText("20 Hz", gx + 192, lgY);
            g.fillText("to", gx + 250, lgY);
            if (bandHigh < 19500.0)
                fillHz(g, max(bandHigh.value(), bandLow * 1.05), gx + 268, lgY);
            else
                g.fillText("20 kHz", gx + 268, lgY);
        }
        aside(g, "drag an edge in to drive only a band; drag between them to move it", gx + gw, lgY,
              TextAlign::Right);
        drawTone(g);
    }

    // --- tone and feedback --------------------------------------------------------------------

    // Knobs: 0 drive, 1 character, 2 bias, 3 dynamics, 4 motion, 5 rate,
    // 6 feedback, 7 pitch, 8 emphasis, 9 low cut, 10 high cut, 11 mix,
    // 12 output.
    double knobX(int k) const {
        switch (k) {
            case 0:
                return 452;
            case 1:
                return 408;
            case 2:
                return 496;
            case 3:
                return 592;
            case 4:
                return 604;
            case 5:
                return 700;
            case 6:
                return 804;
            case 7:
                return 902;
            case 8:
                return 804;
            case 9:
                return 902;
            case 10:
                return 1000;
            case 11:
                return 886;
            default:
                return 1006;
        }
    }
    double knobY(int k) const {
        switch (k) {
            case 0:
                return 574;
            case 1:
            case 2:
                return 702;
            case 3:
                return 428;
            case 4:
            case 5:
                return 680;
            case 6:
            case 7:
                return 268;
            case 8:
            case 9:
            case 10:
                return 144;
            default:
                return 422;
        }
    }
    double knobR(int k) const { return k == 0 ? 38.0 : (k == 11 || k == 12 ? 26.0 : 24.0); }

    void drawTone(Canvas& g) {
        heading(g, "Tone", 772, 80);
        aside(g, "around the curve", 1048, 80, TextAlign::Right);
        dial(g, knobX(8), knobY(8), knobR(8), emphasis.normalized(), kWarm, true, hotKnob == 8,
             "EMPHASIS");
        dial(g, knobX(9), knobY(9), knobR(9), lowCut.normalized(), kLinen, false, hotKnob == 9,
             "LOW CUT");
        dial(g, knobX(10), knobY(10), knobR(10), 1.0 - highCut.normalized(), kLinen, false,
             hotKnob == 10, "HIGH CUT");
        valueStyle(g);
        fillDb(g, emphasis, knobX(8), knobY(8) + 38);
        if (lowCut <= 20.5)
            g.fillText("off", knobX(9), knobY(9) + 38);
        else
            fillHz(g, lowCut, knobX(9), knobY(9) + 38);
        if (highCut >= 19500.0)
            g.fillText("off", knobX(10), knobY(10) + 38);
        else
            fillHz(g, highCut, knobX(10), knobY(10) + 38);
        heading(g, "Feedback", 772, 206);
        aside(g, "a tuned ring through the curve", 1048, 206, TextAlign::Right);
        dial(g, knobX(6), knobY(6), knobR(6), feedback.normalized(), kWild, true, hotKnob == 6,
             "FEEDBACK");
        g.globalAlpha(abs(feedback.value()) > 0.0005 ? 1.0 : 0.4);
        dial(g, knobX(7), knobY(7), knobR(7), feedbackPitch.normalized(), kWild, false,
             hotKnob == 7, "PITCH");
        g.globalAlpha(1.0);
        valueStyle(g);
        g.fillText(format("%+.0f%%", feedback.value() * 100.0), knobX(6), knobY(6) + 38);
        fillHz(g, feedbackPitch, knobX(7), knobY(7) + 38);
        // The pitch as a note, large.
        g.fillStyle(abs(feedback.value()) > 0.0005 ? kWild : Theme::textFaint);
        g.serifFont(22);
        g.textAlign(TextAlign::Center);
        fillNote(g, feedbackPitch, 1000, knobY(7) - 4);
        label(g, feedback.value() < -0.0005 ? "AN OCTAVE DOWN" : "RINGS AT", 1000, knobY(7) + 20,
              TextAlign::Center);
    }

    // --- the curve panel ----------------------------------------------------------------------

    double styleX(int k) const { return 84 + imod(k, 4) * 108; }
    double styleY(int k) const { return 380 + idiv(k, 4) * 44; }

    // A style's button, with its curve drawn small.
    void stylePill(Canvas& g, int st) {
        const double x = styleX(st), y = styleY(st), w = 104, h = 36;
        const bool lit = style.index() == st;
        const bool hot = hovered(x, y, w, h);
        const int colour = familyColour(st);
        g.fillStyle(lit ? colour : (hot ? 0x3A2F27 : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, 8);
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        // The glyph: the curve over -3..3, with its character as set.
        const double gxl = x + 8, gyl = y + 7, gs = 22;
        g.strokeStyle(lit ? kInk : colour);
        g.globalAlpha(lit ? 0.85 : 0.75);
        g.lineWidth(1.3);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int k = 0; k <= 32; ++k) {
            const double u = -3.0 + 6.0 * k / 32.0;
            const double v = clamp(curve(st, u, lit ? character.value() : 0.5), -1.4, 1.4);
            const double px = gxl + gs * k / 32.0, pyy = gyl + gs * 0.5 - v * gs * 0.34;
            if (k == 0)
                g.moveTo(px, pyy);
            else
                g.lineTo(px, pyy);
        }
        g.stroke();
        g.globalAlpha(1.0);
        g.fillStyle(lit ? kInk : Theme::textMuted);
        g.boldFont(9.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        switch (st) {
            case 0:
                g.fillText("SOFT", x + 38, y + h * 0.5 + 0.5);
                break;
            case 1:
                g.fillText("TAPE", x + 38, y + h * 0.5 + 0.5);
                break;
            case 2:
                g.fillText("TUBE", x + 38, y + h * 0.5 + 0.5);
                break;
            case 3:
                g.fillText("IRON", x + 38, y + h * 0.5 + 0.5);
                break;
            case 4:
                g.fillText("TRANSISTOR", x + 38, y + h * 0.5 + 0.5);
                break;
            case 5:
                g.fillText("DIODE", x + 38, y + h * 0.5 + 0.5);
                break;
            case 6:
                g.fillText("CLIP", x + 38, y + h * 0.5 + 0.5);
                break;
            case 7:
                g.fillText("FUZZ", x + 38, y + h * 0.5 + 0.5);
                break;
            case 8:
                g.fillText("FOLD", x + 38, y + h * 0.5 + 0.5);
                break;
            case 9:
                g.fillText("RECTIFY", x + 38, y + h * 0.5 + 0.5);
                break;
            case 10:
                g.fillText("CRUSH", x + 38, y + h * 0.5 + 0.5);
                break;
            default:
                g.fillText("ALIAS", x + 38, y + h * 0.5 + 0.5);
        }
    }

    void drawCurvePanel(Canvas& g) {
        panel(g, ax, py, aw, ph);
        heading(g, "Curve", ax + 16, py + 20);
        label(g, "WARM", ax + 16, styleY(0) + 18, TextAlign::Left);
        label(g, "BITE", ax + 16, styleY(4) + 18, TextAlign::Left);
        label(g, "WILD", ax + 16, styleY(8) + 18, TextAlign::Left);
        for (int st = 0; st < kStyles; ++st) stylePill(g, st);
        drawTransfer(g);
        drawHarmonics(g);
        const int colour = familyColour(style.index());
        dial(g, knobX(0), knobY(0), knobR(0), drive.normalized(), colour, false, hotKnob == 0,
             "DRIVE");
        dial(g, knobX(1), knobY(1), knobR(1), character.normalized(), colour, false, hotKnob == 1,
             "CHARACTER");
        dial(g, knobX(2), knobY(2), knobR(2), bias.normalized(), colour, true, hotKnob == 2,
             "BIAS");
        valueStyle(g);
        g.monoFont(12);
        fillDb(g, drive, knobX(0), knobY(0) + 52);
        g.monoFont(10.5);
        g.fillText(format("%.0f%%", character.value() * 100.0), knobX(1), knobY(1) + 38);
        if (abs(bias.value()) < 0.005)
            g.fillText("0%", knobX(2), knobY(2) + 38);
        else
            g.fillText(format("%+.0f%%", bias.value() * 100.0), knobX(2), knobY(2) + 38);
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(10.5);
        fillCharacter(g, knobX(1), knobY(1) + 52);
        // Where the drive is this moment, when something moves it.
        const double now = driveShown.get();
        if (abs(now - drive.value()) > 0.05) {
            g.fillStyle(colour);
            g.monoFont(9.5);
            g.textAlign(TextAlign::Center);
            g.fillText(format("now %+.1f dB", now), knobX(0), knobY(0) + 66);
        }
    }

    // The curve: what leaves (up) for what comes in (across), auto gain
    // included, both on one scale zoomed to where the music has been, so
    // the diagonal is unity; the music's own path over it.
    void drawTransfer(Canvas& g) {
        well(g, tx, ty, ts, ts);
        const int colour = familyColour(style.index());
        const int head = traceHead.get();
        // The zoom: just above the loudest level heard lately.
        int top = -1;
        for (int b = 0; b < kLevelBins; ++b)
            if (levels[b] > 0.002) top = b;
        const double peak = top < 0 ? 0.25 : dbToGain(-72.0 + 1.5 * (top + 1));
        const double want = clamp(peak * 1.2, 0.004, 1.5);
        if (!zoomed) zoom = want;
        zoomed = true;
        zoom += (want - zoom) * (want > zoom ? 0.5 : 0.1);
        const double cxm = tx + ts * 0.5, cym = ty + ts * 0.5;
        const double sx = ts * 0.5 / zoom, sy = sx;
        g.save();
        g.clip(tx, ty, ts, ts);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(tx, cym + 0.5);
        g.lineTo(tx + ts, cym + 0.5);
        g.moveTo(cxm + 0.5, ty);
        g.lineTo(cxm + 0.5, ty + ts);
        g.stroke();
        g.strokeStyle(kGridLit);
        g.beginPath();
        for (int k = 0; k < 24; ++k) {
            const double t0 = -1.0 + k / 12.0, t1 = t0 + 1.0 / 24.0;
            g.moveTo(cxm + t0 * ts * 0.5, cym - t0 * ts * 0.5);
            g.lineTo(cxm + t1 * ts * 0.5, cym - t1 * ts * 0.5);
        }
        g.stroke();
        // The curve at the drive as set; at the drive this moment too, faint,
        // when dynamics or motion move it.
        const double gain = dbToGain(drive.value()), b = 0.5 * bias.value();
        const double gainNow = dbToGain(driveShown.get());
        const double level = compSetShown.get(), levelNow = compShown.get();
        const double edge = 1.2 * zoom;
        const double c = character.value();
        const int st = style.index();
        const double base = st == 3 ? 0.0 : curve(st, b, c);
        // Where the music has been on the curve: a glow, densest brightest.
        g.fillStyle(colour);
        for (int k = 0; k < kLevelBins; ++k) {
            const double d = levels[k];
            if (d < 0.01) continue;
            const double a0 = dbToGain(-72.0 + 1.5 * k), a1 = dbToGain(-72.0 + 1.5 * (k + 1));
            g.globalAlpha(0.08 + 0.32 * d);
            for (int side = 0; side < 2; ++side) {
                const double sg = side == 0 ? 1.0 : -1.0;
                const double xa = cxm + sg * a0 * sx, xb = cxm + sg * a1 * sx;
                const double ya =
                    cym -
                    clamp(level * (curve(st, sg * gain * a0 + b, c) - base), -edge, edge) * sy;
                const double yb =
                    cym -
                    clamp(level * (curve(st, sg * gain * a1 + b, c) - base), -edge, edge) * sy;
                g.beginPath();
                g.moveTo(xa, ya);
                g.lineTo(xb, yb);
                g.lineWidth(4 + 8 * d);
                g.strokeStyle(colour);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        // The music's path over the last 20 ms, oldest faintest.
        g.lineJoin(LineJoin::Round);
        g.lineCap(LineCap::Round);
        g.strokeStyle(colour);
        g.lineWidth(1.4);
        for (int part = 0; part < 4; ++part) {
            g.globalAlpha(0.15 + 0.2 * part);
            g.beginPath();
            for (int j = part * 256; j <= part * 256 + 256 && j < kTrace; ++j) {
                const int at = wrap(head + j, kTrace);
                const double px = cxm + traceX[at] * sx;
                const double pyy = cym - clamp(traceY[at], -edge, edge) * sy;
                if (j == part * 256)
                    g.moveTo(px, pyy);
                else
                    g.lineTo(px, pyy);
            }
            g.stroke();
        }
        g.globalAlpha(1.0);
        if (abs(driveShown.get() - drive.value()) > 0.5) {
            g.strokeStyle(kCurve);
            g.globalAlpha(0.35);
            g.lineWidth(1);
            g.beginPath();
            for (int k = 0; k <= 160; ++k) {
                const double xin = zoom * (-1.0 + 2.0 * k / 160.0);
                const double y = levelNow * (curve(st, gainNow * xin + b, c) - base);
                const double px = cxm + xin * sx, pyy = cym - clamp(y, -edge, edge) * sy;
                if (k == 0)
                    g.moveTo(px, pyy);
                else
                    g.lineTo(px, pyy);
            }
            g.stroke();
            g.globalAlpha(1.0);
        }
        // The curve as set.
        g.strokeStyle(kCurve);
        g.lineWidth(1.6);
        g.beginPath();
        for (int k = 0; k <= 160; ++k) {
            const double xin = zoom * (-1.0 + 2.0 * k / 160.0);
            const double y = level * (curve(st, gain * xin + b, c) - base);
            const double px = cxm + xin * sx, pyy = cym - clamp(y, -edge, edge) * sy;
            if (k == 0)
                g.moveTo(px, pyy);
            else
                g.lineTo(px, pyy);
        }
        g.stroke();
        g.restore();
        label(g, "IN", tx + ts - 4, ty + ts - 8, TextAlign::Right);
        label(g, "OUT", tx + 5, ty + 9, TextAlign::Left);
        g.fillStyle(kInk);
        g.globalAlpha(0.8);
        g.beginPath();
        g.roundRect(tx + 3, ty + ts - 16, 70, 13, 6.5);
        g.fill();
        g.globalAlpha(1.0);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.fillText(format("to %.0f dBFS", gainToDb(zoom)), tx + 8, ty + ts - 9);
    }

    // The harmonics of the probe's sine: second to twelfth, the even ones in
    // the family's colour.
    void drawHarmonics(Canvas& g) {
        well(g, hx, ty, hw, ts);
        const int colour = familyColour(style.index());
        const double base = ty + ts - 18, top = ty + 26, span = base - top;
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        for (int k = 0; k <= 4; ++k) {
            const double y = base - span * k / 4.0;
            g.beginPath();
            g.moveTo(hx + 6, y + 0.5);
            g.lineTo(hx + hw - 6, y + 0.5);
            g.stroke();
        }
        const double step = (hw - 16) / 11.0;
        for (int h = 1; h < 12; ++h) {
            const double db = harmonics[h];
            const double t = clamp((db + 100.0) / 100.0, 0.0, 1.0);
            const double x = hx + 8 + (h - 1) * step;
            g.fillStyle(imod(h, 2) == 1 ? colour : kLinen);
            g.globalAlpha(imod(h, 2) == 1 ? 0.95 : 0.6);
            if (t > 0.0) {
                g.beginPath();
                g.roundRect(x + 1, base - span * t, step - 3, span * t, 1.5);
                g.fill();
            }
            g.globalAlpha(1.0);
            g.fillStyle(Theme::textFaint);
            g.monoFont(7.5);
            g.textAlign(TextAlign::Center);
            g.fillText(format("%d", h + 1), x + step * 0.5 - 0.5, base + 9);
        }
        label(g, "HARMONICS", hx + 8, ty + 11, TextAlign::Left);
        // The readouts, under the plots.
        const double ry = ty + ts + 18;
        label(g, "THD", tx, ry, TextAlign::Left);
        label(g, "EVEN", tx + 92, ry, TextAlign::Left);
        label(g, "ON THE MUSIC", hx + 30, ry, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(11);
        g.textAlign(TextAlign::Left);
        fillPercent(g, thd.value(), tx, ry + 16);
        g.fillText(format("%.0f%%", evenShare.value() * 100.0), tx + 92, ry + 16);
        g.fillStyle(colour);
        fillPercent(g, distShown.get(), hx + 30, ry + 16);
        aside(g, "a 100 Hz sine at -18 dBFS RMS", tx, ry + 32, TextAlign::Left);
    }

    // --- the movement panel ------------------------------------------------------------------

    // One hit, sketched: its envelope, and the drive dynamics gives it.
    void drawDynamics(Canvas& g) {
        const double sx0 = 632, sy0 = py + 40, sw0 = 168, sh0 = 70;
        well(g, sx0, sy0, sw0, sh0);
        const double d = dynamics.value();
        // A synthetic hit through the two followers (in ms steps).
        double fast = 0.0, slow = 0.0;
        g.save();
        g.clip(sx0, sy0, sw0, sh0);
        g.beginPath();
        g.moveTo(sx0, sy0 + sh0);
        double driveLine = 0.0;
        for (int k = 0; k <= 80; ++k) {
            const double ms = k * 5.0;
            const double level = ms < 20.0 ? 0.0 : (ms < 22.0 ? 1.0 : exp(-(ms - 22.0) / 90.0));
            fast += (level > fast ? 0.95 : 0.28) * (level - fast);
            slow += (level > slow ? 0.18 : 0.02) * (level - slow);
            const double x = sx0 + sw0 * k / 80.0;
            g.lineTo(x, sy0 + sh0 - (sh0 - 14) * level);
            driveLine = 0.0;
        }
        g.lineTo(sx0 + sw0, sy0 + sh0);
        g.closePath();
        g.fillStyle(kLinen);
        g.globalAlpha(0.18);
        g.fill();
        g.globalAlpha(1.0);
        // The drive's offset, about the middle line.
        fast = 0.0;
        slow = 0.0;
        const double mid = sy0 + sh0 * 0.5;
        g.strokeStyle(kBite);
        g.lineWidth(1.6);
        g.beginPath();
        for (int k = 0; k <= 80; ++k) {
            const double ms = k * 5.0;
            const double level = ms < 20.0 ? 0.0 : (ms < 22.0 ? 1.0 : exp(-(ms - 22.0) / 90.0));
            fast += (level > fast ? 0.95 : 0.28) * (level - fast);
            slow += (level > slow ? 0.18 : 0.02) * (level - slow);
            const double gate = clamp((gainToDb(slow) + 70.0) / 10.0, 0.0, 1.0);
            driveLine = d *
                        clamp(gainToDb(max(fast, 1e-9)) - gainToDb(max(slow, 1e-9)), -24.0, 24.0) *
                        gate;
            const double x = sx0 + sw0 * k / 80.0, y = mid - driveLine * 1.3;
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.globalAlpha(abs(d) > 0.005 ? 1.0 : 0.4);
        g.stroke();
        g.globalAlpha(1.0);
        g.restore();
        label(g, "DRIVE", sx0 + sw0 - 6, sy0 + 10, TextAlign::Right);
        aside(g,
              d > 0.005    ? "attacks driven harder"
              : d < -0.005 ? "sustain driven harder, attacks clean"
                           : "the drive holds still",
              sx0 + sw0, sy0 + sh0 + 12, TextAlign::Right);
    }

    // The LFO's wave over two cycles, a dot where it is: it crosses both,
    // and the wave is drawn again as it wraps, every second cycle.
    void drawWave(Canvas& g) {
        well(g, wx, wy, ww, wh);
        const int shape = motionShape.index();
        const double depth = motion.value();
        const int cycle = lfoCycleShown.get();
        const int first = 2 * floorToInt(cycle * 0.5);
        g.save();
        g.clip(wx, wy, ww, wh);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(wx, wy + wh * 0.5 + 0.5);
        g.lineTo(wx + ww, wy + wh * 0.5 + 0.5);
        g.moveTo(wx + ww * 0.5 + 0.5, wy);
        g.lineTo(wx + ww * 0.5 + 0.5, wy + wh);
        g.stroke();
        g.strokeStyle(kWild);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.globalAlpha(depth > 0.0005 ? 1.0 : 0.4);
        g.beginPath();
        for (int k = 0; k <= 200; ++k) {
            const double at = 2.0 * k / 200.0;
            const int n = min(floorToInt(at), 1);
            const double v = lfoShape(shape, at - n, first + n);
            const double x = wx + ww * k / 200.0, y = wy + wh * 0.5 - v * (wh * 0.5 - 7);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        if (depth > 0.0005) {
            const double ph = lfoPhaseShown.get();
            g.fillStyle(kCurve);
            g.beginPath();
            g.circle(wx + ww * 0.5 * (cycle - first + ph),
                     wy + wh * 0.5 - lfoShown.get() * (wh * 0.5 - 7), 3.5);
            g.fill();
        }
        g.globalAlpha(1.0);
        g.restore();
    }

    // The wave buttons: each its shape, small.
    void shapeGlyph(Canvas& g, int shape, double x, double y, double w, double h, bool lit) {
        g.strokeStyle(lit ? kInk : Theme::textMuted);
        g.lineWidth(1.3);
        g.beginPath();
        for (int k = 0; k <= 24; ++k) {
            const double ph = k / 24.0;
            const double v = lfoShape(shape, ph, shape == 5 || shape == 6 ? 3 : 0);
            const double v2 = shape == 5 ? (ph < 0.5 ? 0.6 : -0.4) : v;
            const double px = x + w * ph, pyy = y + h * 0.5 - v2 * h * 0.36;
            if (k == 0)
                g.moveTo(px, pyy);
            else
                g.lineTo(px, pyy);
        }
        g.stroke();
    }

    double shapeX(int k) const { return wx + k * 34; }
    static constexpr double kShapeY = wy + wh + 8;
    double targetX(int k) const { return wx + k * 60; }
    static constexpr double kTargetY = wy + wh + 34;

    void drawMovement(Canvas& g) {
        panel(g, mvx, py, mvw, ph);
        heading(g, "Movement", mvx + 16, py + 20);
        dial(g, knobX(3), knobY(3), knobR(3), dynamics.normalized(), kBite, true, hotKnob == 3,
             "DYNAMICS");
        valueStyle(g);
        if (abs(dynamics.value()) < 0.005)
            g.fillText("0%", knobX(3), knobY(3) + 38);
        else
            g.fillText(format("%+.0f%%", dynamics.value() * 100.0), knobX(3), knobY(3) + 38);
        drawDynamics(g);
        label(g, "MOTION", wx, wy - 12, TextAlign::Left);
        aside(g, "an LFO, free or in time", wx + ww, wy - 12, TextAlign::Right);
        drawWave(g);
        for (int k = 0; k < 7; ++k) {
            const double x = shapeX(k);
            const bool lit = motionShape.index() == k;
            const bool hot = hovered(x, kShapeY, 32, 20);
            g.fillStyle(lit ? kWild : (hot ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(x, kShapeY, 32, 20, 10);
            g.fill();
            if (!lit) {
                g.strokeStyle(Theme::line);
                g.lineWidth(1);
                g.stroke();
            }
            shapeGlyph(g, k, x + 7, kShapeY + 3, 18, 14, lit);
        }
        for (int k = 0; k < 4; ++k) {
            const double x = targetX(k);
            const char* name = k == 0 ? "DRIVE" : (k == 1 ? "CHAR" : (k == 2 ? "BIAS" : "PITCH"));
            pill(g, x, kTargetY, 56, 20, name, motionTarget.index() == k, kWild,
                 hovered(x, kTargetY, 56, 20));
        }
        const bool synced = motionSync.index() > 0;
        dial(g, knobX(4), knobY(4), knobR(4), motion.normalized(), kWild, false, hotKnob == 4,
             "DEPTH");
        g.globalAlpha(synced ? 0.4 : 1.0);
        dial(g, knobX(5), knobY(5), knobR(5), motionRate.normalized(), kWild, false, hotKnob == 5,
             "RATE");
        g.globalAlpha(1.0);
        valueStyle(g);
        g.fillText(format("%.0f%%", motion.value() * 100.0), knobX(4), knobY(4) + 38);
        g.globalAlpha(synced ? 0.4 : 1.0);
        if (motionRate < 1.0)
            g.fillText(format("%.2f Hz", motionRate.value()), knobX(5), knobY(5) + 38);
        else
            g.fillText(format("%.1f Hz", motionRate.value()), knobX(5), knobY(5) + 38);
        g.globalAlpha(1.0);
        // The sync: a stepper, its arrows each side.
        const double sx0 = 744, sy0 = knobY(5) - 22, sw0 = 60;
        label(g, "SYNC", sx0 + sw0 * 0.5, sy0 - 12, TextAlign::Center);
        g.fillStyle(synced ? kWild : Theme::raised);
        g.beginPath();
        g.roundRect(sx0, sy0, sw0, 22, 11);
        g.fill();
        if (!synced) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(synced ? kInk : Theme::textMuted);
        g.boldFont(9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        fillSync(g, motionSync.index(), sx0 + sw0 * 0.5, sy0 + 11.5);
        g.globalAlpha(0.6);
        g.fillText("<", sx0 + 8, sy0 + 11);
        g.fillText(">", sx0 + sw0 - 8, sy0 + 11);
        g.globalAlpha(1.0);
        aside(g, synced ? "to the song" : "free", sx0 + sw0 * 0.5, sy0 + 34, TextAlign::Center);
    }

    // --- the output panel --------------------------------------------------------------------

    void drawOutput(Canvas& g) {
        panel(g, ox, py, ow, ph);
        heading(g, "Output", ox + 16, py + 20);
        dial(g, knobX(11), knobY(11), knobR(11), mix.normalized(), kLinen, false, hotKnob == 11,
             "MIX");
        dial(g, knobX(12), knobY(12), knobR(12), output.normalized(), kLinen, true, hotKnob == 12,
             "OUTPUT");
        valueStyle(g);
        g.fillText(format("%.0f%%", mix.value() * 100.0), knobX(11), knobY(11) + 40);
        fillDb(g, output, knobX(12), knobY(12) + 40);
        const double x = ox + 16, w = ow - 32;
        pill(g, x, py + 136, w, 22, autoGain.on() ? "AUTO GAIN ON" : "AUTO GAIN OFF", autoGain.on(),
             kLinen, hovered(x, py + 136, w, 22));
        label(g, "CHANNELS", x, py + kChannelsY - 10, TextAlign::Left);
        for (int k = 0; k < 3; ++k) {
            const double px = x + k * (w + 4) / 3.0;
            const char* name = k == 0 ? "STEREO" : (k == 1 ? "MID" : "SIDE");
            pill(g, px, py + kChannelsY, (w + 4) / 3.0 - 4, 22, name, channelMode.index() == k,
                 kLinen, hovered(px, py + kChannelsY, (w + 4) / 3.0 - 4, 22));
        }
        label(g, "OVERSAMPLING", x, py + kOverY - 10, TextAlign::Left);
        for (int k = 0; k < 5; ++k) {
            const double px = x + k * (w + 4) / 5.0;
            const char* name =
                k == 0 ? "1x" : (k == 1 ? "2x" : (k == 2 ? "4x" : (k == 3 ? "8x" : "16x")));
            pill(g, px, py + kOverY, (w + 4) / 5.0 - 4, 22, name, quality.index() == k, kLinen,
                 hovered(px, py + kOverY, (w + 4) / 5.0 - 4, 22));
        }
        label(g, "PHASE", x, py + kPhaseY - 10, TextAlign::Left);
        for (int k = 0; k < 2; ++k) {
            const double px = x + k * (w + 4) / 2.0;
            pill(g, px, py + kPhaseY, (w + 4) / 2.0 - 4, 22, k == 0 ? "MINIMUM" : "LINEAR",
                 phase.index() == k, kLinen, hovered(px, py + kPhaseY, (w + 4) / 2.0 - 4, 22));
        }
        label(g, "LISTEN", x, py + kListenY - 10, TextAlign::Left);
        for (int k = 0; k < 3; ++k) {
            const double px = x + k * (w + 4) / 3.0;
            const char* name = k == 0 ? "OFF" : (k == 1 ? "BAND" : "DELTA");
            pill(g, px, py + kListenY, (w + 4) / 3.0 - 4, 22, name, listen.index() == k,
                 k == 0 ? kLinen : Theme::accent,
                 hovered(px, py + kListenY, (w + 4) / 3.0 - 4, 22));
        }
        const char* says = listen.index() == 1   ? "playing the driven band alone"
                           : listen.index() == 2 ? "playing only what the drive changes"
                           : autoGain.on()       ? "level held as the drive moves"
                                                 : "level rises with the drive";
        aside(g, says, x, py + kListenY + 38, TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Left);
        g.fillText(format("latency %d samples, %.2f ms", latencyShown.get(),
                          1000.0 * latencyShown.get() / rateNow()),
                   x, py + ph - 22);
    }

    void draw(Canvas& g) {
        analyse();
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawTop(g);
        drawCurvePanel(g);
        drawMovement(g);
        drawOutput(g);
    }

    // --- the pointer ---------------------------------------------------------------------------

    // how: 0 a value in its unit, 1 a knob position (0..1), 2 the default.
    void act(int how, const Param& p, double v) {
        if (how == 0)
            p.edit(v);
        else if (how == 1)
            p.editNormalized(v);
        else
            p.reset();
    }

    double knobValue(int k) const {
        switch (k) {
            case 0:
                return drive.normalized();
            case 1:
                return character.normalized();
            case 2:
                return bias.normalized();
            case 3:
                return dynamics.normalized();
            case 4:
                return motion.normalized();
            case 5:
                return motionRate.normalized();
            case 6:
                return feedback.normalized();
            case 7:
                return feedbackPitch.normalized();
            case 8:
                return emphasis.normalized();
            case 9:
                return lowCut.normalized();
            case 10:
                // The high cut turns the other way: clockwise closes it.
                return 1.0 - highCut.normalized();
            case 11:
                return mix.normalized();
            default:
                return output.normalized();
        }
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, drive, v);
        if (k == 1) act(how, character, v);
        if (k == 2) act(how, bias, v);
        if (k == 3) act(how, dynamics, v);
        if (k == 4) act(how, motion, v);
        if (k == 5) act(how, motionRate, v);
        if (k == 6) act(how, feedback, v);
        if (k == 7) act(how, feedbackPitch, v);
        if (k == 8) act(how, emphasis, v);
        if (k == 9) act(how, lowCut, v);
        if (k == 10) act(how, highCut, how == 1 ? 1.0 - v : v);
        if (k == 11) act(how, mix, v);
        if (k == 12) act(how, output, v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 13; ++k) {
            const double dx = x - knobX(k), dy = y - knobY(k);
            const double r = knobR(k) + 6;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotKnob = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p);
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            } else if (dragging == 2) {
                bandLow.edit(min(xToHz(p.x(), 20, 20000, gx, gw), bandHigh / 1.05));
            } else if (dragging == 3) {
                bandHigh.edit(max(xToHz(p.x(), 20, 20000, gx, gw), bandLow * 1.05));
            } else if (dragging == 4) {
                // Both edges together, by octaves, kept on the scale.
                double shift = (p.x() - pressX) / gw * log2(1000.0);
                const double lo = log2(pressLow), hi2 = log2(pressHigh);
                shift = clamp(shift, log2(20.0) - lo, log2(20000.0) - hi2);
                bandLow.edit(exp2(lo + shift));
                bandHigh.edit(exp2(hi2 + shift));
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            if (p.over(tx, ty, ts, ts)) {
                drive.edit(drive + notches * 0.5 * fine);
                return;
            }
            if (p.over(744, knobY(5) - 22, 60, 22)) {
                act(0, motionSync, clamp(motionSync.index() + (notches > 0 ? 1 : -1), 0, 13));
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        const int k = knobAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                setKnob(k, 2, 0);
                return;
            }
            dragging = 1;
            dragKnob = k;
            knobPos = knobValue(k);
            return;
        }
        // (The edges sit on the plot's borders when off: take a little
        // either side.)
        if (p.over(gx - 8, gy, gw + 16, gh)) {
            const double lx = bandX(bandLow), hxp = bandX(max(bandHigh.value(), bandLow * 1.05));
            const double dl = abs(x - lx), dh = abs(x - hxp);
            if (dl < 8 && dl <= dh) {
                if (p.doubleClick())
                    bandLow.reset();
                else
                    dragging = 2;
                return;
            }
            if (dh < 8) {
                if (p.doubleClick())
                    bandHigh.reset();
                else
                    dragging = 3;
                return;
            }
            if (bandOn() && x > lx && x < hxp) {
                dragging = 4;
                pressX = x;
                pressLow = bandLow;
                pressHigh = max(bandHigh.value(), bandLow * 1.05);
            }
            return;
        }
        for (int st = 0; st < kStyles; ++st)
            if (p.over(styleX(st), styleY(st), 104, 36)) {
                act(0, style, st);
                return;
            }
        for (int c = 0; c < 7; ++c)
            if (p.over(shapeX(c), kShapeY, 32, 20)) {
                act(0, motionShape, c);
                return;
            }
        for (int c = 0; c < 4; ++c)
            if (p.over(targetX(c), kTargetY, 56, 20)) {
                act(0, motionTarget, c);
                return;
            }
        if (p.over(744, knobY(5) - 22, 60, 22) || p.over(744, knobY(5) + 2, 60, 22)) {
            const int step = x < 774 ? -1 : 1;
            act(0, motionSync, clamp(motionSync.index() + step, 0, 13));
            return;
        }
        const double ox0 = ox + 16, w = ow - 32;
        if (p.over(ox0, py + 136, w, 22)) {
            act(0, autoGain, autoGain.on() ? 0 : 1);
            return;
        }
        for (int c = 0; c < 3; ++c)
            if (p.over(ox0 + c * (w + 4) / 3.0, py + kChannelsY, (w + 4) / 3.0 - 4, 22)) {
                act(0, channelMode, c);
                return;
            }
        for (int c = 0; c < 5; ++c)
            if (p.over(ox0 + c * (w + 4) / 5.0, py + kOverY, (w + 4) / 5.0 - 4, 22)) {
                act(0, quality, c);
                return;
            }
        for (int c = 0; c < 2; ++c)
            if (p.over(ox0 + c * (w + 4) / 2.0, py + kPhaseY, (w + 4) / 2.0 - 4, 22)) {
                act(0, phase, c);
                return;
            }
        for (int c = 0; c < 3; ++c)
            if (p.over(ox0 + c * (w + 4) / 3.0, py + kListenY, (w + 4) / 3.0 - 4, 22)) {
                act(0, listen, c);
                return;
            }
    }
};
