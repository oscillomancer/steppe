// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A drum pattern generator to explore: a map of nine grooves, each a weight
// from 0 to 9 for every sixteenth of a bar on four voices (kick, snare,
// hats, percussion). Down the map (Y) they go from sparse to busy, across
// it (X) from straight (four on the floor) through a backbeat to broken,
// syncopated rhythms; between the grooves their weights blend, so X and Y
// (and cables into them) morph one groove into the next. Each voice's
// Density sets which weights play: at 50 % the strong ones, at 100 % every
// ghost note, at 0 % none. Chaos nudges every weight at random as each step
// comes, adding and dropping hits (the same nudges after every reset, so a
// patch plays the same each time). Accent is high on the strongest hits.
// Each clock plays the next sixteenth; with nothing in Clock it follows the
// song's, locked to the bar.

// The groove map's weights, 0..9 a step: eight steps an int, the first the
// highest digit. Index (node * 4 + voice) * 2 + half.
int grooveHalf(int index) {
    switch (index) {
        case 0:
            return 90009000;
        case 1:
            return 90009000;
        case 2:
            return 0;
        case 3:
            return 0;
        case 4:
            return 600060;
        case 5:
            return 600060;
        case 6:
            return 0;
        case 7:
            return 0;
        case 8:
            return 90000000;
        case 9:
            return 90000000;
        case 10:
            return 9000;
        case 11:
            return 9000;
        case 12:
            return 60006000;
        case 13:
            return 60006000;
        case 14:
            return 0;
        case 15:
            return 0;
        case 16:
            return 90000009;
        case 17:
            return 900000;
        case 18:
            return 9000;
        case 19:
            return 900;
        case 20:
            return 0;
        case 21:
            return 0;
        case 22:
            return 70070070;
        case 23:
            return 7000;
        case 24:
            return 90009000;
        case 25:
            return 90009000;
        case 26:
            return 8000;
        case 27:
            return 8000;
        case 28:
            return 800080;
        case 29:
            return 800080;
        case 30:
            return 30;
        case 31:
            return 30;
        case 32:
            return 90000030;
        case 33:
            return 90300000;
        case 34:
            return 9002;
        case 35:
            return 9003;
        case 36:
            return 70707070;
        case 37:
            return 70707070;
        case 38:
            return 0;
        case 39:
            return 300;
        case 40:
            return 90000009;
        case 41:
            return 900030;
        case 42:
            return 9000;
        case 43:
            return 3009000;
        case 44:
            return 70507050;
        case 45:
            return 70507050;
        case 46:
            return 700700;
        case 47:
            return 700700;
        case 48:
            return 90009000;
        case 49:
            return 90009004;
        case 50:
            return 8004;
        case 51:
            return 8040;
        case 52:
            return 68966896;
        case 53:
            return 68966896;
        case 54:
            return 300303;
        case 55:
            return 300303;
        case 56:
            return 90300030;
        case 57:
            return 90300030;
        case 58:
            return 3009003;
        case 59:
            return 309003;
        case 60:
            return 97979797;
        case 61:
            return 97979797;
        case 62:
            return 30003;
        case 63:
            return 30003;
        case 64:
            return 90030090;
        case 65:
            return 9300300;
        case 66:
            return 909003;
        case 67:
            return 9009039;
        case 68:
            return 96969696;
        case 69:
            return 96969696;
        case 70:
            return 70370370;
        case 71:
            return 7037030;
        default:
            return 0;
    }
}

// Step `step` (0..15) of `voice` (0..3) in groove `node` (0..8): 0..9.
int grooveWeight(int node, int voice, int step) {
    int v = grooveHalf((node * 4 + voice) * 2 + step / 8);
    for (int k = 0; k < 7 - imod(step, 8); ++k) v = v / 10;
    return v % 10;
}

// A nudge in -1..1 for step count n of a voice: the same wherever it plays.
double nudgeAt(int n, int voice, int salt) {
    int h = n * 668265261 + voice * 1013904223 + salt * 374761393;
    h = h ^ ((h >> 15) & 131071);
    h = h * 739982445;
    h = h ^ ((h >> 12) & 1048575);
    h = h * 695872825;
    h = h ^ ((h >> 15) & 131071);
    return static_cast<double>((h >> 8) & 65535) / 32767.5 - 1.0;
}

struct Rhythm : Module {
    static constexpr ModuleInfo info = {
        .name = "Rhythm",
        .id = "com.suppleaudio.rack.rhythm",
        .vendor = "Supple Audio",
        .description =
            "Drum patterns from a map of grooves: X and Y morph them, density and chaos.",
        .tags = {"drums", "sequencer", "generative", "rhythm"}};

    Param x = percent("X", 0).in("Map").help(
        "Across the map: straight at 0 %, a backbeat at 50 %, broken rhythms at 100 %.");
    Param y = percent("Y", 50).in("Map").help("Down the map: sparse at 0 %, busy at 100 %.");
    Param chaos =
        percent("Chaos", 10)
            .in("Map")
            .help("How far each step's weights are nudged at random: hits added and dropped.");
    Param kickDensity =
        percent("Kick Density", 50)
            .in("Density")
            .help("Which kick weights play: none at 0 %, the strong at 50 %, all at 100 %.");
    Param snareDensity =
        percent("Snare Density", 50).in("Density").help("Which snare weights play.");
    Param hatDensity = percent("Hat Density", 50).in("Density").help("Which hat weights play.");
    Param percDensity =
        percent("Perc Density", 50).in("Density").help("Which percussion weights play.");

    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next sixteenth; with nothing here, the song's.");
    Input reset = gateIn("Reset").help("A rising edge makes the next clock play the bar's first.");

    Output kick = gateOut("Kick").help("High with the clock on kick hits.");
    Output snare = gateOut("Snare").help("High with the clock on snare hits.");
    Output hat = gateOut("Hat").help("High with the clock on hat hits.");
    Output perc = gateOut("Perc").help("High with the clock on percussion hits.");
    Output accent = gateOut("Accent").help(
        "High with the clock on steps with a strongest-weight hit (9 after the nudge).");

    Array<bool, 4> hits;
    bool accented = false;
    bool clockHigh = false;
    bool resetHigh = false;
    bool armed = false;
    int count = -1;
    int base = 0;
    int salt = 0;

    Shared<int> shownStep;
    // The editor's own: whether a drag moves the map's point.
    bool dragging = false;

    void factoryPatches(Patches& p) {
        p.add("House").set(x, 0).set(y, 50);
        p.add("Backbeat").set(x, 50).set(y, 50);
        p.add("Broken").set(x, 100).set(y, 50);
        p.add("Sparse Latin").set(x, 100).set(y, 0).set(percDensity, 60);
        p.add("Busy and Loose").set(x, 60).set(y, 100).set(chaos, 35);
    }

    void prepare() { salt = seed(); }

    // A voice's blended weight at a step, at map position (gx, gy) 0..2.
    double blended(int voice, int step, double gx, double gy) const {
        const int c0 = min(1, floorToInt(gx));
        const int r0 = min(1, floorToInt(gy));
        const double fx = clamp(gx - c0, 0.0, 1.0);
        const double fy = clamp(gy - r0, 0.0, 1.0);
        const double a = grooveWeight(r0 * 3 + c0, voice, step);
        const double b = grooveWeight(r0 * 3 + c0 + 1, voice, step);
        const double c = grooveWeight((r0 + 1) * 3 + c0, voice, step);
        const double d = grooveWeight((r0 + 1) * 3 + c0 + 1, voice, step);
        const double top = a + (b - a) * fx;
        const double bottom = c + (d - c) * fx;
        return top + (bottom - top) * fy;
    }

    double densityOf(int voice, int i) const {
        if (voice == 0) return kickDensity.at(i);
        if (voice == 1) return snareDensity.at(i);
        if (voice == 2) return hatDensity.at(i);
        return percDensity.at(i);
    }

    void advance(int sixteenth, bool internal, int i) {
        if (armed) {
            count = 0;
            base = sixteenth;
            armed = false;
        } else {
            count = internal ? sixteenth - base : count + 1;
        }
        const int step = wrap(count, 16);
        const double gx = 2.0 * clamp(x.at(i), 0.0, 1.0);
        const double gy = 2.0 * clamp(y.at(i), 0.0, 1.0);
        const double amount = clamp(chaos.at(i), 0.0, 1.0);
        accented = false;
        for (int v = 0; v < 4; ++v) {
            const double w = blended(v, step, gx, gy) + 9.0 * amount * nudgeAt(count, v, salt);
            const double density = clamp(densityOf(v, i), 0.0, 1.0);
            hits[v] = density > 0.0 && w > 9.0 * (1.0 - density) + 1e-9;
            if (hits[v] && w >= 8.5) accented = true;
        }
    }

    void process(int frames) {
        kick.setChannels(1);
        snare.setChannels(1);
        hat.setChannels(1);
        perc.setChannels(1);
        accent.setChannels(1);
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
                    // Sixteenths from the bar's start: the groove's first is its downbeat.
                    const double position = (t.beats - t.barStart) * 4.0 + i * perFrame + 1e-7;
                    sixteenth = floorToInt(position);
                    high = position - sixteenth < 0.5;
                }
            } else {
                const double c = clock[0][i];
                high = clockHigh ? c >= 0.25 : c >= 0.5;
            }
            if (high && !clockHigh) advance(sixteenth, internal, i);
            clockHigh = high;
            kick[0][i] = high && hits[0] ? 1.0 : 0.0;
            snare[0][i] = high && hits[1] ? 1.0 : 0.0;
            hat[0][i] = high && hits[2] ? 1.0 : 0.0;
            perc[0][i] = high && hits[3] ? 1.0 : 0.0;
            accent[0][i] = high && accented ? 1.0 : 0.0;
        }
        shownStep.set(count < 0 ? -1 : wrap(count, 16));
    }

    // --- the face ------------------------------------------------------------------------

    static constexpr double kMapX = 14.0;
    static constexpr double kMapY = 34.0;
    static constexpr double kMapS = 84.0;
    static constexpr double kGridX = 106.0;
    static constexpr double kGridW = 100.0;

    void panel(Panel& p) {
        p.size(220, 344);
        p.display(8, 28, 204, 96);
        p.knob(x, 37, 156);
        p.knob(y, 110, 156);
        p.knob(chaos, 183, 156);
        p.knob(kickDensity, 30, 214);
        p.knob(snareDensity, 83, 214);
        p.knob(hatDensity, 137, 214);
        p.knob(percDensity, 190, 214);
        p.input(clock, 22, 272);
        p.input(reset, 66, 272);
        p.output(kick, 22, 316);
        p.output(snare, 66, 316);
        p.output(hat, 110, 316);
        p.output(perc, 154, 316);
        p.output(accent, 198, 316);
    }

    void draw(Canvas& g) {
        g.well(8, 28, 204, 96);
        // The map: the nine grooves as dots, the point between them.
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.strokeRect(kMapX, kMapY, kMapS, kMapS);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                g.fillStyle(Theme::lineLit);
                g.beginPath();
                g.circle(kMapX + c * kMapS * 0.5, kMapY + r * kMapS * 0.5, 2.0);
                g.fill();
            }
        const double px = kMapX + kMapS * clamp(x.value(), 0.0, 1.0);
        const double py = kMapY + kMapS * clamp(y.value(), 0.0, 1.0);
        g.fillStyle(Theme::accent);
        g.beginPath();
        g.circle(px, py, 4.5);
        g.fill();
        g.strokeStyle(Theme::text);
        g.beginPath();
        g.circle(px, py, 7.0);
        g.stroke();

        // The blended pattern: a row a voice, brighter for heavier weights,
        // solid where the density plays them.
        const double gx = 2.0 * clamp(x.value(), 0.0, 1.0);
        const double gy = 2.0 * clamp(y.value(), 0.0, 1.0);
        const double col = kGridW / 16.0;
        const int playing = shownStep.get();
        for (int v = 0; v < 4; ++v) {
            const double rowY = kMapY + 4.0 + v * 20.0;
            const double density = clamp(densityOf(v, 0), 0.0, 1.0);
            for (int s = 0; s < 16; ++s) {
                const double w = blended(v, s, gx, gy);
                const bool plays = density > 0.0 && w > 9.0 * (1.0 - density) + 1e-9;
                const double cx = kGridX + s * col;
                if (s == playing) {
                    g.fillStyle(Theme::raised);
                    g.fillRect(cx, rowY - 2, col, 16);
                }
                g.fillStyle(plays ? Theme::accent : Theme::textFaint);
                g.globalAlpha(plays ? 0.35 + 0.65 * w / 9.0 : 0.12 + 0.3 * w / 9.0);
                const double hgt = 2.0 + 10.0 * w / 9.0;
                g.fillRect(cx + 1, rowY + 12 - hgt, col - 2, hgt);
                g.globalAlpha(1.0);
            }
        }
    }

    void pointer(const Pointer& p) {
        if (p.action() == PointerAction::Press)
            dragging = p.over(kMapX - 6, kMapY - 6, kMapS + 12, kMapS + 12);
        if ((p.action() == PointerAction::Press || p.action() == PointerAction::Drag) && dragging) {
            x.edit(clamp((p.x() - kMapX) / kMapS, 0.0, 1.0));
            y.edit(clamp((p.y() - kMapY) / kMapS, 0.0, 1.0));
        }
        if (p.action() == PointerAction::Release) dragging = false;
    }
};
