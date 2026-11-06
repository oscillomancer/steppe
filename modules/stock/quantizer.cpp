// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A quantizer: pitch in, the nearest note of a scale out, poly. Twenty-two
// scales and a custom one, any root, rounding to the nearest note or always
// down or up. Transpose moves the input in semitones before it is
// quantized; Degrees moves the result along the scale (+2 is a third
// higher in a major scale, wherever it lands), so a cable into Degrees
// plays melodies that stay in key. With a clock connected it samples and
// holds, quantizing on each rising edge. Changed fires as a channel's note
// changes: patch it to an envelope to play only new notes. On its face, a
// keyboard: the scale's notes lit, the root marked, the notes playing
// bright; clicking a key makes the scale custom and toggles that note.

constexpr int kCustom = 22;

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
            return 1451;  // phrygian
        case 5:
            return 2773;  // lydian
        case 6:
            return 1717;  // mixolydian
        case 7:
            return 1387;  // locrian
        case 8:
            return 2477;  // harmonic minor
        case 9:
            return 2733;  // melodic minor
        case 10:
            return 661;  // major pentatonic
        case 11:
            return 1193;  // minor pentatonic
        case 12:
            return 1257;  // blues
        case 13:
            return 1365;  // whole tone
        case 14:
            return 2925;  // diminished, whole-half
        case 15:
            return 397;  // hirajoshi
        case 16:
            return 1187;  // in sen
        case 17:
            return 1459;  // phrygian dominant
        case 18:
            return 2483;  // double harmonic
        case 19:
            return 145;  // major triad
        case 20:
            return 137;  // minor triad
        default:
            return 129;  // root and fifth
    }
}

// Whether `degree` (0..11) is in a 12-bit mask.
bool inMask(int mask, int degree) {
    int m = mask;
    for (int k = 0; k < degree; ++k) m = m / 2;
    return m % 2 == 1;
}

// Whether a pitch class (0..11, C is 0) is white on a keyboard.
bool whiteKey(int pc) { return !(pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10); }

struct Quantizer : Module {
    static constexpr ModuleInfo info = {
        .name = "Quantizer",
        .id = "com.suppleaudio.rack.quantizer",
        .vendor = "Supple Audio",
        .description = "Pitch to the nearest note of a scale, with transposition in degrees.",
        .tags = {"quantizer", "pitch", "poly"}};

    Param scale = choice("Scale",
                         {"Chromatic",
                          "Major",
                          "Minor",
                          "Dorian",
                          "Phrygian",
                          "Lydian",
                          "Mixolydian",
                          "Locrian",
                          "Harmonic Minor",
                          "Melodic Minor",
                          "Major Pentatonic",
                          "Minor Pentatonic",
                          "Blues",
                          "Whole Tone",
                          "Diminished",
                          "Hirajoshi",
                          "In Sen",
                          "Phrygian Dominant",
                          "Double Harmonic",
                          "Major Triad",
                          "Minor Triad",
                          "Fifths",
                          "Custom"},
                         1)
                      .help(
                          "The notes it allows; Custom takes the notes ticked below (or "
                          "clicked on its keyboard), whatever the root.");
    Param root =
        choice("Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
            .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                   "a_sharp", "b"})
            .activeUnless(scale, {"custom"})
            .help("The scale's first note.");
    Param transpose =
        semitones("Transpose", -24, 24, 0).help("Added to the input before it is quantized.");
    Param degrees =
        integer("Degrees", -14, 14, 0)
            .help(
                "Steps along the scale after quantizing: in a major scale +2 is a third up, +7 an "
                "octave.");
    Param rounding = choice("Rounding", {"Nearest", "Down", "Up"}, 0)
                         .help(
                             "Which note an input between two takes: the nearer, the lower "
                             "or the higher.");
    Param custom_c = toggle("Custom C", true)
                         .withKey("custom_c")
                         .in("Custom")
                         .help("Whether the custom scale has C.");
    Param custom_cs = toggle("Custom C#", false)
                          .withKey("custom_cs")
                          .in("Custom")
                          .help("Whether the custom scale has C sharp.");
    Param custom_d = toggle("Custom D", true)
                         .withKey("custom_d")
                         .in("Custom")
                         .help("Whether the custom scale has D.");
    Param custom_ds = toggle("Custom D#", false)
                          .withKey("custom_ds")
                          .in("Custom")
                          .help("Whether the custom scale has D sharp.");
    Param custom_e = toggle("Custom E", true)
                         .withKey("custom_e")
                         .in("Custom")
                         .help("Whether the custom scale has E.");
    Param custom_f = toggle("Custom F", false)
                         .withKey("custom_f")
                         .in("Custom")
                         .help("Whether the custom scale has F.");
    Param custom_fs = toggle("Custom F#", false)
                          .withKey("custom_fs")
                          .in("Custom")
                          .help("Whether the custom scale has F sharp.");
    Param custom_g = toggle("Custom G", true)
                         .withKey("custom_g")
                         .in("Custom")
                         .help("Whether the custom scale has G.");
    Param custom_gs = toggle("Custom G#", false)
                          .withKey("custom_gs")
                          .in("Custom")
                          .help("Whether the custom scale has G sharp.");
    Param custom_a = toggle("Custom A", true)
                         .withKey("custom_a")
                         .in("Custom")
                         .help("Whether the custom scale has A.");
    Param custom_as = toggle("Custom A#", false)
                          .withKey("custom_as")
                          .in("Custom")
                          .help("Whether the custom scale has A sharp.");
    Param custom_b = toggle("Custom B", false)
                         .withKey("custom_b")
                         .in("Custom")
                         .help("Whether the custom scale has B.");

    Input pitch = pitchIn("Pitch").help("The pitch to quantize, in octaves from C4; poly.");
    Input clock = gateIn("Clock").help(
        "When connected, it quantizes only on each rising edge and holds the note between.");

    Output out = pitchOut("Out").help("The note, in octaves from C4.");
    Output changed = gateOut("Changed").help("A 1 ms trigger as a channel's note changes.");

    struct Voice {
        double input = 1e9;  // the last input quantized, for reuse
        int shift = 0;
        int note = 0;  // semitones from C4
        bool started = false;
        bool high = false;
        int pulse = 0;
    };
    Array<Voice, 16> voices;
    Array<bool, 12> allowed;  // pitch classes, from C
    int version = 0;          // bumped when the scale changes, to requantize
    int lastMask = -1;
    int pulseFrames = 48;

    SharedArray<int, 16> shownNote;
    Shared<int> shownCount;

    // The editor's own.
    int pressed = -1;

    void factoryPatches(Patches& p) {
        p.add("Major").set(scale, "major");
        p.add("Minor Pentatonic").set(scale, "minor_pentatonic").set(root, "a");
        p.add("Dorian in D").set(scale, "dorian").set(root, "d");
        p.add("Hirajoshi").set(scale, "hirajoshi").set(root, "e");
        p.add("Arpeggio").set(scale, "major_triad");
    }

    void prepare() { pulseFrames = max(1, roundToInt(0.001 * sampleRate())); }

    bool customHas(int pc) const {
        switch (pc) {
            case 0:
                return custom_c.on();
            case 1:
                return custom_cs.on();
            case 2:
                return custom_d.on();
            case 3:
                return custom_ds.on();
            case 4:
                return custom_e.on();
            case 5:
                return custom_f.on();
            case 6:
                return custom_fs.on();
            case 7:
                return custom_g.on();
            case 8:
                return custom_gs.on();
            case 9:
                return custom_a.on();
            case 10:
                return custom_as.on();
            default:
                return custom_b.on();
        }
    }

    void editCustom(int pc, bool on) const {
        const double v = on ? 1.0 : 0.0;
        switch (pc) {
            case 0:
                custom_c.edit(v);
                break;
            case 1:
                custom_cs.edit(v);
                break;
            case 2:
                custom_d.edit(v);
                break;
            case 3:
                custom_ds.edit(v);
                break;
            case 4:
                custom_e.edit(v);
                break;
            case 5:
                custom_f.edit(v);
                break;
            case 6:
                custom_fs.edit(v);
                break;
            case 7:
                custom_g.edit(v);
                break;
            case 8:
                custom_gs.edit(v);
                break;
            case 9:
                custom_a.edit(v);
                break;
            case 10:
                custom_as.edit(v);
                break;
            default:
                custom_b.edit(v);
                break;
        }
    }

    // Whether pitch class pc is in the scale as set (the mask, as 12 bits from C).
    int currentMask(int k, int r) const {
        int mask = 0;
        int bit = 1;
        for (int pc = 0; pc < 12; ++pc) {
            const bool inside =
                k == kCustom ? customHas(pc) : inMask(scaleMask(k), wrap(pc - r, 12));
            if (inside) mask += bit;
            bit = bit * 2;
        }
        return mask == 0 ? 1 : mask;  // an empty custom scale plays C
    }

    bool has(int n) { return allowed[wrap(n, 12)]; }

    // The note for x semitones, then `shift` degrees along the scale.
    int quantize(double x, int shift, int mode) {
        const int below = floorToInt(x);
        int down = below;
        for (int k = 0; k < 12 && !has(down); ++k) --down;
        int up = below + 1;
        if (has(below) && abs(x - below) < 1e-9) up = below;
        for (int k = 0; k < 12 && !has(up); ++k) ++up;
        int n = mode == 1 ? down : (mode == 2 ? up : (x - down <= up - x ? down : up));
        for (int s = 0; s < shift; ++s) {
            ++n;
            for (int k = 0; k < 12 && !has(n); ++k) ++n;
        }
        for (int s = 0; s < -shift; ++s) {
            --n;
            for (int k = 0; k < 12 && !has(n); ++k) --n;
        }
        return n;
    }

    void process(int frames) {
        const int k = clamp(scale.index(), 0, kCustom);
        const int mask = currentMask(k, clamp(root.index(), 0, 11));
        if (mask != lastMask) {
            lastMask = mask;
            ++version;
            for (int pc = 0; pc < 12; ++pc) allowed[pc] = inMask(mask, pc);
            for (int c = 0; c < 16; ++c) voices[c].input = 1e9;
        }
        const int mode = clamp(rounding.index(), 0, 2);
        const bool sampled = clock.connected();
        for (int c = 0; c < channels(); ++c) {
            Voice& v = voices[c];
            for (int i = 0; i < frames; ++i) {
                bool take = !sampled;
                if (sampled) {
                    const double g = clock[c][i];
                    if (!v.high && g >= 0.5) {
                        v.high = true;
                        take = true;
                    } else if (v.high && g < 0.25) {
                        v.high = false;
                    }
                }
                if (take || !v.started) {
                    const double x = pitch[c][i] * 12.0 + transpose.at(i, c);
                    const int shift = roundToInt(degrees.at(i, c));
                    if (x != v.input || shift != v.shift) {
                        const int note = quantize(clamp(x, -1000.0, 1000.0), shift, mode);
                        if (v.started && note != v.note) v.pulse = pulseFrames;
                        v.note = note;
                        v.input = x;
                        v.shift = shift;
                        v.started = true;
                    }
                }
                out[c][i] = v.note / 12.0;
                changed[c][i] = v.pulse > 0 ? 1.0 : 0.0;
                if (v.pulse > 0) --v.pulse;
            }
        }
        for (int c = 0; c < 16; ++c) shownNote[c] = voices[c].note;
        shownCount.set(channels());
    }

    // --- the face ------------------------------------------------------------------------

    static constexpr double kKeysX = 12.0;
    static constexpr double kKeysY = 32.0;
    static constexpr double kKeysW = 152.0;
    static constexpr double kKeysH = 48.0;

    void panel(Panel& p) {
        p.size(176, 252);
        p.display(8, 28, 160, 56);
        p.choice(scale, 8, 92, 104, 18);
        p.choice(root, 116, 92, 52, 18);
        p.knob(transpose, 44, 144);
        p.knob(degrees, 132, 144);
        p.choice(rounding, 8, 180, 160, 18);
        p.input(pitch, 22, 226);
        p.input(clock, 66, 226);
        p.output(out, 110, 226);
        p.output(changed, 154, 226);
    }

    // The key's rectangle: white keys a seventh of the width, black keys
    // narrower and shorter, between them.
    double keyX(int pc) const {
        const double w = kKeysW / 7.0;
        int whites = 0;
        for (int k = 0; k < pc; ++k)
            if (whiteKey(k)) ++whites;
        return whiteKey(pc) ? kKeysX + whites * w : kKeysX + whites * w - w * 0.3;
    }
    double keyW(int pc) const { return whiteKey(pc) ? kKeysW / 7.0 : kKeysW / 7.0 * 0.6; }
    double keyH(int pc) const { return whiteKey(pc) ? kKeysH : kKeysH * 0.6; }

    void drawKey(Canvas& g, int pc, int mask, int r, bool lit) {
        const bool inScale = inMask(mask, pc);
        const double x = keyX(pc);
        const double w = keyW(pc);
        const double h = keyH(pc);
        g.fillStyle(whiteKey(pc) ? Theme::raised : Theme::recess);
        g.beginPath();
        g.roundRect(x + 0.5, kKeysY, w - 1.0, h, 2);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1.0);
        g.stroke();
        if (inScale) {
            g.fillStyle(lit ? Theme::text : Theme::accent);
            g.globalAlpha(lit ? 1.0 : 0.85);
            g.beginPath();
            g.circle(x + w * 0.5, kKeysY + h - 7.0, pc == r ? 4.0 : 2.8);
            g.fill();
            g.globalAlpha(1.0);
            if (pc == r) {
                g.strokeStyle(Theme::accent);
                g.lineWidth(1.2);
                g.beginPath();
                g.circle(x + w * 0.5, kKeysY + h - 7.0, 6.0);
                g.stroke();
            }
        } else if (lit) {
            g.fillStyle(Theme::alert);
            g.beginPath();
            g.circle(x + w * 0.5, kKeysY + h - 7.0, 2.8);
            g.fill();
        }
    }

    void draw(Canvas& g) {
        g.well(8, 28, 160, 56);
        const int k = clamp(scale.index(), 0, kCustom);
        const int r = k == kCustom ? -1 : clamp(root.index(), 0, 11);
        const int mask = currentMask(k, max(0, r));
        const int n = clamp(shownCount.get(), 1, 16);
        // White keys first, then the black ones over them.
        for (int pass = 0; pass < 2; ++pass)
            for (int pc = 0; pc < 12; ++pc) {
                if (whiteKey(pc) != (pass == 0)) continue;
                bool lit = false;
                for (int c = 0; c < n; ++c) lit = lit || wrap(shownNote[c], 12) == pc;
                drawKey(g, pc, mask, r, lit);
            }
    }

    // The key under a point, black keys first; -1 for none.
    int keyAt(double x, double y) const {
        for (int pass = 0; pass < 2; ++pass)
            for (int pc = 0; pc < 12; ++pc) {
                if (whiteKey(pc) != (pass == 1)) continue;
                if (x >= keyX(pc) && x < keyX(pc) + keyW(pc) && y >= kKeysY &&
                    y < kKeysY + keyH(pc))
                    return pc;
            }
        return -1;
    }

    void pointer(const Pointer& p) {
        if (p.action() != PointerAction::Press) return;
        const int pc = keyAt(p.x(), p.y());
        if (pc < 0) return;
        // A click makes the scale custom, starting from the scale as it was.
        const int k = clamp(scale.index(), 0, kCustom);
        const int mask = currentMask(k, clamp(root.index(), 0, 11));
        if (k != kCustom) {
            scale.edit(kCustom);
            for (int q = 0; q < 12; ++q) editCustom(q, inMask(mask, q));
        }
        editCustom(pc, !inMask(mask, pc));
        pressed = pc;
    }
};
