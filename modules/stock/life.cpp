// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A sequencer that plays a living grid: eight by eight cells, read a column
// a clock from left to right, the column's cells as eight gates on a poly
// cable (eight drums, or the notes of a chord), and the grid lives a
// generation every Evolve clocks: with the classic rule a live cell with
// two or three live neighbours lives on, a dead one with exactly three is
// born, and the rest die. So the pattern repeats a while, shifts, grows
// and thins. Rule picks other worlds: HighLife's replicators, Seeds that
// burst and vanish, Day and Night's slow blobs, Maze's corridors. Density
// sows the first grid; Mutate drops a live cell at random as each
// generation comes, and Revive sows again when the grid dies or freezes.
// Value reads the column as a number, Count how many of its cells live,
// Alive how full the grid is; Gen fires at each generation. With nothing in
// Clock it steps on the song's sixteenths and starts over at play; Reset
// sows the same first grid again.

constexpr int kLifeSize = 8;

int lifeBirth(int k) {
    switch (k) {
        case 1:
            return 72;  // 3, 6
        case 2:
            return 4;  // 2
        case 3:
            return 456;  // 3, 6, 7, 8
        case 4:
            return 8;  // 3
        default:
            return 8;  // 3
    }
}

int lifeSurvive(int k) {
    switch (k) {
        case 1:
            return 12;  // 2, 3
        case 2:
            return 0;  // none
        case 3:
            return 472;  // 3, 4, 6, 7, 8
        case 4:
            return 62;  // 1 to 5
        default:
            return 12;  // 2, 3
    }
}

struct Life : Module {
    static constexpr ModuleInfo info = {
        .name = "Life",
        .id = "com.suppleaudio.rack.life",
        .vendor = "Supple Audio",
        .description = "An 8x8 Game of Life played a column a clock: gates that live and die.",
        .tags = {"sequencer", "generative", "rhythm", "poly"}};

    Param rule = choice("Rule", {"Life", "HighLife", "Seeds", "Day and Night", "Maze"}, 0)
                     .keys({"life", "highlife", "seeds", "day_and_night", "maze"})
                     .help(
                         "How cells are born and survive: Life B3/S23, HighLife B36/S23, Seeds "
                         "B2/S-, Day and Night B3678/S34678, Maze B3/S12345.");
    Param evolve =
        integer("Evolve", 1, 16, 8).help("Clocks between generations: 8 is once a pass.");
    Param density = percent("Density", 35).help("How full the grid is sown.");
    Param mutate =
        percent("Mutate", 10).help("How likely each generation is to get one live cell at random.");
    Param torus = toggle("Wrap", true).help("Join the grid's edges: a torus.");
    Param revive =
        toggle("Revive", true).help("Sow again when the grid dies out or stops changing.");

    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next column; with nothing here, the song's sixteenths.");
    Input reset =
        gateIn("Reset").help("A rising edge sows the same first grid and goes to column 1.");

    Output cells = gateOut("Cells").help(
        "The column's cells as eight gates, the top row first, high with the clock.");
    Output value = unipolarOut("Value").help(
        "The column as a number from 0 to 1, the top cell the heaviest: into a quantizer.");
    Output count =
        unipolarOut("Count").help("How many of the column's cells live, an eighth each.");
    Output alive = unipolarOut("Alive").help("How full the grid is, 0 to 1.");
    Output gen = gateOut("Gen").help("A trigger as each generation comes.");

    Array<int, kLifeSize> grid;  // a row a number, bit x the column x
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    Pulse genPulse;
    bool clockHigh = false;
    bool wasPlaying = false;
    int column = -1;
    int sinceGen = 0;
    int salt = 0;
    bool sown = false;

    SharedArray<int, kLifeSize> shownGrid;
    Shared<int> shownColumn;

    void factoryPatches(Patches& p) {
        p.add("Slow Life").set(evolve, 16).set(mutate, 5);
        p.add("Every Step").set(evolve, 1).set(density, 40);
        p.add("Replicators").set(rule, "highlife").set(density, 25);
        p.add("Seeds Burst").set(rule, "seeds").set(density, 10).set(evolve, 4);
        p.add("Blobs").set(rule, "day_and_night").set(density, 50);
        p.add("Corridors").set(rule, "maze").set(density, 15).set(mutate, 0);
    }

    void sow() {
        // Sixty-four draws whatever the density, so the sequence depends
        // only on the seed and the clocks.
        const double d = clamp(density.value(), 0.0, 1.0);
        for (int y = 0; y < kLifeSize; ++y) {
            int row = 0;
            for (int x = 0; x < kLifeSize; ++x) {
                const double u = rng.next();
                if (u < d) row = row | shl(1, x);
            }
            grid[y] = row;
        }
    }

    void startOver() {
        rng.seed(salt);
        sow();
        column = -1;
        sinceGen = 0;
    }

    void prepare() {
        salt = seed();
        sown = false;  // sown on the first block, with the knobs as loaded
    }

    int cellAt(int x, int y, bool joined) const {
        if (!joined && (x < 0 || x >= kLifeSize || y < 0 || y >= kLifeSize)) return 0;
        return bit(grid[wrap(y, kLifeSize)], wrap(x, kLifeSize));
    }

    void generation() {
        const bool joined = torus.on();
        const int k = rule.index();
        const int born = lifeBirth(k);
        const int stays = lifeSurvive(k);
        Array<int, kLifeSize> next;
        bool same = true;
        int total = 0;
        for (int y = 0; y < kLifeSize; ++y) {
            int row = 0;
            for (int x = 0; x < kLifeSize; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (dx != 0 || dy != 0) n += cellAt(x + dx, y + dy, joined);
                const int was = bit(grid[y], x);
                const int now = was == 1 ? bit(stays, n) : bit(born, n);
                if (now == 1) row = row | shl(1, x);
            }
            next[y] = row;
            if (row != grid[y]) same = false;
            total += popcount(row);
        }
        for (int y = 0; y < kLifeSize; ++y) grid[y] = next[y];
        // Two draws a generation, whatever the knobs.
        const double drop = rng.next();
        const double where = rng.next();
        if (drop < clamp(mutate.value(), 0.0, 1.0)) {
            const int c = clamp(floorToInt(where * 64.0), 0, 63);
            grid[idiv(c, 8)] = grid[idiv(c, 8)] | shl(1, imod(c, 8));
            same = false;
            ++total;
        }
        if (revive.on() && (total == 0 || same)) sow();
        genPulse.fire();
    }

    void step() {
        // The first clock after a reset plays the first grid as it was sown.
        if (column >= 0) ++sinceGen;
        column = column < 0 ? 0 : wrap(column + 1, kLifeSize);
        if (sinceGen >= clamp(evolve.index(), 1, 16)) {
            sinceGen = 0;
            generation();
        }
    }

    void process(int frames) {
        if (!sown) {
            sown = true;
            startOver();
        }
        cells.setChannels(kLifeSize);
        value.setChannels(1);
        count.setChannels(1);
        alive.setChannels(1);
        gen.setChannels(1);
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
            if (rose) step();
            const int x = max(0, column);
            double v = 0.0;
            double weight = 0.5;
            int lit = 0;
            for (int y = 0; y < kLifeSize; ++y) {
                const int on = bit(grid[y], x);
                cells[y][i] = high && on == 1 ? 1.0 : 0.0;
                v += on * weight;
                weight *= 0.5;
                lit += on;
            }
            int total = 0;
            for (int y = 0; y < kLifeSize; ++y) total += popcount(grid[y]);
            value[0][i] = v / (255.0 / 256.0);
            count[0][i] = lit / 8.0;
            alive[0][i] = total / 64.0;
            gen[0][i] = genPulse.next();
        }
        for (int y = 0; y < kLifeSize; ++y) shownGrid[y] = grid[y];
        shownColumn.set(column);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 404);
        p.display(8, 28, 204, 164);
        p.choice(rule, 8, 200, 204, 18);
        p.knob(evolve, 30, 250);
        p.knob(density, 83, 250);
        p.knob(mutate, 137, 250);
        p.toggle(torus, 168, 234, 44, 16);
        p.toggle(revive, 168, 256, 44, 16);
        p.input(clock, 30, 314);
        p.input(reset, 83, 314);
        p.output(cells, 137, 314);
        p.output(gen, 190, 314);
        p.output(value, 83, 368);
        p.output(count, 137, 368);
        p.output(alive, 190, 368);
    }

    // The grid, the column playing lit.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 164);
        const double cell = 18.0;
        const double x0 = 8.0 + (204.0 - kLifeSize * cell) / 2.0;
        const double y0 = 28.0 + (164.0 - kLifeSize * cell) / 2.0;
        const int now = shownColumn.get();
        if (now >= 0) {
            g.fillStyle(Theme::lineLit);
            g.globalAlpha(0.35);
            g.beginPath();
            g.roundRect(x0 + now * cell, y0 - 3.0, cell, kLifeSize * cell + 6.0, 3.0);
            g.fill();
            g.globalAlpha(1.0);
        }
        for (int y = 0; y < kLifeSize; ++y) {
            const int row = shownGrid[y];
            for (int x = 0; x < kLifeSize; ++x) {
                const bool on = bit(row, x) == 1;
                if (on) {
                    g.fillStyle(x == now ? Theme::text : Theme::accent);
                    g.globalAlpha(1.0);
                } else {
                    g.fillStyle(Theme::line);
                    g.globalAlpha(0.5);
                }
                g.beginPath();
                g.roundRect(x0 + x * cell + 2.0, y0 + y * cell + 2.0, cell - 4.0, cell - 4.0,
                            on ? 3.0 : 2.0);
                g.fill();
            }
        }
        g.globalAlpha(1.0);
    }
};
