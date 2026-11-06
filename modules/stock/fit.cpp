// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A quantizer that follows the harmony: each pitch in goes to the nearest
// note of the chord playing on Chord (a poly cable of its notes, such as
// Changes' or Chord's), in any octave, so a melody from a shift register or
// a random source lands on chord tones as the chords change. Snap rounds to
// the nearest tone, or always down or up. Degree moves the result along the
// chord's tones (+1 the next tone up), and a CV in Walk moves it further,
// an octave of tones a unit, so an LFO there plays an arpeggio of whatever
// chord is playing. With a clock in, it samples on each rising edge and
// holds between; Follow moves a held note to the new chord's nearest tone
// when the chord changes under it. Changed fires as a channel's note
// changes. With nothing in Chord, it rounds to semitones.

int fitMask(int pc) {
    int m = 1;
    for (int k = 0; k < pc; ++k) m = m * 2;
    return m;
}

bool fitHas(int mask, int n) {
    int m = mask;
    for (int k = 0; k < wrap(n, 12); ++k) m = m / 2;
    return m % 2 == 1;
}

bool fitWhite(int pc) { return !(pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10); }

struct Fit : Module {
    static constexpr ModuleInfo info = {
        .name = "Fit",
        .id = "com.suppleaudio.rack.fit",
        .vendor = "Supple Audio",
        .description = "Pitch to the nearest note of the chord playing, so a melody follows it.",
        .tags = {"pitch", "chord", "utility", "poly"}};

    Param snap = choice("Snap", {"Nearest", "Down", "Up"}, 0)
                     .keys({"nearest", "down", "up"})
                     .help("Round each pitch to the chord's nearest tone, or always down or up.");
    Param degree = integer("Degree", -8, 8, 0)
                       .help("Move the note along the chord's tones: +1 the next tone up.");
    Param follow = toggle("Follow", true)
                       .help(
                           "With a clock in, move a held note to the new chord when the chord "
                           "changes under it.");

    Input pitch = pitchIn("Pitch").help("The pitch to fit, in octaves from C4; poly.");
    Input chord =
        pitchIn("Chord").help("The chord's notes on a poly cable; their octaves do not matter.");
    Input walk = cvIn("Walk").help(
        "Moves the note along the chord's tones: 1 an octave of them, -1 an octave down.");
    Input clock = gateIn("Clock").help(
        "When connected, it fits only on each rising edge and holds the note between.");

    Output out = pitchOut("Out").help("The chord tone, in octaves from C4.");
    Output changed = gateOut("Changed").help("A 1 ms trigger as a channel's note changes.");

    struct Voice {
        double input = 0.0;  // the pitch held, in semitones
        int note = 0;        // semitones from C4
        int walked = 0;      // tones Walk moves it, as held
        int mask = 4095;     // the chord it was fitted to
        double lastInput = 1e9;
        int lastMask = -1;
        int lastShift = 0;
        bool started = false;
        bool high = false;
        int pulse = 0;
    };
    Array<Voice, 16> voices;
    int pulseFrames = 48;
    int lastMask = -1;

    Shared<int> shownMask;
    SharedArray<int, 16> shownNote;
    Shared<int> shownCount;

    void factoryPatches(Patches& p) {
        p.add("Chord Tones");
        p.add("Always Down").set(snap, "down");
        p.add("A Tone Up").set(degree, 1);
        p.add("Sampled").set(follow, false);
    }

    void prepare() { pulseFrames = max(1, roundToInt(0.001 * sampleRate())); }

    // The pitch classes on Chord at frame i, as 12 bits from C; all of them
    // with nothing there.
    int maskAt(int i) {
        const int n = chord.channels();
        if (n == 0) return 4095;
        int mask = 0;
        for (int c = 0; c < n; ++c) {
            const int pc = wrap(roundToInt(clamp(chord[c][i], -20.0, 20.0) * 12.0), 12);
            if (!fitHas(mask, pc)) mask += fitMask(pc);
        }
        return mask;
    }

    // The tone for x semitones under the chord, then `shift` tones along it.
    int fit(double x, int mask, int shift, int mode) {
        const int below = floorToInt(x + 1e-9);
        int down = below;
        for (int k = 0; k < 12 && !fitHas(mask, down); ++k) --down;
        int up = fitHas(mask, below) && abs(x - below) < 1e-9 ? below : below + 1;
        for (int k = 0; k < 12 && !fitHas(mask, up); ++k) ++up;
        int n = mode == 1 ? down : (mode == 2 ? up : (x - down <= up - x ? down : up));
        const int moves = clamp(shift, -48, 48);
        for (int s = 0; s < moves; ++s) {
            ++n;
            for (int k = 0; k < 12 && !fitHas(mask, n); ++k) ++n;
        }
        for (int s = 0; s < -moves; ++s) {
            --n;
            for (int k = 0; k < 12 && !fitHas(mask, n); ++k) --n;
        }
        return n;
    }

    int tones(int mask) const {
        int count = 0;
        for (int pc = 0; pc < 12; ++pc) count += fitHas(mask, pc) ? 1 : 0;
        return max(1, count);
    }

    void process(int frames) {
        const int n = max(1, pitch.channels());
        out.setChannels(n);
        changed.setChannels(n);
        const int mode = clamp(snap.index(), 0, 2);
        const bool sampled = clock.connected();
        const bool moves = follow.on();
        int mask = lastMask;
        for (int i = 0; i < frames; ++i) {
            mask = maskAt(i);
            const int count = tones(mask);
            for (int c = 0; c < n; ++c) {
                Voice& v = voices[c];
                bool take = !sampled || !v.started;
                if (sampled) {
                    const double g = clock[c][i];
                    if (!v.high && g >= 0.5) {
                        v.high = true;
                        take = true;
                    } else if (v.high && g < 0.25) {
                        v.high = false;
                    }
                }
                if (take) {
                    v.input = clamp(pitch[c][i] * 12.0, -1000.0, 1000.0);
                    v.walked = roundToInt(clamp(walk[c][i], -4.0, 4.0) * count);
                }
                // A held note keeps its chord unless Follow moves it.
                if (take || !sampled || moves) v.mask = mask;
                const int shift = degree.index() + v.walked;
                if (v.input != v.lastInput || v.mask != v.lastMask || shift != v.lastShift) {
                    const int note = fit(v.input, v.mask, shift, mode);
                    if (v.started && note != v.note) v.pulse = pulseFrames;
                    v.note = note;
                    v.started = true;
                    v.lastInput = v.input;
                    v.lastMask = v.mask;
                    v.lastShift = shift;
                }
                out[c][i] = v.note / 12.0;
                changed[c][i] = v.pulse > 0 ? 1.0 : 0.0;
                if (v.pulse > 0) --v.pulse;
            }
        }
        lastMask = mask;
        shownMask.set(mask < 0 ? 4095 : mask);
        for (int c = 0; c < 16; ++c) shownNote[c] = voices[c].note;
        shownCount.set(n);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(176, 264);
        p.display(8, 28, 160, 56);
        p.choice(snap, 8, 92, 160, 18);
        p.knob(degree, 44, 146);
        p.toggle(follow, 108, 138, 52, 16);
        p.input(pitch, 22, 196);
        p.input(chord, 66, 196);
        p.input(walk, 110, 196);
        p.input(clock, 154, 196);
        p.output(out, 66, 238);
        p.output(changed, 110, 238);
    }

    // An octave of keys: the chord's tones marked, the notes playing bright.
    double keyX(int pc) const {
        const double w = 152.0 / 7.0;
        int whites = 0;
        for (int k = 0; k < pc; ++k)
            if (fitWhite(k)) ++whites;
        return fitWhite(pc) ? 12.0 + whites * w : 12.0 + whites * w - w * 0.3;
    }

    void draw(Canvas& g) {
        g.well(8, 28, 160, 56);
        const int mask = shownMask.get();
        const int n = clamp(shownCount.get(), 1, 16);
        const double w = 152.0 / 7.0;
        for (int pass = 0; pass < 2; ++pass)
            for (int pc = 0; pc < 12; ++pc) {
                const bool white = fitWhite(pc);
                if (white != (pass == 0)) continue;
                const double x = keyX(pc);
                const double kw = white ? w : w * 0.6;
                const double kh = white ? 48.0 : 29.0;
                g.fillStyle(white ? Theme::raised : Theme::recess);
                g.beginPath();
                g.roundRect(x + 0.5, 32.0, kw - 1.0, kh, 2);
                g.fill();
                g.strokeStyle(Theme::line);
                g.lineWidth(1.0);
                g.stroke();
                bool lit = false;
                for (int c = 0; c < n; ++c) lit = lit || wrap(shownNote[c], 12) == pc;
                if (fitHas(mask, pc) && mask != 4095) {
                    g.fillStyle(Theme::accent);
                    g.beginPath();
                    g.roundRect(x + 3.0, 32.0 + kh - 11.0, kw - 6.0, 7.0, 2);
                    g.fill();
                }
                if (lit) {
                    g.fillStyle(Theme::text);
                    g.beginPath();
                    g.circle(x + kw * 0.5, 32.0 + kh - 18.0, 3.0);
                    g.fill();
                }
            }
    }
};
