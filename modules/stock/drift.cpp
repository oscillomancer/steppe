// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Slow random motion across a poly cable: each voice wanders on its own
// smooth path, never stepping and never quite repeating, about Rate times
// a second. Together blends every voice towards one shared path: at 0 %
// they move apart, at 100 % as one, and between, like players who mostly
// agree. Spread lets each voice wander at its own pace. Into a poly
// oscillator's pitch it detunes a chord the way old machines drift; into
// cutoffs, pans and levels it keeps a patch alive. Shared sends the path
// they all lean towards. Reset starts the same motion over.

constexpr int kDriftSources = 17;  // sixteen voices and the shared path
constexpr int kDriftShown = 96;    // display points a voice

struct Drift : Module {
    static constexpr ModuleInfo info = {
        .name = "Drift",
        .id = "com.suppleaudio.rack.drift",
        .vendor = "Supple Audio",
        .description = "Slow, smooth random motion across poly voices, apart or together.",
        .tags = {"modulation", "random", "generative", "poly"}};

    Param rate = frequency("Rate", 0.01, 20, 0.3).help("About how many turns a second it takes.");
    Param voices = integer("Voices", 1, 16, 4).help("How many voices: one channel each.");
    Param together =
        percent("Together", 30).help("How much the voices follow one shared path, not their own.");
    Param spread =
        percent("Spread", 25).help("How far each voice's pace differs from Rate: up to 2x or 1/2.");
    Param depth = percent("Depth", 100).help("How far the voices move: 100 % is the full range.");
    Param polarity = choice("Polarity", {"Bipolar", "Unipolar"}, 0)
                         .help("Around 0, from -1 to 1; or from 0 to 1, around 0.5.");

    Input reset = gateIn("Reset").help("A rising edge starts the same motion over.");

    Output out = cvOut("Out").help("Each voice's motion, one channel a voice.");
    Output shared = cvOut("Shared").help("The path the voices lean towards, at Depth.");

    // Each source: four knots of a smooth curve and where it is between
    // the middle two.
    Array<double, kDriftSources> k0;
    Array<double, kDriftSources> k1;
    Array<double, kDriftSources> k2;
    Array<double, kDriftSources> k3;
    Array<double, kDriftSources> phase;
    Array<double, kDriftSources> pace;  // each source's multiple of Rate
    Array<Random, kDriftSources> rng;
    Trigger resetEdge;
    int salt = 0;
    int sinceShown = 0;

    SharedArray<float, 16 * kDriftShown> shownPath;
    SharedArray<float, kDriftShown> shownShared;
    Shared<int> shownVoices;

    void factoryPatches(Patches& p) {
        p.add("Ensemble").set(voices, 4).set(together, 30);
        p.add("Tape Wow").set(voices, 1).set(rate, 0.8).set(depth, 15);
        p.add("Flock").set(voices, 8).set(together, 75).set(rate, 0.15);
        p.add("Scatter").set(voices, 8).set(together, 0).set(spread, 100).set(rate, 0.5);
        p.add("Glacier").set(voices, 4).set(rate, 0.02).set(polarity, "unipolar");
    }

    void startOver() {
        for (int s = 0; s < kDriftSources; ++s) {
            rng[s].seed(salt + 7919 * s);
            const double u = rng[s].bipolar();
            pace[s] = s == 16 ? 1.0 : u;
            k0[s] = rng[s].bipolar();
            k1[s] = rng[s].bipolar();
            k2[s] = rng[s].bipolar();
            k3[s] = rng[s].bipolar();
            phase[s] = 0.0;
        }
    }

    void prepare() {
        salt = seed();
        startOver();
        for (int k = 0; k < 16 * kDriftShown; ++k) shownPath[k] = 0.0f;
        for (int k = 0; k < kDriftShown; ++k) shownShared[k] = 0.0f;
    }

    // Source s moved on by `step` turns: a Catmull-Rom curve through its knots.
    double move(int s, double step) {
        phase[s] += step;
        while (phase[s] >= 1.0) {
            phase[s] -= 1.0;
            k0[s] = k1[s];
            k1[s] = k2[s];
            k2[s] = k3[s];
            k3[s] = rng[s].bipolar();
        }
        const double t = phase[s];
        const double t2 = t * t;
        const double t3 = t2 * t;
        return 0.5 * (2.0 * k1[s] + (k2[s] - k0[s]) * t +
                      (2.0 * k0[s] - 5.0 * k1[s] + 4.0 * k2[s] - k3[s]) * t2 +
                      (3.0 * k1[s] - k0[s] - 3.0 * k2[s] + k3[s]) * t3);
    }

    void process(int frames) {
        const int n = clamp(max(voices.index(), channels()), 1, 16);
        out.setChannels(n);
        shared.setChannels(1);
        const bool unipolar = polarity.index() == 1;
        const double perFrame = 1.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            const double common = move(16, clamp(rate.at(i), 0.0, 20.0) * perFrame);
            const double d = clamp(depth.at(i), 0.0, 1.0);
            const double sv = clamp(common, -1.0, 1.0) * d;
            shared[0][i] = unipolar ? 0.5 + 0.5 * sv : sv;
            for (int c = 0; c < n; ++c) {
                const double sp = clamp(spread.at(i, c), 0.0, 1.0);
                const double r = clamp(rate.at(i, c), 0.0, 20.0) * pow(2.0, sp * pace[c]);
                const double own = move(c, r * perFrame);
                const double w = clamp(together.at(i, c), 0.0, 1.0);
                // Equal power, so the motion is as wide at every blend.
                const double mixed =
                    (w * common + (1.0 - w) * own) / sqrt(w * w + (1.0 - w) * (1.0 - w));
                const double v = clamp(mixed, -1.0, 1.0) * clamp(depth.at(i, c), 0.0, 1.0);
                out[c][i] = unipolar ? 0.5 + 0.5 * v : v;
            }
        }
        // The display: about eight turns across, at the first voice's pace.
        sinceShown += frames;
        const int every =
            max(1, roundToInt(sampleRate() * 8.0 / (max(0.01, rate.value()) * kDriftShown)));
        if (sinceShown >= every) {
            sinceShown = 0;
            for (int c = 0; c < 16; ++c) {
                for (int k = 0; k < kDriftShown - 1; ++k)
                    shownPath[c * kDriftShown + k] = shownPath[c * kDriftShown + k + 1];
                shownPath[c * kDriftShown + kDriftShown - 1] =
                    c < n ? static_cast<float>(out[c][frames - 1]) : 0.0f;
            }
            for (int k = 0; k < kDriftShown - 1; ++k) shownShared[k] = shownShared[k + 1];
            shownShared[kDriftShown - 1] = static_cast<float>(shared[0][frames - 1]);
        }
        shownVoices.set(n);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 336);
        p.display(8, 28, 160, 100);
        p.knob(rate, 30, 160);
        p.knob(voices, 88, 160);
        p.knob(together, 146, 160);
        p.knob(spread, 30, 220);
        p.knob(depth, 88, 220);
        p.input(reset, 146, 220);
        p.choice(polarity, 8, 256, 160, 18);
        p.output(out, 88, 304);
        p.output(shared, 146, 304);
    }

    void draw(Canvas& g) {
        const double left = 8.0;
        const double top = 28.0;
        const double w = 160.0;
        const double h = 100.0;
        g.well(left, top, w, h);
        const bool unipolar = polarity.index() == 1;
        const double cy = top + h / 2.0;
        const double half = h / 2.0 - 6.0;
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(left + 4, cy);
        g.lineTo(left + w - 4, cy);
        g.stroke();
        g.save();
        g.clip(left + 1, top + 1, w - 2, h - 2);
        g.lineJoin(LineJoin::Round);
        g.lineCap(LineCap::Round);
        const int n = clamp(shownVoices.get(), 1, 16);
        const double dx = (w - 8.0) / (kDriftShown - 1);
        // The shared path, wide and faint, under the voices.
        g.strokeStyle(Theme::text);
        g.globalAlpha(0.18);
        g.lineWidth(5.0);
        g.beginPath();
        for (int k = 0; k < kDriftShown; ++k) {
            double v = static_cast<double>(shownShared[k]);
            if (unipolar) v = 2.0 * v - 1.0;
            const double x = left + 4.0 + k * dx;
            if (k == 0)
                g.moveTo(x, cy - v * half);
            else
                g.lineTo(x, cy - v * half);
        }
        g.stroke();
        g.lineWidth(1.3);
        for (int c = 0; c < n; ++c) {
            g.strokeStyle(imod(c, 2) == 0 ? Theme::accent : Theme::accent2);
            g.globalAlpha(0.95 - 0.5 * c / 16.0);
            g.beginPath();
            for (int k = 0; k < kDriftShown; ++k) {
                double v = static_cast<double>(shownPath[c * kDriftShown + k]);
                if (unipolar) v = 2.0 * v - 1.0;
                const double x = left + 4.0 + k * dx;
                if (k == 0)
                    g.moveTo(x, cy - v * half);
                else
                    g.lineTo(x, cy - v * half);
            }
            g.stroke();
        }
        g.restore();
        g.globalAlpha(1.0);
    }
};
