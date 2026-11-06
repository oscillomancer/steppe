// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Four rows of attenuverter and offset: each output is its input times the
// gain (negative inverts it) plus the offset, poly. A row with nothing in
// it sends its offset alone, a constant to patch anywhere: a fixed pitch,
// a bias for a cutoff, a hand on a knob that cables can move.

struct Scale : Module {
    static constexpr ModuleInfo info = {
        .name = "Scale",
        .id = "com.suppleaudio.rack.scale",
        .vendor = "Supple Audio",
        .description = "Four attenuverters with offsets: scale, invert, shift, or a constant.",
        .tags = {"utility", "attenuverter"}};

    Params<4> gain = percent("Gain", 100, -200, 200)
                         .in("Gains")
                         .help("The row's input times this: negative inverts it, 0 % silences it.");
    Params<4> offset =
        continuous("Offset", -2, 2, 0)
            .in("Offsets")
            .help("Added after the gain (in the signal's own units: octaves for pitch).");

    Inputs<4> in = cvIn("In").help("A row's input, poly; with nothing here it sends its offset.");

    Outputs<4> out = cvOut("Out").help("A row: its input times the gain, plus the offset.");

    void process(int frames) {
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < channels(); ++c)
                for (int i = 0; i < frames; ++i)
                    out[k][c][i] = in[k][c][i] * gain[k].at(i, c) + offset[k].at(i, c);
    }

    // Four rows: in, gain, offset, out.
    void panel(Panel& p) {
        p.size(208, 268);
        for (int k = 0; k < 4; ++k) {
            const double y = 58.0 + 58.0 * k;
            p.input(in[k], 24, y);
            p.knob(gain[k], 78, y);
            p.knob(offset[k], 130, y);
            p.output(out[k], 184, y);
        }
    }
};
