// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Four rhythms against one span of the song: lane 1 beats Count 1 times a
// bar (or a beat, or two or four bars), lane 2 Count 2 times, and so on:
// 4 against 3 against 5 against 7 by default, every lane meeting again on
// the span's downbeat. Each lane sends a ramp, 0 to 1 over each of its
// beats (to drive a function, a shaper or a crossfade in step), and a gate
// high for Width of each beat. Any fires as any lane starts a beat, and
// Hits says how many started together, from 1/4 to 4/4: an accent where
// the rhythms coincide. Locked to the song position; with the song stopped
// it runs at the song's tempo, and Reset starts the span over.

struct Polyrhythm : Module {
    static constexpr ModuleInfo info = {
        .name = "Polyrhythm",
        .id = "com.suppleaudio.rack.polyrhythm",
        .vendor = "Supple Audio",
        .description = "Four rhythms of any count against a bar: ramps, gates and their meetings.",
        .tags = {"rhythm", "clock", "modulation"}};

    Param span = choice("Span", {"1 Beat", "1 Bar", "2 Bars", "4 Bars"}, 1)
                     .keys({"beat_1", "bar_1", "bars_2", "bars_4"})
                     .help("The stretch of the song every lane divides.");
    Param count_1 = integer("Count 1", 1, 16, 4).help("Lane 1's beats in the span.");
    Param count_2 = integer("Count 2", 1, 16, 3).help("Lane 2's beats in the span.");
    Param count_3 = integer("Count 3", 1, 16, 5).help("Lane 3's beats in the span.");
    Param count_4 = integer("Count 4", 1, 16, 7).help("Lane 4's beats in the span.");
    Param width = percent("Width", 25, 1, 95).help("How much of each beat a gate is high.");

    Input reset = gateIn("Reset").help("A rising edge starts the span over from its downbeat.");

    Outputs<4> ramp = unipolarOut("Ramp").help("Its lane's ramp, 0 to 1 over each of its beats.");
    Outputs<4> gate = gateOut("Gate").help("High for Width of each of its lane's beats.");
    Output any = gateOut("Any").help("A trigger as any lane starts a beat.");
    Output hits = unipolarOut("Hits").help(
        "How many lanes started together at the last start, a quarter each.");

    Trigger resetEdge;
    Pulse anyPulse;
    Array<int, 4> beatAt;  // each lane's beat, to see it change
    double origin = 0.0;
    double freeAt = 0.0;  // beats into the span, while the song is stopped
    double lastPos = -1.0;
    double together = 0.0;
    bool wasPlaying = false;

    Shared<float> shownPos;
    SharedArray<int, 4> shownBeat;

    void factoryPatches(Patches& p) {
        p.add("Four Against Three").set(count_1, 4).set(count_2, 3).set(count_3, 1).set(count_4, 2);
        p.add("Primes").set(count_1, 2).set(count_2, 3).set(count_3, 5).set(count_4, 7);
        p.add("Slow Phasing")
            .set(span, "bars_4")
            .set(count_1, 16)
            .set(count_2, 15)
            .set(count_3, 13)
            .set(count_4, 11);
        p.add("Clave Feel").set(count_1, 8).set(count_2, 3).set(count_3, 5).set(count_4, 6);
    }

    void prepare() {
        for (int k = 0; k < 4; ++k) beatAt[k] = -1;
    }

    int countOf(int k, int i) const {
        switch (k) {
            case 0:
                return clamp(roundToInt(count_1.at(i)), 1, 16);
            case 1:
                return clamp(roundToInt(count_2.at(i)), 1, 16);
            case 2:
                return clamp(roundToInt(count_3.at(i)), 1, 16);
            default:
                return clamp(roundToInt(count_4.at(i)), 1, 16);
        }
    }

    int countShown(int k) const {
        switch (k) {
            case 0:
                return clamp(count_1.index(), 1, 16);
            case 1:
                return clamp(count_2.index(), 1, 16);
            case 2:
                return clamp(count_3.index(), 1, 16);
            default:
                return clamp(count_4.index(), 1, 16);
        }
    }

    double spanBeats(const Transport& t) const {
        const double bar = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        switch (span.index()) {
            case 0:
                return 1.0;
            case 1:
                return bar;
            case 2:
                return 2.0 * bar;
            default:
                return 4.0 * bar;
        }
    }

    void process(int frames) {
        for (int k = 0; k < 4; ++k) {
            ramp[k].setChannels(1);
            gate[k].setChannels(1);
        }
        any.setChannels(1);
        hits.setChannels(1);
        const Transport t = transport();
        if (t.playing && !wasPlaying) origin = 0.0;
        wasPlaying = t.playing;
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const double length = max(0.25, spanBeats(t));
        for (int i = 0; i < frames; ++i) {
            const double songAt = t.beats + i * beatsPerFrame;
            const bool resetNow = resetEdge.rose(reset[0][i]);
            if (resetNow) {
                origin = songAt;
                freeAt = 0.0;
            }
            double into = 0.0;
            if (t.playing) {
                into = songAt - origin;
                into = into - floor(into / length + 1e-9) * length;
            } else {
                into = freeAt;
                freeAt += beatsPerFrame;
                if (freeAt >= length) freeAt -= length;
            }
            const double pos = clamp(into / length, 0.0, 1.0 - 1e-12);
            const bool wrapped = lastPos < 0.0 || pos < lastPos || resetNow;
            lastPos = pos;
            const double share = clamp(width.at(i), 0.01, 0.95);
            int started = 0;
            for (int k = 0; k < 4; ++k) {
                const int n = countOf(k, i);
                const double x = pos * n + 1e-9;
                const int beat = clamp(floorToInt(x), 0, n - 1);
                const double phase = clamp(x - beat, 0.0, 1.0);
                if (wrapped || beat != beatAt[k]) ++started;
                beatAt[k] = beat;
                ramp[k][0][i] = phase;
                gate[k][0][i] = phase < share ? 1.0 : 0.0;
            }
            if (started > 0) {
                anyPulse.fire();
                together = started / 4.0;
            }
            any[0][i] = anyPulse.next();
            hits[0][i] = together;
        }
        shownPos.set(static_cast<float>(max(0.0, lastPos)));
        for (int k = 0; k < 4; ++k) shownBeat[k] = beatAt[k];
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 388);
        p.display(8, 28, 204, 132);
        p.choice(span, 8, 168, 204, 18);
        p.knob(count_1, 30, 218);
        p.knob(count_2, 83, 218);
        p.knob(count_3, 137, 218);
        p.knob(count_4, 190, 218);
        for (int k = 0; k < 4; ++k) {
            p.output(ramp[k], 30 + 53.3 * k, 268);
            p.output(gate[k], 30 + 53.3 * k, 314);
        }
        p.input(reset, 30, 360);
        p.knob(width, 83, 358, 28);
        p.output(any, 137, 360);
        p.output(hits, 190, 360);
    }

    // Four rings, lane 1 outside: a dot for each beat, lit as it plays, and
    // a hand sweeping the span.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 132);
        const double cx = 110.0;
        const double cy = 94.0;
        const double pos = static_cast<double>(shownPos.get());
        for (int k = 0; k < 4; ++k) {
            const double r = 56.0 - 12.0 * k;
            const int n = countShown(k);
            const int lit = shownBeat[k];
            g.strokeStyle(Theme::line);
            g.lineWidth(1.0);
            g.beginPath();
            g.circle(cx, cy, r);
            g.stroke();
            for (int b = 0; b < n; ++b) {
                const double a = 2.0 * pi * b / n - pi / 2.0;
                g.fillStyle(b == lit ? Theme::text
                                     : (imod(k, 2) == 0 ? Theme::accent : Theme::accent2));
                g.globalAlpha(b == lit ? 1.0 : 0.8);
                g.beginPath();
                g.circle(cx + r * cos(a), cy + r * sin(a), b == lit ? 3.6 : 2.4);
                g.fill();
            }
        }
        g.globalAlpha(1.0);
        const double a = 2.0 * pi * pos - pi / 2.0;
        g.strokeStyle(Theme::text);
        g.lineWidth(1.2);
        g.beginPath();
        g.moveTo(cx, cy);
        g.lineTo(cx + 60.0 * cos(a), cy + 60.0 * sin(a));
        g.stroke();
    }
};
