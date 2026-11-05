// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Drumlin: Steppe's drum machine.
//
// A drumlin is a long, low hill shaped by a glacier, smooth and streamlined,
// that lies in swarms across the land it left. Drumlin plays the sounds of
// classic drum machines without their samples: each sound is a recipe of
// resonant modes and filtered noise, fitted to recordings of the machine,
// and played the way the machine played its own.
//
// Two kits so far. The classic sounds are an early drum machine's. Each of
// its voices read an 8-bit recording from an EPROM through a companding DAC
// clocked by the voice's tuning knob, so tuning sped the whole recording up
// or slowed it down, and the DAC's steps went out unsmoothed, their images
// above the clock and all. The kick, toms and congas then went through a
// low-pass filter that closed after each hit; the hi-hat's EPROM played
// round and round, and a VCA opened on it for each hit, wherever the loop
// happened to be. The studio sounds are recordings of real drums at about
// 44 kHz, played through a linear 16-bit DAC whose clock the tuning sets,
// with nothing after it.
//
// So a Drumlin voice makes its sound's recording a sample at a time, at the
// DAC's clock (the recipe's clock at Tune 0, raised or lowered by Tune):
// the modes and the bands, each sample rounded to the DAC's 8-bit companded
// steps and held until the next, with polyBLEP steps at twice the session's
// rate. The analog parts after the DAC run in real time: the filter, the
// coupling to the output and the VCA. A recording stops where its EPROM
// ended; it starts with what was on the tape before the hit, the lead-in
// that makes the classic kit sit a little behind the beat. Each voice's
// output is brought down to the session's rate once.

constexpr int kModes = 16;   // a recipe's resonant modes
constexpr int kBands = 16;   // its bands of noise
constexpr int kLoop = 4096;  // a looping EPROM's samples (the classic hi-hat)
constexpr int kWarm = 4096;  // a loop's recording made before what is kept
constexpr int kSeam = 512;   // ... and its end crossfaded into its start over
constexpr int kModels = 32;  // sounds with a recipe
constexpr int kLoops = 2;    // looping recordings there is room for
constexpr int kVoices = 32;
constexpr int kPads = 16;
constexpr int kBuses = 9;          // the main output and Out 1 to Out 8
constexpr double kChokeMs = 10.0;  // a choked voice's fade
constexpr double kEndMs = 5.0;     // a voice's fade once its EPROM has run out
constexpr int kFactor = 2;         // the voices' oversampling
constexpr int kTick = 16;          // samples between the analog parts' updates
constexpr int kBendTick = 8;       // EPROM samples between the modes' bend updates

// --- small pieces ------------------------------------------------------------------------

// A value from -1 to 1 for each whole number n and seed, the same everywhere.
double scatter(int n, int seed) {
    int h = n * 374761393 + seed * 668265263;
    h = (h ^ (h >> 13)) * 1274126177;
    h = h ^ (h >> 16);
    return (h & 0xFFFFFF) / 8388608.0 - 1.0;
}

double fastTanh(double x) {
    if (x > 4.97) return 1.0;
    if (x < -4.97) return -1.0;
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

// e^(-1/n): what something falling with time constant n (in samples) keeps
// each sample; 0 for no time at all.
double fallPer(double n) { return n > 0.0 ? exp(-1.0 / n) : 0.0; }

// The classic kit's DAC, an AM6070: mu-255 companding, a sign, a chord of eight
// and a step of sixteen. x is in full scale (-1 .. 1); it comes back as the
// level the DAC makes for the code x rounds to.
double companded(double x) {
    const double b = min(abs(x), 1.0) * 8158.0 + 33.0;
    const int chord = clamp(floorToInt(log2(b)) - 5, 0, 7);
    const double unit = exp2(chord);
    const int step = clamp(floorToInt(b / (2.0 * unit)) - 16, 0, 15);
    const double m = ((2.0 * step + 33.0) * unit - 33.0) / 8158.0;
    return x < 0.0 ? -m : m;
}

// A sample through a recipe's DAC: the AM6070 above when `steps` is 0, or
// else a linear one with `steps` levels each side of zero (2^(bits - 1)).
double converted(double x, double steps) {
    if (steps <= 0.0) return companded(x);
    const double v = clamp(x, -1.0, 1.0) * steps;
    return clamp(roundToInt(v), -toInt(steps), toInt(steps) - 1) / steps;
}

// A DAC's levels each side of zero for `bits` (0: the AM6070's mu-law).
double levelsOf(int bits) { return bits > 0 ? exp2(bits - 1) : 0.0; }

// --- the recording: what a sound's EPROM holds -------------------------------------------
//
// Frequencies and times are as they were at Tune 0, in Hz and ms; a voice
// turns them into EPROM samples, which Tune does not change: it only plays
// the samples faster or slower.

// A resonant mode: a damped sine in two parts, each falling at its own rate
// (a drum's first ring and what it settles into).
struct Mode {
    double hz = 0.0;
    double gain = 0.0;   // at the hit
    double fast = 1.0;   // ms to 1/e of its first part
    double slow = 1.0;   // ... and of its second
    double split = 0.0;  // its gain's share in the second
    double phase = 0.0;  // radians at the hit
};

// A band of noise: white noise through a filter, rising and then falling.
struct Band {
    int shape = 0;  // 0 band pass, 1 low pass, 2 high pass
    double hz = 1000.0;
    double q = 0.7;
    double gain = 0.0;
    double delay = 0.0;   // ms after the hit; before it when negative (the lead-in)
    double attack = 0.0;  // ms: its rise
    double fast = 1.0;
    double slow = 1.0;
    double split = 0.0;
};

struct Recipe {
    double clock = 20000.0;  // Hz: the DAC's clock at Tune 0
    double length = 200.0;   // ms of EPROM: the recording stops there
    double start = 0.0;      // ms into it where the hit is
    double drive = 1.0;      // the recording into the DAC (1: full scale)
    int bits = 0;         // the DAC: 0 the classic kit's 8-bit mu-law, else linear, this many bits
    double strike = 0.0;  // ms: the modes' rise
    double bend = 0.0;    // semitones the modes start above their pitch
    double bendMs = 1.0;  // ... falling back with this time constant
    int modes = 0;
    Array<Mode, kModes> mode;
    int bands = 0;
    Array<Band, kBands> band;
    int bursts = 0;            // a clap: band 0 struck this many times more
    double burstMs = 10.0;     // ... this far apart
    double burstGain = 1.0;    // ... each struck this much as hard as the one before
    double burstJitter = 0.0;  // ... their spacing this uneven (0 .. 1)
    int loop = -1;             // which loop it plays (the hi-hat), -1 none
    double gate = 0.0;         // ms after the hit where the recording was cut short, 0 never
    double gateMs = 1.0;       // ... falling away from there with this time constant
    // After the DAC, in real time whatever the Tune.
    double level = 0.0;    // dB
    double vcfOpen = 0.0;  // Hz: the low-pass filter at the hit, 0 for none
    double vcfShut = 0.0;  // Hz: where it closes to
    double vcfMs = 1.0;    // ... with this time constant
    double hpf = 10.0;     // Hz: the output's coupling
    double hold = 0.0;     // ms: the VCA held open before it decays
    double decay = 0.0;    // ms: its time constant as the machine set it; 0 open
    double accent = 0.0;   // dB: the VCA's accent, which hard hits reach
};

// --- a voice ------------------------------------------------------------------------------

struct Voice {
    int model = -1;
    bool sounding = false;
    // The DAC.
    double step = 0.0;   // EPROM samples per (oversampled) sample
    double phase = 0.0;  // through the EPROM sample held
    int at = 0;          // the EPROM sample held
    int end = 0;         // where the EPROM ends
    int from = 0;        // a loop's sample at the hit
    double drive = 1.0;
    double steps = 0.0;  // the DAC's levels each side of zero (0: mu-law)
    double held = 0.0;   // the DAC's output
    double late = 0.0;   // the sample before, still taking its polyBLEP
    bool ended = false;
    int rng = 1;   // the noise
    int seed = 0;  // the hit's: its noise and a clap's spacing
    // The modes: a turning phasor each, and its two falling parts.
    Array<double, kModes> re, im;  // the phasor
    Array<double, kModes> cw, sw;  // its turn per EPROM sample now
    Array<double, kModes> w;       // radians per EPROM sample at its pitch
    Array<double, kModes> ef, es;  // the fast and slow parts
    Array<double, kModes> df, ds;
    int hitAt = 0;
    double bendNow = 0.0;  // the bend, as a fraction of the pitch
    double bendFall = 0.0;
    double rise = 0.0;  // the strike: what is left to rise
    double riseFall = 0.0;
    int gateAt = 0;  // where the recording's cut begins (EPROM samples), 0 none
    double gateGain = 1.0;
    double gateFall = 1.0;
    // The bands: their filters and envelopes.
    Array<double, kBands> b0, b1, b2, a1, a2, z1, z2;
    Array<double, kBands> bf, bs, br, bdf, bds, bdr;
    Array<int, kBands> bandAt;
    double burstAt = 0.0;  // the next burst, EPROM samples
    double burstAmp = 0.0;
    int burstLeft = 0;
    int burstN = 0;
    // After the DAC.
    Array<double, 4> lp;  // the filter's four poles
    double lpG = 1.0;
    double cut = 0.0;  // its corner now, as the log of Hz
    double cutTo = 0.0;
    double cutFall = 0.0;
    bool filtered = false;
    double hp = 0.0;  // the coupling's state
    double hpA = 0.0;
    double vca = 1.0;
    double vcaFall = 1.0;
    int holdLeft = 0;
    double gain = 1.0;  // level and velocity
    double fade = 1.0;  // a choked voice's way out
    double fadeStep = 0.0;
    int tickLeft = 0;
    double last = 0.0;
};

// Uniform white noise, -1 .. 1, from the voice's own generator.
double white(Voice& s) {
    s.rng = s.rng * 1664525 + 1013904223;
    return ((s.rng >> 8) & 0xFFFFFF) / 8388608.0 - 1.0;
}

// Band b's filter, at its frequency in EPROM samples (the RBJ cookbook's).
void design(Voice& s, int b, const Band& d, double clock) {
    const double hz = clamp(d.hz, 10.0, clock * 0.49);
    const double w0 = twoPi * hz / clock;
    const double c = cos(w0);
    const double alpha = sin(w0) / (2.0 * max(d.q, 0.05));
    const double a0 = 1.0 + alpha;
    if (d.shape == 1) {
        s.b0[b] = (1.0 - c) * 0.5 / a0;
        s.b1[b] = (1.0 - c) / a0;
        s.b2[b] = s.b0[b];
    } else if (d.shape == 2) {
        s.b0[b] = (1.0 + c) * 0.5 / a0;
        s.b1[b] = -(1.0 + c) / a0;
        s.b2[b] = s.b0[b];
    } else {
        s.b0[b] = alpha / a0;
        s.b1[b] = 0.0;
        s.b2[b] = -alpha / a0;
    }
    s.a1[b] = -2.0 * c / a0;
    s.a2[b] = (1.0 - alpha) / a0;
}

// The modes' turns per EPROM sample with the bend as it is now.
void bendModes(Voice& s, const Recipe& r) {
    const double f = 1.0 + s.bendNow;
    for (int j = 0; j < r.modes; ++j) {
        s.cw[j] = cos(s.w[j] * f);
        s.sw[j] = sin(s.w[j] * f);
    }
}

// A hit on voice s of recipe r: everything back to its start. `seed` picks
// the noise, `vary` (0 .. 1) how far this hit strays from the recipe, and
// `pick` is the hit's own number for that; `stretch` scales how long it
// rings (its decays, its cut and where its EPROM ends; not its attack).
void strike(Voice& s, const Recipe& r, int model, int seed, double vary, int pick, double stretch) {
    s.model = model;
    s.sounding = true;
    s.ended = false;
    s.phase = 0.0;
    s.at = 0;
    s.held = 0.0;
    s.late = 0.0;
    s.seed = seed;
    s.rng = seed * 747796405 + 1013904223;
    const double perMs = r.clock * 0.001;  // EPROM samples per ms at Tune 0
    const double ring = perMs * stretch;   // EPROM samples per ms of decay
    s.fade = 1.0;
    s.fadeStep = 0.0;
    s.end = r.loop >= 0 ? kLoop : max(1, roundToInt(r.start * perMs + (r.length - r.start) * ring));
    s.from = r.loop >= 0 ? wrap(toInt((scatter(pick, 7919) + 1.0) * 0.5 * kLoop), kLoop) : 0;
    s.drive = r.drive;
    s.steps = levelsOf(r.bits);
    s.hitAt = max(0, roundToInt(r.start * perMs));
    // How this hit strays: its pitch and each mode's and band's gain.
    const double spread = vary * (scatter(pick * 5, seed) * 0.15);
    const double pitch = exp2(spread / 12.0);
    for (int j = 0; j < r.modes; ++j) {
        const Mode& m = r.mode[j];
        const double g = m.gain * dbToGain(vary * 1.5 * scatter(pick * 5 + 1, seed + j));
        s.w[j] = twoPi * m.hz * pitch / r.clock;
        s.re[j] = cos(m.phase);
        s.im[j] = sin(m.phase);
        s.ef[j] = g * (1.0 - m.split);
        s.es[j] = g * m.split;
        s.df[j] = fallPer(m.fast * ring);
        s.ds[j] = fallPer(m.slow * ring);
    }
    s.bendNow = r.bend > 0.0 ? exp2(r.bend / 12.0) - 1.0 : 0.0;
    s.bendFall = fallPer(r.bendMs * perMs / kBendTick);
    bendModes(s, r);
    s.rise = r.strike > 0.0 ? 1.0 : 0.0;
    s.riseFall = fallPer(r.strike * perMs);
    s.gateAt = r.gate > 0.0 ? s.hitAt + roundToInt(r.gate * ring) : 0;
    s.gateGain = 1.0;
    s.gateFall = fallPer(r.gateMs * ring);
    for (int b = 0; b < r.bands; ++b) {
        const Band& d = r.band[b];
        design(s, b, d, r.clock);
        s.z1[b] = 0.0;
        s.z2[b] = 0.0;
        const double g = d.gain * dbToGain(vary * 1.5 * scatter(pick * 5 + 2, seed + b));
        s.bf[b] = g * (1.0 - d.split);
        s.bs[b] = g * d.split;
        s.br[b] = d.attack > 0.0 ? 1.0 : 0.0;
        s.bdf[b] = fallPer(d.fast * ring);
        s.bds[b] = fallPer(d.slow * ring);
        s.bdr[b] = fallPer(d.attack * perMs);
        s.bandAt[b] = max(0, s.hitAt + roundToInt(d.delay * perMs));
    }
    s.burstLeft = r.bands > 0 ? r.bursts : 0;
    s.burstN = 0;
    s.burstAmp = 1.0;
    s.burstAt = s.bandAt[0] + r.burstMs * perMs;
}

// The recording's next EPROM sample, before the DAC.
double record(Voice& s, const Recipe& r) {
    const int k = s.at;
    double y = 0.0;
    if (k >= s.hitAt && r.modes > 0) {
        if ((k - s.hitAt) % kBendTick == 0 && s.bendNow != 0.0) {
            bendModes(s, r);
            s.bendNow *= s.bendFall;
            if (s.bendNow < 1e-6) s.bendNow = 0.0;
        }
        double m = 0.0;
        for (int j = 0; j < r.modes; ++j) {
            const double re = s.re[j] * s.cw[j] - s.im[j] * s.sw[j];
            const double im = s.re[j] * s.sw[j] + s.im[j] * s.cw[j];
            s.re[j] = re;
            s.im[j] = im;
            m += im * (s.ef[j] + s.es[j]);
            s.ef[j] *= s.df[j];
            s.es[j] *= s.ds[j];
        }
        y += m * (1.0 - s.rise);
        s.rise *= s.riseFall;
    }
    if (r.bands > 0) {
        if (s.burstLeft > 0 && k >= s.burstAt) {
            // The clap's next hand: band 0 struck again.
            const Band& d = r.band[0];
            s.burstAmp *= r.burstGain;
            s.bf[0] += d.gain * s.burstAmp * (1.0 - d.split);
            s.bs[0] += d.gain * s.burstAmp * d.split;
            s.burstLeft -= 1;
            s.burstN += 1;
            const double perMs = r.clock * 0.001;
            s.burstAt += r.burstMs * perMs * (1.0 + r.burstJitter * scatter(s.burstN, s.seed + 31));
        }
        const double x = white(s);
        for (int b = 0; b < r.bands; ++b) {
            if (k < s.bandAt[b]) continue;
            // Transposed direct form II.
            const double f = s.b0[b] * x + s.z1[b];
            s.z1[b] = s.b1[b] * x - s.a1[b] * f + s.z2[b];
            s.z2[b] = s.b2[b] * x - s.a2[b] * f;
            y += f * (s.bf[b] + s.bs[b]) * (1.0 - s.br[b]);
            s.bf[b] *= s.bdf[b];
            s.bs[b] *= s.bds[b];
            s.br[b] *= s.bdr[b];
        }
    }
    if (s.gateAt > 0 && k >= s.gateAt) {
        y *= s.gateGain;
        s.gateGain *= s.gateFall;
    }
    return y;
}

// The DAC's next value: the recording's sample through its 8 bits, or a
// loop's from its table; silence once the EPROM has run out.
double nextValue(Voice& s, const Recipe& r, const Buffer<double>& loops) {
    if (r.loop >= 0) {
        const double v = loops[r.loop * kLoop + wrap(s.from + s.at, kLoop)];
        s.at += 1;
        return v;
    }
    if (s.at >= s.end) {
        s.ended = true;
        return 0.0;
    }
    const double x = record(s, r);
    s.at += 1;
    return converted(x * s.drive, s.steps);
}

// A looping recipe's recording, before the DAC, into its place in loops:
// kept from after kWarm samples, once its narrow bands have built up, with
// its end crossfaded into its start so that it goes round without a seam.
void loopRecording(const Recipe& r, Buffer<double>& loops) {
    Voice s;
    Recipe flat = r;
    flat.loop = -1;
    flat.start = 0.0;
    strike(s, flat, 0, 1, 0.0, 0, 1.0);
    for (int k = 0; k < kWarm; ++k) {
        record(s, flat);
        s.at += 1;
    }
    const int at = r.loop * kLoop;
    for (int k = 0; k < kLoop + kSeam; ++k) {
        const double x = record(s, flat);
        s.at += 1;
        if (k < kLoop) {
            loops[at + k] = x;
        } else {
            // Equal power, the two being unrelated noise.
            const int j = k - kLoop;
            const double t = (j + 0.5) / kSeam * pi * 0.5;
            loops[at + j] = loops[at + j] * sin(t) + x * cos(t);
        }
    }
}

// A looping recipe's EPROM, made once: its recording through the DAC.
void makeLoop(const Recipe& r, Buffer<double>& loops) {
    loopRecording(r, loops);
    const double steps = levelsOf(r.bits);
    for (int k = r.loop * kLoop; k < (r.loop + 1) * kLoop; ++k)
        loops[k] = converted(loops[k] * r.drive, steps);
}

// The analog parts' settings for the next kTick samples: the filter's corner
// as it closes.
void settle(Voice& s, double rate) {
    if (!s.filtered) return;
    const double hz = exp(s.cut);
    s.lpG = tan(pi * min(hz, rate * 0.45) / rate);
    s.lpG = s.lpG / (1.0 + s.lpG);
    s.cut = s.cutTo + (s.cut - s.cutTo) * s.cutFall;
}

// After a strike: the analog parts at the hit, for a voice played at `speed`
// (Tune as a ratio) at `rate` (the oversampled rate), with the VCA's decay
// `decayMs` (0: open) and gain `gain`.
void open(Voice& s, const Recipe& r, double speed, double rate, double decayMs, double gain) {
    s.step = r.clock * speed / rate;
    s.filtered = r.vcfOpen > 0.0;
    for (int j = 0; j < 4; ++j) s.lp[j] = 0.0;
    s.cut = log(max(r.vcfOpen, 20.0));
    s.cutTo = log(max(r.vcfShut, 20.0));
    s.cutFall = fallPer(r.vcfMs * 0.001 * rate / kTick);
    s.hp = 0.0;
    s.hpA = exp(-twoPi * max(r.hpf, 1.0) / rate);
    s.vca = 1.0;
    s.vcaFall = decayMs > 0.0 ? fallPer(decayMs * 0.001 * rate) : 1.0;
    s.holdLeft = decayMs > 0.0 ? roundToInt(r.hold * 0.001 * rate) : 0;
    s.gain = gain;
    s.tickLeft = 0;
}

// Voice s fades out over `ms`: cut off by another of its choke group, or
// its EPROM run out. A fade already under way goes on as it was.
void choke(Voice& s, double rate, double ms) {
    if (s.fadeStep == 0.0) s.fadeStep = 1.0 / max(1.0, ms * 0.001 * rate);
}

// Voice s on by n oversampled samples into out[from ..]: the DAC's steps
// (polyBLEP, a sample late), the filter, the coupling and the VCA. It stops
// once its recording has ended and faded out, or its VCA has closed.
void play(Voice& s, const Recipe& r, const Buffer<double>& loops, double rate, Buffer<double>& out,
          int from, int n) {
    for (int i = from; i < from + n; ++i) {
        if (s.tickLeft == 0) {
            settle(s, rate);
            s.tickLeft = kTick;
        }
        s.tickLeft -= 1;
        double corr = 0.0;
        s.phase += s.step;
        while (s.phase >= 1.0) {
            s.phase -= 1.0;
            const double d = s.phase / s.step;  // samples since the step
            const double v = nextValue(s, r, loops);
            const double jump = v - s.held;
            s.held = v;
            s.late += jump * d * d * 0.5;
            corr -= jump * (1.0 - d) * (1.0 - d) * 0.5;
        }
        double x = s.late;
        s.late = s.held + corr;
        if (s.filtered)
            for (int j = 0; j < 4; ++j) {
                const double v = (x - s.lp[j]) * s.lpG;
                x = v + s.lp[j];
                s.lp[j] = x + v;
            }
        // The coupling: a one-pole high pass.
        const double y = x - s.hp;
        s.hp = x - (x - s.hp) * s.hpA;
        double a = y * s.gain * s.vca * s.fade;
        if (s.holdLeft > 0)
            s.holdLeft -= 1;
        else
            s.vca *= s.vcaFall;
        // Past the EPROM's end (its step out is the machine's click) only
        // the coupling's DC is left, which would take seconds to go: it
        // fades out instead.
        if (s.ended) choke(s, rate, kEndMs);
        if (s.fadeStep > 0.0) s.fade = max(0.0, s.fade - s.fadeStep);
        s.last = a;
        out[i] += a;
        // Done, decided sample by sample so that it ends at the same sample
        // in any block: its VCA closed, or its fade (a choke's, or its
        // EPROM's end) out.
        if (s.vca < 1e-5 || (s.fadeStep > 0.0 && s.fade <= 0.0)) {
            s.sounding = false;
            return;
        }
    }
}

// --- end of the engine ---------------------------------------------------------------------

// --- the recipes ------------------------------------------------------------------------
//
// Each sound's recipe, as plugins/drumlin/fit found it: the recording
// (its modes, its bands and a clap's bursts), then the machine around it.

void recording(Recipe& r, double clock, double length, double start, double drive, double strike,
               double bend, double bendMs) {
    r.clock = clock;
    r.length = length;
    r.start = start;
    r.drive = drive;
    r.strike = strike;
    r.bend = bend;
    r.bendMs = bendMs;
}

void machine(Recipe& r, double level, double vcfOpen, double vcfShut, double vcfMs, double hpf,
             double hold, double decay, double accent) {
    r.level = level;
    r.vcfOpen = vcfOpen;
    r.vcfShut = vcfShut;
    r.vcfMs = vcfMs;
    r.hpf = hpf;
    r.hold = hold;
    r.decay = decay;
    r.accent = accent;
}

void cut(Recipe& r, double gate, double gateMs) {
    r.gate = gate;
    r.gateMs = gateMs;
}

void claps(Recipe& r, int bursts, double ms, double gain, double jitter) {
    r.bursts = bursts;
    r.burstMs = ms;
    r.burstGain = gain;
    r.burstJitter = jitter;
}

void addMode(Recipe& r, double hz, double gain, double fast, double slow, double split,
             double phase) {
    if (r.modes >= kModes) return;
    Mode& o = r.mode[r.modes];
    o.hz = hz;
    o.gain = gain;
    o.fast = fast;
    o.slow = slow;
    o.split = split;
    o.phase = phase;
    r.modes += 1;
}

void addBand(Recipe& r, int shape, double hz, double q, double gain, double delay, double attack,
             double fast, double slow, double split) {
    if (r.bands >= kBands) return;
    Band& o = r.band[r.bands];
    o.shape = shape;
    o.hz = hz;
    o.q = q;
    o.gain = gain;
    o.delay = delay;
    o.attack = attack;
    o.fast = fast;
    o.slow = slow;
    o.split = split;
    r.bands += 1;
}

// --- generated by plugins/drumlin/fit: do not edit by hand -------------------------------
void recipes(Array<Recipe, kModels>& book) {
    Recipe none;
    for (int m = 0; m < kModels; ++m) book[m] = none;
    // The classic kick: Tune 0 is the bank's pitch 16.
    recording(book[0], 22583.7, 181.4, 4.55769, 5.21748, 1.96748, 14.2827, 11.3546);
    machine(book[0], -2.13701, 5330.89, 3135.84, 296.225, 1.02577, 0.0, 0.0, 0.0);
    cut(book[0], 56.6474, 21.6677);
    addMode(book[0], 51.39, 0.368424, 55.7536, 22.3318, 0.363158, -3.14073);
    addMode(book[0], 97.6846, 0.0350227, 3.60567, 189.322, 0.698442, -0.0526417);
    addMode(book[0], 264.855, 0.00206255, 13.5535, 5.63652, 0.513838, -1.74221);
    addBand(book[0], 0, 582.143, 0.298966, 0.348483, 0.0, 0.140223, 3.06598, 7.22246, 0.318754);
    addBand(book[0], 1, 181.017, 0.549967, 0.572453, 0.0, 0.0248783, 16.6303, 35.811, 0.176241);
    addBand(book[0], 1, 164.933, 0.713952, 0.322539, -19.4066, 0.287737, 5.50039, 2.30362,
            0.803626);
    // The classic snare: pitch 15.
    recording(book[1], 23385.7, 175.149, 10.8488, 4.0715, 0.0202537, 3.27204, 10.7066);
    machine(book[1], -0.938863, 0.0, 0.0, 1.0, 1.11199, 0.0, 0.0, 0.0);
    cut(book[1], 53.4612, 35.1279);
    addMode(book[1], 132.476, 0.430559, 18.0956, 88.1802, 0.0774716, 2.72185);
    addMode(book[1], 235.514, 0.499581, 3.30568, 18.6435, 0.318263, -0.567044);
    addMode(book[1], 182.088, 0.0112889, 10.315, 18.7, 0.349418, -0.239024);
    addMode(book[1], 283.978, 0.0770927, 12.3917, 35.8292, 0.478788, 1.26222);
    addMode(book[1], 501.852, 0.0945552, 7.72282, 21.9271, 0.349078, 1.60231);
    addMode(book[1], 216.07, 0.0107475, 41.2382, 137.945, 0.915647, -2.34266);
    addBand(book[1], 0, 2703.26, 0.305083, 0.374319, 1.34308, 0.0908036, 1.24543, 90.1039,
            0.130297);
    addBand(book[1], 0, 304.008, 1.49481, 0.329497, -1.07398, 0.78272, 2.26952, 47.1201, 0.786312);
    addBand(book[1], 2, 3284.02, 0.207971, 0.041686, 19.9255, 0.361257, 1.46304, 23.0121, 0.521462);
    addBand(book[1], 1, 218.304, 3.18462, 0.0404812, -19.2641, 0.0519539, 16.2772, 23.2808,
            0.719744);
    // The classic rimshot: pitch 12.
    recording(book[2], 16365.5, 125.141, 1.00519, 7.62398, 0.181005, 0.0, 1.0);
    machine(book[2], -10.253, 0.0, 0.0, 1.0, 2.93652, 0.0, 0.0, 0.0);
    cut(book[2], 31.7719, 46.2633);
    addMode(book[2], 419.323, 0.0687213, 21.9198, 47.9197, 0.182873, 2.48103);
    addMode(book[2], 543.589, 0.146031, 12.8806, 12.8154, 0.113951, -0.0655317);
    addMode(book[2], 1282.2, 0.0747262, 32.9408, 9.68324, 0.170664, 1.0776);
    addMode(book[2], 1260.93, 0.20228, 5.33556, 5.74761, 0.178758, 1.39053);
    addMode(book[2], 453.353, 0.352139, 6.4778, 6.57266, 0.363473, 1.46028);
    addMode(book[2], 729.025, 0.218122, 9.37355, 16.7644, 0.638641, -2.8084);
    addMode(book[2], 2379.09, 0.20418, 4.84479, 4.21901, 0.200834, 2.19067);
    addMode(book[2], 2515.35, 0.0458286, 15.5211, 4.60961, 0.147144, -0.583667);
    addMode(book[2], 672.159, 0.0255605, 40.7983, 39.2837, 0.0947164, 1.9667);
    addMode(book[2], 1855.78, 0.3147, 4.65637, 22.9489, 0.180998, 2.4919);
    addMode(book[2], 149.963, 0.051992, 20.8747, 15.7698, 0.885876, -2.79435);
    addMode(book[2], 1235.7, 0.0201563, 37.3766, 219.507, 0.295069, -2.02683);
    addMode(book[2], 828.278, 0.105022, 12.8714, 16.3679, 0.315177, -0.10813);
    addMode(book[2], 1464.69, 0.224005, 4.03155, 7.38409, 0.0278477, 1.20377);
    addMode(book[2], 1831.73, 0.0118498, 10.7221, 47.4103, 0.0675794, 2.43856);
    addMode(book[2], 252.291, 0.0300188, 21.2232, 33.9196, 0.530641, -0.745302);
    addBand(book[2], 2, 4467.88, 0.610411, 0.0555083, -4.3335, 1.33128, 1.98959, 1.19279, 0.204813);
    addBand(book[2], 0, 704.591, 0.200135, 0.108558, 18.534, 0.0181044, 6.6754, 148.79, 0.691226);
    // The classic clap: pitch 13.
    recording(book[3], 22029.0, 185.936, 1.62687, 15.8018, 0.0, 0.0, 1.0);
    machine(book[3], -4.21888, 0.0, 0.0, 1.0, 10.3277, 0.0, 0.0, 0.0);
    claps(book[3], 4, 4.48425, 0.581706, 0.0359839);
    cut(book[3], 51.7358, 142.313);
    addBand(book[3], 0, 810.799, 2.45154, 0.255137, -7.26551, 0.0933004, 0.674482, 35.6561,
            0.291967);
    addBand(book[3], 0, 1371.33, 3.63085, 0.214329, -2.99806, 3.03744, 46.0266, 43.8635, 0.545605);
    addBand(book[3], 0, 237.049, 0.203519, 0.018559, -10.0472, 21.4559, 4.58819, 112.156, 0.249829);
    addBand(book[3], 2, 5029.61, 0.968407, 0.00587639, 9.51244, 1.06489, 11.5782, 37.4514,
            0.774383);
    // The classic closed hat: the loop at pitch 15 (noise: broad bands, and narrow ones on the real
    // hat's peaks), under the VCA's decay B.
    recording(book[4], 20838.7, 10000.0, 0.0, 1.26096, 0.0, 0.0, 1.0);
    machine(book[4], -15.3321, 0.0, 0.0, 1.0, 7.36279, 6.02762, 28.0, 7.29387);
    book[4].loop = 0;
    addBand(book[4], 0, 497.217, 1.59428, 0.36843, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 2413.48, 1.23365, 0.108327, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 7811.6, 0.347322, 0.393128, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 10104.1, 0.395154, 0.436387, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 1011.21, 37.3299, 0.639754, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 1483.83, 72.3277, 1.88171, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 1815.46, 40.5058, 0.498471, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 2773.26, 147.63, 0.47129, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 3401.44, 140.283, 0.978806, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 3478.61, 242.28, 1.51179, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 3700.37, 475.688, 0.015316, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 4245.92, 360.305, 1.86394, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 5002.68, 198.965, 0.71852, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 6186.18, 133.182, 1.27862, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 7341.86, 520.653, 4.03598, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    addBand(book[4], 0, 8472.84, 668.232, 0.201555, 0.0, 0.0, 100000.0, 100000.0, 0.0);
    // The classic open hat: the same loop, the VCA open and at the accent's level.
    book[5] = book[4];
    machine(book[5], -8.03823, 0.0, 0.0, 1.0, 7.36279, 6.02762, 267.0, 0.0);
    // The classic tom (both tom channels play it): pitch 12 of tom A.
    recording(book[6], 16169.5, 253.316, 24.8979, 9.04559, 0.0173878, 2.44962, 47.9741);
    machine(book[6], -6.82592, 19744.3, 4805.26, 67.4938, 54.9183, 0.0, 0.0, 0.0);
    cut(book[6], 66.8923, 275.142);
    addMode(book[6], 139.322, 0.26007, 6.01713, 15.8573, 0.321758, 1.19584);
    addMode(book[6], 120.621, 0.190758, 22.6372, 75.8344, 0.777451, -2.11285);
    addMode(book[6], 228.924, 0.0614268, 18.6942, 12.9127, 0.640317, 0.457869);
    addMode(book[6], 320.333, 0.0323566, 60.7581, 19.8757, 0.288873, -0.150591);
    addMode(book[6], 242.436, 0.0295572, 134.586, 35.3747, 0.816336, -2.54448);
    addMode(book[6], 192.457, 0.0120027, 110.733, 69.8377, 0.331965, 2.89672);
    addMode(book[6], 475.566, 0.019308, 31.8369, 49.6584, 0.973495, -2.52873);
    addBand(book[6], 0, 4720.58, 1.15875, 0.192445, 0.87354, 0.111247, 9.64455, 6.86303, 0.188432);
    addBand(book[6], 1, 125.678, 1.78386, 0.128412, -10.1337, 0.322928, 22.3007, 6.47, 0.486037);
    // The classic conga (both conga channels): pitch 14 of conga A.
    recording(book[7], 16225.8, 252.437, 2.34362, 1.74205, 0.0953833, 0.0251667, 44.3294);
    machine(book[7], -7.96999, 19212.1, 7477.52, 234.856, 1.18405, 0.0, 0.0, 0.0);
    cut(book[7], 120.21, 291.524);
    addMode(book[7], 229.069, 1.09858, 30.2782, 93.2349, 0.282049, 1.81434);
    addMode(book[7], 806.617, 0.678658, 1.94003, 22.9484, 0.270874, -0.99206);
    addMode(book[7], 285.447, 0.401214, 44.6805, 88.1889, 0.0757073, 1.17201);
    addMode(book[7], 423.284, 0.0143, 3.4409, 17.4937, 0.0369778, 0.326441);
    addMode(book[7], 338.267, 0.0432995, 12.9953, 24.0398, 0.754217, -0.862882);
    addMode(book[7], 494.358, 0.00593911, 79.973, 12.5089, 0.988803, 0.354231);
    addMode(book[7], 414.1, 0.0973825, 9.61321, 63.9391, 0.87422, -0.548637);
    addMode(book[7], 584.972, 0.0953758, 43.8809, 23.2603, 0.691877, -2.62848);
    addBand(book[7], 0, 971.778, 2.92688, 0.393684, 8.83545, 0.0964609, 0.705223, 19.4817,
            0.639599);
    // The classic cabasa: pitch 13.
    recording(book[8], 18948.9, 108.08, 8.87534, 4.56363, 0.0, 0.0, 1.0);
    machine(book[8], -12.4256, 0.0, 0.0, 1.0, 6.05688, 0.0, 0.0, 0.0);
    cut(book[8], 41.1919, 10.5545);
    addBand(book[8], 0, 8384.44, 2.28612, 0.672752, 11.9969, 7.77417, 108.514, 68.5332, 0.625873);
    addBand(book[8], 0, 7167.94, 1.6496, 0.948462, -2.36819, 34.7057, 57.1942, 213.497, 0.166634);
    addBand(book[8], 0, 3990.36, 2.46039, 0.107046, 7.64607, 13.7188, 28.9264, 365.77, 0.457814);
    addBand(book[8], 2, 3643.12, 0.934587, 0.0206251, -13.033, 5.68685, 52.2797, 16.5163, 0.918118);
    // The classic tambourine: pitch 15 (its clocks found from its partials' images): its jingles'
    // partials as narrow bands, broad bands for the rest.
    recording(book[9], 19872.5, 206.11, 1.43956, 61.1774, 0.0, 0.0, 1.0);
    machine(book[9], -10.0325, 0.0, 0.0, 1.0, 15.5916, 0.0, 0.0, 0.0);
    cut(book[9], 70.8125, 75.3348);
    addBand(book[9], 0, 7952.97, 0.385607, 0.0251098, 2.22578, 0.273403, 7.4128, 7.93512, 0.117747);
    addBand(book[9], 0, 10375.0, 0.228089, 0.0155852, 2.87269, 0.0899928, 15.8102, 149.819,
            0.896854);
    addBand(book[9], 0, 6954.47, 3.1845, 0.0121238, -11.0704, 0.83153, 21.491, 161.738, 0.72221);
    addBand(book[9], 0, 511.299, 0.70647, 0.0243673, 7.27562, 0.411058, 0.712818, 7.30961,
            0.0266346);
    addBand(book[9], 0, 6131.08, 95.7085, 0.00442122, 0.0, 1.61217, 75.8619, 37.8122, 0.638693);
    addBand(book[9], 0, 6219.63, 139.776, 0.129793, 0.0, 3050.17, 22.9396, 59.1311, 0.612285);
    addBand(book[9], 0, 6244.49, 431.073, 18.9022, 0.0, 2320.56, 13.5972, 79.526, 0.710671);
    addBand(book[9], 0, 6476.67, 109.102, 0.824353, 0.0, 3026.2, 11.9109, 17.6918, 0.101801);
    addBand(book[9], 0, 6633.56, 163.101, 0.128408, 0.0, 728.657, 6.4073, 96.6351, 0.651602);
    addBand(book[9], 0, 6701.56, 132.242, 15.9691, 0.0, 2620.89, 8.91294, 32.4679, 0.771574);
    addBand(book[9], 0, 6817.41, 162.531, 0.083796, 0.0, 0.782104, 69.3525, 33.2662, 0.539865);
    addBand(book[9], 0, 6881.38, 239.314, 0.124449, 0.0, 1.36099, 61.4073, 69.764, 0.194086);
    addBand(book[9], 0, 7044.58, 98.8554, 0.0702874, 0.0, 217.118, 18.4135, 33.8437, 0.698224);
    addBand(book[9], 0, 7064.22, 578.621, 0.162433, 0.0, 1.7774, 62.6859, 22.1703, 0.821308);
    addBand(book[9], 0, 9044.23, 293.985, 0.859907, 0.0, 34.5574, 54.7498, 77.7269, 0.1997);
    addBand(book[9], 0, 9253.22, 185.725, 2.10415, 0.0, 72.7147, 33.7103, 16.5587, 0.508275);
    // The classic cowbell: pitch 12.
    recording(book[10], 20024.7, 102.274, 1.70998, 7.69814, 0.100339, 0.0, 1.0);
    machine(book[10], -11.3724, 0.0, 0.0, 1.0, 57.8169, 0.0, 0.0, 0.0);
    cut(book[10], 124.032, 17.4375);
    addMode(book[10], 439.64, 0.301673, 3.425, 9.14621, 0.0800256, 0.709912);
    addMode(book[10], 391.833, 0.294269, 26.6459, 28.547, 0.369027, -2.48606);
    addMode(book[10], 415.547, 0.0416338, 14.1393, 51.5061, 0.450703, 0.39709);
    addMode(book[10], 1271.23, 0.123186, 11.1255, 17.5474, 0.0845035, -2.47553);
    addMode(book[10], 2934.49, 0.091225, 3.88428, 15.7279, 0.282491, -1.69739);
    addMode(book[10], 844.899, 0.0223009, 46.0901, 86.2974, 0.217103, -0.865924);
    addMode(book[10], 2471.39, 0.0506475, 13.5605, 40.5479, 0.172699, -1.52462);
    addMode(book[10], 1210.7, 0.00263059, 15.7877, 14.6511, 0.172323, -0.438393);
    addMode(book[10], 1835.11, 0.0254085, 17.8232, 45.8157, 0.386452, 2.20636);
    addBand(book[10], 2, 1054.88, 0.995168, 0.0337997, 3.45754, 0.0328191, 0.610866, 21.7254,
            0.460665);
    // The crash: one recording at its DAC's clock (found from its partials' images): the partials
    // the stick sets ringing as modes, those that bloom after it as narrow bands, broad bands for
    // the wash.
    recording(book[11], 14557.5, 2250.94, 0.0, 7.25221, 19.753, 0.0, 1.0);
    machine(book[11], -12.2761, 0.0, 0.0, 1.0, 5.2968, 0.0, 0.0, 0.0);
    addMode(book[11], 379.914, 0.0433714, 20.5533, 33.0143, 0.38998, 0.0);
    addMode(book[11], 509.549, 0.0578591, 74.1283, 35.3006, 0.346946, 0.0);
    addMode(book[11], 632.737, 0.00195304, 24.9974, 48.2423, 0.796894, 0.0);
    addMode(book[11], 746.367, 0.00664558, 65.2397, 54.3301, 0.783835, 0.0);
    addMode(book[11], 962.471, 0.00189792, 9.74992, 94.0722, 0.464553, 0.0);
    addMode(book[11], 1109.16, 0.00160681, 20.0442, 113.366, 0.641017, 0.0);
    addMode(book[11], 4389.18, 0.0219853, 369.676, 400.357, 0.515596, 0.0);
    addMode(book[11], 4863.94, 0.00872653, 45.0047, 793.561, 0.356654, 0.0);
    addBand(book[11], 0, 248.978, 0.308871, 0.124939, 0.0, 63.4008, 22.4106, 430.739, 0.0420488);
    addBand(book[11], 0, 643.758, 1.68086, 0.144971, 0.0, 14.2687, 42.6093, 663.054, 0.13492);
    addBand(book[11], 0, 2874.63, 0.393531, 0.0300411, 0.0, 9.07912, 151.289, 541.558, 0.727797);
    addBand(book[11], 0, 6709.98, 0.301583, 0.348349, 0.0, 214.273, 349.386, 338.292, 0.729077);
    addBand(book[11], 0, 1687.86, 0.357656, 0.000524113, 0.0, 0.0, 50.1287, 1.0, 0.0);
    addBand(book[11], 0, 2711.31, 490.429, 0.61324, 0.0, 37.4866, 1.0, 407.767, 1.0);
    addBand(book[11], 0, 3207.92, 233.291, 0.878011, 0.0, 2940.81, 1.0, 119.974, 1.0);
    addBand(book[11], 0, 3292.92, 263.744, 0.00577207, 0.0, 4096.02, 1.0, 350.306, 1.0);
    addBand(book[11], 0, 3389.37, 291.569, 10.1216, 0.0, 301.456, 1.0, 142.077, 1.0);
    addBand(book[11], 0, 3480.91, 238.271, 0.0137941, 0.0, 244.306, 1.0, 470.277, 1.0);
    addBand(book[11], 0, 3935.01, 244.616, 4.30235, 0.0, 586.125, 1.0, 198.319, 1.0);
    addBand(book[11], 0, 3996.53, 270.785, 0.217292, 0.0, 925.04, 1.0, 512.196, 1.0);
    addBand(book[11], 0, 4132.41, 316.036, 3.52484, 0.0, 64.8581, 1.0, 167.096, 1.0);
    addBand(book[11], 0, 4207.2, 834.272, 1.79704, 0.0, 265.978, 1.0, 377.551, 1.0);
    addBand(book[11], 0, 5301.69, 393.612, 0.357181, 0.0, 3419.15, 1.0, 405.243, 1.0);
    addBand(book[11], 0, 6317.64, 2301.28, 0.229243, 0.0, 5261.95, 1.0, 152.437, 1.0);
    // The ride: one recording at its DAC's clock (found from its partials' images): the partials
    // that ring from the hit as modes, those that bloom as narrow bands, broad bands for the wash.
    recording(book[12], 17402.3, 1882.97, 0.0, 8.98871, 0.0710268, 0.0, 1.0);
    machine(book[12], -16.8226, 0.0, 0.0, 1.0, 4.72767, 0.0, 0.0, 0.0);
    addMode(book[12], 480.799, 0.0194996, 189.11, 399.759, 0.77913, 0.0);
    addMode(book[12], 507.725, 0.0181649, 102.676, 233.617, 0.390812, 0.0);
    addMode(book[12], 564.569, 0.0160494, 72.4138, 425.055, 0.719146, 0.0);
    addMode(book[12], 621.065, 0.00500791, 96.6369, 170.53, 0.826792, 0.0);
    addMode(book[12], 770.188, 0.0116115, 11.675, 166.598, 0.385338, 0.0);
    addMode(book[12], 848.523, 0.013851, 227.095, 418.764, 0.786847, 0.0);
    addMode(book[12], 1829.99, 0.0136633, 19.4809, 106.106, 0.568579, 0.0);
    addMode(book[12], 2278.65, 0.00592537, 516.763, 513.345, 0.436057, 0.0);
    addMode(book[12], 3694.63, 0.0275096, 304.293, 316.55, 0.300569, 0.0);
    addMode(book[12], 3888.77, 0.0203321, 137.046, 349.22, 0.283178, 0.0);
    addMode(book[12], 4017.02, 0.017226, 397.493, 661.987, 0.0352077, 0.0);
    addMode(book[12], 4218.02, 0.00815547, 206.577, 411.007, 0.351956, 0.0);
    addMode(book[12], 5289.92, 0.0147685, 273.639, 224.898, 0.922515, 0.0);
    addMode(book[12], 5479.99, 0.0036765, 118.638, 565.471, 0.271693, 0.0);
    addMode(book[12], 5479.72, 0.0274483, 106.08, 305.041, 0.709603, 0.0);
    addMode(book[12], 5602.98, 0.0433111, 119.797, 84.0317, 0.648577, 0.0);
    addBand(book[12], 0, 882.791, 3.77151, 0.0348867, 0.0, 12.1409, 211.287, 637.928, 0.488323);
    addBand(book[12], 0, 1091.9, 2.83485, 0.0148841, 0.0, 19.5773, 150.413, 439.785, 0.66797);
    addBand(book[12], 0, 5553.6, 0.309348, 0.0590607, 0.0, 0.706388, 44.0079, 380.471, 0.62986);
    addBand(book[12], 0, 10561.3, 0.302024, 0.135811, 0.0, 2.36548, 171.322, 251.542, 0.861127);
    addBand(book[12], 0, 782.366, 0.772091, 0.373275, 0.0, 0.0, 4.38794, 1.0, 0.0);
    addBand(book[12], 0, 437.962, 53.7101, 0.926801, 0.0, 1018.65, 1.0, 283.53, 1.0);
    addBand(book[12], 0, 536.186, 93.2838, 0.0200415, 0.0, 292.974, 1.0, 140.044, 1.0);
    addBand(book[12], 0, 806.193, 119.331, 0.357762, 0.0, 55.2893, 1.0, 303.51, 1.0);
    addBand(book[12], 0, 1216.43, 261.596, 0.185541, 0.0, 88.2453, 1.0, 587.409, 1.0);
    addBand(book[12], 0, 1651.48, 501.852, 0.528042, 0.0, 28.8017, 1.0, 343.004, 1.0);
    addBand(book[12], 0, 1683.06, 539.757, 1.1478, 0.0, 34.3089, 1.0, 423.937, 1.0);
    addBand(book[12], 0, 2186.29, 391.369, 1.46985, 0.0, 578.473, 1.0, 341.917, 1.0);
    addBand(book[12], 0, 3610.27, 321.048, 0.014046, 0.0, 63.2376, 1.0, 290.118, 1.0);
    addBand(book[12], 0, 3629.58, 750.404, 0.0770406, 0.0, 256.641, 1.0, 636.098, 1.0);
    addBand(book[12], 0, 3818.99, 275.394, 0.582977, 0.0, 47.0599, 1.0, 246.95, 1.0);
    addBand(book[12], 0, 3984.23, 373.411, 0.063117, 0.0, 1.99561, 1.0, 596.331, 1.0);
    // The studio kit: recordings of real drums at a clock of about 44 kHz, through a linear
    // 16-bit DAC, nothing after it.
    // The studio kick: Tune 0 is the bank's pitch 12 (as for every studio sound), its pitch
    // falling into the body.
    recording(book[13], 44100.0, 341.45, 1.497, 8.5352, 0.0425022, 21.3241, 7.75001);
    machine(book[13], -3.3319, 0.0, 0.0, 1.0, 22.78, 0.0, 0.0, 0.0);
    cut(book[13], 104.304, 294.195);
    book[13].bits = 16;
    addMode(book[13], 41.3046, 0.157885, 14.2726, 47.9284, 0.616432, -2.99822);
    addMode(book[13], 686.384, 0.000880821, 1.63016, 2.39822, 0.0690575, -1.73476);
    addMode(book[13], 1487.8, 0.00722328, 12.6486, 9.22366, 0.348148, 2.40213);
    addMode(book[13], 1807.72, 0.0299379, 2.78115, 6.75562, 0.331681, 1.58925);
    addMode(book[13], 2029.04, 0.00349112, 3.95689, 25.3379, 0.255245, 0.933542);
    addBand(book[13], 0, 112.08, 11.9313, 0.999553, 9.4261, 32.5352, 26.7604, 1875.53, 0.0303796);
    addBand(book[13], 0, 349.777, 0.440968, 0.252155, -1.00629, 0.221042, 3.83934, 1868.28,
            0.000404957);
    addBand(book[13], 0, 1445.76, 0.812301, 0.000334498, 0.118924, 0.0131498, 374.745, 28.8672,
            0.320876);
    addBand(book[13], 0, 7104.61, 6.4385, 0.000471864, 5.80063, 0.0101293, 4.4552, 13.8248,
            0.0403437);
    // The studio snare: modes for its shell below 1.5 kHz, its wires bands of noise.
    recording(book[14], 44100.0, 331.5, 0.272, 2.20175, 0.0135083, 2.80967, 5.81315);
    machine(book[14], -3.14699, 0.0, 0.0, 1.0, 8.51235, 0.0, 0.0, 0.0);
    cut(book[14], 149.346, 51.4929);
    book[14].bits = 16;
    addMode(book[14], 155.076, 0.825153, 22.7213, 52.7552, 0.323941, 2.52855);
    addMode(book[14], 338.814, 0.282634, 20.3147, 2.19405, 0.0734187, 0.535508);
    addMode(book[14], 686.084, 0.000421141, 4.02323, 4.37002, 0.176879, 0.52609);
    addBand(book[14], 0, 478.481, 0.548711, 0.0501597, 8.37504, 0.246102, 0.433589, 7.07949,
            0.14295);
    addBand(book[14], 0, 236.323, 4.95876, 2.70821, 11.3209, 0.402978, 43.8533, 9.00362, 0.619444);
    addBand(book[14], 0, 245.802, 0.670838, 1.48927, 3.25821, 0.059232, 3.85502, 9.90231, 0.278723);
    addBand(book[14], 0, 724.467, 0.36316, 0.163175, 3.59374, 6.7644, 30.6475, 50.3279, 0.304359);
    addBand(book[14], 0, 1681.91, 1.21292, 0.0294793, 0.921498, 1.9324, 131.848, 60.4598, 0.157694);
    addBand(book[14], 0, 4904.56, 2.84836, 0.00264603, 7.12065, 5.49571, 0.828597, 131.361,
            0.518061);
    addBand(book[14], 0, 5236.73, 4.88064, 0.380532, 5.70317, 2.58849, 1.56869, 84.3304, 0.303226);
    addBand(book[14], 0, 3167.64, 2.00078, 0.961464, -0.0674305, 0.0937398, 3.99025, 45.302,
            0.222547);
    addBand(book[14], 0, 4090.32, 4.44036, 0.150175, 11.4726, 3.01203, 72.813, 102.654, 0.226331);
    // The studio bright snare.
    recording(book[15], 44100.0, 344.69, 0.204, 2.64734, 0.0191225, 4.84961, 0.824925);
    machine(book[15], 4.57523, 0.0, 0.0, 1.0, 2.01815, 0.0, 0.0, 0.0);
    cut(book[15], 91.6624, 53.8035);
    book[15].bits = 16;
    addMode(book[15], 168.087, 0.178223, 12.3941, 43.3028, 0.404033, 2.45444);
    addMode(book[15], 392.638, 0.016143, 0.403182, 59.9955, 0.157394, 1.21283);
    addMode(book[15], 1158.92, 0.00210896, 37.453, 14.1769, 0.862405, 2.35884);
    addBand(book[15], 0, 239.228, 5.50815, 0.765203, 0.75253, 0.0872545, 2.74498, 47.3821,
            0.451226);
    addBand(book[15], 0, 174.512, 0.225814, 0.117328, 1.1933, 0.0227753, 31.6622, 158.439,
            0.0411941);
    addBand(book[15], 0, 1307.36, 0.435608, 0.00116111, 10.441, 18.745, 2.43685, 235.892, 0.196566);
    addBand(book[15], 0, 1965.32, 3.75587, 0.0810125, 2.09338, 0.41056, 43.0742, 77.239, 0.158092);
    addBand(book[15], 0, 1828.88, 0.695627, 0.00167615, 12.7544, 0.0643975, 21.8412, 22.6242,
            0.319764);
    addBand(book[15], 0, 5340.6, 3.78778, 0.0301518, -0.504976, 0.608627, 74.8494, 1273.99,
            0.871883);
    addBand(book[15], 0, 3788.84, 0.787822, 0.347169, 2.36573, 0.0751626, 0.545821, 1.03166,
            0.16399);
    addBand(book[15], 0, 3183.33, 6.30552, 0.154968, 1.35482, 23.4515, 52.3611, 61.0152, 0.547209);
    addBand(book[15], 0, 18992.4, 0.458661, 0.00031644, 10.5405, 0.327898, 123.563, 1.93518,
            0.304181);
    // The studio deep snare: the longest.
    recording(book[16], 44100.0, 538.71, 0.295, 1.54147, 0.0117325, 3.61349, 16.3328);
    machine(book[16], -0.48859, 0.0, 0.0, 1.0, 19.1402, 0.0, 0.0, 0.0);
    cut(book[16], 78.2551, 74.8468);
    book[16].bits = 16;
    addMode(book[16], 139.914, 0.397628, 37.9204, 50.7803, 0.403807, 1.678);
    addMode(book[16], 192.37, 0.16943, 22.561, 377.409, 0.414272, -1.40462);
    addMode(book[16], 703.103, 0.000591485, 17.8326, 104.223, 0.275057, -0.625032);
    addBand(book[16], 0, 160.499, 2.79134, 2.01582, 0.00130703, 0.237194, 17.5676, 14.7253,
            0.069089);
    addBand(book[16], 0, 633.864, 0.447341, 0.00578696, 3.19409, 0.287593, 66.2799, 218.548,
            0.153466);
    addBand(book[16], 0, 329.415, 5.99337, 1.02961, 5.12803, 0.0348768, 57.2218, 34.6827, 0.623325);
    addBand(book[16], 0, 2803.96, 1.46093, 0.269919, -0.822026, 0.675702, 57.6135, 5.19497,
            0.502378);
    addBand(book[16], 0, 1258.1, 0.791421, 0.0360351, 0.793417, 0.0723978, 7.66586, 166.181,
            0.58574);
    addBand(book[16], 0, 1573.34, 3.31916, 0.0882869, 5.28066, 0.014206, 8.50964, 28.1784,
            0.0488827);
    addBand(book[16], 0, 5191.37, 2.64336, 0.243404, 1.13468, 0.210424, 3.93527, 185.544, 0.284864);
    addBand(book[16], 0, 6713.81, 8.15213, 0.0946921, 11.5391, 0.625181, 19.889, 702.929, 0.427158);
    addBand(book[16], 0, 9715.1, 0.979713, 0.000427848, 0.544618, 0.109902, 77.1978, 31.4468,
            0.342893);
    // The studio tight snare: the shortest.
    recording(book[17], 44100.0, 204.56, 0.249, 0.91938, 0.0166289, 3.38363, 6.14944);
    machine(book[17], 5.45591, 0.0, 0.0, 1.0, 2.23159, 0.0, 0.0, 0.0);
    cut(book[17], 108.9, 26.8175);
    book[17].bits = 16;
    addMode(book[17], 177.308, 0.529286, 38.8029, 6.8048, 0.610528, 2.39967);
    addMode(book[17], 570.179, 0.021497, 37.71, 10.8017, 0.607235, 1.17277);
    addMode(book[17], 642.593, 0.00192682, 81.5409, 9.06839, 0.159774, -0.496788);
    addMode(book[17], 3263.92, 0.000628847, 26.377, 7.06346, 0.292882, -1.97007);
    addMode(book[17], 7695.95, 0.0196194, 33.3618, 1.24834, 0.221755, 1.58139);
    addMode(book[17], 9317.64, 0.0027477, 22.5554, 71.9435, 0.663567, 0.644474);
    addBand(book[17], 0, 265.339, 2.16192, 1.26719, -1.11266, 1.64106, 38.9689, 36.5249, 0.55807);
    addBand(book[17], 0, 201.379, 1.16479, 0.00190124, 9.35557, 1.40319, 51.2251, 38.9132,
            0.204802);
    addBand(book[17], 0, 2283.56, 0.81349, 0.287204, 1.74252, 0.0789249, 3.65463, 56.0942,
            0.503064);
    addBand(book[17], 0, 3006.03, 4.80503, 0.00106302, 0.967248, 0.0133526, 166.265, 11.0113,
            0.383681);
    addBand(book[17], 0, 5458.88, 2.50925, 0.0690788, 4.24464, 0.248432, 256.691, 218.582,
            0.207042);
    addBand(book[17], 0, 5931.85, 0.309258, 0.00208893, -0.546295, 0.0224673, 6.91161, 3.48309,
            0.290262);
    // The studio cross stick: modes for the shell below 1.5 kHz, the stick bands of noise.
    recording(book[18], 44100.0, 197.03, 1.497, 0.386101, 0.0199587, 0.0, 1.0);
    machine(book[18], 3.62008, 0.0, 0.0, 1.0, 3.07167, 0.0, 0.0, 0.0);
    cut(book[18], 21.6033, 131.391);
    book[18].bits = 16;
    addMode(book[18], 178.706, 0.00418855, 6.74641, 579.796, 0.44718, 1.49039);
    addMode(book[18], 207.914, 0.317522, 10.5033, 4.82397, 0.11182, 1.808);
    addMode(book[18], 239.59, 0.0201348, 95.5683, 46.5869, 0.374999, 0.672472);
    addMode(book[18], 372.215, 0.00630272, 60.2105, 16.6643, 0.298055, 2.9632);
    addBand(book[18], 0, 275.323, 2.81088, 0.597192, 3.23895, 10.8016, 28.2227, 23.1409, 0.0514883);
    addBand(book[18], 0, 150.706, 4.12123, 1.81889, 12.0473, 2.35073, 0.496944, 798.691, 0.0223377);
    addBand(book[18], 0, 1356.77, 0.841665, 0.000662507, -1.16498, 21.1448, 9.14081, 302.012,
            0.364104);
    addBand(book[18], 0, 1940.43, 0.587503, 0.34005, -0.990603, 0.0346812, 17.4797, 81.8721,
            0.0839916);
    addBand(book[18], 0, 5155.78, 3.24936, 2.80432, 0.257709, 0.0171843, 25.3399, 2.43875,
            0.759881);
    addBand(book[18], 0, 6919.24, 5.97614, 0.572724, 5.45646, 18.257, 24.8857, 6.41864, 0.374114);
    addBand(book[18], 0, 15304.5, 1.3403, 0.000459331, 0.854935, 0.163658, 4.9011, 36.4, 0.849684);
    addBand(book[18], 0, 12454.0, 0.371694, 0.000804997, -0.47803, 4.00893, 85.3821, 28.4672,
            0.0364739);
    // The studio clap: one recipe for both of the bank's claps, its hands as band 0's bursts.
    recording(book[19], 44100.0, 291.29, 1.678, 15.3625, 0.0, 0.0, 1.0);
    machine(book[19], 4.29148, 0.0, 0.0, 1.0, 18.1997, 0.0, 0.0, 0.0);
    claps(book[19], 3, 8.3836, 1.2165, 0.040728);
    cut(book[19], 11.113, 75.4282);
    book[19].bits = 16;
    addBand(book[19], 0, 688.149, 1.88049, 0.00362633, -0.12748, 0.853928, 22.9088, 60.4481,
            0.9799);
    addBand(book[19], 0, 1369.41, 1.87178, 0.0527832, -0.847782, 0.207138, 195.376, 3.1287,
            0.796502);
    addBand(book[19], 0, 748.98, 1.79216, 0.0036785, 3.86388, 0.47605, 0.308534, 372.394, 0.294807);
    addBand(book[19], 0, 5692.14, 0.20392, 0.00276127, -1.8237, 1.52663, 0.881886, 253.329,
            0.0494396);
    addBand(book[19], 0, 1720.08, 1.02981, 0.0940856, 14.9815, 1.7612, 46.2531, 6.36035, 0.777361);
    addBand(book[19], 0, 5433.12, 8.22592, 0.0010537, 10.0992, 0.228966, 13.0626, 143.939,
            0.0507136);
    // The studio closed hat: a one-shot recording, not a loop; noise only, its metal
    // narrow bands, no tones (a tone there rings).
    recording(book[20], 44100.0, 166.35, 0.499, 1.07125, 0.0, 0.0, 1.0);
    machine(book[20], -4.85503, 0.0, 0.0, 1.0, 6.23779, 0.0, 0.0, 0.0);
    cut(book[20], 90.9639, 37.5686);
    book[20].bits = 16;
    addBand(book[20], 0, 177.227, 2.46053, 0.0603108, 1.69474, 11.5379, 9.74939, 12.0421, 0.503632);
    addBand(book[20], 0, 118.307, 0.200069, 0.203679, 3.88214, 0.0663973, 27.0494, 75.9388,
            0.190403);
    addBand(book[20], 0, 250.742, 1.05446, 0.00132722, 6.46275, 3.70299, 2.65478, 6.64491,
            0.492009);
    addBand(book[20], 0, 963.866, 4.29733, 0.048921, 13.6774, 0.0354465, 3.4212, 50.5965, 0.187207);
    addBand(book[20], 0, 1954.67, 0.689255, 0.0133125, 8.23591, 1.00054, 42.1992, 1373.01, 0.26138);
    addBand(book[20], 0, 1855.25, 4.40246, 0.00114708, 8.433, 2.09581, 2.20612, 1.73022, 0.783787);
    addBand(book[20], 0, 765.375, 2.55384, 0.0894529, -1.90242, 0.101047, 27.0335, 8.88286,
            0.862225);
    addBand(book[20], 0, 5618.38, 5.46086, 0.000365218, 1.62356, 0.028059, 15.088, 311.589,
            0.0363069);
    addBand(book[20], 0, 3400.5, 1.63114, 0.0416859, 10.6269, 53.641, 4.08808, 36.9443, 0.147294);
    addBand(book[20], 0, 5925.14, 15.3862, 0.239869, 2.51368, 8.37144, 23.0376, 56.6486, 0.650572);
    addBand(book[20], 0, 4467.52, 0.349433, 0.317171, 0.602706, 0.163805, 1.79994, 2.37833,
            0.423605);
    addBand(book[20], 0, 11907.3, 0.559109, 0.723012, -1.94069, 1.06035, 28.187, 33.1585, 0.761021);
    // The studio open hat.
    recording(book[21], 44100.0, 322.7, 0.227, 3.51552, 0.0, 0.0, 1.0);
    machine(book[21], -8.6315, 0.0, 0.0, 1.0, 11.6664, 0.0, 0.0, 0.0);
    cut(book[21], 206.045, 22.4533);
    book[21].bits = 16;
    addBand(book[21], 0, 176.256, 0.266123, 0.00185367, 5.73817, 15.1028, 2.11621, 1303.17,
            0.312386);
    addBand(book[21], 0, 877.111, 0.559276, 0.0299076, 10.3293, 1.39519, 60.8416, 16.812, 0.576486);
    addBand(book[21], 0, 595.164, 38.9701, 0.00212104, 9.23147, 0.0657022, 5.81093, 63.0838,
            0.05432);
    addBand(book[21], 0, 234.116, 0.200313, 0.0700703, 0.72973, 0.699689, 51.7241, 335.132,
            0.481755);
    addBand(book[21], 0, 655.461, 4.24967, 0.000450889, 10.8495, 3.27625, 8.86117, 16.6585,
            0.430105);
    addBand(book[21], 0, 785.06, 10.4821, 0.00848091, -1.38473, 0.0870204, 0.955504, 59.9352,
            0.0171262);
    addBand(book[21], 0, 5835.49, 5.56719, 0.32058, 3.49206, 0.059054, 18.2438, 55.6197, 0.509812);
    addBand(book[21], 0, 4806.73, 6.51194, 0.000418368, 12.4745, 0.0478244, 0.797774, 522.8,
            0.14567);
    addBand(book[21], 0, 9784.18, 0.840777, 0.0743064, 12.0389, 16.2441, 122.778, 1011.79, 0.29887);
    addBand(book[21], 0, 7547.16, 8.94548, 0.484298, 4.03161, 0.927903, 102.951, 15.6628, 0.52781);
    addBand(book[21], 0, 10275.0, 1.4174, 0.331996, 0.382959, 0.0723254, 139.571, 1.86069,
            0.105735);
    addBand(book[21], 0, 6464.59, 1.33717, 0.0229973, -1.06152, 0.178709, 14.2169, 391.963,
            0.912253);
    // The studio block.
    recording(book[22], 44100.0, 314.2, 0.181, 1.32589, 0.0180666, 0.0, 1.0);
    machine(book[22], -7.12002, 0.0, 0.0, 1.0, 59.8575, 0.0, 0.0, 0.0);
    cut(book[22], 9.8391, 54.6097);
    book[22].bits = 16;
    addMode(book[22], 1144.51, 0.0153033, 22.2031, 33.509, 0.760652, -1.12324);
    addMode(book[22], 1257.24, 0.655136, 4.90416, 2.42836, 0.154237, -1.85947);
    addMode(book[22], 1387.75, 0.00171614, 48.9092, 8.79124, 0.374122, -2.11225);
    addMode(book[22], 1489.9, 0.505008, 3.94152, 5.71682, 0.656906, 1.00901);
    addMode(book[22], 2626.56, 0.0892291, 4.3652, 1.21433, 0.164987, -3.05604);
    addMode(book[22], 3716.48, 0.125448, 0.357573, 5.01754, 0.608137, 2.36772);
    addBand(book[22], 0, 471.579, 1.33119, 0.00383486, 3.37222, 59.1856, 1.30324, 23.1758,
            0.704669);
    addBand(book[22], 0, 1344.27, 7.5791, 0.0950791, 7.92912, 1.18632, 1.78913, 335.822, 0.926934);
    addBand(book[22], 0, 5141.6, 4.01016, 0.559623, -1.18465, 0.0133001, 5.00361, 5.42415,
            0.585328);
    // The studio echo snare: a snare in a long room, the room's close modes below 1.5 kHz, bands
    // of noise above.
    recording(book[23], 44100.0, 583.29, 0.34, 3.79055, 0.167343, 0.0, 1.0);
    machine(book[23], 0.74117, 0.0, 0.0, 1.0, 2.71768, 0.0, 0.0, 0.0);
    cut(book[23], 93.972, 116.227);
    book[23].bits = 16;
    addMode(book[23], 168.483, 0.0913493, 2.5671, 41.658, 0.989312, 1.40152);
    addMode(book[23], 752.006, 0.0202289, 1.2645, 13.1678, 0.228972, -0.491398);
    addMode(book[23], 902.534, 0.0109838, 42.497, 1.64676, 0.423089, 0.467543);
    addMode(book[23], 976.479, 0.000565302, 39.9495, 35.3344, 0.0764825, -0.523564);
    addMode(book[23], 1071.85, 0.00502625, 3.86178, 4.08977, 0.387077, 1.71377);
    addMode(book[23], 1254.62, 0.00428899, 5.94713, 13.8565, 0.322964, 1.43898);
    addMode(book[23], 1423.23, 0.00204262, 21.918, 10.4812, 0.425434, 2.11111);
    addBand(book[23], 0, 202.401, 10.6989, 2.00159, 14.3377, 0.333616, 1.15368, 939.602, 0.423895);
    addBand(book[23], 0, 158.541, 7.22658, 2.18817, -1.00923, 0.1486, 251.567, 511.749, 0.65184);
    addBand(book[23], 0, 1834.82, 1.51548, 0.122925, 14.7652, 1.34386, 50.4797, 2.10834, 0.546691);
    addBand(book[23], 0, 1667.86, 3.38813, 0.828236, -1.00356, 0.272001, 91.4008, 1.47987,
            0.960113);
    addBand(book[23], 0, 3599.52, 5.16649, 0.191075, 14.4085, 0.179747, 28.7603, 55.3103, 0.731648);
    addBand(book[23], 0, 1370.21, 9.56797, 0.00295646, 14.3526, 0.0346042, 2.34449, 429.041,
            0.159736);
    addBand(book[23], 0, 4290.38, 2.44685, 0.00188384, 9.11438, 8.4117, 0.730724, 43.0165,
            0.104373);
    addBand(book[23], 0, 18215.0, 0.604427, 0.00963955, -0.879179, 26.8472, 3.85479, 2.02788,
            0.583417);
    // The studio muted cowbell.
    recording(book[24], 44100.0, 345.96, 0.635, 0.61109, 1.49149, 0.0, 1.0);
    machine(book[24], 1.32391, 0.0, 0.0, 1.0, 4.05304, 0.0, 0.0, 0.0);
    cut(book[24], 5.45399, 103.628);
    book[24].bits = 16;
    addMode(book[24], 420.963, 0.293339, 26.0138, 9.56548, 0.796315, -0.652558);
    addMode(book[24], 472.812, 0.386751, 25.042, 10.6507, 0.577946, 2.54941);
    addMode(book[24], 1120.33, 0.231273, 10.4438, 1.19838, 0.481484, -2.28813);
    addMode(book[24], 1550.65, 0.000646126, 0.925552, 10.7726, 0.509024, 1.2594);
    addMode(book[24], 1901.47, 0.01023, 0.643191, 25.0804, 0.684449, 2.64732);
    addMode(book[24], 2596.17, 0.102397, 9.641, 45.9881, 0.941582, -1.88597);
    addMode(book[24], 3450.58, 0.00608074, 1.49228, 18.2483, 0.89197, 0.312986);
    addMode(book[24], 3573.78, 0.0143663, 1.58238, 5.19366, 0.585555, -0.97522);
    addBand(book[24], 0, 401.269, 0.489973, 0.026751, 7.82966, 79.268, 6.93712, 640.134, 0.527426);
    addBand(book[24], 0, 2341.82, 1.95029, 0.843036, -1.80061, 0.27455, 6.9709, 43.6629, 0.0955185);
    addBand(book[24], 0, 6767.81, 1.81764, 0.00116474, 6.18792, 0.0901586, 6.95736, 42.8132,
            0.197191);
    // The studio open cowbell.
    recording(book[25], 44100.0, 307.69, 0.771, 3.38178, 0.087378, 0.0, 1.0);
    machine(book[25], -2.54998, 0.0, 0.0, 1.0, 3.93367, 0.0, 0.0, 0.0);
    cut(book[25], 11.8783, 79.2455);
    book[25].bits = 16;
    addMode(book[25], 459.214, 0.0743156, 11.4464, 24.3925, 0.459962, 1.24229);
    addMode(book[25], 508.989, 0.0984643, 2.87353, 26.3448, 0.453458, -1.99239);
    addMode(book[25], 1193.0, 0.048125, 9.257, 20.9867, 0.209769, 0.6895);
    addMode(book[25], 2118.9, 0.0214595, 84.324, 54.9187, 0.705698, -1.1037);
    addMode(book[25], 2782.28, 0.0102829, 37.6095, 9.7603, 0.061511, -0.413435);
    addMode(book[25], 3534.71, 0.0301737, 16.5991, 15.3362, 0.76554, -1.50573);
    addMode(book[25], 5004.2, 0.0015151, 9.44262, 4.11476, 0.367773, -0.85479);
    addMode(book[25], 6111.53, 0.00579228, 4.17136, 6.62794, 0.422012, -1.80388);
    addBand(book[25], 0, 287.278, 1.4898, 0.00108177, 3.47944, 0.34847, 14.0433, 3.43448, 0.175031);
    addBand(book[25], 0, 2368.52, 0.904796, 0.186228, -0.577938, 0.377673, 123.589, 6.62565,
            0.959152);
    addBand(book[25], 0, 12103.6, 0.993484, 0.00135971, 0.190973, 19.2921, 0.788774, 10.1704,
            0.13533);
    // The studio cowbell.
    recording(book[26], 44100.0, 98.16, 0.907, 3.7416, 0.109433, 0.0, 1.0);
    machine(book[26], -9.2368, 0.0, 0.0, 1.0, 3.9259, 0.0, 0.0, 0.0);
    cut(book[26], 66.1343, 12.0259);
    book[26].bits = 16;
    addMode(book[26], 465.364, 0.352981, 12.6357, 6.10541, 0.119695, 1.51737);
    addMode(book[26], 510.364, 0.466741, 17.3786, 9.5641, 0.728442, -1.53373);
    addMode(book[26], 976.196, 0.00553628, 0.434867, 12.1081, 0.29685, 0.841843);
    addMode(book[26], 1180.04, 0.0393382, 4.98466, 11.5821, 0.531575, 1.04705);
    addMode(book[26], 1727.56, 0.0101591, 41.1966, 14.9588, 0.234227, 1.20043);
    addMode(book[26], 2286.21, 0.242364, 1.1993, 14.2982, 0.560412, -0.191166);
    addMode(book[26], 3079.74, 0.0295132, 19.6646, 8.15798, 0.0719439, -2.13026);
    addMode(book[26], 3863.45, 0.426438, 2.04195, 9.37042, 0.420448, 0.602257);
    addBand(book[26], 0, 582.43, 1.65114, 0.0318608, 0.899777, 7.10587, 1.611, 24.3625, 0.0375328);
    addBand(book[26], 0, 1790.64, 0.456386, 0.132396, -1.79442, 0.165961, 3.01688, 38.4247,
            0.302432);
    addBand(book[26], 0, 7141.95, 3.37152, 0.0127392, 4.23046, 5.48021, 0.42007, 10.4777, 0.332816);
    // The studio dark cowbell.
    recording(book[27], 44100.0, 97.44, 1.02, 0.791811, 0.351021, 0.0, 1.0);
    machine(book[27], -1.7275, 0.0, 0.0, 1.0, 94.6193, 0.0, 0.0, 0.0);
    cut(book[27], 6.69844, 40.2493);
    book[27].bits = 16;
    addMode(book[27], 459.212, 0.316749, 5.43893, 26.1674, 0.628522, 0.361478);
    addMode(book[27], 513.345, 0.358455, 21.049, 22.4024, 0.456076, -3.1347);
    addMode(book[27], 848.588, 0.0769542, 25.9489, 6.67811, 0.456449, -0.891979);
    addMode(book[27], 1363.85, 0.125086, 0.313259, 25.3327, 0.834479, 2.63737);
    addMode(book[27], 1524.33, 0.106073, 19.8058, 27.9359, 0.589522, 2.60161);
    addMode(book[27], 2051.21, 0.0388791, 16.5895, 70.0495, 0.369063, -0.467172);
    addMode(book[27], 3063.04, 0.000333478, 41.8267, 1.28094, 0.170866, 2.68605);
    addMode(book[27], 3292.85, 0.00749352, 4.38686, 82.259, 0.550653, -2.87616);
    addBand(book[27], 0, 106.565, 0.514303, 0.0425381, 3.75957, 0.0329612, 13.8554, 6.38898,
            0.527007);
    addBand(book[27], 0, 2006.81, 0.518237, 0.626391, -0.546526, 0.619139, 6.69259, 187.784,
            0.0589107);
    addBand(book[27], 0, 16848.2, 0.552201, 0.0255631, -1.11916, 0.123755, 1.09157, 395.004,
            0.302415);
    // The studio tom (the kit tunes it apart, as the classic tom).
    recording(book[28], 44100.0, 2532.61, 0.295, 5.21512, 0.0941791, 6.1761, 1.88268);
    machine(book[28], -2.32844, 0.0, 0.0, 1.0, 18.1478, 0.0, 0.0, 0.0);
    cut(book[28], 1638.67, 447.216);
    book[28].bits = 16;
    addMode(book[28], 116.934, 0.138233, 26.6635, 500.68, 0.248934, 1.85774);
    addMode(book[28], 454.915, 0.0638068, 4.54894, 23.5346, 0.354161, 2.94661);
    addMode(book[28], 549.629, 0.000566571, 0.444618, 7.12275, 0.519603, -0.562332);
    addMode(book[28], 618.022, 0.00681078, 38.5279, 1.57437, 0.484665, -0.0530158);
    addMode(book[28], 781.149, 0.00282264, 13.7918, 5.20475, 0.577571, -1.32297);
    addMode(book[28], 1243.62, 0.00941139, 35.3341, 1.87004, 0.728854, 2.05705);
    addMode(book[28], 1460.41, 0.0209217, 1.34227, 12.2044, 0.539887, 0.57636);
    addMode(book[28], 1762.03, 0.0137084, 18.4425, 3.19257, 0.360919, 0.0853982);
    addMode(book[28], 2001.85, 0.00357459, 4.9701, 26.6818, 0.727673, -1.79443);
    addMode(book[28], 2121.42, 0.011229, 8.24135, 8.42276, 0.318763, -0.675204);
    addBand(book[28], 0, 108.7, 7.4992, 2.0624, 4.42787, 0.0780057, 295.586, 42.2224, 0.886912);
    addBand(book[28], 0, 3376.61, 3.64715, 0.000303504, 0.288317, 16.2441, 46.0104, 367.356,
            0.336424);
    addBand(book[28], 0, 3728.53, 1.39372, 0.142185, -1.38889, 0.144729, 1.08088, 3.74481,
            0.473996);
    // The studio short duck.
    recording(book[29], 44100.0, 44.54, 0.181, 0.845887, 0.0, 0.0, 1.0);
    machine(book[29], 1.67831, 0.0, 0.0, 1.0, 6.92456, 0.0, 0.0, 0.0);
    cut(book[29], 24.6709, 2.91137);
    book[29].bits = 16;
    addBand(book[29], 0, 357.859, 14.7921, 0.00104467, 1.81728, 40.7882, 22.7693, 172.052,
            0.747015);
    addBand(book[29], 0, 677.768, 2.89592, 0.00686912, 3.10378, 72.3915, 0.841767, 144.786,
            0.494254);
    addBand(book[29], 0, 1165.45, 6.20975, 0.00752, 0.355871, 0.601452, 30.387, 3.58529, 0.926878);
    addBand(book[29], 0, 485.324, 1.05717, 0.00035172, 6.75355, 57.6778, 2.6881, 6.97403, 0.503575);
    addBand(book[29], 0, 1969.56, 5.85333, 1.64172, 3.66605, 0.115409, 8.54038, 2.07666, 0.367782);
    addBand(book[29], 0, 3425.57, 6.06139, 2.12496, 0.487737, 0.0135535, 7.37174, 22.2796,
            0.0185336);
    addBand(book[29], 0, 2002.25, 3.96915, 0.322232, 7.04526, 0.353076, 2.52244, 115.756, 0.158741);
    addBand(book[29], 0, 947.338, 2.53757, 0.00120214, -0.215563, 0.107336, 140.968, 372.093,
            0.705274);
    addBand(book[29], 0, 3135.73, 5.29303, 0.0219866, 7.53709, 0.0436372, 135.217, 202.903,
            0.99228);
    addBand(book[29], 0, 2479.96, 13.4499, 0.270633, -0.954746, 6.24364, 352.403, 934.525,
            0.447219);
    addBand(book[29], 0, 4059.18, 2.20698, 0.000599246, -0.869929, 0.320007, 16.1274, 20.7146,
            0.245879);
    addBand(book[29], 0, 5688.15, 16.3416, 0.00154674, 4.99879, 9.22851, 5.22759, 268.334,
            0.330585);
    // The studio long duck: noise only, its lead-in a band before the hit.
    recording(book[30], 44100.0, 73.99, 15.306, 5.37938, 0.0, 0.0, 1.0);
    machine(book[30], -6.5833, 0.0, 0.0, 1.0, 283.136, 0.0, 0.0, 0.0);
    cut(book[30], 43.5379, 4.15747);
    book[30].bits = 16;
    addBand(book[30], 0, 83.0878, 16.1693, 0.165683, -7.97416, 3.03174, 0.679646, 995.292,
            0.234217);
    addBand(book[30], 0, 707.855, 2.22444, 0.0328145, 4.01045, 0.401949, 1.774, 13.9118, 0.562515);
    addBand(book[30], 0, 188.423, 8.63456, 0.00162398, 8.55831, 1.64136, 25.2571, 182.97, 0.197203);
    addBand(book[30], 0, 1438.62, 0.780199, 0.0110791, 11.126, 0.0198585, 0.662769, 7.95051,
            0.581201);
    addBand(book[30], 0, 2235.45, 0.54793, 0.0127557, 4.66376, 48.8791, 9.0191, 4.46472, 0.48855);
    addBand(book[30], 0, 3016.51, 9.58375, 0.374933, 11.4251, 19.636, 395.737, 16.5636, 0.17013);
    addBand(book[30], 0, 1111.38, 10.0706, 0.292309, -1.97526, 0.132292, 250.631, 7.38512,
            0.967781);
    addBand(book[30], 0, 2410.5, 7.04521, 0.0106864, 1.19562, 2.50008, 73.0751, 330.029, 0.26647);
    addBand(book[30], 0, 3761.06, 8.70103, 0.579574, 10.6327, 7.9727, 96.2866, 120.075, 0.645719);
    addBand(book[30], 0, 2464.94, 8.146, 0.189594, 5.79017, 0.339057, 75.6863, 122.938, 0.399184);
    addBand(book[30], 0, 2736.23, 17.1848, 0.0144463, -1.5284, 0.0310742, 0.335418, 6.16833,
            0.332525);
    addBand(book[30], 0, 4773.31, 4.20713, 0.00422184, 6.43133, 55.9511, 87.8121, 26.1524,
            0.529156);
    // The studio explosion.
    recording(book[31], 44100.0, 1342.47, 0.227, 42.1894, 7.3759, 0.0, 1.0);
    machine(book[31], -7.1765, 0.0, 0.0, 1.0, 10.0979, 0.0, 0.0, 0.0);
    cut(book[31], 222.539, 476.612);
    book[31].bits = 16;
    addMode(book[31], 156.954, 0.0428277, 0.66116, 151.794, 0.242737, -1.88422);
    addMode(book[31], 190.208, 0.00540292, 5.27373, 145.886, 0.872361, -2.50226);
    addMode(book[31], 258.61, 0.000747562, 859.851, 4.22807, 0.213972, -2.98974);
    addMode(book[31], 328.018, 0.00263756, 5.26192, 13.0253, 0.751636, -0.835352);
    addMode(book[31], 498.016, 0.00220071, 0.436862, 12.5269, 0.176851, -2.97315);
    addMode(book[31], 666.871, 0.0187646, 1.98312, 28.9498, 0.0818102, 0.785608);
    addMode(book[31], 870.334, 0.0045642, 26.9758, 324.841, 0.0228565, -1.01965);
    addMode(book[31], 1004.98, 0.000472171, 18.9003, 40.5052, 0.571493, -0.0709644);
    addMode(book[31], 1311.22, 0.0517068, 1.06401, 10.1673, 0.432087, -1.61815);
    addMode(book[31], 1666.46, 0.00119951, 53.1655, 2.63636, 0.529883, 1.61221);
    addBand(book[31], 0, 168.299, 1.54924, 0.690563, 0.110075, 0.170796, 279.818, 333.369,
            0.300902);
    addBand(book[31], 0, 630.915, 10.5936, 0.000782336, -0.95335, 0.108018, 137.36, 167.044,
            0.21497);
    addBand(book[31], 0, 718.686, 10.0557, 1.06683, 0.204961, 0.0750331, 13.1294, 2.07782,
            0.0404016);
    addBand(book[31], 0, 1627.72, 0.361914, 0.00410725, -1.32101, 0.188026, 20.4419, 8.68017,
            0.560918);
    addBand(book[31], 0, 1864.65, 2.44455, 0.00155915, -0.00689439, 1.80382, 42.401, 167.351,
            0.447975);
    addBand(book[31], 0, 12464.9, 9.32748, 0.000731949, 7.01066, 0.100149, 0.731505, 65.2877,
            0.346167);
}
// --- end of generated recipes ------------------------------------------------------------

// A pad's settings as numbers, read from its parameters once they change.
struct Pad {
    int key = 36;  // the MIDI note that plays it
    int sound = 0;
    double tune = 0.0;      // semitones
    double decay = 1.0;     // how long it rings, as a ratio
    double level = 0.0;     // dB
    double pan = 0.0;       // -1 .. 1
    double velocity = 0.7;  // 0 .. 1
    double vary = 0.0;
    int choke = 0;   // 0 none, 1 .. 8
    int output = 0;  // 0 main, 1 .. 8
};

struct Drumlin : Instrument {
    static constexpr Info info = {
        .name = "Drumlin",
        .id = "com.suppleaudio.drumlin",
        .vendor = "Supple Audio",
        .description =
            "Drum machine: realistic drums, synthesized without samples, on sixteen pads, each on "
            "a MIDI note of its own (padN.key; General MIDI's drum map by default), with choke "
            "groups and eight extra outputs.",
        .features = {"drum-machine", "synthesizer"},
        .outputs = {"Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8"},
        .voices = kVoices};

    // --- The pads ---------------------------------------------------------------------------
    //
    // Pad N's parameters are pN...: parameters cannot sit in an array, so
    // each pad has its own, read into pad[] by readPads(). Each pad says the
    // note that plays it (padN.key), so that a host, a player and an agent
    // writing a part all see the map: the default kit is on General MIDI's
    // drum keys, the notation's drum names.

    // Pad 1: Classic Kick on key 36 (C2; notation: kick).
    Param p1Key =
        integer("Pad 1 Key", 0, 127, 36)
            .withKey("pad1.key")
            .in("Pad 1")
            .help("The MIDI note that plays the pad (36, C2, by default: General MIDI's map).");
    Param p1Sound =
        choice("Pad 1 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               0)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad1.sound")
            .in("Pad 1")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p1Tune = semitones("Pad 1 Tune", -12, 12, 0)
                       .withKey("pad1.tune")
                       .in("Pad 1")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p1Decay = percent("Pad 1 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad1.decay")
                        .in("Pad 1")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p1Level = decibels("Pad 1 Level", -60, 12, 0)
                        .withKey("pad1.level")
                        .in("Pad 1")
                        .help("The pad's level.");
    Param p1Pan = percent("Pad 1 Pan", 0, -100, 100)
                      .withKey("pad1.pan")
                      .in("Pad 1")
                      .help("Where it sits, left to right.");
    Param p1Velocity = percent("Pad 1 Velocity", 70)
                           .withKey("pad1.velocity")
                           .in("Pad 1")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p1Vary = percent("Pad 1 Vary", 50)
                       .withKey("pad1.vary")
                       .in("Pad 1")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
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

    // Pad 2: Classic Rimshot on key 37 (C#2; notation: rim).
    Param p2Key =
        integer("Pad 2 Key", 0, 127, 37)
            .withKey("pad2.key")
            .in("Pad 2")
            .help("The MIDI note that plays the pad (37, C#2, by default: General MIDI's map).");
    Param p2Sound =
        choice("Pad 2 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               2)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad2.sound")
            .in("Pad 2")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p2Tune = semitones("Pad 2 Tune", -12, 12, 0)
                       .withKey("pad2.tune")
                       .in("Pad 2")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p2Decay = percent("Pad 2 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad2.decay")
                        .in("Pad 2")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p2Level = decibels("Pad 2 Level", -60, 12, -3)
                        .withKey("pad2.level")
                        .in("Pad 2")
                        .help("The pad's level.");
    Param p2Pan = percent("Pad 2 Pan", 0, -100, 100)
                      .withKey("pad2.pan")
                      .in("Pad 2")
                      .help("Where it sits, left to right.");
    Param p2Velocity = percent("Pad 2 Velocity", 70)
                           .withKey("pad2.velocity")
                           .in("Pad 2")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p2Vary = percent("Pad 2 Vary", 50)
                       .withKey("pad2.vary")
                       .in("Pad 2")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
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

    // Pad 3: Classic Snare on key 38 (D2; notation: snare).
    Param p3Key =
        integer("Pad 3 Key", 0, 127, 38)
            .withKey("pad3.key")
            .in("Pad 3")
            .help("The MIDI note that plays the pad (38, D2, by default: General MIDI's map).");
    Param p3Sound =
        choice("Pad 3 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               1)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad3.sound")
            .in("Pad 3")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p3Tune = semitones("Pad 3 Tune", -12, 12, 0)
                       .withKey("pad3.tune")
                       .in("Pad 3")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p3Decay = percent("Pad 3 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad3.decay")
                        .in("Pad 3")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p3Level = decibels("Pad 3 Level", -60, 12, 0)
                        .withKey("pad3.level")
                        .in("Pad 3")
                        .help("The pad's level.");
    Param p3Pan = percent("Pad 3 Pan", 0, -100, 100)
                      .withKey("pad3.pan")
                      .in("Pad 3")
                      .help("Where it sits, left to right.");
    Param p3Velocity = percent("Pad 3 Velocity", 70)
                           .withKey("pad3.velocity")
                           .in("Pad 3")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p3Vary = percent("Pad 3 Vary", 50)
                       .withKey("pad3.vary")
                       .in("Pad 3")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
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

    // Pad 4: Classic Clap on key 39 (D#2; notation: clap).
    Param p4Key =
        integer("Pad 4 Key", 0, 127, 39)
            .withKey("pad4.key")
            .in("Pad 4")
            .help("The MIDI note that plays the pad (39, D#2, by default: General MIDI's map).");
    Param p4Sound =
        choice("Pad 4 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               3)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad4.sound")
            .in("Pad 4")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p4Tune = semitones("Pad 4 Tune", -12, 12, 0)
                       .withKey("pad4.tune")
                       .in("Pad 4")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p4Decay = percent("Pad 4 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad4.decay")
                        .in("Pad 4")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p4Level = decibels("Pad 4 Level", -60, 12, -2)
                        .withKey("pad4.level")
                        .in("Pad 4")
                        .help("The pad's level.");
    Param p4Pan = percent("Pad 4 Pan", 0, -100, 100)
                      .withKey("pad4.pan")
                      .in("Pad 4")
                      .help("Where it sits, left to right.");
    Param p4Velocity = percent("Pad 4 Velocity", 70)
                           .withKey("pad4.velocity")
                           .in("Pad 4")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p4Vary = percent("Pad 4 Vary", 50)
                       .withKey("pad4.vary")
                       .in("Pad 4")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
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

    // Pad 5: Classic Tom on key 41 (F2; notation: tom_floor).
    Param p5Key =
        integer("Pad 5 Key", 0, 127, 41)
            .withKey("pad5.key")
            .in("Pad 5")
            .help("The MIDI note that plays the pad (41, F2, by default: General MIDI's map).");
    Param p5Sound =
        choice("Pad 5 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               6)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad5.sound")
            .in("Pad 5")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p5Tune = semitones("Pad 5 Tune", -12, 12, -5)
                       .withKey("pad5.tune")
                       .in("Pad 5")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p5Decay = percent("Pad 5 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad5.decay")
                        .in("Pad 5")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p5Level = decibels("Pad 5 Level", -60, 12, -2)
                        .withKey("pad5.level")
                        .in("Pad 5")
                        .help("The pad's level.");
    Param p5Pan = percent("Pad 5 Pan", -15, -100, 100)
                      .withKey("pad5.pan")
                      .in("Pad 5")
                      .help("Where it sits, left to right.");
    Param p5Velocity = percent("Pad 5 Velocity", 70)
                           .withKey("pad5.velocity")
                           .in("Pad 5")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p5Vary = percent("Pad 5 Vary", 50)
                       .withKey("pad5.vary")
                       .in("Pad 5")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
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

    // Pad 6: Classic Closed Hat on key 42 (F#2; notation: hat).
    Param p6Key =
        integer("Pad 6 Key", 0, 127, 42)
            .withKey("pad6.key")
            .in("Pad 6")
            .help("The MIDI note that plays the pad (42, F#2, by default: General MIDI's map).");
    Param p6Sound =
        choice("Pad 6 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               4)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad6.sound")
            .in("Pad 6")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p6Tune = semitones("Pad 6 Tune", -12, 12, 0)
                       .withKey("pad6.tune")
                       .in("Pad 6")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p6Decay = percent("Pad 6 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad6.decay")
                        .in("Pad 6")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p6Level = decibels("Pad 6 Level", -60, 12, -3)
                        .withKey("pad6.level")
                        .in("Pad 6")
                        .help("The pad's level.");
    Param p6Pan = percent("Pad 6 Pan", 0, -100, 100)
                      .withKey("pad6.pan")
                      .in("Pad 6")
                      .help("Where it sits, left to right.");
    Param p6Velocity = percent("Pad 6 Velocity", 70)
                           .withKey("pad6.velocity")
                           .in("Pad 6")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p6Vary = percent("Pad 6 Vary", 50)
                       .withKey("pad6.vary")
                       .in("Pad 6")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
    Param p6Choke = choice("Pad 6 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 1)
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

    // Pad 7: Classic Tom on key 45 (A2; notation: tom_low).
    Param p7Key =
        integer("Pad 7 Key", 0, 127, 45)
            .withKey("pad7.key")
            .in("Pad 7")
            .help("The MIDI note that plays the pad (45, A2, by default: General MIDI's map).");
    Param p7Sound =
        choice("Pad 7 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               6)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad7.sound")
            .in("Pad 7")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p7Tune = semitones("Pad 7 Tune", -12, 12, -2)
                       .withKey("pad7.tune")
                       .in("Pad 7")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p7Decay = percent("Pad 7 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad7.decay")
                        .in("Pad 7")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p7Level = decibels("Pad 7 Level", -60, 12, -2)
                        .withKey("pad7.level")
                        .in("Pad 7")
                        .help("The pad's level.");
    Param p7Pan = percent("Pad 7 Pan", -5, -100, 100)
                      .withKey("pad7.pan")
                      .in("Pad 7")
                      .help("Where it sits, left to right.");
    Param p7Velocity = percent("Pad 7 Velocity", 70)
                           .withKey("pad7.velocity")
                           .in("Pad 7")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p7Vary = percent("Pad 7 Vary", 50)
                       .withKey("pad7.vary")
                       .in("Pad 7")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
    Param p7Choke = choice("Pad 7 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
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

    // Pad 8: Classic Open Hat on key 46 (A#2; notation: ohat).
    Param p8Key =
        integer("Pad 8 Key", 0, 127, 46)
            .withKey("pad8.key")
            .in("Pad 8")
            .help("The MIDI note that plays the pad (46, A#2, by default: General MIDI's map).");
    Param p8Sound =
        choice("Pad 8 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               5)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad8.sound")
            .in("Pad 8")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p8Tune = semitones("Pad 8 Tune", -12, 12, 0)
                       .withKey("pad8.tune")
                       .in("Pad 8")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p8Decay = percent("Pad 8 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad8.decay")
                        .in("Pad 8")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p8Level = decibels("Pad 8 Level", -60, 12, -3)
                        .withKey("pad8.level")
                        .in("Pad 8")
                        .help("The pad's level.");
    Param p8Pan = percent("Pad 8 Pan", 0, -100, 100)
                      .withKey("pad8.pan")
                      .in("Pad 8")
                      .help("Where it sits, left to right.");
    Param p8Velocity = percent("Pad 8 Velocity", 70)
                           .withKey("pad8.velocity")
                           .in("Pad 8")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p8Vary = percent("Pad 8 Vary", 50)
                       .withKey("pad8.vary")
                       .in("Pad 8")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
    Param p8Choke = choice("Pad 8 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 1)
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

    // Pad 9: Classic Tom on key 47 (B2; notation: tom_mid).
    Param p9Key =
        integer("Pad 9 Key", 0, 127, 47)
            .withKey("pad9.key")
            .in("Pad 9")
            .help("The MIDI note that plays the pad (47, B2, by default: General MIDI's map).");
    Param p9Sound =
        choice("Pad 9 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                               "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                               "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                               "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                               "Classic Ride",         "Studio Kick",         "Studio Snare",
                               "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                               "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                               "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                               "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                               "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                               "Studio Long Duck",     "Studio Explosion"},
               6)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad9.sound")
            .in("Pad 9")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p9Tune = semitones("Pad 9 Tune", -12, 12, 1)
                       .withKey("pad9.tune")
                       .in("Pad 9")
                       .help(
                           "The machine's tuning knob: the whole sound faster and higher, or "
                           "slower and lower.");
    Param p9Decay = percent("Pad 9 Decay", 100, 25, 400)
                        .curved(Curve::Log)
                        .withKey("pad9.decay")
                        .in("Pad 9")
                        .help(
                            "How long it rings: 100 % as the machine played it; a hi-hat's VCA, or "
                            "the drum's own decay.");
    Param p9Level = decibels("Pad 9 Level", -60, 12, -2)
                        .withKey("pad9.level")
                        .in("Pad 9")
                        .help("The pad's level.");
    Param p9Pan = percent("Pad 9 Pan", 5, -100, 100)
                      .withKey("pad9.pan")
                      .in("Pad 9")
                      .help("Where it sits, left to right.");
    Param p9Velocity = percent("Pad 9 Velocity", 70)
                           .withKey("pad9.velocity")
                           .in("Pad 9")
                           .help(
                               "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                               "across the range.");
    Param p9Vary = percent("Pad 9 Vary", 50)
                       .withKey("pad9.vary")
                       .in("Pad 9")
                       .help(
                           "Each hit a little different, as a real drum is: new noise, and small "
                           "changes of pitch and tone.");
    Param p9Choke = choice("Pad 9 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
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

    // Pad 10: Classic Crash on key 49 (C#3; notation: crash).
    Param p10Key =
        integer("Pad 10 Key", 0, 127, 49)
            .withKey("pad10.key")
            .in("Pad 10")
            .help("The MIDI note that plays the pad (49, C#3, by default: General MIDI's map).");
    Param p10Sound =
        choice("Pad 10 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               11)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad10.sound")
            .in("Pad 10")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p10Tune = semitones("Pad 10 Tune", -12, 12, 0)
                        .withKey("pad10.tune")
                        .in("Pad 10")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p10Decay = percent("Pad 10 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad10.decay")
                         .in("Pad 10")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p10Level = decibels("Pad 10 Level", -60, 12, -6)
                         .withKey("pad10.level")
                         .in("Pad 10")
                         .help("The pad's level.");
    Param p10Pan = percent("Pad 10 Pan", 0, -100, 100)
                       .withKey("pad10.pan")
                       .in("Pad 10")
                       .help("Where it sits, left to right.");
    Param p10Velocity = percent("Pad 10 Velocity", 70)
                            .withKey("pad10.velocity")
                            .in("Pad 10")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p10Vary = percent("Pad 10 Vary", 50)
                        .withKey("pad10.vary")
                        .in("Pad 10")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
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

    // Pad 11: Classic Tom on key 50 (D3; notation: tom_high).
    Param p11Key =
        integer("Pad 11 Key", 0, 127, 50)
            .withKey("pad11.key")
            .in("Pad 11")
            .help("The MIDI note that plays the pad (50, D3, by default: General MIDI's map).");
    Param p11Sound =
        choice("Pad 11 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               6)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad11.sound")
            .in("Pad 11")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p11Tune = semitones("Pad 11 Tune", -12, 12, 4)
                        .withKey("pad11.tune")
                        .in("Pad 11")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p11Decay = percent("Pad 11 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad11.decay")
                         .in("Pad 11")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p11Level = decibels("Pad 11 Level", -60, 12, -2)
                         .withKey("pad11.level")
                         .in("Pad 11")
                         .help("The pad's level.");
    Param p11Pan = percent("Pad 11 Pan", 15, -100, 100)
                       .withKey("pad11.pan")
                       .in("Pad 11")
                       .help("Where it sits, left to right.");
    Param p11Velocity = percent("Pad 11 Velocity", 70)
                            .withKey("pad11.velocity")
                            .in("Pad 11")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p11Vary = percent("Pad 11 Vary", 50)
                        .withKey("pad11.vary")
                        .in("Pad 11")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
    Param p11Choke = choice("Pad 11 Choke", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}, 0)
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

    // Pad 12: Classic Ride on key 51 (D#3; notation: ride).
    Param p12Key =
        integer("Pad 12 Key", 0, 127, 51)
            .withKey("pad12.key")
            .in("Pad 12")
            .help("The MIDI note that plays the pad (51, D#3, by default: General MIDI's map).");
    Param p12Sound =
        choice("Pad 12 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               12)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad12.sound")
            .in("Pad 12")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p12Tune = semitones("Pad 12 Tune", -12, 12, 0)
                        .withKey("pad12.tune")
                        .in("Pad 12")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p12Decay = percent("Pad 12 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad12.decay")
                         .in("Pad 12")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p12Level = decibels("Pad 12 Level", -60, 12, -6)
                         .withKey("pad12.level")
                         .in("Pad 12")
                         .help("The pad's level.");
    Param p12Pan = percent("Pad 12 Pan", 0, -100, 100)
                       .withKey("pad12.pan")
                       .in("Pad 12")
                       .help("Where it sits, left to right.");
    Param p12Velocity = percent("Pad 12 Velocity", 70)
                            .withKey("pad12.velocity")
                            .in("Pad 12")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p12Vary = percent("Pad 12 Vary", 50)
                        .withKey("pad12.vary")
                        .in("Pad 12")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
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

    // Pad 13: Classic Tambourine on key 54 (F#3; notation: tamb).
    Param p13Key =
        integer("Pad 13 Key", 0, 127, 54)
            .withKey("pad13.key")
            .in("Pad 13")
            .help("The MIDI note that plays the pad (54, F#3, by default: General MIDI's map).");
    Param p13Sound =
        choice("Pad 13 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               9)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad13.sound")
            .in("Pad 13")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p13Tune = semitones("Pad 13 Tune", -12, 12, 0)
                        .withKey("pad13.tune")
                        .in("Pad 13")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p13Decay = percent("Pad 13 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad13.decay")
                         .in("Pad 13")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p13Level = decibels("Pad 13 Level", -60, 12, -4)
                         .withKey("pad13.level")
                         .in("Pad 13")
                         .help("The pad's level.");
    Param p13Pan = percent("Pad 13 Pan", 0, -100, 100)
                       .withKey("pad13.pan")
                       .in("Pad 13")
                       .help("Where it sits, left to right.");
    Param p13Velocity = percent("Pad 13 Velocity", 70)
                            .withKey("pad13.velocity")
                            .in("Pad 13")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p13Vary = percent("Pad 13 Vary", 50)
                        .withKey("pad13.vary")
                        .in("Pad 13")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
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

    // Pad 14: Classic Cowbell on key 56 (G#3; notation: cowbell).
    Param p14Key =
        integer("Pad 14 Key", 0, 127, 56)
            .withKey("pad14.key")
            .in("Pad 14")
            .help("The MIDI note that plays the pad (56, G#3, by default: General MIDI's map).");
    Param p14Sound =
        choice("Pad 14 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               10)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad14.sound")
            .in("Pad 14")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p14Tune = semitones("Pad 14 Tune", -12, 12, 0)
                        .withKey("pad14.tune")
                        .in("Pad 14")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p14Decay = percent("Pad 14 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad14.decay")
                         .in("Pad 14")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p14Level = decibels("Pad 14 Level", -60, 12, -4)
                         .withKey("pad14.level")
                         .in("Pad 14")
                         .help("The pad's level.");
    Param p14Pan = percent("Pad 14 Pan", 0, -100, 100)
                       .withKey("pad14.pan")
                       .in("Pad 14")
                       .help("Where it sits, left to right.");
    Param p14Velocity = percent("Pad 14 Velocity", 70)
                            .withKey("pad14.velocity")
                            .in("Pad 14")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p14Vary = percent("Pad 14 Vary", 50)
                        .withKey("pad14.vary")
                        .in("Pad 14")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
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

    // Pad 15: Classic Conga on key 63 (D#4; notation: n63, GM's open high conga).
    Param p15Key =
        integer("Pad 15 Key", 0, 127, 63)
            .withKey("pad15.key")
            .in("Pad 15")
            .help("The MIDI note that plays the pad (63, D#4, by default: General MIDI's map).");
    Param p15Sound =
        choice("Pad 15 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               7)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad15.sound")
            .in("Pad 15")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p15Tune = semitones("Pad 15 Tune", -12, 12, 0)
                        .withKey("pad15.tune")
                        .in("Pad 15")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p15Decay = percent("Pad 15 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad15.decay")
                         .in("Pad 15")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p15Level = decibels("Pad 15 Level", -60, 12, -3)
                         .withKey("pad15.level")
                         .in("Pad 15")
                         .help("The pad's level.");
    Param p15Pan = percent("Pad 15 Pan", 10, -100, 100)
                       .withKey("pad15.pan")
                       .in("Pad 15")
                       .help("Where it sits, left to right.");
    Param p15Velocity = percent("Pad 15 Velocity", 70)
                            .withKey("pad15.velocity")
                            .in("Pad 15")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p15Vary = percent("Pad 15 Vary", 50)
                        .withKey("pad15.vary")
                        .in("Pad 15")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
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

    // Pad 16: Classic Cabasa on key 69 (A4; notation: cabasa).
    Param p16Key =
        integer("Pad 16 Key", 0, 127, 69)
            .withKey("pad16.key")
            .in("Pad 16")
            .help("The MIDI note that plays the pad (69, A4, by default: General MIDI's map).");
    Param p16Sound =
        choice("Pad 16 Sound", {"Classic Kick",         "Classic Snare",       "Classic Rimshot",
                                "Classic Clap",         "Classic Closed Hat",  "Classic Open Hat",
                                "Classic Tom",          "Classic Conga",       "Classic Cabasa",
                                "Classic Tambourine",   "Classic Cowbell",     "Classic Crash",
                                "Classic Ride",         "Studio Kick",         "Studio Snare",
                                "Studio Bright Snare",  "Studio Deep Snare",   "Studio Tight Snare",
                                "Studio Cross Stick",   "Studio Clap",         "Studio Closed Hat",
                                "Studio Open Hat",      "Studio Block",        "Studio Echo Snare",
                                "Studio Muted Cowbell", "Studio Open Cowbell", "Studio Cowbell",
                                "Studio Dark Cowbell",  "Studio Tom",          "Studio Short Duck",
                                "Studio Long Duck",     "Studio Explosion"},
               8)
            .keys({"classic_kick",         "classic_snare",       "classic_rimshot",
                   "classic_clap",         "classic_closed_hat",  "classic_open_hat",
                   "classic_tom",          "classic_conga",       "classic_cabasa",
                   "classic_tambourine",   "classic_cowbell",     "classic_crash",
                   "classic_ride",         "studio_kick",         "studio_snare",
                   "studio_bright_snare",  "studio_deep_snare",   "studio_tight_snare",
                   "studio_cross_stick",   "studio_clap",         "studio_closed_hat",
                   "studio_open_hat",      "studio_block",        "studio_echo_snare",
                   "studio_muted_cowbell", "studio_open_cowbell", "studio_cowbell",
                   "studio_dark_cowbell",  "studio_tom",          "studio_short_duck",
                   "studio_long_duck",     "studio_explosion"})
            .withKey("pad16.sound")
            .in("Pad 16")
            .help(
                "What the pad plays: one of the machines' sounds. Its Key is the note "
                "that plays it.");
    Param p16Tune = semitones("Pad 16 Tune", -12, 12, 0)
                        .withKey("pad16.tune")
                        .in("Pad 16")
                        .help(
                            "The machine's tuning knob: the whole sound faster and higher, or "
                            "slower and lower.");
    Param p16Decay = percent("Pad 16 Decay", 100, 25, 400)
                         .curved(Curve::Log)
                         .withKey("pad16.decay")
                         .in("Pad 16")
                         .help(
                             "How long it rings: 100 % as the machine played it; a hi-hat's VCA, "
                             "or the drum's own decay.");
    Param p16Level = decibels("Pad 16 Level", -60, 12, -4)
                         .withKey("pad16.level")
                         .in("Pad 16")
                         .help("The pad's level.");
    Param p16Pan = percent("Pad 16 Pan", 0, -100, 100)
                       .withKey("pad16.pan")
                       .in("Pad 16")
                       .help("Where it sits, left to right.");
    Param p16Velocity = percent("Pad 16 Velocity", 70)
                            .withKey("pad16.velocity")
                            .in("Pad 16")
                            .help(
                                "How much quieter a soft hit is than a hard one: at 100 %, 40 dB "
                                "across the range.");
    Param p16Vary = percent("Pad 16 Vary", 50)
                        .withKey("pad16.vary")
                        .in("Pad 16")
                        .help(
                            "Each hit a little different, as a real drum is: new noise, and small "
                            "changes of pitch and tone.");
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

    Param output = decibels("Output", -36, 12, 0)
                       .smoothed(20)
                       .in("Pads")
                       .help(
                           "The level of every output. A gentle limit above -1 dBFS keeps hits "
                           "from clipping.");

    // --- State ------------------------------------------------------------------------------

    Array<Recipe, kModels> recipe;
    Buffer<double> loops;  // each loop's EPROM: kLoop samples apiece
    Array<Voice, kVoices> voice;
    Array<int, kVoices> padOf;  // the pad each voice plays, -1 none
    Array<int, kVoices> busOf;
    Array<double, kVoices> gainL;
    Array<double, kVoices> gainR;
    Array<bool, kVoices> held;  // its key still down
    Array<Pad, kPads> pad;
    Array<int, kPads> hits;  // each pad's hits since prepare(): its variation's seed
    double rate = 88200.0;   // the voices' (oversampled) rate
    int stride = 0;          // a bus's room in the mix
    Buffer<double> wide;     // a voice's samples, oversampled
    Buffer<double> mixL;
    Buffer<double> mixR;
    Array<Oversampler, 18> over;  // two a bus
    Array<int, kBuses> quiet;
    Array<double, kBuses> restL;  // what stolen voices were playing, fading out
    Array<double, kBuses> restR;
    double restFade = 0.99;
    int tail = 0;
    double peakL = 0.0;
    double peakR = 0.0;

    // --- What the editor shows, sent by the audio code ---------------------------------------

    Shared<double> outLeft;  // the main output's peak, dB, falling back
    Shared<double> outRight;
    Shared<int> soundingNow;      // voices playing
    SharedArray<float, 16> glow;  // each pad's loudest voice now, 0 .. 1

    // --- Hooks ------------------------------------------------------------------------------

    void prepare() {
        rate = sampleRate() * kFactor;
        for (int c = 0; c < 18; ++c) {
            over[c].prepare(kFactor);
            over[c].clear();
        }
        // The DAC's steps come a sample late (at 2x) and the down filters
        // add about 1.4 samples: 2 in all at the session's rate.
        setLatency(2);
        stride = maxBlockSize() * kFactor;
        wide.resize(stride);
        mixL.resize(stride * kBuses);
        mixR.resize(stride * kBuses);
        loops.resize(kLoops * kLoop);
        recipes(recipe);
        for (int m = 0; m < kModels; ++m)
            if (recipe[m].loop >= 0) makeLoop(recipe[m], loops);
        for (int v = 0; v < kVoices; ++v) {
            voice[v].sounding = false;
            padOf[v] = -1;
            busOf[v] = 0;
            held[v] = false;
        }
        quiet.fill(4096);
        restL.fill(0.0);
        restR.fill(0.0);
        restFade = exp(-1.0 / (0.003 * rate));
        hits.fill(0);
        tail = 0;
        peakL = 0.0;
        peakR = 0.0;
        reportTail(false);
        readPads();
    }

    void update() {
        setVoiceMode(VoiceMode::Poly);
        readPads();
    }

    // Every pad's settings into pad[].
    void readPads() {
        readPad(0, p1Key, p1Sound, p1Tune, p1Decay, p1Level, p1Pan, p1Velocity, p1Vary, p1Choke,
                p1Output);
        readPad(1, p2Key, p2Sound, p2Tune, p2Decay, p2Level, p2Pan, p2Velocity, p2Vary, p2Choke,
                p2Output);
        readPad(2, p3Key, p3Sound, p3Tune, p3Decay, p3Level, p3Pan, p3Velocity, p3Vary, p3Choke,
                p3Output);
        readPad(3, p4Key, p4Sound, p4Tune, p4Decay, p4Level, p4Pan, p4Velocity, p4Vary, p4Choke,
                p4Output);
        readPad(4, p5Key, p5Sound, p5Tune, p5Decay, p5Level, p5Pan, p5Velocity, p5Vary, p5Choke,
                p5Output);
        readPad(5, p6Key, p6Sound, p6Tune, p6Decay, p6Level, p6Pan, p6Velocity, p6Vary, p6Choke,
                p6Output);
        readPad(6, p7Key, p7Sound, p7Tune, p7Decay, p7Level, p7Pan, p7Velocity, p7Vary, p7Choke,
                p7Output);
        readPad(7, p8Key, p8Sound, p8Tune, p8Decay, p8Level, p8Pan, p8Velocity, p8Vary, p8Choke,
                p8Output);
        readPad(8, p9Key, p9Sound, p9Tune, p9Decay, p9Level, p9Pan, p9Velocity, p9Vary, p9Choke,
                p9Output);
        readPad(9, p10Key, p10Sound, p10Tune, p10Decay, p10Level, p10Pan, p10Velocity, p10Vary,
                p10Choke, p10Output);
        readPad(10, p11Key, p11Sound, p11Tune, p11Decay, p11Level, p11Pan, p11Velocity, p11Vary,
                p11Choke, p11Output);
        readPad(11, p12Key, p12Sound, p12Tune, p12Decay, p12Level, p12Pan, p12Velocity, p12Vary,
                p12Choke, p12Output);
        readPad(12, p13Key, p13Sound, p13Tune, p13Decay, p13Level, p13Pan, p13Velocity, p13Vary,
                p13Choke, p13Output);
        readPad(13, p14Key, p14Sound, p14Tune, p14Decay, p14Level, p14Pan, p14Velocity, p14Vary,
                p14Choke, p14Output);
        readPad(14, p15Key, p15Sound, p15Tune, p15Decay, p15Level, p15Pan, p15Velocity, p15Vary,
                p15Choke, p15Output);
        readPad(15, p16Key, p16Sound, p16Tune, p16Decay, p16Level, p16Pan, p16Velocity, p16Vary,
                p16Choke, p16Output);
    }

    void readPad(int p, const Param& key, const Param& sound, const Param& tune, const Param& decay,
                 const Param& level, const Param& pan, const Param& velocity, const Param& vary,
                 const Param& choke, const Param& out) {
        Pad& d = pad[p];
        d.key = key.index();
        d.sound = clamp(sound.index(), 0, kModels - 1);
        d.tune = tune.value();
        d.decay = decay.value();
        d.level = level.value();
        d.pan = pan.value();
        d.velocity = velocity.value();
        d.vary = vary.value();
        d.choke = choke.index();
        d.output = out.index();
    }

    // --- Notes ------------------------------------------------------------------------------

    void startVoice(int v) {
        Voice& s = voice[v];
        // The pad on this key: the first, if several share it.
        int p = -1;
        for (int q = kPads - 1; q >= 0; --q)
            if (pad[q].key == voiceKey(v)) p = q;
        if (p < 0) {
            padOf[v] = -1;
            s.sounding = false;
            return;
        }
        const Pad& d = pad[p];
        const Recipe& r = recipe[d.sound];
        // Its group's voices, this pad's own too, fade out.
        if (d.choke > 0)
            for (int u = 0; u < voiceCount(); ++u)
                if (u != v && voice[u].sounding && padOf[u] >= 0 && pad[padOf[u]].choke == d.choke)
                    choke(voice[u], rate, kChokeMs);
        padOf[v] = p;
        busOf[v] = d.output;
        held[v] = true;
        // This hit: its number picks its variation (and where a hi-hat's
        // loop is); with Vary at 0 every hit is the machine's, note for note,
        // with the noise the recipe was fitted with (seed 0).
        const int n = hits[p];
        hits[p] = n + 1;
        const int seed = d.vary > 0.0 ? (p + 1) * 7919 + n : 0;
        strike(s, r, d.sound, seed, d.vary, n + p * 104729, d.decay);
        // Velocity: the level, and a hi-hat's accent at the top of its range
        // (louder, and its decay a tenth longer, as the machine's).
        const double vel = voiceVelocity(v);
        const double hard = r.accent > 0.0 ? clamp((vel - 0.6) / 0.4, 0.0, 1.0) : 0.0;
        const double db = r.level + d.level - (1.0 - vel) * 40.0 * d.velocity + r.accent * hard;
        open(s, r, exp2(d.tune / 12.0), rate, r.decay * d.decay * (1.0 + 0.1 * hard), dbToGain(db));
        // Balance: the centre leaves both sides as they are.
        gainL[v] = min(1.0, 1.0 - d.pan);
        gainR[v] = min(1.0, 1.0 + d.pan);
    }

    // A hit plays out whatever its key does, as the machine's did; the key
    // up only stops its pad's light being held full.
    void releaseVoice(int v) { held[v] = false; }

    // Stolen: it stops now, and what it was playing fades out over a few
    // milliseconds instead of stepping to silence.
    void stopVoice(int v) {
        Voice& s = voice[v];
        if (s.sounding && padOf[v] >= 0) {
            restL[busOf[v]] += s.last * gainL[v];
            restR[busOf[v]] += s.last * gainR[v];
        }
        s.sounding = false;
        padOf[v] = -1;
    }

    void process(Block& io) {
        const int n = io.frames * kFactor;
        for (int b = 0; b < kBuses; ++b)
            for (int k = 0; k < n; ++k) {
                mixL[b * stride + k] = 0.0;
                mixR[b * stride + k] = 0.0;
            }
        bool sounding = false;
        for (int v = 0; v < voiceCount(); ++v) {
            Voice& s = voice[v];
            if (!voicePlaying(v)) continue;
            if (s.sounding && padOf[v] >= 0) {
                for (int k = 0; k < n; ++k) wide[k] = 0.0;
                play(s, recipe[s.model], loops, rate, wide, 0, n);
                const int base = busOf[v] * stride;
                for (int k = 0; k < n; ++k) {
                    mixL[base + k] += wide[k] * gainL[v];
                    mixR[base + k] += wide[k] * gainR[v];
                }
            }
            if (!s.sounding) {
                padOf[v] = -1;
                voiceFinished(v);
            } else {
                sounding = true;
            }
        }
        for (int b = 0; b < kBuses; ++b) bring(io, b, n);
        reportTail(sounding);
        show();
    }

    // What the editor shows: the level, the voices and how loud each pad is now.
    void show() {
        outLeft.set(gainToDb(peakL));
        outRight.set(gainToDb(peakR));
        for (int p = 0; p < kPads; ++p) glow[p] = 0.0;
        int count = 0;
        for (int v = 0; v < kVoices; ++v) {
            if (v >= voiceCount() || !voice[v].sounding || padOf[v] < 0) continue;
            ++count;
            const double level = held[v] ? 1.0 : min(1.0, abs(voice[v].last) * 4.0 + 0.2);
            glow[padOf[v]] = max(static_cast<double>(glow[padOf[v]]), level);
        }
        soundingNow.set(count);
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
            for (int k = 0; k < kFactor; ++k)
                if (mixL[base + i * kFactor + k] != 0.0 || mixR[base + i * kFactor + k] != 0.0)
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
            for (int k = 0; k < kFactor; ++k) {
                over[2 * b].set(k, mixL[base + i * kFactor + k]);
                over[2 * b + 1].set(k, mixR[base + i * kFactor + k]);
            }
            const double l = over[2 * b].down();
            const double r = over[2 * b + 1].down();
            const double g = dbToGain(output.at(i));
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

    // How long the output goes on: what is left of each sounding voice's
    // recording at its clock, or its VCA's fall to silence (an accent's
    // too), and 20 ms for a voice's fade and the downsampler. Raised whenever a voice needs
    // more, lowered only with no voice sounding.
    void reportTail(bool sounding) {
        double longest = kChokeMs;
        for (int v = 0; v < voiceCount(); ++v) {
            const Voice& s = voice[v];
            if (!s.sounding || padOf[v] < 0) continue;
            const Recipe& r = recipe[s.model];
            if (r.loop >= 0)
                longest = max(longest, 13.0 * r.decay * pad[padOf[v]].decay);
            else if (s.step > 0.0)
                longest = max(longest, (s.end - s.at) / (s.step * kFactor) / sampleRate() * 1000.0);
        }
        const int want = ceilToInt((min(longest, 60000.0) + 20.0) * 0.001 * sampleRate());
        if (want > tail || (!sounding && want != tail)) {
            tail = want;
            setTail(tail);
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
    // A header with the voices and the level; the sixteen pads, four by four
    // with pad 1 at the bottom left as on hardware, each with its sound's
    // name, kit and shape and a light while it sounds; pressing a pad plays
    // it, harder the higher up, and selects it. Beside them the selected pad:
    // its sound, chosen from a kit's (a tab for each kit, a button for each of
    // its sounds, and a step to the previous or next of all of them), drawn
    // as it will play (its tune and decay), and its settings. Every dial drags
    // up and down (Shift finely), turns with the wheel and goes back to its
    // default on a double click; a button sets its choice.

    static constexpr double kW = 1180;
    static constexpr double kH = 716;
    static constexpr double padsX = 16;  // the panels
    static constexpr double padsY = 76;
    static constexpr double padsW = 520;
    static constexpr double padsH = 560;
    static constexpr double selX = 548;
    static constexpr double selY = 76;
    static constexpr double selW = 616;
    static constexpr double selH = 560;
    static constexpr double barY = 648;  // every pad's
    static constexpr double barH = 52;
    static constexpr double gridX = padsX + 16;  // the pads
    static constexpr double gridY = padsY + 46;
    static constexpr double cellW = 118;
    static constexpr double cellH = 122;
    static constexpr double cellGap = 6;
    static constexpr double rowX = selX + 92;  // the selected pad's rows of buttons
    static constexpr double rowW = selW - 110;
    static constexpr double kitY = selY + 46;     // its sound: the kits' tabs
    static constexpr double soundsY = selY + 78;  // ... and the kit's sounds
    static constexpr int kAcross = 5;
    static constexpr double soundH = 22;
    static constexpr double soundGap = 5;
    static constexpr double wavX = selX + 16;  // the selected pad's sound, drawn
    static constexpr double wavY = selY + 196;
    static constexpr double wavW = selW - 32;
    static constexpr double wavH = 144;
    static constexpr double dialsY = selY + 404;
    static constexpr double chokeY = selY + 458;
    static constexpr double outY = selY + 490;
    static constexpr int kColumns = 240;  // points across a sound's shape

    // The kits: the classic sounds first, then the studio sounds.
    static constexpr int kKits = 2;
    static constexpr int kStudio = 13;  // the first studio sound

    // The palette: the theme's, and the till's.
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
    static constexpr int kTurf = 0xA3B57C;      // the sound: the drumlin's grassed back
    static constexpr int kClay = 0xC99A6B;      // its playing: the till under it
    static constexpr int kSeaGlass = 0x8FB3AC;  // choke and outputs
    static constexpr int kStone = 0x9A8C7A;

    // The editor's codes: a pad's fields 0-9, for the selected pad; every
    // pad's 101.
    static constexpr int cSound = 0;
    static constexpr int cTune = 1;
    static constexpr int cDecay = 2;
    static constexpr int cLevel = 3;
    static constexpr int cPan = 4;
    static constexpr int cVelocity = 5;
    static constexpr int cVary = 6;
    static constexpr int cChoke = 7;
    static constexpr int cOutput = 8;
    static constexpr int cKey = 9;
    static constexpr int cOut = 101;
    // The editor's own: the kit whose sounds it shows (102), and a step
    // through every sound (103: -1 back, +1 on).
    static constexpr int cKit = 102;
    static constexpr int cStep = 103;

    int selected = 0;
    int dragging = 0;  // 1 a dial, 3 a pad played
    int dragDial = -1;
    double dialPos = 0.0;
    int playedKey = -1;
    int playedPad = -1;
    int hotDial = -1;
    int hotButton = -1;
    int hotPad = -1;
    int kit = 0;          // the kit whose sounds show
    int shownSound = -1;  // the selected pad's sound when the kit last followed it
    Array<int, 32> dialCode;
    Array<double, 32> dialX, dialY, dialR;
    Array<int, 32> dialColour;
    Array<bool, 32> dialSide;  // its name and value beside it, not over and under
    int dials = 0;
    Array<int, 48> buttonCode, buttonValue, buttonColour;
    Array<double, 48> buttonX, buttonY, buttonW, buttonH;
    int buttons = 0;
    // Each pad's sound as the editor drew it: its peaks over kColumns, how
    // long that is, and what it was drawn for.
    Array<Recipe, kModels> book;
    bool booked = false;
    Array<float, 3840> shapes;  // kPads * kColumns
    Array<double, 16> shapeMs;
    Array<double, 16> shapeKey;

    void editor(Editor& e) { e.size(kW, kH); }

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
    double padField(int what, int how, const Param& key, const Param& sound, const Param& tune,
                    const Param& decay, const Param& level, const Param& pan, const Param& velocity,
                    const Param& vary, const Param& choke, const Param& out) const {
        if (what == cKey) return readOn(key, how);
        if (what == cSound) return readOn(sound, how);
        if (what == cTune) return readOn(tune, how);
        if (what == cDecay) return readOn(decay, how);
        if (what == cLevel) return readOn(level, how);
        if (what == cPan) return readOn(pan, how);
        if (what == cVelocity) return readOn(velocity, how);
        if (what == cVary) return readOn(vary, how);
        if (what == cChoke) return readOn(choke, how);
        return readOn(out, how);
    }
    void padEdit(int what, int how, double v, const Param& key, const Param& sound,
                 const Param& tune, const Param& decay, const Param& level, const Param& pan,
                 const Param& velocity, const Param& vary, const Param& choke, const Param& out) {
        if (what == cKey) editOn(key, how, v);
        if (what == cSound) editOn(sound, how, v);
        if (what == cTune) editOn(tune, how, v);
        if (what == cDecay) editOn(decay, how, v);
        if (what == cLevel) editOn(level, how, v);
        if (what == cPan) editOn(pan, how, v);
        if (what == cVelocity) editOn(velocity, how, v);
        if (what == cVary) editOn(vary, how, v);
        if (what == cChoke) editOn(choke, how, v);
        if (what == cOutput) editOn(out, how, v);
    }
    void padShow(Canvas& g, int what, double x, double y, const Param& key, const Param& sound,
                 const Param& tune, const Param& decay, const Param& level, const Param& pan,
                 const Param& velocity, const Param& vary, const Param& choke, const Param& out) {
        if (what == cKey) fillNote(g, key.index(), x, y);
        if (what == cSound) g.fillText(sound.text(), x, y);
        if (what == cTune) g.fillText(tune.text(), x, y);
        if (what == cDecay) g.fillText(decay.text(), x, y);
        if (what == cLevel) g.fillText(level.text(), x, y);
        if (what == cPan) g.fillText(pan.text(), x, y);
        if (what == cVelocity) g.fillText(velocity.text(), x, y);
        if (what == cVary) g.fillText(vary.text(), x, y);
        if (what == cChoke) g.fillText(choke.text(), x, y);
        if (what == cOutput) g.fillText(out.text(), x, y);
    }

    // Pad p's field `what` (read as padField says), its parameters found by the pad.
    double padRead(int p, int what, int how) const {
        switch (p) {
            case 0:
                return padField(what, how, p1Key, p1Sound, p1Tune, p1Decay, p1Level, p1Pan,
                                p1Velocity, p1Vary, p1Choke, p1Output);
            case 1:
                return padField(what, how, p2Key, p2Sound, p2Tune, p2Decay, p2Level, p2Pan,
                                p2Velocity, p2Vary, p2Choke, p2Output);
            case 2:
                return padField(what, how, p3Key, p3Sound, p3Tune, p3Decay, p3Level, p3Pan,
                                p3Velocity, p3Vary, p3Choke, p3Output);
            case 3:
                return padField(what, how, p4Key, p4Sound, p4Tune, p4Decay, p4Level, p4Pan,
                                p4Velocity, p4Vary, p4Choke, p4Output);
            case 4:
                return padField(what, how, p5Key, p5Sound, p5Tune, p5Decay, p5Level, p5Pan,
                                p5Velocity, p5Vary, p5Choke, p5Output);
            case 5:
                return padField(what, how, p6Key, p6Sound, p6Tune, p6Decay, p6Level, p6Pan,
                                p6Velocity, p6Vary, p6Choke, p6Output);
            case 6:
                return padField(what, how, p7Key, p7Sound, p7Tune, p7Decay, p7Level, p7Pan,
                                p7Velocity, p7Vary, p7Choke, p7Output);
            case 7:
                return padField(what, how, p8Key, p8Sound, p8Tune, p8Decay, p8Level, p8Pan,
                                p8Velocity, p8Vary, p8Choke, p8Output);
            case 8:
                return padField(what, how, p9Key, p9Sound, p9Tune, p9Decay, p9Level, p9Pan,
                                p9Velocity, p9Vary, p9Choke, p9Output);
            case 9:
                return padField(what, how, p10Key, p10Sound, p10Tune, p10Decay, p10Level, p10Pan,
                                p10Velocity, p10Vary, p10Choke, p10Output);
            case 10:
                return padField(what, how, p11Key, p11Sound, p11Tune, p11Decay, p11Level, p11Pan,
                                p11Velocity, p11Vary, p11Choke, p11Output);
            case 11:
                return padField(what, how, p12Key, p12Sound, p12Tune, p12Decay, p12Level, p12Pan,
                                p12Velocity, p12Vary, p12Choke, p12Output);
            case 12:
                return padField(what, how, p13Key, p13Sound, p13Tune, p13Decay, p13Level, p13Pan,
                                p13Velocity, p13Vary, p13Choke, p13Output);
            case 13:
                return padField(what, how, p14Key, p14Sound, p14Tune, p14Decay, p14Level, p14Pan,
                                p14Velocity, p14Vary, p14Choke, p14Output);
            case 14:
                return padField(what, how, p15Key, p15Sound, p15Tune, p15Decay, p15Level, p15Pan,
                                p15Velocity, p15Vary, p15Choke, p15Output);
            case 15:
                return padField(what, how, p16Key, p16Sound, p16Tune, p16Decay, p16Level, p16Pan,
                                p16Velocity, p16Vary, p16Choke, p16Output);
        }
        return 0.0;
    }
    void padWrite(int p, int what, int how, double v) {
        switch (p) {
            case 0:
                padEdit(what, how, v, p1Key, p1Sound, p1Tune, p1Decay, p1Level, p1Pan, p1Velocity,
                        p1Vary, p1Choke, p1Output);
                return;
            case 1:
                padEdit(what, how, v, p2Key, p2Sound, p2Tune, p2Decay, p2Level, p2Pan, p2Velocity,
                        p2Vary, p2Choke, p2Output);
                return;
            case 2:
                padEdit(what, how, v, p3Key, p3Sound, p3Tune, p3Decay, p3Level, p3Pan, p3Velocity,
                        p3Vary, p3Choke, p3Output);
                return;
            case 3:
                padEdit(what, how, v, p4Key, p4Sound, p4Tune, p4Decay, p4Level, p4Pan, p4Velocity,
                        p4Vary, p4Choke, p4Output);
                return;
            case 4:
                padEdit(what, how, v, p5Key, p5Sound, p5Tune, p5Decay, p5Level, p5Pan, p5Velocity,
                        p5Vary, p5Choke, p5Output);
                return;
            case 5:
                padEdit(what, how, v, p6Key, p6Sound, p6Tune, p6Decay, p6Level, p6Pan, p6Velocity,
                        p6Vary, p6Choke, p6Output);
                return;
            case 6:
                padEdit(what, how, v, p7Key, p7Sound, p7Tune, p7Decay, p7Level, p7Pan, p7Velocity,
                        p7Vary, p7Choke, p7Output);
                return;
            case 7:
                padEdit(what, how, v, p8Key, p8Sound, p8Tune, p8Decay, p8Level, p8Pan, p8Velocity,
                        p8Vary, p8Choke, p8Output);
                return;
            case 8:
                padEdit(what, how, v, p9Key, p9Sound, p9Tune, p9Decay, p9Level, p9Pan, p9Velocity,
                        p9Vary, p9Choke, p9Output);
                return;
            case 9:
                padEdit(what, how, v, p10Key, p10Sound, p10Tune, p10Decay, p10Level, p10Pan,
                        p10Velocity, p10Vary, p10Choke, p10Output);
                return;
            case 10:
                padEdit(what, how, v, p11Key, p11Sound, p11Tune, p11Decay, p11Level, p11Pan,
                        p11Velocity, p11Vary, p11Choke, p11Output);
                return;
            case 11:
                padEdit(what, how, v, p12Key, p12Sound, p12Tune, p12Decay, p12Level, p12Pan,
                        p12Velocity, p12Vary, p12Choke, p12Output);
                return;
            case 12:
                padEdit(what, how, v, p13Key, p13Sound, p13Tune, p13Decay, p13Level, p13Pan,
                        p13Velocity, p13Vary, p13Choke, p13Output);
                return;
            case 13:
                padEdit(what, how, v, p14Key, p14Sound, p14Tune, p14Decay, p14Level, p14Pan,
                        p14Velocity, p14Vary, p14Choke, p14Output);
                return;
            case 14:
                padEdit(what, how, v, p15Key, p15Sound, p15Tune, p15Decay, p15Level, p15Pan,
                        p15Velocity, p15Vary, p15Choke, p15Output);
                return;
            case 15:
                padEdit(what, how, v, p16Key, p16Sound, p16Tune, p16Decay, p16Level, p16Pan,
                        p16Velocity, p16Vary, p16Choke, p16Output);
                return;
        }
    }
    void padText(Canvas& g, int p, int what, double x, double y) {
        switch (p) {
            case 0:
                padShow(g, what, x, y, p1Key, p1Sound, p1Tune, p1Decay, p1Level, p1Pan, p1Velocity,
                        p1Vary, p1Choke, p1Output);
                return;
            case 1:
                padShow(g, what, x, y, p2Key, p2Sound, p2Tune, p2Decay, p2Level, p2Pan, p2Velocity,
                        p2Vary, p2Choke, p2Output);
                return;
            case 2:
                padShow(g, what, x, y, p3Key, p3Sound, p3Tune, p3Decay, p3Level, p3Pan, p3Velocity,
                        p3Vary, p3Choke, p3Output);
                return;
            case 3:
                padShow(g, what, x, y, p4Key, p4Sound, p4Tune, p4Decay, p4Level, p4Pan, p4Velocity,
                        p4Vary, p4Choke, p4Output);
                return;
            case 4:
                padShow(g, what, x, y, p5Key, p5Sound, p5Tune, p5Decay, p5Level, p5Pan, p5Velocity,
                        p5Vary, p5Choke, p5Output);
                return;
            case 5:
                padShow(g, what, x, y, p6Key, p6Sound, p6Tune, p6Decay, p6Level, p6Pan, p6Velocity,
                        p6Vary, p6Choke, p6Output);
                return;
            case 6:
                padShow(g, what, x, y, p7Key, p7Sound, p7Tune, p7Decay, p7Level, p7Pan, p7Velocity,
                        p7Vary, p7Choke, p7Output);
                return;
            case 7:
                padShow(g, what, x, y, p8Key, p8Sound, p8Tune, p8Decay, p8Level, p8Pan, p8Velocity,
                        p8Vary, p8Choke, p8Output);
                return;
            case 8:
                padShow(g, what, x, y, p9Key, p9Sound, p9Tune, p9Decay, p9Level, p9Pan, p9Velocity,
                        p9Vary, p9Choke, p9Output);
                return;
            case 9:
                padShow(g, what, x, y, p10Key, p10Sound, p10Tune, p10Decay, p10Level, p10Pan,
                        p10Velocity, p10Vary, p10Choke, p10Output);
                return;
            case 10:
                padShow(g, what, x, y, p11Key, p11Sound, p11Tune, p11Decay, p11Level, p11Pan,
                        p11Velocity, p11Vary, p11Choke, p11Output);
                return;
            case 11:
                padShow(g, what, x, y, p12Key, p12Sound, p12Tune, p12Decay, p12Level, p12Pan,
                        p12Velocity, p12Vary, p12Choke, p12Output);
                return;
            case 12:
                padShow(g, what, x, y, p13Key, p13Sound, p13Tune, p13Decay, p13Level, p13Pan,
                        p13Velocity, p13Vary, p13Choke, p13Output);
                return;
            case 13:
                padShow(g, what, x, y, p14Key, p14Sound, p14Tune, p14Decay, p14Level, p14Pan,
                        p14Velocity, p14Vary, p14Choke, p14Output);
                return;
            case 14:
                padShow(g, what, x, y, p15Key, p15Sound, p15Tune, p15Decay, p15Level, p15Pan,
                        p15Velocity, p15Vary, p15Choke, p15Output);
                return;
            case 15:
                padShow(g, what, x, y, p16Key, p16Sound, p16Tune, p16Decay, p16Level, p16Pan,
                        p16Velocity, p16Vary, p16Choke, p16Output);
                return;
        }
    }
    // Parameter `code`'s position on its dial (how 0), value in its unit
    // (1), or choice (2): the selected pad's, or every pad's.
    double readParam(int code, int how) const {
        if (code == cOut) return readOn(output, how);
        if (code == cKit) return kit;
        if (code == cStep) return 0.0;
        return padRead(selected, code, how);
    }
    // To a value (how 0), a position (1) or its default (2).
    void editParam(int code, int how, double v) {
        if (code == cOut) editOn(output, how, v);
        if (code == cKit) kit = clamp(roundToInt(v), 0, kKits - 1);
        if (code == cStep) {
            const int sound = wrap(choiceOf(cSound) + roundToInt(v), kModels);
            padWrite(selected, cSound, 0, sound);
        }
        if (code < 100) padWrite(selected, code, how, v);
    }
    // A sound's kit, and the colour it is shown in.
    int kitOf(int sound) const { return sound < kStudio ? 0 : 1; }
    int kitColour(int k) const { return k == 0 ? kTurf : kClay; }
    int choiceOf(int code) const { return roundToInt(readParam(code, 2)); }
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

    // A sound's short name, for a pad and a button.
    void fillSound(Canvas& g, int sound, double x, double y) const {
        if (sound == 0) g.fillText("KICK", x, y);
        if (sound == 1) g.fillText("SNARE", x, y);
        if (sound == 2) g.fillText("RIMSHOT", x, y);
        if (sound == 3) g.fillText("CLAP", x, y);
        if (sound == 4) g.fillText("CLOSED HAT", x, y);
        if (sound == 5) g.fillText("OPEN HAT", x, y);
        if (sound == 6) g.fillText("TOM", x, y);
        if (sound == 7) g.fillText("CONGA", x, y);
        if (sound == 8) g.fillText("CABASA", x, y);
        if (sound == 9) g.fillText("TAMBOURINE", x, y);
        if (sound == 10) g.fillText("COWBELL", x, y);
        if (sound == 11) g.fillText("CRASH", x, y);
        if (sound == 12) g.fillText("RIDE", x, y);
        if (sound == 13) g.fillText("KICK", x, y);
        if (sound == 14) g.fillText("SNARE", x, y);
        if (sound == 15) g.fillText("BRIGHT SNARE", x, y);
        if (sound == 16) g.fillText("DEEP SNARE", x, y);
        if (sound == 17) g.fillText("TIGHT SNARE", x, y);
        if (sound == 18) g.fillText("CROSS STICK", x, y);
        if (sound == 19) g.fillText("CLAP", x, y);
        if (sound == 20) g.fillText("CLOSED HAT", x, y);
        if (sound == 21) g.fillText("OPEN HAT", x, y);
        if (sound == 22) g.fillText("BLOCK", x, y);
        if (sound == 23) g.fillText("ECHO SNARE", x, y);
        if (sound == 24) g.fillText("MUTED COWBELL", x, y);
        if (sound == 25) g.fillText("OPEN COWBELL", x, y);
        if (sound == 26) g.fillText("COWBELL", x, y);
        if (sound == 27) g.fillText("DARK COWBELL", x, y);
        if (sound == 28) g.fillText("TOM", x, y);
        if (sound == 29) g.fillText("SHORT DUCK", x, y);
        if (sound == 30) g.fillText("LONG DUCK", x, y);
        if (sound == 31) g.fillText("EXPLOSION", x, y);
    }
    // ... and its kit's.
    void fillKit(Canvas& g, int kit, double x, double y) const {
        if (kit == 0)
            g.fillText("CLASSIC", x, y);
        else
            g.fillText("STUDIO", x, y);
    }

    void fillParam(Canvas& g, int code, double x, double y) {
        if (code == cKey) {
            g.fillText(format("%d ", padChoice(selected, cKey)), x - 14, y);
            fillNote(g, padChoice(selected, cKey), x + 12, y);
            return;
        }
        if (code == cOut) {
            g.fillText(output.text(), x, y);
            return;
        }
        padText(g, selected, code, x, y);
    }

    void fillDialName(Canvas& g, int code, double x, double y) const {
        if (code == cTune) g.fillText("TUNE", x, y);
        if (code == cDecay) g.fillText("DECAY", x, y);
        if (code == cLevel) g.fillText("LEVEL", x, y);
        if (code == cPan) g.fillText("PAN", x, y);
        if (code == cVelocity) g.fillText("VELOCITY", x, y);
        if (code == cVary) g.fillText("VARY", x, y);
        if (code == cKey) g.fillText("KEY", x, y);
        if (code == cOut) g.fillText("OUTPUT", x, y);
    }

    void fillButton(Canvas& g, int code, int value, double x, double y) const {
        if (code == cSound) fillSound(g, value, x, y);
        if (code == cKit) fillKit(g, value, x, y);
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
    }

    // --- what the editor shows, laid out -----------------------------------------------------

    void dial(int code, double x, double y, double r, int colour) {
        if (dials >= 32) return;
        dialCode[dials] = code;
        dialX[dials] = x;
        dialY[dials] = y;
        dialR[dials] = r;
        dialColour[dials] = colour;
        dialSide[dials] = false;
        ++dials;
    }

    void button(int code, int value, double x, double y, double w, double h, int colour) {
        if (buttons >= 48) return;
        buttonCode[buttons] = code;
        buttonValue[buttons] = value;
        buttonX[buttons] = x;
        buttonY[buttons] = y;
        buttonW[buttons] = w;
        buttonH[buttons] = h;
        buttonColour[buttons] = colour;
        ++buttons;
    }

    // n buttons for `code`'s values first .. first + n - 1, across w.
    void row(int code, int first, int n, double x, double y, double w, double h, int colour) {
        const double gap = 5.0;
        const double each = (w - gap * (n - 1)) / n;
        for (int k = 0; k < n; ++k)
            button(code, first + k, x + k * (each + gap), y, each, h, colour);
    }

    void place() {
        dials = 0;
        buttons = 0;
        // The kit shown follows the selected pad's sound whenever that changes
        // (another pad selected, a step, the host); a tab shows another.
        const int sound = choiceOf(cSound);
        if (sound != shownSound) {
            shownSound = sound;
            kit = kitOf(sound);
        }
        // The kits' tabs and the step through every sound, then the kit's sounds.
        button(cKit, 0, rowX, kitY, 96, 24, kitColour(0));
        button(cKit, 1, rowX + 101, kitY, 96, 24, kitColour(1));
        button(cStep, -1, rowX + rowW - 61, kitY, 28, 24, kLinen);
        button(cStep, 1, rowX + rowW - 28, kitY, 28, 24, kLinen);
        const int first = kit == 0 ? 0 : kStudio;
        const int count = kit == 0 ? kStudio : kModels - kStudio;
        const double each = (rowW - soundGap * (kAcross - 1)) / kAcross;
        for (int k = 0; k < count; ++k)
            button(cSound, first + k, rowX + imod(k, kAcross) * (each + soundGap),
                   soundsY + idiv(k, kAcross) * (soundH + soundGap), each, soundH, kitColour(kit));
        const double r = 19;
        const double step = (selW - 80) / 6.0;
        dial(cKey, selX + 40 + 0 * step, dialsY, r, kLinen);
        dial(cTune, selX + 40 + 1 * step, dialsY, r, kClay);
        dial(cDecay, selX + 40 + 2 * step, dialsY, r, kClay);
        dial(cLevel, selX + 40 + 3 * step, dialsY, r, kTurf);
        dial(cPan, selX + 40 + 4 * step, dialsY, r, kTurf);
        dial(cVelocity, selX + 40 + 5 * step, dialsY, r, kTurf);
        dial(cVary, selX + 40 + 6 * step, dialsY, r, kTurf);
        row(cChoke, 0, 9, rowX, chokeY, 330, 22, kSeaGlass);
        row(cOutput, 0, 9, rowX, outY, 330, 22, kSeaGlass);
        // Every pad's: a small dial in the bar, its name and value beside it.
        dial(cOut, padsX + 34, barY + barH * 0.5, 14, kLinen);
        dialSide[dials - 1] = true;
    }

    int dialAt(double x, double y) const {
        for (int k = 0; k < dials; ++k) {
            const double dx = x - dialX[k], dy = y - dialY[k];
            if (dx * dx + dy * dy <= (dialR[k] + 4) * (dialR[k] + 4)) return k;
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

    // Pad p's cell: four across, pad 1 at the bottom left.
    double cellX(int p) const { return gridX + imod(p, 4) * (cellW + cellGap); }
    double cellY(int p) const { return gridY + (3 - idiv(p, 4)) * (cellH + cellGap); }
    int padAt(double x, double y) const {
        for (int p = 0; p < kPads; ++p)
            if (x >= cellX(p) && x < cellX(p) + cellW && y >= cellY(p) && y < cellY(p) + cellH)
                return p;
        return -1;
    }

    // --- each pad's sound, drawn -----------------------------------------------------------------

    // Pad p's sound as it will play: its recording at the pad's tune and
    // decay, through its 8 bits and (a hi-hat's) VCA, as peaks over
    // kColumns; drawn again only when the sound, tune or decay changes.
    void shape(int p) {
        if (!booked) {
            recipes(book);
            booked = true;
        }
        const int sound = clamp(padChoice(p, cSound), 0, kModels - 1);
        const double tune = padValue(p, cTune);
        const double decay = padValue(p, cDecay);
        const double key = sound + 100.0 * (tune + 13.0) + 10000.0 * decay;
        if (key == shapeKey[p]) return;
        shapeKey[p] = key;
        const Recipe& r = book[sound];
        Recipe flat = r;
        const bool loop = r.loop >= 0;
        // A hi-hat's recording sustains: its VCA, after a hold, decays.
        const double vcaMs = r.decay * decay;
        if (loop) {
            flat.loop = -1;
            flat.start = 0.0;
            flat.length = r.hold + 7.0 * vcaMs;
        }
        Voice s;
        strike(s, flat, sound, loop ? 1 : 0, 0.0, 0, loop ? 1.0 : decay);
        if (loop)
            for (int k = 0; k < kWarm; ++k) {
                record(s, flat);
                s.at += 1;
            }
        const double clock = flat.clock * exp2(tune / 12.0);
        const int n = max(1, s.end);
        shapeMs[p] = n / clock * 1000.0;
        for (int c = 0; c < kColumns; ++c) shapes[p * kColumns + c] = 0.0;
        double top = 1e-9;
        for (int k = 0; k < n; ++k) {
            const double x = record(s, flat);
            double v = abs(converted(x * flat.drive, s.steps));
            s.at += 1;
            if (loop) {
                const double ms = k / flat.clock * 1000.0;
                if (ms > r.hold) v *= exp(-(ms - r.hold) / max(vcaMs, 0.1));
            }
            const int c = min(kColumns - 1, idiv(k * kColumns, n));
            if (v > shapes[p * kColumns + c]) shapes[p * kColumns + c] = v;
            top = max(top, v);
        }
        for (int c = 0; c < kColumns; ++c) shapes[p * kColumns + c] /= top;
    }

    // Pad p's shape over [x, x + w), mirrored about y.
    void drawShape(Canvas& g, int p, double x, double y, double w, double half, int colour,
                   double alpha) {
        g.fillStyle(colour);
        g.globalAlpha(alpha);
        g.beginPath();
        for (int c = 0; c < kColumns; ++c) {
            const double px = x + w * c / (kColumns - 1);
            const double top = y - sqrt(shapes[p * kColumns + c]) * half;
            if (c == 0)
                g.moveTo(px, top);
            else
                g.lineTo(px, top);
        }
        for (int c = kColumns - 1; c >= 0; --c) {
            const double px = x + w * c / (kColumns - 1);
            g.lineTo(px, y + sqrt(shapes[p * kColumns + c]) * half + 0.5);
        }
        g.closePath();
        g.fill();
        g.globalAlpha(1.0);
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
        const double from0 = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 16 ? 5.0 : 4.0);
        g.beginPath();
        g.arc(x, y, r, from0, from0 + sweep);
        g.stroke();
        const double at = from0 + sweep * clamp(readParam(code, 0), 0.0, 1.0);
        // Decay's 100 %, the machine's own, sits at the top: it lights from there too.
        const double from = bipolar(code) || code == cDecay ? from0 + 0.5 * sweep : from0;
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
        g.textBaseline(TextBaseline::Middle);
        if (dialSide[k]) {
            g.textAlign(TextAlign::Left);
            fillDialName(g, code, x + r + 10, y - 7);
            g.fillStyle(hot ? Theme::text : Theme::textMuted);
            g.monoFont(9.5);
            fillParam(g, code, x + r + 10, y + 7);
            return;
        }
        g.textAlign(TextAlign::Center);
        fillDialName(g, code, x, y - r - 9);
        g.fillStyle(hot ? Theme::text : Theme::textMuted);
        g.monoFont(9.5);
        fillParam(g, code, x, y + r + 9);
    }

    void drawButton(Canvas& g, int k) {
        const double x = buttonX[k], y = buttonY[k], w = buttonW[k], h = buttonH[k];
        const bool lit = choiceOf(buttonCode[k]) == buttonValue[k];
        const bool hot = k == hotButton;
        if (buttonCode[k] == cKit) {
            // A tab: the kit shown outlined in its colour, not filled as a
            // choice is, so that it does not read as the pad's sound.
            g.fillStyle(lit ? kInk : (hot ? 0x3A2F27 : Theme::raised));
            g.beginPath();
            g.roundRect(x + 0.5, y + 0.5, w - 1, h - 1, 6);
            g.fill();
            g.strokeStyle(lit ? buttonColour[k] : Theme::line);
            g.lineWidth(lit ? 1.5 : 1.0);
            g.stroke();
            g.fillStyle(lit ? buttonColour[k] : (hot ? Theme::text : Theme::textMuted));
            g.boldFont(9);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            fillKit(g, buttonValue[k], x + w * 0.5, y + h * 0.5 + 0.5);
            return;
        }
        g.fillStyle(lit ? buttonColour[k] : (hot ? 0x3A2F27 : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, min(h * 0.5, 11.0));
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(lit ? kInk : (hot ? Theme::text : Theme::textMuted));
        if (buttonCode[k] == cStep) {
            // A small arrowhead, back or on.
            const double cx = x + w * 0.5, cy = y + h * 0.5, d = buttonValue[k];
            g.beginPath();
            g.moveTo(cx + 3.5 * d, cy);
            g.lineTo(cx - 2.5 * d, cy - 4.5);
            g.lineTo(cx - 2.5 * d, cy + 4.5);
            g.closePath();
            g.fill();
            return;
        }
        g.boldFont(buttonCode[k] == cSound ? 8.5 : 9);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        fillButton(g, buttonCode[k], buttonValue[k], x + w * 0.5, y + h * 0.5 + 0.5);
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

    // A drumlin from above: a long, smooth hill, blunt where the ice came
    // from and drawn out where it went, ringed by its contours; they light
    // while pads sound.
    void drawMark(Canvas& g, double x, double y) {
        double lit = 0.0;
        for (int p = 0; p < kPads; ++p) lit = max(lit, static_cast<double>(glow[p]));
        for (int ring = 0; ring < 4; ++ring) {
            const double s = 1.0 - ring * 0.22;
            g.beginPath();
            g.moveTo(x - 13 * s, y);
            g.bezierCurveTo(x - 13 * s, y - 9 * s, x + 2 * s, y - 8 * s, x + 16 * s, y - 1 * s);
            g.bezierCurveTo(x + 18 * s, y, x + 18 * s, y + 1 * s, x + 16 * s, y + 1.5 * s);
            g.bezierCurveTo(x + 2 * s, y + 8 * s, x - 13 * s, y + 9 * s, x - 13 * s, y);
            g.closePath();
            if (ring == 0) {
                g.linearGradient(x - 13, y - 9, x + 16, y + 9, 0x7F9160, 0x4E5A3A);
                g.fill();
            }
            g.strokeStyle(lit > 0.001 && ring > 0 ? kLinen : (ring == 0 ? 0x3A4229 : kTurf));
            g.globalAlpha(ring == 0 ? 1.0 : 0.55 + 0.4 * lit);
            g.lineWidth(1);
            g.stroke();
            g.globalAlpha(1.0);
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 36, 38);
        g.fillStyle(Theme::text);
        g.serifFont(25);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Drumlin", 64, 45);
        const double x = 72 + g.measureText("Drumlin");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("realistic drums, synthesized, on General MIDI's drum keys", x, 45);
        // The voices playing, then the main output's level, well apart.
        label(g, "VOICES", 876, 26, TextAlign::Left);
        g.fillStyle(Theme::text);
        g.monoFont(13);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%d / %d", soundingNow.get(), kVoices), 876, 43);
        label(g, "MAIN", 984, 26, TextAlign::Left);
        drawLevel(g, 984, 37, 164, outLeft.get(), outRight.get());
    }

    // --- the pads -----------------------------------------------------------------------------

    void drawPad(Canvas& g, int p) {
        const double x = cellX(p), y = cellY(p);
        const bool sel = p == selected;
        const bool hot = p == hotPad || (dragging == 3 && playedPad == p);
        const double lit = glow[p];
        g.linearGradient(0, y, 0, y + cellH, hot ? 0x3A2F27 : Theme::raised, kInk);
        g.beginPath();
        g.roundRect(x, y, cellW, cellH, 7);
        g.fill();
        if (lit > 0.001) {
            g.fillStyle(kTurf);
            g.globalAlpha(0.10 + 0.30 * lit);
            g.beginPath();
            g.roundRect(x, y, cellW, cellH, 7);
            g.fill();
            g.globalAlpha(1.0);
        }
        g.strokeStyle(sel ? kTurf : Theme::line);
        g.lineWidth(sel ? 1.6 : 1.0);
        g.beginPath();
        g.roundRect(x + 0.5, y + 0.5, cellW - 1, cellH - 1, 7);
        g.stroke();
        // Its number and key; its group and output; its sound and machine.
        g.fillStyle(sel ? kTurf : Theme::text);
        g.boldFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%d", p + 1), x + 8, y + 12);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8.5);
        g.fillText(format("%d", padChoice(p, cKey)), x + 26, y + 12);
        fillNote(g, padChoice(p, cKey), x + 46, y + 12);
        g.textAlign(TextAlign::Right);
        g.boldFont(8);
        const int choke = padChoice(p, cChoke), out = padChoice(p, cOutput);
        g.fillStyle(kSeaGlass);
        if (out > 0) g.fillText(format("OUT %d", out), x + cellW - 8, y + 12);
        if (choke > 0) g.fillText(format("CHOKE %d", choke), x + cellW - 8, y + 24);
        const int sound = padChoice(p, cSound);
        shape(p);
        drawShape(g, p, x + 8, y + cellH * 0.5 + 4, cellW - 16, cellH * 0.5 - 30,
                  lit > 0.001 ? kLinen : kitColour(kitOf(sound)), lit > 0.001 ? 0.95 : 0.7);
        g.fillStyle(Theme::text);
        g.boldFont(9);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        fillSound(g, sound, x + 8, y + cellH - 22);
        g.fillStyle(Theme::textFaint);
        g.monoFont(8);
        fillKit(g, kitOf(sound), x + 8, y + cellH - 10);
        const double tune = padValue(p, cTune);
        g.textAlign(TextAlign::Right);
        if (tune != 0) g.fillText(format("%+g st", tune), x + cellW - 8, y + cellH - 10);
    }

    void drawPadsPanel(Canvas& g) {
        panel(g, padsX, padsY, padsW, padsH);
        heading(g, "Pads", padsX + 18, padsY + 25);
        aside(g, "each on its own note; press to play, higher is harder", padsX + padsW - 18,
              padsY + 25, TextAlign::Right);
        for (int p = 0; p < kPads; ++p) drawPad(g, p);
    }

    // --- the selected pad ---------------------------------------------------------------------

    void drawWave(Canvas& g) {
        well(g, wavX, wavY, wavW, wavH);
        const double inX = wavX + 10, inW = wavW - 20;
        shape(selected);
        // A ruler: every 10 ms, or 100 ms for a long sound.
        const double ms = shapeMs[selected];
        const double tick = ms > 400.0 ? 100.0 : ms > 80.0 ? 20.0 : 5.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        for (double t = tick; t < ms; t += tick) {
            const double px = inX + inW * t / ms;
            g.beginPath();
            g.moveTo(px, wavY + wavH - 18);
            g.lineTo(px, wavY + wavH - 12);
            g.stroke();
        }
        drawShape(g, selected, inX, wavY + (wavH - 16) * 0.5 + 2, inW, (wavH - 30) * 0.5,
                  glow[selected] > 0.001 ? kLinen : kitColour(kitOf(choiceOf(cSound))), 0.9);
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.0f ms", ms), wavX + wavW - 10, wavY + wavH - 9);
        g.textAlign(TextAlign::Left);
        g.fillText(format("%g ms a tick", tick), wavX + 10, wavY + wavH - 9);
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
        fillNote(g, padChoice(selected, cKey), selX + 82, selY + 25);
        g.fillStyle(Theme::text);
        g.serifItalicFont(13);
        padText(g, selected, cSound, selX + 130, selY + 25);
        aside(g, "as it plays: tune and decay included", selX + selW - 18, selY + 25,
              TextAlign::Right);
        label(g, "SOUND", selX + 18, kitY + 12, TextAlign::Left);
        drawWave(g);
        label(g, "PLAYS", selX + 18, dialsY - 46, TextAlign::Left);
        label(g, "CHOKE", selX + 18, chokeY + 11, TextAlign::Left);
        label(g, "OUTPUT", selX + 18, outY + 11, TextAlign::Left);
        if (choiceOf(cOutput) > 0)
            aside(g, "out of the main mix", rowX + 342, outY + 11, TextAlign::Left);
        if (choiceOf(cChoke) > 0)
            aside(g, "cuts its group, itself too", rowX + 342, chokeY + 11, TextAlign::Left);
        aside(g, "Decay at 100 % rings as the recording did; a classic hi-hat's is its VCA",
              selX + selW * 0.5, selY + selH - 34, TextAlign::Center);
        aside(g, "Vary makes each hit a little different, as a real drum's are", selX + selW * 0.5,
              selY + selH - 17, TextAlign::Center);
    }

    void drawBar(Canvas& g) {
        panel(g, padsX, barY, kW - 32, barH);
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

    void pointer(const Pointer& p) {
        place();
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hotDial = -1;
            hotButton = -1;
            hotPad = -1;
            return;
        }
        if (a == PointerAction::Move) {
            hotDial = dialAt(p.x(), p.y());
            hotButton = buttonAt(p.x(), p.y());
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
            }
            return;
        }
        if (a == PointerAction::Release) {
            if (dragging == 3) playNote(playedKey, 0.0, false);
            dragging = 0;
            playedKey = -1;
            playedPad = -1;
            hotDial = dialAt(p.x(), p.y());
            hotPad = padAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const int k = dialAt(p.x(), p.y());
            if (k >= 0) {
                const int code = dialCode[k];
                editParam(code, 1, clamp(readParam(code, 0) + p.wheel() * 0.02 * fine, 0.0, 1.0));
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
            editParam(buttonCode[b], 0, buttonValue[b]);
            return;
        }
        const int pad = padAt(x, y);
        if (pad >= 0) {
            selected = pad;
            const double up = 1.0 - clamp((y - cellY(pad)) / cellH, 0.0, 1.0);
            playedKey = padChoice(pad, cKey);
            playedPad = pad;
            playNote(playedKey, 0.35 + 0.65 * up, true);
            dragging = 3;
        }
    }

    // --- Factory kits ---------------------------------------------------------------------
    //
    // Each sets what differs from the default kit (Classic Kit). Pads keep their
    // keys: a kit changes sounds, never the map.

    void factoryPatches(Patches& p) {
        p.add("Classic Kit");
        // Short and dry: the kick and snare cut tight, the hats at the
        // machine's shortest decay, the toms clipped.
        p.add("Classic Tight")
            .set(p1Decay, 60)
            .set(p3Decay, 55)
            .set(p4Decay, 60)
            .set(p5Decay, 50)
            .set(p6Decay, 32)
            .set(p7Decay, 50)
            .set(p8Decay, 50)
            .set(p9Decay, 50)
            .set(p11Decay, 50)
            .set(p14Decay, 60)
            .set(p15Decay, 60);
        // Slow and low: everything tuned down, the hats at a long decay.
        p.add("Classic Low")
            .set(p1Tune, -3)
            .set(p2Tune, -4)
            .set(p3Tune, -4)
            .set(p4Tune, -3)
            .set(p5Tune, -9)
            .set(p6Tune, -5)
            .set(p6Decay, 190)
            .set(p7Tune, -6)
            .set(p8Tune, -5)
            .set(p9Tune, -3)
            .set(p11Tune, 0)
            .set(p13Tune, -4)
            .set(p14Tune, -5)
            .set(p15Tune, -6)
            .set(p16Tune, -4);
        // High and bright: the machine's knobs turned up.
        p.add("Classic High")
            .set(p1Tune, 3)
            .set(p2Tune, 5)
            .set(p3Tune, 5)
            .set(p4Tune, 4)
            .set(p5Tune, -1)
            .set(p6Tune, 7)
            .set(p7Tune, 2)
            .set(p8Tune, 7)
            .set(p9Tune, 5)
            .set(p11Tune, 8)
            .set(p13Tune, 5)
            .set(p14Tune, 6)
            .set(p15Tune, 6)
            .set(p16Tune, 4);
        // Every hit the recording's, note for note: Vary at 0 on every pad.
        p.add("Classic Exact")
            .set(p1Vary, 0)
            .set(p2Vary, 0)
            .set(p3Vary, 0)
            .set(p4Vary, 0)
            .set(p5Vary, 0)
            .set(p6Vary, 0)
            .set(p7Vary, 0)
            .set(p8Vary, 0)
            .set(p9Vary, 0)
            .set(p10Vary, 0)
            .set(p11Vary, 0)
            .set(p12Vary, 0)
            .set(p13Vary, 0)
            .set(p14Vary, 0)
            .set(p15Vary, 0)
            .set(p16Vary, 0);
        // The hand percussion forward: congas on the toms' keys, the
        // cowbell, tambourine and cabasa up, the kick and snare softer.
        p.add("Classic Percussion")
            .set(p1Level, -4)
            .set(p3Level, -5)
            .set(p5Sound, "classic_conga")
            .set(p5Tune, -6)
            .set(p7Sound, "classic_conga")
            .set(p7Tune, -3)
            .set(p9Sound, "classic_conga")
            .set(p9Tune, 0)
            .set(p11Sound, "classic_conga")
            .set(p11Tune, 3)
            .set(p13Level, 0)
            .set(p14Level, -1)
            .set(p16Level, 0);
        // Each kind of drum on an output of its own, for mixing the kit on
        // separate tracks: kick; snare and rim; clap; toms; hats; cymbals;
        // tambourine and cabasa; cowbell and conga.
        p.add("Classic Separate Outs")
            .set(p1Output, "out1")
            .set(p2Output, "out2")
            .set(p3Output, "out2")
            .set(p4Output, "out3")
            .set(p5Output, "out4")
            .set(p6Output, "out5")
            .set(p7Output, "out4")
            .set(p8Output, "out5")
            .set(p9Output, "out4")
            .set(p10Output, "out6")
            .set(p11Output, "out4")
            .set(p12Output, "out6")
            .set(p13Output, "out7")
            .set(p14Output, "out8")
            .set(p15Output, "out8")
            .set(p16Output, "out7");
        // The studio sounds wherever the studio kit has the drum: the classic
        // cymbals, tambourine and cabasa stay, the block on the conga's key.
        p.add("Studio Kit")
            .set(p1Sound, "studio_kick")
            .set(p2Sound, "studio_cross_stick")
            .set(p3Sound, "studio_snare")
            .set(p4Sound, "studio_clap")
            .set(p5Sound, "studio_tom")
            .set(p6Sound, "studio_closed_hat")
            .set(p7Sound, "studio_tom")
            .set(p8Sound, "studio_open_hat")
            .set(p9Sound, "studio_tom")
            .set(p11Sound, "studio_tom")
            .set(p14Sound, "studio_open_cowbell")
            .set(p15Sound, "studio_block");
        // The studio kit's percussion: its four cowbells on the toms' keys,
        // the block, the two ducks and the explosion on the top row.
        p.add("Studio Percussion")
            .set(p1Sound, "studio_kick")
            .set(p2Sound, "studio_cross_stick")
            .set(p3Sound, "studio_snare")
            .set(p4Sound, "studio_clap")
            .set(p5Sound, "studio_muted_cowbell")
            .set(p5Tune, 0)
            .set(p6Sound, "studio_closed_hat")
            .set(p7Sound, "studio_cowbell")
            .set(p7Tune, 0)
            .set(p8Sound, "studio_open_hat")
            .set(p9Sound, "studio_dark_cowbell")
            .set(p9Tune, 0)
            .set(p11Sound, "studio_open_cowbell")
            .set(p11Tune, 0)
            .set(p13Sound, "studio_block")
            .set(p14Sound, "studio_short_duck")
            .set(p15Sound, "studio_long_duck")
            .set(p16Sound, "studio_explosion");
    }
};
