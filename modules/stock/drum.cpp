// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An analog-style drum voice, poly: one module, many drums. Model picks the
// drum; a trigger into Trigger hits it, Accent (read as it hits) makes the
// hit louder and sharper. Tune moves its pitch (as does the Pitch input, 1
// per octave), Tone its colour, Decay its length, Snap its attack.
//
//   Kick      a sine swept down from high: Snap is the sweep and click,
//             Tone the weight.
//   Snare     two tuned heads and a burst of noise: Tone from body to
//             rattle, Snap the crack.
//   Tom       a sine with a short droop; Tone its height.
//   Closed    six square waves at clashing ratios through a high band:
//   Open hat  metal. Tone the band's height, Snap some sizzle; the open hat
//             rings longer.
//   Clap      noise in a quick flurry of bursts, then a short room.
//   Cowbell   two squares a wide fifth apart through a band, a fast drop
//             then a ring.
//   Rimshot   a high, woody knock with a click.
//
// Env carries the hit's loudness (0 to 1) for ducking or for triggering
// something in time with it.

constexpr double kC4 = 261.6255653005986;

// The hats' oscillators, over their base: square roots and the golden ratio,
// which never line up.
double metalRatio(int k) {
    switch (k) {
        case 0:
            return 1.0;
        case 1:
            return 1.41421356;
        case 2:
            return 1.61803399;
        case 3:
            return 1.93185165;
        case 4:
            return 2.23606798;
        default:
            return 2.64575131;
    }
}

struct Hit {
    Array<Oscillator, 6> metal;
    Biquad band, top;
    Noise noise;
    double phase1 = 0.0, phase2 = 0.0;
    double t = 10.0;     // seconds since the hit
    double amp = 0.0;    // the body's envelope
    double hiss = 0.0;   // the noise's envelope
    double bend = 0.0;   // the pitch envelope
    double force = 1.0;  // 1 plus the accent
    double env = 0.0;    // what Env shows
    bool high = false;
};

struct Drum : Module {
    static constexpr ModuleInfo info = {
        .name = "Drum",
        .id = "com.suppleaudio.rack.drum",
        .vendor = "Supple Audio",
        .description =
            "An analog-style drum voice: kick, snare, tom, hats, clap, cowbell, "
            "rimshot.",
        .tags = {"drum", "source", "poly"}};

    Param model =
        choice("Model",
               {"Kick", "Snare", "Tom", "Closed Hat", "Open Hat", "Clap", "Cowbell", "Rimshot"}, 0)
            .help("Which drum it is.");
    Param tune = semitones("Tune", -24, 24, 0).help("Moves the drum's pitch in semitones.");
    Param tone =
        percent("Tone", 50)
            .help(
                "Its colour: a kick's weight, a snare's body or rattle, a hat's or clap's height.");
    Param decay = percent("Decay", 50).help("How long it rings, from a tight hit to a long tail.");
    Param snap = percent("Snap", 50).help("The attack: a kick's sweep and click, a snare's crack.");
    Param level = decibels("Level", -24, 6, 0).smoothed(10).help("The output level.");

    Input trigger = gateIn("Trigger").help("Hits the drum each time it rises.");
    Input accent = unipolarIn("Accent").help(
        "Read as it hits: 1 makes the hit about 6 dB louder and sharper.");
    Input pitch = pitchIn("Pitch").help("Moves the pitch 1 per octave, on top of Tune.");

    Output out = audioOut("Out").help("The drum.");
    Output env = unipolarOut("Env").help("The hit's loudness, 0 to 1: for ducking or follow-ups.");

    Array<Hit, 16> hit;

    void prepare() {
        for (int c = 0; c < 16; ++c) {
            hit[c].noise.seed(seed() + 53 * c);
            for (int k = 0; k < 6; ++k) hit[c].metal[k].setShape(Oscillator::Shape::Square);
        }
    }

    // The block's filters for channel c's model.
    void voice(int c, int m, double tn, double scale) {
        Hit& h = hit[c];
        const double top = 0.45 * sampleRate();
        if (m == 1) {
            h.top.highpass(min(1200.0 + 3500.0 * tn, top), 0.7);
        } else if (m == 3 || m == 4) {
            h.band.bandpass(min(6500.0 + 5000.0 * tn, top), 1.2);
            h.top.highpass(min(5500.0 + 2000.0 * tn, top), 0.7);
        } else if (m == 5) {
            h.band.bandpass(min((900.0 + 1300.0 * tn) * sqrt(scale), top), 2.2);
        } else if (m == 6) {
            h.band.bandpass(min(2600.0 * scale, top), 1.4);
        } else if (m == 7) {
            h.top.highpass(min(2500.0 + 3000.0 * tn, top), 0.7);
        }
    }

    void process(int frames) {
        const double sr = sampleRate();
        const double dt = 1.0 / sr;
        const int m = model.index();
        for (int c = 0; c < channels(); ++c) {
            Hit& h = hit[c];
            const double tn = clamp(tone.at(0, c), 0.0, 1.0);
            const double scale0 = exp2(tune.at(0, c) / 12.0 + clamp(pitch[c][0], -4.0, 4.0));
            voice(c, m, tn, scale0);
            for (int i = 0; i < frames; ++i) {
                const double g = trigger[c][i];
                if (!h.high && g >= 0.5) {
                    h.high = true;
                    h.t = 0.0;
                    h.amp = 1.0;
                    h.hiss = 1.0;
                    h.bend = 1.0;
                    h.phase1 = 0.0;
                    h.phase2 = 0.0;
                    h.force = 1.0 + clamp(accent[c][i], 0.0, 1.0);
                } else if (h.high && g < 0.25) {
                    h.high = false;
                }
                const double t = h.t;
                h.t = min(h.t + dt, 10.0);
                const double scale = exp2(tune.at(i, c) / 12.0 + clamp(pitch[c][i], -4.0, 4.0));
                const double d = clamp(decay.at(i, c), 0.0, 1.0);
                const double sn = clamp(snap.at(i, c) * (0.75 + 0.25 * h.force), 0.0, 1.0);
                const double white = h.noise.white();
                double y = 0.0;
                if (m == 0) {
                    // Kick.
                    h.amp *= exp(-dt / (0.06 + 1.1 * d * d));
                    h.bend *= exp(-dt / (0.006 + 0.03 * (1.0 - sn)));
                    const double hz =
                        42.0 * scale * (0.75 + 0.5 * tn) * exp2((1.5 + 4.0 * sn) * h.bend);
                    h.phase1 += min(hz * dt, 0.45);
                    h.phase1 -= floor(h.phase1);
                    const double click = t < 0.0015 ? white * sn * 0.6 : 0.0;
                    y = tanh(1.6 * sin(twoPi * h.phase1) * h.amp) + click;
                } else if (m == 1) {
                    // Snare.
                    h.amp *= exp(-dt / (0.03 + 0.12 * d));
                    h.hiss *= exp(-dt / (0.05 + 0.35 * d));
                    h.bend *= exp(-dt / 0.012);
                    const double hz = 180.0 * scale * (1.0 + 0.4 * h.bend);
                    h.phase1 += min(hz * dt, 0.45);
                    h.phase1 -= floor(h.phase1);
                    h.phase2 += min(1.77 * hz * dt, 0.45);
                    h.phase2 -= floor(h.phase2);
                    const double body = 0.6 * sin(twoPi * h.phase1) + 0.4 * sin(twoPi * h.phase2);
                    const double crack = 1.0 + 1.2 * sn * exp(-t / 0.004);
                    const double hissed = h.top.process(white);
                    const double rattle = hissed * h.hiss * crack;
                    y = body * h.amp * (1.1 - 0.8 * tn) + rattle * (0.4 + 0.9 * tn);
                } else if (m == 2) {
                    // Tom.
                    h.amp *= exp(-dt / (0.08 + 0.7 * d));
                    h.bend *= exp(-dt / (0.02 + 0.04 * (1.0 - sn)));
                    const double hz = 95.0 * scale * (0.6 + 0.9 * tn) * exp2(0.7 * h.bend);
                    h.phase1 += min(hz * dt, 0.45);
                    h.phase1 -= floor(h.phase1);
                    const double click = t < 0.002 ? white * sn * 0.3 : 0.0;
                    y = tanh(1.3 * sin(twoPi * h.phase1) * h.amp) + click;
                } else if (m == 3 || m == 4) {
                    // Hats: the metal through a high band.
                    const double ring = m == 3 ? 0.012 + 0.07 * d : 0.12 + 0.9 * d;
                    h.amp *= exp(-dt / ring);
                    double metal = 0.0;
                    for (int k = 0; k < 6; ++k) {
                        h.metal[k].setFrequency(min(410.0 * scale * metalRatio(k), 0.45 * sr));
                        metal += h.metal[k].next();
                    }
                    const double sizzle = metal / 6.0 + white * 0.6 * sn;
                    const double banded = h.band.process(sizzle);
                    const double bright = h.top.process(banded);
                    y = 2.2 * bright * h.amp;
                } else if (m == 5) {
                    // Clap: three bursts 9 ms apart, then the room.
                    const double gap = 0.009 + 0.004 * (1.0 - sn);
                    double e = 0.0;
                    if (t < 3.0 * gap) {
                        const double since = t - gap * floor(t / gap);
                        e = exp(-since / 0.0045);
                    } else {
                        e = exp(-(t - 3.0 * gap) / (0.05 + 0.4 * d));
                    }
                    y = 2.5 * h.band.process(white) * e;
                    h.amp = e;
                } else if (m == 6) {
                    // Cowbell.
                    h.amp *= exp(-dt / (0.06 + 0.5 * d));
                    h.hiss *= exp(-dt / 0.012);
                    h.metal[0].setFrequency(min(545.0 * scale, 0.45 * sr));
                    h.metal[1].setFrequency(min(545.0 * 1.505 * scale, 0.45 * sr));
                    const double first = h.metal[0].next();
                    const double second = h.metal[1].next();
                    const double bell = 0.5 * (first + second);
                    const double shape = 0.35 * h.amp + 0.65 * h.hiss;
                    y = 1.8 * h.band.process(bell) * shape * (0.7 + 0.3 * tn);
                } else {
                    // Rimshot.
                    h.amp *= exp(-dt / (0.008 + 0.03 * d));
                    h.phase1 += min(1650.0 * scale * dt, 0.45);
                    h.phase1 -= floor(h.phase1);
                    h.phase2 += min(470.0 * scale * dt, 0.45);
                    h.phase2 -= floor(h.phase2);
                    const double knock = 0.6 * sin(twoPi * h.phase1) + 0.5 * sin(twoPi * h.phase2);
                    const double click = t < 0.001 ? h.top.process(white) * (0.5 + sn) : 0.0;
                    y = tanh(2.0 * knock * h.amp) * (0.6 + 0.4 * tn) + click;
                }
                const double loud = m == 1 ? max(h.amp, h.hiss) : h.amp;
                h.env = clamp(loud, 0.0, 1.0);
                out[c][i] = 0.7 * y * h.force * dbToGain(level.at(i, c));
                env[c][i] = h.env;
            }
        }
    }
};
