// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

struct ArrayModule : Module {
    static constexpr ModuleInfo info = {
        .name = "Array Module", .id = "com.example.arrays", .tags = {"utility"}};
    static constexpr int kSteps = 4;
    Params<kSteps> pitch =
        semitones("Step Pitch", -24, 24, 0).withKey("note").help("The step's pitch.");
    Params<2> enabled = toggle("Enabled", true);
    Params<2> amount = percent("Amount", 50).smoothed(10).activeWhen(enabled[0]);
    Inputs<2> signalIn = audioIn("Signal").normal(0.25).help("Audio to scale.");
    Outputs<2> signalOut = audioOut("Signal").help("The scaled audio.");
    Input gate = gateIn("Gate");
    Output triggerOut = gateOut("Trigger");
    Array<Trigger, 16> edge;
    Array<Pulse, 16> pulse;
    Shared<double> meter;

    double read(Param& p, int i, int c) { return p.at(i, c); }
    void one(const Input& in, Output& out, Param& level, int frames) {
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out[c][i] = in[c][i] * read(level, i, c) + pitch[imod(i, pitch.size())].at(i, c);
    }
    void process(int frames) {
        for (int p = 0; p < signalOut.size(); ++p)
            one(signalIn[p], signalOut[p], amount[imod(p, amount.size())], frames);
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i) {
                if (edge[c].rose(gate[c][i])) pulse[c].fire();
                triggerOut[c][i] = pulse[c].next();
                const int n = i - 2;
                signalOut[0][c][i] +=
                    (shl(1, n) * 1.0 + shr(-1, n) * 1.0 + bit(seed(), n)) * 0.00000001;
                signalOut[1][c][i] += popcount(seed()) * 0.001;
            }
        meter.set(pitch[0].value());
    }
    void panel(Panel& p) {
        p.size(240, 300);
        for (int i = 0; i < pitch.size(); ++i) p.knob(pitch[i], 30 + i * 50, 50);
        for (int i = 0; i < signalIn.size(); ++i) {
            p.input(signalIn[i], 30 + i * 50, 120);
            p.output(signalOut[i], 30 + i * 50, 200);
        }
    }
    void draw(Canvas& g) { g.fillText(format("%.1f", pitch[0].value()), 10, 240); }
    void factoryPatches(Patches& p) {
        p.add("Sequence").set(pitch[0], 7).set(pitch[3], -5).set(enabled[0], false);
    }
};
