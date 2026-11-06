// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A slew limiter, poly: the output follows the input no faster than Rise
// allows going up and Fall going down (each the time a whole unit, an
// octave of pitch, takes). Shape bends the move from a straight line (a
// constant rate, a constant glide in pitch) to an exponential lag that
// slows as it arrives. Glide Limit makes it a portamento for close notes
// only: a jump wider than it is taken at once, so leaps stay crisp and
// steps glide. Smooths stepped CV, rounds a square into a shark fin, glides
// a melody.

struct Slew : Module {
    static constexpr ModuleInfo info = {
        .name = "Slew",
        .id = "com.suppleaudio.rack.slew",
        .vendor = "Supple Audio",
        .description = "A slew limiter and portamento, linear to exponential, poly.",
        .tags = {"slew", "portamento", "utility", "poly"}};

    Param rise =
        milliseconds("Rise", 0, 10000, 100)
            .help("The time to climb a whole unit (an octave of pitch); 0 follows at once.");
    Param fall = milliseconds("Fall", 0, 10000, 100)
                     .help("The time to drop a whole unit; 0 follows at once.");
    Param shape =
        percent("Shape", 0)
            .help(
                "0 % moves at a constant rate; 100 % is an exponential lag, fast at first and "
                "settling, which takes about the time given whatever the distance.");
    Param glideLimit =
        semitones("Glide Limit", 0, 48, 48)
            .help(
                "Jumps wider than this many semitones are taken at once; at 48 every jump slews.");

    Input in = cvIn("In").help("What it follows; poly.");
    Output out = cvOut("Out").help("The input, slewed.");

    Array<double, 16> level;
    Array<double, 16> last;  // the input a frame ago, to tell a jump
    bool fresh = true;

    void factoryPatches(Patches& p) {
        p.add("Portamento").set(rise, 80).set(fall, 80).set(shape, 100);
        p.add("Close Glide").set(rise, 120).set(fall, 120).set(shape, 60).set(glideLimit, 4);
        p.add("Shark Fin").set(rise, 300).set(fall, 20).set(shape, 100);
        p.add("Slow Smooth").set(rise, 2000).set(fall, 2000);
    }

    void process(int frames) {
        const double msToStep = 1000.0 / sampleRate();
        for (int c = 0; c < channels(); ++c)
            for (int i = 0; i < frames; ++i) {
                const double x = in[c][i];
                const double limit = glideLimit.at(i, c);
                if (fresh || (limit < 48.0 && abs(x - last[c]) * 12.0 > limit + 1e-9)) {
                    level[c] = x;
                } else {
                    const double d = x - level[c];
                    const double ms = d > 0.0 ? rise.at(i, c) : fall.at(i, c);
                    if (ms <= 0.0) {
                        level[c] = x;
                    } else {
                        const double step = msToStep / ms;
                        const double straight = clamp(d, -step, step);
                        const double lag = d * (1.0 - exp(-4.0 * step));
                        const double k = clamp(shape.at(i, c), 0.0, 1.0);
                        level[c] += straight + (lag - straight) * k;
                    }
                }
                last[c] = x;
                out[c][i] = level[c];
            }
        fresh = false;
    }
};
