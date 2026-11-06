// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>
struct ArrayBounds : Module {
    static constexpr ModuleInfo info = {.name = "Array Bounds", .id = "com.example.array-bounds"};
    Params<2> knobs = percent("Knob", 25);
    Inputs<2> in = audioIn("In").normal(0.5);
    Outputs<2> out = audioOut("Out");
    void process(int frames) {
        for (int n = -1; n <= knobs.size(); ++n) {
            double gain = knobs[n].at(0, 0);
            double x = in[n][0][0];
            out[n][0][0] = x + gain;
        }
        out[0][0][1] = knobs[-1].value();
        knobs[-1].set(99);
        out[0][0][2] = knobs[-1].value();
        out[-1][0][0] += 10;
        out[0][0][frames - 1] = out[1].channels();
    }
};
