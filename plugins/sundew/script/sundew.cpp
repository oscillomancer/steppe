// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Sundew: a compressor with six characters.
//
// Each character is a whole gain cell, not a preset: its own detector, its
// own law and its own way of moving.
//
//   Clean    feedforward, peak, exact: what the controls say and nothing more.
//   Glue     feedforward, RMS, with a second slow stage that charges under
//            sustained compression, so short peaks let go quickly and long
//            passages slowly (the bus compressor's auto release).
//   Opto     feedback, RMS, a light cell's two-stage release: it drops fast,
//            then lets the last of the reduction go over seconds, and the
//            longer it worked the longer it takes. Big jumps grab faster.
//   FET      feedback, peak, very fast, its ratio steepening above the
//            threshold and its release quickening when it works hard.
//   Vari-Mu  feedforward, RMS, the ratio growing with the reduction (no
//            fixed ratio, a knee as wide as the music), moving in S-curves.
//   Bloom    upward: lifts what is quieter than the threshold, up to the
//            range, and leaves the floor (noise, silence) where it is.
//
// The feedback characters solve for the output they make, sample by sample,
// so a fast loop never rings, and their ratio is the ratio heard. Ratio,
// attack and release mean the same in every character; the transfer curve
// draws exactly what each does with a steady level, and the ballistics
// panel plays a burst through the character's own cell to show how it moves.
//
// The key is the input or the sidechain, high-passed if wanted, linked
// across channels or not, left/right or mid/side. Lookahead (a restart)
// delays the audio so the detector sees what is coming. Colour adds the
// character's harmonics, more as it works harder. Match keeps the output as
// loud as the input, measured over seconds, so bypassing it is a fair test.
// Release can follow the tempo. The history scrolls four seconds of input,
// output and gain change, with the bars and beats when the transport plays.

// --- the law and the cell --------------------------------------------------------------

constexpr int kClean = 0;
constexpr int kGlue = 1;
constexpr int kOpto = 2;
constexpr int kFet = 3;
constexpr int kVariMu = 4;
constexpr int kBloom = 5;

// max(0, d), its corner rounded over w dB: a soft knee.
double softOver(double d, double w) {
    if (w <= 0.0) return max(0.0, d);
    if (d <= -0.5 * w) return 0.0;
    if (d >= 0.5 * w) return d;
    const double t = d + 0.5 * w;
    return t * t / (2.0 * w);
}

// min(a, cap), its corner rounded: a range that bends instead of clipping.
double softCap(double a, double cap) {
    const double w = min(3.0, 0.5 * cap);
    if (a <= cap - w) return a;
    if (a >= cap + w) return cap;
    const double t = a - cap + w;
    return a - t * t / (4.0 * w);
}

// The share of the way a one-pole moves each sample, for a time constant.
double stepFor(double ms, double rate) { return 1.0 - exp(-1000.0 / (max(ms, 0.001) * rate)); }

bool isFeedback(int ch) { return ch == kOpto || ch == kFet; }
bool usesRms(int ch) { return ch == kGlue || ch == kOpto || ch == kVariMu || ch == kBloom; }

// A character's static law and its time constants at one sample rate.
struct Law {
    int ch = 0;
    double thr = -18.0, ratio = 3.0, knee = 6.0, range = 24.0;
    double attackMs = 10.0, releaseMs = 150.0, rate = 48000.0;
    double kA = 0.0, kR = 0.0, kA2 = 0.0, kR2 = 0.0, kSlowA = 0.0, kSlowR = 0.0, kRms = 0.0;
    double hold = 0.0;  // the slow stage's share of the reduction (Glue, Opto)

    void set(int c, double t, double r, double k, double rg, double a, double rl, double fs) {
        ch = c;
        thr = t;
        ratio = max(r, 1.0);
        knee = k;
        range = rg;
        attackMs = a;
        releaseMs = rl;
        rate = fs;
        kA = stepFor(a, fs);
        kR = stepFor(rl, fs);
        // Two stages in a row reach 63% in about twice the time of one.
        kA2 = stepFor(a * 0.47, fs);
        kR2 = stepFor(rl * 0.47, fs);
        kSlowA = stepFor(c == kOpto ? 600.0 : 350.0, fs);
        kSlowR = stepFor(min(rl * (c == kOpto ? 12.0 : 6.0), 8000.0), fs);
        hold = c == kOpto ? 0.6 : 0.75;
        kRms = stepFor(c == kVariMu ? 8.0 : (c == kBloom ? 12.0 : (c == kOpto ? 5.0 : 3.0)), fs);
    }

    double width() const {
        if (ch == kGlue) return knee + 3.0;
        if (ch == kOpto) return knee + 10.0;
        if (ch == kVariMu) return knee + 14.0;
        if (ch == kFet) return knee * 0.5;
        return knee;
    }

    // What the law asks for at a detector level, in dB, 0 or more: the cut,
    // or Bloom's lift. A feedback character's detector hears its output, so
    // its slope is ratio - 1: the ratio heard is the ratio set.
    double amount(double level) const {
        if (ch == kBloom) {
            double lift = softOver(thr - level, knee) * (1.0 - 1.0 / ratio);
            const double floor = thr - 30.0;
            if (level < floor) lift *= clamp(1.0 - (floor - level) / 24.0, 0.0, 1.0);
            return softCap(lift, range);
        }
        const double over = softOver(level - thr, width());
        double cut = 0.0;
        if (ch == kVariMu) {
            const double r = 1.0 + (ratio - 1.0) * (0.25 + 0.75 * (1.0 - exp(-over / 9.0)));
            cut = over * (1.0 - 1.0 / r);
        } else if (isFeedback(ch)) {
            cut = over * (ratio - 1.0);
            if (ch == kFet) cut *= 1.0 + over / 12.0;
        } else {
            cut = over * (1.0 - 1.0 / ratio);
        }
        return softCap(cut, range);
    }

    // The gain change (dB) a steady input level settles at: the transfer curve.
    double steady(double input) const {
        if (ch == kBloom) return amount(input);
        if (!isFeedback(ch)) return -amount(input);
        // The cut c with c = amount(input - c), found by halving.
        double lo = 0.0, hi = range + 1.0;
        for (int k = 0; k < 40; ++k) {
            const double c = 0.5 * (lo + hi);
            if (c - amount(input - c) > 0.0)
                hi = c;
            else
                lo = c;
        }
        return -0.5 * (lo + hi);
    }
};

// One channel's gain cell: the amount it applies (dB, 0 or more) and how it
// gets there.
struct Cell {
    double fast = 0.0, slow = 0.0, second = 0.0;
    double gain = 0.0;

    void settle(double e) {
        fast = e;
        slow = e;
        second = e;
        gain = e;
    }

    // One sample toward what the law asks at `level` (dB): the input's for a
    // feedforward character, and for a feedback one the key's before this
    // cell's gain (it solves for the output it makes).
    double step(double level, const Law& w) {
        const int ch = w.ch;
        if (ch == kBloom) {
            // Its attack is how fast the lift gives way to a louder sound.
            const double t = w.amount(level);
            fast += (t < fast ? w.kA : w.kR) * (t - fast);
            if (fast < 1e-9) fast = 0.0;
            gain = fast;
            return gain;
        }
        if (isFeedback(ch)) {
            const double held = ch == kOpto ? w.hold * slow : 0.0;
            const double t0 = w.amount(level - max(fast, held));
            double k = 0.0;
            if (t0 > fast) {
                k = ch == kOpto ? stepFor(w.attackMs * (1.5 - min(1.0, (t0 - fast) / 12.0)), w.rate)
                                : w.kA;
            } else {
                k = ch == kFet ? stepFor(w.releaseMs / (1.0 + fast / 8.0), w.rate) : w.kR;
            }
            if (held > fast) {
                // The slow stage sets the output: there is no loop to solve.
                fast += k * (t0 - fast);
            } else {
                const double e0 = fast;
                double e = e0;
                for (int n = 0; n < 3; ++n) {
                    const double f = e - e0 - k * (w.amount(level - e) - e0);
                    const double slope =
                        (w.amount(level - e + 0.01) - w.amount(level - e - 0.01)) * 50.0;
                    e = max(0.0, e - f / (1.0 + k * slope));
                }
                fast = e;
            }
            if (fast < 1e-9) fast = 0.0;
            if (ch == kOpto) slow += (fast > slow ? w.kSlowA : w.kSlowR) * (fast - slow);
            gain = max(fast, ch == kOpto ? w.hold * slow : 0.0);
            return gain;
        }
        const double t = w.amount(level);
        if (ch == kVariMu) {
            fast += (t > fast ? w.kA2 : w.kR2) * (t - fast);
            second += (fast > second ? w.kA2 : w.kR2) * (fast - second);
            if (second < 1e-9) second = 0.0;
            gain = second;
            return gain;
        }
        if (ch == kClean) {
            // A decoupled peak detector: the peak held and released, then
            // the attack smooths what it holds, so a steady tone gets a
            // steady gain and not one that ripples with each cycle.
            fast = max(t, fast + w.kR * (t - fast));
            second += w.kA * (fast - second);
            if (second < 1e-9) second = 0.0;
            gain = second;
            return gain;
        }
        fast += (t > fast ? w.kA : w.kR) * (t - fast);
        if (fast < 1e-9) fast = 0.0;
        slow += (t > slow ? w.kSlowA : w.kSlowR) * (t - slow);
        gain = max(fast, w.hold * slow);
        return gain;
    }
};

// Colour: the character's harmonics, second (even) and third (odd).
double evenFor(int ch) {
    if (ch == kGlue) return 0.02;
    if (ch == kOpto) return 0.06;
    if (ch == kFet) return 0.05;
    if (ch == kVariMu) return 0.11;
    return 0.0;
}
double oddFor(int ch) {
    if (ch == kGlue) return -0.10;
    if (ch == kOpto) return -0.03;
    if (ch == kFet) return -0.16;
    if (ch == kVariMu) return -0.05;
    return 0.0;
}

// Release locked to the tempo: a note value in beats (quarter notes).
double syncBeats(int sync, double barBeats) {
    if (sync == 1) return 0.125;
    if (sync == 2) return 0.25;
    if (sync == 3) return 0.5;
    if (sync == 4) return 1.0;
    if (sync == 5) return 2.0;
    return barBeats;
}

// --- the look ----------------------------------------------------------------------------

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
constexpr int kFaceTop = 0xF3E7CD;  // the meter's face: lamp-lit parchment
constexpr int kFaceBottom = 0xD9C49C;
constexpr int kFaceInk = 0x2B1F17;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;

int characterColour(int ch) {
    if (ch == kGlue) return 0x9DB36E;    // moss
    if (ch == kOpto) return 0xE29A52;    // marigold
    if (ch == kFet) return 0xD9775A;     // clay
    if (ch == kVariMu) return 0xAB91C6;  // heather
    if (ch == kBloom) return 0xCF8099;   // madder
    return 0xD5C3A5;                     // Clean: linen
}

constexpr int kCols = 512;   // history columns, a hundredth of a second each
constexpr int kShown = 400;  // four seconds across the graph
constexpr int kGlyph = 240;  // points of the ballistics plot
constexpr int kIcon = 44;    // points of each character's little plot

struct Sundew : Effect {
    static constexpr Info info = {
        .name = "Sundew",
        .id = "com.suppleaudio.sundew",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Compressor with six characters, from transparent to upward, a key input, "
            "lookahead, tempo-locked release and loudness matching.",
        .features = {"compressor"},
        .sidechain = true};

    Param character =
        choice("Character", {"Clean", "Glue", "Opto", "FET", "Vari-Mu", "Bloom"}, 0)
            .keys({"clean", "glue", "opto", "fet", "vari_mu", "bloom"})
            .in("Compressor")
            .help(
                "The gain cell: Clean exact, Glue a bus compressor, Opto smooth and slow to let "
                "go, "
                "FET fast and biting, Vari-Mu gentle and growing, Bloom lifting quiet detail.");
    Param threshold = decibels("Threshold", -60, 0, -18)
                          .modulated()
                          .in("Compressor")
                          .help("The level it works above (Bloom: below), in dBFS.");
    Param ratioSet =
        ratio("Ratio", 1, 20, 3)
            .modulated()
            .withKey("ratio")
            .in("Compressor")
            .help("How hard it works: 4 lets 1 dB through for every 4 dB over the threshold.");
    Param attack = milliseconds("Attack", 0.05, 250, 10)
                       .modulated()
                       .in("Compressor")
                       .help("How fast it reacts to a louder sound.");
    Param sync = choice("Sync", {"Off", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar"}, 0)
                     .keys({"off", "n32", "n16", "n8", "n4", "n2", "bar"})
                     .in("Compressor")
                     .help("Locks the release to the tempo: it recovers within that note value.");
    Param release =
        milliseconds("Release", 5, 3000, 150)
            .modulated()
            .in("Compressor")
            .activeWhen(sync, {"Off"})
            .help("How fast it lets go once the sound falls (ignored while Sync is on).");
    Param knee =
        decibels("Knee", 0, 24, 6)
            .modulated()
            .in("Compressor")
            .help("How gradually it starts, in dB around the threshold; 0 is a hard corner.");
    Param range =
        decibels("Range", 1, 40, 24)
            .modulated()
            .in("Compressor")
            .help("The most it changes the level: the deepest cut, or Bloom's highest lift.");

    Param key =
        choice("Key", {"Internal", "External"}, 0)
            .in("Key")
            .help("What it listens to: its own input, or the sidechain input when connected.");
    Param keyHpf =
        frequency("Key HPF", 20, 800, 20)
            .modulated()
            .in("Key")
            .help("A high-pass on what it listens to, so bass does not drive it; 20 Hz is off.");
    Param link = percent("Link", 100)
                     .modulated()
                     .in("Key")
                     .help("How much the channels share one gain: 100% keeps the image still.");
    Param stereo = choice("Stereo", {"Left/Right", "Mid/Side"}, 0)
                       .keys({"lr", "ms"})
                       .in("Key")
                       .help(
                           "Works on left and right, or on mid and side (link below 100% to treat "
                           "them apart).");
    Param lookahead =
        choice("Lookahead", {"Off", "1 ms", "2 ms", "5 ms"}, 0)
            .keys({"off", "ms1", "ms2", "ms5"})
            .restarts()
            .in("Key")
            .help("Delays the audio so it reacts before a peak arrives; adds that latency.");
    Param listen =
        choice("Listen", {"Off", "Key", "Delta"}, 0)
            .notAutomatable()
            .in("Key")
            .help("Plays what it listens to, or the difference it makes, instead of the output.");

    Param colour =
        percent("Colour", 0)
            .modulated()
            .in("Output")
            .activeUnless(character, {"Clean", "Bloom"})
            .help(
                "The character's harmonics, rising as it works harder (Clean and Bloom add none).");
    Param output = decibels("Output", -24, 24, 0)
                       .modulated()
                       .smoothed(20)
                       .in("Output")
                       .help("The level out, after the mix and the match.");
    Param mix = percent("Mix", 100)
                    .modulated()
                    .smoothed(20)
                    .in("Output")
                    .help("Compressed against dry: below 100% is parallel compression.");
    Param match = toggle("Match", false)
                      .in("Output")
                      .help(
                          "Keeps the output as loud as the input, measured over seconds, for a "
                          "fair comparison.");

    Param gainChange =
        decibels("Gain Change", -48, 24, 0)
            .meter()
            .help("The gain it applies: negative compresses, positive lifts (Bloom).");
    Param loudnessChange =
        decibels("Loudness Change", -24, 24, 0)
            .meter()
            .help("How much louder the output is than the input, before the match.");

    void factoryPatches(Patches& p) {
        p.add("Clean Start");
        p.add("Vocal Leveler")
            .set(character, "opto")
            .set(threshold, -26)
            .set(ratioSet, 3)
            .set(attack, 10)
            .set(release, 120)
            .set(colour, 20)
            .set(match, true);
        p.add("Drum Bus Glue")
            .set(character, "glue")
            .set(threshold, -20)
            .set(ratioSet, 4)
            .set(attack, 30)
            .set(release, 100)
            .set(keyHpf, 90)
            .set(mix, 75)
            .set(match, true);
        p.add("Snare Bite")
            .set(character, "fet")
            .set(threshold, -28)
            .set(ratioSet, 8)
            .set(attack, 0.4)
            .set(release, 60)
            .set(colour, 35);
        p.add("Parallel Smash")
            .set(character, "fet")
            .set(threshold, -40)
            .set(ratioSet, 20)
            .set(attack, 0.1)
            .set(release, 50)
            .set(colour, 50)
            .set(mix, 30);
        p.add("Mix Bus Warmth")
            .set(character, "vari_mu")
            .set(threshold, -16)
            .set(ratioSet, 2)
            .set(attack, 25)
            .set(release, 400)
            .set(knee, 10)
            .set(range, 6)
            .set(colour, 15)
            .set(keyHpf, 60);
        p.add("Bass Steady")
            .set(character, "opto")
            .set(threshold, -24)
            .set(ratioSet, 4)
            .set(attack, 20)
            .set(release, 200);
        p.add("Kick Duck")
            .set(character, "clean")
            .set(key, "external")
            .set(threshold, -30)
            .set(ratioSet, 10)
            .set(attack, 1)
            .set(sync, "n8")
            .set(knee, 0)
            .set(range, 12);
        p.add("Detail Bloom")
            .set(character, "bloom")
            .set(threshold, -30)
            .set(ratioSet, 2.5)
            .set(attack, 3)
            .set(release, 250)
            .set(range, 10);
        p.add("Mid-Side Master")
            .set(character, "glue")
            .set(stereo, "ms")
            .set(link, 50)
            .set(threshold, -14)
            .set(ratioSet, 1.5)
            .set(attack, 30)
            .set(release, 300)
            .set(match, true);
        p.add("Peak Catcher")
            .set(character, "clean")
            .set(lookahead, "ms1")
            .set(threshold, -6)
            .set(ratioSet, 20)
            .set(attack, 0.1)
            .set(release, 60)
            .set(knee, 2);
    }

    // --- audio ------------------------------------------------------------------------------

    Law law;
    Array<Cell, 2> cell;
    Array<Biquad, 2> keyFilter;
    Array<DelayLine, 2> delay;
    Array<DelayLine, 2> keyDelay;
    Array<double, 2> meanSquare;
    Array<double, 2> colourLow;  // colour's DC blocker
    Array<double, 2> bridge;     // dB that eases a change of character, gliding to 0
    double kBridge = 0.0;
    double rate = 48000.0;
    int ahead = 0;  // lookahead, samples
    int ch = 0;
    int syncIndex = 0;
    double syncTempo = 0.0, syncBar = 0.0;
    double releaseMs = 150.0;
    bool filterOn = false;
    double linkAmount = 1.0;
    double colourAmount = 0.0, even = 0.0, odd = 0.0;
    bool midSide = false;
    int listenMode = 0;
    // Key and delta shares; the normal output supplies the remaining share.
    // A new selection fades from the mixture last heard, even mid-fade.
    double listenKey = 0.0, listenDelta = 0.0;
    double listenFromKey = 0.0, listenFromDelta = 0.0;
    int listenLength = 480, listenAt = 480;
    bool unplayed = true;
    // Match: the input's and output's power over about a second, and the gain between.
    double powerIn = 0.0, powerOut = 0.0, matchDb = 0.0, matchNow = 0.0;
    double kPower = 0.0, kMatch = 0.0, kMatchOff = 0.0, kDc = 0.0;
    // The editor's history: per column, the peak in and out and the gain change.
    double colLength = 480.0, colAt = 0.0;
    double colIn = 0.0, colOut = 0.0, colGain = 0.0;
    int colBeat = 0;
    int hw = 0, filled = 0;
    double beatPos = 0.0, beatNext = -1.0;  // where the next block should start
    bool wasPlaying = false;
    double fall = 0.999;
    double peakIn0 = 0.0, peakIn1 = 0.0, peakOut0 = 0.0, peakOut1 = 0.0, keyPeak = 0.0;

    SharedArray<float, 512> hIn, hOut, hGain, hBeat;
    Shared<int> hHead, hFilled;
    Shared<double> keyDb, gainDb, inLeft, inRight, outLeft, outRight, matchShown, loudShown;
    Shared<double> tempoShown, barShown;
    Shared<bool> keyLive;

    void prepare() {
        rate = sampleRate();
        const int choice = lookahead.index();
        const double ms = choice == 1 ? 1.0 : (choice == 2 ? 2.0 : (choice == 3 ? 5.0 : 0.0));
        ahead = roundToInt(ms * 0.001 * rate);
        setLatency(ahead);
        const int room = roundToInt(0.005 * rate) + 8;
        for (int c = 0; c < 2; ++c) {
            delay[c].prepare(room);
            keyDelay[c].prepare(room);
        }
        kPower = stepFor(600.0, rate);
        kBridge = stepFor(30.0, rate);
        kMatch = stepFor(1000.0, rate);
        kMatchOff = stepFor(40.0, rate);
        kDc = stepFor(1000.0 / (twoPi * 8.0), rate);
        listenLength = max(1, roundToInt(0.01 * rate));
        listenAt = listenLength;
        listenKey = 0.0;
        listenDelta = 0.0;
        listenFromKey = 0.0;
        listenFromDelta = 0.0;
        colLength = rate * 0.01;
        fall = exp(-1.0 / (0.3 * rate));
        // A restart prepares an instance that has played: start it afresh.
        for (int c = 0; c < 2; ++c) {
            cell[c].settle(0.0);
            keyFilter[c].clear();
            meanSquare[c] = 0.0;
            colourLow[c] = 0.0;
            bridge[c] = 0.0;
        }
        ch = character.index();
        powerIn = 0.0;
        powerOut = 0.0;
        matchDb = 0.0;
        matchNow = 0.0;
        peakIn0 = 0.0;
        peakIn1 = 0.0;
        peakOut0 = 0.0;
        peakOut1 = 0.0;
        keyPeak = 0.0;
        colAt = 0.0;
        colIn = 0.0;
        colOut = 0.0;
        colGain = 0.0;
        colBeat = 0;
        wasPlaying = false;
        unplayed = true;
    }

    void update() {
        const int was = ch;
        ch = character.index();
        syncIndex = sync.index();
        releaseMs = release;
        if (syncIndex > 0) releaseMs = syncMs(syncTempo > 0.0 ? syncTempo : 120.0, syncBar);
        law.set(ch, threshold, ratioSet, knee, range, attack, releaseMs, rate);
        filterOn = keyHpf > 20.5;
        for (int c = 0; c < 2; ++c) keyFilter[c].highpass(keyHpf, 0.707);
        linkAmount = link;
        colourAmount = colour;
        even = evenFor(ch);
        odd = oddFor(ch);
        midSide = stereo.index() == 1;
        const int wantedListen = listen.index();
        if (wantedListen != listenMode) {
            listenMode = wantedListen;
            listenFromKey = listenKey;
            listenFromDelta = listenDelta;
            listenAt = 0;
        }
        // A new character takes over where the last one left the gain: a
        // cell going the same way starts from its amount; between cutting
        // and lifting (Bloom), the new cell starts at rest and the gain
        // glides from where it was.
        if (ch != was) {
            const bool flips = (ch == kBloom) != (was == kBloom);
            for (int c = 0; c < 2; ++c) {
                const double before = (was == kBloom ? 1.0 : -1.0) * cell[c].gain + bridge[c];
                cell[c].settle(flips ? 0.0 : cell[c].gain);
                bridge[c] = unplayed ? 0.0 : before - (ch == kBloom ? 1.0 : -1.0) * cell[c].gain;
            }
        }
        if (unplayed) {
            listenKey = listenMode == 1 ? 1.0 : 0.0;
            listenDelta = listenMode == 2 ? 1.0 : 0.0;
            listenAt = listenLength;
        }
    }

    double syncMs(double tempo, double barBeats) {
        // Within the note: three time constants, 95% of the way back.
        return 60000.0 / tempo * syncBeats(syncIndex, barBeats) / 3.0;
    }

    void process(Block& io) {
        unplayed = false;
        const Transport& t = io.transport;
        const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        if (syncIndex > 0 && (t.tempo != syncTempo || barBeats != syncBar)) {
            syncTempo = t.tempo;
            syncBar = barBeats;
            releaseMs = syncMs(t.tempo > 0.0 ? t.tempo : 120.0, barBeats);
            law.set(ch, threshold, ratioSet, knee, range, attack, releaseMs, rate);
        }
        const bool stereoIn = io.channels > 1;
        const bool external = key.index() == 1 && io.sidechainConnected;
        const bool rms = usesRms(ch);
        const bool bloom = ch == kBloom;
        const double beatStep = t.tempo / 60.0 / rate;
        beatPos = t.beats;
        // A beat at the block's first sample counts only if the song ran on
        // into it (not after a jump, a loop or a start).
        double prevPos =
            wasPlaying && abs(beatPos - beatNext) < 1e-3 ? beatPos - beatStep : beatPos;
        double extreme = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            double l = io[0][i];
            double r = stereoIn ? io[1][i] : l;
            if (!isFinite(l)) l = 0.0;
            if (!isFinite(r)) r = 0.0;
            double kl = l, kr = r;
            if (external) {
                kl = io.sidechain(0)[i];
                kr = stereoIn ? io.sidechain(1)[i] : kl;
                if (!isFinite(kl)) kl = 0.0;
                if (!isFinite(kr)) kr = 0.0;
            }
            // The audio, delayed by the lookahead; the key is not.
            // Lookahead changes restart and prepare the buffers, so there
            // is no history to retain while it is off.
            double dl = l, dr = r;
            if (ahead > 0) {
                delay[0].push(l);
                delay[1].push(r);
                dl = delay[0].read(ahead);
                dr = delay[1].read(ahead);
            }
            double k0 = midSide ? 0.5 * (kl + kr) : kl;
            double k1 = midSide ? 0.5 * (kl - kr) : kr;
            if (filterOn) {
                k0 = keyFilter[0].process(k0);
                k1 = keyFilter[1].process(k1);
            }
            if (ahead > 0) {
                keyDelay[0].push(midSide ? k0 + k1 : k0);
                keyDelay[1].push(midSide ? k0 - k1 : k1);
            }
            keyPeak = max(max(abs(k0), abs(k1)), keyPeak * fall);

            // The detector: each channel's level in dB, then linked.
            double lev0 = 0.0, lev1 = 0.0;
            if (rms) {
                meanSquare[0] += law.kRms * (k0 * k0 - meanSquare[0]);
                meanSquare[1] += law.kRms * (k1 * k1 - meanSquare[1]);
                lev0 = max(-120.0, 10.0 * log10(meanSquare[0] + 1e-30) + 3.0103);
                lev1 = max(-120.0, 10.0 * log10(meanSquare[1] + 1e-30) + 3.0103);
            } else {
                lev0 = gainToDb(abs(k0));
                lev1 = gainToDb(abs(k1));
            }
            if (!stereoIn) lev1 = lev0;
            const double loud = max(lev0, lev1);
            lev0 += linkAmount * (loud - lev0);
            lev1 += linkAmount * (loud - lev1);
            // Identical cells hearing the same level take the same step.
            // Compare the state too: linking after independent processing
            // must let each channel recover from its own history.
            const bool sharedCell = stereoIn && lev0 == lev1 && cell[0].fast == cell[1].fast &&
                                    cell[0].slow == cell[1].slow &&
                                    cell[0].second == cell[1].second &&
                                    cell[0].gain == cell[1].gain;
            const double e0 = cell[0].step(lev0, law);
            double e1 = e0;
            if (stereoIn) {
                if (sharedCell)
                    cell[1] = cell[0];
                else
                    e1 = cell[1].step(lev1, law);
            }
            const double c0 = (bloom ? e0 : -e0) + bridge[0];
            const double c1 = (bloom ? e1 : -e1) + bridge[1];
            bridge[0] -= kBridge * bridge[0];
            bridge[1] -= kBridge * bridge[1];
            if (abs(c0) > abs(extreme)) extreme = c0;
            if (abs(c1) > abs(extreme)) extreme = c1;

            // The gain, on the delayed audio.
            const double x0 = midSide ? 0.5 * (dl + dr) : dl;
            const double x1 = midSide ? 0.5 * (dl - dr) : dr;
            double y0 = x0 * dbToGain(c0);
            double y1 = x1 * dbToGain(c1);
            if (colourAmount > 0.0 && (even != 0.0 || odd != 0.0)) {
                y0 += tint(0, y0, e0);
                y1 += tint(1, y1, e1);
            }
            double wl = midSide ? y0 + y1 : y0;
            double wr = midSide ? y0 - y1 : y1;
            if (!stereoIn) wr = wl;

            // Mix, match, output.
            const double m = mix.at(i);
            double ol = dl + m * (wl - dl);
            double orr = dr + m * (wr - dr);
            powerIn += kPower * (dl * dl + dr * dr - powerIn);
            powerOut += kPower * (ol * ol + orr * orr - powerOut);
            if (powerIn > 2e-6 && powerOut > 1e-12)
                matchDb +=
                    kMatch * (clamp(10.0 * log10(powerIn / powerOut), -24.0, 24.0) - matchDb);
            matchNow +=
                (match.on() ? kMatch * 8.0 : kMatchOff) * ((match.on() ? matchDb : 0.0) - matchNow);
            colOut = max(colOut, max(abs(ol), abs(orr)));
            const double g = dbToGain(output.at(i) + matchNow);
            ol *= g;
            orr *= g;
            if (listenAt < listenLength) {
                listenAt += 1;
                const double progress = listenAt / (1.0 * listenLength);
                listenKey =
                    listenFromKey + progress * ((listenMode == 1 ? 1.0 : 0.0) - listenFromKey);
                listenDelta =
                    listenFromDelta + progress * ((listenMode == 2 ? 1.0 : 0.0) - listenFromDelta);
            }
            const double normalL = ol, normalR = orr;
            if (listenKey > 0.0) {
                const double hl = ahead > 0 ? keyDelay[0].read(ahead) : (midSide ? k0 + k1 : k0);
                const double hr = ahead > 0 ? keyDelay[1].read(ahead) : (midSide ? k0 - k1 : k1);
                ol += listenKey * (hl - normalL);
                orr += listenKey * (hr - normalR);
            }
            if (listenDelta > 0.0) {
                ol += listenDelta * (dl - wl - normalL);
                orr += listenDelta * (dr - wr - normalR);
            }
            io[0][i] = ol;
            if (stereoIn) io[1][i] = orr;

            // What the editor shows.
            peakIn0 = max(abs(l), peakIn0 * fall);
            peakIn1 = max(abs(r), peakIn1 * fall);
            peakOut0 = max(abs(ol), peakOut0 * fall);
            peakOut1 = max(abs(orr), peakOut1 * fall);
            colIn = max(colIn, max(abs(dl), abs(dr)));
            if (abs(c0) > abs(colGain)) colGain = c0;
            if (abs(c1) > abs(colGain)) colGain = c1;
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
            if (colAt >= colLength) {
                colAt -= colLength;
                hIn[hw] = static_cast<float>(gainToDb(colIn));
                hOut[hw] = static_cast<float>(gainToDb(colOut));
                hGain[hw] = static_cast<float>(colGain);
                hBeat[hw] = static_cast<float>(colBeat);
                hw = hw == kCols - 1 ? 0 : hw + 1;
                filled = min(filled + 1, kCols);
                colIn = 0.0;
                colOut = 0.0;
                colGain = 0.0;
                colBeat = 0;
            }
        }
        wasPlaying = t.playing;
        beatNext = beatPos;
        gainChange.set(extreme);
        const double loudness =
            powerIn > 2e-6 && powerOut > 1e-12 ? clamp(-matchDb, -24.0, 24.0) : 0.0;
        loudnessChange.set(loudness);
        hHead.set(hw);
        hFilled.set(filled);
        keyDb.set(gainToDb(keyPeak));
        gainDb.set(bloom ? max(cell[0].gain, cell[1].gain) : -max(cell[0].gain, cell[1].gain));
        inLeft.set(gainToDb(peakIn0));
        inRight.set(gainToDb(stereoIn ? peakIn1 : peakIn0));
        outLeft.set(gainToDb(peakOut0));
        outRight.set(gainToDb(stereoIn ? peakOut1 : peakOut0));
        matchShown.set(matchNow);
        loudShown.set(loudness);
        tempoShown.set(t.tempo);
        barShown.set(barBeats);
        keyLive.set(external);
    }

    // The colour a channel adds: its harmonics, more as the cell works harder,
    // with the DC the even ones make taken out.
    double tint(int c, double y, double e) {
        const double drive = colourAmount * (0.35 + 0.65 * min(1.0, e / 10.0));
        const double s = clamp(y, -1.5, 1.5);
        const double h = drive * (even * s * s + odd * s * s * s);
        colourLow[c] += kDc * (h - colourLow[c]);
        return h - colourLow[c];
    }

    // --- the editor ---------------------------------------------------------------------------

    static constexpr double kW = 1000;
    static constexpr double kH = 700;
    static constexpr double hx = 16;  // the history
    static constexpr double hy = 60;
    static constexpr double hwid = 652;
    static constexpr double hh = 272;
    static constexpr double gx = 28;  // its graph
    static constexpr double gy = 70;
    static constexpr double gw = 604;
    static constexpr double gh = 252;
    static constexpr double tx = 684;  // the transfer panel
    static constexpr double ts = 220;  // its square
    static constexpr double tqx = 722;
    static constexpr double tqy = 78;
    static constexpr double ky = 344;  // the key strip
    static constexpr double by = 408;  // the bottom panels
    static constexpr double bh = 276;
    static constexpr double cx0 = 16;  // character
    static constexpr double cw = 196;
    static constexpr double dx0 = 228;  // dynamics
    static constexpr double dw = 488;
    static constexpr double ox = 732;  // output
    static constexpr double ow = 252;
    static constexpr double knobY = 488;
    static constexpr double outKnobY = 638;
    static constexpr double glx = 244;  // the ballistics plot
    static constexpr double gly = 572;
    static constexpr double glw = 296;
    static constexpr double glh = 92;
    static constexpr double sx = 566;  // sync pills
    static constexpr double sy = 566;
    static constexpr double rowH = 37;
    static constexpr double rowY = 444;

    // The editor's own.
    double needle = 0.0;
    double needlePeak = 0.0;
    int peakAge = 0;
    Array<double, 24> trailX, trailY;
    int trailAt = 0;
    bool trailReady = false;
    int dragging = 0;  // 1 knob, 2 slider, 3 history threshold, 4 transfer, 5 ballistics
    int dragKnob = -1;
    double knobPos = 0.0;
    bool dragAttack = true;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    Law simLaw;
    Cell simCell;
    Array<float, 240> glyph;
    double glyphMax = 1.0;
    Array<double, 7> glyphKey;
    Array<double, 6> segAt = {0.0, 0.04, 0.17, 0.5, 0.62, 1.0};  // where each part starts
    Array<double, 5> segSeconds;
    Array<Array<float, 44>, 6> icon;
    bool iconsReady = false;

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

    void label(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
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
    void dial(Canvas& g, double x, double y, double r, double norm, int colour, bool bipolar,
              bool enabled, bool hot, const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.globalAlpha(enabled ? 1.0 : 0.35);
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double from = bipolar ? start + 0.5 * sweep : start;
        g.strokeStyle(colour);
        g.lineWidth(3.5);
        g.beginPath();
        if (at >= from)
            g.arc(x, y, r, from, at);
        else
            g.arc(x, y, r, at, from);
        g.stroke();
        g.radialGradient(x, y - 6, 2, r - 4, hot ? 0x5E4E41 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 6);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 14), y + sin(at) * (r - 14));
        g.lineTo(x + cos(at) * (r - 8), y + sin(at) * (r - 8));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, x, y - r - 11);
        g.globalAlpha(1.0);
    }

    void value(Canvas& g, const char* text, double x, double y, bool enabled) {
        g.globalAlpha(enabled ? 1.0 : 0.4);
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
        g.globalAlpha(1.0);
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

    // The mark: a sundew's round leaf, its tentacles each holding a drop of dew.
    void drawMark(Canvas& g, double x, double y) {
        g.strokeStyle(Theme::accent2);
        g.lineCap(LineCap::Round);
        g.lineWidth(1.3);
        g.beginPath();
        g.moveTo(x - 1, y + 4);
        g.quadraticCurveTo(x - 4, y + 11, x - 9, y + 15);
        g.stroke();
        g.fillStyle(0x6E7A4B);
        g.beginPath();
        g.circle(x, y, 5.2);
        g.fill();
        for (int k = 0; k < 11; ++k) {
            const double a = -pi * 0.5 + (k - 5) * 0.27;
            const double len = 6.5 + (k == 5 ? 2.0 : (imod(k, 2) == 0 ? 0.5 : 1.4));
            const double ex = x + cos(a) * (5.0 + len), ey = y + sin(a) * (5.0 + len);
            g.strokeStyle(0xB86B4B);
            g.lineWidth(0.9);
            g.beginPath();
            g.moveTo(x + cos(a) * 4.5, y + sin(a) * 4.5);
            g.lineTo(ex, ey);
            g.stroke();
            g.fillStyle(k == 5 ? Theme::accent : Theme::text);
            g.beginPath();
            g.circle(ex, ey, k == 5 ? 1.6 : 1.2);
            g.fill();
        }
    }

    // --- the header -----------------------------------------------------------------------

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
        drawMark(g, 30, 30);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Sundew", 52, 37);
        const double tw = g.measureText("Sundew");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("compressor", 60 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 620, 23, 140, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 824, 23, 140, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the history ----------------------------------------------------------------------

    double levelY(double db) const { return gy + gh * clamp(-db / 48.0, 0.0, 1.0); }
    double gainY(double db, bool lift) const {
        // Gain change at twice the level scale: a cut hangs from the top, a
        // lift rises from the bottom. Which one is the character's, not the
        // sign's: no change at all lies along the cut's edge, at the top.
        const double depth = gh * clamp(abs(db) / 30.0, 0.0, 1.0);
        return lift ? gy + gh - depth : gy + depth;
    }

    // Column j back from the newest, averaged with its neighbours.
    double eased(const SharedArray<float, 512>& levels, int head, int j, int count) const {
        const double here = levels[wrap(head - 1 - j, kCols)];
        const double newer = j > 0 ? levels[wrap(head - j, kCols)] : here;
        const double older = j + 1 < count ? levels[wrap(head - 2 - j, kCols)] : here;
        return 0.25 * newer + 0.5 * here + 0.25 * older;
    }

    // A level history as a filled area from the floor, its edge eased
    // through the midpoints of the columns.
    void levelArea(Canvas& g, const SharedArray<float, 512>& levels, int head, int count,
                   double step, bool filled) {
        for (int pass = filled ? 0 : 1; pass < 2; ++pass) {
            g.beginPath();
            double px = gx + gw, py = levelY(eased(levels, head, 0, count));
            if (pass == 0) {
                g.moveTo(px, gy + gh);
                g.lineTo(px, py);
            } else {
                g.moveTo(px, py);
            }
            for (int j = 1; j < count; ++j) {
                const double x = gx + gw - j * step, y = levelY(eased(levels, head, j, count));
                g.quadraticCurveTo(px, py, 0.5 * (px + x), 0.5 * (py + y));
                px = x;
                py = y;
            }
            g.lineTo(px, py);
            if (pass == 0) {
                g.lineTo(px, gy + gh);
                g.closePath();
                g.linearGradient(0, gy, 0, gy + gh, 0x6A5F52, 0x2A231D);
                g.globalAlpha(0.75);
                g.fill();
            } else {
                g.strokeStyle(filled ? kCurve : kLinen);
                g.globalAlpha(filled ? 0.75 : 0.32);
                g.lineWidth(filled ? 1.1 : 1.0);
                g.lineJoin(LineJoin::Round);
                g.stroke();
            }
            g.globalAlpha(1.0);
        }
    }

    void drawHistory(Canvas& g) {
        panel(g, hx, hy, hwid, hh);
        well(g, gx, gy, gw, gh);
        const int c = character.index();
        const int colour = characterColour(c);
        g.save();
        g.clip(gx, gy, gw, gh);
        // The level grid, every 6 dB, and the bars and beats.
        for (int k = 1; k < 8; ++k) {
            const double y = levelY(-6.0 * k);
            g.strokeStyle(imod(k, 2) == 0 ? kGridLit : kGrid);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(gx, y + 0.5);
            g.lineTo(gx + gw, y + 0.5);
            g.stroke();
        }
        const int head = hHead.get();
        const int count = min(hFilled.get(), kShown);
        const double step = gw / (kShown - 1.0);
        for (int j = 0; j < count; ++j) {
            const int at = wrap(head - 1 - j, kCols);
            const double beat = hBeat[at];
            if (beat < 0.5) continue;
            const double x = gx + gw - j * step;
            g.strokeStyle(beat > 1.5 ? 0x4C3F34 : kGridLit);
            g.globalAlpha(beat > 1.5 ? 0.9 : 0.55);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(x, gy);
            g.lineTo(x, gy + gh);
            g.stroke();
            g.globalAlpha(1.0);
        }
        if (count > 1) {
            // The input behind, faint; the output in front of it: what was
            // taken away is the band between them.
            levelArea(g, hOut, head, count, step, true);
            levelArea(g, hIn, head, count, step, false);
            // The gain change, in the character's colour, with a soft wash under it.
            const bool lift = c == kBloom;
            g.beginPath();
            g.moveTo(gx + gw, lift ? gy + gh : gy);
            for (int j = 0; j < count; ++j) {
                const int at = wrap(head - 1 - j, kCols);
                g.lineTo(gx + gw - j * step, gainY(hGain[at], lift));
            }
            g.lineTo(gx + gw - (count - 1) * step, lift ? gy + gh : gy);
            g.closePath();
            g.fillStyle(colour);
            g.globalAlpha(0.13);
            g.fill();
            g.globalAlpha(1.0);
            g.beginPath();
            for (int j = 0; j < count; ++j) {
                const int at = wrap(head - 1 - j, kCols);
                const double x = gx + gw - j * step, y = gainY(hGain[at], lift);
                if (j == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.lineJoin(LineJoin::Round);
            g.strokeStyle(kGraphBottom);
            g.globalAlpha(0.7);
            g.lineWidth(4.5);
            g.stroke();
            g.globalAlpha(1.0);
            g.strokeStyle(colour);
            g.lineWidth(2);
            g.stroke();
        }
        // The threshold: where the history's levels start to be worked on.
        const double ty = levelY(threshold);
        const bool hotLine = dragging == 3 || (dragging == 0 && abs(hoverY - ty) < 6 &&
                                               hoverX > gx && hoverX < gx + gw);
        g.strokeStyle(Theme::accent2);
        g.globalAlpha(hotLine ? 1.0 : 0.75);
        g.lineWidth(hotLine ? 1.6 : 1.1);
        for (double x = gx + 4; x < gx + gw - 40; x += 9) {
            g.beginPath();
            g.moveTo(x, ty + 0.5);
            g.lineTo(x + 5, ty + 0.5);
            g.stroke();
        }
        g.globalAlpha(1.0);
        g.restore();
        g.fillStyle(Theme::accent2);
        g.beginPath();
        g.moveTo(gx + gw - 1, ty);
        g.lineTo(gx + gw - 9, ty - 5);
        g.lineTo(gx + gw - 9, ty + 5);
        g.closePath();
        g.fill();
        // Its value, on a dark tab.
        const double labelY = ty - (ty < gy + 14 ? -10 : 10);
        g.fillStyle(kInk);
        g.globalAlpha(0.85);
        g.beginPath();
        g.roundRect(gx + gw - 50, labelY - 7, 40, 14, 4);
        g.fill();
        g.globalAlpha(1.0);
        g.fillStyle(Theme::accent2);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.1f", threshold.value()), gx + gw - 14, labelY);
        // The scales: level on the left in the margin, gain on the right.
        g.monoFont(8.5);
        for (int k = 1; k < 8; ++k) {
            const double y = levelY(-6.0 * k);
            g.fillStyle(Theme::textFaint);
            g.textAlign(TextAlign::Right);
            g.fillText(format("%.0f", -6.0 * k), gx + gw + 30, y);
        }
        // The gain scale inside, on the left, in the character's colour.
        g.fillStyle(colour);
        g.globalAlpha(0.8);
        g.textAlign(TextAlign::Left);
        for (int k = 1; k <= 3; ++k) {
            const double db = c == kBloom ? 6.0 * k : -6.0 * k;
            g.fillText(format("%+.0f", db), gx + 5, gainY(db, c == kBloom));
        }
        g.globalAlpha(1.0);
        // The legend.
        // The legend, away from where the gain line runs.
        const double ly = c == kBloom ? gy + 12 : gy + gh - 12;
        g.fillStyle(kLinen);
        g.globalAlpha(0.4);
        g.fillRect(gx + 12, ly - 0.5, 12, 1);
        g.globalAlpha(1.0);
        label(g, "INPUT", gx + 30, ly, TextAlign::Left);
        g.fillStyle(0x6A5F52);
        g.fillRect(gx + 78, ly - 3, 12, 6);
        g.fillStyle(kCurve);
        g.fillRect(gx + 78, ly - 3, 12, 1);
        label(g, "OUTPUT", gx + 96, ly, TextAlign::Left);
        g.fillStyle(colour);
        g.fillRect(gx + 152, ly - 1, 12, 2);
        label(g, c == kBloom ? "LIFT" : "GAIN REDUCTION", gx + 170, ly, TextAlign::Left);
    }

    // --- the transfer curve ---------------------------------------------------------------------

    double tX(double db) const { return tqx + ts * clamp((db + 60.0) / 60.0, 0.0, 1.0); }
    double tY(double db) const { return tqy + ts * clamp(-db / 60.0, 0.0, 1.0); }

    void drawTransfer(Canvas& g) {
        panel(g, tx, hy, 300, hh);
        well(g, tqx, tqy, ts, ts);
        const int c = character.index();
        const int colour = characterColour(c);
        Law shown;
        shown.set(c, threshold, ratioSet, knee, range, attack, release, 48000.0);
        g.save();
        g.clip(tqx, tqy, ts, ts);
        for (int k = 1; k < 5; ++k) {
            g.strokeStyle(kGrid);
            g.lineWidth(1);
            g.beginPath();
            g.moveTo(tX(-12.0 * k) + 0.5, tqy);
            g.lineTo(tX(-12.0 * k) + 0.5, tqy + ts);
            g.stroke();
            g.beginPath();
            g.moveTo(tqx, tY(-12.0 * k) + 0.5);
            g.lineTo(tqx + ts, tY(-12.0 * k) + 0.5);
            g.stroke();
        }
        // Unity, then the threshold's cross.
        g.strokeStyle(kGridLit);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(tqx, tqy + ts);
        g.lineTo(tqx + ts, tqy);
        g.stroke();
        g.strokeStyle(Theme::accent2);
        g.globalAlpha(0.35);
        g.beginPath();
        g.moveTo(tX(threshold) + 0.5, tqy);
        g.lineTo(tX(threshold) + 0.5, tqy + ts);
        g.stroke();
        g.globalAlpha(1.0);
        // The curve, with a glow under it.
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            for (int k = 0; k <= 120; ++k) {
                const double in = -60.0 + k * 0.5;
                const double out = in + shown.steady(in);
                if (k == 0)
                    g.moveTo(tX(in), tY(out));
                else
                    g.lineTo(tX(in), tY(out));
            }
            g.strokeStyle(pass == 0 ? colour : kCurve);
            g.globalAlpha(pass == 0 ? 0.25 : 1.0);
            g.lineWidth(pass == 0 ? 6 : 1.8);
            g.lineJoin(LineJoin::Round);
            g.stroke();
        }
        g.globalAlpha(1.0);
        // Where the music is now: a dot with a short trail.
        const double level = keyDb.get();
        const double change = gainDb.get();
        if (level > -59.0) {
            trailX[trailAt] = tX(level);
            trailY[trailAt] = tY(level + change);
            trailAt = trailAt == 23 ? 0 : trailAt + 1;
            if (trailAt == 0) trailReady = true;
        }
        const int trail = trailReady ? 24 : trailAt;
        for (int k = 1; k <= trail; ++k) {
            const int at = wrap(trailAt - k, 24);
            g.fillStyle(colour);
            g.globalAlpha(0.5 * (1.0 - k / 25.0));
            g.beginPath();
            g.circle(trailX[at], trailY[at], 2.2);
            g.fill();
        }
        g.globalAlpha(1.0);
        if (level > -59.0) {
            const double x = tX(level), y = tY(level + change);
            g.radialGradient(x, y, 0, 11, colour, kGraphBottom);
            g.globalAlpha(0.6);
            g.beginPath();
            g.circle(x, y, 11);
            g.fill();
            g.globalAlpha(1.0);
            g.fillStyle(kCurve);
            g.beginPath();
            g.circle(x, y, 3.4);
            g.fill();
        }
        g.restore();
        // The scales, and what the curve is.
        g.monoFont(8.5);
        g.fillStyle(Theme::textFaint);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 0; k <= 5; ++k) {
            g.textAlign(TextAlign::Center);
            if (k == 0)
                g.fillText("0", tX(0), tqy + ts + 11);
            else
                g.fillText(format("%.0f", -12.0 * k), tX(-12.0 * k), tqy + ts + 11);
            g.textAlign(TextAlign::Right);
            if (k == 0)
                g.fillText("0", tqx - 6, tY(0));
            else
                g.fillText(format("%.0f", -12.0 * k), tqx - 6, tY(-12.0 * k));
        }
        g.textAlign(TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(13);
        fillRatio(g, tqx + 10, tqy + 16);
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.fillText(format("knee %.0f dB", shown.width()), tqx + 10, tqy + 32);
        label(g, "IN", tqx + ts, tqy + ts + 24, TextAlign::Right);
        label(g, "OUT", tqx - 6, tqy - 10, TextAlign::Right);
        drawGainBar(g, tqx + ts + 22, tqy, ts, change, colour, c == kBloom);
    }

    // The ratio as text, where the fill style and alignment are set.
    void fillRatio(Canvas& g, double x, double y) {
        const double r = ratioSet.value();
        if (character.index() == kVariMu) {
            if (r >= 9.95)
                g.fillText(format("up to %.0f:1", r), x, y);
            else
                g.fillText(format("up to %.1f:1", r), x, y);
        } else if (r >= 9.95) {
            g.fillText(format("%.0f:1", r), x, y);
        } else {
            g.fillText(format("%.1f:1", r), x, y);
        }
    }

    // The gain change now, as a bar beside the curve: hanging from the top
    // for a cut, rising for a lift.
    void drawGainBar(Canvas& g, double x, double y, double h, double change, int colour,
                     bool lift) {
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x, y, 8, h, 4);
        g.fill();
        const double depth = clamp(abs(change) / 24.0, 0.0, 1.0) * h;
        if (depth > 0.5) {
            g.fillStyle(colour);
            g.beginPath();
            if (lift)
                g.roundRect(x, y + h - depth, 8, depth, 4);
            else
                g.roundRect(x, y, 8, depth, 4);
            g.fill();
        }
        g.monoFont(8.5);
        g.fillStyle(Theme::textFaint);
        g.textAlign(TextAlign::Left);
        for (int k = 0; k <= 4; ++k) {
            const double yy = lift ? y + h - h * k / 4.0 : y + h * k / 4.0;
            g.fillRect(x + 10, yy - 0.5, 3, 1);
            const double ty = yy + (k == 4 && !lift ? -5 : (k == 4 ? 5 : 0));
            if ((k == 2 || k == 4) && lift) g.fillText(format("+%.0f", 6.0 * k), x + 15, ty);
            if ((k == 2 || k == 4) && !lift) g.fillText(format("-%.0f", 6.0 * k), x + 15, ty);
        }
    }

    // --- the key strip ---------------------------------------------------------------------

    static constexpr double kKey = 32;
    static constexpr double kHpf = 210;
    static constexpr double kLink = 380;
    static constexpr double kStereo = 552;
    static constexpr double kAhead = 668;
    static constexpr double kListen = 830;
    static constexpr double kSliderW = 96;

    void drawKeyStrip(Canvas& g) {
        panel(g, hx, ky, 968, 52);
        const double ly = ky + 15, cy = ky + 35;
        const bool ext = key.index() == 1;
        label(g, "KEY", kKey, ly, TextAlign::Left);
        if (ext) {
            const bool live = keyLive.get();
            g.fillStyle(live ? Theme::accent2 : Theme::textFaint);
            g.beginPath();
            g.circle(kKey + 26, ly, 2.6);
            g.fill();
            g.fillStyle(live ? Theme::textMuted : Theme::textFaint);
            g.serifItalicFont(10);
            g.textAlign(TextAlign::Left);
            g.fillText(live ? "connected" : "not connected", kKey + 34, ly);
        }
        pill(g, kKey, cy - 11, 70, 22, "INTERNAL", !ext, Theme::accent, true);
        pill(g, kKey + 76, cy - 11, 70, 22, "EXTERNAL", ext, Theme::accent, true);

        label(g, "KEY HPF", kHpf, ly, TextAlign::Left);
        slider(g, kHpf, cy, kSliderW, keyHpf.normalized(), Theme::accent2);
        g.fillStyle(keyHpf > 20.5 ? Theme::text : Theme::textFaint);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.fillText(keyHpf > 20.5 ? format("%.0f Hz", keyHpf.value()) : "off", kHpf + kSliderW + 12,
                   cy);

        label(g, "LINK", kLink, ly, TextAlign::Left);
        slider(g, kLink, cy, kSliderW, link.normalized(), Theme::accent2);
        g.fillStyle(Theme::text);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%.0f%%", link * 100.0), kLink + kSliderW + 12, cy);

        label(g, "STEREO", kStereo, ly, TextAlign::Left);
        pill(g, kStereo, cy - 11, 44, 22, "L / R", stereo.index() == 0, Theme::accent, true);
        pill(g, kStereo + 50, cy - 11, 44, 22, "M / S", stereo.index() == 1, Theme::accent, true);

        label(g, "LOOKAHEAD", kAhead, ly, TextAlign::Left);
        const int la = lookahead.index();
        pill(g, kAhead, cy - 11, 36, 22, "OFF", la == 0, Theme::accent, true);
        pill(g, kAhead + 40, cy - 11, 30, 22, "1", la == 1, Theme::accent, true);
        pill(g, kAhead + 74, cy - 11, 30, 22, "2", la == 2, Theme::accent, true);
        pill(g, kAhead + 108, cy - 11, 30, 22, "5", la == 3, Theme::accent, true);
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Left);
        g.fillText("ms", kAhead + 142, cy);

        label(g, "LISTEN", kListen, ly, TextAlign::Left);
        const int li = listen.index();
        pill(g, kListen, cy - 11, 40, 22, "OFF", li == 0, Theme::alert, true);
        pill(g, kListen + 44, cy - 11, 44, 22, "KEY", li == 1, Theme::alert, true);
        pill(g, kListen + 92, cy - 11, 54, 22, "DELTA", li == 2, Theme::alert, true);
    }

    // --- character -----------------------------------------------------------------------------

    void characterName(Canvas& g, int c, double x, double y) {
        if (c == kClean) g.fillText("Clean", x, y);
        if (c == kGlue) g.fillText("Glue", x, y);
        if (c == kOpto) g.fillText("Opto", x, y);
        if (c == kFet) g.fillText("FET", x, y);
        if (c == kVariMu) g.fillText("Vari-Mu", x, y);
        if (c == kBloom) g.fillText("Bloom", x, y);
    }

    void characterAside(Canvas& g, int c, double x, double y) {
        if (c == kClean) g.fillText("exact and transparent", x, y);
        if (c == kGlue) g.fillText("bus glue, auto release", x, y);
        if (c == kOpto) g.fillText("smooth, slow to let go", x, y);
        if (c == kFet) g.fillText("fast, bright, biting", x, y);
        if (c == kVariMu) g.fillText("gentle, grows with level", x, y);
        if (c == kBloom) g.fillText("lifts the quiet detail", x, y);
    }

    // The ballistics plot's story: a short hit, a rest, a held note and a
    // rest, played through a cell of the character's own. Each part has its
    // share of the width, so an attack shows however short it is, and a
    // release after the hit and after the held note can differ (Glue, Opto).
    // Fills glyph with the amount at each point; returns the deepest.
    double simulate(int c, double a, double r) {
        segSeconds[0] = 0.03;
        segSeconds[1] = clamp(a * 0.005, 0.015, 0.3);
        segSeconds[2] = clamp(r * 0.004, 0.08, 3.0);
        segSeconds[3] = 0.6;
        segSeconds[4] = clamp(r * 0.004 * (c == kOpto ? 4.0 : (c == kGlue ? 2.5 : 1.0)), 0.12, 8.0);
        double total = 0.0;
        for (int k = 0; k < 5; ++k) total += segSeconds[k];
        const double fs = min(4000.0, 20000.0 / total);
        simLaw.set(c, threshold, ratioSet, knee, range, a, r, fs);
        const bool lift = c == kBloom;
        const double quiet = lift ? threshold - 18.0 : threshold - 30.0;
        const double loud = lift ? threshold + 4.0 : threshold + 12.0;
        simCell.settle(0.0);
        if (lift)
            for (int n = 0; n < 4000; ++n) simCell.step(quiet, simLaw);
        double deepest = 0.0;
        for (int seg = 0; seg < 5; ++seg) {
            const int p0 = toInt(segAt[seg] * kGlyph);
            const int p1 = min(kGlyph, toInt(segAt[seg + 1] * kGlyph));
            const int steps = max(1, toInt(segSeconds[seg] * fs));
            const double level = seg == 1 || seg == 3 ? loud : quiet;
            for (int n = 0; n < steps; ++n) {
                const double e = simCell.step(level, simLaw);
                const int at = min(p1 - 1, p0 + toInt((n + 1.0) * (p1 - p0) / steps) - 1);
                glyph[max(at, p0)] = static_cast<float>(e);
                deepest = max(deepest, e);
            }
        }
        return deepest;
    }

    void makeIcons() {
        for (int c = 0; c < 6; ++c) {
            const double deepest = max(0.5, simulate(c, c == kFet ? 0.3 : 8.0, 110.0));
            for (int p = 0; p < kIcon; ++p) {
                const int from = idiv(p * kGlyph, kIcon), to = idiv((p + 1) * kGlyph, kIcon);
                double v = 0.0;
                for (int k = from; k < to; ++k)
                    v = c == kBloom ? max(v, 1.0 * glyph[k]) : max(v, 1.0 * glyph[k]);
                icon[c][p] = static_cast<float>(v / deepest);
            }
        }
        glyphKey[0] = -1.0;
        iconsReady = true;
    }

    void drawIcon(Canvas& g, int c, double x, double y, double w, double h, int colour) {
        g.strokeStyle(colour);
        g.lineWidth(1.5);
        g.lineCap(LineCap::Round);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        const bool lift = c == kBloom;
        for (int p = 0; p < kIcon; ++p) {
            const double px = x + w * p / (kIcon - 1.0);
            const double v = icon[c][p];
            const double py = lift ? y + h - v * h : y + v * h;
            if (p == 0)
                g.moveTo(px, py);
            else
                g.lineTo(px, py);
        }
        g.stroke();
    }

    void drawCharacters(Canvas& g) {
        if (!iconsReady) makeIcons();
        panel(g, cx0, by, cw, bh);
        heading(g, "Character", cx0 + 16, by + 20);
        const int now = character.index();
        for (int c = 0; c < 6; ++c) {
            const double y = rowY + c * rowH;
            const bool lit = c == now;
            const int colour = characterColour(c);
            const bool hot = dragging == 0 && hoverX >= cx0 + 8 && hoverX < cx0 + cw - 8 &&
                             hoverY >= y && hoverY < y + rowH - 3;
            if (lit || hot) {
                g.fillStyle(lit ? Theme::raised : 0x352B23);
                g.beginPath();
                g.roundRect(cx0 + 8, y, cw - 16, rowH - 3, 7);
                g.fill();
                if (lit) {
                    g.strokeStyle(colour);
                    g.globalAlpha(0.8);
                    g.lineWidth(1);
                    g.stroke();
                    g.globalAlpha(1.0);
                }
            }
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(cx0 + 16, y + 5, 42, rowH - 13, 4);
            g.fill();
            drawIcon(g, c, cx0 + 20, y + 9, 34, rowH - 21, lit ? colour : 0x7A6A5B);
            g.fillStyle(lit ? Theme::text : Theme::textMuted);
            g.serifFont(13);
            g.textAlign(TextAlign::Left);
            g.textBaseline(TextBaseline::Middle);
            characterName(g, c, cx0 + 68, y + 11);
            g.fillStyle(lit ? colour : Theme::textFaint);
            g.serifItalicFont(10);
            characterAside(g, c, cx0 + 68, y + 24);
        }
    }

    // --- dynamics -----------------------------------------------------------------------------

    double knobX(int k) const {
        if (k < 6) return dx0 + 46 + k * 79;
        return ox + 50 + (k - 6) * 76;
    }
    double knobYOf(int k) const { return k < 6 ? knobY : outKnobY; }
    double knobR(int k) const { return k < 4 ? 23 : 21; }

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

    void fillSync(Canvas& g, int s, double x, double y) {
        g.fillText(s == 1   ? "1/32"
                   : s == 2 ? "1/16"
                   : s == 3 ? "1/8"
                   : s == 4 ? "1/4"
                   : s == 5 ? "1/2"
                   : s == 6 ? "1 bar"
                            : "off",
                   x, y);
    }

    void valueStyle(Canvas& g, bool enabled) {
        g.globalAlpha(enabled ? 1.0 : 0.4);
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void drawDynamics(Canvas& g) {
        panel(g, dx0, by, dw, bh);
        heading(g, "Dynamics", dx0 + 16, by + 20);
        const int c = character.index();
        const int colour = characterColour(c);
        g.fillStyle(colour);
        g.serifItalicFont(13);
        g.textAlign(TextAlign::Right);
        characterName(g, c, dx0 + dw - 16, by + 20);
        const bool synced = sync.index() > 0;
        dial(g, knobX(0), knobY, knobR(0), threshold.normalized(), colour, false, true,
             hotKnob == 0, "THRESHOLD");
        dial(g, knobX(1), knobY, knobR(1), ratioSet.normalized(), colour, false, true, hotKnob == 1,
             "RATIO");
        dial(g, knobX(2), knobY, knobR(2), attack.normalized(), colour, false, true, hotKnob == 2,
             "ATTACK");
        dial(g, knobX(3), knobY, knobR(3), release.normalized(), colour, false, !synced,
             hotKnob == 3, "RELEASE");
        dial(g, knobX(4), knobY, knobR(4), knee.normalized(), colour, false, true, hotKnob == 4,
             "KNEE");
        dial(g, knobX(5), knobY, knobR(5), range.normalized(), colour, false, true, hotKnob == 5,
             "RANGE");
        value(g, format("%.1f dB", threshold.value()), knobX(0), knobY + 36, true);
        valueStyle(g, true);
        fillRatio(g, knobX(1), knobY + 36);
        fillTime(g, attack, knobX(2), knobY + 36);
        if (synced)
            fillSync(g, sync.index(), knobX(3), knobY + 36);
        else
            fillTime(g, release, knobX(3), knobY + 36);
        g.globalAlpha(1.0);
        value(g, format("%.1f dB", knee.value()), knobX(4), knobY + 36, true);
        value(g, format("%.0f dB", range.value()), knobX(5), knobY + 36, true);
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(dx0 + 14, by + 136);
        g.lineTo(dx0 + dw - 14, by + 136);
        g.stroke();
        drawBallistics(g, c, colour);
        drawSync(g);
    }

    void refreshGlyph(int c) {
        const double a = attack, r = releaseNow();
        Array<double, 7> now = {
            1.0 * c, a, r, threshold.value(), ratioSet.value(), knee.value(), range.value()};
        bool same = true;
        for (int k = 0; k < 7; ++k)
            if (now[k] != glyphKey[k]) same = false;
        if (same) return;
        for (int k = 0; k < 7; ++k) glyphKey[k] = now[k];
        glyphMax = max(1.0, simulate(c, a, r));
    }

    // The release in ms: the knob's, or the note value's at the host's tempo.
    double releaseNow() {
        const int s = sync.index();
        if (s == 0) return release;
        const double tempo = tempoShown.get() > 0.0 ? tempoShown.get() : 120.0;
        const double bar = barShown.get() > 0.0 ? barShown.get() : 4.0;
        return 60000.0 / tempo * syncBeats(s, bar) / 3.0;
    }

    void drawBallistics(Canvas& g, int c, int colour) {
        refreshGlyph(c);
        label(g, "BALLISTICS", glx, gly - 14, TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(10);
        g.textAlign(TextAlign::Right);
        g.fillText("a hit, then a held note, through this cell", glx + glw, gly - 14);
        well(g, glx, gly, glw, glh);
        const bool lift = c == kBloom;
        const double top = gly + 16, depth = glh - 26;
        // The hit and the held note, behind, with what they are.
        for (int seg = 1; seg <= 3; seg += 2) {
            const double x0 = glx + glw * segAt[seg], x1 = glx + glw * segAt[seg + 1];
            g.fillStyle(kLinen);
            g.globalAlpha(0.06);
            g.fillRect(x0, gly + 1, x1 - x0, glh - 2);
            g.globalAlpha(1.0);
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(9.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(seg == 1 ? "hit" : "held", 0.5 * (x0 + x1), gly + 9);
        }
        // The rests' lengths, faintly, at their feet.
        g.fillStyle(Theme::textFaint);
        g.monoFont(8);
        g.textAlign(TextAlign::Right);
        fillTime(g, segSeconds[2] * 1000.0, glx + glw * segAt[3] - 4, gly + glh - 8);
        fillTime(g, segSeconds[4] * 1000.0, glx + glw - 5, gly + glh - 8);
        // The gain change.
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            for (int p = 0; p < kGlyph; ++p) {
                const double x = glx + 1 + (glw - 2) * p / (kGlyph - 1.0);
                const double v = glyph[p] / glyphMax;
                const double y = lift ? top + depth - v * depth : top + v * depth;
                if (p == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.strokeStyle(colour);
            g.globalAlpha(pass == 0 ? 0.22 : 1.0);
            g.lineWidth(pass == 0 ? 5 : 1.8);
            g.lineJoin(LineJoin::Round);
            g.stroke();
        }
        g.globalAlpha(1.0);
        const bool hot = dragging == 5 || (dragging == 0 && hoverX >= glx && hoverX < glx + glw &&
                                           hoverY >= gly && hoverY < gly + glh);
        if (hot) {
            g.fillStyle(Theme::textMuted);
            g.serifItalicFont(10);
            g.textAlign(TextAlign::Center);
            g.fillText("drag across a note for attack, a rest for release", glx + glw * 0.5,
                       gly + glh + 11);
        }
    }

    void drawSync(Canvas& g) {
        label(g, "RELEASE SYNC", sx, sy - 0, TextAlign::Left);
        const int s = sync.index();
        for (int k = 0; k < 7; ++k) {
            const double x = sx + imod(k, 4) * 36;
            const double y = sy + 12 + idiv(k, 4) * 27;
            pill(g, x, y, 32, 22,
                 k == 1   ? "1/32"
                 : k == 2 ? "1/16"
                 : k == 3 ? "1/8"
                 : k == 4 ? "1/4"
                 : k == 5 ? "1/2"
                 : k == 6 ? "BAR"
                          : "OFF",
                 s == k, Theme::accent, true);
        }
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (s > 0)
            g.fillText(format("%.0f ms at %.0f bpm", releaseNow(),
                              tempoShown.get() > 0 ? tempoShown.get() : 120.0),
                       sx, sy + 82);
    }

    // --- output ---------------------------------------------------------------------------------

    // Where a gain change sits on the meter's scale, 0 (-20 dB) to 1 (+6 dB).
    double meterPos(double db) const {
        if (db >= 0.0) return 0.78 + 0.22 * clamp(db / 6.0, 0.0, 1.0);
        return 0.78 * (1.0 - pow(clamp(-db / 20.0, 0.0, 1.0), 0.62));
    }

    // A moving-coil meter for the gain change, on a lamp-lit face.
    void drawNeedle(Canvas& g, double x, double y, double w, double h, int colour) {
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x - 3, y - 3, w + 6, h + 6, 9);
        g.fill();
        g.radialGradient(x + w * 0.5, y + h * 0.95, 10, w * 0.75, kFaceTop, kFaceBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 7);
        g.fill();
        const double px = x + w * 0.5, py = y + h + 34;  // the pivot, below the face
        const double rr = h + 10;
        g.save();
        g.clip(x, y, w, h);
        // The scale's arc, the cut in clay and the lift in sage.
        const double a0 = -0.88, a1 = 0.88;
        g.lineWidth(1.1);
        g.strokeStyle(kFaceInk);
        g.beginPath();
        g.arc(px, py, rr - 26, -pi * 0.5 + a0, -pi * 0.5 + a1);
        g.stroke();
        g.lineWidth(3);
        g.strokeStyle(0xB2563C);
        g.globalAlpha(0.65);
        g.beginPath();
        g.arc(px, py, rr - 22, -pi * 0.5 + a0, -pi * 0.5 + a0 + (a1 - a0) * meterPos(0.0));
        g.stroke();
        g.strokeStyle(0x6E7A4B);
        g.beginPath();
        g.arc(px, py, rr - 22, -pi * 0.5 + a0 + (a1 - a0) * meterPos(0.0), -pi * 0.5 + a1);
        g.stroke();
        g.globalAlpha(1.0);
        Array<double, 11> marks = {-20, -15, -10, -7, -5, -3, -2, -1, 0, 3, 6};
        g.monoFont(8);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        for (int k = 0; k < 11; ++k) {
            const double db = marks[k];
            const double a = -pi * 0.5 + a0 + (a1 - a0) * meterPos(db);
            const bool big = db == -20 || db == -10 || db == -5 || db == -3 || db == -1 ||
                             db == 0 || db == 3 || db == 6;
            g.strokeStyle(kFaceInk);
            g.lineWidth(big ? 1.3 : 0.9);
            g.beginPath();
            g.moveTo(px + cos(a) * (rr - 26), py + sin(a) * (rr - 26));
            g.lineTo(px + cos(a) * (rr - (big ? 17 : 21)), py + sin(a) * (rr - (big ? 17 : 21)));
            g.stroke();
            if (big) {
                g.fillStyle(kFaceInk);
                g.fillText(db > 0 ? format("+%.0f", db) : format("%.0f", db),
                           px + cos(a) * (rr - 8), py + sin(a) * (rr - 8));
            }
        }
        g.fillStyle(kFaceInk);
        g.globalAlpha(0.7);
        g.serifItalicFont(10);
        g.fillText("gain change", px, y + h - 30);
        g.globalAlpha(1.0);
        // The needle, with its shadow.
        const double a = -pi * 0.5 + a0 + (a1 - a0) * meterPos(needle);
        for (int pass = 0; pass < 2; ++pass) {
            const double o = pass == 0 ? 2.0 : 0.0;
            g.strokeStyle(pass == 0 ? 0x8C7552 : kFaceInk);
            g.globalAlpha(pass == 0 ? 0.35 : 1.0);
            g.lineWidth(pass == 0 ? 2.0 : 1.4);
            g.beginPath();
            g.moveTo(px + o + cos(a) * 30, py + o + sin(a) * 30);
            g.lineTo(px + o + cos(a) * (rr - 12), py + o + sin(a) * (rr - 12));
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The peak, a small clay tick on the arc.
        if (needlePeak < -0.3) {
            const double pa = -pi * 0.5 + a0 + (a1 - a0) * meterPos(needlePeak);
            g.strokeStyle(colour);
            g.lineWidth(2.2);
            g.beginPath();
            g.moveTo(px + cos(pa) * (rr - 30), py + sin(pa) * (rr - 30));
            g.lineTo(px + cos(pa) * (rr - 24), py + sin(pa) * (rr - 24));
            g.stroke();
        }
        g.restore();
        // The cover over the pivot, with the number.
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x + w * 0.5 - 46, y + h - 22, 92, 22, 5);
        g.fill();
        g.fillStyle(abs(needle) > 0.05 ? kCurve : Theme::textMuted);
        g.monoFont(11);
        g.textAlign(TextAlign::Center);
        fillDb(g, needle, x + w * 0.5, y + h - 10.5);
    }

    // A signed level, where the fill style and alignment are set; one that
    // rounds to nothing reads 0.0, not -0.0 or +0.0.
    void fillDb(Canvas& g, double db, double x, double y) {
        if (abs(db) < 0.05)
            g.fillText("0.0 dB", x, y);
        else
            g.fillText(format("%+.1f dB", db), x, y);
    }

    void drawOutput(Canvas& g) {
        panel(g, ox, by, ow, bh);
        heading(g, "Output", ox + 16, by + 20);
        pill(g, ox + ow - 82, by + 9, 66, 22, "MATCH", match.on(), Theme::accent, true);
        const int c = character.index();
        const int tone = characterColour(c);
        // The needle: quick to fall, a little slower back, as a meter's coil.
        const double target = gainDb.get();
        needle += (abs(target) > abs(needle) ? 0.45 : 0.18) * (target - needle);
        if (abs(target) > abs(needlePeak) || peakAge > 150) {
            needlePeak = target;
            peakAge = 0;
        } else {
            peakAge += 1;
        }
        drawNeedle(g, ox + 18, by + 40, ow - 36, 104, tone);
        // Loudness, in and out.
        const double loud = loudShown.get();
        label(g, "LOUDNESS CHANGE", ox + 18, by + 164, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(10.5);
        g.textAlign(TextAlign::Right);
        fillDb(g, loud, ox + ow - 18, by + 164);
        label(g, "MATCH ADDS", ox + 18, by + 180, TextAlign::Left);
        g.fillStyle(match.on() ? Theme::accent : Theme::textFaint);
        g.textAlign(TextAlign::Right);
        if (match.on())
            fillDb(g, matchShown.get(), ox + ow - 18, by + 180);
        else
            g.fillText("off", ox + ow - 18, by + 180);
        const bool tinted = c != kClean && c != kBloom;
        dial(g, knobX(6), outKnobY, 21, output.normalized(), Theme::accent, true, true,
             hotKnob == 6, "OUTPUT");
        dial(g, knobX(7), outKnobY, 21, mix.normalized(), Theme::accent, false, true, hotKnob == 7,
             "MIX");
        dial(g, knobX(8), outKnobY, 21, colour.normalized(), tone, false, tinted, hotKnob == 8,
             "COLOUR");
        value(g, format("%+.1f dB", output.value()), knobX(6), outKnobY + 34, true);
        value(g, format("%.0f%%", mix * 100.0), knobX(7), outKnobY + 34, true);
        value(g, format("%.0f%%", colour * 100.0), knobX(8), outKnobY + 34, tinted);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawHistory(g);
        drawTransfer(g);
        drawKeyStrip(g);
        drawCharacters(g);
        drawDynamics(g);
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
        if (k == 0) return threshold.normalized();
        if (k == 1) return ratioSet.normalized();
        if (k == 2) return attack.normalized();
        if (k == 3) return release.normalized();
        if (k == 4) return knee.normalized();
        if (k == 5) return range.normalized();
        if (k == 6) return output.normalized();
        if (k == 7) return mix.normalized();
        if (k == 8) return colour.normalized();
        if (k == 9) return keyHpf.normalized();
        return link.normalized();
    }

    void setKnob(int k, int how, double v) {
        if (k == 0) act(how, threshold, v);
        if (k == 1) act(how, ratioSet, v);
        if (k == 2) act(how, attack, v);
        if (k == 3) act(how, release, v);
        if (k == 4) act(how, knee, v);
        if (k == 5) act(how, range, v);
        if (k == 6) act(how, output, v);
        if (k == 7) act(how, mix, v);
        if (k == 8) act(how, colour, v);
        if (k == 9) act(how, keyHpf, v);
        if (k == 10) act(how, link, v);
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < 9; ++k) {
            const double dx = x - knobX(k), dy = y - knobYOf(k);
            const double r = knobR(k) + 6;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }

    // 9 the key's high-pass, 10 the link.
    int sliderAt(double x, double y) const {
        if (y < ky + 24 || y > ky + 48) return -1;
        if (x >= kHpf - 8 && x <= kHpf + kSliderW + 8) return 9;
        if (x >= kLink - 8 && x <= kLink + kSliderW + 8) return 10;
        return -1;
    }

    void sliderTo(int k, double x) {
        const double from = k == 9 ? kHpf : kLink;
        setKnob(k, 1, clamp((x - from) / kSliderW, 0.0, 1.0));
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
                sliderTo(dragKnob, p.x());
            } else if (dragging == 3) {
                threshold.edit(threshold - p.dy() * fine * 60.0 / gh);
            } else if (dragging == 4) {
                threshold.edit(threshold + p.dx() * fine * 60.0 / ts);
                ratioSet.editNormalized(
                    clamp(ratioSet.normalized() - p.dy() * fine / 180.0, 0.0, 1.0));
            } else if (dragging == 5) {
                if (dragAttack)
                    attack.editNormalized(
                        clamp(attack.normalized() + p.dx() * fine / 260.0, 0.0, 1.0));
                else
                    release.editNormalized(
                        clamp(release.normalized() + p.dx() * fine / 260.0, 0.0, 1.0));
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
                threshold.edit(threshold + notches * 0.5 * fine);
                return;
            }
            if (p.over(tqx, tqy, ts, ts)) {
                ratioSet.editNormalized(
                    clamp(ratioSet.normalized() + notches * 0.02 * fine, 0.0, 1.0));
                return;
            }
            int k = knobAt(p.x(), p.y());
            if (k < 0) k = sliderAt(p.x(), p.y());
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
        const int s = sliderAt(x, y);
        if (s >= 0) {
            if (p.doubleClick()) {
                setKnob(s, 2, 0);
                return;
            }
            dragging = 2;
            dragKnob = s;
            sliderTo(s, x);
            return;
        }
        if (p.over(gx, gy, gw, gh)) {
            if (p.doubleClick()) {
                threshold.reset();
                return;
            }
            if (abs(y - levelY(threshold)) < 8) dragging = 3;
            return;
        }
        if (p.over(tqx, tqy, ts, ts)) {
            dragging = 4;
            return;
        }
        if (p.over(glx, gly, glw, glh)) {
            dragging = 5;
            const double f = (x - glx) / glw;
            dragAttack = (f >= segAt[1] && f < segAt[2]) || (f >= segAt[3] && f < segAt[4]);
            return;
        }
        // Characters.
        for (int c = 0; c < 6; ++c)
            if (p.over(cx0 + 8, rowY + c * rowH, cw - 16, rowH - 3)) act(0, character, c);
        // The key strip.
        const double cy = ky + 35;
        if (p.over(kKey, cy - 11, 70, 22)) act(0, key, 0);
        if (p.over(kKey + 76, cy - 11, 70, 22)) act(0, key, 1);
        if (p.over(kStereo, cy - 11, 44, 22)) act(0, stereo, 0);
        if (p.over(kStereo + 50, cy - 11, 44, 22)) act(0, stereo, 1);
        if (p.over(kAhead, cy - 11, 36, 22)) act(0, lookahead, 0);
        if (p.over(kAhead + 40, cy - 11, 30, 22)) act(0, lookahead, 1);
        if (p.over(kAhead + 74, cy - 11, 30, 22)) act(0, lookahead, 2);
        if (p.over(kAhead + 108, cy - 11, 30, 22)) act(0, lookahead, 3);
        if (p.over(kListen, cy - 11, 40, 22)) act(0, listen, 0);
        if (p.over(kListen + 44, cy - 11, 44, 22)) act(0, listen, listen.index() == 1 ? 0 : 1);
        if (p.over(kListen + 92, cy - 11, 54, 22)) act(0, listen, listen.index() == 2 ? 0 : 2);
        // Sync, match.
        for (int n = 0; n < 7; ++n)
            if (p.over(sx + imod(n, 4) * 36, sy + 12 + idiv(n, 4) * 27, 32, 22)) act(0, sync, n);
        if (p.over(ox + ow - 82, by + 9, 66, 22)) act(0, match, match.on() ? 0 : 1);
    }
};
