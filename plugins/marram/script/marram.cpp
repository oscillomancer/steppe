// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Marram: Steppe's synthesizer.
//
// Marram grass holds a dune together by its roots; under the wind its blades
// sing. Each note is one voice, and a voice takes this road:
//
//   Oscillators  four, each off, or a classic analog wave (saw, square,
//                pulse, triangle, sine; hard sync to Osc 1), a wavetable
//                (seven original tables, made here from harmonic recipes),
//                a sine operator for FM (with feedback), noise, or a sub an
//                octave or two down. Any one may frequency-modulate another.
//                Unison stacks up to eight detuned copies of each pitched
//                oscillator across the stereo field, with a shared filter.
//   Filters      two, in series or in parallel: a transistor ladder
//                (12 or 24 dB, saturating), a state-variable filter, a tuned
//                comb, or a bank of vowel formants. Each oscillator goes to
//                one, both, or past them.
//   Amp          its own envelope, with velocity and pan.
//
// The mod envelope (Env 2) moves each filter's cutoff, the pitch and the
// depth of FM by amounts set here; velocity and the key move the cutoff too.
// Drift wanders each voice's pitch and cutoff a little, the way old
// circuits do.
//
// Modulation goes where the patch's routes say: from the amp envelope, Env 2,
// Env 3, three LFOs (each a note's own or one running free, at a rate or in
// time with the song), four macro knobs, and what every instrument has
// (velocity, key, a random value per note, pressure, slide, the mod wheel,
// breath, expression, aftertouch, pitch bend), to almost any number a voice
// reads: "lfo1>voice.tune": {"amount": "20 ct", "via": "modwheel"}.
//
// Everything a voice does runs oversampled (2x by default, 4x at most),
// summed, and brought down once. Modulation is worked out every 16
// samples of a voice's life and glided in between, so a render is the same
// at any block size; every random choice (free-running phases, drift,
// noise) comes from the order of the notes, so a render is the same every
// time.

constexpr int kVoices = 16;
constexpr int kUnison = 8;
constexpr int kTick = 16;  // frames between a voice's control updates
constexpr double kVoiceGain = 0.5;

// Oscillator types and the filters', as their choices number them.
constexpr int kOff = 0;
constexpr int kAnalog = 1;
constexpr int kWavetable = 2;
constexpr int kFm = 3;
constexpr int kNoise = 4;
constexpr int kSub = 5;
constexpr int kLadder = 1;
constexpr int kSvf = 2;
constexpr int kComb = 3;
constexpr int kFormant = 4;

// The wavetables: kTables tables of kFrames frames, each frame kLevels
// band-limited copies (level L holds at most 256 >> L harmonics in
// max(256, 2048 >> L) samples, plus one that repeats the first).
constexpr int kTables = 7;
constexpr int kFrames = 16;
constexpr int kLevels = 9;
constexpr int kTop = 256;
constexpr int kFft = 2048;
constexpr int kPerFrame = 5129;  // 2049 + 1025 + 513 + 6 * 257

// --- small pieces ------------------------------------------------------------------------

double wrap01(double x) { return x - floor(x); }

// The polyBLEP residual of a unit step at phase 0, and its integral (for a
// change of slope), t the phase and dt its step per sample.
double blep(double t, double dt) {
    if (t < dt) {
        const double x = t / dt;
        return x + x - x * x - 1.0;
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}
double blamp(double t, double dt) {
    if (t < dt) {
        const double x = t / dt - 1.0;
        return -x * x * x / 3.0;
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt + 1.0;
        return x * x * x / 3.0;
    }
    return 0.0;
}

double pulseWave(double t, double dt, double w) {
    const double naive = t < w ? 1.0 : -1.0;
    return naive + blep(t, dt) - blep(wrap01(t - w), dt);
}

// Saw, square, pulse, triangle, sine: band-limited where they break.
double analogWave(int wave, double t, double dt, double width) {
    if (wave == 0) return 2.0 * t - 1.0 - blep(t, dt);
    if (wave == 1) return pulseWave(t, dt, 0.5);
    if (wave == 2) return pulseWave(t, dt, width);
    if (wave == 3) {
        const double naive = t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
        return naive + 8.0 * dt * (blamp(t, dt) - blamp(wrap01(t - 0.5), dt));
    }
    return sin(twoPi * t);
}

// tanh, near enough, and exactly +-1 beyond 4.97.
double fastTanh(double x) {
    if (x > 4.97) return 1.0;
    if (x < -4.97) return -1.0;
    const double x2 = x * x;
    return x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2))) /
           (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)));
}

// An envelope's bend: x 0..1 along a segment; c 0 straight, toward 1 an
// exponential that starts fast and settles slowly when falling.
double rise(double x, double c) {
    if (c == 0.0) return x;
    const double k = c * 6.0;
    return (exp(k * x) - 1.0) / (exp(k) - 1.0);
}
double fall(double x, double c) { return 1.0 - rise(1.0 - x, c); }

// The digital table's way between two frames (x 0..1): it holds one, then
// jumps to the next over the middle quarter.
double stepped(double x) {
    const double e = clamp((x - 0.375) * 4.0, 0.0, 1.0);
    return e * e * (3.0 - 2.0 * e);
}

// The sparse digital frames' (8 to 11) harmonic i (0: none) and its level.
int sparseHarmonic(int f, int i) {
    if (i == 0) return 1;
    if (f == 8) return i == 1 ? 7 : i == 2 ? 11 : 0;
    if (f == 9) return i == 1 ? 9 : i == 2 ? 13 : i == 3 ? 14 : 0;
    if (f == 10) return i == 1 ? 11 : i == 2 ? 17 : i == 3 ? 19 : i == 4 ? 23 : 0;
    return i == 1 ? 13 : i == 2 ? 21 : i == 3 ? 22 : i == 4 ? 29 : i == 5 ? 31 : 0;
}
double sparseLevel(int f, int i) {
    if (i == 0) return f == 10 ? 0.8 : f == 11 ? 0.7 : 1.0;
    if (i == 2) return f == 8 ? 0.6 : 0.7;
    return i == 1 ? 0.8 : i == 3 ? 0.5 : i == 4 ? 0.4 : 0.3;
}

// The digital table's frame f (1..15) as the steps of one cycle, into v; how
// many there are. Frame 0 is the saw. Four of each kind: the saw held on 8,
// 5 and 3 steps; a sine and two bent waves crushed to a few levels; sparse
// high harmonics at 4 bits; and 1-bit patterns. Each wave is read in the
// middle of its steps, never on a level's edge, so every machine makes it
// alike.
int digitalSteps(int f, Array<double, 64>& v) {
    if (f <= 3) {
        const int n = f == 1 ? 8 : f == 2 ? 5 : 3;
        for (int j = 0; j < n; ++j) v[j] = (2.0 * j + 1.0) / n - 1.0;
        return n;
    }
    if (f == 13) {
        // A maximal-length sequence of 15: every harmonic below the 15th as
        // strong as the others.
        int bits = 3929;  // the sequence, first step in the lowest bit
        for (int j = 0; j < 15; ++j) {
            v[j] = bits % 2 == 1 ? 1.0 : -1.0;
            bits = bits / 2;
        }
        return 15;
    }
    const int n = f >= 7 && f <= 11 ? 64 : 32;
    double most = 0.0;
    for (int j = 0; j < n; ++j) {
        const double w = twoPi * (j + 0.5) / n;
        double x = 0.0;
        if (f == 4)
            x = round(sin(w) * 3.0) / 3.0;
        else if (f == 5)
            x = round((0.6 * sin(w) + 0.5 * sin(2.0 * w)) * 2.0) / 2.0;
        else if (f == 6)
            x = round(sin(w + 1.2 * sin(3.0 * w)));
        else if (f == 7)
            x = sin(w) + 0.9 * sin(4.0 * w + 0.7) >= 0.0 ? 1.0 : -1.0;
        else if (f <= 11) {
            for (int i = 0; i < 6; ++i) x += sparseLevel(f, i) * sin(sparseHarmonic(f, i) * w);
            most = max(most, abs(x));
        } else if (f == 12)
            x = sin(w) + 0.8 * sin(6.0 * w + 1.1) >= 0.0 ? 1.0 : -1.0;
        else if (f == 14)
            x = sin(w) + sin(10.0 * w + 0.3) >= 0.0 ? 1.0 : -1.0;
        else
            x = sin(w) + 0.7 * sin(2.0 * w + 0.5) + 0.6 * sin(9.0 * w + 1.0) >= 0.0 ? 1.0 : -1.0;
        v[j] = x;
    }
    if (most > 0.0)
        for (int j = 0; j < n; ++j) v[j] = round(v[j] / most * 8.0) / 8.0;
    return n;
}

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

// --- the voice ---------------------------------------------------------------------------

// A DAHDSR without the D and H: attack, decay, sustain, release, each segment
// exactly as long as its time and bent: the attack straight, the decay and
// release exponential. It starts from wherever it is, so retriggering never
// clicks.
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

// One channel of one filter slot: every type's state, so switching types
// never reads another's.
struct Filter {
    Array<double, 4> stage;  // the ladder's
    double ic1 = 0.0;        // the state-variable filter's
    double ic2 = 0.0;
    Array<double, 6> band;  // the formant bank's three
    DelayLine comb;
    double combLp = 0.0;

    // The other channel's state, as it goes from one channel to two (the
    // comb's line, never fed while there was one, stays empty).
    void follow(const Filter& o) {
        stage = o.stage;
        ic1 = o.ic1;
        ic2 = o.ic2;
        band = o.band;
        combLp = 0.0;
    }

    void clear() {
        stage.fill(0.0);
        ic1 = 0.0;
        ic2 = 0.0;
        band.fill(0.0);
        combLp = 0.0;
    }

    // A transistor ladder: four one-poles with their feedback solved at once
    // (no delay), the input saturating. g the integrators' gain, k 0..4 (it
    // sings near 4), drive a gain.
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

    // Band b of the formant bank, a band pass with 0 dB at its peak.
    double formantBand(int b, double x, double a1, double a2, double a3, double k) {
        const double s1 = band[b * 2];
        const double s2 = band[b * 2 + 1];
        const double v3 = x - s2;
        const double v1 = a1 * s1 + a2 * v3;
        const double v2 = s2 + a2 * s1 + a3 * v3;
        band[b * 2] = 2.0 * v1 - s1;
        band[b * 2 + 1] = 2.0 * v2 - s2;
        return v1 * k;
    }

    // A feedback comb, delay samples long, darkening in its loop.
    double combFilter(double x, double delay, double feedback, double damping) {
        const double tap = comb.read(delay - 1.0);
        combLp += (tap - combLp) * (1.0 - damping * 0.95);
        const double y = x + feedback * combLp;
        comb.push(y);
        return y * (1.0 - abs(feedback) * 0.5);
    }
};

// A filter slot's settings for the next tick, and the glides toward them.
struct FilterNow {
    double g = 0.0;
    double gStep = 0.0;
    double k = 0.0;
    double kStep = 0.0;
    double delay = 1.0;
    double delayStep = 0.0;
    double drive = 1.0;
    double feedback = 0.0;
    double damping = 0.0;
    // The formant coefficients stay fixed until the next control tick.
    Array<double, 3> fa1;
    Array<double, 3> fa2;
    Array<double, 3> fa3;
    Array<double, 3> fk;
};

struct Voice {
    // Per oscillator and unison copy, at o * 8 + u.
    Array<double, 32> phase;
    Array<double, 32> inc;  // cycles per (oversampled) sample, gliding
    Array<double, 32> incStep;
    Array<double, 32> out;  // the copy's last sample, for FM
    Array<double, 32> fb1;  // an FM operator's last two outputs
    Array<double, 32> fb2;
    Array<double, 32> panL;
    Array<double, 32> panR;
    Array<double, 32> panAt;   // pan used to compute each copy's gains
    Array<double, 32> wander;  // each copy's own drift, a standard normal
    Array<double, 8> wrapped;  // where Osc 1's copies wrapped, for sync; -1 not

    // Per oscillator, as of the last tick.
    Array<int, 4> type;
    Array<int, 4> copies;
    Array<double, 4> gain;
    Array<double, 4> gainStep;
    Array<double, 4> fm;  // the depth, in cycles
    Array<int, 4> from;
    Array<double, 4> noise;  // this frame's noise sample
    Array<double, 4> lastPink;
    Array<int, 4> base0;  // where a wavetable's frames start
    Array<int, 4> base1;
    Array<int, 4> size;
    Array<double, 4> blend;
    Array<bool, 4> saw;  // between a table's first two frames, and the first is the saw
    Array<double, 4> mult;
    Array<Noise, 4> noises;

    Array<Filter, 4> filter;  // filter 1 left, right, filter 2 left, right
    Array<FilterNow, 2> now;

    Env amp;
    Env mod;
    Env env3;
    // The LFOs a note has of its own: phase (in cycles), whole cycles, and a
    // seed for random; and how long the note has played, for their delay.
    Array<double, 3> lfoPhase;
    Array<int, 3> lfoCycle;
    int lfoSeed = 0;
    double age = 0.0;
    // As the routes leave them at the last tick: each oscillator's pulse
    // width and FM feedback.
    Array<double, 4> width;
    Array<double, 4> feedback;
    double ampL = 0.0;
    double ampLStep = 0.0;
    double ampR = 0.0;
    double ampRStep = 0.0;
    double ampPanAt = 2.0;  // outside the pan's range until first computed
    double ampCos = 0.0;
    double ampSin = 0.0;
    double velocity = 0.0;
    bool sounding = false;
    bool fresh = false;
    bool done = false;
    bool narrow = true;
    bool wasCentred = true;
    bool ending = false;  // the amp's last target was silence
    int tickLeft = 0;
    double lastL = 0.0;
    double lastR = 0.0;
    // Drift: two slow random walks, pitch and cutoff.
    double driftAt = 0.0;
    double pitchFrom = 0.0;
    double pitchTo = 0.0;
    double cutFrom = 0.0;
    double cutTo = 0.0;
};

// An oscillator's settings, from its parameters.
struct OscSet {
    int type = 0;
    int coarse = 0;
    int wave = 0;
    double width = 0.5;
    bool sync = false;
    int table = 0;
    double position = 0.0;
    double ratio = 1.0;
    double feedback = 0.0;
    int color = 0;
    int octave = -1;
    int from = 0;
    int to = 0;       // filter 1, filter 2, both, direct
    bool toA = true;  // and so: into filter 1's input, filter 2's, or past them
    bool toB = false;
    bool direct = false;
    bool retrigger = false;
};

// An oscillator's settings as a voice hears them, with their routes.
struct OscNow {
    double gain = 0.0;
    double pan = 0.0;
    double tune = 0.0;
    double width = 0.5;
    double position = 0.0;
    double ratio = 1.0;
    double feedback = 0.0;
    double fm = 0.0;
};

// And a filter's.
struct FilterRead {
    double cutoff = 1000.0;
    double resonance = 0.0;
    double feedback = 0.0;
    double damping = 0.0;
    double vowel = 0.0;
    double shift = 0.0;
    double drive = 1.0;
};

struct FilterSet {
    int type = 0;
    double cutoff = 1000.0;
    double resonance = 0.0;
    bool steep = true;
    int mode = 0;
    double feedback = 0.0;
    double damping = 0.0;
    double vowel = 0.0;
    double shift = 0.0;
    double keytrack = 0.0;
    double env = 0.0;
    double velocity = 0.0;
};

struct Marram : Instrument {
    static constexpr Info info = {
        .name = "Marram",
        .id = "com.suppleaudio.marram",
        .vendor = "Supple Audio",
        .description =
            "Synthesizer: four oscillators (analog, wavetable, FM, noise, sub) with unison, two "
            "filters (ladder, state-variable, comb, formant), three envelopes, three LFOs "
            "and four macros, connected by modulation routes.",
        .features = {"synthesizer"},
        .voices = kVoices};

    // --- Voice ----------------------------------------------------------------------------

    Param mode = choice("Mode", {"Poly", "Mono", "Legato"}, 0)
                     .withKey("voice.mode")
                     .in("Voice")
                     .help(
                         "Poly plays chords; Mono one note at a time, each restarting the "
                         "envelopes; Legato one at a time, gliding without restarting them.");
    Param polyphony =
        integer("Polyphony", 1, kVoices, kVoices)
            .withKey("voice.polyphony")
            .in("Voice")
            .activeWhen(mode, {"poly"})
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
                         "Fine tuning of every oscillator. Routes move the whole voice's pitch "
                         "through it: an LFO's vibrato, an envelope's sweep.");
    Param unison =
        integer("Unison", 1, kUnison, 1)
            .withKey("voice.unison")
            .in("Voice")
            .help(
                "Detuned copies of each pitched oscillator per note: 3 thickens, 7 is a supersaw.");
    Param detune = cents("Unison Detune", 0, 100, 12)
                       .withKey("voice.unison_detune")
                       .in("Voice")
                       .activeAbove(unison, 1)
                       .help("How far the outermost copies are detuned, each way.");
    Param spread = percent("Unison Spread", 60)
                       .withKey("voice.unison_spread")
                       .in("Voice")
                       .activeAbove(unison, 1)
                       .help("How wide the copies spread across the stereo field.");
    Param drift = percent("Drift", 0)
                      .withKey("voice.drift")
                      .in("Voice")
                      .help(
                          "Slow random wander of each voice's pitch (up to 5 cents, and each "
                          "oscillator its own) and cutoff, like old analog circuits.");
    Param oversampling = choice("Oversampling", {"1x", "2x", "4x"}, 1)
                             .keys({"x1", "x2", "x4"})
                             .withKey("voice.oversampling")
                             .in("Voice")
                             .restarts()
                             .help("Oversampling, for heavy FM, sync and drive.");

    // --- Oscillators --------------------------------------------------------------------

    Param o1Type = choice("Osc 1 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 1)
                       .withKey("osc1.type")
                       .in("Osc 1")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o1Level = decibels("Osc 1 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc1.level")
                        .polyModulated()
                        .in("Osc 1")
                        .activeUnless(o1Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o1Pan = percent("Osc 1 Pan", 0, -100, 100)
                      .withKey("osc1.pan")
                      .polyModulated()
                      .in("Osc 1")
                      .activeUnless(o1Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o1Coarse = integer("Osc 1 Coarse", -48, 48, 0)
                         .withKey("osc1.coarse")
                         .in("Osc 1")
                         .activeUnless(o1Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o1Fine =
        cents("Osc 1 Fine", -100, 100, 0)
            .withKey("osc1.fine")
            .polyModulated()
            .routeRange(-4800, 4800)
            .in("Osc 1")
            .activeUnless(o1Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o1Wave = choice("Osc 1 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc1.wave")
                       .in("Osc 1")
                       .activeWhen(o1Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o1Width =
        percent("Osc 1 Width", 25, 1, 99)
            .withKey("osc1.width")
            .polyModulated()
            .in("Osc 1")
            .activeWhen(o1Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o1Table =
        choice("Osc 1 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc1.table")
            .in("Osc 1")
            .activeWhen(o1Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a saw falling into crushed and 1-bit waves, or a comb through the "
                "harmonics.");
    Param o1Position = percent("Osc 1 Position", 0)
                           .withKey("osc1.position")
                           .polyModulated()
                           .in("Osc 1")
                           .activeWhen(o1Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o1Ratio = continuous("Osc 1 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc1.ratio")
                        .polyModulated()
                        .in("Osc 1")
                        .activeWhen(o1Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o1Feedback =
        percent("Osc 1 Feedback", 0)
            .withKey("osc1.feedback")
            .polyModulated()
            .in("Osc 1")
            .activeWhen(o1Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o1Color =
        choice("Osc 1 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc1.color")
            .in("Osc 1")
            .activeWhen(o1Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o1Octave = integer("Osc 1 Octave", -2, -1, -1)
                         .withKey("osc1.octave")
                         .in("Osc 1")
                         .activeWhen(o1Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o1Fm = continuous("Osc 1 FM", 0, 10, 0)
                     .withKey("osc1.fm")
                     .polyModulated()
                     .in("Osc 1")
                     .activeWhen(o1Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o1FmFrom = choice("Osc 1 FM From", {"Osc 2", "Osc 3", "Osc 4"}, 0)
                         .keys({"osc2", "osc3", "osc4"})
                         .withKey("osc1.fm_from")
                         .in("Osc 1")
                         .activeAbove(o1Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o1To = choice("Osc 1 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc1.to")
                     .in("Osc 1")
                     .activeUnless(o1Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o1Retrigger = toggle("Osc 1 Retrigger", false)
                            .withKey("osc1.retrigger")
                            .in("Osc 1")
                            .activeUnless(o1Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    Param o2Type = choice("Osc 2 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 0)
                       .withKey("osc2.type")
                       .in("Osc 2")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o2Level = decibels("Osc 2 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc2.level")
                        .polyModulated()
                        .in("Osc 2")
                        .activeUnless(o2Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o2Pan = percent("Osc 2 Pan", 0, -100, 100)
                      .withKey("osc2.pan")
                      .polyModulated()
                      .in("Osc 2")
                      .activeUnless(o2Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o2Coarse = integer("Osc 2 Coarse", -48, 48, 0)
                         .withKey("osc2.coarse")
                         .in("Osc 2")
                         .activeUnless(o2Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o2Fine =
        cents("Osc 2 Fine", -100, 100, 0)
            .withKey("osc2.fine")
            .polyModulated()
            .routeRange(-4800, 4800)
            .in("Osc 2")
            .activeUnless(o2Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o2Wave = choice("Osc 2 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc2.wave")
                       .in("Osc 2")
                       .activeWhen(o2Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o2Width =
        percent("Osc 2 Width", 25, 1, 99)
            .withKey("osc2.width")
            .polyModulated()
            .in("Osc 2")
            .activeWhen(o2Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o2Sync = toggle("Osc 2 Sync", false)
                       .withKey("osc2.sync")
                       .in("Osc 2")
                       .activeWhen(o2Type, {"analog"})
                       .help(
                           "Hard sync: restart its cycle with Osc 1's, so tuning it up sweeps a "
                           "tearing formant.");
    Param o2Table =
        choice("Osc 2 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc2.table")
            .in("Osc 2")
            .activeWhen(o2Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a saw falling into crushed and 1-bit waves, or a comb through the "
                "harmonics.");
    Param o2Position = percent("Osc 2 Position", 0)
                           .withKey("osc2.position")
                           .polyModulated()
                           .in("Osc 2")
                           .activeWhen(o2Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o2Ratio = continuous("Osc 2 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc2.ratio")
                        .polyModulated()
                        .in("Osc 2")
                        .activeWhen(o2Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o2Feedback =
        percent("Osc 2 Feedback", 0)
            .withKey("osc2.feedback")
            .polyModulated()
            .in("Osc 2")
            .activeWhen(o2Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o2Color =
        choice("Osc 2 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc2.color")
            .in("Osc 2")
            .activeWhen(o2Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o2Octave = integer("Osc 2 Octave", -2, -1, -1)
                         .withKey("osc2.octave")
                         .in("Osc 2")
                         .activeWhen(o2Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o2Fm = continuous("Osc 2 FM", 0, 10, 0)
                     .withKey("osc2.fm")
                     .polyModulated()
                     .in("Osc 2")
                     .activeWhen(o2Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o2FmFrom = choice("Osc 2 FM From", {"Osc 1", "Osc 3", "Osc 4"}, 0)
                         .keys({"osc1", "osc3", "osc4"})
                         .withKey("osc2.fm_from")
                         .in("Osc 2")
                         .activeAbove(o2Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o2To = choice("Osc 2 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc2.to")
                     .in("Osc 2")
                     .activeUnless(o2Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o2Retrigger = toggle("Osc 2 Retrigger", false)
                            .withKey("osc2.retrigger")
                            .in("Osc 2")
                            .activeUnless(o2Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    Param o3Type = choice("Osc 3 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 0)
                       .withKey("osc3.type")
                       .in("Osc 3")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o3Level = decibels("Osc 3 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc3.level")
                        .polyModulated()
                        .in("Osc 3")
                        .activeUnless(o3Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o3Pan = percent("Osc 3 Pan", 0, -100, 100)
                      .withKey("osc3.pan")
                      .polyModulated()
                      .in("Osc 3")
                      .activeUnless(o3Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o3Coarse = integer("Osc 3 Coarse", -48, 48, 0)
                         .withKey("osc3.coarse")
                         .in("Osc 3")
                         .activeUnless(o3Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o3Fine =
        cents("Osc 3 Fine", -100, 100, 0)
            .withKey("osc3.fine")
            .polyModulated()
            .routeRange(-4800, 4800)
            .in("Osc 3")
            .activeUnless(o3Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o3Wave = choice("Osc 3 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc3.wave")
                       .in("Osc 3")
                       .activeWhen(o3Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o3Width =
        percent("Osc 3 Width", 25, 1, 99)
            .withKey("osc3.width")
            .polyModulated()
            .in("Osc 3")
            .activeWhen(o3Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o3Sync = toggle("Osc 3 Sync", false)
                       .withKey("osc3.sync")
                       .in("Osc 3")
                       .activeWhen(o3Type, {"analog"})
                       .help(
                           "Hard sync: restart its cycle with Osc 1's, so tuning it up sweeps a "
                           "tearing formant.");
    Param o3Table =
        choice("Osc 3 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc3.table")
            .in("Osc 3")
            .activeWhen(o3Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a saw falling into crushed and 1-bit waves, or a comb through the "
                "harmonics.");
    Param o3Position = percent("Osc 3 Position", 0)
                           .withKey("osc3.position")
                           .polyModulated()
                           .in("Osc 3")
                           .activeWhen(o3Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o3Ratio = continuous("Osc 3 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc3.ratio")
                        .polyModulated()
                        .in("Osc 3")
                        .activeWhen(o3Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o3Feedback =
        percent("Osc 3 Feedback", 0)
            .withKey("osc3.feedback")
            .polyModulated()
            .in("Osc 3")
            .activeWhen(o3Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o3Color =
        choice("Osc 3 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc3.color")
            .in("Osc 3")
            .activeWhen(o3Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o3Octave = integer("Osc 3 Octave", -2, -1, -1)
                         .withKey("osc3.octave")
                         .in("Osc 3")
                         .activeWhen(o3Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o3Fm = continuous("Osc 3 FM", 0, 10, 0)
                     .withKey("osc3.fm")
                     .polyModulated()
                     .in("Osc 3")
                     .activeWhen(o3Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o3FmFrom = choice("Osc 3 FM From", {"Osc 1", "Osc 2", "Osc 4"}, 0)
                         .keys({"osc1", "osc2", "osc4"})
                         .withKey("osc3.fm_from")
                         .in("Osc 3")
                         .activeAbove(o3Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o3To = choice("Osc 3 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc3.to")
                     .in("Osc 3")
                     .activeUnless(o3Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o3Retrigger = toggle("Osc 3 Retrigger", false)
                            .withKey("osc3.retrigger")
                            .in("Osc 3")
                            .activeUnless(o3Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    Param o4Type = choice("Osc 4 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 0)
                       .withKey("osc4.type")
                       .in("Osc 4")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o4Level = decibels("Osc 4 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc4.level")
                        .polyModulated()
                        .in("Osc 4")
                        .activeUnless(o4Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o4Pan = percent("Osc 4 Pan", 0, -100, 100)
                      .withKey("osc4.pan")
                      .polyModulated()
                      .in("Osc 4")
                      .activeUnless(o4Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o4Coarse = integer("Osc 4 Coarse", -48, 48, 0)
                         .withKey("osc4.coarse")
                         .in("Osc 4")
                         .activeUnless(o4Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o4Fine =
        cents("Osc 4 Fine", -100, 100, 0)
            .withKey("osc4.fine")
            .polyModulated()
            .routeRange(-4800, 4800)
            .in("Osc 4")
            .activeUnless(o4Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o4Wave = choice("Osc 4 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc4.wave")
                       .in("Osc 4")
                       .activeWhen(o4Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o4Width =
        percent("Osc 4 Width", 25, 1, 99)
            .withKey("osc4.width")
            .polyModulated()
            .in("Osc 4")
            .activeWhen(o4Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o4Sync = toggle("Osc 4 Sync", false)
                       .withKey("osc4.sync")
                       .in("Osc 4")
                       .activeWhen(o4Type, {"analog"})
                       .help(
                           "Hard sync: restart its cycle with Osc 1's, so tuning it up sweeps a "
                           "tearing formant.");
    Param o4Table =
        choice("Osc 4 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc4.table")
            .in("Osc 4")
            .activeWhen(o4Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a saw falling into crushed and 1-bit waves, or a comb through the "
                "harmonics.");
    Param o4Position = percent("Osc 4 Position", 0)
                           .withKey("osc4.position")
                           .polyModulated()
                           .in("Osc 4")
                           .activeWhen(o4Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o4Ratio = continuous("Osc 4 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc4.ratio")
                        .polyModulated()
                        .in("Osc 4")
                        .activeWhen(o4Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o4Feedback =
        percent("Osc 4 Feedback", 0)
            .withKey("osc4.feedback")
            .polyModulated()
            .in("Osc 4")
            .activeWhen(o4Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o4Color =
        choice("Osc 4 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc4.color")
            .in("Osc 4")
            .activeWhen(o4Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o4Octave = integer("Osc 4 Octave", -2, -1, -1)
                         .withKey("osc4.octave")
                         .in("Osc 4")
                         .activeWhen(o4Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o4Fm = continuous("Osc 4 FM", 0, 10, 0)
                     .withKey("osc4.fm")
                     .polyModulated()
                     .in("Osc 4")
                     .activeWhen(o4Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o4FmFrom = choice("Osc 4 FM From", {"Osc 1", "Osc 2", "Osc 3"}, 0)
                         .keys({"osc1", "osc2", "osc3"})
                         .withKey("osc4.fm_from")
                         .in("Osc 4")
                         .activeAbove(o4Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o4To = choice("Osc 4 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc4.to")
                     .in("Osc 4")
                     .activeUnless(o4Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o4Retrigger = toggle("Osc 4 Retrigger", false)
                            .withKey("osc4.retrigger")
                            .in("Osc 4")
                            .activeUnless(o4Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    // --- Filters -------------------------------------------------------------------------

    Param f1Type = choice("Filter 1 Type", {"Off", "Ladder", "SVF", "Comb", "Formant"}, 1)
                       .withKey("filter1.type")
                       .in("Filter 1")
                       .help(
                           "A warm transistor ladder, a clean state-variable filter, a tuned comb, "
                           "or a bank of vowel formants.");
    Param f1Cutoff = frequency("Filter 1 Cutoff", 20, 20000, 2000)
                         .withKey("filter1.cutoff")
                         .polyModulated()
                         .in("Filter 1")
                         .activeWhen(f1Type, {"ladder", "svf", "comb"})
                         .help(
                             "The corner frequency (a comb's pitch). Key tracking, the envelope "
                             "and velocity move it from here.");
    Param f1Resonance = percent("Filter 1 Resonance", 10)
                            .withKey("filter1.resonance")
                            .polyModulated()
                            .in("Filter 1")
                            .activeWhen(f1Type, {"ladder", "svf"})
                            .help(
                                "The peak at the cutoff: 40 % a bump, 70 % clearly resonant, near "
                                "100 % it rings and the ladder sings.");
    Param f1Slope = choice("Filter 1 Slope", {"12 dB/oct", "24 dB/oct"}, 1)
                        .keys({"db12", "db24"})
                        .withKey("filter1.slope")
                        .in("Filter 1")
                        .activeWhen(f1Type, {"ladder"})
                        .help("How steeply the ladder cuts above the cutoff.");
    Param f1Mode = choice("Filter 1 Mode", {"Low Pass", "Band Pass", "High Pass", "Notch"}, 0)
                       .keys({"lowpass", "bandpass", "highpass", "notch"})
                       .withKey("filter1.mode")
                       .in("Filter 1")
                       .activeWhen(f1Type, {"svf"})
                       .help("What the state-variable filter keeps.");
    Param f1Feedback = percent("Filter 1 Feedback", 50, -99, 99)
                           .withKey("filter1.feedback")
                           .polyModulated()
                           .in("Filter 1")
                           .activeWhen(f1Type, {"comb"})
                           .help(
                               "How long the comb rings; negative feedback keeps the odd "
                               "harmonics, an octave down and hollow.");
    Param f1Damping = percent("Filter 1 Damping", 20)
                          .withKey("filter1.damping")
                          .polyModulated()
                          .in("Filter 1")
                          .activeWhen(f1Type, {"comb"})
                          .help("How much the comb darkens as it rings, like a plucked string.");
    Param f1Vowel =
        continuous("Filter 1 Vowel", 0, 4, 0)
            .withKey("filter1.vowel")
            .polyModulated()
            .in("Filter 1")
            .activeWhen(f1Type, {"formant"})
            .help("The vowel, from a (0) through e, i and o to u (4), blending between.");
    Param f1Shift = semitones("Filter 1 Shift", -12, 12, 0)
                        .withKey("filter1.shift")
                        .polyModulated()
                        .in("Filter 1")
                        .activeWhen(f1Type, {"formant"})
                        .help("Moves the formants up or down: a smaller or larger throat.");
    Param f1Keytrack = percent("Filter 1 Keytrack", 0, 0, 200)
                           .withKey("filter1.keytrack")
                           .in("Filter 1")
                           .activeUnless(f1Type, {"off"})
                           .help(
                               "How far the cutoff follows the note from middle C: 100 % moves it "
                               "an octave per octave.");
    Param f1Env =
        octaves("Filter 1 Env", -8, 8, 2)
            .withKey("filter1.env")
            .in("Filter 1")
            .activeUnless(f1Type, {"off"})
            .help("How far the mod envelope (Env 2) moves the cutoff at its peak, in octaves.");
    Param f1Velocity =
        octaves("Filter 1 Velocity", 0, 4, 0)
            .withKey("filter1.velocity")
            .in("Filter 1")
            .activeUnless(f1Type, {"off"})
            .help("How far the hardest notes open the cutoff, in octaves; softer ones less.");
    Param f1Drive = decibels("Filter 1 Drive", 0, 24, 0)
                        .withKey("filter1.drive")
                        .polyModulated()
                        .in("Filter 1")
                        .activeUnless(f1Type, {"off"})
                        .help(
                            "Gain into the filter: the ladder saturates and thickens, the others "
                            "simply get louder.");

    Param f2Type = choice("Filter 2 Type", {"Off", "Ladder", "SVF", "Comb", "Formant"}, 0)
                       .withKey("filter2.type")
                       .in("Filter 2")
                       .help(
                           "A warm transistor ladder, a clean state-variable filter, a tuned comb, "
                           "or a bank of vowel formants.");
    Param f2Cutoff = frequency("Filter 2 Cutoff", 20, 20000, 1000)
                         .withKey("filter2.cutoff")
                         .polyModulated()
                         .in("Filter 2")
                         .activeWhen(f2Type, {"ladder", "svf", "comb"})
                         .help(
                             "The corner frequency (a comb's pitch). Key tracking, the envelope "
                             "and velocity move it from here.");
    Param f2Resonance = percent("Filter 2 Resonance", 10)
                            .withKey("filter2.resonance")
                            .polyModulated()
                            .in("Filter 2")
                            .activeWhen(f2Type, {"ladder", "svf"})
                            .help(
                                "The peak at the cutoff: 40 % a bump, 70 % clearly resonant, near "
                                "100 % it rings and the ladder sings.");
    Param f2Slope = choice("Filter 2 Slope", {"12 dB/oct", "24 dB/oct"}, 1)
                        .keys({"db12", "db24"})
                        .withKey("filter2.slope")
                        .in("Filter 2")
                        .activeWhen(f2Type, {"ladder"})
                        .help("How steeply the ladder cuts above the cutoff.");
    Param f2Mode = choice("Filter 2 Mode", {"Low Pass", "Band Pass", "High Pass", "Notch"}, 0)
                       .keys({"lowpass", "bandpass", "highpass", "notch"})
                       .withKey("filter2.mode")
                       .in("Filter 2")
                       .activeWhen(f2Type, {"svf"})
                       .help("What the state-variable filter keeps.");
    Param f2Feedback = percent("Filter 2 Feedback", 50, -99, 99)
                           .withKey("filter2.feedback")
                           .polyModulated()
                           .in("Filter 2")
                           .activeWhen(f2Type, {"comb"})
                           .help(
                               "How long the comb rings; negative feedback keeps the odd "
                               "harmonics, an octave down and hollow.");
    Param f2Damping = percent("Filter 2 Damping", 20)
                          .withKey("filter2.damping")
                          .polyModulated()
                          .in("Filter 2")
                          .activeWhen(f2Type, {"comb"})
                          .help("How much the comb darkens as it rings, like a plucked string.");
    Param f2Vowel =
        continuous("Filter 2 Vowel", 0, 4, 0)
            .withKey("filter2.vowel")
            .polyModulated()
            .in("Filter 2")
            .activeWhen(f2Type, {"formant"})
            .help("The vowel, from a (0) through e, i and o to u (4), blending between.");
    Param f2Shift = semitones("Filter 2 Shift", -12, 12, 0)
                        .withKey("filter2.shift")
                        .polyModulated()
                        .in("Filter 2")
                        .activeWhen(f2Type, {"formant"})
                        .help("Moves the formants up or down: a smaller or larger throat.");
    Param f2Keytrack = percent("Filter 2 Keytrack", 0, 0, 200)
                           .withKey("filter2.keytrack")
                           .in("Filter 2")
                           .activeUnless(f2Type, {"off"})
                           .help(
                               "How far the cutoff follows the note from middle C: 100 % moves it "
                               "an octave per octave.");
    Param f2Env =
        octaves("Filter 2 Env", -8, 8, 0)
            .withKey("filter2.env")
            .in("Filter 2")
            .activeUnless(f2Type, {"off"})
            .help("How far the mod envelope (Env 2) moves the cutoff at its peak, in octaves.");
    Param f2Velocity =
        octaves("Filter 2 Velocity", 0, 4, 0)
            .withKey("filter2.velocity")
            .in("Filter 2")
            .activeUnless(f2Type, {"off"})
            .help("How far the hardest notes open the cutoff, in octaves; softer ones less.");
    Param f2Drive = decibels("Filter 2 Drive", 0, 24, 0)
                        .withKey("filter2.drive")
                        .polyModulated()
                        .in("Filter 2")
                        .activeUnless(f2Type, {"off"})
                        .help(
                            "Gain into the filter: the ladder saturates and thickens, the others "
                            "simply get louder.");

    Param routing = choice("Filter Routing", {"Serial", "Parallel"}, 0)
                        .withKey("filters.routing")
                        .in("Filter 2")
                        .activeUnless(f1Type, {"off"})
                        .activeUnless(f2Type, {"off"})
                        .help(
                            "Serial runs filter 1 into filter 2; parallel runs them side by side "
                            "and mixes them.");

    // --- Amp, envelopes ------------------------------------------------------------------

    Param ampAttack = milliseconds("Amp Attack", 0, 30000, 2)
                          .withKey("amp.attack")
                          .polyModulated()
                          .in("Amp")
                          .help("How long a note takes to rise.");
    Param ampDecay = milliseconds("Amp Decay", 1, 30000, 600)
                         .withKey("amp.decay")
                         .polyModulated()
                         .in("Amp")
                         .help("How long it then takes to fall to the sustain level.");
    Param ampSustain = percent("Amp Sustain", 80)
                           .withKey("amp.sustain")
                           .in("Amp")
                           .help("The level held while the key is down.");
    Param ampRelease = milliseconds("Amp Release", 1, 30000, 250)
                           .withKey("amp.release")
                           .polyModulated()
                           .in("Amp")
                           .help("How long the note takes to die away once the key is up.");
    Param ampVelocity =
        percent("Amp Velocity", 50)
            .withKey("amp.velocity")
            .in("Amp")
            .help("How much softer quiet notes play: at 100 % the softest is 40 dB down.");
    Param ampLevel =
        decibels("Amp Level", -48, 12, 0)
            .withKey("amp.level")
            .polyModulated()
            .in("Amp")
            .help("Each voice's level after the envelope; routes make a tremolo of it.");
    Param ampPan = percent("Pan", 0, -100, 100)
                       .withKey("amp.pan")
                       .polyModulated()
                       .in("Amp")
                       .help("Where the whole sound sits, left to right.");

    Param envAttack = milliseconds("Env 2 Attack", 0, 30000, 1)
                          .withKey("env2.attack")
                          .polyModulated()
                          .in("Env 2")
                          .help(
                              "The mod envelope's rise. It moves the cutoffs, the pitch and FM by "
                              "the amounts set for each.");
    Param envDecay = milliseconds("Env 2 Decay", 1, 30000, 400)
                         .withKey("env2.decay")
                         .polyModulated()
                         .in("Env 2")
                         .help("Its fall to the sustain level.");
    Param envSustain = percent("Env 2 Sustain", 20)
                           .withKey("env2.sustain")
                           .in("Env 2")
                           .help("Its level while the key is down.");
    Param envRelease = milliseconds("Env 2 Release", 1, 30000, 300)
                           .withKey("env2.release")
                           .polyModulated()
                           .in("Env 2")
                           .help("Its fall once the key is up.");
    Param envPitch = semitones("Env 2 Pitch", -48, 48, 0)
                         .withKey("env2.pitch")
                         .in("Env 2")
                         .help(
                             "How far it bends every oscillator's pitch at its peak: a drum's or a "
                             "zap's sweep.");
    Param envFm = percent("Env 2 FM", 0)
                      .withKey("env2.fm")
                      .in("Env 2")
                      .help(
                          "How much of each oscillator's FM depth follows it: at 100 % the depth "
                          "rises and falls with the envelope, as an electric piano's bark does.");

    Param env3Attack = milliseconds("Env 3 Attack", 0, 30000, 10)
                           .withKey("env3.attack")
                           .polyModulated()
                           .in("Env 3")
                           .help("A third envelope, for routes only: its rise.");
    Param env3Decay = milliseconds("Env 3 Decay", 1, 30000, 800)
                          .withKey("env3.decay")
                          .polyModulated()
                          .in("Env 3")
                          .help("Its fall to the sustain level.");
    Param env3Sustain = percent("Env 3 Sustain", 0)
                            .withKey("env3.sustain")
                            .in("Env 3")
                            .help("Its level while the key is down.");
    Param env3Release = milliseconds("Env 3 Release", 1, 30000, 300)
                            .withKey("env3.release")
                            .polyModulated()
                            .in("Env 3")
                            .help("Its fall once the key is up.");

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
        frequency("LFO 2 Rate", 0.01, 50, 2)
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

    Param l3Shape =
        choice("LFO 3 Shape",
               {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Drift"}, 0)
            .keys({"sine", "triangle", "saw_up", "saw_down", "square", "random", "drift"})
            .withKey("lfo3.shape")
            .in("LFO 3")
            .help("Its wave: random holds a new value each cycle, drift glides between them.");
    Param l3Sync = toggle("LFO 3 Sync", false)
                       .withKey("lfo3.sync")
                       .in("LFO 3")
                       .help("Follow the song's tempo: a cycle per note value.");
    Param l3Rate =
        frequency("LFO 3 Rate", 0.01, 50, 2)
            .withKey("lfo3.rate")
            .polyModulated()
            .in("LFO 3")
            .activeUnless(l3Sync)
            .help("Cycles a second. Routes move it for each note's LFO, not a free one's.");
    Param l3Division = choice("LFO 3 Division",
                              {"1/32", "1/16", "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2",
                               "1 Bar", "2 Bars", "4 Bars"},
                              6)
                           .keys({"n1_32", "n1_16", "n1_8t", "n1_8", "n1_8d", "n1_4t", "n1_4",
                                  "n1_4d", "n1_2", "bar1", "bar2", "bar4"})
                           .withKey("lfo3.division")
                           .in("LFO 3")
                           .activeWhen(l3Sync)
                           .help("How long a cycle lasts, in notes of the song's tempo.");
    Param l3Mode = choice("LFO 3 Mode", {"Note", "Free"}, 0)
                       .withKey("lfo3.mode")
                       .in("LFO 3")
                       .help(
                           "Note: each note's own, from its start. Free: one for every voice, "
                           "running on (synced, it follows the song's position).");
    Param l3Phase =
        degrees("LFO 3 Phase", 0, 360, 0)
            .withKey("lfo3.phase")
            .in("LFO 3")
            .help("Where in its cycle it starts (Note), or how far it is moved (Free).");
    Param l3Delay = milliseconds("LFO 3 Delay", 0, 10000, 0)
                        .withKey("lfo3.delay")
                        .in("LFO 3")
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
    Source env3Source = voiceSource("Env 3").unipolar().withKey("env3").help("Env 3.");
    Source lfo1 = voiceSource("LFO 1").help("LFO 1, -1 to 1.");
    Source lfo2 = voiceSource("LFO 2").help("LFO 2, -1 to 1.");
    Source lfo3 = voiceSource("LFO 3").help("LFO 3, -1 to 1.");
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
    Shared<int> soundingNow;          // voices playing
    SharedArray<double, 3> envShown;  // the newest note's amp, Env 2 and Env 3: their level,
    SharedArray<int, 3> stageShown;   // and stage (0 idle, 1 attack ... 4 release)
    SharedArray<double, 3> lfoShown;  // each LFO's place in its two drawn cycles (0..1); -1: none

    // --- State ------------------------------------------------------------------------------

    Array<Voice, kVoices> voice;
    Array<Random, kVoices> random;  // each voice's, seeded by the note it plays
    Array<OscSet, 4> osc;
    Array<FilterSet, 2> filt;
    bool parallel = false;
    Array<int, 2> combWas;  // each filter's type, as update() last saw it
    Array<double, kUnison> detuneRatio;
    Array<double, kUnison> spreadAt;
    int copies = 1;
    double copyGain = 1.0;
    int factor = 2;
    double rate = 96000.0;  // the voices' (oversampled) rate
    double limit = 0.2;     // the highest harmonic a wavetable keeps, in cycles per sample
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
    bool tablesMade = false;
    // Free LFO phase anchors and whole frames since the last rate change.
    // Summing fractional phase once per block would depend on block size.
    Array<double, 3> freePhase;
    Array<int, 3> freeCycle;
    Array<double, 3> freeFrames;
    Array<double, 3> freeRate;
    // Playing-transport anchors: host beats detect jumps, phase stays
    // continuous across tempo changes, and whole frames avoid block drift.
    Array<bool, 3> syncActive;
    Array<double, 3> syncBeats;
    Array<double, 3> syncPhase;
    Array<double, 3> syncFrames;
    Array<double, 3> syncTempo;
    Array<double, 3> syncDivision;
    double tempo = 120.0;
    double beats = 0.0;
    bool playing = false;

    // The wavetables, and what makes them.
    Buffer<float> tables;
    Buffer<double> re;
    Buffer<double> im;
    Buffer<double> twiddleC;
    Buffer<double> twiddleS;
    Array<double, 257> amp;  // a frame's harmonics, amplitude and phase
    Array<double, 257> phs;
    Array<int, kLevels> levelSize;
    Array<int, kLevels> levelAt;

    // --- Hooks ------------------------------------------------------------------------------

    void prepare() {
        factor = oversampling.index() == 0 ? 1 : oversampling.index() == 1 ? 2 : 4;
        rate = sampleRate() * factor;
        limit = min(0.5, 20000.0 / rate);
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
        const int longest = ceilToInt(rate / 25.0) + 4;
        for (Voice& s : voice) {
            for (int f = 0; f < 4; ++f) s.filter[f].comb.prepare(longest);
            silence(s);
        }
        combWas.fill(kComb);  // prepared lines are empty
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
        if (!tablesMade) makeTables();
        tablesMade = true;
    }

    void update() {
        loadOsc(osc[0], 0, o1Type, o1Coarse, o1Wave, o1Width, o1Table, o1Position, o1Ratio,
                o1Feedback, o1Color, o1Octave, o1FmFrom, o1To, o1Retrigger);
        loadOsc(osc[1], 1, o2Type, o2Coarse, o2Wave, o2Width, o2Table, o2Position, o2Ratio,
                o2Feedback, o2Color, o2Octave, o2FmFrom, o2To, o2Retrigger);
        loadOsc(osc[2], 2, o3Type, o3Coarse, o3Wave, o3Width, o3Table, o3Position, o3Ratio,
                o3Feedback, o3Color, o3Octave, o3FmFrom, o3To, o3Retrigger);
        loadOsc(osc[3], 3, o4Type, o4Coarse, o4Wave, o4Width, o4Table, o4Position, o4Ratio,
                o4Feedback, o4Color, o4Octave, o4FmFrom, o4To, o4Retrigger);
        osc[1].sync = o2Sync.on();
        osc[2].sync = o3Sync.on();
        osc[3].sync = o4Sync.on();
        loadFilter(filt[0], f1Type, f1Cutoff, f1Resonance, f1Slope, f1Mode, f1Feedback, f1Damping,
                   f1Vowel, f1Shift, f1Keytrack, f1Env, f1Velocity);
        loadFilter(filt[1], f2Type, f2Cutoff, f2Resonance, f2Slope, f2Mode, f2Feedback, f2Damping,
                   f2Vowel, f2Shift, f2Keytrack, f2Env, f2Velocity);
        // A filter that becomes a comb starts from an empty line, not from
        // what a note long gone left in it.
        for (int f = 0; f < 2; ++f) {
            if (filt[f].type == kComb && combWas[f] != kComb)
                for (Voice& s : voice) {
                    s.filter[f * 2].comb.clear();
                    s.filter[f * 2 + 1].comb.clear();
                }
            combWas[f] = filt[f].type;
        }
        // Routing: in series an oscillator sent to both enters filter 1.
        parallel = routing.index() == 1;
        for (int o = 0; o < 4; ++o) {
            OscSet& c = osc[o];
            c.toA = c.to == 0 || c.to == 2;
            c.toB = c.to == 1 || (c.to == 2 && parallel);
            c.direct = c.to == 3;
        }
        copies = unison.index();
        copyGain = 1.0 / sqrt(copies);
        for (int u = 0; u < copies; ++u) {
            const double at = copies > 1 ? 2.0 * u / (copies - 1) - 1.0 : 0.0;
            detuneRatio[u] = exp2(detune * at / 1200.0);
            spreadAt[u] = spread * at;
        }
        setVoiceMode(mode.index() == 0   ? VoiceMode::Poly
                     : mode.index() == 1 ? VoiceMode::Mono
                                         : VoiceMode::Legato);
        setGlide(glide);
        setBendRange(bendRange);
        setVoiceLimit(polyphony.index());
    }

    void loadOsc(OscSet& c, int self, const Param& type, const Param& coarse, const Param& wave,
                 const Param& width, const Param& table, const Param& position, const Param& ratio,
                 const Param& feedback, const Param& color, const Param& octave,
                 const Param& fmFrom, const Param& to, const Param& retrigger) {
        c.type = type.index();
        c.coarse = coarse.index();
        c.wave = wave.index();
        c.width = width;
        c.table = table.index();
        c.position = position;
        c.ratio = ratio;
        c.feedback = feedback;
        c.color = color.index();
        c.octave = octave.index();
        c.from = fmFrom.index() < self ? fmFrom.index() : fmFrom.index() + 1;
        c.to = to.index();
        c.retrigger = retrigger.on();
    }

    void loadFilter(FilterSet& f, const Param& type, const Param& cutoff, const Param& resonance,
                    const Param& slope, const Param& svfMode, const Param& feedback,
                    const Param& damping, const Param& vowel, const Param& shift,
                    const Param& keytrack, const Param& env, const Param& velocity) {
        f.type = type.index();
        f.cutoff = cutoff;
        f.resonance = resonance;
        f.steep = slope.index() == 1;
        f.mode = svfMode.index();
        f.feedback = feedback;
        f.damping = damping;
        f.vowel = vowel;
        f.shift = shift;
        f.keytrack = keytrack;
        f.env = env;
        f.velocity = velocity;
    }

    // --- Notes ------------------------------------------------------------------------------

    void startVoice(int v, bool retrigger) {
        Voice& s = voice[v];
        Random& dice = random[v];
        ++notes;
        newest = v;
        dice.seed(notes * 7919 + 17);
        s.velocity = voiceVelocity(v);
        if (!s.sounding) {
            for (int f = 0; f < 4; ++f) {
                s.filter[f].clear();
                if (filt[f / 2].type == kComb) s.filter[f].comb.clear();
            }
            for (int x = 0; x < 32; ++x) {
                s.phase[x] = osc[x / 8].retrigger ? 0.0 : dice.next();
                s.out[x] = 0.0;
                s.fb1[x] = 0.0;
                s.fb2[x] = 0.0;
                s.wander[x] = dice.gaussian();
            }
            for (int o = 0; o < 4; ++o) {
                s.noises[o].seed(dice.below(1000000000));
                s.lastPink[o] = 0.0;
                s.noise[o] = 0.0;
                s.gain[o] = 0.0;
                s.gainStep[o] = 0.0;
            }
            s.wrapped.fill(-1.0);
            s.panAt.fill(2.0);
            s.amp.kill();
            s.mod.kill();
            s.env3.kill();
            s.ampL = 0.0;
            s.ampR = 0.0;
            s.ampLStep = 0.0;
            s.ampRStep = 0.0;
            s.driftAt = dice.next();
            s.pitchFrom = dice.bipolar();
            s.pitchTo = dice.bipolar();
            s.cutFrom = dice.bipolar();
            s.cutTo = dice.bipolar();
            s.narrow = true;
            s.wasCentred = true;
            s.ending = false;
            s.fresh = true;
            s.tickLeft = 0;
        }
        if (retrigger || !s.sounding) {
            s.amp.gate();
            s.mod.gate();
            s.env3.gate();
            s.lfoPhase[0] = l1Phase / 360.0;
            s.lfoPhase[1] = l2Phase / 360.0;
            s.lfoPhase[2] = l3Phase / 360.0;
            s.lfoCycle.fill(0);
            s.lfoSeed = dice.below(1000000);
            s.age = 0.0;
        }
        s.sounding = true;
        s.done = false;
    }

    void releaseVoice(int v) {
        voice[v].amp.release();
        voice[v].mod.release();
        voice[v].env3.release();
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
        s.env3.kill();
        s.ampL = 0.0;
        s.ampR = 0.0;
        s.ampLStep = 0.0;
        s.ampRStep = 0.0;
        s.lastL = 0.0;
        s.lastR = 0.0;
        s.tickLeft = 0;
        for (int f = 0; f < 4; ++f) s.filter[f].clear();
    }

    // --- Control: every kTick frames of a voice -------------------------------------------

    double ms(double t) { return t * 0.001 * sampleRate(); }

    void control(int v, int i) {
        Voice& s = voice[v];
        Random& dice = random[v];
        const double steps = kTick * factor;
        const bool snap = s.fresh;
        s.fresh = false;

        // The envelopes, and then the sources the routes read: the
        // envelopes and LFOs as they are now.
        const double ampEnv =
            s.amp.advance(kTick, ms(ampAttack.forVoice(v)), ms(ampDecay.forVoice(v)), ampSustain,
                          ms(ampRelease.forVoice(v)));
        const double modEnv =
            s.mod.advance(kTick, ms(envAttack.forVoice(v)), ms(envDecay.forVoice(v)), envSustain,
                          ms(envRelease.forVoice(v)));
        const double thirdEnv =
            s.env3.advance(kTick, ms(env3Attack.forVoice(v)), ms(env3Decay.forVoice(v)),
                           env3Sustain, ms(env3Release.forVoice(v)));
        if (s.ending && s.amp.stage == 0 && !snap) {
            s.done = true;
            return;
        }
        ampSource.set(v, ampEnv);
        env2Source.set(v, modEnv);
        env3Source.set(v, thirdEnv);
        lfo1.set(v, lfo(s, v, 0, i, l1Shape, l1Sync, l1Rate, l1Division, l1Mode, l1Phase, l1Delay));
        lfo2.set(v, lfo(s, v, 1, i, l2Shape, l2Sync, l2Rate, l2Division, l2Mode, l2Phase, l2Delay));
        lfo3.set(v, lfo(s, v, 2, i, l3Shape, l3Sync, l3Rate, l3Division, l3Mode, l3Phase, l3Delay));
        s.age += kTick;

        // Drift: a new random target about every 3 seconds, eased toward.
        s.driftAt += kTick / sampleRate() * 0.3;
        if (s.driftAt >= 1.0) {
            s.driftAt -= 1.0;
            s.pitchFrom = s.pitchTo;
            s.pitchTo = dice.bipolar();
            s.cutFrom = s.cutTo;
            s.cutTo = dice.bipolar();
        }
        const double ease = s.driftAt * s.driftAt * (3.0 - 2.0 * s.driftAt);
        const double driftPitch = drift * 0.05 * lerp(s.pitchFrom, s.pitchTo, ease);  // semitones
        const double driftCut = drift * 0.043 * lerp(s.cutFrom, s.cutTo, ease);       // octaves

        const double pitch =
            voicePitch(v, i) + driftPitch + envPitch * modEnv + tune.forVoice(v) / 100.0;
        const double fmFollow = 1.0 - envFm + envFm * modEnv;
        bool centred = true;
        OscNow m;
        for (int o = 0; o < 4; ++o) {
            const OscSet& c = osc[o];
            s.type[o] = c.type;
            if (c.type == kOff) continue;
            oscAt(o, v, c, m);
            const bool stacked = c.type == kAnalog || c.type == kWavetable || c.type == kFm;
            const int n = stacked ? copies : 1;
            s.copies[o] = n;
            double hz = 0.0;
            if (c.type == kSub)
                hz = mtof(pitch + 12.0 * c.octave);
            else if (c.type == kFm)
                hz = mtof(pitch + m.tune) * m.ratio;
            else
                hz = mtof(pitch + m.tune);
            s.mult[o] = 1.0;
            if (c.type == kWavetable) s.mult[o] = c.table == 3 ? 8.0 : c.table == 4 ? 2.0 : 1.0;
            for (int u = 0; u < n; ++u) {
                const int x = o * 8 + u;
                const double ratio =
                    (stacked ? detuneRatio[u] : 1.0) * exp2(drift * s.wander[x] * 0.002);
                const double target = min(hz * ratio / rate, 0.45) / s.mult[o];
                if (snap) {
                    s.inc[x] = target;
                    s.incStep[x] = 0.0;
                } else {
                    s.incStep[x] = (target - s.inc[x]) / steps;
                }
                const double p = clamp(m.pan + (stacked ? spreadAt[u] : 0.0), -1.0, 1.0);
                if (p != 0.0) centred = false;
                if (s.panAt[x] != p) {
                    const double a = (p + 1.0) * 0.25 * pi;
                    s.panL[x] = cos(a) * sqrt(2.0);
                    s.panR[x] = sin(a) * sqrt(2.0);
                    s.panAt[x] = p;
                }
            }
            const double g = m.gain * (stacked ? copyGain : 1.0);
            if (snap) {
                s.gain[o] = g;
                s.gainStep[o] = 0.0;
            } else {
                s.gainStep[o] = (g - s.gain[o]) / steps;
            }
            s.fm[o] = c.type == kNoise || c.type == kSub || osc[c.from].type == kOff
                          ? 0.0
                          : m.fm * fmFollow / twoPi;
            s.from[o] = c.from;
            s.width[o] = m.width;
            s.feedback[o] = m.feedback;
            if (c.type == kWavetable) table(s, o, c.table, m.position, s.inc[o * 8]);
        }

        // Filters.
        const double keyOct = (pitch - 60.0) / 12.0;
        FilterRead r;
        for (int f = 0; f < 2; ++f) {
            const FilterSet& c = filt[f];
            if (c.type == kOff) continue;
            if (f == 0)
                readFilter(v, c, r, f1Cutoff, f1Resonance, f1Feedback, f1Damping, f1Vowel, f1Shift,
                           f1Drive);
            else
                readFilter(v, c, r, f2Cutoff, f2Resonance, f2Feedback, f2Damping, f2Vowel, f2Shift,
                           f2Drive);
            FilterNow& n = s.now[f];
            const double octs =
                c.keytrack * keyOct + c.env * modEnv + c.velocity * s.velocity + driftCut;
            const double hz = clamp(r.cutoff * exp2(octs), 10.0, min(22000.0, 0.45 * rate));
            const double g = tan(pi * hz / rate);
            const double k = c.type == kLadder
                                 ? 4.0 * r.resonance
                                 : max(0.005, 2.0 * (1.0 - r.resonance) * (1.0 - r.resonance));
            const double delay = clamp(rate / max(hz, 25.0), 2.0, rate / 25.0);
            if (snap) {
                n.g = g;
                n.gStep = 0.0;
                n.k = k;
                n.kStep = 0.0;
                n.delay = delay;
                n.delayStep = 0.0;
            } else {
                n.gStep = (g - n.g) / steps;
                n.kStep = (k - n.k) / steps;
                n.delayStep = (delay - n.delay) / steps;
            }
            n.drive = r.drive;
            n.feedback = r.feedback;
            n.damping = r.damping;
            if (c.type == kFormant) formants(n, r.vowel, r.shift + 12.0 * keyOct * c.keytrack);
        }

        // Stereo: a voice whose oscillators all sit in the centre filters one
        // channel and copies it.
        const bool narrow = centred && s.wasCentred;
        if (s.narrow && !narrow) {
            s.filter[1].follow(s.filter[0]);
            s.filter[3].follow(s.filter[2]);
        }
        s.narrow = narrow;
        s.wasCentred = centred;

        // Amp: the envelope, velocity, its level, the note's own volume and pan.
        const double velDb = -(1.0 - s.velocity) * 40.0 * ampVelocity;
        const double level =
            ampEnv * dbToGain(velDb + ampLevel.forVoice(v)) * voiceVolume(v) * kVoiceGain;
        const double p = clamp(ampPan.forVoice(v) + (voicePan(v) - 0.5) * 2.0, -1.0, 1.0);
        if (s.ampPanAt != p) {
            const double a = (p + 1.0) * 0.25 * pi;
            s.ampCos = cos(a);
            s.ampSin = sin(a);
            s.ampPanAt = p;
        }
        const double l = level * s.ampCos * sqrt(2.0);
        const double rr = level * s.ampSin * sqrt(2.0);
        if (snap) {
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
               const Param& rate, const Param& division, const Param& mode, const Param& phase,
               const Param& delay) {
        const double fade = delay > 0.0 ? min(1.0, s.age / ms(delay)) : 1.0;
        const double beatHz = tempo / 60.0 / divisionBeats(division.index());
        double t = 0.0;
        int cycle = 0;
        int seed = n + 1;
        if (mode.index() == 1) {
            // Free: one for every voice, from where it was at the block's
            // start, or synced to a playing song, from the song's position.
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
            const double hz = sync.on() ? beatHz : rate.forVoice(v);
            const double next = t + hz * kTick / sampleRate();
            const double whole = floor(next);
            s.lfoPhase[n] = next - whole;
            s.lfoCycle[n] = cycle + toInt(whole);
        }
        return lfoWave(shape.index(), t, cycle, seed) * fade;
    }

    // Anchor at a rate change, keeping phase continuous. Between changes,
    // whole-frame counts give the same phase regardless of block partition.
    void freeOn(int n, const Param& sync, const Param& rate, const Param& division) {
        const double hz = sync.on() ? tempo / 60.0 / divisionBeats(division.index()) : rate.value();
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
        // Ignore host rounding below a billionth of a beat. A larger
        // difference is a seek or loop jump and takes the host's phase.
        const bool jump = !syncActive[n] || abs(beats - predicted) > 1e-9;
        if (jump || span != syncDivision[n] || tempo != syncTempo[n]) {
            // Carry the predicted beat through continuous changes too: a
            // division automation event may split a differently sized block.
            const double anchorBeat = jump ? beats : predicted;
            syncPhase[n] = jump || span != syncDivision[n] ? anchorBeat / span : syncedPhase(n, 0);
            syncBeats[n] = anchorBeat;
            syncTempo[n] = tempo;
            syncDivision[n] = span;
            syncFrames[n] = 0.0;
        }
        syncActive[n] = true;
    }

    // An oscillator's settings for voice v, with their routes: only those
    // its type uses.
    void oscAt(int o, int v, const OscSet& c, OscNow& m) {
        if (o == 0)
            readOsc(v, c, m, o1Level, o1Pan, o1Fine, o1Width, o1Position, o1Ratio, o1Feedback,
                    o1Fm);
        else if (o == 1)
            readOsc(v, c, m, o2Level, o2Pan, o2Fine, o2Width, o2Position, o2Ratio, o2Feedback,
                    o2Fm);
        else if (o == 2)
            readOsc(v, c, m, o3Level, o3Pan, o3Fine, o3Width, o3Position, o3Ratio, o3Feedback,
                    o3Fm);
        else
            readOsc(v, c, m, o4Level, o4Pan, o4Fine, o4Width, o4Position, o4Ratio, o4Feedback,
                    o4Fm);
    }
    void readOsc(int v, const OscSet& c, OscNow& m, const Param& level, const Param& pan,
                 const Param& fine, const Param& width, const Param& position, const Param& ratio,
                 const Param& feedback, const Param& fm) {
        const double db = level.forVoice(v);
        m.gain = db <= -60.0 ? 0.0 : dbToGain(db);
        m.pan = pan.forVoice(v);
        const bool pitched = c.type == kAnalog || c.type == kWavetable || c.type == kFm;
        m.tune = pitched ? c.coarse + fine.forVoice(v) / 100.0 : 0.0;
        m.width =
            (c.type == kAnalog || c.type == kSub) && c.wave == 2 ? width.forVoice(v) : c.width;
        m.position = c.type == kWavetable ? position.forVoice(v) : c.position;
        m.ratio = c.type == kFm ? ratio.forVoice(v) : c.ratio;
        m.feedback = c.type == kFm ? feedback.forVoice(v) : c.feedback;
        m.fm = pitched ? fm.forVoice(v) : 0.0;
    }

    // A filter's settings for voice v, with their routes: only those its
    // type uses.
    void readFilter(int v, const FilterSet& c, FilterRead& r, const Param& cutoff,
                    const Param& resonance, const Param& feedback, const Param& damping,
                    const Param& vowel, const Param& shift, const Param& drive) {
        const bool tuned = c.type == kLadder || c.type == kSvf;
        r.cutoff = c.type == kFormant ? c.cutoff : cutoff.forVoice(v);
        r.resonance = tuned ? resonance.forVoice(v) : c.resonance;
        r.feedback = c.type == kComb ? feedback.forVoice(v) : c.feedback;
        r.damping = c.type == kComb ? damping.forVoice(v) : c.damping;
        r.vowel = c.type == kFormant ? vowel.forVoice(v) : c.vowel;
        r.shift = c.type == kFormant ? shift.forVoice(v) : c.shift;
        r.drive = dbToGain(drive.forVoice(v));
    }

    // A wavetable oscillator's band level (the most harmonics that stay
    // under the limit at this pitch) and its two frames either side of the
    // position. The first frame of the harmonic ramp and of the digital
    // table is the analog saw itself, so at 0 % they are that saw exactly.
    void table(Voice& s, int o, int which, double position, double inc) {
        const double x = kTop * inc / limit;
        int level = x <= 1.0 ? 0 : ceilToInt(log2(x));
        level = clamp(level, 0, kLevels - 1);
        const double at = clamp(position, 0.0, 1.0) * (kFrames - 1);
        const int f0 = min(floorToInt(at), kFrames - 2);
        s.blend[o] = which == 5 ? stepped(at - f0) : at - f0;
        s.saw[o] = f0 == 0 && (which == 0 || which == 5);
        s.size[o] = levelSize[level];
        s.base0[o] = (which * kFrames + f0) * kPerFrame + levelAt[level];
        s.base1[o] = s.base0[o] + kPerFrame;
    }

    // The formant bank's three bands for a vowel 0..4 (a e i o u, averages
    // of adult speakers), moved by shift semitones.
    void formants(FilterNow& n, double vowel, double shift) {
        const double v = clamp(vowel, 0.0, 4.0);
        const int i = min(floorToInt(v), 3);
        const double t = v - i;
        const double up = exp2(shift / 12.0);
        for (int b = 0; b < 3; ++b) {
            const double hz = lerp(formantHz(i, b), formantHz(i + 1, b), t) * up;
            const double h = min(hz, 0.45 * rate);
            const double g = tan(pi * h / rate);
            // Speech's bandwidths, widened to a Q of at most 6 so the
            // harmonics of a low note still find the peaks.
            const double width = max(b == 0 ? 80.0 : b == 1 ? 90.0 : 120.0, h / 6.0);
            n.fk[b] = width / h;
            n.fa1[b] = 1.0 / (1.0 + g * (g + n.fk[b]));
            n.fa2[b] = g * n.fa1[b];
            n.fa3[b] = g * n.fa2[b];
        }
    }
    double formantHz(int vowel, int band) {
        if (vowel == 0) return band == 0 ? 730.0 : band == 1 ? 1090.0 : 2440.0;
        if (vowel == 1) return band == 0 ? 530.0 : band == 1 ? 1840.0 : 2480.0;
        if (vowel == 2) return band == 0 ? 270.0 : band == 1 ? 2290.0 : 3010.0;
        if (vowel == 3) return band == 0 ? 570.0 : band == 1 ? 840.0 : 2410.0;
        return band == 0 ? 300.0 : band == 1 ? 870.0 : 2240.0;
    }

    // --- Audio --------------------------------------------------------------------------------

    double filterOne(Filter& f, const FilterSet& c, const FilterNow& n, double x) {
        if (c.type == kLadder) return f.ladder(x, n.g, n.k, n.drive, c.steep);
        if (c.type == kSvf) return f.svf(x * n.drive, n.g, n.k, c.mode);
        if (c.type == kComb) return f.combFilter(x * n.drive, n.delay, n.feedback, n.damping);
        const double one = f.formantBand(0, x, n.fa1[0], n.fa2[0], n.fa3[0], n.fk[0]);
        const double two = f.formantBand(1, x, n.fa1[1], n.fa2[1], n.fa3[1], n.fk[1]);
        const double three = f.formantBand(2, x, n.fa1[2], n.fa2[2], n.fa3[2], n.fk[2]);
        return (one + 0.6 * two + 0.35 * three) * 2.5 * n.drive;
    }

    // One channel through the filters as routed: a into filter 1, b into
    // filter 2, d past both.
    double filters(Voice& s, int c, double a, double b, double d) {
        double one = a;
        if (filt[0].type != kOff) one = filterOne(s.filter[c], filt[0], s.now[0], a);
        if (filt[1].type == kOff) return one + b + d;
        if (parallel) return one + filterOne(s.filter[2 + c], filt[1], s.now[1], b) + d;
        return filterOne(s.filter[2 + c], filt[1], s.now[1], one + b) + d;
    }

    void play(int v, int frames) {
        Voice& s = voice[v];
        for (int i = 0; i < frames; ++i) {
            if (s.tickLeft == 0) {
                control(v, i);
                s.tickLeft = kTick;
                if (s.done) return;
            }
            --s.tickLeft;
            // Noise is made at the plugin's rate and held across the
            // oversampled frame, so it sounds the same at every oversampling.
            for (int o = 0; o < 4; ++o) {
                if (s.type[o] != kNoise) continue;
                const int col = osc[o].color;
                double y = 0.0;
                if (col == 0)
                    y = s.noises[o].white();
                else if (col == 1)
                    y = s.noises[o].pink();
                else if (col == 2)
                    y = s.noises[o].brown();
                else {
                    const double p = s.noises[o].pink();
                    y = (p - s.lastPink[o]) * 0.7;
                    s.lastPink[o] = p;
                }
                s.noise[o] = y * 2.0;
            }
            for (int k = 0; k < factor; ++k) {
                double aL = 0.0;
                double aR = 0.0;
                double bL = 0.0;
                double bR = 0.0;
                double dL = 0.0;
                double dR = 0.0;
                // From Osc 4 down, so a higher oscillator modulates a lower
                // one with this sample (a lower one a higher one with the
                // last), and Osc 1 wraps last, for the others' sync.
                for (int o = 3; o >= 0; --o) {
                    const int type = s.type[o];
                    if (type == kOff) continue;
                    const OscSet& c = osc[o];
                    const int n = s.copies[o];
                    const double depth = s.fm[o];
                    const int src = s.from[o] * 8;
                    const int srcCopies = max(1, s.copies[s.from[o]]);
                    double sumL = 0.0;
                    double sumR = 0.0;
                    for (int u = 0; u < n; ++u) {
                        const int x = o * 8 + u;
                        const double dt = s.inc[x];
                        s.inc[x] = dt + s.incStep[x];
                        double ph = s.phase[x];
                        if (c.sync && type == kAnalog && s.wrapped[u] >= 0.0)
                            ph = s.wrapped[u] * dt;
                        const double pm =
                            depth == 0.0 ? 0.0 : depth * s.out[src + min(u, srcCopies - 1)];
                        double y = 0.0;
                        if (type == kAnalog || type == kSub) {
                            y = analogWave(c.wave, pm == 0.0 ? ph : wrap01(ph + pm), dt,
                                           s.width[o]);
                        } else if (type == kWavetable) {
                            const double tp = pm == 0.0 ? ph : wrap01(ph + pm / s.mult[o]);
                            y = wave(s, o, tp, dt);
                        } else if (type == kFm) {
                            const double self = s.feedback[o] * 0.5 * pi * (s.fb1[x] + s.fb2[x]);
                            y = sin(twoPi * (ph + pm) + self);
                            s.fb2[x] = s.fb1[x];
                            s.fb1[x] = y;
                        } else {
                            y = s.noise[o];
                        }
                        s.out[x] = y;
                        ph += dt;
                        if (ph >= 1.0) {
                            ph -= 1.0;
                            if (o == 0) s.wrapped[u] = ph / dt;
                        } else if (o == 0) {
                            s.wrapped[u] = -1.0;
                        }
                        s.phase[x] = ph;
                        sumL += y * s.panL[x];
                        sumR += y * s.panR[x];
                    }
                    const double g = s.gain[o];
                    s.gain[o] = g + s.gainStep[o];
                    sumL *= g;
                    sumR *= g;
                    if (c.direct) {
                        dL += sumL;
                        dR += sumR;
                    } else {
                        if (c.toA) {
                            aL += sumL;
                            aR += sumR;
                        }
                        if (c.toB) {
                            bL += sumL;
                            bR += sumR;
                        }
                    }
                }
                const double yL = filters(s, 0, aL, bL, dL);
                const double yR = s.narrow ? yL : filters(s, 1, aR, bR, dR);
                for (int f = 0; f < 2; ++f) {
                    FilterNow& n = s.now[f];
                    n.g += n.gStep;
                    n.k += n.kStep;
                    n.delay += n.delayStep;
                }
                s.ampL += s.ampLStep;
                s.ampR += s.ampRStep;
                const double l = yL * s.ampL;
                const double r = yR * s.ampR;
                s.lastL = l;
                s.lastR = r;
                const int at = i * factor + k;
                mixL[at] += l;
                mixR[at] += r;
            }
        }
    }

    // A wavetable's sample at table phase tp (dt its step): between two
    // samples of each frame, and between the frames.
    double wave(Voice& s, int o, double tp, double dt) {
        const int n = s.size[o];
        const double x = tp * n;
        const int i = min(floorToInt(x), n - 1);
        const double a = x - i;
        const int at0 = s.base0[o] + i;
        const int at1 = s.base1[o] + i;
        const double v0 = s.saw[o] ? analogWave(0, tp, dt, 0.5)
                                   : tables[at0] + a * (tables[at0 + 1] - tables[at0]);
        if (s.blend[o] == 0.0) return v0;
        const double v1 = tables[at1] + a * (tables[at1 + 1] - tables[at1]);
        return v0 + s.blend[o] * (v1 - v0);
    }

    void process(Block& io) {
        tempo = io.transport.tempo;
        beats = io.transport.beats;
        playing = io.transport.playing;
        freeOn(0, l1Sync, l1Rate, l1Division);
        freeOn(1, l2Sync, l2Rate, l2Division);
        freeOn(2, l3Sync, l3Rate, l3Division);
        syncOn(0, l1Sync, l1Division);
        syncOn(1, l2Sync, l2Division);
        syncOn(2, l3Sync, l3Division);
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
            play(v, io.frames);
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
            const double g = dbToGain(output.at(i));
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
        for (int n = 0; n < 3; ++n) {
            freeFrames[n] += io.frames;
            if (syncActive[n]) syncFrames[n] += io.frames;
        }
        show();
    }

    // How long the output goes on once the keys are up: the longest amp
    // release (a route may lengthen a voice's), and 20 ms for a stolen
    // voice's fade and the downsampler. Raised whenever a voice needs more,
    // lowered only with no voice sounding, so the host is not told of every
    // wobble of a routed release.
    void reportTail(bool sounding) {
        double longest = ampRelease;
        for (int v = 0; v < voiceCount(); ++v)
            if (voice[v].sounding && voicePlaying(v))
                longest = max(longest, ampRelease.forVoice(v));
        const int want = ceilToInt((longest + 20.0) * 0.001 * sampleRate());
        if (want > tail || (!sounding && want != tail)) {
            tail = want;
            setTail(tail);
        }
    }

    // What the editor shows: the level, the voices, and the newest note's
    // envelopes and LFOs.
    void show() {
        outLeft.set(gainToDb(peakL));
        outRight.set(gainToDb(peakR));
        int count = 0;
        for (int v = 0; v < voiceCount(); ++v)
            if (voice[v].sounding && voicePlaying(v)) ++count;
        soundingNow.set(count);
        const bool on = newest >= 0 && voice[newest].sounding && voicePlaying(newest);
        envShown[0] = on ? voice[newest].amp.level : 0.0;
        envShown[1] = on ? voice[newest].mod.level : 0.0;
        envShown[2] = on ? voice[newest].env3.level : 0.0;
        stageShown[0] = on ? voice[newest].amp.stage : 0;
        stageShown[1] = on ? voice[newest].mod.stage : 0;
        stageShown[2] = on ? voice[newest].env3.stage : 0;
        lfoShown[0] = lfoPlace(0, l1Mode.index() == 1, on, l1Phase / 360.0);
        lfoShown[1] = lfoPlace(1, l2Mode.index() == 1, on, l2Phase / 360.0);
        lfoShown[2] = lfoPlace(2, l3Mode.index() == 1, on, l3Phase / 360.0);
    }
    // Where LFO n is in the two cycles the editor draws from its start phase,
    // 0..1, so its dot crosses both: a free one from its anchor and the frames
    // since, a note's from the newest note's start. -1 with no note sounding.
    double lfoPlace(int n, bool free, bool on, double start) const {
        if (free) return wrap01((freeCycle[n] + freeNow(n)) * 0.5);
        if (!on) return -1.0;
        return wrap01((voice[newest].lfoCycle[n] + voice[newest].lfoPhase[n] - start) * 0.5);
    }
    double freeNow(int n) const {
        return freePhase[n] + freeRate[n] * freeFrames[n] / sampleRate();
    }

    // Untouched below -1 dBFS, then bent smoothly toward full scale.
    double safety(double x) {
        const double t = 0.891250938;
        const double a = abs(x);
        if (a <= t) return x;
        const double y = t + (1.0 - t) * fastTanh((a - t) / (1.0 - t));
        return x < 0.0 ? -y : y;
    }

    // --- The wavetables -----------------------------------------------------------------------

    void makeTables() {
        int at = 0;
        int size = kFft;
        for (int l = 0; l < kLevels; ++l) {
            levelSize[l] = max(256, size);
            levelAt[l] = at;
            at += levelSize[l] + 1;
            size = size / 2;
        }
        tables.resize(kTables * kFrames * kPerFrame);
        re.resize(kFft);
        im.resize(kFft);
        twiddleC.resize(kFft / 2);
        twiddleS.resize(kFft / 2);
        for (int j = 0; j < kFft / 2; ++j) {
            twiddleC[j] = cos(twoPi * j / kFft);
            twiddleS[j] = sin(twoPi * j / kFft);
        }
        for (int t = 0; t < kTables; ++t)
            for (int f = 0; f < kFrames; ++f) {
                if (t == 5)
                    digital(f, amp, phs, kTop);
                else
                    harmonics(t, f / (kFrames - 1.0), amp, phs, kTop);
                const int base = (t * kFrames + f) * kPerFrame;
                // One gain for every level of a frame, from the full band, so
                // the levels match: as loud as a saw (an RMS of 0.577), so a
                // sweep through the table holds its level, with the peak
                // kept within 2.
                double gain = 0.0;
                int top = kTop;
                for (int l = 0; l < kLevels; ++l) {
                    const int size = levelSize[l];
                    renderFrame(top, size);
                    if (l == 0) {
                        double peak = 0.0;
                        double power = 0.0;
                        for (int i = 0; i < size; ++i) {
                            peak = max(peak, abs(re[i]));
                            power += re[i] * re[i];
                        }
                        const double rms = sqrt(power / size);
                        gain = rms > 0.0 ? min(0.57735 / rms, 2.0 / peak) : 0.0;
                    }
                    for (int i = 0; i < size; ++i)
                        tables[base + levelAt[l] + i] = static_cast<float>(re[i] * gain);
                    tables[base + levelAt[l] + size] = tables[base + levelAt[l]];
                    top = top / 2;
                }
            }
    }

    // The frame with harmonics 1 .. top of amp and phs, size samples, into re.
    void renderFrame(int top, int size) {
        for (int i = 0; i < size; ++i) {
            re[i] = 0.0;
            im[i] = 0.0;
        }
        for (int k = 1; k <= top && k < size / 2; ++k) {
            // a sin(2 pi k t + phase) is half its amplitude at k and at
            // size - k, conjugate.
            const double a = amp[k] * 0.5;
            re[k] = a * sin(phs[k]);
            im[k] = -a * cos(phs[k]);
            re[size - k] = re[k];
            im[size - k] = -im[k];
        }
        transform(size);
    }

    // re + i im, n points (a power of two up to kFft), to its inverse
    // transform, unscaled: x[t] is the sum of X[k] e^(2 pi i k t / n).
    void transform(int n) {
        int j = 0;
        for (int i = 1; i < n; ++i) {
            int bit = n >> 1;
            while ((j & bit) != 0) {
                j = j ^ bit;
                bit = bit >> 1;
            }
            j = j ^ bit;
            if (i < j) {
                const double r = re[i];
                re[i] = re[j];
                re[j] = r;
                const double m = im[i];
                im[i] = im[j];
                im[j] = m;
            }
        }
        for (int len = 2; len <= n; len = len * 2) {
            const int half = len / 2;
            const int stride = idiv(kFft, len);
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k) {
                    const double wr = twiddleC[k * stride];
                    const double wi = twiddleS[k * stride];
                    const int a = i + k;
                    const int b = a + half;
                    const double xr = re[b] * wr - im[b] * wi;
                    const double xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
        }
    }

    // The digital table's frame f: the saw's harmonics, or those of the
    // frame's steps, exactly (each step held for its share of the cycle), so
    // the table keeps them band-limited as it does any other's.
    void digital(int f, Array<double, 257>& a, Array<double, 257>& ph, int top) {
        a.fill(0.0);
        ph.fill(0.0);
        if (f == 0) {
            for (int k = 1; k <= top; ++k) {
                a[k] = 2.0 / (pi * k);
                ph[k] = pi;
            }
            return;
        }
        Array<double, 64> v;
        const int n = digitalSteps(f, v);
        for (int k = 1; k <= top; ++k) {
            // The steps' sum of v e^(-2 pi i k j / n), times one step's
            // (1 - e^(-2 pi i k / n)) / (2 pi i k).
            double sr = 0.0;
            double si = 0.0;
            for (int j = 0; j < n; ++j) {
                const double w = twoPi * imod(k * j, n) / n;
                sr += v[j] * cos(w);
                si -= v[j] * sin(w);
            }
            const double w = twoPi * k / n;
            const double dr = sin(w) / (twoPi * k);
            const double di = (cos(w) - 1.0) / (twoPi * k);
            const double cr = sr * dr - si * di;
            const double ci = sr * di + si * dr;
            a[k] = 2.0 * sqrt(cr * cr + ci * ci);
            ph[k] = atan2(ci, cr) + 0.5 * pi;
        }
    }

    // The harmonics 1 .. top of table t (not the digital one, which digital()
    // makes) at position p, into a and ph: the tables' and the editor's
    // picture of them.
    void harmonics(int t, double p, Array<double, 257>& a, Array<double, 257>& ph, int top) {
        a.fill(0.0);
        ph.fill(0.0);
        if (t == 0) {
            // Harmonic ramp: a saw (1/k) rolling off to a sine.
            const double e = 1.0 + 8.0 * p * p;
            for (int k = 1; k <= top; ++k) {
                a[k] = pow(k, -e);
                ph[k] = pi;
            }
        } else if (t == 1) {
            // PWM: a pulse narrowing from 50 % to 5 %.
            const double w = 0.5 - 0.45 * p;
            for (int k = 1; k <= top; ++k) {
                const double v = 4.0 / (pi * k) * sin(pi * k * w);
                a[k] = abs(v);
                ph[k] = -pi * k * w + pi / 2.0 + (v < 0.0 ? pi : 0.0);
            }
        } else if (t == 2) {
            // Formant: a bright source through vowel formants a to u, for a
            // fundamental of about 110 Hz.
            const double x = p * 4.0;
            const int v = min(floorToInt(x), 3);
            const double fr = x - v;
            for (int k = 1; k <= top; ++k) {
                const double hz = k * 110.0;
                double env = 0.03;
                for (int b = 0; b < 3; ++b) {
                    const double f = lerp(formantHz(v, b), formantHz(v + 1, b), fr);
                    const double width = b == 0 ? 90.0 : b == 1 ? 110.0 : 160.0;
                    const double d = (hz - f) / width;
                    env += (b == 0 ? 1.0 : b == 1 ? 0.5 : 0.3) / (1.0 + d * d);
                }
                a[k] = env / sqrt(k);
            }
        } else if (t == 3) {
            // Bell: hum, prime, tierce, quint, nominal and above, in eighths
            // of the note (the table plays an eighth of its pitch);
            // position brightens and stretches the upper partials.
            for (int i = 0; i < 12; ++i) {
                const double r = bellRatio(i) * (1.0 + 0.04 * p * (bellRatio(i) - 1.0));
                const int k = roundToInt(r * 8.0);
                const double dark = exp(-i * (1.0 - p) * 0.35);
                if (k >= 1 && k <= top) a[k] += bellLevel(i) * dark;
            }
        } else if (t == 4) {
            // Organ: nine drawbars (16' to 1') on harmonics of half the note,
            // through eight registrations, 3 dB a step.
            const double x = p * 7.0;
            const int r = min(floorToInt(x), 6);
            const double fr = x - r;
            for (int d = 0; d < 9; ++d) {
                const double level = lerp(drawbar(r, d), drawbar(r + 1, d), fr);
                if (level <= 0.0) continue;
                a[drawbarHarmonic(d)] += pow(10.0, -(8.0 - level) * 3.0 / 20.0);
            }
        } else {
            // Spectral comb: a 1/sqrt-ish series through a comb whose teeth
            // close in from every 16 harmonics to every 3, its phases
            // Schroeder's (pi k^2 / n), which keep the wave from piling into
            // one spike.
            const double period = 16.0 - 13.0 * p;
            for (int k = 1; k <= top; ++k) {
                const double c = 0.5 + 0.5 * cos(twoPi * (k - 1) / period);
                a[k] = c * c / pow(k, 0.6);
                ph[k] = pi * k * k / kTop;
            }
        }
    }

    double bellRatio(int i) {
        if (i == 0) return 0.5;
        if (i == 1) return 1.0;
        if (i == 2) return 1.2;
        if (i == 3) return 1.5;
        if (i == 4) return 2.0;
        if (i == 5) return 2.5;
        if (i == 6) return 2.67;
        if (i == 7) return 3.0;
        if (i == 8) return 4.0;
        if (i == 9) return 5.33;
        if (i == 10) return 6.5;
        return 8.0;
    }
    double bellLevel(int i) {
        if (i == 0) return 0.6;
        if (i == 1) return 1.0;
        if (i == 2) return 0.8;
        if (i == 3) return 0.5;
        if (i == 4) return 0.9;
        if (i == 5) return 0.35;
        if (i == 6) return 0.3;
        if (i == 7) return 0.25;
        if (i == 8) return 0.2;
        if (i == 9) return 0.12;
        if (i == 10) return 0.08;
        return 0.05;
    }
    int drawbarHarmonic(int d) {
        if (d == 0) return 1;
        if (d == 1) return 3;
        if (d == 2) return 2;
        if (d == 3) return 4;
        if (d == 4) return 6;
        if (d == 5) return 8;
        if (d == 6) return 10;
        if (d == 7) return 12;
        return 16;
    }
    // Registration r's drawbar d, 0..8.
    double drawbar(int r, int d) {
        const Array<int, 72> regs = {0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0, 0,
                                     6, 0, 8, 6, 0, 4, 0, 0, 0, 8, 4, 8, 6, 0, 0, 0, 0, 0,
                                     8, 8, 8, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 0, 0, 0, 0, 4,
                                     8, 6, 8, 8, 6, 8, 0, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
        return regs[r * 9 + d];
    }

    // --- The editor ---------------------------------------------------------------------------
    //
    // A header with the macros, the voices and the level; the oscillators
    // and the filters, a tab each; the envelopes and the LFOs; the routes
    // (e.routes) and the voice. Every control is drawn here and
    // answers in pointer(): a dial drags up and down (Shift finely), turns
    // with the wheel and goes back to its default on a double click; a
    // button sets its choice; the filter's curve and the envelope's shape
    // drag as well. Each panel shows the controls the settings use.

    static constexpr double kW = 1240;
    static constexpr double kH = 880;
    // Two columns, the same in every row: the oscillators, the envelopes
    // and the routes on the left; the filters, the LFOs and the voice on
    // the right.
    static constexpr double oscX = 16;  // the panels
    static constexpr double oscY = 76;
    static constexpr double oscW = 604;
    static constexpr double oscH = 336;
    static constexpr double filX = 632;
    static constexpr double filY = 76;
    static constexpr double filW = 592;
    static constexpr double filH = 336;
    static constexpr double envX = 16;
    static constexpr double envY = 424;
    static constexpr double envW = 604;
    static constexpr double envH = 216;
    static constexpr double lfoX = 632;
    static constexpr double lfoY = 424;
    static constexpr double lfoW = 592;
    static constexpr double lfoH = 216;
    static constexpr double modX = 16;
    static constexpr double modY = 652;
    static constexpr double modW = 604;
    static constexpr double modH = 212;
    static constexpr double voiX = 632;
    static constexpr double voiY = 652;
    static constexpr double voiW = 592;
    static constexpr double voiH = 212;
    static constexpr double waveX = oscX + 16;  // the oscillator's picture
    static constexpr double waveY = oscY + 124;
    static constexpr double waveW = 244;
    static constexpr double waveH = 118;
    static constexpr double curveX = filX + 16;  // the filter's response
    static constexpr double curveY = filY + 124;
    static constexpr double curveW = 262;
    static constexpr double curveH = 160;
    static constexpr double shapeX = envX + 16;  // the envelope
    static constexpr double shapeY = envY + 44;
    static constexpr double shapeW = 380;
    static constexpr double shapeH = 98;
    static constexpr double cycleX = lfoX + 16;  // the LFO
    static constexpr double cycleY = lfoY + 78;
    static constexpr double cycleW = 350;
    static constexpr double cycleH = 122;

    // The palette: the theme's, and the dune's.
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
    static constexpr int kGrass = 0xA9BC72;     // the oscillators: marram's blades
    static constexpr int kSand = 0xDDBA80;      // the filters: the dune
    static constexpr int kSeaGlass = 0x8FB3AC;  // the voice
    static constexpr int kStem = 0x8C7553;

    // Parameter codes: what a dial or a button edits. The oscillators' are
    // 100 + 20 o + field, the filters' 200 + 20 f + field, the LFOs'
    // 50 + 10 l + field (the fields in the order the struct declares them);
    // a negative code is the editor's own (a tab, a step through the
    // tables).
    static constexpr int cMode = 1;
    static constexpr int cPolyphony = 2;
    static constexpr int cGlide = 3;
    static constexpr int cBend = 4;
    static constexpr int cTune = 5;
    static constexpr int cUnison = 6;
    static constexpr int cDetune = 7;
    static constexpr int cSpread = 8;
    static constexpr int cDrift = 9;
    static constexpr int cOversampling = 10;
    static constexpr int cRouting = 11;
    static constexpr int cAmp = 20;   // attack, decay, sustain, release, velocity, level, pan
    static constexpr int cEnv2 = 30;  // attack, decay, sustain, release, pitch, fm
    static constexpr int cEnv3 = 40;  // attack, decay, sustain, release
    static constexpr int cMacro = 80;
    static constexpr int cOutput = 84;
    static constexpr int oType = 0;  // an oscillator's fields
    static constexpr int oLevel = 1;
    static constexpr int oPan = 2;
    static constexpr int oCoarse = 3;
    static constexpr int oFine = 4;
    static constexpr int oWave = 5;
    static constexpr int oWidth = 6;
    static constexpr int oSync = 7;
    static constexpr int oTable = 8;
    static constexpr int oPosition = 9;
    static constexpr int oRatio = 10;
    static constexpr int oFeedback = 11;
    static constexpr int oColor = 12;
    static constexpr int oOctave = 13;
    static constexpr int oFm = 14;
    static constexpr int oFmFrom = 15;
    static constexpr int oTo = 16;
    static constexpr int oRetrigger = 17;
    static constexpr int fType = 0;  // a filter's
    static constexpr int fCutoff = 1;
    static constexpr int fResonance = 2;
    static constexpr int fSlope = 3;
    static constexpr int fMode = 4;
    static constexpr int fFeedback = 5;
    static constexpr int fDamping = 6;
    static constexpr int fVowel = 7;
    static constexpr int fShift = 8;
    static constexpr int fKeytrack = 9;
    static constexpr int fEnv = 10;
    static constexpr int fVelocity = 11;
    static constexpr int fDrive = 12;
    static constexpr int lShape = 0;  // an LFO's
    static constexpr int lSync = 1;
    static constexpr int lRate = 2;
    static constexpr int lDivision = 3;
    static constexpr int lMode = 4;
    static constexpr int lPhase = 5;
    static constexpr int lDelay = 6;
    static constexpr int eOscTab = -1;  // the editor's own
    static constexpr int eFilterTab = -2;
    static constexpr int eEnvTab = -3;
    static constexpr int eLfoTab = -4;
    static constexpr int eTableStep = -5;
    static constexpr int eCombSign = -6;  // the comb's feedback: + all harmonics, - odd ones
    // A button's look.
    static constexpr int bChoice = 0;  // lit while its parameter holds its value
    static constexpr int bToggle = 1;  // lit while its parameter is on
    static constexpr int bTab = 2;     // an oscillator's or a filter's tab, drawn by its panel
    static constexpr int bSmallTab = 3;
    static constexpr int bStep = 4;   // an arrow
    static constexpr int bShape = 5;  // an LFO shape's icon

    // The editor's own.
    int oscTab = 0;
    int filterTab = 0;
    int envTab = 0;
    int lfoTab = 0;
    double hoverX = -1.0, hoverY = -1.0;
    int dragging = 0;  // 1 a dial, 2 the filter's curve, 3 the envelope
    int dragDial = -1;
    double dialPos = 0.0;
    int handle = -1;  // the envelope's: 0 attack, 1 decay and sustain, 2 release
    double pressX = 0.0, pressY = 0.0, pressA = 0.0, pressB = 0.0;
    int hotDial = -1;
    int hotButton = -1;
    // What the editor shows now, laid out by place(): the dials and the
    // buttons, where they are and what they edit.
    Array<int, 64> dialCode;
    Array<double, 64> dialX, dialY, dialR;
    Array<int, 64> dialColour;
    Array<bool, 64> dialLive;  // false: dimmed, as its setting does not apply
    int dials = 0;
    Array<int, 96> buttonCode, buttonValue, buttonStyle, buttonColour;
    Array<double, 96> buttonX, buttonY, buttonW, buttonH;
    int buttons = 0;
    // A wavetable frame's harmonics, for its picture; the digital table's
    // two frames as steps (none: the saw) and the way between them.
    Array<double, 257> frameAmp;
    Array<double, 257> framePhase;
    Array<double, 64> stepsA, stepsB;
    int stepsInA = 0;
    int stepsInB = 0;
    double stepsBlend = 0.0;

    void editor(Editor& e) {
        e.size(kW, kH);
        e.routes(modX + 16, modY + 36, modW - 32, modH - 46);
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

    int oc(int o, int field) const { return 100 + 20 * o + field; }
    int fc(int f, int field) const { return 200 + 20 * f + field; }
    int lc(int l, int field) const { return 50 + 10 * l + field; }
    int choiceOf(int code) const { return roundToInt(readParam(code, 2)); }
    double valueOf(int code) const { return readParam(code, 1); }

    // Parameter `code`'s position on its dial (how 0), value in its unit
    // (1), or choice (2).
    double readParam(int code, int how) const {
        switch (code) {
            case 1:
                return readOn(mode, how);
            case 2:
                return readOn(polyphony, how);
            case 3:
                return readOn(glide, how);
            case 4:
                return readOn(bendRange, how);
            case 5:
                return readOn(tune, how);
            case 6:
                return readOn(unison, how);
            case 7:
                return readOn(detune, how);
            case 8:
                return readOn(spread, how);
            case 9:
                return readOn(drift, how);
            case 10:
                return readOn(oversampling, how);
            case 11:
                return readOn(routing, how);
            case 20:
                return readOn(ampAttack, how);
            case 21:
                return readOn(ampDecay, how);
            case 22:
                return readOn(ampSustain, how);
            case 23:
                return readOn(ampRelease, how);
            case 24:
                return readOn(ampVelocity, how);
            case 25:
                return readOn(ampLevel, how);
            case 26:
                return readOn(ampPan, how);
            case 30:
                return readOn(envAttack, how);
            case 31:
                return readOn(envDecay, how);
            case 32:
                return readOn(envSustain, how);
            case 33:
                return readOn(envRelease, how);
            case 34:
                return readOn(envPitch, how);
            case 35:
                return readOn(envFm, how);
            case 40:
                return readOn(env3Attack, how);
            case 41:
                return readOn(env3Decay, how);
            case 42:
                return readOn(env3Sustain, how);
            case 43:
                return readOn(env3Release, how);
            case 50:
                return readOn(l1Shape, how);
            case 51:
                return readOn(l1Sync, how);
            case 52:
                return readOn(l1Rate, how);
            case 53:
                return readOn(l1Division, how);
            case 54:
                return readOn(l1Mode, how);
            case 55:
                return readOn(l1Phase, how);
            case 56:
                return readOn(l1Delay, how);
            case 60:
                return readOn(l2Shape, how);
            case 61:
                return readOn(l2Sync, how);
            case 62:
                return readOn(l2Rate, how);
            case 63:
                return readOn(l2Division, how);
            case 64:
                return readOn(l2Mode, how);
            case 65:
                return readOn(l2Phase, how);
            case 66:
                return readOn(l2Delay, how);
            case 70:
                return readOn(l3Shape, how);
            case 71:
                return readOn(l3Sync, how);
            case 72:
                return readOn(l3Rate, how);
            case 73:
                return readOn(l3Division, how);
            case 74:
                return readOn(l3Mode, how);
            case 75:
                return readOn(l3Phase, how);
            case 76:
                return readOn(l3Delay, how);
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
            case 100:
                return readOn(o1Type, how);
            case 101:
                return readOn(o1Level, how);
            case 102:
                return readOn(o1Pan, how);
            case 103:
                return readOn(o1Coarse, how);
            case 104:
                return readOn(o1Fine, how);
            case 105:
                return readOn(o1Wave, how);
            case 106:
                return readOn(o1Width, how);
            case 108:
                return readOn(o1Table, how);
            case 109:
                return readOn(o1Position, how);
            case 110:
                return readOn(o1Ratio, how);
            case 111:
                return readOn(o1Feedback, how);
            case 112:
                return readOn(o1Color, how);
            case 113:
                return readOn(o1Octave, how);
            case 114:
                return readOn(o1Fm, how);
            case 115:
                return readOn(o1FmFrom, how);
            case 116:
                return readOn(o1To, how);
            case 117:
                return readOn(o1Retrigger, how);
            case 120:
                return readOn(o2Type, how);
            case 121:
                return readOn(o2Level, how);
            case 122:
                return readOn(o2Pan, how);
            case 123:
                return readOn(o2Coarse, how);
            case 124:
                return readOn(o2Fine, how);
            case 125:
                return readOn(o2Wave, how);
            case 126:
                return readOn(o2Width, how);
            case 127:
                return readOn(o2Sync, how);
            case 128:
                return readOn(o2Table, how);
            case 129:
                return readOn(o2Position, how);
            case 130:
                return readOn(o2Ratio, how);
            case 131:
                return readOn(o2Feedback, how);
            case 132:
                return readOn(o2Color, how);
            case 133:
                return readOn(o2Octave, how);
            case 134:
                return readOn(o2Fm, how);
            case 135:
                return readOn(o2FmFrom, how);
            case 136:
                return readOn(o2To, how);
            case 137:
                return readOn(o2Retrigger, how);
            case 140:
                return readOn(o3Type, how);
            case 141:
                return readOn(o3Level, how);
            case 142:
                return readOn(o3Pan, how);
            case 143:
                return readOn(o3Coarse, how);
            case 144:
                return readOn(o3Fine, how);
            case 145:
                return readOn(o3Wave, how);
            case 146:
                return readOn(o3Width, how);
            case 147:
                return readOn(o3Sync, how);
            case 148:
                return readOn(o3Table, how);
            case 149:
                return readOn(o3Position, how);
            case 150:
                return readOn(o3Ratio, how);
            case 151:
                return readOn(o3Feedback, how);
            case 152:
                return readOn(o3Color, how);
            case 153:
                return readOn(o3Octave, how);
            case 154:
                return readOn(o3Fm, how);
            case 155:
                return readOn(o3FmFrom, how);
            case 156:
                return readOn(o3To, how);
            case 157:
                return readOn(o3Retrigger, how);
            case 160:
                return readOn(o4Type, how);
            case 161:
                return readOn(o4Level, how);
            case 162:
                return readOn(o4Pan, how);
            case 163:
                return readOn(o4Coarse, how);
            case 164:
                return readOn(o4Fine, how);
            case 165:
                return readOn(o4Wave, how);
            case 166:
                return readOn(o4Width, how);
            case 167:
                return readOn(o4Sync, how);
            case 168:
                return readOn(o4Table, how);
            case 169:
                return readOn(o4Position, how);
            case 170:
                return readOn(o4Ratio, how);
            case 171:
                return readOn(o4Feedback, how);
            case 172:
                return readOn(o4Color, how);
            case 173:
                return readOn(o4Octave, how);
            case 174:
                return readOn(o4Fm, how);
            case 175:
                return readOn(o4FmFrom, how);
            case 176:
                return readOn(o4To, how);
            case 177:
                return readOn(o4Retrigger, how);
            case 200:
                return readOn(f1Type, how);
            case 201:
                return readOn(f1Cutoff, how);
            case 202:
                return readOn(f1Resonance, how);
            case 203:
                return readOn(f1Slope, how);
            case 204:
                return readOn(f1Mode, how);
            case 205:
                return readOn(f1Feedback, how);
            case 206:
                return readOn(f1Damping, how);
            case 207:
                return readOn(f1Vowel, how);
            case 208:
                return readOn(f1Shift, how);
            case 209:
                return readOn(f1Keytrack, how);
            case 210:
                return readOn(f1Env, how);
            case 211:
                return readOn(f1Velocity, how);
            case 212:
                return readOn(f1Drive, how);
            case 220:
                return readOn(f2Type, how);
            case 221:
                return readOn(f2Cutoff, how);
            case 222:
                return readOn(f2Resonance, how);
            case 223:
                return readOn(f2Slope, how);
            case 224:
                return readOn(f2Mode, how);
            case 225:
                return readOn(f2Feedback, how);
            case 226:
                return readOn(f2Damping, how);
            case 227:
                return readOn(f2Vowel, how);
            case 228:
                return readOn(f2Shift, how);
            case 229:
                return readOn(f2Keytrack, how);
            case 230:
                return readOn(f2Env, how);
            case 231:
                return readOn(f2Velocity, how);
            case 232:
                return readOn(f2Drive, how);
            default:
                return 0.0;
        }
    }

    // Parameter `code` set as a user would: to a value in its unit (how 0),
    // a position on its dial (1), or its default (2).
    void editParam(int code, int how, double v) {
        switch (code) {
            case 1:
                editOn(mode, how, v);
                return;
            case 2:
                editOn(polyphony, how, v);
                return;
            case 3:
                editOn(glide, how, v);
                return;
            case 4:
                editOn(bendRange, how, v);
                return;
            case 5:
                editOn(tune, how, v);
                return;
            case 6:
                editOn(unison, how, v);
                return;
            case 7:
                editOn(detune, how, v);
                return;
            case 8:
                editOn(spread, how, v);
                return;
            case 9:
                editOn(drift, how, v);
                return;
            case 10:
                editOn(oversampling, how, v);
                return;
            case 11:
                editOn(routing, how, v);
                return;
            case 20:
                editOn(ampAttack, how, v);
                return;
            case 21:
                editOn(ampDecay, how, v);
                return;
            case 22:
                editOn(ampSustain, how, v);
                return;
            case 23:
                editOn(ampRelease, how, v);
                return;
            case 24:
                editOn(ampVelocity, how, v);
                return;
            case 25:
                editOn(ampLevel, how, v);
                return;
            case 26:
                editOn(ampPan, how, v);
                return;
            case 30:
                editOn(envAttack, how, v);
                return;
            case 31:
                editOn(envDecay, how, v);
                return;
            case 32:
                editOn(envSustain, how, v);
                return;
            case 33:
                editOn(envRelease, how, v);
                return;
            case 34:
                editOn(envPitch, how, v);
                return;
            case 35:
                editOn(envFm, how, v);
                return;
            case 40:
                editOn(env3Attack, how, v);
                return;
            case 41:
                editOn(env3Decay, how, v);
                return;
            case 42:
                editOn(env3Sustain, how, v);
                return;
            case 43:
                editOn(env3Release, how, v);
                return;
            case 50:
                editOn(l1Shape, how, v);
                return;
            case 51:
                editOn(l1Sync, how, v);
                return;
            case 52:
                editOn(l1Rate, how, v);
                return;
            case 53:
                editOn(l1Division, how, v);
                return;
            case 54:
                editOn(l1Mode, how, v);
                return;
            case 55:
                editOn(l1Phase, how, v);
                return;
            case 56:
                editOn(l1Delay, how, v);
                return;
            case 60:
                editOn(l2Shape, how, v);
                return;
            case 61:
                editOn(l2Sync, how, v);
                return;
            case 62:
                editOn(l2Rate, how, v);
                return;
            case 63:
                editOn(l2Division, how, v);
                return;
            case 64:
                editOn(l2Mode, how, v);
                return;
            case 65:
                editOn(l2Phase, how, v);
                return;
            case 66:
                editOn(l2Delay, how, v);
                return;
            case 70:
                editOn(l3Shape, how, v);
                return;
            case 71:
                editOn(l3Sync, how, v);
                return;
            case 72:
                editOn(l3Rate, how, v);
                return;
            case 73:
                editOn(l3Division, how, v);
                return;
            case 74:
                editOn(l3Mode, how, v);
                return;
            case 75:
                editOn(l3Phase, how, v);
                return;
            case 76:
                editOn(l3Delay, how, v);
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
            case 100:
                editOn(o1Type, how, v);
                return;
            case 101:
                editOn(o1Level, how, v);
                return;
            case 102:
                editOn(o1Pan, how, v);
                return;
            case 103:
                editOn(o1Coarse, how, v);
                return;
            case 104:
                editOn(o1Fine, how, v);
                return;
            case 105:
                editOn(o1Wave, how, v);
                return;
            case 106:
                editOn(o1Width, how, v);
                return;
            case 108:
                editOn(o1Table, how, v);
                return;
            case 109:
                editOn(o1Position, how, v);
                return;
            case 110:
                editOn(o1Ratio, how, v);
                return;
            case 111:
                editOn(o1Feedback, how, v);
                return;
            case 112:
                editOn(o1Color, how, v);
                return;
            case 113:
                editOn(o1Octave, how, v);
                return;
            case 114:
                editOn(o1Fm, how, v);
                return;
            case 115:
                editOn(o1FmFrom, how, v);
                return;
            case 116:
                editOn(o1To, how, v);
                return;
            case 117:
                editOn(o1Retrigger, how, v);
                return;
            case 120:
                editOn(o2Type, how, v);
                return;
            case 121:
                editOn(o2Level, how, v);
                return;
            case 122:
                editOn(o2Pan, how, v);
                return;
            case 123:
                editOn(o2Coarse, how, v);
                return;
            case 124:
                editOn(o2Fine, how, v);
                return;
            case 125:
                editOn(o2Wave, how, v);
                return;
            case 126:
                editOn(o2Width, how, v);
                return;
            case 127:
                editOn(o2Sync, how, v);
                return;
            case 128:
                editOn(o2Table, how, v);
                return;
            case 129:
                editOn(o2Position, how, v);
                return;
            case 130:
                editOn(o2Ratio, how, v);
                return;
            case 131:
                editOn(o2Feedback, how, v);
                return;
            case 132:
                editOn(o2Color, how, v);
                return;
            case 133:
                editOn(o2Octave, how, v);
                return;
            case 134:
                editOn(o2Fm, how, v);
                return;
            case 135:
                editOn(o2FmFrom, how, v);
                return;
            case 136:
                editOn(o2To, how, v);
                return;
            case 137:
                editOn(o2Retrigger, how, v);
                return;
            case 140:
                editOn(o3Type, how, v);
                return;
            case 141:
                editOn(o3Level, how, v);
                return;
            case 142:
                editOn(o3Pan, how, v);
                return;
            case 143:
                editOn(o3Coarse, how, v);
                return;
            case 144:
                editOn(o3Fine, how, v);
                return;
            case 145:
                editOn(o3Wave, how, v);
                return;
            case 146:
                editOn(o3Width, how, v);
                return;
            case 147:
                editOn(o3Sync, how, v);
                return;
            case 148:
                editOn(o3Table, how, v);
                return;
            case 149:
                editOn(o3Position, how, v);
                return;
            case 150:
                editOn(o3Ratio, how, v);
                return;
            case 151:
                editOn(o3Feedback, how, v);
                return;
            case 152:
                editOn(o3Color, how, v);
                return;
            case 153:
                editOn(o3Octave, how, v);
                return;
            case 154:
                editOn(o3Fm, how, v);
                return;
            case 155:
                editOn(o3FmFrom, how, v);
                return;
            case 156:
                editOn(o3To, how, v);
                return;
            case 157:
                editOn(o3Retrigger, how, v);
                return;
            case 160:
                editOn(o4Type, how, v);
                return;
            case 161:
                editOn(o4Level, how, v);
                return;
            case 162:
                editOn(o4Pan, how, v);
                return;
            case 163:
                editOn(o4Coarse, how, v);
                return;
            case 164:
                editOn(o4Fine, how, v);
                return;
            case 165:
                editOn(o4Wave, how, v);
                return;
            case 166:
                editOn(o4Width, how, v);
                return;
            case 167:
                editOn(o4Sync, how, v);
                return;
            case 168:
                editOn(o4Table, how, v);
                return;
            case 169:
                editOn(o4Position, how, v);
                return;
            case 170:
                editOn(o4Ratio, how, v);
                return;
            case 171:
                editOn(o4Feedback, how, v);
                return;
            case 172:
                editOn(o4Color, how, v);
                return;
            case 173:
                editOn(o4Octave, how, v);
                return;
            case 174:
                editOn(o4Fm, how, v);
                return;
            case 175:
                editOn(o4FmFrom, how, v);
                return;
            case 176:
                editOn(o4To, how, v);
                return;
            case 177:
                editOn(o4Retrigger, how, v);
                return;
            case 200:
                editOn(f1Type, how, v);
                return;
            case 201:
                editOn(f1Cutoff, how, v);
                return;
            case 202:
                editOn(f1Resonance, how, v);
                return;
            case 203:
                editOn(f1Slope, how, v);
                return;
            case 204:
                editOn(f1Mode, how, v);
                return;
            case 205:
                editOn(f1Feedback, how, v);
                return;
            case 206:
                editOn(f1Damping, how, v);
                return;
            case 207:
                editOn(f1Vowel, how, v);
                return;
            case 208:
                editOn(f1Shift, how, v);
                return;
            case 209:
                editOn(f1Keytrack, how, v);
                return;
            case 210:
                editOn(f1Env, how, v);
                return;
            case 211:
                editOn(f1Velocity, how, v);
                return;
            case 212:
                editOn(f1Drive, how, v);
                return;
            case 220:
                editOn(f2Type, how, v);
                return;
            case 221:
                editOn(f2Cutoff, how, v);
                return;
            case 222:
                editOn(f2Resonance, how, v);
                return;
            case 223:
                editOn(f2Slope, how, v);
                return;
            case 224:
                editOn(f2Mode, how, v);
                return;
            case 225:
                editOn(f2Feedback, how, v);
                return;
            case 226:
                editOn(f2Damping, how, v);
                return;
            case 227:
                editOn(f2Vowel, how, v);
                return;
            case 228:
                editOn(f2Shift, how, v);
                return;
            case 229:
                editOn(f2Keytrack, how, v);
                return;
            case 230:
                editOn(f2Env, how, v);
                return;
            case 231:
                editOn(f2Velocity, how, v);
                return;
            case 232:
                editOn(f2Drive, how, v);
                return;
            default:
                return;
        }
    }

    // Parameter `code`'s value as hosts show it.
    void fillParam(Canvas& g, int code, double x, double y) const {
        switch (code) {
            case 1:
                g.fillText(mode.text(), x, y);
                return;
            case 2:
                g.fillText(polyphony.text(), x, y);
                return;
            case 3:
                g.fillText(glide.text(), x, y);
                return;
            case 4:
                g.fillText(bendRange.text(), x, y);
                return;
            case 5:
                g.fillText(tune.text(), x, y);
                return;
            case 6:
                g.fillText(unison.text(), x, y);
                return;
            case 7:
                g.fillText(detune.text(), x, y);
                return;
            case 8:
                g.fillText(spread.text(), x, y);
                return;
            case 9:
                g.fillText(drift.text(), x, y);
                return;
            case 10:
                g.fillText(oversampling.text(), x, y);
                return;
            case 11:
                g.fillText(routing.text(), x, y);
                return;
            case 20:
                g.fillText(ampAttack.text(), x, y);
                return;
            case 21:
                g.fillText(ampDecay.text(), x, y);
                return;
            case 22:
                g.fillText(ampSustain.text(), x, y);
                return;
            case 23:
                g.fillText(ampRelease.text(), x, y);
                return;
            case 24:
                g.fillText(ampVelocity.text(), x, y);
                return;
            case 25:
                g.fillText(ampLevel.text(), x, y);
                return;
            case 26:
                g.fillText(ampPan.text(), x, y);
                return;
            case 30:
                g.fillText(envAttack.text(), x, y);
                return;
            case 31:
                g.fillText(envDecay.text(), x, y);
                return;
            case 32:
                g.fillText(envSustain.text(), x, y);
                return;
            case 33:
                g.fillText(envRelease.text(), x, y);
                return;
            case 34:
                g.fillText(envPitch.text(), x, y);
                return;
            case 35:
                g.fillText(envFm.text(), x, y);
                return;
            case 40:
                g.fillText(env3Attack.text(), x, y);
                return;
            case 41:
                g.fillText(env3Decay.text(), x, y);
                return;
            case 42:
                g.fillText(env3Sustain.text(), x, y);
                return;
            case 43:
                g.fillText(env3Release.text(), x, y);
                return;
            case 50:
                g.fillText(l1Shape.text(), x, y);
                return;
            case 51:
                g.fillText(l1Sync.text(), x, y);
                return;
            case 52:
                g.fillText(l1Rate.text(), x, y);
                return;
            case 53:
                g.fillText(l1Division.text(), x, y);
                return;
            case 54:
                g.fillText(l1Mode.text(), x, y);
                return;
            case 55:
                g.fillText(l1Phase.text(), x, y);
                return;
            case 56:
                g.fillText(l1Delay.text(), x, y);
                return;
            case 60:
                g.fillText(l2Shape.text(), x, y);
                return;
            case 61:
                g.fillText(l2Sync.text(), x, y);
                return;
            case 62:
                g.fillText(l2Rate.text(), x, y);
                return;
            case 63:
                g.fillText(l2Division.text(), x, y);
                return;
            case 64:
                g.fillText(l2Mode.text(), x, y);
                return;
            case 65:
                g.fillText(l2Phase.text(), x, y);
                return;
            case 66:
                g.fillText(l2Delay.text(), x, y);
                return;
            case 70:
                g.fillText(l3Shape.text(), x, y);
                return;
            case 71:
                g.fillText(l3Sync.text(), x, y);
                return;
            case 72:
                g.fillText(l3Rate.text(), x, y);
                return;
            case 73:
                g.fillText(l3Division.text(), x, y);
                return;
            case 74:
                g.fillText(l3Mode.text(), x, y);
                return;
            case 75:
                g.fillText(l3Phase.text(), x, y);
                return;
            case 76:
                g.fillText(l3Delay.text(), x, y);
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
            case 100:
                g.fillText(o1Type.text(), x, y);
                return;
            case 101:
                g.fillText(o1Level.text(), x, y);
                return;
            case 102:
                g.fillText(o1Pan.text(), x, y);
                return;
            case 103:
                g.fillText(o1Coarse.text(), x, y);
                return;
            case 104:
                g.fillText(o1Fine.text(), x, y);
                return;
            case 105:
                g.fillText(o1Wave.text(), x, y);
                return;
            case 106:
                g.fillText(o1Width.text(), x, y);
                return;
            case 108:
                g.fillText(o1Table.text(), x, y);
                return;
            case 109:
                g.fillText(o1Position.text(), x, y);
                return;
            case 110:
                g.fillText(o1Ratio.text(), x, y);
                return;
            case 111:
                g.fillText(o1Feedback.text(), x, y);
                return;
            case 112:
                g.fillText(o1Color.text(), x, y);
                return;
            case 113:
                g.fillText(o1Octave.text(), x, y);
                return;
            case 114:
                g.fillText(o1Fm.text(), x, y);
                return;
            case 115:
                g.fillText(o1FmFrom.text(), x, y);
                return;
            case 116:
                g.fillText(o1To.text(), x, y);
                return;
            case 117:
                g.fillText(o1Retrigger.text(), x, y);
                return;
            case 120:
                g.fillText(o2Type.text(), x, y);
                return;
            case 121:
                g.fillText(o2Level.text(), x, y);
                return;
            case 122:
                g.fillText(o2Pan.text(), x, y);
                return;
            case 123:
                g.fillText(o2Coarse.text(), x, y);
                return;
            case 124:
                g.fillText(o2Fine.text(), x, y);
                return;
            case 125:
                g.fillText(o2Wave.text(), x, y);
                return;
            case 126:
                g.fillText(o2Width.text(), x, y);
                return;
            case 127:
                g.fillText(o2Sync.text(), x, y);
                return;
            case 128:
                g.fillText(o2Table.text(), x, y);
                return;
            case 129:
                g.fillText(o2Position.text(), x, y);
                return;
            case 130:
                g.fillText(o2Ratio.text(), x, y);
                return;
            case 131:
                g.fillText(o2Feedback.text(), x, y);
                return;
            case 132:
                g.fillText(o2Color.text(), x, y);
                return;
            case 133:
                g.fillText(o2Octave.text(), x, y);
                return;
            case 134:
                g.fillText(o2Fm.text(), x, y);
                return;
            case 135:
                g.fillText(o2FmFrom.text(), x, y);
                return;
            case 136:
                g.fillText(o2To.text(), x, y);
                return;
            case 137:
                g.fillText(o2Retrigger.text(), x, y);
                return;
            case 140:
                g.fillText(o3Type.text(), x, y);
                return;
            case 141:
                g.fillText(o3Level.text(), x, y);
                return;
            case 142:
                g.fillText(o3Pan.text(), x, y);
                return;
            case 143:
                g.fillText(o3Coarse.text(), x, y);
                return;
            case 144:
                g.fillText(o3Fine.text(), x, y);
                return;
            case 145:
                g.fillText(o3Wave.text(), x, y);
                return;
            case 146:
                g.fillText(o3Width.text(), x, y);
                return;
            case 147:
                g.fillText(o3Sync.text(), x, y);
                return;
            case 148:
                g.fillText(o3Table.text(), x, y);
                return;
            case 149:
                g.fillText(o3Position.text(), x, y);
                return;
            case 150:
                g.fillText(o3Ratio.text(), x, y);
                return;
            case 151:
                g.fillText(o3Feedback.text(), x, y);
                return;
            case 152:
                g.fillText(o3Color.text(), x, y);
                return;
            case 153:
                g.fillText(o3Octave.text(), x, y);
                return;
            case 154:
                g.fillText(o3Fm.text(), x, y);
                return;
            case 155:
                g.fillText(o3FmFrom.text(), x, y);
                return;
            case 156:
                g.fillText(o3To.text(), x, y);
                return;
            case 157:
                g.fillText(o3Retrigger.text(), x, y);
                return;
            case 160:
                g.fillText(o4Type.text(), x, y);
                return;
            case 161:
                g.fillText(o4Level.text(), x, y);
                return;
            case 162:
                g.fillText(o4Pan.text(), x, y);
                return;
            case 163:
                g.fillText(o4Coarse.text(), x, y);
                return;
            case 164:
                g.fillText(o4Fine.text(), x, y);
                return;
            case 165:
                g.fillText(o4Wave.text(), x, y);
                return;
            case 166:
                g.fillText(o4Width.text(), x, y);
                return;
            case 167:
                g.fillText(o4Sync.text(), x, y);
                return;
            case 168:
                g.fillText(o4Table.text(), x, y);
                return;
            case 169:
                g.fillText(o4Position.text(), x, y);
                return;
            case 170:
                g.fillText(o4Ratio.text(), x, y);
                return;
            case 171:
                g.fillText(o4Feedback.text(), x, y);
                return;
            case 172:
                g.fillText(o4Color.text(), x, y);
                return;
            case 173:
                g.fillText(o4Octave.text(), x, y);
                return;
            case 174:
                g.fillText(o4Fm.text(), x, y);
                return;
            case 175:
                g.fillText(o4FmFrom.text(), x, y);
                return;
            case 176:
                g.fillText(o4To.text(), x, y);
                return;
            case 177:
                g.fillText(o4Retrigger.text(), x, y);
                return;
            case 200:
                g.fillText(f1Type.text(), x, y);
                return;
            case 201:
                g.fillText(f1Cutoff.text(), x, y);
                return;
            case 202:
                g.fillText(f1Resonance.text(), x, y);
                return;
            case 203:
                g.fillText(f1Slope.text(), x, y);
                return;
            case 204:
                g.fillText(f1Mode.text(), x, y);
                return;
            case 205:
                g.fillText(f1Feedback.text(), x, y);
                return;
            case 206:
                g.fillText(f1Damping.text(), x, y);
                return;
            case 207:
                g.fillText(f1Vowel.text(), x, y);
                return;
            case 208:
                g.fillText(f1Shift.text(), x, y);
                return;
            case 209:
                g.fillText(f1Keytrack.text(), x, y);
                return;
            case 210:
                g.fillText(f1Env.text(), x, y);
                return;
            case 211:
                g.fillText(f1Velocity.text(), x, y);
                return;
            case 212:
                g.fillText(f1Drive.text(), x, y);
                return;
            case 220:
                g.fillText(f2Type.text(), x, y);
                return;
            case 221:
                g.fillText(f2Cutoff.text(), x, y);
                return;
            case 222:
                g.fillText(f2Resonance.text(), x, y);
                return;
            case 223:
                g.fillText(f2Slope.text(), x, y);
                return;
            case 224:
                g.fillText(f2Mode.text(), x, y);
                return;
            case 225:
                g.fillText(f2Feedback.text(), x, y);
                return;
            case 226:
                g.fillText(f2Damping.text(), x, y);
                return;
            case 227:
                g.fillText(f2Vowel.text(), x, y);
                return;
            case 228:
                g.fillText(f2Shift.text(), x, y);
                return;
            case 229:
                g.fillText(f2Keytrack.text(), x, y);
                return;
            case 230:
                g.fillText(f2Env.text(), x, y);
                return;
            case 231:
                g.fillText(f2Velocity.text(), x, y);
                return;
            case 232:
                g.fillText(f2Drive.text(), x, y);
                return;
            default:
                g.fillText("", x, y);
                return;
        }
    }

    // --- layout -------------------------------------------------------------------------------

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
        if (buttons >= 96) return;
        buttonCode[buttons] = code;
        buttonValue[buttons] = value;
        buttonX[buttons] = x;
        buttonY[buttons] = y;
        buttonW[buttons] = w;
        buttonH[buttons] = h;
        buttonStyle[buttons] = style;
        buttonColour[buttons] = colour;
        ++buttons;
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
        // The header: the macros and the output.
        for (int k = 0; k < 4; ++k) dial(cMacro + k, 604 + 62 * k, 36, 14, Theme::modulation, true);
        dial(cOutput, 960, 36, 14, kLinen, true);
        layoutOsc();
        layoutFilter();
        layoutEnv();
        layoutLfo();
        layoutVoice();
    }

    void layoutOsc() {
        const int o = oscTab;
        for (int k = 0; k < 4; ++k)
            button(eOscTab, k, oscX + 16 + k * 145, oscY + 42, 137, 38, bTab, kGrass);
        row(oc(o, oType), 0, 6, oscX + 16, oscY + 92, oscW - 32, 22, kGrass);
        const int type = choiceOf(oc(o, oType));
        if (type == kOff) return;
        const double c0 = oscX + 312, step = 78, r1 = oscY + 162, r2 = oscY + 240;
        const double r = 21;
        dial(oc(o, oLevel), c0, r1, r, kGrass, true);
        dial(oc(o, oPan), c0 + step, r1, r, kGrass, true);
        const bool pitched = type == kAnalog || type == kWavetable || type == kFm;
        if (pitched) {
            dial(oc(o, oCoarse), c0 + 2 * step, r1, r, kGrass, true);
            dial(oc(o, oFine), c0 + 3 * step, r1, r, kGrass, true);
            dial(oc(o, oFm), c0 + 3 * step, r2, r, kGrass, true);
        }
        const double sx = waveX, sy = waveY + waveH + 8, sw = waveW, sh = 22;
        if (type == kAnalog || type == kSub) row(oc(o, oWave), 0, 5, sx, sy, sw, sh, kGrass);
        if (type == kAnalog) dial(oc(o, oWidth), c0, r2, r, kGrass, choiceOf(oc(o, oWave)) == 2);
        if (type == kWavetable) {
            button(eTableStep, -1, sx, sy, 24, sh, bStep, kGrass);
            button(eTableStep, 1, sx + sw - 24, sy, 24, sh, bStep, kGrass);
            dial(oc(o, oPosition), c0, r2, r, kGrass, true);
        }
        if (type == kFm) {
            dial(oc(o, oRatio), c0, r2, r, kGrass, true);
            dial(oc(o, oFeedback), c0 + step, r2, r, kGrass, true);
        }
        if (type == kNoise) row(oc(o, oColor), 0, 4, sx, sy, sw, sh, kGrass);
        if (type == kSub) {
            button(oc(o, oOctave), -1, c0 - 34, r2 - 11, 68, 22, bChoice, kGrass);
            button(oc(o, oOctave), -2, c0 + step - 34, r2 - 11, 68, 22, bChoice, kGrass);
        }
        // The bottom row: where it goes, what modulates it, sync, retrigger.
        const double by = oscY + oscH - 38;
        row(oc(o, oTo), 0, 4, oscX + 40, by, 228, 22, kGrass);
        if (pitched) row(oc(o, oFmFrom), 0, 3, oscX + 336, by, 132, 22, kGrass);
        if (type != kNoise)
            button(oc(o, oRetrigger), 0, oscX + oscW - 70, by, 54, 22, bToggle, kGrass);
        if (o > 0 && type == kAnalog)
            button(oc(o, oSync), 0, oscX + oscW - 124, by, 48, 22, bToggle, kGrass);
    }

    // The filter's controls: a grid of four columns. The top row is the
    // type's own (two or three dials, then its choices: the ladder's slope,
    // the state-variable's mode, the comb's polarity, the formants' vowel);
    // the bottom row is every type's: keytrack, Env 2, velocity, drive.
    static constexpr double fCol = filX + 310;
    static constexpr double fStep = 76;
    static constexpr double fRow1 = filY + 172;
    static constexpr double fRow2 = filY + 268;

    void layoutFilter() {
        const int f = filterTab;
        button(cRouting, 0, filX + filW - 146, filY + 13, 62, 22, bChoice, kSand);
        button(cRouting, 1, filX + filW - 80, filY + 13, 62, 22, bChoice, kSand);
        for (int k = 0; k < 2; ++k)
            button(eFilterTab, k, filX + 16 + k * 188, filY + 42, 180, 38, bTab, kSand);
        row(fc(f, fType), 0, 5, filX + 16, filY + 92, filW - 32, 22, kSand);
        const int type = choiceOf(fc(f, fType));
        if (type == kOff) return;
        const double r = 21;
        // The choices take the third and fourth columns (the comb's the
        // fourth), level with the dials' names above and values below.
        const double bx = fCol + 2 * fStep - 34, bw = fStep + 68;
        const double half = (bw - 5) * 0.5;
        if (type == kFormant) {
            dial(fc(f, fVowel), fCol, fRow1, r, kSand, true);
            dial(fc(f, fShift), fCol + fStep, fRow1, r, kSand, true);
            row(fc(f, fVowel), 0, 5, bx, fRow1 - 11, bw, 22, kSand);
        } else {
            dial(fc(f, fCutoff), fCol, fRow1, r, kSand, true);
            dial(fc(f, type == kComb ? fFeedback : fResonance), fCol + fStep, fRow1, r, kSand,
                 true);
        }
        if (type == kLadder) row(fc(f, fSlope), 0, 2, bx, fRow1 - 11, bw, 22, kSand);
        if (type == kSvf)
            for (int k = 0; k < 4; ++k)
                button(fc(f, fMode), k, bx + imod(k, 2) * (half + 5), fRow1 - 18 + idiv(k, 2) * 26,
                       half, 22, bChoice, kSand);
        if (type == kComb) {
            dial(fc(f, fDamping), fCol + 2 * fStep, fRow1, r, kSand, true);
            button(eCombSign, 1, fCol + 3 * fStep - 30, fRow1 - 18, 60, 22, bChoice, kSand);
            button(eCombSign, -1, fCol + 3 * fStep - 30, fRow1 + 8, 60, 22, bChoice, kSand);
        }
        dial(fc(f, fKeytrack), fCol, fRow2, r, kSand, true);
        dial(fc(f, fEnv), fCol + fStep, fRow2, r, kSand, true);
        dial(fc(f, fVelocity), fCol + 2 * fStep, fRow2, r, kSand, true);
        dial(fc(f, fDrive), fCol + 3 * fStep, fRow2, r, kSand, true);
    }

    int envColour(int k) const {
        return k == 0 ? Theme::accent : k == 1 ? kSand : Theme::modulation;
    }
    int envBase(int k) const { return k == 0 ? cAmp : k == 1 ? cEnv2 : cEnv3; }

    // The envelope's shape with its four times under it; beside them, its
    // levels in two columns, the lower row level with the times.
    static constexpr double eColA = envX + 456;
    static constexpr double eColB = envX + 544;
    static constexpr double eRow1 = envY + 88;
    static constexpr double eRow2 = envY + 180;

    void layoutEnv() {
        for (int k = 0; k < 3; ++k)
            button(eEnvTab, k, envX + envW - 214 + k * 66, envY + 13, 62, 22, bSmallTab,
                   envColour(k));
        const int base = envBase(envTab);
        const int colour = envColour(envTab);
        for (int k = 0; k < 4; ++k)
            dial(base + k, shapeX + shapeW * (2 * k + 1) / 8.0, eRow2, 17, colour, true);
        if (envTab == 0) {
            dial(cAmp + 4, eColA, eRow1, 17, colour, true);
            dial(cAmp + 5, eColB, eRow1, 17, colour, true);
            dial(cAmp + 6, (eColA + eColB) * 0.5, eRow2, 17, colour, true);
        } else if (envTab == 1) {
            dial(cEnv2 + 4, eColA, eRow1, 17, colour, true);
            dial(cEnv2 + 5, eColB, eRow1, 17, colour, true);
        }
    }

    // The LFO's shapes over its picture; beside them its rate (or
    // division), phase and delay, and under those how it runs.
    void layoutLfo() {
        const int l = lfoTab;
        for (int k = 0; k < 3; ++k)
            button(eLfoTab, k, lfoX + lfoW - 112 + k * 32, lfoY + 13, 28, 22, bSmallTab,
                   Theme::modulation);
        const double each = (cycleW - 6 * 5.0) / 7.0;
        for (int k = 0; k < 7; ++k)
            button(lc(l, lShape), k, cycleX + k * (each + 5), lfoY + 44, each, 26, bShape,
                   Theme::modulation);
        const double x0 = cycleX + cycleW + 16, w = lfoX + lfoW - 16 - x0;
        const bool synced = valueOf(lc(l, lSync)) > 0.5;
        const double y = lfoY + 112;
        dial(lc(l, synced ? lDivision : lRate), x0 + w / 6.0, y, 19, Theme::modulation, true);
        dial(lc(l, lPhase), x0 + w * 0.5, y, 19, Theme::modulation, true);
        dial(lc(l, lDelay), x0 + w * 5.0 / 6.0, y, 19, Theme::modulation, true);
        const double by = cycleY + cycleH - 22;
        const double third = (w - 10) / 3.0;
        row(lc(l, lMode), 0, 2, x0, by, 2 * third + 5, 22, Theme::modulation);
        button(lc(l, lSync), 0, x0 + 2 * (third + 5), by, third, 22, bToggle, Theme::modulation);
    }

    // How the voices play (mode, oversampling) on the left; the dials in two
    // rows: playing (voices, glide, bend, tune) and thickening (unison,
    // detune, spread, drift).
    static constexpr double vRow1 = voiY + 80;
    static constexpr double vRow2 = voiY + 164;

    void layoutVoice() {
        row(cMode, 0, 3, voiX + 16, vRow1 - 11, 210, 22, kSeaGlass);
        row(cOversampling, 0, 3, voiX + 16, vRow2 - 11, 210, 22, kSeaGlass);
        const int many = roundToInt(valueOf(cUnison));
        for (int k = 0; k < 8; ++k) {
            const int code = k == 0   ? cPolyphony
                             : k == 1 ? cGlide
                             : k == 2 ? cBend
                             : k == 3 ? cTune
                             : k == 4 ? cUnison
                             : k == 5 ? cDetune
                             : k == 6 ? cSpread
                                      : cDrift;
            const bool live = (k != 0 || choiceOf(cMode) == 0) && ((k != 5 && k != 6) || many > 1);
            dial(code, voiX + 298 + 80 * imod(k, 4), k < 4 ? vRow1 : vRow2, 17, kSeaGlass, live);
        }
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
            if (x >= buttonX[k] && x < buttonX[k] + buttonW[k] && y >= buttonY[k] &&
                y < buttonY[k] + buttonH[k])
                return k;
        return -1;
    }

    // --- names --------------------------------------------------------------------------------

    void fillType(Canvas& g, int t, double x, double y) const {
        if (t == 1) {
            g.fillText("Analog", x, y);
            return;
        }
        if (t == 2) {
            g.fillText("Wavetable", x, y);
            return;
        }
        if (t == 3) {
            g.fillText("FM", x, y);
            return;
        }
        if (t == 4) {
            g.fillText("Noise", x, y);
            return;
        }
        if (t == 5) {
            g.fillText("Sub", x, y);
            return;
        }
        g.fillText("Off", x, y);
        return;
    }
    void fillFilter(Canvas& g, int t, double x, double y) const {
        if (t == 1) {
            g.fillText("Ladder", x, y);
            return;
        }
        if (t == 2) {
            g.fillText("SVF", x, y);
            return;
        }
        if (t == 3) {
            g.fillText("Comb", x, y);
            return;
        }
        if (t == 4) {
            g.fillText("Formant", x, y);
            return;
        }
        g.fillText("Off", x, y);
        return;
    }
    void fillTable(Canvas& g, int t, double x, double y) const {
        if (t == 0) {
            g.fillText("Harmonic ramp", x, y);
            return;
        }
        if (t == 1) {
            g.fillText("PWM", x, y);
            return;
        }
        if (t == 2) {
            g.fillText("Formant", x, y);
            return;
        }
        if (t == 3) {
            g.fillText("Bell", x, y);
            return;
        }
        if (t == 4) {
            g.fillText("Organ", x, y);
            return;
        }
        if (t == 5) {
            g.fillText("Digital", x, y);
            return;
        }
        g.fillText("Spectral comb", x, y);
        return;
    }
    void fillOsc(Canvas& g, int o, double x, double y) const {
        if (o == 0) {
            g.fillText("Osc 1", x, y);
            return;
        }
        if (o == 1) {
            g.fillText("Osc 2", x, y);
            return;
        }
        if (o == 2) {
            g.fillText("Osc 3", x, y);
            return;
        }
        g.fillText("Osc 4", x, y);
        return;
    }

    // What a button says.
    void fillButton(Canvas& g, int code, int value, double x, double y) const {
        if (code == eEnvTab) {
            g.fillText(value == 0 ? "AMP" : value == 1 ? "ENV 2" : "ENV 3", x, y);
            return;
        }
        if (code == eLfoTab) {
            g.fillText(value == 0 ? "1" : value == 1 ? "2" : "3", x, y);
            return;
        }
        if (code == eTableStep) return;
        if (code == eCombSign) {
            g.fillText(value > 0 ? "+ ALL" : "- ODD", x, y);
            return;
        }
        if (code == cMode) {
            g.fillText(value == 0 ? "POLY" : value == 1 ? "MONO" : "LEGATO", x, y);
            return;
        }
        if (code == cOversampling) {
            g.fillText(value == 0 ? "1x" : value == 1 ? "2x" : "4x", x, y);
            return;
        }
        if (code == cRouting) {
            g.fillText(value == 0 ? "SERIAL" : "PARALLEL", x, y);
            return;
        }
        if (code >= 200) {
            const int field = imod(code - 200, 20);
            if (field == fType) {
                if (value == 1) {
                    g.fillText("LADDER", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("SVF", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("COMB", x, y);
                    return;
                }
                if (value == 4) {
                    g.fillText("FORMANT", x, y);
                    return;
                }
                g.fillText("OFF", x, y);
                return;
            }
            if (field == fSlope) {
                g.fillText(value == 0 ? "12 dB" : "24 dB", x, y);
                return;
            }
            if (field == fMode) {
                if (value == 1) {
                    g.fillText("BAND", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("HIGH", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("NOTCH", x, y);
                    return;
                }
                g.fillText("LOW", x, y);
                return;
            }
            if (field == fVowel) {
                if (value == 1) {
                    g.fillText("E", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("I", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("O", x, y);
                    return;
                }
                if (value == 4) {
                    g.fillText("U", x, y);
                    return;
                }
                g.fillText("A", x, y);
                return;
            }
            g.fillText("", x, y);
            return;
        }
        if (code >= 100) {
            const int o = idiv(code - 100, 20);
            const int field = imod(code - 100, 20);
            if (field == oType) {
                if (value == 1) {
                    g.fillText("ANALOG", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("WAVETABLE", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("FM", x, y);
                    return;
                }
                if (value == 4) {
                    g.fillText("NOISE", x, y);
                    return;
                }
                if (value == 5) {
                    g.fillText("SUB", x, y);
                    return;
                }
                g.fillText("OFF", x, y);
                return;
            }
            if (field == oWave) {
                if (value == 1) {
                    g.fillText("SQR", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("PULSE", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("TRI", x, y);
                    return;
                }
                if (value == 4) {
                    g.fillText("SINE", x, y);
                    return;
                }
                g.fillText("SAW", x, y);
                return;
            }
            if (field == oColor) {
                if (value == 1) {
                    g.fillText("PINK", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("BROWN", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("BLUE", x, y);
                    return;
                }
                g.fillText("WHITE", x, y);
                return;
            }
            if (field == oOctave) {
                g.fillText(value == -1 ? "−1 OCT" : "−2 OCT", x, y);
                return;
            }
            if (field == oTo) {
                if (value == 1) {
                    g.fillText("FILTER 2", x, y);
                    return;
                }
                if (value == 2) {
                    g.fillText("BOTH", x, y);
                    return;
                }
                if (value == 3) {
                    g.fillText("DIRECT", x, y);
                    return;
                }
                g.fillText("FILTER 1", x, y);
                return;
            }
            if (field == oFmFrom) {
                // The other oscillators, in order.
                const int other = value < o ? value : value + 1;
                g.fillText(other == 0 ? "1" : other == 1 ? "2" : other == 2 ? "3" : "4", x, y);
                return;
            }
            if (field == oSync) {
                g.fillText("SYNC", x, y);
                return;
            }
            if (field == oRetrigger) {
                g.fillText("RETRIG", x, y);
                return;
            }
            g.fillText("", x, y);
            return;
        }
        if (code >= 50 && code < 80) {
            const int field = imod(code - 50, 10);
            if (field == lMode) {
                g.fillText(value == 0 ? "NOTE" : "FREE", x, y);
                return;
            }
            if (field == lSync) {
                g.fillText("SYNC", x, y);
                return;
            }
        }
        g.fillText("", x, y);
        return;
    }

    // A dial's name.
    void fillDialName(Canvas& g, int code, double x, double y) const {
        if (code >= cMacro && code < cMacro + 4) {
            if (code == cMacro) {
                g.fillText("MACRO 1", x, y);
                return;
            }
            if (code == cMacro + 1) {
                g.fillText("MACRO 2", x, y);
                return;
            }
            if (code == cMacro + 2) {
                g.fillText("MACRO 3", x, y);
                return;
            }
            g.fillText("MACRO 4", x, y);
            return;
        }
        if (code == cOutput) {
            g.fillText("OUTPUT", x, y);
            return;
        }
        if (code == cPolyphony) {
            g.fillText("VOICES", x, y);
            return;
        }
        if (code == cGlide) {
            g.fillText("GLIDE", x, y);
            return;
        }
        if (code == cBend) {
            g.fillText("BEND", x, y);
            return;
        }
        if (code == cTune) {
            g.fillText("TUNE", x, y);
            return;
        }
        if (code == cUnison) {
            g.fillText("UNISON", x, y);
            return;
        }
        if (code == cDetune) {
            g.fillText("DETUNE", x, y);
            return;
        }
        if (code == cSpread) {
            g.fillText("SPREAD", x, y);
            return;
        }
        if (code == cDrift) {
            g.fillText("DRIFT", x, y);
            return;
        }
        if (code >= cAmp && code < cEnv3 + 4) {
            const int k = imod(code - cAmp, 10);
            if (k == 0) {
                g.fillText("ATTACK", x, y);
                return;
            }
            if (k == 1) {
                g.fillText("DECAY", x, y);
                return;
            }
            if (k == 2) {
                g.fillText("SUSTAIN", x, y);
                return;
            }
            if (k == 3) {
                g.fillText("RELEASE", x, y);
                return;
            }
            if (code == cAmp + 4) {
                g.fillText("VELOCITY", x, y);
                return;
            }
            if (code == cAmp + 5) {
                g.fillText("LEVEL", x, y);
                return;
            }
            if (code == cAmp + 6) {
                g.fillText("PAN", x, y);
                return;
            }
            if (code == cEnv2 + 4) {
                g.fillText("PITCH", x, y);
                return;
            }
            if (code == cEnv2 + 5) {
                g.fillText("FM DEPTH", x, y);
                return;
            }
            g.fillText("", x, y);
            return;
        }
        if (code >= 50 && code < 80) {
            const int field = imod(code - 50, 10);
            if (field == lRate) {
                g.fillText("RATE", x, y);
                return;
            }
            if (field == lDivision) {
                g.fillText("DIVISION", x, y);
                return;
            }
            if (field == lPhase) {
                g.fillText("PHASE", x, y);
                return;
            }
            if (field == lDelay) {
                g.fillText("DELAY", x, y);
                return;
            }
            g.fillText("", x, y);
            return;
        }
        if (code >= 200) {
            const int field = imod(code - 200, 20);
            const int type = choiceOf(code - field);
            if (field == fCutoff) {
                g.fillText(type == kComb ? "PITCH" : "CUTOFF", x, y);
                return;
            }
            if (field == fResonance) {
                g.fillText("RESONANCE", x, y);
                return;
            }
            if (field == fFeedback) {
                g.fillText("FEEDBACK", x, y);
                return;
            }
            if (field == fDamping) {
                g.fillText("DAMPING", x, y);
                return;
            }
            if (field == fVowel) {
                g.fillText("VOWEL", x, y);
                return;
            }
            if (field == fShift) {
                g.fillText("SHIFT", x, y);
                return;
            }
            if (field == fKeytrack) {
                g.fillText("KEYTRACK", x, y);
                return;
            }
            if (field == fEnv) {
                g.fillText("ENV 2", x, y);
                return;
            }
            if (field == fVelocity) {
                g.fillText("VELOCITY", x, y);
                return;
            }
            if (field == fDrive) {
                g.fillText("DRIVE", x, y);
                return;
            }
            g.fillText("", x, y);
            return;
        }
        if (code >= 100) {
            const int field = imod(code - 100, 20);
            if (field == oLevel) {
                g.fillText("LEVEL", x, y);
                return;
            }
            if (field == oPan) {
                g.fillText("PAN", x, y);
                return;
            }
            if (field == oCoarse) {
                g.fillText("COARSE", x, y);
                return;
            }
            if (field == oFine) {
                g.fillText("FINE", x, y);
                return;
            }
            if (field == oWidth) {
                g.fillText("WIDTH", x, y);
                return;
            }
            if (field == oPosition) {
                g.fillText("POSITION", x, y);
                return;
            }
            if (field == oRatio) {
                g.fillText("RATIO", x, y);
                return;
            }
            if (field == oFeedback) {
                g.fillText("FEEDBACK", x, y);
                return;
            }
            if (field == oFm) {
                g.fillText("FM", x, y);
                return;
            }
            g.fillText("", x, y);
            return;
        }
        g.fillText("", x, y);
        return;
    }

    // Whether a dial lights from its middle.
    bool bipolar(int code) const {
        if (code == cTune || code == cAmp + 6 || code == cEnv2 + 4) return true;
        if (code >= 200) {
            const int field = imod(code - 200, 20);
            return field == fFeedback || field == fShift || field == fEnv;
        }
        if (code >= 100) {
            const int field = imod(code - 100, 20);
            return field == oPan || field == oCoarse || field == oFine;
        }
        return false;
    }

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
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 20 ? 5.5 : 4.5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(readParam(code, 0), 0.0, 1.0);
        const double from = bipolar(code) ? start + 0.5 * sweep : start;
        g.strokeStyle(dialColour[k]);
        g.lineWidth(r > 20 ? 4.0 : 3.2);
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
        g.circle(x, y, r - 6);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(1.8);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 13), y + sin(at) * (r - 13));
        g.lineTo(x + cos(at) * (r - 7.5), y + sin(at) * (r - 7.5));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        fillDialName(g, code, x, y - r - 9);
        g.fillStyle(hot ? Theme::text : Theme::textMuted);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        fillParam(g, code, x, y + r + 9);
        g.globalAlpha(1.0);
    }

    bool buttonLit(int k) const {
        const int code = buttonCode[k], value = buttonValue[k];
        if (code == eOscTab) return value == oscTab;
        if (code == eFilterTab) return value == filterTab;
        if (code == eEnvTab) return value == envTab;
        if (code == eLfoTab) return value == lfoTab;
        if (code == eCombSign) return (valueOf(fc(filterTab, fFeedback)) < 0.0) == (value < 0);
        if (code < 0) return false;
        if (buttonStyle[k] == bToggle) return valueOf(code) > 0.5;
        if (code >= 200 && imod(code - 200, 20) == fVowel) return abs(valueOf(code) - value) < 0.5;
        if (code >= 100 && code < 200 && imod(code - 100, 20) == oOctave)
            return roundToInt(valueOf(code)) == value;
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
        if (style == bTab) return;  // its panel draws it
        const double x = buttonX[k], y = buttonY[k], w = buttonW[k], h = buttonH[k];
        const bool hot = k == hotButton;
        const bool lit = buttonLit(k);
        if (style == bStep) {
            g.strokeStyle(hot ? Theme::text : Theme::textMuted);
            g.lineWidth(1.6);
            g.lineCap(LineCap::Round);
            const double cx = x + w * 0.5, cy = y + h * 0.5, d = buttonValue[k] < 0 ? 1.0 : -1.0;
            g.beginPath();
            g.moveTo(cx + 3 * d, cy - 5);
            g.lineTo(cx - 3 * d, cy);
            g.lineTo(cx + 3 * d, cy + 5);
            g.stroke();
            return;
        }
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
        pill(g, x, y, w, h, lit, buttonColour[k], hot);
        fillButton(g, buttonCode[k], buttonValue[k], x + w * 0.5, y + h * 0.5 + 0.5);
    }

    // A tab of the oscillators or the filters: its name, its type, a dot
    // lit while it plays.
    void drawTab(Canvas& g, int k, int type, bool on) {
        const bool osc = buttonCode[k] == eOscTab;
        const double x = buttonX[k], y = buttonY[k], w = buttonW[k], h = buttonH[k];
        const bool lit = buttonLit(k);
        const bool hot = k == hotButton;
        g.fillStyle(lit ? Theme::raised : (hot ? 0x2E251E : kInk));
        g.beginPath();
        g.roundRect(x, y, w, h, 7);
        g.fill();
        g.strokeStyle(lit ? Theme::lineLit : Theme::line);
        g.lineWidth(1);
        g.stroke();
        if (lit) {
            g.strokeStyle(buttonColour[k]);
            g.lineWidth(2);
            g.beginPath();
            g.moveTo(x + 10, y + h - 1.5);
            g.lineTo(x + w - 10, y + h - 1.5);
            g.stroke();
        }
        g.fillStyle(lit ? Theme::text : Theme::textMuted);
        g.boldFont(10.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        if (osc)
            fillOsc(g, buttonValue[k], x + 12, y + 13);
        else
            g.fillText(buttonValue[k] == 0 ? "Filter 1" : "Filter 2", x + 12, y + 13);
        g.fillStyle(on ? (lit ? buttonColour[k] : Theme::textMuted) : Theme::textFaint);
        g.serifItalicFont(11);
        if (osc)
            fillType(g, type, x + 12, y + 27);
        else
            fillFilter(g, type, x + 12, y + 27);
        g.fillStyle(on ? buttonColour[k] : kInk);
        g.beginPath();
        g.circle(x + w - 14, y + h * 0.5, 3.5);
        g.fill();
        if (!on) {
            g.strokeStyle(Theme::lineLit);
            g.lineWidth(1);
            g.stroke();
        }
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

    // Marram on a dune: a swell of sand and a tuft of blades leaning with
    // the wind; a blade lights for each voice playing.
    void drawMark(Canvas& g, double x, double y) {
        g.linearGradient(0, y + 8, 0, y + 22, kSand, 0x9C7A4C);
        g.beginPath();
        g.moveTo(x - 20, y + 22);
        g.bezierCurveTo(x - 10, y + 8, x + 8, y + 6, x + 22, y + 22);
        g.closePath();
        g.fill();
        const int playing = soundingNow.get();
        g.lineCap(LineCap::Round);
        for (int k = 0; k < 7; ++k) {
            const double base = x - 9 + k * 3.2;
            const double tall = 22 + 6 * sin(k * 1.7);
            const double lean = 5 + k * 1.3;
            g.strokeStyle(k < playing ? kLinen : (imod(k, 2) == 0 ? kGrass : 0x8A9A57));
            g.lineWidth(1.5);
            g.beginPath();
            g.moveTo(base, y + 12);
            g.quadraticCurveTo(base + lean * 0.2, y + 12 - tall * 0.6, base + lean, y + 12 - tall);
            g.stroke();
        }
    }

    // The patch in a phrase: its oscillators, and the filters they go
    // through.
    void drawSummary(Canvas& g, double x, double y) {
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        int any = 0;
        for (int o = 0; o < 4; ++o) {
            const int t = choiceOf(oc(o, oType));
            if (t == kOff) continue;
            if (any > 0) {
                g.fillText("+", x, y);
                x += g.measureText("+") + 5;
            }
            const char* name = t == kAnalog      ? "analog"
                               : t == kWavetable ? "wavetable"
                               : t == kFm        ? "fm"
                               : t == kNoise     ? "noise"
                                                 : "sub";
            g.fillText(name, x, y);
            x += g.measureText(name) + 5;
            ++any;
        }
        if (any == 0) {
            g.fillText("silent: every oscillator is off", x, y);
            return;
        }
        const int a = choiceOf(fc(0, fType)), b = choiceOf(fc(1, fType));
        if (a == kOff && b == kOff) return;
        g.fillText("through", x, y);
        x += g.measureText("through") + 5;
        const char* first = a == kLadder ? "a ladder"
                            : a == kSvf  ? "an svf"
                            : a == kComb ? "a comb"
                                         : "formants";
        const char* second = b == kLadder ? "a ladder"
                             : b == kSvf  ? "an svf"
                             : b == kComb ? "a comb"
                                          : "formants";
        if (a != kOff) {
            g.fillText(first, x, y);
            x += g.measureText(first) + 5;
        }
        if (a != kOff && b != kOff) {
            const char* join = choiceOf(cRouting) == 0 ? "into" : "and";
            g.fillText(join, x, y);
            x += g.measureText(join) + 5;
        }
        if (b != kOff) g.fillText(second, x, y);
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 32, 30);
        g.fillStyle(Theme::text);
        g.serifFont(25);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Marram", 62, 41);
        drawSummary(g, 70 + g.measureText("Marram"), 41);
        // The voices playing, of how many.
        label(g, "VOICES", 880, 24, TextAlign::Center);
        g.fillStyle(Theme::text);
        g.monoFont(13);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        const int most = choiceOf(cMode) == 0 ? roundToInt(valueOf(cPolyphony)) : 1;
        g.fillText(format("%d / %d", soundingNow.get(), most), 880, 42);
        label(g, "OUT", 1004, 30, TextAlign::Right);
        drawLevel(g, 1012, 25, 200, outLeft.get(), outRight.get());
    }

    // --- the oscillators ----------------------------------------------------------------------

    // One cycle's point t (0..1) of oscillator o's wave, as its picture
    // shows it.
    double wavePoint(int o, int type, double t, int at) {
        if (type == kAnalog || type == kSub) {
            const int wave = choiceOf(oc(o, oWave));
            return analogWave(wave, wrap01(t), 1.0 / 512.0, valueOf(oc(o, oWidth)));
        }
        if (type == kWavetable) {
            const int table = choiceOf(oc(o, oTable));
            if (table == 5) {
                // Digital: the steps themselves.
                const double u = wrap01(t);
                const double a = stepsInA == 0
                                     ? 2.0 * u - 1.0
                                     : stepsA[min(floorToInt(u * stepsInA), stepsInA - 1)];
                const double b = stepsInB == 0
                                     ? 2.0 * u - 1.0
                                     : stepsB[min(floorToInt(u * stepsInB), stepsInB - 1)];
                return a + stepsBlend * (b - a);
            }
            double y = 0.0;
            for (int k = 1; k <= 48; ++k) y += frameAmp[k] * sin(twoPi * k * t + framePhase[k]);
            return y;
        }
        if (type == kFm) {
            const double ratio = valueOf(oc(o, oRatio));
            return sin(twoPi * ratio * t +
                       valueOf(oc(o, oFeedback)) * 1.6 * sin(twoPi * ratio * t));
        }
        // Noise: a fixed stretch of it, rougher or smoother by its colour.
        const int colour = choiceOf(oc(o, oColor));
        const double a = scatter(at, 11), b = scatter(at + 1, 11), c = scatter(idiv(at, 6), 5);
        if (colour == 0) return a;
        if (colour == 1) return 0.55 * (a + b) * 0.8 + 0.3 * c;
        if (colour == 2) return 0.85 * c + 0.12 * a;
        return 0.9 * (a - b);
    }

    // Table t's frame at position p, for wavePoint().
    void pictureOf(int t, double p) {
        if (t != 5) {
            harmonics(t, p, frameAmp, framePhase, 48);
            return;
        }
        const double at = clamp(p, 0.0, 1.0) * (kFrames - 1);
        const int f0 = min(floorToInt(at), kFrames - 2);
        stepsInA = f0 == 0 ? 0 : digitalSteps(f0, stepsA);
        stepsInB = digitalSteps(f0 + 1, stepsB);
        stepsBlend = stepped(at - f0);
    }

    void drawWave(Canvas& g) {
        const double x = waveX, y = waveY, w = waveW, h = waveH;
        well(g, x, y, w, h);
        const int o = oscTab;
        const int type = choiceOf(oc(o, oType));
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(x + 8, y + h * 0.5);
        g.lineTo(x + w - 8, y + h * 0.5);
        g.stroke();
        if (type == kOff) {
            aside(g, "off: choose what it is above", x + w * 0.5, y + h * 0.5 - 14,
                  TextAlign::Center);
            return;
        }
        // Two cycles, as tall as its level.
        const double gain = 0.42 * h * min(1.0, sqrt(dbToGain(valueOf(oc(o, oLevel))) * 2.0));
        const double cycles = 2.0;
        double peak = 1.0;
        if (type == kWavetable) {
            // The other frames faintly behind this one.
            g.save();
            g.clip(x + 2, y + 2, w - 4, h - 4);
            for (int f = 0; f <= 4; ++f) {
                pictureOf(choiceOf(oc(o, oTable)), f / 4.0);
                double most = 0.0;
                for (int i = 0; i <= 60; ++i)
                    most = max(most, abs(wavePoint(o, type, i / 60.0, i)));
                g.strokeStyle(kGrass);
                g.globalAlpha(0.12);
                g.lineWidth(1);
                g.beginPath();
                for (int i = 0; i <= 120; ++i) {
                    const double t = i / 120.0;
                    const double v = wavePoint(o, type, t * cycles, i) / max(most, 1e-6);
                    if (i == 0)
                        g.moveTo(x + 8 + t * (w - 16), y + h * 0.5 - v * gain);
                    else
                        g.lineTo(x + 8 + t * (w - 16), y + h * 0.5 - v * gain);
                }
                g.stroke();
            }
            g.globalAlpha(1.0);
            g.restore();
            pictureOf(choiceOf(oc(o, oTable)), valueOf(oc(o, oPosition)));
            peak = 0.0;
            for (int i = 0; i <= 120; ++i) peak = max(peak, abs(wavePoint(o, type, i / 120.0, i)));
            peak = max(peak, 1e-6);
        }
        g.strokeStyle(kGrass);
        g.lineWidth(1.8);
        g.lineJoin(LineJoin::Round);
        g.beginPath();
        for (int i = 0; i <= 240; ++i) {
            const double t = i / 240.0;
            const double v = clamp(wavePoint(o, type, t * cycles, i) / peak, -1.1, 1.1);
            if (i == 0)
                g.moveTo(x + 8 + t * (w - 16), y + h * 0.5 - v * gain);
            else
                g.lineTo(x + 8 + t * (w - 16), y + h * 0.5 - v * gain);
        }
        g.stroke();
        // The table's name, between its arrows.
        if (type == kWavetable) {
            g.fillStyle(Theme::text);
            g.boldFont(9.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            fillTable(g, choiceOf(oc(o, oTable)), x + w * 0.5, y + h + 19);
        }
    }

    void drawOscPanel(Canvas& g) {
        panel(g, oscX, oscY, oscW, oscH);
        heading(g, "Oscillators", oscX + 18, oscY + 24);
        aside(g, "what each note is made of", oscX + oscW - 18, oscY + 24, TextAlign::Right);
        for (int k = 0; k < buttons; ++k)
            if (buttonCode[k] == eOscTab) {
                const int o = buttonValue[k];
                const int t = choiceOf(oc(o, oType));
                drawTab(g, k, t, t != kOff);
            }
        drawWave(g);
        const int type = choiceOf(oc(oscTab, oType));
        if (type == kOff) return;
        const double by = oscY + oscH - 38;
        label(g, "TO", oscX + 18, by + 11, TextAlign::Left);
        if (type == kAnalog || type == kWavetable || type == kFm)
            label(g, "FM FROM", oscX + 330, by + 11, TextAlign::Right);
        if (type == kSub) label(g, "OCTAVE", oscX + 312 + 39, oscY + 214, TextAlign::Center);
    }

    // --- the filters --------------------------------------------------------------------------

    // Filter f's response at hz, in dB, as its settings make it (the key at
    // middle C, the envelope at rest).
    double responseDb(int f, double hz) {
        const int type = choiceOf(fc(f, fType));
        const double drive = dbToGain(valueOf(fc(f, fDrive)));
        double re = 1.0, im = 0.0;
        if (type == kLadder || type == kSvf) {
            const double w = hz / valueOf(fc(f, fCutoff));
            const double res = valueOf(fc(f, fResonance));
            if (type == kLadder) {
                // G = 1 / (1 + jw); the ladder's G^4 / (1 + k G^4), or G^2
                // over the same for 12 dB.
                const double k = 4.0 * res;
                const double d = 1.0 + w * w;
                const double gr = 1.0 / d, gi = -w / d;
                const double g2r = gr * gr - gi * gi, g2i = 2.0 * gr * gi;
                const double g4r = g2r * g2r - g2i * g2i, g4i = 2.0 * g2r * g2i;
                const double nr = choiceOf(fc(f, fSlope)) == 1 ? g4r : g2r;
                const double ni = choiceOf(fc(f, fSlope)) == 1 ? g4i : g2i;
                const double dr = 1.0 + k * g4r, di = k * g4i;
                const double m = dr * dr + di * di;
                const double scale = (1.0 + 0.5 * k) * drive / max(1.0, drive * 0.5 + 0.5);
                re = (nr * dr + ni * di) / m * scale;
                im = (ni * dr - nr * di) / m * scale;
            } else {
                const double k = max(0.005, 2.0 * (1.0 - res) * (1.0 - res));
                const double dr = 1.0 - w * w, di = k * w;
                const int mode = choiceOf(fc(f, fMode));
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
        } else if (type == kComb) {
            // 1 / (1 - fb LP(z) z^-D), D a period of the comb's pitch.
            const double fb = valueOf(fc(f, fFeedback));
            const double a = 1.0 - valueOf(fc(f, fDamping)) * 0.95;
            const double rate = 96000.0;
            const double wz = twoPi * hz / rate;
            const double lr = 1.0 - (1.0 - a) * cos(wz), li = (1.0 - a) * sin(wz);
            const double lm = lr * lr + li * li;
            const double pr = a * lr / lm, pi2 = -a * li / lm;
            const double turn = -twoPi * hz / max(valueOf(fc(f, fCutoff)), 25.0);
            const double zr = cos(turn), zi = sin(turn);
            const double tr = fb * (pr * zr - pi2 * zi), ti = fb * (pr * zi + pi2 * zr);
            const double dr = 1.0 - tr, di = -ti;
            const double m = dr * dr + di * di;
            const double scale = (1.0 - abs(fb) * 0.5) * drive;
            re = dr / m * scale;
            im = -di / m * scale;
        } else if (type == kFormant) {
            const double v = clamp(valueOf(fc(f, fVowel)), 0.0, 4.0);
            const int i = min(floorToInt(v), 3);
            const double t = v - i;
            const double up = exp2(valueOf(fc(f, fShift)) / 12.0);
            re = 0.0;
            for (int b = 0; b < 3; ++b) {
                const double centre = lerp(formantHz(i, b), formantHz(i + 1, b), t) * up;
                const double k = max(b == 0 ? 80.0 : b == 1 ? 90.0 : 120.0, centre / 6.0) / centre;
                const double w = hz / centre;
                const double dr = 1.0 - w * w, di = k * w;
                const double m = dr * dr + di * di;
                const double weight = (b == 0 ? 1.0 : b == 1 ? 0.6 : 0.35) * 2.5 * drive;
                re += k * w * di / m * weight;
                im += k * w * dr / m * weight;
            }
        }
        return gainToDb(sqrt(re * re + im * im));
    }

    double hzAt(double x) const { return 20.0 * exp2((x - curveX) / curveW * log2(1000.0)); }
    double xAt(double hz) const {
        return curveX + curveW * log2(max(hz, 20.0) / 20.0) / log2(1000.0);
    }
    double dbY(double db) const {
        return curveY + 10 + (curveH - 20) * (18.0 - clamp(db, -42.0, 18.0)) / 60.0;
    }

    void drawCurve(Canvas& g, int f, int colour, double alpha, bool fill) {
        g.beginPath();
        for (int i = 0; i <= 160; ++i) {
            const double x = curveX + curveW * i / 160.0;
            const double y = dbY(responseDb(f, hzAt(x)));
            if (i == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        if (fill) {
            g.lineTo(curveX + curveW, curveY + curveH);
            g.lineTo(curveX, curveY + curveH);
            g.closePath();
            g.fillStyle(colour);
            g.globalAlpha(0.08);
            g.fill();
            g.beginPath();
            for (int i = 0; i <= 160; ++i) {
                const double x = curveX + curveW * i / 160.0;
                const double y = dbY(responseDb(f, hzAt(x)));
                if (i == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
        }
        g.globalAlpha(alpha);
        g.strokeStyle(colour);
        g.lineWidth(fill ? 1.8 : 1.2);
        g.lineJoin(LineJoin::Round);
        g.stroke();
        g.globalAlpha(1.0);
    }

    // Where the curve's handle is: the cutoff across, the resonance (or the
    // comb's feedback, the formants' shift) up.
    double nodeX(int f) const {
        if (choiceOf(fc(f, fType)) == kFormant)
            return curveX + 12 + (curveW - 24) * clamp(valueOf(fc(f, fVowel)) / 4.0, 0.0, 1.0);
        return clamp(xAt(valueOf(fc(f, fCutoff))), curveX + 6, curveX + curveW - 6);
    }
    double nodeY(int f) const {
        const int type = choiceOf(fc(f, fType));
        double t = valueOf(fc(f, fResonance));
        if (type == kComb) t = 0.5 + 0.5 * valueOf(fc(f, fFeedback)) / 0.99;
        if (type == kFormant) t = 0.5 + valueOf(fc(f, fShift)) / 24.0;
        return curveY + curveH - 12 - (curveH - 24) * clamp(t, 0.0, 1.0);
    }

    void drawFilterPanel(Canvas& g) {
        panel(g, filX, filY, filW, filH);
        heading(g, "Filters", filX + 18, filY + 24);
        for (int k = 0; k < buttons; ++k)
            if (buttonCode[k] == eFilterTab) {
                const int f = buttonValue[k];
                const int t = choiceOf(fc(f, fType));
                drawTab(g, k, t, t != kOff);
            }
        // How the two are joined.
        const double rx = filX + 400, ry = filY + 61;
        const bool both = choiceOf(fc(0, fType)) != kOff && choiceOf(fc(1, fType)) != kOff;
        const bool serial = choiceOf(cRouting) == 0;
        g.strokeStyle(both ? kSand : Theme::lineLit);
        g.lineWidth(1.2);
        g.globalAlpha(both ? 0.9 : 0.5);
        if (serial) {
            g.beginPath();
            g.moveTo(rx, ry);
            g.lineTo(rx + 160, ry);
            g.stroke();
            for (int k = 0; k < 2; ++k) {
                g.fillStyle(Theme::raised);
                g.beginPath();
                g.roundRect(rx + 30 + k * 70, ry - 9, 34, 18, 4);
                g.fill();
                g.stroke();
            }
        } else {
            g.beginPath();
            g.moveTo(rx, ry);
            g.lineTo(rx + 20, ry);
            g.moveTo(rx + 20, ry - 10);
            g.lineTo(rx + 20, ry + 10);
            g.moveTo(rx + 20, ry - 10);
            g.lineTo(rx + 140, ry - 10);
            g.moveTo(rx + 20, ry + 10);
            g.lineTo(rx + 140, ry + 10);
            g.moveTo(rx + 140, ry - 10);
            g.lineTo(rx + 140, ry + 10);
            g.moveTo(rx + 140, ry);
            g.lineTo(rx + 160, ry);
            g.stroke();
            for (int k = 0; k < 2; ++k) {
                g.fillStyle(Theme::raised);
                g.beginPath();
                g.roundRect(rx + 63, ry - 18 + k * 20, 34, 16, 4);
                g.fill();
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        g.fillStyle(Theme::textMuted);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("1", serial ? rx + 47 : rx + 80, serial ? ry : ry - 10);
        g.fillText("2", serial ? rx + 117 : rx + 80, serial ? ry : ry + 10);

        // The response.
        well(g, curveX, curveY, curveW, curveH);
        g.frequencyGrid(curveX, curveY, curveW, curveH, 20, 20000);
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(curveX + 4, dbY(0.0));
        g.lineTo(curveX + curveW - 4, dbY(0.0));
        g.stroke();
        const int f = filterTab;
        const int type = choiceOf(fc(f, fType));
        g.save();
        g.clip(curveX + 1, curveY + 1, curveW - 2, curveH - 2);
        const int other = 1 - f;
        if (choiceOf(fc(other, fType)) != kOff) drawCurve(g, other, Theme::textFaint, 0.5, false);
        if (type != kOff) drawCurve(g, f, kSand, 1.0, true);
        g.restore();
        if (type == kOff) {
            aside(g, "off: the oscillators sent here pass through", curveX + curveW * 0.5,
                  curveY + curveH * 0.5 - 16, TextAlign::Center);
            return;
        }
        const double nx = nodeX(f), ny = nodeY(f);
        const bool hot = dragging == 2 || hovered(nx - 10, ny - 10, 20, 20);
        g.fillStyle(kInk);
        g.beginPath();
        g.circle(nx, ny, hot ? 7.5 : 6.5);
        g.fill();
        g.strokeStyle(kSand);
        g.lineWidth(2);
        g.stroke();
        // The names over the choices, as a dial's.
        const double cx = fCol + 2.5 * fStep;
        if (type == kLadder) label(g, "SLOPE", cx, fRow1 - 30, TextAlign::Center);
        if (type == kSvf) label(g, "MODE", cx, fRow1 - 30, TextAlign::Center);
        if (type == kFormant) label(g, "VOWEL", cx, fRow1 - 30, TextAlign::Center);
        if (type == kComb) {
            label(g, "POLARITY", fCol + 3 * fStep, fRow1 - 30, TextAlign::Center);
            aside(g, "a comb tuned to its pitch: keytrack 100 % plays the note",
                  curveX + curveW * 0.5, curveY + curveH + 34, TextAlign::Center);
        }
    }

    // --- the envelopes ------------------------------------------------------------------------

    // How wide a stage of `ms` is drawn, before fitting.
    double stageWidth(double ms) const { return 10.0 + 22.0 * log2(1.0 + ms / 5.0); }

    // The envelope's corners: where attack, decay and release end.
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
    double levelY(double level) const { return shapeY + shapeH - 10 - (shapeH - 22) * level; }

    void drawEnvPanel(Canvas& g) {
        panel(g, envX, envY, envW, envH);
        heading(g, "Envelopes", envX + 18, envY + 24);
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
        // The handles: the attack's peak, the decay's end, the release's.
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
        if (envTab == 1) {
            const double mid = (eColA + eColB) * 0.5;
            aside(g, "it moves each cutoff", mid, eRow2 - 8, TextAlign::Center);
            aside(g, "by its filter's Env 2", mid, eRow2 + 8, TextAlign::Center);
        }
        if (envTab == 2) {
            const double mid = (eColA + eColB) * 0.5;
            aside(g, "a source for routes alone:", mid, eRow1 - 8, TextAlign::Center);
            aside(g, "route env3 to anything", mid, eRow1 + 8, TextAlign::Center);
        }
    }

    // --- the LFOs -----------------------------------------------------------------------------

    void drawLfoPanel(Canvas& g) {
        panel(g, lfoX, lfoY, lfoW, lfoH);
        heading(g, "LFOs", lfoX + 18, lfoY + 24);
        well(g, cycleX, cycleY, cycleW, cycleH);
        const int l = lfoTab;
        const int shape = choiceOf(lc(l, lShape));
        const double mid = cycleY + cycleH * 0.5, half = cycleH * 0.38;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(cycleX + 6, mid);
        g.lineTo(cycleX + cycleW - 6, mid);
        g.stroke();
        // Two cycles from a note's start, fading in over the delay.
        const bool synced = valueOf(lc(l, lSync)) > 0.5;
        const double hz =
            synced ? 2.0 / divisionBeats(choiceOf(lc(l, lDivision))) : valueOf(lc(l, lRate));
        const double seconds = 2.0 / max(hz, 0.001);
        const double delay = valueOf(lc(l, lDelay)) * 0.001;
        const double start = valueOf(lc(l, lPhase)) / 360.0;
        const bool note = choiceOf(lc(l, lMode)) == 0;
        for (int pass = 0; pass < 2; ++pass) {
            g.beginPath();
            for (int i = 0; i <= 160; ++i) {
                const double t = i / 160.0;
                const double total = start + 2.0 * t;
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
        // Where it is now.
        const double t = lfoShown[l];
        if (t >= 0.0) {
            const double total = start + 2.0 * t;
            const double v = lfoWave(shape, total - floor(total), floorToInt(total), 7);
            g.fillStyle(kLinen);
            g.beginPath();
            g.circle(cycleX + 8 + t * (cycleW - 16), mid - v * half, 3.5);
            g.fill();
        }
    }

    // --- the routes and the voice -------------------------------------------------------------

    void drawModPanel(Canvas& g) {
        panel(g, modX, modY, modW, modH);
        heading(g, "Modulation", modX + 18, modY + 22);
        aside(g, "a source moves a setting by an amount in the setting's own unit",
              modX + modW - 18, modY + 22, TextAlign::Right);
    }

    void drawVoicePanel(Canvas& g) {
        panel(g, voiX, voiY, voiW, voiH);
        heading(g, "Voice", voiX + 18, voiY + 24);
        label(g, "MODE", voiX + 121, vRow1 - 26, TextAlign::Center);
        label(g, "OVERSAMPLING", voiX + 121, vRow2 - 26, TextAlign::Center);
    }

    void draw(Canvas& g) {
        place();
        g.linearGradient(0, 0, 0, kH, Theme::ground, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawOscPanel(g);
        drawFilterPanel(g);
        drawEnvPanel(g);
        drawLfoPanel(g);
        drawModPanel(g);
        drawVoicePanel(g);
        for (int k = 0; k < buttons; ++k) drawButton(g, k);
        for (int k = 0; k < dials; ++k) drawDial(g, k);
    }

    // --- the pointer --------------------------------------------------------------------------

    void pointer(const Pointer& p) {
        place();
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotDial = -1;
            hotButton = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotDial = dialAt(p.x(), p.y());
            hotButton = buttonAt(p.x(), p.y());
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
                dragCurve(p.x() - pressX, p.y() - pressY, fine);
            } else if (dragging == 3) {
                dragEnvelope(p.x() - pressX, p.y() - pressY, fine);
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotDial = dialAt(p.x(), p.y());
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
            if (p.over(curveX, curveY, curveW, curveH)) {
                const int f = filterTab;
                const int type = choiceOf(fc(f, fType));
                const int field = type == kComb      ? fFeedback
                                  : type == kFormant ? fShift
                                                     : fResonance;
                if (type != kOff)
                    editParam(fc(f, field), 1,
                              clamp(readParam(fc(f, field), 0) + notches * 0.02 * fine, 0.0, 1.0));
            }
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
        const int f = filterTab;
        const int type = choiceOf(fc(f, fType));
        if (type != kOff && p.over(curveX, curveY, curveW, curveH)) {
            dragging = 2;
            pressX = x;
            pressY = y;
            pressA = valueOf(fc(f, type == kFormant ? fVowel : fCutoff));
            pressB = valueOf(fc(f, type == kComb      ? fFeedback
                                   : type == kFormant ? fShift
                                                      : fResonance));
            return;
        }
        if (p.over(shapeX, shapeY, shapeW, shapeH)) {
            const int base = envBase(envTab);
            const double s = valueOf(base + 2);
            int best = -1;
            double closest = 400.0;
            for (int h = 0; h < 3; ++h) {
                const double hx = cornerX(base, h);
                const double hy = h == 0 ? levelY(1.0) : h == 1 ? levelY(s) : levelY(0.0);
                const double d = (x - hx) * (x - hx) + (y - hy) * (y - hy);
                if (d < closest) {
                    closest = d;
                    best = h;
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
        if (code == eOscTab) oscTab = value;
        if (code == eFilterTab) filterTab = value;
        if (code == eEnvTab) envTab = value;
        if (code == eLfoTab) lfoTab = value;
        if (code == eTableStep) {
            const int table = oc(oscTab, oTable);
            editParam(table, 0, clamp(choiceOf(table) + value, 0, 6));
        }
        if (code == eCombSign) {
            const int feedback = fc(filterTab, fFeedback);
            editParam(feedback, 0, value * abs(valueOf(feedback)));
        }
        if (code < 0) return;
        if (buttonStyle[b] == bToggle) {
            editParam(code, 0, valueOf(code) > 0.5 ? 0.0 : 1.0);
            return;
        }
        editParam(code, 0, value);
    }

    // The filter's curve: across moves the cutoff (the vowel), up and down
    // the resonance (the comb's feedback, the formants' shift).
    void dragCurve(double dx, double dy, double fine) {
        const int f = filterTab;
        const int type = choiceOf(fc(f, fType));
        const double across = dx * fine / curveW, up = -dy * fine / (curveH - 24);
        if (type == kFormant) {
            editParam(fc(f, fVowel), 0, pressA + across * 4.0);
            editParam(fc(f, fShift), 0, pressB + up * 24.0);
            return;
        }
        editParam(fc(f, fCutoff), 0, pressA * exp2(across * log2(1000.0)));
        if (type == kComb)
            editParam(fc(f, fFeedback), 0, pressB + up * 1.98);
        else
            editParam(fc(f, fResonance), 0, pressB + up);
    }

    // The envelope: a stage's time across, the sustain up and down.
    void dragEnvelope(double dx, double dy, double fine) {
        const int base = envBase(envTab);
        const int time = base + (handle == 0 ? 0 : handle == 1 ? 1 : 3);
        editParam(time, 0, max(pressA, 1.0) * exp2(dx * fine / 40.0));
        if (handle == 1) editParam(base + 2, 0, pressB - dy * fine / (shapeH - 22));
    }

    // --- Factory sounds ---------------------------------------------------------------------

    void factoryPatches(Patches& p) {
        p.add("Dune Pad")
            .set(unison, 3)
            .set(detune, 12)
            .set(spread, 70)
            .set(drift, 25)
            .set(o1Fine, -7)
            .set(o2Type, "analog")
            .set(o2Fine, 7)
            .set(o2Level, -6)
            .set(f1Cutoff, 1200)
            .set(f1Resonance, 25)
            .set(f1Env, 2)
            .set(envAttack, 1500)
            .set(envDecay, 3000)
            .set(envSustain, 30)
            .set(envRelease, 2000)
            .set(ampAttack, 600)
            .set(ampDecay, 1200)
            .set(ampSustain, 80)
            .set(ampRelease, 1500)
            .set(l1Rate, 0.12)
            .set(l1Mode, "free")
            .route("lfo1>filter1.cutoff", 0.6)
            .route("modwheel>filter1.cutoff", 1.5);
        p.add("Pluck Bass")
            .set(o1Retrigger, true)
            .set(o2Type, "sub")
            .set(o2Wave, "sine")
            .set(o2Level, -2)
            .set(o2Retrigger, true)
            .set(f1Cutoff, 600)
            .set(f1Resonance, 30)
            .set(f1Env, 3.5)
            .set(f1Keytrack, 50)
            .set(envDecay, 250)
            .set(envSustain, 0)
            .set(envRelease, 200)
            .set(ampDecay, 450)
            .set(ampSustain, 50)
            .set(ampRelease, 120)
            .set(ampVelocity, 60)
            .set(o1Level, 1);
        p.add("Acid Line")
            .set(mode, "legato")
            .set(glide, 70)
            .set(o1Wave, "saw")
            .set(o1Retrigger, true)
            .set(f1Cutoff, 320)
            .set(f1Resonance, 80)
            .set(f1Env, 3)
            .set(f1Velocity, 1)
            .set(envDecay, 220)
            .set(envSustain, 0)
            .set(f2Type, "ladder")
            .set(f2Cutoff, 20000)
            .set(f2Resonance, 0)
            .set(f2Slope, "db12")
            .set(f2Drive, 9)
            .set(ampLevel, 1.2)
            .set(ampDecay, 300)
            .set(ampSustain, 70)
            .set(ampRelease, 60)
            .set(o1Level, 0);
        p.add("Supersaw")
            .set(unison, 7)
            .set(detune, 28)
            .set(spread, 90)
            .set(o1Level, -4.5)
            .set(f1Slope, "db12")
            .set(f1Cutoff, 6000)
            .set(f1Resonance, 5)
            .set(f1Env, 1)
            .set(envDecay, 800)
            .set(envSustain, 40)
            .set(ampAttack, 5)
            .set(ampDecay, 500)
            .set(ampSustain, 85)
            .set(ampRelease, 400)
            .route("modwheel>filter1.cutoff", 2);
        p.add("Sub Bass")
            .set(o1Wave, "sine")
            .set(o1Level, -10)
            .set(o1Retrigger, true)
            .set(f1Type, "off")
            .set(ampAttack, 3)
            .set(ampDecay, 300)
            .set(ampSustain, 100)
            .set(ampRelease, 120)
            .set(ampVelocity, 30);
        p.add("Tine Piano")
            .set(o1Type, "fm")
            .set(o1Level, -1)
            .set(o1Fm, 2.5)
            .set(o1FmFrom, "osc2")
            .set(o1Retrigger, true)
            .set(o2Type, "fm")
            .set(o2Ratio, 1)
            .set(o2Level, -60)
            .set(o2Retrigger, true)
            .set(o3Type, "fm")
            .set(o3Ratio, 14)
            .set(o3Level, -34)
            .set(o3Retrigger, true)
            .set(f1Type, "off")
            .set(envFm, 100)
            .set(envAttack, 0)
            .set(envDecay, 1600)
            .set(envSustain, 15)
            .set(envRelease, 400)
            .set(ampAttack, 1)
            .set(ampDecay, 4500)
            .set(ampSustain, 0)
            .set(ampRelease, 450)
            .set(ampVelocity, 70)
            .set(l1Rate, 3.5)
            .set(l1Mode, "free")
            .route("velocity>osc1.fm", 1.5)
            .route("lfo1>amp.pan", 35, "modwheel");
        p.add("Glass Bell")
            .set(o1Type, "fm")
            .set(o1Level, -7)
            .set(o1Fm, 3.5)
            .set(o1FmFrom, "osc2")
            .set(o1Retrigger, true)
            .set(o2Type, "fm")
            .set(o2Ratio, 3.5)
            .set(o2Level, -60)
            .set(o2Retrigger, true)
            .set(f1Type, "off")
            .set(envFm, 70)
            .set(envAttack, 0)
            .set(envDecay, 2500)
            .set(envSustain, 0)
            .set(envRelease, 2500)
            .set(ampAttack, 1)
            .set(ampDecay, 6000)
            .set(ampSustain, 0)
            .set(ampRelease, 2500);
        p.add("Vowel Lead")
            .set(mode, "legato")
            .set(glide, 40)
            .set(unison, 2)
            .set(detune, 8)
            .set(spread, 30)
            .set(f1Type, "formant")
            .set(f1Vowel, 1.5)
            .set(f1Keytrack, 30)
            .set(ampAttack, 30)
            .set(ampDecay, 400)
            .set(ampSustain, 85)
            .set(ampRelease, 250)
            .set(o1Level, -2)
            .set(l1Rate, 5.5)
            .set(l1Delay, 350)
            .set(l2Rate, 0.3)
            .set(l2Mode, "free")
            .set(l2Phase, 90)
            .route("lfo1>voice.tune", 25)
            .route("modwheel>filter1.vowel", 2)
            .route("lfo2>filter1.vowel", 0.5);
        p.add("Strings")
            .set(unison, 5)
            .set(detune, 18)
            .set(spread, 80)
            .set(drift, 35)
            .set(o2Type, "analog")
            .set(o2Wave, "pulse")
            .set(o2Width, 30)
            .set(o2Level, -13)
            .set(f1Type, "svf")
            .set(f1Cutoff, 3500)
            .set(f1Resonance, 10)
            .set(f1Env, 0)
            .set(f1Keytrack, 40)
            .set(ampAttack, 350)
            .set(ampDecay, 1)
            .set(ampSustain, 100)
            .set(ampRelease, 900)
            .set(o1Level, -9)
            .set(l1Rate, 5.2)
            .set(l1Delay, 600)
            .route("lfo1>voice.tune", 10)
            .route("modwheel>filter1.cutoff", 1);
        p.add("Noise Snare")
            .set(o1Type, "noise")
            .set(o1Level, 0)
            .set(o2Type, "analog")
            .set(o2Wave, "triangle")
            .set(o2Coarse, -5)
            .set(o2Level, -3)
            .set(o2Retrigger, true)
            .set(o2To, "direct")
            .set(f1Type, "svf")
            .set(f1Mode, "lowpass")
            .set(f1Cutoff, 6000)
            .set(f1Env, 0)
            .set(envPitch, 12)
            .set(envAttack, 0)
            .set(envDecay, 45)
            .set(envSustain, 0)
            .set(ampAttack, 0)
            .set(ampDecay, 190)
            .set(ampSustain, 0)
            .set(ampRelease, 120)
            .set(ampVelocity, 60)
            .set(f2Type, "svf")
            .set(f2Mode, "highpass")
            .set(f2Cutoff, 350)
            .set(f2Env, 0);
        p.add("Drawbar Organ")
            .set(o1Type, "wavetable")
            .set(o1Table, "organ")
            .set(o1Position, 70)
            .set(o1Level, -12)
            .set(f1Cutoff, 20000)
            .set(f1Resonance, 0)
            .set(f1Slope, "db12")
            .set(f1Env, 0)
            .set(f1Drive, 4)
            .set(drift, 15)
            .set(ampAttack, 4)
            .set(ampDecay, 1)
            .set(ampSustain, 100)
            .set(ampRelease, 60)
            .set(ampVelocity, 0)
            .set(l1Rate, 6.5)
            .set(l1Mode, "free")
            .route("lfo1>amp.pan", 25)
            .route("lfo1>voice.tune", 4);
        p.add("Wobble Bass")
            .set(mode, "legato")
            .set(glide, 30)
            .set(o1Retrigger, true)
            .set(o2Type, "sub")
            .set(o2Wave, "sine")
            .set(o2Level, -1)
            .set(o1Level, -3)
            .set(f1Cutoff, 180)
            .set(f1Resonance, 45)
            .set(f1Env, 0)
            .set(f2Type, "ladder")
            .set(f2Cutoff, 20000)
            .set(f2Resonance, 0)
            .set(f2Slope, "db12")
            .set(f2Drive, 8)
            .set(ampLevel, 1)
            .set(ampDecay, 1)
            .set(ampSustain, 100)
            .set(ampRelease, 80)
            .set(l1Sync, true)
            .set(l1Division, "n1_8")
            .set(l1Mode, "free")
            .set(l1Shape, "triangle")
            .set(l1Phase, 270)
            .route("lfo1>filter1.cutoff", 2.5)
            .route("modwheel>lfo1.rate", 4);
        p.add("Sync Lead")
            .set(o1Level, -60)
            .set(o2Type, "analog")
            .set(o2Sync, true)
            .set(o2Coarse, 7)
            .set(o2Level, -4)
            .set(f1Cutoff, 5000)
            .set(f1Resonance, 15)
            .set(f1Env, 0)
            .set(env3Attack, 0)
            .set(env3Decay, 700)
            .set(env3Sustain, 25)
            .set(env3Release, 300)
            .set(l1Rate, 5.5)
            .set(l1Delay, 250)
            .set(ampAttack, 3)
            .set(ampDecay, 400)
            .set(ampSustain, 85)
            .set(ampRelease, 220)
            .route("env3>osc2.fine", 2400)
            .route("lfo1>voice.tune", 30, "modwheel");
        p.add("Pulse Pad")
            .set(o1Wave, "pulse")
            .set(o1Width, 50)
            .set(o1Level, -9)
            .set(o2Type, "analog")
            .set(o2Wave, "pulse")
            .set(o2Width, 50)
            .set(o2Fine, 6)
            .set(o2Level, -9)
            .set(f1Cutoff, 2400)
            .set(f1Resonance, 15)
            .set(f1Env, 0.5)
            .set(envAttack, 1200)
            .set(envDecay, 2000)
            .set(envSustain, 50)
            .set(ampAttack, 450)
            .set(ampDecay, 1000)
            .set(ampSustain, 85)
            .set(ampRelease, 1400)
            .set(l1Rate, 0.4)
            .set(l2Rate, 0.53)
            .set(l2Phase, 90)
            .route("lfo1>osc1.width", 38)
            .route("lfo2>osc2.width", 38)
            .route("velocity>filter1.cutoff", 1);
        p.add("Table Sweep")
            .set(o1Type, "wavetable")
            .set(o1Table, "spectral_comb")
            .set(o1Level, -3)
            .set(unison, 3)
            .set(detune, 10)
            .set(spread, 70)
            .set(f1Cutoff, 6000)
            .set(f1Env, 0)
            .set(env3Attack, 2500)
            .set(env3Decay, 3000)
            .set(env3Sustain, 40)
            .set(env3Release, 1500)
            .set(l1Rate, 0.25)
            .set(l1Shape, "drift")
            .set(ampAttack, 200)
            .set(ampDecay, 1000)
            .set(ampSustain, 85)
            .set(ampRelease, 1500)
            .route("env3>osc1.position", 80)
            .route("lfo1>osc1.position", 15)
            .route("macro1>filter1.cutoff", -3);
    }
};
