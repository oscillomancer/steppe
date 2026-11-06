// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Random with a memory: on each clock a new random note, or the one this
// step held last time round. Loop sets how many steps it remembers and
// Change how likely each step is to be drawn anew as it comes round: 0 % a
// locked phrase, a little a phrase that mutates slowly, 100 % always new.
// What it remembers are the raw draws, so Spread (narrow to wide), Bias
// (low to high), Range and the scale reshape the remembered phrase at once.
// Steps pulls each note to the scale, from smooth (0 %) to quantized
// (100 %). Three gates fire at random on the clock, one at a time, by Gate
// Chance and Gate Bias (towards A or C); they are remembered too, so the
// rhythm loops with the notes. At Voices above 1 each channel has its own
// memory: chords of random notes. With nothing in Clock it steps on the
// song's eighths and starts over at play; Reset starts the same sequence
// again.

// Whether `degree` (0..11) is in a scale given as a 12-bit mask.
bool inMask(int mask, int degree) {
    int m = mask;
    for (int k = 0; k < degree; ++k) m = m / 2;
    return m % 2 == 1;
}

int scaleMask(int k) {
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
        case 7:
            return 129;  // root and fifth
        default:
            return 1;  // octaves
    }
}

// The nearest note of the scale to `semis` (from C4), as semitones.
double nearestNote(double semis, int mask, int root) {
    double best = semis;
    double distance = 1e9;
    const int centre = roundToInt(semis);
    for (int d = -7; d <= 7; ++d) {
        const int n = centre + d;
        if (inMask(mask, wrap(n - root, 12)) && abs(n - semis) < distance) {
            distance = abs(n - semis);
            best = n;
        }
    }
    return best;
}

// x folded back into 0..1 at its edges.
double fold(double x) {
    double y = fmod(abs(x), 2.0);
    return y > 1.0 ? 2.0 - y : y;
}

struct RandomModule : Module {
    static constexpr ModuleInfo info = {
        .name = "Random",
        .id = "com.suppleaudio.rack.random",
        .vendor = "Supple Audio",
        .description = "Clocked random notes and gates with a memory that loops and mutates.",
        .tags = {"random", "generative", "sequencer", "poly"}};

    Param loop =
        integer("Loop", 1, 16, 8).in("Memory").help("How many steps it remembers and repeats.");
    Param change =
        percent("Change", 20)
            .in("Memory")
            .help(
                "How likely a step is to be drawn anew as it comes round: 0 % locks the phrase, "
                "100 % never repeats.");
    Param spread =
        percent("Spread", 70)
            .in("Notes")
            .help("How widely the values scatter around the bias: low keeps them close.");
    Param bias = percent("Bias", 50)
                     .in("Notes")
                     .help("Where the values centre, from the bottom of the range to the top.");
    Param range = octaves("Range", 0, 5, 2)
                      .in("Notes")
                      .help("How many octaves the values span, centred on C4 at a bias of 50 %.");
    Param steps =
        percent("Steps", 100)
            .in("Notes")
            .help(
                "How strongly each value is pulled to the scale: 0 % smooth, 100 % on its notes.");
    Param scale = choice("Scale",
                         {"Chromatic", "Major", "Minor", "Dorian", "Major Pentatonic",
                          "Minor Pentatonic", "Whole Tone", "Fifths", "Octaves"},
                         5)
                      .in("Notes")
                      .help("The notes Steps pulls the values to.");
    Param root =
        choice("Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
            .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                   "a_sharp", "b"})
            .in("Notes")
            .help("The scale's first note.");
    Param gateChance = percent("Gate Chance", 50)
                           .in("Gates")
                           .help("How likely a step is to fire one of the gates.");
    Param gateBias =
        percent("Gate Bias", 0, -100, 100)
            .in("Gates")
            .help("Which gate fires more: towards A below 0, towards C above, evenly at 0.");
    Param voices = integer("Voices", 1, 16, 1)
                       .in("Poly")
                       .help("How many channels it sends, each with a memory of its own.");

    Input clock = gateIn("Clock").help(
        "Each rising edge takes the next step; with nothing here, the song's eighth notes.");
    Input reset = gateIn("Reset").help(
        "A rising edge starts the same random sequence over from its first step.");

    Output stepped = pitchOut("Stepped").help(
        "The value, held from clock to clock, in octaves (pitch when Steps is up).");
    Output smooth = cvOut("Smooth").help("The same values, gliding from each to the next.");
    Output gateA = gateOut("Gate A").help("High with the clock on the steps that fire A.");
    Output gateB = gateOut("Gate B").help("High with the clock on the steps that fire B.");
    Output gateC = gateOut("Gate C").help("High with the clock on the steps that fire C.");

    struct Slot {
        double t = 0.0;      // the raw draw, -1..1 (triangular)
        double fire = 0.0;   // 0..1: whether a gate fires
        double which = 0.0;  // 0..1: which one
        bool filled = false;
    };
    struct Voice {
        Array<Slot, 16> memory;
        Random rng;
        int pos = -1;
        double value = 0.0;  // the stepped output
        double from = 0.0;   // the smooth output glides from here
        int gate = 0;        // 0 none, 1..3 A..C
    };
    Array<Voice, 16> voice;
    bool clockHigh = false;
    bool resetHigh = false;
    bool wasPlaying = false;
    int sinceClock = 0;
    int period = 12000;
    double glide = 1.0;  // the smooth outputs' progress, 0..1
    int salt = 0;

    SharedArray<float, 16> shownValue;
    SharedArray<int, 16> shownGate;
    Shared<int> shownPos;
    Shared<int> shownLoop;

    void factoryPatches(Patches& p) {
        p.add("Slow Mutation").set(loop, 8).set(change, 10);
        p.add("Locked Phrase").set(loop, 8).set(change, 0);
        p.add("Pure Chance").set(change, 100);
        p.add("Chords").set(voices, 3).set(scale, "major").set(loop, 4).set(change, 15);
        p.add("Bass Walk")
            .set(range, 1)
            .set(bias, 35)
            .set(spread, 50)
            .set(scale, "minor")
            .set(loop, 16)
            .set(change, 5);
        p.add("Drifting CV").set(steps, 0).set(range, 2).set(change, 40).set(loop, 5);
    }

    void startOver() {
        for (int c = 0; c < 16; ++c) {
            Voice& v = voice[c];
            v.rng.seed(salt + c * 7919);
            v.pos = -1;
            for (int k = 0; k < 16; ++k) v.memory[k].filled = false;
        }
    }

    void prepare() {
        salt = seed();
        startOver();
    }

    // The value of a remembered draw, in octaves, with the knobs as they are.
    double valueOf(double t) {
        const double x = fold(bias + 0.5 * spread * t);
        const double raw = (x - 0.5) * range * 12.0;
        const double q = nearestNote(raw, scaleMask(scale.index()), root.index());
        return (raw + (q - raw) * clamp(steps.value(), 0.0, 1.0)) / 12.0;
    }

    int gateOf(const Slot& s) {
        if (s.fire >= gateChance) return 0;
        const double b = clamp(gateBias.value(), -1.0, 1.0);
        const double a = 1.0 - b;  // A's weight; B's is 1, C's 1 + b
        const double pick = s.which * 3.0;
        return pick < a ? 1 : (pick < a + 1.0 ? 2 : 3);
    }

    void tick(int n) {
        const int len = clamp(loop.index(), 1, 16);
        period = max(1, sinceClock);
        sinceClock = 0;
        glide = 0.0;
        for (int c = 0; c < n; ++c) {
            Voice& v = voice[c];
            v.pos = v.pos < 0 ? 0 : wrap(v.pos + 1, len);
            // Every step draws the same numbers, whatever the knobs, so a
            // sequence depends only on its seed and its clocks.
            const double anew = v.rng.next();
            const double t = v.rng.next() + v.rng.next() - 1.0;
            const double fire = v.rng.next();
            const double which = v.rng.next();
            Slot& s = v.memory[v.pos];
            if (!s.filled || anew < change) {
                s.t = t;
                s.fire = fire;
                s.which = which;
                s.filled = true;
            }
            v.from = v.value;
            v.value = valueOf(s.t);
            v.gate = gateOf(s);
        }
    }

    void process(int frames) {
        const int n = max(channels(), voices.index());
        stepped.setChannels(n);
        smooth.setChannels(n);
        gateA.setChannels(n);
        gateB.setChannels(n);
        gateC.setChannels(n);
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) startOver();
        wasPlaying = t.playing;
        const double eighthsPerFrame = t.tempo / 30.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            const double r = reset[0][i];
            if (!resetHigh && r >= 0.5) {
                resetHigh = true;
                startOver();
            } else if (resetHigh && r < 0.25) {
                resetHigh = false;
            }
            bool high = false;
            if (internal) {
                if (t.playing) {
                    const double e = t.beats * 2.0 + i * eighthsPerFrame;
                    high = e - floor(e) < 0.5;
                }
            } else {
                const double c = clock[0][i];
                high = clockHigh ? c >= 0.25 : c >= 0.5;
            }
            if (high && !clockHigh) tick(n);
            clockHigh = high;
            ++sinceClock;
            glide = min(1.0, glide + 1.0 / period);
            const double g = 0.5 - 0.5 * cos(pi * glide);
            for (int c = 0; c < n; ++c) {
                const Voice& v = voice[c];
                stepped[c][i] = v.value;
                smooth[c][i] = v.from + (v.value - v.from) * g;
                gateA[c][i] = high && v.gate == 1 ? 1.0 : 0.0;
                gateB[c][i] = high && v.gate == 2 ? 1.0 : 0.0;
                gateC[c][i] = high && v.gate == 3 ? 1.0 : 0.0;
            }
        }
        // The first channel's memory, for the face.
        const int len = clamp(loop.index(), 1, 16);
        const Voice& first = voice[0];
        for (int k = 0; k < 16; ++k) {
            const Slot& s = first.memory[k];
            shownValue[k] = s.filled ? static_cast<float>(fold(bias + 0.5 * spread * s.t)) : -1.0f;
            shownGate[k] = s.filled ? gateOf(s) : 0;
        }
        shownPos.set(first.pos);
        shownLoop.set(len);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 324);
        p.display(8, 28, 204, 60);
        p.knob(loop, 22, 116);
        p.knob(change, 66, 116);
        p.knob(spread, 110, 116);
        p.knob(bias, 154, 116);
        p.knob(range, 198, 116);
        p.knob(steps, 22, 172);
        p.knob(voices, 66, 172);
        p.knob(gateChance, 154, 172);
        p.knob(gateBias, 198, 172);
        p.choice(scale, 8, 210, 120, 18);
        p.choice(root, 132, 210, 80, 18);
        p.input(clock, 22, 256);
        p.input(reset, 66, 256);
        p.output(stepped, 154, 256);
        p.output(smooth, 198, 256);
        p.output(gateA, 110, 296);
        p.output(gateB, 154, 296);
        p.output(gateC, 198, 296);
    }

    void draw(Canvas& g) {
        g.well(8, 28, 204, 60);
        const int len = clamp(shownLoop.get(), 1, 16);
        const int pos = shownPos.get();
        const double colW = 192.0 / 16.0;
        const double top = 34.0;
        const double h = 38.0;
        for (int k = 0; k < 16; ++k) {
            const double x = 14.0 + k * colW;
            const bool live = k < len;
            g.globalAlpha(live ? 1.0 : 0.25);
            if (live && k == pos) {
                g.fillStyle(Theme::raised);
                g.fillRect(x, 30, colW, 56);
            }
            const double v = static_cast<double>(shownValue[k]);
            if (v >= 0.0) {
                const double y = top + h * (1.0 - v);
                g.fillStyle(Theme::accent);
                g.globalAlpha((live ? 1.0 : 0.25) * 0.3);
                g.fillRect(x + 2, y, colW - 4, top + h - y);
                g.globalAlpha(live ? 1.0 : 0.25);
                g.fillRect(x + 2, y - 1.5, colW - 4, 3);
            } else {
                g.fillStyle(Theme::line);
                g.fillRect(x + colW * 0.5 - 1, top + h - 2, 2, 2);
            }
            // The gate this step fires, as a coloured mark.
            const int gate = shownGate[k];
            if (gate > 0) {
                g.fillStyle(gate == 1 ? Theme::accent
                                      : (gate == 2 ? Theme::accent2 : Theme::modulation));
                g.beginPath();
                g.roundRect(x + 2, top + h + 5, colW - 4, 6, 2);
                g.fill();
            }
        }
        g.globalAlpha(1.0);
    }
};
