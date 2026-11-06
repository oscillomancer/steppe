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

    Param gain_1 = percent("Gain 1", 100, -200, 200)
                       .in("Row 1")
                       .help("Row 1's input times this: negative inverts it, 0 % silences it.");
    Param offset_1 =
        continuous("Offset 1", -2, 2, 0)
            .in("Row 1")
            .help("Added to row 1 after the gain (in the signal's own units: octaves for pitch).");
    Param gain_2 = percent("Gain 2", 100, -200, 200)
                       .in("Row 2")
                       .help("Row 2's input times this: negative inverts it, 0 % silences it.");
    Param offset_2 =
        continuous("Offset 2", -2, 2, 0)
            .in("Row 2")
            .help("Added to row 2 after the gain (in the signal's own units: octaves for pitch).");
    Param gain_3 = percent("Gain 3", 100, -200, 200)
                       .in("Row 3")
                       .help("Row 3's input times this: negative inverts it, 0 % silences it.");
    Param offset_3 =
        continuous("Offset 3", -2, 2, 0)
            .in("Row 3")
            .help("Added to row 3 after the gain (in the signal's own units: octaves for pitch).");
    Param gain_4 = percent("Gain 4", 100, -200, 200)
                       .in("Row 4")
                       .help("Row 4's input times this: negative inverts it, 0 % silences it.");
    Param offset_4 =
        continuous("Offset 4", -2, 2, 0)
            .in("Row 4")
            .help("Added to row 4 after the gain (in the signal's own units: octaves for pitch).");

    Input in_1 =
        cvIn("In 1").help("Row 1's input, poly; with nothing here the row sends its offset.");
    Input in_2 =
        cvIn("In 2").help("Row 2's input, poly; with nothing here the row sends its offset.");
    Input in_3 =
        cvIn("In 3").help("Row 3's input, poly; with nothing here the row sends its offset.");
    Input in_4 =
        cvIn("In 4").help("Row 4's input, poly; with nothing here the row sends its offset.");

    Output out_1 = cvOut("Out 1").help("Row 1: its input times the gain, plus the offset.");
    Output out_2 = cvOut("Out 2").help("Row 2: its input times the gain, plus the offset.");
    Output out_3 = cvOut("Out 3").help("Row 3: its input times the gain, plus the offset.");
    Output out_4 = cvOut("Out 4").help("Row 4: its input times the gain, plus the offset.");

    void row(const Input& in, const Output& out, const Param& gain, const Param& offset,
             int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i) out[c][i] = in[c][i] * gain.at(i, c) + offset.at(i, c);
    }

    void process(int frames) {
        row(in_1, out_1, gain_1, offset_1, frames);
        row(in_2, out_2, gain_2, offset_2, frames);
        row(in_3, out_3, gain_3, offset_3, frames);
        row(in_4, out_4, gain_4, offset_4, frames);
    }
};
