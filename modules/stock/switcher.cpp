// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A sequential switch, both ways at once: Out plays one of four inputs,
// and Route goes out of one of four outputs, the same position for both.
// Each rising edge of Step moves to the next position (of Positions, then
// round again); Select adds its own share of the way through them, so a
// knob or a cable into Select chooses a position directly. Fade crossfades
// from one position to the next, from a hard cut to a slow blend. Reset
// returns to the first position. Four sequences in turn, four sounds in
// rotation, or one signal dealt to four destinations.

struct Switch : Module {
    static constexpr ModuleInfo info = {
        .name = "Switch",
        .id = "com.suppleaudio.rack.switcher",
        .vendor = "Supple Audio",
        .description = "A sequential switch, four to one and one to four, stepped or selected.",
        .tags = {"utility", "sequencer", "poly"}};

    Param positions = integer("Positions", 2, 4, 4).help("How many positions it steps through.");
    Param select = percent("Select", 0)
                       .help(
                           "Moves the position on by this share of the positions: a cable chooses "
                           "one directly.");
    Param fade = milliseconds("Fade", 0, 2000, 2)
                     .help("The crossfade from one position to the next; 0 cuts.");

    Input in_1 = cvIn("In 1").help("Out plays this at position 1; poly.");
    Input in_2 = cvIn("In 2").help("Out plays this at position 2.");
    Input in_3 = cvIn("In 3").help("Out plays this at position 3.");
    Input in_4 = cvIn("In 4").help("Out plays this at position 4.");
    Input route = cvIn("Route").help("Sent out of the output of the position; poly.");
    Input step = gateIn("Step").help("Each rising edge moves to the next position.");
    Input reset = gateIn("Reset").help("A rising edge returns to the first position.");

    Output out = cvOut("Out").help("The input of the position.");
    Output out_1 = cvOut("Out 1").help("Route, at position 1.");
    Output out_2 = cvOut("Out 2").help("Route, at position 2.");
    Output out_3 = cvOut("Out 3").help("Route, at position 3.");
    Output out_4 = cvOut("Out 4").help("Route, at position 4.");

    Array<double, 4> level;  // each position's share, crossfading
    int count = 0;
    bool stepHigh = false;
    bool resetHigh = false;
    bool unplayed = true;

    Shared<int> shownPosition;

    void factoryPatches(Patches& p) {
        p.add("Cut");
        p.add("Blend").set(fade, 300);
        p.add("Two Way").set(positions, 2);
    }

    void prepare() { unplayed = true; }

    void process(int frames) {
        int pos = 0;
        for (int i = 0; i < frames; ++i) {
            const double s = step[0][i];
            if (!stepHigh && s >= 0.5) {
                stepHigh = true;
                ++count;
            } else if (stepHigh && s < 0.25) {
                stepHigh = false;
            }
            const double r = reset[0][i];
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                count = 0;
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            const int n = clamp(roundToInt(positions.at(i)), 2, 4);
            count = wrap(count, n);
            pos = wrap(count + min(n - 1, floorToInt(clamp(select.at(i), 0.0, 1.0) * n)), n);
            const double ms = fade.at(i);
            const double move = ms <= 0.0 ? 1.0 : 1000.0 / (ms * sampleRate());
            for (int k = 0; k < 4; ++k) {
                const double target = k == pos ? 1.0 : 0.0;
                level[k] = unplayed ? target : level[k] + clamp(target - level[k], -move, move);
            }
            unplayed = false;
            for (int c = 0; c < channels(); ++c) {
                out[c][i] = in_1[c][i] * level[0] + in_2[c][i] * level[1] + in_3[c][i] * level[2] +
                            in_4[c][i] * level[3];
                const double x = route[c][i];
                out_1[c][i] = x * level[0];
                out_2[c][i] = x * level[1];
                out_3[c][i] = x * level[2];
                out_4[c][i] = x * level[3];
            }
        }
        shownPosition.set(pos);
    }
};
