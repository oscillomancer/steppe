#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One song with all three headline features: the context track, modulation and the
clip launcher, driven as an agent does it.

A scripted MCP client drives the engine's stdio MCP server through its public commands.
A 24-bar song in three keys (C minor, then Eb major, then D minor) is written as a
lead sheet; drums, bass and keys are written as clips in four scenes (Intro, Verse,
Chorus, Break) against the chords; an LFO on the keys' filter, an LFO on the send to a
reverb buss and a macro on the bass's filter move through the song; the scenes are
performed live on the dummy backend with a capture and committed to the timeline;
the same scenes are laid out with `arrangement.build` over the chords, and a section
is captured back into a scene.

What the journey asserts is what is heard, not that a call succeeded: every measure
comes from an offline render of the project (stems and the mix), read back as samples
or through `analyze.track`, and is compared with what the commands said (a launch's
`beat`, a track's volume in dB, a modulation's depth, the commit's items).

It documents the limits of today's build as refusals (a clip with no key of its own has
none to snap to, a clip take's FX take no modulation), and it plays only the path that
works. A clip keeps the key and chords it was written against (ADR-0098): the drums and
the bass are written with `context_at` where their scene sits in the song, the keys with
chord symbols, and the Break's bass snaps within D minor though the project starts in
C minor. The Verse is written once, in C minor: laid under D minor it warns, and laid
with `follow: key` its bass lands in D minor beside its unmoved drums. Nothing here
decides by a wall clock: the live stage polls the transport and the modulation
telemetry, and the timeout only turns a hang into a failure.

  mcp_journey_song.py <engine> <plugin-dir> [--transcript FILE]
"""

import argparse
import math
import re
import tempfile
import time
from pathlib import Path

from mcp_arrangement import Client
from mcp_journey import digest
from mcp_journey_launcher import TIMEOUT, Launcher, difference, samples

PLUGIN = "com.suppleaudio."

# 150 beats per minute: a beat is 0.4 s and a bar 1.6 s.
BPM = 150
BEAT_S = 60.0 / BPM

# The context track: the Intro and the Verse in C minor, the Chorus in Eb major (the
# relative major), the Break in D minor.
LEAD_SHEET = """1: key C minor {Cm7}:w
2: {Cm7}:w
3: {Abmaj7}:w
4: {G7}:w
5: {Cm7}:w
6: {Fm7}:w
7: {Abmaj7}:w
8: {G7}:w
9: {Cm7}:w
10: {Fm7}:w
11: {Abmaj7}:w
12: {G7}:w
13: key Eb major {Ebmaj7}:w
14: {Bb}:w
15: {Cm7}:w
16: {Abmaj7}:w
17: {Ebmaj7}:w
18: {Bb}:w
19: {Cm7}:w
20: {Abmaj7}:w
21: key D minor {Dm7}:w
22: {Bbmaj7}:w
23: {Gm7}:w
24: {A7}:w
"""

# Each scene is four bars, written over its four chords; the scene's length in the song
# is how many times that plays.
CHORDS = {
    "Intro": ["Cm7", "Cm7", "Abmaj7", "G7"],
    "Verse": ["Cm7", "Fm7", "Abmaj7", "G7"],
    "Chorus": ["Ebmaj7", "Bb", "Cm7", "Abmaj7"],
    "Break": ["Dm7", "Bbmaj7", "Gm7", "A7"],
}
ROOTS = {
    "Verse": ["C2", "F2", "Ab1", "G1"],
    "Chorus": ["Eb2", "Bb1", "C2", "Ab1"],
    "Break": ["D2", "Bb1", "G1", "A1"],
}
KEYS_VELOCITY = {"Intro": 60, "Verse": 80, "Chorus": 95, "Break": 70}
DRUMS = {
    "Intro": "1: hat   x.x.x.x.x.x.x.x.",
    "Verse": "1: kick  x...x...x...x...\n1: snare ....x.......x...\n"
             "1: hat   x.x.x.x.x.x.x.x.",
    "Chorus": "1: kick  x...x...x...x...\n1: snare ....x.......x...\n"
              "1: hat   x.x.x.x.x.x.x.x.",
    "Break": "1: kick  x.......x.......\n1: ohat  ..x...x...x...x.",
}


def drums(scene):
    return ("notation 1\nmeter 4/4\nvoice drums ch 10 grid s\n" + DRUMS[scene] +
            "\n2-4 = 1\n")


def bass(scene):
    lines = ["notation 1", "meter 4/4", "voice bass ch 1 vel 100"]
    for bar, root in enumerate(ROOTS[scene], 1):
        lines.append(f"{bar}: {root}:e {root}:e r:e {root}:e {root}:e r:e {root}:e {root}:e")
    return "\n".join(lines) + "\n"


def keys(scene):
    lines = ["notation 1", "meter 4/4", f"voice keys ch 1 vel {KEYS_VELOCITY[scene]} octave 4"]
    for bar, chord in enumerate(CHORDS[scene], 1):
        lines.append(f"{bar}: {{{chord}}}:w")
    return "\n".join(lines) + "\n"


KEYS_PATCH = {"params": {"osc1.wave": "saw", "filter1.type": "ladder",
                         "filter1.cutoff": "700 Hz", "filter1.resonance": "30 %",
                         "amp.attack": "20 ms", "amp.release": "300 ms"}}
BASS_PATCH = {"params": {"osc1.wave": "saw", "filter1.type": "ladder",
                         "filter1.cutoff": "350 Hz", "amp.attack": "5 ms",
                         "amp.release": "120 ms"}}

SONG = [("sc_1", 4), ("sc_2", 8), ("sc_3", 8), ("sc_4", 4)]  # Intro Verse Chorus Break
# The bar each scene starts at in the song above.
SCENE_BAR = {"Intro": 1, "Verse": 5, "Chorus": 13, "Break": 21}
TRACK_NAMES = {"tr_1": "Drums", "tr_2": "Bass", "tr_3": "Keys"}


class Wav:
    """A rendered file's samples, measured by time."""

    def __init__(self, result):
        self.values, self.channels, self.rate = samples(result)

    def span(self, start, end):
        first = int(start * self.rate) * self.channels
        return self.values[first:int(end * self.rate) * self.channels]

    def db(self, start, end):
        """The mean-square level of [start, end) seconds in dB (about -180 for silence)."""
        values = self.span(start, end)
        assert values, (start, end)
        return 10.0 * math.log10(sum(v * v for v in values) / len(values) + 1e-18)

    def first_above(self, level, after=0.0):
        """The time in seconds of the first sample whose magnitude is over `level`."""
        step = self.channels
        for i in range(int(after * self.rate) * step, len(self.values), step):
            if abs(self.values[i]) > level:
                return i / step / self.rate
        return None


class Song(Launcher):
    def stems(self, tracks, start, end, note):
        """Render each track in `tracks` to a file over the beats; a Wav per file name."""
        result = self.exec("render.stems", {"tracks": tracks, "start_beat": start,
                                            "end_beat": end, "bounds": "custom"}, note)
        out = {}
        for f in result["files"]:
            name = Path(f["path"]).stem.split("-", 1)[1]
            out[name] = (Wav({"path": f["path"]}), f)
        assert len(out) == len(tracks), result
        return out

    def centroids(self, track, start, end, note):
        """The track's spectral centroid over time, from analyze.track."""
        report = self.exec("analyze.track", {"track": track, "start_beat": start,
                                             "end_beat": end, "detail": "full"}, note)
        return report["spectral"]["centroid_track_hz"]

    def telemetry(self):
        """modulation.telemetry without a transcript line: it is polled."""
        response = self.c.send("tools/call", {"name": "exec", "arguments": {
            "command": "modulation.telemetry", "args": {}}})["result"]
        assert not response.get("isError"), response
        return {row["modulation"]: row for row in response["structuredContent"]["modulations"]}


def percentile(values, p):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(p * len(ordered)))]


def build(j):
    j.exec("tempo.set", {"points": [{"beat": 0, "bpm": BPM}]}, "150 BPM: a bar is 1.6 s.")
    for name, kind in (("Drums", "channel"), ("Bass", "channel"), ("Keys", "channel"),
                       ("Verb", "buss")):
        j.exec("track.add", {"name": name, "type": kind}, f"Add the {name} track.")
    for track, plugin in (("tr_1", "drumlin"), ("tr_2", "marram"), ("tr_3", "marram"),
                          ("tr_4", "ghyll")):
        j.exec("fx.add", {"track": track, "plugin": PLUGIN + plugin})
    for fx, patch, note in (("fx_3", KEYS_PATCH, "Keys: a saw through a ladder."),
                            ("fx_2", BASS_PATCH, "Bass: a darker saw.")):
        merged = j.exec("fx.patch.merge", {"fx": fx, "patch": patch}, note)
        # A first merge into a fresh instance reports what it changed, not every default.
        assert 1 <= len(merged["changed"]["params"]) <= len(patch["params"]), merged["changed"]
    j.exec("route.add", {"from": "tr_3", "to": "tr_4"}, "Send the keys to the reverb buss.")
    for track, volume in (("tr_1", -8), ("tr_2", -6), ("tr_3", -10)):
        j.exec("track.set", {"track": track, "volume_db": volume}, "Mix: levels in dB.")


def context(j):
    written = j.exec("context.write", {"text": LEAD_SHEET},
                     "The whole song's keys and chords as a lead sheet.")
    assert (written["added"], written["removed"]) == (27, 0), written  # 24 chords, 3 keys
    assert j.exec("context.get", {"from": 0, "to": 96}, "Read it back.")["text"] == LEAD_SHEET
    for bar, key, chord in ((3, ("C", "minor"), "Abmaj7"), (14, ("Eb", "major"), "Bb"),
                            (22, ("D", "minor"), "Bbmaj7")):
        at = j.exec("context.at", {"bar": bar}, f"What holds at bar {bar}?")
        assert (at["key"]["tonic"], at["key"]["scale"]) == key, at
        assert at["chord"]["symbol"] == chord, at
    for tonic in ("C", "Eb", "D"):
        assert tonic in j.described().split("context:")[1].split("chords:")[0], "keys listed"


def scenes(j):
    for name in CHORDS:
        j.exec("scene.add", {"name": name}, f"Scene {name}.")
    count = 0
    for scene, name in (("sc_1", "Intro"), ("sc_2", "Verse"), ("sc_3", "Chorus"),
                        ("sc_4", "Break")):
        cells = [("tr_1", drums(name)), ("tr_3", keys(name))]
        if name != "Intro":
            cells.insert(1, ("tr_2", bass(name)))
        for track, notation in cells:
            args = {"track": track, "scene": scene, "notation": notation,
                    "name": f"{name} {TRACK_NAMES[track]}"}
            if track != "tr_3":
                # The drums and the bass are written against the song's own harmony where
                # the scene will sit: the clip keeps that key and those chords.
                args["context_at"] = {"bar": SCENE_BAR[name]}
            made = j.exec("clip.add", args,
                          f"The {name} clip on {track}, written over its four chords.")
            assert made["kind"] == "midi" and made["length"] == 16.0, made
            count += 1
    assert count == 11
    # The clip holds the chords as notes: Verse keys bar 1 is Cm7.
    text = j.exec("midi.get", {"item": "cl_5"}, "Read the Verse keys back.")
    assert "[C4 Eb4 G4 Bb4]:w" in text["text"], text
    # ... and, written with chord symbols, it also holds them as chords: midi.get shows
    # them by clip bar, and the lead sheet of the clip is the same chords.
    context_of = text["context"]
    assert [c["what"] for c in context_of["chords"]] == CHORDS["Verse"], context_of
    assert [c["item_bar"] for c in context_of["chords"]] == [1, 2, 3, 4], context_of
    assert "project_bar" not in context_of["chords"][0], "a clip has no project bar"
    assert not context_of["keys"], "the notation named no key"
    assert context_of["text"] == "".join(
        f"{bar}: {{{chord}}}:w\n" for bar, chord in enumerate(CHORDS["Verse"], 1)), context_of
    assert text["follow"] == "none", text
    # The bass was written against the song's harmony at its bar: Verse in C minor, the
    # Break in D minor, though the project starts in C minor.
    verse_bass = j.exec("midi.get", {"item": "cl_4"}, "The Verse bass: C minor.")
    assert [k["what"] for k in verse_bass["context"]["keys"]] == ["C minor"], verse_bass
    assert [c["what"] for c in verse_bass["context"]["chords"]] == CHORDS["Verse"], verse_bass
    break_bass = j.exec("midi.get", {"item": "cl_10"}, "The Break bass: D minor.")
    assert [k["what"] for k in break_bass["context"]["keys"]] == ["D minor"], break_bass
    assert "A1:e" in break_bass["text"], break_bass
    assert 'cl_10 "Break Bass" in D minor' in j.described(), "describe names the clip's key"
    # D1: the Break bass snaps to D minor, the scale it was written in, and its A1 stays
    # (read in the project's C minor it became Ab1).
    snapped = j.exec("midi.edit", {"item": "cl_10", "ops": [{"op": "snap_to_scale"}]},
                     "Snap the Break bass to its own key: nothing moves.")
    assert snapped["notes"] == [], snapped  # only the notes an op moved are returned
    pitches = [n["pitch"] for n in j.exec("midi.notes", {"item": "cl_10"},
                                          "Its notes.")["notes"]]
    assert 33 in pitches and 32 not in pitches, pitches  # A1 stays, no Ab1
    forced = j.exec("midi.edit", {"item": "cl_10",
                                  "ops": [{"op": "snap_to_scale", "key": "C minor"}]},
                    "The same in C minor, asked for: what D1 used to do by itself.")
    assert forced["notes"], forced
    assert 32 in [n["pitch"] for n in j.exec("midi.notes", {"item": "cl_10"},
                                             "Its notes.")["notes"]]
    j.undo()
    # What a clip with no key says: the Verse keys were written without one.
    error = j.refused("midi.edit", {"item": "cl_5", "ops": [{"op": "snap_to_scale"}]},
                      "midi.no_key", "A clip with no key of its own has none to read.")
    assert "clip.set" in error["hint"], error
    # The keys' chords are its own: fit_to_chord reads them.
    j.exec("midi.edit", {"item": "cl_5", "ops": [{"op": "fit_to_chord"}]},
           "Fit the Verse keys to their chords: they are in them already.")


def modulation(j):
    """An LFO on the keys' filter (one cycle a bar), an LFO on the send (one cycle in two
    bars) and a macro on the bass's filter."""
    cutoff = j.exec("modulate", {"target": {"fx": "fx_3", "param": "filter1.cutoff"},
                                 "source": "lfo", "depth": 0.3}, "An LFO on the keys' cutoff.")
    send = j.exec("modulate", {"target": {"route": "sd_1", "param": "volume"},
                               "source": "lfo", "depth": -12}, "An LFO on the send level.")
    macro = j.exec("modulate", {"target": {"fx": "fx_2", "param": "filter1.cutoff"},
                                "source": "macro", "depth": 0.4}, "A macro on the bass's cutoff.")
    assert (cutoff["modulation"], send["modulation"], macro["modulation"]) == (
        "mr_1", "mr_2", "mr_3"), (cutoff, send, macro)
    assert cutoff["rack"] == send["rack"] == macro["rack"], "one rack carries all three"
    j.exec("module.set", {"module": cutoff["module"],
                          "params": {"sync": True, "division": "d1_1", "shape": "sine"}},
           "The filter LFO: synced, one cycle a whole note (a bar).")
    j.exec("module.set", {"module": send["module"],
                          "params": {"sync": True, "division": "d2_1", "shape": "triangle"}},
           "The send LFO: synced, one cycle in two bars.")
    j.exec("module.set", {"module": macro["module"], "params": {macro["knob"]: "80 %"}},
           "Move the macro to 80 %: a knob takes text in its unit (its knobs run 0 to 100).")
    listed = j.exec("modulation.list", {}, "The routes, each fed by its module.")["modulations"]
    assert [m["modulation"] for m in listed] == ["mr_1", "mr_2", "mr_3"], listed
    assert all(not m.get("inert") and m["fed_by"] and m["enabled"] for m in listed), listed
    assert [m["fed_by"][0]["type"].rsplit(".", 1)[1] for m in listed] == [
        "lfo", "lfo", "macro"], listed
    # A clip take's FX take envelopes, not modulation (yet): the refusal names the way out.
    j.exec("fx.add", {"take": "tk_5", "plugin": PLUGIN + "gain"}, "Gain on the Verse keys clip.")
    error = j.refused("modulate", {"target": {"fx": "fx_5", "param": "gain"}, "source": "lfo"},
                      "modulation.target", "A clip take's FX take no modulation.")
    assert "take FX" in error["hint"], error
    j.exec("fx.remove", {"fx": "fx_5"}, "Take it off again.")
    return {"cutoff": cutoff, "send": send, "macro": macro}


def live(j, mods):
    """A launched clip is modulated as the timeline is: the telemetry moves while it plays."""
    j.exec("engine.start", {"backend": "dummy"}, "Start the dummy backend.")
    first = j.telemetry()
    assert all(abs(first[m]["offset"]) <= first[m]["high"] + 1e-9 for m in first), first
    j.launch_when_ready("launch.scene", {"scene": "sc_2"}, "Launch the Verse.")
    j.wait("the Verse plays", lambda s: all(r["plays"] == "clip" for r in s.values()))
    start = j.telemetry()["mr_1"]["offset"]
    deadline = time.monotonic() + TIMEOUT
    while True:
        now = j.telemetry()
        if abs(now["mr_1"]["offset"] - start) > 0.05 and abs(now["mr_2"]["offset"]) > 0.5:
            break
        assert time.monotonic() < deadline, f"the LFOs never moved: {start} {now}"
        time.sleep(0.02)
    assert abs(now["mr_1"]["offset"]) <= 0.3 and abs(now["mr_2"]["offset"]) <= 12, now
    # The macro at 80 % with depth 0.4 holds the cutoff 0.32 of its range up.
    assert abs(now["mr_3"]["offset"] - 0.32) < 0.01, now["mr_3"]
    j.exec("launch.stop", {}, "Stop the clips.")
    j.exec("transport.stop", {}, "Stop and return to the start.")


def perform(j):
    """Scenes launched on bars, captured and committed: the timeline holds what was heard.

    Nothing here depends on how fast the machine is: a launch lands on the next bar line after
    the call, and the capture ends whenever its off call arrives, so every expectation below is
    derived from the beats the answers give (on a quick machine they are 0, 8, 16 and 24)."""
    before = j.described()
    j.exec("launch.capture", {"on": True}, "Start capturing launches.")
    assert j.exec("launch.state", {}, "Capturing?")["capturing"]
    beats = []
    for scene in ("sc_1", "sc_2", "sc_3", "sc_4"):
        launched = j.launch_when_ready("launch.scene", {"scene": scene}, f"Launch {scene}.")
        beats.append(launched["beat"])
        # Into the second bar of the scene, so the next launch lands on a later bar line.
        deadline = time.monotonic() + TIMEOUT
        while j.beat() < launched["beat"] + 4.5:
            assert time.monotonic() < deadline, f"the transport never reached {launched}"
            time.sleep(0.02)
    assert beats[0] == 0.0 and all(b % 4 == 0 for b in beats), \
        ("each launch lands on a bar line", beats)
    assert all(a < b for a, b in zip(beats, beats[1:])), ("in order", beats)
    end = j.exec("launch.capture", {"on": False}, "End the capture.")
    # The capture ends when the call lands: it ran at least until the last scene had played a
    # bar (the transport was past it when the call was made), and no longer than that is claimed.
    assert end["from"] == 0.0 and end["to"] >= beats[3] + 4.0, end
    j.exec("launch.arrangement", {}, "Back to the arrangement.")
    j.wait("the arrangement again", lambda s: all(r["plays"] == "arrangement"
                                                  for r in s.values()))
    j.exec("transport.stop", {}, "Stop.")
    committed = j.exec("launch.commit", {}, "Lay the performance on the timeline.")
    assert committed["tracks"] == ["tr_1", "tr_2", "tr_3"], committed
    assert len(committed["items"]) >= 11, committed  # 3 + 2 + 3 + 3 clips played, a period each
    assert (committed["from"], committed["to"]) == (end["from"], end["to"]), committed
    # Each scene launched whole is a section named after it, from its launch to the next
    # one's (the last to the capture's end), as scene.insert makes one.
    sections = committed["sections"]
    assert [(x["scene"], x["start"]) for x in sections] == [
        ("sc_1", beats[0]), ("sc_2", beats[1]), ("sc_3", beats[2]), ("sc_4", beats[3])], sections
    assert [x["end"] for x in sections][:3] == beats[1:], sections
    assert abs(sections[3]["end"] - end["to"]) < 1e-9, sections
    for beat, name in zip(beats, ("Intro", "Verse", "Chorus", "Break")):
        at = j.exec("context.at", {"bar": int(beat // 4) + 1},
                    f"The section over bar {int(beat // 4) + 1} of the performance.")
        assert at["section"]["name"] == name, at

    # What the timeline now holds is what was launched: the bass enters with the Verse, the
    # drums fill out there, and the keys play louder in the Chorus. The Break is measured over
    # its first four beats (the capture ran at least that far).
    stop = beats[3] + 4.0
    stems = j.stems(["tr_1", "tr_2", "tr_3"], 0, stop, "Render the performance, a file a track.")
    drums_wav, bass_wav, keys_wav = (stems[n][0] for n in ("Drums", "Bass", "Keys"))
    intro, verse, chorus, brk = (b * BEAT_S for b in beats)
    assert bass_wav.db(0, verse - 0.05) < -100, "no bass in the Intro"
    onset = bass_wav.first_above(1e-3)
    assert abs(onset - verse) < 0.02, ("the bass enters on the Verse's bar", onset)
    assert drums_wav.db(verse, chorus) - drums_wav.db(intro, verse) > 15, "full kit from the Verse"
    assert 0.5 < keys_wav.db(chorus, brk) - keys_wav.db(verse, chorus) < 3.0, "Chorus keys louder"
    assert 2.0 < drums_wav.db(verse, chorus) - drums_wav.db(brk, stop * BEAT_S) < 6.0, \
        "the Break is lighter"
    performed = stems

    # The same scenes laid out with arrangement.build over the same bars render the same
    # stems away from the 5 ms switch fades, modulation included: a launched clip is
    # modulated as the timeline is.
    j.undo_to(before)
    bars = [int((b - a) // 4) for a, b in zip(beats, beats[1:])] + [2]
    j.exec("arrangement.build", {"sequence": [{"scene": f"sc_{k + 1}", "bars": n}
                                              for k, n in enumerate(bars)]},
           "The same four scenes, the bars that were launched, laid by hand.")
    by_hand = j.stems(["tr_1", "tr_2", "tr_3"], 0, stop, "Render them.")
    spans = [(0.05, verse - 0.05), (verse + 0.05, chorus - 0.05),
             (chorus + 0.05, brk - 0.05), (brk + 0.05, stop * BEAT_S - 0.05)]
    for name in ("Drums", "Bass", "Keys"):
        worst = difference({"path": performed[name][1]["path"]},
                           {"path": by_hand[name][1]["path"]}, spans)
        assert worst < 1e-6, (name, worst)
    j.undo_to(before)
    return end


def song(j, mods):
    """The scenes as a 24-bar song over the chords, measured."""
    built = j.exec("arrangement.build", {"sequence": [{"scene": s, "bars": b} for s, b in SONG]},
                   "Intro 4 bars, Verse 8, Chorus 8, Break 4: the chords' own layout.")
    regions = j.described().split("\nregions: ")[1].split("\n")[0]
    assert re.sub(r"rg_\d+ ", "", regions) == (
        '"Intro" bar 1–bar 5 "Verse" bar 5–bar 13 "Chorus" bar 13–bar 21 "Break" bar 21–bar 25'
    ), regions
    # A clip's name is the name of the item laid from it.
    text = j.described()
    for needle in ('"Verse Bass" bar 5–bar 13', '"Chorus Keys" bar 13–bar 21',
                   '"Break Drums" bar 21–bar 25'):
        assert needle in text, (needle, text)
    for bar, section, symbol in ((3, "Intro", "Abmaj7"), (8, "Verse", "G7"),
                                 (14, "Chorus", "Bb"), (22, "Break", "Bbmaj7")):
        at = j.exec("context.at", {"bar": bar}, f"Bar {bar}: section and chord.")
        assert at["section"]["name"] == section and at["chord"]["symbol"] == symbol, at
    # Each scene's chords are the context's own over its bars: the build and the lead
    # sheet were written from the same plan.
    chorus = built["segments"][2]
    assert chorus["scene"] == "sc_3" and (chorus["start"], chorus["end"]) == (48.0, 80.0), chorus
    chorus_keys = [i["item"] for i in chorus["items"] if i["track"] == "tr_3"][0]
    midi = j.exec("midi.get", {"item": chorus_keys}, "The Chorus keys item.")
    assert [c["what"] for c in midi["context"]["chords"]][:4] == CHORDS["Chorus"], midi
    assert midi["context"]["keys"][0]["what"] == "Eb major", midi

    # The whole song, rendered: what the mix does at each section.
    mix = j.exec("render.mix", {"start_beat": 0, "end_beat": 96, "analyze": "summary"},
                 "Render the song.")
    assert abs(mix["seconds"] - 96 * BEAT_S) < 0.01, mix
    assert mix["peak_dbfs"] < -1.0, "the mix has headroom"
    wav = Wav(mix)
    level = {name: wav.db(a * BEAT_S, b * BEAT_S) for name, (a, b) in dict(
        intro=(0, 16), verse=(16, 48), chorus=(48, 80), brk=(80, 96)).items()}
    assert level["verse"] - level["intro"] > 4, level      # bass and drums come in
    assert level["chorus"] - level["verse"] > 0.4, level   # the Chorus is louder
    assert level["chorus"] - level["brk"] > 1.5, level     # the Break takes the weight off

    # Switches sit where the commands said: the bass is silent for the Intro's 4 bars and
    # sounds on the Verse's first beat, and the Break's drums are lighter than the Chorus's.
    verse_stems = j.stems(["tr_2", "tr_3", "tr_4"], 16, 48, "The Verse's bass, keys and reverb.")
    bass_wav = verse_stems["Bass"][0]
    assert bass_wav.first_above(1e-3) < 0.02, "the bass sounds from the Verse's first beat"
    intro_bass = j.stems(["tr_2"], 0, 16, "The bass over the Intro.")["Bass"][0]
    assert intro_bass.db(0, 16 * BEAT_S) < -100, "silent"

    # Levels are what track.set said: keys 6 dB down is 6 dB down.
    keys_wav = verse_stems["Keys"][0]
    base_keys = keys_wav.db(0, 32 * BEAT_S)
    j.exec("track.set", {"track": "tr_3", "volume_db": -16}, "Keys 6 dB down.")
    quieter = j.stems(["tr_3"], 16, 48, "Keys again.")["Keys"][0]
    assert abs((quieter.db(0, 32 * BEAT_S) - base_keys) + 6.0) < 0.05, "6 dB is 6 dB"
    j.exec("track.set", {"track": "tr_3", "volume_db": -10}, "Back.")

    # The filter LFO: the keys' spectral centroid swings widely with it on, and little with
    # it off (the chord changes still move it).
    on = j.centroids("tr_3", 16, 48, "The keys' centroid over the Verse.")
    j.exec("modulation.set", {"modulation": "mr_1", "enabled": False}, "Filter LFO off.")
    off = j.centroids("tr_3", 16, 48, "The same with it off.")
    j.exec("modulation.set", {"modulation": "mr_1", "enabled": True}, "On again.")
    swing_on = percentile(on, 0.95) / percentile(on, 0.05)
    swing_off = percentile(off, 0.95) / percentile(off, 0.05)
    assert swing_on > 5.0 and swing_off < 2.5, (swing_on, swing_off)

    # The send LFO: the reverb's level, with it on against off, in 0.4 s windows over the
    # Verse. It follows a 2-bar (8 window) triangle of several dB each way.
    reverb_on = verse_stems["Verb"][0]
    j.exec("modulation.set", {"modulation": "mr_2", "enabled": False}, "Send LFO off.")
    reverb_off = j.stems(["tr_4"], 16, 48, "The reverb with it off.")["Verb"][0]
    j.exec("modulation.set", {"modulation": "mr_2", "enabled": True}, "On again.")
    window = 4 * BEAT_S / 4  # a beat
    diff = [reverb_on.db(i * window, (i + 1) * window) - reverb_off.db(i * window, (i + 1) * window)
            for i in range(32)]
    assert max(diff) > 5 and min(diff) < -5, diff
    assert all(abs(diff[i] - diff[i + 8]) < 1.5 for i in range(24)), ("a 2-bar cycle", diff)

    # The macro: the bass's cutoff is 0.32 of its range higher at 80 % than at 0.
    bright = j.centroids("tr_2", 16, 48, "The bass at macro 80 %.")
    macro = mods["macro"]
    j.exec("module.set", {"module": macro["module"], "params": {macro["knob"]: 0}},
           "Macro to 0.")
    dark = j.centroids("tr_2", 16, 48, "The bass at macro 0.")
    j.exec("module.set", {"module": macro["module"], "params": {macro["knob"]: 80}},
           "Back to 80 %.")
    assert sum(bright) > 2.0 * sum(dark), (sum(bright), sum(dark))
    return built


def other_direction(j, built):
    """Scenes from the song, and the song again: scene.capture and scene.insert."""
    captured = j.exec("scene.capture", {"region": built["segments"][2]["region"],
                                        "name": "Chorus again"},
                      "A scene from the Chorus section.")
    assert [c["track"] for c in captured["clips"]] == ["tr_1", "tr_2", "tr_3"], captured
    scenes_block = j.described().split("Scenes (")[1]
    assert scenes_block.count('"Chorus Bass"') == 2, "the captured clip keeps the name"
    for row in captured["clips"]:
        copy = j.exec("midi.get", {"item": row["clip"]}, "A captured clip.")["text"]
        source = j.exec("midi.get", {"item": row["source_item"]}, "Its item.")["text"]
        assert copy == source, (copy, source)
    laid = j.exec("scene.insert", {"scene": captured["scene"], "bars": 4, "bar": 25},
                  "Lay it after the Break.")
    assert (laid["start"], laid["end"]) == (96.0, 112.0), laid
    after = j.described()
    j.undo()
    j.undo()
    assert j.described() != after
    j.redo()
    j.redo()
    assert j.described() == after, "scene.capture and scene.insert are one undo step each"
    render = j.render(start_beat=96, end_beat=112)
    assert render["analysis"]["loudness"]["integrated_lufs"] > -40, "the repeated Chorus sounds"
    j.exec("project.save", {"path": "song.json"}, "Save.")
    j.exec("project.open", {"path": "song.json"}, "Reopen.")
    assert j.described() == after, "scenes, clips, items, sections, chords and modulation survive"
    reopened = j.render(start_beat=96, end_beat=112)
    assert digest(render) == digest(reopened), "a reopened project renders the same"
    for route in ("mr_1", "mr_2", "mr_3"):
        assert route in after, route
    # A scene laid under another key says so: the Verse was written in C minor, and from the
    # Break on the song is in D minor. Its notes are copied as they are (follow: none).
    mismatched = j.exec("scene.insert", {"scene": "sc_2", "bars": 4, "bar": 40},
                        "The Verse, written in C minor, laid under D minor.")
    warnings = [w for w in mismatched.get("warnings", []) if w["code"] == "context.mismatch"]
    # Only the bass: the drums have no pitch to move, and the keys clip names no key.
    assert len(warnings) == 1 and "cl_4" in warnings[0]["message"], mismatched.get("warnings")
    assert all("C minor" in w["message"] and "D minor" in w["message"] and "bar 40" in w["message"]
               for w in warnings), warnings
    j.undo()
    # The Verse is written once, in C minor, and asked to follow the key it is laid under:
    # the bass lands in D minor (C minor's root C2 is D2, its Fm is Gm, Ab is Bb, G is A),
    # the drums are not moved, and the keys clip, which names no key, is copied and says so.
    followed = j.exec("scene.insert", {"scene": "sc_2", "bars": 4, "bar": 40, "follow": "key"},
                      "The same Verse, asked to follow the key: it lands in D minor.")
    assert not [w for w in followed.get("warnings", []) if w["code"] == "context.mismatch"], \
        followed.get("warnings")
    rows = {row["clip"]: row for row in followed["items"]}
    bass_row = rows["cl_4"]["followed"]
    assert bass_row["mode"] == "key" and "unrolled" not in bass_row, bass_row
    assert bass_row["regions"] == [{"from_bar": 40, "to_bar": 43, "from_key": "C minor",
                                    "to_key": "D minor", "by": "transpose", "interval": 2}], bass_row
    assert rows["cl_3"]["followed"]["copied"] == "drums", rows["cl_3"]
    assert rows["cl_5"]["followed"]["copied"] == "no_key", rows["cl_5"]
    laid_bass = j.exec("midi.notes", {"item": rows["cl_4"]["item"]}, "The bass as laid.")
    assert {n["pitch"] for n in laid_bass["notes"]} == {38, 43, 34, 33}, laid_bass["notes"]
    clip_bass = j.exec("midi.notes", {"item": "cl_4"}, "The bass clip, unchanged.")
    assert {n["pitch"] for n in clip_bass["notes"]} == {36, 41, 32, 31}, clip_bass["notes"]
    clip_drums = j.exec("midi.notes", {"item": "cl_3"}, "The drums clip.")
    laid_drums = j.exec("midi.notes", {"item": rows["cl_3"]["item"]}, "The drums as laid.")
    assert [(n["start"], n["pitch"]) for n in laid_drums["notes"]] == \
        [(n["start"], n["pitch"]) for n in clip_drums["notes"]], "the drums are not moved"
    j.undo()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("plugins")
    parser.add_argument("--transcript", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="supple-song-") as workdir:
        client = Client([args.engine, "--mcp", "--workdir", workdir,
                         "--plugin-path", args.plugins])
        try:
            client.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "song", "version": "1"}})
            client.send("notifications/initialized", notify=True)
            j = Song(client)
            build(j)
            context(j)
            scenes(j)
            mods = modulation(j)
            live(j, mods)
            perform(j)
            built = song(j, mods)
            other_direction(j, built)
            client.close()
        finally:
            if client.process.poll() is None:
                client.process.kill()
                client.process.wait(timeout=5)
        if args.transcript:
            import json
            lines = ["# One song: the context track, modulation and the clip launcher", ""]
            for note, tool, arguments, structured in client.transcript:
                text = json.dumps(structured, indent=2).replace(workdir, "<workdir>")
                lines += [f"## {tool}", "", note, "", "```json", json.dumps(arguments, indent=2),
                          "```", "", "```json", text, "```", ""]
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n".join(lines), encoding="utf-8")
    print("MCP song journey: chords, scenes, modulation, a performance committed, "
          "a song built, measured")


if __name__ == "__main__":
    main()
