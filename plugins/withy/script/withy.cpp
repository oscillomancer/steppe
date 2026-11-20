// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Withy: a chorus, from a still widening to a choir of eight and a vibrato.
//
// A withy is a willow rod, cut green and woven: a basket is many of them
// bending over and under one another, none quite where the last one went.
// So are a chorus's voices: copies of the sound a few milliseconds late,
// each swayed back and forth so its pitch drifts a little sharp, then flat.
//
//   Lines    One per channel, read by every voice through a 16-tap windowed
//            sinc, so the highs keep their level wherever a voice reads
//            between samples (a cubic dulls and warbles them as it sweeps).
//   Voices   Laid out by the style: two in anti-phase (classic), two
//            cross-mixed so they fold to mono (dimension), three a third of
//            a cycle apart with a fast flutter on top (ensemble), up to
//            eight at their own delays and rates (multi), or one alone, with
//            no dry, for vibrato.
//   Motion   The depth is the pitch each voice swings, in cents, at any
//            rate: the sweep is worked out from it. Shimmer adds a fast
//            flutter, drift a slow random wander; bloom holds the voices
//            still at each new note and lets them sway again after it.
//   Medium   Clean, or a bucket brigade whose band follows its clock (a
//            longer delay is darker), with its curve and compander hiss.
//
// Low keep leaves the lows dry and centred; the wet is filtered, widened and
// mixed with the dry, which it never leaves out of time.

constexpr int kVoices = 8;
constexpr int kTaps = 16;             // the interpolator's taps
constexpr int kPhases = 256;          // and its table's rows between two samples
constexpr double kSwingMost = 0.025;  // seconds either way a voice may swing
constexpr double kNearest = 0.001;    // seconds: no voice reads closer than this
constexpr double kLine = 0.1;         // seconds of past each line holds
constexpr double kShimmerHz = 6.1;    // the flutter's rate
constexpr int kTrail = 256;           // the strands' points: 3 s

// --- small helpers -----------------------------------------------------------------------

// Quarter notes in a cycle for each synced rate (0: free).
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
        default:
            return 0.75;
    }
}

// A smooth random value per cycle, the same for a cycle wherever it plays: -1..1.
double hashUnit(int n) {
    double x = wrap(n, 65521) + 17.0;
    for (int k = 0; k < 4; ++k) x = fmod(x * x + 12345.0 + k, 65521.0);
    return x / 32760.0 - 1.0;
}

// The modified Bessel function I0, for the interpolator's Kaiser window.
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 30; ++k) {
        const double h = x * 0.5 / k;
        term *= h * h;
        sum += term;
    }
    return sum;
}

// The rounded triangle: a triangle whose corners turn over 1.6% of a cycle,
// so the pitch swings from sharp to flat without a click. Peak 1.
constexpr double kRound = 0.995;
double roundedTriangle(double phase) { return asin(kRound * sin(twoPi * phase)) / asin(kRound); }
// The steepest slope of each shape, per cycle, at an amplitude of 1.
double shapeSlope(int shape) { return shape == 0 ? twoPi : twoPi * kRound / asin(kRound); }

// The bucket brigade's curve, slope 1 at rest: it leans (even harmonics).
double bbdCurve(double x) {
    const double b = 0.25, t = tanh(b);
    return (tanh(x + b) - t) / (1.0 - t * t);
}

// What the loop hands back: untouched to -6 dBFS, easing toward full scale.
double guard(double x) {
    const double a = abs(x);
    if (a <= 0.5) return x;
    return sign(x) * (0.5 + 0.5 * tanh((a - 0.5) / 0.5));
}

// A step from x toward a target, never past it.
double approach(double x, double target, double step) {
    return x < target ? min(target, x + step) : max(target, x - step);
}

// A line of the past, written twice over so a read never wraps.
struct Line {
    Buffer<double> data;
    int size = 64, at = 0;

    void prepare(int n) {
        data.resize(2 * n);
        size = n;
        clear();
    }
    void clear() {
        data.fill(0.0);
        at = 0;
    }
    void push(double x) {
        data[at] = x;
        data[at + size] = x;
        at = at + 1;
        if (at >= size) at = 0;
    }
};

// --- the editor's colours ------------------------------------------------------------------

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
constexpr int kBark = 0x8A6A4A;  // the stakes
constexpr int kLeaf = 0xA3B27C;  // a willow leaf

// Each voice's colour: willow, osier bark, straw, heather, moss, rust, catkin, frost.
int voiceColour(int v) {
    switch (v) {
        case 0:
            return 0xA3BC6E;
        case 1:
            return 0xD9774F;
        case 2:
            return 0xD8B46A;
        case 3:
            return 0xB59BCB;
        case 4:
            return 0x7FA88A;
        case 5:
            return 0xC27C4A;
        case 6:
            return 0xE3D5BE;
        default:
            return 0x8FB3C2;
    }
}

int blend(int a, int b, double t) {
    const double u = clamp(t, 0.0, 1.0);
    const int ar = idiv(a, 65536), ag = imod(idiv(a, 256), 256), ab = imod(a, 256);
    const int br = idiv(b, 65536), bg = imod(idiv(b, 256), 256), bb = imod(b, 256);
    const int r = roundToInt(ar + (br - ar) * u), g = roundToInt(ag + (bg - ag) * u);
    const int bl = roundToInt(ab + (bb - ab) * u);
    return r * 65536 + g * 256 + bl;
}

// One voice: its motion, where it reads, and where it sits.
struct Voice {
    double phase = 0.0;    // the sway's, 0..1
    double flutter = 0.0;  // the shimmer's
    double wander = 0.0;   // the drift's place in its cycle
    int wanderCycle = 0;
    double wanderFrom = 0.0, wanderTo = 0.0;  // the values it glides between
    double centre = 0.0;                      // samples: where it reads at rest (glided)
    double swing = 0.0;  // samples either way: the sway's, the shimmer's, the drift's (glided)
    double swingShimmer = 0.0, swingDrift = 0.0;
    double delay = 0.0;  // samples, now
    double step = 0.0;   // samples the delay moved in the last frame: its pitch
    double toCentre = 0.0, toSwing = 0.0, toShimmer = 0.0, toDrift = 0.0;  // the glides' targets
    double offset = 0.0;  // its place in the cycle against the first voice's
    double rateFactor = 1.0, shimmerFactor = 1.0, spread = 1.0;
    double left = 0.0, right = 0.0;  // its gain into each side
    Biquad bandL, bandR;             // the bucket brigade's band
    double bandHz = -1.0, bandDesigned = -1.0;
    bool bandDirty = false;
    Noise hissL, hissR;
};

struct Withy : Effect {
    static constexpr Info info = {
        .name = "Withy",
        .id = "com.suppleaudio.withy",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A chorus from a still widening to a choir: classic, dimension, ensemble, multi-voice "
            "and vibrato, depth in cents at any rate, shimmer, drift, bloom after each note, "
            "clean or bucket-brigade voices, and lows kept dry.",
        .features = {"chorus"}};

    // --- voices -------------------------------------------------------------------------------

    Param style =
        choice("Style", {"Classic", "Dimension", "Ensemble", "Multi", "Vibrato"}, 0)
            .in("Voices")
            .help(
                "How the voices are laid out: two in anti-phase, left and right (classic); two "
                "cross-mixed, the widest with the shallowest combs, mostly dry folded to mono "
                "(dimension); three a third of a cycle apart (ensemble); up to eight at their own "
                "delays and rates (multi); or one voice alone, with no dry (vibrato).");
    Param voices = integer("Voices", 2, 8, 4)
                       .in("Voices")
                       .activeWhen(style, {"Multi"})
                       .help("How many voices the multi style sings with.");
    Param delay = milliseconds("Delay", 1, 40, 7)
                      .modulated()
                      .in("Voices")
                      .activeUnless(style, {"Vibrato"})
                      .help(
                          "How late the voices are at rest: short is a shimmering chorus, long a "
                          "loose double. Multi spreads its voices around it.");
    Param width = percent("Width", 100, 0, 200)
                      .modulated()
                      .smoothed(20)
                      .in("Voices")
                      .help("The voices' stereo width: 0 is mono, 200 twice as wide.");

    // --- motion -------------------------------------------------------------------------------

    Param sync = choice("Sync",
                        {"Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32",
                         "1/4 T", "1/8 T", "1/16 T", "1/4 D", "1/8 D"},
                        0)
                     .keys({"free", "bars4", "bars2", "bar1", "n2", "n4", "n8", "n16", "n32", "n4t",
                            "n8t", "n16t", "n4d", "n8d"})
                     .in("Motion")
                     .help("The sway's cycle in note values, locked to the song while it plays.");
    Param rate = frequency("Rate", 0.03, 12, 0.6)
                     .modulated()
                     .in("Motion")
                     .activeWhen(sync, {"Free"})
                     .help("How fast the voices sway when sync is free.");
    Param depth = cents("Depth", 0, 100, 8)
                      .modulated()
                      .curved(Curve::Pow, 2.0)
                      .in("Motion")
                      .help(
                          "How far each voice's pitch swings either way, the same at any rate: "
                          "5 is a gentle chorus, 30 a warble, 100 a semitone.");
    Param shape = choice("Shape", {"Sine", "Triangle"}, 1)
                      .in("Motion")
                      .help(
                          "The sway: a sine eases in and out of each swing; a triangle holds the "
                          "pitch steady sharp, then flat, as bucket-brigade choruses do.");
    Param shimmer = percent("Shimmer", 0)
                        .modulated()
                        .in("Motion")
                        .help(
                            "A fast flutter on top of the sway, near 6 Hz, as a share of the "
                            "depth: string ensembles sing with it.");
    Param drift = percent("Drift", 0)
                      .modulated()
                      .in("Motion")
                      .help(
                          "A slow random wander for each voice, as a share of the depth, so no "
                          "two cycles are alike.");
    Param bloom = milliseconds("Bloom", 0, 2000, 0)
                      .modulated()
                      .in("Motion")
                      .activeWhen(sync, {"Free"})
                      .help(
                          "Holds the voices still at each new note, its attack in tune, and lets "
                          "them sway again over this time: a singer's late vibrato. 0 is off.");

    // --- colour -------------------------------------------------------------------------------

    Param medium = choice("Medium", {"Clean", "Bucket Brigade"}, 0)
                       .keys({"clean", "bbd"})
                       .in("Colour")
                       .help(
                           "What the voices pass through: clean, or an analog bucket brigade, "
                           "darker the longer its delay, with its curve and its hiss.");
    Param age = percent("Age", 30)
                    .modulated()
                    .smoothed(50)
                    .in("Colour")
                    .activeWhen(medium, {"Bucket Brigade"})
                    .help(
                        "The bucket brigade's wear: fewer stages, so a darker band, more lean "
                        "and more hiss under the sound.");
    Param highCut = frequency("High Cut", 1000, 20000, 20000)
                        .modulated()
                        .smoothed(30)
                        .in("Colour")
                        .help("A low-pass on the voices (20 kHz: off).");
    Param lowKeep =
        frequency("Low Keep", 20, 1000, 20)
            .modulated()
            .smoothed(30)
            .in("Colour")
            .help(
                "Below this the sound stays dry and centred, whatever the mix: a bass or a kick "
                "keeps its weight (20 Hz: off).");
    Param feedback = percent("Feedback", 0, -80, 80)
                         .modulated()
                         .smoothed(20)
                         .in("Colour")
                         .help(
                             "The voices fed back into the line: a ringing, flanged edge; "
                             "negative turns it hollow.");

    // --- output -------------------------------------------------------------------------------

    Param mix = percent("Mix", 50)
                    .modulated()
                    .smoothed(20)
                    .in("Output")
                    .activeUnless(style, {"Vibrato"})
                    .help("Dry and voices: 50% is the classic even blend, 100% the voices alone.");
    Param output =
        decibels("Output", -24, 24, 0).modulated().smoothed(20).in("Output").help("The level out.");

    Param inputLevel = decibels("Input Level", -96, 24, -96)
                           .meter()
                           .help("The input's peak, dBFS, falling 20 dB a second.");
    Param outputLevel = decibels("Output Level", -96, 24, -96)
                            .meter()
                            .help("The output's peak, dBFS, falling 20 dB a second.");
    Param detune = cents("Detune", 0, 400, 0)
                       .meter()
                       .help(
                           "How far the voices' pitch swings now, the largest of them: the depth "
                           "with shimmer and drift, less while bloom holds them.");
    Param sweep = milliseconds("Sweep", 0, 100, 0)
                      .meter()
                      .help("How far the furthest voice's delay swings either way now.");
    Param rateNow =
        frequency("Rate Now", 0.001, 100, 0.6).meter().help("The sway's rate now, synced or free.");
    Param monoLoss =
        decibels("Mono Loss", -40, 12, 0)
            .meter()
            .help(
                "How much quieter the output folds to mono than the input does, over the last "
                "300 ms: near 0 the chorus is mono-safe.");

    void factoryPatches(Patches& p) {
        p.add("Classic Chorus");
        p.add("Subtle Width")
            .set(style, "dimension")
            .set(delay, 5)
            .set(rate, 0.25)
            .set(depth, 3)
            .set(shape, "sine")
            .set(mix, 50);
        p.add("Deep Dimension")
            .set(style, "dimension")
            .set(delay, 6)
            .set(rate, 0.45)
            .set(depth, 7)
            .set(medium, "bbd")
            .set(age, 20)
            .set(mix, 55);
        p.add("Bucket Brigade")
            .set(medium, "bbd")
            .set(age, 40)
            .set(delay, 4)
            .set(rate, 0.5)
            .set(depth, 9);
        p.add("Fast Warble")
            .set(medium, "bbd")
            .set(age, 30)
            .set(delay, 3.5)
            .set(rate, 5.5)
            .set(depth, 14)
            .set(shape, "sine");
        p.add("String Ensemble")
            .set(style, "ensemble")
            .set(delay, 8)
            .set(rate, 0.6)
            .set(depth, 6)
            .set(shimmer, 60)
            .set(medium, "bbd")
            .set(age, 30)
            .set(highCut, 9000)
            .set(mix, 60);
        p.add("Twelve-String")
            .set(style, "multi")
            .set(voices, 3)
            .set(delay, 11)
            .set(rate, 0.35)
            .set(depth, 4)
            .set(shape, "sine")
            .set(drift, 40)
            .set(mix, 45);
        p.add("Thick Doubler")
            .set(style, "multi")
            .set(voices, 6)
            .set(delay, 20)
            .set(rate, 0.3)
            .set(depth, 5)
            .set(shape, "sine")
            .set(drift, 60)
            .set(lowKeep, 150)
            .set(mix, 45);
        p.add("Choir of Eight")
            .set(style, "multi")
            .set(voices, 8)
            .set(delay, 14)
            .set(rate, 0.4)
            .set(depth, 11)
            .set(shape, "sine")
            .set(shimmer, 15)
            .set(drift, 50)
            .set(width, 140)
            .set(mix, 60);
        p.add("Bass Chorus")
            .set(delay, 6)
            .set(rate, 0.5)
            .set(depth, 7)
            .set(lowKeep, 180)
            .set(mix, 50);
        p.add("Vibrato").set(style, "vibrato").set(rate, 5.5).set(depth, 25).set(shape, "sine");
        p.add("Singer's Vibrato")
            .set(style, "vibrato")
            .set(rate, 5.2)
            .set(depth, 30)
            .set(shape, "sine")
            .set(drift, 15)
            .set(bloom, 450);
        p.add("Warped Record")
            .set(style, "vibrato")
            .set(rate, 0.55)
            .set(depth, 22)
            .set(shape, "sine")
            .set(drift, 70)
            .set(medium, "bbd")
            .set(age, 55);
        p.add("Flanged Swirl")
            .set(sync, "bar1")
            .set(delay, 2.5)
            .set(depth, 12)
            .set(shape, "sine")
            .set(feedback, 55)
            .set(mix, 50);
    }

    // --- audio --------------------------------------------------------------------------------

    double rate48 = 48000.0;
    Buffer<double> sincRow, sincStep;  // the interpolator: kPhases + 1 rows of kTaps
    Array<Line, 2> line;
    Array<Voice, 8> voice;
    Array<Biquad, 2> cutA;               // the high cut on the voices
    Array<Biquad, 4> keepLow, keepHigh;  // low keep: two Butterworths each way, per side
    Array<OnePole, 2> dcBlock;           // the bucket brigade's lean
    Array<double, 2> feedBack;           // the voices' mean, handed back
    Array<OnePole, 2> loopCut;           // and its lows taken out
    Array<double, 2> envelope;           // the compander's, per side

    int styleNow = 0, voicesNow = 2, active = 2;
    double styleGain = 1.0, styleStep = 0.001;
    bool unplayed = true;
    int tick = 0;  // frames until the filters follow their glides again

    // Glides of the script's own, snapped until the first block.
    double cutHz = 20000.0, keepHz = 20.0, cutW = 0.0, keepW = 0.0, fadeStep = 0.001;
    double bbdW = 0.0, bbdStep = 0.001, glide = 0.001, filterGlide = 0.01, triW = 1.0;
    double envUp = 0.01, envDown = 0.001, lockPull = 0.0001, vibratoW = 0.0;
    double normL = 1.0, normR = 1.0, normMono = 1.0;
    double cutDesigned = -1.0, keepDesigned = -1.0;
    bool cutDirty = false, keepDirty = false;
    // Reuse exact gain conversions while the smoothed controls stand still.
    double mixPosition = -1.0, dryGain = 1.0, wetGain = 0.0;
    double outputDb = -1000.0, outputGain = 1.0, hissAge = -1.0, hissGain = 0.0;

    // The sway when synced: the song's tempo and bar, the last heard.
    double tempoNow = 120.0, barNow = 4.0;

    // Bloom: the onset detector and how fast the voices may sway (0..1).
    double fastEnv = 0.0, slowEnv = 0.0, fastUp = 0.1, fastDown = 0.01, slowUp = 0.01,
           slowDown = 0.001;
    bool armed = true;
    double speed = 1.0, dropFrom = 1.0;
    int sinceOnset = 1000000000;

    // Readouts.
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0, fall = 0.999;
    double powInL = 0.0, powInR = 0.0, powInM = 0.0, powOutL = 0.0, powOutR = 0.0, powOutM = 0.0,
           powCoef = 0.001;
    double detuneNow = 0.0, sweepNow = 0.0, rateHz = 0.6;
    int trailAt = 0, trailCount = 0, trailLength = 560;

    Shared<double> inLeft, inRight, outLeft, outRight, speedShown, sampleRateShown;
    Shared<int> trailHead, activeShown, styleShown;
    SharedArray<float, 8> delayShown, leftShown, rightShown, pitchShown;
    SharedArray<float, 2048> trailShown;  // voice v's delay (ms) at point k: v * kTrail + k
    SharedArray<float, 256> trailLevel;   // the input's peak at each point

    void prepare() {
        rate48 = sampleRate();
        cutDesigned = -1.0;
        keepDesigned = -1.0;
        cutDirty = false;
        keepDirty = false;
        mixPosition = -1.0;
        outputDb = -1000.0;
        hissAge = -1.0;
        buildSinc();
        const int length = ceilToInt(kLine * rate48) + 2 * kTaps;
        for (int c = 0; c < 2; ++c) {
            line[c].prepare(length);
            cutA[c].clear();
            dcBlock[c].clear();
            dcBlock[c].setCutoff(4.0);
            loopCut[c].clear();
            loopCut[c].setCutoff(20.0);
            feedBack[c] = 0.0;
            envelope[c] = 0.0;
        }
        for (int k = 0; k < 4; ++k) {
            keepLow[k].clear();
            keepHigh[k].clear();
        }
        for (int v = 0; v < kVoices; ++v) {
            Voice& s = voice[v];
            s.bandL.clear();
            s.bandR.clear();
            s.bandHz = -1.0;
            s.bandDesigned = -1.0;
            s.bandDirty = false;
            s.hissL.seed(1009 + 17 * v);
            s.hissR.seed(2029 + 31 * v);
        }
        styleStep = 1.0 / (0.008 * rate48);
        fadeStep = 1.0 / (0.01 * rate48);
        bbdStep = 1.0 / (0.03 * rate48);
        glide = 1.0 - exp(-1.0 / (0.12 * rate48));
        filterGlide = 1.0 - exp(-16.0 / (0.03 * rate48));
        fastUp = 1.0 - exp(-1.0 / (0.0005 * rate48));
        fastDown = 1.0 - exp(-1.0 / (0.03 * rate48));
        slowUp = 1.0 - exp(-1.0 / (0.03 * rate48));
        slowDown = 1.0 - exp(-1.0 / (0.3 * rate48));
        powCoef = 1.0 - exp(-1.0 / (0.3 * rate48));
        envUp = 1.0 - exp(-1.0 / (0.002 * rate48));
        envDown = 1.0 - exp(-1.0 / (0.06 * rate48));
        lockPull = 1.0 - exp(-1.0 / (1.5 * rate48));
        fall = dbToGain(-20.0 / rate48);
        trailLength = max(1, roundToInt(3.0 * rate48 / kTrail));
        // A restart prepares an instance that has played: start it afresh.
        styleGain = 1.0;
        tick = 0;
        tempoNow = 120.0;
        barNow = 4.0;
        fastEnv = 0.0;
        slowEnv = 0.0;
        armed = true;
        speed = 1.0;
        dropFrom = 1.0;
        sinceOnset = 1000000000;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        powInL = 0.0;
        powInR = 0.0;
        powInM = 0.0;
        powOutL = 0.0;
        powOutR = 0.0;
        powOutM = 0.0;
        detuneNow = 0.0;
        sweepNow = 0.0;
        trailAt = 0;
        trailCount = 0;
        unplayed = true;
        arrange(style.index(), voices.index());
        for (int v = 0; v < kVoices; ++v) {
            voice[v].phase = voice[v].offset;
            voice[v].flutter = voice[v].offset;
            voice[v].wander = 0.0;
            voice[v].wanderCycle = 0;
            voice[v].wanderFrom = hashUnit(v * 131);
            voice[v].wanderTo = hashUnit(7 + v * 131);
        }
        snap();
        reportTail();
    }

    // The interpolator: for each of kPhases + 1 places between two samples,
    // kTaps weights of a Kaiser-windowed sinc that passes 94% of the band,
    // each row summing to 1; and each row's step to the next, to read between.
    void buildSinc() {
        sincRow.resize((kPhases + 1) * kTaps);
        sincStep.resize((kPhases + 1) * kTaps);
        const double beta = 8.0, cut = 0.94, half = kTaps * 0.5;
        const double norm = besselI0(beta);
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = p / static_cast<double>(kPhases);
            double sum = 0.0;
            for (int j = 0; j < kTaps; ++j) {
                const double x = (j - (kTaps / 2 - 1)) - frac;
                const double u = x / half;
                const double w = besselI0(beta * sqrt(max(0.0, 1.0 - u * u))) / norm;
                const double a = pi * cut * x;
                const double s = abs(a) < 1e-9 ? 1.0 : sin(a) / a;
                sincRow[p * kTaps + j] = cut * s * w;
                sum += cut * s * w;
            }
            for (int j = 0; j < kTaps; ++j) sincRow[p * kTaps + j] /= sum;
        }
        for (int p = 0; p <= kPhases; ++p)
            for (int j = 0; j < kTaps; ++j) {
                const int q = p < kPhases ? p + 1 : p;
                sincStep[p * kTaps + j] = sincRow[q * kTaps + j] - sincRow[p * kTaps + j];
            }
    }

    // A line d samples back from the sample about to be pushed (d of 1 is the
    // newest), between samples by the sinc.
    double readLine(const Line& l, double d) const {
        const double c = clamp(d, kTaps * 0.5, l.size - kTaps - 2.0);
        const int k = floorToInt(c);
        // The place sits between the samples k + 1 back (earlier) and k back.
        const double ph = (1.0 - (c - k)) * kPhases;
        int p = floorToInt(ph);
        double t = ph - p;
        if (p >= kPhases) {
            p = kPhases - 1;
            t = 1.0;
        }
        int start = l.at - k - kTaps / 2;
        if (start < 0) start += l.size;
        const int row = p * kTaps;
        double y0 = 0.0, y1 = 0.0;
        for (int j = 0; j < kTaps; ++j) {
            const double s = l.data[start + j];
            y0 += s * sincRow[row + j];
            y1 += s * sincStep[row + j];
        }
        return y0 + t * y1;
    }

    // Both lines at once, d samples back: into pairLeft and pairRight.
    double pairLeft = 0.0, pairRight = 0.0;
    void readPair(double d) {
        const Line& a = line[0];
        const Line& b = line[1];
        const double c = clamp(d, kTaps * 0.5, a.size - kTaps - 2.0);
        const int k = floorToInt(c);
        const double ph = (1.0 - (c - k)) * kPhases;
        int p = floorToInt(ph);
        double t = ph - p;
        if (p >= kPhases) {
            p = kPhases - 1;
            t = 1.0;
        }
        int start = a.at - k - kTaps / 2;
        if (start < 0) start += a.size;
        const int row = p * kTaps;
        double yl = 0.0, yr = 0.0;
        for (int j = 0; j < kTaps; ++j) {
            const double w = sincRow[row + j] + t * sincStep[row + j];
            yl += a.data[start + j] * w;
            yr += b.data[start + j] * w;
        }
        pairLeft = yl;
        pairRight = yr;
    }

    // The voices of a style: their gains into each side, their places in the
    // cycle, their rates and their delays against the delay set.
    void arrange(int st, int count) {
        styleNow = st;
        voicesNow = count;
        for (int v = 0; v < kVoices; ++v) {
            Voice& s = voice[v];
            s.left = 0.0;
            s.right = 0.0;
            s.offset = 0.0;
            s.rateFactor = 1.0;
            s.spread = 1.0;
            s.shimmerFactor = 1.0 + 0.08 * hashUnit(40 + v);
        }
        if (st == 0) {
            active = 2;
            voice[0].left = 1.0;
            voice[1].right = 1.0;
            voice[1].offset = 0.5;
        } else if (st == 1) {
            // Each side has its voice and the other's turned over: wide, with
            // shallow combs; folded to mono the two partly cancel, leaving
            // mostly the dry.
            active = 2;
            voice[0].left = 1.0;
            voice[0].right = -0.3;
            voice[1].left = -0.3;
            voice[1].right = 1.0;
            voice[1].offset = 0.5;
        } else if (st == 2) {
            active = 3;
            voice[0].left = 1.0;
            voice[1].left = 0.7071;
            voice[1].right = 0.7071;
            voice[2].right = 1.0;
            voice[1].offset = 1.0 / 3.0;
            voice[2].offset = 2.0 / 3.0;
            voice[1].shimmerFactor = 1.09;
            voice[2].shimmerFactor = 0.92;
        } else if (st == 3) {
            active = clamp(count, 2, kVoices);
            for (int v = 0; v < active; ++v) {
                Voice& s = voice[v];
                // Delays spread evenly around the one set; places spread
                // evenly across the sides, taken from the edges inward in
                // turn, so neighbours in time are apart in space.
                const double u = v / static_cast<double>(active - 1);
                s.spread = 0.65 + 0.7 * u;
                const int slot = v % 2 == 0 ? idiv(v, 2) : active - 1 - idiv(v, 2);
                const double place = 0.9 * (2.0 * slot / (active - 1) - 1.0);
                const double angle = (place + 1.0) * 0.25 * pi;
                s.left = cos(angle);
                s.right = sin(angle);
                s.offset = fmod(v * 0.618034, 1.0);
                s.rateFactor = 1.0 + 0.11 * hashUnit(7 + 13 * v);
            }
        } else {
            active = 1;
            voice[0].left = 1.0;
            voice[0].right = 1.0;
        }
        // The voices at the level of the input on each side: by their power
        // (they are mostly unlike one another), trimmed for how alike each
        // style's voices are.
        double l2 = 0.0, r2 = 0.0, m2 = 0.0;
        for (int v = 0; v < active; ++v) {
            const Voice& s = voice[v];
            l2 += s.left * s.left;
            r2 += s.right * s.right;
            m2 += 0.25 * (s.left + s.right) * (s.left + s.right);
        }
        const double trim = styleTrim(st);
        normL = trim / sqrt(max(l2, 0.01));
        normR = trim / sqrt(max(r2, 0.01));
        normMono = trim / sqrt(max(m2, 0.01));
    }

    // Measured on pink noise and on notes at the defaults: the voices alone as
    // loud as the input, within half a dB.
    double styleTrim(int st) const {
        if (st == 1) return dbToGain(0.35);
        if (st == 3) return dbToGain(-0.45 * log2(active * 0.5));
        return 1.0;
    }

    void snap() {
        cutHz = highCut.value();
        keepHz = lowKeep.value();
        cutW = cutHz < 19500.0 ? 1.0 : 0.0;
        keepW = keepHz > 20.5 ? 1.0 : 0.0;
        bbdW = medium.index() == 1 ? 1.0 : 0.0;
        triW = shape.index() == 1 ? 1.0 : 0.0;
        vibratoW = styleNow == 4 ? 1.0 : 0.0;
        styleGain = 1.0;
        tuneFilters();
        motionTargets(true);
    }

    void update() {
        if (unplayed) {
            if (style.index() != styleNow || voices.index() != voicesNow) {
                arrange(style.index(), voices.index());
                for (int v = 0; v < kVoices; ++v) {
                    voice[v].phase = voice[v].offset;
                    voice[v].flutter = voice[v].offset;
                }
            }
            snap();
        }
        reportTail();
    }

    // The feedback rings through the longest delay until 90 dB down.
    void reportTail() {
        const double longest = 0.04 * 1.35 + kSwingMost + 0.01;
        const double fb = abs(feedback.value());
        const double passes = fb > 0.01 ? 4.5 / -log10(fb) : 1.0;
        setTail(ceilToInt(rate48 * (longest * (passes + 1.0) + 0.05)));
    }

    // The sway's rate now: free, or the note value at the tempo.
    double swayHz() const {
        const int k = sync.index();
        if (k == 0) return rate.value();
        return tempoNow / 60.0 / syncBeats(k, barNow);
    }

    // Each voice's centre and swings, in samples, for the depth at the rate
    // now (every 16 frames; the voices glide to them); jump puts them there.
    void motionTargets(bool jump) {
        const double hz = swayHz();
        rateHz = hz;
        const double r = exp2(depth.value() / 1200.0) - 1.0;
        const double rShimmer = exp2(depth.value() * shimmer.value() / 1200.0) - 1.0;
        const double rDrift = exp2(depth.value() * drift.value() / 1200.0) - 1.0;
        const double slope = shapeSlope(shape.index());
        const bool vibrato = styleNow == 4;
        double most = 0.0, cents = 0.0;
        for (int v = 0; v < active; ++v) {
            Voice& s = voice[v];
            const double f = hz * (sync.index() == 0 ? s.rateFactor : 1.0);
            double a = r / (f * slope);
            double a2 = rShimmer / (kShimmerHz * s.shimmerFactor * twoPi);
            const double fd = 0.37 * f;
            double ad = rDrift / (pi * fd);
            const double total = a + a2 + ad;
            if (total > kSwingMost) {
                const double k = kSwingMost / total;
                a *= k;
                a2 *= k;
                ad *= k;
            }
            const double swingAll = a + a2 + ad;
            const double centre = vibrato
                                      ? swingAll + kNearest
                                      : max(delay.value() * 0.001 * s.spread, swingAll + kNearest);
            s.toCentre = centre * rate48;
            s.toSwing = a * rate48;
            s.toShimmer = a2 * rate48;
            s.toDrift = ad * rate48;
            if (jump) {
                s.centre = s.toCentre;
                s.swing = s.toSwing;
                s.swingShimmer = s.toShimmer;
                s.swingDrift = s.toDrift;
                s.delay = s.centre;
                s.step = 0.0;
            }
            most = max(most, (s.swing + s.swingShimmer + s.swingDrift) / rate48);
            // Peak pitch: each swing's steepest slope, while bloom lets it move.
            const double dev =
                speed * (s.swing / rate48 * f * slope +
                         s.swingShimmer / rate48 * kShimmerHz * s.shimmerFactor * twoPi +
                         s.swingDrift / rate48 * pi * fd);
            cents = max(cents, 1200.0 * log2(1.0 + dev));
        }
        sweepNow = most;
        detuneNow = cents;
    }

    // The filters' designs, from their glides (every 16 frames).
    void tuneFilters() {
        const double top = 0.45 * rate48;
        const double cut = min(cutHz, top);
        if (cut != cutDesigned) {
            for (int c = 0; c < 2; ++c) cutA[c].lowpass(cut, 0.7071);
            cutDesigned = cut;
        }
        if (keepHz != keepDesigned) {
            for (int c = 0; c < 2; ++c) {
                keepLow[2 * c].lowpass(keepHz, 0.7071);
                keepLow[2 * c + 1].lowpass(keepHz, 0.7071);
                keepHigh[2 * c].highpass(keepHz, 0.7071);
                keepHigh[2 * c + 1].highpass(keepHz, 0.7071);
            }
            keepDesigned = keepHz;
        }
        // A bucket brigade's clock runs at its stages over twice its delay;
        // its filters pass a little under half the clock.
        const double stages = 512.0 * pow(0.25, age.value());
        for (int v = 0; v < active; ++v) {
            Voice& s = voice[v];
            const double clock = stages / (2.0 * max(s.delay, 1.0) / rate48);
            // Keep the same 16-frame target even while the medium is clean;
            // only build its coefficients when the voice will use them.
            s.bandHz = min(0.4 * clock, min(19000.0, top));
        }
    }

    // How fast the voices may sway: 1, or held at an onset and blooming back.
    double bloomSpeed() {
        const double seconds = bloom.value() * 0.001;
        if (seconds <= 0.0 || sync.index() != 0) return 1.0;
        const double t = sinceOnset / rate48;
        if (sinceOnset < 1000000000) sinceOnset += 1;
        if (t < 0.004) return dropFrom * (1.0 - t / 0.004);
        if (t < 0.024) return 0.0;
        const double u = (t - 0.024) / seconds;
        if (u >= 1.0) return 1.0;
        return 0.5 - 0.5 * cos(pi * u);
    }

    void process(Block& io) {
        const int chans = min(io.channels, 2);
        if (io.transport.tempo > 0.0) tempoNow = io.transport.tempo;
        if (io.transport.timeSigDenominator > 0)
            barNow = io.transport.timeSigNumerator * 4.0 / io.transport.timeSigDenominator;
        const bool first = unplayed;
        if (unplayed) {
            unplayed = false;
            snap();
        }
        const int k = sync.index();
        const Transport& tr = io.transport;
        const bool locked = k > 0 && tr.playing && tr.tempo > 0.0;
        const double cycleBeats = syncBeats(k, barNow);
        const int wantStyle = style.index(), wantVoices = voices.index();
        const double bbdTarget = medium.index() == 1 ? 1.0 : 0.0;
        const double cutTarget = highCut.value() < 19500.0 ? 1.0 : 0.0;
        const double keepTarget = lowKeep.value() > 20.5 ? 1.0 : 0.0;
        const double triTarget = shape.index() == 1 ? 1.0 : 0.0;
        for (int i = 0; i < io.frames; ++i) {
            // A new style dips the voices out, lays them out afresh and brings them back.
            if (wantStyle != styleNow || (wantStyle == 3 && wantVoices != voicesNow)) {
                styleGain -= styleStep;
                if (styleGain <= 0.0) {
                    styleGain = 0.0;
                    arrange(wantStyle, wantVoices);
                    for (int v = 0; v < kVoices; ++v) {
                        voice[v].phase = fmod(voice[0].phase + voice[v].offset, 1.0);
                        voice[v].flutter = fmod(voice[0].flutter + voice[v].offset, 1.0);
                        voice[v].bandL.clear();
                        voice[v].bandR.clear();
                        voice[v].bandDirty = false;
                    }
                    motionTargets(true);
                }
            } else if (styleGain < 1.0) {
                styleGain = min(1.0, styleGain + styleStep);
            }
            bbdW = approach(bbdW, bbdTarget, bbdStep);
            cutW = approach(cutW, cutTarget, fadeStep);
            keepW = approach(keepW, keepTarget, fadeStep);
            triW = approach(triW, triTarget, fadeStep);
            if (tick == 0) {
                cutHz += (highCut.at(i) - cutHz) * filterGlide;
                keepHz += (max(lowKeep.at(i), 20.0) - keepHz) * filterGlide;
                tuneFilters();
                motionTargets(false);
            }
            tick = tick + 1;
            if (tick >= 16) tick = 0;

            double xl = io[0][i];
            double xr = chans > 1 ? io[1][i] : xl;
            if (!isFinite(xl)) xl = 0.0;
            if (!isFinite(xr)) xr = 0.0;
            const double peak = max(abs(xl), abs(xr));
            peakIn0 = max(abs(xl), peakIn0 * fall);
            peakIn1 = max(abs(xr), peakIn1 * fall);

            // Bloom: a new note holds the voices still.
            fastEnv += (peak - fastEnv) * (peak > fastEnv ? fastUp : fastDown);
            slowEnv += (peak - slowEnv) * (peak > slowEnv ? slowUp : slowDown);
            if (armed && fastEnv > 2.0 * slowEnv && fastEnv > 0.003) {
                armed = false;
                dropFrom = speed;
                sinceOnset = 0;
            } else if (!armed && fastEnv < 1.3 * slowEnv) {
                armed = true;
            }
            speed = bloomSpeed();

            // The motion: each voice's sway, flutter and wander. Synced to a
            // playing song, the sway is drawn to the song's place in the
            // cycle (at once before the first block, gently after).
            double song = 0.0;
            if (locked) {
                const double at = (tr.beats + i * tr.tempo / (60.0 * rate48)) / cycleBeats;
                song = at - floor(at);
            }
            for (int v = 0; v < active; ++v) {
                Voice& s = voice[v];
                s.centre += (s.toCentre - s.centre) * glide;
                s.swing += (s.toSwing - s.swing) * glide;
                s.swingShimmer += (s.toShimmer - s.swingShimmer) * glide;
                s.swingDrift += (s.toDrift - s.swingDrift) * glide;
                const double f = rateHz * (k == 0 ? s.rateFactor : 1.0);
                s.phase += f * speed / rate48;
                if (locked) {
                    double off = song + s.offset - s.phase;
                    off -= floor(off + 0.5);
                    s.phase += first ? off : off * lockPull;
                }
                if (s.phase >= 1.0 || s.phase < 0.0) s.phase -= floor(s.phase);
                s.flutter += kShimmerHz * s.shimmerFactor * speed / rate48;
                if (s.flutter >= 1.0) s.flutter -= floor(s.flutter);
                s.wander += 0.37 * f * speed / rate48;
                if (s.wander >= 1.0) {
                    s.wander -= floor(s.wander);
                    s.wanderCycle += 1;
                    s.wanderFrom = s.wanderTo;
                    s.wanderTo = hashUnit((s.wanderCycle + 1) * 7 + v * 131);
                }
                double sway = triW < 1.0 ? sin(twoPi * s.phase) : 0.0;
                if (triW > 0.0) sway = lerp(sway, roundedTriangle(s.phase), triW);
                double d = s.centre + s.swing * sway;
                if (s.swingShimmer > 1e-9) d += s.swingShimmer * sin(twoPi * s.flutter);
                if (s.swingDrift > 1e-9)
                    d += s.swingDrift * (s.wanderFrom + (s.wanderTo - s.wanderFrom) *
                                                            (0.5 - 0.5 * cos(pi * s.wander)));
                s.step = d - s.delay;
                s.delay = d;
            }

            // Low keep: the lows go round the voices, the rest into them.
            double lowL = 0.0, lowR = 0.0, srcL = xl, srcR = xr;
            if (keepW > 0.0) {
                keepDirty = true;
                lowL = keepLow[1].process(keepLow[0].process(xl));
                const double hl = keepHigh[1].process(keepHigh[0].process(xl));
                srcL = lerp(xl, hl, keepW);
                lowL *= keepW;
                if (chans > 1) {
                    lowR = keepLow[3].process(keepLow[2].process(xr));
                    const double hr = keepHigh[3].process(keepHigh[2].process(xr));
                    srcR = lerp(xr, hr, keepW);
                    lowR *= keepW;
                } else {
                    lowR = lowL;
                    srcR = srcL;
                }
            } else if (keepDirty) {
                for (int q = 0; q < 4; ++q) {
                    keepLow[q].clear();
                    keepHigh[q].clear();
                }
                keepDirty = false;
            }

            // Into the lines, with the voices handed back and the bucket
            // brigade's lean.
            const double fb = feedback.at(i);
            const double drive = 0.35 + 1.15 * age.at(i);
            double inL = srcL + fb * guard(feedBack[0]);
            double inR = srcR + fb * guard(feedBack[1]);
            if (bbdW > 0.0) {
                inL = lerp(inL, bbdCurve(drive * inL) / drive, bbdW);
                inR = lerp(inR, bbdCurve(drive * inR) / drive, bbdW);
            }
            line[0].push(inL);
            line[1].push(chans > 1 ? inR : 0.0);
            envelope[0] += (abs(inL) - envelope[0]) * (abs(inL) > envelope[0] ? envUp : envDown);
            envelope[1] += (abs(inR) - envelope[1]) * (abs(inR) > envelope[1] ? envUp : envDown);

            // The voices.
            double hiss = 0.0;
            if (bbdW > 0.0) {
                const double a = age.at(i);
                if (a != hissAge) {
                    hissAge = a;
                    hissGain = dbToGain(-66.0 + 28.0 * a);
                }
                hiss = hissGain;
            }
            double wl = 0.0, wr = 0.0, sumL = 0.0, sumR = 0.0;
            for (int v = 0; v < active; ++v) {
                Voice& s = voice[v];
                double yl = 0.0, yr = 0.0;
                if (chans > 1) {
                    readPair(s.delay);
                    yl = pairLeft;
                    yr = pairRight;
                } else {
                    yl = readLine(line[0], s.delay);
                    yr = yl;
                }
                if (bbdW > 0.0) {
                    s.bandDirty = true;
                    if (s.bandHz != s.bandDesigned) {
                        s.bandL.lowpass(s.bandHz, 0.7071);
                        s.bandR.lowpass(s.bandHz, 0.7071);
                        s.bandDesigned = s.bandHz;
                    }
                    const double nl = yl + hiss * envelope[0] * s.hissL.white();
                    yl = lerp(yl, s.bandL.process(nl), bbdW);
                    if (chans > 1) {
                        const double nr = yr + hiss * envelope[1] * s.hissR.white();
                        yr = lerp(yr, s.bandR.process(nr), bbdW);
                    } else {
                        yr = yl;
                    }
                } else if (s.bandDirty) {
                    s.bandL.clear();
                    s.bandR.clear();
                    s.bandDirty = false;
                }
                sumL += yl;
                sumR += yr;
                if (chans > 1) {
                    wl += s.left * yl;
                    wr += s.right * yr;
                } else {
                    wl += 0.5 * (s.left + s.right) * yl;
                }
            }
            // Handed back without its lows, which a loop would only pile up.
            feedBack[0] = loopCut[0].highpass(sumL / active);
            feedBack[1] = loopCut[1].highpass(sumR / active);
            if (chans == 1) wr = wl;

            // The voices' tone, width and level, then the mix.
            // Feedback raises the voices' power, by up to 1 / (1 - fb^2) on
            // noise: taken back off, as measured on music and pink noise.
            const double loopLevel = sqrt(1.0 - 0.6 * fb * fb);
            if (chans > 1) {
                wl *= normL * loopLevel;
                wr *= normR * loopLevel;
            } else {
                wl *= normMono * loopLevel;
                wr *= normMono * loopLevel;
            }
            if (bbdW > 0.0) {
                wl = lerp(wl, dcBlock[0].highpass(wl), bbdW);
                wr = lerp(wr, dcBlock[1].highpass(wr), bbdW);
            }
            if (cutW > 0.0) {
                cutDirty = true;
                wl = lerp(wl, cutA[0].process(wl), cutW);
                wr = lerp(wr, cutA[1].process(wr), cutW);
            } else if (cutDirty) {
                cutA[0].clear();
                cutA[1].clear();
                cutDirty = false;
            }
            const double wd = width.at(i);
            const double mid = 0.5 * (wl + wr), side = 0.5 * (wl - wr) * wd;
            wl = (mid + side) * styleGain;
            wr = (mid - side) * styleGain;
            // The mix keeps the power: the voices and the dry are unlike
            // enough that 50% of each sounds as loud as either alone.
            vibratoW = approach(vibratoW, styleNow == 4 ? 1.0 : 0.0, styleStep);
            const double m = lerp(mix.at(i), 1.0, vibratoW);
            if (m != mixPosition) {
                mixPosition = m;
                dryGain = cos(0.5 * pi * m);
                wetGain = sin(0.5 * pi * m);
            }
            const double gd = dryGain, gw = wetGain;
            const double db = output.at(i);
            if (db != outputDb) {
                outputDb = db;
                outputGain = dbToGain(db);
            }
            const double og = outputGain;
            // The dry: the lows whole, the rest by the mix.
            const double ol = (lowL + srcL * gd + gw * wl) * og;
            const double orr = (lowR + srcR * gd + gw * wr) * og;
            io[0][i] = ol;
            if (chans > 1) io[1][i] = orr;
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);

            // Mono loss: the output's fold to mono against the input's.
            powInL += (xl * xl - powInL) * powCoef;
            powInR += (xr * xr - powInR) * powCoef;
            powInM += (0.25 * (xl + xr) * (xl + xr) - powInM) * powCoef;
            powOutL += (ol * ol - powOutL) * powCoef;
            powOutR += (orr * orr - powOutR) * powCoef;
            powOutM += (0.25 * (ol + orr) * (ol + orr) - powOutM) * powCoef;

            trailCount += 1;
            if (trailCount >= trailLength) {
                trailCount = 0;
                for (int v = 0; v < kVoices; ++v)
                    trailShown[v * kTrail + trailAt] =
                        v < active ? static_cast<float>(voice[v].delay / rate48 * 1000.0) : 0.0f;
                trailLevel[trailAt] = static_cast<float>(gainToDb(max(peakIn0, peakIn1)));
                trailAt = trailAt + 1;
                if (trailAt >= kTrail) trailAt = 0;
            }
        }
        publish(chans);
    }

    // A signal's level folded to mono against its two sides', from their powers.
    double foldDb(double l, double r, double m) const {
        const double sides = 0.5 * (l + r);
        if (sides < 1e-12) return 0.0;
        return 10.0 * log10(max(m / sides, 1e-6));
    }

    void publish(int chans) {
        inputLevel.set(gainToDb(max(peakIn0, peakIn1)));
        outputLevel.set(gainToDb(max(peakOut0, peakOut1)));
        detune.set(detuneNow);
        sweep.set(sweepNow * 1000.0);
        rateNow.set(rateHz);
        double loss = 0.0;
        if (chans > 1 && powOutL + powOutR > 1e-10)
            loss = foldDb(powOutL, powOutR, powOutM) - foldDb(powInL, powInR, powInM);
        monoLoss.set(clamp(loss, -40.0, 12.0));
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(peakIn1));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(peakOut1));
        speedShown.set(speed);
        sampleRateShown.set(rate48);
        trailHead.set(trailAt);
        activeShown.set(active);
        styleShown.set(styleNow);
        for (int v = 0; v < kVoices; ++v) {
            delayShown[v] =
                v < active ? static_cast<float>(voice[v].delay / rate48 * 1000.0) : 0.0f;
            leftShown[v] = static_cast<float>(v < active ? voice[v].left * normL : 0.0);
            rightShown[v] = static_cast<float>(v < active ? voice[v].right * normR : 0.0);
            pitchShown[v] = static_cast<float>(
                v < active ? 1200.0 * log2(max(1.0 - voice[v].step, 0.01)) : 0.0);
        }
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1064;
    static constexpr double kH = 724;
    static constexpr double sx = 32;  // the strands
    static constexpr double sy = 92;
    static constexpr double sw = 660;
    static constexpr double sh = 226;
    static constexpr double fx = 878;  // the stereo field: its listener
    static constexpr double fy = 332;
    static constexpr double fr = 96;
    static constexpr double py = 376;  // the lower panels
    static constexpr double ph = 332;
    static constexpr double vx = 16;  // voices
    static constexpr double vw = 248;
    static constexpr double mx = 276;  // motion
    static constexpr double mw = 340;
    static constexpr double cx = 628;  // colour
    static constexpr double cw = 290;
    static constexpr double ox = 930;  // output
    static constexpr double ow = 118;
    static constexpr double gx = 644;  // the response
    static constexpr double gy = 444;
    static constexpr double gw = 258;
    static constexpr double gh = 128;
    static constexpr double wx = 512;  // one voice's motion
    static constexpr double wy = 492;
    static constexpr double ww = 92;
    static constexpr double wh = 62;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the strands, 3 the low keep, 4 the high cut
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double pressX = 0.0, pressY = 0.0, pressRate = 0.0, pressDepth = 0.0;
    int pressSync = 0;
    double lowMs = 0.0, highMs = 0.0;      // the strands' range, easing to what they need
    Array<double, 8> voiceLow, voiceHigh;  // each voice's own

    void editor(Editor& e) { e.size(kW, kH); }

    double rateNowHz() const { return rateNow.value() > 0.0 ? rateNow.value() : rate.value(); }
    double fs() const { return sampleRateShown.get() > 1000.0 ? sampleRateShown.get() : 48000.0; }
    int shownVoices() const { return clamp(activeShown.get(), 1, kVoices); }

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
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x + w * 0.5, y + h * 0.5 + 0.5);
    }

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value; a bipolar one
    // lights from the top. A knob that does not apply now is dimmed.
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool bipolar,
              bool hot, bool live, const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.globalAlpha(live ? 1.0 : 0.4);
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 30 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(r > 30 ? 4.5 : 3.5);
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
        g.fillText(name, x, y - r - 11);
        g.globalAlpha(1.0);
    }

    void valueStyle(Canvas& g, bool live) {
        g.fillStyle(live ? Theme::text : Theme::textFaint);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void fillHz(Canvas& g, double hz, double x, double y) {
        if (hz < 1.0)
            g.fillText(format("%.2f Hz", hz), x, y);
        else if (hz < 100.0)
            g.fillText(format("%.1f Hz", hz), x, y);
        else if (hz < 1000.0)
            g.fillText(format("%.0f Hz", hz), x, y);
        else
            g.fillText(format("%.1f kHz", hz * 0.001), x, y);
    }

    void fillMs(Canvas& g, double ms, double x, double y) {
        if (ms < 10.0)
            g.fillText(format("%.2f ms", ms), x, y);
        else
            g.fillText(format("%.1f ms", ms), x, y);
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

    void fillStyleName(Canvas& g, int st, double x, double y) {
        if (st == 0) g.fillText("Classic", x, y);
        if (st == 1) g.fillText("Dimension", x, y);
        if (st == 2) g.fillText("Ensemble", x, y);
        if (st == 3) g.fillText("Multi", x, y);
        if (st == 4) g.fillText("Vibrato", x, y);
    }

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 4, TextAlign::Right);
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double y0 = y + c * 6;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x, y0, w, 4, 2);
            g.fill();
            const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(x, y0, w * t, 4, 2);
                g.fill();
            }
        }
    }

    // A voice's sway now, -1..1: where it reads within its range of the last 3 s.
    double swayShown(int v) const {
        const double lo = voiceLow[v], hi = voiceHigh[v];
        if (hi - lo < 1e-4) return 0.0;
        return clamp(2.0 * (delayShown[v] - lo) / (hi - lo) - 1.0, -1.0, 1.0);
    }

    // --- the mark -----------------------------------------------------------------------------

    // Withies woven: four stakes of bark and a weaver for each voice (up to
    // three), passing in front of one stake and behind the next, each bending
    // as its voice sways. Willow leaves sprout from two of the stakes.
    void drawStake(Canvas& g, double x, double y0, double y1) {
        g.strokeStyle(kInk);
        g.lineWidth(4.2);
        g.beginPath();
        g.moveTo(x, y0);
        g.lineTo(x, y1);
        g.stroke();
        g.strokeStyle(kBark);
        g.lineWidth(2.6);
        g.beginPath();
        g.moveTo(x, y0);
        g.lineTo(x, y1);
        g.stroke();
    }

    double weaveY(int v, int n, double x, double cx0, double cy0) const {
        const double row = cy0 + 1 + (v - 0.5 * (n - 1)) * 9.0;
        return row + 2.4 * swayShown(v) + 1.8 * sin(pi * (x - cx0 + 15.0) / 10.0 + v * pi);
    }

    void drawMark(Canvas& g, double x, double y) {
        const int n = min(shownVoices(), 3);
        g.lineCap(LineCap::Round);
        g.lineJoin(LineJoin::Round);
        for (int k = 0; k < 2; ++k) {
            const double sx0 = x - 5 + k * 10;
            for (int leaf = 0; leaf < 2; ++leaf) {
                g.save();
                g.translate(sx0, y - 17);
                g.rotate((leaf == 0 ? -0.55 : 0.5) + (k == 0 ? -0.15 : 0.15));
                g.fillStyle(leaf == 0 ? kLeaf : 0x8E9F68);
                g.beginPath();
                g.ellipse(0, -6, 1.9, 6.2);
                g.fill();
                g.restore();
            }
        }
        for (int s = 0; s < 4; ++s) drawStake(g, x - 15 + s * 10, y - 17, y + 21);
        for (int v = 0; v < n; ++v)
            for (int pass = 0; pass < 2; ++pass) {
                g.strokeStyle(pass == 0 ? kInk : voiceColour(v));
                g.lineWidth(pass == 0 ? 4.8 : 2.8);
                g.beginPath();
                for (int k = 0; k <= 20; ++k) {
                    const double px = x - 21 + k * 2.1;
                    const double pyy = weaveY(v, n, px, x, y);
                    if (k == 0)
                        g.moveTo(px, pyy);
                    else
                        g.lineTo(px, pyy);
                }
                g.stroke();
            }
        // Where a stake passes in front, it crosses back over the weaver.
        for (int v = 0; v < n; ++v)
            for (int s = 0; s < 4; ++s) {
                const double sx0 = x - 15 + s * 10;
                if (imod(s + v, 2) == 1) continue;
                const double at = weaveY(v, n, sx0, x, y);
                drawStake(g, sx0, at - 2.8, at + 2.8);
            }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 34, 32);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Withy", 66, 41);
        const double nameWidth = g.measureText("Withy");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        const int st = styleShown.get();
        const int n = shownVoices();
        const bool bbd = medium.index() == 1;
        const double nx = 74 + nameWidth;
        if (st == 0) g.fillText(bbd ? "a bucket-brigade chorus" : "a classic chorus", nx, 41);
        if (st == 1)
            g.fillText(bbd ? "a dimension, bucket brigade" : "a dimension widening", nx, 41);
        if (st == 2) g.fillText(bbd ? "a string ensemble" : "a three-voice ensemble", nx, 41);
        if (st == 3) g.fillText(format("a choir of %d voices", n), nx, 41);
        if (st == 4) g.fillText("a vibrato", nx, 41);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 760, 26, 110, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 920, 26, 110, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the strands --------------------------------------------------------------------------

    double strandY(double ms) const {
        const double top = sy + 14, bottom = sy + sh - 30;
        return bottom - (ms - lowMs) / max(highMs - lowMs, 1e-6) * (bottom - top);
    }

    // The range the strands need, eased so the view does not jump.
    void rangeStrands() {
        const int n = shownVoices();
        double lo = 1e9, hi = -1e9;
        for (int v = 0; v < n; ++v) {
            voiceLow[v] = 1e9;
            voiceHigh[v] = -1e9;
            for (int k = 0; k < kTrail; ++k) {
                const double d = trailShown[v * kTrail + k];
                if (d <= 0.0) continue;
                voiceLow[v] = min(voiceLow[v], d);
                voiceHigh[v] = max(voiceHigh[v], d);
            }
            lo = min(lo, voiceLow[v]);
            hi = max(hi, voiceHigh[v]);
        }
        if (lo > hi) {
            lo = delay.value() - 1.0;
            hi = delay.value() + 1.0;
        }
        const double mid = 0.5 * (lo + hi);
        const double span = max(hi - lo, 0.4) * 1.15;
        const double wantLo = mid - 0.5 * span, wantHi = mid + 0.5 * span;
        if (highMs <= lowMs) {
            lowMs = wantLo;
            highMs = wantHi;
        } else {
            lowMs += 0.2 * (wantLo - lowMs);
            highMs += 0.2 * (wantHi - highMs);
        }
    }

    void drawStrandGrid(Canvas& g) {
        well(g, sx, sy, sw, sh);
        const double span = highMs - lowMs;
        double step = 0.1;
        if (span > 0.8) step = 0.2;
        if (span > 2.0) step = 0.5;
        if (span > 4.0) step = 1.0;
        if (span > 8.0) step = 2.0;
        if (span > 20.0) step = 5.0;
        g.lineWidth(1);
        g.monoFont(9);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        for (int k = ceilToInt(lowMs / step); k * step <= highMs; ++k) {
            const double y = strandY(k * step);
            if (y < sy + 22 || y > sy + sh - 26) continue;
            g.strokeStyle(kGrid);
            g.beginPath();
            g.moveTo(sx + 1, y);
            g.lineTo(sx + sw - 1, y);
            g.stroke();
            g.fillStyle(Theme::textFaint);
            if (step < 1.0)
                g.fillText(format("%.1f", k * step), sx + 6, y - 6);
            else
                g.fillText(format("%.0f", k * step), sx + 6, y - 6);
        }
        label(g, "MS", sx + 6, sy + 10, TextAlign::Left);
        // Half seconds back from now, at the right.
        for (int k = 0; k <= 6; ++k) {
            const double x = sx + sw - k * sw / 6.0;
            g.strokeStyle(k == 0 ? kGridLit : kGrid);
            g.beginPath();
            g.moveTo(x, sy + 1);
            g.lineTo(x, sy + sh - 22);
            g.stroke();
        }
        label(g, "NOW", sx + sw - 4, sy + sh + 12, TextAlign::Right);
        label(g, "3 S AGO", sx + 4, sy + sh + 12, TextAlign::Left);
        label(g, "1.5 S", sx + sw * 0.5, sy + sh + 12, TextAlign::Center);
    }

    // The input's level along the foot, so each new note shows where it came.
    void drawInputTrail(Canvas& g) {
        const int head = trailHead.get();
        const double base = sy + sh - 2, tall = 24;
        g.fillStyle(kLinen);
        g.globalAlpha(0.16);
        g.beginPath();
        g.moveTo(sx + 1, base);
        for (int k = 0; k < kTrail; ++k) {
            const int at = wrap(head + k, kTrail);
            const double db = trailLevel[at];
            const double x = sx + 1 + (sw - 2) * k / (kTrail - 1.0);
            g.lineTo(x, base - tall * clamp((db + 60.0) / 60.0, 0.0, 1.0));
        }
        g.lineTo(sx + sw - 1, base);
        g.closePath();
        g.fill();
        g.globalAlpha(1.0);
    }

    void drawStrands(Canvas& g) {
        rangeStrands();
        drawStrandGrid(g);
        drawInputTrail(g);
        const int head = trailHead.get();
        const int n = shownVoices();
        g.save();
        g.clip(sx + 1, sy + 1, sw - 2, sh - 2);
        // The delay as set, faint, where the voices sway about.
        if (styleShown.get() != 4) {
            const double y = strandY(delay.value());
            g.strokeStyle(Theme::textFaint);
            g.globalAlpha(0.45);
            g.lineWidth(1);
            for (int k = 0; k < 66; k += 2) {
                g.beginPath();
                g.moveTo(sx + k * 10, y);
                g.lineTo(sx + k * 10 + 10, y);
                g.stroke();
            }
            g.globalAlpha(1.0);
        }
        // Each strand over a dark edge, so crossings read as woven.
        g.lineCap(LineCap::Round);
        g.lineJoin(LineJoin::Round);
        for (int v = 0; v < n; ++v)
            for (int pass = 0; pass < 2; ++pass) {
                g.strokeStyle(pass == 0 ? kInk : voiceColour(v));
                g.lineWidth(pass == 0 ? 5.0 : 2.4);
                g.beginPath();
                bool started = false;
                for (int k = 0; k < kTrail; ++k) {
                    const double d = trailShown[v * kTrail + wrap(head + k, kTrail)];
                    if (d <= 0.0) {
                        started = false;
                        continue;
                    }
                    const double x = sx + 1 + (sw - 2) * k / (kTrail - 1.0);
                    if (started)
                        g.lineTo(x, strandY(d));
                    else
                        g.moveTo(x, strandY(d));
                    started = true;
                }
                g.stroke();
            }
        // Each voice now, at the right edge.
        for (int v = 0; v < n; ++v) {
            const double y = strandY(delayShown[v]);
            g.fillStyle(kInk);
            g.beginPath();
            g.circle(sx + sw - 6, y, 5.5);
            g.fill();
            g.fillStyle(voiceColour(v));
            g.beginPath();
            g.circle(sx + sw - 6, y, 3.8);
            g.fill();
        }
        g.restore();
        aside(
            g,
            "each voice's delay, the last 3 s: drag across for the rate, up and down for the depth",
            sx + 4, sy + sh + 30, TextAlign::Left);
    }

    // --- the readouts and the stereo field ----------------------------------------------------

    void readout(Canvas& g, const char* name, double x, double y) {
        label(g, name, x, y, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(15);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
    }

    void drawReadouts(Canvas& g) {
        const double x0 = 712, x1 = 880, y0 = 92, y1 = 134;
        readout(g, "DETUNE", x0, y0);
        g.fillText(format("±%.1f ct", detune.value()), x0, y0 + 18);
        readout(g, "SWEEP", x1, y0);
        g.fillText(format("±%.2f ms", sweep.value()), x1, y0 + 18);
        readout(g, "RATE", x0, y1);
        const double hz = rateNowHz();
        if (hz < 1.0)
            g.fillText(format("%.2f Hz", hz), x0, y1 + 18);
        else
            g.fillText(format("%.1f Hz", hz), x0, y1 + 18);
        if (sync.index() > 0) {
            g.fillStyle(Theme::textMuted);
            g.boldFont(9);
            fillSync(g, sync.index(), x0 + 104, y1 + 18);
        }
        readout(g, "MONO LOSS", x1, y1);
        const double loss = monoLoss.value();
        g.fillStyle(loss < -3.0 ? Theme::accent : Theme::text);
        if (abs(loss) < 0.05)
            g.fillText("0.0 dB", x1, y1 + 18);
        else
            g.fillText(format("%+.1f dB", loss), x1, y1 + 18);
        drawPitchNow(g, x0, 182);
        // Bloom: how far the voices may sway now.
        if (bloom.value() > 0.0 && sync.index() == 0) {
            const double s = speedShown.get();
            label(g, "BLOOM", x0, 212, TextAlign::Left);
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x0 + 72, 209, 100, 6, 3);
            g.fill();
            g.fillStyle(Theme::accent2);
            g.beginPath();
            g.roundRect(x0 + 72, 209, 100 * clamp(s, 0.0, 1.0), 6, 3);
            g.fill();
            aside(g, s < 0.98 ? "holding still for the note" : "swaying", x0 + 182, 212,
                  TextAlign::Left);
        }
    }

    // Each voice's pitch now against the dry, flat to the left, sharp to the right.
    void drawPitchNow(Canvas& g, double x, double y) {
        label(g, "PITCH NOW", x, y, TextAlign::Left);
        const double left = x + 72, right = x + 316;
        const double reach = max(5.0, ceil(detune.value() * 1.15 / 5.0) * 5.0);
        g.strokeStyle(kGridLit);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(left, y);
        g.lineTo(right, y);
        g.stroke();
        const double mid = 0.5 * (left + right);
        g.beginPath();
        g.moveTo(mid, y - 6);
        g.lineTo(mid, y + 6);
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("-%.0f", reach), left, y + 11);
        g.fillText(format("+%.0f ct", reach), right - 6, y + 11);
        const int n = shownVoices();
        for (int v = 0; v < n; ++v) {
            const double c = clamp(pitchShown[v] / reach, -1.0, 1.0);
            const double px = mid + c * 0.5 * (right - left);
            g.fillStyle(kInk);
            g.beginPath();
            g.circle(px, y, 5.2);
            g.fill();
            g.fillStyle(voiceColour(v));
            g.beginPath();
            g.circle(px, y, 3.6);
            g.fill();
        }
    }

    // Where each voice sits between the sides, and how far it reads now: a
    // voice further back is later, and its pitch falls as it moves away.
    void drawField(Canvas& g) {
        const double r = fr;
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        for (int k = 1; k <= 3; ++k) {
            g.beginPath();
            g.arc(fx, fy, r * k / 3.0, pi, twoPi);
            g.stroke();
        }
        g.beginPath();
        g.moveTo(fx - r - 8, fy);
        g.lineTo(fx + r + 8, fy);
        g.stroke();
        label(g, "L", fx - r - 14, fy, TextAlign::Center);
        label(g, "R", fx + r + 14, fy, TextAlign::Center);
        // The listener.
        g.fillStyle(Theme::textMuted);
        g.beginPath();
        g.circle(fx, fy - 2, 4);
        g.fill();
        const int n = shownVoices();
        const double wd = width.value();
        for (int v = 0; v < n; ++v) {
            const double l = leftShown[v], rr = rightShown[v];
            const double al = abs(l), ar = abs(rr);
            if (al + ar < 1e-6) continue;
            const double place = clamp(wd * (ar - al) / (al + ar), -1.0, 1.0);
            const double angle = pi + 0.5 * pi * (1.0 + 0.8 * place);
            const double reach = r * (0.62 + 0.24 * swayShown(v));
            const double x = fx + cos(angle) * reach, y = fy + sin(angle) * reach;
            const double size = 4.0 + 4.0 * clamp(hypot(l, rr), 0.0, 1.5);
            g.strokeStyle(voiceColour(v));
            g.globalAlpha(0.35);
            g.lineWidth(1.2);
            g.beginPath();
            g.moveTo(fx, fy - 2);
            g.lineTo(x, y);
            g.stroke();
            g.globalAlpha(1.0);
            g.fillStyle(kInk);
            g.beginPath();
            g.circle(x, y, size + 1.8);
            g.fill();
            g.fillStyle(voiceColour(v));
            g.beginPath();
            g.circle(x, y, size);
            g.fill();
            // A voice turned over on the other side (dimension): a ring there.
            const double cross = l * rr < 0.0 ? min(al, ar) / max(al, ar) : 0.0;
            if (cross > 0.0) {
                const double back = pi + 0.5 * pi * (1.0 - 0.8 * place);
                const double bx = fx + cos(back) * reach, by = fy + sin(back) * reach;
                g.strokeStyle(voiceColour(v));
                g.lineWidth(1.4);
                g.beginPath();
                g.circle(bx, by, size * sqrt(cross) + 1.0);
                g.stroke();
            }
        }
        aside(g, "the voices from where you sit; further back is later", fx, fy + 16,
              TextAlign::Center);
    }

    // --- the voices panel ---------------------------------------------------------------------

    // A small picture of each style's voices.
    void styleGlyph(Canvas& g, int st, double x, double y, bool lit) {
        const int colour = lit ? kInk : Theme::textMuted;
        g.fillStyle(colour);
        g.strokeStyle(colour);
        g.lineWidth(1.3);
        if (st == 0) {
            g.beginPath();
            g.circle(x - 8, y, 3);
            g.fill();
            g.beginPath();
            g.circle(x + 8, y, 3);
            g.fill();
        } else if (st == 1) {
            g.beginPath();
            g.circle(x - 8, y, 3);
            g.fill();
            g.beginPath();
            g.circle(x + 8, y, 3);
            g.fill();
            g.beginPath();
            g.circle(x - 8, y, 5.5);
            g.stroke();
            g.beginPath();
            g.circle(x + 8, y, 5.5);
            g.stroke();
        } else if (st == 2) {
            for (int k = 0; k < 3; ++k) {
                g.beginPath();
                g.circle(x - 9 + k * 9, y + (k == 1 ? -2 : 1), 2.8);
                g.fill();
            }
        } else if (st == 3) {
            for (int k = 0; k < 6; ++k) {
                g.beginPath();
                g.circle(x - 11 + k * 4.4, y + (k % 2 == 0 ? -2.5 : 2.5), 1.9);
                g.fill();
            }
        } else {
            g.beginPath();
            for (int k = 0; k <= 20; ++k) {
                const double px = x - 11 + k * 1.1;
                const double py0 = y + 3.5 * sin(k * 0.62);
                if (k == 0)
                    g.moveTo(px, py0);
                else
                    g.lineTo(px, py0);
            }
            g.stroke();
        }
    }

    void drawVoicesPanel(Canvas& g) {
        panel(g, vx, py, vw, ph);
        heading(g, "Voices", vx + 16, py + 20);
        const int st = style.index();
        for (int k = 0; k < 5; ++k) {
            const double y = py + 36 + k * 30;
            const bool lit = st == k;
            const bool hot = hovered(vx + 12, y, vw - 24, 26);
            g.fillStyle(lit ? Theme::accent : (hot ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(vx + 12, y, vw - 24, 26, 13);
            g.fill();
            if (!lit) {
                g.strokeStyle(Theme::line);
                g.lineWidth(1);
                g.stroke();
            }
            styleGlyph(g, k, vx + 36, y + 13, lit);
            g.fillStyle(lit ? kInk : Theme::text);
            g.boldFont(10);
            g.textAlign(TextAlign::Left);
            g.textBaseline(TextBaseline::Middle);
            fillStyleName(g, k, vx + 58, y + 13.5);
            g.fillStyle(lit ? 0x3B2A20 : Theme::textFaint);
            g.serifItalicFont(10);
            g.textAlign(TextAlign::Right);
            const double ax = vx + vw - 22;
            if (k == 0) g.fillText("two, opposed", ax, y + 13);
            if (k == 1) g.fillText("two, crossed", ax, y + 13);
            if (k == 2) g.fillText("three, a third apart", ax, y + 13);
            if (k == 3) g.fillText("up to eight", ax, y + 13);
            if (k == 4) g.fillText("one, no dry", ax, y + 13);
        }
        // How many voices multi sings with.
        const bool multi = st == 3;
        const int n = voices.index();
        for (int k = 2; k <= 8; ++k) {
            const double x = vx + 16 + (k - 2) * 31;
            g.globalAlpha(multi ? 1.0 : 0.4);
            g.fillStyle(n == k ? Theme::accent2 : Theme::raised);
            g.beginPath();
            g.roundRect(x, py + 194, 27, 18, 9);
            g.fill();
            g.fillStyle(n == k ? kInk : Theme::textMuted);
            g.monoFont(10);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(format("%d", k), x + 13.5, py + 203.5);
            g.globalAlpha(1.0);
        }
        knobAndValue(g, 0);
        knobAndValue(g, 1);
    }

    // --- the motion panel ---------------------------------------------------------------------

    double syncCellX(int k) const { return mx + 16 + imod(k, 7) * 44; }
    double syncCellY(int k) const { return py + 34 + idiv(k, 7) * 24; }

    // One voice's motion over two cycles of the sway, as the settings make it.
    void drawMotionWave(Canvas& g) {
        well(g, wx, wy, ww, wh);
        const double hz = max(rateNowHz(), 0.001);
        const double r = exp2(depth.value() / 1200.0) - 1.0;
        const double a = r / (hz * shapeSlope(shape.index()));
        const double a2 =
            (exp2(depth.value() * shimmer.value() / 1200.0) - 1.0) / (kShimmerHz * twoPi);
        const double ad = (exp2(depth.value() * drift.value() / 1200.0) - 1.0) / (pi * 0.37 * hz);
        const double total = max(a + a2 + ad, 1e-9);
        g.strokeStyle(voiceColour(0));
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int k = 0; k <= 120; ++k) {
            const double t = k / 120.0 * 2.0 / hz;  // seconds
            const double phase = t * hz;
            double v = shape.index() == 0 ? sin(twoPi * phase) : roundedTriangle(phase);
            v *= a;
            v += a2 * sin(twoPi * kShimmerHz * t);
            const double w = t * 0.37 * hz;
            const int c = floorToInt(w);
            const double from = hashUnit(c * 7 + 131), to = hashUnit((c + 1) * 7 + 131);
            v += ad * (from + (to - from) * (0.5 - 0.5 * cos(pi * (w - c))));
            const double x = wx + 4 + (ww - 8) * k / 120.0;
            const double y = wy + wh * 0.5 - v / total * (wh * 0.5 - 6);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        aside(g, "one voice, two cycles", wx + ww, wy - 10, TextAlign::Right);
    }

    void drawMotionPanel(Canvas& g) {
        panel(g, mx, py, mw, ph);
        heading(g, "Motion", mx + 16, py + 20);
        aside(g, "the sway, free or in time", mx + mw - 16, py + 20, TextAlign::Right);
        const int k0 = sync.index();
        for (int k = 0; k < 14; ++k) {
            const double x = syncCellX(k), y = syncCellY(k);
            const bool lit = k0 == k;
            const bool hot = hovered(x, y, 40, 20);
            g.fillStyle(lit ? Theme::accent2 : (hot ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(x, y, 40, 20, 10);
            g.fill();
            g.fillStyle(lit ? kInk : Theme::textMuted);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            fillSync(g, k, x + 20, y + 10.5);
        }
        knobAndValue(g, 2);
        knobAndValue(g, 3);
        drawMotionWave(g);
        for (int k = 0; k < 2; ++k)
            pill(g, wx + k * 47, wy + wh + 8, 45, 18, k == 0 ? "SINE" : "TRIANGLE",
                 shape.index() == k, Theme::accent2, hovered(wx + k * 47, wy + wh + 8, 45, 18));
        knobAndValue(g, 4);
        knobAndValue(g, 5);
        knobAndValue(g, 6);
    }

    // --- the colour panel ---------------------------------------------------------------------

    // A Butterworth's response at f against its corner fc (s = j f / fc):
    // high-pass when high. Into respRe, respIm.
    double respRe = 0.0, respIm = 0.0;
    void butter(double f, double fc, bool high) {
        const double w = f / fc;
        // 1 / (1 - w^2 + j sqrt2 w), times -w^2 for the high-pass.
        const double dr = 1.0 - w * w, di = 1.41421356 * w;
        const double m = dr * dr + di * di;
        double re = dr / m, im = -di / m;
        if (high) {
            re *= -w * w;
            im *= -w * w;
        }
        respRe = re;
        respIm = im;
    }

    // The sound now at f: the lows kept (low keep), the dry and the voices
    // through the high cut, the bucket brigade's band and the feedback.
    // Folded to mono (mono) or the two sides' power: in dB.
    double responseDb(double f, bool mono) {
        const int n = shownVoices();
        const double m = styleShown.get() == 4 ? 1.0 : mix.value();
        const double fb = feedback.value();
        const double gd = cos(0.5 * pi * m), gwet = sin(0.5 * pi * m) * sqrt(1.0 - 0.6 * fb * fb);
        // The voices' common filters.
        double cr = 1.0, ci = 0.0;
        if (highCut.value() < 19500.0) {
            butter(f, highCut.value(), false);
            cr = respRe;
            ci = respIm;
        }
        const bool bbd = medium.index() == 1;
        const double stages = 512.0 * pow(0.25, age.value());
        // Each side's voices, and their mean that is fed back.
        double lr = 0.0, li = 0.0, rr = 0.0, ri = 0.0, br = 0.0, bi = 0.0;
        for (int v = 0; v < n; ++v) {
            const double d = delayShown[v] * 0.001;
            double er = cos(twoPi * f * d), ei = -sin(twoPi * f * d);
            if (bbd) {
                const double clock = stages / (2.0 * max(d, 1e-4));
                butter(f, min(0.4 * clock, min(19000.0, 0.45 * fs())), false);
                const double tr = er * respRe - ei * respIm, ti = er * respIm + ei * respRe;
                er = tr;
                ei = ti;
            }
            lr += leftShown[v] * er;
            li += leftShown[v] * ei;
            rr += rightShown[v] * er;
            ri += rightShown[v] * ei;
            br += er / n;
            bi += ei / n;
        }
        // The audio widens only the wet mid/side before mixing the dry.
        // Width cancels in the mono fold; keep that curve and 100% exact.
        if (!mono && width.value() != 1.0) {
            const double mr = 0.5 * (lr + rr), mi = 0.5 * (li + ri);
            const double sr = 0.5 * (lr - rr) * width.value();
            const double si = 0.5 * (li - ri) * width.value();
            lr = mr + sr;
            li = mi + si;
            rr = mr - sr;
            ri = mi - si;
        }
        // The loop: 1 / (1 - fb * mean).
        const double dr = 1.0 - fb * br, di = -fb * bi;
        const double dm = dr * dr + di * di;
        const double qr = dr / dm, qi = -di / dm;
        // Each side: dry + voices x common x loop.
        const double kr = cr * qr - ci * qi, ki = cr * qi + ci * qr;
        const double Lr = gd + gwet * (lr * kr - li * ki), Li = gwet * (lr * ki + li * kr);
        const double Rr = gd + gwet * (rr * kr - ri * ki), Ri = gwet * (rr * ki + ri * kr);
        double outR = 0.0, outI = 0.0, power = 0.0;
        if (mono) {
            outR = 0.5 * (Lr + Rr);
            outI = 0.5 * (Li + Ri);
        }
        // Low keep: the lows pass dry, the rest as above.
        if (lowKeep.value() > 20.5) {
            butter(f, lowKeep.value(), false);
            const double l2r = respRe * respRe - respIm * respIm, l2i = 2.0 * respRe * respIm;
            butter(f, lowKeep.value(), true);
            const double h2r = respRe * respRe - respIm * respIm, h2i = 2.0 * respRe * respIm;
            if (mono) {
                const double tr = l2r + h2r * outR - h2i * outI;
                const double ti = l2i + h2r * outI + h2i * outR;
                outR = tr;
                outI = ti;
            } else {
                const double ar = l2r + h2r * Lr - h2i * Li, ai = l2i + h2r * Li + h2i * Lr;
                const double cr2 = l2r + h2r * Rr - h2i * Ri, ci2 = l2i + h2r * Ri + h2i * Rr;
                power = 0.5 * (ar * ar + ai * ai + cr2 * cr2 + ci2 * ci2);
            }
        } else if (!mono) {
            power = 0.5 * (Lr * Lr + Li * Li + Rr * Rr + Ri * Ri);
        }
        if (mono) power = outR * outR + outI * outI;
        return 10.0 * log10(max(power, 1e-12));
    }

    double respY(double db) const { return dbToY(clamp(db, -24.0, 12.0), -24.0, 12.0, gy, gh); }

    void drawResponse(Canvas& g) {
        well(g, gx, gy, gw, gh);
        g.save();
        g.clip(gx + 1, gy + 1, gw - 2, gh - 2);
        g.frequencyGrid(gx, gy, gw, gh, 20, 20000);
        g.strokeStyle(kGridLit);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(gx, respY(0.0));
        g.lineTo(gx + gw, respY(0.0));
        g.stroke();
        // The low keep's and the high cut's regions, shaded.
        if (lowKeep.value() > 20.5) {
            const double x = hzToX(lowKeep.value(), 20, 20000, gx, gw);
            g.fillStyle(Theme::accent2);
            g.globalAlpha(0.08);
            g.fillRect(gx, gy, x - gx, gh);
            g.globalAlpha(1.0);
        }
        // The longest delay sets how close the combs' teeth are.
        double longest = 0.001;
        for (int v = 0; v < shownVoices(); ++v) longest = max(longest, delayShown[v] * 0.001);
        for (int pass = 0; pass < 2; ++pass) {
            const bool mono = pass == 1;
            g.strokeStyle(mono ? Theme::accent : Theme::accent2);
            g.lineWidth(mono ? 1.8 : 1.3);
            g.globalAlpha(mono ? 1.0 : 0.75);
            g.beginPath();
            for (int k = 0; k <= 180; ++k) {
                const double x = gx + gw * k / 180.0;
                // The power across the point's own band, so dense combs
                // show as how deep they cut, not as noise.
                const double centre = 20.0 * pow(1000.0, k / 180.0);
                const double band = centre * (pow(1000.0, 1.0 / 180.0) - 1.0);
                const int count = clamp(ceilToInt(band * longest * 4.0), 4, 32);
                double power = 0.0;
                for (int q = 0; q < count; ++q) {
                    const double at = k + ((q + 0.5) / count - 0.5);
                    const double f = 20.0 * pow(1000.0, at / 180.0);
                    power += pow(10.0, 0.1 * responseDb(f, mono)) / count;
                }
                const double y = respY(10.0 * log10(max(power, 1e-12)));
                if (k == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The handles: low keep and high cut, dragged across.
        for (int h = 0; h < 2; ++h) {
            const double hz = h == 0 ? lowKeep.value() : highCut.value();
            const bool on = h == 0 ? hz > 20.5 : hz < 19500.0;
            const double x = hzToX(hz, 20, 20000, gx, gw);
            g.strokeStyle(on ? Theme::textMuted : Theme::textFaint);
            g.globalAlpha(on ? 0.9 : 0.4);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(x, gy + 14);
            g.lineTo(x, gy + gh - 2);
            g.stroke();
            g.fillStyle(on ? Theme::text : Theme::textFaint);
            g.beginPath();
            g.roundRect(x - 4, gy + 4, 8, 10, 3);
            g.fill();
            g.globalAlpha(1.0);
        }
        g.restore();
        label(g, "STEREO", gx + 6, gy + gh + 11, TextAlign::Left);
        g.fillStyle(Theme::accent2);
        g.fillRect(gx + 44, gy + gh + 10, 12, 2);
        label(g, "MONO", gx + 70, gy + gh + 11, TextAlign::Left);
        g.fillStyle(Theme::accent);
        g.fillRect(gx + 100, gy + gh + 10, 12, 2);
        aside(g, "the sound now", gx + gw, gy + gh + 11, TextAlign::Right);
    }

    void drawColourPanel(Canvas& g) {
        panel(g, cx, py, cw, ph);
        heading(g, "Colour", cx + 16, py + 20);
        pill(g, cx + 80, py + 11, 60, 20, "CLEAN", medium.index() == 0, Theme::accent2,
             hovered(cx + 80, py + 11, 60, 20));
        pill(g, cx + 146, py + 11, 128, 20, "BUCKET BRIGADE", medium.index() == 1, kBark,
             hovered(cx + 146, py + 11, 128, 20));
        drawResponse(g);
        knobAndValue(g, 7);
        knobAndValue(g, 8);
        knobAndValue(g, 9);
        knobAndValue(g, 10);
    }

    void drawOutputPanel(Canvas& g) {
        panel(g, ox, py, ow, ph);
        heading(g, "Output", ox + 16, py + 20);
        knobAndValue(g, 11);
        knobAndValue(g, 12);
    }

    // --- the knobs ----------------------------------------------------------------------------

    double knobX(int k) const {
        switch (k) {
            case 0:
                return vx + 66;
            case 1:
                return vx + vw - 66;
            case 2:
                return mx + 58;
            case 3:
                return mx + 160;
            case 4:
                return mx + 62;
            case 5:
                return mx + 170;
            case 6:
                return mx + 278;
            case 7:
                return cx + 36;
            case 8:
                return cx + 109;
            case 9:
                return cx + 181;
            case 10:
                return cx + 254;
            default:
                return ox + ow * 0.5;
        }
    }
    double knobY(int k) const {
        if (k <= 1) return py + 282;
        if (k <= 3) return py + 156;
        if (k <= 6) return py + 276;
        if (k <= 10) return py + 282;
        if (k == 11) return py + 104;
        return py + 246;
    }
    double knobR(int k) const {
        if (k == 3) return 38.0;
        if (k == 2 || k == 11) return 30.0;
        if (k == 0 || k == 1 || k == 12) return 25.0;
        return 21.0;
    }

    double knobValue(int k) const {
        switch (k) {
            case 0:
                return delay.normalized();
            case 1:
                return width.normalized();
            case 2:
                return rate.normalized();
            case 3:
                return depth.normalized();
            case 4:
                return shimmer.normalized();
            case 5:
                return drift.normalized();
            case 6:
                return bloom.normalized();
            case 7:
                return age.normalized();
            case 8:
                // The high cut turns the other way: clockwise closes it.
                return 1.0 - highCut.normalized();
            case 9:
                return lowKeep.normalized();
            case 10:
                return feedback.normalized();
            case 11:
                return mix.normalized();
            default:
                return output.normalized();
        }
    }

    // Whether a knob applies now (its parameter's conditions).
    bool knobLive(int k) const {
        if (k == 0) return style.index() != 4;
        if (k == 2 || k == 6) return sync.index() == 0;
        if (k == 7) return medium.index() == 1;
        if (k == 11) return style.index() != 4;
        return true;
    }

    void knobAndValue(Canvas& g, int k) {
        const double x = knobX(k), y = knobY(k), r = knobR(k);
        const bool live = knobLive(k);
        const bool hot = hotKnob == k || (dragging == 1 && dragKnob == k);
        int colour = Theme::accent;
        if (k >= 2 && k <= 6) colour = Theme::accent2;
        if (k >= 7 && k <= 10) colour = kMeterHigh;
        const bool bipolar = k == 10 || k == 12;
        const char* name = "";
        if (k == 0) name = "DELAY";
        if (k == 1) name = "WIDTH";
        if (k == 2) name = "RATE";
        if (k == 3) name = "DEPTH";
        if (k == 4) name = "SHIMMER";
        if (k == 5) name = "DRIFT";
        if (k == 6) name = "BLOOM";
        if (k == 7) name = "AGE";
        if (k == 8) name = "HIGH CUT";
        if (k == 9) name = "LOW KEEP";
        if (k == 10) name = "FEEDBACK";
        if (k == 11) name = "MIX";
        if (k == 12) name = "OUTPUT";
        dial(g, x, y, r, knobValue(k), colour, bipolar, hot, live, name);
        valueStyle(g, live);
        const double ty = y + r + 11;
        if (k == 0) fillMs(g, delay.value(), x, ty);
        if (k == 1) g.fillText(format("%.0f%%", width.value() * 100.0), x, ty);
        if (k == 2) {
            if (sync.index() == 0)
                fillHz(g, rate.value(), x, ty);
            else
                fillSync(g, sync.index(), x, ty);
        }
        if (k == 3) g.fillText(format("±%.1f ct", depth.value()), x, ty);
        if (k == 4) g.fillText(format("%.0f%%", shimmer.value() * 100.0), x, ty);
        if (k == 5) g.fillText(format("%.0f%%", drift.value() * 100.0), x, ty);
        if (k == 6) {
            if (bloom.value() <= 0.0)
                g.fillText("off", x, ty);
            else
                g.fillText(format("%.0f ms", bloom.value()), x, ty);
        }
        if (k == 7) g.fillText(format("%.0f%%", age.value() * 100.0), x, ty);
        if (k == 8) {
            if (highCut.value() >= 19500.0)
                g.fillText("off", x, ty);
            else
                fillHz(g, highCut.value(), x, ty);
        }
        if (k == 9) {
            if (lowKeep.value() <= 20.5)
                g.fillText("off", x, ty);
            else
                fillHz(g, lowKeep.value(), x, ty);
        }
        if (k == 10) g.fillText(format("%+.0f%%", feedback.value() * 100.0), x, ty);
        if (k == 11) g.fillText(format("%.0f%%", mix.value() * 100.0), x, ty);
        if (k == 12) {
            if (abs(output.value()) < 0.05)
                g.fillText("0.0 dB", x, ty);
            else
                g.fillText(format("%+.1f dB", output.value()), x, ty);
        }
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        panel(g, 16, 64, kW - 32, 300);
        drawStrands(g);
        drawReadouts(g);
        drawField(g);
        drawVoicesPanel(g);
        drawMotionPanel(g);
        drawColourPanel(g);
        drawOutputPanel(g);
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

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, delay, v);
        if (k == 1) act(how, width, v);
        if (k == 2) act(how, rate, v);
        if (k == 3) act(how, depth, v);
        if (k == 4) act(how, shimmer, v);
        if (k == 5) act(how, drift, v);
        if (k == 6) act(how, bloom, v);
        if (k == 7) act(how, age, v);
        if (k == 8) act(how, highCut, how == 1 ? 1.0 - v : v);
        if (k == 9) act(how, lowKeep, v);
        if (k == 10) act(how, feedback, v);
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
                dragStrands(p.x() - pressX, p.y() - pressY, fine);
            } else if (dragging == 3) {
                lowKeep.edit(xToHz(p.x(), 20, 20000, gx, gw));
            } else if (dragging == 4) {
                highCut.edit(xToHz(p.x(), 20, 20000, gx, gw));
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
            if (p.over(sx, sy, sw, sh)) {
                depth.editNormalized(depth.normalized() + notches * 0.02 * fine);
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    // Across: the rate (free, by octaves; synced, a step through the note
    // values); up and down: the depth.
    void dragStrands(double dx, double dy, double fine) {
        if (pressSync == 0)
            rate.edit(pressRate * exp2(dx * fine / 200.0));
        else
            act(0, sync, clamp(pressSync + roundToInt(dx * fine / 40.0), 1, 13));
        depth.editNormalized(clamp(pressDepth - dy * fine / 240.0, 0.0, 1.0));
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
        if (p.over(sx, sy, sw, sh)) {
            dragging = 2;
            pressX = x;
            pressY = y;
            pressRate = rate.value();
            pressDepth = depth.normalized();
            pressSync = sync.index();
            return;
        }
        if (p.over(gx, gy, gw, gh)) {
            // The nearer handle, by where each sits.
            const double lowX = hzToX(lowKeep.value(), 20, 20000, gx, gw);
            const double highX = hzToX(highCut.value(), 20, 20000, gx, gw);
            dragging = abs(x - lowX) <= abs(x - highX) ? 3 : 4;
            if (p.doubleClick()) {
                if (dragging == 3)
                    lowKeep.reset();
                else
                    highCut.reset();
                dragging = 0;
            }
            return;
        }
        for (int s = 0; s < 5; ++s)
            if (p.over(vx + 12, py + 36 + s * 30, vw - 24, 26)) {
                act(0, style, s);
                return;
            }
        for (int n = 2; n <= 8; ++n)
            if (p.over(vx + 16 + (n - 2) * 31, py + 194, 27, 18)) {
                act(0, voices, n);
                if (style.index() != 3) act(0, style, 3);
                return;
            }
        for (int s = 0; s < 14; ++s)
            if (p.over(syncCellX(s), syncCellY(s), 40, 20)) {
                act(0, sync, s);
                return;
            }
        for (int s = 0; s < 2; ++s)
            if (p.over(wx + s * 47, wy + wh + 8, 45, 18)) {
                act(0, shape, s);
                return;
            }
        if (p.over(cx + 80, py + 11, 60, 20)) {
            act(0, medium, 0);
            return;
        }
        if (p.over(cx + 146, py + 11, 128, 20)) act(0, medium, 1);
    }
};
