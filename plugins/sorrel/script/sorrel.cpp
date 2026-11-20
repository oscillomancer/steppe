// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Sorrel: a gate, or a gentle downward expander.
//
// It turns down what falls under the threshold (spill between hits, hiss
// between phrases) and means to leave no trace of itself on what it lets
// through:
//
//   Detector  the louder channel of the key (the input, or the sidechain),
//             through an optional high- and low-pass, held at its peak over
//             10 ms so that no cycle of a bass note reads as a fall.
//   State     opens above the threshold; closes once the level has stayed
//             under the close level (the threshold less the hysteresis) for
//             the hold. Expand mode turns down by its ratio instead.
//   Opening   a raised-cosine fade of the attack's length, in amplitude:
//             no corner to click. With lookahead, it ends a millisecond
//             before the level crosses the threshold, so the onset passes
//             whole, its rise included, and the noise before it stays down.
//   Closing   a fade at a steady rate in dB (60 dB in the release time),
//             the way a sound decays in a room, easing in over 3 ms. With
//             Follow, the fade takes the rate the sound was already decaying
//             at, within a factor of four of the release: a hit that stops
//             short closes quickly, a ringing tom fades as it rings.
//   Range     how far a closed gate turns down; -80 dB is silence, exact
//             zeros.
//
// It keeps the last ten seconds of the key's level, a millisecond at a time,
// as a histogram: the quiet levels (noise, spill) gather in a hump, and the
// suggested threshold puts the close level just clear of that hump's top.
// The readouts give an agent the same numbers.

constexpr int kBins = 96;           // the histogram: 1 dB each, -96 to 0 dBFS
constexpr int kChunks = 100;        // its window: 100 chunks of 100 ms
constexpr int kMsRing = 2048;       // the detector's level each millisecond, for Follow
constexpr int kCols = 1024;         // history columns, 5 ms each
constexpr int kShown = 800;         // four seconds across
constexpr double kSilence = -90.0;  // where a gate set to silence lets go to zero
constexpr double kFollowMs = 50.0;  // the stretch Follow measures a decay over

// The largest of the last `hold` values: a monotonic queue, its times and
// values in rings.
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

// All Sorrel's delays are whole samples: no interpolation is needed.
struct SampleDelay {
    Buffer<double> samples;
    int size = 1, at = 0;

    void prepare(int n) {
        size = max(1, n + 1);
        samples.resize(size);
        at = 0;
    }
    void clear() { samples.fill(0.0); }
    void push(double x) {
        at = at + 1 == size ? 0 : at + 1;
        samples[at] = x;
    }
    double read(int delay) const {
        const int i = at - delay;
        // Match the interpolated line's addition at an integer position,
        // including its handling of signed zero.
        return 0.0 + samples[i < 0 ? i + size : i];
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
constexpr int kLeaf = 0x9DBA6A;   // what passes: the leaves
constexpr int kPetal = 0xD99CA6;  // what it takes away: the flowers
constexpr int kVein = 0x7A4B58;
constexpr int kStem = 0x5F7A5A;

struct Sorrel : Effect {
    static constexpr Info info = {
        .name = "Sorrel",
        .id = "com.suppleaudio.sorrel",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Gate and expander: lookahead that opens exactly on the onset, fades that follow "
            "the sound's own decay, a key input, and a level histogram that suggests the "
            "threshold.",
        .features = {"gate"},
        .sidechain = true};

    Param mode = choice("Mode", {"Gate", "Expand"}, 0)
                     .keys({"gate", "expand"})
                     .in("Gate")
                     .help(
                         "Gate opens and closes; Expand turns quieter sound down by the ratio, "
                         "more the quieter it is.");
    Param threshold = decibels("Threshold", -80, 0, -40)
                          .modulated()
                          .in("Gate")
                          .help("The level of the key, in dBFS, above which it opens.");
    Param hysteresis =
        decibels("Hysteresis", 0, 24, 6)
            .modulated()
            .in("Gate")
            .activeWhen(mode, {"Gate"})
            .help("How far under the threshold the level must fall before it closes (Gate).");
    Param range = decibels("Range", -80, 0, -40)
                      .modulated()
                      .offAtMin()
                      .in("Gate")
                      .help("How far it turns down when closed; -80 dB is silence.");
    Param ratioSet = ratio("Ratio", 1.2, 10, 2)
                         .modulated()
                         .withKey("ratio")
                         .in("Gate")
                         .activeWhen(mode, {"Expand"})
                         .help(
                             "Expand: each dB under the threshold becomes this many; 2 turns a "
                             "level 10 dB under it down 10 dB more.");
    Param attack = milliseconds("Attack", 0.05, 50, 1)
                       .modulated()
                       .in("Timing")
                       .help(
                           "How long it takes to open; with lookahead the opening ends as the "
                           "sound arrives.");
    Param hold = milliseconds("Hold", 0, 1000, 20)
                     .modulated()
                     .in("Timing")
                     .help(
                         "Gate stays open this long below the close level; Expand holds a falling "
                         "gain this long.");
    Param release = milliseconds("Release", 10, 3000, 150)
                        .modulated()
                        .in("Timing")
                        .help("How fast it closes: the time it takes to turn down by 60 dB.");
    Param follow =
        toggle("Follow", true)
            .in("Timing")
            .activeWhen(mode, {"Gate"})
            .help(
                "Closes at the rate the sound was decaying at, within a factor of four of the "
                "release (Gate).");
    Param lookahead =
        choice("Lookahead", {"Off", "1 ms", "2 ms", "5 ms"}, 2)
            .keys({"off", "ms1", "ms2", "ms5"})
            .restarts()
            .in("Timing")
            .help("Delays the audio so the gate is open when a sound arrives; adds that latency.");
    Param key =
        choice("Key", {"Internal", "External"}, 0)
            .in("Key")
            .help("What it listens to: its own input, or the sidechain input when connected.");
    Param keyHpf =
        frequency("Key HPF", 20, 2000, 20)
            .modulated()
            .in("Key")
            .help(
                "A high-pass on what it listens to, so rumble or a kick's spill does not "
                "open it; 20 Hz is off.");
    Param keyLpf = frequency("Key LPF", 100, 20000, 20000)
                       .modulated()
                       .in("Key")
                       .help(
                           "A low-pass on what it listens to, so cymbals or hiss do not open it; "
                           "20 kHz is off.");
    Param listen =
        choice("Listen", {"Off", "Key", "Delta"}, 0)
            .notAutomatable()
            .in("Key")
            .help("Plays what it listens to, or what it takes away, instead of the output.");

    Param gainNow = decibels("Gain", -80, 0, 0)
                        .withKey("gain")
                        .meter()
                        .help("The gain it applies now: 0 dB open, the range closed.");
    Param noiseFloor = decibels("Noise Floor", -96, 0, -96)
                           .meter()
                           .help(
                               "The most common quiet level of the key over the last 10 s: the "
                               "noise or spill; -96 until something plays.");
    Param loudLevel =
        decibels("Loud Level", -96, 0, -96)
            .meter()
            .help("The level the key exceeds 5% of the time over the last 10 s: the sound.");
    Param suggested =
        decibels("Suggested Threshold", -96, 0, -96)
            .meter()
            .help(
                "A threshold whose close level sits 2 dB above the noise floor's peaks, from the "
                "last 10 s.");
    Param openTime =
        percent("Open Time", 0).meter().help("How much of the last 10 s it was open, in percent.");
    Param openings =
        continuous("Openings", 0, 10000, 0)
            .meter()
            .help(
                "How many times it opened in the last 10 s: about one a hit; many more is "
                "chatter.");

    void factoryPatches(Patches& p) {
        p.add("Clean Start");
        p.add("Snare Spill")
            .set(threshold, -30)
            .set(range, -30)
            .set(hold, 30)
            .set(release, 120)
            .set(keyHpf, 180);
        p.add("Kick Tight")
            .set(threshold, -28)
            .set(range, -80)
            .set(hold, 30)
            .set(release, 90)
            .set(keyLpf, 180);
        p.add("Tom Ring")
            .set(threshold, -32)
            .set(range, -24)
            .set(hold, 60)
            .set(release, 400)
            .set(keyHpf, 60)
            .set(keyLpf, 2000);
        p.add("Vocal Pauses")
            .set(mode, "expand")
            .set(threshold, -48)
            .set(ratioSet, 2)
            .set(range, -18)
            .set(hold, 120)
            .set(release, 300)
            .set(keyHpf, 100);
        p.add("Amp Hiss")
            .set(threshold, -50)
            .set(hysteresis, 8)
            .set(range, -30)
            .set(hold, 80)
            .set(release, 400)
            .set(keyLpf, 5000);
        p.add("Full Silence").set(range, -80).set(hold, 50).set(release, 200);
        p.add("Keyed Chop")
            .set(key, "external")
            .set(threshold, -30)
            .set(range, -80)
            .set(attack, 0.5)
            .set(hold, 10)
            .set(release, 40)
            .set(follow, false)
            .set(lookahead, "ms5");
    }

    // --- audio ------------------------------------------------------------------------------

    double rate = 48000.0;
    int ahead = 0;  // the lookahead, samples
    int envLength = 480;
    PeakHold env, up;
    double heldPeak = -1.0, heldLevel = -120.0;
    Array<Biquad, 2> hpf, lpf;
    bool hpOn = false, lpOn = false;
    Array<SampleDelay, 2> delay, keyDelay;
    SampleDelay targets;
    bool expand = false;
    double closedDb = -40.0;
    int attackSamples = 48, window = 48, holdSamples = 960;
    double releaseRate = 0.0, riseStep = 1.0, easeStep = 0.0;
    // The state, in the detector's time.
    bool open = false;
    int sinceAbove = 0;
    // The gain, in the audio's time.
    double gainDb = -40.0, amp = 0.0, fallRate = 0.0, fallSpeed = 0.0;
    bool ramping = false;
    double rampFrom = 0.0, rampTo = 1.0, rampToDb = 0.0, rampAt = 0.0, rampStep = 0.1;
    int holdCount = 0;
    int listenMode = 0;
    int listenLength = 480, listenAt = 480;
    double listenKey = 0.0, listenDelta = 0.0;
    double listenFromKey = 0.0, listenFromDelta = 0.0;
    bool unplayed = true;

    // The detector's level each millisecond, and the histogram's chunks.
    double msLength = 48.0, msAt = 0.0;
    Array<double, 2048> msLevel;
    int msHead = 0, msFilled = 0;
    Buffer<int> chunkBins;
    Array<int, 100> chunkCount, chunkTime, chunkOpen, chunkOpenings;
    Array<int, 96> total;
    int totalCount = 0, totalTime = 0, totalOpen = 0, totalOpenings = 0;
    int chunk = 0, msInChunk = 0;
    double floorNow = -96.0, loudNow = -96.0, suggestNow = -96.0;

    // The history and the header's meters.
    double colLength = 240.0, colAt = 0.0;
    double colIn = 0.0, colOut = 0.0, colKey = 0.0, colGain = 0.0;
    int hw = 0, filled = 0;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0;
    double envIn = 0.0, envOut = 0.0, envFall = 0.999, fall = 0.999;

    SharedArray<float, 1024> hIn, hOut, hKey, hGain;
    SharedArray<float, 96> hist;
    Shared<int> hHead, hFilled, histCount;
    Shared<double> inLeft, inRight, outLeft, outRight, openness, floorShown, loudShown,
        suggestShown, keyNow, gainShown;
    Shared<bool> keyLive;

    void prepare() {
        rate = sampleRate();
        const int choice = lookahead.index();
        const double ms = choice == 1 ? 1.0 : (choice == 2 ? 2.0 : (choice == 3 ? 5.0 : 0.0));
        ahead = roundToInt(ms * 0.001 * rate);
        setLatency(ahead);
        envLength = max(1, roundToInt(0.01 * rate));
        env.prepare(envLength + 2);
        heldPeak = -1.0;
        heldLevel = -120.0;
        up.prepare(ahead + 4);
        const int room = ahead + 8;
        for (int c = 0; c < 2; ++c) {
            delay[c].prepare(room);
            keyDelay[c].prepare(room);
            delay[c].clear();
            keyDelay[c].clear();
            hpf[c].clear();
            lpf[c].clear();
        }
        targets.prepare(room);
        targets.clear();
        easeStep = 1.0 / (0.003 * rate);
        listenLength = max(1, roundToInt(0.01 * rate));
        msLength = 0.001 * rate;
        colLength = 0.005 * rate;
        fall = exp(-1.0 / (0.3 * rate));
        envFall = exp(-1.0 / (0.03 * rate));
        chunkBins.resize(kChunks * kBins);
        // A restart prepares an instance that has played: start it afresh.
        chunkBins.fill(0);
        chunkCount.fill(0);
        chunkTime.fill(0);
        chunkOpen.fill(0);
        chunkOpenings.fill(0);
        total.fill(0);
        totalCount = 0;
        totalTime = 0;
        totalOpen = 0;
        totalOpenings = 0;
        chunk = 0;
        msInChunk = 0;
        msAt = 0.0;
        msLevel.fill(-120.0);
        msHead = 0;
        msFilled = 0;
        floorNow = -96.0;
        loudNow = -96.0;
        suggestNow = -96.0;
        open = false;
        sinceAbove = 0;
        ramping = false;
        holdCount = 0;
        fallSpeed = 0.0;
        colAt = 0.0;
        colIn = 0.0;
        colOut = 0.0;
        colKey = 0.0;
        colGain = 0.0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        envIn = 0.0;
        envOut = 0.0;
        unplayed = true;
        settings();
        snapListen();
        gainDb = closedDb;
        amp = gainOf(gainDb);
    }

    void settings() {
        expand = mode.index() == 1;
        closedDb = range <= -79.95 ? kSilence : range.value();
        attackSamples = max(0, roundToInt(attack * 0.001 * rate));
        window = min(attackSamples + roundToInt(0.001 * rate), ahead);
        rampStep = 1.0 / max(1, attackSamples);
        riseStep = 1.0 - exp(-3.0 / max(1, attackSamples));
        holdSamples = roundToInt(hold * 0.001 * rate);
        releaseRate = 60.0 / (release * 0.001 * rate);
        fallRate = releaseRate;
        hpOn = keyHpf > 20.5;
        lpOn = keyLpf < 19900.0;
        for (int c = 0; c < 2; ++c) {
            hpf[c].highpass(keyHpf, 0.707);
            lpf[c].lowpass(min(keyLpf.value(), 0.45 * rate), 0.707);
        }
        const int nextListen = listen.index();
        if (nextListen != listenMode) {
            // A new edit starts from the blend heard at the last sample,
            // even if the previous transition has not finished.
            listenFromKey = listenKey;
            listenFromDelta = listenDelta;
            listenAt = 0;
            listenMode = nextListen;
        }
    }

    void snapListen() {
        listenKey = listenMode == 1 ? 1.0 : 0.0;
        listenDelta = listenMode == 2 ? 1.0 : 0.0;
        listenFromKey = listenKey;
        listenFromDelta = listenDelta;
        listenAt = listenLength;
    }

    void update() {
        settings();
        if (unplayed) {
            snapListen();
            gainDb = closedDb;
            amp = gainOf(gainDb);
        }
    }

    double gainOf(double db) const { return db <= kSilence + 1e-9 ? 0.0 : dbToGain(db); }

    // Expand's curve, with a 6 dB knee: the gain for a level, no lower than
    // the floor.
    double expanded(double level, double floorDb) const {
        const double k = 6.0, d = level - threshold;
        if (d >= 0.5 * k) return 0.0;
        const double r = ratioSet - 1.0;
        if (d <= -0.5 * k) return max(floorDb, r * d);
        const double e = d - 0.5 * k;
        return max(floorDb, -r * e * e / (2.0 * k));
    }

    // The rate to close at: the release's, or with Follow the rate the
    // level fell at from 50 ms before it went under the close level to
    // 20 ms after (or now, if the hold is shorter), within a factor of four
    // of the release's. A sound that stops dead closes fast; one that rings
    // fades as it rang.
    double closingRate() const {
        if (!follow.on() || expand) return releaseRate;
        const int sinceMs = roundToInt(sinceAbove / msLength);
        const int span = roundToInt(kFollowMs);
        const int after = min(sinceMs, 20);
        if (msFilled < sinceMs + span + 2) return releaseRate;
        const double later = msLevel[wrap(msHead - 1 - sinceMs + after, kMsRing)];
        const double before = msLevel[wrap(msHead - 1 - sinceMs - span, kMsRing)];
        const double slope = (before - later) / ((span + after) * msLength);
        return clamp(slope, 0.25 * releaseRate, 4.0 * releaseRate);
    }

    void process(Block& io) {
        unplayed = false;
        const bool stereo = io.channels > 1;
        const bool external = key.index() == 1 && io.sidechainConnected;
        double lowest = 0.0, lastLevel = -120.0;
        for (int i = 0; i < io.frames; ++i) {
            double l = io[0][i];
            double r = stereo ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;
            double kl = l, kr = r;
            if (external) {
                kl = io.sidechain(0)[i];
                kr = stereo ? io.sidechain(1)[i] : kl;
                if (!isFinite(kl)) kl = 0.0;
                if (!isFinite(kr)) kr = 0.0;
            }
            if (hpOn) {
                kl = hpf[0].process(kl);
                kr = hpf[1].process(kr);
            }
            if (lpOn) {
                kl = lpf[0].process(kl);
                kr = lpf[1].process(kr);
            }
            keyDelay[0].push(kl);
            keyDelay[1].push(kr);
            const double peak = env.push(min(max(abs(kl), abs(kr)), 1e4), envLength);
            // A held peak usually stays unchanged for many samples.
            if (peak != heldPeak) {
                heldPeak = peak;
                heldLevel = gainToDb(peak);
            }
            const double level = heldLevel;
            lastLevel = level;

            // The state, as the detector hears it (a lookahead early).
            if (!open) {
                if (level > threshold) {
                    open = true;
                    sinceAbove = 0;
                    chunkOpenings[chunk] += 1;
                    totalOpenings += 1;
                }
            } else if (level >= threshold - hysteresis) {
                sinceAbove = 0;
            } else {
                sinceAbove += 1;
                if (sinceAbove > holdSamples) {
                    open = false;
                    fallRate = closingRate();
                }
            }
            const double target = expand ? expanded(level, closedDb) : (open ? 0.0 : closedDb);

            // The gain the audio now playing gets: it rises for anything
            // asked within the next `window` samples (the opening ends 1 ms
            // before the level crosses the threshold, catching the start of
            // the sound's rise), and falls for what is asked now.
            // (Stored 200 dB up: the line's first zeros read as closed.)
            targets.push(target + 200.0);
            const double want =
                max(closedDb, up.push(targets.read(ahead - window), window + 1) - 200.0);
            step(want);
            if (gainDb < lowest) lowest = gainDb;

            delay[0].push(l);
            delay[1].push(r);
            const double dl = ahead > 0 ? delay[0].read(ahead) : l;
            const double dr = ahead > 0 ? delay[1].read(ahead) : r;
            double ol = dl * amp;
            double orr = dr * amp;
            const double aOut = max(abs(ol), abs(orr));  // what the gate passes, heard or not
            if (listenAt < listenLength) {
                listenAt += 1;
                const double t = static_cast<double>(listenAt) / listenLength;
                const double toKey = listenMode == 1 ? 1.0 : 0.0;
                const double toDelta = listenMode == 2 ? 1.0 : 0.0;
                listenKey = listenAt == listenLength ? toKey : lerp(listenFromKey, toKey, t);
                listenDelta =
                    listenAt == listenLength ? toDelta : lerp(listenFromDelta, toDelta, t);
            }
            // Blend the live wet, key and delta signals. The wet signal's
            // weight is the remainder; no transition fades through silence.
            const double wetL = ol, wetR = orr;
            if (listenKey > 0.0) {
                const double hl = ahead > 0 ? keyDelay[0].read(ahead) : kl;
                const double hr = ahead > 0 ? keyDelay[1].read(ahead) : kr;
                ol += listenKey * (hl - wetL);
                orr += listenKey * (hr - wetR);
            }
            if (listenDelta > 0.0) {
                ol += listenDelta * ((dl - wetL) - wetL);
                orr += listenDelta * ((dr - wetR) - wetR);
            }
            io[0][i] = ol;
            if (stereo) io[1][i] = orr;

            // What the readouts, the histogram and the history see.
            msAt += 1.0;
            if (msAt >= msLength) {
                msAt -= msLength;
                closeMs(level, peak > 0.0);
            }
            const double aIn = max(abs(dl), abs(dr));
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);
            envIn = max(aIn, envIn * envFall);
            envOut = max(aOut, envOut * envFall);
            colIn = max(colIn, envIn);
            colOut = max(colOut, envOut);
            colKey = max(colKey, peak);
            colGain = min(colGain, gainDb);
            colAt += 1.0;
            if (colAt >= colLength) closeColumn();
        }
        hHead.set(hw);
        hFilled.set(filled);
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(stereo ? peakIn1 : peakIn0));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(stereo ? peakOut1 : peakOut0));
        openness.set(clamp((gainDb - closedDb) / max(1.0, -closedDb), 0.0, 1.0));
        keyLive.set(io.sidechainConnected);
        keyNow.set(lastLevel);
        gainShown.set(gainDb);
        gainNow.set(max(-80.0, lowest));
    }

    // The gain's next sample, towards what is wanted.
    void step(double want) {
        if (want > gainDb + 1e-9) {
            holdCount = 0;
            fallSpeed = 0.0;
            if (expand) {
                // Towards the curve in amplitude, 95% of the way in the attack.
                ramping = false;
                const double to = gainOf(want);
                amp += riseStep * (to - amp);
                if (to - amp <= 1e-7 * to) {
                    amp = to;
                    gainDb = want;
                } else {
                    gainDb = max(closedDb, gainToDb(amp));
                }
                return;
            }
            // A raised cosine in amplitude, from where it is.
            if (!ramping || rampToDb != want) {
                ramping = true;
                rampFrom = amp;
                rampToDb = want;
                rampTo = gainOf(want);
                rampAt = 0.0;
            }
            rampAt += rampStep;
            if (rampAt >= 1.0) {
                ramping = false;
                gainDb = want;
                amp = want == 0.0 ? 1.0 : rampTo;
            } else {
                amp = rampFrom + (rampTo - rampFrom) * (0.5 - 0.5 * cos(pi * rampAt));
                gainDb = max(gainToDb(amp), closedDb);
            }
            return;
        }
        ramping = false;
        if (want < gainDb - 1e-9) {
            // Expand holds here (the gate held in its state).
            if (expand && holdCount < holdSamples) {
                holdCount += 1;
                return;
            }
            fallSpeed = min(fallRate, fallSpeed + fallRate * easeStep);
            gainDb = max(want, gainDb - fallSpeed);
            amp = gainOf(gainDb);
            return;
        }
        holdCount = 0;
        fallSpeed = 0.0;
        if (gainDb == 0.0) amp = 1.0;
    }

    // A millisecond: the level for Follow and into the histogram, unless
    // the key was digital silence.
    void closeMs(double level, bool heard) {
        msLevel[msHead] = level;
        msHead = msHead + 1 == kMsRing ? 0 : msHead + 1;
        msFilled = min(msFilled + 1, kMsRing);
        if (heard) {
            const int b = clamp(floorToInt(level + 96.0), 0, kBins - 1);
            chunkBins[chunk * kBins + b] += 1;
            total[b] += 1;
            chunkCount[chunk] += 1;
            totalCount += 1;
        }
        chunkTime[chunk] += 1;
        totalTime += 1;
        if (open) {
            chunkOpen[chunk] += 1;
            totalOpen += 1;
        }
        msInChunk += 1;
        if (msInChunk >= 100) closeChunk();
    }

    // Every 100 ms: the readouts from the last ten seconds, then the oldest
    // chunk makes room.
    void closeChunk() {
        msInChunk = 0;
        measure();
        chunk = chunk + 1 == kChunks ? 0 : chunk + 1;
        for (int b = 0; b < kBins; ++b) {
            total[b] -= chunkBins[chunk * kBins + b];
            chunkBins[chunk * kBins + b] = 0;
        }
        totalCount -= chunkCount[chunk];
        totalTime -= chunkTime[chunk];
        totalOpen -= chunkOpen[chunk];
        totalOpenings -= chunkOpenings[chunk];
        chunkCount[chunk] = 0;
        chunkTime[chunk] = 0;
        chunkOpen[chunk] = 0;
        chunkOpenings[chunk] = 0;
    }

    double smoothed(int b) const {
        double s = 0.0;
        for (int j = -2; j <= 2; ++j) {
            const int q = b + j;
            if (q >= 0 && q < kBins) s += (3 - abs(j)) * total[q];
        }
        return s;
    }

    int percentileBin(double q) const {
        const double want = q * totalCount;
        int sum = 0;
        for (int b = 0; b < kBins; ++b) {
            sum += total[b];
            if (sum >= want) return b;
        }
        return kBins - 1;
    }

    // The noise floor: the commonest level among the quietest 30%. Its
    // hump reaches twice as far above as it does below (to the quietest
    // 2%), and at least 4 dB: noise's peaks spread upwards. The suggestion
    // puts the close level 2 dB above that, kept 3 dB under the loud level.
    // From below, because above the hump lie the sounds' own decays, which
    // fill the levels between noise and hits and leave no edge to find.
    void measure() {
        double most = 0.0;
        for (int b = 0; b < kBins; ++b) most = max(most, smoothed(b));
        for (int b = 0; b < kBins; ++b)
            hist[b] = static_cast<float>(most > 0.0 ? sqrt(smoothed(b) / most) : 0.0);
        histCount.set(totalCount);
        if (totalCount < 300) {
            floorNow = -96.0;
            loudNow = -96.0;
            suggestNow = -96.0;
        } else {
            const int low = percentileBin(0.02), high = percentileBin(0.3);
            int mode = low;
            double best = -1.0;
            for (int b = low; b <= high; ++b) {
                const double s = smoothed(b);
                if (s > best) {
                    best = s;
                    mode = b;
                }
            }
            floorNow = mode - 96.0 + 0.5;
            loudNow = percentileBin(0.95) - 96.0 + 1.0;
            const double top = floorNow + max(4.0, 2.0 * (mode - low));
            suggestNow = clamp(min(top + 2.0 + hysteresis, loudNow - 3.0), -80.0, 0.0);
        }
        noiseFloor.set(floorNow);
        loudLevel.set(loudNow);
        suggested.set(suggestNow);
        floorShown.set(floorNow);
        loudShown.set(loudNow);
        suggestShown.set(suggestNow);
        openTime.set(totalTime > 0 ? static_cast<double>(totalOpen) / totalTime : 0.0);
        openings.set(totalOpenings);
    }

    void closeColumn() {
        colAt -= colLength;
        hIn[hw] = static_cast<float>(gainToDb(colIn));
        hOut[hw] = static_cast<float>(gainToDb(colOut));
        hKey[hw] = static_cast<float>(gainToDb(colKey));
        hGain[hw] = static_cast<float>(colGain);
        hw = hw == kCols - 1 ? 0 : hw + 1;
        filled = min(filled + 1, kCols);
        colIn = 0.0;
        colOut = 0.0;
        colKey = 0.0;
        colGain = 0.0;
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1000;
    static constexpr double kH = 720;
    static constexpr double mx = 16;  // the main panel
    static constexpr double my = 60;
    static constexpr double mw = 968;
    static constexpr double mh = 360;
    static constexpr double gx = 28;  // its level plot
    static constexpr double gy = 92;
    static constexpr double gw = 704;
    static constexpr double gh = 220;
    static constexpr double ly = 322;  // its gain lane
    static constexpr double lh = 52;
    static constexpr double bx = 772;  // the histogram
    static constexpr double bw = 196;
    static constexpr double kTop = 0.0;      // the plots' top, dBFS
    static constexpr double kFloor = -84.0;  // their floor
    static constexpr double py = 434;        // the lower panels
    static constexpr double ph = 270;
    static constexpr double ax = 16;  // the gate panel
    static constexpr double aw = 372;
    static constexpr double tx = 400;  // the timing panel
    static constexpr double tw = 340;
    static constexpr double kx = 752;  // the key panel
    static constexpr double kw = 232;
    static constexpr double sx = tx + 16;  // the timing sketch
    static constexpr double sy = py + 38;
    static constexpr double sw = tw - 32;
    static constexpr double sh = 78;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the threshold, 3 the close level, 4 the range
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;

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

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit to the value.
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool hot,
              const char* name) {
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
        g.fillText(name, x, y - r - 12);
    }

    void valueStyle(Canvas& g) {
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void fillTime(Canvas& g, double ms, double x, double y) {
        if (ms < 1.0)
            g.fillText(format("%.2f ms", ms), x, y);
        else if (ms < 10.0)
            g.fillText(format("%.1f ms", ms), x, y);
        else if (ms < 1000.0)
            g.fillText(format("%.0f ms", ms), x, y);
        else
            g.fillText(format("%.2f s", ms * 0.001), x, y);
    }

    void fillHz(Canvas& g, double hz, double x, double y) {
        if (hz < 1000.0)
            g.fillText(format("%.0f Hz", hz), x, y);
        else
            g.fillText(format("%.1f kHz", hz * 0.001), x, y);
    }

    // A dashed level line across a plot.
    void dashed(Canvas& g, double x0, double x1, double y, double on, double off) {
        g.beginPath();
        for (double x = x0; x < x1; x += on + off) {
            g.moveTo(x, y);
            g.lineTo(min(x + on, x1), y);
        }
        g.stroke();
    }

    // The mark: a sprig of wood sorrel, three heart-shaped leaflets that
    // fold as the gate closes.
    void leaflet(Canvas& g, double x, double y, double angle, double length, double width) {
        const double c = cos(angle), s = sin(angle);
        const double l = length, w = width;
        // (u along the leaflet, v across) to the canvas.
        g.beginPath();
        g.moveTo(x, y);
        g.bezierCurveTo(x + c * 0.25 * l - s * 0.7 * w, y + s * 0.25 * l + c * 0.7 * w,
                        x + c * 0.8 * l - s * 1.15 * w, y + s * 0.8 * l + c * 1.15 * w,
                        x + c * 1.0 * l - s * 0.45 * w, y + s * 1.0 * l + c * 0.45 * w);
        g.quadraticCurveTo(x + c * 1.05 * l - s * 0.1 * w, y + s * 1.05 * l + c * 0.1 * w,
                           x + c * 0.8 * l, y + s * 0.8 * l);
        g.quadraticCurveTo(x + c * 1.05 * l + s * 0.1 * w, y + s * 1.05 * l - c * 0.1 * w,
                           x + c * 1.0 * l + s * 0.45 * w, y + s * 1.0 * l - c * 0.45 * w);
        g.bezierCurveTo(x + c * 0.8 * l + s * 1.15 * w, y + s * 0.8 * l - c * 1.15 * w,
                        x + c * 0.25 * l + s * 0.7 * w, y + s * 0.25 * l - c * 0.7 * w, x, y);
        g.fill();
    }

    void drawMark(Canvas& g, double x, double y, double o) {
        g.strokeStyle(kStem);
        g.lineCap(LineCap::Round);
        g.lineWidth(1.6);
        g.beginPath();
        g.moveTo(x, y + 2);
        g.quadraticCurveTo(x + 1, y + 10, x - 2, y + 17);
        g.stroke();
        const double width = 8.0 * (0.3 + 0.7 * o);
        const double droop = (1.0 - o) * 0.5;
        const double reach = 11.5 - 2.0 * (1.0 - o);
        g.fillStyle(kLeaf);
        g.globalAlpha(0.55 + 0.45 * o);
        leaflet(g, x, y, -0.5 * pi, reach, width);
        leaflet(g, x, y, pi / 6.0 + droop, reach, width);
        leaflet(g, x, y, 5.0 * pi / 6.0 - droop, reach, width);
        g.globalAlpha(1.0);
        g.strokeStyle(0x5D7440);
        g.lineWidth(0.8);
        for (int k = 0; k < 3; ++k) {
            const double a =
                k == 0 ? -0.5 * pi : (k == 1 ? pi / 6.0 + droop : 5.0 * pi / 6.0 - droop);
            g.beginPath();
            g.moveTo(x, y);
            g.lineTo(x + cos(a) * reach * 0.78, y + sin(a) * reach * 0.78);
            g.stroke();
        }
    }

    // --- the header -------------------------------------------------------------------------

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
        drawMark(g, 30, 22, openness.get());
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Sorrel", 52, 37);
        const double nameWidth = g.measureText("Sorrel");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText(mode.index() == 1 ? "expander" : "gate", 60 + nameWidth, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 620, 23, 140, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 824, 23, 140, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the main view ------------------------------------------------------------------------

    double levelY(double db) const {
        return gy + gh * clamp((kTop - db) / (kTop - kFloor), 0.0, 1.0);
    }
    double levelAt(double y) const { return kTop - (y - gy) / gh * (kTop - kFloor); }
    double laneY(double db) const { return ly + 4 + (lh - 8) * clamp(-db / 80.0, 0.0, 1.0); }
    double closeLevel() const { return threshold - hysteresis; }
    bool keyShaped() const { return key.index() == 1 || keyHpf > 20.5 || keyLpf < 19900.0; }

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

    void grid(Canvas& g, double x, double w) {
        for (int k = 0; k <= 7; ++k) {
            const double y = levelY(-12.0 * k);
            g.strokeStyle(k == 0 ? 0x4C3F34 : (imod(k, 2) == 0 ? kGridLit : kGrid));
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(x, y + 0.5);
            g.lineTo(x + w, y + 0.5);
            g.stroke();
        }
    }

    // The threshold, and the close level or Expand's floor, across a plot.
    void levelLines(Canvas& g, double x, double w, bool hotOpen, bool hotClose) {
        const bool exp = mode.index() == 1;
        const double ty = levelY(threshold);
        const double cy =
            exp ? levelY(threshold + max(-80.0, range.value()) / max(0.01, ratioSet - 1.0))
                : levelY(closeLevel());
        if (!exp) {
            g.fillStyle(kLeaf);
            g.globalAlpha(0.07);
            g.fillRect(x, ty, w, cy - ty);
            g.globalAlpha(1.0);
        }
        g.strokeStyle(exp ? kPetal : kLeaf);
        g.globalAlpha(hotClose ? 1.0 : 0.7);
        g.lineWidth(hotClose ? 1.6 : 1.1);
        dashed(g, x, x + w, cy + 0.5, 5, 4);
        g.strokeStyle(kLeaf);
        g.globalAlpha(1.0);
        g.lineWidth(hotOpen ? 2.2 : 1.5);
        g.beginPath();
        g.moveTo(x, ty + 0.5);
        g.lineTo(x + w, ty + 0.5);
        g.stroke();
    }

    void tag(Canvas& g, const char* name, double db, double x, double y, int colour, bool hot) {
        g.fillStyle(kInk);
        g.globalAlpha(0.85);
        g.beginPath();
        g.roundRect(x - 92, y - 8, 92, 16, 8);
        g.fill();
        g.globalAlpha(hot ? 1.0 : 0.85);
        g.fillStyle(colour);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, x - 84, y + 0.5);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        if (db <= -79.95)
            g.fillText("silence", x - 8, y + 0.5);
        else
            g.fillText(format("%.1f dB", db), x - 8, y + 0.5);
        g.globalAlpha(1.0);
    }

    void drawMain(Canvas& g) {
        panel(g, mx, my, mw, mh);
        heading(g, "The gate", mx + 16, my + 20);
        aside(g, "four seconds: what came in, what goes out, and what it took", gx + gw, my + 20,
              TextAlign::Right);
        well(g, gx, gy, gw, gh);
        well(g, gx, ly, gw, lh);
        const bool exp = mode.index() == 1;
        const double ty = levelY(threshold), cy = levelY(closeLevel());
        const bool inPlot = hovered(gx, gy, gw, gh) || hovered(bx, gy, bw, gh);
        const bool hotClose = dragging == 3 || (inPlot && !exp && abs(hoverY - cy) < 6 &&
                                                abs(hoverY - ty) >= abs(hoverY - cy));
        const bool hotOpen = dragging == 2 || (inPlot && !hotClose);
        const int head = hHead.get();
        const int count = min(hFilled.get(), kShown);
        const double step = gw / (kShown - 1.0);
        g.save();
        g.clip(gx, gy, gw, gh);
        grid(g, gx, gw);
        if (count > 1) {
            // What came in, in the colour of what was taken; what goes out
            // over it, so the rose left showing is what the gate removed.
            g.beginPath();
            historyPath(g, hIn, head, count, step, true);
            g.fillStyle(kPetal);
            g.globalAlpha(0.45);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            historyPath(g, hOut, head, count, step, true);
            g.linearGradient(0, gy, 0, gy + gh, 0x625749, 0x2A231D);
            g.fill();
            g.lineJoin(LineJoin::Round);
            g.beginPath();
            historyPath(g, hIn, head, count, step, false);
            g.strokeStyle(kPetal);
            g.globalAlpha(0.55);
            g.lineWidth(1);
            g.stroke();
            g.globalAlpha(1.0);
            g.beginPath();
            historyPath(g, hOut, head, count, step, false);
            g.strokeStyle(kCurve);
            g.lineWidth(1.2);
            g.stroke();
            if (keyShaped()) {
                g.beginPath();
                historyPath(g, hKey, head, count, step, false);
                g.strokeStyle(Theme::accent2);
                g.globalAlpha(0.85);
                g.lineWidth(1);
                g.stroke();
                g.globalAlpha(1.0);
            }
        }
        levelLines(g, gx, gw, hotOpen, hotClose);
        g.restore();
        tag(g, "OPEN", threshold, gx + gw - 8, ty - 11 < gy + 10 ? ty + 11 : ty - 11, kLeaf,
            hotOpen);
        if (!exp)
            tag(g, "CLOSE", closeLevel(), gx + gw - 8, min(cy + 11, gy + gh - 10), kLeaf, hotClose);
        // The scale, between the plot and the histogram.
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillStyle(Theme::textFaint);
        for (int k = 0; k <= 7; ++k) {
            const double db = -12.0 * k;
            g.fillText(k == 0 ? "0" : format("%.0f", db), gx + gw + 8, levelY(db));
        }
        drawLane(g, head, count, step);
        drawHistogram(g, hotOpen, hotClose);
        // The legend.
        const double lgY = my + mh - 18;
        g.fillStyle(kPetal);
        g.globalAlpha(0.6);
        g.fillRect(gx + 2, lgY - 0.5, 12, 1);
        g.globalAlpha(1.0);
        label(g, "IN", gx + 19, lgY, TextAlign::Left);
        g.fillStyle(kCurve);
        g.fillRect(gx + 42, lgY - 1, 12, 2);
        label(g, "OUT", gx + 59, lgY, TextAlign::Left);
        g.fillStyle(kPetal);
        g.globalAlpha(0.6);
        g.fillRect(gx + 88, lgY - 3, 10, 6);
        g.globalAlpha(1.0);
        label(g, "TAKEN", gx + 103, lgY, TextAlign::Left);
        if (keyShaped()) {
            g.fillStyle(Theme::accent2);
            g.fillRect(gx + 146, lgY - 0.5, 12, 1);
            label(g, "KEY", gx + 163, lgY, TextAlign::Left);
        }
        aside(g, "drag a line, or click a level for the threshold; drag the gain for the range",
              gx + gw, lgY, TextAlign::Right);
    }

    // The gain lane: open at the top, the range at the bottom.
    void drawLane(Canvas& g, int head, int count, double step) {
        const bool hot = dragging == 4 || hovered(gx, ly, gw, lh);
        g.save();
        g.clip(gx, ly, gw, lh);
        // Filled from the range up to the gain: empty while closed.
        const double floorY = laneY(range);
        if (count > 1) {
            g.beginPath();
            g.moveTo(gx + gw, floorY);
            for (int j = 0; j < count; ++j)
                g.lineTo(gx + gw - j * step, laneY(hGain[wrap(head - 1 - j, kCols)]));
            g.lineTo(gx + gw - (count - 1) * step, floorY);
            g.closePath();
            g.linearGradient(0, ly, 0, floorY, kLeaf, 0x3E4A2C);
            g.globalAlpha(0.7);
            g.fill();
            g.globalAlpha(1.0);
        }
        g.strokeStyle(kPetal);
        g.globalAlpha(hot ? 1.0 : 0.6);
        g.lineWidth(hot ? 1.6 : 1);
        dashed(g, gx, gx + gw, laneY(range) + 0.5, 5, 4);
        g.globalAlpha(1.0);
        g.restore();
        label(g, "GAIN", gx + 8, ly + 11, TextAlign::Left);
        tag(g, "RANGE", range, gx + gw - 8, laneY(range) > ly + lh * 0.5 ? ly + 13 : ly + lh - 13,
            kPetal, hot);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.fillText("0", gx + gw + 8, laneY(0.0));
        g.fillText("-80", gx + gw + 8, laneY(-80.0));
    }

    void drawHistogram(Canvas& g, bool hotOpen, bool hotClose) {
        heading(g, "Levels", bx, my + 20);
        aside(g, "the key, ten seconds", bx + bw, my + 20, TextAlign::Right);
        well(g, bx, gy, bw, gh);
        const bool exp = mode.index() == 1;
        const double ty = levelY(threshold), cy = exp ? ty : levelY(closeLevel());
        g.save();
        g.clip(bx, gy, bw, gh);
        grid(g, bx, bw);
        const bool any = histCount.get() > 0;
        if (any) {
            // Above the threshold in leaf, under the close level in rose.
            for (int part = 0; part < 3; ++part) {
                g.save();
                if (part == 0) g.clip(bx, gy, bw, ty - gy);
                if (part == 1) g.clip(bx, ty, bw, max(0.0, cy - ty));
                if (part == 2) g.clip(bx, cy, bw, gy + gh - cy);
                g.beginPath();
                g.moveTo(bx, levelY(-95.5));
                for (int b = 0; b < kBins; ++b)
                    g.lineTo(bx + 2 + (bw - 6) * hist[b], levelY(b - 95.5));
                g.lineTo(bx, levelY(-0.5));
                g.closePath();
                g.fillStyle(part == 0 ? kLeaf : (part == 1 ? kLinen : kPetal));
                g.globalAlpha(part == 1 ? 0.35 : 0.55);
                g.fill();
                g.globalAlpha(1.0);
                g.restore();
            }
            g.beginPath();
            for (int b = 0; b < kBins; ++b) {
                const double x = bx + 2 + (bw - 6) * hist[b], y = levelY(b - 95.5);
                if (b == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.strokeStyle(kCurve);
            g.globalAlpha(0.7);
            g.lineWidth(1);
            g.lineJoin(LineJoin::Round);
            g.stroke();
            g.globalAlpha(1.0);
        }
        levelLines(g, bx, bw, hotOpen, hotClose);
        const double sug = suggestShown.get();
        if (any && sug > -95.0) {
            const double y = levelY(sug);
            g.strokeStyle(Theme::accent);
            g.globalAlpha(0.8);
            g.lineWidth(1);
            dashed(g, bx, bx + bw, y + 0.5, 2, 3);
            g.globalAlpha(1.0);
            g.fillStyle(Theme::accent);
            g.beginPath();
            g.moveTo(bx + bw - 2, y);
            g.lineTo(bx + bw - 9, y - 4.5);
            g.lineTo(bx + bw - 9, y + 4.5);
            g.closePath();
            g.fill();
        }
        g.restore();
        if (!any) aside(g, "nothing heard yet", bx + bw * 0.5, gy + gh * 0.5, TextAlign::Center);
        // The readouts, under the histogram.
        const double fl = floorShown.get(), ld = loudShown.get();
        label(g, "FLOOR", bx, ly + 8, TextAlign::Left);
        label(g, "LOUD", bx + 100, ly + 8, TextAlign::Left);
        label(g, "SUGGESTS", bx, ly + 38, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (fl > -95.0) {
            g.fillText(format("%.0f dB", fl), bx + 34, ly + 8.5);
            g.fillText(format("%.0f dB", ld), bx + 128, ly + 8.5);
            g.fillStyle(Theme::accent);
            g.fillText(format("%.0f dB", sug), bx + 56, ly + 38.5);
        } else {
            g.fillStyle(Theme::textFaint);
            g.fillText("--", bx + 34, ly + 8.5);
            g.fillText("--", bx + 128, ly + 8.5);
            g.fillText("--", bx + 56, ly + 38.5);
        }
        g.fillStyle(Theme::textMuted);
        g.monoFont(9);
        g.fillText(format("open %.0f%% · %.0f openings", openTime * 100.0, openings.value()), bx,
                   ly + 23);
        pill(g, bx + bw - 48, ly + 29, 48, 18, "SET", false, Theme::accent,
             hovered(bx + bw - 48, ly + 29, 48, 18));
    }

    // --- the gate panel ----------------------------------------------------------------------

    // Knobs: 0 threshold, 1 hysteresis, 2 range, 3 ratio, 4 attack, 5 hold,
    // 6 release, 7 key HPF, 8 key LPF.
    double knobX(int k) const {
        if (k < 4) return ax + 52 + k * 89;
        if (k < 7) return tx + 62 + (k - 4) * 86;
        return k == 7 ? kx + 58 : kx + 174;
    }
    double knobY(int k) const {
        if (k < 4) return py + 214;
        if (k < 7) return py + 172;
        return py + 120;
    }
    static constexpr double kKnobR = 26;

    void drawGate(Canvas& g) {
        panel(g, ax, py, aw, ph);
        heading(g, "Gate", ax + 16, py + 20);
        const bool exp = mode.index() == 1;
        for (int m = 0; m < 2; ++m) {
            const double x = ax + aw - 160 + m * 76;
            pill(g, x, py + 9, 72, 22, m == 0 ? "GATE" : "EXPAND", mode.index() == m, kLeaf,
                 hovered(x, py + 9, 72, 22));
        }
        dial(g, knobX(0), knobY(0), kKnobR, threshold.normalized(), kLeaf, hotKnob == 0,
             "THRESHOLD");
        g.globalAlpha(exp ? 0.35 : 1.0);
        dial(g, knobX(1), knobY(1), kKnobR, hysteresis.normalized(), kLeaf, hotKnob == 1,
             "HYSTERESIS");
        g.globalAlpha(1.0);
        dial(g, knobX(2), knobY(2), kKnobR, range.normalized(), kPetal, hotKnob == 2, "RANGE");
        g.globalAlpha(exp ? 1.0 : 0.35);
        dial(g, knobX(3), knobY(3), kKnobR, ratioSet.normalized(), kPetal, hotKnob == 3, "RATIO");
        g.globalAlpha(1.0);
        valueStyle(g);
        const double vy = knobY(0) + 40;
        g.fillText(format("%.1f dB", threshold.value()), knobX(0), vy);
        g.globalAlpha(exp ? 0.35 : 1.0);
        g.fillText(format("%.1f dB", hysteresis.value()), knobX(1), vy);
        g.globalAlpha(1.0);
        if (range <= -79.95)
            g.fillText("silence", knobX(2), vy);
        else
            g.fillText(format("%.1f dB", range.value()), knobX(2), vy);
        g.globalAlpha(exp ? 1.0 : 0.35);
        g.fillText(format("%.1f:1", ratioSet.value()), knobX(3), vy);
        g.globalAlpha(1.0);
        drawTransfer(g);
        // What the settings mean, in words.
        const double wx = cx + cw + 16;
        if (exp) {
            aside(g, format("under %.0f dBFS,", threshold.value()), wx, py + 78, TextAlign::Left);
            aside(g, format("each dB becomes %.1f", ratioSet.value()), wx, py + 96,
                  TextAlign::Left);
        } else {
            aside(g, format("opens above %.0f dBFS,", threshold.value()), wx, py + 78,
                  TextAlign::Left);
            aside(g, format("closes under %.0f after %.0f ms", closeLevel(), hold.value()), wx,
                  py + 96, TextAlign::Left);
        }
        if (range <= -79.95)
            aside(g, "and is silent closed", wx, py + 114, TextAlign::Left);
        else
            aside(g, format("and turns down %.0f dB closed", -range.value()), wx, py + 114,
                  TextAlign::Left);
    }

    // The transfer plot: level out for level in. The gate's two branches,
    // open and closed, with the jumps between them at the threshold and the
    // close level: its hysteresis as a loop. Expand's curve. A dot for now.
    static constexpr double cx = ax + 16;
    static constexpr double cy = py + 40;
    static constexpr double cw = 132;
    static constexpr double ch = 122;
    double curveX(double db) const { return cx + cw * clamp((db + 80.0) / 80.0, 0.0, 1.0); }
    double curveY(double db) const { return cy + ch * clamp(-db / 100.0, 0.0, 1.0); }

    void drawTransfer(Canvas& g) {
        well(g, cx, cy, cw, ch);
        const bool exp = mode.index() == 1;
        const double closed = range <= -79.95 ? -200.0 : range.value();
        const double t = threshold, c = closeLevel();
        g.save();
        g.clip(cx, cy, cw, ch);
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        for (int k = 1; k < 4; ++k) {
            g.beginPath();
            g.moveTo(curveX(-20.0 * k) + 0.5, cy);
            g.lineTo(curveX(-20.0 * k) + 0.5, cy + ch);
        }
        for (int k = 1; k < 5; ++k) {
            g.beginPath();
            g.moveTo(cx, curveY(-20.0 * k) + 0.5);
            g.lineTo(cx + cw, curveY(-20.0 * k) + 0.5);
            g.stroke();
        }
        g.strokeStyle(kGridLit);
        g.beginPath();
        g.moveTo(curveX(-80.0), curveY(-80.0));
        g.lineTo(curveX(0.0), curveY(0.0));
        g.stroke();
        g.lineJoin(LineJoin::Round);
        g.lineWidth(1.6);
        if (exp) {
            g.strokeStyle(kCurve);
            g.beginPath();
            for (int k = 0; k <= 80; ++k) {
                const double in = -80.0 + k;
                const double out = in + expanded(in, range <= -79.95 ? kSilence : range.value());
                if (k == 0)
                    g.moveTo(curveX(in), curveY(out));
                else
                    g.lineTo(curveX(in), curveY(out));
            }
            g.stroke();
        } else {
            // Closed: the input turned down, up to the threshold, then the jump.
            g.strokeStyle(kPetal);
            g.beginPath();
            g.moveTo(curveX(-80.0), curveY(-80.0 + closed));
            g.lineTo(curveX(t), curveY(t + closed));
            g.stroke();
            // Open: from the close level up, and the fall back at it.
            g.strokeStyle(kLeaf);
            g.beginPath();
            g.moveTo(curveX(c), curveY(c));
            g.lineTo(curveX(0.0), curveY(0.0));
            g.stroke();
            g.lineWidth(1);
            g.globalAlpha(0.7);
            g.strokeStyle(kLeaf);
            g.beginPath();
            g.moveTo(curveX(t) + 0.5, curveY(t + closed));
            g.lineTo(curveX(t) + 0.5, curveY(t) + 4);
            g.stroke();
            g.strokeStyle(kPetal);
            g.beginPath();
            g.moveTo(curveX(c) + 0.5, curveY(c));
            g.lineTo(curveX(c) + 0.5, curveY(c + closed) - 4);
            g.stroke();
            g.globalAlpha(1.0);
            arrowHead(g, curveX(t) + 0.5, curveY(t) + 2, -1.0, kLeaf);
            arrowHead(g, curveX(c) + 0.5, curveY(c + closed) - 2, 1.0, kPetal);
        }
        // Now: the key's level, and where the gain puts it.
        const double lv = keyNow.get();
        if (lv > -80.0) {
            g.fillStyle(Theme::accent);
            g.beginPath();
            g.circle(curveX(lv), curveY(lv + gainShown.get()), 3.2);
            g.fill();
        }
        g.restore();
        label(g, "IN", cx + cw - 4, cy + ch - 8, TextAlign::Right);
        label(g, "OUT", cx + 5, cy + 9, TextAlign::Left);
    }

    void arrowHead(Canvas& g, double x, double y, double dir, int colour) {
        g.fillStyle(colour);
        g.beginPath();
        g.moveTo(x, y);
        g.lineTo(x - 3.5, y - dir * 5.5);
        g.lineTo(x + 3.5, y - dir * 5.5);
        g.closePath();
        g.fill();
    }

    // --- the timing panel --------------------------------------------------------------------

    double lookMs() const {
        const int c = lookahead.index();
        return c == 1 ? 1.0 : (c == 2 ? 2.0 : (c == 3 ? 5.0 : 0.0));
    }

    // The sketch's height for a gain in dB, open at the top, closed at the
    // bottom.
    double sketchY(double db, double closed) const {
        return sy + 16 + (sh - 30) * clamp(db / closed, 0.0, 1.0);
    }

    // One event, sketched: the opening landing on the onset, the sound, the
    // hold, then the fade (and with Follow the span it may take).
    void drawSketch(Canvas& g) {
        well(g, sx, sy, sw, sh);
        const bool exp = mode.index() == 1;
        const double closed = range <= -79.95 ? kSilence : range.value();
        const double la = lookMs(), at = attack;
        const double onset = sx + 86;
        // As the audio does it: the opening ends a millisecond before the
        // onset where the lookahead allows, else as late as it can.
        const double early = min(at + 1.0, la);
        const double scale = 56.0 / max(max(at, early), 0.5);
        const double rampStart = onset - scale * early;
        const double rampEnd = rampStart + scale * at;
        const double soundEnd = max(rampEnd, onset) + 56;
        const double holdEnd = soundEnd + 4 + 60 * sqrt(hold / 1000.0);
        const double fadeMs = release * (-closed) / 60.0;
        const double fadeW = 12 + 96 * sqrt(min(fadeMs, 3000.0) / 3000.0);
        const double right = sx + sw - 8;
        const double top = sketchY(0.0, closed), bottom = sketchY(closed, closed);
        g.save();
        g.clip(sx, sy, sw, sh);
        // The onset, and the lookahead before it.
        g.strokeStyle(Theme::textFaint);
        g.lineWidth(1);
        g.globalAlpha(0.6);
        g.beginPath();
        for (double y = sy + 6; y < sy + sh - 4; y += 6) {
            g.moveTo(onset + 0.5, y);
            g.lineTo(onset + 0.5, y + 3);
        }
        g.stroke();
        g.globalAlpha(1.0);
        if (la > 0.0) {
            g.fillStyle(Theme::accent2);
            g.globalAlpha(0.08);
            g.fillRect(onset - scale * la, sy + 1, scale * la, sh - 2);
            g.globalAlpha(1.0);
        }
        // With Follow, the fades it may take: four times faster to four
        // times slower.
        if (follow.on() && !exp) {
            const double fast = 12 + 96 * sqrt(min(fadeMs * 0.25, 3000.0) / 3000.0);
            const double slow = 12 + 96 * sqrt(min(fadeMs * 4.0, 3000.0) / 3000.0);
            g.fillStyle(kLeaf);
            g.globalAlpha(0.18);
            g.beginPath();
            g.moveTo(holdEnd, top);
            g.lineTo(holdEnd + fast, bottom);
            g.lineTo(holdEnd + slow, bottom);
            g.closePath();
            g.fill();
            g.globalAlpha(0.6);
            g.strokeStyle(kLeaf);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(holdEnd, top);
            g.lineTo(holdEnd + slow, bottom);
            g.moveTo(holdEnd, top);
            g.lineTo(holdEnd + fast, bottom);
            g.stroke();
            g.globalAlpha(1.0);
        }
        // The gain.
        const double ampClosed = closed <= kSilence ? 0.0 : dbToGain(closed);
        g.beginPath();
        g.moveTo(sx, bottom);
        g.lineTo(rampStart, bottom);
        for (int k = 1; k <= 24; ++k) {
            const double u = k / 24.0;
            const double a = ampClosed + (1.0 - ampClosed) * (0.5 - 0.5 * cos(pi * u));
            const double db = a <= 0.0 ? closed : max(closed, gainToDb(a));
            g.lineTo(rampStart + (rampEnd - rampStart) * u, sketchY(db, closed));
        }
        g.lineTo(holdEnd, top);
        g.quadraticCurveTo(holdEnd + 3, top, holdEnd + 5, sketchY(closed * 5 / fadeW, closed));
        g.lineTo(holdEnd + fadeW, bottom);
        g.lineTo(right + 10, bottom);
        g.strokeStyle(kCurve);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.stroke();
        g.restore();
        // The hold, marked under the open stretch.
        g.strokeStyle(Theme::textFaint);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(soundEnd, top - 6);
        g.lineTo(soundEnd, top - 3);
        g.lineTo(holdEnd, top - 3);
        g.lineTo(holdEnd, top - 6);
        g.stroke();
        label(g, "HOLD", holdEnd + 4, top - 4, TextAlign::Left);
        label(g, "ONSET", onset - 4, sy + 9, TextAlign::Right);
        if (follow.on() && !exp)
            label(g, "FOLLOW", holdEnd + 12 + 96 * sqrt(min(fadeMs * 4.0, 3000.0) / 3000.0) + 6,
                  bottom - 8, TextAlign::Left);
    }

    void drawTiming(Canvas& g) {
        panel(g, tx, py, tw, ph);
        heading(g, "Timing", tx + 16, py + 20);
        aside(g, "one sound, sketched", tx + tw - 16, py + 20, TextAlign::Right);
        drawSketch(g);
        const bool exp = mode.index() == 1;
        dial(g, knobX(4), knobY(4), kKnobR, attack.normalized(), Theme::accent2, hotKnob == 4,
             "ATTACK");
        dial(g, knobX(5), knobY(5), kKnobR, hold.normalized(), Theme::accent2, hotKnob == 5,
             "HOLD");
        dial(g, knobX(6), knobY(6), kKnobR, release.normalized(), Theme::accent2, hotKnob == 6,
             "RELEASE");
        valueStyle(g);
        fillTime(g, attack, knobX(4), knobY(4) + 42);
        fillTime(g, hold, knobX(5), knobY(5) + 42);
        fillTime(g, release, knobX(6), knobY(6) + 42);
        const double fx = tx + tw - 70;
        g.globalAlpha(exp ? 0.35 : 1.0);
        pill(g, fx, knobY(6) - 11, 56, 22, "FOLLOW", follow.on(), kLeaf,
             hovered(fx, knobY(6) - 11, 56, 22));
        g.globalAlpha(1.0);
        label(g, "LOOKAHEAD", tx + 16, py + ph - 24, TextAlign::Left);
        for (int k = 0; k < 4; ++k) {
            const double x = tx + 92 + k * 58;
            const char* name = k == 0 ? "OFF" : (k == 1 ? "1 ms" : (k == 2 ? "2 ms" : "5 ms"));
            pill(g, x, py + ph - 35, 54, 22, name, lookahead.index() == k, Theme::accent2,
                 hovered(x, py + ph - 35, 54, 22));
        }
    }

    // --- the key panel -----------------------------------------------------------------------

    void drawKey(Canvas& g) {
        panel(g, kx, py, kw, ph);
        heading(g, "Key", kx + 16, py + 20);
        const bool ext = key.index() == 1;
        for (int k = 0; k < 2; ++k) {
            const double x = kx + 16 + k * 102;
            pill(g, x, py + 38, 98, 22, k == 0 ? "INTERNAL" : "EXTERNAL", key.index() == k,
                 Theme::accent2, hovered(x, py + 38, 98, 22));
        }
        const bool hpOff = keyHpf <= 20.5, lpOff = keyLpf >= 19900.0;
        dial(g, knobX(7), knobY(7), kKnobR, keyHpf.normalized(), Theme::accent2, hotKnob == 7,
             "HIGH-PASS");
        dial(g, knobX(8), knobY(8), kKnobR, 1.0 - keyLpf.normalized(), Theme::accent2, hotKnob == 8,
             "LOW-PASS");
        valueStyle(g);
        if (hpOff)
            g.fillText("off", knobX(7), knobY(7) + 42);
        else
            fillHz(g, keyHpf, knobX(7), knobY(7) + 42);
        if (lpOff)
            g.fillText("off", knobX(8), knobY(8) + 42);
        else
            fillHz(g, keyLpf, knobX(8), knobY(8) + 42);
        label(g, "LISTEN", kx + 16, py + 196, TextAlign::Left);
        for (int k = 0; k < 3; ++k) {
            const double x = kx + 16 + k * 68;
            const char* name = k == 0 ? "OFF" : (k == 1 ? "KEY" : "DELTA");
            pill(g, x, py + 206, 64, 22, name, listen.index() == k, k == 0 ? kLinen : Theme::accent,
                 hovered(x, py + 206, 64, 22));
        }
        const char* says = listen.index() == 1     ? "playing what it listens to"
                           : listen.index() == 2   ? "playing only what it takes away"
                           : ext && !keyLive.get() ? "external, but nothing is routed to it"
                                                   : (ext ? "listening to the sidechain"
                                                          : "listening to its own input");
        aside(g, says, kx + 16, py + ph - 18, TextAlign::Left);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawMain(g);
        drawGate(g);
        drawTiming(g);
        drawKey(g);
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
        if (k == 0) return threshold.normalized();
        if (k == 1) return hysteresis.normalized();
        if (k == 2) return range.normalized();
        if (k == 3) return ratioSet.normalized();
        if (k == 4) return attack.normalized();
        if (k == 5) return hold.normalized();
        if (k == 6) return release.normalized();
        if (k == 7) return keyHpf.normalized();
        // The low-pass knob turns the other way: clockwise closes it.
        return 1.0 - keyLpf.normalized();
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, threshold, v);
        if (k == 1) act(how, hysteresis, v);
        if (k == 2) act(how, range, v);
        if (k == 3) act(how, ratioSet, v);
        if (k == 4) act(how, attack, v);
        if (k == 5) act(how, hold, v);
        if (k == 6) act(how, release, v);
        if (k == 7) act(how, keyHpf, v);
        if (k == 8) act(how, keyLpf, 1.0 - v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 9; ++k) {
            const double dx = x - knobX(k), dy = y - knobY(k);
            const double r = kKnobR + 6;
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
                threshold.edit(levelAt(p.y()));
            } else if (dragging == 3) {
                hysteresis.edit(threshold - levelAt(p.y()));
            } else if (dragging == 4) {
                range.edit(-80.0 * clamp((p.y() - ly - 4) / (lh - 8), 0.0, 1.0));
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
            if (p.over(gx, gy, gw, gh) || p.over(bx, gy, bw, gh)) {
                threshold.edit(threshold + notches * 0.5 * fine);
                return;
            }
            if (p.over(gx, ly, gw, lh)) {
                range.edit(range + notches * 1.0 * fine);
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
        if (p.over(gx, gy, gw, gh) || p.over(bx, gy, bw, gh)) {
            const bool exp = mode.index() == 1;
            const double ty = levelY(threshold), cy = levelY(closeLevel());
            if (!exp && abs(y - cy) < 6 && abs(y - ty) >= abs(y - cy)) {
                if (p.doubleClick())
                    hysteresis.reset();
                else
                    dragging = 3;
                return;
            }
            if (p.doubleClick()) {
                threshold.reset();
                return;
            }
            dragging = 2;
            // A press away from the line sets the threshold where it lands.
            if (abs(y - ty) >= 6) threshold.edit(levelAt(y));
            return;
        }
        if (p.over(gx, ly, gw, lh)) {
            if (p.doubleClick()) {
                range.reset();
                return;
            }
            dragging = 4;
            range.edit(-80.0 * clamp((y - ly - 4) / (lh - 8), 0.0, 1.0));
            return;
        }
        if (p.over(bx + bw - 48, ly + 29, 48, 18)) {
            if (suggested > -95.0) threshold.edit(suggested);
            return;
        }
        for (int m = 0; m < 2; ++m)
            if (p.over(ax + aw - 160 + m * 76, py + 9, 72, 22)) {
                act(0, mode, m);
                return;
            }
        if (p.over(tx + tw - 70, knobY(6) - 11, 56, 22)) {
            act(0, follow, follow.on() ? 0 : 1);
            return;
        }
        for (int c = 0; c < 4; ++c)
            if (p.over(tx + 92 + c * 58, py + ph - 35, 54, 22)) {
                act(0, lookahead, c);
                return;
            }
        for (int c = 0; c < 2; ++c)
            if (p.over(kx + 16 + c * 102, py + 38, 98, 22)) {
                act(0, key, c);
                return;
            }
        for (int c = 0; c < 3; ++c)
            if (p.over(kx + 16 + c * 68, py + 206, 64, 22)) {
                act(0, listen, c);
                return;
            }
    }
};
