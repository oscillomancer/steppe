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

    Param level_1 = decibels("Level 1", -60, 12, 0)
                        .smoothed(10)
                        .offAtMin()
                        .in("Channel 1")
                        .help("Input 1's level; the minimum is off.");
    Param pan_1 = percent("Pan 1", 0, -100, 100)
                      .smoothed(10)
                      .in("Channel 1")
                      .help("Input 1's place from left (-100 %) to right (100 %), constant power.");
    Param mute_1 = toggle("Mute 1", false).in("Channel 1").help("Silence input 1.");
    Param level_2 = decibels("Level 2", -60, 12, 0)
                        .smoothed(10)
                        .offAtMin()
                        .in("Channel 2")
                        .help("Input 2's level; the minimum is off.");
    Param pan_2 = percent("Pan 2", 0, -100, 100)
                      .smoothed(10)
                      .in("Channel 2")
                      .help("Input 2's place from left (-100 %) to right (100 %), constant power.");
    Param mute_2 = toggle("Mute 2", false).in("Channel 2").help("Silence input 2.");
    Param level_3 = decibels("Level 3", -60, 12, 0)
                        .smoothed(10)
                        .offAtMin()
                        .in("Channel 3")
                        .help("Input 3's level; the minimum is off.");
    Param pan_3 = percent("Pan 3", 0, -100, 100)
                      .smoothed(10)
                      .in("Channel 3")
                      .help("Input 3's place from left (-100 %) to right (100 %), constant power.");
    Param mute_3 = toggle("Mute 3", false).in("Channel 3").help("Silence input 3.");
    Param level_4 = decibels("Level 4", -60, 12, 0)
                        .smoothed(10)
                        .offAtMin()
                        .in("Channel 4")
                        .help("Input 4's level; the minimum is off.");
    Param pan_4 = percent("Pan 4", 0, -100, 100)
                      .smoothed(10)
                      .in("Channel 4")
                      .help("Input 4's place from left (-100 %) to right (100 %), constant power.");
    Param mute_4 = toggle("Mute 4", false).in("Channel 4").help("Silence input 4.");
    Param master = decibels("Master", -60, 12, 0)
                       .smoothed(10)
                       .offAtMin()
                       .help("The level of every output; the minimum is off.");

    Input in_1 = audioIn("In 1").help("Input 1; a poly cable is summed.");
    Input in_2 = audioIn("In 2").help("Input 2; a poly cable is summed.");
    Input in_3 = audioIn("In 3").help("Input 3; a poly cable is summed.");
    Input in_4 = audioIn("In 4").help("Input 4; a poly cable is summed.");

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

    double sum(const Input& in, int i) const {
        double s = 0.0;
        const int n = max(1, in.channels());
        for (int c = 0; c < n; ++c) s += in[c][i];
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
                const bool muted =
                    k == 0 ? mute_1.on()
                           : (k == 1 ? mute_2.on() : (k == 2 ? mute_3.on() : mute_4.on()));
                const double target = muted ? 0.0 : 1.0;
                open[k] = unplayed ? target : open[k] + (target - open[k]) * fade;
                const double x =
                    k == 0 ? sum(in_1, i)
                           : (k == 1 ? sum(in_2, i) : (k == 2 ? sum(in_3, i) : sum(in_4, i)));
                const double db =
                    k == 0 ? level_1.at(i)
                           : (k == 1 ? level_2.at(i) : (k == 2 ? level_3.at(i) : level_4.at(i)));
                const double pan =
                    k == 0 ? pan_1.at(i)
                           : (k == 1 ? pan_2.at(i) : (k == 2 ? pan_3.at(i) : pan_4.at(i)));
                const double y = x * gainOf(db) * open[k];
                const double angle = (clamp(pan, -1.0, 1.0) + 1.0) * pi * 0.25;
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
