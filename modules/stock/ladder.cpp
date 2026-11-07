// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A four-pole ladder low pass, poly: fat, round, and singing at the top of
// Resonance. The four stages' feedback runs through a soft saturator, so the
// resonance thickens and compresses as it rises instead of thinning, and
// self-oscillates (a clean sine at the cutoff, which the Pitch input plays)
// from about 92 %. Compensation restores the bass a ladder loses as its
// resonance rises: 0 for the classic thinning, 100 for a level that holds.
// Drive pushes the input into the saturator. Besides the 24 dB low pass it
// gives the two-pole tap (a brighter 12 dB slope) and a resonant 24 dB high
// pass mixed from the stages.
//
// Each stage is a trapezoidal one-pole; the loop is solved without a sample
// of delay, then saturated, and everything runs at twice the sample rate.

struct Ladder4 {
    double s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
    double u = 0.0, y1 = 0.0, y2 = 0.0, y3 = 0.0, y4 = 0.0;

    // One sample at G = g / (1 + g), feedback k.
    void run(double x, double G, double k) {
        const double h = 1.0 - G;
        const double sigma = G * G * G * h * s1 + G * G * h * s2 + G * h * s3 + h * s4;
        const double G4 = G * G * G * G;
        const double linear = (x - k * sigma) / (1.0 + k * G4);
        u = 3.0 * tanh(linear / 3.0);
        double v = (u - s1) * G;
        y1 = v + s1;
        s1 = y1 + v;
        v = (y1 - s2) * G;
        y2 = v + s2;
        s2 = y2 + v;
        v = (y2 - s3) * G;
        y3 = v + s3;
        s3 = y3 + v;
        v = (y3 - s4) * G;
        y4 = v + s4;
        s4 = y4 + v;
    }
};

struct Lane {
    Ladder4 f;
    Oversampler low, two, high;
};

struct Ladder : Module {
    static constexpr ModuleInfo info = {
        .name = "Ladder",
        .id = "com.suppleaudio.rack.ladder",
        .vendor = "Supple Audio",
        .description =
            "A four-pole ladder low pass with saturating feedback: fat, round, "
            "self-oscillating.",
        .tags = {"filter", "poly"}};

    Param cutoff = frequency("Cutoff", 20, 20000, 800)
                       .help("Where the filter turns; the Pitch input moves it 1 per octave.");
    Param resonance =
        percent("Resonance", 30)
            .help(
                "The peak at the cutoff, thick and compressed; it sings by itself from about 92.");
    Param drive = percent("Drive", 15)
                      .help("Pushes the input into the saturator: warm at 15, growling at 100.");
    Param compensation = percent("Compensation", 60)
                             .help(
                                 "Restores the bass resonance takes away: 0 thins as a classic "
                                 "ladder does, 100 holds.");
    Param fmDepth = octaves("FM Depth", -8, 8, 2)
                        .help("How far the FM input moves the cutoff, in octaves at a CV of 1.");

    Input in = audioIn("In").help("What it filters.");
    Input pitch = pitchIn("Pitch").help("Moves the cutoff 1 per octave: patch a keyboard's pitch.");
    Input fm = cvIn("FM").help("Moves the cutoff by FM Depth's octaves: an envelope or an LFO.");

    Output out = audioOut("Out").help("The low pass, 24 dB per octave.");
    Output low12 = audioOut("Low 12").withKey("low12").help(
        "The two-pole tap: a gentler 12 dB slope, brighter.");
    Output high = audioOut("High").help("A resonant high pass, 24 dB per octave.");

    Array<Lane, 16> lane;

    void prepare() {
        for (int c = 0; c < 16; ++c) {
            lane[c].low.prepare(2);
            lane[c].two.prepare(2);
            lane[c].high.prepare(2);
        }
    }

    void process(int frames) {
        const double rate = 2.0 * sampleRate();
        const double top = 0.45 * sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Lane& s = lane[c];
            for (int i = 0; i < frames; ++i) {
                const double oct = pitch[c][i] + fmDepth.at(i, c) * fm[c][i];
                const double hz = clamp(cutoff.at(i, c) * exp2(clamp(oct, -20.0, 20.0)), 8.0, top);
                const double g = tan(pi * hz / rate);
                const double G = g / (1.0 + g);
                const double k = 4.4 * clamp(resonance.at(i, c), 0.0, 1.0);
                const double d = clamp(drive.at(i, c), 0.0, 1.0);
                // Compensation makes up the bass the feedback takes from the
                // low passes; the high pass loses none, so it is taken back
                // off that output.
                const double lift = 1.0 + k * clamp(compensation.at(i, c), 0.0, 1.0);
                const double pre = dbToGain(24.0 * d) * lift;
                const double post = dbToGain(-12.0 * d);
                s.low.up(pre * in[c][i]);
                s.two.up(0.0);
                s.high.up(0.0);
                for (int j = 0; j < 2; ++j) {
                    s.f.run(s.low.sample(j), G, k);
                    const Ladder4& f = s.f;
                    s.low.set(j, f.y4);
                    s.two.set(j, f.y2);
                    s.high.set(j, f.u - 4.0 * f.y1 + 6.0 * f.y2 - 4.0 * f.y3 + f.y4);
                }
                out[c][i] = post * s.low.down();
                low12[c][i] = post * s.two.down();
                high[c][i] = post * s.high.down() / lift;
            }
        }
    }
};
