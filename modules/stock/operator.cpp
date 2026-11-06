// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A phase-modulation operator, poly: the building block of FM synthesis,
// one sine at a ratio of the pitch. Patch one operator's Sine into
// another's PM and turn up Index: the second's tone grows bright and bell-
// or brass-like as the first bends its phase. Stacks, chains and branches of
// operators make any FM patch; Feedback bends an operator by its own output,
// from a sine towards a saw and on to noise. Level scales the sine (a cable
// from an envelope into it shapes each note's timbre when this operator is a
// modulator, or its loudness when it is the carrier).
//
// Phase carries the operator's own 0..1 cycle, before modulation. A cable
// into another operator's In Phase makes that one follow it at its Ratio
// (keep the ratio whole), locked in phase: a chain with no drift.

constexpr double kC4 = 261.6255653005986;

struct Op {
    double phase = 0.0;
    double y1 = 0.0, y2 = 0.0;  // the last two outputs, for feedback
};

struct Operator : Module {
    static constexpr ModuleInfo info = {
        .name = "Operator",
        .id = "com.suppleaudio.rack.operator",
        .vendor = "Supple Audio",
        .description =
            "A phase-modulation operator: chain them for FM stacks; outputs its sine "
            "and its phase.",
        .tags = {"oscillator", "fm", "poly"}};

    Param ratio = continuous("Ratio", 0.125, 16, 1)
                      .curved(Curve::Log)
                      .help(
                          "The operator's frequency over the pitch: whole numbers stay harmonic, "
                          "others clang.");
    Param fine = cents("Fine", -100, 100, 0).help("Detunes it in cents: a slow beating shimmer.");
    // In Hz, but not a frequency's octave cables: it passes through 0.
    Param offset =
        continuous("Offset", -20, 20, 0)
            .help("Adds this many Hz to its frequency, the same at every pitch: a fixed beating.");
    Param index = continuous("Index", 0, 10, 1)
                      .help("How far the PM input bends the phase, in radians at an input of 1.");
    Param feedback =
        percent("Feedback", 0)
            .help("Bends it by its own output: a sine at 0, towards a saw at 60, noise near 100.");
    Param level =
        percent("Level", 100)
            .smoothed(5)
            .help("The sine's level: a cable from an envelope shapes a modulator's brightness.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Poly: an operator per channel.");
    Input pm = audioIn("PM").help("Bends the phase by Index: patch another operator's Sine here.");
    Input inPhase =
        phaseIn("In Phase")
            .help("While connected, follows this phase times Ratio instead of its own pitch.");

    Output sine = audioOut("Sine").help("The operator's sine, times Level.");
    Output phase = phaseOut("Phase").help("Its own cycle, 0 to 1, before modulation.");

    Array<Op, 16> op;

    void process(int frames) {
        const double sr = sampleRate();
        const bool follow = inPhase.connected();
        for (int c = 0; c < channels(); ++c) {
            Op& o = op[c];
            for (int i = 0; i < frames; ++i) {
                const double r = ratio.at(i, c) * exp2(fine.at(i, c) / 1200.0);
                if (follow) {
                    const double p = inPhase[c][i] * r;
                    o.phase = p - floor(p);
                } else {
                    const double hz =
                        kC4 * exp2(clamp(pitch[c][i], -14.0, 7.0)) * r + offset.at(i, c);
                    o.phase += clamp(hz / sr, -0.45, 0.45);
                    o.phase -= floor(o.phase);
                }
                const double fb = 1.6 * clamp(feedback.at(i, c), 0.0, 1.0);
                const double bend = index.at(i, c) * pm[c][i] + fb * 0.5 * (o.y1 + o.y2);
                const double y = sin(twoPi * o.phase + bend);
                o.y2 = o.y1;
                o.y1 = y;
                sine[c][i] = y * clamp(level.at(i, c), 0.0, 1.0);
                phase[c][i] = o.phase;
            }
        }
    }
};
