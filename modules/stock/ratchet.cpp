// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Ratchets: a gate in becomes a burst of Count quick pulses, spread over
// Spread of the time to the next gate (100 % fills it evenly: three
// ratchets on a sixteenth are a sixteenth-note triplet). Chance and Every
// choose which gates burst: every gate, every second or third, at random,
// or both; the rest pass as they came. Curve bunches the pulses towards
// the end (speeding up, a bouncing ball) or the start (slowing down).
// Length is each pulse's share of its gap, and Level falls by Decay with
// each pulse, for rolls that fade. Poly: each channel counts and tosses on
// its own. Reset starts the counts and the tosses over.

struct Ratchet : Module {
    static constexpr ModuleInfo info = {
        .name = "Ratchet",
        .id = "com.suppleaudio.rack.ratchet",
        .vendor = "Supple Audio",
        .description = "Gates into bursts of quick pulses: by chance or every Nth, curved, fading.",
        .tags = {"rhythm", "random", "generative", "poly"}};

    Param count = integer("Count", 1, 8, 3).help("How many pulses a burst has.");
    Param chance = percent("Chance", 100).help("How likely a gate is to burst.");
    Param every = integer("Every", 1, 8, 1)
                      .help("Burst only every Nth gate, counting from the reset (then Chance).");
    Param spread = percent("Spread", 100, 10, 100)
                       .help("How much of the time to the next gate a burst fills.");
    Param curve = percent("Curve", 0, -100, 100)
                      .help("Above 0 the pulses speed up through the burst; below, slow down.");
    Param length = percent("Length", 50, 5, 95).help("Each pulse's share of the gap to the next.");
    Param decay =
        percent("Decay", 0).help("How much lower each pulse's Level is than the one before.");

    Input gate = gateIn("Gate").help("The gates to burst (rises at 0.5, falls below 0.25); poly.");
    Input reset = gateIn("Reset").help("A rising edge starts the counts and the tosses over.");

    Output out = gateOut("Out").help("The bursts, and the gates that pass as they came.");
    Output level =
        cvOut("Level").help("Each pulse's level, 1 falling by Decay; 1 for a passed gate.");

    struct Voice {
        Trigger edge;
        Random rng;
        int gates = 0;        // gates since the reset
        int since = 1 << 30;  // frames since the last gate rose
        int period = 0;       // frames between the last two gates; 0 unknown
        int pulses = 0;       // the burst's count; 0 when passing the gate
        double span = 0.0;    // frames the burst fills
        double shape = 1.0;   // the curve's exponent
        int pulse = -1;       // the pulse playing
        bool wasHigh = false;
        bool lowFrame = false;  // one frame low, so a gate during a pulse rises again
    };
    Array<Voice, 16> voices;
    Trigger resetEdge;
    int salt = 0;

    Shared<int> shownPulse;  // the first channel's pulse playing, -1 none

    void factoryPatches(Patches& p) {
        p.add("Triplet Rolls").set(count, 3);
        p.add("Every Fourth").set(count, 4).set(every, 4);
        p.add("Bouncing Ball").set(count, 6).set(curve, 70).set(decay, 25).set(spread, 90);
        p.add("Random Flams").set(count, 2).set(chance, 30).set(spread, 25);
        p.add("Slowing Fade").set(count, 5).set(curve, -50).set(decay, 30);
    }

    void startOver() {
        for (int c = 0; c < 16; ++c) {
            voices[c].rng.seed(salt + 7919 * c);
            voices[c].gates = 0;
            voices[c].pulses = 0;
        }
    }

    void prepare() {
        salt = seed();
        startOver();
    }

    // Where pulse k of n starts, in frames from the gate.
    double startOf(const Voice& v, int k) const {
        return v.span * pow(static_cast<double>(k) / v.pulses, v.shape);
    }

    void process(int frames) {
        const int n = max(1, channels());
        out.setChannels(n);
        level.setChannels(n);
        const Transport t = transport();
        // Until two gates have come, a sixteenth at the song's tempo.
        const int guess = max(2, roundToInt(sampleRate() * 15.0 / max(1.0, t.tempo)));
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            for (int c = 0; c < n; ++c) {
                Voice& v = voices[c];
                if (v.since < (1 << 30)) ++v.since;
                if (v.edge.rose(gate[c][i])) {
                    if (v.since < (1 << 30)) v.period = v.since;
                    v.since = 0;
                    // One draw a gate, whatever the settings.
                    const double toss = v.rng.next();
                    const int nth = clamp(roundToInt(every.at(i, c)), 1, 8);
                    const bool burst = imod(v.gates, nth) == 0 && toss < chance.at(i, c);
                    ++v.gates;
                    v.pulses = burst ? clamp(roundToInt(count.at(i, c)), 1, 8) : 0;
                    v.span = (v.period > 0 ? v.period : guess) * clamp(spread.at(i, c), 0.1, 1.0);
                    v.shape = pow(2.0, -1.5 * clamp(curve.at(i, c), -1.0, 1.0));
                    v.pulse = -1;
                    v.lowFrame = v.wasHigh;
                }
                double on = 0.0;
                double lv = 1.0;
                if (v.pulses > 0) {
                    const double x = v.since / v.span;
                    if (x < 1.0) {
                        int k =
                            clamp(floorToInt(v.pulses * pow(x, 1.0 / v.shape)), 0, v.pulses - 1);
                        // Rounding at a boundary: settle on the pulse that has begun.
                        while (k > 0 && startOf(v, k) > v.since) --k;
                        while (k + 1 < v.pulses && startOf(v, k + 1) <= v.since) ++k;
                        const double begin = startOf(v, k);
                        const double gap = startOf(v, k + 1) - begin;
                        const double high = clamp(gap * clamp(length.at(i, c), 0.05, 0.95), 1.0,
                                                  max(1.0, gap - 1.0));
                        on = v.since - begin < high ? 1.0 : 0.0;
                        v.pulse = k;
                        lv = pow(1.0 - clamp(decay.at(i, c), 0.0, 1.0), k);
                    } else {
                        v.pulses = 0;
                        v.pulse = -1;
                        on = 0.0;
                        lv = pow(1.0 - clamp(decay.at(i, c), 0.0, 1.0),
                                 clamp(roundToInt(count.at(i, c)), 1, 8) - 1);
                    }
                } else {
                    on = v.edge.high() ? 1.0 : 0.0;
                }
                if (v.lowFrame) {
                    on = 0.0;
                    v.lowFrame = false;
                }
                out[c][i] = on;
                v.wasHigh = on > 0.5;
                level[c][i] = lv;
            }
        }
        shownPulse.set(voices[0].pulses > 0 ? voices[0].pulse : -1);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 330);
        p.display(8, 28, 160, 62);
        p.knob(count, 30, 124);
        p.knob(chance, 88, 124);
        p.knob(every, 146, 124);
        p.knob(spread, 30, 182);
        p.knob(curve, 88, 182);
        p.knob(length, 146, 182);
        p.knob(decay, 30, 240);
        p.input(gate, 30, 298);
        p.input(reset, 74, 298);
        p.output(out, 118, 298);
        p.output(level, 156, 298);
    }

    // The burst the knobs make: each pulse a bar, as tall as its level.
    void draw(Canvas& g) {
        const double left = 8.0;
        const double top = 28.0;
        const double w = 160.0;
        const double h = 62.0;
        g.well(left, top, w, h);
        const int pulses = clamp(count.index(), 1, 8);
        const double shape = pow(2.0, -1.5 * clamp(curve.value(), -1.0, 1.0));
        const double fill = clamp(spread.value(), 0.1, 1.0);
        const double share = clamp(length.value(), 0.05, 0.95);
        const double fall = 1.0 - clamp(decay.value(), 0.0, 1.0);
        const double x0 = left + 8.0;
        const double width = w - 16.0;
        const double base = top + h - 8.0;
        const double tall = h - 16.0;
        const int lit = shownPulse.get();
        // The time to the next gate.
        g.strokeStyle(Theme::lineLit);
        g.lineWidth(1.0);
        g.beginPath();
        g.moveTo(x0, base + 0.5);
        g.lineTo(x0 + width, base + 0.5);
        g.moveTo(x0 + width, base - tall);
        g.lineTo(x0 + width, base + 3);
        g.stroke();
        double lv = 1.0;
        for (int k = 0; k < pulses; ++k) {
            const double a = fill * pow(static_cast<double>(k) / pulses, shape);
            const double b = fill * pow(static_cast<double>(k + 1) / pulses, shape);
            const double x = x0 + width * a;
            const double bw = max(1.5, width * (b - a) * share);
            g.fillStyle(k == lit ? Theme::text : Theme::accent);
            g.globalAlpha(k == lit ? 1.0 : 0.85);
            g.beginPath();
            g.roundRect(x, base - tall * lv, bw, tall * lv, 1.5);
            g.fill();
            lv *= fall;
        }
        g.globalAlpha(1.0);
    }
};
