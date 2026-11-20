// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Umber: a tape machine, from the faint rounding of peaks that a well-kept
// modern machine gives a mix to a worn, underbiased, warbling cassette.
//
// Umber is an earth pigment, iron oxide with a little manganese: the same rust
// that coats a tape. No particular machine is copied. Each part of the path
// is modelled where it changes the sound, and fitted to published tape data
// (third-harmonic distortion against level, saturation output at 10 and
// 16 kHz, noise and print figures of a current studio tape at 30, 15 and
// 7.5 ips):
//
//   Record   the drive sets how hard the signal meets the tape: at 0 dB a
//            sine at -18 dBFS RMS records at the tape's reference fluxivity.
//            A class-A record amplifier (mostly second harmonic), the
//            machine's HF trim, then the record equalisation: the
//            standard's curve (NAB, IEC or AES) with the lift that makes up
//            the tape's losses at short wavelengths, fitted so the highs
//            saturate where the datasheet says they do.
//   Tape     the oxide's magnetization: the particles are single-domain
//            needles with two states, so the anhysteretic curve is a tanh of
//            the field, sharpened by the tape's squareness. Bias decides how
//            much hysteresis is left: at the recommended bias the
//            irreversible magnetization follows that curve almost at once
//            (clean at low levels, about 2 dB more third harmonic per dB of
//            level, rounding only near the tape's limit); underbias pins it,
//            and the loop opens into the grain of unbiased tape. A
//            magnetized head adds a steady field, and even harmonics.
//   Playback the inverse of the record equalisation, the playback head's
//            contour effect (a bump in the lows whose frequency follows the
//            speed, the dip and ripple above it), its gap and the aligned top
//            end, bias's tilt of the highs, azimuth (highs lost within each
//            track, and the two tracks apart in time), the machine's HF trim
//            and a class-A playback amplifier; dropouts, print-through from
//            the next layer of the reel, crosstalk between the tracks.
//   Motion   wow from the reels' turning and an aperiodic wander, flutter
//            from the capstan, an idler and random friction, each turning
//            part's depth drifting, and scrape: the tape's own vibration over
//            the heads, a jitter in the kilohertz that roughens the highs. All
//            as a moving delay read at the oversampled rate.
//   Noise    hiss as the tape's particles give it through the playback
//            equalisation, modulation noise riding on the signal, and the
//            mains' hum in the playback electronics.
//
// The record path, the tape and the moving read run oversampled, 1x to 16x
// as set: with linear-phase filters and room for deep wow (about 8.5 ms of
// latency), or for tracking with minimum-phase filters, 9 samples in all,
// the wow held within the few samples there are.

constexpr double kRef = 0.17782794100389229;  // a sine at -18 dBFS RMS: its peak
constexpr double kRefRms = 0.12574334296829354;
constexpr int kScope = 512;  // the scope's trail: field and flux at about 8 kHz
constexpr int kTrail = 256;  // the transport's trace: 64 points a second
constexpr int kTick = 32;    // frames between the slow parts' updates

// Gains often hold still for many frames. Cache only exact repeats, so a
// glide still gets every distinct gain it asks for.
struct GainCache {
    double at = 1e100, value = 1.0;
    double get(double db) {
        if (db != at) {
            at = db;
            value = dbToGain(db);
        }
        return value;
    }
    double offAt(double db, double minimum) {
        if (db <= minimum) return 0.0;
        if (db != at) {
            at = db;
            // Keep Param::gain()'s arithmetic for unchanged patches.
            value = db == 0.0 ? 1.0 : pow(10.0, db / 20.0);
        }
        return value;
    }
};

// A first-order section, g (1 + s/wa) / (1 + s/wb), by the bilinear transform
// with both corners prewarped: biproper, so its inverse is one too.
struct Section {
    double b0 = 1.0, b1 = 0.0, a1 = 0.0, z = 0.0;

    void design(double fa, double fb, double g, double fs) {
        const double top = 0.49 * fs;
        const double a = 1.0 / tan(pi * min(fa, top) / fs);
        const double b = 1.0 / tan(pi * min(fb, top) / fs);
        b0 = g * (1.0 + a) / (1.0 + b);
        b1 = g * (1.0 - a) / (1.0 + b);
        a1 = (1.0 - b) / (1.0 + b);
    }
    double process(double x) {
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }
    // Its gain at frequency f, for the editor and the noise's calibration.
    double gainAt(double f, double fs) const {
        const double w = twoPi * f / fs;
        const double nr = b0 + b1 * cos(w), ni = -b1 * sin(w);
        const double dr = 1.0 + a1 * cos(w), di = -a1 * sin(w);
        return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    void clear() { z = 0.0; }
};

// A first-order shelf of `db` in the highs, centred (geometrically) on hz.
// The machine's HF trims centre on 4 kHz per 15 ips: each speed's
// equalisation has its own, so their reach follows the wavelength.
void shelf(Section& s, double db, double hz, double fs) {
    const double r = sqrt(dbToGain(db));
    s.design(hz / r, hz * r, 1.0, fs);
}

// --- the tape -----------------------------------------------------------------------------------

// A tape by its headroom: the level over reference at which it gives 3%
// third harmonic at 1 kHz (its MOL). Three are fitted to datasheets, a
// modern high-output tape (12.5 dB), a classic (9) and an early ferric one
// (5); between them the constants follow the line through the three:
// squareness (sharpens the knee), the field at reference level, the pinning
// left at the recommended bias, A-weighted hiss at 15 ips and modulation
// noise against the reference level, and how much shorter wavelengths cost.
struct Formulation {
    double square = 0.24, field = 0.1978, pin = 1e-5;
    double hissDb = -60.5, modDb = -66.0, loss = 1.0;
    void set(double molDb) {
        const double mol = clamp(molDb, 5.0, 13.0);
        const bool upper = mol > 9.0;
        const double u = upper ? (mol - 9.0) / 3.5 : (mol - 5.0) / 4.0;
        square = upper ? lerp(0.16, 0.24, u) : lerp(0.08, 0.16, u);
        field = exp(upper ? lerp(log(0.2969), log(0.1978), u) : lerp(log(0.4440), log(0.2969), u));
        pin = exp(upper ? lerp(log(3e-5), log(1e-5), u) : lerp(log(1e-4), log(3e-5), u));
        hissDb = upper ? lerp(-57.0, -60.5, u) : lerp(-52.0, -57.0, u);
        modDb = upper ? lerp(-61.0, -66.0, u) : lerp(-55.0, -61.0, u);
        loss = upper ? lerp(1.1, 1.0, u) : lerp(1.3, 1.1, u);
    }
};

double speedIps(int s) { return s == 0 ? 30.0 : (s == 1 ? 15.0 : (s == 2 ? 7.5 : 3.75)); }
// How much harder each speed records a mid frequency for the same level:
// the tape's limit at 1 kHz falls as the wavelength shortens (the datasheet's
// MOL at 30, 15 and 7.5 ips; 3.75 extrapolated).
double speedFieldDb(int s) { return s == 0 ? -0.7 : (s == 1 ? -0.2 : (s == 2 ? 2.75 : 5.0)); }
// The time constant of each speed's standard, in seconds: 30 ips is AES's
// 17.5 us either way, and NAB adds a 3180 us lift of the lows at 15 ips and below.
double speedTau(int s, bool nab) {
    if (s == 0) return 17.5e-6;
    if (s == 1) return nab ? 50e-6 : 35e-6;
    if (s == 2) return nab ? 50e-6 : 70e-6;
    return 90e-6;
}
// The tape's limit at short wavelengths: the recorded flux's own
// demagnetizing field caps what the highs can carry, so they saturate on
// their own, where the mids are not. Above a split at 5 kHz per 15 ips the
// highs meet their own curve, lifted from 9 kHz per 15 ips (so 16 kHz
// saturates before 10), with this ceiling, in reference peaks: fitted so a
// modern tape's saturation output at 10 and 16 kHz (12.5 at 7.5 ips) is the
// datasheet's, with the IEC curve; 3.75 ips carries on by the wavelength.
double speedEdge(int s) { return s == 0 ? 1.6 : (s == 1 ? 3.4 : (s == 2 ? 1.32 : 0.54)); }
// Hiss against the reference level, A-weighted, for a modern tape at each
// speed with the IEC curve.
double speedHissDb(int s) { return s == 0 ? -63.0 : (s == 1 ? -60.5 : (s == 2 ? -58.0 : -55.0)); }
// How far the highs move as bias moves from peak to the recommended setting
// (the datasheet's delta S10), and the head bump's height.
double speedBiasTilt(int s) { return s == 0 ? 1.5 : (s == 1 ? 3.0 : (s == 2 ? 6.5 : 9.0)); }
double speedBumpDb(int s) { return s == 0 ? 1.5 : (s == 1 ? 2.0 : (s == 2 ? 2.5 : 2.75)); }
// The top end of an aligned machine at low level.
double speedTopHz(int s) {
    return s == 0 ? 30000.0 : (s == 1 ? 24000.0 : (s == 2 ? 17000.0 : 10500.0));
}

// The anhysteretic curve: a tanh of the field, squared off by squareness.
// Below 0.3 its series to the ninth power is as exact (within 2e-8) and
// cheaper.
double anhysteretic(double u, double sq) {
    const double y = u + sq * u * u * u;
    if (abs(y) >= 0.3) return tanh(y);
    const double y2 = y * y;
    return y * (1.0 +
                y2 * (-1.0 / 3.0 + y2 * (2.0 / 15.0 + y2 * (-17.0 / 315.0 + y2 * 62.0 / 2835.0))));
}

// The oversampling choice's factor: 1, 2, 4, 8, 16.
int timesOf(int index) {
    return index <= 0 ? 1 : (index == 1 ? 2 : (index == 2 ? 4 : (index == 3 ? 8 : 16)));
}

// A sine turned by rotation each frame (a pair of numbers, kept on the unit
// circle every kTick frames), for the transport's steady parts and the hum.
struct Phasor {
    double c = 1.0, s = 0.0, dc = 1.0, ds = 0.0;
    void start(double cycles) {
        c = cos(twoPi * cycles);
        s = sin(twoPi * cycles);
    }
    void rate(double cyclesPerFrame) {
        dc = cos(twoPi * cyclesPerFrame);
        ds = sin(twoPi * cyclesPerFrame);
    }
    void next() {
        const double nc = c * dc - s * ds;
        s = s * dc + c * ds;
        c = nc;
    }
    void hold() {
        const double g = 1.0 / sqrt(c * c + s * s);
        c *= g;
        s *= g;
    }
};

// Hum's harmonics of the mains, 1 to 16: the head's magnetic pickup on the
// odd ones (fundamental, then falling fast), the supply's ripple on the
// even ones (twice the mains, then a slowly falling buzz), unevenly.
double humWeight(int n) {
    if (n == 1) return 0.6;
    if (n == 3) return 0.21;
    if (n == 5) return 0.09;
    if (n == 7) return 0.036;
    if (n == 2) return 1.0;
    if (n == 4) return 0.45;
    if (n == 6) return 0.30;
    if (n == 8) return 0.22;
    if (n == 10) return 0.14;
    if (n == 12) return 0.11;
    if (n == 14) return 0.07;
    if (n == 16) return 0.05;
    return 0.0;
}

// The oxide of one track: irreversible magnetization pinned until the field
// pushes it on, toward the anhysteretic value (an exact exponential step in
// the field, so a narrow loop never goes unstable), and a reversible part
// that follows the field at once.
struct Oxide {
    double h = 0.0, mirr = 0.0;
    void clear() {
        h = 0.0;
        mirr = 0.0;
    }
    double step(double hn, double sq, double pin) {
        const double dh = hn - h;
        h = hn;
        const double man = anhysteretic(hn, sq);
        const double diff = man - mirr;
        if (dh * diff > 0.0) {
            const double t = abs(dh) / pin;
            mirr = t > 20.0 ? man : man - diff * exp(-t);
        }
        return mirr + 0.3 * (man - mirr);
    }
};

// One of the machine's amplifiers: a class-A stage, mostly second harmonic,
// as a tanh about an offset operating point with unit gain for small
// signals; the offset's even harmonics leave a little DC, which the stage's
// coupling (2 Hz) takes out. 1 in is the reference level's peak.
struct Amp {
    double a = 0.0, offset = 0.3, t0 = 0.0, inv = 1.0, keep = 0.9999, x1 = 0.0, y1 = 0.0;
    void set(double amount, double at, double fs) {
        // About -80 dBc of second harmonic at the reference level at 15%,
        // -47 at 50% and -30 at 100%.
        a = 0.2 * amount * amount * amount;
        offset = at;
        t0 = tanh(at);
        inv = a > 1e-12 ? 1.0 / (a * (1.0 - t0 * t0)) : 1.0;
        keep = exp(-twoPi * 2.0 / fs);
    }
    double process(double x) {
        if (a <= 1e-12) {
            y1 = x - x1 + keep * y1;
            x1 = x;
            return x;
        }
        const double y = (tanh(a * x + offset) - t0) * inv;
        const double out = y - x1 + keep * y1;
        x1 = y;
        y1 = out;
        return out;
    }
    void clear() {
        x1 = 0.0;
        y1 = 0.0;
    }
};

// The playback head's filters as the settings put them, for the audio and
// for the editor's copy.
struct Head {
    Biquad low, bump, dip, ripple, dip2, top, tilt;
    Section repro;
    bool topOn = false;
    void design(int s, double loss, double amount, double bumpHz, double tiltDb, double reproDb,
                double rate) {
        const double v = speedIps(s) / 15.0;
        const double fb = bumpHz * v;
        const double bumpDb = speedBumpDb(s) * amount;
        const double topHz = speedTopHz(s) / loss;
        // A corner past 0.45 of the rate would only droop the top octave:
        // there the machine is flat, so the filters stay out (they still
        // run, at that edge, so they come in from a live state).
        topOn = topHz < 0.45 * rate;
        low.highpass(max(5.0, 0.36 * fb * min(1.0, amount)), 0.75);
        bump.peak(fb, 1.2, 1.07 * bumpDb);
        // The interference's undulation, every bump frequency on: a dip of
        // about 1 dB at twice it, a ripple at three times, a fainter dip at
        // four, each smaller as the core ends stop contributing.
        dip.peak(2.0 * fb, 1.8, -0.8 * bumpDb);
        ripple.peak(min(3.0 * fb, 0.4 * rate), 3.4, 0.38 * bumpDb);
        dip2.peak(min(4.0 * fb, 0.4 * rate), 3.5, -0.15 * bumpDb);
        top.lowpass(min(topHz, 0.45 * rate), 0.75);
        tilt.highShelf(min(5000.0 * v, 0.4 * rate), 0.6, tiltDb);
        shelf(repro, reproDb, min(4000.0 * v, 0.3 * rate), rate);
    }
    double process(double x) {
        double y = repro.process(tilt.process(x));
        const double topped = top.process(y);
        if (topOn) y = topped;
        return dip2.process(ripple.process(dip.process(bump.process(low.process(y)))));
    }
    double responseDb(double hz, double rate) const {
        double db = low.responseDb(hz) + bump.responseDb(hz) + dip.responseDb(hz) +
                    ripple.responseDb(hz) + dip2.responseDb(hz) + tilt.responseDb(hz) +
                    gainToDb(repro.gainAt(hz, rate));
        if (topOn) db += top.responseDb(hz);
        return db;
    }
    void clear() {
        low.clear();
        bump.clear();
        dip.clear();
        ripple.clear();
        dip2.clear();
        top.clear();
        tilt.clear();
        repro.clear();
    }
};

// Bias's tilt of the highs, in dB: up toward the peak of sensitivity below
// the recommended bias, then down again; down above it.
double biasTiltDb(int s, double b) {
    const double dS = speedBiasTilt(s);
    if (b < 1.0) return b >= 0.7 ? dS * (1.0 - b) / 0.3 : dS * max(0.0, (b - 0.4) / 0.3);
    return -dS * (b - 1.0) / 0.3;
}

// Every frequency's level against bias: far under the recommended bias it
// falls; over it, a little.
double biasLevelDb(double b) {
    if (b < 0.7) return -6.0 * (0.7 - b) * (0.7 - b) / 0.04;
    if (b > 1.0) return -0.8 * (b - 1.0) / 0.5;
    return 0.0;
}

// Azimuth: the playback gap tilted by an angle crosses each 2 mm track at a
// slant, so it hears the track's signal spread over the time the tape takes
// to move that slant: an average over a window, whose response is a sinc
// with true nulls. The same tilt reaches the two tracks' centres, 4 mm apart,
// at different times. In seconds, at a speed:
double azimuthWindow(double degrees, double atIps) {
    return 0.002 * tan(degrees * pi / 180.0) / (atIps * 0.0254);
}
double azimuthSpread(double degrees, double atIps) {
    return 0.004 * tan(degrees * pi / 180.0) / (atIps * 0.0254);
}
// Both tracks' reads must fit the moving delay, in the audio and its plot.
void capAzimuth(double& spread, double& window, double reach) {
    const double extent = spread + 0.5 * window, room = 0.6 * reach;
    if (extent > room) {
        spread *= room / extent;
        window *= room / extent;
    }
}
// The window's loss at hz, for the editor's response.
double azimuthDb(double hz, double window) {
    const double x = pi * hz * window;
    if (x < 1e-9) return 0.0;
    return gainToDb(max(abs(sin(x) / x), 1e-3));
}
constexpr int kTaps = 8;  // reads across the azimuth window

// The record equalisation's lows: NAB's lift below 50 Hz (3180 us, held to
// +10 dB), which playback undoes; IEC and AES leave the lows flat. Their
// treatment of the highs is the edge's (speedEdge, Edge).
struct Emphasis {
    Section low;
    void design(bool nab, double hi, bool inverse) {
        const double eps = 0.316;
        const double fLow = 1.0 / (twoPi * 3180e-6);
        if (!nab)
            low.design(fLow, fLow, 1.0, hi);
        else if (inverse)
            low.design(fLow * eps, fLow, eps, hi);
        else
            low.design(fLow, fLow * eps, 1.0 / eps, hi);
    }
    double process(double x) { return low.process(x); }
    void clear() { low.clear(); }
    double gainAt(double f, double hi) const { return low.gainAt(f, hi); }
};

// The highs' ceiling, in reference peaks: the speed's, higher with NAB (its
// curve records the highs lower, by the two curves' ratio at 10 kHz), lower
// on a lossier tape, and HF Saturation's trim (at 200%, half: 6 dB less
// headroom in the highs).
double edgeFor(int s, bool nab, double loss, double hfSat) {
    const double w = twoPi * 10000.0;
    const double iec = speedTau(s, false), std = speedTau(s, nab);
    const double ratio = sqrt((1.0 + w * w * std * std) / (1.0 + w * w * iec * iec));
    return speedEdge(s) * ratio / (loss * loss) * pow(2.0, 1.0 - hfSat);
}

// The highs' own saturation. The flux splits into lows and highs, each
// through two one-poles (bilinear, so a low and a high sum to exactly the
// signal); the highs, lifted, meet a tanh at the ceiling and are lowered
// again; and the lows less the highs make the flux again: for a quiet
// signal an allpass, so the level is untouched at every frequency.
struct Edge {
    double g = 0.1, l1 = 0.0, l2 = 0.0, h1 = 0.0;
    Section lift, drop;
    void design(double splitHz, double liftHz, double fs) {
        const double w = tan(pi * min(splitHz, 0.45 * fs) / fs);
        g = w / (1.0 + w);
        lift.design(min(liftHz, 0.3 * fs), 0.45 * fs, 1.0, fs);
        drop.design(0.45 * fs, min(liftHz, 0.3 * fs), 1.0, fs);
    }
    double process(double x, double ceiling) {
        // Two low-passes, and two high-passes (each the input less its low).
        const double v1 = (x - l1) * g;
        const double lo1 = v1 + l1;
        l1 = lo1 + v1;
        const double v2 = (lo1 - l2) * g;
        const double lo2 = v2 + l2;
        l2 = lo2 + v2;
        const double hi1 = x - lo1;
        const double v3 = (hi1 - h1) * g;
        const double lo3 = v3 + h1;
        h1 = lo3 + v3;
        const double hi2 = hi1 - lo3;
        const double u = lift.process(hi2) / ceiling;
        const double held =
            abs(u) < 0.4 ? u * (1.0 - u * u * (1.0 / 3.0 - u * u * (2.0 / 15.0))) : tanh(u);
        return lo2 - drop.process(held * ceiling);
    }
    void clear() {
        l1 = 0.0;
        l2 = 0.0;
        h1 = 0.0;
        lift.clear();
        drop.clear();
    }
};

// A-weighting's gain at f (IEC 61672).
double aWeight(double f) {
    const double f2 = f * f;
    const double num = 12194.0 * 12194.0 * f2 * f2;
    const double den = (f2 + 20.6 * 20.6) * sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) *
                       (f2 + 12194.0 * 12194.0);
    return num / den * 1.2589254117941673;  // +2.0 dB: 0 dB at 1 kHz
}

// What one track's record path, tape and playback hold.
struct Track {
    Oversampler os;
    Amp recAmp, reproAmp;
    Section recTrim;     // the record side's HF trim
    Emphasis rec, play;  // NAB's lows, and playback's undoing of them
    Edge edge;           // the tape's limit at short wavelengths
    Oxide oxide;
    Buffer<double> ring;  // the moving read, at the oversampled rate
    int at = 0;
    Section hissTau, hissLow, hissTop;  // the hiss through the playback equalisation
    Section rumbleTau, rumbleLow;       // the electronics' noise through it
    Biquad rumbleCut;                   // and the deck's limit below it
    Head head;
    double modZ1 = 0.0, modZ2 = 0.0;  // modulation noise, low-passed
    double drop1 = 0.0, drop2 = 0.0;  // a dropout's spacing loss, two poles
    double crossLow = 0.0;            // what leaks to the other track: the lows
    Buffer<double> print;             // the reel's next layer, decimated
    int printAt = 0, printPhase = 0;
    Biquad printIn, printOut;
    double vu1 = 0.0, vu2 = 0.0;  // the VU meter's two poles
    void clear() {
        os.clear();
        recAmp.clear();
        reproAmp.clear();
        recTrim.clear();
        edge.clear();
        rec.clear();
        play.clear();
        oxide.clear();
        for (int i = 0; i < ring.size(); ++i) ring[i] = 0.0;
        at = 0;
        hissTau.clear();
        hissLow.clear();
        hissTop.clear();
        rumbleTau.clear();
        rumbleLow.clear();
        rumbleCut.clear();
        head.clear();
        modZ1 = 0.0;
        modZ2 = 0.0;
        drop1 = 0.0;
        drop2 = 0.0;
        crossLow = 0.0;
        for (int i = 0; i < print.size(); ++i) print[i] = 0.0;
        printAt = 0;
        printPhase = 0;
        printIn.clear();
        printOut.clear();
        vu1 = 0.0;
        vu2 = 0.0;
    }
};

// The palette: the house's earth, with umber and its oxides.
constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;
constexpr int kGraphBottom = 0x130F0C;
constexpr int kGrid = 0x2B231D;
constexpr int kGridLit = 0x3E332A;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kUmber = 0xC08A55;  // burnt umber, lit
constexpr int kOxide = 0x7A4A2E;  // the tape's coat
constexpr int kRust = 0xC4673F;
constexpr int kCream = 0xEADFC8;  // the meters' faces
constexpr int kMoss = 0xA9B86C;
constexpr int kTrace = 0xE3AE6E;  // the scope's beam

struct Umber : Effect {
    static constexpr Info info = {
        .name = "Umber",
        .id = "com.suppleaudio.umber",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A tape machine modelled from the oxide up: class-A electronics, record "
            "equalisation, a magnetic core whose hysteresis follows the bias, the playback "
            "head's bump and losses, wow, flutter and scrape, dropouts, print-through, "
            "crosstalk, hiss and hum, at four speeds, from a faint rounding of peaks to a worn, "
            "warbling cassette.",
        .features = {"distortion", "tape"}};

    // --- record -----------------------------------------------------------------------------

    Param drive = decibels("Drive", -24, 24, 0)
                      .modulated()
                      .smoothed(30)
                      .in("Record")
                      .role({"level"})
                      .help(
                          "How hard the signal meets the tape: at 0 dB a sine at -18 dBFS RMS "
                          "records at the tape's reference level.");
    Param tape = decibels("Tape", 5, 13, 12.5)
                     .modulated()
                     .in("Record")
                     .help(
                         "The tape's headroom: how far over reference level it reaches 3% "
                         "distortion. 12.5 dB is a modern high-output tape, 9 a classic, 5 an "
                         "early one, each lower softer and noisier.");
    Param bias = percent("Bias", 100, 50, 150)
                     .modulated()
                     .in("Record")
                     .help(
                         "The bias against the recommended setting: under it, brighter and "
                         "grittier, distorting at low levels too; over it, darker and cleaner.");
    Param recordAmp = percent("Record Amp", 15)
                          .modulated()
                          .in("Record")
                          .help(
                              "How hard the class-A record amplifier works: second harmonic, "
                              "about -80 dB at reference level at 15%, -30 dB at 100%.");
    Param hfSaturation = percent("HF Saturation", 100, 0, 200)
                             .modulated()
                             .in("Record")
                             .help(
                                 "How soon the highs saturate against the mids: 100% is the "
                                 "tape's own; up to 6 dB less headroom in the highs at 200%, "
                                 "6 dB more at 0%. The response stays flat.");
    Param recordHf = decibels("Record HF", -6, 6, 0)
                         .modulated()
                         .in("Record")
                         .help(
                             "The record side's HF trim (a shelf at 4 kHz at 15 ips, following the "
                             "speed): up writes the highs harder, so they saturate sooner and come "
                             "back brighter.");
    Param autoGain = toggle("Auto Gain", true)
                         .in("Record")
                         .help(
                             "Turns the playback down as the drive turns the recording up, so "
                             "the level holds while the tape works harder.");

    // --- the machine -------------------------------------------------------------------------

    Param speed = choice("Speed", {"30 ips", "15 ips", "7.5 ips", "3.75 ips"}, 1)
                      .keys({"ips30", "ips15", "ips7_5", "ips3_75"})
                      .in("Machine")
                      .help(
                          "Tape speed: slower lowers the head bump, saturates the highs "
                          "sooner, loses more top, hisses more and wavers more.");
    Param emphasis = choice("Emphasis", {"NAB", "IEC"}, 1)
                         .keys({"nab", "iec"})
                         .in("Machine")
                         .activeUnless(speed, {"ips30"})
                         .help(
                             "The equalisation standard: NAB lifts the lows into the tape (they "
                             "saturate sooner) and trades a little hiss for headroom in the "
                             "highs; 30 ips is AES.");

    // --- heads -------------------------------------------------------------------------------

    Param headBump = percent("Head Bump", 100, 0, 200)
                         .modulated()
                         .in("Heads")
                         .help(
                             "The playback head's swell in the lows (and the dip and ripple "
                             "above it), where the speed puts it: 100% as a real head gives it.");
    Param bumpFreq = frequency("Bump Freq", 30, 120, 57)
                         .modulated()
                         .in("Heads")
                         .activeAbove(headBump, 0)
                         .help(
                             "Where the head bump peaks at 15 ips (the head's length decides "
                             "it); it doubles at 30 ips and halves at 7.5.");
    Param azimuth = degrees("Azimuth", 0, 1, 0)
                        .modulated()
                        .curved(Curve::Pow, 2)
                        .in("Heads")
                        .help(
                            "The playback head's tilt against the record head's, in degrees: "
                            "highs lost within each track (a sinc, with nulls) and the two "
                            "tracks apart in time. 0.02 is a well-aligned machine.");
    Param headMagnetism = percent("Head Magnetism", 0, -100, 100)
                              .modulated()
                              .in("Heads")
                              .help(
                                  "A magnetized record head: a steady field that puts even "
                                  "harmonics into the tape itself (about -40 dB at reference level "
                                  "at 100%) and DC noise, the oxide's grain on that field, with "
                                  "the modulation noise; the sign is its polarity.");

    // --- the tape's condition ----------------------------------------------------------------

    Param dropouts = percent("Dropouts", 0)
                         .modulated()
                         .curved(Curve::Pow, 2)
                         .in("Tape")
                         .help(
                             "How often the oxide loses touch with the head: at 100% about "
                             "four a second.");
    Param dropDepth = percent("Drop Depth", 30)
                          .modulated()
                          .in("Tape")
                          .activeAbove(dropouts, 0)
                          .help(
                              "How deep each dropout goes: the highs first (spacing loss), then "
                              "the level.");
    Param printThrough = decibels("Print-Through", -80, -30, -80)
                             .modulated()
                             .smoothed(20)
                             .offAtMin()
                             .in("Tape")
                             .help(
                                 "The echo of the next layer of the reel, one turn later (1.4 s "
                                 "at 15 ips): -56 dB is a modern tape stored a day; off at the "
                                 "minimum.");
    Param crosstalk = decibels("Crosstalk", -80, -20, -80)
                          .modulated()
                          .smoothed(20)
                          .offAtMin()
                          .in("Tape")
                          .help(
                              "Leakage between the two tracks, mostly the lows: -55 dB is a "
                              "two-track machine; off at the minimum.");

    // --- transport ---------------------------------------------------------------------------

    Param wow = percent("Wow", 0.02, 0, 2)
                    .modulated()
                    .curved(Curve::Pow, 3)
                    .in("Transport")
                    .help(
                        "Slow pitch drift from the reels and the tape's wander, as a peak speed "
                        "error at 15 ips (more at slower speeds, less at 30): 0.02% is a good "
                        "studio machine.");
    Param flutter = percent("Flutter", 0.02, 0, 1)
                        .modulated()
                        .curved(Curve::Pow, 3)
                        .in("Transport")
                        .help(
                            "Faster pitch waver from the capstan, an idler and friction, as a "
                            "peak speed error at 15 ips (more at slower speeds, less at 30).");
    Param scrape = percent("Scrape", 10)
                       .modulated()
                       .curved(Curve::Pow, 2)
                       .in("Transport")
                       .help(
                           "The tape's vibration over the heads and guides: a jitter in the "
                           "kilohertz that roughens the highs; 10% is a good machine, 100% a "
                           "badly worn, dirty transport.");

    // --- playback ----------------------------------------------------------------------------

    Param reproAmp = percent("Repro Amp", 15)
                         .modulated()
                         .in("Playback")
                         .help(
                             "How hard the class-A playback amplifier works: second harmonic, "
                             "about -80 dB at reference level at 15%.");
    Param reproHf = decibels("Repro HF", -6, 6, 0)
                        .modulated()
                        .in("Playback")
                        .help(
                            "The playback side's HF trim (a shelf at 4 kHz at 15 ips, following "
                            "the speed): brighter or darker playback, the tape's saturation "
                            "unchanged.");
    Param hiss = decibels("Hiss", -30, 12, -30)
                     .modulated()
                     .smoothed(20)
                     .offAtMin()
                     .in("Playback")
                     .help(
                         "The machine's noise floor: the tape's hiss against what this tape at "
                         "this speed gives, and the playback electronics' low rumble beneath "
                         "it. 0 dB is real; off at the minimum.");
    Param modNoise = decibels("Mod Noise", -30, 12, 0)
                         .modulated()
                         .smoothed(20)
                         .offAtMin()
                         .in("Playback")
                         .help(
                             "Modulation noise, the grain that rides on the signal from the "
                             "oxide's roughness, against this tape's: 0 dB is real; off at the "
                             "minimum.");
    Param hum = decibels("Hum", -100, -40, -100)
                    .modulated()
                    .smoothed(20)
                    .offAtMin()
                    .in("Playback")
                    .help(
                        "The mains' hum, against the reference level: the power supply's "
                        "ripple (a buzz at twice the mains) and the playback head's pickup "
                        "(the mains itself, less with NAB); -85 dB a well-earthed machine; off "
                        "at the minimum.");
    Param mains = choice("Mains", {"50 Hz", "60 Hz"}, 1)
                      .keys({"hz50", "hz60"})
                      .in("Playback")
                      .activeAbove(hum, -100)
                      .help(
                          "The mains' frequency: the hum is its harmonics, the power "
                          "supply's ripple at twice it and the head's pickup at it.");

    // --- output -----------------------------------------------------------------------------

    Param output =
        decibels("Output", -24, 24, 0).modulated().smoothed(20).in("Output").help("The level out.");
    Param oversampling = choice("Oversampling", {"1x", "2x", "4x", "8x", "16x"}, 2)
                             .keys({"x1", "x2", "x4", "x8", "x16"})
                             .restarts()
                             .in("Output")
                             .help(
                                 "How many times the session rate the tape runs at: best "
                                 "between 176 and 384 kHz (4x or 8x at 44.1 and 48 kHz, 2x or "
                                 "4x at 88.2 and 96). Lower aliases on loud highs.");
    Param latency = choice("Latency", {"Full", "Low"}, 0)
                        .keys({"full", "low"})
                        .restarts()
                        .in("Output")
                        .help(
                            "Full: linear-phase oversampling and room for deep wow (about "
                            "8.5 ms). Low: 9 samples for tracking, wow held within them.");

    // --- readouts ---------------------------------------------------------------------------

    Param vu = decibels("VU", -40, 12, -40)
                   .meter()
                   .help(
                       "The record level against the tape's reference, as a VU meter reads "
                       "it (0 VU is the reference level).");
    Param headroom = decibels("Headroom", -36, 48, 48)
                         .meter()
                         .help(
                             "How far the loudest recent peak stayed below the tape's 3% "
                             "limit (the midband's: the highs have their own); negative is over.");
    Param saturation =
        decibels("Saturation", 0, 24, 0)
            .meter()
            .help("How much the tape rounded the loudest recent peaks, in dB of level lost.");

    // --- the audio code's -----------------------------------------------------------------------

    double rate = 48000.0, hi = 192000.0;
    int factor = 4;
    int center = 0;      // the moving read's resting delay, in base samples
    double reach = 0.0;  // how far it may move either way, in seconds
    int lag = 0;         // the latency, in base samples: the scope's field waits as long
    bool unplayed = true;

    Array<Track, 2> track;
    Random random;  // the transport's
    Random noise;   // the tape's noise
    Formulation oxideNow;
    // The speed and standard as last set up, and a switch of either: 1 while
    // the output fades out, 2 while the input fades back in.
    int speedNow = 1, nabNow = 0;
    int switching = 0;
    double fade = 1.0, fadeStep = 0.005;
    // The settings that glide (30 ms), their values now.
    double tapeNow = 12.5, biasNow = 1.0, hfSatNow = 1.0, recHfNow = 0.0, reproHfNow = 0.0;
    double bumpNow = 1.0, bumpHzNow = 57.0, azNow = 0.0, magNow = 0.0;
    double recAmpNow = 0.15, reproAmpNow = 0.15, glideCoef = 0.1;
    double field = 0.2, square = 0.24, pin = 1e-5, molField = 1.0;
    double edgeCeiling = 1.0;  // the highs' ceiling, in the flux's units
    double hissGain = 0.0, hissPower = 1.0, rumbleGain = 0.0, modTapeGain = 0.0, modScale = 1.0;
    double printTapeGain = 1.0;
    double sensitivity = 1.0, dc = 0.0, dcOut = 0.0;
    // Azimuth, in seconds: half the time between the tracks (left late by it,
    // right early) and the window each track's gap spreads over; where they
    // are going, now and at the frame before, gliding per frame.
    double spread = 0.0, window = 0.0, spreadNow = 0.0, windowNow = 0.0;
    double spreadWas = 0.0, windowWas = 0.0, skewCoef = 0.001;
    double ips = 15.0;
    // The transport: the speed error's parts, each turning part's depth
    // drifting toward a new target every quarter second.
    Phasor reel, takeup, capstan, idler;
    Array<double, 4> sway = {1.0, 1.0, 1.0, 1.0}, swayTarget = {1.0, 1.0, 1.0, 1.0};
    double swayCoef = 1e-4;
    int swayTicks = 0, swayEvery = 375;
    double drift1 = 0.0, drift2 = 0.0, driftCoef = 0.0, driftScale = 1.0, driftTarget = 0.0;
    double jitter1 = 0.0, jitterLow = 0.0, jitterCoef = 0.0, jitterLowCoef = 0.0;
    double jitterScale = 1.0, jitterTarget = 0.0;
    Biquad scrapeBand;
    double scrapeNorm = 1.0;
    double scrapeRms = 0.0;                    // depends only on the scrape setting and the speed
    double travel = 0.0, travelKeep = 0.9999;  // the tape's displacement, in seconds
    double readNow = 0.0, readWas = 0.0;       // the read's delay, oversampled samples
    double wowHeld = 1.0, flutterHeld = 1.0;   // what fits the read's reach
    // Wow and flutter grow as the tape slows (a machine's specification
    // runs about 0.04, 0.05, 0.08% at 30, 15, 7.5 ips): the knobs are the
    // speed error at 15 ips, times the square root of 15 over the speed.
    double speedDepth = 1.0;
    double movedNow = 0.0;
    int tick = 0;
    // Dropouts.
    double dropDepthNow = 0.0, dropAt = 1.0, dropStep = 0.0, dropOther = 1.0;
    double dropChance = 0.0, dropReach = 1.0;
    // Print-through, crosstalk, hum.
    int printDecim = 4;
    double printRate = 12000.0, printDelay = 1.0;
    double crossCoef = 0.01;
    // Hum: the mains' fundamental and odd harmonics picked up magnetically
    // at the playback head (so through the playback curve's lows), and the
    // power supply's ripple, twice the mains with a buzz of harmonics into
    // the low kilohertz. The balance is fixed, a little uneven as a real
    // machine's is (not random: offline renders must match the live
    // output), and the whole is `hum` RMS against reference.
    Phasor humBase;
    Section humLow;  // the magnetic part through NAB's playback lows
    static constexpr double kHumNorm = 1.0 / 0.946967792483;
    double modCoef = 0.1;
    // Meters.
    double peakField = 0.0, peakKeep = 0.9999, rounding = 0.0, roundKeep = 0.9999;
    double roundRatio = 1.0;  // the rounding as a ratio, refreshed every kTick frames
    GainCache driveCache, outputCache;
    GainCache printCache, crossCache, hissCache, humCache, modCache;
    double autoMix = 1.0, autoFrom = 1.0, autoTarget = 1.0;
    int autoLeft = 0, autoSamples = 960;
    double vuCoef = 0.001;
    double tapeRun = 0.0;  // metres of tape played, for the editor's reels
    bool rolling = false;
    double quiet = 0.0;  // seconds since the input was last above silence
    // The scope: the field as the record head gave it, held back by the
    // latency to meet the flux as the playback side gives it back, both
    // low-passed at 2 kHz and taken at about 8 kHz.
    Buffer<double> lagRing;
    int lagAt = 0;
    Biquad scopeLowH, scopeLowM;
    int scopeStride = 6, scopePhase = 0, scopeAt = 0;
    int trailStride = 750, trailPhase = 0, trailAt = 0;

    // --- shared with the editor -------------------------------------------------------------

    Shared<double> vuLeft, vuRight, inLeft, inRight, outLeft, outRight;
    Shared<double> tapeMetres, sampleRateNow, idle, heldWow, readReach;
    Shared<int> latencyNow;
    Shared<bool> running;
    Shared<int> scopeHead, trailHead;
    SharedArray<float, 512> scopeH, scopeM;
    SharedArray<float, 256> trail;  // the tape's displacement, in microseconds
    double peakInL = 0.0, peakInR = 0.0, peakOutL = 0.0, peakOutR = 0.0, fall = 0.999;

    void factoryPatches(Patches& p) {
        // A modern two-track machine at 30 ips, well kept: the peaks just
        // rounded, the electronics clean, a real tape's hiss and grain,
        // the transport's motion at a good machine's figures.
        p.add("Mastering")
            .set(speed, "ips30")
            .set(drive, -2)
            .set(recordAmp, 12)
            .set(reproAmp, 12)
            .set(azimuth, 0.01)
            .set(wow, 0.017)
            .set(flutter, 0.021)
            .set(scrape, 8)
            .set(crosstalk, -60)
            .set(hiss, 0)
            .set(hum, -85);
        p.add("Mix Bus")
            .set(drive, 2)
            .set(azimuth, 0.02)
            .set(wow, 0.02)
            .set(flutter, 0.02)
            .set(scrape, 12)
            .set(crosstalk, -55)
            .set(hiss, 0);
        p.add("Peak Rounding")
            .set(drive, 5)
            .set(wow, 0)
            .set(flutter, 0)
            .set(scrape, 0)
            .set(output, 0.5);
        p.add("Tracking")
            .set(emphasis, "nab")
            .set(drive, 3)
            .set(oversampling, "x2")
            .set(latency, "low")
            .set(output, 0.5);
        p.add("Hot Drums")
            .set(tape, 9)
            .set(emphasis, "nab")
            .set(drive, 9)
            .set(recordAmp, 30)
            .set(reproAmp, 25)
            .set(headBump, 120)
            .set(scrape, 15)
            .set(output, 1.5);
        p.add("Fat Bass")
            .set(tape, 9)
            .set(speed, "ips7_5")
            .set(emphasis, "nab")
            .set(drive, 6)
            .set(headBump, 140)
            .set(bumpFreq, 50)
            .set(output, 3.3);
        p.add("Saturated")
            .set(speed, "ips7_5")
            .set(drive, 14)
            .set(recordAmp, 35)
            .set(reproAmp, 30)
            .set(wow, 0.04)
            .set(flutter, 0.04)
            .set(scrape, 20)
            .set(output, 4.3);
        p.add("Warm Electronics")
            .set(drive, 2)
            .set(recordAmp, 55)
            .set(reproAmp, 45)
            .set(output, 0.5);
        p.add("Underbiased").set(tape, 9).set(bias, 65).set(drive, 4).set(output, 0.5);
        p.add("Cassette")
            .set(tape, 5)
            .set(speed, "ips3_75")
            .set(drive, -3)
            .set(azimuth, 0.08)
            .set(wow, 0.06)
            .set(flutter, 0.05)
            .set(scrape, 35)
            .set(dropouts, 15)
            .set(dropDepth, 25)
            .set(hiss, 0)
            .set(modNoise, 3)
            .set(hum, -75)
            .set(output, 3);
        p.add("Worn Machine")
            .set(tape, 5)
            .set(speed, "ips7_5")
            .set(bias, 85)
            .set(drive, 4)
            .set(recordAmp, 40)
            .set(reproAmp, 40)
            .set(azimuth, 0.12)
            .set(headMagnetism, 40)
            .set(wow, 0.18)
            .set(flutter, 0.11)
            .set(scrape, 50)
            .set(dropouts, 45)
            .set(dropDepth, 50)
            .set(printThrough, -48)
            .set(crosstalk, -38)
            .set(hiss, 4)
            .set(modNoise, 6)
            .set(hum, -65)
            .set(output, 3.5);
        p.add("Warped")
            .set(tape, 5)
            .set(speed, "ips3_75")
            .set(drive, -2)
            .set(azimuth, 0.1)
            .set(wow, 0.7)
            .set(flutter, 0.18)
            .set(scrape, 40)
            .set(dropouts, 30)
            .set(hiss, 6)
            .set(output, 3);
        p.add("Clean Transfer")
            .set(speed, "ips30")
            .set(drive, -12)
            .set(recordAmp, 0)
            .set(reproAmp, 0)
            .set(headBump, 0)
            .set(wow, 0)
            .set(flutter, 0)
            .set(scrape, 0)
            .set(modNoise, -30);
    }

    void prepare() {
        rate = sampleRate();
        const bool low = latency.index() == 1;
        const int want = timesOf(oversampling.index());
        for (Track& t : track) {
            if (low)
                t.os.prepare(want);
            else
                t.os.prepare(want, Oversampler::Phase::Linear);
        }
        factor = track[0].os.factor();
        hi = rate * factor;
        const int osLatency = track[0].os.latency();
        center = low ? max(2, 9 - osLatency) : roundToInt(0.008 * rate);
        reach = (center - 1.5) / rate;
        readReach.set(reach);
        lag = osLatency + center;
        setLatency(lag);
        const int size = (center * 2 + 8) * factor + 8;
        printDecim = max(1, roundToInt(rate / 12000.0));
        printRate = rate / printDecim;
        // The slowest speed's turn of the reel, and a little over.
        const int printSize = ceilToInt(turnSeconds(3.75) * printRate) + 8;
        for (Track& t : track) {
            t.ring.resize(size);
            t.print.resize(printSize);
            t.recAmp.set(0.0, 0.35, hi);
            t.reproAmp.set(0.0, 0.25, hi);
        }
        lagRing.resize(lag + 2);
        random.seed(0x5eed);
        noise.seed(0x7a9e);
        // The electronics' noise, fixed (it is not the tape's): 8 dB over a
        // modern tape's hiss at 15 ips at 50 Hz, where the IEC curve leaves
        // it 7 dB under its own floor; A-weighted, 26 dB under the hiss.
        rumbleGain = kRefRms * dbToGain(-60.5) / sqrt(measureHiss(1, 1.0)) * dbToGain(8.0) *
                     2.2360679774997898;
        peakKeep = exp(-1.0 / (0.3 * rate));
        roundKeep = peakKeep;
        vuCoef = 1.0 - exp(-1.0 / (0.065 * rate));
        fall = exp(-1.0 / (0.25 * rate));
        travelKeep = exp(-1.0 / (4.0 * rate));
        glideCoef = 1.0 - exp(-kTick / (0.03 * rate));
        fadeStep = 1.0 / (0.004 * rate);
        autoSamples = max(1, roundToInt(0.02 * rate));
        swayCoef = 1.0 - exp(-1.0 / (0.7 * rate));
        skewCoef = 1.0 - exp(-1.0 / (0.03 * rate));
        swayEvery = max(1, roundToInt(0.25 * rate / kTick));
        // Scrape: noise through the tape span's resonance near 3 kHz (Q 0.9),
        // its band's RMS made 1.
        const double scrapeHz = min(3000.0, 0.35 * rate);
        scrapeBand.bandpass(scrapeHz, 0.9);
        scrapeNorm = 1.0 / sqrt(pi * scrapeHz / (0.9 * rate));
        scopeLowH.lowpass(2000.0, 0.707);
        scopeLowM.lowpass(2000.0, 0.707);
        scopeStride = max(1, roundToInt(rate / 8000.0));
        trailStride = max(1, roundToInt(rate / 64.0));
        for (Track& t : track) t.clear();
        clearState();
        settle();
        applyChoices();
        landAzimuth();
        unplayed = true;
    }

    // Seconds of one turn of a 10.5-inch reel half wound, at a speed.
    double turnSeconds(double atIps) const { return 0.534 / (atIps * 0.0254); }

    void clearState() {
        reel.start(0.0);
        takeup.start(0.25);
        capstan.start(0.0);
        idler.start(0.5);
        for (int k = 0; k < 4; ++k) {
            sway[k] = 1.0;
            swayTarget[k] = 1.0;
        }
        swayTicks = 0;
        drift1 = 0.0;
        drift2 = 0.0;
        driftTarget = 0.0;
        jitter1 = 0.0;
        jitterTarget = 0.0;
        jitterLow = 0.0;
        scrapeBand.clear();
        travel = 0.0;
        readNow = center * factor;
        readWas = readNow;
        movedNow = 0.0;
        tick = 0;
        switching = 0;
        fade = 1.0;
        dropDepthNow = 0.0;
        dropAt = 1.0;
        dropStep = 0.0;
        dropOther = 1.0;
        humBase.start(0.0);
        humLow.clear();
        peakField = 0.0;
        rounding = 0.0;
        roundRatio = 1.0;
        driveCache.at = 1e100;
        outputCache.at = 1e100;
        printCache.at = 1e100;
        crossCache.at = 1e100;
        hissCache.at = 1e100;
        humCache.at = 1e100;
        modCache.at = 1e100;
        tapeRun = 0.0;
        rolling = false;
        quiet = 0.0;
        for (int i = 0; i < lagRing.size(); ++i) lagRing[i] = 0.0;
        lagAt = 0;
        scopeLowH.clear();
        scopeLowM.clear();
        scopePhase = 0;
        scopeAt = 0;
        trailPhase = 0;
        trailAt = 0;
        peakInL = 0.0;
        peakInR = 0.0;
        peakOutL = 0.0;
        peakOutR = 0.0;
    }

    // The gliding settings, landed where they are going.
    void settle() {
        tapeNow = tape.value();
        biasNow = bias.value();
        hfSatNow = hfSaturation.value();
        recHfNow = recordHf.value();
        reproHfNow = reproHf.value();
        bumpNow = headBump.value();
        bumpHzNow = bumpFreq.value();
        azNow = azimuth.value();
        magNow = headMagnetism.value();
        recAmpNow = recordAmp.value();
        reproAmpNow = reproAmp.value();
        autoMix = autoGain.on() ? 1.0 : 0.0;
        autoFrom = autoMix;
        autoTarget = autoMix;
        autoLeft = 0;
    }

    bool choicesChanged() const {
        const int s = speed.index();
        const int nab = emphasis.index() == 0 && s != 0 ? 1 : 0;
        return s != speedNow || nab != nabNow;
    }

    void updateTail() {
        setTail(printThrough.value() > -80.0 ? ceilToInt((turnSeconds(ips) + 0.1) * rate) : 0);
    }

    void update() {
        const bool changed = choicesChanged();
        if (unplayed) {
            // Until the first block, settings are where they are going.
            settle();
            if (changed)
                applyChoices();
            else
                designAll();
            landAzimuth();
        } else if (changed && switching == 0) {
            switching = 1;
        }
        const double wantedAuto = autoGain.on() ? 1.0 : 0.0;
        if (wantedAuto != autoTarget) {
            autoFrom = autoMix;
            autoTarget = wantedAuto;
            autoLeft = autoSamples;
        }
        holdTransport();
        const double mainsHz = mains.index() == 0 ? 50.0 : 60.0;
        humBase.rate(mainsHz / rate);
        dropChance = 4.0 * dropouts.value() * dropouts.value() / rate;
        updateTail();
    }

    // The speed and the standard: the record and playback curves, the noise's
    // shaping and the transport's rates. A change while playing waits for the
    // output to fade out, then everything restarts clear.
    void applyChoices() {
        const int s = speed.index();
        const bool nab = emphasis.index() == 0 && s != 0;
        speedNow = s;
        nabNow = nab ? 1 : 0;
        ips = speedIps(s);
        const double v = ips / 15.0;
        const double fTau = 1.0 / (twoPi * speedTau(s, nab));
        const double eps = 0.316;
        const double fLow = 1.0 / (twoPi * 3180e-6);
        for (Track& k : track) {
            // These curves depend only on the speed, standard and sample rate.
            k.rec.design(nab, hi, false);
            k.edge.design(5000.0 * v, 9000.0 * v, hi);
            k.play.design(nab, hi, true);
            // The hiss: white in the tape, through the playback curve.
            k.hissTau.design(fTau, 0.45 * rate, 1.0, rate);
            k.hissTop.design(0.49 * rate, hissCorner(s), 1.0, rate);
            if (nab)
                k.hissLow.design(fLow * eps, fLow, eps, rate);
            else
                k.hissLow.design(fLow, fLow, 1.0, rate);
            // The playback electronics' own noise: white at the head, so
            // the playback curve's integration leaves it falling 6 dB an
            // octave from the standard's corner down to the deck's limit at
            // 25 Hz, and below 15 Hz nothing.
            k.rumbleTau.design(fTau, 25.0, 1.0, rate);
            if (nab)
                k.rumbleLow.design(fLow * eps, fLow, eps, rate);
            else
                k.rumbleLow.design(fLow, fLow, 1.0, rate);
            k.rumbleCut.highpass(15.0, 0.707);
            k.printIn.lowpass(min(3000.0 * v, 0.4 * printRate), 0.707);
            // Print-through: the recording's stray field reaches the next
            // layer across the base film strongest at wavelengths about 2 pi
            // times that distance (shorter ones die off across the gap,
            // longer ones barely field it): a broad band about 1 kHz at
            // 15 ips, moving with the speed as a wavelength does.
            k.printOut.bandpass(min(1000.0 * v, 0.4 * rate), 0.5);
        }
        if (nab)
            humLow.design(fLow * eps, fLow, eps, rate);
        else
            humLow.design(fLow, fLow, 1.0, rate);
        printDelay = turnSeconds(ips) * printRate;
        // Modulation noise: a low noise of unit RMS that the signal rides.
        const double modHz = 1500.0 * v;
        modCoef = 1.0 - exp(-twoPi * modHz / rate);
        modScale = sqrt(2.0 * rate / (pi * modHz));
        // A dropout lifts the tape up to 40 um off the head at full depth:
        // spacing loss, 54.6 dB per wavelength of lift, puts its corner at
        // v / (2 pi d); two poles at 0.76 of that follow the exponential
        // to about twice it.
        dropReach = 0.76 * ips * 0.0254 / (twoPi * 40e-6) / rate;
        crossCoef = 1.0 - exp(-twoPi * 300.0 * v / rate);
        // The transport's rates follow the speed: the reels turn once per
        // metre or so, the capstan (25 mm) and an idler (11 mm) faster.
        speedDepth = sqrt(1.0 / v);
        reel.rate(0.6 * v / rate);
        takeup.rate(0.5 * v / rate);
        capstan.rate(4.85 * v / rate);
        idler.rate(11.0 * v / rate);
        // The random parts: noise held for kTick frames through low-passes,
        // scaled to unit RMS by their noise bandwidths.
        const double held = rate / kTick;
        driftCoef = 1.0 - exp(-twoPi * 0.5 * v / rate);
        driftScale = sqrt(2.0 * held / (pi * 0.5 * v));
        jitterCoef = 1.0 - exp(-twoPi * 25.0 * v / rate);
        jitterLowCoef = 1.0 - exp(-twoPi * 6.0 * v / rate);
        jitterScale = sqrt(held / (pi * 19.0 * v));
        designAll();
        holdTransport();
        updateTail();
    }

    // What follows the tape as it glides. Softer, older tapes print more
    // (print-through goes about as one over the coercivity): 10 dB more on
    // the 5 dB tape than the 12.5.
    void designTape() {
        oxideNow.set(tapeNow);
        printTapeGain = dbToGain(1.333 * (12.5 - tapeNow));
        modTapeGain = dbToGain(oxideNow.modDb);
    }

    // Everything the gliding settings decide.
    void designAll() {
        designTape();
        hissPower = measureHiss(speedNow, oxideNow.loss);
        designRecord();
        designHead();
    }

    // The tape's constants and the record side's curves.
    void designRecord() {
        const int s = speedNow;
        const bool nab = nabNow == 1;
        square = oxideNow.square;
        field = oxideNow.field * dbToGain(speedFieldDb(s)) / kRef;
        molField = oxideNow.field * dbToGain(tapeNow);
        edgeCeiling = edgeFor(s, nab, oxideNow.loss, hfSatNow) * kRef;
        const double v = ips / 15.0;
        for (Track& k : track) {
            shelf(k.recTrim, recHfNow, min(4000.0 * v, 0.3 * hi), hi);
            k.recAmp.set(recAmpNow, 0.35, hi);
            k.reproAmp.set(reproAmpNow, 0.25, hi);
        }
        const double wanted = kRefRms * dbToGain(speedHissDb(s) + oxideNow.hissDb + 60.5);
        hissGain = wanted / sqrt(hissPower);
    }

    // What bias, the heads and the hum decide.
    void designHead() {
        const int s = speedNow;
        const double b = biasNow;
        // Pinning: the recommended bias leaves almost none; underbias lets
        // it grow by 25 dB at half the current.
        const double basePin = oxideNow.pin * (s >= 2 ? (s == 2 ? 3.0 : 8.0) : 1.0);
        pin = b < 1.0 ? basePin * pow(10.0, 2.5 * (1.0 - b) / 0.5)
                      : basePin * max(0.2, 1.0 - 0.6 * (b - 1.0));
        sensitivity = dbToGain(biasLevelDb(b));
        // A magnetized head: a steady field, KMAG of reference at 100%.
        dc = 1.5 * magNow * oxideNow.field;
        dcOut = anhysteretic(dc, square);
        // Azimuth's half spread and window; together they may take at most
        // 0.6 of the read's reach (low latency's 9 samples), shrinking
        // together past it.
        spread = 0.5 * azimuthSpread(azNow, ips);
        window = azimuthWindow(azNow, ips);
        capAzimuth(spread, window, reach);
        for (Track& k : track)
            k.head.design(s, oxideNow.loss, bumpNow, bumpHzNow, biasTiltDb(s, b), reproHfNow, rate);
    }

    // Azimuth where it is going, for restarts.
    void landAzimuth() {
        spreadNow = spread;
        spreadWas = spread;
        windowNow = window;
        windowWas = window;
    }

    // How far a read reaches either side of the moving centre for azimuth.
    double azimuthExtent() const { return spread + 0.5 * window; }

    // The gliding settings move 30 ms toward their values; their curves are
    // redesigned every kTick frames while they move.
    double glideTo(double now, double target, double tiny) const {
        const double next = now + glideCoef * (target - now);
        return abs(target - next) < tiny ? target : next;
    }
    void glide() {
        const bool azimuthMoves = azNow != azimuth.value();
        const bool recordMoves = tapeNow != tape.value() || hfSatNow != hfSaturation.value() ||
                                 recHfNow != recordHf.value() || recAmpNow != recordAmp.value() ||
                                 reproAmpNow != reproAmp.value();
        const bool headMoves = biasNow != bias.value() || reproHfNow != reproHf.value() ||
                               bumpNow != headBump.value() || bumpHzNow != bumpFreq.value() ||
                               azNow != azimuth.value() || magNow != headMagnetism.value();
        if (!recordMoves && !headMoves) return;
        if (recordMoves) {
            const bool tapeMoves = tapeNow != tape.value();
            tapeNow = glideTo(tapeNow, tape.value(), 1e-4);
            hfSatNow = glideTo(hfSatNow, hfSaturation.value(), 1e-4);
            recHfNow = glideTo(recHfNow, recordHf.value(), 1e-4);
            recAmpNow = glideTo(recAmpNow, recordAmp.value(), 1e-5);
            reproAmpNow = glideTo(reproAmpNow, reproAmp.value(), 1e-5);
            if (tapeMoves) {
                designTape();
                // Integrating the noise response is done once, when the glide lands.
                if (tapeNow == tape.value()) hissPower = measureHiss(speedNow, oxideNow.loss);
            }
            designRecord();
        }
        biasNow = glideTo(biasNow, bias.value(), 1e-4);
        reproHfNow = glideTo(reproHfNow, reproHf.value(), 1e-4);
        bumpNow = glideTo(bumpNow, headBump.value(), 1e-4);
        bumpHzNow = glideTo(bumpHzNow, bumpFreq.value(), 1e-3);
        azNow = glideTo(azNow, azimuth.value(), 1e-6);
        magNow = glideTo(magNow, headMagnetism.value(), 1e-4);
        designHead();
        if (azimuthMoves) holdTransport();
    }

    // The particles' own noise loses its shortest wavelengths too.
    double hissCorner(int s) const { return 10000.0 * speedIps(s) / 15.0; }

    // Unit white noise's power, A-weighted, through the IEC playback curve
    // and the aligned head: the hiss's gain is set so it measures the tape's
    // figure (NAB, with more lift of the highs on playback, then comes out
    // a little noisier by itself).
    double measureHiss(int s, double loss) const {
        Section tau, top;
        tau.design(1.0 / (twoPi * speedTau(s, false)), 0.45 * rate, 1.0, rate);
        top.design(0.49 * rate, hissCorner(s), 1.0, rate);
        Biquad head;
        const double topHz = speedTopHz(s) / loss;
        const bool headOn = topHz < 0.45 * rate;
        if (headOn) head.lowpass(topHz, 0.75);
        double power = 0.0;
        const int n = 512;
        for (int i = 1; i <= n; ++i) {
            const double f = (i - 0.5) * 0.5 * rate / n;
            double g = tau.gainAt(f, rate) * top.gainAt(f, rate) * aWeight(f);
            if (headOn) g *= dbToGain(head.responseDb(f));
            power += g * g;
        }
        return power / n;
    }

    // How far the read must reach for wow and flutter of 1 (a 100% speed
    // error), in seconds: each part's displacement, eps / (2 pi f), at its
    // deepest sway, and the random parts' at about three times their RMS.
    double wowReach() const {
        const double v = ips / 15.0;
        return 1.45 * (0.5 / (twoPi * 0.6 * v) + 0.25 / (twoPi * 0.5 * v)) +
               0.3 * 3.0 / (twoPi * 0.5 * v);
    }
    double flutterReach() const {
        const double v = ips / 15.0;
        return 1.45 * (0.45 / (twoPi * 4.85 * v) + 0.2 / (twoPi * 11.0 * v)) +
               0.35 * 3.0 / (twoPi * 6.0 * v);
    }

    // What of the wow and flutter fits the read's reach: all of it at full
    // latency but for the deepest settings at the slowest speeds; with low
    // latency the wow is scaled down first (its shape kept), then the
    // flutter.
    void holdTransport() {
        scrapeRms = scrapeSeconds();
        const double budget = 0.8 * max(0.0, reach - azimuthExtent()) - scrapeRms * 4.0;
        const double w = wow.value() * speedDepth * wowReach();
        const double f = flutter.value() * speedDepth * flutterReach();
        wowHeld = 1.0;
        flutterHeld = 1.0;
        if (w + f <= budget) return;
        if (f < budget) {
            wowHeld = (budget - f) / w;
        } else {
            wowHeld = 0.0;
            flutterHeld = budget > 0.0 ? budget / f : 0.0;
        }
    }

    // Scrape's RMS jitter in seconds: 380 ns at 100% at 15 ips (sidebands
    // about -35 dB around a 10 kHz tone: a badly worn, dirty transport),
    // rising as the setting to the 1.25 so the low end stays a good
    // machine's (about -57 dB at 10%); more at lower speeds (the same
    // vibration is a longer time).
    double scrapeSeconds() const {
        const double sc = scrape.value();
        return sc > 0.0 ? 380e-9 * pow(sc, 1.25) * sqrt(15.0 / ips) : 0.0;
    }

    // A soft bound: linear to 80% of the reach, then easing toward it, so
    // the deepest settings compress the motion instead of clipping it.
    double bound(double x, double limit) const {
        const double knee = 0.8 * limit, a = abs(x);
        if (a <= knee || limit <= 0.0) return limit > 0.0 ? x : 0.0;
        const double over = a - knee, room = limit - knee;
        const double y = knee + room * over / (over + room);
        return x < 0.0 ? -y : y;
    }

    // The transport, once per frame: where the read is, in oversampled
    // samples, and the dropouts.
    void moveTape() {
        if (tick == 0) {
            driftTarget = random.gaussian();
            jitterTarget = random.gaussian();
            glide();
            roundRatio = dbToGain(rounding);
            reel.hold();
            takeup.hold();
            capstan.hold();
            idler.hold();
            humBase.hold();
            if (++swayTicks >= swayEvery) {
                swayTicks = 0;
                for (int k = 0; k < 4; ++k) swayTarget[k] = 1.0 + 0.45 * random.bipolar();
            }
        }
        tick = (tick + 1) % kTick;
        for (int k = 0; k < 4; ++k) sway[k] += swayCoef * (swayTarget[k] - sway[k]);
        // The random parts: a slow wander, and a band of friction.
        drift1 += driftCoef * (driftTarget - drift1);
        drift2 += driftCoef * (drift1 - drift2);
        jitter1 += jitterCoef * (jitterTarget - jitter1);
        jitterLow += jitterLowCoef * (jitter1 - jitterLow);
        const double drift = drift2 * driftScale, jitter = (jitter1 - jitterLow) * jitterScale;
        reel.next();
        takeup.next();
        capstan.next();
        idler.next();
        const double wowNow = wow.value() * speedDepth * wowHeld;
        const double flutterNow = flutter.value() * speedDepth * flutterHeld;
        const double speedError =
            wowNow * (0.5 * sway[0] * reel.s + 0.25 * sway[1] * takeup.s + 0.3 * drift) +
            flutterNow * (0.45 * sway[2] * capstan.s + 0.2 * sway[3] * idler.s + 0.35 * jitter);
        travel = travel * travelKeep + speedError / rate;
        double moved = travel;
        if (scrapeRms > 0.0) moved += scrapeRms * scrapeNorm * scrapeBand.process(noise.gaussian());
        moved = bound(moved, reach - max(azimuthExtent(), spreadNow + 0.5 * windowNow));
        movedNow = moved;
        readWas = readNow;
        readNow = (center + moved * rate) * factor;
        spreadWas = spreadNow;
        windowWas = windowNow;
        spreadNow += skewCoef * (spread - spreadNow);
        windowNow += skewCoef * (window - windowNow);
        if (abs(spread - spreadNow) < 1e-12) spreadNow = spread;
        if (abs(window - windowNow) < 1e-12) windowNow = window;
        // Dropouts: a moment's spacing, the highs lost first.
        if (dropAt >= 1.0 && dropChance > 0.0 && random.next() < dropChance) {
            dropAt = 0.0;
            dropDepthNow = dropDepth.value() * (0.3 + 0.7 * random.next());
            dropStep = 1.0 / ((0.006 + random.next() * 0.06) * rate);
            dropOther = 0.6 + 0.4 * random.next();
        }
    }

    // A read between samples of the ring: a cubic (Lagrange) through four.
    double readRing(const Track& t, double delay) const {
        const int n = t.ring.size();
        const int whole = floorToInt(delay);
        const double f = delay - whole;
        const int i0 = t.at - whole;
        double ym1 = 0.0, y0 = 0.0, y1 = 0.0, y2 = 0.0;
        if (i0 >= 2 && i0 + 1 < n) {
            ym1 = t.ring[i0 + 1];
            y0 = t.ring[i0];
            y1 = t.ring[i0 - 1];
            y2 = t.ring[i0 - 2];
        } else {
            ym1 = t.ring[wrap(i0 + 1, n)];
            y0 = t.ring[wrap(i0, n)];
            y1 = t.ring[wrap(i0 - 1, n)];
            y2 = t.ring[wrap(i0 - 2, n)];
        }
        if (f == 0.0) return y0;
        // Lagrange through the four, as a cubic in f (Horner).
        const double third = 1.0 / 3.0, sixth = 1.0 / 6.0;
        const double c1 = y1 - ym1 * third - y0 * 0.5 - y2 * sixth;
        const double c2 = (ym1 + y1) * 0.5 - y0;
        const double c3 = (y2 - ym1) * sixth + (y0 - y1) * 0.5;
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    // The print-through's echo: the decimated layer read one turn back,
    // between its samples.
    double printEcho(const Track& t) const {
        const int n = t.print.size();
        const double back = printDelay - static_cast<double>(t.printPhase) / printDecim;
        const int whole = floorToInt(back);
        const double f = back - whole;
        const double a = t.print[wrap(t.printAt - whole, n)];
        const double b = t.print[wrap(t.printAt - whole - 1, n)];
        return a + (b - a) * f;
    }

    void process(Block& io) {
        if (unplayed) unplayed = false;
        const int channels = min(io.channels, 2);
        double vuL = 0.0, vuR = 0.0;
        Array<double, 2> y = {0.0, 0.0};
        double scopeIn = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            moveTape();
            double drop = 0.0;
            if (dropAt < 1.0) {
                drop = dropDepthNow * sin(pi * dropAt);
                dropAt += dropStep;
            }
            // A switch: the output fades out; at silence the new curves go
            // in and every stage starts clear; then the input fades in, so
            // the tape fills again from nothing.
            double outFade = 1.0, inFade = 1.0;
            if (switching == 1) {
                fade = max(0.0, fade - fadeStep);
                outFade = fade;
                if (fade == 0.0) {
                    applyChoices();
                    for (Track& t : track) t.clear();
                    switching = 2;
                    inFade = 0.0;
                }
            } else if (switching == 2) {
                fade = min(1.0, fade + fadeStep);
                inFade = fade;
                if (fade == 1.0) switching = choicesChanged() ? 1 : 0;
            }
            // The levels glide on their own; the tape's factors with it.
            const double printGain = printCache.offAt(printThrough.at(i), -80.0) * printTapeGain;
            const double crossGain = channels == 2 ? crossCache.offAt(crosstalk.at(i), -80.0) : 0.0;
            const double hissOn = hissCache.offAt(hiss.at(i), -30.0);
            const double humGain = humCache.offAt(hum.at(i), -100.0) * kRefRms * kHumNorm;
            const double modGain = modTapeGain * modCache.offAt(modNoise.at(i), -30.0);
            if (autoLeft > 0) {
                --autoLeft;
                autoMix = autoLeft == 0 ? autoTarget
                                        : lerp(autoFrom, autoTarget,
                                               1.0 - static_cast<double>(autoLeft) / autoSamples);
            }
            const double g = drive.at(i);
            const double driveLevel = driveCache.get(g);
            const double driveGain = driveLevel * inFade;
            const double back = autoMix == 1.0   ? 1.0 / driveLevel
                                : autoMix == 0.0 ? 1.0
                                                 : lerp(1.0, 1.0 / driveLevel, autoMix);
            const double out = outputCache.get(output.at(i)) * back * sensitivity * outFade;
            double humNow = 0.0;
            if (humGain > 0.0) {
                humBase.next();
                // Each harmonic's sine by rotation from the fundamental's.
                double hr = humBase.c, hs = humBase.s, magnetic = 0.0, ripple = 0.0;
                for (int n = 1; n <= 16; ++n) {
                    if (n % 2 == 1)
                        magnetic += humWeight(n) * hs;
                    else
                        ripple += humWeight(n) * hs;
                    const double nr = hr * humBase.c - hs * humBase.s;
                    hs = hs * humBase.c + hr * humBase.s;
                    hr = nr;
                }
                humNow = humGain * (humLow.process(magnetic) + ripple) * inFade;
            }
            const double invField = 1.0 / field;
            const double readStep = (readNow - readWas) / factor;
            const double spreadStep = (spreadNow - spreadWas) / factor;
            const double windowStep = (windowNow - windowWas) / factor;
            const bool slanted = spreadNow > 0.0 || spreadWas > 0.0;
            bool loud = false;
            for (int c = 0; c < channels; ++c) {
                Track& t = track[c];
                double x = io[c][i];
                if (!isFinite(x)) x = 0.0;
                if (abs(x) > 1e-5) loud = true;
                const double rec = x * driveGain;
                if (c == 0) scopeIn = rec;
                // The VU meter: full-wave, two poles of 65 ms, read as RMS.
                t.vu1 += vuCoef * (abs(rec) - t.vu1);
                t.vu2 += vuCoef * (t.vu1 - t.vu2);
                if (c == 0)
                    peakInL = max(abs(x), peakInL * fall);
                else
                    peakInR = max(abs(x), peakInR * fall);
                // Modulation noise: the magnetization rides a low noise, a
                // magnetized head's steady part too (DC noise: the grain
                // stays when the signal stops).
                double ride = 1.0;
                if (modGain > 0.0) {
                    t.modZ1 += modCoef * (noise.bipolar() * 1.7320508075688772 - t.modZ1);
                    t.modZ2 += modCoef * (t.modZ1 - t.modZ2);
                    ride = 1.0 + modGain * modScale * t.modZ2;
                }
                t.os.up(rec);
                const double side = c == 0 ? 1.0 : -1.0;
                for (int k = 0; k < factor; ++k) {
                    double u = t.os.sample(k);
                    u = t.recAmp.process(u * (1.0 / kRef)) * kRef;
                    u = t.rec.process(t.recTrim.process(u));
                    const double h = u * field + dc;
                    const double m = t.oxide.step(h, square, pin);
                    const double a = abs(h - dc);
                    if (a > peakField) peakField = a;
                    // The rounding: the field against the flux, at the peaks.
                    if (a > 0.05 * molField && a > roundRatio * abs(m - dcOut)) {
                        roundRatio = a / max(abs(m - dcOut), 1e-9);
                        rounding = gainToDb(roundRatio);
                    }
                    // The flux, its highs held to what the tape can take, and
                    // NAB's lows undone.
                    const double flux = t.edge.process((m * ride - dcOut) * invField, edgeCeiling);
                    const double played = t.play.process(flux);
                    t.at = t.at + 1 == t.ring.size() ? 0 : t.at + 1;
                    t.ring[t.at] = played;
                    const double where = readWas + readStep * (k + 1);
                    double r = 0.0;
                    if (!slanted) {
                        r = readRing(t, where);
                    } else {
                        // Each track's gap hears its window's average, the
                        // left half the tracks' spread later, the right
                        // earlier.
                        const double centre =
                            where + side * (spreadWas + spreadStep * (k + 1)) * hi;
                        const double width = (windowWas + windowStep * (k + 1)) * hi;
                        for (int j = 0; j < kTaps; ++j)
                            r += readRing(t, centre + width * ((j + 0.5) / kTaps - 0.5));
                        r *= 1.0 / kTaps;
                    }
                    t.os.set(k, t.reproAmp.process(r * (1.0 / kRef)) * kRef);
                }
                double v = t.os.down();
                // The next layer of the reel prints on this one: what was
                // played a turn ago comes back, faintly, in a band.
                const double into = t.printIn.process(v);
                if (++t.printPhase >= printDecim) {
                    t.printPhase = 0;
                    t.printAt = t.printAt + 1 == t.print.size() ? 0 : t.printAt + 1;
                    t.print[t.printAt] = into;
                }
                if (printGain > 0.0) v += printGain * t.printOut.process(printEcho(t));
                if (hissOn > 0.0) {
                    const double n =
                        t.hissTop.process(t.hissLow.process(t.hissTau.process(noise.gaussian())));
                    const double low = t.rumbleCut.process(
                        t.rumbleLow.process(t.rumbleTau.process(noise.gaussian())));
                    v += (n * hissGain + low * rumbleGain) * hissOn * inFade;
                }
                v = t.head.process(v);
                // A dropout: the tape lifts off the head and its spacing loss
                // sweeps down from the top and back as it lifts and settles;
                // a little of the flux is simply missing (6 dB at the
                // deepest).
                double lift = 1.0;
                if (drop > 0.0) {
                    const double e = drop * (c == 0 ? 1.0 : dropOther);
                    lift = 1.0 - exp(-twoPi * min(dropReach / max(e, 1e-6), 0.45));
                    v *= 1.0 - 0.5 * e;
                }
                t.drop1 += lift * (v - t.drop1);
                t.drop2 += lift * (t.drop1 - t.drop2);
                v = t.drop2;
                t.crossLow += crossCoef * (v - t.crossLow);
                y[c] = v;
            }
            // Crosstalk moves the lows between the tracks: what one gains
            // the other loses, so a centred sound keeps its level.
            if (crossGain > 0.0) {
                const double d = crossGain * (track[1].crossLow - track[0].crossLow);
                y[0] += d;
                y[1] -= d;
            }
            for (int c = 0; c < channels; ++c) {
                const double v = (y[c] + humNow) * out;
                io[c][i] = v;
                if (c == 0) {
                    peakOutL = max(abs(v), peakOutL * fall);
                    vuL = track[0].vu2;
                } else {
                    peakOutR = max(abs(v), peakOutR * fall);
                    vuR = track[1].vu2;
                }
            }
            if (channels == 1) {
                peakInR = peakInL;
                peakOutR = peakOutL;
                vuR = vuL;
            }
            // The scope: the field as recorded, held back by the latency,
            // against the flux played back (before the output's level).
            lagAt = lagAt + 1 == lagRing.size() ? 0 : lagAt + 1;
            lagRing[lagAt] = scopeIn;
            const double fieldThen = lagRing[wrap(lagAt - lag, lagRing.size())];
            const double hs = scopeLowH.process(fieldThen * (1.0 / kRef));
            const double ms = scopeLowM.process((y[0] + humNow) * sensitivity * (1.0 / kRef));
            if (++scopePhase >= scopeStride) {
                scopePhase = 0;
                scopeH[scopeAt] = static_cast<float>(hs);
                scopeM[scopeAt] = static_cast<float>(ms);
                scopeAt = scopeAt + 1 == kScope ? 0 : scopeAt + 1;
            }
            if (++trailPhase >= trailStride) {
                trailPhase = 0;
                trail[trailAt] = static_cast<float>(movedNow * 1e6);
                trailAt = trailAt + 1 == kTrail ? 0 : trailAt + 1;
            }
            peakField *= peakKeep;
            rounding *= roundKeep;
            if (rolling) tapeRun += ips * 0.0254 / rate;
            quiet = loud ? 0.0 : quiet + 1.0 / rate;
        }
        // The VU reads RMS for a sine: its average times pi / (2 sqrt 2).
        const double l = gainToDb(vuL * 1.1107207345395915 / kRefRms);
        const double r = gainToDb(vuR * 1.1107207345395915 / kRefRms);
        vu.set(clamp(max(l, r), -40.0, 12.0));
        headroom.set(peakField > 1e-9 ? clamp(gainToDb(molField / peakField), -36.0, 48.0) : 48.0);
        saturation.set(clamp(rounding, 0.0, 24.0));
        vuLeft.set(l);
        vuRight.set(r);
        inLeft.set(gainToDb(peakInL));
        inRight.set(gainToDb(peakInR));
        outLeft.set(gainToDb(peakOutL));
        outRight.set(gainToDb(peakOutR));
        rolling = quiet < 1.0 || io.transport.playing;
        tapeMetres.set(tapeRun);
        running.set(rolling);
        idle.set(quiet);
        scopeHead.set(scopeAt);
        trailHead.set(trailAt);
        heldWow.set(wow.value() > 0.0 ? wowHeld : 1.0);
        latencyNow.set(lag);
        sampleRateNow.set(rate);
    }

    // --- the editor's layout ------------------------------------------------------------------

    static constexpr double kW = 1200;
    static constexpr double kH = 880;
    // The machine's face: reels, heads and meters.
    static constexpr double fx = 16, fy = 64, fw = 1168, fh = 210;
    static constexpr double reelY = 150, reelR = 74;
    static constexpr double leftReelX = 112, rightReelX = 1088;
    static constexpr double tapeY = 232;
    static constexpr double meterY = 76, meterW = 180, meterH = 128;
    static constexpr double meterLX = 410, meterRX = 604;
    // The panels below: record and heads on the left, the scope in the
    // middle, playback and transport on the right.
    static constexpr double topY = 286, topH = 306, lowY = 604, lowH = 260;
    static constexpr double leftX = 16, sideW = 380, rightX = 804;
    static constexpr double mx = 408, mw = 384, mh = 578;            // magnetism
    static constexpr double lx = 424, ly = 322, ls = 352;            // the scope's well
    static constexpr double gx = 820, gy = 398, gw = 348, gh = 94;   // the response
    static constexpr double tx = 820, ty = 744, tw = 252, th = 100;  // the transport's trace
    static constexpr int kKnobs = 23;
    static constexpr int kShown = 384;  // the scope's points drawn: 48 ms

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the scope (drive and bias)
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    double spanH = 0.35, spanM = 0.35;  // the scope's scales, eased
    Emphasis view;                      // the record curve, for the response graph
    Section viewTrim;
    Head viewHead;

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

    bool hovering(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit from `from` (a position) to the value.
    void dial(Canvas& g, double x, double y, double r, double norm, double from, int colour,
              bool hot) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 30 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double base = start + sweep * clamp(from, 0.0, 1.0);
        g.strokeStyle(colour);
        g.lineWidth(r > 30 ? 4.5 : 3.5);
        if (abs(at - base) > 0.001) {
            g.beginPath();
            if (at >= base)
                g.arc(x, y, r, base, at);
            else
                g.arc(x, y, r, at, base);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 4, hot ? 0x5E4E41 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 7);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 14), y + sin(at) * (r - 14));
        g.lineTo(x + cos(at) * (r - 9), y + sin(at) * (r - 9));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    void valueStyle(Canvas& g, double size) {
        g.fillStyle(Theme::text);
        g.monoFont(size);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    // The mark: a reel of tape, its pack in umber.
    void drawMark(Canvas& g, double x, double y) {
        g.fillStyle(kOxide);
        g.beginPath();
        g.circle(x, y, 12);
        g.fill();
        g.strokeStyle(kUmber);
        g.lineWidth(1.5);
        g.beginPath();
        g.circle(x, y, 12);
        g.stroke();
        g.fillStyle(kGroundTop);
        for (int k = 0; k < 3; ++k) {
            const double a = -pi * 0.5 + k * twoPi / 3.0;
            g.beginPath();
            g.circle(x + cos(a) * 6.5, y + sin(a) * 6.5, 2.6);
            g.fill();
        }
        g.fillStyle(kUmber);
        g.beginPath();
        g.circle(x, y, 2.2);
        g.fill();
    }

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
        g.fillText("Umber", 52, 37);
        const double tw = g.measureText("Umber");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("tape machine", 60 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 800, 23, 150, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 1030, 23, 150, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the machine's face -------------------------------------------------------------------

    // A reel: its flange with three windows, the pack of tape at its radius,
    // the hub; turned by `angle`.
    void drawReel(Canvas& g, double x, double y, double pack, double angle, const char* name) {
        g.fillStyle(kInk);
        g.globalAlpha(0.55);
        g.beginPath();
        g.circle(x + 3, y + 5, reelR);
        g.fill();
        g.globalAlpha(1.0);
        // The pack, seen through the flange.
        g.radialGradient(x, y, 18, pack, 0x5B3A25, kOxide);
        g.beginPath();
        g.circle(x, y, pack);
        g.fill();
        g.strokeStyle(0x8E5C3C);
        g.lineWidth(0.6);
        g.globalAlpha(0.45);
        for (int r = 26; r < pack; r += 6) {
            g.beginPath();
            g.circle(x, y, r);
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The flange: a metal disc with windows onto the pack.
        g.save();
        g.translate(x, y);
        g.rotate(angle);
        g.fillStyle(0x6F655B);
        g.globalAlpha(0.92);
        for (int k = 0; k < 3; ++k) {
            const double a = k * twoPi / 3.0;
            g.beginPath();
            g.moveTo(cos(a - 0.26) * 19, sin(a - 0.26) * 19);
            g.arc(0, 0, reelR, a - 0.48, a + 0.48);
            g.lineTo(cos(a + 0.26) * 19, sin(a + 0.26) * 19);
            g.closePath();
            g.fill();
        }
        g.globalAlpha(1.0);
        g.strokeStyle(0x9C9083);
        g.lineWidth(1.5);
        g.beginPath();
        g.circle(0, 0, reelR);
        g.stroke();
        g.fillStyle(0x8C8073);
        g.beginPath();
        g.circle(0, 0, 19);
        g.fill();
        g.fillStyle(kInk);
        for (int k = 0; k < 3; ++k) {
            const double a = pi / 3.0 + k * twoPi / 3.0;
            g.beginPath();
            g.roundRect(cos(a) * 8 - 3, sin(a) * 8 - 3, 6, 6, 2);
            g.fill();
        }
        g.restore();
        g.fillStyle(kCream);
        g.beginPath();
        g.circle(x, y, 5);
        g.fill();
        g.fillStyle(Theme::textMuted);
        g.boldFont(8);
        g.textAlign(TextAlign::Center);
        g.fillText(name, x, y + reelR + 14);
    }

    // A head: a block the tape wraps, its gap a line at the top.
    void drawHead(Canvas& g, double x, const char* name, bool lit) {
        g.linearGradient(0, tapeY, 0, tapeY + 24, 0x8C8073, 0x4A4239);
        g.beginPath();
        g.roundRect(x - 12, tapeY + 2, 24, 20, 4);
        g.fill();
        g.fillStyle(lit ? kUmber : kInk);
        g.fillRect(x - 0.6, tapeY + 2, 1.2, 7);
        label(g, name, x, tapeY + 29, TextAlign::Center);
    }

    // A VU meter: a cream face, the scale in volts (as VU meters are), a red
    // zone above 0 VU, the needle.
    double vuAngle(double db) const {
        const double v = dbToGain(clamp(db, -22.0, 3.5));
        const double t = (v - 0.1) / (dbToGain(3.0) - 0.1);
        return -pi * 0.5 - 0.82 + 1.64 * t;
    }

    void drawMeter(Canvas& g, double x, double y, double needle, const char* name) {
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x - 4, y - 4, meterW + 8, meterH + 8, 8);
        g.fill();
        g.linearGradient(0, y, 0, y + meterH, kCream, 0xD9CBB0);
        g.beginPath();
        g.roundRect(x, y, meterW, meterH, 5);
        g.fill();
        const double cx = x + meterW * 0.5, cy = y + meterH + 31, r = 120;
        g.save();
        g.clip(x, y, meterW, meterH);
        // The red zone.
        g.strokeStyle(0xB8452A);
        g.lineWidth(5);
        g.beginPath();
        g.arc(cx, cy, r - 3, vuAngle(0.0), vuAngle(3.0));
        g.stroke();
        g.strokeStyle(0x3B2F26);
        g.lineWidth(1.2);
        g.beginPath();
        g.arc(cx, cy, r, vuAngle(-20.0), vuAngle(0.0));
        g.stroke();
        Array<double, 11> ticks = {-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3};
        for (int k = 0; k < 11; ++k) {
            const double a = vuAngle(ticks[k]);
            const bool red = ticks[k] > 0.0;
            g.strokeStyle(red ? 0xB8452A : 0x3B2F26);
            g.lineWidth(1.2);
            g.beginPath();
            g.moveTo(cx + cos(a) * r, cy + sin(a) * r);
            g.lineTo(cx + cos(a) * (r + 7), cy + sin(a) * (r + 7));
            g.stroke();
            g.fillStyle(red ? 0xB8452A : 0x3B2F26);
            g.font(8.5);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            const double lx2 = cx + cos(a) * (r + 15), ly2 = cy + sin(a) * (r + 15);
            if (ticks[k] == 0.0)
                g.fillText("0", lx2, ly2);
            else if (ticks[k] > 0.0)
                g.fillText(format("+%.0f", ticks[k]), lx2, ly2);
            else if (ticks[k] != -1.0 && ticks[k] != -2.0)
                g.fillText(format("%.0f", -ticks[k]), lx2, ly2);
        }
        g.fillStyle(0x3B2F26);
        g.serifItalicFont(14);
        g.fillText("VU", cx, y + meterH - 34);
        g.boldFont(8);
        g.fillText(name, cx, y + meterH - 18);
        // The needle.
        const double a = vuAngle(needle);
        g.strokeStyle(0x1E1813);
        g.lineWidth(1.4);
        g.beginPath();
        g.moveTo(cx + cos(a) * 36, cy + sin(a) * 36);
        g.lineTo(cx + cos(a) * (r + 5), cy + sin(a) * (r + 5));
        g.stroke();
        g.restore();
        // The pivot's cover.
        g.fillStyle(0x3B2F26);
        g.fillRect(x, y + meterH - 9, meterW, 9);
        g.fillStyle(0x2A221C);
        g.fillRect(x, y + meterH - 9, meterW, 1);
    }

    void drawFace(Canvas& g) {
        panel(g, fx, fy, fw, fh);
        const double metres = tapeMetres.get();
        // The reels: a 30-minute reel's tape, wound from left to right.
        const double whole = 686.0, hub = 22.0, full = 68.0;
        const double u = fmod(metres, whole) / whole;
        const double left = sqrt(full * full - (full * full - hub * hub) * u);
        const double right = sqrt(hub * hub + (full * full - hub * hub) * u);
        // Each turns at the tape's speed over its pack's radius (a pack of 68
        // drawn units standing for 13 cm).
        const double scale = 0.13 / full;
        const double leftAngle = metres / (left * scale);
        const double rightAngle = metres / (right * scale);
        // The tape's path: off the left pack, round a guide, over the heads,
        // between the capstan and its roller, round a guide, onto the right.
        g.strokeStyle(kOxide);
        g.lineWidth(3);
        g.lineCap(LineCap::Round);
        g.beginPath();
        g.moveTo(leftReelX - left * 0.2, reelY + left * 0.98);
        g.lineTo(226, tapeY - 6);
        g.lineTo(820, tapeY - 2);
        g.lineTo(930, tapeY - 2);
        g.lineTo(974, tapeY - 6);
        g.lineTo(rightReelX + right * 0.2, reelY + right * 0.98);
        g.stroke();
        drawReel(g, leftReelX, reelY, left, -leftAngle, "SUPPLY");
        drawReel(g, rightReelX, reelY, right, -rightAngle, "TAKE-UP");
        // Guides, the heads, the capstan and its pinch roller.
        g.fillStyle(0x8C8073);
        g.beginPath();
        g.circle(226, tapeY - 12, 7);
        g.fill();
        g.beginPath();
        g.circle(974, tapeY - 12, 7);
        g.fill();
        const bool lit = running.get();
        drawHead(g, 262, "ERASE", false);
        drawHead(g, 314, "RECORD", lit);
        drawHead(g, 366, "PLAY", lit);
        g.fillStyle(0xB5AA9C);
        g.beginPath();
        g.circle(900, tapeY - 9, 7);
        g.fill();
        g.fillStyle(0x2E2620);
        g.beginPath();
        g.circle(900, tapeY + 9, 11);
        g.fill();
        label(g, "CAPSTAN", 912, tapeY + 29, TextAlign::Left);
        // The settings, as a tape box's label would say them.
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        const int s = speed.index();
        const char* eq = emphasis.index() == 0 ? "NAB" : "IEC";
        if (s == 0) eq = "AES";
        g.fillText(s == 0 ? "30 ips" : (s == 1 ? "15 ips" : (s == 2 ? "7.5 ips" : "3.75 ips")), 812,
                   104);
        g.fillText(eq, 812, 120);
        g.fillText(format("%.1f dB tape", tape.value()), 812, 136);
        g.fillStyle(Theme::textFaint);
        g.fillText(format("%.0fx", timesOf(oversampling.index())), 812, 152);
        g.fillText(latency.index() == 0 ? "full" : "low latency", 846, 152);
        // The meters (their ballistics are the audio code's).
        drawMeter(g, meterLX, meterY, vuLeft.get(), "LEFT");
        drawMeter(g, meterRX, meterY, vuRight.get(), "RIGHT");
        // The peak lamp: the loudest recent peak over the tape's 3% limit.
        const bool over = headroom.value() < 0.0;
        g.fillStyle(over ? Theme::alert : 0x3A2B22);
        g.beginPath();
        g.circle(meterLX - 24, meterY + 12, 5);
        g.fill();
        if (over) {
            g.globalAlpha(0.3);
            g.beginPath();
            g.circle(meterLX - 24, meterY + 12, 10);
            g.fill();
            g.globalAlpha(1.0);
        }
        label(g, "PEAK", meterLX - 24, meterY + 28, TextAlign::Center);
    }

    // --- the knobs ----------------------------------------------------------------------------

    // 0 drive, 1 tape, 2 bias; 3 record amp, 4 HF saturation, 5 record HF,
    // 6 output; 7 head bump, 8 bump freq, 9 azimuth, 10 head magnetism;
    // 11 dropouts, 12 drop depth, 13 print-through, 14 crosstalk; 15 repro
    // amp, 16 repro HF, 17 hiss, 18 mod noise; 19 wow, 20 flutter, 21 scrape,
    // 22 hum.
    double knobX(int k) const {
        if (k == 0) return leftX + 76;
        if (k == 1) return leftX + 196;
        if (k == 2) return leftX + 306;
        const int col = k < 7 ? k - 3 : (k < 15 ? (k - 7) % 4 : (k - 15) % 4);
        return (k >= 15 ? rightX : leftX) + 52 + col * 92;
    }
    double knobY(int k) const {
        if (k < 3) return topY + 88;
        if (k < 7) return topY + 198;
        if (k < 11) return lowY + 82;
        if (k < 15) return lowY + 186;
        if (k < 19) return topY + 262;
        return lowY + 82;
    }
    double knobR(int k) const {
        if (k == 0) return 34;
        if (k < 3) return 27;
        return 22;
    }

    // A knob's parameter, for the few things every knob does.
    double knobValue(int k) const {
        if (k == 0) return drive.normalized();
        if (k == 1) return tape.normalized();
        if (k == 2) return bias.normalized();
        if (k == 3) return recordAmp.normalized();
        if (k == 4) return hfSaturation.normalized();
        if (k == 5) return recordHf.normalized();
        if (k == 6) return output.normalized();
        if (k == 7) return headBump.normalized();
        if (k == 8) return bumpFreq.normalized();
        if (k == 9) return azimuth.normalized();
        if (k == 10) return headMagnetism.normalized();
        if (k == 11) return dropouts.normalized();
        if (k == 12) return dropDepth.normalized();
        if (k == 13) return printThrough.normalized();
        if (k == 14) return crosstalk.normalized();
        if (k == 15) return reproAmp.normalized();
        if (k == 16) return reproHf.normalized();
        if (k == 17) return hiss.normalized();
        if (k == 18) return modNoise.normalized();
        if (k == 19) return wow.normalized();
        if (k == 20) return flutter.normalized();
        if (k == 21) return scrape.normalized();
        return hum.normalized();
    }

    // how: 1 a knob position (0..1), 2 the default.
    void setOne(const Param& p, int how, double v) {
        if (how == 1)
            p.editNormalized(v);
        else
            p.reset();
    }
    void setKnob(int k, int how, double v) {
        if (k == 0) setOne(drive, how, v);
        if (k == 1) setOne(tape, how, v);
        if (k == 2) setOne(bias, how, v);
        if (k == 3) setOne(recordAmp, how, v);
        if (k == 4) setOne(hfSaturation, how, v);
        if (k == 5) setOne(recordHf, how, v);
        if (k == 6) setOne(output, how, v);
        if (k == 7) setOne(headBump, how, v);
        if (k == 8) setOne(bumpFreq, how, v);
        if (k == 9) setOne(azimuth, how, v);
        if (k == 10) setOne(headMagnetism, how, v);
        if (k == 11) setOne(dropouts, how, v);
        if (k == 12) setOne(dropDepth, how, v);
        if (k == 13) setOne(printThrough, how, v);
        if (k == 14) setOne(crosstalk, how, v);
        if (k == 15) setOne(reproAmp, how, v);
        if (k == 16) setOne(reproHf, how, v);
        if (k == 17) setOne(hiss, how, v);
        if (k == 18) setOne(modNoise, how, v);
        if (k == 19) setOne(wow, how, v);
        if (k == 20) setOne(flutter, how, v);
        if (k == 21) setOne(scrape, how, v);
        if (k == 22) setOne(hum, how, v);
    }

    // Where a knob's arc starts: the middle for the two-sided ones.
    double knobFrom(int k) const {
        if (k == 0 || k == 2 || k == 4 || k == 5 || k == 6 || k == 10 || k == 16) return 0.5;
        if (k == 7) return headBump.value() >= 1.0 ? 0.5 : 0.0;
        return 0.0;
    }

    void knobLabel(Canvas& g, int k, double x, double y) {
        const char* name = "HUM";
        if (k == 0) name = "DRIVE";
        if (k == 1) name = "TAPE";
        if (k == 2) name = "BIAS";
        if (k == 3) name = "RECORD AMP";
        if (k == 4) name = "HF SATURATION";
        if (k == 5) name = "RECORD HF";
        if (k == 6) name = "OUTPUT";
        if (k == 7) name = "HEAD BUMP";
        if (k == 8) name = "BUMP FREQ";
        if (k == 9) name = "AZIMUTH";
        if (k == 10) name = "HEAD MAGNETISM";
        if (k == 11) name = "DROPOUTS";
        if (k == 12) name = "DROP DEPTH";
        if (k == 13) name = "PRINT-THROUGH";
        if (k == 14) name = "CROSSTALK";
        if (k == 15) name = "REPRO AMP";
        if (k == 16) name = "REPRO HF";
        if (k == 17) name = "HISS";
        if (k == 18) name = "MOD NOISE";
        if (k == 19) name = "WOW";
        if (k == 20) name = "FLUTTER";
        if (k == 21) name = "SCRAPE";
        g.fillText(name, x, y);
    }

    // A knob that is switched off (at its minimum) or does not apply.
    bool knobDim(int k) const {
        if (k == 8) return headBump.value() <= 0.0;
        if (k == 12) return dropouts.value() <= 0.0;
        if (k == 13) return printThrough.value() <= -80.0;
        if (k == 14) return crosstalk.value() <= -80.0;
        if (k == 17) return hiss.value() <= -30.0;
        if (k == 18) return modNoise.value() <= -30.0;
        if (k == 22) return hum.value() <= -100.0;
        return false;
    }

    void signedDb(Canvas& g, double db, double x, double y) {
        if (abs(db) < 0.05)
            g.fillText("0.0 dB", x, y);
        else
            g.fillText(format("%+.1f dB", db), x, y);
    }

    void knobText(Canvas& g, int k, double x, double y) {
        if (k == 0 || k == 5 || k == 6 || k == 16) {
            double db = drive.value();
            if (k == 5) db = recordHf.value();
            if (k == 6) db = output.value();
            if (k == 16) db = reproHf.value();
            signedDb(g, db, x, y);
        } else if (k == 1) {
            g.fillText(format("%.1f dB", tape.value()), x, y);
        } else if (k == 8) {
            g.fillText(format("%.0f Hz", bumpFreq.value()), x, y);
        } else if (k == 9) {
            if (azimuth.value() <= 0.0)
                g.fillText("aligned", x, y);
            else
                g.fillText(format("%.3f deg", azimuth.value()), x, y);
        } else if (k == 10) {
            g.fillText(format("%+.0f%%", headMagnetism.value() * 100.0), x, y);
        } else if (k == 13 || k == 14 || k == 22) {
            double db = printThrough.value();
            if (k == 14) db = crosstalk.value();
            if (k == 22) db = hum.value();
            if (knobDim(k))
                g.fillText("off", x, y);
            else
                g.fillText(format("%.0f dB", db), x, y);
        } else if (k == 17 || k == 18) {
            const double db = k == 17 ? hiss.value() : modNoise.value();
            if (db <= -30.0)
                g.fillText("off", x, y);
            else if (abs(db) < 0.5)
                g.fillText("real", x, y);
            else
                g.fillText(format("%+.0f dB", db), x, y);
        } else if (k == 19 || k == 20) {
            g.fillText(format("%.3f%%", (k == 19 ? wow.value() : flutter.value()) * 100.0), x, y);
        } else {
            double v = bias.value();
            if (k == 3) v = recordAmp.value();
            if (k == 4) v = hfSaturation.value();
            if (k == 7) v = headBump.value();
            if (k == 11) v = dropouts.value();
            if (k == 12) v = dropDepth.value();
            if (k == 15) v = reproAmp.value();
            if (k == 21) v = scrape.value();
            g.fillText(format("%.0f%%", v * 100.0), x, y);
        }
    }

    void drawKnob(Canvas& g, int k) {
        int colour = kUmber;
        if (k == 2 && bias.value() < 0.995) colour = kRust;
        if (k == 2 && bias.value() > 1.005) colour = kMoss;
        if (k >= 19 && k <= 21) colour = kMoss;
        const bool dim = knobDim(k);
        if (dim) colour = 0x6B5A48;
        g.globalAlpha(dim ? 0.6 : 1.0);
        dial(g, knobX(k), knobY(k), knobR(k), knobValue(k), knobFrom(k), colour, hotKnob == k);
        knobLabel(g, k, knobX(k), knobY(k) - knobR(k) - 12);
        valueStyle(g, k == 0 ? 12 : 10);
        knobText(g, k, knobX(k), knobY(k) + knobR(k) + 12);
        g.globalAlpha(1.0);
    }

    // --- record and heads ---------------------------------------------------------------------

    void drawRecord(Canvas& g) {
        panel(g, leftX, topY, sideW, topH);
        heading(g, "Record", leftX + 16, topY + 18);
        aside(g, "how hard, onto what, through what", leftX + sideW - 16, topY + 18,
              TextAlign::Right);
        for (int k = 0; k < 7; ++k) drawKnob(g, k);
        // Readouts: the headroom left and how much the peaks were rounded.
        const double ry = topY + 248;
        for (int k = 0; k < 2; ++k) {
            const double x = leftX + 16 + k * 116;
            g.fillStyle(Theme::recess);
            g.beginPath();
            g.roundRect(x, ry, 108, 44, 6);
            g.fill();
            label(g, k == 0 ? "HEADROOM" : "ROUNDED", x + 10, ry + 12, TextAlign::Left);
            g.fillStyle(Theme::text);
            g.monoFont(14);
            g.textAlign(TextAlign::Left);
            g.textBaseline(TextBaseline::Middle);
            if (k == 0) {
                const double h = headroom.value();
                if (h >= 47.9)
                    g.fillText("-", x + 10, ry + 30);
                else {
                    g.fillStyle(h < 0.0 ? Theme::alert : Theme::text);
                    g.fillText(format("%+.1f dB", h), x + 10, ry + 30);
                }
            } else {
                g.fillText(format("%.2f dB", saturation.value()), x + 10, ry + 30);
            }
        }
        pill(g, leftX + 252, ry + 11, 112, 22, "AUTO GAIN", autoGain.on(), kUmber,
             hovering(leftX + 252, ry + 11, 112, 22));
    }

    void drawHeads(Canvas& g) {
        panel(g, leftX, lowY, sideW, lowH);
        heading(g, "Heads and tape", leftX + 16, lowY + 18);
        aside(g, "alignment, wear and the reel", leftX + sideW - 16, lowY + 18, TextAlign::Right);
        for (int k = 7; k < 15; ++k) drawKnob(g, k);
    }

    // --- the scope ----------------------------------------------------------------------------

    void drawScope(Canvas& g) {
        panel(g, mx, topY, mw, mh);
        heading(g, "Magnetism", mx + 16, topY + 18);
        aside(g, "the field recorded against the flux played", mx + mw - 16, topY + 18,
              TextAlign::Right);
        well(g, lx, ly, ls, ls);
        // The trail's extent sets each axis's scale on its own, so the shape
        // fills the glass at any level: up at once, down over half a second.
        const int head = scopeHead.get();
        double mostH = 0.0, mostM = 0.0;
        for (int j = 0; j < kShown; ++j) {
            const int idx = wrap(head - 1 - j, kScope);
            mostH = max(mostH, abs(static_cast<double>(scopeH[idx])));
            mostM = max(mostM, abs(static_cast<double>(scopeM[idx])));
        }
        const double wantH = max(0.35, mostH * 1.18), wantM = max(0.35, mostM * 1.18);
        spanH = wantH > spanH ? wantH : spanH + 0.06 * (wantH - spanH);
        spanM = wantM > spanM ? wantM : spanM + 0.06 * (wantM - spanM);
        const double cx = lx + ls * 0.5, cy = ly + ls * 0.5;
        const double sx = (ls * 0.5 - 6) / spanH, sy = (ls * 0.5 - 6) / spanM;
        g.save();
        g.clip(lx, ly, ls, ls);
        // A graticule, and the axes.
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        for (int k = 1; k < 8; ++k) {
            g.moveTo(lx + ls * k / 8.0, ly);
            g.lineTo(lx + ls * k / 8.0, ly + ls);
            g.moveTo(lx, ly + ls * k / 8.0);
            g.lineTo(lx + ls, ly + ls * k / 8.0);
        }
        g.stroke();
        g.strokeStyle(kGridLit);
        g.beginPath();
        g.moveTo(lx, cy);
        g.lineTo(lx + ls, cy);
        g.moveTo(cx, ly);
        g.lineTo(cx, ly + ls);
        g.stroke();
        // The reference level and the tape's 3% limit, when the field
        // reaches them.
        const double mol = dbToGain(tape.value());
        for (int side = -1; side <= 1; side += 2) {
            if (spanH > 1.0) {
                g.strokeStyle(kGridLit);
                g.globalAlpha(0.9);
                g.beginPath();
                g.moveTo(cx + side * sx, ly + 6);
                g.lineTo(cx + side * sx, ly + ls - 6);
                g.stroke();
            }
            if (spanH > mol) {
                g.strokeStyle(kRust);
                g.globalAlpha(0.55);
                g.beginPath();
                g.moveTo(cx + side * mol * sx, ly + 6);
                g.lineTo(cx + side * mol * sx, ly + ls - 6);
                g.stroke();
            }
        }
        g.globalAlpha(1.0);
        if (spanH > 1.0) label(g, "0 VU", cx + sx + 4, ly + 14, TextAlign::Left);
        if (spanH > mol) {
            g.fillStyle(kRust);
            g.boldFont(8.5);
            g.textAlign(TextAlign::Left);
            g.fillText("3%", cx + mol * sx + 4, ly + 28);
        }
        // The trail, oldest to newest, brightening: drawn as straight lines
        // between points 8 kHz apart, so the highs draw stars and rays.
        const double gone = clamp((idle.get() - 0.4) / 1.2, 0.0, 1.0);
        const double shine = 1.0 - 0.8 * gone;
        constexpr int kRuns = 24;
        const int per = kShown / kRuns;
        g.lineCap(LineCap::Round);
        g.lineJoin(LineJoin::Round);
        g.strokeStyle(kTrace);
        for (int r = 0; r < kRuns; ++r) {
            const double t = r / (kRuns - 1.0);
            g.globalAlpha((0.05 + 0.6 * t * t) * shine);
            g.lineWidth(1.0 + 0.6 * t);
            g.beginPath();
            for (int j = 0; j <= per; ++j) {
                if (r * per + j >= kShown) break;
                const int idx = wrap(head - kShown + r * per + j, kScope);
                const double x = cx + scopeH[idx] * sx, y = cy - scopeM[idx] * sy;
                if (j == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        // Where the tape is now.
        const int last = wrap(head - 1, kScope);
        const double nx = cx + scopeH[last] * sx, ny = cy - scopeM[last] * sy;
        g.fillStyle(kCream);
        g.globalAlpha(0.25 * shine);
        g.beginPath();
        g.circle(nx, ny, 6);
        g.fill();
        g.globalAlpha(shine);
        g.beginPath();
        g.circle(nx, ny, 2.4);
        g.fill();
        g.globalAlpha(1.0);
        g.restore();
        label(g, "FIELD", lx + 8, ly + ls - 12, TextAlign::Left);
        label(g, "FLUX", lx + 8, ly + 12, TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Right);
        g.fillText(format("%+.0f dB", gainToDb(spanH)), lx + ls - 8, ly + ls - 12);
        g.fillText(format("%+.0f dB", gainToDb(spanM)), lx + ls - 8, ly + 12);
        const double b = bias.value();
        const char* about = "biased: the oxide follows the curve, and only the peaks bend";
        if (b < 0.9) about = "underbiased: the loop opens, and quiet sounds turn grainy";
        if (b > 1.1) about = "overbiased: cleaner, with less top";
        aside(g, about, mx + mw * 0.5, ly + ls + 18, TextAlign::Center);
        aside(g, "drag: across for drive, up and down for bias", mx + mw * 0.5, ly + ls + 34,
              TextAlign::Center);
        drawQuality(g);
    }

    // Oversampling and latency, under the scope.
    void drawQuality(Canvas& g) {
        const double qy = ly + ls + 54;
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(mx + 16, qy);
        g.lineTo(mx + mw - 16, qy);
        g.stroke();
        label(g, "OVERSAMPLING", mx + 16, qy + 16, TextAlign::Left);
        const int os = oversampling.index();
        for (int k = 0; k < 5; ++k) {
            const double x = mx + 16 + k * 70;
            pill(g, x, qy + 26, 64, 22,
                 k == 0 ? "1X" : (k == 1 ? "2X" : (k == 2 ? "4X" : (k == 3 ? "8X" : "16X"))),
                 os == k, kMoss, hovering(x, qy + 26, 64, 22));
        }
        const double r = sampleRateNow.get() > 1000.0 ? sampleRateNow.get() : 48000.0;
        const double inside = r * timesOf(os) / 1000.0;
        const bool good = inside >= 170.0 && inside <= 400.0;
        g.fillStyle(good ? Theme::textMuted : kRust);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("tape at %.0f kHz", inside), mx + 16, qy + 62);
        aside(g, "best 176 to 384 kHz", mx + mw - 16, qy + 62, TextAlign::Right);
        label(g, "LATENCY", mx + 16, qy + 86, TextAlign::Left);
        pill(g, mx + 86, qy + 75, 72, 22, "FULL", latency.index() == 0, kMoss,
             hovering(mx + 86, qy + 75, 72, 22));
        pill(g, mx + 164, qy + 75, 72, 22, "LOW", latency.index() == 1, kMoss,
             hovering(mx + 164, qy + 75, 72, 22));
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        const int n = latencyNow.get();
        g.fillText(format("%d samples, %.1f ms", n, n * 1000.0 / r), mx + mw - 16, qy + 86);
    }

    // --- playback and transport ---------------------------------------------------------------

    // The fundamental's gain of a sine of amplitude a through the curve.
    double describe(double a, double sq) const {
        if (a < 1e-6) return 1.0;
        double sum = 0.0;
        for (int k = 0; k < 24; ++k) {
            const double th = (k + 0.5) * pi * 0.5 / 24.0;
            sum += anhysteretic(a * sin(th), sq) * sin(th);
        }
        return sum * (pi * 0.5 / 24.0) * 4.0 / (pi * a);
    }

    // The edge's gain for a sine of `level` reference peaks: the lows less
    // the highs, the highs at their describing gain d through the tanh,
    // (1 + d r^2) / (1 + j r)^2 with r the frequency over the split.
    double edgeGain(double hz, double splitHz, double liftHz, double level, double ceiling) const {
        const double r = hz / splitHz;
        const double den = 1.0 + r * r;
        const double lifted = level * r * r / den * sqrt(1.0 + hz * hz / (liftHz * liftHz));
        const double d = describe(lifted / ceiling, 0.0);
        return (1.0 + d * r * r) / den;
    }

    double responseY(double db) const { return dbToY(db, -12.0, 6.0, gy, gh); }

    void drawResponse(Canvas& g) {
        well(g, gx, gy, gw, gh);
        g.save();
        g.clip(gx, gy, gw, gh);
        g.frequencyGrid(gx, gy, gw, gh, 20, 20000);
        g.strokeStyle(kGridLit);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(gx, responseY(0.0));
        g.lineTo(gx + gw, responseY(0.0));
        g.stroke();
        const int s = speed.index();
        const bool nab = emphasis.index() == 0 && s != 0;
        const double r = sampleRateNow.get() > 1000.0 ? sampleRateNow.get() : 48000.0;
        const double hiRate = r * (timesOf(oversampling.index()));
        Formulation f;
        f.set(tape.value());
        view.design(nab, hiRate, false);
        const double ceiling = edgeFor(s, nab, f.loss, hfSaturation.value());
        const double v = speedIps(s) / 15.0;
        shelf(viewTrim, recordHf.value(), min(4000.0 * v, 0.3 * hiRate), hiRate);
        viewHead.design(s, f.loss, headBump.value(), bumpFreq.value(), biasTiltDb(s, bias.value()),
                        reproHf.value(), r);
        double azWindow = azimuthWindow(azimuth.value(), speedIps(s));
        double azSpread = 0.5 * azimuthSpread(azimuth.value(), speedIps(s));
        if (readReach.get() > 0.0) capAzimuth(azSpread, azWindow, readReach.get());
        const double fieldRef = f.field * dbToGain(speedFieldDb(s));
        const double levelDb = biasLevelDb(bias.value());
        for (int pass = 0; pass < 2; ++pass) {
            // Quiet (-20 VU), then hot (+10 VU): the highs meet their
            // ceiling and saturate first.
            const double level = dbToGain(pass == 0 ? -20.0 : 10.0);
            const double amp = fieldRef * level;
            g.strokeStyle(pass == 0 ? Theme::textMuted : kUmber);
            g.lineWidth(pass == 0 ? 1.4 : 2.0);
            g.beginPath();
            for (int i = 0; i <= 96; ++i) {
                const double hz = 20.0 * pow(1000.0, i / 96.0);
                const double e = view.gainAt(hz, hiRate);
                const double trim = viewTrim.gainAt(hz, hiRate);
                const double core = describe(amp * e * trim, f.square);
                const double edge =
                    edgeGain(hz, 5000.0 * v, 9000.0 * v, level * trim * core, ceiling);
                const double db = viewHead.responseDb(hz, r) + azimuthDb(hz, azWindow) + levelDb +
                                  gainToDb(trim) + gainToDb(core * edge);
                const double x = hzToX(hz, 20, 20000, gx, gw), y = responseY(db);
                if (i == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        g.restore();
        g.fillStyle(Theme::textMuted);
        g.boldFont(8);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("-20 VU", gx + gw - 52, gy + 10);
        g.fillStyle(kUmber);
        g.fillText("+10 VU", gx + gw - 8, gy + 10);
    }

    void drawPlayback(Canvas& g) {
        panel(g, rightX, topY, sideW, topH);
        heading(g, "Playback", rightX + 16, topY + 18);
        aside(g, "the speed, the standard, the head", rightX + sideW - 16, topY + 18,
              TextAlign::Right);
        const int s = speed.index();
        for (int k = 0; k < 4; ++k) {
            const double x = rightX + 16 + k * 88;
            pill(g, x, topY + 38, 82, 22,
                 k == 0 ? "30 IPS" : (k == 1 ? "15 IPS" : (k == 2 ? "7.5 IPS" : "3.75 IPS")),
                 s == k, kUmber, hovering(x, topY + 38, 82, 22));
        }
        g.globalAlpha(s == 0 ? 0.45 : 1.0);
        pill(g, rightX + 16, topY + 70, 82, 22, "NAB", emphasis.index() == 0 && s != 0, kUmber,
             s != 0 && hovering(rightX + 16, topY + 70, 82, 22));
        pill(g, rightX + 104, topY + 70, 82, 22, s == 0 ? "AES" : "IEC",
             emphasis.index() == 1 || s == 0, kUmber,
             s != 0 && hovering(rightX + 104, topY + 70, 82, 22));
        g.globalAlpha(1.0);
        const double tau = speedTau(s, emphasis.index() == 0 && s != 0) * 1e6;
        g.fillStyle(Theme::textMuted);
        g.monoFont(9.5);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%.1f us, bump %.0f Hz", tau, bumpFreq.value() * speedIps(s) / 15.0),
                   rightX + sideW - 16, topY + 81);
        drawResponse(g);
        for (int k = 15; k < 19; ++k) drawKnob(g, k);
    }

    void drawTransport(Canvas& g) {
        panel(g, rightX, lowY, sideW, lowH);
        heading(g, "Transport", rightX + 16, lowY + 18);
        aside(g, "the tape's motion, the mains", rightX + sideW - 16, lowY + 18, TextAlign::Right);
        for (int k = 19; k < 23; ++k) drawKnob(g, k);
        // The tape's displacement from where it should be, over four seconds.
        well(g, tx, ty, tw, th);
        const int head = trailHead.get();
        double most = 0.0;
        for (int j = 0; j < kTrail; ++j) most = max(most, abs(static_cast<double>(trail[j])));
        const double span = max(most * 1.2, 0.5);
        g.save();
        g.clip(tx, ty, tw, th);
        g.strokeStyle(kGridLit);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(tx, ty + th * 0.5);
        g.lineTo(tx + tw, ty + th * 0.5);
        g.stroke();
        g.strokeStyle(kMoss);
        g.lineWidth(1.4);
        g.beginPath();
        for (int j = 0; j < kTrail; ++j) {
            const int idx = wrap(head + j, kTrail);
            const double x = tx + tw * j / (kTrail - 1.0);
            const double y = ty + th * 0.5 - trail[idx] / span * (th * 0.5 - 6);
            if (j == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.restore();
        label(g, "DISPLACEMENT", tx + 8, ty + 12, TextAlign::Left);
        g.fillStyle(Theme::textFaint);
        g.monoFont(9);
        g.textAlign(TextAlign::Right);
        g.fillText(format("%.0f us", span), tx + tw - 8, ty + 12);
        const double held = heldWow.get();
        if (held < 0.995) {
            g.fillStyle(kRust);
            g.fillText(format("wow held to %.0f%%", held * 100.0), tx + tw - 8, ty + th - 12);
        }
        // The mains, for the hum.
        const bool humOn = hum.value() > -100.0;
        label(g, "MAINS", rightX + 324, ty + 10, TextAlign::Center);
        g.globalAlpha(humOn ? 1.0 : 0.45);
        pill(g, rightX + 292, ty + 24, 64, 22, "50 HZ", mains.index() == 0, kUmber,
             hovering(rightX + 292, ty + 24, 64, 22));
        pill(g, rightX + 292, ty + 54, 64, 22, "60 HZ", mains.index() == 1, kUmber,
             hovering(rightX + 292, ty + 54, 64, 22));
        g.globalAlpha(1.0);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawFace(g);
        drawRecord(g);
        drawHeads(g);
        drawScope(g);
        drawPlayback(g);
        drawTransport(g);
    }

    // --- the pointer --------------------------------------------------------------------------

    int knobAt(double x, double y) const {
        for (int k = 0; k < kKnobs; ++k) {
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
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            } else if (dragging == 2) {
                drive.edit(drive.value() + p.dx() * fine * 0.12);
                bias.edit(bias.value() - p.dy() * fine * 0.003);
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
            if (p.over(lx, ly, ls, ls)) {
                drive.edit(drive.value() + notches * 0.5 * fine);
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void toggleOf(const Param& t) { t.edit(t.on() ? 0 : 1); }

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
        if (p.over(lx, ly, ls, ls)) {
            if (p.doubleClick()) {
                drive.reset();
                bias.reset();
            } else {
                dragging = 2;
            }
            return;
        }
        if (p.over(leftX + 252, topY + 259, 112, 22)) {
            toggleOf(autoGain);
            return;
        }
        for (int m = 0; m < 4; ++m)
            if (p.over(rightX + 16 + m * 88, topY + 38, 82, 22)) {
                speed.edit(m);
                return;
            }
        if (speed.index() != 0) {
            if (p.over(rightX + 16, topY + 70, 82, 22)) emphasis.edit(0);
            if (p.over(rightX + 104, topY + 70, 82, 22)) emphasis.edit(1);
        }
        const double qy = ly + ls + 54;
        for (int m = 0; m < 5; ++m)
            if (p.over(mx + 16 + m * 70, qy + 26, 64, 22)) {
                oversampling.edit(m);
                return;
            }
        if (p.over(mx + 86, qy + 75, 72, 22)) latency.edit(0);
        if (p.over(mx + 164, qy + 75, 72, 22)) latency.edit(1);
        if (p.over(rightX + 292, ty + 24, 64, 22)) mains.edit(0);
        if (p.over(rightX + 292, ty + 54, 64, 22)) mains.edit(1);
    }
};
