// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A Euclidean rhythm: Pulses hits spread as evenly as they can be over
// Steps, turned by Rotation. Three in eight is the tresillo, five in eight
// the cinquillo, five in sixteen a bossa nova's clave. Accents are a second
// even spread over the hits themselves (Accents of them, turned by Accent
// Shift), so they always fall on a hit. Each clock plays the next step; with
// nothing in Clock it steps on the song's sixteenths, locked to the
// position. Gate is high with the clock on hits, Accent on accented hits,
// Off on the steps between: three interlocking parts from one module. A
// cable into Pulses morphs the rhythm from sparse to dense.

// Whether step j (from the pattern's start) of n holds one of k hits.
bool hitAt(int j, int n, int k) { return imod(j * k, n) < k; }

struct Euclid : Module {
    static constexpr ModuleInfo info = {
        .name = "Euclid",
        .id = "com.suppleaudio.rack.euclid",
        .vendor = "Supple Audio",
        .description = "A Euclidean rhythm with accents of its own, from a clock or the song.",
        .tags = {"sequencer", "rhythm", "generative"}};

    Param steps = integer("Steps", 1, 32, 16).in("Rhythm").help("How many steps the pattern has.");
    Param pulses = integer("Pulses", 0, 32, 5)
                       .in("Rhythm")
                       .help("How many of the steps are hits, spread as evenly as they go.");
    Param rotation =
        integer("Rotation", 0, 31, 0).in("Rhythm").help("How many steps later the pattern starts.");
    Param accents = integer("Accents", 0, 32, 2)
                        .in("Accents")
                        .help("How many of the hits are accented, spread evenly over them.");
    Param accentShift =
        integer("Shift", 0, 31, 0).in("Accents").help("How many hits later the accents start.");

    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next step; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge makes the next clock play the first step.");

    Output gate = gateOut("Gate").help("High with the clock on every hit.");
    Output accent = gateOut("Accent").help("High with the clock on accented hits.");
    Output off = gateOut("Off").help("High with the clock on the steps without a hit.");

    bool clockHigh = false;
    bool resetHigh = false;
    bool armed = false;
    int count = -1;
    int base = 0;
    int current = 0;
    bool isHit = false;
    bool isAccent = false;

    Shared<int> shownStep;

    void factoryPatches(Patches& p) {
        p.add("Bossa").set(steps, 16).set(pulses, 5).set(accents, 2);
        p.add("Tresillo").set(steps, 8).set(pulses, 3).set(accents, 1);
        p.add("Cinquillo").set(steps, 8).set(pulses, 5).set(accents, 2);
        p.add("Four on the Floor").set(steps, 16).set(pulses, 4).set(accents, 1);
        p.add("Off-Beat Hats").set(steps, 8).set(pulses, 4).set(rotation, 1).set(accents, 0);
        p.add("Thirteen in Seven").set(steps, 13).set(pulses, 7).set(accents, 3);
    }

    // A clock: the next step, and what it holds.
    void advance(int sixteenth, bool internal) {
        if (armed) {
            count = 0;
            base = sixteenth;
            armed = false;
        } else {
            count = internal ? sixteenth - base : count + 1;
        }
        const int n = clamp(roundToInt(steps.at(0)), 1, 32);
        const int k = clamp(roundToInt(pulses.at(0)), 0, n);
        const int r = clamp(roundToInt(rotation.at(0)), 0, 31);
        current = wrap(count, n);
        const int j = wrap(current - r, n);
        isHit = hitAt(j, n, k);
        isAccent = false;
        if (isHit) {
            int index = 0;  // which hit this is
            for (int q = 0; q < j; ++q)
                if (hitAt(q, n, k)) ++index;
            const int a = clamp(roundToInt(accents.at(0)), 0, k);
            const int shift = clamp(roundToInt(accentShift.at(0)), 0, 31);
            isAccent = a > 0 && hitAt(wrap(index - shift, k), k, a);
        }
    }

    void process(int frames) {
        gate.setChannels(1);
        accent.setChannels(1);
        off.setChannels(1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        const double perFrame = t.tempo / 15.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                armed = true;
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            bool high = false;
            int sixteenth = 0;
            if (internal) {
                if (t.playing) {
                    const double position = t.beats * 4.0 + i * perFrame + 1e-7;
                    sixteenth = floorToInt(position);
                    high = position - sixteenth < 0.5;
                }
            } else {
                const double c = clock[0][i];
                high = clockHigh ? c >= 0.25 : c >= 0.5;
            }
            if (high && !clockHigh) advance(sixteenth, internal);
            clockHigh = high;
            gate[0][i] = high && isHit ? 1.0 : 0.0;
            accent[0][i] = high && isAccent ? 1.0 : 0.0;
            off[0][i] = high && !isHit ? 1.0 : 0.0;
        }
        shownStep.set(count < 0 ? -1 : current);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 352);
        p.display(8, 28, 160, 124);
        p.knob(steps, 30, 180);
        p.knob(pulses, 88, 180);
        p.knob(rotation, 146, 180);
        p.knob(accents, 59, 234);
        p.knob(accentShift, 117, 234);
        p.input(clock, 30, 286);
        p.input(reset, 88, 286);
        p.output(gate, 30, 326);
        p.output(accent, 88, 326);
        p.output(off, 146, 326);
    }

    void draw(Canvas& g) {
        g.well(8, 28, 160, 124);
        const int n = clamp(steps.index(), 1, 32);
        const int k = clamp(pulses.index(), 0, n);
        const int r = clamp(rotation.index(), 0, 31);
        const int a = clamp(accents.index(), 0, k);
        const int shift = clamp(accentShift.index(), 0, 31);
        const int playing = shownStep.get();
        const double cx = 88.0;
        const double cy = 90.0;
        const double radius = 48.0;
        // The ring, and the hits joined into a polygon.
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.beginPath();
        g.circle(cx, cy, radius);
        g.stroke();
        if (k > 1) {
            g.strokeStyle(Theme::accent);
            g.globalAlpha(0.35);
            g.beginPath();
            bool first = true;
            for (int s = 0; s < n; ++s) {
                if (!hitAt(wrap(s - r, n), n, k)) continue;
                const double angle = twoPi * s / n - pi * 0.5;
                const double x = cx + radius * cos(angle);
                const double y = cy + radius * sin(angle);
                if (first)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
                first = false;
            }
            g.closePath();
            g.stroke();
            g.globalAlpha(1.0);
        }
        for (int s = 0; s < n; ++s) {
            const int j = wrap(s - r, n);
            const double angle = twoPi * s / n - pi * 0.5;
            const double x = cx + radius * cos(angle);
            const double y = cy + radius * sin(angle);
            bool hit = false;
            bool accented = false;
            if (hitAt(j, n, k)) {
                hit = true;
                int which = 0;
                for (int q = 0; q < j; ++q)
                    if (hitAt(q, n, k)) ++which;
                accented = a > 0 && hitAt(wrap(which - shift, k), k, a);
            }
            if (s == playing) {
                g.strokeStyle(Theme::text);
                g.lineWidth(1.5);
                g.beginPath();
                g.circle(x, y, 7.0);
                g.stroke();
            }
            g.fillStyle(hit ? (accented ? Theme::text : Theme::accent) : Theme::recess);
            g.beginPath();
            g.circle(x, y, hit ? (accented ? 4.6 : 3.6) : 2.4);
            g.fill();
            if (!hit) {
                g.strokeStyle(Theme::lineLit);
                g.lineWidth(1.0);
                g.stroke();
            }
        }
        g.fillStyle(Theme::textMuted);
        g.monoFont(11);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(format("%d / %d", k, n), cx, cy);
    }
};
