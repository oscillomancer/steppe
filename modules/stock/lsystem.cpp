// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A melody grown by rewriting, as an L-system grows a plant: start from A,
// and at each generation rewrite every symbol by its rule (A becomes AB,
// B becomes CA, C becomes A, ...), Depth times. The string that grows is
// self-similar: its phrases echo at every scale, like a theme and its
// variations. It is played a symbol a clock, and each symbol moves the
// melody by its Move, in degrees of the scale, so the tune walks the shape
// of the string; Rest silences one symbol. A rule is a word of one to three
// symbols, picked by its knob (the face shows the words). Loop plays only
// the first so many symbols (0: all of it, up to 256); the melody starts
// on the root each time round, folded by octaves into Range around Octave.
// Symbol sends which symbol plays (A 0, B 0.5, C 1) and Start fires at the
// start. With nothing in Clock it steps on the song's eighths and starts
// over at play; Reset goes back to the first symbol.

constexpr int kLMax = 256;
constexpr int kLWords = 39;  // 3 of one symbol, 9 of two, 27 of three

int lsysMask(int k) {
    switch (k) {
        case 0:
            return 4095;  // chromatic
        case 1:
            return 2741;  // major
        case 2:
            return 1453;  // natural minor
        case 3:
            return 1709;  // dorian
        case 4:
            return 661;  // major pentatonic
        case 5:
            return 1193;  // minor pentatonic
        case 6:
            return 1365;  // whole tone
        default:
            return 1257;  // blues
    }
}

int wordLength(int w) { return w < 3 ? 1 : (w < 12 ? 2 : 3); }

// Symbol j of word w (0 A, 1 B, 2 C).
int wordSymbol(int w, int j) {
    if (w < 3) return w;
    if (w < 12) return j == 0 ? idiv(w - 3, 3) : imod(w - 3, 3);
    const int v = w - 12;
    if (j == 0) return idiv(v, 9);
    if (j == 1) return imod(idiv(v, 3), 3);
    return imod(v, 3);
}

struct LSystem : Module {
    static constexpr ModuleInfo info = {
        .name = "L-System",
        .id = "com.suppleaudio.rack.lsystem",
        .vendor = "Supple Audio",
        .description = "A self-similar melody grown by rewriting rules, a symbol a clock.",
        .tags = {"sequencer", "generative", "pitch"}};

    Param rule_a = integer("A Rule", 0, kLWords - 1, 4)
                       .in("Rules")
                       .help(
                           "The word A becomes (the face shows it): 0-2 one symbol, 3-11 two, "
                           "12-38 three.");
    Param rule_b = integer("B Rule", 0, kLWords - 1, 9)
                       .in("Rules")
                       .help("The word B becomes (the face shows it).");
    Param rule_c = integer("C Rule", 0, kLWords - 1, 0)
                       .in("Rules")
                       .help("The word C becomes (the face shows it).");
    Param depth = integer("Depth", 1, 8, 5)
                      .in("Rules")
                      .help("How many generations it grows from A (up to 256 symbols).");
    Param move_a =
        integer("A Move", -7, 7, 2).in("Moves").help("Degrees the melody moves on an A.");
    Param move_b =
        integer("B Move", -7, 7, -1).in("Moves").help("Degrees the melody moves on a B.");
    Param move_c =
        integer("C Move", -7, 7, -3).in("Moves").help("Degrees the melody moves on a C.");
    Param rest = choice("Rest", {"None", "A", "B", "C"}, 0)
                     .keys({"none", "a", "b", "c"})
                     .in("Moves")
                     .help("A symbol that plays no gate (it still moves the melody).");
    Param loop = integer("Loop", 0, 64, 0).help("Play only the first so many symbols; 0 all.");
    Param scale = choice("Scale",
                         {"Chromatic", "Major", "Minor", "Dorian", "Major Pentatonic",
                          "Minor Pentatonic", "Whole Tone", "Blues"},
                         5)
                      .in("Notes")
                      .help("The scale the moves count degrees of.");
    Param root =
        choice("Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
            .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                   "a_sharp", "b"})
            .in("Notes")
            .help("The scale's first note, where the melody starts.");
    Param octave = integer("Octave", -3, 3, 0).in("Notes").help("The root's octave: 0 is C4.");
    Param range = integer("Range", 1, 4, 2)
                      .in("Notes")
                      .help("The octaves the melody may span, folded by octaves into them.");

    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next symbol; with nothing here, the song's eighth notes.");
    Input reset = gateIn("Reset").help("A rising edge goes back to the first symbol.");

    Output pitch = pitchOut("Pitch").help("The note, in octaves, held until the next.");
    Output gate = gateOut("Gate").help("High with the clock, except on the Rest symbol.");
    Output symbol = unipolarOut("Symbol").help("The symbol playing: A 0, B 0.5, C 1.");
    Output start = gateOut("Start").help("A trigger as the string starts again.");

    Array<int, kLMax> text;  // the grown string
    Array<int, kLMax> spare;
    int length = 1;
    int grownFrom = -1;  // the rules and depth it was grown from
    int at = -1;         // the symbol playing
    int degree = 0;      // the melody, in degrees from the root
    double note = 0.0;   // in octaves
    Trigger clockEdge;
    Trigger resetEdge;
    Pulse startPulse;
    bool clockHigh = false;
    bool wasPlaying = false;

    SharedArray<int, kLMax> shownText;
    Shared<int> shownLength;
    Shared<int> shownAt;

    void factoryPatches(Patches& p) {
        p.add("Fibonacci")
            .set(rule_a, 4)
            .set(rule_b, 0)
            .set(depth, 7)
            .set(move_a, 1)
            .set(move_b, -2);
        p.add("Thue-Morse")
            .set(rule_a, 4)
            .set(rule_b, 6)
            .set(depth, 6)
            .set(move_a, 2)
            .set(move_b, -2)
            .set(scale, "major");
        p.add("Cantor Rests")
            .set(rule_a, 15)
            .set(rule_b, 25)
            .set(depth, 4)
            .set(rest, "b")
            .set(move_a, 3)
            .set(move_b, -1);
        p.add("Sixteen Step Theme").set(loop, 16).set(depth, 6);
    }

    void prepare() {
        grownFrom = -1;
        at = -1;
        degree = 0;
    }

    int ruleOf(int s) const {
        if (s == 0) return clamp(rule_a.index(), 0, kLWords - 1);
        if (s == 1) return clamp(rule_b.index(), 0, kLWords - 1);
        return clamp(rule_c.index(), 0, kLWords - 1);
    }

    int moveShown(int s) const {
        if (s == 0) return clamp(move_a.index(), -7, 7);
        if (s == 1) return clamp(move_b.index(), -7, 7);
        return clamp(move_c.index(), -7, 7);
    }

    int moveOf(int s, int i) const {
        if (s == 0) return clamp(roundToInt(move_a.at(i)), -7, 7);
        if (s == 1) return clamp(roundToInt(move_b.at(i)), -7, 7);
        return clamp(roundToInt(move_c.at(i)), -7, 7);
    }

    // Grows the string again if a rule or the depth has changed.
    void grow() {
        const int gens = clamp(depth.index(), 1, 8);
        const int signature = ((ruleOf(0) * kLWords + ruleOf(1)) * kLWords + ruleOf(2)) * 16 + gens;
        if (signature == grownFrom) return;
        grownFrom = signature;
        text[0] = 0;
        length = 1;
        for (int g = 0; g < gens; ++g) {
            int n = 0;
            for (int k = 0; k < length; ++k) {
                const int w = ruleOf(text[k]);
                for (int j = 0; j < wordLength(w); ++j) {
                    if (n < kLMax) {
                        spare[n] = wordSymbol(w, j);
                        ++n;
                    }
                }
            }
            for (int k = 0; k < n; ++k) text[k] = spare[k];
            length = n;
        }
        if (at >= length) at = -1;
    }

    // The note `deg` degrees from the root, folded into Range, in octaves.
    double noteOf(int deg) const {
        const int r = clamp(root.index(), 0, 11);
        const int m = lsysMask(scale.index());
        const int per = popcount(m);
        const int span = per * clamp(range.index(), 1, 4);
        const int low = -idiv(span, 2);
        int d = deg;
        while (d < low) d += per;
        while (d >= low + span) d -= per;
        const int oct = d >= 0 ? idiv(d, per) : -idiv(per - 1 - d, per);
        const int idx = d - oct * per;
        int pc = 0;
        int seen = 0;
        for (int k = 0; k < 12; ++k) {
            if (bit(m, k) == 1) {
                if (seen == idx) pc = k;
                ++seen;
            }
        }
        return (r + pc + 12.0 * (oct + octave.index())) / 12.0;
    }

    void step(int i) {
        grow();
        const int window = loop.index() > 0 ? min(length, loop.index()) : length;
        if (at >= 0) degree += moveOf(text[at], i);
        at = at + 1;
        if (at >= window) at = 0;
        if (at == 0) {
            degree = 0;
            startPulse.fire();
        }
        note = noteOf(degree);
    }

    void process(int frames) {
        pitch.setChannels(1);
        gate.setChannels(1);
        symbol.setChannels(1);
        start.setChannels(1);
        grow();
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) at = -1;
        wasPlaying = t.playing;
        const double perFrame = t.tempo / 30.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) at = -1;
            bool high = false;
            bool rose = false;
            if (internal) {
                if (t.playing) {
                    const double e = t.beats * 2.0 + i * perFrame + 1e-7;
                    high = e - floor(e) < 0.5;
                }
                rose = high && !clockHigh;
                clockHigh = high;
            } else {
                rose = clockEdge.rose(clock[0][i]);
                high = clockEdge.high();
            }
            if (rose) step(i);
            const int s = at >= 0 ? text[at] : 0;
            const bool silent = at < 0 || rest.index() == s + 1;
            pitch[0][i] = at >= 0 ? note : noteOf(0);
            gate[0][i] = high && !silent ? 1.0 : 0.0;
            symbol[0][i] = s * 0.5;
            start[0][i] = startPulse.next();
        }
        for (int k = 0; k < length; ++k) shownText[k] = text[k];
        shownLength.set(loop.index() > 0 ? min(length, loop.index()) : length);
        shownAt.set(at);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 404);
        p.display(8, 28, 204, 104);
        p.knob(rule_a, 30, 160);
        p.knob(rule_b, 83, 160);
        p.knob(rule_c, 137, 160);
        p.knob(depth, 190, 160);
        p.knob(move_a, 30, 216);
        p.knob(move_b, 83, 216);
        p.knob(move_c, 137, 216);
        p.knob(loop, 190, 216);
        p.choice(scale, 8, 254, 120, 18);
        p.choice(root, 132, 254, 80, 18);
        p.choice(rest, 8, 278, 120, 18);
        p.knob(octave, 30, 324);
        p.knob(range, 83, 324);
        p.input(clock, 137, 324);
        p.input(reset, 190, 324);
        p.output(pitch, 30, 376);
        p.output(gate, 83, 376);
        p.output(symbol, 137, 376);
        p.output(start, 190, 376);
    }

    void drawLetter(Canvas& g, int s, double x, double y) const {
        if (s == 0) {
            g.fillText("A", x, y);
        } else if (s == 1) {
            g.fillText("B", x, y);
        } else {
            g.fillText("C", x, y);
        }
    }

    // The three rules as words, then the string: a bar a symbol, the one
    // playing lit.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 104);
        g.monoFont(10.0);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        for (int s = 0; s < 3; ++s) {
            const double x = 16.0 + 66.0 * s;
            const double y = 40.0;
            g.fillStyle(Theme::text);
            drawLetter(g, s, x, y);
            g.fillStyle(Theme::textMuted);
            g.fillText(">", x + 9.0, y);
            g.fillStyle(Theme::accent);
            const int w = ruleOf(s);
            for (int j = 0; j < wordLength(w); ++j)
                drawLetter(g, wordSymbol(w, j), x + 20.0 + 8.0 * j, y);
        }
        // The melody as heard, folded into Range: a step a symbol, coloured
        // by symbol.
        const int n = clamp(shownLength.get(), 1, kLMax);
        const int now = shownAt.get();
        const int per = popcount(lsysMask(scale.index()));
        const int span = per * clamp(range.index(), 1, 4);
        const int low = -idiv(span, 2);
        const double cw = 188.0 / n;
        const double top = 58.0;
        const double tall = 64.0;
        const double dy = tall / max(1, span - 1);
        int deg = 0;
        for (int k = 0; k < n; ++k) {
            const int sym = shownText[k];
            int f = deg;
            while (f < low) f += per;
            while (f >= low + span) f -= per;
            const double x = 16.0 + k * cw;
            const double y = top + (low + span - 1 - f) * dy;
            g.fillStyle(k == now ? Theme::text
                                 : (sym == 0 ? Theme::accent
                                             : (sym == 1 ? Theme::accent2 : Theme::textMuted)));
            g.globalAlpha(k == now ? 1.0 : 0.85);
            g.fillRect(x, y - 1.5, max(1.0, cw - (cw > 3.0 ? 1.0 : 0.0)), 3.0);
            deg += moveShown(sym);
        }
        g.globalAlpha(1.0);
    }
};
