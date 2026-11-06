// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A distortion, poly: Drive pushes the input into one of seven curves,
//
//   Soft      a smooth saturation, warm and round;
//   Tube      a lopsided one that gives way sooner below than above:
//             even harmonics, a valve's growl;
//   Hard      a hard clip at 1: a fuzz's buzz;
//   Fold      a sine fold: past 1 the wave turns back, glassy and hollow;
//   Rectify   the wave's size, its negative half turned up: an octave up;
//   Bitcrush  fewer bits, Bits of them: grit, a lo-fi sampler's steps;
//   Downsample  held at Rate, a lower sample rate: whistling aliases.
//
// Bias shifts the input before the curve, so it bends one side more:
// even harmonics, and with Drive low, a sputter. Tone tilts the result
// darker or brighter; Mix blends it with the dry input, Level sets the
// output. The first five curves run at four times the sample rate so their
// harmonics do not fold back as aliases (the last two alias on purpose),
// through linear-phase filters that keep a saturated wave's peaks at 1:
// the output, dry part and all, is 72 samples late (1.5 ms at 48 kHz). The
// DC a bias or a rectifier leaves is filtered out.

double tube(double x) { return x >= 0.0 ? 1.0 - exp(-x) : (exp(2.0 * x) - 1.0) / 2.0; }

struct Lane {
    Oversampler over;
    DelayLine dry;
    double held = 0.0, clock = 1.0;  // the downsampler's
    double x1 = 0.0, y1 = 0.0;       // the DC blocker's
    double low = 0.0;                // the tone's low half
};

struct Drive : Module {
    static constexpr ModuleInfo info = {
        .name = "Drive",
        .id = "com.suppleaudio.rack.drive",
        .vendor = "Supple Audio",
        .description =
            "A distortion: soft, tube, hard clip, fold, rectify, bitcrush and "
            "downsample, oversampled.",
        .tags = {"distortion", "effect", "poly"}};

    Param mode =
        choice("Mode", {"Soft", "Tube", "Hard", "Fold", "Rectify", "Bitcrush", "Downsample"}, 0)
            .help("The curve the input is pushed into.");
    Param drive = decibels("Drive", 0, 40, 12)
                      .smoothed(20)
                      .help("The gain into the curve: how hard it is pushed.");
    Param bias = percent("Bias", 0, -100, 100)
                     .smoothed(20)
                     .help("Shifts the input before the curve: one side bends more.");
    Param bits = continuous("Bits", 1, 16, 6)
                     .in("Crush")
                     .help("Bitcrush: the bits it keeps, between whole numbers as well.");
    Param rate = frequency("Rate", 100, 48000, 6000)
                     .in("Crush")
                     .help("Downsample: the lower sample rate it holds the input at.");
    Param tone = percent("Tone", 0, -100, 100)
                     .in("Output")
                     .help("Tilts the result, 9 dB each way: darker below 0, brighter above.");
    Param mix = percent("Mix", 100)
                    .smoothed(20)
                    .in("Output")
                    .help("The distorted share, the rest the dry input.");
    Param level = decibels("Level", -24, 12, 0).smoothed(20).in("Output").help("The output level.");

    Input in = audioIn("In").help("What to distort. Poly: each channel alone.");
    Output out = audioOut("Out").help("The distorted mix.");

    Array<Lane, 16> lane;

    void prepare() {
        for (int c = 0; c < 16; ++c) {
            lane[c].over.prepare(4, Oversampler::Phase::Linear);
            lane[c].dry.prepare(256);
        }
    }

    void process(int frames) {
        const double sr = sampleRate();
        const int m = mode.index();
        const double r = 1.0 - twoPi * 10.0 / sr;  // the DC blocker's pole
        const double a = 1.0 - exp(-twoPi * 700.0 / sr);
        for (int c = 0; c < channels(); ++c) {
            Lane& l = lane[c];
            const double latency = l.over.latency();
            for (int i = 0; i < frames; ++i) {
                const double dry = in[c][i];
                const double x = dry * dbToGain(drive.at(i, c)) + clamp(bias.at(i, c), -1.0, 1.0);
                double base = x;
                if (m == 5) {
                    const double steps = exp2(clamp(bits.at(i, c), 1.0, 16.0) - 1.0);
                    base = clamp(round(x * steps) / steps, -1.0, 1.0);
                } else if (m == 6) {
                    l.clock += clamp(rate.at(i, c), 1.0, sr) / sr;
                    if (l.clock >= 1.0) {
                        l.clock -= floor(l.clock);
                        l.held = x;
                    }
                    base = clamp(l.held, -1.0, 1.0);
                }
                l.over.up(base);
                if (m < 5)
                    for (int k = 0; k < 4; ++k) {
                        const double s = l.over.sample(k);
                        double y = s;
                        if (m == 0)
                            y = tanh(s);
                        else if (m == 1)
                            y = tube(s);
                        else if (m == 2)
                            y = clamp(s, -1.0, 1.0);
                        else if (m == 3)
                            y = sin(0.5 * pi * s);
                        else
                            y = abs(tanh(s));
                        l.over.set(k, y);
                    }
                const double wet = l.over.down();
                const double blocked = wet - l.x1 + r * l.y1;
                l.x1 = wet;
                l.y1 = blocked;
                l.low += a * (blocked - l.low);
                const double t = clamp(tone.at(i, c), -1.0, 1.0);
                const double shaped =
                    l.low * dbToGain(-9.0 * t) + (blocked - l.low) * dbToGain(9.0 * t);
                l.dry.push(dry);
                const double w = clamp(mix.at(i, c), 0.0, 1.0);
                const double late = l.dry.read(latency);
                out[c][i] = dbToGain(level.at(i, c)) * (w * shaped + (1.0 - w) * late);
            }
        }
    }
};
