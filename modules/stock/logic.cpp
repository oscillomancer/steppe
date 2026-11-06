// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Gate logic, poly: AND, OR and XOR of A and B, NOT A, a flip-flop that
// changes state at each rising edge of Toggle (of A, with nothing in
// Toggle), so it divides a clock by two, and Change, a 1 ms trigger at
// every edge of A, rising or falling. Inputs read high at 0.5 and low
// below 0.25. Combine two rhythms into a third, or gate one with another.

struct Logic : Module {
    static constexpr ModuleInfo info = {
        .name = "Logic",
        .id = "com.suppleaudio.rack.logic",
        .vendor = "Supple Audio",
        .description = "AND, OR, XOR, NOT, a flip-flop and a trigger on every change.",
        .tags = {"logic", "utility", "poly"}};

    Input a = gateIn("A").help("The first gate, poly.");
    Input b = gateIn("B").help("The second gate, poly; low with nothing here.");
    Input toggle =
        gateIn("Toggle").help("Each rising edge flips the flip-flop; with nothing here, A's do.");

    Output both = gateOut("And").withKey("and").help("High while A and B are both high.");
    Output either = gateOut("Or").withKey("or").help("High while A or B (or both) is high.");
    Output exclusive =
        gateOut("Xor").withKey("xor").help("High while one of A and B is high, not both.");
    Output notA = gateOut("Not A").help("High while A is low.");
    Output flipFlop =
        gateOut("Flipflop").help("Changes state at each toggle: half the rate of its clock.");
    Output change = gateOut("Change").help("A 1 ms trigger at every edge of A.");

    struct Voice {
        bool a = false;
        bool b = false;
        bool t = false;
        bool state = false;
        int pulse = 0;
    };
    Array<Voice, 16> voices;
    int pulseFrames = 48;

    void prepare() { pulseFrames = max(1, roundToInt(0.001 * sampleRate())); }

    void process(int frames) {
        const bool ownToggle = toggle.connected();
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voices[c];
            for (int i = 0; i < frames; ++i) {
                const double x = a[c][i];
                const bool wasA = v.a;
                v.a = v.a ? x >= 0.25 : x >= 0.5;
                const double y = b[c][i];
                v.b = v.b ? y >= 0.25 : y >= 0.5;
                if (v.a != wasA) v.pulse = pulseFrames;
                bool rose = false;
                if (ownToggle) {
                    const double z = toggle[c][i];
                    const bool wasT = v.t;
                    v.t = v.t ? z >= 0.25 : z >= 0.5;
                    rose = v.t && !wasT;
                } else {
                    rose = v.a && !wasA;
                }
                if (rose) v.state = !v.state;
                both[c][i] = v.a && v.b ? 1.0 : 0.0;
                either[c][i] = v.a || v.b ? 1.0 : 0.0;
                exclusive[c][i] = v.a != v.b ? 1.0 : 0.0;
                notA[c][i] = v.a ? 0.0 : 1.0;
                flipFlop[c][i] = v.state ? 1.0 : 0.0;
                change[c][i] = v.pulse > 0 ? 1.0 : 0.0;
                if (v.pulse > 0) --v.pulse;
            }
        }
    }
};
