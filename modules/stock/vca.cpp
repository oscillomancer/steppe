// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A voltage-controlled amplifier: the input times a gain the CV opens, poly.
// With nothing in CV the level knob alone sets the gain, so it doubles as a
// level control. Exponential response sounds natural on envelopes; linear
// suits audio-rate amplitude modulation.
struct Vca : Module {
    static constexpr ModuleInfo info = {
        .name = "VCA",
        .id = "com.suppleaudio.rack.vca",
        .vendor = "Supple Audio",
        .description = "An amplifier a CV opens: an envelope shapes a note, an LFO a tremolo.",
        .tags = {"amplifier", "utility"}};

    Input in = audioIn("In").help("What it amplifies.");
    Input cv = unipolarIn("CV").normal(1.0).help(
        "How far it opens, 0 to 1; open fully while nothing is connected.");
    Param level =
        decibels("Level", -60, 12, 0).smoothed(10).offAtMin().help("The gain at a CV of 1.");
    Param response = choice("Response", {"Exponential", "Linear"}, 0)
                         .keys({"exponential", "linear"})
                         .help("How the CV maps to gain: exponential (60 dB of travel) or linear.");
    Output out = audioOut("Out").help("The input times the gain.");

    void process(int frames) {
        const bool linear = response.index() == 1;
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i) {
                const double open = clamp(cv[c][i], 0.0, 1.0);
                const double shape =
                    linear ? open : (open <= 0.0 ? 0.0 : dbToGain(60.0 * (open - 1.0)));
                const double gain = level.at(i, c) <= -60.0 ? 0.0 : dbToGain(level.at(i, c));
                out[c][i] = in[c][i] * gain * shape;
            }
    }
};
