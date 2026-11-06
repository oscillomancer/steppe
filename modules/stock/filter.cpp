// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A multimode state-variable filter, poly: low, band, high and notch outputs
// at once (12 dB/oct), and a steeper 24 dB/oct low pass. The cutoff is a knob
// in Hz that the Pitch input moves 1 per octave (keyboard tracking) and the FM
// input by FM Depth's octaves (an envelope's sweep, or audio-rate FM).
// Resonance rises to self-oscillation near the top of its travel: the filter
// then sings a sine at its cutoff, which the Pitch input plays. Drive
// saturates the input first, gently at its lowest, a fuzz at its highest.
//
// The core is a trapezoidal state-variable filter, whose coefficients may move
// every sample without clicks or blowing up. Its damping grows with the band
// output's level, which holds self-oscillation steady at the same level at
// any cutoff and rounds loud resonances, as an analog filter's would.
// The 24 dB output follows the resonant low pass with a second, gentle stage
// at the same cutoff.

struct Svf {
    double s1 = 0.0, s2 = 0.0;  // the integrators' states
    double low = 0.0, band = 0.0;
    double damping = 2.0;  // the k the last sample used
    double power = 0.0;    // the band output's power, over ~5 ms

    // One sample at g = tan(pi fc / fs) and damping k (2 / Q); `hold` adds
    // damping as the band output's power grows (smoothed by `follow`, a
    // one-pole's coefficient), which holds self-oscillation (k < 0) at a peak
    // of sqrt(-2 k / hold) at any cutoff.
    void run(double x, double g, double k0, double hold, double follow) {
        const double k = k0 + hold * power;
        damping = k;
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - s2;
        band = a1 * s1 + a2 * v3;
        low = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0 * band - s1;
        s2 = 2.0 * low - s2;
        power = band * band + follow * (power - band * band);
    }
};

struct Stages {
    Svf main, steep;
};

struct Filter : Module {
    static constexpr ModuleInfo info = {
        .name = "Filter",
        .id = "com.suppleaudio.rack.filter",
        .vendor = "Supple Audio",
        .description =
            "A multimode state-variable filter: low, band, high, notch and a 24 dB "
            "low pass at once.",
        .tags = {"filter", "poly"}};

    Param cutoff = frequency("Cutoff", 20, 20000, 1000)
                       .help("Where the filter turns; the Pitch input moves it 1 per octave.");
    Param resonance =
        percent("Resonance", 30)
            .help("The peak at the cutoff; past about 97 the filter sings a sine of its own.");
    Param drive =
        percent("Drive", 0).help("Saturates the input: clean at 0, warm at 30, a fuzz at 100.");
    Param fmDepth = octaves("FM Depth", -8, 8, 2)
                        .help("How far the FM input moves the cutoff, in octaves at a CV of 1.");

    Input in = audioIn("In").help("What it filters.");
    Input pitch = pitchIn("Pitch").help(
        "Moves the cutoff 1 per octave: patch the keyboard's pitch to track it.");
    Input fm = cvIn("FM").help("Moves the cutoff by FM Depth's octaves: an envelope or an LFO.");

    Output low = audioOut("Low").help("The low pass, 12 dB per octave.");
    Output band = audioOut("Band").help("The band pass around the cutoff.");
    Output high = audioOut("High").help("The high pass, 12 dB per octave.");
    Output notch = audioOut("Notch").help("Everything but the cutoff.");
    Output low24 = audioOut("Low 24").withKey("low24").help(
        "The low pass, 24 dB per octave: darker and steeper.");

    Array<Stages, 16> state;

    void process(int frames) {
        const double sr = sampleRate();
        const double top = 0.45 * sr;
        const double follow = exp(-1.0 / (0.005 * sr));
        for (int c = 0; c < channels(); ++c) {
            Stages& s = state[c];
            for (int i = 0; i < frames; ++i) {
                const double oct = pitch[c][i] + fmDepth.at(i, c) * fm[c][i];
                const double hz = clamp(cutoff.at(i, c) * exp2(clamp(oct, -20.0, 20.0)), 8.0, top);
                const double g = tan(pi * hz / sr);
                const double k = 2.0 - 2.05 * clamp(resonance.at(i, c), 0.0, 1.0);
                const double gain = 0.25 * dbToGain(36.0 * clamp(drive.at(i, c), 0.0, 1.0));
                const double x = tanh(gain * in[c][i]) / min(gain, 1.0);
                s.main.run(x, g, k, 0.2, follow);
                s.steep.run(s.main.low, g, 1.41421356, 0.0, 0.0);
                low[c][i] = s.main.low;
                band[c][i] = s.main.band;
                high[c][i] = x - s.main.damping * s.main.band - s.main.low;
                notch[c][i] = x - s.main.damping * s.main.band;
                low24[c][i] = s.steep.low;
            }
        }
    }
};
