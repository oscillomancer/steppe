// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A round from any line: the notes coming in (a pitch and its gate) are
// taken a step a clock, and up to three followers play them again Delay
// steps later, Shift semitones away: a canon at the octave, the fifth or
// the unison, a melody chasing itself. Each follower has its own Pitch and
// Gate, and All and All Gates carry the lead and its followers on one poly
// cable, for one poly voice. Thin drops a follower's note now and then, so
// the echoes answer only in part. A step sounds when the lead's gate is
// high at its clock, and a follower's gate lasts as long as the lead's did
// in that step (a held note ties on). With nothing in Clock it steps on the
// song's sixteenths; Reset forgets the line and thins the same way again.

constexpr int kCanonSteps = 64;

struct Canon : Module {
    static constexpr ModuleInfo info = {
        .name = "Canon",
        .id = "com.suppleaudio.rack.canon",
        .vendor = "Supple Audio",
        .description = "A melody chased by up to three delayed, transposed copies of itself.",
        .tags = {"sequencer", "pitch", "generative", "poly"}};

    Param voices = integer("Voices", 1, 3, 3).help("How many followers play.");
    Param delay_1 = integer("Delay 1", 1, 32, 4).help("Steps follower 1 plays behind the lead.");
    Param shift_1 = semitones("Shift 1", -24, 24, 0).help("Follower 1's transposition.");
    Param delay_2 = integer("Delay 2", 1, 32, 8).help("Steps follower 2 plays behind the lead.");
    Param shift_2 = semitones("Shift 2", -24, 24, 7).help("Follower 2's transposition.");
    Param delay_3 = integer("Delay 3", 1, 32, 12).help("Steps follower 3 plays behind the lead.");
    Param shift_3 = semitones("Shift 3", -24, 24, 12).help("Follower 3's transposition.");
    Param thin = percent("Thin", 0).help("How likely a follower is to leave a note out.");

    Input in = pitchIn("In").help("The lead's pitch, taken at each clock.");
    Input in_gate = gateIn("In Gate").help(
        "The lead's gate: a step sounds when it is high at the clock, for as long as it stays "
        "high.");
    Input clock = gateIn("Clock").help(
        "Each rising edge takes a step; with nothing here, the song's sixteenths.");
    Input reset =
        gateIn("Reset").help("A rising edge forgets the line and starts the thinning over.");

    Outputs<3> pitch = pitchOut("Pitch").help("Its follower's note, held from step to step.");
    Outputs<3> gate = gateOut("Gate").help("Its follower's gate, as long as the lead's was.");
    Output all =
        pitchOut("All").help("The lead and its followers, a channel each: for a poly voice.");
    Output all_gates =
        gateOut("All Gates").help("The lead's gate and the followers', a channel each.");

    Array<double, kCanonSteps> notes;
    Array<int, kCanonSteps> sounded;
    Array<int, kCanonSteps> lengths;  // frames the lead's gate stayed high in each step
    Array<double, 3> held;            // each follower's note
    Array<int, 3> playing;            // whether its step sounds
    Array<int, 3> remain;             // frames left of its gate
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    bool clockHigh = false;
    bool wasPlaying = false;
    int head = -1;  // the step last written
    int run = 0;    // frames the lead's gate has been high in this step
    bool open = false;
    int salt = 0;
    bool started = false;

    SharedArray<float, 32> shownNotes;
    SharedArray<int, 32> shownSounded;
    SharedArray<int, 3> shownDelay;
    SharedArray<int, 3> shownShift;
    Shared<int> shownVoices;

    void factoryPatches(Patches& p) {
        p.add("Round")
            .set(delay_1, 8)
            .set(shift_1, 0)
            .set(delay_2, 16)
            .set(shift_2, 0)
            .set(voices, 2);
        p.add("Fifths and Octaves");
        p.add("Close Echo").set(voices, 1).set(delay_1, 1).set(shift_1, 12);
        p.add("Answers").set(thin, 40).set(delay_1, 3).set(delay_2, 5).set(delay_3, 7);
        p.add("Bass Shadow").set(voices, 1).set(delay_1, 2).set(shift_1, -12);
    }

    void prepare() {
        salt = seed();
        started = false;
    }

    void forget() {
        for (int k = 0; k < kCanonSteps; ++k) {
            notes[k] = 0.0;
            sounded[k] = 0;
            lengths[k] = 0;
        }
        for (int v = 0; v < 3; ++v) {
            held[v] = 0.0;
            playing[v] = 0;
            remain[v] = 0;
        }
        head = -1;
        run = 0;
        open = false;
        rng.seed(salt);
    }

    int delayOf(int v) const {
        switch (v) {
            case 0:
                return clamp(delay_1.index(), 1, 32);
            case 1:
                return clamp(delay_2.index(), 1, 32);
            default:
                return clamp(delay_3.index(), 1, 32);
        }
    }

    double shiftOf(int v, int i) const {
        switch (v) {
            case 0:
                return roundToInt(shift_1.at(i));
            case 1:
                return roundToInt(shift_2.at(i));
            default:
                return roundToInt(shift_3.at(i));
        }
    }

    int shiftShown(int v) const {
        switch (v) {
            case 0:
                return roundToInt(shift_1.value());
            case 1:
                return roundToInt(shift_2.value());
            default:
                return roundToInt(shift_3.value());
        }
    }

    void step(int i) {
        head = wrap(head + 1, kCanonSteps);
        notes[head] = in[0][i];
        sounded[head] = in_gate[0][i] >= 0.5 ? 1 : 0;
        lengths[head] = 0;
        run = 0;
        open = sounded[head] == 1;
        const double drop = clamp(thin.at(i), 0.0, 1.0);
        for (int v = 0; v < 3; ++v) {
            // A draw a follower a step, whatever Thin and Voices.
            const double u = rng.next();
            const int from = wrap(head - delayOf(v), kCanonSteps);
            playing[v] = sounded[from] == 1 && u >= drop ? 1 : 0;
            remain[v] = playing[v] == 1 ? lengths[from] : 0;
            if (sounded[from] == 1) held[v] = notes[from] + shiftOf(v, i) / 12.0;
        }
    }

    void process(int frames) {
        if (!started) {
            started = true;
            forget();
        }
        const int n = clamp(voices.index(), 1, 3);
        for (int v = 0; v < 3; ++v) {
            pitch[v].setChannels(1);
            gate[v].setChannels(1);
        }
        all.setChannels(n + 1);
        all_gates.setChannels(n + 1);
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) forget();
        wasPlaying = t.playing;
        const double perFrame = t.tempo / 15.0 / sampleRate();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) forget();
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
            }
            if (rose) step(i);
            if (open && in_gate[0][i] < 0.25) open = false;
            if (open && head >= 0) {
                ++run;
                lengths[head] = run;
            }
            all[0][i] = in[0][i];
            all_gates[0][i] = in_gate[0][i];
            for (int v = 0; v < 3; ++v) {
                const double g = remain[v] > 0 && v < n ? 1.0 : 0.0;
                if (remain[v] > 0) --remain[v];
                pitch[v][0][i] = held[v];
                gate[v][0][i] = g;
                if (v < n) {
                    all[v + 1][i] = held[v];
                    all_gates[v + 1][i] = g;
                }
            }
        }
        for (int k = 0; k < 32; ++k) {
            const int at = wrap(head - 31 + k, kCanonSteps);
            shownNotes[k] = static_cast<float>(notes[at]);
            shownSounded[k] = head < 0 ? 0 : sounded[at];
        }
        for (int v = 0; v < 3; ++v) {
            shownDelay[v] = delayOf(v);
            shownShift[v] = shiftShown(v);
        }
        shownVoices.set(n);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(240, 444);
        p.display(8, 28, 224, 110);
        p.knob(voices, 70, 172, 28);
        p.knob(thin, 170, 172, 28);
        // A column a follower: its Delay, Shift, Pitch and Gate.
        for (int v = 0; v < 3; ++v) {
            const double x = 50.0 + 70.0 * v;
            p.output(pitch[v], x, 330);
            p.output(gate[v], x, 372);
        }
        p.knob(delay_1, 50, 226, 28);
        p.knob(delay_2, 120, 226, 28);
        p.knob(delay_3, 190, 226, 28);
        p.knob(shift_1, 50, 280, 28);
        p.knob(shift_2, 120, 280, 28);
        p.knob(shift_3, 190, 280, 28);
        p.input(in, 28, 420);
        p.input(in_gate, 65, 420);
        p.input(clock, 102, 420);
        p.input(reset, 139, 420);
        p.output(all, 176, 420);
        p.output(all_gates, 213, 420);
    }

    // The last 32 steps as a roll: the lead's notes, and each follower's
    // copy where it plays, later and shifted.
    void draw(Canvas& g) {
        g.well(8, 28, 224, 110);
        double lo = 1e9;
        double hi = -1e9;
        for (int k = 0; k < 32; ++k) {
            if (shownSounded[k] == 0) continue;
            const double v = static_cast<double>(shownNotes[k]);
            lo = min(lo, v);
            hi = max(hi, v);
        }
        if (lo > hi) {
            lo = 0.0;
            hi = 1.0;
        }
        const int n = shownVoices.get();
        double top = hi;
        double bottom = lo;
        for (int v = 0; v < n; ++v) {
            top = max(top, hi + shownShift[v] / 12.0);
            bottom = min(bottom, lo + shownShift[v] / 12.0);
        }
        const double span = max(0.25, top - bottom);
        const double x0 = 14.0;
        const double w = 212.0 / 32.0;
        for (int v = -1; v < n; ++v) {
            const int d = v < 0 ? 0 : shownDelay[v];
            const double s = v < 0 ? 0.0 : shownShift[v] / 12.0;
            g.fillStyle(v < 0 ? Theme::text : (imod(v, 2) == 0 ? Theme::accent : Theme::accent2));
            g.globalAlpha(v < 0 ? 1.0 : 0.85);
            for (int k = 0; k + d < 32; ++k) {
                if (shownSounded[k] == 0) continue;
                const double y =
                    130.0 - (static_cast<double>(shownNotes[k]) + s - bottom) / span * 92.0;
                g.beginPath();
                g.roundRect(x0 + (k + d) * w, y - 2.0, w - 1.0, 4.0, 1.5);
                g.fill();
            }
        }
        g.globalAlpha(1.0);
    }
};
