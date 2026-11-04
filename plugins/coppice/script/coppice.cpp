// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Coppice: Steppe's sampler.
//
// A coppiced wood is cut back to its stools and grows again from them, every
// shoot a new stem from the same root. Coppice plays one sound -- a file of
// the user's, or one of its factory samples -- three ways:
//
//   Classic    pitched across the keys from its root: a region of the file,
//              forward or reversed, and inside it a loop (forward with a
//              crossfade, or ping-pong) that sustains while the key is held
//              and, with Play On, lets the sound run to its end once it is up.
//   One-Shot   the region whole at each key, whatever the key does.
//   Slice      the region cut at its hits (or into equal parts), a slice a
//              key from First Key up: Trigger plays a slice to its end, Gate
//              while the key is held, Thru on through the rest of the region.
//
// Each voice then goes through one filter (a ladder or a state-variable
// filter) and its amp envelope; a second envelope bends the pitch and moves
// the cutoff; two LFOs, four macros and what every instrument has (velocity,
// the key, the mod wheel...) reach the rest through modulation routes:
// "velocity>sample.start": {"amount": "20 %"} starts harder notes later.
//
// Files arrive resampled to the session's rate, measured as they load (the
// runtime's peak() and rms()), so a slice's place, the waveform and the
// level found for Normalize are ready in the block the file arrives in. As
// in Marram, every voice runs oversampled (2x at Normal, 4x at High) and is
// brought down once; modulation is worked out every 16 samples of a voice's
// life, so a render is the same at any block size.

constexpr int kVoices = 32;
constexpr int kColumns = 1024;  // the waveform's, per channel
constexpr int kSlices = 64;
// Room for a chord: the factory samples peak at -3 dBFS, and with this the
// default's note peaks near -16 dBFS and a chord near -7, about as Marram's do.
constexpr double kHeadroom = 0.35481339;  // -9 dB
constexpr int kPane = 256;                // points in a close-up
constexpr double kPaneMs = 2.5;           // each side of a close-up's centre

// Modes, by their choices' indices.
constexpr int kClassic = 0;
constexpr int kOneShot = 1;
constexpr int kSlice = 2;
constexpr int kTrigger = 0;
constexpr int kGate = 1;
constexpr int kThru = 2;

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

struct Coppice : Instrument {
    static constexpr Info info = {
        .name = "Coppice",
        .id = "com.suppleaudio.coppice",
        .vendor = "Supple Audio",
        .description =
            "Sampler: one sound, played pitched with a crossfaded or ping-pong loop, as a "
            "one-shot, or cut into slices across the keys, through a filter, two envelopes, two "
            "LFOs and four macros, connected by modulation routes.",
        .features = {"sampler"},
        .voices = kVoices};

    // --- Sample ---------------------------------------------------------------------------

    AudioFile sample = audioFile("Sample")
                           .withDefault("factory:keys.flac")
                           .help(
                               "The sound it plays: a WAV, AIFF or FLAC of your own, or one of "
                               "its factory samples.");
    Param mode = choice("Mode", {"Classic", "One-Shot", "Slice"}, 0)
                     .keys({"classic", "one_shot", "slice"})
                     .withKey("sample.mode")
                     .in("Sample")
                     .help(
                         "Classic plays the sound pitched across the keys, with a loop; "
                         "One-Shot plays it whole at each key; Slice cuts it into slices, one "
                         "a key.");
    Param start = percent("Start", 0)
                      .withKey("sample.start")
                      .polyModulated()
                      .in("Sample")
                      .help(
                          "Where in the file the sound starts. Routes move each note's own "
                          "start, read as it begins: velocity to start, a harder note later.");
    Param end = percent("End", 100)
                    .withKey("sample.end")
                    .in("Sample")
                    .help("Where it ends: the region between the two is what plays.");
    Param reverse = toggle("Reverse", false)
                        .withKey("sample.reverse")
                        .in("Sample")
                        .help("Play the region backwards, from its end to its start.");
    Param normalize = toggle("Normalize", false)
                          .withKey("sample.normalize")
                          .in("Sample")
                          .help("Raise or lower the file so its loudest sample is at 0 dBFS.");
    Param gain = decibels("Gain", -24, 24, 0)
                     .withKey("sample.gain")
                     .in("Sample")
                     .help("The file's level into the voice.");
    Param root = integer("Root", 0, 127, 60)
                     .withKey("sample.root")
                     .in("Sample")
                     .activeUnless(mode, {"slice"})
                     .help("The key that plays the sound as it was recorded (60 is middle C).");
    Param transpose = semitones("Transpose", -48, 48, 0)
                          .withKey("sample.transpose")
                          .in("Sample")
                          .help("Every key's pitch, moved by whole semitones.");
    Param keytrack = toggle("Keytrack", true)
                         .withKey("sample.keytrack")
                         .in("Sample")
                         .activeUnless(mode, {"slice"})
                         .help("Keys change the pitch. Off, every key plays it at the root's.");

    // --- Loop -----------------------------------------------------------------------------

    Param loopMode = choice("Loop", {"Off", "Forward", "Ping-Pong"}, 0)
                         .keys({"off", "forward", "ping_pong"})
                         .withKey("loop.mode")
                         .in("Loop")
                         .activeWhen(mode, {"classic"})
                         .help(
                             "While the key is held, play the loop again and again: forward, "
                             "or back and forth.");
    Param loopStart = percent("Loop Start", 50)
                          .withKey("loop.start")
                          .in("Loop")
                          .activeWhen(loopMode, {"forward", "ping_pong"})
                          .help("Where in the file the loop starts, kept inside the region.");
    Param loopEnd = percent("Loop End", 100)
                        .withKey("loop.end")
                        .in("Loop")
                        .activeWhen(loopMode, {"forward", "ping_pong"})
                        .help("Where it ends.");
    Param crossfade = milliseconds("Crossfade", 0, 1000, 20)
                          .withKey("loop.crossfade")
                          .in("Loop")
                          .activeWhen(loopMode, {"forward"})
                          .help(
                              "How long the end of the loop fades into its start, so the seam "
                              "is not heard; at most half the loop.");
    Param snap = toggle("Snap", true)
                     .withKey("loop.snap")
                     .in("Loop")
                     .activeWhen(loopMode, {"forward", "ping_pong"})
                     .help(
                         "Move the loop's ends to the nearest rising zero crossings (within "
                         "5 ms), where joining makes no click.");
    Param playOn = toggle("Play On", false)
                       .withKey("loop.release")
                       .in("Loop")
                       .activeWhen(loopMode, {"forward", "ping_pong"})
                       .help(
                           "Once the key is up, leave the loop and play on to the end of the "
                           "region; off, the loop goes on through the release.");

    // --- Slice ----------------------------------------------------------------------------

    Param sliceBy = choice("Slice By", {"Transients", "Grid"}, 0)
                        .withKey("slice.by")
                        .in("Slice")
                        .activeWhen(mode, {"slice"})
                        .help("Cut at the sound's hits, or into equal parts.");
    Param sensitivity = percent("Sensitivity", 50)
                            .withKey("slice.sensitivity")
                            .in("Slice")
                            .activeWhen(sliceBy, {"transients"})
                            .help("How small a hit makes a slice: more finds softer ones.");
    Param sliceCount = integer("Slices", 2, kSlices, 16)
                           .withKey("slice.count")
                           .in("Slice")
                           .activeWhen(sliceBy, {"grid"})
                           .help("How many equal parts.");
    Param firstKey = integer("First Key", 0, 127, 36)
                         .withKey("slice.key")
                         .in("Slice")
                         .activeWhen(mode, {"slice"})
                         .help("The key that plays the first slice; each key above, the next.");
    Param slicePlay = choice("Play", {"Trigger", "Gate", "Thru"}, 0)
                          .keys({"trigger", "gate", "thru"})
                          .withKey("slice.play")
                          .in("Slice")
                          .activeWhen(mode, {"slice"})
                          .help(
                              "Trigger plays a slice to its end; Gate while its key is held; "
                              "Thru from its start on through the rest of the region.");

    // --- Voice ----------------------------------------------------------------------------

    Param voiceMode = choice("Voice Mode", {"Poly", "Mono", "Legato"}, 0)
                          .withKey("voice.mode")
                          .in("Voice")
                          .help(
                              "Poly plays chords; Mono one note at a time, each restarting the "
                              "envelopes; Legato one at a time, gliding without restarting them.");
    Param polyphony =
        integer("Polyphony", 1, kVoices, 16)
            .withKey("voice.polyphony")
            .in("Voice")
            .activeWhen(voiceMode, {"poly"})
            .help("How many notes sound at once; a note past them takes the oldest's voice.");
    Param glide = milliseconds("Glide", 0, 5000, 0)
                      .withKey("voice.glide")
                      .in("Voice")
                      .help("How long the pitch takes to slide to a new note.");
    Param bendRange = semitones("Bend Range", 0, 48, 2)
                          .withKey("voice.bend_range")
                          .in("Voice")
                          .help("How far the pitch wheel bends, each way.");
    Param tune = cents("Tune", -100, 100, 0)
                     .withKey("voice.tune")
                     .polyModulated()
                     .routeRange(-4800, 4800)
                     .in("Voice")
                     .help(
                         "Fine tuning. Routes move the whole voice's pitch through it: an "
                         "LFO's vibrato, an envelope's sweep.");
    Param oversampling =
        choice("Oversampling", {"1x", "2x", "4x"}, 1)
            .keys({"x1", "x2", "x4"})
            .withKey("voice.oversampling")
            .in("Voice")
            .restarts()
            .help("Oversampling, which keeps a sound played far above its root free of aliasing.");

    // --- Filter ---------------------------------------------------------------------------

    Param fType = choice("Filter", {"Off", "Ladder", "SVF"}, 0)
                      .withKey("filter.type")
                      .in("Filter")
                      .help("A warm transistor ladder, or a clean state-variable filter.");
    Param cutoff = frequency("Cutoff", 20, 20000, 2000)
                       .withKey("filter.cutoff")
                       .polyModulated()
                       .in("Filter")
                       .activeUnless(fType, {"off"})
                       .help(
                           "The corner frequency. Key tracking, Env 2 and velocity move it "
                           "from here.");
    Param resonance = percent("Resonance", 10)
                          .withKey("filter.resonance")
                          .polyModulated()
                          .in("Filter")
                          .activeUnless(fType, {"off"})
                          .help("The peak at the cutoff: near 100 % it rings.");
    Param slope = choice("Slope", {"12 dB/oct", "24 dB/oct"}, 1)
                      .keys({"db12", "db24"})
                      .withKey("filter.slope")
                      .in("Filter")
                      .activeWhen(fType, {"ladder"})
                      .help("How steeply the ladder cuts above the cutoff.");
    Param fMode = choice("Filter Mode", {"Low Pass", "Band Pass", "High Pass", "Notch"}, 0)
                      .keys({"lowpass", "bandpass", "highpass", "notch"})
                      .withKey("filter.mode")
                      .in("Filter")
                      .activeWhen(fType, {"svf"})
                      .help("What the state-variable filter keeps.");
    Param fKeytrack = percent("Filter Keytrack", 0, 0, 200)
                          .withKey("filter.keytrack")
                          .in("Filter")
                          .activeUnless(fType, {"off"})
                          .help(
                              "How far the cutoff follows the note from middle C: 100 % moves "
                              "it an octave per octave.");
    Param fEnv = octaves("Filter Env", -8, 8, 0)
                     .withKey("filter.env")
                     .in("Filter")
                     .activeUnless(fType, {"off"})
                     .help("How far Env 2 moves the cutoff at its peak, in octaves.");
    Param fVelocity =
        octaves("Filter Velocity", 0, 4, 0)
            .withKey("filter.velocity")
            .in("Filter")
            .activeUnless(fType, {"off"})
            .help("How far the hardest notes open the cutoff, in octaves; softer ones less.");
    Param fDrive = decibels("Drive", 0, 24, 0)
                       .withKey("filter.drive")
                       .polyModulated()
                       .in("Filter")
                       .activeUnless(fType, {"off"})
                       .help("Gain into the filter: the ladder saturates and thickens.");

    // --- Amp, Env 2 -------------------------------------------------------------------------

    Param ampAttack = milliseconds("Amp Attack", 0, 30000, 0)
                          .withKey("amp.attack")
                          .polyModulated()
                          .in("Amp")
                          .help(
                              "How long a note takes to rise; 0 keeps a drum's attack as "
                              "sharp as it was recorded.");
    Param ampDecay = milliseconds("Amp Decay", 1, 30000, 1000)
                         .withKey("amp.decay")
                         .polyModulated()
                         .in("Amp")
                         .help("How long it then takes to fall to the sustain level.");
    Param ampSustain = percent("Amp Sustain", 100)
                           .withKey("amp.sustain")
                           .in("Amp")
                           .help("The level held while the key is down.");
    Param ampRelease = milliseconds("Amp Release", 1, 30000, 100)
                           .withKey("amp.release")
                           .polyModulated()
                           .in("Amp")
                           .help("How long the note takes to die away once the key is up.");
    Param ampVelocity =
        percent("Amp Velocity", 50)
            .withKey("amp.velocity")
            .in("Amp")
            .help("How much softer quiet notes play: at 100 % the softest is 40 dB down.");
    Param ampLevel = decibels("Amp Level", -48, 12, 0)
                         .withKey("amp.level")
                         .polyModulated()
                         .in("Amp")
                         .help("Each voice's level after the envelope; routes make a tremolo.");
    Param ampPan = percent("Pan", 0, -100, 100)
                       .withKey("amp.pan")
                       .polyModulated()
                       .in("Amp")
                       .help("Where the sound sits, left to right.");

    Param envAttack = milliseconds("Env 2 Attack", 0, 30000, 1)
                          .withKey("env2.attack")
                          .polyModulated()
                          .in("Env 2")
                          .help("The mod envelope's rise. It moves the pitch and the cutoff.");
    Param envDecay = milliseconds("Env 2 Decay", 1, 30000, 400)
                         .withKey("env2.decay")
                         .polyModulated()
                         .in("Env 2")
                         .help("Its fall to the sustain level.");
    Param envSustain = percent("Env 2 Sustain", 0)
                           .withKey("env2.sustain")
                           .in("Env 2")
                           .help("Its level while the key is down.");
    Param envRelease = milliseconds("Env 2 Release", 1, 30000, 300)
                           .withKey("env2.release")
                           .polyModulated()
                           .in("Env 2")
                           .help("Its fall once the key is up.");
    Param envPitch =
        semitones("Env 2 Pitch", -48, 48, 0)
            .withKey("env2.pitch")
            .in("Env 2")
            .help("How far it bends the pitch at its peak: a drum's or a zap's sweep.");

    // --- LFOs, macros ---------------------------------------------------------------------

    Param l1Shape =
        choice("LFO 1 Shape",
               {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Drift"}, 0)
            .keys({"sine", "triangle", "saw_up", "saw_down", "square", "random", "drift"})
            .withKey("lfo1.shape")
            .in("LFO 1")
            .help("Its wave: random holds a new value each cycle, drift glides between them.");
    Param l1Sync = toggle("LFO 1 Sync", false)
                       .withKey("lfo1.sync")
                       .in("LFO 1")
                       .help("Follow the song's tempo: a cycle per note value.");
    Param l1Rate =
        frequency("LFO 1 Rate", 0.01, 50, 2)
            .withKey("lfo1.rate")
            .polyModulated()
            .in("LFO 1")
            .activeUnless(l1Sync)
            .help("Cycles a second. Routes move it for each note's LFO, not a free one's.");
    Param l1Division = choice("LFO 1 Division",
                              {"1/32", "1/16", "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2",
                               "1 Bar", "2 Bars", "4 Bars"},
                              6)
                           .keys({"n1_32", "n1_16", "n1_8t", "n1_8", "n1_8d", "n1_4t", "n1_4",
                                  "n1_4d", "n1_2", "bar1", "bar2", "bar4"})
                           .withKey("lfo1.division")
                           .in("LFO 1")
                           .activeWhen(l1Sync)
                           .help("How long a cycle lasts, in notes of the song's tempo.");
    Param l1Mode = choice("LFO 1 Mode", {"Note", "Free"}, 0)
                       .withKey("lfo1.mode")
                       .in("LFO 1")
                       .help(
                           "Note: each note's own, from its start. Free: one for every voice, "
                           "running on (synced, it follows the song's position).");
    Param l1Phase =
        degrees("LFO 1 Phase", 0, 360, 0)
            .withKey("lfo1.phase")
            .in("LFO 1")
            .help("Where in its cycle it starts (Note), or how far it is moved (Free).");
    Param l1Delay = milliseconds("LFO 1 Delay", 0, 10000, 0)
                        .withKey("lfo1.delay")
                        .in("LFO 1")
                        .help("How long it takes to fade in after a note starts: a late vibrato.");

    Param l2Shape =
        choice("LFO 2 Shape",
               {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Drift"}, 0)
            .keys({"sine", "triangle", "saw_up", "saw_down", "square", "random", "drift"})
            .withKey("lfo2.shape")
            .in("LFO 2")
            .help("Its wave: random holds a new value each cycle, drift glides between them.");
    Param l2Sync = toggle("LFO 2 Sync", false)
                       .withKey("lfo2.sync")
                       .in("LFO 2")
                       .help("Follow the song's tempo: a cycle per note value.");
    Param l2Rate =
        frequency("LFO 2 Rate", 0.01, 50, 0.5)
            .withKey("lfo2.rate")
            .polyModulated()
            .in("LFO 2")
            .activeUnless(l2Sync)
            .help("Cycles a second. Routes move it for each note's LFO, not a free one's.");
    Param l2Division = choice("LFO 2 Division",
                              {"1/32", "1/16", "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2",
                               "1 Bar", "2 Bars", "4 Bars"},
                              6)
                           .keys({"n1_32", "n1_16", "n1_8t", "n1_8", "n1_8d", "n1_4t", "n1_4",
                                  "n1_4d", "n1_2", "bar1", "bar2", "bar4"})
                           .withKey("lfo2.division")
                           .in("LFO 2")
                           .activeWhen(l2Sync)
                           .help("How long a cycle lasts, in notes of the song's tempo.");
    Param l2Mode = choice("LFO 2 Mode", {"Note", "Free"}, 0)
                       .withKey("lfo2.mode")
                       .in("LFO 2")
                       .help(
                           "Note: each note's own, from its start. Free: one for every voice, "
                           "running on (synced, it follows the song's position).");
    Param l2Phase =
        degrees("LFO 2 Phase", 0, 360, 0)
            .withKey("lfo2.phase")
            .in("LFO 2")
            .help("Where in its cycle it starts (Note), or how far it is moved (Free).");
    Param l2Delay = milliseconds("LFO 2 Delay", 0, 10000, 0)
                        .withKey("lfo2.delay")
                        .in("LFO 2")
                        .help("How long it takes to fade in after a note starts: a late vibrato.");

    Param m1 = percent("Macro 1", 0)
                   .withKey("macro1")
                   .in("Macros")
                   .help("A knob for routes to follow (source macro1): one move, many changes.");
    Param m2 = percent("Macro 2", 0)
                   .withKey("macro2")
                   .in("Macros")
                   .help("A knob for routes to follow (source macro2): one move, many changes.");
    Param m3 = percent("Macro 3", 0)
                   .withKey("macro3")
                   .in("Macros")
                   .help("A knob for routes to follow (source macro3): one move, many changes.");
    Param m4 = percent("Macro 4", 0)
                   .withKey("macro4")
                   .in("Macros")
                   .help("A knob for routes to follow (source macro4): one move, many changes.");

    // --- Sources: what the routes may name ------------------------------------------------

    Source ampSource = voiceSource("Amp").unipolar().withKey("amp").help("The amp envelope.");
    Source env2Source =
        voiceSource("Env 2").unipolar().withKey("env2").help("The mod envelope, Env 2.");
    Source lfo1 = voiceSource("LFO 1").help("LFO 1, -1 to 1.");
    Source lfo2 = voiceSource("LFO 2").help("LFO 2, -1 to 1.");
    Source macro1 = source("Macro 1").unipolar().help("The Macro 1 knob.");
    Source macro2 = source("Macro 2").unipolar().help("The Macro 2 knob.");
    Source macro3 = source("Macro 3").unipolar().help("The Macro 3 knob.");
    Source macro4 = source("Macro 4").unipolar().help("The Macro 4 knob.");

    Param output =
        decibels("Output", -36, 12, 0)
            .smoothed(20)
            .in("Amp")
            .help("The level out. A gentle limit above -1 dBFS keeps chords from clipping.");

    // --- What the editor shows, sent by the audio code ---------------------------------------

    Shared<double> outLeft;  // the output's peak, dB, falling back
    Shared<double> outRight;
    Shared<int> soundingNow;            // voices playing
    Shared<int> fileFrames;             // the file's length, 0 for none
    Shared<int> fileChannels;           // its channels, 1 or 2
    SharedArray<float, 2048> overview;  // each channel's peak over kColumns columns
    Shared<int> regionA;                // the region, in file frames
    Shared<int> regionB;
    Shared<int> loopA;  // the loop as it plays (snapped), and its crossfade
    Shared<int> loopB;
    Shared<double> fadeShown;
    SharedArray<int, 65> sliceShown;  // the slices' starts, and how many
    Shared<int> slicesShown;
    SharedArray<double, 32> heads;  // each sounding voice's file frame; -1 none
    SharedArray<float, 512> pane;   // the close-ups: kPane points each
    Shared<int> paneKind;  // 0 start and end, 3 a slice; +10 the loop's seam; +100 reversed
    Shared<int> paneSlice;
    SharedArray<double, 2> envShown;  // the newest note's amp and Env 2: their level,
    SharedArray<int, 2> stageShown;   // and stage (0 idle, 1 attack ... 4 release)
    SharedArray<double, 2> lfoShown;  // each LFO's place in its two drawn cycles (0..1); -1: none
    Shared<int> newestKey;            // the newest note's key, -1 none

    // --- State ------------------------------------------------------------------------------

    Array<Voice, kVoices> voice;
    int factor = 2;
    double rate = 96000.0;  // the voices' (oversampled) rate
    Buffer<double> mixL;
    Buffer<double> mixR;
    Array<Oversampler, 2> over;
    int newest = -1;     // the voice of the latest note
    double peakL = 0.0;  // the output's peaks, falling back
    double peakR = 0.0;
    double restL = 0.0;  // what stolen voices were playing, fading out
    double restR = 0.0;
    double restFade = 0.99;
    int notes = 0;
    int quiet = 0;
    int tail = 0;  // the tail last reported, in samples
    // Free LFO phase anchors and whole frames since the last rate change;
    // a playing song's anchors (see Marram).
    Array<double, 2> freePhase;
    Array<int, 2> freeCycle;
    Array<double, 2> freeFrames;
    Array<double, 2> freeRate;
    Array<bool, 2> syncActive;
    Array<double, 2> syncBeats;
    Array<double, 2> syncPhase;
    Array<double, 2> syncFrames;
    Array<double, 2> syncTempo;
    Array<double, 2> syncDivision;
    double tempo = 120.0;
    double beats = 0.0;
    bool playing = false;

    // The file as measured, and the places worked out from it and the
    // settings: redone when either changes.
    int frames = 0;
    int channels = 0;
    double mark = -1.0;   // the file's fingerprint
    double normal = 1.0;  // Normalize's gain
    int s0 = 0;           // the region
    int s1 = 0;
    int ls = 0;  // the loop, snapped
    int le = 0;
    Array<int, 65> slices;  // their starts, then the region's end
    int sliceN = 0;
    Array<double, 4096> hopLevel;
    // What the places were worked out for.
    double wasStart = -1.0, wasEnd = -1.0, wasLoopStart = -1.0, wasLoopEnd = -1.0;
    double wasSensitivity = -1.0;
    int wasBy = -1, wasCount = -1, wasSnap = -1;

    // --- Hooks ------------------------------------------------------------------------------

    void prepare() {
        factor = oversampling.index() == 0 ? 1 : oversampling.index() == 1 ? 2 : 4;
        rate = sampleRate() * factor;
        for (int c = 0; c < 2; ++c) {
            over[c].prepare(factor);
            over[c].clear();
        }
        // The voices are made at the oversampled rate and only come down, so the
        // delay is the down filters' alone, not latency()'s way up and down: 1.4
        // samples at 2x and 1.8 at 4x (the suite's Oversampler test measures it).
        setLatency(factor == 1 ? 0 : factor == 2 ? 1 : 2);
        newest = -1;
        peakL = 0.0;
        peakR = 0.0;
        mixL.resize(maxBlockSize() * 4);
        mixR.resize(maxBlockSize() * 4);
        for (Voice& s : voice) silence(s);
        restL = 0.0;
        restR = 0.0;
        restFade = exp(-1.0 / (0.003 * rate));
        notes = 0;
        quiet = 0;
        tail = 0;
        reportTail(false);
        freePhase.fill(0.0);
        freeCycle.fill(0);
        freeFrames.fill(0.0);
        freeRate.fill(0.0);
        syncActive.fill(false);
        syncBeats.fill(0.0);
        syncPhase.fill(0.0);
        syncFrames.fill(0.0);
        syncTempo.fill(120.0);
        syncDivision.fill(1.0);
        // The file arrives after this, at the start of a block: measured then.
        frames = 0;
        channels = 0;
        mark = -1.0;
        wasStart = -1.0;
    }

    void update() {
        setVoiceMode(voiceMode.index() == 0   ? VoiceMode::Poly
                     : voiceMode.index() == 1 ? VoiceMode::Mono
                                              : VoiceMode::Legato);
        setGlide(glide);
        setBendRange(bendRange);
        setVoiceLimit(polyphony.index());
    }

    // --- The file and its places ------------------------------------------------------------

    // Enough of the file to tell one from another: its length, channels, and
    // its level over the whole and over its first half.
    double fingerprint() {
        if (!sample.loaded()) return 0.0;
        const int n = sample.frames();
        double m = n * 3.0 + sample.channels();
        for (int c = 0; c < min(sample.channels(), 2); ++c)
            m += sample.rms(c, 0, n) * 1e3 * (c + 1) + sample.rms(c, 0, n / 2) * 7e3;
        return m;
    }

    // At a block's start: a new file is measured and drawn, and the places
    // (the region, the loop, the slices) worked out again when they move.
    void places() {
        const double m = fingerprint();
        const bool fresh = m != mark;
        if (fresh) {
            mark = m;
            frames = sample.loaded() ? sample.frames() : 0;
            channels = sample.loaded() ? min(sample.channels(), 2) : 0;
            const double loudest = frames > 0 ? peakOf(sample, 0, frames) : 0.0;
            normal = loudest > 1e-6 ? 1.0 / loudest : 1.0;
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < kColumns; ++k) {
                    const int a = frames * k / kColumns,
                              b = max(a + 1, frames * (k + 1) / kColumns);
                    overview[c * kColumns + k] =
                        frames > 0 ? sample.peak(min(c, channels - 1), a, b) : 0.0;
                }
            fileFrames.set(frames);
            fileChannels.set(max(channels, 1));
            // What the voices were playing is gone: they fade out.
            for (int v = 0; v < voiceCount(); ++v)
                if (voice[v].sounding) {
                    restL += voice[v].lastL;
                    restR += voice[v].lastR;
                    voice[v].done = true;
                }
        }
        const bool moved = fresh || start.value() != wasStart || end.value() != wasEnd ||
                           loopStart.value() != wasLoopStart || loopEnd.value() != wasLoopEnd ||
                           snap.index() != wasSnap || sliceBy.index() != wasBy ||
                           sliceCount.index() != wasCount || sensitivity.value() != wasSensitivity;
        if (!moved) return;
        wasStart = start.value();
        wasEnd = end.value();
        wasLoopStart = loopStart.value();
        wasLoopEnd = loopEnd.value();
        wasSnap = snap.index();
        wasBy = sliceBy.index();
        wasCount = sliceCount.index();
        wasSensitivity = sensitivity.value();
        const int a = toInt(min(start.value(), end.value()) * frames);
        const int b = toInt(max(start.value(), end.value()) * frames);
        s0 = clampInt(a, 0, max(frames - 1, 0));
        s1 = clampInt(max(b, s0 + 1), 0, frames);
        // The loop: inside the region, and on zero crossings when asked.
        const int reach = ceilToInt(0.005 * sampleRate());
        int x = clampInt(toInt(min(loopStart.value(), loopEnd.value()) * frames), s0, s1);
        int y = clampInt(toInt(max(loopStart.value(), loopEnd.value()) * frames), s0, s1);
        if (snap.on() && frames > 0) {
            x = clampInt(zeroNear(sample, x, reach), s0, s1);
            y = clampInt(zeroNear(sample, y, reach), s0, s1);
        }
        ls = x;
        le = max(y, x);
        fadeShown.set(min(crossfade * 0.001 * sampleRate(), (le - ls) * 0.5));
        // The slices.
        if (frames == 0) {
            sliceN = 0;
        } else if (sliceBy.index() == 1) {
            sliceN = sliceCount.index();
            for (int k = 0; k < sliceN; ++k) slices[k] = s0 + toInt((s1 - s0) * (k * 1.0 / sliceN));
        } else {
            sliceN = findHits(sample, s0, s1, sensitivity.value(), sampleRate(), slices, hopLevel,
                              kSlices);
        }
        slices[sliceN] = s1;
        regionA.set(s0);
        regionB.set(s1);
        loopA.set(ls);
        loopB.set(le);
        for (int k = 0; k <= sliceN; ++k) sliceShown[k] = slices[k];
        slicesShown.set(sliceN);
    }

    // --- Notes ------------------------------------------------------------------------------

    void startVoice(int v, bool retrigger) {
        // A note may come before the block's process(), with a new file.
        places();
        Voice& s = voice[v];
        ++notes;
        newest = v;
        s.velocity = voiceVelocity(v);
        const bool fresh = !s.sounding;
        if (fresh) {
            for (int c = 0; c < 2; ++c) s.filter[c].clear();
            s.amp.kill();
            s.mod.kill();
            s.ampL = 0.0;
            s.ampR = 0.0;
            s.ampLStep = 0.0;
            s.ampRStep = 0.0;
            s.ending = false;
            s.fresh = true;
            s.tickLeft = 0;
        }
        if (retrigger || fresh) {
            s.amp.gate();
            s.mod.gate();
            s.lfoPhase[0] = l1Phase / 360.0;
            s.lfoPhase[1] = l2Phase / 360.0;
            s.lfoCycle.fill(0);
            s.lfoSeed = imod(notes * 7919 + 17, 1000000);
            s.age = 0.0;
            aim(v);
        }
        s.sounding = true;
        s.done = frames == 0 || s.span.length() <= 0;
    }

    // Where voice v reads, for the note it starts: the region, a slice, and
    // in Classic the loop; and from where.
    void aim(int v) {
        Voice& s = voice[v];
        const int m = mode.index();
        s.span.reversed = reverse.on();
        s.span.loop = kLoopOff;
        s.slice = -1;
        s.gated = true;
        if (m == kSlice) {
            const int k = voiceKey(v) - firstKey.index();
            s.slice = k;
            if (k < 0 || k >= sliceN) {
                s.span.a = 0;
                s.span.b = 0;
                return;
            }
            const int play = slicePlay.index();
            s.span.a = slices[k];
            s.span.b = play == kThru ? s1 : slices[k + 1];
            s.gated = play != kTrigger;
        } else {
            // A route moves where a note starts (the region's start, or
            // played backwards its end), within the region.
            const int moved = toInt(start.forVoice(v) * frames) - toInt(start * frames);
            s.span.a = s0;
            s.span.b = s1;
            if (s.span.reversed)
                s.span.b = clampInt(s1 - max(moved, 0), s0 + 1, s1);
            else
                s.span.a = clampInt(s0 + moved, s0, s1 - 1);
            s.gated = m == kClassic;
            if (m == kClassic && loopMode.index() != kLoopOff)
                s.span.loopOn(loopMode.index(), ls, le, crossfade * 0.001 * sampleRate());
        }
        s.q = 0.0;
        s.dir = 1.0;
        s.looping = s.span.loop != kLoopOff;
        s.narrow = channels < 2;
    }

    void releaseVoice(int v) {
        Voice& s = voice[v];
        if (s.gated) {
            s.amp.release();
            s.mod.release();
        }
        if (playOn.on()) s.looping = false;
    }

    // Stolen or choked: it stops now, and what it was playing fades out over
    // a few milliseconds instead of stepping to silence.
    void stopVoice(int v) {
        Voice& s = voice[v];
        if (s.sounding) {
            restL += s.lastL;
            restR += s.lastR;
        }
        silence(s);
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

    double ms(double t) { return t * 0.001 * sampleRate(); }

    void control(int v, int i) {
        Voice& s = voice[v];
        const double steps = kTick * factor;
        const bool snapNow = s.fresh;
        s.fresh = false;

        const double ampEnv =
            s.amp.advance(kTick, ms(ampAttack.forVoice(v)), ms(ampDecay.forVoice(v)), ampSustain,
                          ms(ampRelease.forVoice(v)));
        const double modEnv =
            s.mod.advance(kTick, ms(envAttack.forVoice(v)), ms(envDecay.forVoice(v)), envSustain,
                          ms(envRelease.forVoice(v)));
        if (s.ending && s.amp.stage == 0 && !snapNow) {
            s.done = true;
            return;
        }
        ampSource.set(v, ampEnv);
        env2Source.set(v, modEnv);
        lfo1.set(v, lfo(s, v, 0, i, l1Shape, l1Sync, l1Rate, l1Division, l1Mode, l1Phase, l1Delay));
        lfo2.set(v, lfo(s, v, 1, i, l2Shape, l2Sync, l2Rate, l2Division, l2Mode, l2Phase, l2Delay));
        s.age += kTick;

        // The speed: the key from the root (or the root's own pitch), with
        // the bend and glide either way, transposed, tuned and swept by Env 2.
        const double pitch = voicePitch(v, i);
        const bool tracked = mode.index() != kSlice && keytrack.on();
        const double semis = (tracked ? pitch - root.index() : pitch - voiceKey(v)) +
                             transpose.index() + tune.forVoice(v) / 100.0 + envPitch * modEnv;
        const double target = clamp(exp2(semis / 12.0), 1.0 / 256.0, 32.0) / factor;
        if (snapNow) {
            s.step = target;
            s.stepStep = 0.0;
        } else {
            s.stepStep = (target - s.step) / steps;
        }

        // The filter.
        if (fType.index() != kOff) {
            const double keyOct = (pitch - 60.0) / 12.0;
            const double octs = fKeytrack * keyOct + fEnv * modEnv + fVelocity * s.velocity;
            const double hz =
                clamp(cutoff.forVoice(v) * exp2(octs), 10.0, min(22000.0, 0.45 * rate));
            const double g = tan(pi * hz / rate);
            const double r = clamp(resonance.forVoice(v), 0.0, 1.0);
            const double k =
                fType.index() == kLadder ? 4.0 * r : max(0.005, 2.0 * (1.0 - r) * (1.0 - r));
            if (snapNow) {
                s.g = g;
                s.gStep = 0.0;
                s.k = k;
                s.kStep = 0.0;
            } else {
                s.gStep = (g - s.g) / steps;
                s.kStep = (k - s.k) / steps;
            }
            s.drive = dbToGain(fDrive.forVoice(v));
        }

        // Amp: the envelope, velocity, the file's gain, its level, the note's
        // own volume and pan.
        const double velDb = -(1.0 - s.velocity) * 40.0 * ampVelocity;
        const double level = ampEnv * dbToGain(velDb + ampLevel.forVoice(v) + gain) *
                             (normalize.on() ? normal : 1.0) * voiceVolume(v);
        const double p = clamp(ampPan.forVoice(v) + (voicePan(v) - 0.5) * 2.0, -1.0, 1.0);
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

    // LFO n's value for voice v at frame i of the block, faded in over its
    // delay from the note's start. A note's own moves on by a tick.
    double lfo(Voice& s, int v, int n, int i, const Param& shape, const Param& sync,
               const Param& rateParam, const Param& division, const Param& lfoMode,
               const Param& phase, const Param& delay) {
        const double fade = delay > 0.0 ? min(1.0, s.age / ms(delay)) : 1.0;
        const double beatHz = tempo / 60.0 / divisionBeats(division.index());
        double t = 0.0;
        int cycle = 0;
        int seed = n + 1;
        if (lfoMode.index() == 1) {
            double total = 0.0;
            if (sync.on() && playing) {
                total = syncedPhase(n, i) + phase / 360.0;
            } else {
                total =
                    freePhase[n] + freeRate[n] * (freeFrames[n] + i) / sampleRate() + phase / 360.0;
                cycle = freeCycle[n];
            }
            const double whole = floor(total);
            t = total - whole;
            cycle += toInt(whole);
        } else {
            t = s.lfoPhase[n];
            cycle = s.lfoCycle[n];
            seed = s.lfoSeed + n * 7919;
            const double hz = sync.on() ? beatHz : rateParam.forVoice(v);
            const double next = t + hz * kTick / sampleRate();
            const double whole = floor(next);
            s.lfoPhase[n] = next - whole;
            s.lfoCycle[n] = cycle + toInt(whole);
        }
        return lfoWave(shape.index(), t, cycle, seed) * fade;
    }

    void freeOn(int n, const Param& sync, const Param& rateParam, const Param& division) {
        const double hz =
            sync.on() ? tempo / 60.0 / divisionBeats(division.index()) : rateParam.value();
        if (hz == freeRate[n]) return;
        const double total = freePhase[n] + freeRate[n] * freeFrames[n] / sampleRate();
        const double whole = floor(total);
        freePhase[n] = total - whole;
        freeCycle[n] += toInt(whole);
        freeFrames[n] = 0.0;
        freeRate[n] = hz;
    }

    double syncedPhase(int n, int i) {
        return syncPhase[n] +
               (syncFrames[n] + i) * syncTempo[n] / 60.0 / sampleRate() / syncDivision[n];
    }

    void syncOn(int n, const Param& sync, const Param& division) {
        if (!playing || !sync.on()) {
            syncActive[n] = false;
            return;
        }
        const double span = divisionBeats(division.index());
        const double predicted = syncBeats[n] + syncFrames[n] * syncTempo[n] / 60.0 / sampleRate();
        const bool jump = !syncActive[n] || abs(beats - predicted) > 1e-9;
        if (jump || span != syncDivision[n] || tempo != syncTempo[n]) {
            const double anchorBeat = jump ? beats : predicted;
            syncPhase[n] = jump || span != syncDivision[n] ? anchorBeat / span : syncedPhase(n, 0);
            syncBeats[n] = anchorBeat;
            syncTempo[n] = tempo;
            syncDivision[n] = span;
            syncFrames[n] = 0.0;
        }
        syncActive[n] = true;
    }

    // --- Playing ------------------------------------------------------------------------------

    void play(int v, int count) {
        Voice& s = voice[v];
        const double endFade = 0.001 * sampleRate();  // a region's last millisecond fades
        int i = 0;
        while (i < count) {
            if (s.tickLeft == 0) {
                control(v, i);
                s.tickLeft = kTick;
                if (s.done) return;
            }
            const int run = min(s.tickLeft, count - i);
            render(s, sample, i, run, factor, endFade, fType.index(), slope.index() == 1,
                   fMode.index(), mixL, mixR, 0);
            s.tickLeft -= run;
            i += run;
            if (s.done) return;
        }
    }

    void process(Block& io) {
        tempo = io.transport.tempo;
        beats = io.transport.beats;
        playing = io.transport.playing;
        places();
        freeOn(0, l1Sync, l1Rate, l1Division);
        freeOn(1, l2Sync, l2Rate, l2Division);
        syncOn(0, l1Sync, l1Division);
        syncOn(1, l2Sync, l2Division);
        macro1.set(m1);
        macro2.set(m2);
        macro3.set(m3);
        macro4.set(m4);
        const int n = io.frames * factor;
        for (int k = 0; k < n; ++k) {
            mixL[k] = 0.0;
            mixR[k] = 0.0;
        }
        for (int v = 0; v < voiceCount(); ++v) {
            Voice& s = voice[v];
            if (!s.sounding || !voicePlaying(v)) continue;
            if (!s.done) play(v, io.frames);
            if (s.done) {
                silence(s);
                voiceFinished(v);
            }
        }
        for (int k = 0; k < n; ++k) {
            if (restL == 0.0 && restR == 0.0) break;
            mixL[k] += restL;
            mixR[k] += restR;
            restL *= restFade;
            restR *= restFade;
            if (abs(restL) + abs(restR) < 1e-9) {
                restL = 0.0;
                restR = 0.0;
            }
        }
        for (int i = 0; i < io.frames; ++i) {
            // Long silent: nothing to bring down, and the downsampler has
            // long since rung out.
            bool any = false;
            for (int k = 0; k < factor; ++k)
                if (mixL[i * factor + k] != 0.0 || mixR[i * factor + k] != 0.0) any = true;
            if (any)
                quiet = 0;
            else if (quiet < 4096)
                ++quiet;
            if (quiet == 4095) {
                over[0].clear();
                over[1].clear();
            }
            double l = 0.0;
            double r = 0.0;
            if (quiet < 4096) {
                over[0].up(0.0);
                over[1].up(0.0);
                for (int k = 0; k < factor; ++k) {
                    over[0].set(k, mixL[i * factor + k]);
                    over[1].set(k, mixR[i * factor + k]);
                }
                l = over[0].down();
                r = over[1].down();
            }
            const double g = kHeadroom * dbToGain(output.at(i));
            if (io.channels == 1) {
                io[0][i] = safety((l + r) * 0.5 * g);
            } else {
                io[0][i] = safety(l * g);
                io[1][i] = safety(r * g);
            }
            peakL = max(abs(io[0][i]), peakL * 0.99985);
            peakR = max(abs(io[io.channels - 1][i]), peakR * 0.99985);
        }
        bool sounding = false;
        for (int v = 0; v < voiceCount(); ++v)
            if (voice[v].sounding && voicePlaying(v)) sounding = true;
        reportTail(sounding);
        for (int k = 0; k < 2; ++k) {
            freeFrames[k] += io.frames;
            if (syncActive[k]) syncFrames[k] += io.frames;
        }
        show();
    }

    // How long the output goes on once the keys are up: the longest amp
    // release (a route may lengthen a voice's), or what is left of a note
    // that plays on whatever its key does; and 20 ms for a stolen voice's
    // fade and the downsampler. Raised whenever a voice needs more, lowered
    // only with no voice sounding.
    void reportTail(bool sounding) {
        double longest = ampRelease;
        for (int v = 0; v < voiceCount(); ++v) {
            const Voice& s = voice[v];
            if (!s.sounding || !voicePlaying(v)) continue;
            longest = max(longest, ampRelease.forVoice(v));
            if ((!s.gated || !s.looping) && s.step > 0.0)
                longest = max(longest,
                              (s.span.length() - s.q) / (s.step * factor) / sampleRate() * 1000.0);
        }
        const int want = ceilToInt((min(longest, 600000.0) + 20.0) * 0.001 * sampleRate());
        if (want > tail || (!sounding && want != tail)) {
            tail = want;
            setTail(tail);
        }
    }

    // What the editor shows: the level, the voices, where each plays, the
    // newest note's envelopes and LFOs, and a close-up.
    void show() {
        outLeft.set(gainToDb(peakL));
        outRight.set(gainToDb(peakR));
        int count = 0;
        for (int v = 0; v < kVoices; ++v) {
            const Voice& s = voice[v];
            const bool on = v < voiceCount() && s.sounding && voicePlaying(v) && !s.done;
            if (on) ++count;
            heads[v] = on ? s.span.frame(s.q) : -1.0;
        }
        soundingNow.set(count);
        const bool on = newest >= 0 && voice[newest].sounding && voicePlaying(newest);
        envShown[0] = on ? voice[newest].amp.level : 0.0;
        envShown[1] = on ? voice[newest].mod.level : 0.0;
        stageShown[0] = on ? voice[newest].amp.stage : 0;
        stageShown[1] = on ? voice[newest].mod.stage : 0;
        // Each LFO's place in the two cycles the editor draws from its start
        // phase, so its dot crosses both: a free one from its anchor and the
        // frames since, a note's from the newest note's start.
        for (int k = 0; k < 2; ++k) {
            const bool free = (k == 0 ? l1Mode.index() : l2Mode.index()) == 1;
            const double phase = (k == 0 ? l1Phase.value() : l2Phase.value()) / 360.0;
            lfoShown[k] =
                free ? wrap01((freeCycle[k] + freePhase[k] +
                               freeRate[k] * freeFrames[k] / sampleRate()) *
                              0.5)
                : on ? wrap01((voice[newest].lfoCycle[k] + voice[newest].lfoPhase[k] - phase) * 0.5)
                     : -1.0;
        }
        newestKey.set(on ? voiceKey(newest) : -1);
        closeUp(on);
    }

    // The close-ups, 2.5 ms each side: the start (or in Slice the newest
    // note's slice), and the loop's seam as it plays (or in One-Shot the end).
    // Each runs as it plays: reversed, from the region's end backwards.
    void closeUp(bool on) {
        const int m = mode.index();
        const double reach = kPaneMs * 0.001 * sampleRate();
        const bool back = reverse.on();
        const double way = back ? -1.0 : 1.0;
        int kind = 0;
        double at = back ? s1 : s0;
        if (m == kSlice) {
            kind = 3;
            int k = on ? voice[newest].slice : 0;
            if (k < 0 || k >= sliceN) k = 0;
            paneSlice.set(k);
            const double end = slicePlay.index() == kThru ? s1 : slices[k + 1];
            at = sliceN == 0 ? s0 : back ? end : slices[k];
        }
        paneKind.set(kind + (back ? 100 : 0));
        for (int j = 0; j < kPane; ++j) {
            const double x = at + way * (2.0 * j / (kPane - 1) - 1.0) * reach;
            pane[j] = frames > 0 && x >= 0.0 && x < frames ? sample.read(0, x) : 0.0;
        }
        // The second: the seam, as the loop's end runs into its start.
        if (m == kClassic && loopMode.index() != kLoopOff && le > ls) {
            Span sp;
            sp.a = s0;
            sp.b = s1;
            sp.reversed = reverse.on();
            sp.loopOn(loopMode.index(), ls, le, crossfade * 0.001 * sampleRate());
            for (int j = 0; j < kPane; ++j) {
                double q = sp.l1 + (2.0 * j / (kPane - 1) - 1.0) * reach;
                if (sp.loop == kForward && q >= sp.l1) q -= sp.l1 - sp.l0;
                if (sp.loop == kPingPong && q >= sp.l1) q = 2.0 * sp.l1 - q;
                pane[kPane + j] = frames > 0 ? hear(sample, sp, 0, q, true) : 0.0;
            }
            paneKind.set(kind + 10 + (back ? 100 : 0));
        } else {
            const double e = back ? s0 : s1;
            for (int j = 0; j < kPane; ++j) {
                const double x = e + way * (2.0 * j / (kPane - 1) - 1.0) * reach;
                pane[kPane + j] = frames > 0 && x >= 0.0 && x < frames ? sample.read(0, x) : 0.0;
            }
        }
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
    // A header with the play mode, the macros, the voices and the level; the
    // sample: its file, the waveform with the region, the loop or the slices
    // and where each voice plays, the controls the mode uses, and close-ups
    // of the places that matter to the sample; pitch and voice, and the
    // filter; the envelopes, the LFOs and the routes (e.routes); a keyboard.
    // Every control is drawn here and answers in pointer(), as Marram's do: a
    // dial drags up and down (Shift finely), turns with the wheel and goes
    // back to its default on a double click; a button sets its choice; the
    // waveform's flags, the loop's ends, the filter's curve and the
    // envelope's handles drag as well.

    static constexpr double kW = 1240;
    static constexpr double kH = 940;
    static constexpr double smpX = 16;  // the panels
    static constexpr double smpY = 76;
    static constexpr double smpW = 1208;
    static constexpr double smpH = 364;
    static constexpr double pitX = 16;
    static constexpr double pitY = 452;
    static constexpr double pitW = 604;
    static constexpr double pitH = 196;
    static constexpr double filX = 632;
    static constexpr double filY = 452;
    static constexpr double filW = 592;
    static constexpr double filH = 196;
    static constexpr double envX = 16;
    static constexpr double envY = 660;
    static constexpr double envW = 397;
    static constexpr double envH = 196;
    static constexpr double lfoX = 421;
    static constexpr double lfoY = 660;
    static constexpr double lfoW = 397;
    static constexpr double lfoH = 196;
    static constexpr double modX = 826;
    static constexpr double modY = 660;
    static constexpr double modW = 398;
    static constexpr double modH = 196;
    static constexpr double keyY = 878;  // the keyboard
    static constexpr double keyH = 48;
    static constexpr double stripY = 862;  // over it, the slices' keys or the root's
    static constexpr int kLowKey = 24;
    static constexpr int kHighKey = 96;
    static constexpr double wavX = smpX + 16;  // the waveform
    static constexpr double wavY = smpY + 50;
    static constexpr double wavW = smpW - 32;
    static constexpr double wavH = 156;
    static constexpr double flagH = 16;               // the band the start and end flags hang from
    static constexpr double loopH = 14;               // the loop's bracket, under them
    static constexpr double rowY = wavY + wavH + 62;  // the mode's controls
    static constexpr double paneW = 214;              // the close-ups
    static constexpr double paneH = 92;
    static constexpr double paneY = wavY + wavH + 24;
    static constexpr double paneAX = smpX + smpW - 16 - 2 * paneW - 12;
    static constexpr double paneBX = smpX + smpW - 16 - paneW;
    static constexpr double curveX = filX + 16;  // the filter's response
    static constexpr double curveY = filY + 70;
    static constexpr double curveW = 250;
    static constexpr double curveH = 112;
    static constexpr double shapeX = envX + 16;  // the envelope
    static constexpr double shapeY = envY + 42;
    static constexpr double shapeW = envW - 32;
    static constexpr double shapeH = 76;
    static constexpr double cycleX = lfoX + 16;  // the LFO
    static constexpr double cycleY = lfoY + 76;
    static constexpr double cycleW = 210;
    static constexpr double cycleH = 104;

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
    static constexpr int kHazel = 0xC9A46A;  // the sample: hazel wood
    static constexpr int kShoot = 0xA9BC72;  // the loop and the slices: new shoots
    static constexpr int kBark = 0x8C7553;
    static constexpr int kSand = 0xDDBA80;      // the filter
    static constexpr int kSeaGlass = 0x8FB3AC;  // pitch and voice

    // The editor's own codes and the buttons' looks.
    static constexpr int eEnvTab = -1;
    static constexpr int eLfoTab = -2;
    static constexpr int bChoice = 0;  // lit while its parameter holds its value
    static constexpr int bToggle = 1;  // lit while its parameter is on
    static constexpr int bSmallTab = 3;
    static constexpr int bShape = 5;  // an LFO shape's icon

    int envTab = 0;
    int lfoTab = 0;
    double hoverX = -1.0, hoverY = -1.0;
    // 1 a dial, 2 the filter's curve, 3 the envelope, 4 a flag or a loop end,
    // 5 the whole loop
    int dragging = 0;
    int dragDial = -1;
    double dialPos = 0.0;
    int handle =
        -1;  // the envelope's: 0 attack, 1 decay and sustain, 2 release; the waveform's code
    double pressX = 0.0, pressY = 0.0, pressA = 0.0, pressB = 0.0;
    int hotDial = -1;
    int hotButton = -1;
    int hotHandle = -1;  // the waveform's handle under the pointer
    Array<int, 64> dialCode;
    Array<double, 64> dialX, dialY, dialR;
    Array<int, 64> dialColour;
    Array<bool, 64> dialLive;  // false: dimmed, as its setting does not apply
    int dials = 0;
    Array<int, 64> buttonCode, buttonValue, buttonStyle, buttonColour;
    Array<double, 64> buttonX, buttonY, buttonW, buttonH;
    Array<bool, 64> buttonLive;  // false: dimmed, as its setting does not apply
    int buttons = 0;

    void editor(Editor& e) {
        e.size(kW, kH);
        e.file(sample, smpX + 120, smpY + 11, 560, 28);
        e.routes(modX + 16, modY + 38, modW - 32, modH - 48);
        e.keyboard(kLowKey, kHighKey, smpX, keyY, smpW, keyH);
    }

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
    int choiceOf(int code) const { return roundToInt(readParam(code, 2)); }
    double valueOf(int code) const { return readParam(code, 1); }
    int lc(int l, int field) const { return 60 + 10 * l + field; }

    bool bipolar(int code) const {
        return code == 8 || code == 25 || code == 36 || code == 46 || code == 54;
    }

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

    // --- parameters by code -------------------------------------------------------------------
    //
    // Parameters cannot sit in an array, so the editor names each by a code:
    // the sample's 1-9, the loop's 10-15, the slices' 16-20, the voice's 21-26,
    // the filter's 30-38, the amp's 40-46, Env 2's 50-54, LFO l's 60 + 10 l +
    // field (in the order the struct declares them), the macros 80-83 and the
    // output 84.

    // Parameter `code`'s position on its dial (how 0), value in its unit
    // (1), or choice (2).
    double readParam(int code, int how) const {
        switch (code) {
            case 1:
                return readOn(mode, how);
            case 2:
                return readOn(start, how);
            case 3:
                return readOn(end, how);
            case 4:
                return readOn(reverse, how);
            case 5:
                return readOn(normalize, how);
            case 6:
                return readOn(gain, how);
            case 7:
                return readOn(root, how);
            case 8:
                return readOn(transpose, how);
            case 9:
                return readOn(keytrack, how);
            case 10:
                return readOn(loopMode, how);
            case 11:
                return readOn(loopStart, how);
            case 12:
                return readOn(loopEnd, how);
            case 13:
                return readOn(crossfade, how);
            case 14:
                return readOn(snap, how);
            case 15:
                return readOn(playOn, how);
            case 16:
                return readOn(sliceBy, how);
            case 17:
                return readOn(sensitivity, how);
            case 18:
                return readOn(sliceCount, how);
            case 19:
                return readOn(firstKey, how);
            case 20:
                return readOn(slicePlay, how);
            case 21:
                return readOn(voiceMode, how);
            case 22:
                return readOn(polyphony, how);
            case 23:
                return readOn(glide, how);
            case 24:
                return readOn(bendRange, how);
            case 25:
                return readOn(tune, how);
            case 26:
                return readOn(oversampling, how);
            case 30:
                return readOn(fType, how);
            case 31:
                return readOn(cutoff, how);
            case 32:
                return readOn(resonance, how);
            case 33:
                return readOn(slope, how);
            case 34:
                return readOn(fMode, how);
            case 35:
                return readOn(fKeytrack, how);
            case 36:
                return readOn(fEnv, how);
            case 37:
                return readOn(fVelocity, how);
            case 38:
                return readOn(fDrive, how);
            case 40:
                return readOn(ampAttack, how);
            case 41:
                return readOn(ampDecay, how);
            case 42:
                return readOn(ampSustain, how);
            case 43:
                return readOn(ampRelease, how);
            case 44:
                return readOn(ampVelocity, how);
            case 45:
                return readOn(ampLevel, how);
            case 46:
                return readOn(ampPan, how);
            case 50:
                return readOn(envAttack, how);
            case 51:
                return readOn(envDecay, how);
            case 52:
                return readOn(envSustain, how);
            case 53:
                return readOn(envRelease, how);
            case 54:
                return readOn(envPitch, how);
            case 60:
                return readOn(l1Shape, how);
            case 61:
                return readOn(l1Sync, how);
            case 62:
                return readOn(l1Rate, how);
            case 63:
                return readOn(l1Division, how);
            case 64:
                return readOn(l1Mode, how);
            case 65:
                return readOn(l1Phase, how);
            case 66:
                return readOn(l1Delay, how);
            case 70:
                return readOn(l2Shape, how);
            case 71:
                return readOn(l2Sync, how);
            case 72:
                return readOn(l2Rate, how);
            case 73:
                return readOn(l2Division, how);
            case 74:
                return readOn(l2Mode, how);
            case 75:
                return readOn(l2Phase, how);
            case 76:
                return readOn(l2Delay, how);
            case 80:
                return readOn(m1, how);
            case 81:
                return readOn(m2, how);
            case 82:
                return readOn(m3, how);
            case 83:
                return readOn(m4, how);
            case 84:
                return readOn(output, how);
        }
        return 0.0;
    }

    // Parameter `code` set as a user would: to a value in its unit (how 0),
    // a position on its dial (1), or its default (2).
    void editParam(int code, int how, double v) {
        switch (code) {
            case 1:
                editOn(mode, how, v);
                return;
            case 2:
                editOn(start, how, v);
                return;
            case 3:
                editOn(end, how, v);
                return;
            case 4:
                editOn(reverse, how, v);
                return;
            case 5:
                editOn(normalize, how, v);
                return;
            case 6:
                editOn(gain, how, v);
                return;
            case 7:
                editOn(root, how, v);
                return;
            case 8:
                editOn(transpose, how, v);
                return;
            case 9:
                editOn(keytrack, how, v);
                return;
            case 10:
                editOn(loopMode, how, v);
                return;
            case 11:
                editOn(loopStart, how, v);
                return;
            case 12:
                editOn(loopEnd, how, v);
                return;
            case 13:
                editOn(crossfade, how, v);
                return;
            case 14:
                editOn(snap, how, v);
                return;
            case 15:
                editOn(playOn, how, v);
                return;
            case 16:
                editOn(sliceBy, how, v);
                return;
            case 17:
                editOn(sensitivity, how, v);
                return;
            case 18:
                editOn(sliceCount, how, v);
                return;
            case 19:
                editOn(firstKey, how, v);
                return;
            case 20:
                editOn(slicePlay, how, v);
                return;
            case 21:
                editOn(voiceMode, how, v);
                return;
            case 22:
                editOn(polyphony, how, v);
                return;
            case 23:
                editOn(glide, how, v);
                return;
            case 24:
                editOn(bendRange, how, v);
                return;
            case 25:
                editOn(tune, how, v);
                return;
            case 26:
                editOn(oversampling, how, v);
                return;
            case 30:
                editOn(fType, how, v);
                return;
            case 31:
                editOn(cutoff, how, v);
                return;
            case 32:
                editOn(resonance, how, v);
                return;
            case 33:
                editOn(slope, how, v);
                return;
            case 34:
                editOn(fMode, how, v);
                return;
            case 35:
                editOn(fKeytrack, how, v);
                return;
            case 36:
                editOn(fEnv, how, v);
                return;
            case 37:
                editOn(fVelocity, how, v);
                return;
            case 38:
                editOn(fDrive, how, v);
                return;
            case 40:
                editOn(ampAttack, how, v);
                return;
            case 41:
                editOn(ampDecay, how, v);
                return;
            case 42:
                editOn(ampSustain, how, v);
                return;
            case 43:
                editOn(ampRelease, how, v);
                return;
            case 44:
                editOn(ampVelocity, how, v);
                return;
            case 45:
                editOn(ampLevel, how, v);
                return;
            case 46:
                editOn(ampPan, how, v);
                return;
            case 50:
                editOn(envAttack, how, v);
                return;
            case 51:
                editOn(envDecay, how, v);
                return;
            case 52:
                editOn(envSustain, how, v);
                return;
            case 53:
                editOn(envRelease, how, v);
                return;
            case 54:
                editOn(envPitch, how, v);
                return;
            case 60:
                editOn(l1Shape, how, v);
                return;
            case 61:
                editOn(l1Sync, how, v);
                return;
            case 62:
                editOn(l1Rate, how, v);
                return;
            case 63:
                editOn(l1Division, how, v);
                return;
            case 64:
                editOn(l1Mode, how, v);
                return;
            case 65:
                editOn(l1Phase, how, v);
                return;
            case 66:
                editOn(l1Delay, how, v);
                return;
            case 70:
                editOn(l2Shape, how, v);
                return;
            case 71:
                editOn(l2Sync, how, v);
                return;
            case 72:
                editOn(l2Rate, how, v);
                return;
            case 73:
                editOn(l2Division, how, v);
                return;
            case 74:
                editOn(l2Mode, how, v);
                return;
            case 75:
                editOn(l2Phase, how, v);
                return;
            case 76:
                editOn(l2Delay, how, v);
                return;
            case 80:
                editOn(m1, how, v);
                return;
            case 81:
                editOn(m2, how, v);
                return;
            case 82:
                editOn(m3, how, v);
                return;
            case 83:
                editOn(m4, how, v);
                return;
            case 84:
                editOn(output, how, v);
                return;
        }
    }

    // Parameter `code`'s value as hosts show it (the keys as note names).
    void fillParam(Canvas& g, int code, double x, double y) const {
        if (code == 7 || code == 19) {
            fillNote(g, roundToInt(readParam(code, 1)), x, y);
            return;
        }
        switch (code) {
            case 1:
                g.fillText(mode.text(), x, y);
                return;
            case 2:
                g.fillText(start.text(), x, y);
                return;
            case 3:
                g.fillText(end.text(), x, y);
                return;
            case 4:
                g.fillText(reverse.text(), x, y);
                return;
            case 5:
                g.fillText(normalize.text(), x, y);
                return;
            case 6:
                g.fillText(gain.text(), x, y);
                return;
            case 8:
                g.fillText(transpose.text(), x, y);
                return;
            case 9:
                g.fillText(keytrack.text(), x, y);
                return;
            case 10:
                g.fillText(loopMode.text(), x, y);
                return;
            case 11:
                g.fillText(loopStart.text(), x, y);
                return;
            case 12:
                g.fillText(loopEnd.text(), x, y);
                return;
            case 13:
                g.fillText(crossfade.text(), x, y);
                return;
            case 14:
                g.fillText(snap.text(), x, y);
                return;
            case 15:
                g.fillText(playOn.text(), x, y);
                return;
            case 16:
                g.fillText(sliceBy.text(), x, y);
                return;
            case 17:
                g.fillText(sensitivity.text(), x, y);
                return;
            case 18:
                g.fillText(sliceCount.text(), x, y);
                return;
            case 20:
                g.fillText(slicePlay.text(), x, y);
                return;
            case 21:
                g.fillText(voiceMode.text(), x, y);
                return;
            case 22:
                g.fillText(polyphony.text(), x, y);
                return;
            case 23:
                g.fillText(glide.text(), x, y);
                return;
            case 24:
                g.fillText(bendRange.text(), x, y);
                return;
            case 25:
                g.fillText(tune.text(), x, y);
                return;
            case 26:
                g.fillText(oversampling.text(), x, y);
                return;
            case 30:
                g.fillText(fType.text(), x, y);
                return;
            case 31:
                g.fillText(cutoff.text(), x, y);
                return;
            case 32:
                g.fillText(resonance.text(), x, y);
                return;
            case 33:
                g.fillText(slope.text(), x, y);
                return;
            case 34:
                g.fillText(fMode.text(), x, y);
                return;
            case 35:
                g.fillText(fKeytrack.text(), x, y);
                return;
            case 36:
                g.fillText(fEnv.text(), x, y);
                return;
            case 37:
                g.fillText(fVelocity.text(), x, y);
                return;
            case 38:
                g.fillText(fDrive.text(), x, y);
                return;
            case 40:
                g.fillText(ampAttack.text(), x, y);
                return;
            case 41:
                g.fillText(ampDecay.text(), x, y);
                return;
            case 42:
                g.fillText(ampSustain.text(), x, y);
                return;
            case 43:
                g.fillText(ampRelease.text(), x, y);
                return;
            case 44:
                g.fillText(ampVelocity.text(), x, y);
                return;
            case 45:
                g.fillText(ampLevel.text(), x, y);
                return;
            case 46:
                g.fillText(ampPan.text(), x, y);
                return;
            case 50:
                g.fillText(envAttack.text(), x, y);
                return;
            case 51:
                g.fillText(envDecay.text(), x, y);
                return;
            case 52:
                g.fillText(envSustain.text(), x, y);
                return;
            case 53:
                g.fillText(envRelease.text(), x, y);
                return;
            case 54:
                g.fillText(envPitch.text(), x, y);
                return;
            case 60:
                g.fillText(l1Shape.text(), x, y);
                return;
            case 61:
                g.fillText(l1Sync.text(), x, y);
                return;
            case 62:
                g.fillText(l1Rate.text(), x, y);
                return;
            case 63:
                g.fillText(l1Division.text(), x, y);
                return;
            case 64:
                g.fillText(l1Mode.text(), x, y);
                return;
            case 65:
                g.fillText(l1Phase.text(), x, y);
                return;
            case 66:
                g.fillText(l1Delay.text(), x, y);
                return;
            case 70:
                g.fillText(l2Shape.text(), x, y);
                return;
            case 71:
                g.fillText(l2Sync.text(), x, y);
                return;
            case 72:
                g.fillText(l2Rate.text(), x, y);
                return;
            case 73:
                g.fillText(l2Division.text(), x, y);
                return;
            case 74:
                g.fillText(l2Mode.text(), x, y);
                return;
            case 75:
                g.fillText(l2Phase.text(), x, y);
                return;
            case 76:
                g.fillText(l2Delay.text(), x, y);
                return;
            case 80:
                g.fillText(m1.text(), x, y);
                return;
            case 81:
                g.fillText(m2.text(), x, y);
                return;
            case 82:
                g.fillText(m3.text(), x, y);
                return;
            case 83:
                g.fillText(m4.text(), x, y);
                return;
            case 84:
                g.fillText(output.text(), x, y);
                return;
        }
    }

    // Its name over its dial.
    void fillDialName(Canvas& g, int code, double x, double y) const {
        switch (code) {
            case 1:
                g.fillText("MODE", x, y);
                return;
            case 2:
                g.fillText("START", x, y);
                return;
            case 3:
                g.fillText("END", x, y);
                return;
            case 4:
                g.fillText("REVERSE", x, y);
                return;
            case 5:
                g.fillText("NORMALIZE", x, y);
                return;
            case 6:
                g.fillText("GAIN", x, y);
                return;
            case 7:
                g.fillText("ROOT", x, y);
                return;
            case 8:
                g.fillText("TRANSPOSE", x, y);
                return;
            case 9:
                g.fillText("KEYTRACK", x, y);
                return;
            case 10:
                g.fillText("LOOP", x, y);
                return;
            case 11:
                g.fillText("LOOP START", x, y);
                return;
            case 12:
                g.fillText("LOOP END", x, y);
                return;
            case 13:
                g.fillText("CROSSFADE", x, y);
                return;
            case 14:
                g.fillText("SNAP", x, y);
                return;
            case 15:
                g.fillText("PLAY ON", x, y);
                return;
            case 16:
                g.fillText("SLICE BY", x, y);
                return;
            case 17:
                g.fillText("SENSITIVITY", x, y);
                return;
            case 18:
                g.fillText("SLICES", x, y);
                return;
            case 19:
                g.fillText("FIRST KEY", x, y);
                return;
            case 20:
                g.fillText("PLAY", x, y);
                return;
            case 21:
                g.fillText("MODE", x, y);
                return;
            case 22:
                g.fillText("VOICES", x, y);
                return;
            case 23:
                g.fillText("GLIDE", x, y);
                return;
            case 24:
                g.fillText("BEND", x, y);
                return;
            case 25:
                g.fillText("TUNE", x, y);
                return;
            case 26:
                g.fillText("OVERSAMPLING", x, y);
                return;
            case 30:
                g.fillText("TYPE", x, y);
                return;
            case 31:
                g.fillText("CUTOFF", x, y);
                return;
            case 32:
                g.fillText("RESONANCE", x, y);
                return;
            case 33:
                g.fillText("SLOPE", x, y);
                return;
            case 34:
                g.fillText("MODE", x, y);
                return;
            case 35:
                g.fillText("KEYTRACK", x, y);
                return;
            case 36:
                g.fillText("ENV 2", x, y);
                return;
            case 37:
                g.fillText("VELOCITY", x, y);
                return;
            case 38:
                g.fillText("DRIVE", x, y);
                return;
            case 40:
                g.fillText("ATTACK", x, y);
                return;
            case 41:
                g.fillText("DECAY", x, y);
                return;
            case 42:
                g.fillText("SUSTAIN", x, y);
                return;
            case 43:
                g.fillText("RELEASE", x, y);
                return;
            case 44:
                g.fillText("VELOCITY", x, y);
                return;
            case 45:
                g.fillText("LEVEL", x, y);
                return;
            case 46:
                g.fillText("PAN", x, y);
                return;
            case 50:
                g.fillText("ATTACK", x, y);
                return;
            case 51:
                g.fillText("DECAY", x, y);
                return;
            case 52:
                g.fillText("SUSTAIN", x, y);
                return;
            case 53:
                g.fillText("RELEASE", x, y);
                return;
            case 54:
                g.fillText("PITCH", x, y);
                return;
            case 60:
                g.fillText("SHAPE", x, y);
                return;
            case 61:
                g.fillText("SYNC", x, y);
                return;
            case 62:
                g.fillText("RATE", x, y);
                return;
            case 63:
                g.fillText("DIVISION", x, y);
                return;
            case 64:
                g.fillText("MODE", x, y);
                return;
            case 65:
                g.fillText("PHASE", x, y);
                return;
            case 66:
                g.fillText("DELAY", x, y);
                return;
            case 70:
                g.fillText("SHAPE", x, y);
                return;
            case 71:
                g.fillText("SYNC", x, y);
                return;
            case 72:
                g.fillText("RATE", x, y);
                return;
            case 73:
                g.fillText("DIVISION", x, y);
                return;
            case 74:
                g.fillText("MODE", x, y);
                return;
            case 75:
                g.fillText("PHASE", x, y);
                return;
            case 76:
                g.fillText("DELAY", x, y);
                return;
            case 80:
                g.fillText("MACRO 1", x, y);
                return;
            case 81:
                g.fillText("MACRO 2", x, y);
                return;
            case 82:
                g.fillText("MACRO 3", x, y);
                return;
            case 83:
                g.fillText("MACRO 4", x, y);
                return;
            case 84:
                g.fillText("OUTPUT", x, y);
                return;
        }
    }

    // A button's text: a choice's option, or a toggle's name.
    void fillButton(Canvas& g, int code, int value, double x, double y) const {
        if (code == eEnvTab) {
            g.fillText(value == 0 ? "AMP" : "ENV 2", x, y);
            return;
        }
        if (code == eLfoTab) {
            g.fillText(value == 0 ? "1" : "2", x, y);
            return;
        }
        if (code == 1) {
            if (value == 0) {
                g.fillText("CLASSIC", x, y);
                return;
            }
            if (value == 1) {
                g.fillText("ONE-SHOT", x, y);
                return;
            }
            g.fillText("SLICE", x, y);
            return;
        }
        if (code == 10) {
            if (value == 0) {
                g.fillText("OFF", x, y);
                return;
            }
            if (value == 1) {
                g.fillText("FORWARD", x, y);
                return;
            }
            g.fillText("PING-PONG", x, y);
            return;
        }
        if (code == 16) {
            if (value == 0) {
                g.fillText("TRANSIENTS", x, y);
                return;
            }
            g.fillText("GRID", x, y);
            return;
        }
        if (code == 20) {
            if (value == 0) {
                g.fillText("TRIGGER", x, y);
                return;
            }
            if (value == 1) {
                g.fillText("GATE", x, y);
                return;
            }
            g.fillText("THRU", x, y);
            return;
        }
        if (code == 21) {
            if (value == 0) {
                g.fillText("POLY", x, y);
                return;
            }
            if (value == 1) {
                g.fillText("MONO", x, y);
                return;
            }
            g.fillText("LEGATO", x, y);
            return;
        }
        if (code == 26) {
            g.fillText(value == 0 ? "1x" : value == 1 ? "2x" : "4x", x, y);
            return;
        }
        if (code == 30) {
            if (value == 0) {
                g.fillText("OFF", x, y);
                return;
            }
            if (value == 1) {
                g.fillText("LADDER", x, y);
                return;
            }
            g.fillText("SVF", x, y);
            return;
        }
        if (code == 33) {
            if (value == 0) {
                g.fillText("12 dB", x, y);
                return;
            }
            g.fillText("24 dB", x, y);
            return;
        }
        if (code == 34) {
            if (value == 0) {
                g.fillText("LOW", x, y);
                return;
            }
            if (value == 1) {
                g.fillText("BAND", x, y);
                return;
            }
            if (value == 2) {
                g.fillText("HIGH", x, y);
                return;
            }
            g.fillText("NOTCH", x, y);
            return;
        }
        if (code == 64) {
            if (value == 0) {
                g.fillText("NOTE", x, y);
                return;
            }
            g.fillText("FREE", x, y);
            return;
        }
        if (code == 74) {
            if (value == 0) {
                g.fillText("NOTE", x, y);
                return;
            }
            g.fillText("FREE", x, y);
            return;
        }
        fillDialName(g, code, x, y);
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

    // Everything the editor shows for the settings now.
    void place() {
        dials = 0;
        buttons = 0;
        row(1, 0, 3, 404, 20, 300, 24, kHazel);
        for (int k = 0; k < 4; ++k) dial(80 + k, 752 + 52 * k, 36, 13, Theme::modulation, true);
        dial(84, 966, 36, 13, kLinen, true);
        layoutSample();
        layoutPitch();
        layoutFilter();
        layoutEnv();
        layoutLfo();
    }

    // Under the waveform: where it starts and ends and its gain, then the
    // mode's own: the loop, or how it is sliced and how a slice plays.
    void layoutSample() {
        button(4, 0, smpX + smpW - 212, smpY + 14, 92, 22, bToggle, kHazel);
        button(5, 0, smpX + smpW - 112, smpY + 14, 96, 22, bToggle, kHazel);
        const double r = 17;
        dial(2, smpX + 36, rowY, r, kHazel, true);
        dial(3, smpX + 96, rowY, r, kHazel, true);
        dial(6, smpX + 156, rowY, r, kHazel, true);
        const int m = choiceOf(1);
        if (m == kClassic) {
            row(10, 0, 3, smpX + 200, rowY - 11, 252, 22, kShoot);
            const bool on = choiceOf(10) != kLoopOff;
            dial(11, smpX + 492, rowY, r, kShoot, on);
            dial(12, smpX + 552, rowY, r, kShoot, on);
            dial(13, smpX + 612, rowY, r, kShoot, choiceOf(10) == kForward);
            const int first = buttons;
            button(14, 0, smpX + 652, rowY - 25, 76, 21, bToggle, kShoot);
            button(15, 0, smpX + 652, rowY + 4, 76, 21, bToggle, kShoot);
            if (!on) dimSince(first);
        } else if (m == kSlice) {
            row(16, 0, 2, smpX + 200, rowY - 11, 176, 22, kShoot);
            dial(choiceOf(16) == 0 ? 17 : 18, smpX + 418, rowY, r, kShoot, true);
            dial(19, smpX + 478, rowY, r, kShoot, true);
            row(20, 0, 3, smpX + 518, rowY - 11, 210, 22, kShoot);
        }
    }

    // The pitch: the root, transposing, tuning, bending and key tracking; the
    // voices: how notes take them, how many, the glide and the oversampling.
    static constexpr double pRow1 = pitY + 88;
    static constexpr double pRow2 = pitY + 160;

    void layoutPitch() {
        const bool sliced = choiceOf(1) == kSlice;
        const double r = 19;
        dial(7, pitX + 48, pRow1, r, kSeaGlass, !sliced);
        dial(8, pitX + 124, pRow1, r, kSeaGlass, true);
        dial(25, pitX + 200, pRow1, r, kSeaGlass, true);
        dial(24, pitX + 276, pRow1, r, kSeaGlass, true);
        if (!sliced) button(9, 0, pitX + 176, pRow2 - 11, 92, 22, bToggle, kSeaGlass);
        dial(22, pitX + 48, pRow2, r, kSeaGlass, choiceOf(21) == 0);
        dial(23, pitX + 124, pRow2, r, kSeaGlass, true);
        row(21, 0, 3, pitX + 400, pRow1 - 11, 188, 22, kSeaGlass);
        row(26, 0, 3, pitX + 400, pRow2 - 11, 188, 22, kSeaGlass);
    }

    // The filter's type over its response; its dials in two rows: the
    // cutoff and resonance (and the ladder's slope or the svf's mode), then
    // what moves the cutoff and the drive.
    static constexpr double fCol = filX + 304;
    static constexpr double fStep = 72;
    static constexpr double fRow1 = filY + 80;
    static constexpr double fRow2 = filY + 156;

    // Off, all of it stays in place dimmed, with the ladder's slope.
    void layoutFilter() {
        row(30, 0, 3, filX + filW - 256, filY + 13, 240, 22, kSand);
        const int type = choiceOf(30);
        const bool on = type != kOff;
        const double r = 19;
        dial(31, fCol, fRow1, r, kSand, on);
        dial(32, fCol + fStep, fRow1, r, kSand, on);
        if (type != kSvf) {
            const int first = buttons;
            row(33, 0, 2, fCol + fStep + 40, fRow1 - 11, 136, 22, kSand);
            if (!on) dimSince(first);
        } else {
            const double half = 65.5;
            for (int k = 0; k < 4; ++k)
                button(34, k, fCol + fStep + 40 + imod(k, 2) * (half + 5),
                       fRow1 - 24 + idiv(k, 2) * 26, half, 22, bChoice, kSand);
        }
        dial(35, fCol, fRow2, r, kSand, on);
        dial(36, fCol + fStep, fRow2, r, kSand, on);
        dial(37, fCol + 2 * fStep, fRow2, r, kSand, on);
        dial(38, fCol + 3 * fStep, fRow2, r, kSand, on);
    }

    int envColour(int k) const { return k == 0 ? Theme::accent : kSand; }
    int envBase(int k) const { return k == 0 ? 40 : 50; }

    void layoutEnv() {
        for (int k = 0; k < 2; ++k)
            button(eEnvTab, k, envX + envW - 148 + k * 66, envY + 13, 62, 22, bSmallTab,
                   envColour(k));
        const int base = envBase(envTab);
        const int colour = envColour(envTab);
        const int n = envTab == 0 ? 7 : 5;
        // The amp's release does not apply where notes play on whatever the key does.
        const bool unheld =
            choiceOf(1) == kOneShot || (choiceOf(1) == kSlice && choiceOf(20) == kTrigger);
        for (int k = 0; k < n; ++k)
            dial(base + k, shapeX + 26 + k * 52, envY + 158, 15, colour,
                 !(envTab == 0 && k == 3 && unheld));
    }

    void layoutLfo() {
        const int l = lfoTab;
        for (int k = 0; k < 2; ++k)
            button(eLfoTab, k, lfoX + lfoW - 82 + k * 34, lfoY + 13, 30, 22, bSmallTab,
                   Theme::modulation);
        const double each = (lfoW - 32 - 6 * 4.0) / 7.0;
        for (int k = 0; k < 7; ++k)
            button(lc(l, 0), k, lfoX + 16 + k * (each + 4), lfoY + 42, each, 24, bShape,
                   Theme::modulation);
        const double x0 = cycleX + cycleW + 12, w = lfoX + lfoW - 16 - x0;
        const bool synced = valueOf(lc(l, 1)) > 0.5;
        const double y = cycleY + 34;
        dial(lc(l, synced ? 3 : 2), x0 + w / 6.0, y, 15, Theme::modulation, true);
        dial(lc(l, 5), x0 + w * 0.5, y, 15, Theme::modulation, true);
        dial(lc(l, 6), x0 + w * 5.0 / 6.0, y, 15, Theme::modulation, true);
        const double by = cycleY + cycleH - 22;
        row(lc(l, 4), 0, 2, x0, by, w - 50, 22, Theme::modulation);
        button(lc(l, 1), 0, x0 + w - 46, by, 46, 22, bToggle, Theme::modulation);
    }

    int dialAt(double x, double y) const {
        for (int k = 0; k < dials; ++k) {
            const double dx = x - dialX[k], dy = y - dialY[k], r = dialR[k] + 6;
            if (dialLive[k] && dx * dx + dy * dy <= r * r) return k;
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

    bool hovered(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX < x + w && hoverY >= y && hoverY < y + h;
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
        const int code = buttonCode[k], value = buttonValue[k];
        if (code == eEnvTab) return value == envTab;
        if (code == eLfoTab) return value == lfoTab;
        if (code < 0) return false;
        if (buttonStyle[k] == bToggle) return valueOf(code) > 0.5;
        return choiceOf(code) == value;
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

    // An LFO shape's icon: a cycle of it.
    void shapeIcon(Canvas& g, int shape, double x, double y, double w, double h, int colour) {
        g.strokeStyle(colour);
        g.lineWidth(1.4);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int i = 0; i <= 24; ++i) {
            const double t = i / 24.0;
            const double v = lfoWave(shape, t * 0.999, shape >= 5 ? floorToInt(t * 3.0) : 0, 3);
            const double vy = shape == 5 ? lfoWave(5, 0.0, floorToInt(t * 3.0), 3) : v;
            if (i == 0)
                g.moveTo(x + t * w, y + h * 0.5 - vy * h * 0.4);
            else
                g.lineTo(x + t * w, y + h * 0.5 - vy * h * 0.4);
        }
        g.stroke();
    }

    void drawButton(Canvas& g, int k) {
        const int style = buttonStyle[k];
        const double x = buttonX[k], y = buttonY[k], w = buttonW[k], h = buttonH[k];
        const bool hot = k == hotButton;
        const bool lit = buttonLit(k);
        if (style == bShape) {
            g.fillStyle(lit ? 0x3A2F3F : (hot ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(x, y, w, h, 6);
            g.fill();
            g.strokeStyle(lit ? buttonColour[k] : Theme::line);
            g.lineWidth(1);
            g.stroke();
            shapeIcon(g, buttonValue[k], x + 8, y + 4, w - 16, h - 8,
                      lit ? buttonColour[k] : (hot ? Theme::text : Theme::textFaint));
            return;
        }
        if (!buttonLive[k]) g.globalAlpha(0.35);
        pill(g, x, y, w, h, lit, buttonColour[k], hot);
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

    // A coppice stool: a cut stump, and the shoots that grow from it; a shoot
    // lights for each voice playing.
    void drawMark(Canvas& g, double x, double y) {
        g.linearGradient(0, y + 10, 0, y + 22, kBark, 0x5E4B36);
        g.beginPath();
        g.moveTo(x - 15, y + 22);
        g.lineTo(x - 11, y + 11);
        g.quadraticCurveTo(x, y + 8, x + 11, y + 11);
        g.lineTo(x + 15, y + 22);
        g.closePath();
        g.fill();
        g.fillStyle(kHazel);
        g.beginPath();
        g.ellipse(x, y + 11, 11, 3);
        g.fill();
        const int playing = soundingNow.get();
        g.lineCap(LineCap::Round);
        for (int k = 0; k < 7; ++k) {
            const double base = x - 8 + k * 2.7;
            const double lean = (k - 3) * 2.6;
            const double tall = 18 + 5 * sin(k * 2.1);
            g.strokeStyle(k < playing ? kLinen : (imod(k, 2) == 0 ? kShoot : 0x8A9A57));
            g.lineWidth(1.5);
            g.beginPath();
            g.moveTo(base, y + 10);
            g.quadraticCurveTo(base + lean * 0.2, y + 10 - tall * 0.6, base + lean, y + 10 - tall);
            g.stroke();
        }
    }

    // The patch in a phrase: how it plays.
    void drawSummary(Canvas& g, double x, double y) {
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        if (fileFrames.get() == 0) {
            g.fillText("no sound: choose a file", x, y);
            return;
        }
        const int m = choiceOf(1);
        const bool back = valueOf(4) > 0.5;
        if (m == kSlice) {
            g.fillText(format("%d slices from ", slicesShown.get()), x, y);
            fillNote(g, choiceOf(19),
                     x + g.measureText(format("%d slices from ", slicesShown.get())), y);
            return;
        }
        if (m == kOneShot) {
            g.fillText(back ? "one-shot, backwards" : "one-shot", x, y);
            return;
        }
        const int loop = choiceOf(10);
        if (loop == kLoopOff)
            g.fillText(back ? "pitched, backwards" : "pitched from its root", x, y);
        else if (loop == kForward)
            g.fillText(back ? "pitched, backwards, looping" : "pitched, looping", x, y);
        else
            g.fillText(back ? "pitched, backwards, ping-pong" : "pitched, ping-pong", x, y);
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 34, 30);
        g.fillStyle(Theme::text);
        g.serifFont(25);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Coppice", 60, 41);
        drawSummary(g, 68 + g.measureText("Coppice"), 41);
        label(g, "VOICES", 1032, 24, TextAlign::Center);
        g.fillStyle(Theme::text);
        g.monoFont(13);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        const int most = choiceOf(21) == 0 ? roundToInt(valueOf(22)) : 1;
        g.fillText(format("%d / %d", soundingNow.get(), most), 1032, 42);
        label(g, "OUT", 1080, 30, TextAlign::Right);
        drawLevel(g, 1088, 25, 120, outLeft.get(), outRight.get());
    }

    // --- the sample ---------------------------------------------------------------------------

    // The waveform's inside, and where a file frame or a percent of it falls.
    static constexpr double inX = wavX + 8;
    static constexpr double inW = wavW - 16;
    static constexpr double laneTop = wavY + flagH + loopH + 4;
    static constexpr double laneBottom = wavY + wavH - 18;

    double xOfFrame(double f) const {
        const int n = fileFrames.get();
        return inX + inW * (n > 0 ? clamp(f / n, 0.0, 1.0) : 0.0);
    }
    double xOfShare(double p) const { return inX + inW * clamp(p, 0.0, 1.0); }

    // Channel c's peaks, mirrored about the middle of its lane.
    void drawPeaks(Canvas& g, int c, double top, double bottom) {
        const double mid = (top + bottom) * 0.5, half = (bottom - top) * 0.5 - 1;
        const int columns = floorToInt(inW);
        g.beginPath();
        for (int px = 0; px <= columns; ++px) {
            const int k = min(kColumns - 1, idiv(px * kColumns, columns + 1));
            const double y = mid - min(1.0, overview[c * kColumns + k]) * half;
            if (px == 0)
                g.moveTo(inX + px, y);
            else
                g.lineTo(inX + px, y);
        }
        for (int px = columns; px >= 0; --px) {
            const int k = min(kColumns - 1, idiv(px * kColumns, columns + 1));
            g.lineTo(inX + px, mid + min(1.0, overview[c * kColumns + k]) * half + 0.5);
        }
        g.closePath();
        g.fill();
    }

    void drawWave(Canvas& g) {
        well(g, wavX, wavY, wavW, wavH);
        const int n = fileFrames.get();
        if (n == 0) {
            aside(g,
                  "no file: drop one on the box above, click it to choose, or pick a factory "
                  "patch",
                  wavX + wavW * 0.5, wavY + wavH * 0.5, TextAlign::Center);
            return;
        }
        const int m = choiceOf(1);
        const int lanes = fileChannels.get() > 1 ? 2 : 1;
        const double laneH = (laneBottom - laneTop) / lanes;
        // The file, dim, then the region lit.
        const double ra = xOfFrame(regionA.get()), rb = xOfFrame(regionB.get());
        for (int pass = 0; pass < 2; ++pass) {
            g.save();
            if (pass == 1) g.clip(ra, wavY, max(rb - ra, 1.0), wavH);
            g.fillStyle(kHazel);
            g.globalAlpha(pass == 0 ? 0.25 : 0.85);
            for (int c = 0; c < lanes; ++c)
                drawPeaks(g, c, laneTop + c * laneH, laneTop + (c + 1) * laneH);
            g.restore();
        }
        g.globalAlpha(1.0);
        // The loop: a bracket over the waveform, and the crossfade before its end.
        if (m == kClassic && choiceOf(10) != kLoopOff) {
            const double la = xOfFrame(loopA.get()), lb = xOfFrame(loopB.get());
            const double y = wavY + flagH + loopH * 0.5 + 1;
            const double fade = choiceOf(10) == kForward ? fadeShown.get() / max(n, 1) * inW : 0.0;
            if (fade > 0.5) {
                g.fillStyle(kShoot);
                g.globalAlpha(0.12);
                g.beginPath();
                g.moveTo(lb - fade, laneBottom);
                g.lineTo(lb, laneTop);
                g.lineTo(lb, laneBottom);
                g.closePath();
                g.fill();
                g.globalAlpha(1.0);
            }
            g.fillStyle(kShoot);
            g.globalAlpha(0.07);
            g.fillRect(la, laneTop, max(lb - la, 1.0), laneBottom - laneTop);
            g.globalAlpha(1.0);
            g.strokeStyle(kShoot);
            g.lineWidth(hotHandle == -10 || (dragging == 5) ? 2.4 : 1.6);
            g.beginPath();
            g.moveTo(la, laneBottom);
            g.lineTo(la, y);
            g.lineTo(lb, y);
            g.lineTo(lb, laneBottom);
            g.stroke();
            for (int k = 0; k < 2; ++k) {
                const double x = k == 0 ? la : lb;
                const bool hot = hotHandle == 11 + k || (dragging == 4 && handle == 11 + k);
                g.fillStyle(hot ? kLinen : kShoot);
                g.beginPath();
                g.roundRect(x - 4, y - 5, 8, 10, 2);
                g.fill();
            }
            if (choiceOf(10) == kPingPong)
                aside(g, "back and forth", (la + lb) * 0.5, y - 1, TextAlign::Center);
        }
        // The slices, numbered, with the keys that play them.
        if (m == kSlice) {
            const int count = slicesShown.get();
            double lastLabel = -100.0;
            const int first = choiceOf(19);
            const int playing = newestKey.get() - first;
            for (int k = 0; k < count; ++k) {
                const double x = xOfFrame(sliceShown[k]);
                const bool lit = k == playing;
                g.strokeStyle(lit ? kLinen : kShoot);
                g.globalAlpha(lit ? 1.0 : 0.6);
                g.lineWidth(lit ? 1.6 : 1.0);
                g.beginPath();
                g.moveTo(x + 0.5, wavY + flagH + 2);
                g.lineTo(x + 0.5, laneBottom);
                g.stroke();
                g.globalAlpha(1.0);
                if (x - lastLabel >= 24.0) {
                    g.fillStyle(lit ? kLinen : kShoot);
                    g.boldFont(8);
                    g.textAlign(TextAlign::Left);
                    g.textBaseline(TextBaseline::Middle);
                    g.fillText(format("%d", k + 1), x + 3, wavY + flagH + loopH * 0.5 + 1);
                    g.fillStyle(lit ? kLinen : Theme::textFaint);
                    g.monoFont(8);
                    fillNote(g, first + k, x + 3, wavY + wavH - 9);
                    lastLabel = x;
                }
            }
        }
        // Where each voice plays.
        g.strokeStyle(kLinen);
        g.lineWidth(1);
        g.globalAlpha(0.8);
        for (int v = 0; v < kVoices; ++v) {
            if (heads[v] < 0.0) continue;
            const double x = xOfFrame(heads[v]);
            g.beginPath();
            g.moveTo(x + 0.5, laneTop);
            g.lineTo(x + 0.5, laneBottom);
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The start and end flags.
        for (int k = 0; k < 2; ++k) {
            const double x = xOfShare(valueOf(2 + k));
            const bool hot = hotHandle == 2 + k || (dragging == 4 && handle == 2 + k);
            g.strokeStyle(hot ? kLinen : kHazel);
            g.lineWidth(hot ? 1.6 : 1.1);
            g.beginPath();
            g.moveTo(x + 0.5, wavY + 3);
            g.lineTo(x + 0.5, laneBottom);
            g.stroke();
            const double d = k == 0 ? 1.0 : -1.0;
            g.fillStyle(hot ? kLinen : kHazel);
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

    // A close-up: kPane points of the sound from `from`, scaled to fill it.
    void drawPane(Canvas& g, double x, int from, const char* name) {
        label(g, name, x + 2, paneY - 8, TextAlign::Left);
        aside(g, "2.5 ms each way", x + paneW - 2, paneY - 8, TextAlign::Right);
        well(g, x, paneY, paneW, paneH);
        const double mid = paneY + paneH * 0.5;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(x + 4, mid);
        g.lineTo(x + paneW - 4, mid);
        g.moveTo(x + paneW * 0.5, paneY + 4);
        g.lineTo(x + paneW * 0.5, paneY + paneH - 4);
        g.stroke();
        if (fileFrames.get() == 0) return;
        double most = 0.05;
        for (int j = 0; j < kPane; ++j) most = max(most, abs(pane[from + j]));
        g.strokeStyle(kHazel);
        g.lineWidth(1.4);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int j = 0; j < kPane; ++j) {
            const double px = x + 6 + (paneW - 12) * j / (kPane - 1.0);
            const double py = mid - pane[from + j] / most * (paneH * 0.5 - 8);
            if (j == 0)
                g.moveTo(px, py);
            else
                g.lineTo(px, py);
        }
        g.stroke();
    }

    void drawSamplePanel(Canvas& g) {
        panel(g, smpX, smpY, smpW, smpH);
        heading(g, "Sample", smpX + 18, smpY + 25);
        drawWave(g);
        const int m = choiceOf(1);
        if (m == kClassic) label(g, "LOOP", smpX + 326, rowY - 26, TextAlign::Center);
        if (m == kSlice) {
            label(g, "SLICE BY", smpX + 288, rowY - 26, TextAlign::Center);
            label(g, "PLAY", smpX + 623, rowY - 26, TextAlign::Center);
        }
        if (m == kOneShot) {
            aside(g, "plays the region whole at each key, whatever the key does after", smpX + 200,
                  rowY - 8, TextAlign::Left);
            aside(g, "(so the amp's release does not apply)", smpX + 200, rowY + 8,
                  TextAlign::Left);
        }
        // Reversed, the start is the region's end (or the slice's), and the
        // end its start.
        const int kind = imod(paneKind.get(), 100);
        const bool back = paneKind.get() >= 100;
        if (kind == 3 || kind == 13)
            drawPane(g, paneAX, 0, back ? "SLICE START, FROM ITS END" : "SLICE START");
        else
            drawPane(g, paneAX, 0, back ? "START, FROM THE END" : "START");
        if (kind >= 10)
            drawPane(g, paneBX, kPane, "LOOP SEAM");
        else
            drawPane(g, paneBX, kPane, back ? "END, AT THE START" : "END");
    }

    // --- pitch, voice and the filter ----------------------------------------------------------

    void drawPitchPanel(Canvas& g) {
        panel(g, pitX, pitY, pitW, pitH);
        heading(g, "Pitch & Voice", pitX + 18, pitY + 25);
        label(g, "MODE", pitX + 494, pRow1 - 26, TextAlign::Center);
        label(g, "OVERSAMPLING", pitX + 494, pRow2 - 26, TextAlign::Center);
        if (choiceOf(1) == kSlice)
            aside(g, "a slice keeps its own pitch", pitX + 176, pRow2, TextAlign::Left);
    }

    double hzAt(double x) const { return 20.0 * exp2((x - curveX) / curveW * log2(1000.0)); }
    double xAt(double hz) const {
        return curveX + curveW * log2(max(hz, 20.0) / 20.0) / log2(1000.0);
    }
    double dbY(double db) const {
        return curveY + 10 + (curveH - 20) * (18.0 - clamp(db, -42.0, 18.0)) / 60.0;
    }

    // The filter's response at hz, in dB.
    double responseDb(double hz) {
        const int type = choiceOf(30);
        const double drive = dbToGain(valueOf(38));
        const double w = hz / valueOf(31);
        const double res = valueOf(32);
        double re = 1.0, im = 0.0;
        if (type == kLadder) {
            // G = 1 / (1 + jw); the ladder's G^4 / (1 + k G^4), or G^2 over
            // the same for 12 dB.
            const double k = 4.0 * res;
            const double d = 1.0 + w * w;
            const double gr = 1.0 / d, gi = -w / d;
            const double g2r = gr * gr - gi * gi, g2i = 2.0 * gr * gi;
            const double g4r = g2r * g2r - g2i * g2i, g4i = 2.0 * g2r * g2i;
            const double nr = choiceOf(33) == 1 ? g4r : g2r;
            const double ni = choiceOf(33) == 1 ? g4i : g2i;
            const double dr = 1.0 + k * g4r, di = k * g4i;
            const double m = dr * dr + di * di;
            const double scale = (1.0 + 0.5 * k) * drive / max(1.0, drive * 0.5 + 0.5);
            re = (nr * dr + ni * di) / m * scale;
            im = (ni * dr - nr * di) / m * scale;
        } else if (type == kSvf) {
            const double k = max(0.005, 2.0 * (1.0 - res) * (1.0 - res));
            const double dr = 1.0 - w * w, di = k * w;
            const int mode = choiceOf(34);
            double nr = 1.0, ni = 0.0;
            if (mode == 1) {
                nr = 0.0;
                ni = w;
            } else if (mode == 2) {
                nr = -w * w;
            } else if (mode == 3) {
                nr = 1.0 - w * w;
            }
            const double m = dr * dr + di * di;
            re = (nr * dr + ni * di) / m * drive;
            im = (ni * dr - nr * di) / m * drive;
        }
        return gainToDb(sqrt(re * re + im * im));
    }

    void drawFilterPanel(Canvas& g) {
        panel(g, filX, filY, filW, filH);
        heading(g, "Filter", filX + 18, filY + 25);
        well(g, curveX, curveY, curveW, curveH);
        g.frequencyGrid(curveX, curveY, curveW, curveH, 20, 20000);
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(curveX + 4, dbY(0.0));
        g.lineTo(curveX + curveW - 4, dbY(0.0));
        g.stroke();
        const int type = choiceOf(30);
        const double cx = fCol + fStep + 40 + 68;
        if (type == kOff) g.globalAlpha(0.35);
        if (type != kSvf) label(g, "SLOPE", cx, fRow1 - 26, TextAlign::Center);
        g.globalAlpha(1.0);
        if (type == kSvf) label(g, "MODE", cx, fRow1 - 36, TextAlign::Center);
        if (type == kOff) {
            aside(g, "off: the sample plays as it is", curveX + curveW * 0.5,
                  curveY + curveH * 0.5 - 14, TextAlign::Center);
            return;
        }
        g.save();
        g.clip(curveX + 1, curveY + 1, curveW - 2, curveH - 2);
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            for (int i = 0; i <= 160; ++i) {
                const double x = curveX + curveW * i / 160.0;
                const double y = dbY(responseDb(hzAt(x)));
                if (i == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            if (pass == 0) {
                g.lineTo(curveX + curveW, curveY + curveH);
                g.lineTo(curveX, curveY + curveH);
                g.closePath();
                g.fillStyle(kSand);
                g.globalAlpha(0.08);
                g.fill();
                g.globalAlpha(1.0);
            } else {
                g.strokeStyle(kSand);
                g.lineWidth(1.8);
                g.lineJoin(LineJoin::Round);
                g.stroke();
            }
        }
        g.restore();
        const double nx = clamp(xAt(valueOf(31)), curveX + 6, curveX + curveW - 6);
        const double ny = curveY + curveH - 12 - (curveH - 24) * clamp(valueOf(32), 0.0, 1.0);
        const bool hot = dragging == 2 || hovered(nx - 10, ny - 10, 20, 20);
        g.fillStyle(kInk);
        g.beginPath();
        g.circle(nx, ny, hot ? 7.5 : 6.5);
        g.fill();
        g.strokeStyle(kSand);
        g.lineWidth(2);
        g.stroke();
    }

    // --- the envelopes ------------------------------------------------------------------------

    // How wide a stage of `ms` is drawn, before fitting.
    double stageWidth(double ms) const { return 10.0 + 22.0 * log2(1.0 + ms / 5.0); }

    double envScale(int base) const {
        const double hold = shapeW * 0.16;
        const double total = stageWidth(valueOf(base)) + stageWidth(valueOf(base + 1)) +
                             stageWidth(valueOf(base + 3));
        return min(1.0, (shapeW - 16 - hold) / total);
    }
    double cornerX(int base, int k) const {
        const double s = envScale(base);
        double x = shapeX + 8 + stageWidth(valueOf(base)) * s;
        if (k == 0) return x;
        x += stageWidth(valueOf(base + 1)) * s;
        if (k == 1) return x;
        return x + shapeW * 0.16 + stageWidth(valueOf(base + 3)) * s;
    }
    double levelY(double level) const { return shapeY + shapeH - 8 - (shapeH - 18) * level; }

    void drawEnvPanel(Canvas& g) {
        panel(g, envX, envY, envW, envH);
        heading(g, "Envelopes", envX + 18, envY + 25);
        well(g, shapeX, shapeY, shapeW, shapeH);
        const int base = envBase(envTab);
        const int colour = envColour(envTab);
        const double s = valueOf(base + 2);
        const double x0 = shapeX + 8, xa = cornerX(base, 0), xd = cornerX(base, 1);
        const double xs = xd + shapeW * 0.16, xr = cornerX(base, 2);
        const double bottom = levelY(0.0), top = levelY(1.0), sy = levelY(s);
        g.beginPath();
        g.moveTo(x0, bottom);
        g.lineTo(xa, top);
        for (int i = 1; i <= 24; ++i) {
            const double t = i / 24.0;
            g.lineTo(xa + (xd - xa) * t, levelY(s + (1.0 - s) * (1.0 - t) * (1.0 - t) * (1.0 - t)));
        }
        g.lineTo(xs, sy);
        for (int i = 1; i <= 24; ++i) {
            const double t = i / 24.0;
            g.lineTo(xs + (xr - xs) * t, levelY(s * (1.0 - t) * (1.0 - t) * (1.0 - t)));
        }
        g.lineTo(x0, bottom);
        g.closePath();
        g.fillStyle(colour);
        g.globalAlpha(0.12);
        g.fill();
        g.globalAlpha(1.0);
        g.strokeStyle(colour);
        g.lineWidth(1.8);
        g.lineJoin(LineJoin::Round);
        g.stroke();
        for (int k = 0; k < 3; ++k) {
            const double hx = k == 0 ? xa : k == 1 ? xd : xr;
            const double hy = k == 0 ? top : k == 1 ? sy : bottom;
            const bool hot = (dragging == 3 && handle == k) || hovered(hx - 9, hy - 9, 18, 18);
            g.fillStyle(kInk);
            g.beginPath();
            g.circle(hx, hy, hot ? 6 : 5);
            g.fill();
            g.strokeStyle(colour);
            g.lineWidth(1.8);
            g.stroke();
        }
        // Where the newest note is.
        const int stage = stageShown[envTab];
        if (stage > 0) {
            const double level = clamp(envShown[envTab], 0.0, 1.0);
            double x = xd;
            if (stage == 1) x = x0 + (xa - x0) * level;
            if (stage == 2)
                x = xa + (xd - xa) * clamp(1.0 - cbrt((level - s) / max(1.0 - s, 1e-6)), 0.0, 1.0);
            if (stage == 3) x = xd + 0.5 * (xs - xd);
            if (stage == 4) x = xs + (xr - xs) * clamp(1.0 - cbrt(level / max(s, 1e-6)), 0.0, 1.0);
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(x, levelY(level), 3.5);
            g.fill();
        }
    }

    // --- the LFOs -----------------------------------------------------------------------------

    void drawLfoPanel(Canvas& g) {
        panel(g, lfoX, lfoY, lfoW, lfoH);
        heading(g, "LFOs", lfoX + 18, lfoY + 25);
        well(g, cycleX, cycleY, cycleW, cycleH);
        const int l = lfoTab;
        const int shape = choiceOf(lc(l, 0));
        const double mid = cycleY + cycleH * 0.5, half = cycleH * 0.38;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(cycleX + 6, mid);
        g.lineTo(cycleX + cycleW - 6, mid);
        g.stroke();
        // Two cycles from a note's start, fading in over the delay.
        const bool synced = valueOf(lc(l, 1)) > 0.5;
        const double hz = synced ? 2.0 / divisionBeats(choiceOf(lc(l, 3))) : valueOf(lc(l, 2));
        const double seconds = 2.0 / max(hz, 0.001);
        const double delay = valueOf(lc(l, 6)) * 0.001;
        const double from = valueOf(lc(l, 5)) / 360.0;
        const bool note = choiceOf(lc(l, 4)) == 0;
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            for (int i = 0; i <= 120; ++i) {
                const double t = i / 120.0;
                const double total = from + 2.0 * t;
                const double fade =
                    pass == 0 || !note || delay <= 0.0 ? 1.0 : min(1.0, t * seconds / delay);
                const double v = lfoWave(shape, total - floor(total), floorToInt(total), 7) * fade;
                const double x = cycleX + 8 + t * (cycleW - 16);
                if (i == 0)
                    g.moveTo(x, mid - v * half);
                else
                    g.lineTo(x, mid - v * half);
            }
            g.strokeStyle(Theme::modulation);
            g.globalAlpha(pass == 0 ? 0.25 : 1.0);
            g.lineWidth(pass == 0 ? 1.0 : 1.8);
            g.stroke();
        }
        g.globalAlpha(1.0);
        const double t = lfoShown[l];
        if (t >= 0.0) {
            const double total = from + 2.0 * t;
            const double v = lfoWave(shape, total - floor(total), floorToInt(total), 7);
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(cycleX + 8 + t * (cycleW - 16), mid - v * half, 3.5);
            g.fill();
        }
    }

    void drawModPanel(Canvas& g) {
        panel(g, modX, modY, modW, modH);
        heading(g, "Modulation", modX + 18, modY + 25);
        aside(g, "a source moves a setting by its amount", modX + modW - 18, modY + 25,
              TextAlign::Right);
    }

    void draw(Canvas& g) {
        place();
        g.linearGradient(0, 0, 0, kH, Theme::ground, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawSamplePanel(g);
        drawPitchPanel(g);
        drawFilterPanel(g);
        drawEnvPanel(g);
        drawLfoPanel(g);
        drawModPanel(g);
        drawStrip(g);
        for (int k = 0; k < buttons; ++k) drawButton(g, k);
        for (int k = 0; k < dials; ++k) drawDial(g, k);
    }

    // --- over the keyboard --------------------------------------------------------------------

    // Where the keyboard draws a key, as the framework lays it out: the white
    // keys share the width, a black one sits across their join.
    bool black(int key) const {
        const int n = imod(key, 12);
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }
    int whitesBefore(int key) const {
        int n = 0;
        for (int k = kLowKey; k < key; ++k)
            if (!black(k)) ++n;
        return n;
    }
    double keyWidth(int key) const {
        const double white = smpW / whitesBefore(kHighKey + 1);
        return black(key) ? white * 0.62 : white;
    }
    double keyLeft(int key) const {
        const double x = smpX + smpW / whitesBefore(kHighKey + 1) * whitesBefore(key);
        return black(key) ? x - keyWidth(key) * 0.5 : x;
    }

    // In Slice a bar over each slice's key, the newest note's lit; otherwise
    // a mark over the root.
    void drawStrip(Canvas& g) {
        if (choiceOf(1) == kSlice) {
            const int first = choiceOf(19);
            const int count = slicesShown.get();
            for (int k = 0; k < count; ++k) {
                const int key = first + k;
                if (key < kLowKey || key > kHighKey) continue;
                g.fillStyle(key == newestKey.get() ? kLinen : kShoot);
                g.beginPath();
                g.roundRect(keyLeft(key) + 1, stripY + (black(key) ? 0 : 6), keyWidth(key) - 2, 6,
                            1.5);
                g.fill();
            }
            return;
        }
        const int key = choiceOf(7);
        if (key < kLowKey || key > kHighKey) return;
        const double x = keyLeft(key) + keyWidth(key) * 0.5;
        g.fillStyle(kHazel);
        g.beginPath();
        g.moveTo(x - 5, stripY + 3);
        g.lineTo(x + 5, stripY + 3);
        g.lineTo(x, stripY + 11);
        g.closePath();
        g.fill();
    }

    // --- the pointer --------------------------------------------------------------------------

    // The waveform's handle at (x, y): the start or end flag (2, 3), a loop
    // end (11, 12) or the loop's middle (-10); 0 for none.
    int waveHandle(double x, double y) const {
        if (fileFrames.get() == 0 || x < wavX || x >= wavX + wavW || y < wavY || y >= wavY + wavH)
            return 0;
        const double xs = xOfShare(valueOf(2)), xe = xOfShare(valueOf(3));
        const bool loop = choiceOf(1) == kClassic && choiceOf(10) != kLoopOff;
        if (loop && y >= wavY + flagH && y < wavY + flagH + loopH + 4) {
            const double la = xOfFrame(loopA.get()), lb = xOfFrame(loopB.get());
            if (abs(x - la) <= 7) return 11;
            if (abs(x - lb) <= 7) return 12;
            if (x > la && x < lb) return -10;
        }
        const double reach = y < wavY + flagH ? 10.0 : 6.0;
        const double ds = abs(x - xs), de = abs(x - xe);
        if (ds <= reach && ds <= de) return 2;
        if (de <= reach) return 3;
        return 0;
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
            hotHandle = 0;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotDial = dialAt(p.x(), p.y());
            hotButton = buttonAt(p.x(), p.y());
            hotHandle = waveHandle(p.x(), p.y());
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
                const double across = (p.x() - pressX) * fine / curveW;
                const double up = -(p.y() - pressY) * fine / (curveH - 24);
                editParam(31, 0, pressA * exp2(across * log2(1000.0)));
                editParam(32, 0, pressB + up);
            } else if (dragging == 3) {
                const int base = envBase(envTab);
                const int time = base + (handle == 0 ? 0 : handle == 1 ? 1 : 3);
                editParam(time, 0, max(pressA, 1.0) * exp2((p.x() - pressX) * fine / 40.0));
                if (handle == 1)
                    editParam(base + 2, 0, pressB - (p.y() - pressY) * fine / (shapeH - 18));
            } else if (dragging == 4) {
                editParam(handle, 0, clamp(pressA + (p.x() - pressX) * fine / inW, 0.0, 1.0));
            } else if (dragging == 5) {
                // The whole loop, its length kept.
                const double d = clamp((p.x() - pressX) * fine / inW, -min(pressA, pressB),
                                       1.0 - max(pressA, pressB));
                editParam(11, 0, pressA + d);
                editParam(12, 0, pressB + d);
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotDial = dialAt(p.x(), p.y());
            hotHandle = waveHandle(p.x(), p.y());
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
            if (h > 0) editParam(h, 0, clamp(valueOf(h) + notches * 0.001 * fine, 0.0, 1.0));
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
        if (h != 0) {
            pressX = x;
            pressY = y;
            if (h == -10) {
                dragging = 5;
                pressA = valueOf(11);
                pressB = valueOf(12);
                return;
            }
            if (p.doubleClick()) {
                editParam(h, 2, 0.0);
                return;
            }
            dragging = 4;
            handle = h;
            pressA = valueOf(h);
            return;
        }
        if (choiceOf(30) != kOff && p.over(curveX, curveY, curveW, curveH)) {
            dragging = 2;
            pressX = x;
            pressY = y;
            pressA = valueOf(31);
            pressB = valueOf(32);
            return;
        }
        if (p.over(shapeX, shapeY, shapeW, shapeH)) {
            const int base = envBase(envTab);
            const double s = valueOf(base + 2);
            int best = -1;
            double closest = 400.0;
            for (int c = 0; c < 3; ++c) {
                const double hx = cornerX(base, c);
                const double hy = c == 0 ? levelY(1.0) : c == 1 ? levelY(s) : levelY(0.0);
                const double d = (x - hx) * (x - hx) + (y - hy) * (y - hy);
                if (d < closest) {
                    closest = d;
                    best = c;
                }
            }
            if (best < 0) return;
            dragging = 3;
            handle = best;
            pressX = x;
            pressY = y;
            pressA = valueOf(base + (best == 0 ? 0 : best == 1 ? 1 : 3));
            pressB = s;
        }
    }

    void press(int b) {
        const int code = buttonCode[b], value = buttonValue[b];
        if (code == eEnvTab) envTab = value;
        if (code == eLfoTab) lfoTab = value;
        if (code < 0) return;
        if (buttonStyle[b] == bToggle) {
            editParam(code, 0, valueOf(code) > 0.5 ? 0.0 : 1.0);
            return;
        }
        editParam(code, 0, value);
    }

    // --- Factory sounds ---------------------------------------------------------------------

    void factoryPatches(Patches& p) {
        p.add("Tine Keys")
            .file(sample, "factory:keys.flac")
            .set(fType, "ladder")
            .set(cutoff, 1800)
            .set(resonance, 15)
            .set(fKeytrack, 50)
            .set(fVelocity, 2.5)
            .set(ampRelease, 350)
            .set(ampVelocity, 60);
        p.add("Air Pad")
            .file(sample, "factory:pad.flac")
            .set(root, 48)
            .set(loopMode, "forward")
            .set(loopStart, 35)
            .set(loopEnd, 95)
            .set(crossfade, 300)
            .set(ampAttack, 400)
            .set(ampRelease, 1500)
            .set(fType, "ladder")
            .set(cutoff, 2600)
            .set(resonance, 20)
            .set(l1Rate, 0.3)
            .set(l1Mode, "free")
            .route("lfo1>filter.cutoff", 0.6, "modwheel");
        p.add("Ping-Pong Pad")
            .file(sample, "factory:pad.flac")
            .set(root, 48)
            .set(loopMode, "ping_pong")
            .set(loopStart, 30)
            .set(loopEnd, 90)
            .set(ampAttack, 200)
            .set(ampRelease, 1200);
        p.add("Reverse Swell")
            .file(sample, "factory:pad.flac")
            .set(mode, "one_shot")
            .set(root, 48)
            .set(reverse, true)
            .set(end, 85);
        p.add("Harp")
            .file(sample, "factory:pluck.flac")
            .set(fType, "ladder")
            .set(cutoff, 1100)
            .set(resonance, 25)
            .set(fEnv, 3)
            .set(fKeytrack, 60)
            .set(envDecay, 350)
            .set(ampRelease, 900)
            .set(ampVelocity, 70);
        p.add("Glide Bass")
            .file(sample, "factory:bass.flac")
            .set(root, 36)
            .set(voiceMode, "legato")
            .set(glide, 80)
            .set(fType, "ladder")
            .set(cutoff, 900)
            .set(resonance, 30)
            .set(fEnv, 2)
            .set(envDecay, 250)
            .set(ampRelease, 80);
        p.add("Chime")
            .file(sample, "factory:bell.flac")
            .set(root, 72)
            .set(ampRelease, 1800)
            .set(l1Rate, 4.5)
            .route("lfo1>voice.tune", 12, "modwheel");
        p.add("Chopped Break")
            .file(sample, "factory:break.flac")
            .set(mode, "slice")
            .set(voiceMode, "mono");
        p.add("Gated Break")
            .file(sample, "factory:break.flac")
            .set(mode, "slice")
            .set(sliceBy, "grid")
            .set(sliceCount, 16)
            .set(slicePlay, "gate")
            .set(ampRelease, 30);
        p.add("Break Thru")
            .file(sample, "factory:break.flac")
            .set(mode, "slice")
            .set(slicePlay, "thru")
            .set(voiceMode, "mono")
            .set(ampRelease, 60);
        p.add("Vowel Chop")
            .file(sample, "factory:voice.flac")
            .set(mode, "slice")
            .set(sliceBy, "grid")
            .set(sliceCount, 8)
            .set(ampAttack, 5)
            .set(ampRelease, 120);
        p.add("First Hit")
            .file(sample, "factory:break.flac")
            .set(mode, "one_shot")
            .set(end, 6)
            .set(keytrack, false);
    }
};
