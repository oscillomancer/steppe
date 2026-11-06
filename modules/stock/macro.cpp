// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Eight macro knobs, each sent out of its own jack: a patch's performance
// controls in one place. Turn a knob and everything patched to it moves;
// automate the knobs from the timeline and the patch performs itself.
// Range makes every output unipolar (0 to 1, for levels and mixes) or
// bipolar (-1 to 1, centred at 50 %); Smooth glides the outputs so a knob
// turned by hand, or an automation step, does not click or zipper.

struct Macro : Module {
    static constexpr ModuleInfo info = {
        .name = "Macro",
        .id = "com.suppleaudio.rack.macro",
        .vendor = "Supple Audio",
        .description = "Eight knobs out of eight jacks: a patch's performance controls.",
        .tags = {"utility", "controller"}};

    Params<8> macro = percent("Macro", 0).help("A knob, sent out of the Out below it.");
    Param range = choice("Range", {"Unipolar", "Bipolar"}, 0)
                      .help("Outputs from 0 to 1, or from -1 to 1 with 50 % at 0.");
    Param smooth = milliseconds("Smooth", 0, 2000, 20)
                       .help("How long the outputs take to follow a knob: 0 jumps.");

    Outputs<8> out = cvOut("Out").help("Its knob, in the range chosen, glided by Smooth.");

    Array<double, 8> glided;
    bool unplayed = true;

    void prepare() { unplayed = true; }

    void process(int frames) {
        const double ms = smooth.value();
        const double coefficient = ms <= 0.0 ? 0.0 : exp(-1.0 / (ms * 0.001 * sampleRate()));
        const bool bipolar = range.index() == 1;
        for (int k = 0; k < 8; ++k) {
            out[k].setChannels(1);
            for (int i = 0; i < frames; ++i) {
                const double v = clamp(macro[k].at(i), 0.0, 1.0);
                const double target = bipolar ? 2.0 * v - 1.0 : v;
                glided[k] = unplayed ? target : target + (glided[k] - target) * coefficient;
                out[k][0][i] = glided[k];
            }
        }
        unplayed = false;
    }

    // Two rows of four, each knob over its jack.
    void panel(Panel& p) {
        p.size(176, 300);
        for (int k = 0; k < 8; ++k) {
            const double x = 22.0 + 44.0 * imod(k, 4);
            const double y = k < 4 ? 60.0 : 164.0;
            p.knob(macro[k], x, y);
            p.output(out[k], x, y + 48.0);
        }
        p.choice(range, 8, 244, 104, 18);
        p.knob(smooth, 146, 256, 24);
    }
};
