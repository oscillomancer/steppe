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

    Param macro_1 = percent("Macro 1", 0).help("Macro 1's value, sent out of Out 1.");
    Param macro_2 = percent("Macro 2", 0).help("Macro 2's value, sent out of Out 2.");
    Param macro_3 = percent("Macro 3", 0).help("Macro 3's value, sent out of Out 3.");
    Param macro_4 = percent("Macro 4", 0).help("Macro 4's value, sent out of Out 4.");
    Param macro_5 = percent("Macro 5", 0).help("Macro 5's value, sent out of Out 5.");
    Param macro_6 = percent("Macro 6", 0).help("Macro 6's value, sent out of Out 6.");
    Param macro_7 = percent("Macro 7", 0).help("Macro 7's value, sent out of Out 7.");
    Param macro_8 = percent("Macro 8", 0).help("Macro 8's value, sent out of Out 8.");
    Param range = choice("Range", {"Unipolar", "Bipolar"}, 0)
                      .help("Outputs from 0 to 1, or from -1 to 1 with 50 % at 0.");
    Param smooth = milliseconds("Smooth", 0, 2000, 20)
                       .help("How long the outputs take to follow a knob: 0 jumps.");

    Output out_1 = cvOut("Out 1").help("Macro 1, in the range chosen, glided by Smooth.");
    Output out_2 = cvOut("Out 2").help("Macro 2, in the range chosen, glided by Smooth.");
    Output out_3 = cvOut("Out 3").help("Macro 3, in the range chosen, glided by Smooth.");
    Output out_4 = cvOut("Out 4").help("Macro 4, in the range chosen, glided by Smooth.");
    Output out_5 = cvOut("Out 5").help("Macro 5, in the range chosen, glided by Smooth.");
    Output out_6 = cvOut("Out 6").help("Macro 6, in the range chosen, glided by Smooth.");
    Output out_7 = cvOut("Out 7").help("Macro 7, in the range chosen, glided by Smooth.");
    Output out_8 = cvOut("Out 8").help("Macro 8, in the range chosen, glided by Smooth.");

    Array<double, 8> glided;
    double coefficient = 0.0;
    bool unplayed = true;

    void prepare() { unplayed = true; }

    void send(const Output& out, const Param& macro, int k, int frames, bool bipolar) {
        for (int i = 0; i < frames; ++i) {
            const double v = clamp(macro.at(i), 0.0, 1.0);
            const double target = bipolar ? 2.0 * v - 1.0 : v;
            glided[k] = unplayed ? target : target + (glided[k] - target) * coefficient;
            out[0][i] = glided[k];
        }
    }

    void process(int frames) {
        const double ms = smooth.value();
        coefficient = ms <= 0.0 ? 0.0 : exp(-1.0 / (ms * 0.001 * sampleRate()));
        const bool bipolar = range.index() == 1;
        out_1.setChannels(1);
        out_2.setChannels(1);
        out_3.setChannels(1);
        out_4.setChannels(1);
        out_5.setChannels(1);
        out_6.setChannels(1);
        out_7.setChannels(1);
        out_8.setChannels(1);
        send(out_1, macro_1, 0, frames, bipolar);
        send(out_2, macro_2, 1, frames, bipolar);
        send(out_3, macro_3, 2, frames, bipolar);
        send(out_4, macro_4, 3, frames, bipolar);
        send(out_5, macro_5, 4, frames, bipolar);
        send(out_6, macro_6, 5, frames, bipolar);
        send(out_7, macro_7, 6, frames, bipolar);
        send(out_8, macro_8, 7, frames, bipolar);
        unplayed = false;
    }

    void panel(Panel& p) {
        p.size(176, 300);
        p.knob(macro_1, 22, 60);
        p.output(out_1, 22, 108);
        p.knob(macro_2, 66, 60);
        p.output(out_2, 66, 108);
        p.knob(macro_3, 110, 60);
        p.output(out_3, 110, 108);
        p.knob(macro_4, 154, 60);
        p.output(out_4, 154, 108);
        p.knob(macro_5, 22, 164);
        p.output(out_5, 22, 212);
        p.knob(macro_6, 66, 164);
        p.output(out_6, 66, 212);
        p.knob(macro_7, 110, 164);
        p.output(out_7, 110, 212);
        p.knob(macro_8, 154, 164);
        p.output(out_8, 154, 212);
        p.choice(range, 8, 244, 104, 18);
        p.knob(smooth, 146, 256, 24);
    }
};
