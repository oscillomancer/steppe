// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Gorse: a transient shaper.
//
// Attack raises or softens each hit's onset, Sustain its tail, and neither
// cares how loud the hit is: the detector works in decibels, so a hit 20 dB
// quieter is shaped the same.
//
//   Detector  the louder channel, through a peak hold as long as a cycle of
//             what is playing (1 to 50 ms), so a held tone's waveform never
//             reads as onsets. Focus can make it listen to one band (the
//             kick's lows, the snare's mids, the hats' highs) while the
//             whole signal is shaped.
//   Onset     two followers in dB, a fast one and a slow one, both scaled
//             by Length: while the fast one leads, the hit is starting;
//             while it trails, the hit is decaying. Sensitivity says how
//             far apart they must be for the full amount: 10 dB at 50%.
//   Gain      Attack and Sustain take turns (only one acts at a time), so
//             the change is never more than 12 dB. The gain arrives along a
//             1 ms ramp that the lookahead lets land exactly on the onset:
//             a cut takes the very first click too, and a boost has no
//             corner to click.
//   Clip      last of all, after mix and output: transparent below -6 dBFS,
//             then a smooth bend that never passes 0 dBFS, at four times
//             the sample rate. It can be switched while playing.
//
// The hit view lays the last hits over each other at their onsets and
// shows, on a time axis that opens out the first milliseconds, what came in
// and what goes out: the onset's change in gorse yellow, the tail's in
// moss. Until hits play, it shows a model hit through the same law. The
// history scrolls four seconds with the gain the shaping applied, and marks
// each hit the detector found.

// --- the hit view's time axis -----------------------------------------------------------

constexpr int kBins = 256;          // points across the hit view
constexpr int kPre = 16;            // of them before the onset: the lookahead
constexpr double kLookMs = 1.0;     // the lookahead
constexpr double kSpanMs = 500.0;   // how long after the onset the view runs
constexpr double kTrigger = 0.5;    // a hit, for the view: the onset half of full
constexpr double kRearm = 0.1;      // and under this before the next
constexpr double kHitWeight = 0.3;  // each new hit's share of the average
constexpr double kSpread = 30.0;    // dB the slow follower may trail the fast one
constexpr double kTail = 3.0;       // how much more readily a fall counts than a rise

// The time (ms from the onset) where point k starts, 0 <= k <= kBins: the
// lookahead evenly, then a square law that gives the first milliseconds room.
double binTime(double k) {
    if (k < kPre) return -kLookMs * (kPre - k) / kPre;
    const double u = (k - kPre) / (kBins - kPre);
    return kSpanMs * u * u;
}

// Where a time falls on that axis, as a fractional point.
double binAt(double ms) {
    if (ms < 0.0) return kPre * (1.0 + max(ms, -kLookMs) / kLookMs);
    return kPre + (kBins - kPre) * sqrt(min(ms, kSpanMs) / kSpanMs);
}

// --- the gain path -----------------------------------------------------------------------

// Every delay here is a fixed whole number of samples. A ring reads that
// sample directly, without the fractional delay's unused interpolation.
struct SampleDelay {
    Buffer<double> ring;
    int length = 1;
    int at = 0;

    void prepare(int samples) {
        length = samples + 1;
        ring.resize(length);
        ring.fill(0.0);
        at = 0;
    }
    double step(double x) {
        ring[at] = x;
        at = at + 1 == length ? 0 : at + 1;
        return ring[at];
    }
};

// Reuse the exact conversion while its input stays still, including after
// a smoothed parameter arrives. A moving value is converted every sample.
struct CachedGain {
    double db = 0.0;
    double gain = 1.0;

    void clear() {
        db = 0.0;
        gain = 1.0;
    }
    double step(double value) {
        if (value != db) {
            db = value;
            gain = dbToGain(value);
        }
        return gain;
    }
};

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

// The largest of the last `hold` values, for a hold that changes as it
// goes: a monotonic queue, its times and values in rings.
struct PeakHold {
    Buffer<double> value;
    Buffer<int> time;
    int size = 1, head = 0, tail = 0, clock = 0;

    void prepare(int n) {
        size = max(2, n);
        value.resize(size);
        time.resize(size);
        clear();
    }
    void clear() {
        value.fill(0.0);
        time.fill(0);
        head = 0;
        tail = 0;
        clock = 0;
    }
    double push(double x, int hold) {
        while (head != tail && clock - time[head] >= hold) head = head + 1 == size ? 0 : head + 1;
        while (head != tail) {
            const int back = tail == 0 ? size - 1 : tail - 1;
            if (value[back] > x) break;
            tail = back;
        }
        value[tail] = x;
        time[tail] = clock;
        tail = tail + 1 == size ? 0 : tail + 1;
        clock += 1;
        return value[head];
    }
};

// The soft clip: straight to the knee, then a tanh bend towards 1.
constexpr double kKnee = 0.5;  // -6 dBFS
double bend(double x) {
    const double a = abs(x);
    if (a <= kKnee) return x;
    const double y = kKnee + (1.0 - kKnee) * tanh((a - kKnee) / (1.0 - kKnee));
    return x < 0.0 ? -y : y;
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
constexpr int kGorse = 0xE9B949;  // the onset: the flowers
constexpr int kMoss = 0x9DB36E;   // the tail: the bush
constexpr int kStem = 0x5F7A5A;

constexpr int kCols = 1024;  // history columns, 5 ms each
constexpr int kShown = 800;  // four seconds across the graph

struct Gorse : Effect {
    static constexpr Info info = {
        .name = "Gorse",
        .id = "com.suppleaudio.gorse",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Transient shaper: raises or softens each hit's onset and its tail, the same at "
            "any level, with a detector that can listen to one band, and a soft clip.",
        .features = {"transient-shaper"}};

    Param attack =
        decibels("Attack", -12, 12, 0)
            .modulated()
            .smoothed(20)
            .in("Shape")
            .help("How much each hit's onset is raised (or softened), in dB, at a clear onset.");
    Param sustain =
        decibels("Sustain", -12, 12, 0)
            .modulated()
            .smoothed(20)
            .in("Shape")
            .help("How much each hit's tail is raised (or shortened) as it decays, in dB.");
    Param length =
        milliseconds("Length", 1, 100, 10)
            .modulated()
            .smoothed(20)
            .in("Detector")
            .help("How long an onset lasts before the tail begins; slower decays need more.");
    Param sensitivity =
        percent("Sensitivity", 50)
            .modulated()
            .smoothed(20)
            .in("Detector")
            .help(
                "How readily it hears onsets and tails: at 50% a rise of 10 dB is a full onset, "
                "and a fall a third as steep a full tail.");
    Param focus =
        choice("Focus", {"Full", "Low", "Mid", "High"}, 0)
            .keys({"full", "low", "mid", "high"})
            .notAutomatable()
            .in("Detector")
            .help(
                "What the detector listens to: everything, the lows (below 150 Hz), the mids "
                "(250 Hz to 4 kHz) or the highs (above 5 kHz). The whole signal is shaped.");
    Param clip =
        toggle("Clip", false)
            .in("Output")
            .help(
                "A soft clip, last of all: nothing under -6 dBFS changes, and nothing passes 0 "
                "dBFS.");
    Param mix = percent("Mix", 100)
                    .modulated()
                    .smoothed(20)
                    .in("Output")
                    .help("The shaped signal against the dry one, in time with it.");
    Param output = decibels("Output", -24, 24, 0)
                       .modulated()
                       .smoothed(20)
                       .in("Output")
                       .help("The level out, before the clip.");

    Param gainChange =
        decibels("Gain Change", -12, 12, 0).meter().help("The gain the shaping applies now.");
    Param onsetChange =
        decibels("Onset Change", -12, 12, 0)
            .meter()
            .help("How much the recent hits' onsets were raised or softened, at the most, in dB.");
    Param tailChange =
        decibels("Tail Change", -12, 12, 0)
            .meter()
            .help("How much the recent hits' tails were raised or lowered, on average, in dB.");

    void factoryPatches(Patches& p) {
        p.add("Punch").set(attack, 6).set(sustain, -2);
        p.add("Kick Thump").set(attack, 6).set(length, 15).set(focus, "low");
        p.add("Snare Crack").set(attack, 7).set(sustain, -3).set(length, 6).set(focus, "mid");
        p.add("Tighten").set(sustain, -9).set(length, 12).set(sensitivity, 60);
        p.add("Soften Attack").set(attack, -6).set(length, 5);
        p.add("Room Bloom").set(attack, -2).set(sustain, 6).set(length, 30).set(sensitivity, 70);
        p.add("Drum Bus Snap").set(attack, 4).set(sustain, -2).set(mix, 60).set(clip, true);
    }

    // --- audio ------------------------------------------------------------------------------

    double rate = 48000.0;
    int look = 48;      // the lookahead, samples
    int overDelay = 0;  // the clip's oversampler's delay
    int minHold = 48, maxHold = 2400, hold = 48;
    PeakHold peaks;
    Box rampA, rampB;
    Array<Biquad, 2> keyA, keyB;
    Biquad lowA, lowB;
    double lowPower = 0.0, allPower = 0.0, kPower = 0.0, lowPeriod = 0.0;
    int sinceCross = 0, lowHold = 0, lengthHold = 0;
    bool lowArmed = false;
    Array<SampleDelay, 2> delay, plain;
    SampleDelay partDelay;
    Array<Oversampler, 2> over;
    int focusNow = 0;
    double previous = 0.0, previous2 = 0.0;
    int sinceMax = 0;
    bool haveMax = false;
    double cycleTop = 0.0, lowSince = 0.0;
    bool started = false;
    double reference = 0.0, fast = 0.0, slow = 0.0;
    double detectorPeak = 0.0, detectorLevel = -120.0;
    CachedGain shapeGain, outputGain;
    double lengthNow = 0.0, fastAttack = 0.0, fastRelease = 0.0, slowRelease = 0.0;
    double clipMix = 0.0, clipStep = 0.001;
    bool unplayed = true;

    // The hits, for the view and the meters.
    Array<int, 257> edge;  // where each point starts, in samples from a capture's start
    bool armed = true, capturing = false;
    int sinceHit = 0, minGap = 960;
    int capAt = 0, capBin = 0, binCount = 0;
    double binIn = 0.0, binOut = 0.0, binPart = 0.0;
    Array<double, 256> capIn, capOut, capPart;
    Array<double, 256> avgIn, avgOut, avgPart, avgSeen;
    int hits = 0;
    bool hitsChanged = false;
    double onsetNow = 0.0, tailNow = 0.0;

    // The history and the header's meters.
    double colLength = 240.0, colAt = 0.0;
    double colIn = 0.0, colOut = 0.0, colGain = 0.0, colPart = 0.0;
    int colHit = 0, colBeat = 0, hw = 0, filled = 0;
    double beatPos = 0.0, beatNext = -1.0, fall = 0.999;
    bool wasPlaying = false;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0;
    double envIn = 0.0, envOut = 0.0, envFall = 0.999;

    SharedArray<float, 1024> hIn, hOut, hGain, hPart, hHit, hBeat;
    SharedArray<float, 256> hitIn, hitOut, hitPart, hitSeen;
    Shared<int> hHead, hFilled, hitsShown;
    Shared<double> inLeft, inRight, outLeft, outRight, onsetShown, tailShown;

    void prepare() {
        rate = sampleRate();
        look = max(2, roundToInt(kLookMs * 0.001 * rate));
        // Two boxes whose ramp spans the lookahead: a step in the gain
        // asked for has fully arrived when the sample that asked plays.
        const int a = idiv(look, 2) + 1;
        rampA.prepare(a);
        rampB.prepare(look + 1 - a);
        minHold = max(1, roundToInt(0.001 * rate));
        maxHold = max(minHold, roundToInt(0.05 * rate));
        hold = minHold;
        peaks.prepare(maxHold + 2);
        lowA.lowpass(200.0, 0.707);
        lowB.lowpass(200.0, 0.707);
        lowA.clear();
        lowB.clear();
        kPower = 1.0 - exp(-1.0 / (0.02 * rate));
        lowPower = 0.0;
        allPower = 0.0;
        lowPeriod = 0.0;
        sinceCross = 0;
        lowHold = 0;
        lowArmed = false;
        for (int c = 0; c < 2; ++c) {
            over[c].prepare(4);
            over[c].clear();
            delay[c].prepare(look);
            keyA[c].clear();
            keyB[c].clear();
        }
        overDelay = over[0].latency();
        for (int c = 0; c < 2; ++c) {
            plain[c].prepare(overDelay);
        }
        partDelay.prepare(look);
        setLatency(look + overDelay);
        setFocus(focus.index());
        clipStep = 1.0 / (0.01 * rate);
        clipMix = clip.on() ? 1.0 : 0.0;
        // A restart prepares an instance that has played: start it afresh.
        previous = 0.0;
        previous2 = 0.0;
        sinceMax = 0;
        haveMax = false;
        cycleTop = 0.0;
        lowSince = 0.0;
        started = false;
        reference = 0.0;
        fast = 0.0;
        slow = 0.0;
        detectorPeak = 0.0;
        detectorLevel = gainToDb(0.0);
        shapeGain.clear();
        outputGain.clear();
        lengthNow = 0.0;
        setLength(length);
        for (int k = 0; k <= kBins; ++k) {
            const double t = binTime(k);
            edge[k] = k <= kPre ? idiv(look * k, kPre) : look + roundToInt(t * 0.001 * rate);
        }
        minGap = roundToInt(0.02 * rate);
        armed = true;
        capturing = false;
        sinceHit = minGap;
        capAt = 0;
        capBin = 0;
        binCount = 0;
        binIn = 0.0;
        binOut = 0.0;
        binPart = 0.0;
        capIn.fill(0.0);
        capOut.fill(0.0);
        capPart.fill(0.0);
        avgIn.fill(0.0);
        avgOut.fill(0.0);
        avgPart.fill(0.0);
        avgSeen.fill(0.0);
        hits = 0;
        hitsChanged = true;
        onsetNow = 0.0;
        tailNow = 0.0;
        colLength = 0.005 * rate;
        colAt = 0.0;
        colIn = 0.0;
        colOut = 0.0;
        colGain = 0.0;
        colPart = 0.0;
        colHit = 0;
        colBeat = 0;
        wasPlaying = false;
        fall = exp(-1.0 / (0.3 * rate));
        envFall = exp(-1.0 / (0.03 * rate));
        envIn = 0.0;
        envOut = 0.0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        unplayed = true;
    }

    void setLength(double ms) {
        lengthNow = ms;
        lengthHold = min(maxHold, roundToInt(ms * 0.001 * rate));
        fastAttack = exp(-1.0 / (rate * ms * 0.00001));
        fastRelease = exp(-1.0 / (rate * ms * 0.001));
        slowRelease = exp(-1.0 / (rate * ms * 0.004));
    }

    void setFocus(int f) {
        focusNow = f;
        for (int c = 0; c < 2; ++c) {
            if (f == 1) {
                keyA[c].lowpass(150.0, 0.707);
                keyB[c].lowpass(150.0, 0.707);
            } else if (f == 2) {
                keyA[c].highpass(250.0, 0.707);
                keyB[c].lowpass(4000.0, 0.707);
            } else if (f == 3) {
                keyA[c].highpass(5000.0, 0.707);
                keyB[c].highpass(5000.0, 0.707);
            }
            keyA[c].clear();
            keyB[c].clear();
        }
    }

    void update() {
        if (focus.index() != focusNow) setFocus(focus.index());
        if (unplayed) clipMix = clip.on() ? 1.0 : 0.0;
    }

    // The detector's level: the peak held over a cycle of what plays, 1 to
    // 50 ms. A cycle is the span between two crests with a dip below half
    // of each between them: the dips of a low carrier, not the ripple of
    // noise or of a quantized waveform near its crest, nor a hit's own
    // decay, any of which would pull the hold short and let a bass line's
    // cycles through as onsets.
    double detect(double x) {
        sinceMax = min(sinceMax + 1, maxHold);
        lowSince = min(lowSince, x);
        if (previous >= previous2 && previous > x) {
            if (!haveMax || lowSince < 0.5 * min(cycleTop, previous)) {
                if (haveMax) hold = clamp(sinceMax + idiv(sinceMax, 8), minHold, maxHold);
                haveMax = true;
                cycleTop = previous;
                sinceMax = 0;
                lowSince = x;
            } else if (previous > cycleTop) {
                // The same crest, higher: measure from here.
                cycleTop = previous;
                sinceMax = 0;
                lowSince = x;
            }
        }
        previous2 = previous;
        previous = x;
        return peaks.push(x, max(max(hold, lowHold), lengthHold));
    }

    // Under a low band that leads the key (a bass line, a kick's body), the
    // hold spans a whole cycle of it: ripple riding on it (a pad, a voice)
    // makes crests of its own, but the dips that matter are the low band's,
    // and a saw or a pulse dips once a cycle, not twice. Its period comes
    // from its rising crossings, with some hysteresis.
    void listenLow(double mono) {
        const double low = lowB.process(lowA.process(mono));
        lowPower += kPower * (low * low - lowPower);
        allPower += kPower * (mono * mono - allPower);
        const double edge = 0.3 * sqrt(lowPower);
        sinceCross = min(sinceCross + 1, maxHold * 4);
        if (low < -edge) lowArmed = true;
        if (lowArmed && low > edge) {
            lowArmed = false;
            if (sinceCross >= rate / 400.0 && sinceCross <= rate / 20.0)
                lowPeriod =
                    lowPeriod <= 0.0 ? sinceCross : lowPeriod + 0.35 * (sinceCross - lowPeriod);
            sinceCross = 0;
        }
        const bool leads = lowPeriod > 0.0 && sinceCross <= rate / 20.0 &&
                           lowPower > 0.2 * allPower && lowPower > 1e-12;
        lowHold = leads ? min(maxHold, roundToInt(1.125 * lowPeriod)) : 0;
    }

    // How far the hit has started (above 0) or decayed (below), -1 to 1,
    // from the followers.
    double onsetness(double peak, double sens) {
        if (peak != detectorPeak) {
            detectorPeak = peak;
            detectorLevel = gainToDb(peak);
        }
        const double level = detectorLevel;
        if (!started) {
            if (peak <= 0.0) return 0.0;
            reference = level;
            fast = level - 60.0;
            slow = level - 60.0;
            started = true;
        }
        reference = max(reference, level);
        const double l = max(level, reference - 120.0);
        const double fa = l > fast ? fastAttack : fastRelease;
        const double sa = l > slow ? fastRelease : slowRelease;
        fast = fa * fast + (1.0 - fa) * l;
        slow = max(sa * slow + (1.0 - sa) * l, fast - kSpread);
        const double d = (fast - slow) * sens / 5.0;
        return clamp(d > 0.0 ? d : kTail * d, -1.0, 1.0);
    }

    void process(Block& io) {
        unplayed = false;
        const Transport& t = io.transport;
        const bool stereo = io.channels > 1;
        const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        const double beatStep = t.tempo / 60.0 / rate;
        beatPos = t.beats;
        double prevPos =
            wasPlaying && abs(beatPos - beatNext) < 1e-3 ? beatPos - beatStep : beatPos;
        const bool clipOn = clip.on();
        double extreme = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            double l = io[0][i];
            double r = stereo ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;

            // The detector, on the louder channel of the key.
            double kl = l, kr = r;
            if (focusNow > 0) {
                kl = keyB[0].process(keyA[0].process(l));
                kr = stereo ? keyB[1].process(keyA[1].process(r)) : kl;
            }
            listenLow(0.5 * (kl + kr));
            const double peak = detect(max(abs(kl), abs(kr)));
            if (length.at(i) != lengthNow) setLength(length.at(i));
            const double o = onsetness(peak, sensitivity.at(i));
            const double want = o > 0.0 ? attack.at(i) * o : -sustain.at(i) * o;
            const double gainDb = rampB.step(rampA.step(want));
            const double part = partDelay.step(o);

            // A hit, for the view: an onset the shaping treats as half of
            // one or more (so what the view shows follows sensitivity).
            if (armed && started && o >= kTrigger && sinceHit >= minGap) {
                startHit();
                armed = false;
                sinceHit = 0;
                colHit = 1;
            }
            if (o < kRearm) armed = true;
            sinceHit = min(sinceHit + 1, minGap);

            // The audio, delayed to meet its gain; mixed, then the clip.
            const double dl = delay[0].step(l);
            const double dr = delay[1].step(r);
            const double gain = shapeGain.step(gainDb);
            const double m = mix.at(i), out = outputGain.step(output.at(i));
            const double ml = dl + m * (dl * gain - dl);
            const double mr = dr + m * (dr * gain - dr);
            const double sl = ml * out, sr = mr * out;
            double yl = plain[0].step(sl);
            double yr = plain[1].step(sr);
            // The clip fades in and out over 10 ms; switched off, it rests,
            // and starts afresh as it fades back in.
            if (clipOn && clipMix == 0.0) {
                over[0].clear();
                over[1].clear();
            }
            clipMix = clamp(clipMix + (clipOn ? clipStep : -clipStep), 0.0, 1.0);
            if (clipMix > 0.0) {
                const double cl = clipped(0, sl);
                const double cr = clipped(1, sr);
                yl = yl + clipMix * (cl - yl);
                yr = yr + clipMix * (cr - yr);
            }
            io[0][i] = yl;
            if (stereo) io[1][i] = yr;

            // What the view and the history see.
            const double aIn = max(abs(dl), abs(dr)), aOut = max(abs(ml), abs(mr));
            if (capturing) captureStep(aIn, aOut, part);
            if (abs(gainDb) > abs(extreme)) extreme = gainDb;
            if (abs(gainDb) > abs(colGain)) {
                colGain = gainDb;
                colPart = part;
            }
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(yl), peakOut0 * fall);
            peakOut1 = max(abs(yr), peakOut1 * fall);
            envIn = max(aIn, envIn * envFall);
            envOut = max(max(abs(yl), abs(yr)), envOut * envFall);
            colIn = max(colIn, envIn);
            colOut = max(colOut, envOut);
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
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(stereo ? peakIn1 : peakIn0));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(stereo ? peakOut1 : peakOut0));
        if (hitsChanged) {
            for (int k = 0; k < kBins; ++k) {
                hitIn[k] = static_cast<float>(avgIn[k]);
                hitOut[k] = static_cast<float>(avgOut[k]);
                hitPart[k] = static_cast<float>(avgPart[k]);
                hitSeen[k] = static_cast<float>(avgSeen[k]);
            }
            hitsShown.set(hits);
            onsetShown.set(onsetNow);
            tailShown.set(tailNow);
            hitsChanged = false;
        }
        gainChange.set(extreme);
        onsetChange.set(onsetNow);
        tailChange.set(tailNow);
    }

    // The clip, at four times the rate. Coming down, the filters can ring
    // past full scale on a hard-driven, noisy hit (a snare raised 20 dB):
    // those samples, and only those, are held at it.
    double clipped(int c, double x) {
        over[c].up(x);
        for (int k = 0; k < 4; ++k) over[c].set(k, bend(over[c].sample(k)));
        return clamp(over[c].down(), -1.0, 1.0);
    }

    // --- the hits --------------------------------------------------------------------------

    // A hit: the one being captured ends here, and a new capture begins
    // with the lookahead before its onset.
    void startHit() {
        if (capturing) finishHit(capBin);
        capturing = true;
        capAt = 0;
        capBin = 0;
        binCount = 0;
        binIn = 0.0;
        binOut = 0.0;
        binPart = 0.0;
    }

    void captureStep(double aIn, double aOut, double part) {
        while (capBin < kBins && capAt >= edge[capBin + 1]) closeBin();
        if (capBin >= kBins) {
            finishHit(kBins);
            capturing = false;
            return;
        }
        binIn = max(binIn, aIn);
        binOut = max(binOut, aOut);
        binPart += part;
        binCount += 1;
        capAt += 1;
    }

    // A point's peaks, or (a point shorter than a sample) the last point's.
    void closeBin() {
        if (binCount > 0) {
            capIn[capBin] = binIn;
            capOut[capBin] = binOut;
            capPart[capBin] = binPart / binCount;
        } else {
            const int before = max(0, capBin - 1);
            capIn[capBin] = capIn[before];
            capOut[capBin] = capOut[before];
            capPart[capBin] = capPart[before];
        }
        capBin += 1;
        binCount = 0;
        binIn = 0.0;
        binOut = 0.0;
        binPart = 0.0;
    }

    // Into the average: the captured points, in dB from the hit's peak.
    void finishHit(int n) {
        if (n < roundToInt(binAt(10.0))) return;  // a double trigger, not a hit
        const int peakEnd = min(n, roundToInt(binAt(30.0)));
        double top = 0.0;
        for (int k = 0; k < peakEnd; ++k) top = max(top, capIn[k]);
        if (top < 1e-6) return;
        const double ref = gainToDb(top);
        for (int k = 0; k < kBins; ++k) {
            if (k < n) {
                const double vi = max(-90.0, gainToDb(capIn[k]) - ref);
                const double vo = max(-90.0, gainToDb(capOut[k]) - ref);
                if (avgSeen[k] < 0.01) {
                    avgIn[k] = vi;
                    avgOut[k] = vo;
                    avgPart[k] = capPart[k];
                } else {
                    avgIn[k] += kHitWeight * (vi - avgIn[k]);
                    avgOut[k] += kHitWeight * (vo - avgOut[k]);
                    avgPart[k] += kHitWeight * (capPart[k] - avgPart[k]);
                }
                avgSeen[k] += kHitWeight * (1.0 - avgSeen[k]);
            } else {
                avgSeen[k] *= 1.0 - kHitWeight;
            }
        }
        hits += 1;
        hitsChanged = true;
        measureHits();
    }

    // The meters: the onset's largest change, the tail's mean change (to
    // 300 ms), where the hits reach.
    void measureHits() {
        const int end = roundToInt(binAt(300.0));
        double best = 0.0, sum = 0.0, weight = 0.0;
        for (int k = 0; k < end; ++k) {
            if (avgSeen[k] < 0.3) continue;
            const double change = avgOut[k] - avgIn[k];
            if (avgPart[k] >= 0.0) {
                if (abs(change) > abs(best)) best = change;
            } else {
                sum += avgSeen[k] * change;
                weight += avgSeen[k];
            }
        }
        onsetNow = clamp(best, -12.0, 12.0);
        tailNow = weight > 0.0 ? clamp(sum / weight, -12.0, 12.0) : 0.0;
    }

    void closeColumn() {
        colAt -= colLength;
        hIn[hw] = static_cast<float>(gainToDb(colIn));
        hOut[hw] = static_cast<float>(gainToDb(colOut));
        hGain[hw] = static_cast<float>(colGain);
        hPart[hw] = static_cast<float>(colPart);
        hHit[hw] = static_cast<float>(colHit);
        hBeat[hw] = static_cast<float>(colBeat);
        hw = hw == kCols - 1 ? 0 : hw + 1;
        filled = min(filled + 1, kCols);
        colIn = 0.0;
        colOut = 0.0;
        colGain = 0.0;
        colPart = 0.0;
        colHit = 0;
        colBeat = 0;
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1000;
    static constexpr double kH = 700;
    static constexpr double vx = 16;  // the hit view
    static constexpr double vy = 60;
    static constexpr double vw = 600;
    static constexpr double vh = 352;
    static constexpr double px = 28;  // its plot
    static constexpr double py = 96;
    static constexpr double pw = 548;
    static constexpr double ph = 280;
    static constexpr double kHitTop = 12.0;     // dB above the hit's peak
    static constexpr double kHitFloor = -48.0;  // and below
    static constexpr double sx = 632;           // the shape panel
    static constexpr double sw = 352;
    static constexpr double hx = 16;  // the history
    static constexpr double hy = 424;
    static constexpr double hwid = 600;
    static constexpr double hh = 260;
    static constexpr double gx = 28;  // its level graph
    static constexpr double gy = 456;
    static constexpr double gw = 548;
    static constexpr double gh = 136;
    static constexpr double ly = 600;  // its gain lane
    static constexpr double lh = 72;
    static constexpr double kTop = 6.0;      // the graph's top, dBFS
    static constexpr double kFloor = -42.0;  // its floor
    static constexpr double ox = 632;        // the output panel
    static constexpr double ow = 352;
    static constexpr double focusY = 356;      // the focus pills
    static constexpr double clipX = ox + 282;  // the clip's centre

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 Length's handle, 3 the onset, 4 the tail
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    // The model hit, while no hits have played: worked out again when a
    // setting it depends on changes.
    Array<double, 256> modelIn, modelOut, modelPart;
    Array<double, 256> shownIn, shownOut, shownPart;
    Array<double, 4> modelKey = {-99.0, -99.0, -99.0, -99.0};
    static constexpr int kModelSteps = 8040;  // 16 kHz, a lookahead and 501 ms
    Array<double, 8040> modelGain, modelOnsetness;
    double modelOnset = 0.0, modelTail = 0.0;

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
        g.beginPath();
        if (abs(at - from) > 0.001) {
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

    void valueStyle(Canvas& g, double size) {
        g.fillStyle(Theme::text);
        g.monoFont(size);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
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
        else
            g.fillText(format("%.0f ms", ms), x, y);
    }

    // The mark: a sprig of gorse, its spines out and its flowers open.
    void drawMark(Canvas& g, double x, double y) {
        g.strokeStyle(kStem);
        g.lineCap(LineCap::Round);
        g.lineWidth(1.6);
        g.beginPath();
        g.moveTo(x - 9, y + 14);
        g.quadraticCurveTo(x - 2, y + 2, x + 7, y - 13);
        g.stroke();
        g.lineWidth(1.0);
        for (int k = 0; k < 7; ++k) {
            const double u = 0.12 + k * 0.13;
            const double bx = x - 9 + 16 * u, by = y + 14 - 27 * u;
            const double side = imod(k, 2) == 0 ? -1.0 : 1.0;
            g.beginPath();
            g.moveTo(bx, by);
            g.lineTo(bx + side * 5.5, by - 2.5 + side * 1.5);
            g.stroke();
        }
        Array<double, 5> fx = {-4.0, 3.5, -1.0, 7.0, 1.5};
        Array<double, 5> fy = {4.5, 0.0, -6.0, -8.5, -13.0};
        for (int k = 0; k < 5; ++k) {
            g.fillStyle(kGorse);
            g.beginPath();
            g.ellipse(x + fx[k], y + fy[k], 3.0, 2.3);
            g.fill();
            g.fillStyle(0xF6D98A);
            g.beginPath();
            g.circle(x + fx[k] - 0.8, y + fy[k] - 0.6, 1.0);
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
        drawMark(g, 31, 30);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Gorse", 52, 37);
        const double tw = g.measureText("Gorse");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("transient shaper", 60 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 620, 23, 140, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 824, 23, 140, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the hit view -------------------------------------------------------------------------

    double hitX(double k) const { return px + pw * k / (kBins - 1.0); }
    double hitY(double db) const {
        return py + ph * clamp((kHitTop - db) / (kHitTop - kHitFloor), 0.0, 1.0);
    }
    double lengthX() const { return hitX(binAt(length)); }
    double timeAtX(double x) const {
        const double k = clamp((x - px) / pw, 0.0, 1.0) * (kBins - 1.0);
        return binTime(k);
    }

    // The model hit: a sharp onset decaying 100 dB a second, through the
    // followers and the law at 16 kHz, its gain landing as the lookahead
    // lands it.
    void refreshModel() {
        const double a = attack, s = sustain, len = length, sens = sensitivity;
        if (modelKey[0] == a && modelKey[1] == s && modelKey[2] == len && modelKey[3] == sens)
            return;
        modelKey[0] = a;
        modelKey[1] = s;
        modelKey[2] = len;
        modelKey[3] = sens;
        const double steps = 16.0;  // a millisecond
        const double fa = exp(-1.0 / (steps * len * 0.01));
        const double fr = exp(-1.0 / (steps * len));
        const double sr = exp(-1.0 / (steps * len * 4.0));
        // At step n the detector hears the hit n steps after its onset; the
        // audio then playing is a lookahead (16 steps) older.
        double f = -60.0, sl = -60.0, sumA = 0.0, sumB = 0.0;
        Array<double, 9> boxA;
        Array<double, 8> boxB;
        boxA.fill(0.0);
        boxB.fill(0.0);
        int atA = 0, atB = 0;
        for (int n = 0; n < kModelSteps; ++n) {
            const double l = max(-60.0, -0.1 * n / steps);
            const double ka = l > f ? fa : fr;
            const double kb = l > sl ? fr : sr;
            f = ka * f + (1.0 - ka) * l;
            sl = max(kb * sl + (1.0 - kb) * l, f - kSpread);
            const double d = (f - sl) * sens / 5.0;
            const double o = clamp(d > 0.0 ? d : kTail * d, -1.0, 1.0);
            const double want = o > 0.0 ? a * o : -s * o;
            sumA += want - boxA[atA];
            boxA[atA] = want;
            atA = atA == 8 ? 0 : atA + 1;
            const double ga = sumA / 9.0;
            sumB += ga - boxB[atB];
            boxB[atB] = ga;
            atB = atB == 7 ? 0 : atB + 1;
            modelGain[n] = sumB / 8.0;
            modelOnsetness[n] = o;
        }
        for (int k = 0; k < kBins; ++k) {
            const double t = 0.5 * (binTime(k) + binTime(k + 1));
            const int n = clamp(roundToInt(16.0 + steps * t), 0, kModelSteps - 1);
            modelIn[k] = t < 0.0 ? -90.0 : max(-60.0, -0.1 * t);
            modelOut[k] = modelIn[k] + modelGain[n];
            modelPart[k] =
                t < 0.0 ? 0.0 : modelOnsetness[clamp(roundToInt(steps * t), 0, kModelSteps - 1)];
        }
        double best = 0.0, sum = 0.0;
        int count = 0;
        const int end = roundToInt(binAt(300.0));
        for (int k = 0; k < end; ++k) {
            const double change = modelOut[k] - modelIn[k];
            if (modelPart[k] >= 0.0) {
                if (abs(change) > abs(best)) best = change;
            } else {
                sum += change;
                count += 1;
            }
        }
        modelOnset = best;
        modelTail = count > 0 ? sum / count : 0.0;
    }

    // What the view draws: the hits (or the model), eased over a few
    // points, which near the onset are a fraction of a millisecond and out
    // in the tail a few: the waveform's own ripple goes, the shape stays.
    void ease(bool model, int n) {
        for (int k = 0; k < kBins; ++k) {
            double si = 0.0, so = 0.0, sp = 0.0, w = 0.0;
            for (int j = -3; j <= 3; ++j) {
                const int q = k + j;
                if (q < 0 || q >= n) continue;
                // Not across the onset: the silence before it stays silent.
                if ((q < kPre) != (k < kPre)) continue;
                const double wj = 4.0 - abs(j);
                si += wj * (model ? modelIn[q] : hitIn[q]);
                so += wj * (model ? modelOut[q] : hitOut[q]);
                sp += wj * (model ? modelPart[q] : hitPart[q]);
                w += wj;
            }
            shownIn[k] = w > 0.0 ? si / w : -90.0;
            shownOut[k] = w > 0.0 ? so / w : -90.0;
            shownPart[k] = w > 0.0 ? sp / w : 0.0;
        }
    }

    void drawHitView(Canvas& g) {
        panel(g, vx, vy, vw, vh);
        const bool model = hitsShown.get() == 0;
        if (model) refreshModel();
        heading(g, "The hit", vx + 16, vy + 20);
        aside(g,
              model ? "a model hit, until hits play"
                    : "the last hits, laid over each other at their onsets",
              vx + vw - 16, vy + 20, TextAlign::Right);
        well(g, px, py, pw, ph);
        const double lx = lengthX();
        const bool live = dragging == 0 && p0over(hoverX, hoverY, px, py, pw, ph);
        const bool hotLength = dragging == 2 || (live && abs(hoverX - lx) < 7);
        const bool hotOnset = dragging == 3 || (live && !hotLength && hoverX < lx);
        const bool hotTail = dragging == 4 || (live && !hotLength && hoverX >= lx);
        g.save();
        g.clip(px, py, pw, ph);
        // The region under the pointer, faintly lit in its colour.
        if (hotOnset || hotTail) {
            g.fillStyle(hotOnset ? kGorse : kMoss);
            g.globalAlpha(0.05);
            if (hotOnset)
                g.fillRect(px, py, lx - px, ph);
            else
                g.fillRect(lx, py, px + pw - lx, ph);
            g.globalAlpha(1.0);
        }
        // The grid: every 12 dB from the hit's peak; the onset and some times.
        for (int k = 0; k < 5; ++k) {
            const double db = 12.0 - 12.0 * k;
            const double y = hitY(db);
            g.strokeStyle(db == 0.0 ? 0x4C3F34 : kGrid);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(px, y + 0.5);
            g.lineTo(px + pw, y + 0.5);
            g.stroke();
        }
        Array<double, 9> marks = {0.0, 1.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0};
        for (int k = 0; k < 9; ++k) {
            const double x = hitX(binAt(marks[k]));
            g.strokeStyle(k == 0 ? 0x4C3F34 : kGrid);
            g.beginPath();
            g.moveTo(x + 0.5, py);
            g.lineTo(x + 0.5, py + ph);
            g.stroke();
        }
        // How far the hits reach before the next one comes.
        int n = kBins;
        if (!model)
            for (int k = kPre; k < kBins; ++k)
                if (hitSeen[k] < 0.2) {
                    n = k;
                    break;
                }
        ease(model, n);
        if (n > 1) {
            // What goes out, filled.
            g.beginPath();
            g.moveTo(hitX(0), py + ph);
            for (int k = 0; k < n; ++k) g.lineTo(hitX(k), hitY(shownOut[k]));
            g.lineTo(hitX(n - 1), py + ph);
            g.closePath();
            g.linearGradient(0, py, 0, py + ph, 0x6A5F52, 0x2A231D);
            g.globalAlpha(0.55);
            g.fill();
            g.globalAlpha(1.0);
            // The change: between what came in and what goes out, in the
            // colour of the part that made it, a run of one colour at a time.
            int from = 0;
            while (from + 1 < n) {
                const bool onset = shownPart[from] + shownPart[from + 1] >= 0.0;
                int to = from + 1;
                while (to + 1 < n && (shownPart[to] + shownPart[to + 1] >= 0.0) == onset) to += 1;
                g.beginPath();
                g.moveTo(hitX(from), hitY(shownOut[from]));
                for (int k = from + 1; k <= to; ++k) g.lineTo(hitX(k), hitY(shownOut[k]));
                for (int k = to; k >= from; --k) g.lineTo(hitX(k), hitY(shownIn[k]));
                g.closePath();
                g.fillStyle(onset ? kGorse : kMoss);
                g.globalAlpha(0.4);
                g.fill();
                from = to;
            }
            g.globalAlpha(1.0);
            // What came in, then what goes out.
            g.lineJoin(LineJoin::Round);
            g.beginPath();
            for (int k = 0; k < n; ++k) {
                if (k == 0)
                    g.moveTo(hitX(k), hitY(shownIn[k]));
                else
                    g.lineTo(hitX(k), hitY(shownIn[k]));
            }
            g.strokeStyle(kLinen);
            g.globalAlpha(0.6);
            g.lineWidth(1);
            g.stroke();
            g.globalAlpha(1.0);
            g.beginPath();
            for (int k = 0; k < n; ++k) {
                if (k == 0)
                    g.moveTo(hitX(k), hitY(shownOut[k]));
                else
                    g.lineTo(hitX(k), hitY(shownOut[k]));
            }
            g.strokeStyle(kCurve);
            g.globalAlpha(model ? 0.75 : 1.0);
            g.lineWidth(1.8);
            g.stroke();
            g.globalAlpha(1.0);
        }
        if (n < kBins) {
            const double x = hitX(n - 1);
            g.fillStyle(Theme::textFaint);
            for (double y = py + 4; y < py + ph - 4; y += 6) g.fillRect(x, y, 1, 2);
            aside(g, "the next hit", x - 6, py + 34, TextAlign::Right);
        }
        // Length's handle.
        g.strokeStyle(Theme::accent2);
        g.globalAlpha(hotLength ? 1.0 : 0.7);
        g.lineWidth(hotLength ? 2.0 : 1.2);
        g.beginPath();
        g.moveTo(lx + 0.5, py + 22);
        g.lineTo(lx + 0.5, py + ph);
        g.stroke();
        g.globalAlpha(1.0);
        g.restore();
        // The handle's tab, and each part's change.
        g.fillStyle(kInk);
        g.globalAlpha(0.9);
        g.beginPath();
        g.roundRect(lx - 34, py + 4, 68, 16, 4);
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(Theme::accent2);
        g.globalAlpha(0.6);
        g.lineWidth(1);
        g.stroke();
        g.globalAlpha(1.0);
        g.fillStyle(Theme::accent2);
        g.monoFont(9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        fillTime(g, length, lx, py + 12.5);
        const double onset = model ? modelOnset : onsetShown.get();
        const double tail = model ? modelTail : tailShown.get();
        partTag(g, "ONSET", onset, kGorse, py + 12.5, hotOnset);
        partTag(g, "TAIL", tail, kMoss, py + 30.5, hotTail);
        // The scales: dB from the peak at the right, time below.
        g.monoFont(8.5);
        g.textBaseline(TextBaseline::Middle);
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        for (int k = 0; k < 5; ++k) {
            const double db = 12.0 - 12.0 * k;
            g.fillText(db == 0.0 ? "peak" : format("%+.0f", db), px + pw + 6, hitY(db));
        }
        g.textAlign(TextAlign::Center);
        for (int k = 0; k < 9; ++k) {
            const double x = hitX(binAt(marks[k]));
            if (k == 8)
                g.fillText("500 ms", x - 14, py + ph + 12);
            else
                g.fillText(format("%.0f", marks[k]), x, py + ph + 12);
        }
        // The legend.
        const double lgY = vy + vh - 14;
        g.fillStyle(kLinen);
        g.globalAlpha(0.5);
        g.fillRect(px + 2, lgY - 0.5, 12, 1);
        g.globalAlpha(1.0);
        label(g, "IN", px + 19, lgY, TextAlign::Left);
        g.fillStyle(kCurve);
        g.fillRect(px + 42, lgY - 1, 12, 2);
        label(g, "OUT", px + 59, lgY, TextAlign::Left);
        g.fillStyle(kGorse);
        g.globalAlpha(0.6);
        g.fillRect(px + 88, lgY - 3, 10, 6);
        g.globalAlpha(1.0);
        label(g, "ATTACK", px + 103, lgY, TextAlign::Left);
        g.fillStyle(kMoss);
        g.globalAlpha(0.6);
        g.fillRect(px + 148, lgY - 3, 10, 6);
        g.globalAlpha(1.0);
        label(g, "SUSTAIN", px + 163, lgY, TextAlign::Left);
        aside(g, "drag a part up or down; drag the line for Length", px + pw, lgY,
              TextAlign::Right);
    }

    // A part's name and its change, at the plot's top right.
    void partTag(Canvas& g, const char* name, double db, int colour, double y, bool hot) {
        const double right = px + pw - 10;
        g.fillStyle(kInk);
        g.globalAlpha(0.75);
        g.beginPath();
        g.roundRect(right - 104, y - 8, 108, 16, 8);
        g.fill();
        g.globalAlpha(1.0);
        g.fillStyle(colour);
        g.globalAlpha(hot ? 1.0 : 0.85);
        g.beginPath();
        g.circle(right - 95, y, 3);
        g.fill();
        g.boldFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, right - 86, y + 0.5);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        fillDb(g, db, right - 4, y + 0.5);
        g.globalAlpha(1.0);
    }

    bool p0over(double x, double y, double x0, double y0, double w, double h) const {
        return x >= x0 && x <= x0 + w && y >= y0 && y <= y0 + h;
    }

    // --- the controls -------------------------------------------------------------------------

    // Knobs: 0 attack, 1 sustain, 2 length, 3 sensitivity, 4 mix, 5 output.
    double knobX(int k) const {
        if (k == 0 || k == 2) return sx + 96;
        if (k == 1 || k == 3) return sx + 256;
        return k == 4 ? ox + 72 : ox + 178;
    }
    double knobY(int k) const {
        if (k < 2) return vy + 104;
        if (k < 4) return vy + 226;
        return hy + 112;
    }
    double knobR(int k) const { return k < 2 ? 44 : (k < 4 ? 30 : 32); }

    void drawShape(Canvas& g) {
        panel(g, sx, vy, sw, vh);
        heading(g, "Shape", sx + 16, vy + 20);
        aside(g, "the same at any level", sx + sw - 16, vy + 20, TextAlign::Right);
        dial(g, knobX(0), knobY(0), knobR(0), attack.normalized(), kGorse, true, hotKnob == 0,
             "ATTACK");
        dial(g, knobX(1), knobY(1), knobR(1), sustain.normalized(), kMoss, true, hotKnob == 1,
             "SUSTAIN");
        valueStyle(g, 12);
        fillDb(g, attack, knobX(0), knobY(0) + 60);
        fillDb(g, sustain, knobX(1), knobY(1) + 60);
        dial(g, knobX(2), knobY(2), knobR(2), length.normalized(), Theme::accent2, false,
             hotKnob == 2, "LENGTH");
        dial(g, knobX(3), knobY(3), knobR(3), sensitivity.normalized(), Theme::accent2, false,
             hotKnob == 3, "SENSITIVITY");
        valueStyle(g, 10.5);
        fillTime(g, length, knobX(2), knobY(2) + 44);
        g.fillText(format("%.0f%%", sensitivity * 100.0), knobX(3), knobY(3) + 44);
        // Focus: what the detector hears.
        label(g, "FOCUS", sx + 18, focusY + 11, TextAlign::Left);
        const int f = focus.index();
        for (int k = 0; k < 4; ++k) {
            const double x = sx + 70 + k * 68;
            const bool hot = dragging == 0 && p0over(hoverX, hoverY, x, focusY, 62, 22);
            const char* name = k == 0 ? "FULL" : (k == 1 ? "LOW" : (k == 2 ? "MID" : "HIGH"));
            pill(g, x, focusY, 62, 22, name, f == k, Theme::accent2, hot);
        }
        const char* hears = f == 0   ? "it hears everything"
                            : f == 1 ? "it hears below 150 Hz: the kick, the bass"
                            : f == 2 ? "it hears 250 Hz to 4 kHz: the snare, the voice"
                                     : "it hears above 5 kHz: the hats, the picking";
        aside(g, hears, sx + 70, focusY + 38, TextAlign::Left);
    }

    void drawOutput(Canvas& g) {
        panel(g, ox, hy, ow, hh);
        heading(g, "Output", ox + 16, hy + 20);
        dial(g, knobX(4), knobY(4), knobR(4), mix.normalized(), Theme::text, false, hotKnob == 4,
             "MIX");
        dial(g, knobX(5), knobY(5), knobR(5), output.normalized(), Theme::text, true, hotKnob == 5,
             "OUTPUT");
        valueStyle(g, 10.5);
        g.fillText(format("%.0f%%", mix * 100.0), knobX(4), knobY(4) + 48);
        fillDb(g, output, knobX(5), knobY(5) + 48);
        // The clip: its curve, and its switch.
        const bool on = clip.on();
        const double cx = clipX, top = hy + 66, side = 68;
        const bool hot = dragging == 0 && p0over(hoverX, hoverY, cx - 38, top - 4, 76, 112);
        label(g, "CLIP", cx, top - 12, TextAlign::Center);
        well(g, cx - side * 0.5, top, side, side);
        const double x0 = cx - side * 0.5 + 6, y0 = top + side - 6, span = side - 12;
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(x0, y0);
        g.lineTo(x0 + span, y0 - span);
        g.stroke();
        // Full scale, where the curve flattens.
        g.strokeStyle(Theme::alert);
        g.globalAlpha(0.35);
        g.beginPath();
        g.moveTo(x0, y0 - span / 1.6);
        g.lineTo(x0 + span, y0 - span / 1.6);
        g.stroke();
        g.globalAlpha(1.0);
        g.strokeStyle(on ? Theme::accent : Theme::textFaint);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int k = 0; k <= 40; ++k) {
            const double u = k / 40.0 * 1.6;
            const double v = on ? bend(u) : u;
            const double x = x0 + span * u / 1.6, y = y0 - span * min(v, 1.6) / 1.6;
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        pill(g, cx - 30, top + side + 12, 60, 22, on ? "ON" : "OFF", on, Theme::accent, hot);
        aside(g,
              on ? "last of all: nothing under -6 dBFS changes, nothing passes 0 dBFS"
                 : "off: the peaks the shaping raises pass as they are",
              ox + 16, hy + hh - 22, TextAlign::Left);
    }

    // --- the history ----------------------------------------------------------------------

    double levelY(double db) const {
        return gy + gh * clamp((kTop - db) / (kTop - kFloor), 0.0, 1.0);
    }
    double laneY(double db) const {
        return ly + lh * 0.5 - (lh * 0.5 - 4) * clamp(db / 12.0, -1.0, 1.0);
    }

    // Column j back from the newest, averaged with its neighbours.
    double eased(const SharedArray<float, 1024>& v, int head, int j, int count) const {
        const double here = v[wrap(head - 1 - j, kCols)];
        const double newer = j > 0 ? v[wrap(head - j, kCols)] : here;
        const double older = j + 1 < count ? v[wrap(head - 2 - j, kCols)] : here;
        return 0.25 * newer + 0.5 * here + 0.25 * older;
    }

    // A level history as a path through the midpoints of its columns.
    void historyPath(Canvas& g, const SharedArray<float, 1024>& v, int head, int count, double step,
                     bool fromEdge) {
        double qx = gx + gw, qy = levelY(eased(v, head, 0, count));
        if (fromEdge) {
            g.moveTo(qx, gy + gh);
            g.lineTo(qx, qy);
        } else {
            g.moveTo(qx, qy);
        }
        for (int j = 1; j < count; ++j) {
            const double x = gx + gw - j * step;
            const double y = levelY(eased(v, head, j, count));
            g.quadraticCurveTo(qx, qy, 0.5 * (qx + x), 0.5 * (qy + y));
            qx = x;
            qy = y;
        }
        g.lineTo(qx, qy);
        if (fromEdge) {
            g.lineTo(qx, gy + gh);
            g.closePath();
        }
    }

    // The gain lane's part for one side: the onset's columns (onset true) or the tail's.
    void lanePart(Canvas& g, int head, int count, double step, bool onset) {
        const double mid = laneY(0.0);
        g.beginPath();
        g.moveTo(gx + gw, mid);
        for (int j = 0; j < count; ++j) {
            const int c = wrap(head - 1 - j, kCols);
            const bool mine = onset ? hPart[c] > 0.0 : hPart[c] <= 0.0;
            g.lineTo(gx + gw - j * step, mine ? laneY(hGain[c]) : mid);
        }
        g.lineTo(gx + gw - (count - 1) * step, mid);
        g.closePath();
        g.fillStyle(onset ? kGorse : kMoss);
        g.globalAlpha(0.7);
        g.fill();
        g.globalAlpha(1.0);
    }

    void drawHistory(Canvas& g) {
        panel(g, hx, hy, hwid, hh);
        heading(g, "History", hx + 16, hy + 18);
        aside(g, "four seconds, each dot a hit it found", hx + hwid - 16, hy + 18,
              TextAlign::Right);
        well(g, gx, gy, gw, gh);
        well(g, gx, ly, gw, lh);
        const int head = hHead.get();
        const int count = min(hFilled.get(), kShown);
        const double step = gw / (kShown - 1.0);
        g.save();
        g.clip(gx, gy, gw, gh);
        for (int k = 0; k < 8; ++k) {
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
            g.beginPath();
            historyPath(g, hOut, head, count, step, true);
            g.linearGradient(0, gy, 0, gy + gh, 0x6A5F52, 0x2A231D);
            g.globalAlpha(0.8);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            historyPath(g, hOut, head, count, step, false);
            g.strokeStyle(kCurve);
            g.globalAlpha(0.8);
            g.lineWidth(1.1);
            g.lineJoin(LineJoin::Round);
            g.stroke();
            g.beginPath();
            historyPath(g, hIn, head, count, step, false);
            g.strokeStyle(kLinen);
            g.globalAlpha(0.35);
            g.lineWidth(1);
            g.stroke();
            g.globalAlpha(1.0);
            // The hits the detector found.
            g.fillStyle(kGorse);
            for (int j = 0; j < count; ++j) {
                if (hHit[wrap(head - 1 - j, kCols)] < 0.5) continue;
                g.beginPath();
                g.circle(gx + gw - j * step, gy + 8, 2.4);
                g.fill();
            }
        }
        g.restore();
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillStyle(Theme::textFaint);
        for (int k = 0; k < 4; ++k) {
            const double db = -12.0 * k;
            g.fillText(db == 0.0 ? "0" : format("%.0f", db), gx + gw + 6, levelY(db));
        }
        // The gain lane: raised above the middle, lowered below it.
        g.save();
        g.clip(gx, ly, gw, lh);
        for (int k = -1; k <= 1; ++k) {
            const double y = laneY(6.0 * k);
            g.strokeStyle(k == 0 ? 0x4C3F34 : kGrid);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(gx, y + 0.5);
            g.lineTo(gx + gw, y + 0.5);
            g.stroke();
        }
        if (count > 1) {
            lanePart(g, head, count, step, true);
            lanePart(g, head, count, step, false);
        }
        g.restore();
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.fillText("+6", gx + gw + 6, laneY(6.0));
        g.fillText("0", gx + gw + 6, laneY(0.0));
        g.fillText("-6", gx + gw + 6, laneY(-6.0));
        label(g, "GAIN", gx + 8, ly + 10, TextAlign::Left);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawHitView(g);
        drawShape(g);
        drawHistory(g);
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
        if (k == 0) return attack.normalized();
        if (k == 1) return sustain.normalized();
        if (k == 2) return length.normalized();
        if (k == 3) return sensitivity.normalized();
        if (k == 4) return mix.normalized();
        return output.normalized();
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, attack, v);
        if (k == 1) act(how, sustain, v);
        if (k == 2) act(how, length, v);
        if (k == 3) act(how, sensitivity, v);
        if (k == 4) act(how, mix, v);
        if (k == 5) act(how, output, v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 6; ++k) {
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
        const double perDb = ph / (kHitTop - kHitFloor);
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            } else if (dragging == 2) {
                length.edit(timeAtX(p.x()));
            } else if (dragging == 3) {
                attack.edit(attack - p.dy() * fine / perDb);
            } else if (dragging == 4) {
                sustain.edit(sustain - p.dy() * fine / perDb);
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
            if (p.over(px, py, pw, ph)) {
                if (p.x() < lengthX())
                    attack.edit(attack + notches * 0.5 * fine);
                else
                    sustain.edit(sustain + notches * 0.5 * fine);
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
        if (p.over(px, py, pw, ph)) {
            const double lx = lengthX();
            if (abs(x - lx) < 7) {
                if (p.doubleClick())
                    length.reset();
                else
                    dragging = 2;
            } else if (x < lx) {
                if (p.doubleClick())
                    attack.reset();
                else
                    dragging = 3;
            } else {
                if (p.doubleClick())
                    sustain.reset();
                else
                    dragging = 4;
            }
            return;
        }
        for (int f = 0; f < 4; ++f)
            if (p.over(sx + 70 + f * 68, focusY, 62, 22)) {
                act(0, focus, f);
                return;
            }
        if (p.over(clipX - 38, hy + 62, 76, 112)) act(0, clip, clip.on() ? 0 : 1);
    }
};
