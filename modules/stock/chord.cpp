// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// A chord from one note: pitch in, a poly cable of chord tones out, one
// channel a voice, lowest first. Twenty-two chord types, or chords built in
// a key: In Major and In Minor stack the key's thirds on the note played
// (D in C major gives D minor, G gives G major), so a melody into it plays
// the key's own harmony. Voices sets how many notes (a triad at four
// voices doubles its root an octave up; in a key, more voices stack more
// thirds: sevenths, ninths). Inversion lifts the lowest notes an octave
// (or drops the highest), and Voicing spreads them: close, drop 2, open or
// wide. Gate is the gate in for every voice; Strum staggers it across the
// voices by Strum ms, up, down or alternating, for a strummed chord.

constexpr int kVoices = 8;
constexpr int kInMajor = 22;

int chordSize(int t) {
    if (t <= 5 || t == 18) return 3;
    if (t >= 15 && t <= 17) return 5;
    return 4;
}

// Tone k (below chordSize) of chord type t, in semitones above its root.
int chordTone(int t, int k) {
    switch (t) {
        case 0:  // major
            return k == 0 ? 0 : (k == 1 ? 4 : 7);
        case 1:  // minor
            return k == 0 ? 0 : (k == 1 ? 3 : 7);
        case 2:  // diminished
            return k == 0 ? 0 : (k == 1 ? 3 : 6);
        case 3:  // augmented
            return k == 0 ? 0 : (k == 1 ? 4 : 8);
        case 4:  // sus2
            return k == 0 ? 0 : (k == 1 ? 2 : 7);
        case 5:  // sus4
            return k == 0 ? 0 : (k == 1 ? 5 : 7);
        case 6:  // major 7
            return k == 0 ? 0 : (k == 1 ? 4 : (k == 2 ? 7 : 11));
        case 7:  // minor 7
            return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 7 : 10));
        case 8:  // dominant 7
            return k == 0 ? 0 : (k == 1 ? 4 : (k == 2 ? 7 : 10));
        case 9:  // half-diminished
            return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 6 : 10));
        case 10:  // diminished 7
            return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 6 : 9));
        case 11:  // major 6
            return k == 0 ? 0 : (k == 1 ? 4 : (k == 2 ? 7 : 9));
        case 12:  // minor 6
            return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 7 : 9));
        case 13:  // add 9
            return k == 0 ? 0 : (k == 1 ? 4 : (k == 2 ? 7 : 14));
        case 14:  // minor add 9
            return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 7 : 14));
        case 15:  // major 9
            return k == 0 ? 0 : (k == 1 ? 4 : (k == 2 ? 7 : (k == 3 ? 11 : 14)));
        case 16:  // minor 9
            return k == 0 ? 0 : (k == 1 ? 3 : (k == 2 ? 7 : (k == 3 ? 10 : 14)));
        case 17:  // dominant 9
            return k == 0 ? 0 : (k == 1 ? 4 : (k == 2 ? 7 : (k == 3 ? 10 : 14)));
        case 18:  // power
            return k == 0 ? 0 : (k == 1 ? 7 : 12);
        case 19:  // cluster
            return k == 0 ? 0 : (k == 1 ? 2 : (k == 2 ? 3 : 5));
        case 20:  // quartal
            return 5 * k;
        default:  // quintal
            return 7 * k;
    }
}

// Degree i of a major (or natural minor) scale from its root, in semitones,
// any octave.
int scaleStep(int i, bool isMinor) {
    const int o = i >= 0 ? i / 7 : -((-i + 6) / 7);
    const int d = i - 7 * o;
    int s = 0;
    if (d == 1) s = 2;
    if (d == 2) s = isMinor ? 3 : 4;
    if (d == 3) s = 5;
    if (d == 4) s = 7;
    if (d == 5) s = isMinor ? 8 : 9;
    if (d == 6) s = isMinor ? 10 : 11;
    return 12 * o + s;
}

struct Chord : Module {
    static constexpr ModuleInfo info = {
        .name = "Chord",
        .id = "com.suppleaudio.rack.chord",
        .vendor = "Supple Audio",
        .description = "A note to a poly chord: types or a key's own, inversions, voicings, strum.",
        .tags = {"chord", "pitch", "poly"}};

    Param type =
        choice("Type", {"Major",        "Minor",   "Diminished", "Augmented",  "Sus2",
                        "Sus4",         "Major 7", "Minor 7",    "Dominant 7", "Half-Diminished",
                        "Diminished 7", "Major 6", "Minor 6",    "Add 9",      "Minor Add 9",
                        "Major 9",      "Minor 9", "Dominant 9", "Power",      "Cluster",
                        "Quartal",      "Quintal", "In Major",   "In Minor"},
               0)
            .keys({"major",  "minor",   "diminished", "augmented",       "sus2",        "sus4",
                   "major7", "minor7",  "dominant7",  "half_diminished", "diminished7", "major6",
                   "minor6", "add9",    "minor_add9", "major9",          "minor9",      "dominant9",
                   "power",  "cluster", "quartal",    "quintal",         "in_major",    "in_minor"})
            .in("Chord")
            .help(
                "The chord on the note: a fixed type, or In Major or In Minor to "
                "stack the key's thirds on it (the note taken down into the key).");
    Param key = choice("Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0)
                    .keys({"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a",
                           "a_sharp", "b"})
                    .activeWhen(type, {"in_major", "in_minor"})
                    .in("Chord")
                    .help("The key's first note, for In Major and In Minor.");
    Param voices = integer("Voices", 1, 8, 4)
                       .in("Chord")
                       .help(
                           "How many notes it sends: past the chord's own, its notes again an "
                           "octave up (in a key, "
                           "more thirds).");
    Param inversion =
        integer("Inversion", -4, 4, 0)
            .in("Voicing")
            .help(
                "Each step up lifts the lowest note an octave; each step down drops the highest.");
    Param voicing = choice("Voicing", {"Close", "Drop 2", "Open", "Wide"}, 0)
                        .keys({"close", "drop2", "open", "wide"})
                        .in("Voicing")
                        .help(
                            "How far apart the notes sit: close, the second highest dropped "
                            "an octave, every other note raised one, or spread over octaves.");
    Param strum = milliseconds("Strum", 0, 200, 25)
                      .in("Strum")
                      .help("The time between one voice's gate and the next on Strum.");
    Param direction = choice("Direction", {"Up", "Down", "Alternate"}, 0)
                          .in("Strum")
                          .help(
                              "Which voice the strum starts from: the lowest, the highest, or "
                              "each in turn.");

    Input pitch = pitchIn("Pitch").help("The chord's note, in octaves from C4.");
    Input gate = gateIn("Gate").help("The chord's gate, for Gate and Strum.");

    Output out = pitchOut("Out").help("The chord's notes, one channel a voice, lowest first.");
    Output gates = gateOut("Gates").help("The gate in, on every voice at once.");
    Output strummed = gateOut("Strummed").help("The gate in, voice after voice, Strum ms apart.");

    Array<double, 8> tones;
    Buffer<double> history;  // the gate in, for the strum's delays
    int writeAt = 0;
    bool gateHigh = false;
    bool downward = false;  // this strum's direction
    int strums = 0;

    void factoryPatches(Patches& p) {
        p.add("Triad").set(type, "major").set(voices, 3);
        p.add("Minor 7 Open").set(type, "minor7").set(voicing, "open");
        p.add("Key of C").set(type, "in_major").set(voices, 4);
        p.add("Power Strum").set(type, "power").set(voices, 4).set(strum, 15);
        p.add("Quartal Pad").set(type, "quartal").set(voices, 5).set(voicing, "wide");
        p.add("Slow Harp").set(type, "major9").set(voices, 6).set(strum, 120);
    }

    void prepare() {
        history.resize(roundToInt(sampleRate() * 0.2 * kVoices) + 2);
        for (int k = 0; k < history.size(); ++k) history[k] = 0.0;
    }

    // The chord on `note` (semitones from C4), lowest first, into tones.
    void build(double note, int t, int n, int inv, int v) {
        const int root = roundToInt(note);
        if (t >= kInMajor) {
            const bool isMinor = t == kInMajor + 1;
            const int k0 = clamp(key.index(), 0, 11);
            const int rel = root - k0;
            const int octave = rel >= 0 ? rel / 12 : -((-rel + 11) / 12);
            const int pc = rel - 12 * octave;
            int d = 0;
            for (int i = 0; i < 7; ++i)
                if (scaleStep(i, isMinor) <= pc) d = i;
            const int degree = 7 * octave + d;
            for (int k = 0; k < n; ++k) tones[k] = k0 + scaleStep(degree + 2 * k, isMinor);
        } else {
            const int size = chordSize(t);
            for (int k = 0; k < n; ++k)
                tones[k] = root + chordTone(t, imod(k, size)) + 12 * idiv(k, size);
        }
        // Inversions: the lowest up an octave, or the highest down.
        for (int s = 0; s < abs(inv); ++s) {
            int at = 0;
            for (int k = 1; k < n; ++k)
                if (inv > 0 ? tones[k] < tones[at] : tones[k] > tones[at]) at = k;
            tones[at] += inv > 0 ? 12.0 : -12.0;
        }
        sortTones(n);
        if (v == 1 && n >= 3) tones[n - 2] -= 12.0;
        if (v == 2)
            for (int k = 1; k < n; k += 2) tones[k] += 12.0;
        if (v == 3)
            for (int k = 1; k < n; ++k) tones[k] += 12.0 * idiv(k + 1, 2);
        sortTones(n);
    }

    void sortTones(int n) {
        for (int k = 1; k < n; ++k)
            for (int j = k; j > 0 && tones[j] < tones[j - 1]; --j) {
                const double x = tones[j];
                tones[j] = tones[j - 1];
                tones[j - 1] = x;
            }
    }

    void process(int frames) {
        const int n = clamp(voices.index(), 1, kVoices);
        out.setChannels(n);
        gates.setChannels(n);
        strummed.setChannels(n);
        const int t = clamp(type.index(), 0, kInMajor + 1);
        const int inv = clamp(inversion.index(), -4, 4);
        const int v = clamp(voicing.index(), 0, 3);
        const int size = history.size();
        for (int i = 0; i < frames; ++i) {
            build(pitch[0][i] * 12.0, t, n, inv, v);
            const double g = gate[0][i];
            if (!gateHigh && g >= 0.5) {
                gateHigh = true;
                const int d = direction.index();
                downward = d == 1 || (d == 2 && imod(strums, 2) == 1);
                ++strums;
            } else if (gateHigh && g < 0.25) {
                gateHigh = false;
            }
            history[writeAt] = gateHigh ? 1.0 : 0.0;
            const int spacing = roundToInt(strum.at(i) * 0.001 * sampleRate());
            for (int k = 0; k < n; ++k) {
                out[k][i] = tones[k] / 12.0;
                gates[k][i] = gateHigh ? 1.0 : 0.0;
                const int order = downward ? n - 1 - k : k;
                const int delay = min(size - 1, order * spacing);
                strummed[k][i] = history[wrap(writeAt - delay, size)];
            }
            writeAt = wrap(writeAt + 1, size);
        }
    }
};
