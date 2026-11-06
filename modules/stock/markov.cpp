// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A melody that walks from note to note by the odds of a table: for each
// of the twelve notes, how likely each note is to come next. Style fills
// the table: Steps moves mostly to the scale's neighbours, Leaps jumps
// thirds and fifths, Tonal steps but leans home to the root, fifth and
// third, and Even makes every note of the scale as likely. Play a melody
// into In, a gate into Learn, and it counts which note follows which:
// Learned blends that table over the style's, so it improvises in the
// manner of what it heard (Memory is how many transitions a note keeps;
// Clear forgets them; what it learned is not saved with the patch).
// Surprise sharpens the odds (0 %: the likeliest next note nearly always)
// or flattens them (100 %: almost any). Each note goes to the octave
// nearest the last one, within Range around Octave. Rest leaves steps
// silent, and Odds sends how likely the note played was, to accent the
// surprises. With nothing in Clock it steps on the song's eighths and
// starts over at play; Reset starts the same melody over.

int markovMask(int k) {
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

// How strongly a style leans to a note `dd` scale steps away.
double stepWeight(int dd) {
    switch (dd) {
        case 0:
            return 0.25;
        case 1:
            return 1.0;
        case 2:
            return 0.4;
        case 3:
            return 0.15;
        default:
            return 0.05;
    }
}

double leapWeight(int dd) {
    switch (dd) {
        case 0:
            return 0.05;
        case 1:
            return 0.25;
        case 2:
            return 1.0;
        case 3:
            return 0.9;
        default:
            return 0.6;
    }
}

struct Markov : Module {
    static constexpr ModuleInfo info = {
        .name = "Markov",
        .id = "com.suppleaudio.rack.markov",
        .vendor = "Supple Audio",
        .description = "A melody by the odds of which note follows which: styled or learned.",
        .tags = {"sequencer", "random", "generative", "pitch"}};

    Param style = choice("Style", {"Steps", "Leaps", "Tonal", "Even"}, 2)
                      .in("Notes")
                      .help(
                          "The table's own odds: to the scale's neighbours, by thirds and fifths, "
                          "stepping but leaning home, or every note alike.");
    Param scale = choice("Scale",
                         {"Chromatic", "Major", "Minor", "Dorian", "Major Pentatonic",
                          "Minor Pentatonic", "Whole Tone", "Blues"},
                         1)
                      .in("Notes")
                      .help("The notes it plays; learned notes outside it are left out.");
    Param root =
        choice("Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
            .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                   "a_sharp", "b"})
            .in("Notes")
            .help("The scale's first note, and home for the Tonal style.");
    Param octave =
        integer("Octave", -3, 3, 0).in("Notes").help("The middle of the range: 0 is C4.");
    Param range = octaves("Range", 1, 4, 2).in("Notes").help("How many octaves the melody spans.");
    Param surprise = percent("Surprise", 50)
                         .in("Notes")
                         .help(
                             "0 %: nearly always the likeliest next note; 50 %: the table's odds; "
                             "100 %: almost any note.");
    Param rest = percent("Rest", 10).in("Rhythm").help("How likely a step is to stay silent.");
    Param length = percent("Length", 50, 5, 100)
                       .in("Rhythm")
                       .help("The gate's share of a step; 100 % ties the notes together.");
    Param learned = percent("Learned", 100)
                        .in("Learning")
                        .help(
                            "How much the learned table counts over the style's, where it has "
                            "heard the note.");
    Param memory =
        integer("Memory", 2, 64, 16)
            .in("Learning")
            .help("About how many transitions each note remembers before old ones fade.");

    Input clock = gateIn("Clock").help(
        "Each rising edge plays the next note; with nothing here, the song's eighth notes.");
    Input reset = gateIn("Reset").help("A rising edge starts the same melody over.");
    Input in = pitchIn("In").help("The melody to learn from, as pitch.");
    Input learn = gateIn("Learn").help("Each rising edge learns the note at In.");
    Input clear = gateIn("Clear").help("A rising edge forgets everything learned.");

    Output pitch = pitchOut("Pitch").help("The note, held until the next one, in octaves.");
    Output gate = gateOut("Gate").help("High for Length of each step that is not a rest.");
    Output odds =
        unipolarOut("Odds").help("How likely the note played was, 0 to 1: low for the surprises.");

    Array<double, 144> taught;  // learned counts, row: from, column: to
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    Trigger learnEdge;
    Trigger clearEdge;
    bool clockHigh = false;
    bool wasPlaying = false;
    bool sounding = false;  // this step has a gate
    bool lowFrame = false;
    int state = 0;        // the pitch class last played
    int lastTaught = -1;  // the pitch class last learned
    double note = 0.0;    // in semitones from C4
    double chance = 1.0;
    int since = 0;
    int period = 12000;    // frames a step: the song's eighth until two clocks have come
    bool stepped = false;  // a clock has come since the reset
    int salt = 0;

    SharedArray<float, 144> shownOdds;
    Shared<int> shownTo;
    Shared<int> shownMask;

    void factoryPatches(Patches& p) {
        p.add("Wandering").set(style, "steps").set(scale, "major");
        p.add("Pentatonic Leaps").set(style, "leaps").set(scale, "minor_pentatonic");
        p.add("Bass Walk")
            .set(style, "tonal")
            .set(scale, "minor")
            .set(octave, -2)
            .set(range, 1)
            .set(rest, 0)
            .set(length, 80);
        p.add("Twelve Tones").set(style, "even").set(scale, "chromatic").set(surprise, 80);
        p.add("Predictable").set(style, "tonal").set(surprise, 10).set(rest, 25);
    }

    void startOver() {
        rng.seed(salt);
        state = clamp(root.index(), 0, 11);
        note = state + 12.0 * octave.index();
        sounding = false;
        stepped = false;
    }

    void forget() {
        for (int k = 0; k < 144; ++k) taught[k] = 0.0;
        lastTaught = -1;
    }

    void prepare() {
        salt = seed();
        forget();
        startOver();
    }

    // The scale's notes (as pitch classes) are the 12-bit mask turned by root.
    int maskNow() const {
        const int m = markovMask(scale.index());
        const int r = clamp(root.index(), 0, 11);
        return (shl(m, r) | shr(m, 12 - r)) & 4095;
    }

    // Scale steps between two pitch classes, the short way round.
    int stepsBetween(int mask, int from, int to) const {
        const int n = popcount(mask);
        int a = 0;
        int b = 0;
        for (int k = 0; k < 12; ++k) {
            if (bit(mask, k) == 1 && k <= from) ++a;
            if (bit(mask, k) == 1 && k <= to) ++b;
        }
        const int d = abs(a - b);
        return min(d, n - d);
    }

    double styleWeight(int mask, int from, int to) const {
        const int dd = stepsBetween(mask, from, to);
        const int k = style.index();
        if (k == 0) return stepWeight(dd);
        if (k == 1) return leapWeight(dd);
        if (k == 3) return 1.0;
        const int above = wrap(to - root.index(), 12);
        double home = 0.1;
        if (above == 0) home = 0.8;
        if (above == 7) home = 0.5;
        if (above == 3 || above == 4) home = 0.35;
        return 0.6 * stepWeight(dd) + home;
    }

    // Row `from` of the table as played: the style's odds and the learned
    // ones blended, sharpened or flattened by Surprise, left unnormalised.
    void row(int from, int i, Array<double, 12>& w) const {
        const int mask = maskNow();
        double styleSum = 0.0;
        double taughtSum = 0.0;
        for (int to = 0; to < 12; ++to) {
            if (bit(mask, to) == 1) {
                styleSum += styleWeight(mask, from, to);
                taughtSum += taught[from * 12 + to];
            }
        }
        const double b = taughtSum > 0.0 ? clamp(learned.at(i), 0.0, 1.0) : 0.0;
        const double e = pow(4.0, 1.0 - 2.0 * clamp(surprise.at(i), 0.0, 1.0));
        for (int to = 0; to < 12; ++to) {
            w[to] = 0.0;
            if (bit(mask, to) == 1) {
                const double s = styleWeight(mask, from, to) / max(1e-9, styleSum);
                const double t = taughtSum > 0.0 ? taught[from * 12 + to] / taughtSum : 0.0;
                w[to] = pow((1.0 - b) * s + b * t, e);
            }
        }
    }

    void step(int i, bool measure) {
        if (measure && stepped) period = max(1, since);
        stepped = true;
        since = 0;
        // Two draws a step, whatever the knobs.
        const double silent = rng.next();
        const double pick = rng.next();
        Array<double, 12> w;
        row(state, i, w);
        double total = 0.0;
        for (int to = 0; to < 12; ++to) total += w[to];
        int next = state;
        if (total > 0.0) {
            double acc = 0.0;
            next = -1;
            for (int to = 0; to < 12; ++to) {
                acc += w[to];
                if (next < 0 && w[to] > 0.0 && pick * total < acc) next = to;
            }
            if (next < 0) next = state;
            chance = w[next] / total;
        }
        lowFrame = sounding;
        sounding = silent >= clamp(rest.at(i), 0.0, 1.0);
        if (!sounding) {
            lowFrame = false;
            return;
        }
        // The octave nearest the last note, folded into Range around Octave.
        double n = note + wrap(next - state, 12);
        if (n - note > 6.0) n -= 12.0;
        const double span = 12.0 * clamp(range.at(i), 1.0, 4.0);
        const double low = 12.0 * octave.index() + 6.0 - 0.5 * span;
        while (n < low) n += 12.0;
        while (n >= low + span) n -= 12.0;
        if (n < low) n += 12.0;  // a span under an octave keeps the note above low
        state = next;
        note = n;
        // Legato: a tie holds the gate through.
        if (clamp(length.at(i), 0.05, 1.0) >= 1.0) lowFrame = false;
    }

    void teach(double pitchIn) {
        const int pc = wrap(roundToInt(pitchIn * 12.0), 12);
        if (lastTaught >= 0) {
            const double keep = 1.0 - 1.0 / clamp(memory.value(), 2.0, 64.0);
            for (int to = 0; to < 12; ++to) taught[lastTaught * 12 + to] *= keep;
            taught[lastTaught * 12 + pc] += 1.0;
        }
        lastTaught = pc;
    }

    void process(int frames) {
        pitch.setChannels(1);
        gate.setChannels(1);
        odds.setChannels(1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) startOver();
        wasPlaying = t.playing;
        const double perFrame = t.tempo / 30.0 / sampleRate();
        const int eighth = max(1, roundToInt(sampleRate() * 30.0 / max(1.0, t.tempo)));
        if (internal || !stepped) period = eighth;
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            if (clearEdge.rose(clear[0][i])) forget();
            if (learnEdge.rose(learn[0][i])) teach(in[0][i]);
            bool rose = false;
            if (internal) {
                bool high = false;
                if (t.playing) {
                    const double e = t.beats * 2.0 + i * perFrame + 1e-7;
                    high = e - floor(e) < 0.5;
                }
                rose = high && !clockHigh;
                clockHigh = high;
            } else {
                rose = clockEdge.rose(clock[0][i]);
            }
            ++since;
            if (rose) step(i, !internal);
            const double share = clamp(length.at(i), 0.05, 1.0);
            bool on = sounding && (share >= 1.0 || since < share * period);
            if (lowFrame) {
                on = false;
                lowFrame = false;
            }
            pitch[0][i] = note / 12.0;
            gate[0][i] = on ? 1.0 : 0.0;
            odds[0][i] = chance;
        }
        // The table as played, each row as odds, for the face.
        Array<double, 12> w;
        for (int from = 0; from < 12; ++from) {
            row(from, frames - 1, w);
            double total = 0.0;
            for (int to = 0; to < 12; ++to) total += w[to];
            for (int to = 0; to < 12; ++to)
                shownOdds[from * 12 + to] = total > 0.0 ? static_cast<float>(w[to] / total) : 0.0f;
        }
        shownTo.set(state);
        shownMask.set(maskNow());
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 404);
        p.display(8, 28, 204, 136);
        p.choice(style, 8, 172, 60, 18);
        p.choice(scale, 72, 172, 92, 18);
        p.choice(root, 168, 172, 44, 18);
        p.knob(octave, 30, 222);
        p.knob(range, 83, 222);
        p.knob(surprise, 137, 222);
        p.knob(rest, 190, 222);
        p.knob(length, 30, 280);
        p.knob(learned, 83, 280);
        p.knob(memory, 137, 280);
        p.input(clear, 190, 280);
        p.input(clock, 30, 330);
        p.input(reset, 83, 330);
        p.input(in, 137, 330);
        p.input(learn, 190, 330);
        p.output(pitch, 83, 376);
        p.output(gate, 137, 376);
        p.output(odds, 190, 376);
    }

    void drawName(Canvas& g, int pc, double x, double y) const {
        switch (pc) {
            case 0:
                g.fillText("C", x, y);
                break;
            case 1:
                g.fillText("C#", x, y);
                break;
            case 2:
                g.fillText("D", x, y);
                break;
            case 3:
                g.fillText("D#", x, y);
                break;
            case 4:
                g.fillText("E", x, y);
                break;
            case 5:
                g.fillText("F", x, y);
                break;
            case 6:
                g.fillText("F#", x, y);
                break;
            case 7:
                g.fillText("G", x, y);
                break;
            case 8:
                g.fillText("G#", x, y);
                break;
            case 9:
                g.fillText("A", x, y);
                break;
            case 10:
                g.fillText("A#", x, y);
                break;
            default:
                g.fillText("B", x, y);
                break;
        }
    }

    // The table: a row for each note, a dot as big as the odds of each note
    // after it. The row of the note playing is lit.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 136);
        const int mask = shownMask.get();
        const int now = clamp(shownTo.get(), 0, 11);
        const double cell = 10.0;  // a row's height
        const double colW = 13.0;
        const double x0 = 8.0 + (204.0 - 12.0 * colW) / 2.0 + 8.0;
        const double y0 = 28.0 + 8.0;
        g.monoFont(7.0);
        g.textBaseline(TextBaseline::Middle);
        g.textAlign(TextAlign::Right);
        g.fillStyle(Theme::recess);
        g.globalAlpha(0.6);
        g.fillRect(x0, y0 + now * cell, 12.0 * colW, cell);
        g.globalAlpha(1.0);
        for (int from = 0; from < 12; ++from) {
            const bool inFrom = bit(mask, from) == 1;
            g.fillStyle(from == now ? Theme::text : (inFrom ? Theme::textMuted : Theme::textFaint));
            drawName(g, from, x0 - 4.0, y0 + from * cell + cell / 2.0);
            for (int to = 0; to < 12; ++to) {
                const double cx = x0 + to * colW + colW / 2.0;
                const double cy = y0 + from * cell + cell / 2.0;
                const double v = static_cast<double>(shownOdds[from * 12 + to]);
                if (bit(mask, to) == 0) {
                    g.fillStyle(Theme::line);
                    g.fillRect(cx - 0.5, cy - 0.5, 1.0, 1.0);
                } else if (v > 0.0) {
                    g.fillStyle(from == now ? Theme::accent : Theme::accent2);
                    g.globalAlpha(from == now ? 1.0 : (inFrom ? 0.8 : 0.35));
                    g.beginPath();
                    g.circle(cx, cy, 0.6 + 4.2 * sqrt(v));
                    g.fill();
                    g.globalAlpha(1.0);
                }
            }
        }
    }
};
