// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A granular processor: the input runs into a four-second buffer, and up to
// thirty-two grains, short windowed slices of it, play back from it at
// once: clouds, stretched textures, shimmering pitch trails, stutters.
//
// Density is how many grains start each second, Size how long each lasts;
// Position how far back in the buffer they start (0 is now, so a grain
// follows the input just behind it), Spray how much further back at random.
// Pitch transposes every grain, Scatter each one at random by up to its
// semitones either way; Reverse is the chance a grain plays backwards;
// Spread pans the grains across the stereo field at random.
//
// Freeze, or a gate into Freeze Gate, stops the buffer recording: the
// grains go on playing what it holds, and Position scans through it. A
// grain that would run past the present (pitched up, near now) or off the
// buffer's end is started where it fits. The level is kept roughly steady as
// grains overlap: at Size times Density 2, with no spray or scatter, the
// grains add up to the input itself, delayed by Position.

constexpr int kGrains = 32;

struct Grain {
    bool on = false;
    double delay = 0.0;  // samples behind the newest
    double ratio = 1.0;  // its speed through the buffer
    double dir = 1.0;    // 1 forwards, -1 reversed
    double age = 0.0, length = 1.0;
    double left = 0.0, right = 0.0;
};

struct Granular : Module {
    static constexpr ModuleInfo info = {
        .name = "Granular",
        .id = "com.suppleaudio.rack.granular",
        .vendor = "Supple Audio",
        .description =
            "A granular processor on a live four-second buffer: clouds, freezes, "
            "pitch trails.",
        .tags = {"granular", "effect", "stereo"}};

    Param size =
        milliseconds("Size", 10, 1000, 100).in("Grains").help("How long each grain lasts.");
    Param density =
        frequency("Density", 0.5, 200, 20).in("Grains").help("How many grains start each second.");
    Param position = milliseconds("Position", 0, 3800, 0)
                         .in("Buffer")
                         .help("How far back in the buffer grains start: 0 follows the input.");
    Param spray = milliseconds("Spray", 0, 1000, 0)
                      .in("Buffer")
                      .help("Starts each grain up to this much further back, at random.");
    Param pitch = semitones("Pitch", -24, 24, 0).in("Pitch").help("Transposes every grain.");
    Param scatter = semitones("Scatter", 0, 12, 0)
                        .in("Pitch")
                        .help("Transposes each grain at random by up to this, either way.");
    Param reverse = percent("Reverse", 0).in("Pitch").help("The chance a grain plays backwards.");
    Param spread =
        percent("Spread", 50).in("Output").help("Pans grains at random across the stereo field.");
    Param freeze =
        toggle("Freeze", false).in("Buffer").help("Stops recording: the grains play what is held.");
    Param mix =
        percent("Mix", 100).smoothed(20).in("Output").help("The grains' share, the rest dry.");

    Input in = audioIn("In").help("What goes into the buffer; a poly cable is summed.");
    Input freezeGate = gateIn("Freeze Gate").help("Freezes the buffer while high, as Freeze does.");

    Output outL = audioOut("Out L").help("The grains panned left, and the dry input.");
    Output outR = audioOut("Out R").help("The grains panned right, and the dry input.");

    DelayLine buffer;
    Array<Grain, kGrains> grain;
    Random random;
    double countdown = 1.0;  // samples to the next grain: one starts at once
    bool gateHigh = false;

    void prepare() {
        buffer.prepare(toInt(4.0 * sampleRate()) + 8);
        random.seed(seed());
    }

    // Starts a grain, if one is free.
    void spawn(bool frozen, int i) {
        int k = -1;
        for (int g = 0; g < kGrains; ++g)
            if (k < 0 && !grain[g].on) k = g;
        if (k < 0) return;
        const double sr = sampleRate();
        const double sprayed = random.next();
        const double shiftBy = random.bipolar();
        const double backwards = random.next();
        const double pan = random.bipolar();
        Grain& g = grain[k];
        g.on = true;
        g.age = 0.0;
        g.length = max(1.0, size.at(i, 0) * 0.001 * sr);
        g.ratio = exp2((pitch.at(i, 0) + scatter.at(i, 0) * shiftBy) / 12.0);
        g.dir = backwards < clamp(reverse.at(i, 0), 0.0, 1.0) ? -1.0 : 1.0;
        // How its delay moves each sample, and where it may start so that it
        // stays inside the buffer and behind the present all through.
        const double move = (frozen ? 0.0 : 1.0) - g.dir * g.ratio;
        const double top = 4.0 * sr - 4.0;
        double lo = 2.0, hi = top;
        if (move < 0.0) lo = 2.0 - move * g.length;
        if (move > 0.0) hi = top - move * g.length;
        if (lo > hi) {
            g.length = (top - 2.0) / abs(move);
            lo = move < 0.0 ? top : 2.0;
            hi = lo;
        }
        const double want = (position.at(i, 0) + spray.at(i, 0) * sprayed) * 0.001 * sr;
        g.delay = clamp(want, lo, hi);
        const double p = clamp(spread.at(i, 0), 0.0, 1.0) * pan;
        g.left = sqrt(0.5 * (1.0 - p));
        g.right = sqrt(0.5 * (1.0 + p));
    }

    void process(int frames) {
        const double sr = sampleRate();
        outL.setChannels(1);
        outR.setChannels(1);
        for (int i = 0; i < frames; ++i) {
            const double fg = freezeGate[0][i];
            if (!gateHigh && fg >= 0.5) gateHigh = true;
            if (gateHigh && fg < 0.25) gateHigh = false;
            const bool frozen = gateHigh || freeze.at(i, 0) >= 0.5;
            double x = 0.0;
            for (int c = 0; c < max(in.channels(), 1); ++c) x += in[c][i];
            if (!frozen) buffer.push(x);
            countdown -= 1.0;
            if (countdown <= 0.0) {
                countdown += sr / clamp(density.at(i, 0), 0.1, 1000.0);
                spawn(frozen, i);
            }
            const double top = 4.0 * sr - 4.0;
            double l = 0.0, r = 0.0;
            for (int k = 0; k < kGrains; ++k) {
                Grain& g = grain[k];
                if (!g.on) continue;
                const double w = 0.5 - 0.5 * cos(twoPi * g.age / g.length);
                const double y = buffer.read(g.delay) * w;
                l += y * g.left;
                r += y * g.right;
                const double move = (frozen ? 0.0 : 1.0) - g.dir * g.ratio;
                g.delay = clamp(g.delay + move, 2.0, top);
                g.age += 1.0;
                if (g.age >= g.length) g.on = false;
            }
            // Grains at a 50 % overlap add up to 1; denser ones are scaled as
            // if unrelated. Each side carries half the power, so times sqrt 2.
            const double overlap = size.at(i, 0) * 0.001 * density.at(i, 0);
            const double gain = sqrt(2.0 / max(overlap, 2.0)) * sqrt(2.0);
            const double m = clamp(mix.at(i, 0), 0.0, 1.0);
            outL[0][i] = m * l * gain + (1.0 - m) * x;
            outR[0][i] = m * r * gain + (1.0 - m) * x;
        }
    }
};
