// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Melodies in phrases, a call and its answer: the call is a short line on
// a rhythm drawn by the metre (downbeats most likely, sixteenths least),
// moving mostly by step, a leap turned back the other way, and ending on
// an open note (the second or the fourth of the key). The answer keeps the
// call's rhythm and its first notes, then goes its own way, Contrast of it,
// often turning the call's moves upside down, and comes home to the key's
// first note. Form is the order: a call and an answer; the call twice and
// the answer (as a blues verse); or a period, call, half answer (ending on
// the fifth), call, full answer. Renew is how likely a new call is written
// each time the form comes round (0 locks it); a trigger in Write writes
// one next time. Each step is a sixteenth of the song, or a clock. Answer
// is high while an answer plays, Start fires at each call and answer, and
// Accent is each note's weight: the form's first, then downbeats, then
// beats.

constexpr int kPhraseSteps = 32;  // two bars of sixteenths

// The seven notes of each mode, in semitones above the key, packed a hex
// digit a degree from the lowest.
int phraseMode(int m) {
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

struct Phrase : Module {
    static constexpr ModuleInfo info = {
        .name = "Phrase",
        .id = "com.suppleaudio.rack.phrase",
        .vendor = "Supple Audio",
        .description = "Melodies in phrases: a call, and an answer on its rhythm that comes home.",
        .tags = {"generative", "sequencer", "pitch", "random"}};

    Param key = choice("Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
                    .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                           "a_sharp", "b"})
                    .help("The key's first note: where the answers come home.");
    Param mode =
        choice("Mode",
               {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Harmonic Minor"},
               1)
            .keys(
                {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "harmonic_minor"})
            .help("The scale the melody is drawn from.");
    Param form = choice("Form", {"Call, Answer", "Call, Call, Answer", "Period"}, 0)
                     .keys({"ab", "aab", "period"})
                     .help(
                         "The order of the phrases: a call and its answer; the call twice and the "
                         "answer; or call, half answer, call, full answer.");
    Param length = choice("Length", {"1 Bar", "2 Bars"}, 0)
                       .keys({"bar_1", "bars_2"})
                       .help("How long each call and answer is, in sixteenths of 4/4.");
    Param density =
        percent("Density", 50)
            .help("How many of the steps start a note: downbeats first, sixteenths last.");
    Param range = integer("Range", 3, 12, 7).help("How many notes of the scale the melody spans.");
    Param contrast = percent("Contrast", 50)
                         .help(
                             "How much of each answer goes its own way: at 0 only its last note "
                             "differs from the call's.");
    Param renew =
        percent("Renew", 25)
            .help("How likely a new call is written each time the form comes round: 0 % locks it.");
    Param legato =
        percent("Legato", 60).help("How much of the time to the next note each note holds.");
    Param octave = integer("Octave", -2, 2, 0).help("Where the melody sits: 0 from C4.");

    Input clock = gateIn("Clock").help(
        "Each rising edge is a step; with nothing here, the song's sixteenths.");
    Input reset = gateIn("Reset").help("A rising edge starts the same phrases over.");
    Input write = gateIn("Write").help("A rising edge writes a new call for the next time round.");

    Output pitch = pitchOut("Pitch").help("The note, held between notes.");
    Output gate =
        gateOut("Gate").help("High while a note sounds, low for 2 ms between tied notes.");
    Output accent = unipolarOut("Accent").help(
        "The note's weight, held: 1 for the form's first, less on downbeats, beats and between.");
    Output answer = gateOut("Answer").help("High while an answer plays.");
    Output start = gateOut("Start").help("A trigger as each call and answer begins.");

    // The call: whether each step starts a note, its degree, and how many
    // steps it holds; then the answers' degrees, half (open) and full.
    Array<bool, kPhraseSteps> onset;
    Array<int, kPhraseSteps> callDeg;
    Array<int, kPhraseSteps> halfDeg;
    Array<int, kPhraseSteps> fullDeg;
    Array<int, kPhraseSteps> holds;
    Random rng;
    Trigger clockEdge;
    Trigger resetEdge;
    Trigger writeEdge;
    Pulse startPulse;
    int salt = 0;
    int pos = -1;  // the step playing in the form
    int lastSong = -1;
    int written = -1;  // the steps a phrase it was written for
    int rounds = 0;    // times the form has begun since the start
    bool wanted = false;
    bool wasPlaying = false;
    bool started = false;
    int note = 0;  // semitones from C4
    double weight = 0.0;
    bool open = false;
    int remain = 0;  // steps left of the note
    int gap = 0;     // frames left of a tie's gap

    SharedArray<int, kPhraseSteps> shownCall;
    SharedArray<int, kPhraseSteps> shownHalf;
    SharedArray<int, kPhraseSteps> shownFull;
    SharedArray<int, kPhraseSteps> shownHold;
    Shared<int> shownPos;
    Shared<int> shownSteps;
    Shared<int> shownForm;

    void factoryPatches(Patches& p) {
        p.add("Call and Answer");
        p.add("Blues Verse").set(form, "aab").set(mode, "mixolydian").set(density, 40);
        p.add("Period").set(form, "period").set(mode, "major").set(length, "bars_2");
        p.add("Busy Riff").set(density, 80).set(range, 5).set(legato, 40).set(renew, 10);
        p.add("Locked").set(renew, 0).set(contrast, 70);
    }

    void prepare() {
        salt = seed();
        started = false;  // written on the first block
    }

    int steps() const { return length.index() == 1 ? 32 : 16; }
    int sections() const {
        const int f = form.index();
        return f == 1 ? 3 : (f == 2 ? 4 : 2);
    }
    // What section s of the form plays: 0 the call, 1 the half answer, 2 the full.
    int kindOf(int s) const {
        const int f = form.index();
        if (f == 1) return s == 2 ? 2 : 0;
        if (f == 2) return s == 1 ? 1 : (s == 3 ? 2 : 0);
        return s == 1 ? 2 : 0;
    }

    // Degree d's note in semitones above the key, any octave.
    int noteOf(int d) const {
        const int packed = phraseMode(mode.index());
        const int oct = d >= 0 ? idiv(d, 7) : -idiv(6 - d, 7);
        const int k = d - 7 * oct;
        return imod(shr(packed, 4 * k), 16) + 12 * oct;
    }

    // A melodic move for a draw: mostly steps, sometimes thirds, seldom leaps.
    int move(double u) const {
        if (u < 0.28) return -1;
        if (u < 0.56) return 1;
        if (u < 0.68) return -2;
        if (u < 0.80) return 2;
        if (u < 0.88) return 0;
        if (u < 0.94) return u < 0.91 ? -3 : -4;
        return u < 0.97 ? 3 : 4;
    }

    // The nearest degree to `from` that is `ending` in some octave, inside lo..hi.
    int land(int ending, int from, int lo, int hi) const {
        int best = ending;
        int bestCost = 1000;
        for (int o = -3; o <= 3; ++o) {
            const int d = ending + 7 * o;
            if (d < lo || d > hi) continue;
            const int cost = abs(d - from);
            if (cost < bestCost) {
                bestCost = cost;
                best = d;
            }
        }
        return best;
    }

    // The next degree from `from` by `by`, kept in lo..hi by turning back.
    int keep(int from, int by, int lo, int hi) const {
        int d = from + by;
        if (d > hi) d = from - abs(by);
        if (d < lo) d = from + abs(by);
        return clamp(d, lo, hi);
    }

    // Writes a new call and its answers. The same number of draws whatever
    // the knobs: three a step and two more.
    void compose() {
        const int n = steps();
        const int span = clamp(range.index(), 3, 12);
        const int lo = -(span / 2);
        const int hi = lo + span - 1;
        const double dens = clamp(density.value(), 0.0, 1.0);
        const double turn = clamp(contrast.value(), 0.0, 1.0);
        const double hold = clamp(legato.value(), 0.0, 1.0);
        const double endA = rng.next();
        const double startU = rng.next();
        int count = 0;
        for (int k = 0; k < kPhraseSteps; ++k) {
            const double u = rng.next();
            double w = 0.35;
            if (k % 16 == 0)
                w = 1.0;
            else if (k % 4 == 0)
                w = 0.8;
            else if (k % 2 == 0)
                w = 0.55;
            // The last step breathes.
            const bool last = k == n - 1;
            onset[k] = k < n && (k == 0 || (!last && u < min(1.0, 2.0 * dens * w)));
            if (onset[k]) ++count;
        }
        // The call's line, and the answers' moves, from their own draws.
        int d = startU < 0.5 ? 0 : (startU < 0.8 ? 2 : 4);
        d = clamp(d, lo, hi);
        int lastLeap = 0;
        int seen = 0;
        const int changed = roundToInt(turn * (count - 1));
        int a = d;  // the answer's line
        for (int k = 0; k < kPhraseSteps; ++k) {
            const double u = rng.next();
            const double v = rng.next();
            if (!onset[k]) {
                callDeg[k] = d;
                halfDeg[k] = a;
                fullDeg[k] = a;
                continue;
            }
            const int prev = d;
            if (seen > 0) {
                int by = move(u);
                // A leap is filled in: the next move steps back.
                if (lastLeap != 0) by = lastLeap > 0 ? -1 : 1;
                lastLeap = abs(by) >= 3 ? by : 0;
                d = keep(d, by, lo, hi);
            }
            // The answer follows the call until its last `changed` notes.
            if (seen == 0 || count - seen > changed) {
                a = d;
            } else {
                const int by = v < 0.5 ? prev - d : move(v * 2.0 - 1.0);
                a = keep(a, by, lo, hi);
            }
            callDeg[k] = d;
            halfDeg[k] = a;
            fullDeg[k] = a;
            ++seen;
        }
        // Endings: the call open, on the second or the fourth; the half
        // answer on the fifth; the full answer home.
        int lastOn = 0;
        for (int k = 0; k < n; ++k)
            if (onset[k]) lastOn = k;
        if (lastOn > 0) {
            int before = 0;
            for (int k = 0; k < lastOn; ++k)
                if (onset[k]) before = k;
            callDeg[lastOn] = land(endA < 0.6 ? 1 : 3, callDeg[before], lo, hi);
            halfDeg[lastOn] = land(4, halfDeg[before], lo, hi);
            fullDeg[lastOn] = land(0, fullDeg[before], lo - 7, hi);
        } else {
            fullDeg[0] = 0;
            halfDeg[0] = land(4, 0, lo, hi);
        }
        // How long each note holds, in steps: Legato of the way to the next.
        for (int k = 0; k < kPhraseSteps; ++k) {
            holds[k] = 0;
            if (!onset[k]) continue;
            int next = n;
            for (int j = k + 1; j < n; ++j)
                if (onset[j]) {
                    next = j;
                    break;
                }
            holds[k] = max(1, roundToInt(hold * (next - k)));
        }
        written = n;
    }

    void startOver() {
        rng.seed(salt);
        compose();
        pos = -1;
        lastSong = -1;
        wanted = false;
        rounds = 0;
        open = false;
        remain = 0;
        note = 0;
        weight = 0.0;
    }

    void step(int i) {
        const int n = steps();
        const int total = n * sections();
        pos = pos + 1 >= total ? 0 : pos + 1;
        if (pos == 0) {
            // One draw each time round, whatever the knobs.
            const double u = rng.next();
            if (rounds > 0 && (wanted || written != n || u < clamp(renew.at(i), 0.0, 1.0)))
                compose();
            wanted = false;
            ++rounds;
        }
        const int s = idiv(pos, n);
        const int k = pos - s * n;
        if (k == 0) startPulse.fire();
        const bool was = open;
        if (remain > 0) {
            --remain;
            if (remain == 0) open = false;
        }
        if (onset[k]) {
            const int kind = kindOf(s);
            const int d = kind == 0 ? callDeg[k] : (kind == 1 ? halfDeg[k] : fullDeg[k]);
            note = clamp(key.index(), 0, 11) + noteOf(d) + 12 * clamp(octave.index(), -2, 2);
            weight = pos == 0 ? 1.0 : (k % 16 == 0 ? 0.85 : (k % 4 == 0 ? 0.7 : 0.55));
            if (was) gap = max(1, roundToInt(0.002 * sampleRate()));
            open = true;
            remain = holds[k];
        }
    }

    void process(int frames) {
        if (!started) {
            started = true;
            startOver();
        }
        const Transport t = transport();
        const bool internal = !clock.connected();
        if (internal && t.playing && !wasPlaying) startOver();
        if (internal && !t.playing) open = false;
        wasPlaying = t.playing;
        const double beatsPerFrame = t.tempo / 60.0 / sampleRate();
        const int total = steps() * sections();
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            if (writeEdge.rose(write[0][i])) wanted = true;
            if (internal) {
                if (t.playing) {
                    // The song's sixteenths: the form lines up with its bars.
                    const int song = floorToInt((t.beats + i * beatsPerFrame) * 4.0 + 1e-9);
                    if (song != lastSong) {
                        if (lastSong < 0 || song != lastSong + 1) pos = wrap(song - 1, total);
                        lastSong = song;
                        step(i);
                    }
                }
            } else if (clockEdge.rose(clock[0][i])) {
                step(i);
            }
            const bool sounding = open && gap == 0;
            if (gap > 0) --gap;
            pitch[0][i] = note / 12.0;
            gate[0][i] = sounding ? 1.0 : 0.0;
            accent[0][i] = weight;
            const int s = pos < 0 ? 0 : idiv(pos, steps());
            answer[0][i] = pos >= 0 && kindOf(s) != 0 ? 1.0 : 0.0;
            start[0][i] = startPulse.next();
        }
        for (int k = 0; k < kPhraseSteps; ++k) {
            shownCall[k] = callDeg[k];
            shownHalf[k] = halfDeg[k];
            shownFull[k] = fullDeg[k];
            shownHold[k] = onset[k] ? holds[k] : 0;
        }
        shownPos.set(pos);
        shownSteps.set(steps());
        shownForm.set(form.index());
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 440);
        p.display(8, 28, 204, 96);
        p.choice(key, 8, 132, 64, 18);
        p.choice(mode, 78, 132, 134, 18);
        p.choice(form, 8, 156, 134, 18);
        p.choice(length, 148, 156, 64, 18);
        p.knob(density, 30, 210);
        p.knob(range, 83, 210);
        p.knob(contrast, 137, 210);
        p.knob(renew, 190, 210);
        p.knob(legato, 30, 266, 28);
        p.knob(octave, 83, 266, 28);
        p.input(clock, 137, 268);
        p.input(reset, 190, 268);
        p.input(write, 30, 330);
        p.output(pitch, 83, 330);
        p.output(gate, 137, 330);
        p.output(accent, 190, 330);
        p.output(answer, 83, 392);
        p.output(start, 137, 392);
    }

    // The form as a roll: a block a phrase, answers tinted, notes as bars at
    // their degree, the step playing marked.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 96);
        const int n = clamp(shownSteps.get(), 16, 32);
        const int f = clamp(shownForm.get(), 0, 2);
        const int count = f == 1 ? 3 : (f == 2 ? 4 : 2);
        // The rows the notes use, with one spare above and below.
        int lo = 0;
        int hi = 0;
        for (int k = 0; k < n; ++k) {
            if (shownHold[k] <= 0) continue;
            lo = min(lo, min(shownCall[k], min(shownHalf[k], shownFull[k])));
            hi = max(hi, max(shownCall[k], max(shownHalf[k], shownFull[k])));
        }
        lo -= 1;
        hi += 1;
        const double x0 = 12.0;
        const double w = 196.0 / count;
        const double y0 = 34.0;
        const double h = 84.0;
        const double cell = (w - 4.0) / n;
        const double row = h / (hi - lo + 1);
        const int playing = shownPos.get();
        for (int s = 0; s < count; ++s) {
            int kind = 0;
            if (f == 1) kind = s == 2 ? 2 : 0;
            if (f == 2) kind = s == 1 ? 1 : (s == 3 ? 2 : 0);
            if (f == 0) kind = s == 1 ? 2 : 0;
            const double bx = x0 + s * w;
            g.fillStyle(kind == 0 ? Theme::recess : Theme::raised);
            g.beginPath();
            g.roundRect(bx, y0, w - 4.0, h, 3);
            g.fill();
            // Home, a line.
            g.strokeStyle(Theme::line);
            g.lineWidth(1.0);
            g.beginPath();
            g.moveTo(bx + 2.0, y0 + h - (0 - lo + 0.5) * row);
            g.lineTo(bx + w - 6.0, y0 + h - (0 - lo + 0.5) * row);
            g.stroke();
            g.fillStyle(kind == 0 ? Theme::accent : Theme::text);
            for (int k = 0; k < n; ++k) {
                const int held = shownHold[k];
                if (held <= 0) continue;
                const int d = kind == 0 ? shownCall[k] : (kind == 1 ? shownHalf[k] : shownFull[k]);
                const double bar = min(4.0, row - 1.0);
                const double y = y0 + h - (clamp(d, lo, hi) - lo + 0.5) * row - bar * 0.5;
                const bool now = playing >= s * n + k && playing < s * n + k + held;
                g.globalAlpha(now ? 1.0 : 0.7);
                g.beginPath();
                g.roundRect(bx + k * cell + 0.5, y, max(1.5, held * cell - 1.0), bar, 1);
                g.fill();
            }
            g.globalAlpha(1.0);
        }
        if (playing >= 0) {
            const int s = idiv(playing, n);
            const double x = x0 + s * w + (playing - s * n + 0.5) * cell;
            g.strokeStyle(Theme::text);
            g.lineWidth(1.0);
            g.globalAlpha(0.5);
            g.beginPath();
            g.moveTo(x, y0);
            g.lineTo(x, y0 + h);
            g.stroke();
            g.globalAlpha(1.0);
        }
    }
};
