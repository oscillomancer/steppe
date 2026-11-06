// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A modulation matrix: four inputs, A to D, to four outputs, 1 to 4, with
// an attenuverter where each row meets each column. Out 1 is A times "A to
// 1" plus B times "B to 1" and so on: four LFOs and envelopes blended into
// four destinations, a sound's modulation in one place. Every knob runs
// from -100 % (inverted) through 0 (none) to 100 %. An input with nothing
// in it sends 1, so its row's knobs are offsets. Poly: a mono input reaches
// every channel.

struct Matrix : Module {
    static constexpr ModuleInfo info = {
        .name = "Matrix",
        .id = "com.suppleaudio.rack.matrix",
        .vendor = "Supple Audio",
        .description = "Four inputs to four outputs through sixteen attenuverters.",
        .tags = {"utility", "mixer", "modulation", "poly"}};

    Param a_to_1 = percent("A to 1", 0, -100, 100)
                       .smoothed(5)
                       .in("A to")
                       .help("How much of In A goes to Out 1; below 0 inverted.");
    Param a_to_2 = percent("A to 2", 0, -100, 100)
                       .smoothed(5)
                       .in("A to")
                       .help("How much of In A goes to Out 2; below 0 inverted.");
    Param a_to_3 = percent("A to 3", 0, -100, 100)
                       .smoothed(5)
                       .in("A to")
                       .help("How much of In A goes to Out 3; below 0 inverted.");
    Param a_to_4 = percent("A to 4", 0, -100, 100)
                       .smoothed(5)
                       .in("A to")
                       .help("How much of In A goes to Out 4; below 0 inverted.");
    Param b_to_1 = percent("B to 1", 0, -100, 100)
                       .smoothed(5)
                       .in("B to")
                       .help("How much of In B goes to Out 1; below 0 inverted.");
    Param b_to_2 = percent("B to 2", 0, -100, 100)
                       .smoothed(5)
                       .in("B to")
                       .help("How much of In B goes to Out 2; below 0 inverted.");
    Param b_to_3 = percent("B to 3", 0, -100, 100)
                       .smoothed(5)
                       .in("B to")
                       .help("How much of In B goes to Out 3; below 0 inverted.");
    Param b_to_4 = percent("B to 4", 0, -100, 100)
                       .smoothed(5)
                       .in("B to")
                       .help("How much of In B goes to Out 4; below 0 inverted.");
    Param c_to_1 = percent("C to 1", 0, -100, 100)
                       .smoothed(5)
                       .in("C to")
                       .help("How much of In C goes to Out 1; below 0 inverted.");
    Param c_to_2 = percent("C to 2", 0, -100, 100)
                       .smoothed(5)
                       .in("C to")
                       .help("How much of In C goes to Out 2; below 0 inverted.");
    Param c_to_3 = percent("C to 3", 0, -100, 100)
                       .smoothed(5)
                       .in("C to")
                       .help("How much of In C goes to Out 3; below 0 inverted.");
    Param c_to_4 = percent("C to 4", 0, -100, 100)
                       .smoothed(5)
                       .in("C to")
                       .help("How much of In C goes to Out 4; below 0 inverted.");
    Param d_to_1 = percent("D to 1", 0, -100, 100)
                       .smoothed(5)
                       .in("D to")
                       .help("How much of In D goes to Out 1; below 0 inverted.");
    Param d_to_2 = percent("D to 2", 0, -100, 100)
                       .smoothed(5)
                       .in("D to")
                       .help("How much of In D goes to Out 2; below 0 inverted.");
    Param d_to_3 = percent("D to 3", 0, -100, 100)
                       .smoothed(5)
                       .in("D to")
                       .help("How much of In D goes to Out 3; below 0 inverted.");
    Param d_to_4 = percent("D to 4", 0, -100, 100)
                       .smoothed(5)
                       .in("D to")
                       .help("How much of In D goes to Out 4; below 0 inverted.");

    Input a = cvIn("A").normal(1.0).help("Row A, poly; 1 with nothing here, for offsets.");
    Input b = cvIn("B").normal(1.0).help("Row B, poly; 1 with nothing here, for offsets.");
    Input c = cvIn("C").normal(1.0).help("Row C, poly; 1 with nothing here, for offsets.");
    Input d = cvIn("D").normal(1.0).help("Row D, poly; 1 with nothing here, for offsets.");

    Outputs<4> out = cvOut("Out").help("Its column: each input times its knob, summed.");

    void factoryPatches(Patches& p) {
        p.add("Straight Through")
            .set(a_to_1, 100)
            .set(b_to_2, 100)
            .set(c_to_3, 100)
            .set(d_to_4, 100);
        p.add("Blend A and B").set(a_to_1, 100).set(b_to_1, 50).set(a_to_2, -100).set(b_to_2, 50);
        p.add("Spread A").set(a_to_1, 100).set(a_to_2, 50).set(a_to_3, -50).set(a_to_4, -100);
    }

    double amount(int k, int i, int ch) const {
        switch (k) {
            case 0:
                return a_to_1.at(i, ch);
            case 1:
                return a_to_2.at(i, ch);
            case 2:
                return a_to_3.at(i, ch);
            case 3:
                return a_to_4.at(i, ch);
            case 4:
                return b_to_1.at(i, ch);
            case 5:
                return b_to_2.at(i, ch);
            case 6:
                return b_to_3.at(i, ch);
            case 7:
                return b_to_4.at(i, ch);
            case 8:
                return c_to_1.at(i, ch);
            case 9:
                return c_to_2.at(i, ch);
            case 10:
                return c_to_3.at(i, ch);
            case 11:
                return c_to_4.at(i, ch);
            case 12:
                return d_to_1.at(i, ch);
            case 13:
                return d_to_2.at(i, ch);
            case 14:
                return d_to_3.at(i, ch);
            default:
                return d_to_4.at(i, ch);
        }
    }

    void process(int frames) {
        const int n = max(1, channels());
        for (int k = 0; k < 4; ++k) out[k].setChannels(n);
        for (int ch = 0; ch < n; ++ch) {
            for (int i = 0; i < frames; ++i) {
                const double va = a[ch][i];
                const double vb = b[ch][i];
                const double vc = c[ch][i];
                const double vd = d[ch][i];
                for (int k = 0; k < 4; ++k)
                    out[k][ch][i] = va * amount(k, i, ch) + vb * amount(4 + k, i, ch) +
                                    vc * amount(8 + k, i, ch) + vd * amount(12 + k, i, ch);
            }
        }
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(240, 300);
        p.input(a, 26, 56);
        p.knob(a_to_1, 74, 56, 30);
        p.knob(a_to_2, 120, 56, 30);
        p.knob(a_to_3, 166, 56, 30);
        p.knob(a_to_4, 212, 56, 30);
        p.input(b, 26, 108);
        p.knob(b_to_1, 74, 108, 30);
        p.knob(b_to_2, 120, 108, 30);
        p.knob(b_to_3, 166, 108, 30);
        p.knob(b_to_4, 212, 108, 30);
        p.input(c, 26, 160);
        p.knob(c_to_1, 74, 160, 30);
        p.knob(c_to_2, 120, 160, 30);
        p.knob(c_to_3, 166, 160, 30);
        p.knob(c_to_4, 212, 160, 30);
        p.input(d, 26, 212);
        p.knob(d_to_1, 74, 212, 30);
        p.knob(d_to_2, 120, 212, 30);
        p.knob(d_to_3, 166, 212, 30);
        p.knob(d_to_4, 212, 212, 30);
        for (int k = 0; k < 4; ++k) p.output(out[k], 74 + 46 * k, 272);
    }
};
