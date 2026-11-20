// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Sedge: a chromatic tuner.
//
// It passes the input through untouched (or, with Mute, silent, to tune on
// stage) and shows the note it hears and how far from it the pitch lies.
//
// One plain detector. Every 15 ms or so it takes the last Window of sound
// and finds the lag at which the sound best repeats: the normalised square
// difference of McLeod's method, which reads 1 where a stretch matches the
// one a lag before it exactly. The lag is found coarsely at about 6 kHz, then
// to a fraction of a sample at the full rate in the fundamental alone: a
// string's overtones run a little sharp and fade first, so a period read from
// all of them reads sharp and sags as the note dies. A reading counts when the input
// is above the threshold, the sound clearly pitched and still sounding at the
// window's end; anything else leaves the display where it was. Readings of the same note are
// smoothed a little; a new note is shown once two readings agree on it, so a stray reading never
// flashes up a wrong one. A longer window listens to more periods, steadier
// and surer on low strings, and answers later.
//
// The display is in cells, as a tuner's lights: a row of coarse ones across
// the half-semitone either side, the middle one lit while the pitch is within
// the tolerance, and above it a tuft of fine ones a cent apart, so a cent's
// drift shows even while the middle cell stays lit. A thin mark under the
// coarse row follows the pitch between cells.
//
// The notes are equal-tempered, or tuned to one of five older temperaments
// built on a key; A4 stays at the reference either way.

constexpr double kLowHz = 25.0;     // the lowest pitch looked for (A0 is 27.5 Hz)
constexpr double kHighHz = 2200.0;  // and the highest (C7 is 2093 Hz)
constexpr double kPick = 0.9;       // a period's peak: the first this close to the highest
constexpr double kClear = 0.85;     // how clearly pitched a reading must be to count
constexpr double kSettle = 0.05;    // the display's time constant on one note, in seconds
constexpr double kLive = 0.25;      // seconds without a reading before the display dims
constexpr int kFineCells = 10;      // fine cells either side of the middle, a cent each
constexpr int kSideCells = 8;       // coarse cells either side of the middle one

double ratioCents(double ratio, int semitones) { return 1200.0 * log2(ratio) - 100.0 * semitones; }

// --- the look --------------------------------------------------------------------------

constexpr int kInk = 0x1A1611;
constexpr int kGroundTop = 0x29231B;
constexpr int kGroundBottom = 0x1D1913;
constexpr int kGraphTop = 0x1D1914;
constexpr int kGraphBottom = 0x13110D;
constexpr int kCell = 0x241F18;  // an unlit cell
constexpr int kCellEdge = 0x3A3228;
constexpr int kKnobLight = 0x54493B;
constexpr int kKnobDark = 0x2A231C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kSedge = 0xA9BC62;  // the blades: in tune
constexpr int kSedgeDeep = 0x5F6C38;
constexpr int kSeed = 0xB0844F;   // the seed heads
constexpr int kAmber = 0xDCA24E;  // near
constexpr int kRust = 0xC8653E;   // well off
constexpr int kLinen = 0xE6D9C0;

constexpr double kW = 780;
constexpr double kH = 572;

struct Sedge : Effect {
    static constexpr Info info = {
        .name = "Sedge",
        .id = "com.suppleaudio.sedge",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "Chromatic tuner: the note heard and how many cents off it is, in equal or an older "
            "temperament, with the sound passed through untouched or muted.",
        .features = {"analyzer"}};

    Param reference = frequency("Reference", 415, 466, 440)
                          .modulated()
                          .in("Tuning")
                          .help("The pitch of A4, in Hz: 440 is concert pitch, 415 baroque.");
    Param temperament =
        choice("Temperament",
               {"Equal", "Just", "Pythagorean", "Meantone", "Werckmeister III", "Vallotti"}, 0)
            .keys({"equal", "just", "pythagorean", "meantone", "werckmeister3", "vallotti"})
            .in("Tuning")
            .help(
                "What the notes are tuned to: equal temperament, or 5-limit just intonation, "
                "Pythagorean, quarter-comma meantone, Werckmeister III or Vallotti on Key.");
    Param key = choice("Key", {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"}, 0)
                    .keys({"c", "d_flat", "d", "e_flat", "e", "f", "f_sharp", "g", "a_flat", "a",
                           "b_flat", "b"})
                    .activeUnless(temperament, {"Equal"})
                    .in("Tuning")
                    .help("The note the temperament is built on.");
    Param window =
        milliseconds("Window", 10, 200, 40)
            .modulated()
            .in("Listening")
            .help(
                "How much sound each reading listens to: longer is steadier and surer on low "
                "notes (80 ms or more for a bass), shorter answers sooner.");
    Param tolerance = cents("Tolerance", 1, 10, 3)
                          .modulated()
                          .in("Listening")
                          .help("How close to the note counts as in tune, in cents either way.");
    Param threshold = decibels("Threshold", -90, -20, -60)
                          .modulated()
                          .in("Listening")
                          .help(
                              "The input's peak level, in dBFS, below which nothing is read: "
                              "the display holds the last note.");
    Param mute = toggle("Mute", false)
                     .in("Listening")
                     .help("Silences the output, faded over 10 ms, to tune without being heard.");

    Param note =
        choice("Note", {"None", "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"}, 0)
            .keys({"none", "c", "d_flat", "d", "e_flat", "e", "f", "f_sharp", "g", "a_flat", "a",
                   "b_flat", "b"})
            .meter()
            .in("Reading")
            .help("The note nearest the pitch last heard; None until one is heard.");
    Param octave = integer("Octave", 0, 8, 0)
                       .meter()
                       .in("Reading")
                       .help("The note's octave, as in A4 (the A above middle C).");
    Param offset = cents("Cents", -50, 50, 0)
                       .withKey("cents")
                       .meter()
                       .in("Reading")
                       .help("How far the pitch is from the note, in cents: positive is sharp.");
    Param heard = continuous("Frequency", 0, 4000, 0, Unit::Hertz)
                      .withKey("frequency")
                      .meter()
                      .in("Reading")
                      .help("The pitch last heard, in Hz; 0 until one is heard.");

    void factoryPatches(Patches& p) {
        p.add("Concert A440");
        p.add("A442").set(reference, 442);
        p.add("Baroque A415").set(reference, 415);
        p.add("Bass").set(window, 100);
        p.add("Stage, Muted").set(mute, true);
        p.add("Werckmeister III").set(temperament, "werckmeister3");
        p.add("Vallotti").set(temperament, "vallotti");
        p.add("Meantone").set(temperament, "meantone");
    }

    // --- audio ------------------------------------------------------------------------------

    double rate = 48000.0;
    int down = 8;    // the coarse search's decimation
    int tauLow = 2;  // its lags, at the decimated rate
    int tauHigh = 240;
    int spanMost = 1201;  // its longest window, at the decimated rate
    int fineMost = 9600;  // and the full rate's
    int ringSize = 1;
    int writeAt = 0;
    Buffer<double> ring;     // the filtered input, at the full rate
    Buffer<double> coarse;   // a reading's decimated stretch, newest first
    Buffer<double> energy;   // its running sum of squares
    Buffer<double> curve;    // the normalised square difference by lag
    Buffer<double> fine;     // a reading's full-rate stretch, newest first
    Array<int, 256> peaks;   // the coarse curve's key maxima, by lag
    Biquad dc;               // DC out
    Array<Biquad, 4> guard;  // the low-pass before the decimation
    Array<Biquad, 4> fund;   // a reading's low-pass to its fundamental
    double level = 0.0, fall = 0.999;
    double ramp = 1.0, rampStep = 0.001;
    bool unplayed = true;

    // The window, set from its parameter: lengths, the hop between readings,
    // and the work a frame may do toward a reading.
    int span = 320, fineSpan = 1920, hop = 480, hopLeft = 480, perFrame = 256;
    double glide = 0.18;

    // The reading in progress: stage 0 waits for the next hop.
    int stage = 0, base = 0, lag = 0, centre = 0, climbs = 0, copied = 0, filterAt = 0;
    int useSpan = 320, useFine = 1920, fineLength = 0, settle = 0;
    double clarity = 0.0, nLeft = 0.0, nMid = 0.0, nRight = 0.0;

    // What is shown, in semitones from A4 at 440 Hz.
    bool any = false, pending = false;
    double shown = 0.0, candidate = 0.0, sinceHeard = 1.0;
    Array<double, 12> table;  // each pitch class's offset from equal, in cents

    // What the editor shows.
    Shared<int> showNote, showOctave;
    Shared<double> showCents, showHz, inputDb;
    Shared<bool> live;

    void prepare() {
        rate = sampleRate();
        down = max(1, floorToInt(rate / 6000.0));
        const double low = rate / down;
        tauLow = max(1, floorToInt(low / kHighHz) - 1);
        tauHigh = ceilToInt(low / kLowHz);
        spanMost = ceilToInt(0.2 * low) + 1;
        fineMost = ceilToInt(0.2 * rate) + 1;
        const int lagMost = (tauHigh + 3) * down + 4;
        coarse.resize(spanMost + tauHigh + 2);
        energy.resize(spanMost + tauHigh + 3);
        curve.resize(tauHigh + 2);
        // The full-rate stretch, with up to 100 ms before it for the
        // fundamental's low-pass to settle in.
        fine.resize(fineMost + lagMost + ceilToInt(0.1 * rate) + 4);
        // The ring holds the longest stretch a reading reads, and the hop of
        // new sound written while it is worked out.
        const int hopMost = ceilToInt(0.2 / 4.0 * rate) + roundToInt(0.015 * rate) + 2;
        ringSize = max((spanMost + tauHigh + 2) * down, fine.size()) + hopMost + 16;
        ring.resize(ringSize);
        ring.fill(0.0);
        coarse.fill(0.0);
        energy.fill(0.0);
        curve.fill(0.0);
        fine.fill(0.0);
        writeAt = 0;
        dc.highpass(20.0, 0.7071);
        // Eighth-order: C7 passes, and what folds down onto it is 28 dB down.
        const double cutoff = min(2400.0, 0.4 * low);
        guard[0].lowpass(cutoff, 0.5098);
        guard[1].lowpass(cutoff, 0.6013);
        guard[2].lowpass(cutoff, 0.9000);
        guard[3].lowpass(cutoff, 2.5629);
        fall = dbToGain(-20.0 / rate);  // the input meter falls 20 dB a second
        rampStep = 1.0 / max(1.0, 0.01 * rate);
        unplayed = true;
        stage = 0;
        setWindow();
        hopLeft = hop;
        makeTable();
    }

    void update() {
        if (unplayed) ramp = mute.on() ? 0.0 : 1.0;
        if (window.changed()) setWindow();
        if (temperament.changed() || key.changed()) makeTable();
    }

    void setWindow() {
        const double seconds = window.value() * 0.001;
        const double decimated = rate / down;
        span = clamp(roundToInt(seconds * decimated), 8, spanMost);
        fineSpan = clamp(roundToInt(seconds * rate), 8, fineMost);
        // A reading every 15 ms, or every quarter of a long window.
        hop = max(roundToInt(0.015 * rate), roundToInt(seconds * rate / 4.0));
        glide = 1.0 - exp(-hop / rate / kSettle);
        // A reading's work, spread over half its hop: the copies, the coarse
        // curve, the fundamental's filter and about eight lags at the full
        // rate. A frame does at most 12 000 turns and then finishes the step
        // it is in (40 000 at most), inside the player's budget of 65 536 a
        // frame, as a block may be one frame.
        const double work = (span + tauHigh) * 2.0 + 1.0 * span * tauHigh + 60.0 * (fineSpan + 64);
        perFrame = clamp(ceilToInt(work / (0.5 * hop)), 256, 12000);
    }

    // Each pitch class's offset from equal temperament, in cents, with A at 0.
    void makeTable() {
        // From the key up, and for the circle of fifths each fifth's
        // distance from an equal one: key-G, G-D, ... F-key.
        Array<double, 12> fromKey;
        Array<double, 12> fifths;
        fromKey.fill(0.0);
        fifths.fill(0.0);
        const double pure = ratioCents(1.5, 7);
        const double comma = 12.0 * pure;  // the Pythagorean comma
        const double syntonic = 1200.0 * log2(81.0 / 80.0);
        const int t = temperament.index();
        if (t == 1) {
            fromKey = {0.0,
                       ratioCents(16.0 / 15.0, 1),
                       ratioCents(9.0 / 8.0, 2),
                       ratioCents(6.0 / 5.0, 3),
                       ratioCents(5.0 / 4.0, 4),
                       ratioCents(4.0 / 3.0, 5),
                       ratioCents(45.0 / 32.0, 6),
                       ratioCents(3.0 / 2.0, 7),
                       ratioCents(8.0 / 5.0, 8),
                       ratioCents(5.0 / 3.0, 9),
                       ratioCents(9.0 / 5.0, 10),
                       ratioCents(15.0 / 8.0, 11)};
        } else if (t >= 2) {
            fifths.fill(pure);
            if (t == 2) {
                // Eleven pure fifths; the wolf, the ninth (G#-Eb on C), closes the circle.
                fifths[8] = pure - comma;
            } else if (t == 3) {
                // Fifths a quarter of the syntonic comma narrow, so the thirds are pure.
                fifths.fill(pure - 0.25 * syntonic);
                fifths[8] = -11.0 * (pure - 0.25 * syntonic);
            } else if (t == 4) {
                // C-G, G-D, D-A and B-F# (on C) a quarter comma narrow.
                fifths[0] = pure - 0.25 * comma;
                fifths[1] = pure - 0.25 * comma;
                fifths[2] = pure - 0.25 * comma;
                fifths[5] = pure - 0.25 * comma;
            } else {
                // F-C to E-B (on C) a sixth of a comma narrow.
                fifths[0] = pure - comma / 6.0;
                fifths[1] = pure - comma / 6.0;
                fifths[2] = pure - comma / 6.0;
                fifths[3] = pure - comma / 6.0;
                fifths[4] = pure - comma / 6.0;
                fifths[11] = pure - comma / 6.0;
            }
            double at = 0.0;
            for (int k = 0; k < 12; ++k) {
                fromKey[imod(7 * k, 12)] = at;
                at += fifths[k];
            }
        }
        const int k0 = key.index();
        const double a = fromKey[wrap(9 - k0, 12)];
        for (int pc = 0; pc < 12; ++pc) table[pc] = fromKey[wrap(pc - k0, 12)] - a;
    }

    void process(Block& io) {
        if (unplayed) unplayed = false;
        const double target = mute.on() ? 0.0 : 1.0;
        const double share = 1.0 / max(1, io.channels);
        for (int i = 0; i < io.frames; ++i) {
            double x = 0.0;
            for (int c = 0; c < io.channels; ++c) x += io[c][i];
            x = isFinite(x) ? x * share : 0.0;
            level = max(abs(x), level * fall);
            double y = dc.process(x);
            for (int f = 0; f < 4; ++f) y = guard[f].process(y);
            ring[writeAt] = y;
            writeAt = writeAt + 1 == ringSize ? 0 : writeAt + 1;
            hopLeft -= 1;
            if (hopLeft <= 0) {
                hopLeft = hop;
                if (stage == 0) begin();
            }
            int work = 0;
            while (stage != 0 && work < perFrame) work += step();
            if (ramp != target)
                ramp = target > ramp ? min(target, ramp + rampStep) : max(target, ramp - rampStep);
            if (ramp != 1.0)
                for (int c = 0; c < io.channels; ++c) io[c][i] *= ramp;
        }
        report();
    }

    // A reading starts from the newest frame, with the window as it is now.
    void begin() {
        base = writeAt;
        useSpan = span;
        useFine = fineSpan;
        stage = gainToDb(level) >= threshold.value() ? 1 : 10;
    }

    // The filtered input `back` frames before the reading's newest.
    double past(int back) const { return ring[wrap(base - 1 - back, ringSize)]; }

    // The normalised square difference at full-rate lag `l`.
    double fineAt(int l) const {
        double r = 0.0, m = 0.0;
        for (int j = 0; j < useFine; ++j) {
            const double a = fine[j], b = fine[j + l];
            r += a * b;
            m += a * a + b * b;
        }
        return m > 1e-20 ? 2.0 * r / m : 0.0;
    }

    // One step of a reading; it returns about how many loop turns it took.
    // 1 copies the decimated stretch, 2 takes one lag of the coarse curve, 3
    // picks the period, 4 copies the full-rate stretch a chunk at a time, 5
    // filters it to the fundamental a chunk at a time, 6 to 8 read the three
    // lags about the guess, 9 climbs to the highest and reads the pitch
    // there; 10 is a reading of nothing.
    int step() {
        if (stage == 1) {
            const int n = useSpan + tauHigh + 1;
            energy[0] = 0.0;
            for (int k = 0; k < n; ++k) {
                const double v = past(k * down);
                coarse[k] = v;
                energy[k + 1] = energy[k] + v * v;
            }
            lag = tauLow;
            stage = 2;
            return n;
        }
        if (stage == 2) {
            double r = 0.0;
            for (int j = 0; j < useSpan; ++j) r += coarse[j] * coarse[j + lag];
            const double m = energy[useSpan] + energy[useSpan + lag] - energy[lag];
            curve[lag] = m > 1e-20 ? 2.0 * r / m : 0.0;
            lag += 1;
            if (lag > tauHigh) stage = 3;
            return useSpan;
        }
        if (stage == 3) {
            pick();
            return tauHigh;
        }
        if (stage == 4) {
            int at = wrap(base - 1 - copied, ringSize);
            int n = 0;
            const int length = fineLength + settle;
            while (n < 8192 && copied < length) {
                fine[copied] = ring[at];
                copied += 1;
                n += 1;
                at = at == 0 ? ringSize - 1 : at - 1;
            }
            if (copied >= length) {
                // Filtered oldest first, from rest: the settling falls
                // before the stretch read.
                for (int f = 0; f < 4; ++f) fund[f].clear();
                filterAt = length - 1;
                stage = 5;
            }
            return n;
        }
        if (stage == 5) {
            int n = 0;
            while (n < 2048 && filterAt >= 0) {
                double v = fine[filterAt];
                for (int f = 0; f < 4; ++f) v = fund[f].process(v);
                fine[filterAt] = v;
                filterAt -= 1;
                n += 1;
            }
            if (filterAt < 0) stage = 6;
            return 10 * n;
        }
        if (stage == 6) {
            nLeft = fineAt(centre - 1);
            stage = 7;
            return useFine;
        }
        if (stage == 7) {
            nMid = fineAt(centre);
            stage = 8;
            return useFine;
        }
        if (stage == 8) {
            nRight = fineAt(centre + 1);
            climbs = 0;
            stage = 9;
            return useFine;
        }
        if (stage == 9) {
            // A climb a lag at a time toward the higher side, then the top
            // of a parabola through the three about the highest.
            const bool room = climbs < 2 * down + 2;
            if (room && nRight > nMid) {
                centre += 1;
                nLeft = nMid;
                nMid = nRight;
                nRight = fineAt(centre + 1);
                climbs += 1;
                return useFine;
            }
            if (room && nLeft > nMid && centre > 2) {
                centre -= 1;
                nRight = nMid;
                nMid = nLeft;
                nLeft = fineAt(centre - 1);
                climbs += 1;
                return useFine;
            }
            const double bend = nLeft - 2.0 * nMid + nRight;
            const double shift = bend < 0.0 ? clamp(0.5 * (nLeft - nRight) / bend, -0.5, 0.5) : 0.0;
            const double top = nMid - 0.25 * (nLeft - nRight) * shift;
            read(top >= kClear ? rate / (centre + shift) : 0.0);
            stage = 0;
            return 1;
        }
        read(0.0);
        stage = 0;
        return 1;
    }

    // McLeod's choice: once the curve has first dipped below zero, each
    // rise above zero has a highest point (a key maximum); the period is the
    // first of those within kPick of the highest of them all.
    void pick() {
        int count = 0, top = -1;
        bool armed = false;
        double best = 0.0;
        for (int t = tauLow; t <= tauHigh; ++t) {
            const double v = curve[t];
            if (!armed) {
                armed = v < 0.0;
            } else if (v > 0.0) {
                if (top < 0 || v > curve[top]) top = t;
            } else if (top >= 0) {
                if (count < 256) {
                    peaks[count] = top;
                    count += 1;
                }
                best = max(best, curve[top]);
                top = -1;
            }
        }
        // A rise still going at the longest lag counts once it has turned.
        if (top >= 0 && top < tauHigh && count < 256) {
            peaks[count] = top;
            count += 1;
            best = max(best, curve[top]);
        }
        int chosen = -1;
        for (int k = 0; k < count && chosen < 0; ++k)
            if (curve[peaks[k]] >= kPick * best) chosen = peaks[k];
        if (chosen <= tauLow || chosen >= tauHigh || curve[chosen] < kClear) {
            stage = 10;
            return;
        }
        // The sound must still be sounding at the window's end: a window a
        // note stops in reads the period wrong, and the display would hold
        // that. Its newest whole periods, an eighth of the window or more,
        // keep half the power of all the reading compares, or it does not
        // count. (Whole periods: a shorter slice of a low note is quiet or
        // loud by its phase.)
        const int newest = chosen * max(1, idiv(useSpan + 8 * chosen - 1, 8 * chosen));
        const int all = useSpan + chosen;
        if (energy[newest] * all < 0.5 * newest * energy[all]) {
            stage = 10;
            return;
        }
        const double a = curve[chosen - 1], b = curve[chosen], c = curve[chosen + 1];
        const double bend = a - 2.0 * b + c;
        const double shift = bend < 0.0 ? clamp(0.5 * (a - c) / bend, -0.5, 0.5) : 0.0;
        const double guess = (chosen + shift) * down;
        centre = max(2, roundToInt(guess));
        // The full-rate stretch: the window, or a period if that is longer,
        // against itself a lag back.
        useFine = max(useFine, min(centre, fineMost));
        fineLength = useFine + centre + 2 * down + 6;
        settle = min(4 * centre, ceilToInt(0.1 * rate));
        // An eighth-order Butterworth low-pass a little above the pitch: the
        // second harmonic 35 dB down, so the fundamental leads even where
        // it is the weaker.
        const double corner = min(1.25 * rate / guess, 0.4 * rate);
        fund[0].lowpass(corner, 0.5098);
        fund[1].lowpass(corner, 0.6013);
        fund[2].lowpass(corner, 0.9000);
        fund[3].lowpass(corner, 2.5629);
        copied = 0;
        clarity = b;
        stage = 4;
    }

    // A reading: a pitch, or 0 for none.
    void read(double hz) {
        if (hz < kLowHz || hz > kHighHz) {
            sinceHeard += hop / rate;
            pending = false;
            return;
        }
        sinceHeard = 0.0;
        const double m = 12.0 * log2(hz / 440.0);
        if (any && abs(m - shown) < 0.5) {
            shown += glide * (m - shown);
            pending = false;
        } else if (pending && abs(m - candidate) < 0.3) {
            shown = m;
            any = true;
            pending = false;
        } else {
            pending = true;
            candidate = m;
        }
    }

    // The readouts and the display, from what is shown.
    void report() {
        inputDb.set(gainToDb(level));
        live.set(any && sinceHeard < kLive);
        if (!any) return;
        // Semitones from C-1, against the nearest note as tuned.
        const double m = shown - 12.0 * log2(reference.value() / 440.0) + 69.0;
        const int n0 = roundToInt(m);
        int n = n0;
        double best = 1e9;
        for (int k = n0 - 1; k <= n0 + 1; ++k) {
            const double d = m - (k + 0.01 * table[wrap(k, 12)]);
            if (abs(d) < abs(best)) {
                best = d;
                n = k;
            }
        }
        const double c = clamp(100.0 * best, -50.0, 50.0);
        const double hz = 440.0 * pow(2.0, shown / 12.0);
        note.set(wrap(n, 12) + 1);
        octave.set(clamp(floorToInt(n / 12.0) - 1, 0, 8));
        offset.set(c);
        heard.set(hz);
        showNote.set(wrap(n, 12) + 1);
        showOctave.set(floorToInt(n / 12.0) - 1);
        showCents.set(c);
        showHz.set(hz);
    }

    // --- the editor's layout ------------------------------------------------------------------

    // The header's input meter, with the threshold on it, and Mute.
    static constexpr double ix = 470, iy = 24, iw = 170;
    static constexpr double muteX = 690, muteY = 15, muteW = 74, pillH = 22;
    // The reading.
    static constexpr double rx = 16, ry = 60, rw = 748, rh = 344;
    static constexpr double cx = rx + rw * 0.5;
    static constexpr double bladeW = 16, bladeGap = 8, bladeBase = ry + 204, bladeTall = 64;
    static constexpr double rowX = rx + 24, rowW = rw - 48, rowY = ry + 228, rowH = 88;
    static constexpr double midW = 84, cellGap = 6;
    // The settings, two panels side by side: tuning (the temperament, its
    // key and A4) and listening (the window, threshold and tolerance). Each
    // row of pills has its name over it, both rows the same width.
    static constexpr double sy = 416, sh = 140;
    static constexpr double tx = 16, tw = 496;
    static constexpr double lx = tx + tw + 12, lw = rx + rw - lx;
    static constexpr double pillX = tx + 16, tempW = 62, tempStep = 66, keyW = 29, keyStep = 33;
    static constexpr double tempY = sy + 50, keyY = sy + 98;
    static constexpr double knobR = 22, knobY = sy + 75;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the threshold
    int dragKnob = -1;
    double dragValue = 0.0;
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
              int colour, bool hot, bool dim) {
        g.globalAlpha(dim ? 0.4 : 1.0);
        g.fillStyle(lit ? colour : (hot ? 0x3A3127 : Theme::raised));
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

    bool hovering(double x, double y, double w, double h) const {
        return hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit from `from` (a position) to the value.
    void dial(Canvas& g, double x, double y, double norm, double from, bool hot, const char* name) {
        const double r = knobR;
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double base0 = start + sweep * clamp(from, 0.0, 1.0);
        g.strokeStyle(kSedge);
        g.lineWidth(3.5);
        if (abs(at - base0) > 0.001) {
            g.beginPath();
            if (at >= base0)
                g.arc(x, y, r, base0, at);
            else
                g.arc(x, y, r, at, base0);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 4, hot ? 0x635646 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 7);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 15), y + sin(at) * (r - 15));
        g.lineTo(x + cos(at) * (r - 9), y + sin(at) * (r - 9));
        g.stroke();
        label(g, name, x, y - r - 11, TextAlign::Center);
    }

    // A blade of sedge: narrow, leaning, to a point.
    void blade(Canvas& g, double x, double y, double lean, double tall, double wide) {
        g.beginPath();
        g.moveTo(x - wide, y);
        g.quadraticCurveTo(x + lean * 0.25, y - tall * 0.6, x + lean, y - tall);
        g.quadraticCurveTo(x + lean * 0.25 + wide * 0.8, y - tall * 0.55, x + wide, y);
        g.closePath();
        g.fill();
    }

    // The mark: a tuft of blades with a seed spike on its stem.
    void drawMark(Canvas& g, double x, double y, bool tuned) {
        g.fillStyle(tuned ? kSedge : kSedgeDeep);
        blade(g, x - 4, y + 14, -12, 20, 1.6);
        blade(g, x + 4, y + 14, 12, 19, 1.6);
        blade(g, x - 1, y + 14, -6, 26, 1.6);
        blade(g, x + 1, y + 14, 5, 24, 1.6);
        g.strokeStyle(kSeed);
        g.lineWidth(1.2);
        g.lineCap(LineCap::Round);
        g.beginPath();
        g.moveTo(x, y + 14);
        g.quadraticCurveTo(x + 1, y - 1, x + 2, y - 13);
        g.stroke();
        g.fillStyle(kSeed);
        g.beginPath();
        g.ellipse(x + 2.2, y - 16, 2.3, 5.0);
        g.fill();
    }

    // The colour of a pitch `c` cents off: the blades' green within the
    // tolerance, amber near it, rust well off.
    int shade(double c) const {
        const double off = abs(c), tol = tolerance.value();
        if (off <= tol) return kSedge;
        if (off <= max(3.0 * tol, 10.0)) return kAmber;
        return kRust;
    }

    // --- the header ---------------------------------------------------------------------------

    double thresholdX() const {
        return ix + iw * clamp((threshold.value() + 90.0) / 90.0, 0.0, 1.0);
    }

    void drawHeader(Canvas& g, bool tuned) {
        drawMark(g, 30, 25, tuned);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Sedge", 52, 37);
        const double tw = g.measureText("Sedge");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("chromatic tuner", 60 + tw, 37);
        // The input, and on it the threshold below which nothing is read.
        label(g, "INPUT", ix - 10, iy + 2, TextAlign::Right);
        const double db = inputDb.get();
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(ix, iy - 2, iw, 8, 4);
        g.fill();
        const double t = clamp((db + 90.0) / 90.0, 0.0, 1.0);
        if (t > 0.0) {
            g.linearGradient(ix, 0, ix + iw, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
            g.globalAlpha(db >= threshold.value() ? 1.0 : 0.4);
            g.beginPath();
            g.roundRect(ix, iy - 2, iw * t, 8, 4);
            g.fill();
            g.globalAlpha(1.0);
        }
        const double tx = thresholdX();
        const bool hot = dragging == 2 || hovering(ix - 6, iy - 12, iw + 12, 26);
        g.fillStyle(hot ? Theme::text : kAmber);
        g.beginPath();
        g.moveTo(tx, iy - 3);
        g.lineTo(tx - 4.5, iy - 10);
        g.lineTo(tx + 4.5, iy - 10);
        g.closePath();
        g.fill();
        g.fillRect(tx - 0.75, iy - 3, 1.5, 10);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.0f dB", threshold.value()), clamp(tx, ix + 16, ix + iw - 16), iy + 17);
        pill(g, muteX, muteY, muteW, pillH, "MUTE", mute.on(), Theme::alert,
             hovering(muteX, muteY, muteW, pillH), false);
    }

    // --- the reading --------------------------------------------------------------------------

    // The note's name: its letter, a flat or sharp, and its octave.
    void drawNote(Canvas& g, int n, int oct, int colour) {
        // 1 C, 2 Db, 3 D ... 12 B: the black keys as flats, but F#.
        const int pc = n - 1;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        const int white = !black ? pc : (pc == 6 ? 5 : pc + 1);
        const char* letter = "C";
        if (white == 2) letter = "D";
        if (white == 4) letter = "E";
        if (white == 5) letter = "F";
        if (white == 7) letter = "G";
        if (white == 9) letter = "A";
        if (white == 11) letter = "B";
        g.fillStyle(colour);
        g.serifFont(76);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText(letter, cx, ry + 104);
        const double half = g.measureText(letter) * 0.5;
        g.textAlign(TextAlign::Left);
        if (black) {
            g.serifFont(30);
            g.fillText(pc == 6 ? "#" : "b", cx + half + 3, ry + 62);
        }
        g.serifFont(24);
        g.fillText(format("%d", oct), cx + half + 4, ry + 104);
    }

    // Where the pitch `c` cents off lies along the coarse row: the middle
    // cell holds the tolerance, each side cell an equal share of the rest.
    double rowAt(double c) const {
        const double tol = tolerance.value();
        const double sideW = (rowW - midW) * 0.5;
        if (abs(c) <= tol) return cx + 0.5 * midW * c / tol;
        const double u = clamp((abs(c) - tol) / (50.0 - tol), 0.0, 1.0);
        const double x = 0.5 * midW + u * sideW;
        return c > 0.0 ? cx + x : cx - x;
    }

    // The coarse cell that `c` lies in: 0 the middle, then 1 to kSideCells
    // outward, negative flat.
    int cellOf(double c) const {
        const double tol = tolerance.value();
        if (abs(c) <= tol) return 0;
        const double each = (50.0 - tol) / kSideCells;
        const int k = min(kSideCells, 1 + floorToInt((abs(c) - tol) / each));
        return c > 0.0 ? k : -k;
    }

    void drawCell(Canvas& g, double x, double y, double w, double h, bool lit, int colour,
                  double fade) {
        g.fillStyle(kCell);
        g.beginPath();
        g.roundRect(x, y, w, h, 3);
        g.fill();
        g.strokeStyle(kCellEdge);
        g.lineWidth(1);
        g.stroke();
        if (!lit) return;
        g.globalAlpha(fade);
        g.fillStyle(colour);
        g.beginPath();
        g.roundRect(x, y, w, h, 3);
        g.fill();
        g.globalAlpha(1.0);
    }

    // The fine cells: a tuft of blades a cent apart, tallest in the middle.
    void drawBlades(Canvas& g, bool shown, double c, double fade) {
        const double tol = tolerance.value();
        const int lit = shown ? clamp(roundToInt(c), -kFineCells, kFineCells) : 99;
        const double step = bladeW + bladeGap;
        for (int k = -kFineCells; k <= kFineCells; ++k) {
            const double tall = bladeTall * (1.0 - 0.62 * abs(k) / kFineCells);
            const double x = cx + k * step - bladeW * 0.5;
            const bool on = k == lit;
            drawCell(g, x, bladeBase - tall, bladeW, tall, on, shade(c), fade);
            // The cells within the tolerance carry a trace of green.
            if (!on && abs(k) <= tol) {
                g.fillStyle(kSedge);
                g.globalAlpha(0.12);
                g.fillRect(x + 1, bladeBase - tall + 1, bladeW - 2, tall - 2);
                g.globalAlpha(1.0);
            }
        }
        // Past the tuft's end, an arrow on the end cell says which way.
        if (shown && abs(c) > kFineCells + 0.5) {
            const double dir = c > 0.0 ? 1.0 : -1.0;
            const double ax = cx + dir * (kFineCells * step + bladeW * 0.5 + 10);
            const double ay = bladeBase - bladeTall * 0.38 * 0.5;
            g.globalAlpha(fade);
            g.fillStyle(shade(c));
            g.beginPath();
            g.moveTo(ax + dir * 7, ay);
            g.lineTo(ax, ay - 6);
            g.lineTo(ax, ay + 6);
            g.closePath();
            g.fill();
            g.globalAlpha(1.0);
        }
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        for (int k = -kFineCells; k <= kFineCells; k += 5) {
            const double x = cx + k * step;
            if (k == 0)
                g.fillText("0", x, bladeBase + 10);
            else
                g.fillText(format("%+d", k), x, bladeBase + 10);
        }
    }

    // The coarse cells: the middle one, the tolerance, wide; eight a side.
    void drawRow(Canvas& g, bool shown, double c, double fade) {
        const int lit = shown ? cellOf(c) : 99;
        const double sideW = (rowW - midW) * 0.5;
        const double each = (sideW - kSideCells * cellGap) / kSideCells;
        const int colour = shade(c);
        drawCell(g, cx - midW * 0.5, rowY, midW, rowH, lit == 0, colour, fade);
        for (int k = 1; k <= kSideCells; ++k) {
            const double off = 0.5 * midW + cellGap + (k - 1) * (each + cellGap);
            drawCell(g, cx + off, rowY, each, rowH, lit == k, colour, fade);
            drawCell(g, cx - off - each, rowY, each, rowH, lit == -k, colour, fade);
        }
        // The mark between cells: where the pitch is exactly.
        if (shown) {
            const double at = rowAt(c);
            g.globalAlpha(fade);
            g.fillStyle(kLinen);
            g.fillRect(at - 0.75, rowY + rowH + 3, 1.5, 7);
            g.beginPath();
            g.moveTo(at, rowY + rowH + 3);
            g.lineTo(at - 4.5, rowY + rowH + 10);
            g.lineTo(at + 4.5, rowY + rowH + 10);
            g.closePath();
            g.fill();
            g.globalAlpha(1.0);
        }
        label(g, "FLAT", rowX, rowY + rowH + 16, TextAlign::Left);
        label(g, "SHARP", rowX + rowW, rowY + rowH + 16, TextAlign::Right);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("±%.1f", tolerance.value()), cx, rowY + rowH + 16);
        g.fillText("-25", rowAt(-25.0), rowY + rowH + 16);
        g.fillText("+25", rowAt(25.0), rowY + rowH + 16);
    }

    void drawReading(Canvas& g, bool tuned) {
        panel(g, rx, ry, rw, rh);
        const int n = showNote.get();
        const bool shown = n > 0;
        const bool now = live.get();
        const double fade = now ? 1.0 : 0.4;
        const double c = showCents.get();
        // What the notes are tuned to, top left; the window, top right.
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(11.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        double at = rx + 16;
        if (temperament.index() == 0) {
            g.fillText("equal temperament", at, ry + 18);
            at += g.measureText("equal temperament");
        } else {
            g.fillText(temperament.text(), at, ry + 18);
            at += g.measureText(temperament.text());
            g.fillText(" on ", at, ry + 18);
            at += g.measureText(" on ");
            g.fillText(key.text(), at, ry + 18);
            at += g.measureText(key.text());
        }
        g.fillText(format(", A4 = %.1f Hz", reference.value()), at, ry + 18);
        g.textAlign(TextAlign::Right);
        g.fillText(format("listening %.0f ms at a time", window.value()), rx + rw - 16, ry + 18);
        // The note.
        if (shown) {
            g.globalAlpha(fade);
            drawNote(g, n, showOctave.get(), tuned ? kSedge : Theme::text);
            g.globalAlpha(1.0);
        } else {
            g.fillStyle(Theme::textFaint);
            g.serifItalicFont(22);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Alphabetic);
            g.fillText("play a note", cx, ry + 92);
        }
        // Cents to the left, hertz to the right.
        const double lx = rx + 150, hx = rx + rw - 150;
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Alphabetic);
        g.globalAlpha(shown ? fade : 0.35);
        g.fillStyle(shown ? shade(c) : Theme::textFaint);
        g.monoFont(28);
        if (!shown)
            g.fillText("--", lx, ry + 92);
        else if (abs(c) < 0.05)
            g.fillText("0.0", lx, ry + 92);
        else
            g.fillText(format("%+.1f", c), lx, ry + 92);
        g.fillStyle(shown ? Theme::text : Theme::textFaint);
        g.monoFont(22);
        if (shown)
            g.fillText(format("%.2f", showHz.get()), hx, ry + 90);
        else
            g.fillText("--", hx, ry + 90);
        g.globalAlpha(1.0);
        label(g, "CENTS", lx, ry + 108, TextAlign::Center);
        label(g, "HZ", hx, ry + 108, TextAlign::Center);
        // What it makes of it, under the note.
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(12);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        const char* state = "";
        if (shown && now) state = tuned ? "in tune" : (c > 0.0 ? "sharp" : "flat");
        if (shown && !now) state = "holding the last note";
        g.fillText(state, cx, ry + 122);
        drawBlades(g, shown, c, fade);
        drawRow(g, shown, c, fade);
    }

    // --- the settings -------------------------------------------------------------------------

    // 0 A4, at the tuning panel's end; 1 window, 2 threshold, 3 tolerance,
    // across the listening panel.
    double knobX(int k) const { return k == 0 ? tx + tw - 44 : lx + 50 + 70 * (k - 1); }
    double tempX(int k) const { return pillX + tempStep * k; }
    double keyX(int k) const { return pillX + keyStep * k; }

    void drawSettings(Canvas& g) {
        panel(g, tx, sy, tw, sh);
        heading(g, "Tuning", tx + 16, sy + 18);
        panel(g, lx, sy, lw, sh);
        heading(g, "Listening", lx + 16, sy + 18);
        label(g, "TEMPERAMENT", pillX, tempY - 8, TextAlign::Left);
        const int t = temperament.index();
        for (int k = 0; k < 6; ++k) {
            const char* name = "EQUAL";
            if (k == 1) name = "JUST";
            if (k == 2) name = "PYTHAG.";
            if (k == 3) name = "MEANTONE";
            if (k == 4) name = "WERCK. III";
            if (k == 5) name = "VALLOTTI";
            pill(g, tempX(k), tempY, tempW, pillH, name, t == k, kSedge,
                 hovering(tempX(k), tempY, tempW, pillH), false);
        }
        label(g, "KEY", pillX, keyY - 8, TextAlign::Left);
        const int k0 = key.index();
        for (int k = 0; k < 12; ++k) {
            const char* name = "C";
            if (k == 1) name = "Db";
            if (k == 2) name = "D";
            if (k == 3) name = "Eb";
            if (k == 4) name = "E";
            if (k == 5) name = "F";
            if (k == 6) name = "F#";
            if (k == 7) name = "G";
            if (k == 8) name = "Ab";
            if (k == 9) name = "A";
            if (k == 10) name = "Bb";
            if (k == 11) name = "B";
            pill(g, keyX(k), keyY, keyW, pillH, name, k0 == k && t != 0, kSeed,
                 hovering(keyX(k), keyY, keyW, pillH), t == 0);
        }
        // Lit from concert pitch, 440 Hz, along the knob's logarithmic travel.
        const double concert = log(440.0 / 415.0) / log(466.0 / 415.0);
        dial(g, knobX(0), knobY, reference.normalized(), concert, hotKnob == 0, "A4");
        dial(g, knobX(1), knobY, window.normalized(), 0.0, hotKnob == 1, "WINDOW");
        dial(g, knobX(2), knobY, threshold.normalized(), 0.0, hotKnob == 2, "THRESHOLD");
        dial(g, knobX(3), knobY, tolerance.normalized(), 0.0, hotKnob == 3, "TOLERANCE");
        g.fillStyle(Theme::text);
        g.monoFont(10);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.1f Hz", reference.value()), knobX(0), knobY + knobR + 11);
        g.fillText(format("%.0f ms", window.value()), knobX(1), knobY + knobR + 11);
        g.fillText(format("%.0f dB", threshold.value()), knobX(2), knobY + knobR + 11);
        g.fillText(format("±%.1f ct", tolerance.value()), knobX(3), knobY + knobR + 11);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        const bool tuned =
            showNote.get() > 0 && live.get() && abs(showCents.get()) <= tolerance.value();
        drawHeader(g, tuned);
        drawReading(g, tuned);
        drawSettings(g);
    }

    // --- the pointer ---------------------------------------------------------------------------

    int knobAt(double x, double y) const {
        for (int k = 0; k < 4; ++k) {
            const double dx = x - knobX(k), dy = y - knobY;
            if (dx * dx + dy * dy <= (knobR + 6) * (knobR + 6)) return k;
        }
        return -1;
    }

    // The reference moves in tenths of a hertz; the others along their knobs.
    void turn(int k, double amount) {
        if (k == 0) {
            dragValue = clamp(dragValue + amount * 10.0, 415.0, 466.0);
            reference.edit(round(dragValue * 10.0) / 10.0);
        } else if (k == 1) {
            dragValue = clamp(dragValue + amount, 0.0, 1.0);
            window.editNormalized(dragValue);
        } else if (k == 2) {
            dragValue = clamp(dragValue + amount, 0.0, 1.0);
            threshold.editNormalized(dragValue);
        } else {
            dragValue = clamp(dragValue + amount, 0.0, 1.0);
            tolerance.editNormalized(dragValue);
        }
    }

    double knobStart(int k) const {
        if (k == 0) return reference.value();
        if (k == 1) return window.normalized();
        if (k == 2) return threshold.normalized();
        return tolerance.normalized();
    }

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.1 : 1.0;
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
            if (dragging == 1) turn(dragKnob, -p.dy() * fine / 200.0);
            if (dragging == 2) threshold.edit(-90.0 + 90.0 * clamp((p.x() - ix) / iw, 0.0, 1.0));
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) {
                dragValue = knobStart(k);
                turn(k, p.wheel() * fine * (k == 0 ? 0.01 : 0.02));
            }
            if (p.over(ix - 6, iy - 12, iw + 12, 26))
                threshold.edit(threshold.value() + p.wheel() * fine);
        }
    }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        const int k = knobAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                if (k == 0) reference.reset();
                if (k == 1) window.reset();
                if (k == 2) threshold.reset();
                if (k == 3) tolerance.reset();
                return;
            }
            dragging = 1;
            dragKnob = k;
            dragValue = knobStart(k);
            return;
        }
        if (p.over(ix - 6, iy - 12, iw + 12, 26)) {
            if (p.doubleClick()) {
                threshold.reset();
                return;
            }
            threshold.edit(-90.0 + 90.0 * clamp((x - ix) / iw, 0.0, 1.0));
            dragging = 2;
            return;
        }
        if (p.over(muteX, muteY, muteW, pillH)) {
            mute.edit(mute.on() ? 0 : 1);
            return;
        }
        for (int m = 0; m < 6; ++m)
            if (p.over(tempX(m), tempY, tempW, pillH)) {
                temperament.edit(m);
                return;
            }
        for (int m = 0; m < 12; ++m)
            if (p.over(keyX(m), keyY, keyW, pillH)) {
                key.edit(m);
                return;
            }
    }
};
