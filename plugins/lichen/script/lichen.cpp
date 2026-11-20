// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Lichen: an analyser that listens and never touches.
//
// Lichens grow on stone and bark and take nothing from them; what they
// gather tells you about the air. This one sits anywhere in a chain, passes
// every sample through as it came (the audio code never writes the block),
// and reports what it hears:
//
//   Loudness   BS.1770 and EBU R128: momentary, short-term, integrated
//              (gated), the loudness range, their highest, against a target.
//   Peaks      the true peak (the signal read at three points between each
//              pair of samples, every top refined by a parabola), the sample
//              peak, PLR and crest factor, overs, clipped runs, DC.
//   Stereo     correlation, width (the side's share), the low end's width,
//              balance, and what a mono fold-down costs in loudness.
//   Tone       a 16k-point transform (2.9 Hz bins at 48 kHz) smoothed to a
//              twelfth of an octave, its mid and side; the seven named bands,
//              the tilt and the centroid, as the offline analysis defines
//              them, so a live reading compares with an analysis report.
//   Harmony    the pitch classes heard, and the key they most resemble.
//
// The readings that summarise cover the last 60 s of the play or, with
// Accumulate on (the clock lit), all of it from start to stop. They restart
// when play starts (with Restart on Play) and hold when it stops, so an agent
// can play or render a passage and read them afterwards. With the transport
// never started (a live input) they keep gathering until reset.

// --- measuring ------------------------------------------------------------------------

constexpr int kPhases = 4;  // the true-peak detector: each sample and three points after it
constexpr int kTaps = 32;   // taps of each phase's interpolator
constexpr int kHalf = 16;   // the interpolator's centre

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
// each pair of samples (a Kaiser-windowed sinc per phase), each local top
// refined by a parabola. push() returns the peak around sample n - kHalf.
// Where the samples there are all below `gate` it returns the sample alone:
// a peak between samples of real audio rises at most about 3 dB above its
// neighbours, so 6 dB under the highest peak so far (or full scale) the
// interpolation would change nothing it reports.
struct TruePeak {
    Array<double, 64> hist;  // the last kTaps inputs, twice over, read as one run
    int at = 0;
    double before = 0.0;
    double lastPoint = 0.0;

    void clear() {
        hist.fill(0.0);
        at = 0;
        before = 0.0;
        lastPoint = 0.0;
    }

    double push(double x, const Array<Array<double, 4>, 32>& h, double gate) {
        at = at == 0 ? kTaps - 1 : at - 1;
        hist[at] = x;
        hist[at + kTaps] = x;
        const double a = abs(hist[at + kHalf]), b = abs(hist[at + kHalf - 1]);
        const double around =
            max(max(a, b), max(abs(hist[at + kHalf + 1]), abs(hist[at + kHalf - 2])));
        if (around < gate) {
            lastPoint = b;
            before = 0.0;
            return a;
        }
        Array<double, 6> s;
        s[0] = lastPoint;
        s[1] = abs(hist[at + kHalf]);
        // Read the history once for all phases, keeping each sum's tap order.
        double y1 = 0.0, y2 = 0.0, y3 = 0.0;
        for (int j = 0; j < kTaps; ++j) {
            const double xj = hist[at + j];
            y1 += h[j][1] * xj;
            y2 += h[j][2] * xj;
            y3 += h[j][3] * xj;
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

// Loudness in LUFS of a mean square (K-weighted, summed over channels).
double lufs(double meanSquare) {
    return meanSquare > 1e-12 ? -0.691 + 10.0 * log10(meanSquare) : -120.0;
}

// A power as dB, floored.
double powerDb(double p) { return p > 1e-30 ? 10.0 * log10(p) : -300.0; }

// The 31 ISO third-octave bands, 20 Hz to 20 kHz: centre b (0..30), base two.
double thirdHz(int b) { return 1000.0 * pow(2.0, (b - 17) / 3.0); }

constexpr int kPoints = 240;        // the drawn spectrum: 20 Hz to 20 kHz, 24 points an octave
constexpr int kSmooth = 12;         // smoothed over a twelfth of an octave
constexpr int kThirds = 31;         // third-octave bands
constexpr int kNamed = 7;           // sub, low, low mid, mid, high mid, presence, air
constexpr int kBins = 900;          // loudness histograms: -70 to +20 LUFS in tenths
constexpr int kCentroids = 300;     // the centroid's histogram, 20 Hz to 20 kHz
constexpr int kHistory = 600;       // a minute of loudness, tenths of a second
constexpr int kScope = 1024;        // the vectorscope's samples
constexpr int kSpread = 60;         // the short-term distribution drawn: -60 to 0 LUFS, 1 LU each
constexpr int kWindow = 60;         // the readings' window: a minute in seconds, the newest filling
constexpr int kWindowBlocks = 600;  // and in loudness blocks of 100 ms

// A second of what the summarising readings add up, so the window can drop
// its oldest: the spectrum's sums, the stereo sums and the peaks.
struct Second {
    Array<double, kPoints> allM;
    Array<double, kThirds> thirdL, thirdR, thirdX, thirdM;
    Array<double, kNamed> namedM;
    Array<double, 12> chroma;
    Array<int, kCentroids> centroids;
    double frames = 0.0, sumL = 0.0, sumR = 0.0, sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    double totalM = 0.0, lowM = 0.0, lowS = 0.0, peakTrue = 0.0, peakSample = 0.0;
    int framesAll = 0, centroidFrames = 0, overs = 0, clips = 0;

    void clear() {
        allM.fill(0.0);
        thirdL.fill(0.0);
        thirdR.fill(0.0);
        thirdX.fill(0.0);
        thirdM.fill(0.0);
        namedM.fill(0.0);
        chroma.fill(0.0);
        centroids.fill(0);
        frames = 0.0;
        sumL = 0.0;
        sumR = 0.0;
        sumLL = 0.0;
        sumRR = 0.0;
        sumLR = 0.0;
        totalM = 0.0;
        lowM = 0.0;
        lowS = 0.0;
        peakTrue = 0.0;
        peakSample = 0.0;
        framesAll = 0;
        centroidFrames = 0;
        overs = 0;
        clips = 0;
    }
};

// The pitch-class profiles keys are matched against: the share of each
// scale degree's duration in tonal music (Albrecht and Shanahan, 2013).
constexpr double kMajor0 = 0.238, kMajor1 = 0.006, kMajor2 = 0.111, kMajor3 = 0.006;
constexpr double kMajor4 = 0.137, kMajor5 = 0.094, kMajor6 = 0.016, kMajor7 = 0.214;
constexpr double kMajor8 = 0.009, kMajor9 = 0.080, kMajor10 = 0.008, kMajor11 = 0.081;
constexpr double kMinor0 = 0.220, kMinor1 = 0.006, kMinor2 = 0.104, kMinor3 = 0.123;
constexpr double kMinor4 = 0.019, kMinor5 = 0.103, kMinor6 = 0.012, kMinor7 = 0.214;
constexpr double kMinor8 = 0.062, kMinor9 = 0.022, kMinor10 = 0.061, kMinor11 = 0.052;

// --- the look --------------------------------------------------------------------------

constexpr int kInk = 0x17150F;
constexpr int kGroundTop = 0x24221A;
constexpr int kGroundBottom = 0x1A1913;
constexpr int kGraphTop = 0x1A1914;
constexpr int kGraphBottom = 0x11100C;
constexpr int kGrid = 0x2A281F;
constexpr int kGridLit = 0x3B3829;
constexpr int kMeterLow = 0x6B7A4A;
constexpr int kMeterHigh = 0xD6CF9E;
constexpr int kLinen = 0xE6DAC2;   // the spectrum as it is now
constexpr int kLichen = 0xB2BF8E;  // the mark, the momentary loudness, all is well
constexpr int kLichenDeep = 0x6F7D52;
constexpr int kRust = 0xD98A4C;      // a caution (the orange lichen on old roofs)
constexpr int kClay = 0xD4664E;      // a problem
constexpr int kMarigold = 0xE6B656;  // loudness, the long-term spectrum
constexpr int kSky = 0x8DB3C7;       // the side
constexpr int kBark = 0x9C7550;

int blend(int a, int b, double t) {
    const double u = clamp(t, 0.0, 1.0);
    const int ar = idiv(a, 65536), ag = imod(idiv(a, 256), 256), ab = imod(a, 256);
    const int br = idiv(b, 65536), bg = imod(idiv(b, 256), 256), bb = imod(b, 256);
    const int r = roundToInt(ar + (br - ar) * u), g = roundToInt(ag + (bg - ag) * u);
    const int bl = roundToInt(ab + (bb - ab) * u);
    return r * 65536 + g * 256 + bl;
}

struct Lichen : Effect {
    static constexpr Info info = {
        .name = "Lichen",
        .id = "com.suppleaudio.lichen",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Analyser that passes every sample untouched: loudness to BS.1770, true peak, "
            "stereo, spectrum and key, for the ear and for agents.",
        .features = {"analyzer"}};

    // --- settings (none of them touch the sound) ----------------------------------------

    Param target = decibels("Target", -30, -5, -14)
                       .modulated()
                       .in("Settings")
                       .help(
                           "The integrated loudness you aim for, in LUFS: the meters read "
                           "against it.");
    Param restartOnPlay =
        toggle("Restart on Play", true)
            .notAutomatable()
            .in("Settings")
            .help(
                "On, the summarising readings start afresh each time play starts; off, each "
                "play adds to them.");
    Param accumulate =
        toggle("Accumulate", false)
            .notAutomatable()
            .in("Settings")
            .help(
                "Off, the summarising readings follow the last 60 s; on, they total everything "
                "since play started. Switching starts them afresh.");
    Param reset = toggle("Reset", false)
                      .notAutomatable()
                      .in("Settings")
                      .help("Any change to it starts every reading afresh.");
    Param slope = choice("Slope", {"Flat", "3 dB/oct", "4.5 dB/oct"}, 2)
                      .keys({"flat", "db3", "db4_5"})
                      .notAutomatable()
                      .in("Settings")
                      .help(
                          "The tilt the spectrum is drawn with: at 4.5 dB/oct a balanced mix "
                          "reads level.");

    // --- loudness -----------------------------------------------------------------------

    Param integrated =
        decibels("Integrated", -70, 10, -70)
            .meter()
            .in("Loudness")
            .help("Gated loudness over the last 60 s or since play (BS.1770), in LUFS.");
    Param shortTerm = decibels("Short-Term", -70, 10, -70)
                          .meter()
                          .in("Loudness")
                          .help("Loudness over the last 3 s, in LUFS.");
    Param momentary = decibels("Momentary", -70, 10, -70)
                          .meter()
                          .in("Loudness")
                          .help("Loudness over the last 400 ms, in LUFS.");
    Param lra = decibels("Loudness Range", 0, 40, 0)
                    .meter()
                    .in("Loudness")
                    .help("EBU R128 loudness range over the last 60 s or since play, in LU.");
    Param shortTermMax = decibels("Short-Term Max", -70, 10, -70)
                             .meter()
                             .in("Loudness")
                             .help("The loudest 3 s of the last 60 s or since play, in LUFS.");
    Param momentaryMax = decibels("Momentary Max", -70, 10, -70)
                             .meter()
                             .in("Loudness")
                             .help("The loudest 400 ms of the last 60 s or since play, in LUFS.");

    // --- peaks and problems ---------------------------------------------------------------

    Param truePeak = decibels("True Peak", -70, 20, -70)
                         .meter()
                         .in("Peaks")
                         .help("The highest true peak of the last 60 s or since play, in dBTP.");
    Param samplePeak = decibels("Sample Peak", -70, 20, -70)
                           .meter()
                           .in("Peaks")
                           .help("The highest sample of the last 60 s or since play, in dBFS.");
    Param plr = decibels("PLR", 0, 70, 0)
                    .meter()
                    .in("Peaks")
                    .help("Peak to loudness ratio: true peak less integrated loudness, in dB.");
    Param crest = decibels("Crest", 0, 70, 0)
                      .meter()
                      .in("Peaks")
                      .help("Crest factor: the sample peak against the RMS level, in dB.");
    Param overs = integer("Overs", 0, 1000000, 0)
                      .meter()
                      .in("Peaks")
                      .help("How many times the true peak went above 0 dBTP.");
    Param clipped = integer("Clipped", 0, 1000000, 0)
                        .meter()
                        .in("Peaks")
                        .help(
                            "Runs of three or more samples held at full scale: clipping "
                            "already in the signal.");
    Param dcOffset = decibels("DC Offset", -120, 0, -120)
                         .meter()
                         .in("Peaks")
                         .help("The larger channel's average value, in dBFS.");

    // --- stereo ---------------------------------------------------------------------------

    Param correlation = continuous("Correlation", -1, 1, 1)
                            .meter()
                            .in("Stereo")
                            .help(
                                "+1 mono, 0 unrelated, below 0 the channels oppose each "
                                "other (the last 60 s or since play).");
    Param width = percent("Width", 0)
                      .meter()
                      .in("Stereo")
                      .help(
                          "The side's share of the energy: 0 mono, 50 unrelated, 100 "
                          "opposed.");
    Param lowWidth = percent("Low Width", 0)
                         .meter()
                         .in("Stereo")
                         .help("The width below 250 Hz, where most mixes keep it near 0.");
    Param balance = decibels("Balance", -30, 30, 0)
                        .meter()
                        .in("Stereo")
                        .help("The right channel's level against the left's, in dB.");
    Param monoLoss = decibels("Mono Loss", -10, 40, 0)
                         .meter()
                         .in("Stereo")
                         .help(
                             "How much quieter a mono fold-down is, in LU: 0 for mono, "
                             "about 3 for unrelated channels.");

    // --- tone -----------------------------------------------------------------------------

    Param centroid = frequency("Centroid", 20, 20000, 1000)
                         .meter()
                         .in("Tone")
                         .help(
                             "The spectrum's centre of mass, the median over the play: "
                             "higher sounds brighter.");
    Param tilt = decibels("Tilt", -24, 24, 0)
                     .meter()
                     .in("Tone")
                     .help(
                         "The spectrum's slope from 100 Hz to 10 kHz, in dB per octave: "
                         "pink noise reads 0, most mixes -1 to -3.");
    Param sub =
        decibels("Sub", -80, 0, -80).meter().in("Tone").help("20 to 60 Hz, in dB of the whole.");
    Param low =
        decibels("Low", -80, 0, -80).meter().in("Tone").help("60 to 250 Hz, in dB of the whole.");
    Param lowMid = decibels("Low Mid", -80, 0, -80)
                       .meter()
                       .in("Tone")
                       .help("250 to 500 Hz, in dB of the whole.");
    Param mid = decibels("Mid", -80, 0, -80)
                    .meter()
                    .in("Tone")
                    .help("500 Hz to 2 kHz, in dB of the whole.");
    Param highMid = decibels("High Mid", -80, 0, -80)
                        .meter()
                        .in("Tone")
                        .help("2 to 4 kHz, in dB of the whole.");
    Param presence = decibels("Presence", -80, 0, -80)
                         .meter()
                         .in("Tone")
                         .help("4 to 6 kHz, in dB of the whole.");
    Param air =
        decibels("Air", -80, 0, -80).meter().in("Tone").help("6 to 20 kHz, in dB of the whole.");

    // --- harmony and coverage ---------------------------------------------------------------

    Param key =
        choice("Key",
               {"None",     "C Major", "Db Major", "D Major", "Eb Major", "E Major",  "F Major",
                "F# Major", "G Major", "Ab Major", "A Major", "Bb Major", "B Major",  "C Minor",
                "C# Minor", "D Minor", "Eb Minor", "E Minor", "F Minor",  "F# Minor", "G Minor",
                "G# Minor", "A Minor", "Bb Minor", "B Minor"},
               0)
            .keys({"none",    "c_major",       "d_flat_major",  "d_major",      "e_flat_major",
                   "e_major", "f_major",       "f_sharp_major", "g_major",      "a_flat_major",
                   "a_major", "b_flat_major",  "b_major",       "c_minor",      "c_sharp_minor",
                   "d_minor", "e_flat_minor",  "e_minor",       "f_minor",      "f_sharp_minor",
                   "g_minor", "g_sharp_minor", "a_minor",       "b_flat_minor", "b_minor"})
            .meter()
            .in("Harmony")
            .help(
                "The key the pitch classes heard most resemble, or none when no key "
                "stands out: a guess, not a transcription.");
    Param keyStrength = percent("Key Strength", 0)
                            .meter()
                            .in("Harmony")
                            .help(
                                "How closely the pitch classes match that key: above 70 is "
                                "clear, below 50 doubtful.");
    Param measured =
        milliseconds("Measured", 0, 36000000, 0)
            .meter()
            .help(
                "How much audio the summarising readings cover: 60 s at most unless accumulating.");

    void factoryPatches(Patches& p) {
        p.add("Streaming").set(target, -14);
        p.add("Streaming, Quieter").set(target, -16);
        p.add("Broadcast, EBU R128").set(target, -23);
        p.add("Broadcast, ATSC A/85").set(target, -24);
        p.add("Loud Master").set(target, -9);
        p.add("Level Spectrum").set(target, -14).set(slope, "flat");
    }

    // --- audio ------------------------------------------------------------------------------

    Array<Array<double, 4>, 32> fir;  // tap, then phase: the three weights are adjacent
    Array<TruePeak, 2> detect;
    Array<KWeighting, 2> kw, playKw;
    Array<Stft, 2> stft;
    Stft brief;  // the mid in short frames, for the centroid
    double rate = 48000.0;
    int bins = 8193;
    double binHz = 2.9296875;
    // Per frame: each bin's mid and side power, and running sums of the mid,
    // side, left, right and cross powers (cum[k] sums bins below k).
    Buffer<double> pM, pS, cM, cS, cL, cR, cX;
    Buffer<int> pcOf;                       // each bin's pitch class, -1 outside 80 Hz to 2.5 kHz
    Buffer<double> pcW;                     // its weight: 1 at a semitone's centre, 0 halfway
    Array<double, kPoints> gridLo, gridHi;  // each drawn point's band, in bins
    Array<double, kThirds> thirdLo, thirdHi;
    Array<double, 8> namedEdge;  // the named bands' edges, in bins
    double kFrameNow = 0.0, kFrameBand = 0.0, kFrameChroma = 0.0;
    double kNeedle = 0.0, fall = 0.999;

    // The state of the play: measuring, or holding the last play.
    bool wasPlaying = false;
    bool holding = false;
    bool stereoNow = true;
    bool unplayed = true;
    bool requireCleanWindows = false;
    bool accumulating = false;
    double sinceRestart = 0.0;  // samples measured since the readings restarted

    // The window: the last minute in seconds, the newest filling, and the
    // loudness blocks' readings, -200 where a block was not yet complete.
    Array<Second, kWindow> seconds;
    int secondAt = 0, secondLength = 48000;
    Array<double, kWindowBlocks> winMomentary, winEnergy, winMono, winMonoEnergy;
    Array<double, kWindowBlocks> winShort, winShortEnergy;
    int winAt = 0, winFilled = 0;

    // Loudness: hundred-millisecond energies (stereo, and the mono fold-down).
    Array<double, 30> blockE, blockMono;
    Array<double, 30> playE, playMono;
    int blockAt = 0, blocksFilled = 0, blockLength = 4800, blockCount = 0;
    double sumE = 0.0, sumMono = 0.0, blockPeak = 0.0;
    double sumPlayE = 0.0, sumPlayMono = 0.0;
    Array<double, kBins> gatedEnergy, monoEnergy;
    Array<int, kBins> gatedCount, monoCount, rangeCount;
    double gatedTotal = 0.0, monoTotal = 0.0, rangeTotal = 0.0;
    int gatedBlocks = 0, monoBlocks = 0, rangeBlocks = 0;
    double momentaryNow = -120.0, shortNow = -120.0, integratedNow = -120.0;
    double monoIntegrated = -120.0, rangeNow = 0.0, rangeLow = -120.0, rangeHigh = -120.0;
    double momentaryTop = -120.0, shortTop = -120.0;
    // Peaks, problems and the stereo sums: the window's, or since play.
    double peakTrue = 0.0, peakSample = 0.0, peakReference = 0.0;
    int overCount = 0, clipCount = 0;
    bool inOver = false;
    Array<int, 2> flatRun;
    double frames = 0.0;  // samples measured
    double sumL = 0.0, sumR = 0.0, sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    // The spectrum: now (smoothed over 300 ms) and the sums of the window or play.
    Array<double, kPoints> nowM, nowS, allM;
    Array<double, kThirds> thirdNowL, thirdNowR, thirdNowX, thirdAllL, thirdAllR, thirdAllX;
    Array<double, kThirds> thirdAllM;
    Array<double, kNamed> namedM;
    double totalM = 0.0, lowM = 0.0, lowS = 0.0;
    int framesAll = 0;
    Array<int, kCentroids> centroidCount;
    int centroidFrames = 0;
    double centroidNow = 1000.0, tiltNow = 0.0, tiltAt = -120.0;
    Array<double, 12> chromaFrame, chromaNow, chromaAll;
    Array<double, 24> keyScore;
    int keyNow = 0;
    double keyFit = 0.0;
    // What the needle shows: correlation and balance now.
    double needleLL = 0.0, needleRR = 0.0, needleLR = 0.0;
    double peakL = 0.0, peakR = 0.0;
    int scopeAt = 0, historyAt = 0, historyFilled = 0;

    // What the editor sees.
    SharedArray<float, kPoints> specNowM, specNowS, specAllM;
    SharedArray<float, kThirds> corrNowBand, corrAllBand;
    SharedArray<float, 12> chromaShown, chromaNowShown;
    SharedArray<float, 24> keyShown;
    SharedArray<float, kHistory> histShort, histMomentary, histPeak;
    SharedArray<float, kSpread> spreadShown;
    SharedArray<float, kScope> scopeL, scopeR;
    Shared<int> scopeHead, historyHead, historyCount, stateShown;
    Shared<double> levelLeft, levelRight, corrNeedle, balanceNeedle;
    Shared<double> lraLowShown, lraHighShown, centroidShown, tiltShown, tiltAtShown;
    Shared<bool> specValid, specAllValid, toneValid;

    void prepare() {
        rate = sampleRate();
        // The true-peak interpolators: a sinc under a Kaiser window, one per
        // phase, each summing to 1.
        const double beta = 8.0;
        for (int k = 0; k < kPhases; ++k) {
            double sum = 0.0;
            for (int j = 0; j < kTaps; ++j) {
                const double u = j - kHalf + k / (1.0 * kPhases);
                const double s = u == 0.0 ? 1.0 : sin(pi * u) / (pi * u);
                const double r = 1.0 - (u / kHalf) * (u / kHalf);
                const double w = r > 0.0 ? besselI0(beta * sqrt(r)) / besselI0(beta) : 0.0;
                fir[j][k] = s * w;
                sum += s * w;
            }
            for (int j = 0; j < kTaps; ++j) fir[j][k] /= sum;
        }
        // The transform: about a third of a second (16384 at 44.1 and 48 kHz),
        // a new frame every quarter of it.
        int size = 16;
        while (size < 32768 && size * 1.5 < 0.34 * rate) size *= 2;
        for (int c = 0; c < 2; ++c) {
            stft[c].prepareAnalysis(size, 4);
            kw[c].design(rate);
            kw[c].clear();
            detect[c].clear();
        }
        // The centroid's frames are the offline analysis's: about 43 ms
        // (2048 at 48 kHz), a quarter of one apart.
        int shortSize = 16;
        while (shortSize < 32768 && shortSize * 1.5 < 0.0427 * rate) shortSize *= 2;
        brief.prepareAnalysis(shortSize, 4);
        bins = stft[0].bins();
        binHz = rate / stft[0].size();
        pM.resize(bins);
        pS.resize(bins);
        cM.resize(bins + 1);
        cS.resize(bins + 1);
        cL.resize(bins + 1);
        cR.resize(bins + 1);
        cX.resize(bins + 1);
        pcOf.resize(bins);
        pcW.resize(bins);
        pM.fill(0.0);
        pS.fill(0.0);
        cM.fill(0.0);
        cS.fill(0.0);
        cL.fill(0.0);
        cR.fill(0.0);
        cX.fill(0.0);
        for (int k = 0; k < bins; ++k) {
            const double hz = k * binHz;
            pcOf[k] = -1;
            pcW[k] = 0.0;
            if (hz < 80.0 || hz > 2500.0) continue;
            const double note = 69.0 + 12.0 * log2(hz / 440.0);
            const int nearest = roundToInt(note);
            pcOf[k] = imod(nearest + 1200, 12);
            pcW[k] = max(0.0, 1.0 - 2.0 * abs(note - nearest));
        }
        // Bands in bins, each bin the interval around its centre.
        const double spread = pow(2.0, 0.5 / kSmooth);
        for (int i = 0; i < kPoints; ++i) {
            const double hz = pointHz(i);
            gridLo[i] = hz / spread / binHz + 0.5;
            gridHi[i] = hz * spread / binHz + 0.5;
        }
        const double sixth = pow(2.0, 1.0 / 6.0);
        for (int b = 0; b < kThirds; ++b) {
            thirdLo[b] = thirdHz(b) / sixth / binHz + 0.5;
            thirdHi[b] = thirdHz(b) * sixth / binHz + 0.5;
        }
        namedEdge[0] = 20.0;
        namedEdge[1] = 60.0;
        namedEdge[2] = 250.0;
        namedEdge[3] = 500.0;
        namedEdge[4] = 2000.0;
        namedEdge[5] = 4000.0;
        namedEdge[6] = 6000.0;
        namedEdge[7] = 20000.0;
        for (int e = 0; e < 8; ++e) namedEdge[e] = min(namedEdge[e], 0.5 * rate) / binHz + 0.5;
        const double hop = stft[0].hop();
        kFrameNow = 1.0 - exp(-hop / (0.3 * rate));
        kFrameBand = 1.0 - exp(-hop / (1.0 * rate));
        kFrameChroma = 1.0 - exp(-hop / (2.0 * rate));
        kNeedle = 1.0 - exp(-1.0 / (0.3 * rate));
        fall = exp(-1.0 / (0.3 * rate));
        blockLength = max(1, roundToInt(0.1 * rate));
        secondLength = max(1, roundToInt(rate));
        // A restart prepares an instance that has played: start it afresh.
        wasPlaying = false;
        holding = false;
        unplayed = true;
        accumulating = accumulate.on();
        restartAll();
        blockE.fill(0.0);
        blockMono.fill(0.0);
        blockAt = 0;
        blocksFilled = 0;
        blockCount = 0;
        sumE = 0.0;
        sumMono = 0.0;
        blockPeak = 0.0;
        momentaryNow = -120.0;
        shortNow = -120.0;
        nowM.fill(0.0);
        nowS.fill(0.0);
        thirdNowL.fill(0.0);
        thirdNowR.fill(0.0);
        thirdNowX.fill(0.0);
        chromaNow.fill(0.0);
        chromaFrame.fill(0.0);
        needleLL = 0.0;
        needleRR = 0.0;
        needleLR = 0.0;
        peakL = 0.0;
        peakR = 0.0;
        scopeAt = 0;
        historyAt = 0;
        historyFilled = 0;
        histShort.fill(-120.0f);
        histMomentary.fill(-120.0f);
        histPeak.fill(-120.0f);
        scopeL.fill(0.0f);
        scopeR.fill(0.0f);
        specValid.set(false);
    }

    double pointHz(int i) const { return 20.0 * pow(1000.0, i / (kPoints - 1.0)); }

    // Every reading since play: on a start of play, a reset, a prepare.
    void restartAll() {
        // Initial zero padding is clean. After a restart the rolling filters
        // and transforms stay live, but their old windows must not enter the
        // new play's accumulated readings.
        requireCleanWindows = !unplayed;
        // A clean window alone cannot remove a prior play's IIR filter tail.
        // Keep the rolling filters for "now" and start this play's weighting
        // from silence. The extra path is needed only after a restart.
        for (int c = 0; c < 2; ++c) {
            playKw[c] = kw[c];
            playKw[c].clear();
        }
        playE.fill(0.0);
        playMono.fill(0.0);
        sumPlayE = 0.0;
        sumPlayMono = 0.0;
        sinceRestart = 0.0;
        for (auto& second : seconds) second.clear();
        secondAt = 0;
        winAt = 0;
        winFilled = 0;
        gatedEnergy.fill(0.0);
        monoEnergy.fill(0.0);
        gatedCount.fill(0);
        monoCount.fill(0);
        rangeCount.fill(0);
        gatedTotal = 0.0;
        monoTotal = 0.0;
        rangeTotal = 0.0;
        gatedBlocks = 0;
        monoBlocks = 0;
        rangeBlocks = 0;
        integratedNow = -120.0;
        monoIntegrated = -120.0;
        rangeNow = 0.0;
        rangeLow = -120.0;
        rangeHigh = -120.0;
        momentaryTop = -120.0;
        shortTop = -120.0;
        peakTrue = 0.0;
        peakSample = 0.0;
        peakReference = 0.0;
        overCount = 0;
        clipCount = 0;
        inOver = false;
        flatRun.fill(0);
        frames = 0.0;
        sumL = 0.0;
        sumR = 0.0;
        sumLL = 0.0;
        sumRR = 0.0;
        sumLR = 0.0;
        allM.fill(0.0);
        thirdAllL.fill(0.0);
        thirdAllR.fill(0.0);
        thirdAllX.fill(0.0);
        thirdAllM.fill(0.0);
        namedM.fill(0.0);
        totalM = 0.0;
        lowM = 0.0;
        lowS = 0.0;
        framesAll = 0;
        centroidCount.fill(0);
        centroidFrames = 0;
        centroidNow = 1000.0;
        tiltNow = 0.0;
        tiltAt = -120.0;
        chromaAll.fill(0.0);
        keyScore.fill(0.0);
        keyNow = 0;
        keyFit = 0.0;
        spreadShown.fill(0.0f);
        specAllValid.set(false);
        toneValid.set(false);
        publish();
    }

    void update() {
        // A reset, or the clock switched, starts every summarising reading afresh.
        if (reset.changed() || accumulate.on() != accumulating) {
            accumulating = accumulate.on();
            holding = false;
            restartAll();
        }
    }

    void process(Block& io) {
        const bool playing = io.transport.playing;
        if (playing && !wasPlaying) {
            if (restartOnPlay.on()) restartAll();
            holding = false;
        }
        if (!playing && wasPlaying) holding = true;
        wasPlaying = playing;
        stereoNow = io.channels > 1;
        const bool measuring = !holding;
        if (io.frames > 0) unplayed = false;
        // The audio code reads the block and never writes it: what comes in
        // goes out as it came.
        for (int i = 0; i < io.frames; ++i) {
            double l = io[0][i];
            double r = stereoNow ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;

            // Keep the interpolator's existing gate (and rolling peak history)
            // independent of which windows the since-play reading may count.
            const double gate = 0.5 * min(peakReference, 1.0);
            const double tl = detect[0].push(l, fir, gate);
            const double tr = stereoNow ? detect[1].push(r, fir, gate) : tl;
            const double tp = max(tl, tr);
            const double al = abs(l), ar = abs(r);
            blockPeak = max(blockPeak, tp);
            if (measuring) {
                Second& now = seconds[secondAt];
                peakReference = max(peakReference, tp);
                // The interpolator uses 32 inputs and two preceding peak
                // points. Wait until those also belong to the new play.
                if (!requireCleanWindows || sinceRestart >= kTaps + 1.0) {
                    peakTrue = max(peakTrue, tp);
                    now.peakTrue = max(now.peakTrue, tp);
                    if (tp > 1.0) {
                        if (!inOver) {
                            overCount += 1;
                            now.overs += 1;
                        }
                        inOver = true;
                    } else {
                        inOver = false;
                    }
                }
                const double sample = max(al, ar);
                peakSample = max(peakSample, sample);
                now.peakSample = max(now.peakSample, sample);
                for (int c = 0; c < 2; ++c) {
                    if (c == 1 && !stereoNow) continue;
                    const double a = c == 0 ? al : ar;
                    if (a >= 0.999) {
                        flatRun[c] += 1;
                        if (flatRun[c] == 3) {
                            clipCount += 1;
                            now.clips += 1;
                        }
                    } else {
                        flatRun[c] = 0;
                    }
                }
                frames += 1.0;
                sumL += l;
                sumR += r;
                sumLL += l * l;
                sumRR += r * r;
                sumLR += l * r;
                now.frames += 1.0;
                now.sumL += l;
                now.sumR += r;
                now.sumLL += l * l;
                now.sumRR += r * r;
                now.sumLR += l * r;
                sinceRestart += 1.0;
            }

            // Loudness: the K-weighted energy, and the fold-down's (the mono
            // mix fed to both channels).
            const double kl = kw[0].process(l);
            if (stereoNow) {
                const double kr = kw[1].process(r);
                sumE += kl * kl + kr * kr;
                sumMono += 0.5 * (kl + kr) * (kl + kr);
            } else {
                sumE += kl * kl;
                sumMono += kl * kl;
            }
            if (requireCleanWindows) {
                const double pl = playKw[0].process(l);
                if (stereoNow) {
                    const double pr = playKw[1].process(r);
                    sumPlayE += pl * pl + pr * pr;
                    sumPlayMono += 0.5 * (pl + pr) * (pl + pr);
                } else {
                    sumPlayE += pl * pl;
                    sumPlayMono += pl * pl;
                }
            }
            blockCount += 1;
            if (blockCount >= blockLength) closeBlock(measuring);

            // What the editor shows.
            needleLL += kNeedle * (l * l - needleLL);
            needleRR += kNeedle * (r * r - needleRR);
            needleLR += kNeedle * (l * r - needleLR);
            peakL = max(al, peakL * fall);
            peakR = max(ar, peakR * fall);
            scopeL[scopeAt] = static_cast<float>(l);
            scopeR[scopeAt] = static_cast<float>(r);
            scopeAt = scopeAt + 1 == kScope ? 0 : scopeAt + 1;

            // The transforms, for analysis only: both channels push, and a
            // frame is read when one is ready.
            const bool ready = stft[0].push(l);
            if (stereoNow) stft[1].push(r);
            if (ready) frame(measuring);
            if (brief.push(0.5 * (l + r)) && measuring &&
                (!requireCleanWindows || sinceRestart >= brief.size()))
                centroidFrame();
            if (measuring && seconds[secondAt].frames >= secondLength) nextSecond();
        }
        scopeHead.set(scopeAt);
        levelLeft.set(gainToDb(peakL));
        levelRight.set(gainToDb(stereoNow ? peakR : peakL));
        const double nd = sqrt(needleLL * needleRR);
        corrNeedle.set(nd > 1e-12 ? clamp(needleLR / nd, -1.0, 1.0) : 1.0);
        balanceNeedle.set(needleLL > 1e-12 && needleRR > 1e-12
                              ? clamp(10.0 * log10(needleRR / needleLL), -30.0, 30.0)
                              : 0.0);
        stateShown.set(holding ? 2 : (playing ? 1 : 0));
    }

    // --- loudness ---------------------------------------------------------------------------

    // The highest bin's level whose count passes the gate, for the gated mean.
    double gatedMean(const Array<double, kBins>& energy, const Array<int, kBins>& count,
                     double total, int blocks) const {
        if (blocks == 0) return -120.0;
        const double gate = lufs(total / blocks) - 10.0;
        double e = 0.0;
        int n = 0;
        for (int b = 0; b < kBins; ++b) {
            if (count[b] == 0 || -70.0 + (b + 0.5) * 0.1 <= gate) continue;
            e += energy[b];
            n += count[b];
        }
        return n > 0 ? lufs(e / n) : -120.0;
    }

    // A hundred milliseconds: the momentary (400 ms) block enters the gates
    // of the integrated loudness and the fold-down's, the short-term (3 s)
    // the range's.
    void closeBlock(bool measuring) {
        blockE[blockAt] = sumE / blockCount;
        blockMono[blockAt] = sumMono / blockCount;
        if (requireCleanWindows) {
            playE[blockAt] = sumPlayE / blockCount;
            playMono[blockAt] = sumPlayMono / blockCount;
        }
        blockAt = blockAt == 29 ? 0 : blockAt + 1;
        blocksFilled = min(blocksFilled + 1, 30);
        sumE = 0.0;
        sumMono = 0.0;
        sumPlayE = 0.0;
        sumPlayMono = 0.0;
        blockCount = 0;
        double m = 0.0, mm = 0.0, s = 0.0;
        for (int k = 0; k < blocksFilled; ++k) {
            const int b = wrap(blockAt - 1 - k, 30);
            if (k < 4) {
                m += blockE[b];
                mm += blockMono[b];
            }
            s += blockE[b];
        }
        momentaryNow = lufs(m / min(4, blocksFilled));
        shortNow = lufs(s / blocksFilled);
        double pm = m, pmm = mm, ps = s;
        if (requireCleanWindows) {
            pm = 0.0;
            pmm = 0.0;
            ps = 0.0;
            for (int k = 0; k < blocksFilled; ++k) {
                const int b = wrap(blockAt - 1 - k, 30);
                if (k < 4) {
                    pm += playE[b];
                    pmm += playMono[b];
                }
                ps += playE[b];
            }
        }
        const double playMomentary =
            requireCleanWindows ? lufs(pm / min(4, blocksFilled)) : momentaryNow;
        const double playShort = requireCleanWindows ? lufs(ps / blocksFilled) : shortNow;
        const double monoMomentary = lufs(pmm / min(4, blocksFilled));
        const bool momentaryReady =
            measuring && blocksFilled >= 4 && sinceRestart >= 4.0 * blockLength;
        const bool shortReady =
            measuring && blocksFilled >= 30 && sinceRestart >= 30.0 * blockLength;
        if (measuring) {
            winMomentary[winAt] = momentaryReady ? playMomentary : -200.0;
            winEnergy[winAt] = pm / 4.0;
            winMono[winAt] = monoMomentary;
            winMonoEnergy[winAt] = pmm / 4.0;
            winShort[winAt] = shortReady ? playShort : -200.0;
            winShortEnergy[winAt] = ps / 30.0;
            winAt = winAt == kWindowBlocks - 1 ? 0 : winAt + 1;
            winFilled = min(winFilled + 1, kWindowBlocks);
        }
        if (accumulating) {
            if (momentaryReady) countMomentary(playMomentary, pm / 4.0, monoMomentary, pmm / 4.0);
            if (shortReady) countShort(playShort, ps / 30.0);
            loudnessReadings();
        } else if (measuring) {
            windowLoudness();
        }
        histShort[historyAt] = static_cast<float>(blocksFilled >= 30 ? shortNow : -120.0);
        histMomentary[historyAt] = static_cast<float>(blocksFilled >= 4 ? momentaryNow : -120.0);
        histPeak[historyAt] = static_cast<float>(gainToDb(blockPeak));
        blockPeak = 0.0;
        historyAt = historyAt == kHistory - 1 ? 0 : historyAt + 1;
        historyFilled = min(historyFilled + 1, kHistory);
        historyHead.set(historyAt);
        historyCount.set(historyFilled);
        publish();
    }

    // A complete momentary block (400 ms) into the gates of the integrated
    // loudness and the fold-down's.
    void countMomentary(double level, double energy, double monoLevel, double monoE) {
        momentaryTop = max(momentaryTop, level);
        if (level > -70.0) {
            const int bin = clamp(floorToInt((level + 70.0) * 10.0), 0, kBins - 1);
            gatedEnergy[bin] += energy;
            gatedCount[bin] += 1;
            gatedTotal += energy;
            gatedBlocks += 1;
        }
        if (monoLevel > -70.0) {
            const int bin = clamp(floorToInt((monoLevel + 70.0) * 10.0), 0, kBins - 1);
            monoEnergy[bin] += monoE;
            monoCount[bin] += 1;
            monoTotal += monoE;
            monoBlocks += 1;
        }
    }

    // A complete short-term block (3 s) into the range's.
    void countShort(double level, double energy) {
        shortTop = max(shortTop, level);
        if (level > -70.0) {
            const int bin = clamp(floorToInt((level + 70.0) * 10.0), 0, kBins - 1);
            rangeCount[bin] += 1;
            rangeTotal += energy;
            rangeBlocks += 1;
            const int lu = floorToInt(level + 60.0);
            if (lu >= 0 && lu < kSpread) spreadShown[lu] += 1.0f;
        }
    }

    void loudnessReadings() {
        integratedNow = gatedMean(gatedEnergy, gatedCount, gatedTotal, gatedBlocks);
        monoIntegrated = gatedMean(monoEnergy, monoCount, monoTotal, monoBlocks);
        if (rangeBlocks > 0) loudnessRange();
    }

    // The window's loudness: the gates rebuilt from the last minute's blocks,
    // oldest first.
    void windowLoudness() {
        gatedEnergy.fill(0.0);
        monoEnergy.fill(0.0);
        gatedCount.fill(0);
        monoCount.fill(0);
        rangeCount.fill(0);
        gatedTotal = 0.0;
        monoTotal = 0.0;
        rangeTotal = 0.0;
        gatedBlocks = 0;
        monoBlocks = 0;
        rangeBlocks = 0;
        momentaryTop = -120.0;
        shortTop = -120.0;
        rangeNow = 0.0;
        rangeLow = -120.0;
        rangeHigh = -120.0;
        spreadShown.fill(0.0f);
        for (int k = winFilled; k > 0; --k) {
            const int b = wrap(winAt - k, kWindowBlocks);
            if (winMomentary[b] > -199.0)
                countMomentary(winMomentary[b], winEnergy[b], winMono[b], winMonoEnergy[b]);
            if (winShort[b] > -199.0) countShort(winShort[b], winShortEnergy[b]);
        }
        loudnessReadings();
    }

    // A second of the window has passed: the oldest gives way to a new one
    // and, unless accumulating, the sums become the window's again.
    void nextSecond() {
        secondAt = secondAt == kWindow - 1 ? 0 : secondAt + 1;
        seconds[secondAt].clear();
        if (accumulating) return;
        frames = 0.0;
        sumL = 0.0;
        sumR = 0.0;
        sumLL = 0.0;
        sumRR = 0.0;
        sumLR = 0.0;
        peakTrue = 0.0;
        peakSample = 0.0;
        overCount = 0;
        clipCount = 0;
        allM.fill(0.0);
        thirdAllL.fill(0.0);
        thirdAllR.fill(0.0);
        thirdAllX.fill(0.0);
        thirdAllM.fill(0.0);
        namedM.fill(0.0);
        chromaAll.fill(0.0);
        centroidCount.fill(0);
        totalM = 0.0;
        lowM = 0.0;
        lowS = 0.0;
        framesAll = 0;
        centroidFrames = 0;
        for (const auto& second : seconds) {
            frames += second.frames;
            sumL += second.sumL;
            sumR += second.sumR;
            sumLL += second.sumLL;
            sumRR += second.sumRR;
            sumLR += second.sumLR;
            peakTrue = max(peakTrue, second.peakTrue);
            peakSample = max(peakSample, second.peakSample);
            overCount += second.overs;
            clipCount += second.clips;
            for (int i = 0; i < kPoints; ++i) allM[i] += second.allM[i];
            for (int b = 0; b < kThirds; ++b) {
                thirdAllL[b] += second.thirdL[b];
                thirdAllR[b] += second.thirdR[b];
                thirdAllX[b] += second.thirdX[b];
                thirdAllM[b] += second.thirdM[b];
            }
            for (int n = 0; n < kNamed; ++n) namedM[n] += second.namedM[n];
            for (int c = 0; c < 12; ++c) chromaAll[c] += second.chroma[c];
            for (int b = 0; b < kCentroids; ++b) centroidCount[b] += second.centroids[b];
            totalM += second.totalM;
            lowM += second.lowM;
            lowS += second.lowS;
            framesAll += second.framesAll;
            centroidFrames += second.centroidFrames;
        }
        // A peak that left the window must no longer suppress interpolation
        // of the quieter peaks still in it. Accumulation keeps its reference.
        peakReference = peakTrue;
        findKey();
        tone();
        share();
    }

    // EBU 3342: short-term values gated at -70 LUFS and 20 LU under their
    // mean, from the 10th to the 95th percentile.
    void loudnessRange() {
        const double gate = lufs(rangeTotal / rangeBlocks) - 20.0;
        int n = 0;
        for (int b = 0; b < kBins; ++b)
            if (-70.0 + (b + 0.5) * 0.1 > gate) n += rangeCount[b];
        if (n < 2) return;
        int seen = 0;
        double lo = 0.0, hi = 0.0;
        bool loFound = false;
        for (int b = 0; b < kBins; ++b) {
            if (-70.0 + (b + 0.5) * 0.1 <= gate) continue;
            seen += rangeCount[b];
            if (!loFound && seen >= 0.10 * n) {
                lo = -70.0 + (b + 0.5) * 0.1;
                loFound = true;
            }
            if (seen >= 0.95 * n) {
                hi = -70.0 + (b + 0.5) * 0.1;
                break;
            }
        }
        rangeNow = max(0.0, hi - lo);
        rangeLow = lo;
        rangeHigh = hi;
    }

    // --- the spectrum -----------------------------------------------------------------------

    // The summed power of a run of bins below a fractional bin position.
    double below(const Buffer<double>& cum, const Buffer<double>& per, double x) const {
        const double c = clamp(x, 0.0, bins * 1.0);
        const int k = floorToInt(c);
        return cum[k] + (c - k) * per[min(k, bins - 1)];
    }
    double between(const Buffer<double>& cum, const Buffer<double>& per, double lo,
                   double hi) const {
        return max(0.0, below(cum, per, hi) - below(cum, per, lo));
    }
    // The same for the left, right and cross sums, which keep no per-bin copy.
    double sumBelow(const Buffer<double>& cum, double x) const {
        const double c = clamp(x, 0.0, bins * 1.0);
        const int k = floorToInt(c);
        const int k1 = min(k + 1, bins);
        return cum[k] + (c - k) * (cum[k1] - cum[k]);
    }
    double sumBetween(const Buffer<double>& cum, double lo, double hi) const {
        return sumBelow(cum, hi) - sumBelow(cum, lo);
    }

    double binDb(int k) const { return powerDb(pM[clamp(k, 0, bins - 1)]); }
    double sideDb(int k) const { return powerDb(pS[clamp(k, 0, bins - 1)]); }

    // A drawn point's power: the bins' average over a twelfth of an octave
    // where they are dense, a smooth curve through them where they are
    // sparse (the bass). A sine of amplitude 1 reads 1 at its peak.
    double gridPower(int i, bool side) const {
        const double lo = gridLo[i], hi = gridHi[i];
        const double width = hi - lo;
        const double smooth =
            (side ? between(cS, pS, lo, hi) : between(cM, pM, lo, hi)) / max(width, 1e-9);
        // Once the band spans two bins the interpolation has zero weight.
        if (width >= 2.0) return smooth;
        const double at = 0.5 * (lo + hi) - 0.5;
        const int k = floorToInt(at);
        const double t = at - k;
        double y0 = side ? sideDb(k - 1) : binDb(k - 1);
        double y1 = side ? sideDb(k) : binDb(k);
        double y2 = side ? sideDb(k + 1) : binDb(k + 1);
        double y3 = side ? sideDb(k + 2) : binDb(k + 2);
        y0 = max(y0, -240.0);
        y1 = max(y1, -240.0);
        y2 = max(y2, -240.0);
        y3 = max(y3, -240.0);
        const double curve =
            y1 + 0.5 * t *
                     (y2 - y0 +
                      t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0)));
        const double mixAmount = clamp(width - 1.0, 0.0, 1.0);
        return lerp(dbToGain(2.0 * min(curve, 60.0)), smooth, mixAmount);
    }

    void frame(bool measuring) {
        // Each bin's powers: left, right, their cross product, the mid and side.
        double total = 0.0;
        chromaFrame.fill(0.0);
        cM[0] = 0.0;
        cS[0] = 0.0;
        cL[0] = 0.0;
        cR[0] = 0.0;
        cX[0] = 0.0;
        for (int k = 0; k < bins; ++k) {
            const double lr = stft[0].real(k), li = stft[0].imag(k);
            double rr = lr, ri = li;
            if (stereoNow) {
                rr = stft[1].real(k);
                ri = stft[1].imag(k);
            }
            const double pl = lr * lr + li * li, pr = rr * rr + ri * ri;
            const double x = lr * rr + li * ri;
            const double m = max(0.0, 0.25 * (pl + pr + 2.0 * x));
            const double s = max(0.0, 0.25 * (pl + pr - 2.0 * x));
            pM[k] = m;
            pS[k] = s;
            cM[k + 1] = cM[k] + m;
            cS[k + 1] = cS[k] + s;
            cL[k + 1] = cL[k] + pl;
            cR[k + 1] = cR[k] + pr;
            cX[k + 1] = cX[k] + x;
            total += 0.5 * (pl + pr);
            if (pcOf[k] >= 0) chromaFrame[pcOf[k]] += pcW[k] * sqrt(m);
        }
        // A sine of amplitude a sums to 1.5 a^2 over its bins: a frame's mean
        // square is the total over three.
        const bool silent = total / 3.0 < 1e-7;
        const bool counted =
            measuring && !silent && (!requireCleanWindows || sinceRestart >= stft[0].size());
        Second& now = seconds[secondAt];

        // The drawn spectrum, now and since play.
        for (int i = 0; i < kPoints; ++i) {
            const double m = gridPower(i, false), s = gridPower(i, true);
            nowM[i] += kFrameNow * (m - nowM[i]);
            nowS[i] += kFrameNow * (s - nowS[i]);
            if (counted) {
                allM[i] += m;
                now.allM[i] += m;
            }
        }
        // Third octaves: the correlation by band, now and since play; the mid
        // for the tilt.
        for (int b = 0; b < kThirds; ++b) {
            const double lo = thirdLo[b], hi = thirdHi[b];
            const double bl = sumBetween(cL, lo, hi), br = sumBetween(cR, lo, hi);
            const double bx = sumBetween(cX, lo, hi);
            thirdNowL[b] += kFrameBand * (bl - thirdNowL[b]);
            thirdNowR[b] += kFrameBand * (br - thirdNowR[b]);
            thirdNowX[b] += kFrameBand * (bx - thirdNowX[b]);
            if (counted) {
                const double bm = between(cM, pM, lo, hi);
                thirdAllL[b] += bl;
                thirdAllR[b] += br;
                thirdAllX[b] += bx;
                thirdAllM[b] += bm;
                now.thirdL[b] += bl;
                now.thirdR[b] += br;
                now.thirdX[b] += bx;
                now.thirdM[b] += bm;
            }
        }
        for (int c = 0; c < 12; ++c) chromaNow[c] += kFrameChroma * (chromaFrame[c] - chromaNow[c]);
        if (counted) {
            framesAll += 1;
            now.framesAll += 1;
            for (int n = 0; n < kNamed; ++n) {
                const double e = between(cM, pM, namedEdge[n], namedEdge[n + 1]);
                namedM[n] += e;
                now.namedM[n] += e;
            }
            const double t = between(cM, pM, namedEdge[0], namedEdge[7]);
            const double lm = between(cM, pM, namedEdge[0], namedEdge[2]);
            const double ls = between(cS, pS, namedEdge[0], namedEdge[2]);
            totalM += t;
            lowM += lm;
            lowS += ls;
            now.totalM += t;
            now.lowM += lm;
            now.lowS += ls;
            // Each frame's pitch classes count alike, however loud.
            double chromaSum = 0.0;
            for (int c = 0; c < 12; ++c) chromaSum += chromaFrame[c];
            if (chromaSum > 0.0) {
                for (int c = 0; c < 12; ++c) {
                    const double part = chromaFrame[c] / chromaSum;
                    chromaAll[c] += part;
                    now.chroma[c] += part;
                }
            }
            findKey();
        }
        tone();
        share();
    }

    // A short frame of the mid: its centroid (the magnitudes' centre, DC
    // left out), into the histogram the median comes from, when it is not
    // silent.
    void centroidFrame() {
        double magSum = 0.0, magHz = 0.0, power = 0.0;
        const int n = brief.bins();
        const double hz0 = rate / brief.size();
        for (int k = 1; k < n; ++k) {
            const double re = brief.real(k), im = brief.imag(k);
            const double p = re * re + im * im;
            const double mag = sqrt(p);
            power += p;
            magSum += mag;
            magHz += mag * k * hz0;
        }
        if (power / 3.0 < 1e-7 || magSum <= 0.0) return;
        const double hz = clamp(magHz / magSum, 20.0, 20000.0);
        const int at =
            clamp(floorToInt(kCentroids * log2(hz / 20.0) / log2(1000.0)), 0, kCentroids - 1);
        centroidCount[at] += 1;
        centroidFrames += 1;
        seconds[secondAt].centroids[at] += 1;
        seconds[secondAt].centroidFrames += 1;
    }

    // The profile of key k (0..11 major from C, 12..23 minor) at pitch class c.
    double profile(int k, int c) const {
        const int d = imod(c - imod(k, 12) + 12, 12);
        if (k < 12) {
            switch (d) {
                case 0:
                    return kMajor0;
                case 1:
                    return kMajor1;
                case 2:
                    return kMajor2;
                case 3:
                    return kMajor3;
                case 4:
                    return kMajor4;
                case 5:
                    return kMajor5;
                case 6:
                    return kMajor6;
                case 7:
                    return kMajor7;
                case 8:
                    return kMajor8;
                case 9:
                    return kMajor9;
                case 10:
                    return kMajor10;
                default:
                    return kMajor11;
            }
        }
        switch (d) {
            case 0:
                return kMinor0;
            case 1:
                return kMinor1;
            case 2:
                return kMinor2;
            case 3:
                return kMinor3;
            case 4:
                return kMinor4;
            case 5:
                return kMinor5;
            case 6:
                return kMinor6;
            case 7:
                return kMinor7;
            case 8:
                return kMinor8;
            case 9:
                return kMinor9;
            case 10:
                return kMinor10;
            default:
                return kMinor11;
        }
    }

    // Each key's correlation with the pitch classes heard; the best one,
    // when it stands out from a flat spread.
    void findKey() {
        double mean = 0.0;
        for (int c = 0; c < 12; ++c) mean += chromaAll[c];
        mean /= 12.0;
        double vc = 0.0;
        for (int c = 0; c < 12; ++c) vc += (chromaAll[c] - mean) * (chromaAll[c] - mean);
        int best = -1;
        double bestR = -2.0;
        for (int k = 0; k < 24; ++k) {
            double pm = 0.0;
            for (int c = 0; c < 12; ++c) pm += profile(k, c);
            pm /= 12.0;
            double cov = 0.0, vp = 0.0;
            for (int c = 0; c < 12; ++c) {
                const double p = profile(k, c) - pm;
                cov += (chromaAll[c] - mean) * p;
                vp += p * p;
            }
            const double r = vc > 1e-12 && vp > 0.0 ? cov / sqrt(vc * vp) : 0.0;
            keyScore[k] = r;
            if (r > bestR) {
                bestR = r;
                best = k;
            }
        }
        keyFit = max(0.0, bestR);
        // Two seconds of sound at least, a clear match, and harmony: five
        // pitch classes or more with a real share (a lone note, with its
        // overtones, sounds three or four).
        const bool enough = framesAll * stft[0].hop() >= 2.0 * rate;
        int heard = 0;
        for (int c = 0; c < 12; ++c)
            if (chromaAll[c] >= 0.05 * 12.0 * (mean > 0.0 ? mean : 1.0)) heard += 1;
        keyNow = enough && heard >= 5 && best >= 0 && bestR >= 0.5 ? best + 1 : 0;
    }

    // The tone readings, from the sums since play.
    void tone() {
        if (framesAll == 0) return;
        // The tilt: the least-squares slope of the third-octave levels from
        // 100 Hz to 10 kHz against octaves.
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        int n = 0;
        for (int b = 7; b <= 27; ++b) {
            if (thirdHz(b) * pow(2.0, 1.0 / 6.0) > 0.5 * rate) continue;
            const double x = (b - 17) / 3.0;
            const double y = max(powerDb(thirdAllM[b] / framesAll / 1.5), -150.0);
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            n += 1;
        }
        if (n >= 2) {
            const double den = n * sxx - sx * sx;
            const double slopeNow = den > 0.0 ? (n * sxy - sx * sy) / den : 0.0;
            tiltNow = slopeNow;
            tiltAt = (sy - slopeNow * sx) / n;
            tiltShown.set(tiltNow);
            tiltAtShown.set(tiltAt);
        }
        // The centroid's median.
        int seen = 0;
        for (int b = 0; b < kCentroids; ++b) {
            seen += centroidCount[b];
            if (2 * seen >= centroidFrames) {
                centroidNow = 20.0 * pow(1000.0, (b + 0.5) / kCentroids);
                break;
            }
        }
        centroidShown.set(centroidNow);
    }

    // The editor's arrays, after a frame.
    void share() {
        for (int i = 0; i < kPoints; ++i) {
            specNowM[i] = static_cast<float>(max(powerDb(nowM[i]), -200.0));
            specNowS[i] = static_cast<float>(max(powerDb(nowS[i]), -200.0));
            specAllM[i] = static_cast<float>(
                framesAll > 0 ? max(powerDb(allM[i] / framesAll), -200.0) : -200.0);
        }
        for (int b = 0; b < kThirds; ++b) {
            const double dn = sqrt(thirdNowL[b] * thirdNowR[b]);
            corrNowBand[b] =
                static_cast<float>(dn > 1e-14 ? clamp(thirdNowX[b] / dn, -1.0, 1.0) : 2.0);
            const double da = sqrt(thirdAllL[b] * thirdAllR[b]);
            corrAllBand[b] = static_cast<float>(
                da > 1e-14 * max(1, framesAll) ? clamp(thirdAllX[b] / da, -1.0, 1.0) : 2.0);
        }
        double ca = 0.0, cn = 0.0;
        for (int c = 0; c < 12; ++c) {
            ca += chromaAll[c];
            cn += chromaNow[c];
        }
        for (int c = 0; c < 12; ++c) {
            chromaShown[c] = static_cast<float>(ca > 0.0 ? chromaAll[c] / ca : 0.0);
            chromaNowShown[c] = static_cast<float>(cn > 0.0 ? chromaNow[c] / cn : 0.0);
        }
        for (int k = 0; k < 24; ++k) keyShown[k] = static_cast<float>(keyScore[k]);
        specValid.set(true);
        specAllValid.set(framesAll > 0);
        toneValid.set(framesAll > 0 && totalM > 1e-12 * framesAll);
    }

    // The meters, every hundred milliseconds and on a restart.
    void publish() {
        integrated.set(max(-70.0, integratedNow));
        shortTerm.set(max(-70.0, shortNow));
        momentary.set(max(-70.0, momentaryNow));
        lra.set(rangeNow);
        shortTermMax.set(max(-70.0, shortTop));
        momentaryMax.set(max(-70.0, momentaryTop));
        const double tpDb = gainToDb(peakTrue);
        truePeak.set(max(-70.0, tpDb));
        samplePeak.set(max(-70.0, gainToDb(peakSample)));
        plr.set(integratedNow > -70.0 ? clamp(tpDb - integratedNow, 0.0, 70.0) : 0.0);
        const double rms = frames > 0.0 ? sqrt((sumLL + sumRR) / (2.0 * frames)) : 0.0;
        crest.set(rms > 1e-9 ? clamp(gainToDb(peakSample / rms), 0.0, 70.0) : 0.0);
        overs.set(overCount);
        clipped.set(clipCount);
        const double dc = frames > 0.0 ? max(abs(sumL), abs(sumR)) / frames : 0.0;
        dcOffset.set(clamp(gainToDb(dc), -120.0, 0.0));
        // Stereo: Pearson's correlation, the side's share, the balance.
        double corr = 1.0, share = 0.0, bal = 0.0;
        if (stereoNow && frames > 1.0) {
            const double vl = sumLL - sumL * sumL / frames, vr = sumRR - sumR * sumR / frames;
            const double cv = sumLR - sumL * sumR / frames;
            if (vl > 1e-12 && vr > 1e-12) corr = clamp(cv / sqrt(vl * vr), -1.0, 1.0);
            const double em = sumLL + sumRR + 2.0 * sumLR, es = sumLL + sumRR - 2.0 * sumLR;
            if (em + es > 1e-12) share = clamp(es / (em + es), 0.0, 1.0);
            if (sumLL > 1e-12 && sumRR > 1e-12)
                bal = clamp(10.0 * log10(sumRR / sumLL), -30.0, 30.0);
        }
        correlation.set(corr);
        width.set(share);
        lowWidth.set(stereoNow && lowM + lowS > 1e-12 ? clamp(lowS / (lowM + lowS), 0.0, 1.0)
                                                      : 0.0);
        balance.set(bal);
        double loss = 0.0;
        if (stereoNow && integratedNow > -70.0) loss = integratedNow - max(monoIntegrated, -70.0);
        monoLoss.set(clamp(loss, -10.0, 40.0));
        // Tone, from the long-term spectrum.
        if (framesAll > 0 && totalM > 0.0) {
            centroid.set(centroidNow);
            tilt.set(clamp(tiltNow, -24.0, 24.0));
            sub.set(relative(0));
            low.set(relative(1));
            lowMid.set(relative(2));
            mid.set(relative(3));
            highMid.set(relative(4));
            presence.set(relative(5));
            air.set(relative(6));
        } else {
            centroid.set(1000.0);
            tilt.set(0.0);
            sub.set(-80.0);
            low.set(-80.0);
            lowMid.set(-80.0);
            mid.set(-80.0);
            highMid.set(-80.0);
            presence.set(-80.0);
            air.set(-80.0);
        }
        key.set(keyNow);
        keyStrength.set(keyFit);
        measured.set(1000.0 * frames / rate);
        lraLowShown.set(rangeLow);
        lraHighShown.set(rangeHigh);
    }

    double relative(int n) const { return clamp(powerDb(namedM[n] / totalM), -80.0, 0.0); }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1080;
    static constexpr double kH = 800;
    // The loudness panel, the history beside it.
    static constexpr double lx = 16, ly = 60, lw = 340, lh = 272;
    static constexpr double hx = 368, hy = 60, hw = 696, hh = 272;
    static constexpr double gx = 384, gy = 94, gw = 600, gh = 208;  // the history graph
    static constexpr double dx = 994, dw = 54;                      // its distribution
    static constexpr double kLoudTop = 0.0, kLoudFloor = -48.0;
    // The spectrum.
    static constexpr double sx = 16, sy = 344, sw = 1048, sh = 236;
    static constexpr double fx = 32, fy = 380, fw = 1016, fh = 148;
    static constexpr double kSpecTop = 0.0, kSpecFloor = -96.0;
    // The bottom row: stereo, harmony, health.
    static constexpr double ox = 16, oy = 592, ow = 452, oh = 192;
    static constexpr double vx = 32, vy = 626, vs = 146;  // the vectorscope
    static constexpr double cx = 196, cw = 256;           // the stereo readings
    static constexpr double ax = 480, ay = 592, aw = 280, ah = 192;
    static constexpr double wx = 560, wy = 702, wr = 56;  // the pitch-class wheel
    static constexpr double ex = 772, ey = 592, ew = 292, eh = 192;
    // Controls.
    static constexpr double restartX = 872, resetX = 1000, pillY = 15, pillH = 20;
    static constexpr double clockY = 33;  // the timer's clock, in the header
    static constexpr double slopeX = 884, slopeY = 355;
    static constexpr double chipX = 88, chipY = 297, chipW = 36, chipH = 18;

    // The editor's own.
    double hoverX = -1.0, hoverY = -1.0;
    double clockX = 0.0, clockW = 0.0;  // where the clock and its text were drawn
    int dragging = 0;                   // 1 the target
    Array<double, kPoints> eNowM, eNowS, eAll, eHold;
    bool primed = false;
    double zoom = 1.0;
    bool zoomed = false;

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
              int colour, bool hot) {
        g.fillStyle(lit ? colour : (hot ? 0x38352A : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, h * 0.5);
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(lit ? kInk : Theme::textMuted);
        g.boldFont(9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x + w * 0.5, y + h * 0.5 + 0.5);
    }

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    void valueStyle(Canvas& g, double size) {
        g.fillStyle(Theme::text);
        g.monoFont(size);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
    }

    void fillHz(Canvas& g, double hz, double x, double y) {
        if (hz < 100.0)
            g.fillText(format("%.1f Hz", hz), x, y);
        else if (hz < 1000.0)
            g.fillText(format("%.0f Hz", hz), x, y);
        else if (hz < 10000.0)
            g.fillText(format("%.2f kHz", hz * 0.001), x, y);
        else
            g.fillText(format("%.1f kHz", hz * 0.001), x, y);
    }

    void fillDb(Canvas& g, double db, double x, double y) {
        if (abs(db) < 0.05)
            g.fillText("0.0 dB", x, y);
        else
            g.fillText(format("%+.1f dB", db), x, y);
    }

    // A loudness: a dash while nothing has been measured.
    void fillLoud(Canvas& g, double v, double x, double y) {
        if (v <= -69.95)
            g.fillText("–", x, y);
        else
            g.fillText(format("%.1f", v), x, y);
    }

    void fillClock(Canvas& g, double ms, double x, double y) {
        const double s = ms * 0.001;
        const int minutes = floorToInt(s / 60.0);
        if (minutes > 0)
            g.fillText(format("%d:%04.1f", minutes, s - 60.0 * minutes), x, y);
        else
            g.fillText(format("%.1f s", s), x, y);
    }

    // A pitch class's name.
    void fillPitch(Canvas& g, int pc, double x, double y) {
        switch (imod(pc + 12, 12)) {
            case 0:
                g.fillText("C", x, y);
                break;
            case 1:
                g.fillText("C#", x, y);
                break;
            case 2:
                g.fillText("D", x, y);
                break;
            case 3:
                g.fillText("Eb", x, y);
                break;
            case 4:
                g.fillText("E", x, y);
                break;
            case 5:
                g.fillText("F", x, y);
                break;
            case 6:
                g.fillText("F#", x, y);
                break;
            case 7:
                g.fillText("G", x, y);
                break;
            case 8:
                g.fillText("Ab", x, y);
                break;
            case 9:
                g.fillText("A", x, y);
                break;
            case 10:
                g.fillText("Bb", x, y);
                break;
            default:
                g.fillText("B", x, y);
                break;
        }
    }

    // A note with its octave and how far off it the frequency is.
    void fillNote(Canvas& g, double hz, double x, double y) {
        const double note = 69.0 + 12.0 * log2(hz / 440.0);
        const int n = roundToInt(note);
        const int cents = roundToInt(100.0 * (note - n));
        const int octave = idiv(n + 1200, 12) - 101;
        switch (imod(n + 1200, 12)) {
            case 0:
                g.fillText(format("C%d %+dc", octave, cents), x, y);
                break;
            case 1:
                g.fillText(format("C#%d %+dc", octave, cents), x, y);
                break;
            case 2:
                g.fillText(format("D%d %+dc", octave, cents), x, y);
                break;
            case 3:
                g.fillText(format("Eb%d %+dc", octave, cents), x, y);
                break;
            case 4:
                g.fillText(format("E%d %+dc", octave, cents), x, y);
                break;
            case 5:
                g.fillText(format("F%d %+dc", octave, cents), x, y);
                break;
            case 6:
                g.fillText(format("F#%d %+dc", octave, cents), x, y);
                break;
            case 7:
                g.fillText(format("G%d %+dc", octave, cents), x, y);
                break;
            case 8:
                g.fillText(format("Ab%d %+dc", octave, cents), x, y);
                break;
            case 9:
                g.fillText(format("A%d %+dc", octave, cents), x, y);
                break;
            case 10:
                g.fillText(format("Bb%d %+dc", octave, cents), x, y);
                break;
            default:
                g.fillText(format("B%d %+dc", octave, cents), x, y);
                break;
        }
    }

    // A key's name: 0..11 major from C, 12..23 minor.
    void fillKey(Canvas& g, int k, double x, double y) {
        switch (k) {
            case 0:
                g.fillText("C major", x, y);
                break;
            case 1:
                g.fillText("Db major", x, y);
                break;
            case 2:
                g.fillText("D major", x, y);
                break;
            case 3:
                g.fillText("Eb major", x, y);
                break;
            case 4:
                g.fillText("E major", x, y);
                break;
            case 5:
                g.fillText("F major", x, y);
                break;
            case 6:
                g.fillText("F# major", x, y);
                break;
            case 7:
                g.fillText("G major", x, y);
                break;
            case 8:
                g.fillText("Ab major", x, y);
                break;
            case 9:
                g.fillText("A major", x, y);
                break;
            case 10:
                g.fillText("Bb major", x, y);
                break;
            case 11:
                g.fillText("B major", x, y);
                break;
            case 12:
                g.fillText("C minor", x, y);
                break;
            case 13:
                g.fillText("C# minor", x, y);
                break;
            case 14:
                g.fillText("D minor", x, y);
                break;
            case 15:
                g.fillText("Eb minor", x, y);
                break;
            case 16:
                g.fillText("E minor", x, y);
                break;
            case 17:
                g.fillText("F minor", x, y);
                break;
            case 18:
                g.fillText("F# minor", x, y);
                break;
            case 19:
                g.fillText("G minor", x, y);
                break;
            case 20:
                g.fillText("G# minor", x, y);
                break;
            case 21:
                g.fillText("A minor", x, y);
                break;
            case 22:
                g.fillText("Bb minor", x, y);
                break;
            default:
                g.fillText("B minor", x, y);
                break;
        }
    }

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 3, TextAlign::Right);
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

    // --- the mark and the header ----------------------------------------------------------------

    // A lichen's rosette: lobes around a darker heart, with the small orange
    // cups it fruits in, brightening as it hears.
    void drawMark(Canvas& g, double x, double y) {
        const double glow = clamp((max(levelLeft.get(), levelRight.get()) + 48.0) / 48.0, 0.0, 1.0);
        for (int k = 0; k < 9; ++k) {
            const double a = twoPi * k / 9.0 + 0.3;
            const double r = 9.5 + 1.5 * imod(k, 2);
            g.fillStyle(blend(kLichenDeep, kLichen, 0.55 + 0.45 * glow));
            g.beginPath();
            g.circle(x + r * cos(a), y + r * sin(a), 5.2 - 0.6 * imod(k, 2));
            g.fill();
        }
        g.fillStyle(kLichenDeep);
        g.beginPath();
        g.circle(x, y, 8);
        g.fill();
        g.fillStyle(blend(kLichenDeep, kLichen, 0.3 + 0.3 * glow));
        g.beginPath();
        g.circle(x, y, 5);
        g.fill();
        for (int k = 0; k < 3; ++k) {
            const double a = twoPi * k / 3.0 + 1.1;
            g.fillStyle(blend(kBark, kRust, 0.4 + 0.6 * glow));
            g.beginPath();
            g.circle(x + 6.5 * cos(a), y + 6.5 * sin(a), 1.9);
            g.fill();
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 32, 27);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Lichen", 58, 37);
        const double nameWidth = g.measureText("Lichen");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        const int state = stateShown.get();
        const char* what = state == 2   ? "holding the last play"
                           : state == 1 ? "measuring the play"
                                        : "listening";
        g.fillText(what, 66 + nameWidth, 37);
        const double w2 = g.measureText(what);
        g.textBaseline(TextBaseline::Middle);
        clockX = 90 + nameWidth + w2;
        drawClock(g);
        drawLevel(g, 680, 22, 150, levelLeft.get(), levelRight.get(), "LEVEL");
        pill(g, restartX, pillY, 120, pillH, "RESTART ON PLAY", restartOnPlay.on(), kLichen,
             hovered(restartX, pillY, 120, pillH));
        pill(g, resetX, pillY, 64, pillH, "RESET", false, kLichen,
             hovered(resetX, pillY, 64, pillH));
    }

    // The timer and its clock: accumulating since play started (lit, the
    // time it covers), or following the last 60 s. A click on either switches.
    void drawClock(Canvas& g) {
        const bool on = accumulate.on();
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (on) {
            const int whole = toInt(0.001 * measured.value());
            g.fillStyle(Theme::textMuted);
            g.monoFont(10.5);
            g.fillText(format("%d:%02d", idiv(whole, 60), imod(whole, 60)), clockX + 11, clockY);
            clockW = 11 + g.measureText(format("%d:%02d", idiv(whole, 60), imod(whole, 60)));
        } else {
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(11);
            g.fillText("last 60 s", clockX + 11, clockY);
            clockW = 11 + g.measureText("last 60 s");
        }
        const bool hot = dragging == 0 && onClock(hoverX, hoverY);
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
        return y > clockY - 10 && y < clockY + 10 && x > clockX - 9 && x < clockX + clockW + 2;
    }

    // --- loudness -------------------------------------------------------------------------------

    void loudBar(Canvas& g, double y, double v, double top, const char* name) {
        label(g, name, lx + 16, y, TextAlign::Left);
        const double bx = lx + 98, bw = 186;
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(bx, y - 3, bw, 6, 3);
        g.fill();
        const double t = clamp((v + 45.0) / 45.0, 0.0, 1.0);
        if (v > -69.95 && t > 0.0) {
            g.linearGradient(bx, 0, bx + bw, 0, kLichenDeep, kMarigold);
            g.beginPath();
            g.roundRect(bx, y - 3, bw * t, 6, 3);
            g.fill();
        }
        if (top > -69.95) {
            const double mx = bx + bw * clamp((top + 45.0) / 45.0, 0.0, 1.0);
            g.fillStyle(kLinen);
            g.fillRect(mx - 1, y - 5, 2, 10);
        }
        const double tx = bx + bw * clamp((target + 45.0) / 45.0, 0.0, 1.0);
        g.fillStyle(kMarigold);
        g.beginPath();
        g.moveTo(tx - 4, y - 8);
        g.lineTo(tx + 4, y - 8);
        g.lineTo(tx, y - 4);
        g.closePath();
        g.fill();
        valueStyle(g, 10.5);
        g.textAlign(TextAlign::Right);
        fillLoud(g, v, lx + lw - 16, y);
    }

    void reading(Canvas& g, double x, double y, const char* name) {
        label(g, name, x, y, TextAlign::Left);
        valueStyle(g, 12);
    }

    void drawLoudness(Canvas& g) {
        panel(g, lx, ly, lw, lh);
        heading(g, "Loudness", lx + 16, ly + 20);
        aside(g, "BS.1770, EBU R128", lx + lw - 16, ly + 20, TextAlign::Right);
        label(g, "INTEGRATED", lx + 16, ly + 42, TextAlign::Left);
        const double i = integrated.value();
        g.fillStyle(Theme::text);
        g.monoFont(30);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        fillLoud(g, i, lx + 14, ly + 76);
        const double iw = i > -69.95 ? g.measureText(format("%.1f", i)) : g.measureText("–");
        g.fillStyle(Theme::textMuted);
        g.boldFont(10);
        g.fillText("LUFS", lx + 22 + iw, ly + 76);
        // Against the target.
        g.textAlign(TextAlign::Right);
        if (i > -69.95) {
            const double d = i - target;
            const int colour = abs(d) <= 1.0 ? kLichen : (abs(d) <= 3.0 ? kRust : kClay);
            g.fillStyle(colour);
            g.monoFont(13);
            if (abs(d) < 0.05)
                g.fillText("on target", lx + lw - 16, ly + 60);
            else
                g.fillText(format("%+.1f LU", d), lx + lw - 16, ly + 60);
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(10.5);
            g.fillText(d > 0.0 ? "louder than the target" : "quieter than the target", lx + lw - 16,
                       ly + 76);
        } else {
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(10.5);
            g.fillText("play something", lx + lw - 16, ly + 70);
        }
        g.textBaseline(TextBaseline::Middle);
        loudBar(g, ly + 100, shortTerm.value(), shortTermMax.value(), "SHORT-TERM");
        loudBar(g, ly + 122, momentary.value(), momentaryMax.value(), "MOMENTARY");
        // The readings since play.
        const double c0 = lx + 16, c1 = lx + 124, c2 = lx + 232;
        const double r0 = ly + 152, r1 = ly + 194;
        reading(g, c0, r0, "RANGE");
        g.fillText(format("%.1f LU", lra.value()), c0, r0 + 16);
        reading(g, c1, r0, "TRUE PEAK");
        if (truePeak.value() <= -69.95)
            g.fillText("–", c1, r0 + 16);
        else
            g.fillText(format("%.1f dBTP", truePeak.value()), c1, r0 + 16);
        reading(g, c2, r0, "PLR");
        g.fillText(format("%.1f dB", plr.value()), c2, r0 + 16);
        reading(g, c0, r1, "LOUDEST 3 S");
        fillLoud(g, shortTermMax.value(), c0, r1 + 16);
        reading(g, c1, r1, "CREST");
        g.fillText(format("%.1f dB", crest.value()), c1, r1 + 16);
        reading(g, c2, r1, "LOUDEST 0.4 S");
        fillLoud(g, momentaryMax.value(), c2, r1 + 16);
        // The target: four usual ones, and the value to drag.
        label(g, "TARGET", lx + 16, chipY + chipH * 0.5, TextAlign::Left);
        for (int k = 0; k < 4; ++k) {
            const double v = chipValue(k);
            const double x = chipX + k * (chipW + 6);
            const bool lit = abs(target - v) < 0.05;
            if (k == 0)
                pill(g, x, chipY, chipW, chipH, "-23", lit, kMarigold,
                     hovered(x, chipY, chipW, chipH));
            if (k == 1)
                pill(g, x, chipY, chipW, chipH, "-16", lit, kMarigold,
                     hovered(x, chipY, chipW, chipH));
            if (k == 2)
                pill(g, x, chipY, chipW, chipH, "-14", lit, kMarigold,
                     hovered(x, chipY, chipW, chipH));
            if (k == 3)
                pill(g, x, chipY, chipW, chipH, "-9", lit, kMarigold,
                     hovered(x, chipY, chipW, chipH));
        }
        const bool hot = dragging == 1 || hovered(lx + lw - 84, chipY - 4, 70, chipH + 8);
        g.fillStyle(hot ? kMarigold : Theme::text);
        g.monoFont(11);
        g.textAlign(TextAlign::Right);
        g.fillText(format("%.1f LUFS", target.value()), lx + lw - 16, chipY + chipH * 0.5);
    }

    double chipValue(int k) const {
        if (k == 0) return -23.0;
        if (k == 1) return -16.0;
        if (k == 2) return -14.0;
        return -9.0;
    }

    // --- the history
    // ------------------------------------------------------------------------------

    double loudY(double v) const {
        return dbToY(clamp(v, kLoudFloor, kLoudTop), kLoudFloor, kLoudTop, gy, gh);
    }

    void drawHistory(Canvas& g) {
        panel(g, hx, hy, hw, hh);
        heading(g, "The last minute", hx + 16, hy + 20);
        // Its key.
        g.fillStyle(kMarigold);
        g.fillRect(hx + 330, hy + 19, 14, 2);
        label(g, "SHORT-TERM", hx + 350, hy + 20, TextAlign::Left);
        g.fillStyle(kLichen);
        g.globalAlpha(0.5);
        g.fillRect(hx + 430, hy + 15, 14, 10);
        g.globalAlpha(1.0);
        label(g, "MOMENTARY", hx + 450, hy + 20, TextAlign::Left);
        g.fillStyle(kLinen);
        g.globalAlpha(0.45);
        g.fillRect(hx + 526, hy + 19, 14, 1);
        g.globalAlpha(1.0);
        label(g, "TRUE PEAK", hx + 546, hy + 20, TextAlign::Left);
        label(g, "SPREAD", dx + dw * 0.5, hy + 20, TextAlign::Center);

        well(g, gx, gy, gw, gh);
        well(g, dx, gy, dw, gh);
        g.save();
        g.clip(gx, gy, gw, gh);
        g.lineWidth(1);
        for (int k = 1; k < 8; ++k) {
            const double y = loudY(-6.0 * k);
            g.strokeStyle(kGrid);
            g.beginPath();
            g.moveTo(gx, y);
            g.lineTo(gx + gw, y);
            g.stroke();
        }
        for (int k = 1; k < 4; ++k) {
            const double x = gx + gw * k / 4.0;
            g.beginPath();
            g.moveTo(x, gy);
            g.lineTo(x, gy + gh);
            g.stroke();
        }
        // The range: where the middle of the play's short-term loudness lies.
        if (lra.value() > 0.05 && lraHighShown.get() > -69.0) {
            const double y0 = loudY(lraHighShown.get()), y1 = loudY(lraLowShown.get());
            g.fillStyle(kMarigold);
            g.globalAlpha(0.06);
            g.fillRect(gx, y0, gw, y1 - y0);
            g.globalAlpha(1.0);
        }
        // The target.
        const double ty = loudY(target);
        g.fillStyle(kMarigold);
        g.globalAlpha(0.55);
        for (double x = gx + 4; x < gx + gw; x += 10) g.fillRect(x, ty - 0.5, 5, 1);
        g.globalAlpha(1.0);

        const int head = historyHead.get(), count = historyCount.get();
        const double step = gw / kHistory;
        // The momentary loudness, as an area.
        if (count > 1) {
            g.fillStyle(kLichen);
            g.globalAlpha(0.22);
            g.beginPath();
            g.moveTo(gx + gw - (count - 1) * step, gy + gh);
            for (int j = 0; j < count; ++j) {
                const int k = wrap(head - count + j, kHistory);
                g.lineTo(gx + gw - (count - 1 - j) * step, loudY(histMomentary[k]));
            }
            g.lineTo(gx + gw, gy + gh);
            g.closePath();
            g.fill();
            g.globalAlpha(1.0);
            // The true peaks, and the short-term loudness.
            trace(g, histPeak, head, count, step, kLinen, 0.35, 1.0);
            trace(g, histShort, head, count, step, kMarigold, 1.0, 1.8);
            // Overs, along the top.
            g.fillStyle(kClay);
            for (int j = 0; j < count; ++j) {
                const int k = wrap(head - count + j, kHistory);
                if (histPeak[k] > 0.0)
                    g.fillRect(gx + gw - (count - 1 - j) * step - 1, gy + 2, 2, 5);
            }
        }
        // A hover: the moment under the pointer.
        if (hovered(gx, gy, gw, gh) && count > 0) {
            const int back = clamp(roundToInt((gx + gw - hoverX) / step), 0, count - 1);
            const int k = wrap(head - 1 - back, kHistory);
            const double x = gx + gw - back * step;
            g.strokeStyle(Theme::textFaint);
            g.globalAlpha(0.6);
            g.beginPath();
            g.moveTo(x, gy);
            g.lineTo(x, gy + gh);
            g.stroke();
            g.globalAlpha(1.0);
            const double bx = x > gx + gw - 170 ? x - 166 : x + 8;
            g.fillStyle(kInk);
            g.globalAlpha(0.85);
            g.beginPath();
            g.roundRect(bx, gy + 8, 158, 52, 5);
            g.fill();
            g.globalAlpha(1.0);
            valueStyle(g, 9.5);
            g.fillStyle(Theme::textMuted);
            g.fillText(format("%.1f s ago", back * 0.1), bx + 8, gy + 18);
            g.fillStyle(kMarigold);
            if (histShort[k] > -69.95)
                g.fillText(format("short-term %.1f", histShort[k]), bx + 8, gy + 31);
            g.fillStyle(kLichen);
            if (histMomentary[k] > -69.95)
                g.fillText(format("momentary  %.1f", histMomentary[k]), bx + 8, gy + 43);
            g.fillStyle(kLinen);
            if (histPeak[k] > -119.0)
                g.fillText(format("true peak  %.1f", histPeak[k]), bx + 8, gy + 55);
        }
        g.restore();
        // The scale, and the time.
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 1; k < 8; ++k) g.fillText(format("%d", -6 * k), gx + 4, loudY(-6.0 * k) - 6);
        g.textAlign(TextAlign::Center);
        g.fillText("-60 s", gx + 14, gy + gh + 11);
        g.fillText("-45 s", gx + gw * 0.25, gy + gh + 11);
        g.fillText("-30 s", gx + gw * 0.5, gy + gh + 11);
        g.fillText("-15 s", gx + gw * 0.75, gy + gh + 11);
        g.fillText("now", gx + gw - 10, gy + gh + 11);
        drawSpread(g);
    }

    void trace(Canvas& g, const SharedArray<float, kHistory>& v, int head, int count, double step,
               int colour, double alpha, double weight) {
        g.strokeStyle(colour);
        g.globalAlpha(alpha);
        g.lineWidth(weight);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        bool open = false;
        for (int j = 0; j < count; ++j) {
            const int k = wrap(head - count + j, kHistory);
            const double x = gx + gw - (count - 1 - j) * step;
            if (v[k] <= -69.95) {
                open = false;
                continue;
            }
            if (open)
                g.lineTo(x, loudY(v[k]));
            else
                g.moveTo(x, loudY(v[k]));
            open = true;
        }
        g.stroke();
        g.globalAlpha(1.0);
    }

    // The play's short-term loudness, how often each level came, with the
    // loudness range bracketed beside it.
    void drawSpread(Canvas& g) {
        double most = 0.0;
        for (int b = 0; b < kSpread; ++b) most = max(most, static_cast<double>(spreadShown[b]));
        g.save();
        g.clip(dx, gy, dw, gh);
        if (most > 0.0) {
            for (int b = 0; b < kSpread; ++b) {
                if (spreadShown[b] <= 0.0f) continue;
                const double lo = -60.0 + b;
                if (lo + 1.0 < kLoudFloor) continue;
                const double y0 = loudY(lo + 1.0), y1 = loudY(lo);
                const double inRange =
                    lo + 0.5 >= lraLowShown.get() - 0.5 && lo + 0.5 <= lraHighShown.get() + 0.5;
                g.fillStyle(inRange ? kMarigold : kLichenDeep);
                g.globalAlpha(inRange ? 0.75 : 0.6);
                g.fillRect(dx + 2, y0 + 0.5, (dw - 14) * spreadShown[b] / most,
                           max(1.0, y1 - y0 - 1));
            }
            g.globalAlpha(1.0);
        }
        if (lra.value() > 0.05 && lraHighShown.get() > -69.0) {
            const double y0 = loudY(lraHighShown.get()), y1 = loudY(lraLowShown.get());
            g.strokeStyle(kMarigold);
            g.lineWidth(1.2);
            g.beginPath();
            g.moveTo(dx + dw - 9, y0);
            g.lineTo(dx + dw - 5, y0);
            g.lineTo(dx + dw - 5, y1);
            g.lineTo(dx + dw - 9, y1);
            g.stroke();
        }
        g.restore();
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("LRA %.1f", lra.value()), dx + dw * 0.5, gy + gh + 11);
    }

    // --- the spectrum
    // -----------------------------------------------------------------------------

    double slopeDb() const {
        if (slope.index() == 0) return 0.0;
        if (slope.index() == 1) return 3.0;
        return 4.5;
    }
    double drawnTilt(int i) const { return slopeDb() * log2(pointHz(i) / 1000.0); }
    double specY(double db) const {
        return dbToY(clamp(db, kSpecFloor - 6.0, kSpecTop + 6.0), kSpecFloor, kSpecTop, fy, fh);
    }
    double pointX(int i) const { return fx + fw * i / (kPoints - 1.0); }

    // The drawn lines ease toward each new frame, rising faster than they fall.
    void ease() {
        const bool valid = specValid.get();
        for (int i = 0; i < kPoints; ++i) {
            const double m = valid ? specNowM[i] : -200.0, s = valid ? specNowS[i] : -200.0;
            const double a = specAllValid.get() ? specAllM[i] : -200.0;
            if (!primed) {
                eNowM[i] = m;
                eNowS[i] = s;
                eAll[i] = a;
                eHold[i] = m;
            } else {
                eNowM[i] += (m - eNowM[i]) * (m > eNowM[i] ? 0.45 : 0.18);
                eNowS[i] += (s - eNowS[i]) * (s > eNowS[i] ? 0.45 : 0.18);
                eAll[i] += (a - eAll[i]) * 0.3;
                eHold[i] = max(eNowM[i], eHold[i] - 0.15);
            }
        }
        primed = true;
    }

    // A line through the points, as Bezier segments (Catmull-Rom).
    void curve(Canvas& g, const Array<double, kPoints>& v, int colour, double alpha, double weight,
               bool area) {
        g.lineJoin(LineJoin::Round);
        g.lineCap(LineCap::Round);
        g.beginPath();
        double x0 = pointX(0), y0 = specY(v[0] + drawnTilt(0));
        double x1 = x0, y1 = y0;
        g.moveTo(x1, y1);
        for (int i = 0; i < kPoints - 1; ++i) {
            const double x2 = pointX(i + 1), y2 = specY(v[i + 1] + drawnTilt(i + 1));
            const int j = min(i + 2, kPoints - 1);
            const double x3 = pointX(j), y3 = specY(v[j] + drawnTilt(j));
            g.bezierCurveTo(x1 + (x2 - x0) / 6.0, y1 + (y2 - y0) / 6.0, x2 - (x3 - x1) / 6.0,
                            y2 - (y3 - y1) / 6.0, x2, y2);
            x0 = x1;
            y0 = y1;
            x1 = x2;
            y1 = y2;
        }
        if (area) {
            g.lineTo(fx + fw, fy + fh + 8);
            g.lineTo(fx, fy + fh + 8);
            g.closePath();
            g.linearGradient(0, fy, 0, fy + fh, colour, kGraphBottom);
            g.globalAlpha(alpha);
            g.fill();
        } else {
            g.strokeStyle(colour);
            g.globalAlpha(alpha);
            g.lineWidth(weight);
            g.stroke();
        }
        g.globalAlpha(1.0);
    }

    void drawSpectrum(Canvas& g) {
        ease();
        panel(g, sx, sy, sw, sh);
        heading(g, "Spectrum", sx + 16, sy + 20);
        g.textBaseline(TextBaseline::Middle);
        valueStyle(g, 10);
        g.fillStyle(Theme::textMuted);
        if (toneValid.get()) {
            if (abs(tilt.value()) < 0.05)
                g.fillText("tilt 0.0 dB/oct", sx + 100, sy + 20);
            else
                g.fillText(format("tilt %+.1f dB/oct", tilt.value()), sx + 100, sy + 20);
            g.fillText("centroid", sx + 238, sy + 20);
            fillHz(g, centroid.value(), sx + 300, sy + 20);
        }
        // The key to the lines.
        g.fillStyle(kLinen);
        g.globalAlpha(0.5);
        g.fillRect(sx + 470, sy + 15, 14, 10);
        g.globalAlpha(1.0);
        label(g, "NOW, MID", sx + 490, sy + 20, TextAlign::Left);
        g.fillStyle(kSky);
        g.fillRect(sx + 560, sy + 19, 14, 2);
        label(g, "SIDE", sx + 580, sy + 20, TextAlign::Left);
        g.fillStyle(kMarigold);
        g.fillRect(sx + 622, sy + 19, 14, 2);
        label(g, accumulate.on() ? "SINCE PLAY" : "LAST 60 S", sx + 642, sy + 20, TextAlign::Left);
        g.fillStyle(kLinen);
        g.globalAlpha(0.3);
        g.fillRect(sx + 716, sy + 19, 14, 1);
        g.globalAlpha(1.0);
        label(g, "PEAK", sx + 736, sy + 20, TextAlign::Left);
        label(g, "SLOPE", slopeX - 8, slopeY + 9, TextAlign::Right);
        pill(g, slopeX, slopeY, 46, 18, "FLAT", slope.index() == 0, kLinen,
             hovered(slopeX, slopeY, 46, 18));
        pill(g, slopeX + 52, slopeY, 46, 18, "3 dB", slope.index() == 1, kLinen,
             hovered(slopeX + 52, slopeY, 46, 18));
        pill(g, slopeX + 104, slopeY, 56, 18, "4.5 dB", slope.index() == 2, kLinen,
             hovered(slopeX + 104, slopeY, 56, 18));

        well(g, fx, fy, fw, fh);
        g.save();
        g.clip(fx, fy, fw, fh);
        g.lineWidth(1);
        for (int d = 1; d <= 4; ++d) {
            const double decade = pow(10.0, d);
            for (int m = 1; m < 10; ++m) {
                const double hz = decade * m;
                if (hz < 20.0 || hz > 20000.0) continue;
                const double x = hzToX(hz, 20, 20000, fx, fw);
                g.strokeStyle(m == 1 ? kGridLit : kGrid);
                g.globalAlpha(m == 1 || m == 2 || m == 5 ? 1.0 : 0.45);
                g.beginPath();
                g.moveTo(x, fy);
                g.lineTo(x, fy + fh);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        for (int k = 1; k < 8; ++k) {
            const double y = specY(-12.0 * k);
            g.strokeStyle(kGrid);
            g.beginPath();
            g.moveTo(fx, y);
            g.lineTo(fx + fw, y);
            g.stroke();
        }
        if (specValid.get()) {
            curve(g, eNowM, kLinen, 0.28, 0.0, true);
            curve(g, eHold, kLinen, 0.3, 1.0, false);
            curve(g, eNowS, kSky, 0.8, 1.2, false);
            if (specAllValid.get()) curve(g, eAll, kMarigold, 0.9, 1.4, false);
            curve(g, eNowM, kLinen, 0.9, 1.5, false);
        }
        // The centroid, along the floor.
        if (toneValid.get()) {
            const double cxp = hzToX(centroid.value(), 20, 20000, fx, fw);
            g.fillStyle(kMarigold);
            g.beginPath();
            g.moveTo(cxp - 5, fy + fh);
            g.lineTo(cxp + 5, fy + fh);
            g.lineTo(cxp, fy + fh - 7);
            g.closePath();
            g.fill();
        }
        g.restore();
        // The scale.
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 1; k < 8; ++k) g.fillText(format("%d", -12 * k), fx + 4, specY(-12.0 * k) - 6);
        drawBands(g);
        if (hovered(fx, fy, fw, fh)) drawProbe(g);
    }

    // The seven named bands under their stretch of the spectrum, each in dB
    // of the whole.
    void drawBands(Canvas& g) {
        const double by = fy + fh + 6, bh = 34;
        for (int n = 0; n < kNamed; ++n) {
            const double lo = bandEdge(n), hi = bandEdge(n + 1);
            const double x0 = hzToX(lo, 20, 20000, fx, fw) + 1,
                         x1 = hzToX(hi, 20, 20000, fx, fw) - 1;
            const double v = bandValue(n);
            const double t = toneValid.get() ? clamp((v + 30.0) / 30.0, 0.0, 1.0) : 0.0;
            g.fillStyle(blend(kInk, kLichenDeep, 0.35 + 0.65 * t));
            g.beginPath();
            g.roundRect(x0, by, x1 - x0, bh, 5);
            g.fill();
            const double mx = 0.5 * (x0 + x1);
            g.fillStyle(t > 0.6 ? kInk : Theme::textFaint);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            if (n == 0) g.fillText("SUB", mx, by + 10);
            if (n == 1) g.fillText("LOW", mx, by + 10);
            if (n == 2) g.fillText("LOW MID", mx, by + 10);
            if (n == 3) g.fillText("MID", mx, by + 10);
            if (n == 4) g.fillText("HIGH MID", mx, by + 10);
            if (n == 5) g.fillText("PRESENCE", mx, by + 10);
            if (n == 6) g.fillText("AIR", mx, by + 10);
            g.fillStyle(t > 0.6 ? kInk : Theme::text);
            g.monoFont(10);
            if (toneValid.get())
                g.fillText(format("%.1f dB", v), mx, by + 24);
            else
                g.fillText("–", mx, by + 24);
        }
    }

    double bandEdge(int n) const {
        if (n == 0) return 20.0;
        if (n == 1) return 60.0;
        if (n == 2) return 250.0;
        if (n == 3) return 500.0;
        if (n == 4) return 2000.0;
        if (n == 5) return 4000.0;
        if (n == 6) return 6000.0;
        return 20000.0;
    }
    double bandValue(int n) const {
        if (n == 0) return sub.value();
        if (n == 1) return low.value();
        if (n == 2) return lowMid.value();
        if (n == 3) return mid.value();
        if (n == 4) return highMid.value();
        if (n == 5) return presence.value();
        return air.value();
    }

    // Under the pointer: the frequency, its note, the levels there.
    void drawProbe(Canvas& g) {
        const double hz = xToHz(hoverX, 20, 20000, fx, fw);
        const int i = clamp(roundToInt((hoverX - fx) / fw * (kPoints - 1)), 0, kPoints - 1);
        g.strokeStyle(Theme::textFaint);
        g.globalAlpha(0.6);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(hoverX, fy);
        g.lineTo(hoverX, fy + fh);
        g.stroke();
        g.globalAlpha(1.0);
        const double bx = hoverX > fx + fw - 180 ? hoverX - 176 : hoverX + 8;
        g.fillStyle(kInk);
        g.globalAlpha(0.88);
        g.beginPath();
        g.roundRect(bx, fy + 8, 168, 56, 5);
        g.fill();
        g.globalAlpha(1.0);
        valueStyle(g, 9.5);
        g.fillStyle(Theme::text);
        fillHz(g, hz, bx + 8, fy + 19);
        g.fillStyle(Theme::textMuted);
        fillNote(g, hz, bx + 92, fy + 19);
        g.fillStyle(kLinen);
        if (eNowM[i] > -150.0) g.fillText(format("now   %.1f dB", eNowM[i]), bx + 8, fy + 33);
        g.fillStyle(kSky);
        if (eNowS[i] > -150.0) g.fillText(format("side  %.1f dB", eNowS[i]), bx + 8, fy + 45);
        g.fillStyle(kMarigold);
        if (specAllValid.get() && eAll[i] > -150.0)
            g.fillText(format("play  %.1f dB", eAll[i]), bx + 8, fy + 57);
    }

    // --- stereo
    // -------------------------------------------------------------------------------------

    // A bar from -1 to +1 (or a span either side of 0): the reading now as a
    // needle, the play's as a tick.
    void stereoBar(Canvas& g, double y, double now, double all, double span, const char* name,
                   bool phase) {
        label(g, name, cx, y - 10, TextAlign::Left);
        const double bx = cx, bw = cw - 52;
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(bx, y - 3, bw, 6, 3);
        g.fill();
        const double mid0 = bx + bw * 0.5;
        g.fillStyle(kGridLit);
        g.fillRect(mid0 - 0.5, y - 5, 1, 10);
        const bool heard = max(levelLeft.get(), levelRight.get()) > -100.0;
        const double tn = heard ? clamp(0.5 + 0.5 * now / span, 0.0, 1.0) : 0.5;
        const int colour = phase ? (now < 0.0 ? kClay : (now < 0.2 ? kRust : kLichen)) : kLichen;
        g.fillStyle(colour);
        g.beginPath();
        if (tn >= 0.5)
            g.roundRect(mid0, y - 3, bw * (tn - 0.5), 6, 3);
        else
            g.roundRect(bx + bw * tn, y - 3, bw * (0.5 - tn), 6, 3);
        g.fill();
        const double ta = clamp(0.5 + 0.5 * all / span, 0.0, 1.0);
        g.fillStyle(kMarigold);
        g.fillRect(bx + bw * ta - 1, y - 6, 2, 12);
        valueStyle(g, 10.5);
        g.textAlign(TextAlign::Right);
        if (phase)
            g.fillText(format("%+.2f", all), cx + cw, y);
        else if (abs(all) < 0.05)
            g.fillText("0.0 dB", cx + cw, y);
        else
            g.fillText(format("%+.1f dB", all), cx + cw, y);
    }

    void drawStereo(Canvas& g) {
        panel(g, ox, oy, ow, oh);
        heading(g, "Stereo", ox + 16, oy + 20);
        aside(g, accumulate.on() ? "bars now, ticks since play" : "bars now, ticks the last 60 s",
              ox + ow - 16, oy + 20, TextAlign::Right);
        drawScope(g);
        stereoBar(g, oy + 52, corrNeedle.get(), correlation.value(), 1.0, "CORRELATION", true);
        stereoBar(g, oy + 84, balanceNeedle.get(), balance.value(), 12.0, "BALANCE", false);
        const double r = oy + 106;
        reading(g, cx, r, "WIDTH");
        g.fillText(format("%.0f %%", 100.0 * width.value()), cx, r + 15);
        reading(g, cx + 86, r, "BELOW 250 HZ");
        g.fillText(format("%.0f %%", 100.0 * lowWidth.value()), cx + 86, r + 15);
        reading(g, cx + 182, r, "MONO LOSS");
        g.fillText(format("%.1f LU", monoLoss.value()), cx + 182, r + 15);
        drawBandPhase(g);
    }

    void drawScope(Canvas& g) {
        well(g, vx, vy, vs, vs);
        const double scx = vx + vs * 0.5, scy = vy + vs * 0.5, r = vs * 0.5 - 12;
        g.save();
        g.clip(vx, vy, vs, vs);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.circle(scx, scy, r);
        g.stroke();
        g.strokeStyle(kGridLit);
        g.beginPath();
        g.moveTo(scx, scy - r);
        g.lineTo(scx, scy + r);
        g.moveTo(scx - r, scy);
        g.lineTo(scx + r, scy);
        g.stroke();
        const double d = r * 0.7071;
        g.strokeStyle(kGrid);
        g.beginPath();
        g.moveTo(scx - d, scy - d);
        g.lineTo(scx + d, scy + d);
        g.moveTo(scx + d, scy - d);
        g.lineTo(scx - d, scy + d);
        g.stroke();
        // Scaled so the cloud's spread fills a third of the circle and its
        // peaks never run far past it.
        double top = 0.0, power = 0.0;
        for (int k = 0; k < kScope; ++k) {
            const double m = 0.5 * (scopeL[k] + scopeR[k]), s = 0.5 * (scopeL[k] - scopeR[k]);
            top = max(top, max(abs(m + s), abs(m - s)));
            power += m * m + s * s;
        }
        const double spread = sqrt(power / kScope);
        const double want = top > 1e-5 ? min(0.4 / spread, 2.0 / top) : zoom;
        zoom = want < zoom || !zoomed ? want : zoom + 0.08 * (want - zoom);
        zoomed = top > 1e-5;
        zoom = clamp(zoom, 0.5, 3000.0);
        const int head = scopeHead.get();
        for (int half = 0; half < 2; ++half) {
            g.fillStyle(half == 0 ? kLichenDeep : kLichen);
            g.globalAlpha(half == 0 ? 0.6 : 0.7);
            g.beginPath();
            for (int j = 0; j < kScope / 2; ++j) {
                const int k = wrap(head + half * (kScope / 2) + j, kScope);
                const double m = 0.5 * (scopeL[k] + scopeR[k]), s = 0.5 * (scopeL[k] - scopeR[k]);
                if (m == 0.0 && s == 0.0) continue;
                g.rect(scx - s * zoom * r - 0.75, scy - m * zoom * r - 0.75, 1.5, 1.5);
            }
            g.fill();
        }
        g.globalAlpha(1.0);
        g.restore();
        label(g, "L", scx - d - 5, scy - d - 5, TextAlign::Center);
        label(g, "R", scx + d + 5, scy - d - 5, TextAlign::Center);
    }

    double bandCellW() const { return (cw + 0.0) / kThirds; }

    // Third octave by third octave: how alike the two channels are there.
    void drawBandPhase(Canvas& g) {
        const double y = oy + 150, h = 20;
        label(g, "BY BAND", cx, y - 9, TextAlign::Left);
        const double cell = bandCellW();
        for (int b = 0; b < kThirds; ++b) {
            const double c = corrNowBand[b], all = corrAllBand[b];
            const double x = cx + b * cell;
            int colour = kInk;
            if (c < 1.5)
                colour =
                    c < 0.0 ? blend(kRust, kClay, -c) : blend(kRust, kLichen, min(1.0, c * 2.0));
            g.fillStyle(colour);
            g.beginPath();
            g.roundRect(x + 0.5, y, cell - 1, h, 2);
            g.fill();
            if (all < 1.5) {
                g.fillStyle(all < 0.0 ? kClay : kMarigold);
                g.fillRect(x + 1, y + h + 2, cell - 2, 2);
            }
        }
        g.fillStyle(Theme::textFaint);
        g.monoFont(8);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("20", cx + 0.5 * cell, y + h + 11);
        g.fillText("200", cx + 10.5 * cell, y + h + 11);
        g.fillText("2k", cx + 20.5 * cell, y + h + 11);
        g.fillText("20k", cx + 30.5 * cell, y + h + 11);
        if (hovered(cx, y, cw, h)) {
            const int b = clamp(floorToInt((hoverX - cx) / cell), 0, kThirds - 1);
            g.fillStyle(kInk);
            g.globalAlpha(0.9);
            g.beginPath();
            g.roundRect(cx + 50, y - 19, cw - 50, 15, 4);
            g.fill();
            g.globalAlpha(1.0);
            valueStyle(g, 9);
            g.textAlign(TextAlign::Left);
            fillHz(g, thirdHz(b), cx + 56, y - 11.5);
            if (corrNowBand[b] < 1.5)
                g.fillText(format("now %+.2f", corrNowBand[b]), cx + 118, y - 11.5);
            if (corrAllBand[b] < 1.5)
                g.fillText(format("play %+.2f", corrAllBand[b]), cx + 186, y - 11.5);
        }
    }

    // --- harmony
    // ------------------------------------------------------------------------------------

    bool inKey(int k, int pc) const {
        // The scale degrees of a major or a natural minor key.
        const int d = imod(pc - imod(k, 12) + 12, 12);
        if (k < 12) return d == 0 || d == 2 || d == 4 || d == 5 || d == 7 || d == 9 || d == 11;
        return d == 0 || d == 2 || d == 3 || d == 5 || d == 7 || d == 8 || d == 10;
    }

    void drawHarmony(Canvas& g) {
        panel(g, ax, ay, aw, ah);
        heading(g, "Harmony", ax + 16, ay + 20);
        const int k = key.index() - 1;
        double most = 0.0;
        for (int c = 0; c < 12; ++c) most = max(most, static_cast<double>(chromaShown[c]));
        double mostNow = 0.0;
        for (int c = 0; c < 12; ++c) mostNow = max(mostNow, static_cast<double>(chromaNowShown[c]));
        // The wheel: pitch classes round the circle of fifths, each petal as
        // long as its share of the play.
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.circle(wx, wy, wr);
        g.stroke();
        g.beginPath();
        g.circle(wx, wy, wr * 0.5);
        g.stroke();
        for (int i = 0; i < 12; ++i) {
            const int pc = imod(7 * i, 12);
            const double a = -0.5 * pi + twoPi * i / 12.0;
            const double share = most > 0.0 ? chromaShown[pc] / most : 0.0;
            const double len = 8.0 + (wr - 8.0) * share;
            const bool lit = k >= 0 && inKey(k, pc);
            g.save();
            g.translate(wx, wy);
            g.rotate(a);
            g.fillStyle(lit ? kLichen : kBark);
            g.globalAlpha(most > 0.0 ? 0.35 + 0.55 * share : 0.2);
            g.beginPath();
            g.ellipse(len * 0.5 + 3, 0, len * 0.5, 3.5 + 3.0 * share);
            g.fill();
            g.globalAlpha(1.0);
            if (mostNow > 0.0) {
                const double rn = 8.0 + (wr - 8.0) * chromaNowShown[pc] / mostNow;
                g.fillStyle(kLinen);
                g.beginPath();
                g.circle(rn + 3, 0, 1.8);
                g.fill();
            }
            g.restore();
            g.fillStyle(k >= 0 && pc == imod(k, 12) ? kMarigold
                                                    : (lit ? Theme::text : Theme::textFaint));
            g.boldFont(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            fillPitch(g, pc, wx + (wr + 11) * cos(a), wy + (wr + 11) * sin(a));
        }
        // The key, and the next likeliest.
        const double tx = ax + 158;
        label(g, "KEY", tx, ay + 52, TextAlign::Left);
        g.fillStyle(k >= 0 ? Theme::text : Theme::textFaint);
        g.serifFont(16);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (k >= 0)
            fillKey(g, k, tx, ay + 72);
        else
            g.fillText("none yet", tx, ay + 72);
        valueStyle(g, 9.5);
        g.fillStyle(Theme::textMuted);
        if (k >= 0) g.fillText(format("match %.0f %%", 100.0 * keyStrength.value()), tx, ay + 90);
        int first = -1, second = -1, third = -1;
        for (int c = 0; c < 24; ++c) {
            const double v = keyShown[c];
            if (first < 0 || v > keyShown[first]) {
                third = second;
                second = first;
                first = c;
            } else if (second < 0 || v > keyShown[second]) {
                third = second;
                second = c;
            } else if (third < 0 || v > keyShown[third]) {
                third = c;
            }
        }
        if (specAllValid.get() && first >= 0 && keyShown[first] > 0.3f) {
            label(g, "ALSO LIKELY", tx, ay + 116, TextAlign::Left);
            const int a1 = k == first ? second : first;
            const int a2 = k == first ? third : second;
            g.fillStyle(Theme::textMuted);
            g.font(10.5);
            g.textAlign(TextAlign::Left);
            if (a1 >= 0) {
                fillKey(g, a1, tx, ay + 134);
                valueStyle(g, 9.5);
                g.fillStyle(Theme::textFaint);
                g.textAlign(TextAlign::Right);
                g.fillText(format("%.0f", 100.0 * max(0.0, static_cast<double>(keyShown[a1]))),
                           ax + aw - 16, ay + 134);
            }
            if (a2 >= 0) {
                g.fillStyle(Theme::textMuted);
                g.font(10.5);
                g.textAlign(TextAlign::Left);
                fillKey(g, a2, tx, ay + 152);
                valueStyle(g, 9.5);
                g.fillStyle(Theme::textFaint);
                g.textAlign(TextAlign::Right);
                g.fillText(format("%.0f", 100.0 * max(0.0, static_cast<double>(keyShown[a2]))),
                           ax + aw - 16, ay + 152);
            }
        }
        aside(g, "a guess from the notes heard", ax + aw - 16, ay + ah - 14, TextAlign::Right);
    }

    // --- health
    // -------------------------------------------------------------------------------------

    // 0 well, 1 worth a look, 2 a problem; -1 nothing measured yet.
    void check(Canvas& g, int row, int state, const char* name) {
        const double y = ey + 46 + row * 20;
        const int colour =
            state < 0 ? kGridLit : (state == 0 ? kLichen : (state == 1 ? kRust : kClay));
        g.fillStyle(colour);
        g.beginPath();
        g.circle(ex + 22, y, 4);
        g.fill();
        g.fillStyle(state > 0 ? Theme::text : Theme::textMuted);
        g.font(10.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, ex + 34, y);
        valueStyle(g, 10.5);
        g.fillStyle(state > 0 ? colour : Theme::textMuted);
        g.textAlign(TextAlign::Right);
    }

    void drawHealth(Canvas& g) {
        panel(g, ex, ey, ew, eh);
        heading(g, "Health", ex + 16, ey + 20);
        aside(g, accumulate.on() ? "since play" : "last 60 s", ex + ew - 16, ey + 20,
              TextAlign::Right);
        const bool any = samplePeak.value() > -69.95;
        const double vx0 = ex + ew - 16;
        const double tp = truePeak.value();
        check(g, 0, !any ? -1 : (tp > 0.0 ? 2 : (tp > -1.0 ? 1 : 0)), "True peak");
        if (tp <= -69.95)
            g.fillText("–", vx0, ey + 46);
        else
            g.fillText(format("%.1f dBTP", tp), vx0, ey + 46);
        check(g, 1, !any ? -1 : (overs.value() > 0.0 ? 2 : 0), "Overs above 0 dBTP");
        g.fillText(format("%.0f", overs.value()), vx0, ey + 66);
        check(g, 2, !any ? -1 : (clipped.value() > 0.0 ? 2 : 0), "Clipped runs");
        g.fillText(format("%.0f", clipped.value()), vx0, ey + 86);
        const double dc = dcOffset.value();
        check(g, 3, !any ? -1 : (dc > -40.0 ? 2 : (dc > -60.0 ? 1 : 0)), "DC offset");
        if (dc <= -119.5)
            g.fillText("none", vx0, ey + 106);
        else
            g.fillText(format("%.0f dB", dc), vx0, ey + 106);
        const double corr = correlation.value();
        const bool stereoSeen = any && (width.value() > 0.0 || corr < 0.999);
        check(g, 4, !stereoSeen ? -1 : (corr < 0.0 ? 2 : (corr < 0.2 ? 1 : 0)), "Phase");
        g.fillText(format("%+.2f", corr), vx0, ey + 126);
        const double loss = monoLoss.value();
        check(g, 5, !stereoSeen ? -1 : (loss > 3.0 ? 2 : (loss > 1.5 ? 1 : 0)), "Mono fold-down");
        if (abs(loss) < 0.05)
            g.fillText("0.0 LU", vx0, ey + 146);
        else
            g.fillText(format("%.1f LU", -loss), vx0, ey + 146);
        const double lowW = lowWidth.value();
        check(g, 6, !stereoSeen ? -1 : (lowW > 0.4 ? 2 : (lowW > 0.25 ? 1 : 0)), "Bass width");
        g.fillText(format("%.0f %%", 100.0 * lowW), vx0, ey + 166);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawLoudness(g);
        drawHistory(g);
        drawSpectrum(g);
        drawStereo(g);
        drawHarmony(g);
        drawHealth(g);
    }

    // --- the pointer
    // ----------------------------------------------------------------------------------

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Press) pressed(p);
        if (a == PointerAction::Drag && dragging == 1)
            target.edit(target - p.dy() * (p.shift() ? 0.02 : 0.1));
        if (a == PointerAction::Release) dragging = 0;
        if (a == PointerAction::Wheel && p.over(lx + lw - 84, chipY - 4, 70, chipH + 8))
            target.edit(target + p.wheel() * (p.shift() ? 0.1 : 0.5));
    }

    void pressed(const Pointer& p) {
        if (onClock(p.x(), p.y())) {
            accumulate.edit(accumulate.on() ? 0.0 : 1.0);
            return;
        }
        if (p.over(restartX, pillY, 120, pillH)) {
            restartOnPlay.edit(restartOnPlay.on() ? 0.0 : 1.0);
            return;
        }
        if (p.over(resetX, pillY, 64, pillH)) {
            reset.edit(reset.on() ? 0.0 : 1.0);
            return;
        }
        for (int k = 0; k < 3; ++k) {
            const double x = slopeX + 52 * k;
            if (p.over(x, slopeY, k == 2 ? 56 : 46, 18)) {
                slope.edit(k);
                return;
            }
        }
        for (int k = 0; k < 4; ++k) {
            if (p.over(chipX + k * (chipW + 6), chipY, chipW, chipH)) {
                target.edit(chipValue(k));
                return;
            }
        }
        if (p.over(lx + lw - 84, chipY - 4, 70, chipH + 8)) {
            if (p.doubleClick())
                target.reset();
            else
                dragging = 1;
        }
    }
};
