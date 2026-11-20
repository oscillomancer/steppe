// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Treeline: a true-peak limiter for the end of a mix.
//
// Three controls and an engine that listens. Gain drives the mix into the
// ceiling; the ceiling holds every peak, between the samples too; release
// says how fast it lets go, and the engine bends it to the music:
//
//   True peak   the detector reads the signal at three points between
//               each pair of samples and fits each peak, so the ceiling
//               is a true-peak meter's reading (to a hundredth of a dB to
//               20 kHz).
//   Attack      every reduction arrives along a smooth curve that has
//               landed by the time its peak plays, so nothing overshoots.
//               Two curves run side by side, a sharp one (1 ms) and a
//               broad one (4 ms): a hit takes the sharp one and keeps its
//               snap, a held tone the broad one and keeps its purity. Any
//               blend of the two still lands in time.
//   Bass guard  under a strong bass line it holds each reduction for a
//               full cycle of the bass, so the gain never rides the
//               waveform (the low-end grit of most limiters).
//   Release     two stages: peaks above what the music sustains recover
//               at the release time, the sustained part over seconds, so
//               a dense chorus does not pump and a lone hit does not dent
//               the bar after it.
//
// The loudness panel measures the output as BS.1770 and EBU R128 say:
// momentary, short-term, integrated, loudness range and true peak, over
// the recent music or (accumulating, the clock lit) since play started,
// and how far all that sits from a target. Listen Matched plays the output
// as loud as the input, so the only difference left to hear is what the
// limiting costs; Delta plays what it takes away.

// --- the true-peak detector ------------------------------------------------------------

constexpr int kPhases = 4;  // oversampling: each sample and three points after it
constexpr int kTaps = 48;   // taps of each phase's interpolator
constexpr int kHalf = 24;   // the detector's delay, samples

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        const double q = x / (2.0 * k);
        term *= q * q;
        sum += term;
    }
    return sum;
}

// The top of a peak through three neighbouring points: the vertex of the
// parabola through them, when b is the highest.
double vertex(double a, double b, double c) {
    const double bend = a - 2.0 * b + c;
    if (bend >= 0.0) return b;
    return b - 0.125 * (a - c) * (a - c) / bend;
}

// One channel's true peak: the signal interpolated at three points between
// each pair of samples (a windowed sinc, its phases made in prepare()), every
// local top refined by a parabola. push() returns the peak around sample
// n - kHalf: its own value and the highest point on either side of it.
struct TruePeak {
    Array<double, 96> hist;  // the last kTaps inputs, twice over, read as one run
    int at = 0;
    double before = 0.0;     // the highest point between the previous pair
    double lastPoint = 0.0;  // the last interpolated point of the previous pair

    void clear() {
        hist.fill(0.0);
        at = 0;
        before = 0.0;
        lastPoint = 0.0;
    }

    double push(double x, const Array<Array<double, 48>, 4>& h) {
        at = at == 0 ? kTaps - 1 : at - 1;
        hist[at] = x;
        hist[at + kTaps] = x;
        // hist[at + j] is x[n - j]; the pair is x[n - kHalf] and x[n - kHalf + 1].
        Array<double, 6> s;
        s[0] = lastPoint;
        s[1] = abs(hist[at + kHalf]);
        // All three phases read the same history. Keep each phase's sum in
        // tap order while sharing that read and its bounds check.
        double y1 = 0.0, y2 = 0.0, y3 = 0.0;
        for (int j = 0; j < kTaps; ++j) {
            const double v = hist[at + j];
            y1 += h[1][j] * v;
            y2 += h[2][j] * v;
            y3 += h[3][j] * v;
        }
        s[2] = abs(y1);
        s[3] = abs(y2);
        s[4] = abs(y3);
        s[5] = abs(hist[at + kHalf - 1]);
        double top = 0.0;
        for (int k = 1; k <= 4; ++k)
            if (s[k] >= s[k - 1] && s[k] >= s[k + 1])
                top = max(top, vertex(s[k - 1], s[k], s[k + 1]));
        top = max(top, max(s[2], max(s[3], s[4])));
        lastPoint = s[4];
        const double peak = max(s[1], max(top, before));
        before = top;
        return peak;
    }
};

// --- the gain path ---------------------------------------------------------------------

// A moving average over the last `length` values: one box of a cascade.
struct Box {
    Buffer<double> ring;
    int length = 1;
    int at = 0;
    int age = 0;
    double sum = 0.0;

    void prepare(int n) {
        ring.resize(max(1, n));
        length = max(1, n);
        clear();
    }
    void clear() {
        ring.fill(0.0);
        at = 0;
        age = 0;
        sum = 0.0;
    }
    double step(double x) {
        sum += x - ring[at];
        ring[at] = x;
        at = at + 1 == length ? 0 : at + 1;
        // Now and then, the sum afresh, so rounding never builds up.
        age += 1;
        if (age >= 65536) {
            age = 0;
            sum = 0.0;
            for (int k = 0; k < length; ++k) sum += ring[k];
        }
        return sum / length;
    }
};

// Three boxes in a row: a bell-shaped average whose span is the sum of
// their lengths, less two. Each output is an average of inputs, so it is no
// higher than the highest of them.
struct Bell {
    Box a, b, c;
    void prepare(int span) {
        const int whole = span + 2;
        const int la = max(1, roundToInt(0.42 * whole));
        const int lb = max(1, roundToInt(0.33 * whole));
        a.prepare(la);
        b.prepare(lb);
        c.prepare(max(1, whole - la - lb));
    }
    void clear() {
        a.clear();
        b.clear();
        c.clear();
    }
    double step(double x) { return c.step(b.step(a.step(x))); }
};

// The lowest of the last `span` values, for any span up to its size (a
// monotonic queue: each value waits only while nothing newer is lower).
struct Hold {
    Buffer<double> value;
    Buffer<int> when;
    int size = 1, head = 0, count = 0, now = 0;

    void prepare(int n) {
        size = max(2, n);
        value.resize(size);
        when.resize(size);
        clear();
    }
    void clear() {
        head = 0;
        count = 0;
        now = 0;
    }
    double push(double v, int span) {
        while (count > 0 && value[wrap(head + count - 1, size)] >= v) count -= 1;
        if (count == size) {
            head = wrap(head + 1, size);
            count -= 1;
        }
        const int slot = wrap(head + count, size);
        value[slot] = v;
        when[slot] = now;
        count += 1;
        while (now - when[head] >= span) {
            head = wrap(head + 1, size);
            count -= 1;
        }
        now += 1;
        return value[head];
    }
};

// A second-order section, transposed direct form II, with its coefficients given.
struct Section {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
    void clear() {
        z1 = 0.0;
        z2 = 0.0;
    }
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// BS.1770's K-weighting at any sample rate: its high shelf and its
// high-pass, each the analogue prototype behind the standard's 48 kHz
// coefficients, through the bilinear transform.
struct KWeighting {
    Section shelf, high;
    void design(double fs) {
        double k = tan(pi * 1681.974450955533 / fs);
        double q = 0.7071752369554196;
        const double vh = pow(10.0, 3.999843853973347 / 20.0);
        const double vb = pow(vh, 0.4996667741545416);
        double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
        k = tan(pi * 38.13547087602444 / fs);
        q = 0.5003270373238773;
        a0 = 1.0 + k / q + k * k;
        high.b0 = 1.0;
        high.b1 = -2.0;
        high.b2 = 1.0;
        high.a1 = 2.0 * (k * k - 1.0) / a0;
        high.a2 = (1.0 - k / q + k * k) / a0;
    }
    void clear() {
        shelf.clear();
        high.clear();
    }
    double process(double x) { return high.process(shelf.process(x)); }
};

// The share of the way a one-pole moves each sample, for a time constant.
double stepFor(double ms, double fs) { return 1.0 - exp(-1000.0 / (ms * fs)); }

// Loudness in LUFS of a mean square (K-weighted, summed over channels).
double lufs(double meanSquare) {
    return meanSquare > 1e-12 ? -0.691 + 10.0 * log10(meanSquare) : -120.0;
}

// A level can stay still for many samples, including after a glide lands.
// Reuse exactly the same conversion until its dB value changes.
struct LevelGain {
    double db = 0.0, linear = 1.0;

    double get(double value) {
        if (value != db) {
            db = value;
            linear = dbToGain(value);
        }
        return linear;
    }
};

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
constexpr int kMoss = 0x9DB36E;      // the sustained reduction
constexpr int kClay = 0xD9775A;      // the peaks' reduction
constexpr int kMarigold = 0xE2B04F;  // loudness
constexpr int kSpruce = 0x5F7A5A;    // the mark's trees

constexpr int kCols = 1024;      // history columns, a hundredth of a second each
constexpr int kShown = 800;      // eight seconds across the graph
constexpr int kBins = 900;       // loudness histograms: -70 to +20 LUFS in tenths
constexpr int kRecent = 300;     // the recent meters' window: 30 s in tenths of a second
constexpr int kRecentPeak = 20;  // the recent true peak's: 2 s

struct Treeline : Effect {
    static constexpr Info info = {
        .name = "Treeline",
        .id = "com.suppleaudio.treeline",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "True-peak limiter that adapts its attack, hold and release to the music, with "
            "loudness metering to BS.1770 and level-matched listening.",
        .features = {"limiter"}};

    Param gain =
        decibels("Gain", 0, 24, 0)
            .modulated()
            .smoothed(30)
            .in("Limiter")
            .help("How hard it drives the mix into the ceiling: louder, with more held down.");
    Param ceiling =
        decibels("Ceiling", -24, 0, -1)
            .modulated()
            .smoothed(30)
            .in("Limiter")
            .help("The highest the output goes, in dB true peak (between samples too).");
    Param release = milliseconds("Release", 10, 1000, 120)
                        .modulated()
                        .in("Limiter")
                        .help(
                            "How fast it recovers after a peak; it slows by itself over bass and "
                            "dense passages.");
    Param target =
        decibels("Target", -24, -6, -14)
            .modulated()
            .in("Loudness")
            .help("The integrated loudness you aim for, in LUFS: the meters read against it.");
    Param listen =
        choice("Listen", {"Output", "Matched", "Delta"}, 0)
            .keys({"output", "matched", "delta"})
            .notAutomatable()
            .in("Loudness")
            .help(
                "The output; the output as loud as the input, to hear only what limiting costs; or "
                "what it takes away.");
    Param accumulate =
        toggle("Accumulate", false)
            .notAutomatable()
            .in("Loudness")
            .help(
                "Off, the meters follow the last 30 s (true peak the last 2 s, overs the last 5 "
                "s); on, they total everything since it was switched on or play started.");

    Param gainReduction =
        decibels("Gain Reduction", -48, 0, 0).meter().help("The gain it applies now, 0 or less.");
    Param shortTermLoudness =
        decibels("Short-Term Loudness", -70, 0, -70)
            .meter()
            .help("The output's loudness over the last three seconds, in LUFS.");
    Param integratedLoudness =
        decibels("Integrated Loudness", -70, 0, -70)
            .meter()
            .help(
                "The output's gated loudness over the last 30 s, or since play started while "
                "accumulating, in LUFS.");
    Param truePeak =
        decibels("True Peak", -70, 6, -70)
            .meter()
            .help(
                "The output's highest true peak in the last 2 s, or since play started while "
                "accumulating, in dBTP.");
    Param loudnessRange =
        decibels("Loudness Range", 0, 40, 0)
            .meter()
            .help(
                "The output's loudness range (EBU R128 LRA) over the last 30 s, or since play "
                "started while accumulating, in LU.");

    void factoryPatches(Patches& p) {
        p.add("Transparent Master").set(gain, 3).set(ceiling, -1).set(release, 120);
        p.add("Streaming Loud").set(gain, 6).set(ceiling, -1).set(release, 90).set(target, -14);
        p.add("Club Master").set(gain, 9).set(ceiling, -0.3).set(release, 60).set(target, -8);
        p.add("Broadcast Safe").set(gain, 2).set(ceiling, -2).set(release, 200).set(target, -23);
        p.add("Acoustic Gentle").set(gain, 2).set(ceiling, -1).set(release, 300).set(target, -16);
        p.add("Bus Peak Catcher").set(gain, 0).set(ceiling, -3).set(release, 50);
    }

    // --- audio ------------------------------------------------------------------------------

    static constexpr double kBroadMs = 4.0;  // the broad attack, and the lookahead
    static constexpr double kSharpMs = 1.0;  // the sharp attack
    static constexpr double kHoldMs = 3.0;   // the hold past the broad attack
    static constexpr double kMargin = 0.0;   // dB kept under the ceiling

    Array<Array<double, 48>, 4> fir;
    Array<TruePeak, 2> detect, outDetect;
    Hold hold;
    Bell broad, sharp;
    Buffer<double> trail;  // the released gain, for the sharp curve's later start
    int trailAt = 0;
    Array<DelayLine, 2> delay;
    Array<Biquad, 2> bassLow;
    Array<KWeighting, 2> kOut, kIn;
    double rate = 48000.0;
    int broadSpan = 192, sharpSpan = 48, baseHold = 336, maxHold = 2400, latency = 215;
    // The engine's state.
    double plateau = 0.0;  // the sustained reduction, dB
    double gainDb = 0.0;   // the released reduction, dB
    double kPlateauIn = 0.0, kPlateauOut = 0.0, kRelease = 0.0, releaseNow = 120.0;
    double fastPower = 0.0, slowPower = 0.0, kFast = 0.0, kSlow = 0.0;
    double sharpness = 0.0, kSharpUp = 0.0, kSharpDown = 0.0;
    double bassPower = 0.0, allPower = 0.0, kBass = 0.0;
    double period = 0.0, guard = 0.0, kGuard = 0.0;
    int sinceCross = 0;
    bool armed = false;
    bool stereoNow = true;
    // Loudness: hundred-millisecond energies, their histograms, and the readings.
    Array<double, 30> blockOut, blockIn;
    int blockAt = 0, blocksFilled = 0, blockLength = 4800, blockCount = 0;
    double sumOut = 0.0, sumIn = 0.0;
    // Accumulated windows start at the restart sample, independently of the
    // rolling meters' blocks, weighting filters and interpolator history.
    Array<double, 30> totalsOut;
    Array<KWeighting, 2> totalsWeight;
    Array<TruePeak, 2> totalsDetect;
    int totalsAt = 0, totalsFilled = 0, totalsCount = 0, freshPeakSamples = 0;
    double totalsSum = 0.0;
    Array<double, kBins> gatedEnergy;
    Array<int, kBins> gatedCount, rangeCount;
    double gatedTotal = 0.0, rangeTotal = 0.0;
    int gatedBlocks = 0, rangeBlocks = 0;
    double momentaryNow = -120.0, shortNow = -120.0, integratedNow = -120.0, rangeNow = 0.0;
    double shortIn = -120.0, peakHeld = 0.0, measuredSeconds = 0.0;
    int overs = 0;
    bool accumulating = false;
    // The recent meters: the last 30 s in tenths of a second (momentary
    // energies, short-term loudness, true peaks), and overs until 5 s pass
    // without one.
    Array<double, kRecent> recentMomentary, recentShort, recentPeak, sorted;
    int recentAt = 0, recentFilled = 0;
    double blockPeak = 0.0, peak2 = 0.0, peak30 = 0.0;
    int recentOvers = 0, overAge = 0, overHold = 240000;
    // Listening.
    int listenMode = 0;
    LevelGain driveGain, overGain, matchGain;
    double matchDb = 0.0, kMatch = 0.0, kMatchOff = 0.0, deltaMix = 0.0, deltaStep = 0.001;
    bool unplayed = true;
    bool wasPlaying = false;
    int spanNow = 336;
    double lastPeakGr = 0.0, kLastPeak = 0.0;
    int lastPeakAge = 0, peakHoldSamples = 24000;
    // The editor's history: per column, the input's and output's true peaks,
    // the reduction and its sustained part, the momentary loudness, the beats.
    double colLength = 480.0, colAt = 0.0;
    double colIn = 0.0, colOut = 0.0, colGr = 0.0, colPlateau = 0.0, colEnergy = 0.0;
    Array<double, 40> colEnergies;
    int colEnergyAt = 0, colEnergyFilled = 0;
    int colBeat = 0, hw = 0, filled = 0;
    double beatPos = 0.0, beatNext = -1.0, fall = 0.999;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0;
    double envIn = 0.0, envOut = 0.0, envFall = 0.999;

    SharedArray<float, 1024> hIn, hOut, hGr, hPlateau, hLoud, hBeat;
    Shared<int> hHead, hFilled, oversShown;
    Shared<double> inLeft, inRight, outLeft, outRight, grShown, plateauShown, sharpShown;
    Shared<double> guardShown, holdShown, bassShown, releaseShown, sustainShown;
    Shared<double> momentaryShown, shortShown, integratedShown, rangeShown, peakShown;
    Shared<double> secondsShown, matchShown, plrPeakShown, grDeepShown;

    void prepare() {
        rate = sampleRate();
        // The detector's interpolators: a sinc under a Kaiser window, one per
        // phase, each summing to 1.
        const double beta = 8.0;
        for (int k = 0; k < kPhases; ++k) {
            double sum = 0.0;
            for (int j = 0; j < kTaps; ++j) {
                const double u = j - kHalf + k / (1.0 * kPhases);
                const double s = u == 0.0 ? 1.0 : sin(pi * u) / (pi * u);
                const double r = 1.0 - (u / kHalf) * (u / kHalf);
                const double w = r > 0.0 ? besselI0(beta * sqrt(r)) / besselI0(beta) : 0.0;
                fir[k][j] = s * w;
                sum += s * w;
            }
            for (int j = 0; j < kTaps; ++j) fir[k][j] /= sum;
        }
        broadSpan = max(2, roundToInt(kBroadMs * 0.001 * rate));
        sharpSpan = max(1, min(broadSpan, roundToInt(kSharpMs * 0.001 * rate)));
        baseHold = broadSpan + roundToInt(kHoldMs * 0.001 * rate);
        maxHold = baseHold + roundToInt(0.045 * rate);
        latency = broadSpan - 1 + kHalf;
        setLatency(latency);
        hold.prepare(maxHold + 2);
        broad.prepare(broadSpan);
        sharp.prepare(sharpSpan);
        trail.resize(broadSpan + 1);
        trail.fill(0.0);
        trailAt = 0;
        for (int c = 0; c < 2; ++c) {
            delay[c].prepare(latency + 8);
            bassLow[c].lowpass(110.0, 0.707);
            bassLow[c].clear();
            detect[c].clear();
            outDetect[c].clear();
            kOut[c].design(rate);
            kIn[c].design(rate);
            totalsWeight[c].design(rate);
            kOut[c].clear();
            kIn[c].clear();
        }
        kPlateauIn = stepFor(80.0, rate);
        kFast = stepFor(1.0, rate);
        kSlow = stepFor(50.0, rate);
        kSharpUp = stepFor(0.5, rate);
        kSharpDown = stepFor(40.0, rate);
        kBass = stepFor(30.0, rate);
        kGuard = stepFor(40.0, rate);
        kMatch = stepFor(400.0, rate);
        kMatchOff = stepFor(40.0, rate);
        deltaStep = 1.0 / (0.01 * rate);
        blockLength = max(1, roundToInt(0.1 * rate));
        overHold = roundToInt(5.0 * rate);
        peakHoldSamples = roundToInt(0.5 * rate);
        kLastPeak = stepFor(100.0, rate);
        lastPeakGr = 0.0;
        lastPeakAge = 0;
        accumulating = accumulate.on();
        setRelease(release);
        // A restart prepares an instance that has played: start it afresh.
        plateau = 0.0;
        gainDb = 0.0;
        fastPower = 0.0;
        slowPower = 0.0;
        sharpness = 0.0;
        bassPower = 0.0;
        allPower = 0.0;
        period = 0.0;
        guard = 0.0;
        sinceCross = 0;
        armed = false;
        restartLoudness();
        matchDb = 0.0;
        deltaMix = 0.0;
        wasPlaying = false;
        colLength = rate * 0.01;
        fall = exp(-1.0 / (0.3 * rate));
        envFall = exp(-1.0 / (0.12 * rate));
        envIn = 0.0;
        envOut = 0.0;
        colAt = 0.0;
        colIn = 0.0;
        colOut = 0.0;
        colGr = 0.0;
        colPlateau = 0.0;
        colEnergy = 0.0;
        colEnergies.fill(0.0);
        colEnergyAt = 0;
        colEnergyFilled = 0;
        colBeat = 0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        spanNow = baseHold;
        unplayed = true;
    }

    void setRelease(double ms) {
        releaseNow = ms;
        kRelease = stepFor(ms, rate);
        kPlateauOut = stepFor(clamp(10.0 * ms, 400.0, 4000.0), rate);
    }

    // Everything accumulated: on a start of play, and when accumulating is switched.
    void restartTotals() {
        totalsOut.fill(0.0);
        totalsAt = 0;
        totalsFilled = 0;
        totalsCount = 0;
        totalsSum = 0.0;
        for (int c = 0; c < 2; ++c) {
            totalsWeight[c].clear();
            totalsDetect[c].clear();
        }
        // After this many inputs the rolling interpolator contains only
        // post-restart samples, including its two cached peak neighbours.
        freshPeakSamples = kTaps + 2;
        gatedEnergy.fill(0.0);
        gatedCount.fill(0);
        rangeCount.fill(0);
        gatedTotal = 0.0;
        rangeTotal = 0.0;
        gatedBlocks = 0;
        rangeBlocks = 0;
        integratedNow = -120.0;
        rangeNow = 0.0;
        peakHeld = 0.0;
        measuredSeconds = 0.0;
        overs = 0;
    }

    void restartLoudness() {
        restartTotals();
        recentMomentary.fill(-1.0);
        recentShort.fill(-200.0);
        recentPeak.fill(0.0);
        recentAt = 0;
        recentFilled = 0;
        blockPeak = 0.0;
        peak2 = 0.0;
        peak30 = 0.0;
        recentOvers = 0;
        overAge = overHold;
        blockOut.fill(0.0);
        blockIn.fill(0.0);
        blockAt = 0;
        blocksFilled = 0;
        blockCount = 0;
        sumOut = 0.0;
        sumIn = 0.0;
        momentaryNow = -120.0;
        shortNow = -120.0;
        shortIn = -120.0;
    }

    void update() {
        listenMode = listen.index();
        if (release.changed() || unplayed) setRelease(release);
        if (unplayed) deltaMix = listenMode == 2 ? 1.0 : 0.0;
        if (accumulate.on() != accumulating) {
            accumulating = accumulate.on();
            restartTotals();
        }
    }

    // What the music is doing at the detector: how sudden it is (the sharp
    // attack's share), and whether a bass line leads it (the guard), from
    // the drive's output, linked across channels.
    void listenTo(double l, double r, double peak) {
        const double p2 = peak * peak;
        fastPower += kFast * (p2 - fastPower);
        slowPower += kSlow * (p2 - slowPower);
        const double jump = 10.0 * log10((fastPower + 1e-12) / (slowPower + 1e-12));
        const double want = clamp((jump - 3.0) / 6.0, 0.0, 1.0);
        sharpness += (want > sharpness ? kSharpUp : kSharpDown) * (want - sharpness);

        const double mono = stereoNow ? 0.5 * (l + r) : l;
        const double low = bassLow[1].process(bassLow[0].process(mono));
        bassPower += kBass * (low * low - bassPower);
        allPower += kBass * (mono * mono - allPower);
        // The bass's period, from its rising crossings (with some hysteresis).
        const double edge = 0.3 * sqrt(bassPower);
        sinceCross += 1;
        if (low < -edge) armed = true;
        if (armed && low > edge) {
            armed = false;
            const double low25 = rate / 25.0, high300 = rate / 300.0;
            if (sinceCross <= low25 && sinceCross >= high300)
                period = period <= 0.0 ? sinceCross : period + 0.35 * (sinceCross - period);
            sinceCross = 0;
        }
        const bool leads = period > 0.0 && sinceCross < rate / 20.0 && bassPower > 0.3 * allPower &&
                           bassPower > 1e-7;
        guard += kGuard * ((leads ? 1.0 : 0.0) - guard);
    }

    void process(Block& io) {
        unplayed = false;
        const Transport& t = io.transport;
        if (t.playing && !wasPlaying) restartLoudness();
        stereoNow = io.channels > 1;
        const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        const double beatStep = t.tempo / 60.0 / rate;
        beatPos = t.beats;
        // A beat at the block's first sample counts only if the song ran on
        // into it (not after a jump, a loop or a start).
        double prevPos =
            wasPlaying && abs(beatPos - beatNext) < 1e-3 ? beatPos - beatStep : beatPos;
        double deepest = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            double l = io[0][i];
            double r = stereoNow ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;
            const double drive = driveGain.get(gain.at(i));
            const double xl = l * drive, xr = r * drive;

            // The peak to hold, and the reduction that holds it.
            const double pl = detect[0].push(xl, fir);
            const double pr = stereoNow ? detect[1].push(xr, fir) : pl;
            const double peak = max(pl, pr);
            const double need = min(0.0, ceiling.at(i) - kMargin - gainToDb(peak));
            listenTo(xl, xr, peak);

            // Held for the broad attack and a little more, for a bass cycle
            // when the bass leads; released in two stages.
            int span = baseHold;
            if (guard > 0.01) span = max(span, roundToInt(baseHold + guard * (period - baseHold)));
            span = min(span, maxHold);
            spanNow = span;
            const double held = hold.push(need, span);
            plateau += (held < plateau ? kPlateauIn : kPlateauOut) * (held - plateau);
            const double back = gainDb < plateau ? gainDb + kRelease * (plateau - gainDb)
                                                 : gainDb - kPlateauOut * gainDb;
            gainDb = min(held, back);
            // The last peak's reduction, held half a second for the editor.
            if (gainDb <= lastPeakGr) {
                lastPeakGr = gainDb;
                lastPeakAge = 0;
            } else if (lastPeakAge < peakHoldSamples) {
                lastPeakAge += 1;
            } else {
                lastPeakGr += kLastPeak * (gainDb - lastPeakGr);
            }

            // The two attacks: the broad curve now, the sharp one over the
            // last of the same span; then their blend.
            const double gBroad = broad.step(gainDb);
            trail[trailAt] = gainDb;
            const int from = wrap(trailAt - (broadSpan - sharpSpan), broadSpan + 1);
            trailAt = trailAt == broadSpan ? 0 : trailAt + 1;
            const double gSharp = sharp.step(trail[from]);
            const double g = gBroad + sharpness * (gSharp - gBroad);

            // The audio, delayed to meet its gain.
            delay[0].push(xl);
            delay[1].push(xr);
            const double dl = delay[0].read(latency);
            const double dr = delay[1].read(latency);
            const double lin = dbToGain(g);
            double ol = dl * lin;
            double orr = dr * lin;
            deepest = min(deepest, g);

            // The output's true peak and loudness; the input's loudness.
            const double tl = outDetect[0].push(ol, fir);
            const double tr = stereoNow ? outDetect[1].push(orr, fir) : tl;
            const double tp = max(tl, tr);
            blockPeak = max(blockPeak, tp);
            const double over = overGain.get(ceiling.at(i) + 0.05);
            if (tp > over) {
                recentOvers += 1;
                overAge = 0;
            } else if (overAge < overHold) {
                overAge += 1;
                if (overAge == overHold) recentOvers = 0;
            }
            const double ko0 = kOut[0].process(ol);
            const double ki0 = kIn[0].process(l);
            double energy = ko0 * ko0;
            sumIn += ki0 * ki0;
            if (stereoNow) {
                const double ko1 = kOut[1].process(orr);
                const double ki1 = kIn[1].process(r);
                energy += ko1 * ko1;
                sumIn += ki1 * ki1;
            }
            sumOut += energy;
            blockCount += 1;
            if (blockCount >= blockLength) closeBlock();
            if (accumulating) accumulateSample(ol, orr, tp, over);

            // Listening.
            const bool matched = listenMode == 1 && shortIn > -70.0 && shortNow > -70.0;
            matchDb += (listenMode == 1 ? kMatch : kMatchOff) *
                       ((matched ? clamp(shortIn - shortNow, -30.0, 6.0) : 0.0) - matchDb);
            const double m = matchGain.get(matchDb);
            deltaMix = clamp(deltaMix + (listenMode == 2 ? deltaStep : -deltaStep), 0.0, 1.0);
            // Delta: what it took, at the level it came in.
            const double undo = 1.0 / drive;
            const double tookL = (dl - ol) * undo, tookR = (dr - orr) * undo;
            ol = ol * m + deltaMix * (tookL - ol * m);
            orr = orr * m + deltaMix * (tookR - orr * m);
            io[0][i] = ol;
            if (stereoNow) io[1][i] = orr;

            // What the editor shows.
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);
            envIn = max(peak, envIn * envFall);
            envOut = max(tp, envOut * envFall);
            colIn = max(colIn, envIn);
            colOut = max(colOut, envOut);
            colGr = min(colGr, g);
            colPlateau = min(colPlateau, plateau);
            colEnergy += energy;
            if (t.playing) {
                const double barLength = max(barBeats, 0.25);
                if (floor((beatPos - t.barStart) / barLength) >
                    floor((prevPos - t.barStart) / barLength))
                    colBeat = 2;
                else if (floor(beatPos) > floor(prevPos) && colBeat == 0)
                    colBeat = 1;
                prevPos = beatPos;
                beatPos += beatStep;
            }
            colAt += 1.0;
            if (colAt >= colLength) closeColumn();
        }
        wasPlaying = t.playing;
        beatNext = beatPos;
        hHead.set(hw);
        hFilled.set(filled);
        const double peakNow = accumulating ? peakHeld : max(peak2, blockPeak);
        oversShown.set(accumulating ? overs : recentOvers);
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(stereoNow ? peakIn1 : peakIn0));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(stereoNow ? peakOut1 : peakOut0));
        grShown.set(gainDb);
        grDeepShown.set(lastPeakGr);
        plateauShown.set(max(plateau, gainDb));
        sharpShown.set(sharpness);
        guardShown.set(guard);
        holdShown.set(1000.0 * spanNow / rate);
        bassShown.set(period > 0.0 ? rate / period : 0.0);
        releaseShown.set(releaseNow);
        sustainShown.set(clamp(10.0 * releaseNow, 400.0, 4000.0));
        momentaryShown.set(momentaryNow);
        shortShown.set(shortNow);
        integratedShown.set(integratedNow);
        rangeShown.set(rangeNow);
        peakShown.set(gainToDb(peakNow));
        plrPeakShown.set(gainToDb(accumulating ? peakHeld : max(peak30, blockPeak)));
        secondsShown.set(measuredSeconds + totalsCount / rate);
        matchShown.set(matchDb);
        gainReduction.set(deepest);
        shortTermLoudness.set(max(-70.0, shortNow));
        integratedLoudness.set(max(-70.0, integratedNow));
        truePeak.set(max(-70.0, gainToDb(peakNow)));
        loudnessRange.set(rangeNow);
    }

    void closeColumn() {
        colAt -= colLength;
        colEnergies[colEnergyAt] = colEnergy / colLength;
        colEnergyAt = colEnergyAt == 39 ? 0 : colEnergyAt + 1;
        colEnergyFilled = min(colEnergyFilled + 1, 40);
        double e = 0.0;
        for (int k = 0; k < colEnergyFilled; ++k) e += colEnergies[k];
        hIn[hw] = static_cast<float>(gainToDb(colIn));
        hOut[hw] = static_cast<float>(gainToDb(colOut));
        hGr[hw] = static_cast<float>(colGr);
        hPlateau[hw] = static_cast<float>(max(colPlateau, colGr));
        hLoud[hw] = static_cast<float>(lufs(e / max(1, colEnergyFilled)));
        hBeat[hw] = static_cast<float>(colBeat);
        hw = hw == kCols - 1 ? 0 : hw + 1;
        filled = min(filled + 1, kCols);
        colIn = 0.0;
        colOut = 0.0;
        colGr = 0.0;
        colPlateau = 0.0;
        colEnergy = 0.0;
        colBeat = 0;
    }

    // A hundred milliseconds for the rolling loudness and recent windows.
    // Accumulated measurements have their own restart-aligned windows below.
    void closeBlock() {
        blockOut[blockAt] = sumOut / blockCount;
        blockIn[blockAt] = sumIn / blockCount;
        blockAt = blockAt == 29 ? 0 : blockAt + 1;
        blocksFilled = min(blocksFilled + 1, 30);
        sumOut = 0.0;
        sumIn = 0.0;
        blockCount = 0;
        double m = 0.0, s = 0.0, si = 0.0;
        for (int k = 0; k < blocksFilled; ++k) {
            const int b = wrap(blockAt - 1 - k, 30);
            if (k < 4) m += blockOut[b];
            s += blockOut[b];
            si += blockIn[b];
        }
        momentaryNow = lufs(m / min(4, blocksFilled));
        shortNow = lufs(s / blocksFilled);
        shortIn = lufs(si / blocksFilled);
        recentMomentary[recentAt] = blocksFilled >= 4 ? m / 4.0 : -1.0;
        recentShort[recentAt] = blocksFilled >= 30 ? shortNow : -200.0;
        recentPeak[recentAt] = blockPeak;
        blockPeak = 0.0;
        recentAt = recentAt == kRecent - 1 ? 0 : recentAt + 1;
        recentFilled = min(recentFilled + 1, kRecent);
        peak2 = 0.0;
        peak30 = 0.0;
        for (int k = 0; k < recentFilled; ++k) {
            const double v = recentPeak[wrap(recentAt - 1 - k, kRecent)];
            peak30 = max(peak30, v);
            if (k < kRecentPeak) peak2 = max(peak2, v);
        }
        if (!accumulating) recentLoudness();
    }

    void accumulateSample(double l, double r, double rollingPeak, double over) {
        const double left = totalsWeight[0].process(l);
        double energy = left * left;
        if (stereoNow) {
            const double right = totalsWeight[1].process(r);
            energy += right * right;
        }
        totalsSum += energy;
        totalsCount += 1;
        double peak = rollingPeak;
        if (freshPeakSamples > 0) {
            const double pl = totalsDetect[0].push(l, fir);
            const double pr = stereoNow ? totalsDetect[1].push(r, fir) : pl;
            peak = max(pl, pr);
            freshPeakSamples -= 1;
        }
        peakHeld = max(peakHeld, peak);
        if (peak > over) overs += 1;
        if (totalsCount >= blockLength) closeTotalsBlock();
    }

    void closeTotalsBlock() {
        totalsOut[totalsAt] = totalsSum / totalsCount;
        totalsAt = totalsAt == 29 ? 0 : totalsAt + 1;
        totalsFilled = min(totalsFilled + 1, 30);
        measuredSeconds += totalsCount / rate;
        totalsSum = 0.0;
        totalsCount = 0;
        double m = 0.0, s = 0.0;
        for (int k = 0; k < totalsFilled; ++k) {
            const double v = totalsOut[wrap(totalsAt - 1 - k, 30)];
            if (k < 4) m += v;
            s += v;
        }
        const double momentary = lufs(m / min(4, totalsFilled));
        const double shortTerm = lufs(s / totalsFilled);
        if (totalsFilled >= 4 && momentary > -70.0) {
            const int bin = clamp(floorToInt((momentary + 70.0) * 10.0), 0, kBins - 1);
            gatedEnergy[bin] += m / 4.0;
            gatedCount[bin] += 1;
            gatedTotal += m / 4.0;
            gatedBlocks += 1;
            // Gated twice: at -70 LUFS, then 10 LU under what passed.
            const double gate = lufs(gatedTotal / gatedBlocks) - 10.0;
            double e = 0.0;
            int n = 0;
            for (int b = 0; b < kBins; ++b) {
                if (gatedCount[b] == 0 || -70.0 + (b + 0.5) * 0.1 <= gate) continue;
                e += gatedEnergy[b];
                n += gatedCount[b];
            }
            integratedNow = n > 0 ? lufs(e / n) : -120.0;
        }
        if (totalsFilled >= 30 && shortTerm > -70.0) {
            const int bin = clamp(floorToInt((shortTerm + 70.0) * 10.0), 0, kBins - 1);
            rangeCount[bin] += 1;
            rangeTotal += s / 30.0;
            rangeBlocks += 1;
            // Gated at -70 LUFS and 20 LU under; then the 10th to the 95th percentile.
            const double gate = lufs(rangeTotal / rangeBlocks) - 20.0;
            int n = 0;
            for (int b = 0; b < kBins; ++b)
                if (-70.0 + (b + 0.5) * 0.1 > gate) n += rangeCount[b];
            if (n > 1) {
                int seen = 0;
                double low = 0.0, high = 0.0;
                bool lowFound = false;
                for (int b = 0; b < kBins; ++b) {
                    if (-70.0 + (b + 0.5) * 0.1 <= gate) continue;
                    seen += rangeCount[b];
                    if (!lowFound && seen >= 0.10 * n) {
                        low = -70.0 + (b + 0.5) * 0.1;
                        lowFound = true;
                    }
                    if (seen >= 0.95 * n) {
                        high = -70.0 + (b + 0.5) * 0.1;
                        break;
                    }
                }
                rangeNow = max(0.0, high - low);
            }
        }
    }

    // The last 30 s as BS.1770 and EBU 3342 gate a whole programme.
    void recentLoudness() {
        double e = 0.0;
        int n = 0;
        for (int k = 0; k < recentFilled; ++k) {
            const double v = recentMomentary[k];
            if (v >= 0.0 && lufs(v) > -70.0) {
                e += v;
                n += 1;
            }
        }
        integratedNow = -120.0;
        if (n > 0) {
            // The relative gate must not admit blocks the absolute gate rejected.
            const double gate = max(-70.0, lufs(e / n) - 10.0);
            double passed = 0.0;
            int m = 0;
            for (int k = 0; k < recentFilled; ++k) {
                const double v = recentMomentary[k];
                if (v >= 0.0 && lufs(v) > gate) {
                    passed += v;
                    m += 1;
                }
            }
            integratedNow = m > 0 ? lufs(passed / m) : -120.0;
        }
        // The range: short-term values gated at -70 LUFS and 20 LU under,
        // from the 10th to the 95th percentile.
        double se = 0.0;
        int sn = 0;
        for (int k = 0; k < recentFilled; ++k) {
            const double v = recentShort[k];
            if (v > -70.0) {
                se += pow(10.0, (v + 0.691) / 10.0);
                sn += 1;
            }
        }
        rangeNow = 0.0;
        if (sn < 2) return;
        const double gate = lufs(se / sn) - 20.0;
        int count = 0;
        for (int k = 0; k < recentFilled; ++k) {
            const double v = recentShort[k];
            if (v <= -70.0 || v <= gate) continue;
            int j = count;
            while (j > 0 && sorted[j - 1] > v) {
                sorted[j] = sorted[j - 1];
                j -= 1;
            }
            sorted[j] = v;
            count += 1;
        }
        if (count < 2) return;
        const double low = sorted[roundToInt(0.10 * (count - 1))];
        const double high = sorted[roundToInt(0.95 * (count - 1))];
        rangeNow = max(0.0, high - low);
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1000;
    static constexpr double kH = 700;
    static constexpr double hx = 16;  // the history
    static constexpr double hy = 60;
    static constexpr double hwid = 700;
    static constexpr double hh = 364;
    static constexpr double gx = 28;  // its level graph
    static constexpr double gy = 72;
    static constexpr double gw = 636;
    static constexpr double gh = 236;
    static constexpr double ly = 318;  // its reduction lane
    static constexpr double lh = 94;
    static constexpr double kTop = 9.0;      // the graph's top, dBFS
    static constexpr double kFloor = -27.0;  // its floor
    static constexpr double kLane = 9.0;     // the lane's depth, dB
    static constexpr double mx = 732;        // the loudness panel
    static constexpr double mw = 252;
    static constexpr double by = 440;  // the bottom panels
    static constexpr double bh = 244;
    static constexpr double cw = 440;  // the limiter's controls
    static constexpr double ex = 472;  // the engine
    static constexpr double ew = 512;
    static constexpr double knobY = 548;
    static constexpr double pillY = 638;
    static constexpr double sliderX = 790;  // the target's slider
    static constexpr double sliderY = 398;
    static constexpr double sliderW = 110;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the ceiling, 3 the target, 4 the gain, 5 the target's slider
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double sharpEased = 0.0, guardEased = 0.0, grEased = 0.0, grPeak = 0.0;
    int grAge = 0;
    bool drawn = false;
    double clockX = mx + mw - 60;  // the clock's centre, where the timer's text leaves it
    double recoveryScale = 6.0;    // dB the release plot spans

    void editor(Editor& e) { e.size(kW, kH); }

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
              int colour) {
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
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value.
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool hot,
              const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 34 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        g.strokeStyle(colour);
        g.lineWidth(r > 34 ? 4.5 : 3.5);
        g.beginPath();
        g.arc(x, y, r, start, max(start + 0.001, at));
        g.stroke();
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

    void valueStyle(Canvas& g, double size) {
        g.fillStyle(Theme::text);
        g.monoFont(size);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    // A small horizontal slider: a track lit to the value and a round thumb.
    void slider(Canvas& g, double x, double y, double w, double norm, int colour) {
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x, y - 3, w, 6, 3);
        g.fill();
        const double at = x + w * clamp(norm, 0.0, 1.0);
        g.fillStyle(colour);
        g.globalAlpha(0.85);
        g.beginPath();
        g.roundRect(x, y - 3, at - x, 6, 3);
        g.fill();
        g.globalAlpha(1.0);
        g.radialGradient(at, y - 2, 1, 7, 0x6B5B4C, kKnobDark);
        g.beginPath();
        g.circle(at, y, 6.5);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(1.2);
        g.beginPath();
        g.circle(at, y, 6.5);
        g.stroke();
    }

    // A signed level, where the fill style and alignment are set; one that
    // rounds to nothing reads 0.0, not -0.0 or +0.0.
    void fillDb(Canvas& g, double db, double x, double y) {
        if (abs(db) < 0.05)
            g.fillText("0.0 dB", x, y);
        else
            g.fillText(format("%+.1f dB", db), x, y);
    }

    void fillTime(Canvas& g, double ms, double x, double y) {
        if (ms < 10.0)
            g.fillText(format("%.1f ms", ms), x, y);
        else if (ms < 1000.0)
            g.fillText(format("%.0f ms", ms), x, y);
        else
            g.fillText(format("%.1f s", ms * 0.001), x, y);
    }

    // The mark: a mountainside, its trees stopping at a line, bare stone and
    // snow above it.
    void drawMark(Canvas& g, double x, double y) {
        g.fillStyle(0x5A4B3E);
        g.beginPath();
        g.moveTo(x - 17, y + 12);
        g.lineTo(x - 3, y - 13);
        g.lineTo(x + 3, y - 6);
        g.lineTo(x + 6, y - 9);
        g.lineTo(x + 17, y + 12);
        g.closePath();
        g.fill();
        g.fillStyle(Theme::text);
        g.beginPath();
        g.moveTo(x - 3, y - 13);
        g.lineTo(x - 6.4, y - 7);
        g.lineTo(x - 3.5, y - 8.2);
        g.lineTo(x - 1.2, y - 6.6);
        g.lineTo(x + 0.6, y - 9.6);
        g.closePath();
        g.fill();
        g.strokeStyle(Theme::accent2);
        g.lineWidth(1.1);
        for (double dx = -12; dx < 13; dx += 4) {
            g.beginPath();
            g.moveTo(x + dx, y + 0.5);
            g.lineTo(x + dx + 2.2, y + 0.5);
            g.stroke();
        }
        Array<double, 6> tx = {-12.0, -7.5, -3.0, 2.0, 6.5, 11.0};
        Array<double, 6> ts = {4.2, 5.6, 6.6, 6.0, 5.2, 4.0};
        for (int k = 0; k < 6; ++k) {
            const double base = y + 12, top = y + 12 - ts[k] * 1.55;
            g.fillStyle(imod(k, 2) == 0 ? kSpruce : 0x6E8A63);
            g.beginPath();
            g.moveTo(x + tx[k], top);
            g.lineTo(x + tx[k] - ts[k] * 0.5, base);
            g.lineTo(x + tx[k] + ts[k] * 0.5, base);
            g.closePath();
            g.fill();
        }
    }

    // --- the header -------------------------------------------------------------------------

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 4, TextAlign::Right);
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double my = y + c * 6;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x, my, w, 4, 2);
            g.fill();
            const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(x, my, w * t, 4, 2);
                g.fill();
            }
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 32, 30);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Treeline", 56, 37);
        const double tw = g.measureText("Treeline");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("limiter", 64 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 620, 23, 140, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 824, 23, 140, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the history ----------------------------------------------------------------------

    double levelY(double db) const {
        return gy + gh * clamp((kTop - db) / (kTop - kFloor), 0.0, 1.0);
    }
    double laneY(double db) const { return ly + 4 + (lh - 8) * clamp(-db / kLane, 0.0, 1.0); }

    // Column j back from the newest, averaged with its neighbours.
    double eased(const SharedArray<float, 1024>& v, int head, int j, int count) const {
        const double here = v[wrap(head - 1 - j, kCols)];
        const double newer = j > 0 ? v[wrap(head - j, kCols)] : here;
        const double older = j + 1 < count ? v[wrap(head - 2 - j, kCols)] : here;
        return 0.25 * newer + 0.5 * here + 0.25 * older;
    }

    // A history as a path through the midpoints of its columns: the level
    // graph's (lane false) or the lane's.
    void trace(Canvas& g, const SharedArray<float, 1024>& v, int head, int count, double step,
               bool lane, bool fromEdge) {
        double px = gx + gw;
        double py = lane ? laneY(eased(v, head, 0, count)) : levelY(eased(v, head, 0, count));
        if (fromEdge) {
            g.moveTo(px, lane ? ly : gy + gh);
            g.lineTo(px, py);
        } else {
            g.moveTo(px, py);
        }
        for (int j = 1; j < count; ++j) {
            const double x = gx + gw - j * step;
            const double y =
                lane ? laneY(eased(v, head, j, count)) : levelY(eased(v, head, j, count));
            g.quadraticCurveTo(px, py, 0.5 * (px + x), 0.5 * (py + y));
            px = x;
            py = y;
        }
        g.lineTo(px, py);
        if (fromEdge) {
            g.lineTo(px, lane ? ly : gy + gh);
            g.closePath();
        }
    }

    void drawHistory(Canvas& g) {
        panel(g, hx, hy, hwid, hh);
        well(g, gx, gy, gw, gh);
        well(g, gx, ly, gw, lh);
        const int head = hHead.get();
        const int count = min(hFilled.get(), kShown);
        const double step = gw / (kShown - 1.0);
        const double cy = levelY(ceiling);
        g.save();
        g.clip(gx, gy, gw, gh);
        // The grid, every 6 dB, full scale a little brighter; above the
        // ceiling, a faint sky.
        g.fillStyle(0x2A2A24);
        g.globalAlpha(0.35);
        g.fillRect(gx, gy, gw, cy - gy);
        g.globalAlpha(1.0);
        for (int k = 0; k < 7; ++k) {
            const double db = 6.0 - 6.0 * k;
            if (db >= kTop || db <= kFloor) continue;
            const double y = levelY(db);
            g.strokeStyle(db == 0.0 ? 0x4C3F34 : (imod(k, 2) == 0 ? kGridLit : kGrid));
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(gx, y + 0.5);
            g.lineTo(gx + gw, y + 0.5);
            g.stroke();
        }
        // Bars and beats.
        for (int j = 0; j < count; ++j) {
            const double beat = hBeat[wrap(head - 1 - j, kCols)];
            if (beat < 0.5) continue;
            const double x = gx + gw - j * step;
            g.strokeStyle(beat > 1.5 ? 0x4C3F34 : kGridLit);
            g.globalAlpha(beat > 1.5 ? 0.9 : 0.5);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(x, gy);
            g.lineTo(x, gy + gh);
            g.stroke();
            g.globalAlpha(1.0);
        }
        if (count > 1) {
            // What came in, driven: above the ceiling, in clay, the part held down.
            g.save();
            g.clip(gx, gy, gw, cy - gy);
            g.beginPath();
            trace(g, hIn, head, count, step, false, true);
            g.fillStyle(kClay);
            g.globalAlpha(0.42);
            g.fill();
            g.globalAlpha(1.0);
            g.restore();
            // What goes out.
            g.beginPath();
            trace(g, hOut, head, count, step, false, true);
            g.linearGradient(0, gy, 0, gy + gh, 0x6A5F52, 0x2A231D);
            g.globalAlpha(0.8);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            trace(g, hOut, head, count, step, false, false);
            g.strokeStyle(kCurve);
            g.globalAlpha(0.8);
            g.lineWidth(1.1);
            g.lineJoin(LineJoin::Round);
            g.stroke();
            g.beginPath();
            trace(g, hIn, head, count, step, false, false);
            g.strokeStyle(kLinen);
            g.globalAlpha(0.3);
            g.lineWidth(1);
            g.stroke();
            g.globalAlpha(1.0);
            // The momentary loudness.
            g.beginPath();
            trace(g, hLoud, head, count, step, false, false);
            g.strokeStyle(kInk);
            g.globalAlpha(0.6);
            g.lineWidth(4);
            g.stroke();
            g.globalAlpha(1.0);
            g.strokeStyle(kMarigold);
            g.lineWidth(1.6);
            g.stroke();
        }
        // The target, dotted, in the loudness's colour.
        const double ty = levelY(target);
        const bool hotTarget =
            dragging == 3 || (dragging == 0 && abs(hoverY - ty) < 6 && hoverX > gx &&
                              hoverX < gx + gw && abs(hoverY - cy) >= 6);
        g.fillStyle(kMarigold);
        g.globalAlpha(hotTarget ? 1.0 : 0.6);
        for (double x = gx + 3; x < gx + gw - 60; x += 6)
            g.fillRect(x, ty - 0.5, 2, hotTarget ? 2 : 1);
        g.globalAlpha(1.0);
        // The ceiling: the treeline itself.
        const bool hotCeiling = dragging == 2 || (dragging == 0 && abs(hoverY - cy) < 6 &&
                                                  hoverX > gx && hoverX < gx + gw);
        g.strokeStyle(Theme::accent2);
        g.globalAlpha(hotCeiling ? 1.0 : 0.85);
        g.lineWidth(hotCeiling ? 2.0 : 1.4);
        g.beginPath();
        g.moveTo(gx, cy + 0.5);
        g.lineTo(gx + gw, cy + 0.5);
        g.stroke();
        g.globalAlpha(1.0);
        g.restore();
        // The ceiling's and the target's tags, at the right.
        tag(g, gx + gw - 58, cy, Theme::accent2, true);
        tag(g, gx + gw - 58, ty, kMarigold, false);
        // The level scale, in the margin.
        g.monoFont(8.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 0; k < 6; ++k) {
            const double db = 6.0 - 6.0 * k;
            g.fillStyle(Theme::textFaint);
            g.fillText(db == 0.0 ? "0" : format("%+.0f", db), gx + gw + 30, levelY(db));
        }
        drawLane(g, head, count, step);
        // The legend, on a dark backing so the levels under it do not show through.
        const double lgY = gy + 13;
        g.fillStyle(kInk);
        g.globalAlpha(0.72);
        g.beginPath();
        g.roundRect(gx + 6, lgY - 9, 404, 18, 9);
        g.fill();
        g.globalAlpha(1.0);
        g.fillStyle(kClay);
        g.globalAlpha(0.6);
        g.fillRect(gx + 14, lgY - 3, 10, 6);
        g.globalAlpha(1.0);
        label(g, "HELD DOWN", gx + 29, lgY, TextAlign::Left);
        g.fillStyle(0x6A5F52);
        g.fillRect(gx + 94, lgY - 3, 10, 6);
        g.fillStyle(kCurve);
        g.fillRect(gx + 94, lgY - 3, 10, 1);
        label(g, "OUTPUT", gx + 109, lgY, TextAlign::Left);
        g.fillStyle(kMarigold);
        g.fillRect(gx + 156, lgY - 1, 10, 2);
        label(g, "LOUDNESS", gx + 171, lgY, TextAlign::Left);
        g.fillStyle(kMoss);
        g.fillRect(gx + 230, lgY - 3, 10, 6);
        label(g, "SUSTAINED GR", gx + 245, lgY, TextAlign::Left);
        g.fillStyle(kClay);
        g.fillRect(gx + 324, lgY - 1, 10, 2);
        label(g, "PEAKS' GR", gx + 339, lgY, TextAlign::Left);
        // Listening to something else: say so, plainly.
        const int mode = listen.index();
        if (mode > 0) {
            g.fillStyle(kInk);
            g.globalAlpha(0.85);
            g.beginPath();
            g.roundRect(gx + gw - 236, gy + 6, 170, 18, 9);
            g.fill();
            g.globalAlpha(1.0);
            g.fillStyle(Theme::accent);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            if (mode == 1)
                g.fillText(format("HEARING MATCHED  %+.1f dB", matchShown.get()), gx + gw - 151,
                           gy + 15.5);
            else
                g.fillText("HEARING WHAT IT TAKES", gx + gw - 151, gy + 15.5);
        }
    }

    // A value's tab at the right of the graph: the ceiling's (dBTP) or the target's (LUFS).
    void tag(Canvas& g, double x, double y, int colour, bool isCeiling) {
        const double at = clamp(y, gy + 8, gy + gh - 8);
        g.fillStyle(kInk);
        g.globalAlpha(0.9);
        g.beginPath();
        g.roundRect(x, at - 8, 56, 16, 4);
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(colour);
        g.globalAlpha(0.6);
        g.lineWidth(1);
        g.stroke();
        g.globalAlpha(1.0);
        g.fillStyle(colour);
        g.monoFont(9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        if (isCeiling)
            g.fillText(format("%.1f TP", ceiling.value()), x + 28, at + 0.5);
        else
            g.fillText(format("%.0f LUFS", target.value()), x + 28, at + 0.5);
    }

    // The reduction lane: the sustained part in moss, the peaks' in clay below it.
    void drawLane(Canvas& g, int head, int count, double step) {
        g.save();
        g.clip(gx, ly, gw, lh);
        for (int k = 1; k < 3; ++k) {
            const double y = laneY(-3.0 * k);
            g.strokeStyle(kGrid);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(gx, y + 0.5);
            g.lineTo(gx + gw, y + 0.5);
            g.stroke();
        }
        if (count > 1) {
            g.beginPath();
            trace(g, hGr, head, count, step, true, true);
            g.fillStyle(kClay);
            g.globalAlpha(0.3);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            trace(g, hPlateau, head, count, step, true, true);
            g.fillStyle(kMoss);
            g.globalAlpha(0.55);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            trace(g, hGr, head, count, step, true, false);
            g.strokeStyle(kClay);
            g.lineWidth(1.6);
            g.lineJoin(LineJoin::Round);
            g.stroke();
        }
        g.restore();
        g.monoFont(8.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillStyle(Theme::textFaint);
        for (int k = 1; k < 3; ++k)
            g.fillText(format("%.0f", -3.0 * k), gx + gw + 30, laneY(-3.0 * k));
        g.fillText("GR", gx + gw + 30, ly + 8);
    }

    // --- loudness -----------------------------------------------------------------------------

    static constexpr double applyX = mx + mw - 16 - 86;  // the gain-to-target button
    static constexpr double applyY = hy + 106;
    static constexpr double clockY = hy + 20;  // the timer's clock

    // The gain that would bring the integrated loudness to the target, when
    // it has been measured and is off by half a unit or more; the gain as it
    // is otherwise. Below the ceiling, loudness follows gain one for one;
    // above it a click gets most of the way, and another finishes.
    double suggestedGain() const {
        const double integ = integratedShown.get();
        if (integ <= -70.0 || abs(integ - target) < 0.5) return gain;
        return clamp(gain - (integ - target), 0.0, 24.0);
    }

    double barX(double lu) const {
        return mx + 34 + (mw - 52) * clamp((lu + 40.0) / 40.0, 0.0, 1.0);
    }

    void loudBar(Canvas& g, double y, double lu, const char* name) {
        label(g, name, mx + 18, y, TextAlign::Left);
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(mx + 34, y - 4, mw - 52, 8, 4);
        g.fill();
        if (lu > -40.0) {
            g.linearGradient(mx + 34, 0, mx + mw - 18, 0, 0x7A5E2E, kMarigold);
            g.beginPath();
            g.roundRect(mx + 34, y - 4, barX(lu) - mx - 34, 8, 4);
            g.fill();
        }
        g.fillStyle(Theme::text);
        g.fillRect(barX(target) - 0.75, y - 7, 1.5, 14);
    }

    void row(Canvas& g, double y, const char* name) {
        label(g, name, mx + 18, y, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
    }

    // The timer and its clock: accumulating since play started (a running
    // time), or following the last 30 s. A click on either switches.
    void drawClock(Canvas& g) {
        const bool on = accumulate.on();
        const double right = mx + mw - 16;
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        double tw = 0.0;
        if (on) {
            const int whole = toInt(secondsShown.get());
            g.fillStyle(Theme::textMuted);
            g.monoFont(9.5);
            g.fillText(format("%d:%02d", idiv(whole, 60), imod(whole, 60)), right, clockY);
            tw = g.measureText(format("%d:%02d", idiv(whole, 60), imod(whole, 60)));
        } else {
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(10.5);
            g.fillText("last 30 s", right, clockY);
            tw = g.measureText("last 30 s");
        }
        clockX = right - tw - 11;
        const bool hot = hoverY > clockY - 10 && hoverY < clockY + 10 && hoverX > clockX - 9 &&
                         hoverX < right + 2;
        const int colour = on ? kMarigold : (hot ? Theme::textMuted : Theme::textFaint);
        if (on) {
            g.fillStyle(kMarigold);
            g.globalAlpha(0.18);
            g.beginPath();
            g.circle(clockX, clockY, 6);
            g.fill();
            g.globalAlpha(1.0);
        }
        g.strokeStyle(colour);
        g.lineWidth(1.3);
        g.beginPath();
        g.circle(clockX, clockY, 5.5);
        g.stroke();
        g.lineWidth(1.2);
        g.beginPath();
        g.moveTo(clockX, clockY - 3.4);
        g.lineTo(clockX, clockY);
        g.lineTo(clockX + 2.6, clockY);
        g.stroke();
    }

    bool onClock(double x, double y) const {
        return y > clockY - 10 && y < clockY + 10 && x > clockX - 9 && x < mx + mw - 14;
    }

    void drawLoudness(Canvas& g) {
        panel(g, mx, hy, mw, hh);
        heading(g, "Loudness", mx + 16, hy + 20);
        drawClock(g);
        const double integ = integratedShown.get();
        const bool measured = integ > -70.0;
        label(g, "INTEGRATED", mx + 18, hy + 48, TextAlign::Left);
        g.fillStyle(measured ? Theme::text : Theme::textFaint);
        g.serifFont(40);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        if (measured)
            g.fillText(format("%.1f", integ), mx + 16, hy + 94);
        else
            g.fillText("--", mx + 16, hy + 94);
        const double nw = measured ? g.measureText(format("%.1f", integ)) : g.measureText("--");
        g.fillStyle(Theme::textMuted);
        g.boldFont(10);
        g.fillText("LUFS", mx + 22 + nw, hy + 94);
        g.textBaseline(TextBaseline::Middle);
        // Against the target.
        const double off = integ - target;
        if (!measured) {
            aside(g,
                  accumulate.on() ? "plays to measure, from the start of play"
                                  : "plays to measure, over the last 30 s",
                  mx + 18, hy + 116, TextAlign::Left);
        } else if (abs(off) < 0.5) {
            g.fillStyle(kMarigold);
            g.boldFont(10);
            g.textAlign(TextAlign::Left);
            g.fillText("ON TARGET", mx + 18, hy + 116);
            aside(g, format("within half a unit of %.0f LUFS", target.value()), mx + 18, hy + 133,
                  TextAlign::Left);
        } else if (off > 0.0) {
            g.fillStyle(kMarigold);
            g.boldFont(10);
            g.textAlign(TextAlign::Left);
            g.fillText(format("%.1f LU OVER TARGET", off), mx + 18, hy + 116);
            aside(g, format("a %.0f LUFS service plays it %.1f dB quieter", target.value(), off),
                  mx + 18, hy + 133, TextAlign::Left);
        } else {
            g.fillStyle(kMarigold);
            g.boldFont(10);
            g.textAlign(TextAlign::Left);
            g.fillText(format("%.1f LU UNDER TARGET", -off), mx + 18, hy + 116);
            aside(g, format("about %+.1f dB more gain reaches it", -off), mx + 18, hy + 133,
                  TextAlign::Left);
        }
        // One click towards the target: the gain the difference suggests.
        const double step = suggestedGain() - gain;
        if (abs(step) >= 0.05)
            pill(g, applyX, applyY, 86, 20, format("GAIN %+.1f", step), false, kMarigold);
        loudBar(g, hy + 164, momentaryShown.get(), "M");
        loudBar(g, hy + 184, shortShown.get(), "S");
        g.fillStyle(Theme::textFaint);
        g.monoFont(8);
        g.textAlign(TextAlign::Center);
        for (int k = 0; k <= 4; ++k) {
            const double lu = -40.0 + 10.0 * k;
            g.fillText(format("%.0f", lu), barX(lu), hy + 200);
        }
        // The numbers.
        const double tp = peakShown.get();
        row(g, hy + 226, "TRUE PEAK");
        g.fillStyle(tp > ceiling + 0.05 ? Theme::alert : Theme::text);
        if (tp > -70.0)
            g.fillText(format("%.2f dBTP", tp), mx + mw - 18, hy + 226);
        else
            g.fillText("--", mx + mw - 18, hy + 226);
        row(g, hy + 246, "LOUDNESS RANGE");
        g.fillText(rangeShown.get() > 0.0 ? format("%.1f LU", rangeShown.get()) : "--",
                   mx + mw - 18, hy + 246);
        row(g, hy + 266, "PEAK TO LOUDNESS");
        const double plrPeak = plrPeakShown.get();
        g.fillText(measured && plrPeak > -70.0 ? format("%.1f dB", plrPeak - integ) : "--",
                   mx + mw - 18, hy + 266);
        row(g, hy + 286, "SHORT-TERM");
        const double st = shortShown.get();
        g.fillText(st > -70.0 ? format("%.1f LUFS", st) : "--", mx + mw - 18, hy + 286);
        row(g, hy + 306, "OVER THE CEILING");
        const int n = oversShown.get();
        g.fillStyle(n > 0 ? Theme::alert : Theme::accent2);
        g.fillText(n > 0 ? format("%d samples", n) : "none", mx + mw - 18, hy + 306);
        // The target's slider.
        label(g, "TARGET", mx + 18, sliderY, TextAlign::Left);
        slider(g, sliderX, sliderY, sliderW, target.normalized(), kMarigold);
        g.fillStyle(kMarigold);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        g.fillText(format("%.0f LUFS", target.value()), mx + mw - 18, sliderY);
    }

    // --- the controls -----------------------------------------------------------------------

    double knobX(int k) const { return k == 0 ? 112 : (k == 1 ? 256 : 382); }
    double knobR(int k) const { return k == 0 ? 42 : 31; }

    void drawControls(Canvas& g) {
        panel(g, hx, by, cw, bh);
        heading(g, "Limiter", hx + 16, by + 20);
        aside(g, "drive it up into the ceiling", hx + cw - 16, by + 20, TextAlign::Right);
        dial(g, knobX(0), knobY, knobR(0), gain.normalized(), Theme::accent, hotKnob == 0, "GAIN");
        dial(g, knobX(1), knobY, knobR(1), ceiling.normalized(), Theme::accent2, hotKnob == 1,
             "CEILING");
        dial(g, knobX(2), knobY, knobR(2), release.normalized(), kMoss, hotKnob == 2, "RELEASE");
        valueStyle(g, 12);
        g.fillText(format("%+.1f dB", gain.value()), knobX(0), knobY + 60);
        valueStyle(g, 10.5);
        g.fillText(format("%.1f dBTP", ceiling.value()), knobX(1), knobY + 48);
        fillTime(g, release, knobX(2), knobY + 48);
        label(g, "LISTEN", hx + 18, pillY + 11, TextAlign::Left);
        const int mode = listen.index();
        pill(g, hx + 72, pillY, 90, 22, "OUTPUT", mode == 0, Theme::text);
        pill(g, hx + 170, pillY, 90, 22, "MATCHED", mode == 1, Theme::accent);
        pill(g, hx + 268, pillY, 90, 22, "DELTA", mode == 2, Theme::accent);
    }

    // --- the engine ---------------------------------------------------------------------------

    // Smoothstep: an S from 0 to 1 over u in 0..1.
    double ease(double u) const {
        const double v = clamp(u, 0.0, 1.0);
        return v * v * (3.0 - 2.0 * v);
    }

    void drawEngine(Canvas& g) {
        panel(g, ex, by, ew, bh);
        heading(g, "Engine", ex + 16, by + 20);
        aside(g, "what it hears, and how it answers", ex + ew - 16, by + 20, TextAlign::Right);
        // Eased at the editor's frame rate; the first frame starts where the audio is.
        const double follow = drawn ? 0.25 : 1.0;
        sharpEased += follow * (sharpShown.get() - sharpEased);
        guardEased += follow * (guardShown.get() - guardEased);
        const double pw = 148, ph = 74, py = by + 44;
        drawAttack(g, ex + 16, py, pw, ph);
        drawGuard(g, ex + 182, py, pw, ph);
        drawRecovery(g, ex + 348, py, pw, ph);
        drawReduction(g, ex + 16, by + 200, ew - 32);
    }

    void plotTitle(Canvas& g, const char* name, double x, double y) {
        label(g, name, x, y - 9, TextAlign::Left);
    }

    // The attack: the broad curve and the sharp one into a peak, and the blend it uses now.
    void drawAttack(Canvas& g, double x, double y, double w, double h) {
        plotTitle(g, "ATTACK", x, y);
        well(g, x, y, w, h);
        const double peakX = x + w * 0.78, top = y + 14, depth = h - 26;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(peakX + 0.5, y + 4);
        g.lineTo(peakX + 0.5, y + h - 4);
        g.stroke();
        const double span = (peakX - x - 6) / kBroadMs;  // px per ms
        for (int pass = 0; pass < 3; ++pass) {
            const double mix = pass == 0 ? 0.0 : (pass == 1 ? 1.0 : sharpEased);
            g.beginPath();
            for (int k = 0; k <= 60; ++k) {
                const double px = x + 4 + (w - 8) * k / 60.0;
                const double ms = (px - peakX) / span;  // 0 at the peak
                const double broadPart = ease((ms + kBroadMs) / kBroadMs);
                const double sharpPart = ease((ms + kSharpMs) / kSharpMs);
                const double v = broadPart + mix * (sharpPart - broadPart);
                const double py = top + depth * v;
                if (k == 0)
                    g.moveTo(px, py);
                else
                    g.lineTo(px, py);
            }
            g.strokeStyle(pass == 2 ? Theme::accent : Theme::textFaint);
            g.globalAlpha(pass == 2 ? 1.0 : 0.35);
            g.lineWidth(pass == 2 ? 2.0 : 1.0);
            g.stroke();
            g.globalAlpha(1.0);
        }
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(9.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("peak", peakX - 4, y + 9);
        const bool hit = sharpEased > 0.5;
        g.fillStyle(Theme::text);
        g.boldFont(10);
        g.textAlign(TextAlign::Left);
        g.fillText(hit ? "A HIT: SHARP" : "HELD SOUND: BROAD", x, y + h + 16);
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.fillText(format("%.1f ms to the peak", kBroadMs + sharpEased * (kSharpMs - kBroadMs)), x,
                   y + h + 32);
        aside(g, "keeps a hit's snap, a tone's purity", x, y + h + 48, TextAlign::Left);
    }

    // The bass guard: the bass it hears and the hold that spans a cycle of it.
    void drawGuard(Canvas& g, double x, double y, double w, double h) {
        plotTitle(g, "BASS GUARD", x, y);
        well(g, x, y, w, h);
        const double hz = bassShown.get();
        const bool on = guardEased > 0.5 && hz > 0.0;
        const double holdMs = holdShown.get();
        const double pxPerMs = (w - 12) / 50.0;  // 50 ms across
        const double mid = y + h * 0.5 - 4;
        g.beginPath();
        for (int k = 0; k <= 90; ++k) {
            const double px = x + 6 + (w - 12) * k / 90.0;
            const double ms = (px - x - 6) / pxPerMs;
            const double v = on ? sin(twoPi * hz * ms * 0.001 - 0.5 * pi) : 0.0;
            const double py = mid + v * (h * 0.3);
            if (k == 0)
                g.moveTo(px, py);
            else
                g.lineTo(px, py);
        }
        g.strokeStyle(on ? kMoss : Theme::textFaint);
        g.globalAlpha(on ? 0.95 : 0.4);
        g.lineWidth(1.6);
        g.stroke();
        g.globalAlpha(1.0);
        // The hold, a bracket from the first trough.
        const double x0 = x + 6, x1 = min(x + w - 6, x0 + holdMs * pxPerMs);
        const double hy0 = y + h - 12;
        g.strokeStyle(on ? Theme::accent2 : Theme::textMuted);
        g.lineWidth(1.4);
        g.beginPath();
        g.moveTo(x0, hy0 - 4);
        g.lineTo(x0, hy0);
        g.lineTo(x1, hy0);
        g.lineTo(x1, hy0 - 4);
        g.stroke();
        g.fillStyle(Theme::text);
        g.boldFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(on ? format("FOLLOWING %.0f HZ", hz) : "NO BASS LEADS", x, y + h + 16);
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.fillText(format("holds %.0f ms", holdMs), x, y + h + 32);
        aside(g, "never rides the bass waveform", x, y + h + 48, TextAlign::Left);
    }

    // The release: the way back from the last peak (the deepest reduction of
    // the last half second) to rest, if the music let go there. The peaks'
    // part recovers at the release time to what the music sustains (clay),
    // and that over seconds; at rest, a faint example of the shape.
    void drawRecovery(Canvas& g, double x, double y, double w, double h) {
        plotTitle(g, "RELEASE", x, y);
        well(g, x, y, w, h);
        const double rel = releaseShown.get(), sus = sustainShown.get();
        const double total = 2.0 * sus;  // ms across
        const double p0 = plateauShown.get();
        const double g0 = min(min(grDeepShown.get(), grShown.get()), p0);
        // The scale grows at once with the reduction and shrinks slowly.
        const double want = max(3.0, -1.15 * g0);
        recoveryScale =
            !drawn || want > recoveryScale ? want : recoveryScale + 0.03 * (want - recoveryScale);
        const double top = y + 8, depth = h - 18;
        // The reduction follows the sustained part at the release time
        // while that decays: g' = (p - g) / rel, p = p0 e^(-t / sus).
        const double b = p0 * sus / (sus - rel);
        for (int pass = 0; pass < 3; ++pass) {
            const bool example = pass == 0;
            if (example && g0 < -0.5) continue;
            const double start = example ? -0.75 * recoveryScale : g0;
            const double body = example ? 0.35 * start : p0;
            const double bb = example ? body * sus / (sus - rel) : b;
            g.beginPath();
            for (int k = 0; k <= 80; ++k) {
                const double px = x + 6 + (w - 12) * k / 80.0;
                const double ms = total * pow(k / 80.0, 1.8);
                const double sustained = body * exp(-ms / sus);
                const double v =
                    pass == 1 ? sustained : bb * exp(-ms / sus) + (start - bb) * exp(-ms / rel);
                const double py = top + depth * clamp(-v / recoveryScale, 0.0, 1.0);
                if (k == 0)
                    g.moveTo(px, py);
                else
                    g.lineTo(px, py);
            }
            if (pass == 1 && p0 > -0.05) continue;
            g.strokeStyle(example ? Theme::textFaint : (pass == 1 ? kClay : kMoss));
            g.globalAlpha(example ? 0.45 : (pass == 1 ? 0.7 : 1.0));
            g.lineWidth(pass == 2 ? 2.0 : 1.0);
            g.stroke();
            g.globalAlpha(1.0);
        }
        g.fillStyle(Theme::textFaint);
        g.monoFont(8);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("-%.0f dB", recoveryScale), x + w - 5, y + h - 7);
        g.fillStyle(Theme::text);
        g.boldFont(10);
        g.textAlign(TextAlign::Left);
        g.fillText("TWO STAGES", x, y + h + 16);
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.fillText(format("peaks %.0f ms, body %.1f s", rel, sus * 0.001), x, y + h + 32);
        aside(g, "no pumping, no dent after a hit", x, y + h + 48, TextAlign::Left);
    }

    // The reduction now: the sustained part and the peaks' part, with the peak held.
    void drawReduction(Canvas& g, double x, double y, double w) {
        const double now = grShown.get(), plat = plateauShown.get();
        grEased += (!drawn ? 1.0 : (now < grEased ? 0.6 : 0.2)) * (now - grEased);
        if (now < grPeak || grAge > 120) {
            grPeak = now;
            grAge = 0;
        } else {
            grAge += 1;
        }
        label(g, "GAIN REDUCTION", x, y, TextAlign::Left);
        const double bx = x + 100, bw = w - 176;
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(bx, y - 5, bw, 10, 5);
        g.fill();
        const double scale = bw / 12.0;
        const double total = min(12.0, -grEased), body = min(total, -plat);
        if (total > 0.02) {
            g.fillStyle(kClay);
            g.beginPath();
            g.roundRect(bx, y - 5, total * scale, 10, 5);
            g.fill();
        }
        if (body > 0.02) {
            g.fillStyle(kMoss);
            g.beginPath();
            g.roundRect(bx, y - 5, body * scale, 10, 5);
            g.fill();
        }
        const double pk = min(12.0, -grPeak);
        if (pk > 0.05) {
            g.fillStyle(Theme::text);
            g.fillRect(bx + pk * scale - 1, y - 7, 2, 14);
        }
        g.fillStyle(Theme::textFaint);
        g.monoFont(8);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 0; k <= 4; ++k)
            g.fillText(k == 0 ? "0" : format("%.0f", -3.0 * k), bx + 3.0 * k * scale, y + 14);
        g.fillStyle(abs(grEased) > 0.05 ? kCurve : Theme::textMuted);
        g.monoFont(11);
        g.textAlign(TextAlign::Right);
        fillDb(g, grEased, x + w, y);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawHistory(g);
        drawLoudness(g);
        drawControls(g);
        drawEngine(g);
        drawn = true;
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
        if (k == 0) return gain.normalized();
        if (k == 1) return ceiling.normalized();
        if (k == 2) return release.normalized();
        return target.normalized();
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, gain, v);
        if (k == 1) act(how, ceiling, v);
        if (k == 2) act(how, release, v);
        if (k == 3) act(how, target, v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 3; ++k) {
            const double dx = x - knobX(k), dy = y - knobY;
            const double r = knobR(k) + 6;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }

    bool onSlider(double x, double y) const {
        return y > sliderY - 10 && y < sliderY + 10 && x > sliderX - 10 &&
               x < sliderX + sliderW + 10;
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
        const double perDb = gh / (kTop - kFloor);
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            } else if (dragging == 2) {
                ceiling.edit(ceiling - p.dy() * fine / perDb);
            } else if (dragging == 3) {
                target.edit(target - p.dy() * fine / perDb);
            } else if (dragging == 4) {
                gain.edit(gain - p.dy() * fine / perDb);
            } else if (dragging == 5) {
                target.editNormalized(clamp((p.x() - sliderX) / sliderW, 0.0, 1.0));
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
            if (p.over(gx, gy, gw, gh)) {
                gain.edit(gain + notches * 0.5 * fine);
                return;
            }
            if (onSlider(p.x(), p.y())) {
                target.edit(target + notches * fine);
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
        if (onSlider(x, y)) {
            if (p.doubleClick()) {
                target.reset();
                return;
            }
            dragging = 5;
            target.editNormalized(clamp((x - sliderX) / sliderW, 0.0, 1.0));
            return;
        }
        if (p.over(gx, gy, gw, gh)) {
            const double cy = levelY(ceiling), ty = levelY(target);
            if (abs(y - cy) < 6) {
                if (p.doubleClick())
                    ceiling.reset();
                else
                    dragging = 2;
            } else if (abs(y - ty) < 6) {
                if (p.doubleClick())
                    target.reset();
                else
                    dragging = 3;
            } else if (p.doubleClick()) {
                gain.reset();
            } else {
                dragging = 4;
            }
            return;
        }
        if (onClock(x, y)) {
            act(0, accumulate, accumulate.on() ? 0 : 1);
            return;
        }
        if (p.over(applyX, applyY, 86, 20) && abs(suggestedGain() - gain) >= 0.05) {
            gain.edit(suggestedGain());
            return;
        }
        if (p.over(hx + 72, pillY, 90, 22)) act(0, listen, 0);
        if (p.over(hx + 170, pillY, 90, 22)) act(0, listen, listen.index() == 1 ? 0 : 1);
        if (p.over(hx + 268, pillY, 90, 22)) act(0, listen, listen.index() == 2 ? 0 : 2);
    }
};
