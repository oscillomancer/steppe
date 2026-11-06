// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A row of up to sixteen cells, each alive or not, that grows a new row on
// every clock by a Rule: a cell's next state depends on itself and its two
// neighbours, eight cases, and the rule's eight bits say which of them
// live. Rule 90 grows nested triangles from a single cell, 30 grows chaos,
// 110 grows patterns that crawl, 184 moves like traffic. Each cell is a
// gate on Cells, one channel a cell, so a poly cable plays the row on a
// drum module or a chord; Tap 1 to 4 take four cells spread across the
// row, Value reads the row as a number (into a quantizer) and Alive sends
// how full it is. Start sows one cell in the middle or cells at random by
// Density; Wrap joins the ends of the row; Mutate flips a random cell now
// and then, so a settled pattern moves again, and Revive starts over when
// the row dies or stops changing. With nothing in Clock it steps on the
// song's sixteenths and starts over at play; Reset starts the same rows over.

constexpr int kCellHistory = 32;

struct Cellular : Module {
    static constexpr ModuleInfo info = {
        .name = "Cellular",
        .id = "com.suppleaudio.rack.cellular",
        .vendor = "Supple Audio",
        .description = "A 1-D cellular automaton: rows of cells grown by a rule, as gates and CV.",
        .tags = {"sequencer", "generative", "rhythm", "poly"}};

    Param rule = integer("Rule", 0, 255, 90)
                     .help(
                         "Which of the eight neighbourhoods give a live cell, as 8 bits: 90, 30, "
                         "110, 184 are "
                         "good places to start.");
    Param width = integer("Width", 3, 16, 8)
                      .help("How many cells the row has; a change starts the rows over.");
    Param start = choice("Start", {"Middle", "Random"}, 0)
                      .help("One cell in the middle, or cells at random by Density.");
    Param density = percent("Density", 50)
                        .activeWhen(start, {"random"})
                        .help("How many cells a random start sows.");
    Param mutate = percent("Mutate", 0).help("How likely a step is to flip one random cell.");
    Param wrap = toggle("Wrap", true).help("Join the row's ends, so the edges are neighbours.");
    Param revive =
        toggle("Revive", true).help("Start over when the row dies out or stops changing.");

    Input clock = gateIn("Clock").help(
        "Each rising edge grows the next row; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge starts the same rows over.");

    Output cells = gateOut("Cells").help("A gate a cell, one channel each, high with the clock.");
    Outputs<4> tap = gateOut("Tap").help(
        "One cell's gate, high with the clock: four cells spread evenly across the row.");
    Output value = unipolarOut("Value").help(
        "The row as a number from 0 to 1, the first cell the heaviest: into a quantizer.");
    Output alive = unipolarOut("Alive").help("The share of the row's cells alive, 0 to 1.");

    int row = 0;  // the cells, bit k cell k
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    bool clockHigh = false;
    bool wasPlaying = false;
    int salt = 0;
    int lastWidth = 0;

    SharedArray<int, kCellHistory> shownRows;  // oldest first
    Shared<int> shownWidth;

    void factoryPatches(Patches& p) {
        p.add("Triangles").set(rule, 90).set(width, 16);
        p.add("Chaos").set(rule, 30).set(width, 8);
        p.add("Crawlers").set(rule, 110).set(width, 12).set(start, "random");
        p.add("Traffic").set(rule, 184).set(width, 8).set(start, "random").set(density, 40);
        p.add("Settling Drums").set(rule, 54).set(width, 6).set(mutate, 15);
    }

    int widthNow() const { return clamp(width.index(), 3, 16); }

    void sow() {
        const int n = widthNow();
        // Sixteen draws, whatever the start, so the rows after a reseed
        // depend only on the seed and the clocks.
        int sown = 0;
        for (int k = 0; k < 16; ++k) {
            const double d = rng.next();
            if (d < clamp(density.value(), 0.0, 1.0)) sown = sown | shl(1, k);
        }
        if (start.index() == 0) sown = shl(1, idiv(n, 2));
        row = sown & (shl(1, n) - 1);
    }

    void startOver() {
        rng.seed(salt);
        sow();
        for (int k = 0; k < kCellHistory; ++k) shownRows[k] = 0;
        shownRows[kCellHistory - 1] = row;
    }

    void prepare() {
        salt = seed();
        startOver();
    }

    int next(int cur, int n, bool joined, int r) const {
        int out = 0;
        for (int k = 0; k < n; ++k) {
            int left = 0;
            int right = 0;
            if (k > 0) {
                left = bit(cur, k - 1);
            } else if (joined) {
                left = bit(cur, n - 1);
            }
            if (k < n - 1) {
                right = bit(cur, k + 1);
            } else if (joined) {
                right = bit(cur, 0);
            }
            const int hood = left * 4 + bit(cur, k) * 2 + right;
            if (bit(r, hood) == 1) out = out | shl(1, k);
        }
        return out;
    }

    void grow(int i) {
        const int n = widthNow();
        const int before = row & (shl(1, n) - 1);
        int after = next(before, n, wrap.on(), clamp(roundToInt(rule.at(i)), 0, 255));
        // Two draws a step, whatever the knobs.
        const double flip = rng.next();
        const double where = rng.next();
        if (flip < clamp(mutate.at(i), 0.0, 1.0)) {
            const int c = clamp(floorToInt(where * n), 0, n - 1);
            after = bit(after, c) == 1 ? after - shl(1, c) : after + shl(1, c);
        }
        row = after;
        if (revive.on() && (after == 0 || after == before)) sow();
        for (int k = 0; k < kCellHistory - 1; ++k) shownRows[k] = shownRows[k + 1];
        shownRows[kCellHistory - 1] = row;
    }

    double rowValue(int n) const {
        double v = 0.0;
        double weight = 0.5;
        for (int k = 0; k < n; ++k) {
            v += bit(row, k) * weight;
            weight *= 0.5;
        }
        return v / (1.0 - 2.0 * weight);
    }

    void process(int frames) {
        const int n = widthNow();
        if (n != lastWidth) {
            lastWidth = n;
            startOver();
        }
        cells.setChannels(n);
        for (int k = 0; k < 4; ++k) tap[k].setChannels(1);
        value.setChannels(1);
        alive.setChannels(1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) startOver();
        wasPlaying = t.playing;
        const double perFrame = t.tempo / 15.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            bool high = false;
            bool rose = false;
            if (internal) {
                if (t.playing) {
                    const double position = t.beats * 4.0 + i * perFrame + 1e-7;
                    high = position - floor(position) < 0.5;
                }
                rose = high && !clockHigh;
                clockHigh = high;
            } else {
                rose = clockEdge.rose(clock[0][i]);
                high = clockEdge.high();
            }
            if (rose) grow(i);
            const int r = row & (shl(1, n) - 1);
            for (int k = 0; k < n; ++k) cells[k][i] = high && bit(r, k) == 1 ? 1.0 : 0.0;
            for (int k = 0; k < 4; ++k)
                tap[k][0][i] = high && bit(r, idiv((2 * k + 1) * n, 8)) == 1 ? 1.0 : 0.0;
            value[0][i] = rowValue(n);
            alive[0][i] = popcount(r) / static_cast<double>(n);
        }
        shownWidth.set(n);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 380);
        p.display(8, 28, 204, 132);
        p.knob(rule, 30, 192);
        p.knob(width, 83, 192);
        p.knob(mutate, 137, 192);
        p.knob(density, 190, 192);
        p.choice(start, 8, 232, 96, 18);
        p.toggle(wrap, 116, 232, 44, 18);
        p.toggle(revive, 164, 232, 48, 18);
        p.input(clock, 22, 284);
        p.input(reset, 66, 284);
        p.output(cells, 110, 284);
        p.output(value, 154, 284);
        p.output(alive, 198, 284);
        for (int k = 0; k < 4; ++k) p.output(tap[k], 44 + k * 44, 338);
    }

    // The last rows, the newest at the bottom, each cell a square; the four
    // tapped cells marked under the row.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 132);
        const int n = clamp(shownWidth.get(), 3, 16);
        const double cw = min(11.0, 188.0 / n);
        const double ch = 3.4;
        const double x0 = 8.0 + (204.0 - n * cw) / 2.0;
        const double y0 = 34.0;
        for (int j = 0; j < kCellHistory; ++j) {
            const int r = shownRows[j];
            const bool newest = j == kCellHistory - 1;
            for (int k = 0; k < n; ++k) {
                if (bit(r, k) == 1) {
                    g.fillStyle(newest ? Theme::accent : Theme::accent2);
                    g.globalAlpha(newest ? 1.0 : 0.25 + 0.6 * j / kCellHistory);
                    g.fillRect(x0 + k * cw + 0.5, y0 + j * ch, cw - 1.0, ch - 0.6);
                }
            }
        }
        g.globalAlpha(1.0);
        g.fillStyle(Theme::textMuted);
        for (int k = 0; k < 4; ++k) {
            const int at = idiv((2 * k + 1) * n, 8);
            const double cx = x0 + at * cw + cw / 2.0;
            const double ty = y0 + kCellHistory * ch + 3.0;
            g.beginPath();
            g.moveTo(cx, ty);
            g.lineTo(cx - 3.0, ty + 4.0);
            g.lineTo(cx + 3.0, ty + 4.0);
            g.fill();
        }
    }
};
