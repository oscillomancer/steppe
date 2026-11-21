#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Batch 2 acceptance: an agent works with the clip launcher end to end.

A scripted MCP client drives the engine's stdio MCP server the way an agent does,
using only the public commands and what their descriptions say. Scenes and clips
are made with scene.* and clip.*, MIDI is written into clips in notation and edited
with the content commands, and the scenes are launched on the dummy audio backend:
a quantized launch, a stop, a `once` clip ending, a return to the arrangement,
read back with launch.state. A performance is captured (launch.capture) and committed
to the timeline (launch.commit), and the committed timeline renders as what was
launched. An envelope on a clip take's FX is taken in clip time and heard where the
clip is laid on the timeline. Then the scenes become a song (scene.insert,
scene.capture, arrangement.build), chords are written over it, and offline renders
of the arrangement are measured against what the scenes hold: the same song laid by
hand renders bit-identically, a clip playing live changes nothing in a render, and
the render is the same twice and after a save and reopen. Undo and redo cross all of
it, and the refusals an agent will meet are checked for a code, a path and a hint.

Nothing here decides by a wall clock. The dummy backend is paced like a device, so
the live stage polls launch.state until a condition holds; the timeout only turns a
hang into a failure. The quantize boundaries are checked on the song positions the
commands report, which depend on the tempo map and not on the clock.

  mcp_journey_launcher.py <engine> <plugin-dir> [--transcript FILE]
"""

import argparse
import json
import struct
import tempfile
import time
from array import array
from pathlib import Path

from mcp_arrangement import Client
from mcp_journey import Journey, digest, momentary

PLUGIN = "com.suppleaudio."

# 240 beats per minute: a bar is one second, so the live stage is short.
BPM = 240

VERSE_DRUMS = """notation 1
meter 4/4
voice drums ch 10 grid s
1: kick  x...x...x...x...
"""
CHORUS_DRUMS = """notation 1
meter 4/4
voice drums ch 10 grid s
1: kick  x...x...x...x...
1: hat   x.x.x.x.x.x.x.x.
"""
VERSE_BASS = """notation 1
meter 4/4
voice bass ch 1 vel 100
1: C2:q E2:q G2:q B1:q
"""
CHORUS_BASS = """notation 1
meter 4/4
voice bass ch 1 vel 110
1: C2:e C2:e C3:e C2:e C2:e C2:e C3:e C2:e
2: F2:e F2:e F3:e F2:e G2:e G2:e G3:e G2:e
"""
VERSE_KEYS = """notation 1
meter 4/4
voice keys ch 1 vel 80 octave 4
1: {Cm7}:w
"""
CHORUS_KEYS = """notation 1
meter 4/4
voice keys ch 1 vel 120 octave 4
1: {Ab}:w
2: {Bb}:w
"""
BREAK_DRUMS = VERSE_DRUMS
BREAK_BASS = """notation 1
meter 4/4
voice bass ch 1 vel 100
1: G1:q G1:q G1:q G1:q
"""

# Chords over the built song: twelve bars, Verse Chorus Verse.
SONG_CHORDS = """1: key C minor {Cm7}:w
2: {Cm7}:w
3: {Fm7}:w
4: {Fm7}:h {G7}:h
5: {Ab}:w
6: {Bb}:w
7: {Ab}:w
8: {Bb}:w
9: {Cm7}:w
10: {Cm7}:w
11: {Fm7}:w
12: {Fm7}:h {G7}:h
"""

# Only a hang ends a poll: every condition polled for here takes a few seconds on any machine
# that plays in real time, so the limit is far above what a slow runner needs, and below
# ctest's own test timeout (1500 s).
TIMEOUT = 600.0


class Launcher(Journey):
    def state_result(self):
        """launch.state without a transcript line: it is polled."""
        response = self.c.send("tools/call", {"name": "exec", "arguments": {
            "command": "launch.state", "args": {}}})["result"]
        assert not response.get("isError"), response
        return response["structuredContent"]

    def state(self):
        return {row["track"]: row for row in self.state_result()["tracks"]}

    def wait(self, what, condition):
        """Poll launch.state until `condition(state)`; the timeout only ends a hang."""
        deadline = time.monotonic() + TIMEOUT
        while True:
            state = self.state()
            if condition(state):
                return state
            assert time.monotonic() < deadline, f"never saw: {what}: {state}"
            time.sleep(0.02)

    def beat(self):
        """The transport's song position, without a transcript line: it is polled."""
        response = self.c.send("tools/call", {"name": "exec", "arguments": {
            "command": "transport.get", "args": {}}})["result"]
        assert not response.get("isError"), response
        return response["structuredContent"]["beat"]

    def launch_when_ready(self, command, args, note):
        """A launch right after the transport started is refused with launch.later: the
        transport is still relocating, and the hint says to launch again in a moment."""
        deadline = time.monotonic() + TIMEOUT
        refused = False
        while True:
            response = self.c.send("tools/call", {"name": "exec", "arguments": {
                "command": command, "args": args}})["result"]
            result = response["structuredContent"]
            if not response.get("isError"):
                self.c.transcript.append((note, "exec", {"command": command, "args": args},
                                          result))
                return result
            assert result["code"] == "launch.later" and result["hint"], result
            if not refused:
                refused = True
                self.c.transcript.append((note + " (refused while the transport relocated)",
                                          "exec", {"command": command, "args": args}, result))
            assert time.monotonic() < deadline, f"still relocating: {result}"
            time.sleep(0.05)

    def described(self):
        return self.exec("session.describe", {}, "Read the session.")["text"]

    def undo_to(self, text, limit=20):
        """Undo until the session reads as `text`: the steps in between are not counted."""
        for _ in range(limit):
            if self.described() == text:
                return
            self.undo()
        raise AssertionError("undo never reached the earlier session")

    def lead_points(self):
        return self.exec("context.get", {"from": 0, "to": 400}, "Read the lead sheet.")["points"]


def samples(result):
    """The interleaved samples of a rendered WAV (float32 or 16, 24 or 32-bit integer)."""
    data = Path(result["path"]).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "a WAV file"
    at, fmt, body, sub = 12, None, None, None
    while at + 8 <= len(data):
        name, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        if name == b"fmt ":
            fmt = struct.unpack("<HHIIHH", data[at + 8:at + 24])
            if fmt[0] == 0xFFFE:  # extensible: the sample format is the GUID's first field
                sub = struct.unpack("<H", data[at + 8 + 24:at + 8 + 26])[0]
        elif name == b"data":
            body = data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    tag = sub or tag
    if tag == 3 and bits == 32:
        values = array("f")
        values.frombytes(body[:len(body) // 4 * 4])
        return list(values), channels, rate
    step = bits // 8
    scale = float(1 << (bits - 1))
    out = [int.from_bytes(body[i:i + step], "little", signed=True) / scale
           for i in range(0, len(body) - step + 1, step)]
    return out, channels, rate


def difference(a, b, spans):
    """The largest sample difference between two renders inside the given spans (seconds)."""
    (xs, channels, rate), (ys, other, _) = samples(a), samples(b)
    assert channels == other, (channels, other)
    worst = 0.0
    for start, end in spans:
        first, last = int(start * rate) * channels, min(int(end * rate) * channels, len(xs), len(ys))
        assert last > first, (start, end, len(xs), len(ys))
        worst = max(worst, max(abs(x - y) for x, y in zip(xs[first:last], ys[first:last])))
    return worst


def energy(result, start, end):
    """The root mean square of a span (seconds) of a render."""
    values, channels, rate = samples(result)
    part = values[int(start * rate) * channels:int(end * rate) * channels]
    return (sum(v * v for v in part) / max(len(part), 1)) ** 0.5


def bar_aligned(beat, bars=1):
    return abs(beat / (4.0 * bars) - round(beat / (4.0 * bars))) < 1e-9


def scenes(j):
    """Three tracks, scenes of clips made in notation, content edited in place."""
    j.exec("tempo.set", {"points": [{"beat": 0, "bpm": BPM}]}, "Four beats a second.")
    for name, kind in (("Drums", "channel"), ("Bass", "channel"), ("Keys", "channel"),
                       ("Verb", "buss")):
        j.exec("track.add", {"name": name, "type": kind}, f"Add the {name} track.")
    for track, plugin in (("tr_1", "drumlin"), ("tr_2", "marram"), ("tr_3", "marram")):
        j.exec("fx.add", {"track": track, "plugin": PLUGIN + plugin})
    for name in ("Verse", "Chorus", "Break", "Spare"):
        j.exec("scene.add", {"name": name}, f"The {name} scene.")
    ids = {}
    for scene, rows in (("sc_1", (("tr_1", VERSE_DRUMS), ("tr_2", VERSE_BASS),
                                  ("tr_3", VERSE_KEYS))),
                        ("sc_2", (("tr_1", CHORUS_DRUMS), ("tr_2", CHORUS_BASS),
                                  ("tr_3", CHORUS_KEYS))),
                        ("sc_3", (("tr_1", BREAK_DRUMS), ("tr_2", BREAK_BASS)))):
        for track, notation in rows:
            made = j.exec("clip.add", {"track": track, "scene": scene, "notation": notation},
                          f"A clip for {track} in {scene}.")
            assert made["kind"] == "midi", made
            ids[(scene, track)] = made
    assert ids[("sc_1", "tr_2")]["length"] == 4.0 and ids[("sc_2", "tr_2")]["length"] == 8.0
    listed = j.exec("scene.list", {}, "The scenes, with their clip counts.")
    assert [(s["id"], s["clips"]) for s in listed["scenes"]] == [
        ("sc_1", 3), ("sc_2", 3), ("sc_3", 2), ("sc_4", 0)], listed

    # Write over a clip's MIDI: it keeps its length, and the engine says when the
    # notation does not fit in it.
    longer = CHORUS_KEYS.replace("2: {Bb}:w", "2: {Bb}:w\n3: {Cm7}:w")
    written = j.exec("midi.write", {"item": ids[("sc_2", "tr_3")]["clip"],
                                    "notation": longer}, "A third bar written into a "
                     "two-bar clip.")
    assert any(w["code"] == "midi.beyond_length" for w in written.get("warnings", [])), written
    j.exec("midi.write", {"item": ids[("sc_2", "tr_3")]["clip"], "notation": CHORUS_KEYS},
           "Back to two bars.")
    # Edit it with the content commands, and read it back.
    clip = ids[("sc_1", "tr_2")]["clip"]
    j.exec("midi.edit", {"item": clip, "ops": [
        {"op": "add", "pitch": 36, "start": 3.5, "length": 0.5},
        {"op": "velocity", "notes": [0], "velocity": 120}]},
        "A pickup note and a louder first note in the Verse bass.")
    text = j.exec("midi.get", {"item": clip}, "Read the Verse bass back.")["text"]
    assert "C2:q" in text and ("C2:e" in text or "C2:s" in text), text
    j.exec("midi.write", {"item": clip, "notation": VERSE_BASS}, "Verse bass as it was.")
    j.exec("clip.set", {"clip": ids[("sc_1", "tr_3")]["clip"], "name": "Pad", "gain_db": -9},
           "Name the Verse keys clip, and make it much quieter.")
    line = next(l for l in j.described().splitlines() if ids[("sc_1", "tr_3")]["clip"] in l)
    assert '"Pad"' in line and "-9 dB" in line, line  # describe is how an agent reads a clip back
    # Break: its drums loop; its bass plays once, on beat boundaries.
    j.exec("clip.set", {"clip": ids[("sc_3", "tr_2")]["clip"], "mode": "once",
                        "quantize": "beat"}, "The Break bass plays once, on the beat.")
    j.exec("clip.set", {"clip": ids[("sc_3", "tr_1")]["clip"], "name": "Break beat"})
    return ids


def refusals_before(j, ids):
    """Timeline commands refuse a clip."""
    clip = ids[("sc_1", "tr_3")]["clip"]
    for command, args in (("item.set", {"item": clip, "position": 4}),
                          ("item.remove", {"item": clip})):
        error = j.refused(command, args, "item.kind", f"{command} given a clip.")
        assert "clip.set" in error["hint"] or "clip.remove" in error["hint"], error


def clip_fx_envelope(j):
    """An envelope on a clip take's FX is in clip time (C2): it is taken, and heard where
    the clip is laid on the timeline. On a track of its own, undone afterwards."""
    before = j.described()
    track = j.exec("track.add", {"name": "Lead", "type": "channel"},
                   "A track with no FX of its own.")["track"]
    made = j.exec("clip.add", {"track": track, "scene": "sc_1", "notation": VERSE_BASS},
                  "A bass clip on it.")
    take = made["takes"][0]
    j.exec("fx.add", {"take": take, "plugin": PLUGIN + "marram"},
           "The clip take plays its own synth.")
    gain = j.exec("fx.add", {"take": take, "plugin": PLUGIN + "gain"},
                  "And a Gain after it, on the clip take's chain.")["fx"]
    # At bar 2, so the points move: clip beats are not timeline beats there.
    j.exec("item.add", {"track": track, "clip": made["clip"], "bar": 2},
           "A copy of the clip on the timeline, before any envelope.")
    plain = j.render(end_beat=8)
    j.undo()  # the copy
    env = j.exec("env.add", {"fx": gain, "param": "gain", "points": [
        {"beat": 0, "value": 0}, {"beat": 2, "value": 0}, {"beat": 2.05, "value": -32}]},
        "The Gain down 32 dB from the clip's third beat, in beats from the clip's start.")
    assert env["points"] == 3, env
    row = next(e for e in j.exec("env.list")["envelopes"] if e["env"] == env["env"])
    assert row["target"] == gain and row["param"] == "gain" and row["active"], row
    j.exec("item.add", {"track": track, "clip": made["clip"], "bar": 2},
           "The same copy: the envelope comes with it, in timeline position.")
    shaped = j.render(end_beat=8)
    # A beat is 0.25 s and the copy starts at 1 s: its first two beats are as before,
    # its last two 32 dB down.
    assert difference(plain, shaped, [(1.0, 1.45)]) < 1e-4, "the first two beats changed"
    down, level = energy(shaped, 1.6, 2.0), energy(plain, 1.6, 2.0)
    assert level > 0 and down < 0.1 * level, (down, level)
    j.undo_to(before)


def live(j, ids):
    """Launch on the dummy backend and read launch.state back."""
    status = j.exec("engine.start", {"backend": "dummy"}, "Start the engine, no device.")
    assert status["running"] and status["backend"] == "dummy", status
    state = j.state()
    assert [row["plays"] for row in state.values()] == ["arrangement"] * 3, state
    assert "tr_4" not in state, "tracks without clips are not listed"

    # Refusals that need no playing.
    j.refused("launch.stop", {"track": "tr_4"}, "launch.track",
              "launch.stop on a buss with no clips.")
    j.refused("launch.scene", {"scene": "sc_4"}, "launch.scene",
              "launch.scene on a scene with no clips.")

    # A scene launched while stopped starts the transport at the play position.
    launched = j.exec("launch.scene", {"scene": "sc_3"}, "Launch the Break scene.")
    assert launched["started"] and launched["beat"] == 0.0, launched
    assert [t["track"] for t in launched["tracks"]] == ["tr_1", "tr_2"], launched
    state = j.wait("Break plays on both tracks",
                   lambda s: s["tr_1"]["plays"] == "clip" and s["tr_2"]["plays"] == "clip")
    assert state["tr_1"]["clip"] == ids[("sc_3", "tr_1")]["clip"], state
    assert state["tr_3"]["plays"] == "arrangement", "tracks outside the scene are untouched"
    # Both clips started on the same beat, so they are at the same place.
    assert abs(state["tr_1"]["position"] - state["tr_2"]["position"]) < 1e-6 or \
        state["tr_2"]["plays"] != "clip", state
    # The once clip ends and is held, silent, until the next launch; the loop goes on.
    state = j.wait("the Break bass has ended", lambda s: s["tr_2"]["plays"] == "held")
    assert state["tr_2"]["clip"] == ids[("sc_3", "tr_2")]["clip"] and \
        "position" not in state["tr_2"], state
    assert state["tr_1"]["plays"] == "clip", "the drum loop plays on"

    # A launch lands on the clip's own quantize, else the project's (a bar).
    keys = ids[("sc_1", "tr_3")]["clip"]
    launched = j.exec("launch.clip", {"clip": keys}, "The Verse keys, on the next bar.")
    assert not launched["started"] and bar_aligned(launched["beat"]), launched
    seen = j.state()["tr_3"]
    assert seen["plays"] == "clip" or seen.get("queued", {}).get("beat") == launched["beat"], seen
    j.wait("the keys play", lambda s: s["tr_3"]["plays"] == "clip")
    bass = ids[("sc_3", "tr_2")]["clip"]
    launched = j.exec("launch.clip", {"clip": bass}, "The Break bass again: its own quantize "
                      "is a beat.")
    assert float(launched["beat"]).is_integer(), launched
    j.wait("the bass plays or has ended again",
           lambda s: s["tr_2"]["plays"] in ("clip", "held"))
    stopped = j.exec("launch.stop", {"track": "tr_1"}, "Stop the drums at the next bar.")
    assert bar_aligned(stopped["beat"]) and stopped["tracks"] == [{"track": "tr_1"}], stopped
    j.wait("the drums are stopped", lambda s: s["tr_1"]["plays"] == "stopped")
    assert j.state()["tr_3"]["plays"] == "clip", "stopping one track leaves the others"
    back = j.exec("launch.arrangement", {}, "Everything back to the arrangement.")
    assert bar_aligned(back["beat"]), back
    assert {t["track"] for t in back["tracks"]} == {"tr_1", "tr_2", "tr_3"}, back
    state = j.wait("every track follows the arrangement",
                   lambda s: all(r["plays"] == "arrangement" for r in s.values()))
    assert all("queued" not in r and "clip" not in r for r in state.values()), state


def performance(j, ids):
    """Capture a performance and commit it to the timeline: it renders as what was launched."""
    before = j.described()
    j.refused("launch.commit", {}, "launch.capture", "Committing before anything is captured.")

    # Play from the start: stop, seek to the first bar, then keep what is launched.
    j.exec("transport.stop", {}, "Stop, and go back to the start.")
    j.exec("transport.seek", {"beat": 0}, "Seek to the first bar.")
    assert not j.state_result()["capturing"]
    began = j.exec("launch.capture", {"on": True}, "Start capturing the launches.")
    assert began["on"] and began["from"] == 0.0, began
    assert j.state_result()["capturing"], "launch.state says a capture is on"
    first = j.launch_when_ready("launch.scene", {"scene": "sc_1"}, "Launch the Verse.")
    assert first["started"] and first["beat"] == 0.0, first
    j.wait("the Verse plays", lambda s: all(r["plays"] == "clip" for r in s.values()))
    second = j.launch_when_ready("launch.scene", {"scene": "sc_2"}, "Launch the Chorus.")
    switch = second["beat"]
    assert not second["started"] and bar_aligned(switch) and switch > 0.0, second
    # Let the Chorus play a bar, then stop capturing.
    deadline = time.monotonic() + TIMEOUT
    while j.beat() < switch + 4.0:
        assert time.monotonic() < deadline, "the Chorus never played a bar"
        time.sleep(0.02)
    ended = j.exec("launch.capture", {"on": False}, "Stop capturing.")
    assert not ended["on"] and ended["from"] == 0.0 and ended["to"] >= switch + 4.0, ended
    assert not j.state_result()["capturing"]
    # Back to the arrangement while the transport runs: a launch given while it is stopped
    # would stay queued. The capture has ended, so this is not kept.
    j.exec("launch.arrangement", {}, "Everything back to the arrangement.")
    j.wait("every track follows the arrangement",
           lambda s: all(r["plays"] == "arrangement" for r in s.values()))
    j.exec("transport.stop", {}, "Stop the transport.")
    verse_bars = int(switch // 4)
    chorus_bars = int((ended["to"] - switch + 1e-6) // 4)
    end_beat = switch + 4.0 * chorus_bars
    assert verse_bars >= 1 and chorus_bars >= 1, (switch, ended)

    # Commit: ordinary items, one undo step, laid out by running time.
    committed = j.exec("launch.commit", {}, "Write the performance onto the timeline.")
    assert committed["tracks"] == ["tr_1", "tr_2", "tr_3"] and len(committed["items"]) == 6, \
        committed
    assert committed["from"] == 0.0 and committed["to"] == ended["to"], committed
    after = j.described()
    assert after != before
    # Each track switches at the launch: a Verse item up to the switch bar, a Chorus item from it.
    bar = f"bar {int(switch // 4) + 1}"
    lines = after.splitlines()
    for track in ("tr_1", "tr_2", "tr_3"):
        head = next(i for i, line in enumerate(lines) if line.startswith(f"  {track} "))
        spans = [line.strip().split(" (")[0].split(" ", 1)[1] for line in lines[head + 1:head + 3]]
        assert len(spans) == 2 and spans[0].split("–")[1] == bar and \
            spans[1].split("–")[0] == bar, (track, spans, bar)

    # It renders as what was launched: the Verse, then the Chorus.
    heard = j.render(end_beat=end_beat)
    seconds = end_beat * 60.0 / BPM
    switch_seconds = switch * 60.0 / BPM
    assert energy(heard, 0.0, switch_seconds) > 0.01, "the Verse is heard"
    assert energy(heard, switch_seconds, seconds) > energy(heard, 0.0, switch_seconds), \
        "the Chorus is louder than the Verse"
    # Committing again writes the same performance over itself.
    j.exec("launch.commit", {}, "Commit again: it writes the same performance over itself.")
    assert digest(j.render(end_beat=end_beat)) == digest(heard), "committing twice changes nothing"
    # One undo step each; redo restores them.
    j.undo()
    j.undo()
    assert j.described() == before, "launch.commit is one undo step"
    j.redo()
    assert j.described() == after
    assert digest(j.render(end_beat=end_beat)) == digest(heard), "redo restores the render"
    j.undo()
    assert j.described() == before

    # The same song laid out by the scenes renders the same away from the switch.
    j.exec("arrangement.build", {"sequence": [{"scene": "sc_1", "bars": verse_bars},
                                               {"scene": "sc_2", "bars": chorus_bars}]},
           "The scenes laid out for the bars that were launched.")
    laid = j.render(end_beat=end_beat)
    margin = 0.02  # the switch crossfades are 5 ms
    worst = difference(heard, laid, [(margin, switch_seconds - margin),
                                     (switch_seconds + margin, seconds - margin)])
    assert worst < 1e-6, f"the committed performance differs from the scenes by {worst}"
    j.undo_to(before)


def song(j, ids):
    """Scenes become a song with sections; chords go over it; the audio is measured."""
    first = j.described()
    sequence = [{"scene": "sc_1", "bars": 4}, {"scene": "sc_2", "bars": 4},
                {"scene": "sc_1", "bars": 4}]
    built = j.exec("arrangement.build", {"sequence": sequence},
                   "Verse, Chorus, Verse: twelve bars, a section each.")
    assert [(s["start"], s["end"], s["scene"]) for s in built["segments"]] == [
        (0.0, 16.0, "sc_1"), (16.0, 32.0, "sc_2"), (32.0, 48.0, "sc_1")], built
    assert [len(s["items"]) for s in built["segments"]] == [3, 3, 3], built
    after = j.described()
    for needle in ('"Verse" bar 1–bar 5', '"Chorus" bar 5–bar 9', '"Verse" bar 9–bar 13'):
        assert needle in after, (needle, after)

    written = j.exec("context.write", {"text": SONG_CHORDS}, "Chords over the twelve bars.")
    assert written["added"] >= 12, written
    assert j.exec("context.at", {"bar": 6})["chord"]["symbol"] == "Bb"
    # An item the scenes made reads its chords from the project's: the Chorus keys.
    chorus_keys = next(i["item"] for i in built["segments"][1]["items"] if i["track"] == "tr_3")
    context = j.exec("midi.get", {"item": chorus_keys}, "Chorus keys with their chords.")
    chords = [c["what"] for c in context["context"]["chords"]]
    assert chords[:2] == ["Ab", "Bb"], context

    # What the scenes hold, measured: the Chorus is denser and louder than the Verse,
    # the two Verses match, and a render is the same twice.
    mix = j.render(end_beat=48)
    again = j.render(end_beat=48)
    assert digest(mix) == digest(again), "the same project renders the same"
    series = momentary(mix)
    third = len(series) // 3
    verse1, chorus, verse2 = (sum(series[i * third:(i + 1) * third]) / third
                              for i in range(3))
    assert chorus > verse1 + 1.0, (verse1, chorus)
    assert abs(verse1 - verse2) < 1.5, (verse1, verse2)

    # A frozen track plays its print, so its clips cannot be launched.
    j.exec("track.freeze", {"track": "tr_3"}, "Freeze the keys: it plays its print.")
    j.refused("launch.clip", {"clip": ids[("sc_1", "tr_3")]["clip"]}, "track.frozen",
              "A clip on a frozen track cannot play.")
    j.exec("track.unfreeze", {"track": "tr_3"}, "Unfreeze.")

    # A clip playing live changes nothing in a render: renders play the arrangement.
    j.exec("launch.clip", {"clip": ids[("sc_3", "tr_2")]["clip"]}, "Hold the bass on a clip.")
    j.wait("the clip plays or is held", lambda s: s["tr_2"]["plays"] in ("clip", "held"))
    during = j.render(end_beat=48)
    assert digest(during) == digest(mix), "a render plays the arrangement only"
    j.exec("launch.arrangement", {}, "Back to the arrangement.")
    j.wait("the arrangement again", lambda s: s["tr_2"]["plays"] == "arrangement")
    return first, built, mix


def by_hand(j, first, built, mix):
    """The same song laid out with item.add {clip} and item.set renders bit for bit."""
    j.undo_to(first)  # the build, its chords, and the freeze and thaw on the way
    assert not j.lead_points(), "undo took the chords too"
    j.exec("context.write", {"text": SONG_CHORDS}, "The chords again.")
    for segment in built["segments"]:
        for item in segment["items"]:
            laid = j.exec("item.add", {"track": item["track"], "clip": item["clip"],
                                       "bar": int(segment["start"] // 4) + 1},
                          "A copy of the clip on the timeline.")
            j.exec("item.set", {"item": laid["item"], "length": 16.0, "loop": True},
                   "Four bars, looped.")
    by_hand_mix = j.render(end_beat=48)
    assert digest(by_hand_mix) == digest(mix), \
        "scene.insert lays what item.add and item.set lay"


def round_trip(j, first, ids):
    """Build the song again, capture a section, refuse what cannot be laid, then undo,
    redo, save and reopen across all of it."""
    sequence = [{"scene": "sc_1", "bars": 4}, {"scene": "sc_2", "bars": 4},
                {"scene": "sc_1", "bars": 4}]
    j.undo_to(first)
    built = j.exec("arrangement.build", {"sequence": sequence}, "The song again.")
    after_build = j.described()
    assert after_build != first
    j.exec("context.write", {"text": SONG_CHORDS}, "Its chords again.")
    after_chords = j.described()
    chords = j.lead_points()
    assert chords and "C minor" in after_chords, after_chords

    # What the song cannot take, each refused with the index or the span in its path.
    error = j.refused("scene.insert", {"scene": "sc_1", "bar": 3, "bars": 2},
                      "scene.insert.occupied", "A scene laid over items.")
    assert "it_" in error["message"] and error["path"] == "/args/bar", error
    j.refused("scene.insert", {"scene": "sc_4", "bar": 13, "bars": 2}, "scene.empty",
              "The empty scene.")
    j.refused("arrangement.build", {"sequence": [{"scene": "sc_1", "bars": 2},
                                                 {"scene": "sc_4", "bars": 2}], "bar": 13},
              "scene.empty", "An empty scene in the sequence.")
    bass = ids[("sc_2", "tr_2")]["clip"]
    j.exec("clip.set", {"clip": bass, "length": 6}, "A bass clip that repeats every 6 beats "
           "over 8 beats of notes.")
    error = j.refused("scene.insert", {"scene": "sc_2", "bar": 13, "bars": 4},
                      "scene.insert.period", "A repeat no single item reproduces.")
    assert "clip.set" in error["hint"], error
    j.undo()  # the length
    assert j.described() == after_chords, "refusals and their set-up leave nothing behind"

    # Capture the Chorus: a new scene whose clips hold what the section's items hold.
    chorus = built["segments"][1]
    captured = j.exec("scene.capture", {"region": chorus["region"]},
                      "A scene from the Chorus section.")
    assert [c["track"] for c in captured["clips"]] == ["tr_1", "tr_2", "tr_3"], captured
    for row, item in zip(captured["clips"], chorus["items"]):
        assert row["source_item"] == item["item"], (row, item)
        copy = j.exec("midi.get", {"item": row["clip"]}, "A captured clip's MIDI.")["text"]
        source = j.exec("midi.get", {"item": item["item"]}, "The item's.")["text"]
        assert copy == source, (copy, source)
    after_capture = j.described()
    assert after_capture != after_chords and "Chorus" in after_capture

    # Undo and redo across all of it: each command is one step.
    j.undo()
    assert j.described() == after_chords, "scene.capture is one undo step"
    j.undo()
    assert j.described() == after_build and not j.lead_points(), "so is context.write"
    j.undo()
    assert j.described() == first, "so is arrangement.build"
    for text in (after_build, after_chords, after_capture):
        j.redo()
        assert j.described() == text
    assert j.lead_points() == chords, "redo restores the chords exactly"

    # The captured scene plays, and the project saves and reopens as it was.
    j.exec("launch.scene", {"scene": captured["scene"]}, "Launch the captured scene.")
    state = j.wait("the captured scene plays", lambda s: all(
        r["plays"] == "clip" for r in s.values()))
    assert {r["clip"] for r in state.values()} == {c["clip"] for c in captured["clips"]}, state
    j.exec("launch.arrangement", {}, "Back to the arrangement.")
    j.wait("the arrangement again", lambda s: all(
        r["plays"] == "arrangement" for r in s.values()))
    before = j.render(end_beat=48)
    j.exec("project.save", {"path": "launcher.json"}, "Save.")
    j.exec("project.open", {"path": "launcher.json"}, "Reopen.")
    assert j.described() == after_capture, "scenes, clips, items and sections survive"
    assert j.lead_points() == chords
    reopened = j.render(end_beat=48)
    assert digest(before) == digest(reopened), "a reopened project renders identically"
    j.exec("launch.scene", {"scene": captured["scene"]}, "Launch it again after reopening.")
    j.wait("the captured scene plays after reopening", lambda s: all(
        r["plays"] == "clip" for r in s.values()))
    j.exec("launch.stop", {}, "Stop every track.")
    j.wait("everything is stopped", lambda s: all(r["plays"] == "stopped" for r in s.values()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("plugins")
    parser.add_argument("--transcript", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="supple-journey-") as workdir:
        client = Client([args.engine, "--mcp", "--workdir", workdir,
                         "--plugin-path", args.plugins])
        try:
            client.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "journey", "version": "1"}})
            client.send("notifications/initialized", notify=True)
            j = Launcher(client)
            ids = scenes(j)
            refusals_before(j, ids)
            clip_fx_envelope(j)
            live(j, ids)
            performance(j, ids)
            first, built, mix = song(j, ids)
            by_hand(j, first, built, mix)
            round_trip(j, first, ids)
            client.close()
        finally:
            if client.process.poll() is None:
                client.process.kill()
                client.process.wait(timeout=5)
        if args.transcript:
            lines = ["# The clip launcher, from scenes to a song", ""]
            for note, tool, arguments, structured in client.transcript:
                text = json.dumps(structured, indent=2).replace(workdir, "<workdir>")
                lines += [f"## {tool}", "", note, "", "```json", json.dumps(arguments, indent=2),
                          "```", "", "```json", text, "```", ""]
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n".join(lines), encoding="utf-8")
    print("MCP launcher journey: scenes, launches, a song from scenes, renders, undo, reopen")


if __name__ == "__main__":
    main()
