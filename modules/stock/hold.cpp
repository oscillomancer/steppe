// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Sample and hold, or track and hold, poly. Sampling, each rising edge of
// Trigger takes the input and holds it to the next; tracking, the output
// follows the input while the gate is high and holds it while low. With
// nothing in In it samples noise: a random value each trigger, from -1 to
// 1, one per voice (Voices), a source of random notes alone (into a
// quantizer) or of random levels. A mono trigger samples every channel at
// once; a poly one each channel by itself.

struct Hold : Module {
    static constexpr ModuleInfo info = {
        .name = "Hold",
        .id = "com.suppleaudio.rack.hold",
        .vendor = "Supple Audio",
        .description = "Sample and hold or track and hold, with noise when nothing is in.",
        .tags = {"random", "utility", "poly"}};

    Param mode = choice("Mode", {"Sample", "Track"}, 0)
                     .help(
                         "Sample takes the input at each rising edge; track follows it while "
                         "the gate is high.");
    Param voices = integer("Voices", 1, 16, 1)
                       .help(
                           "How many channels it sends at least: with noise, that many random "
                           "values a trigger.");

    Input in = cvIn("In").help("What it takes; with nothing here, noise from -1 to 1.");
    Input trigger = gateIn("Trigger").help(
        "Sample: each rising edge takes a value. Track: high follows, low holds.");

    Output out = cvOut("Out").help("The value held (or followed).");

    Array<double, 16> held;
    Array<bool, 16> high;
    Random rng;

    void factoryPatches(Patches& p) {
        p.add("Sample");
        p.add("Track").set(mode, "track");
        p.add("Random Chord").set(voices, 3);
    }

    void prepare() { rng.seed(seed()); }

    void process(int frames) {
        const int n = max(channels(), voices.index());
        out.setChannels(n);
        const bool noise = !in.connected();
        const bool tracking = mode.index() == 1;
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < n; ++c) {
                const double g = trigger[c][i];
                bool rose = false;
                if (!high[c] && g >= 0.5) {
                    high[c] = true;
                    rose = true;
                } else if (high[c] && g < 0.25) {
                    high[c] = false;
                }
                if (rose || (tracking && high[c])) held[c] = noise ? rng.bipolar() : in[c][i];
                out[c][i] = held[c];
            }
    }
};
