// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A low-pass gate, poly: an amplifier and a low pass opened together by a
// light-dependent cell, as the old west-coast gates were. A trigger into
// Ping flashes the light: the gate snaps open and dies away over Decay, the
// highs first, so any steady source turns into plucks, knocks and bongos (a
// sine with a little Resonance is a hand drum). A gate held high holds it
// open. CV (and Offset) open it continuously instead, for swells and
// envelopes of your own. Mode picks both (Combo), the filter alone, or the
// amplifier alone. The cell's own level is an output, a natural envelope for
// anything else.
//
// The cell answers fast and lets go slowly, and the dimmer it is the slower
// it falls, which gives the gate's decay its long, soft tail.

struct Gate {
    double cell = 0.0;   // 0 shut .. 1 open
    double flash = 0.0;  // seconds the ping's light has left
    bool high = false;
    double s1 = 0.0, s2 = 0.0;  // the filter's states
};

struct Lpg : Module {
    static constexpr ModuleInfo info = {
        .name = "LPG",
        .id = "com.suppleaudio.rack.lpg",
        .vendor = "Supple Audio",
        .description = "A low-pass gate: pinged, it turns any sound into plucks and bongos.",
        .tags = {"amplifier", "filter", "poly"}};

    Param decay =
        milliseconds("Decay", 20, 4000, 350)
            .help("How long a ping takes to die away; the tail lingers longer than this.");
    Param offset =
        percent("Offset", 0)
            .help("Opens the gate this far with nothing else: a drone to ping on top of.");
    Param resonance =
        percent("Resonance", 25)
            .help("A peak at the gate's cutoff that rings as it closes: knock, wood, skin.");
    Param mode = choice("Mode", {"Combo", "Low Pass", "VCA"}, 0)
                     .keys({"combo", "low_pass", "vca"})
                     .help("Opens the filter and the amplifier together, or only one of them.");

    Input in = audioIn("In").help("What it gates: a sine, a VCO, noise, anything.");
    Input ping = gateIn("Ping").help(
        "A trigger flashes it open to die away over Decay; a held gate holds it open.");
    Input cv = unipolarIn("CV").help("Opens it continuously, 0 to 1, on top of Offset and Ping.");

    Output out = audioOut("Out").help("The gated sound.");
    Output level = unipolarOut("Level").help("How open the gate is, 0 to 1: a soft envelope.");

    Array<Gate, 16> gate;

    void process(int frames) {
        const double sr = sampleRate();
        const double dt = 1.0 / sr;
        const double rise = 1.0 - exp(-1.0 / (0.002 * sr));
        const int m = mode.index();
        for (int c = 0; c < channels(); ++c) {
            Gate& s = gate[c];
            for (int i = 0; i < frames; ++i) {
                const double p = ping[c][i];
                if (!s.high && p >= 0.5) {
                    s.high = true;
                    s.flash = 0.005;
                } else if (s.high && p < 0.25) {
                    s.high = false;
                }
                const double lit = s.high || s.flash > 0.0 ? 1.0 : 0.0;
                s.flash = max(s.flash - dt, 0.0);
                const double light = clamp(offset.at(i, c) + cv[c][i] + lit, 0.0, 1.0);
                if (light > s.cell) {
                    s.cell += (light - s.cell) * rise;
                } else {
                    // Falling: the time constant grows as the cell dims.
                    const double tau = 0.001 * decay.at(i, c) * (0.25 + 1.5 * (1.0 - s.cell));
                    s.cell += (light - s.cell) * (1.0 - exp(-dt / tau));
                }
                const double v = s.cell;
                const double hz = m == 2 ? 0.45 * sr : min(30.0 * pow(600.0, v), 0.45 * sr);
                const double amp = m == 1 ? 1.0 : pow(v, 1.4);
                const double g = tan(pi * hz / sr);
                const double k = 2.0 - 1.85 * clamp(resonance.at(i, c), 0.0, 1.0);
                const double a1 = 1.0 / (1.0 + g * (g + k));
                const double a2 = g * a1;
                const double a3 = g * a2;
                const double v3 = in[c][i] - s.s2;
                const double band = a1 * s.s1 + a2 * v3;
                const double low = s.s2 + a2 * s.s1 + a3 * v3;
                s.s1 = 2.0 * band - s.s1;
                s.s2 = 2.0 * low - s.s2;
                out[c][i] = (m == 2 ? in[c][i] : low) * amp;
                level[c][i] = v;
            }
        }
    }
};
