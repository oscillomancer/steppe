// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A chord progression that writes itself: a chord a bar (or a clock), each
// built from the key's own thirds, so its quality is the mode's (in C major,
// D is minor and B diminished). The next chord is drawn by how chords like
// to move: down a fifth most of all, then up a fifth or down a third, less
// often by step; Home pulls back to the key's first chord, and the last
// chord of the loop leans to one that leads home. Loop is how many chords
// the progression has before it comes round, starting on the home chord;
// Change is how likely a chord is drawn anew as it comes round, so the
// progression keeps its shape and slowly changes. Notes stacks three to
// five (triads, sevenths, ninths); Lead voices each chord as close as it
// can to the last. Chord sends its notes on a poly cable, with a Gate that
// opens again at each chord; Root is its root an octave below, Degree
// which of the seven it is, Next a trigger as it changes. With nothing in
// Clock it follows the song, a chord every Length; Reset starts the same
// progression over.

constexpr int kChangesSlots = 16;

// The seven notes of each mode, in semitones above the key, packed a hex
// digit a degree from the lowest.
int changesMode(int m) {
    switch (m) {
        case 1:
            return 0xA875320;  // minor
        case 2:
            return 0xA975320;  // dorian
        case 3:
            return 0xA875310;  // phrygian
        case 4:
            return 0xB976420;  // lydian
        case 5:
            return 0xA975420;  // mixolydian
        case 6:
            return 0xB875320;  // harmonic minor
        default:
            return 0xB975420;  // major
    }
}

struct Changes : Module {
    static constexpr ModuleInfo info = {
        .name = "Changes",
        .id = "com.suppleaudio.rack.changes",
        .vendor = "Supple Audio",
        .description = "A chord progression that writes itself in a key, voiced smoothly.",
        .tags = {"chord", "pitch", "generative", "sequencer"}};

    Param key = choice("Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
                    .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                           "a_sharp", "b"})
                    .help("The key's first note: the home chord's root.");
    Param mode =
        choice("Mode",
               {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Harmonic Minor"},
               0)
            .keys(
                {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "harmonic_minor"})
            .help("The scale the chords are built from, and so each chord's quality.");
    Param length = choice("Length", {"2 Beats", "1 Bar", "2 Bars", "4 Bars"}, 1)
                       .keys({"beats_2", "bar_1", "bars_2", "bars_4"})
                       .help("How long each chord lasts while following the song.");
    Param loop = integer("Loop", 1, kChangesSlots, 4)
                     .help("How many chords before the progression comes round.");
    Param change = percent("Change", 25)
                       .help(
                           "How likely each chord (but the first) is drawn anew as it comes round: "
                           "0 % locks it.");
    Param home =
        percent("Home", 40).help("How strongly the progression is drawn back to the home chord.");
    Param notes = integer("Notes", 3, 5, 3)
                      .help("How many notes a chord stacks: 3 triads, 4 sevenths, 5 ninths.");
    Param octave = integer("Octave", -2, 2, 0).help("Where the chords sit: 0 around C4.");
    Param lead = toggle("Lead", true)
                     .help(
                         "Voice each chord as close as it can to the last; off, every chord in "
                         "root position.");

    Input clock = gateIn("Clock").help(
        "Each rising edge moves to the next chord; with nothing here, the song.");
    Input reset =
        gateIn("Reset").help("A rising edge starts the same progression over from the home chord.");

    Output chord = pitchOut("Chord").help("The chord's notes, lowest first, a channel each.");
    Output gate =
        gateOut("Gate").help("High on every note of the chord, low for 2 ms as the next begins.");
    Output root = pitchOut("Root").help("The chord's root, an octave below: for a bass.");
    Output degree =
        unipolarOut("Degree").help("Which chord of the key it is: 0 the first, 1 the seventh.");
    Output next = gateOut("Next").help("A trigger as each chord begins.");

    Array<int, kChangesSlots> slots;  // each chord's degree, 0..6
    Array<int, 5> voiced;             // the chord, in semitones from C4
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    Pulse nextPulse;
    int at = -1;  // the slot playing
    int salt = 0;
    int gap = 0;  // frames left of the gate's gap
    int count = 0;
    int lastSpan = -1;
    bool wasPlaying = false;
    bool started = false;

    SharedArray<int, kChangesSlots> shownSlots;
    Shared<int> shownAt;
    Shared<int> shownLoop;
    Shared<int> shownMode;

    void factoryPatches(Patches& p) {
        p.add("Four Chords");
        p.add("Minor Loop").set(mode, "minor").set(loop, 4).set(change, 10);
        p.add("Jazz Sevenths").set(notes, 4).set(home, 20).set(change, 35);
        p.add("Modal Drift").set(mode, "dorian").set(loop, 8).set(home, 10).set(change, 50);
        p.add("Slow Pads").set(length, "bars_2").set(notes, 5).set(loop, 6);
    }

    void prepare() {
        salt = seed();
        started = false;  // sown on the first block
    }

    // Degree d's note in semitones above the key, any octave.
    int noteOf(int d) const {
        const int packed = changesMode(mode.index());
        const int oct = d >= 0 ? idiv(d, 7) : -idiv(6 - d, 7);
        const int k = d - 7 * oct;
        return imod(shr(packed, 4 * k), 16) + 12 * oct;
    }

    // 0 major, 1 minor, 2 diminished, 3 augmented.
    int quality(int d) const {
        const int third = noteOf(d + 2) - noteOf(d);
        const int fifth = noteOf(d + 4) - noteOf(d);
        if (third == 4) return fifth == 8 ? 3 : 0;
        return fifth == 6 ? 2 : 1;
    }

    // How much a chord on degree a likes to move to degree b.
    double motion(int a, int b) const {
        double w = 0.0;
        switch (wrap(b - a, 7)) {
            case 3:
                w = 5.0;  // down a fifth
                break;
            case 4:
            case 5:
                w = 3.0;  // up a fifth, down a third
                break;
            case 1:
                w = 2.0;
                break;
            case 2:
            case 6:
                w = 1.5;
                break;
            default:
                w = 0.0;  // not to itself
        }
        const int q = quality(b);
        if (q == 2) w *= 0.25;
        if (q == 3) w *= 0.3;
        if (b == 0) w *= 1.0 + 4.0 * clamp(home.value(), 0.0, 1.0);
        return w;
    }

    // The same, with the loop's last chord leaning to one that leads home.
    double pull(int a, int b, bool cadence) const {
        const double w = motion(a, b);
        return cadence && b != 0 ? w * (1.0 + motion(b, 0) / 2.0) : w;
    }

    int walk(int from, double u, bool cadence) const {
        double total = 0.0;
        for (int b = 0; b < 7; ++b) total += pull(from, b, cadence);
        double left = u * total;
        for (int b = 0; b < 7; ++b) {
            left -= pull(from, b, cadence);
            if (left < 0.0) return b;
        }
        return imod(from + 3, 7);
    }

    void startOver() {
        rng.seed(salt);
        const int n = clamp(loop.index(), 1, kChangesSlots);
        slots[0] = 0;
        // Sixteen draws whatever Loop, so the progression depends only on
        // the seed.
        for (int k = 1; k < kChangesSlots; ++k)
            slots[k] = walk(slots[k - 1], rng.next(), k == n - 1);
        at = -1;
        count = 0;
        for (int k = 0; k < 5; ++k) voiced[k] = -1000;
    }

    void voice() {
        const int d = slots[at];
        const int n = clamp(notes.index(), 3, 5);
        const int keyRoot = clamp(key.index(), 0, 11);
        const int centre = 12 * clamp(octave.index(), -2, 2);
        Array<int, 5> tones;  // the chord's notes, stacked from its root
        for (int t = 0; t < n; ++t) tones[t] = keyRoot + noteOf(d + 2 * t);
        // Root position, its root in the octave from the centre.
        const int rootPc = imod(keyRoot + noteOf(d), 12);
        Array<int, 5> best;
        for (int t = 0; t < n; ++t) best[t] = centre + rootPc + (tones[t] - tones[0]);
        if (lead.on() && voiced[0] > -1000) {
            int bestCost = 1000000;
            for (int inv = 0; inv < n; ++inv) {
                Array<int, 5> cand;
                for (int t = 0; t < n; ++t) {
                    const int src = inv + t < n ? inv + t : inv + t - n;
                    const int up = inv + t < n ? 0 : 12;
                    cand[t] = tones[src] - tones[inv] + up;
                }
                // The lowest note as near the last chord's lowest as it can
                // be, kept within a fifth or so of the centre.
                const int pc = imod(keyRoot + noteOf(d + 2 * inv), 12);
                int low = centre + pc;
                while (low - voiced[0] > 6) low -= 12;
                while (voiced[0] - low > 6) low += 12;
                while (low < centre - 7) low += 12;
                while (low > centre + 7) low -= 12;
                for (int t = 0; t < n; ++t) cand[t] += low;
                // Lowest first (a ninth's inversions can cross).
                for (int a = 1; a < n; ++a)
                    for (int b = a; b > 0 && cand[b] < cand[b - 1]; --b) {
                        const int swap = cand[b];
                        cand[b] = cand[b - 1];
                        cand[b - 1] = swap;
                    }
                int cost = 0;
                for (int t = 0; t < n; ++t) {
                    const int moved = cand[t] - voiced[t];
                    cost += moved < 0 ? -moved : moved;
                }
                if (cost < bestCost) {
                    bestCost = cost;
                    for (int t = 0; t < n; ++t) best[t] = cand[t];
                }
            }
        }
        for (int t = 0; t < 5; ++t) voiced[t] = t < n ? best[t] : best[n - 1];
    }

    void step(int i) {
        const int n = clamp(loop.index(), 1, kChangesSlots);
        at = at + 1 >= n ? 0 : at + 1;
        // Two draws a chord, whatever the knobs.
        const double u = rng.next();
        const double v = rng.next();
        if (at > 0 && count >= n && u < clamp(change.at(i), 0.0, 1.0))
            slots[at] = walk(slots[at - 1], v, at == n - 1);
        ++count;
        voice();
        nextPulse.fire();
        gap = max(1, roundToInt(0.002 * sampleRate()));
    }

    double spanBeats(const Transport& t) const {
        const double bar = t.timeSigNumerator * 4.0 / max(1, t.timeSigDenominator);
        switch (length.index()) {
            case 0:
                return 2.0;
            case 2:
                return 2.0 * bar;
            case 3:
                return 4.0 * bar;
            default:
                return bar;
        }
    }

    void process(int frames) {
        if (!started) {
            started = true;
            startOver();
        }
        const int n = clamp(notes.index(), 3, 5);
        chord.setChannels(n);
        gate.setChannels(n);
        root.setChannels(1);
        degree.setChannels(1);
        next.setChannels(1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) {
            startOver();
            lastSpan = -1;
        }
        wasPlaying = t.playing;
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const double span = max(0.25, spanBeats(t));
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) {
                startOver();
                lastSpan = -1;
            }
            bool rose = false;
            if (internal) {
                if (t.playing) {
                    const int now = floorToInt((t.beats + i * beatsPerFrame) / span + 1e-9);
                    rose = now != lastSpan;
                    lastSpan = now;
                }
            } else {
                rose = clockEdge.rose(clock[0][i]);
            }
            if (rose) step(i);
            const bool sounding = at >= 0;
            const bool open = sounding && gap == 0;
            if (gap > 0) --gap;
            for (int c = 0; c < n; ++c) {
                chord[c][i] = sounding ? voiced[c] / 12.0 : 0.0;
                gate[c][i] = open ? 1.0 : 0.0;
            }
            const int d = sounding ? slots[at] : 0;
            root[0][i] =
                (imod(clamp(key.index(), 0, 11) + noteOf(d), 12) + 12.0 * (octave.index() - 1)) /
                12.0;
            degree[0][i] = d / 6.0;
            next[0][i] = nextPulse.next();
        }
        for (int k = 0; k < kChangesSlots; ++k) shownSlots[k] = slots[k];
        shownAt.set(at);
        shownLoop.set(clamp(loop.index(), 1, kChangesSlots));
        shownMode.set(mode.index());
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 440);
        p.display(8, 28, 204, 96);
        p.choice(key, 8, 132, 64, 18);
        p.choice(mode, 78, 132, 134, 18);
        p.choice(length, 8, 156, 204, 18);
        p.knob(loop, 30, 210);
        p.knob(change, 83, 210);
        p.knob(home, 137, 210);
        p.knob(notes, 190, 210);
        p.knob(octave, 30, 266, 28);
        p.toggle(lead, 70, 258, 44, 16);
        p.input(clock, 137, 268);
        p.input(reset, 190, 268);
        p.output(chord, 30, 330);
        p.output(gate, 83, 330);
        p.output(root, 137, 330);
        p.output(next, 190, 330);
        p.output(degree, 30, 384);
    }

    // The numeral of degree d with quality q: upper case major, lower minor.
    void numeral(Canvas& g, int d, int q, double x, double y) {
        switch (d * 4 + q) {
            case 0:
                g.fillText("I", x, y);
                break;
            case 1:
                g.fillText("i", x, y);
                break;
            case 2:
                g.fillText("io", x, y);
                break;
            case 3:
                g.fillText("I+", x, y);
                break;
            case 4:
                g.fillText("II", x, y);
                break;
            case 5:
                g.fillText("ii", x, y);
                break;
            case 6:
                g.fillText("iio", x, y);
                break;
            case 7:
                g.fillText("II+", x, y);
                break;
            case 8:
                g.fillText("III", x, y);
                break;
            case 9:
                g.fillText("iii", x, y);
                break;
            case 10:
                g.fillText("iiio", x, y);
                break;
            case 11:
                g.fillText("III+", x, y);
                break;
            case 12:
                g.fillText("IV", x, y);
                break;
            case 13:
                g.fillText("iv", x, y);
                break;
            case 14:
                g.fillText("ivo", x, y);
                break;
            case 15:
                g.fillText("IV+", x, y);
                break;
            case 16:
                g.fillText("V", x, y);
                break;
            case 17:
                g.fillText("v", x, y);
                break;
            case 18:
                g.fillText("vo", x, y);
                break;
            case 19:
                g.fillText("V+", x, y);
                break;
            case 20:
                g.fillText("VI", x, y);
                break;
            case 21:
                g.fillText("vi", x, y);
                break;
            case 22:
                g.fillText("vio", x, y);
                break;
            case 23:
                g.fillText("VI+", x, y);
                break;
            case 24:
                g.fillText("VII", x, y);
                break;
            case 25:
                g.fillText("vii", x, y);
                break;
            case 26:
                g.fillText("viio", x, y);
                break;
            default:
                g.fillText("VII+", x, y);
                break;
        }
    }

    // The loop's chords as numerals, six to a row, the one playing lit.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 96);
        const int n = shownLoop.get();
        const int now = shownAt.get();
        const double w = 196.0 / 6.0;
        for (int k = 0; k < n; ++k) {
            const double x = 12.0 + w * imod(k, 6);
            const double y = 31.0 + 31.0 * idiv(k, 6);
            if (k == now) {
                g.fillStyle(Theme::lineLit);
                g.globalAlpha(0.35);
                g.beginPath();
                g.roundRect(x, y, w - 3.0, 28.0, 3.0);
                g.fill();
                g.globalAlpha(1.0);
            }
            const int d = shownSlots[k];
            g.fillStyle(k == now ? Theme::text : (d == 0 ? Theme::accent : Theme::textMuted));
            numeral(g, d, quality(d), x + 3.0, y + 20.0);
        }
    }
};
