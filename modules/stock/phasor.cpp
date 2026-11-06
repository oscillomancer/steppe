// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A phase generator, poly: a 0..1 ramp a cycle at the pitch, the raw stuff
// of oscillators. Patch it into a Shaper for phase distortion synthesis,
// into an Operator's In Phase to lock FM stacks together, or slow it down as
// a clock and an LFO. Skew bends the ramp (its first half faster than its
// second, or slower), which through a Shaper's sine sweeps from pure to
// brassy; Offset shifts it (a cable into Offset is phase modulation). Sync
// restarts the cycle as it rises; Wrap fires a trigger at each start.

constexpr double kC4 = 261.6255653005986;

double warpOf(double p, double a) { return p * a / (1.0 + p * (a - 1.0)); }

struct Ramp {
    double phase = 0.0;
    int hold = 0;  // frames left of Wrap's trigger
    bool high = false;
};

struct Phasor : Module {
    static constexpr ModuleInfo info = {
        .name = "Phasor",
        .id = "com.suppleaudio.rack.phasor",
        .vendor = "Supple Audio",
        .description =
            "A phase generator: a 0..1 ramp at the pitch, skewed and offset, for "
            "shapers and operators.",
        .tags = {"oscillator", "phase", "poly"}};

    Param coarse = semitones("Coarse", -48, 48, 0).help("Transposes the pitch in semitones.");
    Param fine = cents("Fine", -100, 100, 0).help("Detunes the pitch in cents.");
    Param skew = percent("Skew", 0, -100, 100)
                     .help(
                         "Bends the ramp: above 0 it races through its first half, below 0 through "
                         "its second.");
    Param offset =
        percent("Offset", 0)
            .help("Shifts the ramp by this share of a cycle; a cable into it is phase modulation.");

    Input pitch = pitchIn("Pitch").help("1 per octave; 0 is C4. Slow it right down for an LFO.");
    Input sync = gateIn("Sync").help("Restarts the cycle each time it rises.");

    Output phase = phaseOut("Phase").help("The ramp, 0 to 1 a cycle, skewed and offset.");
    Output wrap = gateOut("Wrap").help("A 1 ms trigger at the start of each cycle.");

    Array<Ramp, 16> ramp;

    void process(int frames) {
        const double sr = sampleRate();
        for (int c = 0; c < channels(); ++c) {
            Ramp& r = ramp[c];
            for (int i = 0; i < frames; ++i) {
                const double oct = pitch[c][i] + coarse.at(i, c) / 12.0 + fine.at(i, c) / 1200.0;
                const double inc = min(kC4 * exp2(clamp(oct, -20.0, 7.0)) / sr, 0.45);
                const double g = sync[c][i];
                bool start = false;
                if (!r.high && g >= 0.5) {
                    r.high = true;
                    r.phase = 0.0;
                    start = true;
                } else {
                    if (r.high && g < 0.25) r.high = false;
                    r.phase += inc;
                    if (r.phase >= 1.0) {
                        r.phase -= floor(r.phase);
                        start = true;
                    }
                }
                if (start) r.hold = roundToInt(0.001 * sr);
                wrap[c][i] = r.hold > 0 ? 1.0 : 0.0;
                r.hold = max(r.hold - 1, 0);
                const double a = exp2(3.0 * clamp(skew.at(i, c), -1.0, 1.0));
                const double q = warpOf(r.phase, a) + clamp(offset.at(i, c), 0.0, 1.0);
                phase[c][i] = q - floor(q);
            }
        }
    }
};
