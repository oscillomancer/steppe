// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A second melody against a lead, note against note, by the old rules of
// two voices: each lead note gets a note of the key below it (or above) a
// consonance away, thirds and sixths most, fifths and octaves less, never
// a dissonance. Motion is how much it moves against the lead, down as it
// goes up; it moves mostly by step, never in parallel fifths or octaves,
// and seldom into one by similar motion; on the key's first note it likes
// to meet the lead at the octave, as a cadence does. Hold is how often it
// keeps its note through the lead's next while that is still consonant,
// so the two lines get rhythms of their own. Vary lets it take a less
// than best note now and then, from the seed; Reset starts those choices
// over. Its gate follows the lead's, closing 5 ms after it so a held note
// ties. With nothing in Gate, each new pitch is a new note.

constexpr int kCounterShown = 16;

// The seven notes of each mode, in semitones above the key, packed a hex
// digit a degree from the lowest.
int counterMode(int m) {
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

// How good an interval of `ic` semitones (0..11) is between two voices:
// 0 for a dissonance.
double counterConsonance(int ic) {
    switch (ic) {
        case 3:
        case 4:
        case 8:
        case 9:
            return 3.0;  // thirds and sixths
        case 7:
            return 1.5;
        case 0:
            return 1.0;
        default:
            return 0.0;
    }
}

struct Counter : Module {
    static constexpr ModuleInfo info = {
        .name = "Counter",
        .id = "com.suppleaudio.rack.counter",
        .vendor = "Supple Audio",
        .description = "A counter-melody against a lead, by the rules of two-voice counterpoint.",
        .tags = {"generative", "pitch", "harmony", "sequencer"}};

    Param key = choice("Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
                    .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                           "a_sharp", "b"})
                    .help("The key's first note.");
    Param mode =
        choice("Mode",
               {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Harmonic Minor"},
               0)
            .keys(
                {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "harmonic_minor"})
            .help("The scale the counter-melody is drawn from.");
    Param place = choice("Place", {"Below", "Above"}, 0)
                      .keys({"below", "above"})
                      .help("Whether it sounds below the lead or above, a third to a tenth away.");
    Param motion = percent("Motion", 60)
                       .help(
                           "How much it moves against the lead: down as the lead goes up, and up "
                           "as it goes down.");
    Param hold = percent("Hold", 20)
                     .help(
                         "How often it keeps its note through the lead's next, while that is still "
                         "consonant.");
    Param vary =
        percent("Vary", 20).help("How often it takes a less than best note, from the seed.");

    Input lead = pitchIn("Lead").help("The lead's pitch.");
    Input lead_gate = gateIn("Gate").help(
        "The lead's gate: each rising edge is a note; with nothing, each new pitch.");
    Input reset = gateIn("Reset").help("A rising edge starts the same choices over.");

    Output pitch = pitchOut("Pitch").help("The counter-melody.");
    Output gate = gateOut("Gate").help("High with the lead's gate, held through a held note.");
    Output interval =
        cvOut("Interval").help("The counter-melody less the lead, in octaves: below it, negative.");
    Output moved = gateOut("Moved").help("A 1 ms trigger as it moves to a new note.");

    Random rng;
    Trigger resetEdge;
    Pulse movedPulse;
    int salt = 0;
    bool started = false;
    bool leadHigh = false;
    bool open = false;
    int closing = 0;   // frames left before the gate follows the lead down
    int gap = 0;       // frames left of a retrigger's gap
    int last = -1000;  // the counter's note, semitones from C4
    int lastLead = -1000;
    int lastIc = -1;
    int heard = -1000;  // the lead's rounded pitch, with nothing in Gate

    SharedArray<int, kCounterShown> shownLead;
    SharedArray<int, kCounterShown> shownOwn;
    Shared<int> shownWritten;
    int written = 0;

    void factoryPatches(Patches& p) {
        p.add("Bass Line");
        p.add("Descant").set(place, "above").set(motion, 70);
        p.add("Strict").set(hold, 0).set(vary, 0).set(motion, 80);
        p.add("Loose").set(hold, 40).set(vary, 50).set(motion, 40);
    }

    void prepare() {
        salt = seed();
        started = false;
    }

    void startOver() {
        rng.seed(salt);
        last = -1000;
        lastLead = -1000;
        lastIc = -1;
        written = 0;
        for (int k = 0; k < kCounterShown; ++k) {
            shownLead[k] = -1000;
            shownOwn[k] = -1000;
        }
    }

    bool inKey(int n) const {
        const int packed = counterMode(mode.index());
        const int pc = wrap(n - clamp(key.index(), 0, 11), 12);
        for (int d = 0; d < 7; ++d)
            if (imod(shr(packed, 4 * d), 16) == pc) return true;
        return false;
    }

    // How good note c is against lead note l, coming from the last pair.
    double score(int c, int l, double u) const {
        const int dist = abs(l - c);
        const int ic = imod(dist, 12);
        const double cons = counterConsonance(ic);
        if (cons <= 0.0) return -1000.0;
        double s = cons;
        if (last > -1000 && lastLead > -1000) {
            const int ours = c - last;
            const int theirs = l - lastLead;
            const bool perfect = ic == 0 || ic == 7;
            if (abs(ours) > 12) return -1000.0;
            // Parallel fifths and octaves never; into one by similar motion seldom.
            if (perfect && ic == lastIc && ours != 0 && theirs != 0) return -1000.0;
            if (perfect && ours != 0 && theirs != 0 && (ours > 0) == (theirs > 0)) s -= 2.0;
            const double against = clamp(motion.value(), 0.0, 1.0);
            if (ours != 0 && theirs != 0 && (ours > 0) != (theirs > 0)) s += 4.0 * against;
            if (ours != 0 && theirs != 0 && (ours > 0) == (theirs > 0)) s += 1.0 - 2.0 * against;
            if (ours == 0) s += 0.5;
            const int leap = abs(ours);
            if (leap >= 1 && leap <= 2) s += 1.5;
            if (leap >= 3 && leap <= 4) s += 0.5;
            if (leap > 7) s -= 2.0;
            // Not the same third or sixth over and over.
            if (ic == lastIc && !perfect) s -= 1.0;
        }
        // The key's first note in the lead: meet it at the octave.
        if (wrap(l - clamp(key.index(), 0, 11), 12) == 0 && ic == 0) s += 2.5;
        return s + 4.0 * clamp(vary.value(), 0.0, 1.0) * u;
    }

    // A new lead note: choose the counter's. Sixteen draws, whatever happens.
    void note(int l) {
        Array<double, 14> draws;
        for (int k = 0; k < 14; ++k) draws[k] = rng.next();
        const double keepU = rng.next();
        rng.next();
        const bool above = place.index() == 1;
        // Hold: keep the last note if it is still consonant and in range.
        if (last > -1000 && keepU < clamp(hold.value(), 0.0, 1.0)) {
            const int dist = above ? last - l : l - last;
            if (dist >= 3 && dist <= 16 && counterConsonance(imod(dist, 12)) > 0.0 &&
                !(imod(dist, 12) == lastIc && (lastIc == 0 || lastIc == 7))) {
                lastLead = l;
                lastIc = imod(dist, 12);
                remember(l);
                return;
            }
        }
        int best = above ? l + 3 : l - 3;
        double bestScore = -1e9;
        for (int k = 0; k < 14; ++k) {
            const int c = above ? l + 3 + k : l - 3 - k;
            if (!inKey(c)) continue;
            const double s = score(c, l, draws[k]);
            if (s > bestScore) {
                bestScore = s;
                best = c;
            }
        }
        if (bestScore <= -1000.0) best = above ? l + 12 : l - 12;  // nothing fits: the octave
        const bool changedNote = best != last;
        lastIc = imod(abs(l - best), 12);
        last = best;
        lastLead = l;
        if (changedNote) {
            movedPulse.fire();
            if (open) gap = max(1, roundToInt(0.002 * sampleRate()));
        }
        remember(l);
    }

    void remember(int l) {
        const int k = imod(written, kCounterShown);
        shownLead[k] = l;
        shownOwn[k] = last;
        ++written;
    }

    void process(int frames) {
        if (!started) {
            started = true;
            startOver();
        }
        const bool gated = lead_gate.connected();
        const int tail = max(1, roundToInt(0.005 * sampleRate()));
        for (int i = 0; i < frames; ++i) {
            if (resetEdge.rose(reset[0][i])) startOver();
            const int l = roundToInt(clamp(lead[0][i], -10.0, 10.0) * 12.0);
            if (gated) {
                const double g = lead_gate[0][i];
                if (!leadHigh && g >= 0.5) {
                    leadHigh = true;
                    note(l);
                    open = true;
                    closing = 0;
                } else if (leadHigh && g < 0.25) {
                    leadHigh = false;
                    closing = tail;
                }
                if (!leadHigh && closing > 0) {
                    --closing;
                    if (closing == 0) open = false;
                }
            } else {
                if (l != heard) {
                    heard = l;
                    note(l);
                }
                open = true;
            }
            const bool sounding = open && gap == 0;
            if (gap > 0) --gap;
            pitch[0][i] = last > -1000 ? last / 12.0 : 0.0;
            gate[0][i] = sounding && last > -1000 ? 1.0 : 0.0;
            interval[0][i] = last > -1000 && lastLead > -1000 ? (last - lastLead) / 12.0 : 0.0;
            moved[0][i] = movedPulse.next();
        }
        shownWritten.set(written);
    }

    // --- the face ------------------------------------------------------------------------

    void panel(Panel& p) {
        p.size(220, 352);
        p.display(8, 28, 204, 96);
        p.choice(key, 8, 132, 64, 18);
        p.choice(mode, 78, 132, 134, 18);
        p.choice(place, 8, 156, 204, 18);
        p.knob(motion, 40, 208);
        p.knob(hold, 110, 208);
        p.knob(vary, 180, 208);
        p.input(lead, 40, 266);
        p.input(lead_gate, 110, 266);
        p.input(reset, 180, 266);
        p.output(pitch, 30, 320);
        p.output(gate, 83, 320);
        p.output(interval, 137, 320);
        p.output(moved, 190, 320);
    }

    // The last sixteen notes of both lines, oldest on the left: the lead
    // pale, the counter bright, a rung between each pair.
    void draw(Canvas& g) {
        g.well(8, 28, 204, 96);
        const int n = shownWritten.get();
        if (n == 0) return;
        int lo = 1000;
        int hi = -1000;
        for (int k = 0; k < kCounterShown; ++k) {
            if (shownLead[k] <= -1000) continue;
            lo = min(lo, min(shownLead[k], shownOwn[k]));
            hi = max(hi, max(shownLead[k], shownOwn[k]));
        }
        lo -= 2;
        hi += 2;
        const double x0 = 14.0;
        const double cell = 192.0 / kCounterShown;
        const double y0 = 34.0;
        const double h = 84.0;
        const double row = h / max(1, hi - lo);
        const int count = min(n, kCounterShown);
        for (int j = 0; j < count; ++j) {
            const int k = imod(n - count + j, kCounterShown);
            const double x = x0 + j * cell;
            const double yl = y0 + h - (shownLead[k] - lo) * row;
            const double yc = y0 + h - (shownOwn[k] - lo) * row;
            g.strokeStyle(Theme::line);
            g.lineWidth(1.0);
            g.beginPath();
            g.moveTo(x + cell * 0.5, yl);
            g.lineTo(x + cell * 0.5, yc);
            g.stroke();
            g.fillStyle(Theme::text);
            g.globalAlpha(0.6);
            g.beginPath();
            g.roundRect(x + 1.0, yl - 2.0, cell - 2.0, 4.0, 1);
            g.fill();
            g.globalAlpha(j == count - 1 ? 1.0 : 0.8);
            g.fillStyle(Theme::accent);
            g.beginPath();
            g.roundRect(x + 1.0, yc - 2.0, cell - 2.0, 4.0, 1);
            g.fill();
            g.globalAlpha(1.0);
        }
    }
};
