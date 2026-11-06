// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Exercises the module surface at its bounds, in native C++ and both MIR modes.
struct ModuleSurface : Module {
    static constexpr ModuleInfo info = {.name = "Surface", .id = "com.example.surface"};
    Input in = audioIn("In").normal(0.25);
    Param gain = percent("Gain", 50).smoothed(10);
    Output out = audioOut("Out");
    Random random;
    Shared<double> last;

    void prepare() { random.seed(this->seed()); }
    void update() { last.set(gain.value()); }
    void process(int frames) {
        out.setChannels(99);
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i) out[c][i] = in[c][i] * gain.at(i, c);
        out[14][0] = gain.at(0, -1);
        out[14][1] = gain.at(-1, 0);
        out[14][2] = gain.at(frames);
        out[14][3] = gain.at(0, 16);
        out[15][0] = in.channels() + (in.connected() ? 100 : 0) + out.channels() * 1000;
        out.setChannels(-2);
        out[15][0] += out.channels() * 100000;
        out.setChannels(99);
        out[15][1] = this->transport().tempo;
        out[15][2] = seed() + maxBlockSize() + sampleRate() + (offline() ? 1 : 0);
        out[15][3] = random.next() + gain.at(0);
        out[13][0] = in[-1][0];
        out[13][1] = in[16][0];
        out[13][2] = in[0][-1];
        out[13][3] = in[0][frames];
        out[-1][0] = 99;
        out[16][0] = 99;
        out[0][-1] = 99;
        out[0][frames] = 99;
    }
    void panel(Panel& p) {
        p.size(120, 200);
        p.knob(gain, 40, 40);
        p.knob(gain, 80, 40, 24);
        p.slider(gain, 10, 70, 100, 20);
        p.toggle(gain, 10, 90, 100, 20);
        p.choice(gain, 10, 110, 100, 20);
        p.readout(gain, 10, 130, 100, 20);
        p.input(in, 30, 180);
        p.output(out, 90, 180);
        p.display(10, 150, 100, 20);
    }
};
