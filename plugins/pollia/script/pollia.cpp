// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Pollia: a phaser, from a slow four-stage swirl to a resonant, endlessly
// rising barber pole.
//
// The marble berry (Pollia condensata) is blue without a blue pigment: its
// colour is light interfering with itself, layers of cells delaying part of it
// against the rest. A phaser does that to sound. A chain of all-pass stages
// delays each frequency by its own amount; mixed with the dry sound, the
// frequencies that come out half a cycle late cancel, leaving notches, and
// moving the stages moves the notches.
//
//   Stages   1 to 24 trapezoidal all-passes, their corners spread across up
//            to 6 octaves (staggered, as in a photocell vibe) or all at one.
//   Feedback the chain's output back into it: positive sharpens peaks
//            between the notches, negative moves them.
//   Motion   sweep: an LFO (free or in time, six shapes) moves the corners;
//            rise and fall: the corners stay, and the wet sound's phase
//            turns against the dry's (a 90-degree pair, as in a frequency
//            shifter), so every notch slides up or down and the next takes
//            its place, forever and at full depth: a barber pole.
//   Envelope the playing moves the corners too, measured against its own
//            recent peak, so quiet and loud tracks both reach the range.
//   Response direct, or a photocell: the motion lights a lamp and the cell
//            follows the light, quick to brighten and slow to darken.
//
// Below a frequency the sound may pass untouched (keep below); the mix runs
// from dry through the deepest notches (50%) to the wet alone, a vibrato.

constexpr int kStages = 24;  // the most stages
constexpr int kTick = 8;     // frames between the corners' targets
constexpr int kPair = 7;     // sections in each path of the 90-degree pair
constexpr double kLowest = 5.0;
constexpr int kRows = 96;  // the editor's history: 4 s in rows of 1/24 s

// The 90-degree pair: two chains of first-order all-passes whose phases differ
// by 90 degrees (within 0.34) over four decades. Corners in the warped domain
// u = tan(pi f / fs), relative to the band's centre (designed for this plugin
// by a minimax fit; the band covers 10 Hz to 20 kHz at 44.1 kHz and wider at
// higher rates).
double pairCorner(int path, int k) {
    if (path == 0) {
        switch (k) {
            case 0:
                return 0.00386157818;
            case 1:
                return 0.0322772304;
            case 2:
                return 0.15021365;
            case 3:
                return 0.684616762;
            case 4:
                return 3.11701816;
            case 5:
                return 14.2630046;
            default:
                return 71.9686872;
        }
    }
    switch (k) {
        case 0:
            return 0.013894933;
        case 1:
            return 0.0701114444;
        case 2:
            return 0.320819339;
        case 3:
            return 1.46067074;
        case 4:
            return 6.65718302;
        case 5:
            return 30.9815916;
        default:
            return 258.961523;
    }
}

// Quarter notes per cycle of each synced rate (0: free).
double syncBeats(int k, double barBeats) {
    switch (k) {
        case 1:
            return 8.0 * barBeats;
        case 2:
            return 4.0 * barBeats;
        case 3:
            return 2.0 * barBeats;
        case 4:
            return barBeats;
        case 5:
            return 2.0;
        case 6:
            return 1.0;
        case 7:
            return 0.5;
        case 8:
            return 0.25;
        case 9:
            return 3.0;
        case 10:
            return 1.5;
        case 11:
            return 0.75;
        case 12:
            return 4.0 / 3.0;
        case 13:
            return 2.0 / 3.0;
        default:
            return 1.0 / 3.0;
    }
}

// A smooth random value per cycle, the same for a cycle wherever it plays.
double hashUnit(int n) {
    double x = wrap(n, 65521) + 17.0;
    for (int k = 0; k < 4; ++k) x = fmod(x * x + 12345.0 + k, 65521.0);
    return x / 32760.0 - 1.0;
}

// The LFO's shapes, from its phase (0..1) and the cycle it is in: -1..1.
// Every corner is rounded, so no shape jumps.
double lfoShape(int shape, double ph, int cycle) {
    switch (shape) {
        case 0:
            return sin(twoPi * ph);
        case 1:
            return ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0);
        case 2:
        case 3: {
            // A ramp whose return takes the last 4% of the cycle, as a cosine.
            const double e = 0.04;
            double v = ph < 1.0 - e ? -1.0 + 2.0 * ph / (1.0 - e) : cos(pi * (ph - 1.0 + e) / e);
            return shape == 2 ? v : -v;
        }
        case 4: {
            // A value held each cycle, gliding to it over the first 6%.
            const double a = hashUnit(cycle - 1), b = hashUnit(cycle);
            const double e = 0.06;
            if (ph >= e) return b;
            return a + (b - a) * (0.5 - 0.5 * cos(pi * ph / e));
        }
        default: {
            const double a = hashUnit(cycle), b = hashUnit(cycle + 1);
            return a + (b - a) * (0.5 - 0.5 * cos(pi * ph));
        }
    }
}

// What the loop feeds back: untouched to -4.4 dBFS, then easing toward full
// scale, so a loop driven hard rounds over instead of running away.
double guard(double x) {
    const double a = abs(x);
    if (a <= 0.6) return x;
    return sign(x) * (0.6 + 0.4 * tanh((a - 0.6) / 0.4));
}

// One all-pass stage, trapezoidal: its corner (as G = g / (1 + g), g =
// tan(pi f / fs)) may move every frame without zipper or blow-up.
struct Stage {
    double s = 0.0;
    double process(double x, double G) {
        const double v = (x - s) * G;
        const double lp = v + s;
        s = lp + v;
        return 2.0 * lp - x;
    }
};

// One path of the 90-degree pair.
struct Path {
    Array<Stage, 7> stage;
    Array<double, 7> G;
    void tune(int path, double centre) {
        for (int k = 0; k < kPair; ++k) {
            const double g = centre * pairCorner(path, k);
            G[k] = g / (1.0 + g);
        }
    }
    double process(double x) {
        double y = x;
        for (int k = 0; k < kPair; ++k) y = stage[k].process(y, G[k]);
        return y;
    }
    void clear() {
        for (int k = 0; k < kPair; ++k) stage[k].s = 0.0;
    }
};

// A channel's phaser: the stages, their corners gliding from tick to tick,
// the loop, the pair for rise and fall, and the photocell.
struct Side {
    Array<Stage, 24> stage;
    Array<double, 24> G, step;  // each corner now, and its step per frame
    Array<double, 24> place;    // each stage's octaves from the centre, gliding
    double fed = 0.0;           // the chain's last output, fed back
    Path dryPath, wetI, wetQ;   // the pair: the dry's reference, the wet's two
    double cell = 1.0;          // the photocell's conductance (1: the range's top)
    double lamp = 1.0;          // the lamp's light
    double octaves = 0.0;       // the centre's offset now, octaves
    bool turning = false;       // the pair has run since it was cleared
    Biquad split;

    void clear() {
        for (int k = 0; k < kStages; ++k) stage[k].s = 0.0;
        fed = 0.0;
        dryPath.clear();
        wetI.clear();
        wetQ.clear();
        split.clear();
        turning = false;
    }
};

// --- the look ----------------------------------------------------------------------------

constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;
constexpr int kGraphBottom = 0x130F0C;
constexpr int kGrid = 0x2B231D;
constexpr int kGridLit = 0x3E332A;
constexpr int kLinen = 0xE3D5BE;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kStem = 0x7E6A4E;
constexpr int kBerry = 0x2C3550;  // the berry's dark body
constexpr int kBerryLight = 0x46557A;
constexpr int kSheenBlue = 0x8FB0C8;  // its structural colour, shifting
constexpr int kSheenHeather = 0xB59BCB;
constexpr int kSheenSage = 0x9CC2A8;
constexpr int kLeft = 0xA3B27C;   // sage: the left side's notches
constexpr int kRight = 0xB59BCB;  // heather: the right's
constexpr int kPeak = 0xD97757;   // clay: the peaks feedback raises
constexpr int kLamp = 0xE8B85C;   // the photocell's lamp
constexpr int kEnvelope = 0xD8B46A;

// A colour between two, t of the way from a to b.
int blend(int a, int b, double t) {
    const double u = clamp(t, 0.0, 1.0);
    const int ar = idiv(a, 65536), ag = imod(idiv(a, 256), 256), ab = imod(a, 256);
    const int br = idiv(b, 65536), bg = imod(idiv(b, 256), 256), bb = imod(b, 256);
    const int r = roundToInt(ar + (br - ar) * u), g = roundToInt(ag + (bg - ag) * u);
    const int bl = roundToInt(ab + (bb - ab) * u);
    return r * 65536 + g * 256 + bl;
}

struct Pollia : Effect {
    static constexpr Info info = {
        .name = "Pollia",
        .id = "com.suppleaudio.pollia",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A phaser of 1 to 24 stages with feedback and stage spread, swept by an LFO (free or "
            "in time) and the playing's envelope, through a photocell if wanted, or turning "
            "forever as a barber pole, with a stereo offset, a low end kept clean and a mix from "
            "dry through notches to vibrato.",
        .features = {"phaser"}};

    // --- the stages ---------------------------------------------------------------------------

    Param stages = integer("Stages", 1, 24, 6)
                       .in("Stages")
                       .help("All-pass stages in the chain: every two make one notch.");
    Param centre = frequency("Centre", 30, 10000, 700)
                       .modulated()
                       .smoothed(30)
                       .in("Stages")
                       .help("Where the stages sit at the middle of the sweep.");
    Param spread = octaves("Spread", 0, 6, 0)
                       .modulated()
                       .smoothed(60)
                       .in("Stages")
                       .help(
                           "Staggers the stages over this many octaves: 0 stacks them for even, "
                           "deep notches; more spaces the notches unevenly, as in a vibe.");
    Param feedback = percent("Feedback", 0, -95, 95)
                         .modulated()
                         .smoothed(20)
                         .in("Stages")
                         .help(
                             "The chain's output fed back into it: positive rings at the peaks "
                             "between notches, negative moves the peaks between them.");

    // --- the motion ---------------------------------------------------------------------------

    Param motion = choice("Motion", {"Sweep", "Rise", "Fall"}, 0)
                       .in("Motion")
                       .help(
                           "Sweep moves the stages back and forth; rise and fall turn the "
                           "phase so the notches climb or sink forever (a barber pole).");
    Param range = octaves("Range", 0, 8, 3)
                      .modulated()
                      .smoothed(30)
                      .in("Motion")
                      .activeWhen(motion, {"Sweep"})
                      .help("How far the sweep moves the stages, top to bottom, in octaves.");
    Param sync = choice("Sync",
                        {"Free", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16",
                         "1/2 D", "1/4 D", "1/8 D", "1/2 T", "1/4 T", "1/8 T"},
                        0)
                     .keys({"free", "bars8", "bars4", "bars2", "bar1", "n2", "n4", "n8", "n16",
                            "n2d", "n4d", "n8d", "n2t", "n4t", "n8t"})
                     .in("Motion")
                     .help("A cycle in note values, locked to the song while it plays, or free.");
    Param rate = frequency("Rate", 0.01, 20, 0.3)
                     .modulated()
                     .smoothed(50)
                     .in("Motion")
                     .activeWhen(sync, {"Free"})
                     .help(
                         "Cycles a second: sweeps there and back, or (rising and falling) "
                         "notches passing each place.");
    Param shape =
        choice("Shape", {"Sine", "Triangle", "Ramp Up", "Ramp Down", "Steps", "Drift"}, 0)
            .in("Motion")
            .activeWhen(motion, {"Sweep"})
            .help(
                "The sweep's wave: steps jump to a new place each cycle (gliding briefly), drift "
                "wanders between them.");
    Param phase = degrees("Phase", 0, 360, 0)
                      .modulated()
                      .smoothed(50)
                      .in("Motion")
                      .help(
                          "Moves the motion along its cycle: synced, where it is at each bar's "
                          "start.");
    Param stereo = degrees("Stereo", 0, 180, 0)
                       .modulated()
                       .smoothed(50)
                       .in("Motion")
                       .help(
                           "The right side's motion ahead of the left's: 90 a wide swirl, 180 "
                           "the right's notches between the left's.");
    Param response = choice("Response", {"Direct", "Photocell"}, 0)
                         .in("Motion")
                         .help(
                             "Direct moves the stages exactly; photocell lights a lamp that a "
                             "cell follows, quick up and slow down, for a lopsided throb.");

    // --- the envelope -------------------------------------------------------------------------

    Param envelope = octaves("Envelope", -4, 4, 0)
                         .modulated()
                         .smoothed(30)
                         .in("Envelope")
                         .help(
                             "How far the playing moves the stages, up or down, at its loudest "
                             "against its own recent peak.");
    Param attack = milliseconds("Attack", 1, 500, 10)
                       .modulated()
                       .in("Envelope")
                       .help("How fast the envelope follows a louder sound.");
    Param release = milliseconds("Release", 10, 3000, 200)
                        .modulated()
                        .in("Envelope")
                        .help("How fast it falls back.");

    // --- output -------------------------------------------------------------------------------

    Param keepBelow = frequency("Keep Below", 20, 1000, 20)
                          .modulated()
                          .smoothed(30)
                          .in("Output")
                          .help("Frequencies below this pass untouched (20 Hz: off).");
    Param mix = percent("Mix", 50).modulated().smoothed(20).in("Output").help(
        "Dry to wet: 50% gives the deepest notches, 100% the wet alone, a "
        "vibrato.");
    Param output =
        decibels("Output", -24, 24, 0).modulated().smoothed(20).in("Output").help("The level out.");

    Param inputLevel = decibels("Input Level", -96, 24, -96)
                           .meter()
                           .help("The input's peak, dBFS, falling 20 dB a second.");
    Param outputLevel = decibels("Output Level", -96, 24, -96)
                            .meter()
                            .help("The output's peak, dBFS, falling 20 dB a second.");
    Param centreNow = frequency("Centre Now", 1, 40000, 700)
                          .meter()
                          .help("Where the left side's stages sit this moment.");
    Param envelopeNow = percent("Envelope Now", 0)
                            .meter()
                            .help("The envelope this moment: 100% at the recent peak.");

    void factoryPatches(Patches& p) {
        p.add("Swirl");
        p.add("Classic Four")
            .set(stages, 4)
            .set(centre, 600)
            .set(range, 3.5)
            .set(rate, 0.5)
            .set(shape, "triangle");
        p.add("Resonant Sweep")
            .set(stages, 8)
            .set(centre, 900)
            .set(range, 3)
            .set(feedback, 70)
            .set(rate, 0.2);
        p.add("Hollow Twelve")
            .set(stages, 12)
            .set(centre, 800)
            .set(range, 2.5)
            .set(feedback, -55)
            .set(rate, 0.12)
            .set(stereo, 90);
        p.add("Photocell Throb")
            .set(stages, 4)
            .set(spread, 3)
            .set(centre, 900)
            .set(range, 3)
            .set(rate, 4.5)
            .set(feedback, 15)
            .set(response, "photocell");
        p.add("Lamp Vibrato")
            .set(stages, 4)
            .set(spread, 3)
            .set(centre, 1000)
            .set(range, 1.5)
            .set(rate, 6)
            .set(response, "photocell")
            .set(mix, 100)
            .set(output, -2);
        p.add("Endless Rise")
            .set(motion, "rise")
            .set(stages, 12)
            .set(spread, 4)
            .set(centre, 1200)
            .set(rate, 0.12)
            .set(feedback, 40);
        p.add("Falling Spiral")
            .set(motion, "fall")
            .set(stages, 16)
            .set(spread, 5)
            .set(centre, 1000)
            .set(rate, 0.2)
            .set(feedback, 50)
            .set(stereo, 180);
        p.add("Envelope Funk")
            .set(stages, 6)
            .set(centre, 250)
            .set(range, 0)
            .set(feedback, 55)
            .set(envelope, 3.5)
            .set(attack, 4)
            .set(release, 150);
        p.add("Two-Bar Sweep")
            .set(sync, "bars2")
            .set(stages, 8)
            .set(centre, 700)
            .set(range, 4)
            .set(feedback, 35)
            .set(shape, "triangle");
        p.add("Stepped Notches")
            .set(sync, "n8")
            .set(shape, "steps")
            .set(stages, 8)
            .set(centre, 900)
            .set(range, 4)
            .set(feedback, 45)
            .set(stereo, 90);
        p.add("Bass Phase")
            .set(stages, 4)
            .set(centre, 500)
            .set(range, 2.5)
            .set(rate, 0.4)
            .set(feedback, 30)
            .set(keepBelow, 150);
    }

    // --- the audio's own ----------------------------------------------------------------------

    Array<Side, 2> ch;
    double sr = 48000.0;
    bool unplayed = true;
    double lfoPhase = 0.0;  // 0..1, the left's
    int lfoCycle = 0;
    double turn = 0.0;  // rise and fall: the wet's phase against the dry's, cycles
    int tick = 0;
    int ran = 6;  // stages running
    int fromStages = 6, toStages = 6;
    double stageFade = 1.0;  // 0..1 from the old count to the new
    double spin = 0.0;       // 0 sweep .. 1 rise or fall, crossfading
    double follow = 0.0;     // the envelope's level, linear
    double peakRef = -60.0;  // dBFS: the recent peak it is measured against
    double envNow = 0.0;     // 0..1
    double inPeak = 0.0, outPeak = 0.0;
    double lastKeep = 0.0;  // the cutoff last designed on both channels
    // The editor's history: frames into the row, its newest row, rows written.
    int rowAt = 0, head = 0, rows = 0;

    // What the editor shows, from the audio: levels, the motion now, and the
    // last four seconds in rows of 1/24 s (each side's centre in octaves, the
    // turn, the barber pole's share and the envelope).
    Shared<double> inShown, outShown, rateShown, lfoShown, cellShown, spinShown, turnShown;
    Shared<double> envShown, hzShown;
    Shared<int> headShown, rowsShown;
    SharedArray<float, 96> histLeft, histRight, histTurn, histSpin, histEnv;

    void prepare() {
        sr = sampleRate();
        const double uc = sqrt(tan(pi * 10.0 / sr) * tan(pi * min(20000.0, 0.45 * sr) / sr));
        for (int c = 0; c < 2; ++c) {
            Side& h = ch[c];
            h.clear();
            h.dryPath.tune(0, uc);
            h.wetI.tune(0, uc);
            h.wetQ.tune(1, uc);
            for (int k = 0; k < kStages; ++k) {
                h.G[k] = 0.5;
                h.step[k] = 0.0;
                h.place[k] = 0.0;
            }
            h.cell = 1.0;
            h.lamp = 1.0;
            h.octaves = 0.0;
        }
        unplayed = true;
        lfoPhase = 0.0;
        lfoCycle = 0;
        turn = 0.0;
        tick = 0;
        ran = stages.index();
        fromStages = ran;
        toStages = ran;
        stageFade = 1.0;
        spin = motion.index() > 0 ? 1.0 : 0.0;
        follow = 0.0;
        peakRef = -60.0;
        envNow = 0.0;
        inPeak = 0.0;
        outPeak = 0.0;
        lastKeep = 0.0;
        rowAt = 0;
        head = 0;
        rows = 0;
        tailNow();
    }

    void update() {
        const int want = stages.index();
        if (unplayed) {
            fromStages = want;
            toStages = want;
            stageFade = 1.0;
        } else if (want != toStages) {
            fromStages = stageFade < 1.0 ? (stageFade < 0.5 ? fromStages : toStages) : toStages;
            toStages = want;
            stageFade = 0.0;
        }
        tailNow();
    }

    // The tail: the loop's ring at its slowest stage, to 60 dB down.
    void tailNow() {
        const double lowest =
            max(kLowest, centre.value() * exp2(-0.5 * range.value() - 0.5 * spread.value() -
                                               abs(envelope.value())));
        const double perPass = max(1, stages.index()) / (twoPi * lowest);
        const double fb = abs(feedback.value());
        const double passes = fb > 0.001 ? 6.9 / -log(fb) : 1.0;
        setTail(toInt(min(10.0, passes * perPass + 0.05) * sr));
    }

    // The LFO (or the turn) for frame i: its cycle position, the left's.
    double cyclePos(const Block& io, int i) {
        const int s = sync.index();
        const Transport& t = io.transport;
        const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        if (s > 0 && t.playing && t.tempo > 0.0) {
            const double at = (t.beats + i * t.tempo / (60.0 * sr)) / syncBeats(s, barBeats);
            return at;
        }
        double hz = rate.at(i);
        if (s > 0) hz = (t.tempo > 0.0 ? t.tempo : 120.0) / 60.0 / syncBeats(s, barBeats);
        return -1.0 - hz / sr;  // a step, marked negative
    }

    // The corner targets for channel c at frame i: octaves from the centre.
    // Stages from `fresh` on (all on the first frame) are put there at once;
    // the lamp and the stages' places move on only at the ticks (`advance`).
    void target(int c, int i, double sweepPos, double depth, double env, int run, int fresh,
                bool advance) {
        const bool snap = fresh == 0;
        Side& h = ch[c];
        const double r = range.at(i);
        double oct = 0.0;
        if (response.index() == 1) {
            // The lamp: brightness follows the motion linearly; the cell's
            // conductance follows the light, quick up and slow down.
            const double floorG = exp2(-r);
            const double lit = floorG + (1.0 - floorG) * sweepPos;
            const double want = lerp(exp2(-0.5 * r), lit, depth) * exp2(env);
            const double dt = kTick / sr;
            if (snap) {
                h.lamp = want;
                h.cell = want;
            } else if (advance) {
                h.lamp += (want - h.lamp) * (1.0 - exp(-dt / 0.012));
                const double tau =
                    h.lamp > h.cell ? 0.005 : 0.03 + 0.17 * clamp(1.0 - h.cell, 0.0, 1.0);
                h.cell += (h.lamp - h.cell) * (1.0 - exp(-dt / tau));
            }
            oct = log2(max(h.cell, 1e-6)) + 0.5 * r;
        } else {
            oct = r * (sweepPos - 0.5) * depth + env;
        }
        h.octaves = oct;
        const double base = centre.at(i) * exp2(oct);
        const double top = min(22000.0, 0.47 * sr);
        const double sp = spread.at(i);
        const double glide = snap ? 1.0 : (advance ? 1.0 - exp(-kTick / (0.03 * sr)) : 0.0);
        for (int k = 0; k < run; ++k) {
            const double want = toStages > 1 ? sp * (k / (toStages - 1.0) - 0.5) : 0.0;
            h.place[k] += (want - h.place[k]) * (k >= fresh ? 1.0 : (k < toStages ? glide : 0.0));
            const double f = clamp(base * exp2(h.place[k]), kLowest, top);
            const double g = tan(pi * f / sr);
            const double G = g / (1.0 + g);
            if (k >= fresh) {
                h.G[k] = G;
                h.step[k] = 0.0;
            } else {
                h.step[k] = (G - h.G[k]) / kTick;
            }
        }
    }

    void process(Block& io) {
        const bool first = unplayed;
        unplayed = false;
        const int chans = min(io.channels, 2);
        const int mode = motion.index();
        // Song positions may be negative during a host's count-in.
        const bool locked = sync.index() > 0 && io.transport.playing && io.transport.tempo > 0.0;
        const double fall = dbToGain(-20.0 / sr);  // the meters' fall, 20 dB a second
        // These parameters are unsmoothed; the runtime splits at every change.
        const double atk = exp(-1.0 / (attack.value() * 0.001 * sr));
        const double rel = exp(-1.0 / (release.value() * 0.001 * sr));
        // Start outside the parameter ranges so the first frame calculates both gains.
        double lastFb = 2.0, lastMix = -1.0, lastDb = 25.0;
        double level = 1.0, outGain = 1.0;
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < chans; ++c)
                if (!isFinite(io[c][i])) io[c][i] = 0.0;
            // The motion: the left's cycle position, the right's ahead of it.
            const double pos = cyclePos(io, i);
            if (locked) {
                lfoCycle = floorToInt(pos);
                lfoPhase = pos - lfoCycle;
            } else {
                lfoPhase += -1.0 - pos;
                if (lfoPhase >= 1.0) {
                    lfoPhase -= floor(lfoPhase);
                    lfoCycle += 1;
                }
            }
            // Rise and fall turn the wet's phase a cycle each cycle of the motion.
            if (locked) {
                turn = pos * (mode == 1 ? -1.0 : 1.0);
            } else if (mode > 0) {
                turn += (-1.0 - pos) * (mode == 1 ? -1.0 : 1.0);
                turn -= floor(turn);
            }
            // The envelope, against its own recent peak.
            double x0 = abs(io[0][i]);
            if (chans > 1) x0 = max(x0, abs(io[1][i]));
            follow =
                x0 > follow ? atk * follow + (1.0 - atk) * x0 : rel * follow + (1.0 - rel) * x0;
            const double db = gainToDb(follow);
            peakRef = max(db, max(-60.0, peakRef - 4.0 / sr));
            const double gate = clamp((db + 66.0) / 6.0, 0.0, 1.0);
            envNow = clamp(1.0 + (db - peakRef) / 24.0, 0.0, 1.0) * gate;
            inPeak = max(inPeak * fall, x0);

            // The stage count's crossfade and the motion's.
            if (stageFade < 1.0) stageFade = first ? 1.0 : min(1.0, stageFade + 1.0 / (0.02 * sr));
            const double spinWant = mode > 0 ? 1.0 : 0.0;
            if (first)
                spin = spinWant;
            else if (spin < spinWant)
                spin = min(spinWant, spin + 1.0 / (0.03 * sr));
            else if (spin > spinWant)
                spin = max(spinWant, spin - 1.0 / (0.03 * sr));
            const int run = stageFade < 1.0 ? max(fromStages, toStages) : toStages;
            if (run < ran)
                for (int c = 0; c < 2; ++c)
                    for (int k = run; k < ran; ++k) ch[c].stage[k].s = 0.0;
            // Stages that start now take their places at once.
            const int started = first && i == 0 ? 0 : (run > ran ? ran : run);
            ran = run;

            // Every tick (and as stages start), the corners' targets.
            if (tick == 0 || started < run) {
                const double env = envelope.at(i) * envNow;
                for (int c = 0; c < chans; ++c) {
                    const double at =
                        lfoPhase + (phase.at(i) + (c == 1 ? stereo.at(i) : 0.0)) / 360.0;
                    const int cyc = lfoCycle + floorToInt(at);
                    const double ph = at - floorToInt(at);
                    const double lfo = lfoShape(shape.index(), ph, cyc);
                    target(c, i, 0.5 + 0.5 * lfo, 1.0 - spin, env, run, started, tick == 0);
                }
            }
            tick = tick + 1;
            if (tick >= kTick) tick = 0;

            const double fb = feedback.at(i);
            const double m = mix.at(i);
            // Feedback adds power, 1/(1 - fb^2) of the wet's on noise: most of
            // it is taken back, so turning it up leaves the level near where
            // it was and the peaks still ring out.
            if (fb != lastFb || m != lastMix) {
                const double plain = (1.0 - m) * (1.0 - m) + m * m;
                const double fed = (1.0 - m) * (1.0 - m) + m * m / (1.0 - fb * fb);
                level = pow(plain / fed, 0.4);
                lastFb = fb;
                lastMix = m;
            }
            const double keep = keepBelow.at(i);
            const bool tuneKeep = abs(keep - lastKeep) > 0.01 || (first && i == 0);
            const double outDb = output.at(i);
            if (outDb != lastDb) {
                outGain = dbToGain(outDb);
                lastDb = outDb;
            }
            double yMax = 0.0;
            for (int c = 0; c < chans; ++c) {
                Side& h = ch[c];
                const double x = io[c][i];
                // Keep below: the low part passes untouched (a split that sums
                // back exactly).
                if (tuneKeep) h.split.lowpass(keep, 0.5);
                const double lowPart = h.split.process(x);
                const double kept = keep > 20.5 ? lowPart : 0.0;
                const double dry = x - kept;
                // The chain, with the loop around it.
                double u = dry + fb * guard(h.fed);
                double tapFrom = u, tapTo = u;
                for (int k = 0; k < run; ++k) {
                    h.G[k] += h.step[k];
                    u = h.stage[k].process(u, h.G[k]);
                    if (k == fromStages - 1) tapFrom = u;
                    if (k == toStages - 1) tapTo = u;
                }
                const double chain = stageFade < 1.0 ? lerp(tapFrom, tapTo, stageFade) : tapTo;
                h.fed = chain;
                double out = dry * (1.0 - m) + chain * m;
                if (spin > 0.0) {
                    // Rise and fall: the wet turned against a dry with the
                    // pair's own phase, so only the turn differs.
                    const double t =
                        twoPi * (turn + (phase.at(i) + (c == 1 ? stereo.at(i) : 0.0)) / 360.0);
                    const double ref = h.dryPath.process(dry);
                    const double wi = h.wetI.process(chain), wq = h.wetQ.process(chain);
                    const double turned = wi * cos(t) - wq * sin(t);
                    const double barber = ref * (1.0 - m) + turned * m;
                    out = lerp(out, barber, spin);
                    h.turning = true;
                } else if (h.turning) {
                    // Cleared once it stops, so it starts afresh.
                    h.dryPath.clear();
                    h.wetI.clear();
                    h.wetQ.clear();
                    h.turning = false;
                }
                const double y = (out * level + kept) * outGain;
                io[c][i] = y;
                yMax = max(yMax, abs(y));
            }
            outPeak = max(outPeak * fall, yMax);
            if (tuneKeep) lastKeep = keep;
            rowAt += 1;
            if (rowAt >= toInt(sr / 24.0)) {
                rowAt = 0;
                head = head + 1;
                if (head >= kRows) head = 0;
                histLeft[head] = static_cast<float>(ch[0].octaves);
                histRight[head] = static_cast<float>(chans > 1 ? ch[1].octaves : ch[0].octaves);
                const double t = turn + phase.value() / 360.0;
                histTurn[head] = static_cast<float>(t - floor(t));
                histSpin[head] = static_cast<float>(spin);
                histEnv[head] = static_cast<float>(envNow);
                rows = min(rows + 1, kRows);
            }
        }
        inputLevel.set(gainToDb(inPeak));
        outputLevel.set(gainToDb(outPeak));
        centreNow.set(centre.value() * exp2(ch[0].octaves));
        envelopeNow.set(envNow);
        inShown.set(gainToDb(inPeak));
        outShown.set(gainToDb(outPeak));
        rateShown.set(sr);
        const double lp = lfoPhase + phase.value() / 360.0;
        lfoShown.set(lp - floor(lp));
        cellShown.set(clamp(ch[0].cell, 0.0, 1.0));
        spinShown.set(spin);
        const double t = turn + phase.value() / 360.0;
        turnShown.set(t - floor(t));
        envShown.set(envNow);
        const Transport& tr = io.transport;
        const double barBeats = tr.timeSigNumerator * 4.0 / max(1, tr.timeSigDenominator);
        hzShown.set(sync.index() > 0 ? (tr.tempo > 0.0 ? tr.tempo : 120.0) / 60.0 /
                                           syncBeats(sync.index(), barBeats)
                                     : rate.value());
        headShown.set(head);
        rowsShown.set(rows);
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1040;
    static constexpr double kH = 720;
    static constexpr double gx = 32;  // the response graph
    static constexpr double gy = 98;
    static constexpr double gw = 652;
    static constexpr double gh = 150;
    static constexpr double wy = 268;  // the notches' history, under it
    static constexpr double wh = 144;
    static constexpr double mx = 712;  // the motion panel
    static constexpr double mw = 312;
    static constexpr double py = 436;  // the lower panels
    static constexpr double ph = 268;
    static constexpr double kTopDb = 12.0;
    static constexpr double kBottomDb = -42.0;
    static constexpr int kPoints = 200;  // the response's points across the graph
    static constexpr int kMarks = 12;    // notches (or peaks) kept per row

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the graph
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double pressX = 0.0, pressY = 0.0, pressCentre = 700.0, pressRange = 3.0;

    // The response's working values; each history row's notches and peaks
    // (0..1 across the graph), found once per row and again when the settings
    // change.
    Array<double, 200> pointU, pointW, curve;
    Array<double, 24> placeNow, cornerU;
    Array<float, 1152> notchL, notchR, peakL;
    Array<int, 96> notchesL, notchesR, peaksL, rowVersion;
    Array<double, 12> notchNow;
    int notchesNow = 0;
    int version = 1, lastHead = -1;
    int shownRows = 96;                 // the history's rows on show, 1/24 s each
    Array<double, 121> wave, cellWave;  // the motion's wave, and the cell's path
    double sigRate = 0.0, sigCentre = 0.0, sigSpread = -1.0, sigFeedback = -9.0, sigMix = -1.0;
    double sigStereo = -1.0;
    int sigStages = -1;

    void editor(Editor& e) { e.size(kW, kH); }

    double rateNow() const { return rateShown.get() > 1000.0 ? rateShown.get() : 48000.0; }

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
        g.lineWidth(r > 26 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(r > 26 ? 4.5 : 3.5);
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
        g.moveTo(x + cos(at) * (r - 15), y + sin(at) * (r - 15));
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

    void fillOctaves(Canvas& g, double oct, double x, double y, bool withSign) {
        if (abs(oct) < 0.005)
            g.fillText("0 oct", x, y);
        else if (withSign)
            g.fillText(format("%+.2f oct", oct), x, y);
        else
            g.fillText(format("%.2f oct", oct), x, y);
    }

    void fillSync(Canvas& g, int k, double x, double y) {
        switch (k) {
            case 0:
                g.fillText("FREE", x, y);
                break;
            case 1:
                g.fillText("8 BARS", x, y);
                break;
            case 2:
                g.fillText("4 BARS", x, y);
                break;
            case 3:
                g.fillText("2 BARS", x, y);
                break;
            case 4:
                g.fillText("1 BAR", x, y);
                break;
            case 5:
                g.fillText("1/2", x, y);
                break;
            case 6:
                g.fillText("1/4", x, y);
                break;
            case 7:
                g.fillText("1/8", x, y);
                break;
            case 8:
                g.fillText("1/16", x, y);
                break;
            case 9:
                g.fillText("1/2 D", x, y);
                break;
            case 10:
                g.fillText("1/4 D", x, y);
                break;
            case 11:
                g.fillText("1/8 D", x, y);
                break;
            case 12:
                g.fillText("1/2 T", x, y);
                break;
            case 13:
                g.fillText("1/4 T", x, y);
                break;
            default:
                g.fillText("1/8 T", x, y);
        }
    }

    void fillShape(Canvas& g, int k, double x, double y) {
        switch (k) {
            case 0:
                g.fillText("SINE", x, y);
                break;
            case 1:
                g.fillText("TRI", x, y);
                break;
            case 2:
                g.fillText("UP", x, y);
                break;
            case 3:
                g.fillText("DOWN", x, y);
                break;
            case 4:
                g.fillText("STEPS", x, y);
                break;
            default:
                g.fillText("DRIFT", x, y);
        }
    }

    // --- the response -------------------------------------------------------------------------

    double hzX(double hz) const { return hzToX(hz, 20.0, 20000.0, gx, gw); }
    double fracHz(double f) const { return 20.0 * exp2(f * log2(1000.0)); }

    // The response for one side at the graph's points into curve: the
    // centre's offset (octaves), the turn (cycles) and the barber pole's share.
    void respond(double oct, double turnAt, double spinAt) {
        const double r = rateNow();
        const int n = stages.index();
        const double fb = feedback.value(), m = mix.value();
        const double base = centre.value() * exp2(oct);
        const double top = min(22000.0, 0.47 * r);
        for (int k = 0; k < n; ++k)
            cornerU[k] = tan(pi * clamp(base * exp2(placeNow[k]), kLowest, top) / r);
        const double plain = (1.0 - m) * (1.0 - m) + m * m;
        const double fed = (1.0 - m) * (1.0 - m) + m * m / (1.0 - fb * fb);
        const double level = pow(plain / fed, 0.4);
        // The wet turns back against the dry as the turn goes on.
        const double ct = cos(twoPi * turnAt), st = -sin(twoPi * turnAt);
        for (int j = 0; j < kPoints; ++j) {
            double phi = 0.0;
            for (int k = 0; k < n; ++k) phi -= 2.0 * atan(pointU[j] / cornerU[k]);
            const double ar = cos(phi), ai = sin(phi);
            // The loop, a frame late: C = A / (1 - fb A e^-jw).
            const double er = cos(pointW[j]), ei = -sin(pointW[j]);
            const double dr = 1.0 - fb * (ar * er - ai * ei), di = -fb * (ar * ei + ai * er);
            const double dd = max(dr * dr + di * di, 1e-12);
            const double cr = (ar * dr + ai * di) / dd, ci = (ai * dr - ar * di) / dd;
            const double sr0 = (1.0 - m) + m * cr, si0 = m * ci;
            const double br = (1.0 - m) + m * (cr * ct - ci * st), bi = m * (cr * st + ci * ct);
            const double yr = lerp(sr0, br, spinAt), yi = lerp(si0, bi, spinAt);
            curve[j] = gainToDb(level * sqrt(yr * yr + yi * yi));
        }
    }

    // A minimum's place between points, from the parabola through three.
    double refine(int j) const {
        const double a = curve[j - 1], b = curve[j], c = curve[j + 1];
        const double d = a - 2.0 * b + c;
        const double off = d > 1e-9 ? clamp(0.5 * (a - c) / d, -0.5, 0.5) : 0.0;
        return (j + off) / (kPoints - 1.0);
    }

    // The notches (and, for the left, peaks) of one history row, as found now.
    void findRow(int slot, int side) {
        const double oct = side == 0 ? histLeft[slot] : histRight[slot];
        const double turnAt = histTurn[slot] + (side == 1 ? stereo.value() / 360.0 : 0.0);
        respond(oct, turnAt, histSpin[slot]);
        int found = 0, raised = 0;
        for (int j = 1; j < kPoints - 1 && found < kMarks; ++j)
            if (curve[j] < curve[j - 1] && curve[j] <= curve[j + 1] && curve[j] < -8.0) {
                if (side == 0)
                    notchL[slot * kMarks + found] = static_cast<float>(refine(j));
                else
                    notchR[slot * kMarks + found] = static_cast<float>(refine(j));
                found += 1;
            }
        if (side == 0) {
            notchesL[slot] = found;
            for (int j = 1; j < kPoints - 1 && raised < kMarks; ++j)
                if (curve[j] > curve[j - 1] && curve[j] >= curve[j + 1] && curve[j] > 3.0) {
                    peakL[slot * kMarks + raised] = static_cast<float>(j / (kPoints - 1.0));
                    raised += 1;
                }
            peaksL[slot] = raised;
        } else {
            notchesR[slot] = found;
        }
    }

    // Keeps the rows' notches current: new rows, and every row again (newest
    // first, a few each paint) when a setting that shapes the response moved.
    void refresh() {
        const double r = rateNow();
        const int n = stages.index();
        if (r != sigRate) {
            for (int j = 0; j < kPoints; ++j) {
                const double hz = fracHz(j / (kPoints - 1.0));
                pointU[j] = tan(pi * min(hz, 0.499 * r) / r);
                pointW[j] = twoPi * min(hz, 0.499 * r) / r;
            }
        }
        if (r != sigRate || n != sigStages || spread.value() != sigSpread ||
            feedback.value() != sigFeedback || mix.value() != sigMix ||
            centre.value() != sigCentre || stereo.value() != sigStereo) {
            version += 1;
            sigRate = r;
            sigStages = n;
            sigSpread = spread.value();
            sigFeedback = feedback.value();
            sigMix = mix.value();
            sigCentre = centre.value();
            sigStereo = stereo.value();
            for (int k = 0; k < n; ++k)
                placeNow[k] = n > 1 ? spread.value() * (k / (n - 1.0) - 0.5) : 0.0;
        }
        const int now = headShown.get();
        int budget = lastHead < 0 ? kRows : 24;
        if (now != lastHead) {
            // Slots written since the last paint hold new rows.
            int k = lastHead < 0 ? kRows : imod(now - lastHead + kRows, kRows);
            if (k == 0) k = kRows;
            for (int i = 0; i < k; ++i) rowVersion[imod(now - i + kRows, kRows)] = 0;
            lastHead = now;
        }
        for (int i = 0; i < kRows && budget > 0; ++i) {
            const int slot = imod(now - i + kRows, kRows);
            if (rowVersion[slot] == version) continue;
            findRow(slot, 0);
            findRow(slot, 1);
            rowVersion[slot] = version;
            budget -= 1;
        }
    }

    // --- the mark -----------------------------------------------------------------------------

    // A cluster of marble berries on a short stem: their blue is structural,
    // and its sheen turns round each berry with the motion and brightens
    // with the sound.
    double berryX(int i) const {
        switch (i) {
            case 0:
                return -11.0;
            case 1:
                return 1.0;
            case 2:
                return 12.0;
            case 3:
                return -5.0;
            case 4:
                return 7.5;
            case 5:
                return -15.5;
            default:
                return 1.5;
        }
    }
    double berryY(int i) const {
        switch (i) {
            case 0:
                return -5.0;
            case 1:
                return -9.0;
            case 2:
                return -3.0;
            case 3:
                return 5.5;
            case 4:
                return 8.0;
            case 5:
                return 6.0;
            default:
                return 16.5;
        }
    }
    double berryR(int i) const {
        if (i == 6) return 5.2;
        if (i == 5) return 5.6;
        if (i == 3 || i == 4) return 7.4;
        return 6.6;
    }

    void drawMark(Canvas& g, double x, double y) {
        g.lineCap(LineCap::Round);
        g.strokeStyle(kStem);
        g.lineWidth(1.4);
        g.beginPath();
        g.moveTo(x + 15, y - 24);
        g.quadraticCurveTo(x + 6, y - 18, x + 1, y - 9);
        g.moveTo(x + 7, y - 17);
        g.quadraticCurveTo(x - 4, y - 14, x - 11, y - 5);
        g.moveTo(x + 7, y - 17);
        g.lineTo(x + 12, y - 3);
        g.stroke();
        const double turnAt = spinShown.get() > 0.5 ? turnShown.get() : lfoShown.get();
        const double a = twoPi * turnAt;
        const double lit = clamp((outShown.get() + 48.0) / 48.0, 0.0, 1.0);
        for (int i = 0; i < 7; ++i) {
            const double bx = x + berryX(i), by = y + berryY(i), r = berryR(i);
            g.fillStyle(kInk);
            g.beginPath();
            g.circle(bx, by, r + 0.8);
            g.fill();
            g.radialGradient(bx - r * 0.3, by - r * 0.35, 1, r, kBerryLight, kBerry);
            g.beginPath();
            g.circle(bx, by, r);
            g.fill();
            // The sheen: a glow of shifting colour, carried round by the motion.
            const double ang = a + i * 0.9;
            const int sheen = blend(blend(kSheenBlue, kSheenHeather, 0.5 + 0.5 * sin(ang)),
                                    kSheenSage, 0.25 + 0.25 * cos(ang + i));
            g.globalAlpha(0.45 + 0.5 * lit);
            g.radialGradient(bx + cos(ang) * r * 0.38, by + sin(ang) * r * 0.38, 0.5, r * 0.95,
                             sheen, kBerry);
            g.beginPath();
            g.circle(bx, by, r - 0.4);
            g.fill();
            g.globalAlpha(0.85);
            g.fillStyle(0xF4ECDF);
            g.beginPath();
            g.circle(bx - r * 0.38, by - r * 0.42, max(0.9, r * 0.15));
            g.fill();
            g.globalAlpha(1.0);
        }
    }

    void drawLevel(Canvas& g, double x, double y, double w, double db, const char* name) {
        label(g, name, x - 8, y + 2, TextAlign::Right);
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x, y, w, 5, 2.5);
        g.fill();
        const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
        if (t > 0.0) {
            g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
            g.beginPath();
            g.roundRect(x, y, w * t, 5, 2.5);
            g.fill();
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 30, 32);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Pollia", 60, 39);
        const double nameWidth = g.measureText("Pollia");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        const int mode = motion.index();
        const char* what = mode == 1               ? "notches rising, forever"
                           : mode == 2             ? "notches falling, forever"
                           : response.index() == 1 ? "a photocell sweep"
                                                   : "a sweep of notches";
        g.fillText(what, 68 + nameWidth, 39);
        drawLevel(g, 760, 26, 100, inShown.get(), "IN");
        drawLevel(g, 924, 26, 100, outShown.get(), "OUT");
    }

    // --- the notches --------------------------------------------------------------------------

    void drawGraph(Canvas& g) {
        well(g, gx, gy, gw, gh);
        g.save();
        g.clip(gx, gy, gw, gh);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        for (int d = 0; d < 4; ++d)
            for (int k = 1; k < 10; ++k) {
                const double hz = k * exp2(d * log2(10.0)) * 10.0;
                if (hz < 20.0 || hz > 20000.0) continue;
                const double x = hzX(hz);
                g.moveTo(x, gy);
                g.lineTo(x, wy + wh);
            }
        for (int db = -36; db <= 6; db += 6) {
            const double y = dbToY(db, kBottomDb, kTopDb, gy, gh);
            g.moveTo(gx, y);
            g.lineTo(gx + gw, y);
        }
        g.stroke();
        g.strokeStyle(kGridLit);
        g.beginPath();
        const double zero = dbToY(0, kBottomDb, kTopDb, gy, gh);
        g.moveTo(gx, zero);
        g.lineTo(gx + gw, zero);
        g.stroke();

        // The sweep's reach: where the centre travels, and the stages now.
        const double c = centre.value(), r = range.value();
        const bool sweeping = motion.index() == 0;
        const double lowHz = c * exp2(sweeping ? -0.5 * r : 0.0);
        const double highHz = c * exp2(sweeping ? 0.5 * r : 0.0);
        g.fillStyle(kLeft);
        g.globalAlpha(0.10);
        g.fillRect(hzX(lowHz), gy, max(2.0, hzX(highHz) - hzX(lowHz)), gh);
        g.globalAlpha(1.0);
        const int n = stages.index();
        const int slot = headShown.get();
        const double base = c * exp2(histLeft[slot]);
        g.strokeStyle(kSheenBlue);
        g.lineWidth(1.5);
        g.beginPath();
        for (int k = 0; k < n; ++k) {
            const double x = hzX(clamp(base * exp2(placeNow[k]), 20.0, 20000.0));
            g.moveTo(x, gy + gh - 9);
            g.lineTo(x, gy + gh - 3);
        }
        g.stroke();

        // The right's response, when it differs, then the left's over it.
        const bool both = stereo.value() > 0.5;
        if (both) {
            respond(histRight[slot], histTurn[slot] + stereo.value() / 360.0, histSpin[slot]);
            g.strokeStyle(kRight);
            g.lineWidth(1.5);
            g.globalAlpha(0.8);
            g.beginPath();
            for (int j = 0; j < kPoints; ++j) {
                const double x = gx + gw * j / (kPoints - 1.0);
                const double y =
                    dbToY(clamp(curve[j], kBottomDb - 6, kTopDb + 6), kBottomDb, kTopDb, gy, gh);
                if (j == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
            g.globalAlpha(1.0);
        }
        respond(histLeft[slot], histTurn[slot], histSpin[slot]);
        notchesNow = 0;
        for (int j = 1; j < kPoints - 1 && notchesNow < kMarks; ++j)
            if (curve[j] < curve[j - 1] && curve[j] <= curve[j + 1] && curve[j] < -8.0) {
                notchNow[notchesNow] = fracHz(refine(j));
                notchesNow += 1;
            }
        g.linearGradient(0, gy, 0, gy + gh, blend(kLeft, kGraphTop, 0.55), kGraphBottom);
        g.globalAlpha(0.6);
        g.beginPath();
        g.moveTo(gx, gy + gh);
        for (int j = 0; j < kPoints; ++j)
            g.lineTo(gx + gw * j / (kPoints - 1.0),
                     dbToY(clamp(curve[j], kBottomDb - 6, kTopDb + 6), kBottomDb, kTopDb, gy, gh));
        g.lineTo(gx + gw, gy + gh);
        g.closePath();
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(kLinen);
        g.lineWidth(2);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int j = 0; j < kPoints; ++j) {
            const double x = gx + gw * j / (kPoints - 1.0);
            const double y =
                dbToY(clamp(curve[j], kBottomDb - 6, kTopDb + 6), kBottomDb, kTopDb, gy, gh);
            if (j == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        // A bead at the foot of each notch.
        for (int k = 0; k < notchesNow; ++k) {
            const double x = hzX(notchNow[k]);
            g.fillStyle(kBerryLight);
            g.beginPath();
            g.circle(x, gy + gh - 16, 3.5);
            g.fill();
            g.fillStyle(kSheenBlue);
            g.beginPath();
            g.circle(x - 1, gy + gh - 17, 1.2);
            g.fill();
        }
        g.restore();
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("50", hzX(50), gy + gh + 10);
        g.fillText("100", hzX(100), gy + gh + 10);
        g.fillText("200", hzX(200), gy + gh + 10);
        g.fillText("500", hzX(500), gy + gh + 10);
        g.fillText("1k", hzX(1000), gy + gh + 10);
        g.fillText("2k", hzX(2000), gy + gh + 10);
        g.fillText("5k", hzX(5000), gy + gh + 10);
        g.fillText("10k", hzX(10000), gy + gh + 10);
        label(g, "+12", gx + gw - 6, dbToY(12, kBottomDb, kTopDb, gy, gh) + 7, TextAlign::Right);
        label(g, "0 DB", gx + gw - 6, zero - 7, TextAlign::Right);
        label(g, "-36", gx + gw - 6, dbToY(-36, kBottomDb, kTopDb, gy, gh) - 7, TextAlign::Right);
    }

    // The history runs across, the newest row at the right; frequency up.
    double rowX(int i) const { return gx + gw - 6 - (gw - 12) * i / (shownRows - 1.0); }
    double fracY(double f) const { return wy + wh - 4 - (wh - 8) * f; }

    // One side's trails through the rows: each notch joined to the nearest in
    // the row before, fading with age.
    void drawTrails(Canvas& g, int side, int colour, double width) {
        const int now = headShown.get();
        g.strokeStyle(colour);
        g.lineWidth(width);
        g.lineCap(LineCap::Round);
        g.lineJoin(LineJoin::Round);
        for (int band = 0; band < 4; ++band) {
            g.globalAlpha(band == 0 ? 0.95 : (band == 1 ? 0.65 : (band == 2 ? 0.4 : 0.2)));
            g.beginPath();
            const int last = min(idiv((band + 1) * shownRows, 4), rowsShown.get() - 1);
            for (int i = idiv(band * shownRows, 4); i < last; ++i) {
                const int slot = imod(now - i + kRows, kRows),
                          next = imod(now - i - 1 + kRows, kRows);
                const int count = side == 0 ? notchesL[slot] : notchesR[slot];
                const int countNext = side == 0 ? notchesL[next] : notchesR[next];
                const double x0 = rowX(i), x1 = rowX(i + 1);
                for (int k = 0; k < count; ++k) {
                    const double a =
                        side == 0 ? notchL[slot * kMarks + k] : notchR[slot * kMarks + k];
                    double best = 1.0, at = a;
                    for (int q = 0; q < countNext; ++q) {
                        const double b =
                            side == 0 ? notchL[next * kMarks + q] : notchR[next * kMarks + q];
                        if (abs(b - a) < best) {
                            best = abs(b - a);
                            at = b;
                        }
                    }
                    // A notch that came from nowhere near (a step, or one the
                    // barber pole brings in at an end) starts a new trail.
                    g.moveTo(x0, fracY(a));
                    if (best < 0.08)
                        g.lineTo(x1, fracY(at));
                    else
                        g.lineTo(x0 - 0.5, fracY(a));
                }
            }
            g.stroke();
        }
        g.globalAlpha(1.0);
    }

    void drawPeaks(Canvas& g) {
        const int now = headShown.get();
        const double w = (gw - 12) / (shownRows - 1.0);
        g.fillStyle(kPeak);
        for (int band = 0; band < 4; ++band) {
            g.globalAlpha(band == 0 ? 0.5 : (band == 1 ? 0.32 : (band == 2 ? 0.18 : 0.08)));
            g.beginPath();
            const int last = min(idiv((band + 1) * shownRows, 4), rowsShown.get());
            for (int i = idiv(band * shownRows, 4); i < last; ++i) {
                const int slot = imod(now - i + kRows, kRows);
                for (int k = 0; k < peaksL[slot]; ++k)
                    g.rect(rowX(i) - w * 0.5 - 0.3, fracY(peakL[slot * kMarks + k]) - 2, w + 0.6,
                           4);
            }
            g.fill();
        }
        g.globalAlpha(1.0);
    }

    void drawHistory(Canvas& g) {
        // About a cycle and a half of the motion, 1 to 4 s; 4 s when only
        // the envelope moves the stages.
        const bool still = motion.index() == 0 && range.value() < 0.01;
        const double hz = max(hzShown.get(), 0.01);
        shownRows = still ? kRows : clamp(roundToInt(36.0 / hz), 24, kRows);
        well(g, gx, wy, gw, wh);
        g.save();
        g.clip(gx, wy, gw, wh);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        for (int d = 0; d < 3; ++d) {
            // 100 Hz, 1 kHz, 10 kHz.
            const double y = fracY(log2(5.0 * exp2(d * log2(10.0))) / log2(1000.0));
            g.moveTo(gx, y);
            g.lineTo(gx + gw, y);
        }
        for (int sec = 1; sec * 24 < shownRows; ++sec) {
            const double x = rowX(sec * 24);
            g.moveTo(x, wy);
            g.lineTo(x, wy + wh);
        }
        g.stroke();
        if (abs(feedback.value()) > 0.25) drawPeaks(g);
        if (stereo.value() > 0.5) drawTrails(g, 1, kRight, 1.6);
        drawTrails(g, 0, kLeft, 2.0);
        g.restore();
        label(g, "NOW", gx + gw - 6, wy + 10, TextAlign::Right);
        label(g, format("%.1f S AGO", shownRows / 24.0), gx + 6, wy + wh - 9, TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("100", gx + 6, fracY(log2(5.0) / log2(1000.0)) - 7);
        g.fillText("1k", gx + 6, fracY(log2(50.0) / log2(1000.0)) - 7);
        g.fillText("10k", gx + 6, fracY(log2(500.0) / log2(1000.0)) - 7);
    }

    void drawNotches(Canvas& g) {
        panel(g, 16, 64, 684, 364);
        heading(g, "Notches", 32, 82);
        aside(g, "now, and where they went", gx + gw, 82, TextAlign::Right);
        drawGraph(g);
        drawHistory(g);
    }

    // --- motion -------------------------------------------------------------------------------

    void drawWave(Canvas& g) {
        const double x = mx + 16, y = 134, w = mw - 32, h = 62;
        well(g, x, y, w, h);
        g.save();
        g.clip(x, y, w, h);
        const int mode = motion.index();
        if (mode > 0) {
            // A barber pole: stripes that climb (or sink) as the phase turns.
            const double t = turnShown.get();
            g.fillStyle(kSheenBlue);
            g.globalAlpha(0.28);
            for (int k = -2; k < 10; ++k) {
                const double off = (k + (mode == 1 ? -t : t)) * 36.0;
                g.beginPath();
                g.moveTo(x + off, y + h);
                g.lineTo(x + off + 16, y + h);
                g.lineTo(x + off + 16 + h * 0.8, y);
                g.lineTo(x + off + h * 0.8, y);
                g.closePath();
                g.fill();
            }
            g.globalAlpha(1.0);
        } else {
            const int sh = shape.index();
            const double ph = lfoShown.get();
            const bool cell = response.index() == 1;
            for (int i = 0; i <= 120; ++i) {
                const double at = 2.0 * i / 120.0;
                const int cyc = floorToInt(at);
                wave[i] = lfoShape(sh, at - cyc, cyc);
            }
            if (cell) simulateCell(sh);
            g.lineJoin(LineJoin::Round);
            for (int pass = 0; pass < (cell ? 2 : 1); ++pass) {
                g.strokeStyle(pass == 0 ? kLeft : kLamp);
                g.lineWidth(pass == 0 && cell ? 1.2 : 2);
                g.globalAlpha(pass == 0 && cell ? 0.45 : 1.0);
                g.beginPath();
                for (int i = 0; i <= 120; ++i) {
                    const double v = pass == 0 ? wave[i] : cellWave[i];
                    const double px = x + 8 + (w - 16) * i / 120.0,
                                 pyy = y + h * 0.5 - v * (h * 0.5 - 8);
                    if (i == 0)
                        g.moveTo(px, pyy);
                    else
                        g.lineTo(px, pyy);
                }
                g.stroke();
            }
            g.globalAlpha(1.0);
            const int at = clamp(roundToInt(ph * 60.0), 0, 120);
            const double v = cell ? cellWave[at] : lfoShape(sh, ph, 0);
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(x + 8 + (w - 16) * ph * 0.5, y + h * 0.5 - v * (h * 0.5 - 8), 3.5);
            g.fill();
            if (cell) aside(g, "where the cell takes it", x + w - 8, y + h - 9, TextAlign::Right);
        }
        g.restore();
    }

    // The photocell's path over two cycles of the wave at the rate now: the
    // lamp and cell as the audio code runs them, settled over two cycles
    // first; -1..1 across the range.
    void simulateCell(int sh) {
        const double hz = max(hzShown.get(), 0.01), r = max(range.value(), 0.01);
        const double floorG = exp2(-r), dt = 1.0 / (hz * 60.0);
        double lamp = exp2(-0.5 * r), light = lamp;
        for (int n = 0; n <= 240; ++n) {
            const double at = 2.0 * n / 120.0;
            const int cyc = floorToInt(at);
            const double p = 0.5 + 0.5 * lfoShape(sh, at - cyc, cyc);
            lamp += (floorG + (1.0 - floorG) * p - lamp) * (1.0 - exp(-dt / 0.012));
            const double tau = lamp > light ? 0.005 : 0.03 + 0.17 * clamp(1.0 - light, 0.0, 1.0);
            light += (lamp - light) * (1.0 - exp(-dt / tau));
            if (n >= 120)
                cellWave[n - 120] = clamp(2.0 * (log2(max(light, 1e-6)) / r + 0.5), -1.0, 1.0);
        }
    }

    void drawMotion(Canvas& g) {
        panel(g, mx, 64, mw, 364);
        heading(g, "Motion", mx + 16, 82);
        const int mode = motion.index();
        const bool synced = sync.index() > 0;
        if (mode == 0)
            aside(g, synced ? "a sweep in time" : "a sweep, free", mx + mw - 16, 82,
                  TextAlign::Right);
        else
            aside(g, "a barber pole", mx + mw - 16, 82, TextAlign::Right);
        pill(g, mx + 16, 100, 88, 24, "SWEEP", mode == 0, kLeft, hovered(mx + 16, 100, 88, 24));
        pill(g, mx + 112, 100, 88, 24, "RISE", mode == 1, kSheenBlue,
             hovered(mx + 112, 100, 88, 24));
        pill(g, mx + 208, 100, 88, 24, "FALL", mode == 2, kSheenBlue,
             hovered(mx + 208, 100, 88, 24));
        drawWave(g);
        dial(g, knobX(4), knobY(4), knobR(4), rate.normalized(), kLeft, false, hotKnob == 4,
             "RATE");
        valueStyle(g);
        if (synced) {
            g.fillStyle(Theme::textFaint);
            g.fillText("in time", knobX(4), knobY(4) + 42);
        } else if (rate.value() < 1.0) {
            g.fillText(format("%.2f Hz", rate.value()), knobX(4), knobY(4) + 42);
        } else {
            g.fillText(format("%.1f Hz", rate.value()), knobX(4), knobY(4) + 42);
        }
        for (int k = 0; k < 15; ++k) {
            const int col = imod(k, 4), row = idiv(k, 4);
            const double cx = mx + 108 + col * 48, cy = 220 + row * 24;
            const bool on = sync.index() == k;
            g.fillStyle(on ? kLeft : (hovered(cx, cy, 44, 20) ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(cx, cy, 44, 20, 5);
            g.fill();
            g.fillStyle(on ? kInk : Theme::textMuted);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            fillSync(g, k, cx + 22, cy + 10.5);
        }
        g.globalAlpha(mode == 0 ? 1.0 : 0.35);
        for (int k = 0; k < 6; ++k) {
            const double cx = mx + 16 + k * 47.5;
            const bool on = shape.index() == k;
            g.fillStyle(on ? kLeft : (hovered(cx, 322, 44, 20) ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(cx, 322, 44, 20, 10);
            g.fill();
            g.fillStyle(on ? kInk : Theme::textMuted);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            fillShape(g, k, cx + 22, 332.5);
        }
        g.globalAlpha(1.0);
        dial(g, knobX(5), knobY(5), knobR(5), phase.normalized(), kLinen, false, hotKnob == 5,
             "PHASE");
        dial(g, knobX(6), knobY(6), knobR(6), stereo.normalized(), kRight, false, hotKnob == 6,
             "STEREO");
        valueStyle(g);
        g.fillText(format("%.0f°", phase.value()), knobX(5), knobY(5) + 30);
        g.fillText(format("%.0f°", stereo.value()), knobX(6), knobY(6) + 30);
        const bool cell = response.index() == 1;
        pill(g, mx + 156, 360, 140, 22, "DIRECT", !cell, kLinen, hovered(mx + 156, 360, 140, 22));
        pill(g, mx + 156, 388, 140, 22, "PHOTOCELL", cell, kLamp, hovered(mx + 156, 388, 140, 22));
        // The lamp, glowing with the cell's light.
        const double glow = cell ? cellShown.get() : 0.0;
        g.radialGradient(mx + 172, 399, 1, 9, blend(Theme::raised, 0xFFE7B0, glow),
                         blend(Theme::raised, kLamp, glow * 0.7));
        g.beginPath();
        g.circle(mx + 172, 399, 6);
        g.fill();
    }

    // --- stages, envelope, output -------------------------------------------------------------

    int stagePick(int k) const {
        switch (k) {
            case 0:
                return 2;
            case 1:
                return 4;
            case 2:
                return 6;
            case 3:
                return 8;
            case 4:
                return 10;
            case 5:
                return 12;
            case 6:
                return 16;
            default:
                return 24;
        }
    }

    void drawStages(Canvas& g) {
        panel(g, 16, py, 404, ph);
        heading(g, "Stages", 32, py + 20);
        const int n = stages.index();
        if (n == 1)
            aside(g, "one stage: a tilt, no notch", 404, py + 20, TextAlign::Right);
        else if (n < 4)
            aside(g, "one notch", 404, py + 20, TextAlign::Right);
        else
            aside(g, format("%.0f notches", idiv(n, 2) * 1.0), 404, py + 20, TextAlign::Right);
        for (int k = 0; k < 8; ++k) {
            const double cx = 32 + k * 47;
            const bool on = n == stagePick(k);
            g.fillStyle(on ? kSheenBlue
                           : (hovered(cx, py + 38, 43, 22) ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(cx, py + 38, 43, 22, 11);
            g.fill();
            g.fillStyle(on ? kInk : Theme::textMuted);
            g.boldFont(9.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(format("%.0f", stagePick(k) * 1.0), cx + 21.5, py + 49.5);
        }
        // The stages' corners at the middle of the sweep.
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        const double c = centre.value(), sp = spread.value();
        if (n > 1 && sp > 0.01) {
            fillHz(g, c * exp2(-0.5 * sp), 0, -100);
            g.fillText(format("%.0f stages, spread", n * 1.0), 32, py + 80);
            g.textAlign(TextAlign::Right);
            fillHz(g, c * exp2(-0.5 * sp), 300, py + 80);
            g.fillText("to", 318, py + 80);
            fillHz(g, c * exp2(0.5 * sp), 404, py + 80);
        } else {
            g.fillText(format("%.0f stages, all at the centre", n * 1.0), 32, py + 80);
        }
        dial(g, knobX(0), knobY(0), knobR(0), centre.normalized(), kLinen, false, hotKnob == 0,
             "CENTRE");
        dial(g, knobX(1), knobY(1), knobR(1), range.normalized(), kLeft, false, hotKnob == 1,
             "RANGE");
        dial(g, knobX(2), knobY(2), knobR(2), spread.normalized(), kSheenBlue, false, hotKnob == 2,
             "SPREAD");
        dial(g, knobX(3), knobY(3), knobR(3), feedback.normalized(), kPeak, true, hotKnob == 3,
             "FEEDBACK");
        valueStyle(g);
        fillHz(g, centre.value(), knobX(0), knobY(0) + 46);
        fillOctaves(g, range.value(), knobX(1), knobY(1) + 40, false);
        fillOctaves(g, spread.value(), knobX(2), knobY(2) + 40, false);
        g.fillText(format("%+.0f%%", feedback.value() * 100.0), knobX(3), knobY(3) + 46);
        aside(g, motion.index() == 0 ? "the sweep's reach" : "still while the phase turns",
              knobX(1), knobY(1) + 56, TextAlign::Center);
    }

    void drawEnvelope(Canvas& g) {
        const double x = 432, w = 280;
        panel(g, x, py, w, ph);
        heading(g, "Envelope", x + 16, py + 20);
        aside(g, "the playing moves the stages", x + w - 16, py + 20, TextAlign::Right);
        const double sx = x + 16, sy = py + 38, sw = w - 32, sh = 62;
        well(g, sx, sy, sw, sh);
        const int now = headShown.get();
        g.save();
        g.clip(sx, sy, sw, sh);
        g.fillStyle(kEnvelope);
        g.globalAlpha(0.5);
        g.beginPath();
        g.moveTo(sx + sw, sy + sh);
        const int rowsNow = max(1, rowsShown.get());
        for (int i = 0; i < rowsNow; ++i) {
            const int slot = imod(now - i + kRows, kRows);
            g.lineTo(sx + sw - sw * i / (kRows - 1.0), sy + sh - 4 - (sh - 8) * histEnv[slot]);
        }
        g.lineTo(sx + sw - sw * (rowsNow - 1) / (kRows - 1.0), sy + sh);
        g.closePath();
        g.fill();
        g.globalAlpha(1.0);
        g.restore();
        valueStyle(g);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%.0f%%", envShown.get() * 100.0), sx + 8, sy + 12);
        dial(g, knobX(7), knobY(7), knobR(7), envelope.normalized(), kEnvelope, true, hotKnob == 7,
             "ENVELOPE");
        dial(g, knobX(8), knobY(8), knobR(8), attack.normalized(), kLinen, false, hotKnob == 8,
             "ATTACK");
        dial(g, knobX(9), knobY(9), knobR(9), release.normalized(), kLinen, false, hotKnob == 9,
             "RELEASE");
        valueStyle(g);
        fillOctaves(g, envelope.value(), knobX(7), knobY(7) + 46, true);
        g.fillText(format("%.0f ms", attack.value()), knobX(8), knobY(8) + 36);
        g.fillText(format("%.0f ms", release.value()), knobX(9), knobY(9) + 36);
    }

    void drawOutput(Canvas& g) {
        const double x = 724, w = 300;
        panel(g, x, py, w, ph);
        heading(g, "Output", x + 16, py + 20);
        aside(g, "50% is the deepest notch", x + w - 16, py + 20, TextAlign::Right);
        dial(g, knobX(10), knobY(10), knobR(10), keepBelow.normalized(), kLinen, false,
             hotKnob == 10, "KEEP BELOW");
        dial(g, knobX(11), knobY(11), knobR(11), mix.normalized(), kLeft, false, hotKnob == 11,
             "MIX");
        dial(g, knobX(12), knobY(12), knobR(12), output.normalized(), kLinen, true, hotKnob == 12,
             "OUTPUT");
        valueStyle(g);
        if (keepBelow.value() <= 20.5)
            g.fillText("off", knobX(10), knobY(10) + 40);
        else
            fillHz(g, keepBelow.value(), knobX(10), knobY(10) + 40);
        g.fillText(format("%.0f%%", mix.value() * 100.0), knobX(11), knobY(11) + 44);
        fillDb(g, output.value(), knobX(12), knobY(12) + 44);
        // Where the notches are now.
        label(g, "NOTCHES NOW", x + 16, py + 196, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (notchesNow == 0) {
            g.fillStyle(Theme::textFaint);
            g.fillText(mix.value() > 0.97 ? "none: the wet alone" : "none deeper than 8 dB", x + 16,
                       py + 218);
        }
        for (int k = 0; k < notchesNow && k < 6; ++k)
            fillHz(g, notchNow[k], x + 16 + imod(k, 3) * 92, py + 218 + idiv(k, 3) * 20);
    }

    void draw(Canvas& g) {
        refresh();
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawNotches(g);
        drawMotion(g);
        drawStages(g);
        drawEnvelope(g);
        drawOutput(g);
    }

    // --- the knobs ----------------------------------------------------------------------------

    double knobX(int k) const {
        switch (k) {
            case 0:
                return 76;
            case 1:
                return 166;
            case 2:
                return 254;
            case 3:
                return 352;
            case 4:
                return mx + 52;
            case 5:
                return mx + 40;
            case 6:
                return mx + 108;
            case 7:
                return 492;
            case 8:
                return 580;
            case 9:
                return 654;
            case 10:
                return 774;
            case 11:
                return 870;
            default:
                return 966;
        }
    }
    double knobY(int k) const {
        if (k <= 3) return k == 0 || k == 3 ? py + 162 : py + 166;
        if (k == 4) return 262;
        if (k <= 6) return 386;
        if (k <= 9) return k == 7 ? py + 178 : py + 184;
        return py + 112;
    }
    double knobR(int k) const {
        if (k == 0 || k == 3 || k == 7 || k == 11 || k == 12) return 30.0;
        if (k == 4) return 28.0;
        if (k == 5 || k == 6) return 18.0;
        if (k == 8 || k == 9) return 22.0;
        return 25.0;
    }

    // --- the pointer --------------------------------------------------------------------------

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
                return centre.normalized();
            case 1:
                return range.normalized();
            case 2:
                return spread.normalized();
            case 3:
                return feedback.normalized();
            case 4:
                return rate.normalized();
            case 5:
                return phase.normalized();
            case 6:
                return stereo.normalized();
            case 7:
                return envelope.normalized();
            case 8:
                return attack.normalized();
            case 9:
                return release.normalized();
            case 10:
                return keepBelow.normalized();
            case 11:
                return mix.normalized();
            default:
                return output.normalized();
        }
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, centre, v);
        if (k == 1) act(how, range, v);
        if (k == 2) act(how, spread, v);
        if (k == 3) act(how, feedback, v);
        if (k == 4) act(how, rate, v);
        if (k == 5) act(how, phase, v);
        if (k == 6) act(how, stereo, v);
        if (k == 7) act(how, envelope, v);
        if (k == 8) act(how, attack, v);
        if (k == 9) act(how, release, v);
        if (k == 10) act(how, keepBelow, v);
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

    bool overGraph(double x, double y) const {
        return x >= gx && x <= gx + gw && y >= gy && y <= wy + wh;
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
                // Across: the centre, following the pointer; up and down: the range.
                const double octaves = (p.x() - pressX) * fine * log2(1000.0) / gw;
                centre.edit(pressCentre * exp2(octaves));
                range.edit(pressRange - (p.y() - pressY) * fine / 30.0);
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
            if (overGraph(p.x(), p.y())) {
                feedback.edit(feedback + notches * 0.01 * fine);
                return;
            }
            if (p.over(32, py + 38, 372, 22)) {
                stages.edit(stages.index() + (notches > 0 ? 1 : -1));
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
        if (overGraph(x, y)) {
            if (p.doubleClick()) {
                centre.reset();
                range.reset();
                return;
            }
            dragging = 2;
            pressX = x;
            pressY = y;
            pressCentre = centre.value();
            pressRange = range.value();
            return;
        }
        for (int c = 0; c < 3; ++c)
            if (p.over(mx + 16 + c * 96, 100, 88, 24)) {
                act(0, motion, c);
                return;
            }
        for (int s = 0; s < 15; ++s)
            if (p.over(mx + 108 + imod(s, 4) * 48, 220 + idiv(s, 4) * 24, 44, 20)) {
                act(0, sync, s);
                return;
            }
        for (int s = 0; s < 6; ++s)
            if (p.over(mx + 16 + s * 47.5, 322, 44, 20)) {
                act(0, shape, s);
                return;
            }
        if (p.over(mx + 156, 360, 140, 22)) {
            act(0, response, 0);
            return;
        }
        if (p.over(mx + 156, 388, 140, 22)) {
            act(0, response, 1);
            return;
        }
        for (int s = 0; s < 8; ++s)
            if (p.over(32 + s * 47, py + 38, 43, 22)) {
                stages.edit(stagePick(s));
                return;
            }
    }
};
