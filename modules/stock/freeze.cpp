// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A spectral freeze, poly: holds the sound's spectrum at the moment Freeze
// goes on (or its gate goes high) and plays it on and on, a chord turned
// into an endless pad, a vowel into a drone, a cymbal into a wash. Blur
// smears the spectrum in time, so the live sound turns to a haze and a
// freeze fades in and out instead of cutting; Shift moves every partial up
// or down by semitones; Tilt darkens or brightens by dB per octave.
//
// The sound is cut into overlapping frames of about 85 ms, eight over each
// sample, and each frame's partials are measured for their level and their
// exact frequency. A freeze keeps both and lets the phases run on at those
// frequencies, so a held note stays a steady note rather than a buzz. With
// nothing frozen, shifted or blurred the sound comes back as it went in,
// about 85 ms late; the dry part of Mix is not delayed, so a frozen pad sits
// under live playing as it is played.

// The phase x brought into -pi .. pi.
double wrapPhase(double x) { return x - twoPi * floor((x + pi) / twoPi); }

struct Spectral {
    Stft stft;
    bool high = false;    // the gate, with hysteresis
    bool frozen = false;  // at the latest frame
};

struct Freeze : Module {
    static constexpr ModuleInfo info = {
        .name = "Spectral Freeze",
        .id = "com.suppleaudio.rack.freeze",
        .vendor = "Supple Audio",
        .description =
            "A spectral freeze: holds the spectrum and plays it on; blur, shift by "
            "semitones, tilt.",
        .tags = {"effect", "spectral", "poly"}};

    Param freeze = toggle("Freeze", false)
                       .in("Freeze")
                       .help("Holds the spectrum and plays it on, as the gate does while high.");
    Param blur = percent("Blur", 0)
                     .in("Spectrum")
                     .help(
                         "Smears the spectrum in time, from none to about 4 seconds; a freeze "
                         "fades in and out as slowly.");
    Param shift = semitones("Shift", -24, 24, 0)
                      .in("Spectrum")
                      .help("Moves every partial up or down, frozen or live.");
    Param tilt = decibels("Tilt", -6, 6, 0)
                     .in("Spectrum")
                     .help("dB per octave about 1 kHz: below 0 darker, above 0 brighter.");
    Param mix = percent("Mix", 100)
                    .smoothed(20)
                    .in("Output")
                    .help("The spectral share, the rest the dry input (not delayed).");

    Input in = audioIn("In").help("What to freeze. Poly: each channel alone.");
    Input freezeGate = gateIn("Gate").help(
        "Freezes while high, as Freeze does. A mono cable freezes every channel.");
    Output out = audioOut("Out").help("The spectrum, about 85 ms late, and the dry input by Mix.");

    Array<Spectral, 16> spectral;
    // Per channel and bin, at channel * bins + bin: the phase measured last
    // frame; the bin's (blurred) level and how far its phase runs each
    // frame, which a freeze holds; its phase about the frame's middle, live
    // or running on; how far Shift has turned it; and for
    // finding each partial's peak, whether the bin is one and the last peak
    // at or below it.
    Buffer<double> measured, level, advance, source, turned;
    Buffer<int> isPeak, lastPeak;
    int bins = 0;
    int top = 0;  // the bins below 20 kHz, the only ones changed

    void prepare() {
        const double sr = sampleRate();
        const int size = clamp(roundToInt(exp2(roundToInt(log2(sr * 0.085)))), 1024, 16384);
        for (int c = 0; c < 16; ++c) spectral[c].stft.prepare(size, 8);
        bins = spectral[0].stft.bins();
        top = min(bins, floorToInt(20000.0 * spectral[0].stft.size() / sr) + 1);
        measured.resize(16 * bins);
        level.resize(16 * bins);
        advance.resize(16 * bins);
        source.resize(16 * bins);
        turned.resize(16 * bins);
        isPeak.resize(16 * bins);
        lastPeak.resize(16 * bins);
    }

    // One frame of channel c: measure each bin, then write the spectrum
    // back held, blurred, shifted and tilted.
    void frame(int c, double ratio, double smooth, double tiltDb) {
        Stft& s = spectral[c].stft;
        const bool frozen = spectral[c].frozen;
        const bool still = abs(ratio - 1.0) < 1e-6;
        const double expect = twoPi * s.hop() / s.size();
        const int base = c * bins;
        // Measure, and clear the bins for the shifted partials to add into.
        // Phases are taken about the frame's middle (bin k's turned by k
        // half turns), where a partial's bins share one phase.
        int last = -1;
        for (int k = 0; k < top; ++k) {
            const int at = base + k;
            const double ph = s.phase(k);
            const double step = k * expect + wrapPhase(ph - measured[at] - k * expect);
            measured[at] = ph;
            // Blur, and a freeze (a blur that takes nothing new): the level
            // glides, and the phase is a blend of the live one and the old
            // one run on at the bin's frequency, weighted by their levels,
            // so a fading tail keeps its partials' pitches. The frequency
            // is measured while the live sound is a fair share of the level.
            const double mag = s.magnitude(k);
            const double a = frozen ? 0.0 : smooth;
            if (!frozen && mag > 0.25 * level[at]) advance[at] = step;
            const double run = source[at] + advance[at];
            const double live = ph + pi * (k % 2);
            const double old = (1.0 - a) * level[at];
            source[at] =
                atan2(old * sin(run) + a * mag * sin(live), old * cos(run) + a * mag * cos(live));
            level[at] += a * (mag - level[at]);
            turned[at] = still ? 0.0 : wrapPhase(turned[at] + (ratio - 1.0) * advance[at]);
            s.set(k, 0.0, 0.0);
            isPeak[at] = 0;
            if (k >= 2 && level[at - 1] > level[at - 2] && level[at - 1] >= level[at]) {
                isPeak[at - 1] = 1;
                last = k - 1;
            }
            lastPeak[at] = last;
        }
        // Each partial, its peak's bin and the bins nearer that peak than
        // any other, moves whole to the peak's bin times the ratio, keeping
        // its shape, and turns on by what Shift adds to its frequency, so
        // its bins stay in step and it comes out as loud as it went in.
        int next = -1;
        for (int k = top - 1; k >= 0; --k) {
            const int at = base + k;
            if (isPeak[at] == 1) next = k;
            const int below = isPeak[at] == 1 ? k : lastPeak[at];
            int peak = k;
            if (below >= 0 && next >= 0) peak = k - below <= next - k ? below : next;
            if (below >= 0 && next < 0) peak = below;
            if (below < 0 && next >= 0) peak = next;
            const int to = k + roundToInt(peak * (ratio - 1.0));
            if (to >= 0 && to < top) {
                const double hz = max(s.binHz(to), 20.0);
                const double m = level[at] * pow(10.0, tiltDb * log2(hz / 1000.0) / 20.0);
                const double ph = source[at] + turned[base + peak] - pi * (to % 2);
                s.set(to, s.real(to) + m * cos(ph), s.imag(to) + m * sin(ph));
            }
        }
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int hop = spectral[0].stft.hop();
        for (int c = 0; c < channels(); ++c) {
            Spectral& sp = spectral[c];
            for (int i = 0; i < frames; ++i) {
                const double g = freezeGate[c][i];
                sp.high = sp.high ? g >= 0.25 : g >= 0.5;
                const double x = in[c][i];
                if (sp.stft.push(x)) {
                    sp.frozen = sp.high || freeze.on();
                    const double b = clamp(blur.at(i, c), 0.0, 1.0);
                    // Blur's time constant: none at 0, 10 ms just above, 4 s at 100.
                    const double tau = 0.01 * pow(400.0, b);
                    const double smooth = b <= 0.0 ? 1.0 : 1.0 - exp(-hop / (tau * sr));
                    const double ratio = exp2(clamp(shift.at(i, c), -24.0, 24.0) / 12.0);
                    frame(c, ratio, smooth, clamp(tilt.at(i, c), -6.0, 6.0));
                }
                const double y = sp.stft.pull();
                const double m = clamp(mix.at(i, c), 0.0, 1.0);
                out[c][i] = m * y + (1.0 - m) * x;
            }
        }
    }

    void factoryPatches(Patches& p) {
        p.add("Haze").set(blur, 60);
        p.add("Octave Pad").set(blur, 45).set(shift, 12).set(tilt, -1.5);
        p.add("Dark Drone").set(blur, 70).set(shift, -12).set(tilt, -3);
        p.add("Glass").set(blur, 30).set(shift, 19).set(tilt, 1.5).set(mix, 50);
    }
};
