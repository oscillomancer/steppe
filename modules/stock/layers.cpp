// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// An arrangement for four parts: each part's gates pass while Level is at
// or above the part's Enter, so a rising level brings parts in one by one
// and a falling one takes them out. Patch Form's Energy to Level and the
// drums, bass, chords and lead to the four Gates, and the song builds
// through its intro and opens up in its choruses. Parts come and go only
// on a beat, a bar or every 2 or 4 bars (At), Level read as each begins,
// and a note that began keeps playing to its end. Rest lets a part above
// the first sit out a span it would play, drawn from the seed for that
// span, so a long section breathes. The spans are the song's (or edges in
// Bar), so the arrangement is the same wherever playback starts.

constexpr int kLayers = 4;

struct Layers : Module {
    static constexpr ModuleInfo info = {
        .name = "Layers",
        .id = "com.suppleaudio.rack.layers",
        .vendor = "Supple Audio",
        .description = "An arrangement: parts enter and leave with a level, on the bar.",
        .tags = {"sequencer", "generative", "logic", "poly"}};

    Param enter_1 = percent("Enter 1", 0).help("The level from which part 1 plays.");
    Param enter_2 = percent("Enter 2", 25).help("The level from which part 2 plays.");
    Param enter_3 = percent("Enter 3", 50).help("The level from which part 3 plays.");
    Param enter_4 = percent("Enter 4", 75).help("The level from which part 4 plays.");
    Param at = choice("At", {"Beat", "Bar", "2 Bars", "4 Bars"}, 1)
                   .keys({"beat", "bar", "bars_2", "bars_4"})
                   .help("When parts come and go: each beat, bar, or every 2 or 4 bars.");
    Param rest = percent("Rest", 0).help(
        "How likely parts 2 to 4 are to sit out a span they would play, from the seed.");

    Input level = unipolarIn("Level").normal(1.0).help(
        "Which parts play, read as each span begins; with nothing here, 1: all of them.");
    Input bar = gateIn("Bar").help(
        "Each rising edge is a bar (and Beat a bar); with nothing here, the song's.");
    Input gate_1 = gateIn("Gate 1").help("Part 1's gates; poly.");
    Input gate_2 = gateIn("Gate 2").help("Part 2's gates; poly.");
    Input gate_3 = gateIn("Gate 3").help("Part 3's gates; poly.");
    Input gate_4 = gateIn("Gate 4").help("Part 4's gates; poly.");

    Output out_1 = gateOut("Out 1").help("Part 1's gates while it plays.");
    Output out_2 = gateOut("Out 2").help("Part 2's gates while it plays.");
    Output out_3 = gateOut("Out 3").help("Part 3's gates while it plays.");
    Output out_4 = gateOut("Out 4").help("Part 4's gates while it plays.");
    Output on_1 = gateOut("On 1").help("High while part 1 plays: for a VCA or a switch.");
    Output on_2 = gateOut("On 2").help("High while part 2 plays.");
    Output on_3 = gateOut("On 3").help("High while part 3 plays.");
    Output on_4 = gateOut("On 4").help("High while part 4 plays.");
    Output count = unipolarOut("Count").help("How many parts play, a quarter each.");

    Array<bool, kLayers> playing;
    Array<bool, 64> held;  // part k, voice c at k * 16 + c: its gate is high
    Array<bool, 64> pass;  // and rose while the part played
    Random rng;
    Trigger barEdge;
    int salt = 0;
    int span = -1000000;  // the span the parts were chosen for
    int bars = -1;        // edges counted in Bar
    double lastLevel = 0.0;

    SharedArray<int, kLayers> shownPlaying;  // 1 while the part plays
    Shared<float> shownAt;
    Shared<float> shownLevel;

    void factoryPatches(Patches& p) {
        p.add("Build Up");
        p.add("Breathing").set(rest, 25);
        p.add("Slow Changes").set(at, "bars_4");
        p.add("Two and Two").set(enter_2, 0).set(enter_3, 60).set(enter_4, 60);
    }

    void prepare() { salt = seed(); }

    double enterOf(int k, int i) {
        switch (k) {
            case 0:
                return enter_1.at(i);
            case 1:
                return enter_2.at(i);
            case 2:
                return enter_3.at(i);
            default:
                return enter_4.at(i);
        }
    }

    double enterValue(int k) const {
        switch (k) {
            case 0:
                return enter_1.value();
            case 1:
                return enter_2.value();
            case 2:
                return enter_3.value();
            default:
                return enter_4.value();
        }
    }

    // Which parts play in span s, from Level now.
    void choose(int s, int i) {
        const double lv = level[0][i];
        lastLevel = lv;
        // Three draws a span, from the seed and the span alone.
        rng.seed(salt + s * 7919);
        for (int k = 0; k < kLayers; ++k) {
            const double u = k == 0 ? 1.0 : rng.next();
            playing[k] = lv >= enterOf(k, i) - 1e-9 && (k == 0 || u >= rest.at(i));
        }
    }

    // One part: its gates through while it plays, a note keeping its start's.
    void part(int k, Input& in, Output& out, Output& on, int i) {
        const int n = max(1, in.channels());
        for (int c = 0; c < n; ++c) {
            const int at16 = k * 16 + c;
            const double g = in[c][i];
            if (!held[at16] && g >= 0.5) {
                held[at16] = true;
                pass[at16] = playing[k];
            } else if (held[at16] && g < 0.25) {
                held[at16] = false;
                pass[at16] = false;
            }
            out[c][i] = held[at16] && pass[at16] ? 1.0 : 0.0;
        }
        on[0][i] = playing[k] ? 1.0 : 0.0;
    }

    void process(int frames) {
        out_1.setChannels(max(1, gate_1.channels()));
        out_2.setChannels(max(1, gate_2.channels()));
        out_3.setChannels(max(1, gate_3.channels()));
        out_4.setChannels(max(1, gate_4.channels()));
        on_1.setChannels(1);
        on_2.setChannels(1);
        on_3.setChannels(1);
        on_4.setChannels(1);
        count.setChannels(1);
        const Transport t = transport();
        const bool internal = !bar.connected();
        const double barBeats = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        const double beatBeats = 4.0 / max(1, t.timeSigDenominator);
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const int a = at.index();
        const double spanBars = a == 3 ? 4.0 : (a == 2 ? 2.0 : 1.0);
        double place = 0.0;
        for (int i = 0; i < frames; ++i) {
            int s = 0;
            if (internal) {
                const double beats = t.beats + i * beatsPerFrame;
                const double len = a == 0 ? beatBeats : barBeats * spanBars;
                s = floorToInt(beats / max(0.0625, len) + 1e-9);
                place = beats / max(0.25, barBeats * 4.0);
            } else {
                if (barEdge.rose(bar[0][i])) ++bars;
                s = bars < 0 ? 0 : floorToInt(bars / spanBars + 1e-9);
                place = max(0, bars) / 4.0;
            }
            if (s != span) {
                span = s;
                choose(s, i);
            }
            part(0, gate_1, out_1, on_1, i);
            part(1, gate_2, out_2, on_2, i);
            part(2, gate_3, out_3, on_3, i);
            part(3, gate_4, out_4, on_4, i);
            int playingCount = 0;
            for (int k = 0; k < kLayers; ++k) playingCount += playing[k] ? 1 : 0;
            count[0][i] = playingCount * 0.25;
        }
        for (int k = 0; k < kLayers; ++k) shownPlaying[k] = playing[k] ? 1 : 0;
        shownAt.set(static_cast<float>(place - floor(place)));
        shownLevel.set(static_cast<float>(lastLevel));
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 420);
        p.display(8, 28, 204, 84);
        p.knob(enter_1, 32, 146);
        p.knob(enter_2, 84, 146);
        p.knob(enter_3, 136, 146);
        p.knob(enter_4, 188, 146);
        p.choice(at, 8, 186, 120, 18);
        p.knob(rest, 172, 200);
        p.input(gate_1, 32, 252);
        p.input(gate_2, 84, 252);
        p.input(gate_3, 136, 252);
        p.input(gate_4, 188, 252);
        p.output(out_1, 32, 300);
        p.output(out_2, 84, 300);
        p.output(out_3, 136, 300);
        p.output(out_4, 188, 300);
        p.output(on_1, 32, 348);
        p.output(on_2, 84, 348);
        p.output(on_3, 136, 348);
        p.output(on_4, 188, 348);
        p.input(level, 32, 394);
        p.input(bar, 84, 394);
        p.output(count, 188, 394);
    }

    // Four lanes, one a part, lit while it plays; on the left, each part's
    // Enter on a level scale, the level last read beside them.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 84);
        const double top = 34.0;
        const double laneH = 17.0;
        const double scaleX = 16.0;
        const double scaleTop = 34.0;
        const double scaleH = 72.0;
        // The level scale, its Enters as ticks, the level as a bar.
        g.fillStyle(Theme::line);
        g.beginPath();
        g.roundRect(scaleX, scaleTop, 6.0, scaleH, 2.0);
        g.fill();
        const double lv = clamp(static_cast<double>(shownLevel.get()), 0.0, 1.0);
        g.fillStyle(Theme::accent);
        g.beginPath();
        g.roundRect(scaleX, scaleTop + scaleH * (1.0 - lv), 6.0, scaleH * lv, 2.0);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(1.0);
        g.beginPath();
        for (int k = 0; k < kLayers; ++k) {
            const double e = clamp(enterValue(k), 0.0, 1.0);
            const double y = scaleTop + scaleH * (1.0 - e);
            g.moveTo(scaleX - 3.0, y);
            g.lineTo(scaleX + 9.0, y);
        }
        g.stroke();
        // The lanes: part 4 on top.
        g.font(9.0);
        g.textAlign(TextAlign::Left);
        for (int k = 0; k < kLayers; ++k) {
            const double y = top + laneH * (kLayers - 1 - k);
            const bool on = shownPlaying[k] == 1;
            g.fillStyle(on ? Theme::accent : Theme::line);
            g.globalAlpha(on ? 0.9 : 0.4);
            g.beginPath();
            g.roundRect(48.0, y + 2.0, 156.0, laneH - 4.0, 2.0);
            g.fill();
            g.globalAlpha(1.0);
            g.fillStyle(on ? Theme::text : Theme::line);
            g.fillText(format("%d", k + 1), 34.0, y + laneH - 5.0);
        }
        // Where in four bars the song is.
        const double px = 48.0 + 156.0 * clamp(static_cast<double>(shownAt.get()), 0.0, 1.0);
        g.strokeStyle(Theme::text);
        g.beginPath();
        g.moveTo(px, top);
        g.lineTo(px, top + laneH * kLayers);
        g.stroke();
    }
};
