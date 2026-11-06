// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A four-channel mixer: each input with a level, a pan and a mute, summed
// to a stereo pair and to mono, through a master level. A poly cable into
// an input is summed to one signal first, so a whole poly voice section
// takes one channel. Panning is constant power (each side -3 dB at the
// centre); the mono output takes the levels without the pans. Mutes fade
// over a few milliseconds, so they do not click. It mixes CV as well as
// audio.

struct Mixer : Module {
    static constexpr ModuleInfo info = {
        .name = "Mixer",
        .id = "com.suppleaudio.rack.mixer",
        .vendor = "Supple Audio",
        .description = "Four channels with level, pan and mute, to stereo and mono.",
        .tags = {"mixer", "utility"}};

    Params<4> level = decibels("Level", -60, 12, 0)
                          .smoothed(10)
                          .offAtMin()
                          .in("Levels")
                          .help("The input's level; the minimum is off.");
    Params<4> pan =
        percent("Pan", 0, -100, 100)
            .smoothed(10)
            .in("Pans")
            .help("The input's place from left (-100 %) to right (100 %), constant power.");
    Params<4> mute = toggle("Mute", false).in("Mutes").help("Silence the input.");
    Param master = decibels("Master", -60, 12, 0)
                       .smoothed(10)
                       .offAtMin()
                       .help("The level of every output; the minimum is off.");

    Inputs<4> in = audioIn("In").help("An input; a poly cable is summed.");

    Output left = audioOut("Left").help("The left of the stereo mix.");
    Output right = audioOut("Right").help("The right of the stereo mix.");
    Output mono = audioOut("Mono").help("Every input at its level, unpanned.");

    Array<double, 4> open;  // each channel's mute fade, 0 muted .. 1 open
    double fade = 0.0;
    bool unplayed = true;

    void prepare() {
        fade = 1.0 - exp(-1.0 / (0.005 * sampleRate()));
        unplayed = true;
    }

    double sum(const Input& port, int i) const {
        double s = 0.0;
        const int n = max(1, port.channels());
        for (int c = 0; c < n; ++c) s += port[c][i];
        return s;
    }

    // A level knob as a gain, silent at its minimum.
    double gainOf(double db) const { return db <= -60.0 ? 0.0 : dbToGain(db); }

    void process(int frames) {
        left.setChannels(1);
        right.setChannels(1);
        mono.setChannels(1);
        for (int i = 0; i < frames; ++i) {
            double l = 0.0;
            double r = 0.0;
            double m = 0.0;
            for (int k = 0; k < 4; ++k) {
                const double target = mute[k].on() ? 0.0 : 1.0;
                open[k] = unplayed ? target : open[k] + (target - open[k]) * fade;
                const double x = sum(in[k], i);
                const double db = level[k].at(i);
                const double y = x * gainOf(db) * open[k];
                const double angle = (clamp(pan[k].at(i), -1.0, 1.0) + 1.0) * pi * 0.25;
                l += y * cos(angle);
                r += y * sin(angle);
                m += y;
            }
            unplayed = false;
            const double g = gainOf(master.at(i));
            left[0][i] = l * g;
            right[0][i] = r * g;
            mono[0][i] = m * g;
        }
    }
};
