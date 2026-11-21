#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The song again, after batch 3: one set of harmony, written once, heard bar by bar.

`mcp_journey_song.py` plays one song whose scenes were written to match its chords. This
journey writes a different one the way ADR-0098 means it to be used: a 28-bar lead sheet
with three key changes (A minor, C major, F# minor, A minor) and two borrowed chords (Bb
and Fm in C major) is written ONCE on the context track; the clips keep the key and chords
they were written against (`context_at`, `context`) and say how they follow
(`follow: chords | key | none`); `arrangement.build` and `scene.insert` lay them, and the
journey checks that what it hears is what the lead sheet says:

* note level: `midi.notes` of every laid item against the chord at its beat, for each follow
  mode (the same Verse laid under all seven four-bar spans of the song);
* audible level: the bass's pitch per bar (`analyze.track`, the f0 of a monophonic line)
  and the keys' pitch classes per bar (a Goertzel chroma of the rendered stem) against the
  chord, so the measurement can tell the three modes apart;
* one clip written in the wrong key with `follow: none`, which warns and sounds wrong;
* modulation by a parameter's name, CLAP id and key on a plug-in without keys (the host
  fixture `Test Raw Gain`) and one with them (`Test Gain`), a synced LFO and a macro,
  measured on renders;
* a launch under a master playrate, a performance captured and committed with its sections;
* a section captured back as a scene whose clip keeps the harmony it sat under;
* a chord changed after the song was laid: the laid items stay as they were, and one
  `batch` (remove the items and the section, `scene.insert` again) brings them in line.

Nothing decides by a wall clock: the live stage polls the transport and `launch.state`; the
timeout only turns a hang into a failure. Measurement uses the standard library only.

  mcp_journey_song_2.py <engine> <plugin-dir> <test-fixtures.clap> [--transcript FILE]
"""

import argparse
import math
import re
import tempfile
import time
from pathlib import Path

from mcp_arrangement import Client
from mcp_journey_launcher import TIMEOUT
from mcp_journey_song import Song, Wav

PLUGIN = "com.suppleaudio."
FIXTURE = "com.suppleaudio.test."

BPM = 160
BEAT_S = 60.0 / BPM
BAR_S = 4 * BEAT_S

# The context track: A minor, C major (with a borrowed Bb and Fm), F# minor, A minor.
LEAD_SHEET = """1: key A minor {Am}:w
2: {F}:w
3: {G}:w
4: {Am}:w
5: {Am}:w
6: {Dm}:w
7: {E7}:w
8: {Am}:w
9: key C major {C}:w
10: {Am}:w
11: {F}:w
12: {G}:w
13: {C}:w
14: {Em}:w
15: {Bb}:w
16: {Fm}:h {G}:h
17: key F# minor {F#m}:w
18: {D}:w
19: {A}:w
20: {E}:w
21: {F#m}:w
22: {Bm}:w
23: {C#7}:w
24: {F#m}:w
25: key A minor {Am}:w
26: {F}:w
27: {E7}:w
28: {Am}:w
"""

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
LETTER = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
QUALITY = {"": (0, 4, 7), "m": (0, 3, 7), "7": (0, 4, 7, 10)}
SCALE = {"major": (0, 2, 4, 5, 7, 9, 11), "minor": (0, 2, 3, 5, 7, 8, 10)}


def pitch_class(name):
    value = LETTER[name[0]]
    for mark in name[1:]:
        value += {"#": 1, "b": -1}[mark]
    return value % 12


def chord_tones(symbol):
    """(root pitch class, set of chord-tone pitch classes) of a symbol such as F#m or E7."""
    root, quality = re.match(r"([A-G][#b]?)(.*)$", symbol).groups()
    return pitch_class(root), {(pitch_class(root) + i) % 12 for i in QUALITY[quality]}


def scale_tones(key):
    tonic, scale = key.split()
    return {(pitch_class(tonic) + i) % 12 for i in SCALE[scale]}


def lead_sheet_points(text):
    """The lead sheet as (beat, 'key' | 'chord', what), four beats a bar."""
    points = []
    for line in text.strip().splitlines():
        bar, body = re.match(r"(\d+):\s*(.*)", line).groups()
        beat = (int(bar) - 1) * 4.0
        for token in re.findall(r"key [A-G][#b]? \w+|\{[^}]*\}:\w+", body):
            if token.startswith("key"):
                points.append((beat, "key", token[4:]))
            else:
                symbol, duration = re.match(r"\{([^}]*)\}:(\w+)", token).groups()
                points.append((beat, "chord", symbol))
                beat += {"w": 4, "h": 2}[duration]
    return points


POINTS = lead_sheet_points(LEAD_SHEET)


def at(kind, beat):
    held = None
    for start, what_kind, what in POINTS:
        if what_kind == kind and start <= beat + 1e-9:
            held = what
    return held


# Bars whose chord was changed after the lead sheet was written (bar -> symbol).
CHANGED = {}


def chord_at(bar, beat_in_bar=0.0):
    if bar in CHANGED:
        return CHANGED[bar]
    return at("chord", (bar - 1) * 4.0 + beat_in_bar)


# --- the music --------------------------------------------------------------------------

def drums(kind):
    kick, snare, hat = {
        "verse": ("x...x...x...x...", "....x.......x...", "x.x.x.x.x.x.x.x."),
        "hook": ("x..xx...x..xx...", "....x.......x..x", "xxxxxxxxxxxxxxxx"),
        "bridge": ("x.......x.......", "........x.......", "..x...x...x...x."),
    }[kind]
    return (f"notation 1\nmeter 4/4\nvoice drums ch 10 grid s\n1: kick  {kick}\n"
            f"1: snare {snare}\n1: hat   {hat}\n2-4 = 1\n")


def bass(roots):
    lines = ["notation 1", "meter 4/4", "voice bass ch 1 vel 100"]
    for bar, root in enumerate(roots, 1):
        lines.append(f"{bar}: {root}:e {root}:e r:e {root}:e {root}:e r:e {root}:e {root}:e")
    return "\n".join(lines) + "\n"


def keys(chords):
    lines = ["notation 1", "meter 4/4", "voice keys ch 1 vel 80 octave 4"]
    lines += [f"{bar}: {{{chord}}}:w" for bar, chord in enumerate(chords, 1)]
    return "\n".join(lines) + "\n"


def lead(bars):
    lines = ["notation 1", "meter 4/4", "voice lead ch 1 vel 90 octave 5"]
    lines += [f"{bar}: {notes}" for bar, notes in enumerate(bars, 1)]
    return "\n".join(lines) + "\n"


VERSE_LEAD = ["E5:q C5:q D5:q E5:q", "F5:q A5:q C6:q A5:q", "D5:q B4:q G4:q B4:q", "A4:h r:h"]
HOOK_LEAD = ["G5:q E5:q C5:q E5:q", "A5:q E5:q C5:q E5:q", "C6:q A5:q F5:q A5:q", "G5:h D5:h"]

KEYS_PATCH = {"params": {"osc1.wave": "saw", "filter1.type": "ladder", "filter1.cutoff": "900 Hz",
                         "amp.attack": "20 ms", "amp.release": "300 ms"}}
BASS_PATCH = {"params": {"osc1.wave": "saw", "filter1.type": "ladder", "filter1.cutoff": "350 Hz",
                         "amp.attack": "5 ms", "amp.release": "120 ms"}}
LEAD_PATCH = {"params": {"osc1.wave": "square", "filter1.type": "svf", "filter1.cutoff": "2.5 kHz",
                         "amp.attack": "5 ms", "amp.release": "150 ms"}}

# The tracks: Drums tr_1, Bass tr_2, Keys tr_3, Lead tr_4, Verb tr_5. Scenes: Verse sc_1,
# Hook sc_2, Bridge sc_3. A scene is four bars; the song is seven of them.
SONG = [("sc_1", 4), ("sc_1", 4), ("sc_2", 4), ("sc_2", 4), ("sc_3", 4), ("sc_3", 4),
        ("sc_1", 4)]
TRACK = {"tr_1": "Drums", "tr_2": "Bass", "tr_3": "Keys", "tr_4": "Lead"}


def build(j):
    j.exec("tempo.set", {"points": [{"beat": 0, "bpm": BPM}]}, "160 BPM: a bar is 1.5 s.")
    for name, kind in (("Drums", "channel"), ("Bass", "channel"), ("Keys", "channel"),
                       ("Lead", "channel"), ("Verb", "buss")):
        j.exec("track.add", {"name": name, "type": kind}, f"Add the {name} track.")
    for track, plugin in (("tr_1", "drumlin"), ("tr_2", "marram"), ("tr_3", "marram"),
                          ("tr_4", "marram"), ("tr_5", "ghyll")):
        j.exec("fx.add", {"track": track, "plugin": PLUGIN + plugin})
    for fx, patch in (("fx_2", BASS_PATCH), ("fx_3", KEYS_PATCH), ("fx_4", LEAD_PATCH)):
        j.exec("fx.patch.merge", {"fx": fx, "patch": patch}, "A patch for the synth.")
    j.exec("route.add", {"from": "tr_3", "to": "tr_5"}, "Send the keys to the reverb buss.")
    for track, volume in (("tr_1", -8), ("tr_2", -6), ("tr_3", -11), ("tr_4", -12)):
        j.exec("track.set", {"track": track, "volume_db": volume}, "Mix.")
    # Two host fixtures that apply a gain: one without keys (its parameters are known by id
    # and name, as a third-party plug-in's are) and one with a semantic key.
    listed = {p["id"] for p in j.exec("plugin.list", {}, "Which plug-ins can be added?")["plugins"]}
    assert FIXTURE + "raw-gain" in listed and FIXTURE + "gain" in listed, \
        "pass the test-fixtures.clap path as the third argument"
    raw = j.exec("fx.add", {"track": "tr_3", "plugin": FIXTURE + "raw-gain"},
                 "A plug-in with no semantic keys, after the keys' synth.")
    keyed = j.exec("fx.add", {"track": "tr_4", "plugin": FIXTURE + "gain"},
                   "A plug-in with a key, after the lead's synth.")
    assert (raw["fx"], raw["semantic"], keyed["fx"], keyed["semantic"]) == (
        "fx_6", False, "fx_7", True), (raw, keyed)


def context(j):
    written = j.exec("context.write", {"text": LEAD_SHEET},
                     "The whole song's keys and chords, written once, as a lead sheet.")
    assert (written["added"], written["removed"]) == (33, 0), written  # 29 chords, 4 keys
    assert j.exec("context.get", {"from": 0, "to": 112}, "Read it back.")["text"] == LEAD_SHEET
    for bar, beat, key, chord in ((7, 0.0, "A minor", "E7"), (10, 0.0, "C major", "Am"),
                                  (15, 0.0, "C major", "Bb"), (16, 2.0, "C major", "G"),
                                  (22, 0.0, "F# minor", "Bm")):
        found = j.exec("context.at", {"at": (bar - 1) * 4.0 + beat}, f"Bar {bar}.")
        assert f"{found['key']['tonic']} {found['key']['scale']}" == key, found
        assert found["chord"]["symbol"] == chord, found


def scenes(j):
    for name in ("Verse", "Hook", "Bridge"):
        j.exec("scene.add", {"name": name}, f"Scene {name}.")
    clips = [
        # (scene, track, notation, name, follow, where its harmony comes from)
        ("sc_1", "tr_1", drums("verse"), "Verse drums", "none", {"context_at": {"bar": 1}}),
        ("sc_1", "tr_2", bass(["A1", "F1", "G1", "A1"]), "Verse bass", "chords",
         {"context_at": {"bar": 1}}),
        ("sc_1", "tr_3", keys(["Am", "F", "G", "Am"]), "Verse keys", "chords",
         {"context_at": {"bar": 1}}),
        ("sc_1", "tr_4", lead(VERSE_LEAD), "Verse lead", "key", {"context_at": {"bar": 1}}),
        ("sc_2", "tr_1", drums("hook"), "Hook drums", "none", {"context_at": {"bar": 9}}),
        ("sc_2", "tr_2", bass(["C2", "A1", "F1", "G1"]), "Hook bass", "chords",
         {"context_at": {"bar": 9}}),
        ("sc_2", "tr_3", keys(["C", "Am", "F", "G"]), "Hook keys", "chords",
         {"context_at": {"bar": 9}}),
        ("sc_2", "tr_4", lead(HOOK_LEAD), "Hook lead", "key", {"context_at": {"bar": 9}}),
        ("sc_3", "tr_1", drums("bridge"), "Bridge drums", "none", {"context_at": {"bar": 17}}),
        ("sc_3", "tr_2", bass(["F#1", "D1", "A1", "E1"]), "Bridge bass", "chords",
         {"context_at": {"bar": 17}}),
        ("sc_3", "tr_3", keys(["F#m", "D", "A", "E"]), "Bridge keys", "chords",
         {"context_at": {"bar": 17}}),
        # The wrong key on purpose: the Verse's melody, written in A minor, kept as written
        # under F# minor.
        ("sc_3", "tr_4", lead(VERSE_LEAD), "Bridge lead (A minor)", "none",
         {"context": "1: key A minor ~:w\n2: ~:w\n3: ~:w\n4: ~:w\n"}),
    ]
    for scene, track, notation, name, follow, where in clips:
        made = j.exec("clip.add", {"track": track, "scene": scene, "notation": notation,
                                   "name": name, "follow": follow, **where},
                      f"{name}: written against its own key and chords, follow {follow}.")
        assert made["kind"] == "midi" and made["length"] == 16.0, made
        # Chord symbols for the notes and context_at for the harmony agree: nothing to say.
        assert not [w for w in made.get("warnings", []) if w["code"] == "clip.chords_ignored"], made
    # A clip knows the harmony it was written against: the lead sheet of bars 1-4 and 9-12.
    verse_keys = j.exec("midi.get", {"item": "cl_3"}, "The Verse keys clip.")
    assert verse_keys["context"]["text"] == "1: key A minor {Am}:w\n2: {F}:w\n3: {G}:w\n4: {Am}:w\n"
    assert verse_keys["follow"] == "chords", verse_keys
    assert "[A4 C5 E5]:w" in verse_keys["text"], verse_keys  # Am voiced around octave 4
    hook_bass = j.exec("midi.get", {"item": "cl_6"}, "The Hook bass clip.")
    assert hook_bass["context"]["text"] == "1: key C major {C}:w\n2: {Am}:w\n3: {F}:w\n4: {G}:w\n"
    wrong = j.exec("midi.get", {"item": "cl_12"}, "The wrong-key lead.")
    assert [k["what"] for k in wrong["context"]["keys"]] == ["A minor"], wrong
    assert wrong["follow"] == "none" and not wrong["context"]["chords"], wrong
    described = j.described()
    assert 'cl_2 "Verse bass" in A minor 16 beats' in described, described
    assert "follows the chords" in described and "follows the key" in described, described


# --- reading a laid item against the lead sheet ----------------------------------------

def laid_notes(j, item, base_beat):
    """An item's sounding notes as (project beat, pitch class, pitch)."""
    # What sounds: the notes are laid as written and mapped as they play (ADR-0098).
    return [(base_beat + n["start"], n["sounds"] % 12, n["sounds"])
            for n in j.exec("midi.notes", {"item": item, "sounding": True},
                            "An item's notes, as they sound.")["notes"]]


def bar_matches(notes, first_bar, bars):
    """For each bar: do the bass-style notes (every note) sit on the chord's root, and do
    the keys-style notes (all of them) sit in the chord and cover it? Two lists of booleans,
    `roots` and `tones`, by bar. A bar with two chords (bar 16) is read by half-bars."""
    roots, tones = [], []
    for bar in range(first_bar, first_bar + bars):
        spans = [(0.0, 2.0), (2.0, 4.0)] if bar == 16 else [(0.0, 4.0)]
        ok_root = ok_tone = True
        for lo, hi in spans:
            root, pcs = chord_tones(chord_at(bar, lo))
            here = [(p) for (b, p, _) in notes
                    if (bar - 1) * 4.0 + lo - 1e-9 <= b < (bar - 1) * 4.0 + hi - 1e-9]
            ok_root = ok_root and bool(here) and all(p == root for p in here)
            # A whole-note chord held through bar 16's change has no onset in the G half: it
            # sounds as the Fm it began as, so a span without an onset is not read.
            ok_tone = ok_tone and (not here or (set(here) <= pcs and len(set(here)) >= 3))
        roots.append(ok_root)
        tones.append(ok_tone)
    return roots, tones


def mode_matrix(j):
    """The Verse (written in A minor over Am F G Am) laid under each of the song's seven
    four-bar spans, in each follow mode: how many of the 28 bars' bass roots and keys' chords
    are the lead sheet's? By notes."""
    result = {}
    for mode in ("none", "key", "chords"):
        bass_ok, keys_ok = [], []
        for index in range(7):
            bar = 1 + 4 * index
            laid = j.exec("scene.insert", {"scene": "sc_1", "bar": bar, "bars": 4, "follow": mode},
                          f"The Verse at bar {bar}, follow {mode}.")
            rows = {row["track"]: row for row in laid["items"]}
            base = (bar - 1) * 4.0
            roots, _ = bar_matches(laid_notes(j, rows["tr_2"]["item"], base), bar, 4)
            _, tones = bar_matches(laid_notes(j, rows["tr_3"]["item"], base), bar, 4)
            bass_ok += roots
            keys_ok += tones
            # A drum clip is never moved; the lead is a melody and is read in the key below.
            assert rows["tr_1"]["followed"].get("copied") in (None, "drums"), rows["tr_1"]
            j.undo()
        result[mode] = (bass_ok, keys_ok)
    return result


def check_mode_matrix(matrix):
    counts = {mode: (sum(b), sum(k)) for mode, (b, k) in matrix.items()}
    # Copied as written, the Verse is right only where the lead sheet happens to repeat it:
    # bars 1-5, 8 and 25 (9 of 28). Following the key (A minor to C major by degree, to F#
    # minor by -3) keeps the notes in each key but not on each chord. Following the chords
    # puts every bass note on its root and every keys chord on its chord, in all 28 bars,
    # through the two borrowed chords.
    assert counts["none"] == (9, 9), counts
    assert counts["chords"] == (28, 28), counts
    assert counts["none"][0] < counts["key"][0] < counts["chords"][0], counts
    assert counts["none"][1] < counts["key"][1] < counts["chords"][1], counts
    assert matrix["none"][0][:5] == [True] * 5 and not any(matrix["none"][0][8:12]), matrix["none"]


def keys_chroma_top(wav, start, end):
    """The three strongest pitch classes of a stem over [start, end) seconds: Goertzel at the
    semitones A3..B5 over a Hann window of the channel's samples, averaged 8 to 1."""
    step = wav.channels
    first, last = int(start * wav.rate), int(end * wav.rate)
    mono = [wav.values[i * step] for i in range(first, last)]
    block = 8
    low = [sum(mono[i:i + block]) / block for i in range(0, len(mono) - block + 1, block)]
    rate = wav.rate / block
    count = len(low)
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / (count - 1)) for i in range(count)]
    samples = [v * w for v, w in zip(low, window)]
    energy = [0.0] * 12
    for midi in range(57, 84):
        frequency = 440.0 * 2 ** ((midi - 69) / 12)
        coefficient = 2 * math.cos(2 * math.pi * frequency / rate)
        s1 = s2 = 0.0
        for v in samples:
            s0 = v + coefficient * s1 - s2
            s2, s1 = s1, s0
        energy[midi % 12] += s1 * s1 + s2 * s2 - coefficient * s1 * s2
    return set(sorted(range(12), key=lambda p: -energy[p])[:3])


def heard_bars(j):
    """Bar by bar over the 28 bars: does the bass sound the chord's root (the pitch the
    engine reports for the bass over the bar) and do the keys sound the chord's tones (a
    chroma of the rendered stem)? Two lists of booleans."""
    stem = j.stems(["tr_3"], 0, 112, "Render the keys over the whole song.")["Keys"][0]
    bass_ok, keys_ok = [], []
    for bar in range(1, 29):
        half = bar == 16  # the Fm holds the first half of bar 16
        start, end = (bar - 1) * 4.0, bar * 4.0 - (2.0 if half else 0.0)
        root, pcs = chord_tones(chord_at(bar))
        report = j.exec("analyze.track", {"track": "tr_2", "start_beat": start + 0.5,
                                          "end_beat": end - 0.5}, f"The bass over bar {bar}.")
        note = report["harmonic"].get("note") or ""
        bass_ok.append(re.match(r"[A-G]#?", note) is not None and
                       pitch_class(re.match(r"[A-G]#?", note).group(0)) == root)
        top = keys_chroma_top(stem, start * BEAT_S + 0.15, end * BEAT_S - 0.15)
        keys_ok.append(top == pcs or (len(pcs) == 4 and top <= pcs))
    return bass_ok, keys_ok


# --- modulation ------------------------------------------------------------------------

def modulation(j):
    """The resolver: a parameter by name, by CLAP id and by key reaches the same stored
    parameter; a synced LFO on a plug-in without keys, a macro on one with a key."""
    by_name = j.exec("modulate", {"target": {"fx": "fx_6", "param": "Gain"}, "source": "lfo",
                                  "depth": 0.5}, "A plug-in without keys: by its name.")
    # The same parameter by id and in another case must find it again, not a new one.
    by_id = j.exec("modulate", {"target": {"fx": "fx_6", "param": "0"}, "source": "lfo"},
                   "The same parameter by its CLAP id.")
    by_case = j.exec("modulate", {"target": {"fx": "fx_6", "param": "gain"}, "source": "lfo"},
                     "The same parameter by its name in lower case.")
    assert (by_name["param"], by_id["param"], by_case["param"]) == ("0", "0", "0"), \
        "a plug-in without keys stores the id"
    # Three routes from LFOs onto one parameter are allowed, and each answer names the routes
    # already there: their offsets add.
    def stacked(answer):
        return [w["message"] for w in answer.get("warnings", []) if w["code"] == "modulation.stacked"]
    assert not stacked(by_name), by_name
    assert by_name["modulation"] in stacked(by_id)[0], stacked(by_id)
    assert by_name["modulation"] in stacked(by_case)[0] and by_id["modulation"] in stacked(by_case)[0]
    assert by_name["param_name"] == by_id["param_name"] == by_case["param_name"] == "Gain"
    macro = j.exec("modulate", {"target": {"fx": "fx_7", "param": "gain"}, "source": "macro",
                                "depth": 0.5}, "A plug-in with a key: by its key.")
    by_semantic_id = j.exec("modulate", {"target": {"fx": "fx_7", "param": "0"},
                                         "source": "lfo"}, "The same one by its CLAP id.")
    by_semantic_name = j.exec("modulate", {"target": {"fx": "fx_7", "param": "Gain"},
                                           "source": "lfo"}, "... and by its name.")
    assert not stacked(macro), "a first route"
    assert macro["param"] == by_semantic_id["param"] == by_semantic_name["param"] == "gain", \
        "a plug-in with keys stores the key, whichever way it was named"
    # Keep one route a target: the three spellings were to see that they agree.
    for extra in (by_id, by_case, by_semantic_id, by_semantic_name):
        j.exec("modulation.remove", {"modulation": extra["modulation"]}, "Keep one route.")
    listed = {m["modulation"]: m for m in
              j.exec("modulation.list", {}, "The routes left.")["modulations"]}
    assert set(listed) == {by_name["modulation"], macro["modulation"]}, listed
    assert listed[by_name["modulation"]]["target"] == {"fx": "fx_6", "param": "0"}, listed
    assert listed[macro["modulation"]]["target"] == {"fx": "fx_7", "param": "gain"}, listed
    assert all(not m.get("inert") and m["fed_by"] for m in listed.values()), listed
    # A stepped parameter takes no route, and says what to use.
    error = j.refused("modulate", {"target": {"fx": "fx_6", "param": "1"}, "source": "lfo"},
                      "modulation.target", "A stepped parameter takes an envelope, not a route.")
    assert "env.add" in error["hint"], error
    j.exec("module.set", {"module": by_name["module"],
                          "params": {"sync": True, "division": "d1_1", "shape": "sine"}},
           "The LFO: synced, one cycle a bar.")
    j.exec("module.set", {"module": macro["module"], "params": {macro["knob"]: "60 %"}},
           "The macro at 60 %.")
    return {"lfo": by_name, "macro": macro}


def level_by_beat(wav, first, count):
    return [wav.db((first + i) * BEAT_S, (first + i + 1) * BEAT_S - 0.02) for i in range(count)]


def modulation_heard(j, mods):
    """On the built song: the synced LFO on the keys' gain swings the keys' level by the bar,
    and the macro on the lead's gain raises it."""
    on = j.stems(["tr_3", "tr_4"], 0, 16, "The keys and the lead, modulated.")
    j.exec("modulation.set", {"modulation": mods["lfo"]["modulation"], "enabled": False},
           "LFO off.")
    j.exec("module.set", {"module": mods["macro"]["module"],
                          "params": {mods["macro"]["knob"]: 0}}, "Macro to 0.")
    off = j.stems(["tr_3", "tr_4"], 0, 16, "The same without them.")
    j.exec("modulation.set", {"modulation": mods["lfo"]["modulation"], "enabled": True},
           "LFO on.")
    j.exec("module.set", {"module": mods["macro"]["module"],
                          "params": {mods["macro"]["knob"]: "60 %"}}, "Macro back to 60 %.")
    keys_on = level_by_beat(on["Keys"][0], 0, 16)
    keys_off = level_by_beat(off["Keys"][0], 0, 16)
    # An LFO at offset 0 starts a cycle at its bottom: with the LFO on, the keys are far
    # quieter on a bar's first beat than on its third, bar after bar; with it off they are
    # not (the chords change, not the level).
    swing_on = [keys_on[b + 2] - keys_on[b] for b in range(0, 16, 4)]
    swing_off = [keys_off[b + 2] - keys_off[b] for b in range(0, 16, 4)]
    assert all(g > 10.0 for g in swing_on), (swing_on, swing_off)
    assert all(abs(g) < 2.0 for g in swing_off), (swing_on, swing_off)
    # The macro lifts the lead's gain: 60 % of its depth is several dB.
    lift = on["Lead"][0].db(0, 16 * BEAT_S) - off["Lead"][0].db(0, 16 * BEAT_S)
    assert lift > 2.0, lift


# --- launching under a playrate, capturing, committing ---------------------------------

def poll(what, condition, limit=TIMEOUT):
    deadline = time.monotonic() + limit
    while not condition():
        assert time.monotonic() < deadline, f"timed out: {what}"
        time.sleep(0.02)


def live(j):
    """A launch while the transport runs under a master playrate lands on a bar and plays;
    a capture is refused until the rate is 1."""
    j.exec("engine.start", {"backend": "dummy"}, "Start the dummy backend.")
    before = j.exec("transport.playrate.set", {"rate": 1.25}, "A master playrate of 1.25.")
    assert before["requested_rate"] == 1.25, before
    j.exec("transport.play", {}, "Play.")
    poll("the transport runs", lambda: j.beat() > 0.5)
    launched = j.launch_when_ready("launch.scene", {"scene": "sc_1"}, "Launch the Verse.")
    assert launched["beat"] % 4.0 == 0.0 and launched["beat"] > 0, launched
    assert not launched.get("started"), launched
    poll("the Verse plays", lambda: all(r["plays"] == "clip" for r in j.state().values()))
    warnings = [w["code"] for w in launched.get("warnings", [])]
    assert warnings == [], launched  # the clips follow live (ADR-0098 X4a), within budget
    followed = [r.get("followed", "none") for r in j.state().values()]
    assert any(f != "none" for f in followed), followed
    error = j.refused("launch.capture", {"on": True}, "launch.playrate",
                      "A capture is refused under a playrate.")
    assert "playrate" in error["hint"], error
    j.exec("launch.stop", {}, "Stop the clips.")
    j.exec("transport.playrate.set", {"rate": 1.0}, "Back to 1.")
    # The audible rate follows the requested one over about a second of playing; a capture
    # begun before it has arrived would lay its launches off the bar lines.
    poll("the audible rate reaches 1",
         lambda: abs(j.exec("transport.playrate.get", {}, "The rate heard now.")
                     ["effective_rate"] - 1.0) < 1e-4)
    j.exec("transport.stop", {}, "Stop.")


def perform(j):
    """Three scenes launched on bars, captured and committed: sections named after them."""
    before = j.described()
    j.exec("launch.capture", {"on": True}, "Capture.")
    beats = []
    clips_launched = 0
    for scene in ("sc_1", "sc_2", "sc_3"):
        launched = j.launch_when_ready("launch.scene", {"scene": scene}, f"Launch {scene}.")
        beats.append(launched["beat"])
        clips_launched += len(launched["tracks"])
        poll(f"the transport passes {launched['beat'] + 4.5}",
             lambda: j.beat() >= launched["beat"] + 4.5)
    # A launch lands on the next bar line after its call, so a slow call lands later: the
    # expectations below come from the beats the answers give (0, 8, 16 on a quick machine).
    assert beats[0] == 0.0 and all(b % 4 == 0 for b in beats), beats
    assert all(a < b for a, b in zip(beats, beats[1:])), beats
    end = j.exec("launch.capture", {"on": False}, "End the capture.")
    j.exec("launch.arrangement", {}, "Back to the arrangement.")
    j.exec("transport.stop", {}, "Stop.")
    committed = j.exec("launch.commit", {}, "Lay the performance on the timeline.")
    assert [(s["scene"], s["start"]) for s in committed["sections"]] == [
        ("sc_1", beats[0]), ("sc_2", beats[1]), ("sc_3", beats[2])], committed
    assert committed["sections"][2]["end"] == end["to"], committed
    # One item for each clip launched (four in each of three scenes), however late.
    assert len(committed["items"]) == clips_launched == 12, (clips_launched, committed)
    for beat, name in zip(beats, ("Verse", "Hook", "Bridge")):
        bar = int(beat // 4) + 1
        found = j.exec("context.at", {"bar": bar}, f"The section over bar {bar}.")
        assert found["section"]["name"] == name, found
    # The commit lays notes as written with each clip's context and the follow the plan used
    # (ADR-0098, following is playback): the clips that follow the chords or the key keep
    # following where they are laid, so nothing sits under a key it was not written in; the
    # drums and the Bridge lead (follow none, written in A minor, which the lead sheet says
    # for those bars) were not mapped when heard and are not now.
    mismatched = sorted(re.match(r"(cl_\d+)", w["message"]).group(1)
                        for w in committed.get("warnings", []) if w["code"] == "context.mismatch")
    assert mismatched == [], committed.get("warnings")
    follows = [j.exec("midi.get", {"item": i}, "A committed item.").get("follow")
               for i in committed["items"]]
    assert sorted(f for f in follows if f) == ["chords"] * 6 + ["key"] * 2, follows
    hook_bass = [i for i in committed["items"]
                 if j.exec("midi.get", {"item": i}, "A committed item.")["text"].count("C2:e") > 0]
    assert hook_bass, "the Hook's bass was committed as written (C2 first)"
    j.undo_to(before)


# --- the built song, measured ----------------------------------------------------------

def song(j):
    built = j.exec("arrangement.build", {"sequence": [{"scene": s, "bars": b} for s, b in SONG]},
                   "Verse, Verse, Hook, Hook, Bridge, Bridge, Verse: four bars each.")
    regions = j.described().split("\nregions: ")[1].split("\n")[0]
    assert re.sub(r"rg_\d+ ", "", regions) == (
        '"Verse" bar 1–bar 5 "Verse" bar 5–bar 9 "Hook" bar 9–bar 13 "Hook" bar 13–bar 17 '
        '"Bridge" bar 17–bar 21 "Bridge" bar 21–bar 25 "Verse" bar 25–bar 29'), regions
    # What `followed` says: the bass and keys follow the chords (a region a laid item, no
    # transposition to name), the lead follows the key by a transpose of 0 (A minor to A
    # minor), the drums are not followed.
    verse = {row["track"]: row for row in built["segments"][0]["items"]}
    assert verse["tr_1"]["followed"] == {"mode": "none"}, verse["tr_1"]
    assert verse["tr_2"]["followed"] == {"mode": "chords", "regions": [
        {"from_bar": 1, "to_bar": 4, "from_key": "A minor", "to_key": "A minor",
         "by": "chords"}]}, verse["tr_2"]
    assert verse["tr_4"]["followed"]["regions"] == [
        {"from_bar": 1, "to_bar": 4, "from_key": "A minor", "to_key": "A minor",
         "by": "transpose", "interval": 0}], verse["tr_4"]
    # Only the wrong-key lead warns, once for each of its two items.
    codes = [(w["code"], w["path"]) for w in built["warnings"]]
    assert codes == [("context.mismatch", "/sequence/4/scene"),
                     ("context.mismatch", "/sequence/5/scene")], built["warnings"]
    assert all("A minor" in w["message"] and "F# minor" in w["message"] and "cl_12" in w["message"]
               for w in built["warnings"]), built["warnings"]
    # By notes: every item of the song against the lead sheet, bar by bar.
    for index, segment in enumerate(built["segments"]):
        first_bar = 1 + 4 * index
        rows = {row["track"]: row for row in segment["items"]}
        base = (first_bar - 1) * 4.0
        roots, _ = bar_matches(laid_notes(j, rows["tr_2"]["item"], base), first_bar, 4)
        _, tones = bar_matches(laid_notes(j, rows["tr_3"]["item"], base), first_bar, 4)
        if segment["scene"] == "sc_3":
            assert all(roots) and all(tones), (first_bar, roots, tones)
        else:
            assert all(roots) and all(tones), (segment["scene"], first_bar, roots, tones)
        lead_notes = laid_notes(j, rows["tr_4"]["item"], base)
        key = at("key", base)
        off_key = [p for (_, p, _) in lead_notes if p not in scale_tones(key)]
        if segment["scene"] == "sc_3":
            assert len(off_key) >= 4, "the wrong-key lead has notes outside F# minor"
        else:
            assert not off_key, (segment["scene"], key, off_key)
    # The borrowed chords: the Hook's second pass (bars 13-16) lands on Em, Bb, and on Fm
    # then G within bar 16, and the bass plays their roots.
    hook_two = {row["track"]: row for row in built["segments"][3]["items"]}
    bass_notes = laid_notes(j, hook_two["tr_2"]["item"], 48.0)
    roots_by_bar = {bar: sorted({NAMES[p] for (b, p, _) in bass_notes
                                 if (bar - 1) * 4.0 <= b < bar * 4.0 - (2.0 if bar == 16 else 0.0)
                                 and not (bar == 16 and b >= 62.0)})
                    for bar in (13, 14, 15, 16)}
    assert roots_by_bar == {13: ["C"], 14: ["E"], 15: ["A#"], 16: ["F"]}, roots_by_bar
    assert {NAMES[p] for (b, p, _) in bass_notes if b >= 62.0} == {"G"}, "Fm:h then G:h"
    return built


def song_heard(j):
    bass_ok, keys_ok = heard_bars(j)
    assert all(bass_ok) and all(keys_ok), ("the song as heard, by bar", bass_ok, keys_ok)


def stems_skip_the_modulators(j):
    """The Modulators channel `modulate` made holds a rack with only control outputs: a default
    stems render leaves it out and says so; naming it renders it."""
    answer = j.exec("render.stems", {"start_beat": 0, "end_beat": 4, "bounds": "custom"},
                    "Stems of everything: the Modulators channel makes no sound.")
    skipped = answer["skipped"]
    assert [(row["track"], row["reason"]) for row in skipped] == [("tr_6", "no_audio")], skipped
    assert len(answer["files"]) == 5, answer["files"]  # drums, bass, keys, lead, the buss
    named = j.exec("render.stems", {"tracks": ["tr_6"], "start_beat": 0, "end_beat": 4,
                                    "bounds": "custom"}, "Named, it is rendered anyway.")
    assert len(named["files"]) == 1 and "skipped" not in named, named


def song_from_scenes(j):
    """A song built from scenes with no lead sheet under it: `write_context` writes the lead sheet
    from the clips, and `context.get` reads it back."""
    before = j.described()
    built = j.exec("arrangement.build", {"bar": 101, "write_context": True,
                                         "sequence": [{"scene": "sc_1", "bars": 4},
                                                      {"scene": "sc_2", "bars": 4}]},
                   "Two scenes far past the song, bars 101-108, writing their harmony: nothing "
                   "is written on the context track first.")
    notes = [seg["context"] for seg in built["segments"]]
    # A minor still holds from the song, so the Verse writes only its chords (the Am it starts on
    # holds too); the Hook's key and four chords are new.
    assert [(n["written"], n["keys"], n["chords"]) for n in notes] == [
        (True, 0, 3), (True, 1, 4)], notes
    sheet = j.exec("context.get", {"from": 404, "to": 432},
                   "The lead sheet it wrote, read back.")
    assert sheet["text"] == ("102: {F}:w\n103: {G}:w\n104: {Am}:w\n105: key C major {C}:w\n"
                             "106: {Am}:w\n107: {F}:w\n108: {G}:w\n"), sheet["text"]
    j.undo_to(before)


def copied_as_written_heard(j):
    """The same measurement over the Verse copied as written seven times: it tells the
    modes apart by ear, not only by notes."""
    before = j.described()
    for index in range(7):
        j.exec("scene.insert", {"scene": "sc_1", "bar": 1 + 4 * index, "bars": 4,
                                "follow": "none"}, "The Verse, copied as written.")
    bass_ok, keys_ok = heard_bars(j)
    assert (sum(bass_ok), sum(keys_ok)) == (9, 9), (bass_ok, keys_ok)
    j.undo_to(before)


def other_direction(j, built):
    """A section back as a scene, and that scene laid again somewhere else."""
    hook_region = built["segments"][3]["region"]
    captured = j.exec("scene.capture", {"region": hook_region, "name": "Hook again"},
                      "A scene from the Hook's second pass (bars 13-16: C Em Bb Fm|G).")
    clip = {row["track"]: row["clip"] for row in captured["clips"]}
    got = j.exec("midi.get", {"item": clip["tr_2"]}, "The captured bass clip.")
    # A following take's context and follow become the clip's (X4d): the Hook bass was written
    # against bars 9-12 (C Am F G) and follows the chords, so launched it sounds as it did there.
    assert got["context"]["text"] == (
        "1: key C major {C}:w\n2: {Am}:w\n3: {F}:w\n4: {G}:w\n"), got["context"]
    for track in ("tr_2", "tr_3"):
        followed = j.exec("midi.get", {"item": clip[track]}, "A captured clip.").get("follow")
        assert followed == "chords", (track, followed)
    # Laid under A minor, it follows the chords there.
    laid = j.exec("scene.insert", {"scene": captured["scene"], "bar": 29, "bars": 4},
                  "Lay it after the song, bars 29-32: A minor holds, Am F E7 Am would not be "
                  "written there, so the chords there are the last ones: Am.")
    rows = {row["track"]: row for row in laid["items"]}
    for track in ("tr_2", "tr_3"):
        assert rows[track]["followed"]["mode"] == "chords", rows[track]
    for _ in range(2):  # the lay-out, the capture
        j.undo()


def written_notes(j, item):
    """An item's notes as written: (start, pitch)."""
    return [(n["start"], n["pitch"])
            for n in j.exec("midi.notes", {"item": item}, "An item's notes, as written.")["notes"]]


def edit_after(j, built):
    """A chord changed on the context track after the song was laid: the laid notes stay as
    written, and the items that follow sound the new chord at once (following is playback)."""
    verse_two = {row["track"]: row for row in built["segments"][1]["items"]}
    keys_item, bass_item = verse_two["tr_3"]["item"], verse_two["tr_2"]["item"]
    before = laid_notes(j, bass_item, 16.0)
    written = written_notes(j, bass_item)
    changed = j.exec("context.write", {"text": "6: {Bb}:w\n"}, "Bar 6 becomes Bb: one call.")
    assert (changed["replaced"], changed["added"], changed["removed"]) == (1, 0, 0), changed
    CHANGED[6] = "Bb"
    assert written_notes(j, bass_item) == written, "the laid bass's notes are not rewritten"
    assert laid_notes(j, bass_item, 16.0) != before, "the laid bass sounds the new chord"
    context_now = j.exec("midi.get", {"item": keys_item}, "The keys item reads the new chord.")
    assert [c["what"] for c in context_now["context"]["chords"]] == ["Am", "Bb", "E7", "Am"]
    roots, _ = bar_matches(laid_notes(j, bass_item, 16.0), 5, 4)
    assert all(roots), roots
    _, tones = bar_matches(laid_notes(j, keys_item, 16.0), 5, 4)
    assert all(tones), tones
    report = j.exec("analyze.track", {"track": "tr_2", "start_beat": 20.5, "end_beat": 23.5},
                    "The bass over bar 6, heard.")
    assert report["harmonic"]["note"].startswith("A#"), report["harmonic"]
    # The keys are heard on the new chord too, from a render of bars 5-8 (beats 16-32): bar 6 is
    # Bb's tones, and bars 5 and 7 are still Am's and E7's (one chord moved, nothing re-laid).
    stem = j.stems(["tr_3"], 16, 32, "Render the keys over bars 5-8.")["Keys"][0]
    for bar, symbol in ((5, "Am"), (6, "Bb"), (7, "E7")):
        start = (bar - 5) * 4.0 * BEAT_S + 0.15
        top = keys_chroma_top(stem, start, start + 4.0 * BEAT_S - 0.3)
        pcs = chord_tones(symbol)[1]
        assert top == pcs or (len(pcs) == 4 and top <= pcs), (bar, symbol, top, pcs)
    j.undo()  # the context change
    CHANGED.clear()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("plugins")
    parser.add_argument("fixtures")
    parser.add_argument("--transcript", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="supple-song2-") as workdir:
        client = Client([args.engine, "--mcp", "--workdir", workdir, "--plugin-path", args.plugins,
                         "--plugin-path", args.fixtures])
        try:
            client.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "song2", "version": "1"}})
            client.send("notifications/initialized", notify=True)
            j = Song(client)
            build(j)
            context(j)
            scenes(j)
            mods = modulation(j)
            check_mode_matrix(mode_matrix(j))
            live(j)
            perform(j)
            copied_as_written_heard(j)
            built = song(j)
            song_heard(j)
            modulation_heard(j, mods)
            stems_skip_the_modulators(j)
            song_from_scenes(j)
            other_direction(j, built)
            edit_after(j, built)
            client.close()
        finally:
            if client.process.poll() is None:
                client.process.kill()
                client.process.wait(timeout=5)
        if args.transcript:
            import json
            lines = ["# The song again: harmony written once", ""]
            for note, tool, arguments, structured in client.transcript:
                text = json.dumps(structured, indent=2).replace(workdir, "<workdir>")
                lines += [f"## {tool}", "", note, "", "```json", json.dumps(arguments, indent=2),
                          "```", "", "```json", text, "```", ""]
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n".join(lines), encoding="utf-8")
    print("MCP song journey 2: one lead sheet, clips that follow it by key and by chords, "
          "measured bar by bar")


if __name__ == "__main__":
    main()
