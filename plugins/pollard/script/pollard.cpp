// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Pollard: Steppe's drum sampler.
//
// A pollard is a tree cut back at head height, which grows a crown of new
// stems from the cut, as a coppice grows them from its stool. Pollard plays
// sixteen pads, one sound each -- a file of the user's, or one of its
// factory hits -- on the keys from First Key (36, C2: the General MIDI kick)
// up. Each pad is played by Coppice's engine, copied below: its region of
// the file, forward or reversed, one-shot or gated, through an envelope, a
// state-variable filter and its level and pan, to the main output or one of
// eight more. Pads in a choke group cut each other off; a pad's Vary makes
// each hit a little different in pitch, level and start, drawn from the hit's
// number so a render is the same every time.
//
// Files arrive resampled to the session's rate and are measured as they
// load (the runtime's peak() and rms()). As in Coppice, every voice runs
// oversampled (2x at Normal, 4x at High) and each output is brought down
// once; a voice's settings are worked out every 16 samples of its life, so a
// render is the same at any block size.

constexpr int kVoices = 32;
constexpr int kPads = 16;
constexpr int kBuses = 9;                 // the main output and Out 1 to Out 8
constexpr int kColumns = 512;             // a pad's waveform
constexpr int kPane = 256;                // points in a pad's close-up
constexpr double kPaneMs = 2.5;           // each side of its start
constexpr double kFull = 10000;           // Decay at its top: no fade
constexpr double kChokeMs = 10;           // a choked voice's fade
constexpr double kHeadroom = 0.50118723;  // -6 dB, room for hits together
constexpr int kOneShot = 0;
constexpr int kGated = 1;

// --- engine: shared with Pollard, byte for byte ------------------------------------------
//
// From here to the end of the engine, the same text is in Pollard's script
// (plugins/pollard), which plays a pad with it as this plays a key; the
// test plugin.pollard.engine-copy fails when the two differ. Make a change
// in both.

constexpr int kTick = 16;    // frames between a voice's control updates
constexpr int kHops = 4096;  // the most hops a search for hits looks at
// Loops and filters, by their choices' indices.
constexpr int kLoopOff = 0;
constexpr int kForward = 1;
constexpr int kPingPong = 2;
constexpr int kOff = 0;
constexpr int kLadder = 1;
constexpr int kSvf = 2;

// --- small pieces ------------------------------------------------------------------------

double wrap01(double x) { return x - floor(x); }

double fastTanh(double x) {
    if (x > 4.97) return 1.0;
    if (x < -4.97) return -1.0;
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

double rise(double x, double c) {
    const double k = 1.0 + c * 8.0;
    return (1.0 - exp(-k * x)) / (1.0 - exp(-k));
}
double fall(double x, double c) { return 1.0 - rise(1.0 - x, c); }

// A value from -1 to 1 for each whole number n and seed, the same everywhere:
// a random LFO's for its cycle n.
double scatter(int n, int seed) {
    int h = n * 374761393 + seed * 668265263;
    h = (h ^ (h >> 13)) * 1274126177;
    h = h ^ (h >> 16);
    return (h & 0xFFFFFF) / 8388608.0 - 1.0;
}

// An LFO's wave at phase t (0..1) of cycle n: sine, triangle, saw up, saw
// down, square, random (a new value each cycle) and drift (gliding from one
// to the next).
double lfoWave(int shape, double t, int n, int seed) {
    if (shape == 0) return sin(twoPi * t);
    if (shape == 1) return t < 0.25 ? 4.0 * t : t < 0.75 ? 2.0 - 4.0 * t : 4.0 * t - 4.0;
    if (shape == 2) return 2.0 * t - 1.0;
    if (shape == 3) return 1.0 - 2.0 * t;
    if (shape == 4) return t < 0.5 ? 1.0 : -1.0;
    if (shape == 5) return scatter(n, seed);
    const double e = t * t * (3.0 - 2.0 * t);
    return lerp(scatter(n, seed), scatter(n + 1, seed), e);
}

// A synced LFO's cycle in quarter notes, by its division's index.
double divisionBeats(int d) {
    if (d == 0) return 0.125;
    if (d == 1) return 0.25;
    if (d == 2) return 1.0 / 3.0;
    if (d == 3) return 0.5;
    if (d == 4) return 0.75;
    if (d == 5) return 2.0 / 3.0;
    if (d == 6) return 1.0;
    if (d == 7) return 1.5;
    if (d == 8) return 2.0;
    if (d == 9) return 4.0;
    if (d == 10) return 8.0;
    return 16.0;
}

int clampInt(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

// --- the voice ---------------------------------------------------------------------------
//
// What plays a sound, given numbers and the file: the envelope, the filter,
// where a note reads its file and what it hears there. None of it reads a
// parameter.

// Attack, decay, sustain, release, each segment exactly as long as its time:
// the attack straight, the decay and release exponential. It starts from
// wherever it is, so retriggering never clicks.
struct Env {
    int stage = 0;  // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
    double level = 0.0;
    double start = 0.0;
    double x = 0.0;

    void gate() {
        start = level;
        stage = 1;
        x = 0.0;
    }
    void release() {
        if (stage == 0 || stage == 4) return;
        start = level;
        stage = 4;
        x = 0.0;
    }
    void kill() {
        stage = 0;
        level = 0.0;
        start = 0.0;
        x = 0.0;
    }
    // On by dt samples, times in samples; the level at the end.
    double advance(double dt, double a, double d, double s, double r) {
        int guard = 0;
        while (dt > 0.0 && guard < 6) {
            ++guard;
            if (stage == 0) {
                level = 0.0;
                return level;
            }
            if (stage == 3) {
                level = s;
                return level;
            }
            const double length = stage == 1 ? a : stage == 2 ? d : r;
            if (length <= 0.5) {
                x = 1.0;
            } else {
                const double left = (1.0 - x) * length;
                if (dt < left) {
                    x += dt / length;
                    dt = 0.0;
                } else {
                    dt -= left;
                    x = 1.0;
                }
            }
            if (stage == 1)
                level = start + (1.0 - start) * x;
            else if (stage == 2)
                level = 1.0 + (s - 1.0) * fall(x, 0.5);
            else
                level = start * (1.0 - fall(x, 0.5));
            if (x >= 1.0) {
                x = 0.0;
                stage = stage == 1 ? 2 : stage == 2 ? 3 : 0;
                if (stage == 2) start = 1.0;
                if (stage == 0) level = 0.0;
            }
        }
        return level;
    }
};

// One channel of the filter: the ladder's four stages and the state-variable
// filter's two, so switching type never reads the other's.
struct Filter {
    Array<double, 4> stage;
    double ic1 = 0.0;
    double ic2 = 0.0;

    void clear() {
        stage.fill(0.0);
        ic1 = 0.0;
        ic2 = 0.0;
    }
    void follow(const Filter& o) {
        stage = o.stage;
        ic1 = o.ic1;
        ic2 = o.ic2;
    }

    // A transistor ladder: four one-poles with their feedback solved at once,
    // the input saturating. g the integrators' gain, k 0..4, drive a gain.
    double ladder(double x, double g, double k, double drive, bool steep) {
        const double G = g / (1.0 + g);
        const double B = 1.0 / (1.0 + g);
        const double S =
            G * G * G * B * stage[0] + G * G * B * stage[1] + G * B * stage[2] + B * stage[3];
        const double G4 = G * G * G * G;
        const double y4 = (G4 * x * drive + S) / (1.0 + k * G4);
        double u = fastTanh(x * drive - k * y4);
        double half = 0.0;
        for (int i = 0; i < 4; ++i) {
            const double v = (u - stage[i]) * G;
            const double y = v + stage[i];
            stage[i] = y + v;
            u = y;
            if (i == 1) half = y;
        }
        return (steep ? u : half) * (1.0 + 0.5 * k) / max(1.0, drive * 0.5 + 0.5);
    }

    // A state-variable filter (trapezoidal), k = 1/Q. Mode: low, band, high
    // pass, notch.
    double svf(double x, double g, double k, int mode) {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        if (mode == 0) return v2;
        if (mode == 1) return v1;
        const double high = x - k * v1 - v2;
        if (mode == 2) return high;
        return v2 + high;
    }
};

// Where a note reads: its span of the file, [a, b), played forward or
// reversed, and the loop inside it, all in "play" frames counted from where
// it starts (0 to b - a, whichever way it reads).
struct Span {
    int a = 0;
    int b = 0;
    bool reversed = false;
    int loop = kLoopOff;
    double l0 = 0.0;  // the loop, in play frames
    double l1 = 0.0;
    double fade = 0.0;  // the forward loop's crossfade, in play frames

    int length() const { return b - a; }
    // File frame of play frame q.
    double frame(double q) const { return reversed ? b - 1 - q : a + q; }
    // The loop [ls, le) of the file in play frames, its crossfade `xf`
    // frames at most: half the loop, and no more than the sound before it.
    void loopOn(int mode, int ls, int le, double xf) {
        loop = mode;
        const int lo = clampInt(ls, a, b), hi = clampInt(le, a, b);
        l0 = reversed ? b - hi : lo - a;
        l1 = reversed ? b - lo : hi - a;
        if (l1 - l0 < 2.0) loop = kLoopOff;
        fade = mode == kForward ? min(xf, min((l1 - l0) * 0.5, l0)) : 0.0;
    }
};

// Channel c of the file at play frame q of span s.
double readAt(const AudioFile& f, const Span& s, int c, double q) { return f.read(c, s.frame(q)); }

// What a note hears at play frame q: the file, and across the last `fade`
// frames before a forward loop's end, the same distance before its start
// faded in, so the loop's seam is not heard.
double hear(const AudioFile& f, const Span& s, int c, double q, bool looping) {
    const double x = readAt(f, s, c, q);
    if (!looping || s.loop != kForward || s.fade < 1.0 || q < s.l1 - s.fade || q >= s.l1) return x;
    const double t = (q - (s.l1 - s.fade)) / s.fade;
    return x * (1.0 - t) + readAt(f, s, c, q - (s.l1 - s.l0)) * t;
}

// The nearest rising zero crossing to `at` on channel 0 within `reach`
// frames each way, or `at` when there is none.
int zeroNear(const AudioFile& f, int at, int reach) {
    for (int d = 0; d <= reach; ++d) {
        if (f.sample(0, at + d - 1) < 0.0 && f.sample(0, at + d) >= 0.0) return at + d;
        if (d > 0 && f.sample(0, at - d - 1) < 0.0 && f.sample(0, at - d) >= 0.0) return at - d;
    }
    return at;
}

// The rising zero crossing at or before `at`, within `reach` frames, or
// `at` itself.
int zeroBefore(const AudioFile& f, int at, int reach) {
    for (int d = 0; d <= reach; ++d)
        if (f.sample(0, at - d - 1) < 0.0 && f.sample(0, at - d) >= 0.0) return at - d;
    return at;
}

// The loudest of the file's channels over [from, to).
double peakOf(const AudioFile& f, int from, int to) {
    double most = 0.0;
    for (int c = 0; c < min(f.channels(), 2); ++c) most = max(most, f.peak(c, from, to));
    return most;
}
double rmsOf(const AudioFile& f, int from, int to) {
    double most = 0.0;
    for (int c = 0; c < min(f.channels(), 2); ++c) most = max(most, f.rms(c, from, to));
    return most;
}

// Where the hits are in [from, to): each place the level jumps well above
// what came just before, `sensitivity` (0..1) setting how far and how loud;
// at most `most`, the first at `from`, at least 50 ms apart. A hit starts at
// the rising zero crossing just before its first loud sample.
int findHits(const AudioFile& f, int from, int to, double sensitivity, double rate,
             Array<int, 65>& at, Array<double, 4096>& level, int most) {
    at[0] = from;
    int count = 1;
    const int length = to - from;
    if (length < 512) return count;
    const int hop = max(256, (length + kHops - 1) / kHops);
    const int hops = min(kHops, idiv(length, hop));
    double loudest = 0.0;
    for (int h = 0; h < hops; ++h) {
        level[h] = rmsOf(f, from + h * hop, from + (h + 1) * hop);
        loudest = max(loudest, level[h]);
    }
    if (loudest <= 0.0) return count;
    const double quiet = loudest * dbToGain(-(12.0 + 36.0 * sensitivity));
    const double jump = 3.0 - 2.4 * sensitivity;
    const int gap = max(1, ceilToInt(0.05 * rate / hop));
    int last = 0;
    for (int h = 1; h < hops && count < most; ++h) {
        // Against the level a little before: a hit rising over two hops still counts.
        const double before = max(h >= 2 ? level[h - 2] : 0.0, h >= 3 ? level[h - 3] : 0.0);
        if (level[h] < quiet || level[h] < jump * before + loudest * 0.002 || h - last < gap)
            continue;
        // Its first loud sample, found by halving with peak(): the start of
        // the hop before, to the end of this one.
        const int lo0 = from + (h - 1) * hop, hi0 = from + (h + 1) * hop;
        const double loud = 0.3 * peakOf(f, from + h * hop, hi0);
        int lo = lo0, hi = hi0;
        for (int k = 0; k < 20 && hi - lo > 1; ++k) {
            const int mid = (lo + hi) / 2;
            if (peakOf(f, lo0, mid) > loud)
                hi = mid;
            else
                lo = mid;
        }
        const int start = max(from + 1, zeroBefore(f, hi, ceilToInt(0.002 * rate)));
        if (start - at[count - 1] < 0.05 * rate) continue;
        at[count] = start;
        ++count;
        last = h;
    }
    return count;
}

// A voice: where it is in its span, how fast it goes, and what shapes it.
struct Voice {
    Span span;
    double q = 0.0;       // the play frame
    double dir = 1.0;     // ping-pong's way
    double step = 0.0;    // play frames per (oversampled) sample, gliding
    double stepTo = 0.0;  // ... toward this
    double stepStep = 0.0;
    bool looping = false;  // in its loop (no longer, once Play On lets it go)
    bool gated = true;     // the key's release releases it
    bool narrow = true;    // the file is mono: one channel filtered and copied
    int slice = -1;

    Array<Filter, 2> filter;
    double g = 0.0, gStep = 0.0, k = 0.0, kStep = 0.0, drive = 1.0;

    Env amp;
    Env mod;
    // The LFOs a note has of its own: phase (in cycles), whole cycles, and a
    // seed for random; and how long the note has played, for their delay.
    Array<double, 2> lfoPhase;
    Array<int, 2> lfoCycle;
    int lfoSeed = 0;
    double age = 0.0;

    double ampL = 0.0, ampLStep = 0.0, ampR = 0.0, ampRStep = 0.0;
    double ampPanAt = 2.0, ampCos = 0.0, ampSin = 0.0;
    double velocity = 0.0;
    bool sounding = false;
    bool fresh = false;
    bool done = false;
    bool ending = false;  // the amp's last target was silence
    int tickLeft = 0;
    double lastL = 0.0, lastR = 0.0;
};

// Voice s on by `frames` frames from frame `from` of the block, all within
// one control tick: its file read where it is (wrapping in its loop),
// faded over a region's last `endFade` frames, filtered (`type` kOff,
// kLadder or kSvf; `steep` the ladder's 24 dB; `mode` the state-variable
// filter's) and through its amp, `factor` samples a frame, added into
// mixL and mixR from `base`. It stops, done, once it runs past its span.
void render(Voice& s, const AudioFile& f, int from, int frames, int factor, double endFade,
            int type, bool steep, int mode, Buffer<double>& mixL, Buffer<double>& mixR, int base) {
    const double length = s.span.length();
    for (int i = from; i < from + frames; ++i) {
        for (int k = 0; k < factor; ++k) {
            // Past the end, or its loop.
            if (s.looping && s.span.loop == kForward && s.q >= s.span.l1) {
                s.q -= s.span.l1 - s.span.l0;
            } else if (s.looping && s.span.loop == kPingPong) {
                if (s.dir > 0.0 && s.q >= s.span.l1) {
                    s.q = 2.0 * s.span.l1 - s.q;
                    s.dir = -1.0;
                } else if (s.dir < 0.0 && s.q < s.span.l0) {
                    s.q = 2.0 * s.span.l0 - s.q;
                    s.dir = 1.0;
                }
            }
            if (s.q >= length || s.q < 0.0) {
                s.done = true;
                return;
            }
            double l = hear(f, s.span, 0, s.q, s.looping);
            double r = s.narrow ? l : hear(f, s.span, 1, s.q, s.looping);
            if (!s.looping) {
                const double left = (length - s.q) / endFade;
                if (left < 1.0) {
                    l *= left;
                    r *= left;
                }
            }
            if (type != kOff) {
                if (type == kLadder) {
                    l = s.filter[0].ladder(l, s.g, s.k, s.drive, steep);
                    if (!s.narrow) r = s.filter[1].ladder(r, s.g, s.k, s.drive, steep);
                } else {
                    l = s.filter[0].svf(l * s.drive, s.g, s.k, mode);
                    if (!s.narrow) r = s.filter[1].svf(r * s.drive, s.g, s.k, mode);
                }
                if (s.narrow) r = l;
                s.g += s.gStep;
                s.k += s.kStep;
            }
            s.ampL += s.ampLStep;
            s.ampR += s.ampRStep;
            l *= s.ampL;
            r *= s.ampR;
            s.lastL = l;
            s.lastR = r;
            const int at = base + i * factor + k;
            mixL[at] += l;
            mixR[at] += r;
            s.q += s.step * s.dir;
            s.step += s.stepStep;
        }
    }
}

// --- end of the engine ---------------------------------------------------------------------

// A pad's settings as numbers, read from its parameters once they change.
struct Pad {
    double level = 0.0;  // dB
    double pan = 0.0;    // -1 .. 1
    double tune = 0.0;   // semitones
    double start = 0.0;  // 0 .. 1 of the file
    double end = 1.0;
    bool reverse = false;
    int play = kOneShot;
    double attack = 0.0;  // ms
    double decay = kFull;
    double release = 100.0;
    int filter = 0;  // 0 off, then the state-variable filter's mode + 1
    double cutoff = 20000.0;
    double resonance = 0.0;
    double velocity = 0.7;  // 0 .. 1
    double vary = 0.0;
    int choke = 0;   // 0 none, 1 .. 8
    int output = 0;  // 0 main, 1 .. 8
};

struct Pollard : Instrument {
    static constexpr Info info = {
        .name = "Pollard",
        .id = "com.suppleaudio.pollard",
        .vendor = "Supple Audio",
        .description =
            "Drum sampler: sixteen pads on the keys from C2, each a sample played one-shot or "
            "gated through an envelope and a filter, with choke groups, a little variation per "
            "hit and eight extra outputs.",
        .features = {"drum-machine", "sampler"},
        .outputs = {"Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"},
        .voices = kVoices};

    // --- The pads ---------------------------------------------------------------------------
    //
    // Pad N's file is the member padN, its parameters pN...: parameters
    // cannot sit in an array, so each pad has its own, read into pad[] by
    // readPads(), and the editor reaches them through padRead(), padWrite() and padText().

    // Pad 1: GM's kick (key 36 from C2).
    AudioFile pad1 = audioFile("Pad 1")
                         .withKey("pad1")
                         .withDefault("factory:analog-kick.flac")
                         .help("Pad 1's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p1Level = decibels("Pad 1 Level", -60, 12, 0)
                        .withKey("pad1.level")
                        .in("Pad 1")
                        .help("The pad's level.");
    Param p1Pan = percent("Pad 1 Pan", 0, -100, 100)
                      .withKey("pad1.pan")
                      .in("Pad 1")
                      .help("Where it sits, left to right.");
    Param p1Tune =
        semitones("Pad 1 Tune", -24, 24, 0)
            .withKey("pad1.tune")
            .in("Pad 1")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p1Start = percent("Pad 1 Start", 0)
                        .withKey("pad1.start")
                        .in("Pad 1")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p1End = percent("Pad 1 End", 100)
                      .withKey("pad1.end")
                      .in("Pad 1")
                      .help("Where it ends: the region between the two is what plays.");
    Param p1Reverse = toggle("Pad 1 Reverse", false)
                          .withKey("pad1.reverse")
                          .in("Pad 1")
                          .help("Play the region backwards, from its end to its start.");
    Param p1Play = choice("Pad 1 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad1.play")
                       .in("Pad 1")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p1Attack = milliseconds("Pad 1 Attack", 0, 500, 0)
                         .withKey("pad1.attack")
                         .in("Pad 1")
                         .help("How long it takes to rise at the start.");
    Param p1Decay = milliseconds("Pad 1 Decay", 5, kFull, kFull)
                        .withKey("pad1.decay")
                        .in("Pad 1")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p1Release = milliseconds("Pad 1 Release", 1, 5000, 100)
                          .withKey("pad1.release")
                          .in("Pad 1")
                          .activeWhen(p1Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p1Filter = choice("Pad 1 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad1.filter")
                         .in("Pad 1")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p1Cutoff = frequency("Pad 1 Cutoff", 20, 20000, 20000)
                         .withKey("pad1.cutoff")
                         .in("Pad 1")
                         .activeUnless(p1Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p1Resonance = percent("Pad 1 Resonance", 0)
                            .withKey("pad1.resonance")
                            .in("Pad 1")
                            .activeUnless(p1Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p1Velocity = percent("Pad 1 Velocity", 70)
                           .withKey("pad1.velocity")
                           .in("Pad 1")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p1Vary = percent("Pad 1 Vary", 0)
                       .withKey("pad1.vary")
                       .in("Pad 1")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p1Choke = choice("Pad 1 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad1.choke")
                        .in("Pad 1")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p1Output =
        choice("Pad 1 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad1.output")
            .in("Pad 1")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 2: GM's side stick (key 37 from C2).
    AudioFile pad2 = audioFile("Pad 2")
                         .withKey("pad2")
                         .withDefault("factory:analog-rim.flac")
                         .help("Pad 2's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p2Level = decibels("Pad 2 Level", -60, 12, -6)
                        .withKey("pad2.level")
                        .in("Pad 2")
                        .help("The pad's level.");
    Param p2Pan = percent("Pad 2 Pan", 0, -100, 100)
                      .withKey("pad2.pan")
                      .in("Pad 2")
                      .help("Where it sits, left to right.");
    Param p2Tune =
        semitones("Pad 2 Tune", -24, 24, 0)
            .withKey("pad2.tune")
            .in("Pad 2")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p2Start = percent("Pad 2 Start", 0)
                        .withKey("pad2.start")
                        .in("Pad 2")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p2End = percent("Pad 2 End", 100)
                      .withKey("pad2.end")
                      .in("Pad 2")
                      .help("Where it ends: the region between the two is what plays.");
    Param p2Reverse = toggle("Pad 2 Reverse", false)
                          .withKey("pad2.reverse")
                          .in("Pad 2")
                          .help("Play the region backwards, from its end to its start.");
    Param p2Play = choice("Pad 2 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad2.play")
                       .in("Pad 2")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p2Attack = milliseconds("Pad 2 Attack", 0, 500, 0)
                         .withKey("pad2.attack")
                         .in("Pad 2")
                         .help("How long it takes to rise at the start.");
    Param p2Decay = milliseconds("Pad 2 Decay", 5, kFull, kFull)
                        .withKey("pad2.decay")
                        .in("Pad 2")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p2Release = milliseconds("Pad 2 Release", 1, 5000, 100)
                          .withKey("pad2.release")
                          .in("Pad 2")
                          .activeWhen(p2Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p2Filter = choice("Pad 2 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad2.filter")
                         .in("Pad 2")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p2Cutoff = frequency("Pad 2 Cutoff", 20, 20000, 20000)
                         .withKey("pad2.cutoff")
                         .in("Pad 2")
                         .activeUnless(p2Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p2Resonance = percent("Pad 2 Resonance", 0)
                            .withKey("pad2.resonance")
                            .in("Pad 2")
                            .activeUnless(p2Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p2Velocity = percent("Pad 2 Velocity", 70)
                           .withKey("pad2.velocity")
                           .in("Pad 2")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p2Vary = percent("Pad 2 Vary", 0)
                       .withKey("pad2.vary")
                       .in("Pad 2")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p2Choke = choice("Pad 2 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad2.choke")
                        .in("Pad 2")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p2Output =
        choice("Pad 2 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad2.output")
            .in("Pad 2")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 3: GM's snare (key 38 from C2).
    AudioFile pad3 = audioFile("Pad 3")
                         .withKey("pad3")
                         .withDefault("factory:analog-snare.flac")
                         .help("Pad 3's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p3Level = decibels("Pad 3 Level", -60, 12, -2)
                        .withKey("pad3.level")
                        .in("Pad 3")
                        .help("The pad's level.");
    Param p3Pan = percent("Pad 3 Pan", 0, -100, 100)
                      .withKey("pad3.pan")
                      .in("Pad 3")
                      .help("Where it sits, left to right.");
    Param p3Tune =
        semitones("Pad 3 Tune", -24, 24, 0)
            .withKey("pad3.tune")
            .in("Pad 3")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p3Start = percent("Pad 3 Start", 0)
                        .withKey("pad3.start")
                        .in("Pad 3")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p3End = percent("Pad 3 End", 100)
                      .withKey("pad3.end")
                      .in("Pad 3")
                      .help("Where it ends: the region between the two is what plays.");
    Param p3Reverse = toggle("Pad 3 Reverse", false)
                          .withKey("pad3.reverse")
                          .in("Pad 3")
                          .help("Play the region backwards, from its end to its start.");
    Param p3Play = choice("Pad 3 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad3.play")
                       .in("Pad 3")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p3Attack = milliseconds("Pad 3 Attack", 0, 500, 0)
                         .withKey("pad3.attack")
                         .in("Pad 3")
                         .help("How long it takes to rise at the start.");
    Param p3Decay = milliseconds("Pad 3 Decay", 5, kFull, kFull)
                        .withKey("pad3.decay")
                        .in("Pad 3")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p3Release = milliseconds("Pad 3 Release", 1, 5000, 100)
                          .withKey("pad3.release")
                          .in("Pad 3")
                          .activeWhen(p3Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p3Filter = choice("Pad 3 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad3.filter")
                         .in("Pad 3")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p3Cutoff = frequency("Pad 3 Cutoff", 20, 20000, 20000)
                         .withKey("pad3.cutoff")
                         .in("Pad 3")
                         .activeUnless(p3Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p3Resonance = percent("Pad 3 Resonance", 0)
                            .withKey("pad3.resonance")
                            .in("Pad 3")
                            .activeUnless(p3Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p3Velocity = percent("Pad 3 Velocity", 70)
                           .withKey("pad3.velocity")
                           .in("Pad 3")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p3Vary = percent("Pad 3 Vary", 0)
                       .withKey("pad3.vary")
                       .in("Pad 3")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p3Choke = choice("Pad 3 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad3.choke")
                        .in("Pad 3")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p3Output =
        choice("Pad 3 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad3.output")
            .in("Pad 3")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 4: GM's clap (key 39 from C2).
    AudioFile pad4 = audioFile("Pad 4")
                         .withKey("pad4")
                         .withDefault("factory:analog-clap.flac")
                         .help("Pad 4's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p4Level = decibels("Pad 4 Level", -60, 12, -3)
                        .withKey("pad4.level")
                        .in("Pad 4")
                        .help("The pad's level.");
    Param p4Pan = percent("Pad 4 Pan", 0, -100, 100)
                      .withKey("pad4.pan")
                      .in("Pad 4")
                      .help("Where it sits, left to right.");
    Param p4Tune =
        semitones("Pad 4 Tune", -24, 24, 0)
            .withKey("pad4.tune")
            .in("Pad 4")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p4Start = percent("Pad 4 Start", 0)
                        .withKey("pad4.start")
                        .in("Pad 4")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p4End = percent("Pad 4 End", 100)
                      .withKey("pad4.end")
                      .in("Pad 4")
                      .help("Where it ends: the region between the two is what plays.");
    Param p4Reverse = toggle("Pad 4 Reverse", false)
                          .withKey("pad4.reverse")
                          .in("Pad 4")
                          .help("Play the region backwards, from its end to its start.");
    Param p4Play = choice("Pad 4 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad4.play")
                       .in("Pad 4")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p4Attack = milliseconds("Pad 4 Attack", 0, 500, 0)
                         .withKey("pad4.attack")
                         .in("Pad 4")
                         .help("How long it takes to rise at the start.");
    Param p4Decay = milliseconds("Pad 4 Decay", 5, kFull, kFull)
                        .withKey("pad4.decay")
                        .in("Pad 4")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p4Release = milliseconds("Pad 4 Release", 1, 5000, 100)
                          .withKey("pad4.release")
                          .in("Pad 4")
                          .activeWhen(p4Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p4Filter = choice("Pad 4 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad4.filter")
                         .in("Pad 4")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p4Cutoff = frequency("Pad 4 Cutoff", 20, 20000, 20000)
                         .withKey("pad4.cutoff")
                         .in("Pad 4")
                         .activeUnless(p4Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p4Resonance = percent("Pad 4 Resonance", 0)
                            .withKey("pad4.resonance")
                            .in("Pad 4")
                            .activeUnless(p4Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p4Velocity = percent("Pad 4 Velocity", 70)
                           .withKey("pad4.velocity")
                           .in("Pad 4")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p4Vary = percent("Pad 4 Vary", 0)
                       .withKey("pad4.vary")
                       .in("Pad 4")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p4Choke = choice("Pad 4 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad4.choke")
                        .in("Pad 4")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p4Output =
        choice("Pad 4 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad4.output")
            .in("Pad 4")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 5: GM's snare 2 (key 40 from C2).
    AudioFile pad5 = audioFile("Pad 5")
                         .withKey("pad5")
                         .withDefault("factory:analog-snare.flac")
                         .help("Pad 5's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p5Level = decibels("Pad 5 Level", -60, 12, -4)
                        .withKey("pad5.level")
                        .in("Pad 5")
                        .help("The pad's level.");
    Param p5Pan = percent("Pad 5 Pan", 0, -100, 100)
                      .withKey("pad5.pan")
                      .in("Pad 5")
                      .help("Where it sits, left to right.");
    Param p5Tune =
        semitones("Pad 5 Tune", -24, 24, 3)
            .withKey("pad5.tune")
            .in("Pad 5")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p5Start = percent("Pad 5 Start", 0)
                        .withKey("pad5.start")
                        .in("Pad 5")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p5End = percent("Pad 5 End", 100)
                      .withKey("pad5.end")
                      .in("Pad 5")
                      .help("Where it ends: the region between the two is what plays.");
    Param p5Reverse = toggle("Pad 5 Reverse", false)
                          .withKey("pad5.reverse")
                          .in("Pad 5")
                          .help("Play the region backwards, from its end to its start.");
    Param p5Play = choice("Pad 5 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad5.play")
                       .in("Pad 5")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p5Attack = milliseconds("Pad 5 Attack", 0, 500, 0)
                         .withKey("pad5.attack")
                         .in("Pad 5")
                         .help("How long it takes to rise at the start.");
    Param p5Decay = milliseconds("Pad 5 Decay", 5, kFull, kFull)
                        .withKey("pad5.decay")
                        .in("Pad 5")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p5Release = milliseconds("Pad 5 Release", 1, 5000, 100)
                          .withKey("pad5.release")
                          .in("Pad 5")
                          .activeWhen(p5Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p5Filter = choice("Pad 5 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad5.filter")
                         .in("Pad 5")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p5Cutoff = frequency("Pad 5 Cutoff", 20, 20000, 20000)
                         .withKey("pad5.cutoff")
                         .in("Pad 5")
                         .activeUnless(p5Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p5Resonance = percent("Pad 5 Resonance", 0)
                            .withKey("pad5.resonance")
                            .in("Pad 5")
                            .activeUnless(p5Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p5Velocity = percent("Pad 5 Velocity", 70)
                           .withKey("pad5.velocity")
                           .in("Pad 5")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p5Vary = percent("Pad 5 Vary", 0)
                       .withKey("pad5.vary")
                       .in("Pad 5")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p5Choke = choice("Pad 5 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad5.choke")
                        .in("Pad 5")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p5Output =
        choice("Pad 5 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad5.output")
            .in("Pad 5")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 6: GM's low floor tom (key 41 from C2).
    AudioFile pad6 = audioFile("Pad 6")
                         .withKey("pad6")
                         .withDefault("factory:analog-tom.flac")
                         .help("Pad 6's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p6Level = decibels("Pad 6 Level", -60, 12, -3)
                        .withKey("pad6.level")
                        .in("Pad 6")
                        .help("The pad's level.");
    Param p6Pan = percent("Pad 6 Pan", 0, -100, 100)
                      .withKey("pad6.pan")
                      .in("Pad 6")
                      .help("Where it sits, left to right.");
    Param p6Tune =
        semitones("Pad 6 Tune", -24, 24, -5)
            .withKey("pad6.tune")
            .in("Pad 6")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p6Start = percent("Pad 6 Start", 0)
                        .withKey("pad6.start")
                        .in("Pad 6")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p6End = percent("Pad 6 End", 100)
                      .withKey("pad6.end")
                      .in("Pad 6")
                      .help("Where it ends: the region between the two is what plays.");
    Param p6Reverse = toggle("Pad 6 Reverse", false)
                          .withKey("pad6.reverse")
                          .in("Pad 6")
                          .help("Play the region backwards, from its end to its start.");
    Param p6Play = choice("Pad 6 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad6.play")
                       .in("Pad 6")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p6Attack = milliseconds("Pad 6 Attack", 0, 500, 0)
                         .withKey("pad6.attack")
                         .in("Pad 6")
                         .help("How long it takes to rise at the start.");
    Param p6Decay = milliseconds("Pad 6 Decay", 5, kFull, kFull)
                        .withKey("pad6.decay")
                        .in("Pad 6")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p6Release = milliseconds("Pad 6 Release", 1, 5000, 100)
                          .withKey("pad6.release")
                          .in("Pad 6")
                          .activeWhen(p6Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p6Filter = choice("Pad 6 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad6.filter")
                         .in("Pad 6")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p6Cutoff = frequency("Pad 6 Cutoff", 20, 20000, 20000)
                         .withKey("pad6.cutoff")
                         .in("Pad 6")
                         .activeUnless(p6Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p6Resonance = percent("Pad 6 Resonance", 0)
                            .withKey("pad6.resonance")
                            .in("Pad 6")
                            .activeUnless(p6Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p6Velocity = percent("Pad 6 Velocity", 70)
                           .withKey("pad6.velocity")
                           .in("Pad 6")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p6Vary = percent("Pad 6 Vary", 0)
                       .withKey("pad6.vary")
                       .in("Pad 6")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p6Choke = choice("Pad 6 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad6.choke")
                        .in("Pad 6")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p6Output =
        choice("Pad 6 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad6.output")
            .in("Pad 6")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 7: GM's closed hat (key 42 from C2).
    AudioFile pad7 = audioFile("Pad 7")
                         .withKey("pad7")
                         .withDefault("factory:analog-hat.flac")
                         .help("Pad 7's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p7Level = decibels("Pad 7 Level", -60, 12, -6)
                        .withKey("pad7.level")
                        .in("Pad 7")
                        .help("The pad's level.");
    Param p7Pan = percent("Pad 7 Pan", 0, -100, 100)
                      .withKey("pad7.pan")
                      .in("Pad 7")
                      .help("Where it sits, left to right.");
    Param p7Tune =
        semitones("Pad 7 Tune", -24, 24, 0)
            .withKey("pad7.tune")
            .in("Pad 7")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p7Start = percent("Pad 7 Start", 0)
                        .withKey("pad7.start")
                        .in("Pad 7")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p7End = percent("Pad 7 End", 100)
                      .withKey("pad7.end")
                      .in("Pad 7")
                      .help("Where it ends: the region between the two is what plays.");
    Param p7Reverse = toggle("Pad 7 Reverse", false)
                          .withKey("pad7.reverse")
                          .in("Pad 7")
                          .help("Play the region backwards, from its end to its start.");
    Param p7Play = choice("Pad 7 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad7.play")
                       .in("Pad 7")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p7Attack = milliseconds("Pad 7 Attack", 0, 500, 0)
                         .withKey("pad7.attack")
                         .in("Pad 7")
                         .help("How long it takes to rise at the start.");
    Param p7Decay = milliseconds("Pad 7 Decay", 5, kFull, kFull)
                        .withKey("pad7.decay")
                        .in("Pad 7")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p7Release = milliseconds("Pad 7 Release", 1, 5000, 100)
                          .withKey("pad7.release")
                          .in("Pad 7")
                          .activeWhen(p7Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p7Filter = choice("Pad 7 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad7.filter")
                         .in("Pad 7")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p7Cutoff = frequency("Pad 7 Cutoff", 20, 20000, 20000)
                         .withKey("pad7.cutoff")
                         .in("Pad 7")
                         .activeUnless(p7Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p7Resonance = percent("Pad 7 Resonance", 0)
                            .withKey("pad7.resonance")
                            .in("Pad 7")
                            .activeUnless(p7Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p7Velocity = percent("Pad 7 Velocity", 70)
                           .withKey("pad7.velocity")
                           .in("Pad 7")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p7Vary = percent("Pad 7 Vary", 0)
                       .withKey("pad7.vary")
                       .in("Pad 7")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p7Choke = choice("Pad 7 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 1)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad7.choke")
                        .in("Pad 7")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p7Output =
        choice("Pad 7 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad7.output")
            .in("Pad 7")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 8: GM's high floor tom (key 43 from C2).
    AudioFile pad8 = audioFile("Pad 8")
                         .withKey("pad8")
                         .withDefault("factory:analog-tom.flac")
                         .help("Pad 8's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p8Level = decibels("Pad 8 Level", -60, 12, -3)
                        .withKey("pad8.level")
                        .in("Pad 8")
                        .help("The pad's level.");
    Param p8Pan = percent("Pad 8 Pan", 0, -100, 100)
                      .withKey("pad8.pan")
                      .in("Pad 8")
                      .help("Where it sits, left to right.");
    Param p8Tune =
        semitones("Pad 8 Tune", -24, 24, -3)
            .withKey("pad8.tune")
            .in("Pad 8")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p8Start = percent("Pad 8 Start", 0)
                        .withKey("pad8.start")
                        .in("Pad 8")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p8End = percent("Pad 8 End", 100)
                      .withKey("pad8.end")
                      .in("Pad 8")
                      .help("Where it ends: the region between the two is what plays.");
    Param p8Reverse = toggle("Pad 8 Reverse", false)
                          .withKey("pad8.reverse")
                          .in("Pad 8")
                          .help("Play the region backwards, from its end to its start.");
    Param p8Play = choice("Pad 8 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad8.play")
                       .in("Pad 8")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p8Attack = milliseconds("Pad 8 Attack", 0, 500, 0)
                         .withKey("pad8.attack")
                         .in("Pad 8")
                         .help("How long it takes to rise at the start.");
    Param p8Decay = milliseconds("Pad 8 Decay", 5, kFull, kFull)
                        .withKey("pad8.decay")
                        .in("Pad 8")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p8Release = milliseconds("Pad 8 Release", 1, 5000, 100)
                          .withKey("pad8.release")
                          .in("Pad 8")
                          .activeWhen(p8Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p8Filter = choice("Pad 8 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad8.filter")
                         .in("Pad 8")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p8Cutoff = frequency("Pad 8 Cutoff", 20, 20000, 20000)
                         .withKey("pad8.cutoff")
                         .in("Pad 8")
                         .activeUnless(p8Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p8Resonance = percent("Pad 8 Resonance", 0)
                            .withKey("pad8.resonance")
                            .in("Pad 8")
                            .activeUnless(p8Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p8Velocity = percent("Pad 8 Velocity", 70)
                           .withKey("pad8.velocity")
                           .in("Pad 8")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p8Vary = percent("Pad 8 Vary", 0)
                       .withKey("pad8.vary")
                       .in("Pad 8")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p8Choke = choice("Pad 8 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad8.choke")
                        .in("Pad 8")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p8Output =
        choice("Pad 8 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad8.output")
            .in("Pad 8")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 9: GM's pedal hat (key 44 from C2).
    AudioFile pad9 = audioFile("Pad 9")
                         .withKey("pad9")
                         .withDefault("factory:analog-hat.flac")
                         .help("Pad 9's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p9Level = decibels("Pad 9 Level", -60, 12, -8)
                        .withKey("pad9.level")
                        .in("Pad 9")
                        .help("The pad's level.");
    Param p9Pan = percent("Pad 9 Pan", 0, -100, 100)
                      .withKey("pad9.pan")
                      .in("Pad 9")
                      .help("Where it sits, left to right.");
    Param p9Tune =
        semitones("Pad 9 Tune", -24, 24, -2)
            .withKey("pad9.tune")
            .in("Pad 9")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p9Start = percent("Pad 9 Start", 0)
                        .withKey("pad9.start")
                        .in("Pad 9")
                        .help("Where in the file it starts; played backwards, where it ends.");
    Param p9End = percent("Pad 9 End", 100)
                      .withKey("pad9.end")
                      .in("Pad 9")
                      .help("Where it ends: the region between the two is what plays.");
    Param p9Reverse = toggle("Pad 9 Reverse", false)
                          .withKey("pad9.reverse")
                          .in("Pad 9")
                          .help("Play the region backwards, from its end to its start.");
    Param p9Play = choice("Pad 9 Play", {"One-Shot", "Gated"}, 0)
                       .keys({"one_shot", "gated"})
                       .withKey("pad9.play")
                       .in("Pad 9")
                       .help(
                           "One-Shot plays the region whatever the key does; Gated fades it "
                           "through Release once the key is up.");
    Param p9Attack = milliseconds("Pad 9 Attack", 0, 500, 0)
                         .withKey("pad9.attack")
                         .in("Pad 9")
                         .help("How long it takes to rise at the start.");
    Param p9Decay = milliseconds("Pad 9 Decay", 5, kFull, 40)
                        .withKey("pad9.decay")
                        .in("Pad 9")
                        .help(
                            "How long it takes to fade to silence; at its top, Full, it does not "
                            "fade and the region plays to its end.");
    Param p9Release = milliseconds("Pad 9 Release", 1, 5000, 100)
                          .withKey("pad9.release")
                          .in("Pad 9")
                          .activeWhen(p9Play, {"gated"})
                          .help("How long a gated pad fades once its key is up.");
    Param p9Filter = choice("Pad 9 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                         .keys({"off", "lowpass", "bandpass", "highpass"})
                         .withKey("pad9.filter")
                         .in("Pad 9")
                         .help("A state-variable filter on the pad: low, band or high pass.");
    Param p9Cutoff = frequency("Pad 9 Cutoff", 20, 20000, 20000)
                         .withKey("pad9.cutoff")
                         .in("Pad 9")
                         .activeUnless(p9Filter, {"off"})
                         .help(
                             "The filter's frequency; 20 kHz by default, which leaves a low pass "
                             "open, so set it with a band or high pass (choosing one in the editor "
                             "moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p9Resonance = percent("Pad 9 Resonance", 0)
                            .withKey("pad9.resonance")
                            .in("Pad 9")
                            .activeUnless(p9Filter, {"off"})
                            .help("The filter's peak at its cutoff.");
    Param p9Velocity = percent("Pad 9 Velocity", 70)
                           .withKey("pad9.velocity")
                           .in("Pad 9")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p9Vary = percent("Pad 9 Vary", 0)
                       .withKey("pad9.vary")
                       .in("Pad 9")
                       .help(
                           "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 dB "
                           "of level and 1 ms further into the sound.");
    Param p9Choke = choice("Pad 9 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 1)
                        .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                               "group7", "group8"})
                        .withKey("pad9.choke")
                        .in("Pad 9")
                        .help(
                            "Pads in a group cut each other off, and themselves: the closed hat "
                            "stops the open one.");
    Param p9Output =
        choice("Pad 9 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad9.output")
            .in("Pad 9")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 10: GM's low tom (key 45 from C2).
    AudioFile pad10 =
        audioFile("Pad 10")
            .withKey("pad10")
            .withDefault("factory:analog-tom.flac")
            .help("Pad 10's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p10Level = decibels("Pad 10 Level", -60, 12, -3)
                         .withKey("pad10.level")
                         .in("Pad 10")
                         .help("The pad's level.");
    Param p10Pan = percent("Pad 10 Pan", 0, -100, 100)
                       .withKey("pad10.pan")
                       .in("Pad 10")
                       .help("Where it sits, left to right.");
    Param p10Tune =
        semitones("Pad 10 Tune", -24, 24, -1)
            .withKey("pad10.tune")
            .in("Pad 10")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p10Start = percent("Pad 10 Start", 0)
                         .withKey("pad10.start")
                         .in("Pad 10")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p10End = percent("Pad 10 End", 100)
                       .withKey("pad10.end")
                       .in("Pad 10")
                       .help("Where it ends: the region between the two is what plays.");
    Param p10Reverse = toggle("Pad 10 Reverse", false)
                           .withKey("pad10.reverse")
                           .in("Pad 10")
                           .help("Play the region backwards, from its end to its start.");
    Param p10Play = choice("Pad 10 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad10.play")
                        .in("Pad 10")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p10Attack = milliseconds("Pad 10 Attack", 0, 500, 0)
                          .withKey("pad10.attack")
                          .in("Pad 10")
                          .help("How long it takes to rise at the start.");
    Param p10Decay = milliseconds("Pad 10 Decay", 5, kFull, kFull)
                         .withKey("pad10.decay")
                         .in("Pad 10")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p10Release = milliseconds("Pad 10 Release", 1, 5000, 100)
                           .withKey("pad10.release")
                           .in("Pad 10")
                           .activeWhen(p10Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p10Filter = choice("Pad 10 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad10.filter")
                          .in("Pad 10")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p10Cutoff = frequency("Pad 10 Cutoff", 20, 20000, 20000)
                          .withKey("pad10.cutoff")
                          .in("Pad 10")
                          .activeUnless(p10Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p10Resonance = percent("Pad 10 Resonance", 0)
                             .withKey("pad10.resonance")
                             .in("Pad 10")
                             .activeUnless(p10Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p10Velocity = percent("Pad 10 Velocity", 70)
                            .withKey("pad10.velocity")
                            .in("Pad 10")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p10Vary = percent("Pad 10 Vary", 0)
                        .withKey("pad10.vary")
                        .in("Pad 10")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p10Choke = choice("Pad 10 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad10.choke")
                         .in("Pad 10")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p10Output =
        choice("Pad 10 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad10.output")
            .in("Pad 10")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 11: GM's open hat (key 46 from C2).
    AudioFile pad11 =
        audioFile("Pad 11")
            .withKey("pad11")
            .withDefault("factory:analog-open-hat.flac")
            .help("Pad 11's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p11Level = decibels("Pad 11 Level", -60, 12, -6)
                         .withKey("pad11.level")
                         .in("Pad 11")
                         .help("The pad's level.");
    Param p11Pan = percent("Pad 11 Pan", 0, -100, 100)
                       .withKey("pad11.pan")
                       .in("Pad 11")
                       .help("Where it sits, left to right.");
    Param p11Tune =
        semitones("Pad 11 Tune", -24, 24, 0)
            .withKey("pad11.tune")
            .in("Pad 11")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p11Start = percent("Pad 11 Start", 0)
                         .withKey("pad11.start")
                         .in("Pad 11")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p11End = percent("Pad 11 End", 100)
                       .withKey("pad11.end")
                       .in("Pad 11")
                       .help("Where it ends: the region between the two is what plays.");
    Param p11Reverse = toggle("Pad 11 Reverse", false)
                           .withKey("pad11.reverse")
                           .in("Pad 11")
                           .help("Play the region backwards, from its end to its start.");
    Param p11Play = choice("Pad 11 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad11.play")
                        .in("Pad 11")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p11Attack = milliseconds("Pad 11 Attack", 0, 500, 0)
                          .withKey("pad11.attack")
                          .in("Pad 11")
                          .help("How long it takes to rise at the start.");
    Param p11Decay = milliseconds("Pad 11 Decay", 5, kFull, kFull)
                         .withKey("pad11.decay")
                         .in("Pad 11")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p11Release = milliseconds("Pad 11 Release", 1, 5000, 100)
                           .withKey("pad11.release")
                           .in("Pad 11")
                           .activeWhen(p11Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p11Filter = choice("Pad 11 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad11.filter")
                          .in("Pad 11")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p11Cutoff = frequency("Pad 11 Cutoff", 20, 20000, 20000)
                          .withKey("pad11.cutoff")
                          .in("Pad 11")
                          .activeUnless(p11Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p11Resonance = percent("Pad 11 Resonance", 0)
                             .withKey("pad11.resonance")
                             .in("Pad 11")
                             .activeUnless(p11Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p11Velocity = percent("Pad 11 Velocity", 70)
                            .withKey("pad11.velocity")
                            .in("Pad 11")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p11Vary = percent("Pad 11 Vary", 0)
                        .withKey("pad11.vary")
                        .in("Pad 11")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p11Choke = choice("Pad 11 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 1)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad11.choke")
                         .in("Pad 11")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p11Output =
        choice("Pad 11 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad11.output")
            .in("Pad 11")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 12: GM's low-mid tom (key 47 from C2).
    AudioFile pad12 =
        audioFile("Pad 12")
            .withKey("pad12")
            .withDefault("factory:analog-tom.flac")
            .help("Pad 12's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p12Level = decibels("Pad 12 Level", -60, 12, -3)
                         .withKey("pad12.level")
                         .in("Pad 12")
                         .help("The pad's level.");
    Param p12Pan = percent("Pad 12 Pan", 0, -100, 100)
                       .withKey("pad12.pan")
                       .in("Pad 12")
                       .help("Where it sits, left to right.");
    Param p12Tune =
        semitones("Pad 12 Tune", -24, 24, 1)
            .withKey("pad12.tune")
            .in("Pad 12")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p12Start = percent("Pad 12 Start", 0)
                         .withKey("pad12.start")
                         .in("Pad 12")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p12End = percent("Pad 12 End", 100)
                       .withKey("pad12.end")
                       .in("Pad 12")
                       .help("Where it ends: the region between the two is what plays.");
    Param p12Reverse = toggle("Pad 12 Reverse", false)
                           .withKey("pad12.reverse")
                           .in("Pad 12")
                           .help("Play the region backwards, from its end to its start.");
    Param p12Play = choice("Pad 12 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad12.play")
                        .in("Pad 12")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p12Attack = milliseconds("Pad 12 Attack", 0, 500, 0)
                          .withKey("pad12.attack")
                          .in("Pad 12")
                          .help("How long it takes to rise at the start.");
    Param p12Decay = milliseconds("Pad 12 Decay", 5, kFull, kFull)
                         .withKey("pad12.decay")
                         .in("Pad 12")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p12Release = milliseconds("Pad 12 Release", 1, 5000, 100)
                           .withKey("pad12.release")
                           .in("Pad 12")
                           .activeWhen(p12Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p12Filter = choice("Pad 12 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad12.filter")
                          .in("Pad 12")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p12Cutoff = frequency("Pad 12 Cutoff", 20, 20000, 20000)
                          .withKey("pad12.cutoff")
                          .in("Pad 12")
                          .activeUnless(p12Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p12Resonance = percent("Pad 12 Resonance", 0)
                             .withKey("pad12.resonance")
                             .in("Pad 12")
                             .activeUnless(p12Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p12Velocity = percent("Pad 12 Velocity", 70)
                            .withKey("pad12.velocity")
                            .in("Pad 12")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p12Vary = percent("Pad 12 Vary", 0)
                        .withKey("pad12.vary")
                        .in("Pad 12")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p12Choke = choice("Pad 12 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad12.choke")
                         .in("Pad 12")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p12Output =
        choice("Pad 12 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad12.output")
            .in("Pad 12")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 13: GM's high-mid tom (key 48 from C2).
    AudioFile pad13 =
        audioFile("Pad 13")
            .withKey("pad13")
            .withDefault("factory:analog-tom.flac")
            .help("Pad 13's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p13Level = decibels("Pad 13 Level", -60, 12, -3)
                         .withKey("pad13.level")
                         .in("Pad 13")
                         .help("The pad's level.");
    Param p13Pan = percent("Pad 13 Pan", 0, -100, 100)
                       .withKey("pad13.pan")
                       .in("Pad 13")
                       .help("Where it sits, left to right.");
    Param p13Tune =
        semitones("Pad 13 Tune", -24, 24, 3)
            .withKey("pad13.tune")
            .in("Pad 13")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p13Start = percent("Pad 13 Start", 0)
                         .withKey("pad13.start")
                         .in("Pad 13")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p13End = percent("Pad 13 End", 100)
                       .withKey("pad13.end")
                       .in("Pad 13")
                       .help("Where it ends: the region between the two is what plays.");
    Param p13Reverse = toggle("Pad 13 Reverse", false)
                           .withKey("pad13.reverse")
                           .in("Pad 13")
                           .help("Play the region backwards, from its end to its start.");
    Param p13Play = choice("Pad 13 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad13.play")
                        .in("Pad 13")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p13Attack = milliseconds("Pad 13 Attack", 0, 500, 0)
                          .withKey("pad13.attack")
                          .in("Pad 13")
                          .help("How long it takes to rise at the start.");
    Param p13Decay = milliseconds("Pad 13 Decay", 5, kFull, kFull)
                         .withKey("pad13.decay")
                         .in("Pad 13")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p13Release = milliseconds("Pad 13 Release", 1, 5000, 100)
                           .withKey("pad13.release")
                           .in("Pad 13")
                           .activeWhen(p13Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p13Filter = choice("Pad 13 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad13.filter")
                          .in("Pad 13")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p13Cutoff = frequency("Pad 13 Cutoff", 20, 20000, 20000)
                          .withKey("pad13.cutoff")
                          .in("Pad 13")
                          .activeUnless(p13Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p13Resonance = percent("Pad 13 Resonance", 0)
                             .withKey("pad13.resonance")
                             .in("Pad 13")
                             .activeUnless(p13Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p13Velocity = percent("Pad 13 Velocity", 70)
                            .withKey("pad13.velocity")
                            .in("Pad 13")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p13Vary = percent("Pad 13 Vary", 0)
                        .withKey("pad13.vary")
                        .in("Pad 13")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p13Choke = choice("Pad 13 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad13.choke")
                         .in("Pad 13")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p13Output =
        choice("Pad 13 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad13.output")
            .in("Pad 13")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 14: GM's crash (key 49 from C2).
    AudioFile pad14 =
        audioFile("Pad 14")
            .withKey("pad14")
            .withDefault("factory:analog-cymbal.flac")
            .help("Pad 14's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p14Level = decibels("Pad 14 Level", -60, 12, -8)
                         .withKey("pad14.level")
                         .in("Pad 14")
                         .help("The pad's level.");
    Param p14Pan = percent("Pad 14 Pan", 0, -100, 100)
                       .withKey("pad14.pan")
                       .in("Pad 14")
                       .help("Where it sits, left to right.");
    Param p14Tune =
        semitones("Pad 14 Tune", -24, 24, 0)
            .withKey("pad14.tune")
            .in("Pad 14")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p14Start = percent("Pad 14 Start", 0)
                         .withKey("pad14.start")
                         .in("Pad 14")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p14End = percent("Pad 14 End", 100)
                       .withKey("pad14.end")
                       .in("Pad 14")
                       .help("Where it ends: the region between the two is what plays.");
    Param p14Reverse = toggle("Pad 14 Reverse", false)
                           .withKey("pad14.reverse")
                           .in("Pad 14")
                           .help("Play the region backwards, from its end to its start.");
    Param p14Play = choice("Pad 14 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad14.play")
                        .in("Pad 14")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p14Attack = milliseconds("Pad 14 Attack", 0, 500, 0)
                          .withKey("pad14.attack")
                          .in("Pad 14")
                          .help("How long it takes to rise at the start.");
    Param p14Decay = milliseconds("Pad 14 Decay", 5, kFull, kFull)
                         .withKey("pad14.decay")
                         .in("Pad 14")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p14Release = milliseconds("Pad 14 Release", 1, 5000, 100)
                           .withKey("pad14.release")
                           .in("Pad 14")
                           .activeWhen(p14Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p14Filter = choice("Pad 14 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad14.filter")
                          .in("Pad 14")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p14Cutoff = frequency("Pad 14 Cutoff", 20, 20000, 20000)
                          .withKey("pad14.cutoff")
                          .in("Pad 14")
                          .activeUnless(p14Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p14Resonance = percent("Pad 14 Resonance", 0)
                             .withKey("pad14.resonance")
                             .in("Pad 14")
                             .activeUnless(p14Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p14Velocity = percent("Pad 14 Velocity", 70)
                            .withKey("pad14.velocity")
                            .in("Pad 14")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p14Vary = percent("Pad 14 Vary", 0)
                        .withKey("pad14.vary")
                        .in("Pad 14")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p14Choke = choice("Pad 14 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad14.choke")
                         .in("Pad 14")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p14Output =
        choice("Pad 14 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad14.output")
            .in("Pad 14")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 15: GM's high tom (key 50 from C2).
    AudioFile pad15 =
        audioFile("Pad 15")
            .withKey("pad15")
            .withDefault("factory:analog-tom.flac")
            .help("Pad 15's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p15Level = decibels("Pad 15 Level", -60, 12, -3)
                         .withKey("pad15.level")
                         .in("Pad 15")
                         .help("The pad's level.");
    Param p15Pan = percent("Pad 15 Pan", 0, -100, 100)
                       .withKey("pad15.pan")
                       .in("Pad 15")
                       .help("Where it sits, left to right.");
    Param p15Tune =
        semitones("Pad 15 Tune", -24, 24, 5)
            .withKey("pad15.tune")
            .in("Pad 15")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p15Start = percent("Pad 15 Start", 0)
                         .withKey("pad15.start")
                         .in("Pad 15")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p15End = percent("Pad 15 End", 100)
                       .withKey("pad15.end")
                       .in("Pad 15")
                       .help("Where it ends: the region between the two is what plays.");
    Param p15Reverse = toggle("Pad 15 Reverse", false)
                           .withKey("pad15.reverse")
                           .in("Pad 15")
                           .help("Play the region backwards, from its end to its start.");
    Param p15Play = choice("Pad 15 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad15.play")
                        .in("Pad 15")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p15Attack = milliseconds("Pad 15 Attack", 0, 500, 0)
                          .withKey("pad15.attack")
                          .in("Pad 15")
                          .help("How long it takes to rise at the start.");
    Param p15Decay = milliseconds("Pad 15 Decay", 5, kFull, kFull)
                         .withKey("pad15.decay")
                         .in("Pad 15")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p15Release = milliseconds("Pad 15 Release", 1, 5000, 100)
                           .withKey("pad15.release")
                           .in("Pad 15")
                           .activeWhen(p15Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p15Filter = choice("Pad 15 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad15.filter")
                          .in("Pad 15")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p15Cutoff = frequency("Pad 15 Cutoff", 20, 20000, 20000)
                          .withKey("pad15.cutoff")
                          .in("Pad 15")
                          .activeUnless(p15Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p15Resonance = percent("Pad 15 Resonance", 0)
                             .withKey("pad15.resonance")
                             .in("Pad 15")
                             .activeUnless(p15Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p15Velocity = percent("Pad 15 Velocity", 70)
                            .withKey("pad15.velocity")
                            .in("Pad 15")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p15Vary = percent("Pad 15 Vary", 0)
                        .withKey("pad15.vary")
                        .in("Pad 15")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p15Choke = choice("Pad 15 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad15.choke")
                         .in("Pad 15")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p15Output =
        choice("Pad 15 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad15.output")
            .in("Pad 15")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // Pad 16: GM's ride (key 51 from C2).
    AudioFile pad16 =
        audioFile("Pad 16")
            .withKey("pad16")
            .withDefault("factory:fm-ride.flac")
            .help("Pad 16's sound: a WAV, AIFF or FLAC of your own, or a factory hit.");
    Param p16Level = decibels("Pad 16 Level", -60, 12, -9)
                         .withKey("pad16.level")
                         .in("Pad 16")
                         .help("The pad's level.");
    Param p16Pan = percent("Pad 16 Pan", 0, -100, 100)
                       .withKey("pad16.pan")
                       .in("Pad 16")
                       .help("Where it sits, left to right.");
    Param p16Tune =
        semitones("Pad 16 Tune", -24, 24, 0)
            .withKey("pad16.tune")
            .in("Pad 16")
            .help("Its pitch in semitones (and hundredths): the sample played faster or slower.");
    Param p16Start = percent("Pad 16 Start", 0)
                         .withKey("pad16.start")
                         .in("Pad 16")
                         .help("Where in the file it starts; played backwards, where it ends.");
    Param p16End = percent("Pad 16 End", 100)
                       .withKey("pad16.end")
                       .in("Pad 16")
                       .help("Where it ends: the region between the two is what plays.");
    Param p16Reverse = toggle("Pad 16 Reverse", false)
                           .withKey("pad16.reverse")
                           .in("Pad 16")
                           .help("Play the region backwards, from its end to its start.");
    Param p16Play = choice("Pad 16 Play", {"One-Shot", "Gated"}, 0)
                        .keys({"one_shot", "gated"})
                        .withKey("pad16.play")
                        .in("Pad 16")
                        .help(
                            "One-Shot plays the region whatever the key does; Gated fades it "
                            "through Release once the key is up.");
    Param p16Attack = milliseconds("Pad 16 Attack", 0, 500, 0)
                          .withKey("pad16.attack")
                          .in("Pad 16")
                          .help("How long it takes to rise at the start.");
    Param p16Decay = milliseconds("Pad 16 Decay", 5, kFull, kFull)
                         .withKey("pad16.decay")
                         .in("Pad 16")
                         .help(
                             "How long it takes to fade to silence; at its top, Full, it does not "
                             "fade and the region plays to its end.");
    Param p16Release = milliseconds("Pad 16 Release", 1, 5000, 100)
                           .withKey("pad16.release")
                           .in("Pad 16")
                           .activeWhen(p16Play, {"gated"})
                           .help("How long a gated pad fades once its key is up.");
    Param p16Filter = choice("Pad 16 Filter", {"Off", "Low Pass", "Band Pass", "High Pass"}, 0)
                          .keys({"off", "lowpass", "bandpass", "highpass"})
                          .withKey("pad16.filter")
                          .in("Pad 16")
                          .help("A state-variable filter on the pad: low, band or high pass.");
    Param p16Cutoff = frequency("Pad 16 Cutoff", 20, 20000, 20000)
                          .withKey("pad16.cutoff")
                          .in("Pad 16")
                          .activeUnless(p16Filter, {"off"})
                          .help(
                              "The filter's frequency; 20 kHz by default, which leaves a low pass "
                              "open, so set it with a band or high pass (choosing one in the "
                              "editor moves an unmoved cutoff to 1 kHz or 20 Hz).");
    Param p16Resonance = percent("Pad 16 Resonance", 0)
                             .withKey("pad16.resonance")
                             .in("Pad 16")
                             .activeUnless(p16Filter, {"off"})
                             .help("The filter's peak at its cutoff.");
    Param p16Velocity = percent("Pad 16 Velocity", 70)
                            .withKey("pad16.velocity")
                            .in("Pad 16")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p16Vary = percent("Pad 16 Vary", 0)
                        .withKey("pad16.vary")
                        .in("Pad 16")
                        .help(
                            "Each hit a little different: at 100 %, up to 25 cents of pitch, 2.5 "
                            "dB of level and 1 ms further into the sound.");
    Param p16Choke = choice("Pad 16 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
                         .keys({"off", "group1", "group2", "group3", "group4", "group5", "group6",
                                "group7", "group8"})
                         .withKey("pad16.choke")
                         .in("Pad 16")
                         .help(
                             "Pads in a group cut each other off, and themselves: the closed hat "
                             "stops the open one.");
    Param p16Output =
        choice("Pad 16 Output",
               {"Main", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"}, 0)
            .keys({"main", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"})
            .withKey("pad16.output")
            .in("Pad 16")
            .help(
                "The main output, or one of the extra ones (Out 1 to Out 8), which takes the pad "
                "out of the main mix.");

    // --- Every pad's ------------------------------------------------------------------------

    Param firstKey =
        integer("First Key", 0, 112, 36)
            .withKey("pads.key")
            .in("Pads")
            .help("The key of pad 1; pad N is N - 1 keys above it (36, C2: GM's map).");
    Param oversampling =
        choice("Oversampling", {"1x", "2x", "4x"}, 1)
            .keys({"x1", "x2", "x4"})
            .withKey("voice.oversampling")
            .in("Pads")
            .restarts()
            .help("Oversampling, which keeps a pad tuned far up free of aliasing.");
    Param output = decibels("Output", -36, 12, 0)
                       .smoothed(20)
                       .in("Pads")
                       .help(
                           "The level of every output. A gentle limit above -1 dBFS keeps hits "
                           "from clipping.");

    // --- What the editor shows, sent by the audio code ---------------------------------------

    Shared<double> outLeft;  // the main output's peak, dB, falling back
    Shared<double> outRight;
    Shared<int> soundingNow;            // voices playing
    SharedArray<int, 16> framesShown;   // each pad's file's length, 0 for none
    SharedArray<float, 8192> overview;  // each pad's peaks over kColumns columns
    SharedArray<float, 4096> pane;      // each pad's close-up of its start
    SharedArray<double, 32> heads;      // each sounding voice's file frame; -1 none
    SharedArray<int, 32> headPad;       // and its pad
    SharedArray<float, 16> glow;        // each pad's loudest voice now, 0 .. 1
    Shared<int> newestPad;              // the pad last hit, -1 none

    // --- State ------------------------------------------------------------------------------

    Array<Voice, kVoices> voice;
    Array<int, kVoices> padOf;     // the pad each voice plays, -1 none
    Array<int, kVoices> busOf;     // where it goes
    Array<bool, kVoices> choked;   // cut off by a pad of its group: fading over kChokeMs
    Array<double, kVoices> nudge;  // its hit's variation: semitones
    Array<double, kVoices> lift;   // and dB
    Array<Pad, kPads> pad;
    Array<int, kPads> hits;    // each pad's hits since prepare(): its variation's seed
    Array<int, kPads> frames;  // each pad's file as measured
    Array<int, kPads> channels;
    Array<double, kPads> mark;       // its fingerprint
    Array<double, kPads> paneStart;  // where its close-up was drawn from, and which way
    Array<int, kPads> paneWay;
    int factor = 2;
    double rate = 96000.0;  // the voices' (oversampled) rate
    int stride = 0;         // a bus's room in the mix
    Buffer<double> mixL;
    Buffer<double> mixR;
    Array<Oversampler, 18> over;  // two a bus
    Array<int, kBuses> quiet;
    Array<double, kBuses> restL;  // what stolen voices were playing, fading out
    Array<double, kBuses> restR;
    double restFade = 0.99;
    double peakL = 0.0;
    double peakR = 0.0;
    int newest = -1;
    int tail = 0;

    // --- Hooks ------------------------------------------------------------------------------

    void prepare() {
        factor = oversampling.index() == 0 ? 1 : oversampling.index() == 1 ? 2 : 4;
        rate = sampleRate() * factor;
        for (int c = 0; c < 18; ++c) {
            over[c].prepare(factor);
            over[c].clear();
        }
        // The voices are made at the oversampled rate and only come down, so the
        // delay is the down filters' alone, not latency()'s way up and down: 1.4
        // samples at 2x and 1.8 at 4x (the suite's Oversampler test measures it).
        setLatency(factor == 1 ? 0 : factor == 2 ? 1 : 2);
        stride = maxBlockSize() * 4;
        mixL.resize(stride * kBuses);
        mixR.resize(stride * kBuses);
        for (int v = 0; v < kVoices; ++v) {
            silence(voice[v]);
            padOf[v] = -1;
            busOf[v] = 0;
            choked[v] = false;
        }
        quiet.fill(4096);
        restL.fill(0.0);
        restR.fill(0.0);
        restFade = exp(-1.0 / (0.003 * rate));
        peakL = 0.0;
        peakR = 0.0;
        newest = -1;
        hits.fill(0);
        tail = 0;
        reportTail(false);
        // The files arrive after this, at the start of a block: measured then.
        frames.fill(0);
        channels.fill(0);
        mark.fill(-1.0);
        paneStart.fill(-1.0);
        readPads();
    }

    void update() {
        setVoiceMode(VoiceMode::Poly);
        readPads();
    }

    // Every pad's settings into pad[].
    void readPads() {
        readPad(0, p1Level, p1Pan, p1Tune, p1Start, p1End, p1Reverse, p1Play, p1Attack, p1Decay,
                p1Release, p1Filter, p1Cutoff, p1Resonance, p1Velocity, p1Vary, p1Choke, p1Output);
        readPad(1, p2Level, p2Pan, p2Tune, p2Start, p2End, p2Reverse, p2Play, p2Attack, p2Decay,
                p2Release, p2Filter, p2Cutoff, p2Resonance, p2Velocity, p2Vary, p2Choke, p2Output);
        readPad(2, p3Level, p3Pan, p3Tune, p3Start, p3End, p3Reverse, p3Play, p3Attack, p3Decay,
                p3Release, p3Filter, p3Cutoff, p3Resonance, p3Velocity, p3Vary, p3Choke, p3Output);
        readPad(3, p4Level, p4Pan, p4Tune, p4Start, p4End, p4Reverse, p4Play, p4Attack, p4Decay,
                p4Release, p4Filter, p4Cutoff, p4Resonance, p4Velocity, p4Vary, p4Choke, p4Output);
        readPad(4, p5Level, p5Pan, p5Tune, p5Start, p5End, p5Reverse, p5Play, p5Attack, p5Decay,
                p5Release, p5Filter, p5Cutoff, p5Resonance, p5Velocity, p5Vary, p5Choke, p5Output);
        readPad(5, p6Level, p6Pan, p6Tune, p6Start, p6End, p6Reverse, p6Play, p6Attack, p6Decay,
                p6Release, p6Filter, p6Cutoff, p6Resonance, p6Velocity, p6Vary, p6Choke, p6Output);
        readPad(6, p7Level, p7Pan, p7Tune, p7Start, p7End, p7Reverse, p7Play, p7Attack, p7Decay,
                p7Release, p7Filter, p7Cutoff, p7Resonance, p7Velocity, p7Vary, p7Choke, p7Output);
        readPad(7, p8Level, p8Pan, p8Tune, p8Start, p8End, p8Reverse, p8Play, p8Attack, p8Decay,
                p8Release, p8Filter, p8Cutoff, p8Resonance, p8Velocity, p8Vary, p8Choke, p8Output);
        readPad(8, p9Level, p9Pan, p9Tune, p9Start, p9End, p9Reverse, p9Play, p9Attack, p9Decay,
                p9Release, p9Filter, p9Cutoff, p9Resonance, p9Velocity, p9Vary, p9Choke, p9Output);
        readPad(9, p10Level, p10Pan, p10Tune, p10Start, p10End, p10Reverse, p10Play, p10Attack,
                p10Decay, p10Release, p10Filter, p10Cutoff, p10Resonance, p10Velocity, p10Vary,
                p10Choke, p10Output);
        readPad(10, p11Level, p11Pan, p11Tune, p11Start, p11End, p11Reverse, p11Play, p11Attack,
                p11Decay, p11Release, p11Filter, p11Cutoff, p11Resonance, p11Velocity, p11Vary,
                p11Choke, p11Output);
        readPad(11, p12Level, p12Pan, p12Tune, p12Start, p12End, p12Reverse, p12Play, p12Attack,
                p12Decay, p12Release, p12Filter, p12Cutoff, p12Resonance, p12Velocity, p12Vary,
                p12Choke, p12Output);
        readPad(12, p13Level, p13Pan, p13Tune, p13Start, p13End, p13Reverse, p13Play, p13Attack,
                p13Decay, p13Release, p13Filter, p13Cutoff, p13Resonance, p13Velocity, p13Vary,
                p13Choke, p13Output);
        readPad(13, p14Level, p14Pan, p14Tune, p14Start, p14End, p14Reverse, p14Play, p14Attack,
                p14Decay, p14Release, p14Filter, p14Cutoff, p14Resonance, p14Velocity, p14Vary,
                p14Choke, p14Output);
        readPad(14, p15Level, p15Pan, p15Tune, p15Start, p15End, p15Reverse, p15Play, p15Attack,
                p15Decay, p15Release, p15Filter, p15Cutoff, p15Resonance, p15Velocity, p15Vary,
                p15Choke, p15Output);
        readPad(15, p16Level, p16Pan, p16Tune, p16Start, p16End, p16Reverse, p16Play, p16Attack,
                p16Decay, p16Release, p16Filter, p16Cutoff, p16Resonance, p16Velocity, p16Vary,
                p16Choke, p16Output);
    }

    void readPad(int p, const Param& level, const Param& pan, const Param& tune, const Param& start,
                 const Param& end, const Param& reverse, const Param& play, const Param& attack,
                 const Param& decay, const Param& release, const Param& filter, const Param& cutoff,
                 const Param& resonance, const Param& velocity, const Param& vary,
                 const Param& choke, const Param& out) {
        Pad& d = pad[p];
        d.level = level.value();
        d.pan = pan.value();
        d.tune = tune.value();
        d.start = start.value();
        d.end = end.value();
        d.reverse = reverse.on();
        d.play = play.index();
        d.attack = attack.value();
        d.decay = decay.value();
        d.release = release.value();
        d.filter = filter.index();
        d.cutoff = cutoff.value();
        d.resonance = resonance.value();
        d.velocity = velocity.value();
        d.vary = vary.value();
        d.choke = choke.index();
        d.output = out.index();
    }

    // --- The files --------------------------------------------------------------------------

    // Enough of a file to tell one from another: its length, channels, and
    // its level over the whole and over its first half.
    double fingerprint(const AudioFile& f) {
        if (!f.loaded()) return 0.0;
        const int n = f.frames();
        double m = n * 3.0 + f.channels();
        for (int c = 0; c < min(f.channels(), 2); ++c)
            m += f.rms(c, 0, n) * 1e3 * (c + 1) + f.rms(c, 0, n / 2) * 7e3;
        return m;
    }

    // Pad p's file, measured and drawn when it is new; its close-up when its
    // start moves.
    void measure(int p, const AudioFile& f) {
        const double m = fingerprint(f);
        if (m != mark[p]) {
            mark[p] = m;
            frames[p] = f.loaded() ? f.frames() : 0;
            channels[p] = f.loaded() ? min(f.channels(), 2) : 0;
            for (int k = 0; k < kColumns; ++k) {
                const int a = frames[p] * k / kColumns;
                const int b = max(a + 1, frames[p] * (k + 1) / kColumns);
                overview[p * kColumns + k] = frames[p] > 0 ? peakOf(f, a, b) : 0.0;
            }
            framesShown[p] = frames[p];
            paneStart[p] = -1.0;
            // What its voices were playing is gone: they fade out.
            for (int v = 0; v < voiceCount(); ++v)
                if (voice[v].sounding && padOf[v] == p) {
                    restL[busOf[v]] += voice[v].lastL;
                    restR[busOf[v]] += voice[v].lastR;
                    voice[v].done = true;
                }
        }
        const int way = pad[p].reverse ? -1 : 1;
        const double at = pad[p].reverse ? max(pad[p].start, pad[p].end) * frames[p]
                                         : min(pad[p].start, pad[p].end) * frames[p];
        if (at == paneStart[p] && way == paneWay[p]) return;
        paneStart[p] = at;
        paneWay[p] = way;
        const double reach = kPaneMs * 0.001 * sampleRate();
        for (int j = 0; j < kPane; ++j) {
            const double x = at + way * (2.0 * j / (kPane - 1) - 1.0) * reach;
            pane[p * kPane + j] = frames[p] > 0 && x >= 0.0 && x < frames[p] ? f.read(0, x) : 0.0;
        }
    }

    // Voice s of pad p rendered (the engine's render()) from its pad's file.
    void renderPad(Voice& s, int p, int from, int run, double endFade, int base) {
        const int type = pad[p].filter > 0 ? kSvf : kOff;
        const int mode = max(pad[p].filter - 1, 0);
        switch (p) {
            case 0:
                render(s, pad1, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 1:
                render(s, pad2, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 2:
                render(s, pad3, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 3:
                render(s, pad4, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 4:
                render(s, pad5, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 5:
                render(s, pad6, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 6:
                render(s, pad7, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 7:
                render(s, pad8, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 8:
                render(s, pad9, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 9:
                render(s, pad10, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 10:
                render(s, pad11, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 11:
                render(s, pad12, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 12:
                render(s, pad13, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 13:
                render(s, pad14, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 14:
                render(s, pad15, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
            case 15:
                render(s, pad16, from, run, factor, endFade, type, false, mode, mixL, mixR, base);
                return;
        }
    }

    // --- Notes ------------------------------------------------------------------------------

    double ms(double t) { return t * 0.001 * sampleRate(); }

    void startVoice(int v) {
        // A note may come before the block's process(), with a new file.
        places();
        Voice& s = voice[v];
        const int p = voiceKey(v) - firstKey.index();
        if (p < 0 || p >= kPads || frames[p] == 0) {
            padOf[v] = -1;
            s.sounding = true;
            s.done = true;
            return;
        }
        const Pad& d = pad[p];
        // Its group's voices, this pad's own too, fade out.
        if (d.choke > 0)
            for (int u = 0; u < voiceCount(); ++u)
                if (u != v && voice[u].sounding && padOf[u] >= 0 &&
                    pad[padOf[u]].choke == d.choke) {
                    choked[u] = true;
                    voice[u].amp.release();
                }
        newest = v;
        padOf[v] = p;
        busOf[v] = d.output;
        choked[v] = false;
        s.velocity = voiceVelocity(v);
        if (!s.sounding) {
            for (int c = 0; c < 2; ++c) s.filter[c].clear();
            s.amp.kill();
            s.ampL = 0.0;
            s.ampR = 0.0;
            s.ampLStep = 0.0;
            s.ampRStep = 0.0;
        }
        s.ending = false;
        s.fresh = true;
        s.tickLeft = 0;
        s.amp.gate();
        // This hit's variation: three values from its number.
        const int n = hits[p];
        hits[p] = n + 1;
        nudge[v] = d.vary * 0.25 * scatter(n * 3, p + 1);
        lift[v] = d.vary * 2.5 * scatter(n * 3 + 1, p + 1);
        const int later =
            toInt(d.vary * 0.5 * (scatter(n * 3 + 2, p + 1) + 1.0) * 0.001 * sampleRate());
        // The region, and the hit's start within it.
        const int a = toInt(min(d.start, d.end) * frames[p]);
        const int b = toInt(max(d.start, d.end) * frames[p]);
        s.span.a = clampInt(a, 0, max(frames[p] - 1, 0));
        s.span.b = clampInt(max(b, s.span.a + 1), 0, frames[p]);
        s.span.reversed = d.reverse;
        s.span.loop = kLoopOff;
        if (d.reverse)
            s.span.b = max(s.span.b - later, s.span.a + 1);
        else
            s.span.a = min(s.span.a + later, s.span.b - 1);
        s.q = 0.0;
        s.dir = 1.0;
        s.looping = false;
        s.gated = d.play == kGated;
        s.narrow = channels[p] < 2;
        s.slice = -1;
        s.sounding = true;
        s.done = false;
    }

    void releaseVoice(int v) {
        if (voice[v].gated) voice[v].amp.release();
    }

    // Stolen: it stops now, and what it was playing fades out over a few
    // milliseconds instead of stepping to silence.
    void stopVoice(int v) {
        Voice& s = voice[v];
        if (s.sounding) {
            restL[busOf[v]] += s.lastL;
            restR[busOf[v]] += s.lastR;
        }
        silence(s);
        padOf[v] = -1;
    }

    void silence(Voice& s) {
        s.sounding = false;
        s.done = false;
        s.amp.kill();
        s.mod.kill();
        s.ampL = 0.0;
        s.ampR = 0.0;
        s.ampLStep = 0.0;
        s.ampRStep = 0.0;
        s.lastL = 0.0;
        s.lastR = 0.0;
        s.tickLeft = 0;
        s.span.a = 0;
        s.span.b = 0;
        for (int c = 0; c < 2; ++c) s.filter[c].clear();
    }

    // --- Control: every kTick frames of a voice -------------------------------------------

    void control(int v, int i) {
        Voice& s = voice[v];
        const Pad& d = pad[padOf[v]];
        const double steps = kTick * factor;
        const bool snapNow = s.fresh;
        s.fresh = false;
        const bool full = d.decay >= kFull;
        const double release = choked[v] ? kChokeMs : d.release;
        const double ampEnv = s.amp.advance(kTick, ms(d.attack), full ? 0.0 : ms(d.decay),
                                            full ? 1.0 : 0.0, ms(release));
        if (s.ending && s.amp.stage == 0 && !snapNow) {
            s.done = true;
            return;
        }
        // The speed: the pad's tune and the hit's nudge, with the bend.
        const double semis = d.tune + nudge[v] + voicePitch(v, i) - voiceKey(v);
        const double target = clamp(exp2(semis / 12.0), 1.0 / 256.0, 32.0) / factor;
        if (snapNow) {
            s.step = target;
            s.stepStep = 0.0;
        } else {
            s.stepStep = (target - s.step) / steps;
        }
        if (d.filter > 0) {
            const double hz = clamp(d.cutoff, 10.0, min(22000.0, 0.45 * rate));
            const double g = tan(pi * hz / rate);
            const double r = clamp(d.resonance, 0.0, 1.0);
            const double k = max(0.005, 2.0 * (1.0 - r) * (1.0 - r));
            if (snapNow) {
                s.g = g;
                s.gStep = 0.0;
                s.k = k;
                s.kStep = 0.0;
            } else {
                s.gStep = (g - s.g) / steps;
                s.kStep = (k - s.k) / steps;
            }
            s.drive = 1.0;
        }
        // Amp: the envelope, velocity, the pad's level and the hit's lift,
        // the note's own volume and pan.
        const double velDb = -(1.0 - s.velocity) * 40.0 * d.velocity;
        const double level = ampEnv * dbToGain(velDb + d.level + lift[v]) * voiceVolume(v);
        const double p = clamp(d.pan + (voicePan(v) - 0.5) * 2.0, -1.0, 1.0);
        if (s.ampPanAt != p) {
            const double a = (p + 1.0) * 0.25 * pi;
            s.ampCos = cos(a);
            s.ampSin = sin(a);
            s.ampPanAt = p;
        }
        const double l = level * s.ampCos * sqrt(2.0);
        const double rr = level * s.ampSin * sqrt(2.0);
        if (snapNow) {
            s.ampL = 0.0;
            s.ampR = 0.0;
        }
        s.ampLStep = (l - s.ampL) / steps;
        s.ampRStep = (rr - s.ampR) / steps;
        s.ending = level == 0.0;
    }

    // --- Playing ------------------------------------------------------------------------------

    void play(int v, int count) {
        Voice& s = voice[v];
        const int p = padOf[v];
        const double endFade = 0.001 * sampleRate();  // a region's last millisecond fades
        int i = 0;
        while (i < count) {
            if (s.tickLeft == 0) {
                control(v, i);
                s.tickLeft = kTick;
                if (s.done) return;
            }
            const int run = min(s.tickLeft, count - i);
            renderPad(s, p, i, run, endFade, busOf[v] * stride);
            s.tickLeft -= run;
            i += run;
            if (s.done) return;
        }
    }

    // Every pad's file, at a block's start.
    void places() {
        measure(0, pad1);
        measure(1, pad2);
        measure(2, pad3);
        measure(3, pad4);
        measure(4, pad5);
        measure(5, pad6);
        measure(6, pad7);
        measure(7, pad8);
        measure(8, pad9);
        measure(9, pad10);
        measure(10, pad11);
        measure(11, pad12);
        measure(12, pad13);
        measure(13, pad14);
        measure(14, pad15);
        measure(15, pad16);
    }

    void process(Block& io) {
        places();
        const int n = io.frames * factor;
        for (int b = 0; b < kBuses; ++b)
            for (int k = 0; k < n; ++k) {
                mixL[b * stride + k] = 0.0;
                mixR[b * stride + k] = 0.0;
            }
        for (int v = 0; v < voiceCount(); ++v) {
            Voice& s = voice[v];
            if (!s.sounding || !voicePlaying(v)) continue;
            if (!s.done) play(v, io.frames);
            if (s.done) {
                silence(s);
                padOf[v] = -1;
                voiceFinished(v);
            }
        }
        for (int b = 0; b < kBuses; ++b) bring(io, b, n);
        bool sounding = false;
        for (int v = 0; v < voiceCount(); ++v)
            if (voice[v].sounding && voicePlaying(v)) sounding = true;
        reportTail(sounding);
        show();
    }

    // Bus b's mix (and what stolen voices left it) brought down to the
    // session's rate, through the output level and the limit, to its output.
    void bring(Block& io, int b, int n) {
        const int base = b * stride;
        for (int k = 0; k < n; ++k) {
            if (restL[b] == 0.0 && restR[b] == 0.0) break;
            mixL[base + k] += restL[b];
            mixR[base + k] += restR[b];
            restL[b] *= restFade;
            restR[b] *= restFade;
            if (abs(restL[b]) + abs(restR[b]) < 1e-9) {
                restL[b] = 0.0;
                restR[b] = 0.0;
            }
        }
        const Channel left = b == 0 ? io[0] : io.output(b - 1, 0);
        const Channel right = b == 0 ? io[io.channels - 1] : io.output(b - 1, io.channels - 1);
        for (int i = 0; i < io.frames; ++i) {
            // Long silent: nothing to bring down, and the downsampler has
            // long since rung out.
            bool any = false;
            for (int k = 0; k < factor; ++k)
                if (mixL[base + i * factor + k] != 0.0 || mixR[base + i * factor + k] != 0.0)
                    any = true;
            if (any)
                quiet[b] = 0;
            else if (quiet[b] < 4096)
                ++quiet[b];
            if (quiet[b] == 4095) {
                over[2 * b].clear();
                over[2 * b + 1].clear();
            }
            if (quiet[b] >= 4096) {
                if (b == 0) {
                    peakL *= 0.99985;
                    peakR *= 0.99985;
                }
                continue;
            }
            over[2 * b].up(0.0);
            over[2 * b + 1].up(0.0);
            for (int k = 0; k < factor; ++k) {
                over[2 * b].set(k, mixL[base + i * factor + k]);
                over[2 * b + 1].set(k, mixR[base + i * factor + k]);
            }
            const double l = over[2 * b].down();
            const double r = over[2 * b + 1].down();
            const double g = kHeadroom * dbToGain(output.at(i));
            if (io.channels == 1) {
                left[i] = safety((l + r) * 0.5 * g);
            } else {
                left[i] = safety(l * g);
                right[i] = safety(r * g);
            }
            if (b == 0) {
                peakL = max(abs(left[i]), peakL * 0.99985);
                peakR = max(abs(right[i]), peakR * 0.99985);
            }
        }
    }

    // How long the output goes on once the keys are up: the longest gated
    // release, or what is left of a hit that plays on; and 20 ms for a
    // stolen voice's fade and the downsampler. Raised whenever a voice needs
    // more, lowered only with no voice sounding.
    void reportTail(bool sounding) {
        double longest = kChokeMs;
        for (int v = 0; v < voiceCount(); ++v) {
            const Voice& s = voice[v];
            if (!s.sounding || !voicePlaying(v) || padOf[v] < 0) continue;
            longest = max(longest, pad[padOf[v]].release);
            if (s.step > 0.0)
                longest = max(longest,
                              (s.span.length() - s.q) / (s.step * factor) / sampleRate() * 1000.0);
        }
        const int want = ceilToInt((min(longest, 600000.0) + 20.0) * 0.001 * sampleRate());
        if (want > tail || (!sounding && want != tail)) {
            tail = want;
            setTail(tail);
        }
    }

    // What the editor shows: the level, the voices, where each plays and how
    // loud each pad is now.
    void show() {
        outLeft.set(gainToDb(peakL));
        outRight.set(gainToDb(peakR));
        for (int p = 0; p < kPads; ++p) glow[p] = 0.0;
        int count = 0;
        for (int v = 0; v < kVoices; ++v) {
            const Voice& s = voice[v];
            const bool on =
                v < voiceCount() && s.sounding && voicePlaying(v) && !s.done && padOf[v] >= 0;
            if (on) {
                ++count;
                glow[padOf[v]] = max(glow[padOf[v]], min(1.0, abs(s.ampL) + abs(s.ampR)));
            }
            heads[v] = on ? s.span.frame(s.q) : -1.0;
            headPad[v] = on ? padOf[v] : -1;
        }
        soundingNow.set(count);
        newestPad.set(newest >= 0 && padOf[newest] >= 0 ? padOf[newest] : -1);
    }

    // Untouched below -1 dBFS, then bent smoothly toward full scale.
    double safety(double x) {
        const double t = 0.891250938;
        const double a = abs(x);
        if (a <= t) return x;
        const double y = t + (1.0 - t) * fastTanh((a - t) / (1.0 - t));
        return x < 0.0 ? -y : y;
    }

    // --- The editor ---------------------------------------------------------------------------
    //
    // A header with the level; the sixteen pads, four by four with pad 1 at
    // the bottom left as on hardware, each with its file's slot (click to
    // choose, drop a file on it, or its menu's factory hits), its waveform,
    // its tags and a light while it sounds; pressing a pad plays it, harder
    // the higher up, and selects it. Beside them the selected pad: its
    // waveform with the region's flags and where each voice plays, a
    // close-up of where it starts, and its settings. Every dial drags up and
    // down (Shift finely), turns with the wheel and goes back to its default
    // on a double click; a button sets its choice; the flags drag.

    static constexpr double kW = 1240;
    static constexpr double kH = 776;
    static constexpr double padsX = 16;  // the panels
    static constexpr double padsY = 76;
    static constexpr double padsW = 560;
    static constexpr double padsH = 610;
    static constexpr double selX = 588;
    static constexpr double selY = 76;
    static constexpr double selW = 636;
    static constexpr double selH = 610;
    static constexpr double barY = 698;  // every pad's
    static constexpr double barH = 62;
    static constexpr double gridX = padsX + 18;  // the pads
    static constexpr double gridY = padsY + 46;
    static constexpr double cellW = 126;
    static constexpr double cellH = 134;
    static constexpr double cellGap = 6;
    static constexpr double slotH = 24;        // a pad's file, at its foot
    static constexpr double wavX = selX + 16;  // the selected pad's waveform
    static constexpr double wavY = selY + 82;
    static constexpr double wavW = selW - 32;
    static constexpr double wavH = 150;
    static constexpr double flagH = 16;
    static constexpr double paneX = selX + selW - 16 - 196;  // its close-up
    static constexpr double paneY = selY + 374;
    static constexpr double paneW = 196;
    static constexpr double paneH = 84;

    // The palette: the theme's, and the wood's.
    static constexpr int kInk = 0x1A1411;
    static constexpr int kGroundBottom = 0x201914;
    static constexpr int kGraphTop = 0x1D1713;
    static constexpr int kGraphBottom = 0x130F0C;
    static constexpr int kKnobLight = 0x54463B;
    static constexpr int kKnobDark = 0x2A221C;
    static constexpr int kKnobHot = 0x5E4E41;
    static constexpr int kLinen = 0xE3D5BE;
    static constexpr int kMeterLow = 0x6E7A4B;
    static constexpr int kMeterHigh = 0xD8CE9C;
    static constexpr int kWillow = 0xC9A46A;  // the sound: pollarded willow
    static constexpr int kShoot = 0xA9BC72;   // the pads' crowns of new stems
    static constexpr int kBark = 0x8C7553;
    static constexpr int kSand = 0xDDBA80;      // the filter
    static constexpr int kSeaGlass = 0x8FB3AC;  // choke and outputs

    // The editor's codes: a pad's fields 0-16 (in the order Pad declares
    // them), for the selected pad; every pad's 100-102.
    static constexpr int cLevel = 0;
    static constexpr int cPan = 1;
    static constexpr int cTune = 2;
    static constexpr int cStart = 3;
    static constexpr int cEnd = 4;
    static constexpr int cReverse = 5;
    static constexpr int cPlay = 6;
    static constexpr int cAttack = 7;
    static constexpr int cDecay = 8;
    static constexpr int cRelease = 9;
    static constexpr int cFilter = 10;
    static constexpr int cCutoff = 11;
    static constexpr int cResonance = 12;
    static constexpr int cVelocity = 13;
    static constexpr int cVary = 14;
    static constexpr int cChoke = 15;
    static constexpr int cOutput = 16;
    static constexpr int cFirstKey = 100;
    static constexpr int cOversampling = 101;
    static constexpr int cOut = 102;
    static constexpr int bChoice = 0;  // lit while its parameter holds its value
    static constexpr int bToggle = 1;  // lit while its parameter is on

    int selected = 0;
    double hoverX = -1.0, hoverY = -1.0;
    int dragging = 0;  // 1 a dial, 2 a flag, 3 a pad played
    int dragDial = -1;
    double dialPos = 0.0;
    int handle = -1;  // the flag dragged: cStart or cEnd
    int playedKey = -1;
    double pressX = 0.0, pressA = 0.0;
    int hotDial = -1;
    int hotButton = -1;
    int hotHandle = -1;
    int hotPad = -1;
    Array<int, 64> dialCode;
    Array<double, 64> dialX, dialY, dialR;
    Array<int, 64> dialColour;
    Array<bool, 64> dialLive;  // false: dimmed, as its setting does not apply
    int dials = 0;
    Array<int, 64> buttonCode, buttonValue, buttonStyle, buttonColour;
    Array<double, 64> buttonX, buttonY, buttonW, buttonH;
    Array<bool, 64> buttonLive;
    int buttons = 0;

    void editor(Editor& e) {
        e.size(kW, kH);
        e.file(pad1, cellX(0) + 2, cellY(0) + cellH - slotH, cellW - 4, slotH);
        e.file(pad2, cellX(1) + 2, cellY(1) + cellH - slotH, cellW - 4, slotH);
        e.file(pad3, cellX(2) + 2, cellY(2) + cellH - slotH, cellW - 4, slotH);
        e.file(pad4, cellX(3) + 2, cellY(3) + cellH - slotH, cellW - 4, slotH);
        e.file(pad5, cellX(4) + 2, cellY(4) + cellH - slotH, cellW - 4, slotH);
        e.file(pad6, cellX(5) + 2, cellY(5) + cellH - slotH, cellW - 4, slotH);
        e.file(pad7, cellX(6) + 2, cellY(6) + cellH - slotH, cellW - 4, slotH);
        e.file(pad8, cellX(7) + 2, cellY(7) + cellH - slotH, cellW - 4, slotH);
        e.file(pad9, cellX(8) + 2, cellY(8) + cellH - slotH, cellW - 4, slotH);
        e.file(pad10, cellX(9) + 2, cellY(9) + cellH - slotH, cellW - 4, slotH);
        e.file(pad11, cellX(10) + 2, cellY(10) + cellH - slotH, cellW - 4, slotH);
        e.file(pad12, cellX(11) + 2, cellY(11) + cellH - slotH, cellW - 4, slotH);
        e.file(pad13, cellX(12) + 2, cellY(12) + cellH - slotH, cellW - 4, slotH);
        e.file(pad14, cellX(13) + 2, cellY(13) + cellH - slotH, cellW - 4, slotH);
        e.file(pad15, cellX(14) + 2, cellY(14) + cellH - slotH, cellW - 4, slotH);
        e.file(pad16, cellX(15) + 2, cellY(15) + cellH - slotH, cellW - 4, slotH);
    }

    // --- parameters by code -------------------------------------------------------------------

    double readOn(const Param& p, int how) const {
        if (how == 0) return p.normalized();
        if (how == 1) return p.value();
        return p.index();
    }
    void editOn(const Param& p, int how, double v) {
        if (how == 0)
            p.edit(v);
        else if (how == 1)
            p.editNormalized(v);
        else
            p.reset();
    }

    // A pad's field `what`, given its parameters: read (how 0 its dial's
    // position, 1 its value, 2 its choice), edited (how 0 to a value, 1 to a
    // position, 2 to its default) or written out.
    double padField(int what, int how, const Param& level, const Param& pan, const Param& tune,
                    const Param& start, const Param& end, const Param& reverse, const Param& play,
                    const Param& attack, const Param& decay, const Param& release,
                    const Param& filter, const Param& cutoff, const Param& resonance,
                    const Param& velocity, const Param& vary, const Param& choke,
                    const Param& out) const {
        if (what == cLevel) return readOn(level, how);
        if (what == cPan) return readOn(pan, how);
        if (what == cTune) return readOn(tune, how);
        if (what == cStart) return readOn(start, how);
        if (what == cEnd) return readOn(end, how);
        if (what == cReverse) return readOn(reverse, how);
        if (what == cPlay) return readOn(play, how);
        if (what == cAttack) return readOn(attack, how);
        if (what == cDecay) return readOn(decay, how);
        if (what == cRelease) return readOn(release, how);
        if (what == cFilter) return readOn(filter, how);
        if (what == cCutoff) return readOn(cutoff, how);
        if (what == cResonance) return readOn(resonance, how);
        if (what == cVelocity) return readOn(velocity, how);
        if (what == cVary) return readOn(vary, how);
        if (what == cChoke) return readOn(choke, how);
        return readOn(out, how);
    }
    void padEdit(int what, int how, double v, const Param& level, const Param& pan,
                 const Param& tune, const Param& start, const Param& end, const Param& reverse,
                 const Param& play, const Param& attack, const Param& decay, const Param& release,
                 const Param& filter, const Param& cutoff, const Param& resonance,
                 const Param& velocity, const Param& vary, const Param& choke, const Param& out) {
        if (what == cLevel) editOn(level, how, v);
        if (what == cPan) editOn(pan, how, v);
        if (what == cTune) editOn(tune, how, v);
        if (what == cStart) editOn(start, how, v);
        if (what == cEnd) editOn(end, how, v);
        if (what == cReverse) editOn(reverse, how, v);
        if (what == cPlay) editOn(play, how, v);
        if (what == cAttack) editOn(attack, how, v);
        if (what == cDecay) editOn(decay, how, v);
        if (what == cRelease) editOn(release, how, v);
        if (what == cFilter) editOn(filter, how, v);
        if (what == cCutoff) editOn(cutoff, how, v);
        if (what == cResonance) editOn(resonance, how, v);
        if (what == cVelocity) editOn(velocity, how, v);
        if (what == cVary) editOn(vary, how, v);
        if (what == cChoke) editOn(choke, how, v);
        if (what == cOutput) editOn(out, how, v);
    }
    void padShow(Canvas& g, int what, double x, double y, const Param& level, const Param& pan,
                 const Param& tune, const Param& start, const Param& end, const Param& reverse,
                 const Param& play, const Param& attack, const Param& decay, const Param& release,
                 const Param& filter, const Param& cutoff, const Param& resonance,
                 const Param& velocity, const Param& vary, const Param& choke, const Param& out) {
        if (what == cLevel) g.fillText(level.text(), x, y);
        if (what == cPan) g.fillText(pan.text(), x, y);
        if (what == cTune) g.fillText(tune.text(), x, y);
        if (what == cStart) g.fillText(start.text(), x, y);
        if (what == cEnd) g.fillText(end.text(), x, y);
        if (what == cReverse) g.fillText(reverse.text(), x, y);
        if (what == cPlay) g.fillText(play.text(), x, y);
        if (what == cAttack) g.fillText(attack.text(), x, y);
        if (what == cDecay) g.fillText(decay.value() >= kFull ? "FULL" : decay.text(), x, y);
        if (what == cRelease) g.fillText(release.text(), x, y);
        if (what == cFilter) g.fillText(filter.text(), x, y);
        if (what == cCutoff) g.fillText(cutoff.text(), x, y);
        if (what == cResonance) g.fillText(resonance.text(), x, y);
        if (what == cVelocity) g.fillText(velocity.text(), x, y);
        if (what == cVary) g.fillText(vary.text(), x, y);
        if (what == cChoke) g.fillText(choke.text(), x, y);
        if (what == cOutput) g.fillText(out.text(), x, y);
    }

    // Pad p's field, its parameters found by the pad.
    double padRead(int p, int what, int how) const {
        switch (p) {
            case 0:
                return padField(what, how, p1Level, p1Pan, p1Tune, p1Start, p1End, p1Reverse,
                                p1Play, p1Attack, p1Decay, p1Release, p1Filter, p1Cutoff,
                                p1Resonance, p1Velocity, p1Vary, p1Choke, p1Output);
            case 1:
                return padField(what, how, p2Level, p2Pan, p2Tune, p2Start, p2End, p2Reverse,
                                p2Play, p2Attack, p2Decay, p2Release, p2Filter, p2Cutoff,
                                p2Resonance, p2Velocity, p2Vary, p2Choke, p2Output);
            case 2:
                return padField(what, how, p3Level, p3Pan, p3Tune, p3Start, p3End, p3Reverse,
                                p3Play, p3Attack, p3Decay, p3Release, p3Filter, p3Cutoff,
                                p3Resonance, p3Velocity, p3Vary, p3Choke, p3Output);
            case 3:
                return padField(what, how, p4Level, p4Pan, p4Tune, p4Start, p4End, p4Reverse,
                                p4Play, p4Attack, p4Decay, p4Release, p4Filter, p4Cutoff,
                                p4Resonance, p4Velocity, p4Vary, p4Choke, p4Output);
            case 4:
                return padField(what, how, p5Level, p5Pan, p5Tune, p5Start, p5End, p5Reverse,
                                p5Play, p5Attack, p5Decay, p5Release, p5Filter, p5Cutoff,
                                p5Resonance, p5Velocity, p5Vary, p5Choke, p5Output);
            case 5:
                return padField(what, how, p6Level, p6Pan, p6Tune, p6Start, p6End, p6Reverse,
                                p6Play, p6Attack, p6Decay, p6Release, p6Filter, p6Cutoff,
                                p6Resonance, p6Velocity, p6Vary, p6Choke, p6Output);
            case 6:
                return padField(what, how, p7Level, p7Pan, p7Tune, p7Start, p7End, p7Reverse,
                                p7Play, p7Attack, p7Decay, p7Release, p7Filter, p7Cutoff,
                                p7Resonance, p7Velocity, p7Vary, p7Choke, p7Output);
            case 7:
                return padField(what, how, p8Level, p8Pan, p8Tune, p8Start, p8End, p8Reverse,
                                p8Play, p8Attack, p8Decay, p8Release, p8Filter, p8Cutoff,
                                p8Resonance, p8Velocity, p8Vary, p8Choke, p8Output);
            case 8:
                return padField(what, how, p9Level, p9Pan, p9Tune, p9Start, p9End, p9Reverse,
                                p9Play, p9Attack, p9Decay, p9Release, p9Filter, p9Cutoff,
                                p9Resonance, p9Velocity, p9Vary, p9Choke, p9Output);
            case 9:
                return padField(what, how, p10Level, p10Pan, p10Tune, p10Start, p10End, p10Reverse,
                                p10Play, p10Attack, p10Decay, p10Release, p10Filter, p10Cutoff,
                                p10Resonance, p10Velocity, p10Vary, p10Choke, p10Output);
            case 10:
                return padField(what, how, p11Level, p11Pan, p11Tune, p11Start, p11End, p11Reverse,
                                p11Play, p11Attack, p11Decay, p11Release, p11Filter, p11Cutoff,
                                p11Resonance, p11Velocity, p11Vary, p11Choke, p11Output);
            case 11:
                return padField(what, how, p12Level, p12Pan, p12Tune, p12Start, p12End, p12Reverse,
                                p12Play, p12Attack, p12Decay, p12Release, p12Filter, p12Cutoff,
                                p12Resonance, p12Velocity, p12Vary, p12Choke, p12Output);
            case 12:
                return padField(what, how, p13Level, p13Pan, p13Tune, p13Start, p13End, p13Reverse,
                                p13Play, p13Attack, p13Decay, p13Release, p13Filter, p13Cutoff,
                                p13Resonance, p13Velocity, p13Vary, p13Choke, p13Output);
            case 13:
                return padField(what, how, p14Level, p14Pan, p14Tune, p14Start, p14End, p14Reverse,
                                p14Play, p14Attack, p14Decay, p14Release, p14Filter, p14Cutoff,
                                p14Resonance, p14Velocity, p14Vary, p14Choke, p14Output);
            case 14:
                return padField(what, how, p15Level, p15Pan, p15Tune, p15Start, p15End, p15Reverse,
                                p15Play, p15Attack, p15Decay, p15Release, p15Filter, p15Cutoff,
                                p15Resonance, p15Velocity, p15Vary, p15Choke, p15Output);
            case 15:
                return padField(what, how, p16Level, p16Pan, p16Tune, p16Start, p16End, p16Reverse,
                                p16Play, p16Attack, p16Decay, p16Release, p16Filter, p16Cutoff,
                                p16Resonance, p16Velocity, p16Vary, p16Choke, p16Output);
        }
        return 0.0;
    }
    void padWrite(int p, int what, int how, double v) {
        switch (p) {
            case 0:
                padEdit(what, how, v, p1Level, p1Pan, p1Tune, p1Start, p1End, p1Reverse, p1Play,
                        p1Attack, p1Decay, p1Release, p1Filter, p1Cutoff, p1Resonance, p1Velocity,
                        p1Vary, p1Choke, p1Output);
                return;
            case 1:
                padEdit(what, how, v, p2Level, p2Pan, p2Tune, p2Start, p2End, p2Reverse, p2Play,
                        p2Attack, p2Decay, p2Release, p2Filter, p2Cutoff, p2Resonance, p2Velocity,
                        p2Vary, p2Choke, p2Output);
                return;
            case 2:
                padEdit(what, how, v, p3Level, p3Pan, p3Tune, p3Start, p3End, p3Reverse, p3Play,
                        p3Attack, p3Decay, p3Release, p3Filter, p3Cutoff, p3Resonance, p3Velocity,
                        p3Vary, p3Choke, p3Output);
                return;
            case 3:
                padEdit(what, how, v, p4Level, p4Pan, p4Tune, p4Start, p4End, p4Reverse, p4Play,
                        p4Attack, p4Decay, p4Release, p4Filter, p4Cutoff, p4Resonance, p4Velocity,
                        p4Vary, p4Choke, p4Output);
                return;
            case 4:
                padEdit(what, how, v, p5Level, p5Pan, p5Tune, p5Start, p5End, p5Reverse, p5Play,
                        p5Attack, p5Decay, p5Release, p5Filter, p5Cutoff, p5Resonance, p5Velocity,
                        p5Vary, p5Choke, p5Output);
                return;
            case 5:
                padEdit(what, how, v, p6Level, p6Pan, p6Tune, p6Start, p6End, p6Reverse, p6Play,
                        p6Attack, p6Decay, p6Release, p6Filter, p6Cutoff, p6Resonance, p6Velocity,
                        p6Vary, p6Choke, p6Output);
                return;
            case 6:
                padEdit(what, how, v, p7Level, p7Pan, p7Tune, p7Start, p7End, p7Reverse, p7Play,
                        p7Attack, p7Decay, p7Release, p7Filter, p7Cutoff, p7Resonance, p7Velocity,
                        p7Vary, p7Choke, p7Output);
                return;
            case 7:
                padEdit(what, how, v, p8Level, p8Pan, p8Tune, p8Start, p8End, p8Reverse, p8Play,
                        p8Attack, p8Decay, p8Release, p8Filter, p8Cutoff, p8Resonance, p8Velocity,
                        p8Vary, p8Choke, p8Output);
                return;
            case 8:
                padEdit(what, how, v, p9Level, p9Pan, p9Tune, p9Start, p9End, p9Reverse, p9Play,
                        p9Attack, p9Decay, p9Release, p9Filter, p9Cutoff, p9Resonance, p9Velocity,
                        p9Vary, p9Choke, p9Output);
                return;
            case 9:
                padEdit(what, how, v, p10Level, p10Pan, p10Tune, p10Start, p10End, p10Reverse,
                        p10Play, p10Attack, p10Decay, p10Release, p10Filter, p10Cutoff,
                        p10Resonance, p10Velocity, p10Vary, p10Choke, p10Output);
                return;
            case 10:
                padEdit(what, how, v, p11Level, p11Pan, p11Tune, p11Start, p11End, p11Reverse,
                        p11Play, p11Attack, p11Decay, p11Release, p11Filter, p11Cutoff,
                        p11Resonance, p11Velocity, p11Vary, p11Choke, p11Output);
                return;
            case 11:
                padEdit(what, how, v, p12Level, p12Pan, p12Tune, p12Start, p12End, p12Reverse,
                        p12Play, p12Attack, p12Decay, p12Release, p12Filter, p12Cutoff,
                        p12Resonance, p12Velocity, p12Vary, p12Choke, p12Output);
                return;
            case 12:
                padEdit(what, how, v, p13Level, p13Pan, p13Tune, p13Start, p13End, p13Reverse,
                        p13Play, p13Attack, p13Decay, p13Release, p13Filter, p13Cutoff,
                        p13Resonance, p13Velocity, p13Vary, p13Choke, p13Output);
                return;
            case 13:
                padEdit(what, how, v, p14Level, p14Pan, p14Tune, p14Start, p14End, p14Reverse,
                        p14Play, p14Attack, p14Decay, p14Release, p14Filter, p14Cutoff,
                        p14Resonance, p14Velocity, p14Vary, p14Choke, p14Output);
                return;
            case 14:
                padEdit(what, how, v, p15Level, p15Pan, p15Tune, p15Start, p15End, p15Reverse,
                        p15Play, p15Attack, p15Decay, p15Release, p15Filter, p15Cutoff,
                        p15Resonance, p15Velocity, p15Vary, p15Choke, p15Output);
                return;
            case 15:
                padEdit(what, how, v, p16Level, p16Pan, p16Tune, p16Start, p16End, p16Reverse,
                        p16Play, p16Attack, p16Decay, p16Release, p16Filter, p16Cutoff,
                        p16Resonance, p16Velocity, p16Vary, p16Choke, p16Output);
                return;
        }
    }
    void padText(Canvas& g, int p, int what, double x, double y) {
        switch (p) {
            case 0:
                padShow(g, what, x, y, p1Level, p1Pan, p1Tune, p1Start, p1End, p1Reverse, p1Play,
                        p1Attack, p1Decay, p1Release, p1Filter, p1Cutoff, p1Resonance, p1Velocity,
                        p1Vary, p1Choke, p1Output);
                return;
            case 1:
                padShow(g, what, x, y, p2Level, p2Pan, p2Tune, p2Start, p2End, p2Reverse, p2Play,
                        p2Attack, p2Decay, p2Release, p2Filter, p2Cutoff, p2Resonance, p2Velocity,
                        p2Vary, p2Choke, p2Output);
                return;
            case 2:
                padShow(g, what, x, y, p3Level, p3Pan, p3Tune, p3Start, p3End, p3Reverse, p3Play,
                        p3Attack, p3Decay, p3Release, p3Filter, p3Cutoff, p3Resonance, p3Velocity,
                        p3Vary, p3Choke, p3Output);
                return;
            case 3:
                padShow(g, what, x, y, p4Level, p4Pan, p4Tune, p4Start, p4End, p4Reverse, p4Play,
                        p4Attack, p4Decay, p4Release, p4Filter, p4Cutoff, p4Resonance, p4Velocity,
                        p4Vary, p4Choke, p4Output);
                return;
            case 4:
                padShow(g, what, x, y, p5Level, p5Pan, p5Tune, p5Start, p5End, p5Reverse, p5Play,
                        p5Attack, p5Decay, p5Release, p5Filter, p5Cutoff, p5Resonance, p5Velocity,
                        p5Vary, p5Choke, p5Output);
                return;
            case 5:
                padShow(g, what, x, y, p6Level, p6Pan, p6Tune, p6Start, p6End, p6Reverse, p6Play,
                        p6Attack, p6Decay, p6Release, p6Filter, p6Cutoff, p6Resonance, p6Velocity,
                        p6Vary, p6Choke, p6Output);
                return;
            case 6:
                padShow(g, what, x, y, p7Level, p7Pan, p7Tune, p7Start, p7End, p7Reverse, p7Play,
                        p7Attack, p7Decay, p7Release, p7Filter, p7Cutoff, p7Resonance, p7Velocity,
                        p7Vary, p7Choke, p7Output);
                return;
            case 7:
                padShow(g, what, x, y, p8Level, p8Pan, p8Tune, p8Start, p8End, p8Reverse, p8Play,
                        p8Attack, p8Decay, p8Release, p8Filter, p8Cutoff, p8Resonance, p8Velocity,
                        p8Vary, p8Choke, p8Output);
                return;
            case 8:
                padShow(g, what, x, y, p9Level, p9Pan, p9Tune, p9Start, p9End, p9Reverse, p9Play,
                        p9Attack, p9Decay, p9Release, p9Filter, p9Cutoff, p9Resonance, p9Velocity,
                        p9Vary, p9Choke, p9Output);
                return;
            case 9:
                padShow(g, what, x, y, p10Level, p10Pan, p10Tune, p10Start, p10End, p10Reverse,
                        p10Play, p10Attack, p10Decay, p10Release, p10Filter, p10Cutoff,
                        p10Resonance, p10Velocity, p10Vary, p10Choke, p10Output);
                return;
            case 10:
                padShow(g, what, x, y, p11Level, p11Pan, p11Tune, p11Start, p11End, p11Reverse,
                        p11Play, p11Attack, p11Decay, p11Release, p11Filter, p11Cutoff,
                        p11Resonance, p11Velocity, p11Vary, p11Choke, p11Output);
                return;
            case 11:
                padShow(g, what, x, y, p12Level, p12Pan, p12Tune, p12Start, p12End, p12Reverse,
                        p12Play, p12Attack, p12Decay, p12Release, p12Filter, p12Cutoff,
                        p12Resonance, p12Velocity, p12Vary, p12Choke, p12Output);
                return;
            case 12:
                padShow(g, what, x, y, p13Level, p13Pan, p13Tune, p13Start, p13End, p13Reverse,
                        p13Play, p13Attack, p13Decay, p13Release, p13Filter, p13Cutoff,
                        p13Resonance, p13Velocity, p13Vary, p13Choke, p13Output);
                return;
            case 13:
                padShow(g, what, x, y, p14Level, p14Pan, p14Tune, p14Start, p14End, p14Reverse,
                        p14Play, p14Attack, p14Decay, p14Release, p14Filter, p14Cutoff,
                        p14Resonance, p14Velocity, p14Vary, p14Choke, p14Output);
                return;
            case 14:
                padShow(g, what, x, y, p15Level, p15Pan, p15Tune, p15Start, p15End, p15Reverse,
                        p15Play, p15Attack, p15Decay, p15Release, p15Filter, p15Cutoff,
                        p15Resonance, p15Velocity, p15Vary, p15Choke, p15Output);
                return;
            case 15:
                padShow(g, what, x, y, p16Level, p16Pan, p16Tune, p16Start, p16End, p16Reverse,
                        p16Play, p16Attack, p16Decay, p16Release, p16Filter, p16Cutoff,
                        p16Resonance, p16Velocity, p16Vary, p16Choke, p16Output);
                return;
        }
    }

    // Parameter `code`'s position on its dial (how 0), value in its unit
    // (1), or choice (2): the selected pad's, or every pad's.
    double readParam(int code, int how) const {
        if (code == cFirstKey) return readOn(firstKey, how);
        if (code == cOversampling) return readOn(oversampling, how);
        if (code == cOut) return readOn(output, how);
        return padRead(selected, code, how);
    }
    // To a value (how 0), a position (1) or its default (2).
    void editParam(int code, int how, double v) {
        if (code == cFirstKey) editOn(firstKey, how, v);
        if (code == cOversampling) editOn(oversampling, how, v);
        if (code == cOut) editOn(output, how, v);
        if (code < 100) padWrite(selected, code, how, v);
    }
    int choiceOf(int code) const { return roundToInt(readParam(code, 2)); }
    double valueOf(int code) const { return readParam(code, 1); }
    double padValue(int p, int what) const { return padRead(p, what, 1); }
    int padChoice(int p, int what) const { return roundToInt(padRead(p, what, 2)); }

    bool bipolar(int code) const { return code == cPan || code == cTune; }

    // A key's name: 60 is C4.
    void fillNote(Canvas& g, int key, double x, double y) const {
        const int octave = idiv(key, 12) - 1;
        const int n = imod(key, 12);
        if (n == 0) g.fillText(format("C%d", octave), x, y);
        if (n == 1) g.fillText(format("C#%d", octave), x, y);
        if (n == 2) g.fillText(format("D%d", octave), x, y);
        if (n == 3) g.fillText(format("D#%d", octave), x, y);
        if (n == 4) g.fillText(format("E%d", octave), x, y);
        if (n == 5) g.fillText(format("F%d", octave), x, y);
        if (n == 6) g.fillText(format("F#%d", octave), x, y);
        if (n == 7) g.fillText(format("G%d", octave), x, y);
        if (n == 8) g.fillText(format("G#%d", octave), x, y);
        if (n == 9) g.fillText(format("A%d", octave), x, y);
        if (n == 10) g.fillText(format("A#%d", octave), x, y);
        if (n == 11) g.fillText(format("B%d", octave), x, y);
    }

    void fillParam(Canvas& g, int code, double x, double y) {
        if (code == cFirstKey) {
            fillNote(g, choiceOf(cFirstKey), x, y);
            return;
        }
        if (code == cOut) {
            g.fillText(output.text(), x, y);
            return;
        }
        padText(g, selected, code, x, y);
    }

    void fillDialName(Canvas& g, int code, double x, double y) const {
        if (code == cLevel) g.fillText("LEVEL", x, y);
        if (code == cPan) g.fillText("PAN", x, y);
        if (code == cTune) g.fillText("TUNE", x, y);
        if (code == cAttack) g.fillText("ATTACK", x, y);
        if (code == cDecay) g.fillText("DECAY", x, y);
        if (code == cRelease) g.fillText("RELEASE", x, y);
        if (code == cCutoff) g.fillText("CUTOFF", x, y);
        if (code == cResonance) g.fillText("RESO", x, y);
        if (code == cVelocity) g.fillText("VELOCITY", x, y);
        if (code == cVary) g.fillText("VARY", x, y);
        if (code == cFirstKey) g.fillText("FIRST KEY", x, y);
        if (code == cOut) g.fillText("OUTPUT", x, y);
    }

    void fillButton(Canvas& g, int code, int value, double x, double y) const {
        if (code == cPlay) g.fillText(value == kOneShot ? "ONE-SHOT" : "GATED", x, y);
        if (code == cReverse) g.fillText("REVERSE", x, y);
        if (code == cFilter) {
            if (value == 0) g.fillText("OFF", x, y);
            if (value == 1) g.fillText("LP", x, y);
            if (value == 2) g.fillText("BP", x, y);
            if (value == 3) g.fillText("HP", x, y);
        }
        if (code == cChoke) {
            if (value == 0)
                g.fillText("OFF", x, y);
            else
                g.fillText(format("%d", value), x, y);
        }
        if (code == cOutput) {
            if (value == 0)
                g.fillText("MAIN", x, y);
            else
                g.fillText(format("%d", value), x, y);
        }
        if (code == cOversampling) {
            g.fillText(value == 0 ? "1x" : value == 1 ? "2x" : "4x", x, y);
        }
    }

    // --- what the editor shows, laid out -----------------------------------------------------

    void dial(int code, double x, double y, double r, int colour, bool live) {
        if (dials >= 64) return;
        dialCode[dials] = code;
        dialX[dials] = x;
        dialY[dials] = y;
        dialR[dials] = r;
        dialColour[dials] = colour;
        dialLive[dials] = live;
        ++dials;
    }

    void button(int code, int value, double x, double y, double w, double h, int style,
                int colour) {
        if (buttons >= 64) return;
        buttonCode[buttons] = code;
        buttonValue[buttons] = value;
        buttonX[buttons] = x;
        buttonY[buttons] = y;
        buttonW[buttons] = w;
        buttonH[buttons] = h;
        buttonStyle[buttons] = style;
        buttonColour[buttons] = colour;
        buttonLive[buttons] = true;
        ++buttons;
    }

    // Dims the buttons placed since `first` was the count.
    void dimSince(int first) {
        for (int k = first; k < buttons; ++k) buttonLive[k] = false;
    }

    // n buttons for `code`'s values first .. first + n - 1, across w.
    void row(int code, int first, int n, double x, double y, double w, double h, int colour) {
        const double gap = 5.0;
        const double each = (w - gap * (n - 1)) / n;
        for (int k = 0; k < n; ++k)
            button(code, first + k, x + k * (each + gap), y, each, h, bChoice, colour);
    }

    // The rows of the selected pad's panel, and every pad's bar.
    static constexpr double soundY = selY + 302;
    static constexpr double envY = selY + 404;
    static constexpr double filterY = selY + 488;
    static constexpr double routeY = selY + 566;

    void place() {
        dials = 0;
        buttons = 0;
        const double r = 17;
        dial(cLevel, selX + 44, soundY, r, kWillow, true);
        dial(cPan, selX + 108, soundY, r, kWillow, true);
        dial(cTune, selX + 172, soundY, r, kWillow, true);
        dial(cVelocity, selX + 236, soundY, r, kWillow, true);
        dial(cVary, selX + 300, soundY, r, kWillow, true);
        row(cPlay, 0, 2, selX + 340, soundY - 24, 160, 22, kWillow);
        button(cReverse, 0, selX + 340, soundY + 4, 160, 22, bToggle, kWillow);
        const bool gated = choiceOf(cPlay) == kGated;
        dial(cAttack, selX + 44, envY, r, kWillow, true);
        dial(cDecay, selX + 108, envY, r, kWillow, true);
        dial(cRelease, selX + 172, envY, r, kWillow, gated);
        const bool on = choiceOf(cFilter) != 0;
        row(cFilter, 0, 4, selX + 24, filterY - 11, 190, 22, kSand);
        dial(cCutoff, selX + 262, filterY, r, kSand, on);
        dial(cResonance, selX + 326, filterY, r, kSand, on);
        row(cChoke, 0, 9, selX + 92, routeY - 30, 300, 20, kSeaGlass);
        row(cOutput, 0, 9, selX + 92, routeY + 2, 300, 20, kSeaGlass);
        dial(cFirstKey, padsX + 52, barY + barH * 0.5 + 4, 15, kLinen, true);
        row(cOversampling, 0, 3, padsX + 150, barY + barH * 0.5 - 8, 210, 22, kLinen);
        dial(cOut, padsX + 430, barY + barH * 0.5 + 4, 15, kLinen, true);
    }

    int dialAt(double x, double y) const {
        for (int k = 0; k < dials; ++k) {
            const double dx = x - dialX[k], dy = y - dialY[k];
            if (dialLive[k] && dx * dx + dy * dy <= (dialR[k] + 4) * (dialR[k] + 4)) return k;
        }
        return -1;
    }
    int buttonAt(double x, double y) const {
        for (int k = 0; k < buttons; ++k)
            if (buttonLive[k] && x >= buttonX[k] && x < buttonX[k] + buttonW[k] &&
                y >= buttonY[k] && y < buttonY[k] + buttonH[k])
                return k;
        return -1;
    }

    // Pad p's cell: four across, pad 1 at the bottom left.
    double cellX(int p) const { return gridX + imod(p, 4) * (cellW + cellGap); }
    double cellY(int p) const { return gridY + (3 - idiv(p, 4)) * (cellH + cellGap); }
    // The pad whose playing face (above its file's slot) is at (x, y), or -1.
    int padAt(double x, double y) const {
        for (int p = 0; p < kPads; ++p)
            if (x >= cellX(p) && x < cellX(p) + cellW && y >= cellY(p) &&
                y < cellY(p) + cellH - slotH - 4)
                return p;
        return -1;
    }

    // --- drawing --------------------------------------------------------------------------------

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
        g.serifFont(15);
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

    // A dial: an arc from 7 to 5 o'clock, lit to the value (a bipolar one
    // from the top), its name over it and its value under it.
    void drawDial(Canvas& g, int k) {
        const int code = dialCode[k];
        const double x = dialX[k], y = dialY[k], r = dialR[k];
        const bool hot = k == hotDial || (dragging == 1 && k == dragDial);
        if (!dialLive[k]) g.globalAlpha(0.35);
        const double from0 = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 16 ? 5.0 : 4.0);
        g.beginPath();
        g.arc(x, y, r, from0, from0 + sweep);
        g.stroke();
        const double at = from0 + sweep * clamp(readParam(code, 0), 0.0, 1.0);
        const double from = bipolar(code) ? from0 + 0.5 * sweep : from0;
        g.strokeStyle(dialColour[k]);
        g.lineWidth(r > 16 ? 3.6 : 3.0);
        if (abs(at - from) > 0.001) {
            g.beginPath();
            if (at >= from)
                g.arc(x, y, r, from, at);
            else
                g.arc(x, y, r, at, from);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 3, hot ? kKnobHot : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 5.5);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(1.8);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 12), y + sin(at) * (r - 12));
        g.lineTo(x + cos(at) * (r - 7), y + sin(at) * (r - 7));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        fillDialName(g, code, x, y - r - 9);
        g.fillStyle(hot ? Theme::text : Theme::textMuted);
        g.monoFont(9.5);
        fillParam(g, code, x, y + r + 9);
        g.globalAlpha(1.0);
    }

    bool buttonLit(int k) const {
        if (buttonStyle[k] == bToggle) return valueOf(buttonCode[k]) > 0.5;
        return choiceOf(buttonCode[k]) == buttonValue[k];
    }

    // A button's face, and the style its text takes.
    void pill(Canvas& g, double x, double y, double w, double h, bool lit, int colour, bool hot) {
        g.fillStyle(lit ? colour : (hot ? 0x3A2F27 : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, min(h * 0.5, 11.0));
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(lit ? kInk : (hot ? Theme::text : Theme::textMuted));
        g.boldFont(9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void drawButton(Canvas& g, int k) {
        const double x = buttonX[k], y = buttonY[k], w = buttonW[k], h = buttonH[k];
        if (!buttonLive[k]) g.globalAlpha(0.35);
        pill(g, x, y, w, h, buttonLit(k), buttonColour[k], k == hotButton);
        fillButton(g, buttonCode[k], buttonValue[k], x + w * 0.5, y + h * 0.5 + 0.5);
        g.globalAlpha(1.0);
    }

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right) {
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

    // --- the mark and the header --------------------------------------------------------------

    // A pollard: a short trunk with a knuckled head, and the crown of stems
    // that grows from it; a stem lights for each pad sounding.
    void drawMark(Canvas& g, double x, double y) {
        g.linearGradient(0, y + 2, 0, y + 24, kBark, 0x5E4B36);
        g.beginPath();
        g.moveTo(x - 6, y + 24);
        g.lineTo(x - 5, y + 6);
        g.quadraticCurveTo(x - 10, y + 4, x - 8, y);
        g.quadraticCurveTo(x, y - 3, x + 8, y);
        g.quadraticCurveTo(x + 10, y + 4, x + 5, y + 6);
        g.lineTo(x + 6, y + 24);
        g.closePath();
        g.fill();
        g.lineCap(LineCap::Round);
        for (int k = 0; k < kPads; ++k) {
            const double base = x - 7 + k * 0.93;
            const double lean = (k - 7.5) * 1.5;
            const double tall = 14 + 4 * sin(k * 2.3);
            g.strokeStyle(glow[k] > 0.001 ? kLinen : (imod(k, 2) == 0 ? kShoot : 0x8A9A57));
            g.lineWidth(1.1);
            g.beginPath();
            g.moveTo(base, y + 1);
            g.quadraticCurveTo(base + lean * 0.2, y + 1 - tall * 0.6, base + lean, y + 1 - tall);
            g.stroke();
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 34, 34);
        g.fillStyle(Theme::text);
        g.serifFont(25);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Pollard", 60, 41);
        const double x = 68 + g.measureText("Pollard");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("sixteen pads from ", x, 41);
        fillNote(g, choiceOf(cFirstKey), x + g.measureText("sixteen pads from "), 41);
        label(g, "VOICES", 1032, 24, TextAlign::Center);
        g.fillStyle(Theme::text);
        g.monoFont(13);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%d / %d", soundingNow.get(), kVoices), 1032, 42);
        label(g, "MAIN", 1080, 30, TextAlign::Right);
        drawLevel(g, 1088, 25, 120, outLeft.get(), outRight.get());
    }

    // --- the pads -----------------------------------------------------------------------------

    // Pad p's peaks over [x, x + w), mirrored about y, the region lit.
    void drawPeaks(Canvas& g, int p, double x, double y, double w, double half, int colour,
                   bool lit) {
        const int columns = floorToInt(w);
        const double a = min(padValue(p, cStart), padValue(p, cEnd));
        const double b = max(padValue(p, cStart), padValue(p, cEnd));
        for (int pass = 0; pass < 2; ++pass) {
            g.save();
            if (pass == 1) g.clip(x + a * w, y - half - 1, max((b - a) * w, 1.0), 2 * half + 2);
            g.fillStyle(colour);
            g.globalAlpha(pass == 0 ? 0.22 : (lit ? 0.95 : 0.8));
            g.beginPath();
            for (int px = 0; px <= columns; ++px) {
                const int k = min(kColumns - 1, idiv(px * kColumns, columns + 1));
                const double top = y - min(1.0, overview[p * kColumns + k]) * half;
                if (px == 0)
                    g.moveTo(x + px, top);
                else
                    g.lineTo(x + px, top);
            }
            for (int px = columns; px >= 0; --px) {
                const int k = min(kColumns - 1, idiv(px * kColumns, columns + 1));
                g.lineTo(x + px, y + min(1.0, overview[p * kColumns + k]) * half + 0.5);
            }
            g.closePath();
            g.fill();
            g.restore();
        }
        g.globalAlpha(1.0);
    }

    void drawPad(Canvas& g, int p) {
        const double x = cellX(p), y = cellY(p);
        const double faceH = cellH - slotH - 4;
        const bool sel = p == selected;
        const bool hot = p == hotPad || (dragging == 3 && playedKey - choiceOf(cFirstKey) == p);
        const double lit = glow[p];
        g.linearGradient(0, y, 0, y + faceH, hot ? 0x3A2F27 : Theme::raised, kInk);
        g.beginPath();
        g.roundRect(x, y, cellW, faceH, 7);
        g.fill();
        if (lit > 0.001) {
            g.fillStyle(kShoot);
            g.globalAlpha(0.10 + 0.30 * lit);
            g.beginPath();
            g.roundRect(x, y, cellW, faceH, 7);
            g.fill();
            g.globalAlpha(1.0);
        }
        g.strokeStyle(sel ? kWillow : Theme::line);
        g.lineWidth(sel ? 1.6 : 1.0);
        g.beginPath();
        g.roundRect(x + 0.5, y + 0.5, cellW - 1, faceH - 1, 7);
        g.stroke();
        // Its number and key; its group and output.
        g.fillStyle(sel ? kWillow : Theme::text);
        g.boldFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%d", p + 1), x + 8, y + 12);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        fillNote(g, choiceOf(cFirstKey) + p, x + 26, y + 12);
        // Its tune, which tells apart pads that share a file (a kit's toms).
        const double tune = padValue(p, cTune);
        if (tune != 0) g.fillText(format("%+g st", tune), x + 8, y + 24);
        g.textAlign(TextAlign::Right);
        g.boldFont(8);
        const int choke = padChoice(p, cChoke), out = padChoice(p, cOutput);
        g.fillStyle(kSeaGlass);
        if (out > 0) g.fillText(format("OUT %d", out), x + cellW - 8, y + 12);
        if (choke > 0) g.fillText(format("CHOKE %d", choke), x + cellW - 8, y + 24);
        if (padValue(p, cReverse) > 0.5) {
            g.fillStyle(kWillow);
            g.fillText("REV", x + cellW - 8, y + faceH - 10);
        }
        if (framesShown[p] == 0) {
            aside(g, "no sound", x + cellW * 0.5, y + faceH * 0.5 + 6, TextAlign::Center);
            return;
        }
        drawPeaks(g, p, x + 8, y + faceH * 0.5 + 8, cellW - 16, faceH * 0.5 - 22,
                  lit > 0.001 ? kLinen : kWillow, lit > 0.001);
    }

    void drawPadsPanel(Canvas& g) {
        panel(g, padsX, padsY, padsW, padsH);
        heading(g, "Pads", padsX + 18, padsY + 25);
        aside(g, "press to play, higher is harder; drop a file on a pad's name", padsX + padsW - 18,
              padsY + 25, TextAlign::Right);
        for (int p = 0; p < kPads; ++p) drawPad(g, p);
    }

    // --- the selected pad ---------------------------------------------------------------------

    static constexpr double inX = wavX + 8;
    static constexpr double inW = wavW - 16;
    double xOfShare(double p) const { return inX + inW * clamp(p, 0.0, 1.0); }

    void drawWave(Canvas& g) {
        well(g, wavX, wavY, wavW, wavH);
        const int n = framesShown[selected];
        if (n == 0) {
            aside(g,
                  "no file: drop one on the pad's name, click it to choose, or pick a factory hit",
                  wavX + wavW * 0.5, wavY + wavH * 0.5, TextAlign::Center);
            return;
        }
        const double mid = wavY + flagH + (wavH - flagH - 10) * 0.5;
        drawPeaks(g, selected, inX, mid, inW, (wavH - flagH - 14) * 0.5, kWillow, false);
        // Where each of its voices plays.
        g.strokeStyle(kLinen);
        g.lineWidth(1);
        g.globalAlpha(0.8);
        for (int v = 0; v < kVoices; ++v) {
            if (heads[v] < 0.0 || headPad[v] != selected) continue;
            const double x = inX + inW * clamp(heads[v] / n, 0.0, 1.0);
            g.beginPath();
            g.moveTo(x + 0.5, wavY + flagH + 2);
            g.lineTo(x + 0.5, wavY + wavH - 6);
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The start and end flags.
        for (int k = 0; k < 2; ++k) {
            const int code = k == 0 ? cStart : cEnd;
            const double x = xOfShare(valueOf(code));
            const bool hot = hotHandle == code || (dragging == 2 && handle == code);
            g.strokeStyle(hot ? kLinen : kWillow);
            g.lineWidth(hot ? 1.6 : 1.1);
            g.beginPath();
            g.moveTo(x + 0.5, wavY + 3);
            g.lineTo(x + 0.5, wavY + wavH - 6);
            g.stroke();
            const double d = k == 0 ? 1.0 : -1.0;
            g.fillStyle(hot ? kLinen : kWillow);
            g.beginPath();
            g.moveTo(x + 0.5, wavY + 3);
            g.lineTo(x + 0.5 + 10 * d, wavY + 8);
            g.lineTo(x + 0.5, wavY + 13);
            g.closePath();
            g.fill();
            g.fillStyle(kInk);
            g.boldFont(7);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            g.fillText(k == 0 ? "S" : "E", x + 0.5 + 3.5 * d, wavY + 8.5);
        }
    }

    // Where the selected pad starts, 2.5 ms each way: reversed, the region's end.
    void drawPane(Canvas& g) {
        const bool back = valueOf(cReverse) > 0.5;
        label(g, back ? "START, FROM THE END" : "START", paneX + 2, paneY - 8, TextAlign::Left);
        aside(g, "2.5 ms each way", paneX + paneW - 2, paneY - 8, TextAlign::Right);
        well(g, paneX, paneY, paneW, paneH);
        const double mid = paneY + paneH * 0.5;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(paneX + 4, mid);
        g.lineTo(paneX + paneW - 4, mid);
        g.moveTo(paneX + paneW * 0.5, paneY + 4);
        g.lineTo(paneX + paneW * 0.5, paneY + paneH - 4);
        g.stroke();
        if (framesShown[selected] == 0) return;
        const int from = selected * kPane;
        double most = 0.05;
        for (int j = 0; j < kPane; ++j) most = max(most, abs(pane[from + j]));
        g.strokeStyle(kWillow);
        g.lineWidth(1.4);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int j = 0; j < kPane; ++j) {
            const double px = paneX + 6 + (paneW - 12) * j / (kPane - 1.0);
            const double py = mid - pane[from + j] / most * (paneH * 0.5 - 8);
            if (j == 0)
                g.moveTo(px, py);
            else
                g.lineTo(px, py);
        }
        g.stroke();
    }

    // The envelope as it plays: up over the attack, down over the decay,
    // and for a gated pad the release after a held key.
    void drawShape(Canvas& g, double x, double y, double w, double h) {
        well(g, x, y, w, h);
        const double a = valueOf(cAttack), d = valueOf(cDecay), rel = valueOf(cRelease);
        const bool full = d >= kFull;
        const bool gated = choiceOf(cPlay) == kGated;
        const double span = a + (full ? 400.0 : min(d, 2000.0)) + (gated ? min(rel, 1000.0) : 0.0);
        const double top = y + 8, bottom = y + h - 8;
        const double sx = (w - 16) / max(span, 1.0);
        g.strokeStyle(kWillow);
        g.lineWidth(1.6);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        g.moveTo(x + 8, bottom);
        g.lineTo(x + 8 + a * sx, top);
        const double held = full ? 400.0 : min(d, 2000.0);
        const double hold = gated && !full ? held * 0.4 : held;
        for (int k = 1; k <= 24; ++k) {
            const double t = hold * k / 24.0;
            const double level = full ? 1.0 : 1.0 - fall(t / max(d, 1.0), 0.5);
            g.lineTo(x + 8 + (a + t) * sx, bottom - (bottom - top) * level);
        }
        if (gated) {
            const double from = full ? 1.0 : 1.0 - fall(hold / max(d, 1.0), 0.5);
            const double r = min(rel, 1000.0);
            for (int k = 1; k <= 16; ++k) {
                const double t = r * k / 16.0;
                const double level = from * (1.0 - fall(t / max(r, 1.0), 0.5));
                g.lineTo(x + 8 + (a + hold + t) * sx, bottom - (bottom - top) * level);
            }
        }
        g.stroke();
        if (full) aside(g, "to its end", x + w - 10, top + 6, TextAlign::Right);
    }

    void drawSelectedPanel(Canvas& g) {
        panel(g, selX, selY, selW, selH);
        g.fillStyle(Theme::text);
        g.serifFont(15);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("Pad %d", selected + 1), selX + 18, selY + 25);
        g.fillStyle(Theme::textMuted);
        g.monoFont(11);
        fillNote(g, choiceOf(cFirstKey) + selected, selX + 82, selY + 25);
        aside(g, "drag the flags for the region it plays", selX + selW - 18, selY + 25,
              TextAlign::Right);
        drawWave(g);
        drawPane(g);
        label(g, "SOUND", selX + 18, soundY - 40, TextAlign::Left);
        label(g, "ENVELOPE", selX + 18, envY - 40, TextAlign::Left);
        drawShape(g, selX + 214, envY - 30, 168, 62);
        label(g, "FILTER", selX + 18, filterY - 32, TextAlign::Left);
        label(g, "CHOKE", selX + 18, routeY - 20, TextAlign::Left);
        label(g, "OUTPUT", selX + 18, routeY + 12, TextAlign::Left);
        if (choiceOf(cOutput) > 0)
            aside(g, "out of the main mix", selX + 404, routeY + 12, TextAlign::Left);
        if (choiceOf(cChoke) > 0)
            aside(g, "cuts its group, itself too", selX + 404, routeY - 20, TextAlign::Left);
    }

    void drawBar(Canvas& g) {
        panel(g, padsX, barY, kW - 32, barH);
        label(g, "OVERSAMPLING", padsX + 255, barY + 12, TextAlign::Center);
        aside(g, "settings for every pad; a pad's own are above", kW - 34, barY + barH * 0.5,
              TextAlign::Right);
    }

    void draw(Canvas& g) {
        place();
        g.linearGradient(0, 0, 0, kH, Theme::ground, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawPadsPanel(g);
        drawSelectedPanel(g);
        drawBar(g);
        for (int k = 0; k < buttons; ++k) drawButton(g, k);
        for (int k = 0; k < dials; ++k) drawDial(g, k);
    }

    // --- the pointer --------------------------------------------------------------------------

    // The flag at (x, y): cStart or cEnd, or -1.
    int waveHandle(double x, double y) const {
        if (framesShown[selected] == 0 || x < wavX || x >= wavX + wavW || y < wavY ||
            y >= wavY + wavH)
            return -1;
        const double xs = xOfShare(valueOf(cStart)), xe = xOfShare(valueOf(cEnd));
        const double reach = y < wavY + flagH ? 10.0 : 6.0;
        const double ds = abs(x - xs), de = abs(x - xe);
        if (ds <= reach && ds <= de) return cStart;
        if (de <= reach) return cEnd;
        return -1;
    }

    void pointer(const Pointer& p) {
        place();
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotDial = -1;
            hotButton = -1;
            hotHandle = -1;
            hotPad = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotDial = dialAt(p.x(), p.y());
            hotButton = buttonAt(p.x(), p.y());
            hotHandle = waveHandle(p.x(), p.y());
            hotPad = padAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p);
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                dialPos = clamp(dialPos - p.dy() * fine / 200.0, 0.0, 1.0);
                editParam(dialCode[dragDial], 1, dialPos);
            } else if (dragging == 2) {
                editParam(handle, 0, clamp(pressA + (p.x() - pressX) * fine / inW, 0.0, 1.0));
            }
            return;
        }
        if (a == PointerAction::Release) {
            if (dragging == 3) playNote(playedKey, 0.0, false);
            dragging = 0;
            playedKey = -1;
            hotDial = dialAt(p.x(), p.y());
            hotHandle = waveHandle(p.x(), p.y());
            hotPad = padAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            const int k = dialAt(p.x(), p.y());
            if (k >= 0) {
                const int code = dialCode[k];
                editParam(code, 1, clamp(readParam(code, 0) + notches * 0.02 * fine, 0.0, 1.0));
                return;
            }
            const int h = waveHandle(p.x(), p.y());
            if (h >= 0) editParam(h, 0, clamp(valueOf(h) + notches * 0.001 * fine, 0.0, 1.0));
        }
    }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        const int k = dialAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                editParam(dialCode[k], 2, 0.0);
                return;
            }
            dragging = 1;
            dragDial = k;
            dialPos = readParam(dialCode[k], 0);
            return;
        }
        const int b = buttonAt(x, y);
        if (b >= 0) {
            press(b);
            return;
        }
        const int h = waveHandle(x, y);
        if (h >= 0) {
            if (p.doubleClick()) {
                editParam(h, 2, 0.0);
                return;
            }
            dragging = 2;
            handle = h;
            pressX = x;
            pressA = valueOf(h);
            return;
        }
        const int pad = padAt(x, y);
        if (pad >= 0) {
            selected = pad;
            const double faceH = cellH - slotH - 4;
            const double up = 1.0 - clamp((y - cellY(pad)) / faceH, 0.0, 1.0);
            playedKey = choiceOf(cFirstKey) + pad;
            playNote(playedKey, 0.35 + 0.65 * up, true);
            dragging = 3;
        }
    }

    // A button: its choice, or its toggle flipped. A filter type chosen here
    // takes the cutoff where that type is open, if it was where the last was.
    void press(int b) {
        const int code = buttonCode[b], value = buttonValue[b];
        if (buttonStyle[b] == bToggle) {
            editParam(code, 0, valueOf(code) > 0.5 ? 0.0 : 1.0);
            return;
        }
        if (code == cFilter) {
            const double was = valueOf(cCutoff);
            const int type = choiceOf(cFilter);
            if (was == openAt(type) && value != type) editParam(cCutoff, 0, openAt(value));
        }
        editParam(code, 0, value);
    }
    double openAt(int type) const { return type == 3 ? 20.0 : type == 2 ? 1000.0 : 20000.0; }

    // --- Factory kits ---------------------------------------------------------------------
    //
    // Each sets the pads that differ from the default kit (Analog Kit).

    void factoryPatches(Patches& p) {
        p.add("Analog Kit");
        p.add("Boom Kit")
            .file(pad1, "factory:analog-boom.flac")
            .set(p3Tune, -2)
            .set(p4Level, -2)
            .set(p7Decay, 35)
            .set(p6Tune, -9)
            .set(p8Tune, -7)
            .set(p10Tune, -5)
            .set(p12Tune, -3)
            .set(p13Tune, -1)
            .set(p15Tune, 1)
            .set(p6Decay, 300)
            .set(p8Decay, 300)
            .set(p10Decay, 300)
            .set(p12Decay, 300)
            .set(p13Decay, 300)
            .set(p15Decay, 300);
        p.add("Dusty Kit")
            .set(p1Filter, "lowpass")
            .set(p2Filter, "lowpass")
            .set(p3Filter, "lowpass")
            .set(p4Filter, "lowpass")
            .set(p5Filter, "lowpass")
            .set(p6Filter, "lowpass")
            .set(p7Filter, "lowpass")
            .set(p8Filter, "lowpass")
            .set(p9Filter, "lowpass")
            .set(p10Filter, "lowpass")
            .set(p11Filter, "lowpass")
            .set(p12Filter, "lowpass")
            .set(p13Filter, "lowpass")
            .set(p14Filter, "lowpass")
            .set(p15Filter, "lowpass")
            .set(p16Filter, "lowpass")
            .set(p1Cutoff, 4500)
            .set(p2Cutoff, 4500)
            .set(p3Cutoff, 4500)
            .set(p4Cutoff, 4500)
            .set(p5Cutoff, 4500)
            .set(p6Cutoff, 4500)
            .set(p7Cutoff, 7000)
            .set(p8Cutoff, 4500)
            .set(p9Cutoff, 7000)
            .set(p10Cutoff, 4500)
            .set(p11Cutoff, 7000)
            .set(p12Cutoff, 4500)
            .set(p13Cutoff, 4500)
            .set(p14Cutoff, 7000)
            .set(p15Cutoff, 4500)
            .set(p16Cutoff, 7000)
            .set(p1Resonance, 15)
            .set(p2Resonance, 15)
            .set(p3Resonance, 15)
            .set(p4Resonance, 15)
            .set(p5Resonance, 15)
            .set(p6Resonance, 15)
            .set(p7Resonance, 15)
            .set(p8Resonance, 15)
            .set(p9Resonance, 15)
            .set(p10Resonance, 15)
            .set(p11Resonance, 15)
            .set(p12Resonance, 15)
            .set(p13Resonance, 15)
            .set(p14Resonance, 15)
            .set(p15Resonance, 15)
            .set(p16Resonance, 15)
            .set(p1Vary, 35)
            .set(p2Vary, 35)
            .set(p3Vary, 35)
            .set(p4Vary, 35)
            .set(p5Vary, 35)
            .set(p6Vary, 35)
            .set(p7Vary, 35)
            .set(p8Vary, 35)
            .set(p9Vary, 35)
            .set(p10Vary, 35)
            .set(p11Vary, 35)
            .set(p12Vary, 35)
            .set(p13Vary, 35)
            .set(p14Vary, 35)
            .set(p15Vary, 35)
            .set(p16Vary, 35)
            .set(p1Tune, -2)
            .set(p3Tune, -1);
        p.add("FM Kit")
            .file(pad1, "factory:fm-kick.flac")
            .file(pad2, "factory:fm-zap.flac")
            .file(pad3, "factory:fm-snare.flac")
            .file(pad5, "factory:fm-snare.flac")
            .file(pad6, "factory:fm-tom.flac")
            .file(pad7, "factory:fm-hat.flac")
            .file(pad8, "factory:fm-tom.flac")
            .file(pad9, "factory:fm-hat.flac")
            .file(pad10, "factory:fm-tom.flac")
            .file(pad11, "factory:fm-open-hat.flac")
            .file(pad12, "factory:fm-tom.flac")
            .file(pad13, "factory:fm-tom.flac")
            .file(pad15, "factory:fm-tom.flac")
            .set(p2Level, -6);
        p.add("Hand Percussion")
            .file(pad1, "factory:hand-conga.flac")
            .file(pad2, "factory:hand-conga.flac")
            .file(pad3, "factory:hand-conga.flac")
            .file(pad4, "factory:hand-bongo.flac")
            .file(pad5, "factory:hand-bongo.flac")
            .file(pad6, "factory:hand-clave.flac")
            .file(pad7, "factory:analog-shaker.flac")
            .file(pad8, "factory:hand-woodblock.flac")
            .file(pad9, "factory:hand-cabasa.flac")
            .file(pad10, "factory:hand-woodblock.flac")
            .file(pad11, "factory:hand-tambourine.flac")
            .file(pad12, "factory:analog-cowbell.flac")
            .file(pad13, "factory:hand-conga.flac")
            .file(pad14, "factory:hand-bongo.flac")
            .file(pad15, "factory:hand-clave.flac")
            .file(pad16, "factory:analog-cowbell.flac")
            .set(p1Tune, -5)
            .set(p2Tune, 0)
            .set(p3Tune, 4)
            .set(p4Tune, -3)
            .set(p5Tune, 2)
            .set(p6Tune, 0)
            .set(p8Tune, -5)
            .set(p10Tune, 2)
            .set(p12Tune, 0)
            .set(p13Tune, 7)
            .set(p14Tune, 7)
            .set(p15Tune, -5)
            .set(p16Tune, -5)
            .set(p1Level, -2)
            .set(p2Level, -2)
            .set(p3Level, -2)
            .set(p5Level, -1)
            .set(p6Level, -4)
            .set(p7Level, -6)
            .set(p9Level, -6)
            .set(p11Level, -6)
            .set(p12Level, -6)
            .set(p16Level, -8)
            .set(p13Decay, 120)
            .set(p9Decay, kFull)
            .set(p7Choke, "off")
            .set(p9Choke, "off")
            .set(p11Choke, "off")
            .set(p7Vary, 40)
            .set(p9Vary, 40)
            .set(p11Vary, 30)
            .set(p1Pan, -20)
            .set(p2Pan, -10)
            .set(p3Pan, 0)
            .set(p4Pan, 20)
            .set(p5Pan, 30)
            .set(p6Pan, -30)
            .set(p8Pan, 25)
            .set(p10Pan, -25)
            .set(p12Pan, 15);
        p.add("Reverse Kit")
            .set(p5Reverse, true)
            .set(p11Reverse, true)
            .set(p14Reverse, true)
            .set(p14Play, "gated")
            .set(p14Release, 200);
        p.add("Multi-Out Kit")
            .set(p1Output, "out1")
            .set(p3Output, "out2")
            .set(p5Output, "out2")
            .set(p7Output, "out3")
            .set(p9Output, "out3")
            .set(p11Output, "out3")
            .set(p6Output, "out4")
            .set(p8Output, "out4")
            .set(p10Output, "out4")
            .set(p12Output, "out4")
            .set(p13Output, "out4")
            .set(p15Output, "out4")
            .set(p14Output, "out5")
            .set(p16Output, "out5")
            .set(p2Output, "out6")
            .set(p4Output, "out6");
    }
};
