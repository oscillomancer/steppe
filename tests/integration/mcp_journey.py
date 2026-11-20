#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Batch 1 acceptance: an agent uses the context track and modulation together.

A scripted MCP client drives the engine's stdio MCP server the way an agent does,
with the dummy audio backend and offline renders only: tracks with stock
instruments and effects, a drum part and a bass part in notation, a chord
progression written as a lead sheet (read back, rewritten, undone and redone),
scale-aware edits on the bass, then modulation (an LFO on an effect parameter, a
follower ducking the bass from the drums, a random source on a send level, a
macro), its refusals, and renders that show each modulation in the audio. The
project is saved and reopened and renders bit-identically.

Nothing here reads a wall clock: the renders are offline and the analysis numbers
come from the render, so the same project gives the same numbers.

  mcp_journey.py <engine> <plugin-dir> [--transcript FILE]
"""

import argparse
import hashlib
import json
import tempfile
from pathlib import Path

from mcp_arrangement import Client

DRUMS = """notation 1
meter 4/4
voice drums ch 10 grid s
1: kick  x...............
1: hat   ..x...x...x...x.
2-8 = 1
"""

BASS = """notation 1
meter 4/4
voice bass ch 1 vel 100
1: C2:e E2:e G2:e B1:e C2:e D2:e F#2:e A1:e
2: C2:q E2:q G2:q B1:q
3: F2:q A2:q D2:q F2:q
4: G2:q B2:q D3:q F3:q
5-8 = 1-4
"""

KEYS = """notation 1
meter 4/4
voice keys ch 1 vel 80 octave 4
1: {Cm7}:w
2: {Cm7}:w
3: {Fm7}:h {Dm7b5}:h
4: {G7}:w
5: {Ebmaj7}:w
6: {Ebmaj7}:w
7: {Ab}:h {G7}:h
8: {Cm7}:w
"""

LEAD_SHEET = """1: key C minor {Cm7}:w
2: {Cm7}:w
3: {Fm7}:w
4: {Fm7}:h {Bb7}:h
5: {Ebmaj7}:w
6: {Ebmaj7}:w
7: {Ab}:h {G7}:h
8: {Cm7}:w
"""

REWRITTEN_BARS = "3: {Fm7}:h {Dm7b5}:h\n4: {G7}:w\n"

PLUGIN = "com.suppleaudio."


class Journey:
    def __init__(self, client):
        self.c = client

    def exec(self, command, args=None, note=""):
        return self.c.call("exec", {"command": command, "args": args or {}},
                           note or command)

    def refused(self, command, args, code, note=""):
        """A call the engine must refuse with this code, a path and a usable hint."""
        response = self.c.send("tools/call", {"name": "exec", "arguments": {
            "command": command, "args": args}})["result"]
        assert response.get("isError"), (command, args, response)
        error = response["structuredContent"]
        self.c.transcript.append((note or f"{command} refused", "exec",
                                  {"command": command, "args": args}, error))
        assert error["code"] == code, (command, error)
        assert error["message"] and error["hint"], (command, error)
        assert "path" in error, (command, error)
        return error

    def undo(self):
        return self.exec("history.undo", {}, "Undo the last step.")

    def redo(self):
        return self.exec("history.redo", {}, "Redo it.")

    def render(self, **extra):
        args = {"start_beat": 0, "end_beat": 16, "analyze": "full"}
        args.update(extra)
        return self.exec("render.mix", args, "Render four bars and read the analysis.")


def momentary(result):
    """Momentary loudness every 100 ms; the windows before there is sound are null."""
    series = [v for v in result["analysis"]["loudness"]["momentary_lufs"] if v is not None]
    return series


def digest(result):
    return hashlib.sha256(Path(result["path"]).read_bytes()).hexdigest()


def build(j):
    for name, kind in (("Drums", "channel"), ("Bass", "channel"), ("Keys", "channel"),
                       ("Verb", "buss")):
        j.exec("track.add", {"name": name, "type": kind}, f"Add the {name} track.")
    for track, plugin in (("tr_1", "drumlin"), ("tr_2", "marram"), ("tr_3", "marram"),
                          ("tr_3", "gain"), ("tr_4", "ghyll")):
        j.exec("fx.add", {"track": track, "plugin": PLUGIN + plugin})
    j.exec("item.add", {"track": "tr_1", "bar": 1, "notation": DRUMS}, "Drums, eight bars.")
    j.exec("item.add", {"track": "tr_2", "bar": 1, "notation": BASS}, "A bass line.")
    j.exec("item.add", {"track": "tr_3", "bar": 1, "notation": KEYS}, "Chords on the keys.")
    j.exec("route.add", {"from": "tr_3", "to": "tr_4"}, "Send the keys to the reverb buss.")
    described = j.exec("session.describe")["text"]
    for name in ("Drums", "Bass", "Keys", "Verb"):
        assert name in described, described


def context(j):
    written = j.exec("context.write", {"text": LEAD_SHEET}, "Eight bars of chords in C minor.")
    assert (written["added"], written["removed"], written["unchanged"]) == (11, 0, 0), written
    first = j.exec("context.get", {"from": 0, "to": 32}, "Read the lead sheet back.")
    assert first["text"] == LEAD_SHEET, first["text"]
    at = j.exec("context.at", {"bar": 3}, "What holds at bar 3?")
    assert (at["key"]["tonic"], at["key"]["scale"], at["chord"]["symbol"]) == (
        "C", "minor", "Fm7"), at

    second = j.exec("context.write", {"text": REWRITTEN_BARS}, "Rewrite bars 3 and 4 only.")
    assert (second["added"], second["replaced"], second["removed"], second["unchanged"]) == (
        1, 1, 1, 1), second
    changed = j.exec("context.get", {"from": 0, "to": 32}, "Read it again.")
    assert "3: {Fm7}:h {Dm7b5}:h\n4: {G7}:w" in changed["text"], changed["text"]
    # Bars 1-2 and 5-8 are untouched, with the ids they had.
    assert [p["id"] for p in changed["points"] if p["bar"] in (1, 2, 5, 6, 7, 8)] == [
        p["id"] for p in first["points"] if p["bar"] in (1, 2, 5, 6, 7, 8)]

    j.undo()
    assert j.exec("context.get", {"from": 0, "to": 32})["points"] == first["points"], \
        "undo restores the first lead sheet exactly"
    j.redo()
    assert j.exec("context.get", {"from": 0, "to": 32})["points"] == changed["points"], \
        "redo restores the rewrite exactly"
    assert "C minor" in j.exec("session.describe")["text"]
    return changed


def spelling(j):
    """Notes are spelled by the key at the item's start."""
    j.exec("midi.write", {"item": "it_2", "notation": "voice bass; 1: F#2:q D#2:q A#1:q G#2:q"},
           "Write sharps.")
    flat = j.exec("midi.get", {"item": "it_2"}, "In C minor they read back as flats.")["text"]
    assert "Gb2:q Eb2:q Bb1:q Ab2:q" in flat and "#" not in flat, flat
    j.exec("key.set", {"at": 0, "tonic": "D", "scale": "major"}, "Change to D major.")
    sharp = j.exec("midi.get", {"item": "it_2"}, "In D major, the same notes are sharps.")["text"]
    assert "F#2:q" in sharp and "A#1:q" in sharp, sharp
    j.undo()  # the key
    j.undo()  # the sharps: back to the bass line
    assert "Gb2" in j.exec("midi.get", {"item": "it_2"})["text"]


def scale_edits(j):
    j.exec("midi.write", {"item": "it_2", "notation": BASS}, "The bass line again.")
    original = j.exec("midi.get", {"item": "it_2"})["text"]
    j.exec("midi.edit", {"item": "it_2", "ops": [{"op": "snap_to_scale", "mode": "nearest"}]},
           "Snap the bass to C minor.")
    snapped = j.exec("midi.get", {"item": "it_2"})["text"]
    assert "1: C2:e Eb2:e G2:e Bb1:e C2:e D2:e F2:e Ab1:e" in snapped, snapped
    j.undo()
    j.exec("midi.edit", {"item": "it_2", "ops": [{"op": "fit_to_chord"}]},
           "Fit every note to the chord at its beat.")
    fitted = j.exec("midi.get", {"item": "it_2"})["text"]
    # Bar 3 has Fm7 then Dm7b5; bar 4 is G7; bars 5-8 follow the chords of bars 5-8.
    assert "3: F2:q Ab2:q D2:q F2:q" in fitted, fitted
    assert "8: G2:q Bb2:q Eb3:q Eb3:q" in fitted, fitted
    j.undo()
    j.exec("midi.edit", {"item": "it_2", "ops": [
        {"op": "transpose_degrees", "notes": [0, 1, 2], "degrees": 2}]},
        "Move the first three notes up a third, by the key.")
    third = j.exec("midi.get", {"item": "it_2"})["text"]
    assert "1: Eb2:e G2:e Bb2:e B1:e" in third, third
    j.undo()
    assert j.exec("midi.get", {"item": "it_2"})["text"] == original, "undo restores the line"

    # A looped item sounds the same notes over different chords: refused with a way out.
    j.exec("item.set", {"item": "it_2", "loop": True, "length": 64}, "Loop the bass item.")
    error = j.refused("midi.edit", {"item": "it_2", "ops": [{"op": "fit_to_chord"}]},
                      "midi.context_varies", "A loop plays one note over two chords.")
    assert "chord" in error["hint"], error
    j.exec("midi.edit", {"item": "it_2", "ops": [{"op": "fit_to_chord", "chord": "Cm7"}]},
           "Name the chord, and it goes through.")
    j.undo()
    j.undo()  # the loop
    j.refused("midi.edit", {"item": "it_2", "ops": [{"op": "snap_to_scale", "key": "C nope"}]},
              "scale.unknown", "A key it cannot read.")


def modulate(j):
    lfo = j.exec("modulate", {"target": {"fx": "fx_4", "param": "gain"}, "source": "lfo",
                              "depth": 0.25}, "An LFO on the Gain plug-in's gain.")
    duck = j.exec("modulate", {"target": {"track": "tr_2", "param": "volume"},
                               "source": "follower", "from": "tr_1", "depth": -24},
                  "Duck the bass from the drums.")
    rand = j.exec("modulate", {"target": {"route": "sd_1", "param": "volume"},
                               "source": "random", "depth": -12},
                  "A random source on the keys' send to the reverb.")
    macro = j.exec("modulate", {"target": {"fx": "fx_4", "param": "gain"},
                                "source": "macro", "depth": 0.1}, "A macro on the same gain.")
    listed = j.exec("modulation.list")["modulations"]
    assert [m["modulation"] for m in listed] == [lfo["modulation"], duck["modulation"],
                                                 rand["modulation"], macro["modulation"]]
    assert [m["depth"] for m in listed] == [0.25, -24.0, -12.0, 0.1], listed

    # What feeds a route, and a route whose Mod Out input nothing feeds is inert.
    assert listed[0]["fed_by"][0]["type"].endswith("lfo"), listed[0]
    unfed = j.exec("modulation.add", {"source": {"module": duck["mod_out"], "input": "in_9"},
                                      "target": {"fx": "fx_4", "param": "gain"}, "depth": 0.1},
                   "A route from an input nothing is cabled into.")
    row = next(m for m in j.exec("modulation.list")["modulations"]
               if m["modulation"] == unfed["modulation"])
    assert row["fed_by"] == [] and row["inert"] == "source", row
    j.exec("modulation.remove", {"modulation": unfed["modulation"]}, "Take it out again.")

    # Drumlin's parameters are not modulatable, but its tune is automatable and
    # continuous: a route reaches it through its envelope (ADR-0096 M4), which
    # modulate adds first, in the same undo step.
    tune = j.exec("modulate", {"target": {"fx": "fx_1", "param": "pad1.tune"},
                               "source": "lfo"}, "Drumlin's tune, through its envelope.")
    row = next(m for m in j.exec("modulation.list")["modulations"]
               if m["modulation"] == tune["modulation"])
    assert "inert" not in row, row
    envelopes = j.exec("env.list")["envelopes"]
    assert any(e["target"] == "fx_1" and e["param"] == "pad1.tune" and e["active"]
               for e in envelopes), envelopes
    j.undo()
    assert tune["modulation"] not in [m["modulation"]
                                      for m in j.exec("modulation.list")["modulations"]]

    # Refusals, each with a code, a path and a hint.
    error = j.refused("modulate", {"target": {"fx": "fx_1", "param": "kick_level"},
                                   "source": "lfo"}, "modulation.target",
                      "A name Drumlin does not have: its hint says nothing is modulatable.")
    assert "automate" in error["hint"] and "pad1" not in error["hint"], error
    error = j.refused("modulate", {"target": {"fx": "fx_4", "param": "nope"}, "source": "lfo"},
                      "modulation.target", "A name Gain does not have: its hint lists "
                      "what it takes.")
    assert "gain" in error["hint"], error
    j.refused("modulate", {"target": {"track": "tr_2", "param": "mute"}, "source": "lfo"},
              "modulation.target", "A switch cannot be modulated.")
    j.refused("modulate", {"target": {"track": "tr_2", "param": "volume"}, "source": "lfo",
                           "depth": 100}, "modulation.depth", "A depth out of range.")
    j.refused("modulate", {"target": {"track": "tr_2", "param": "volume"},
                           "source": "follower", "from": "tr_2"},
              "modulation.cycle", "A real cycle: the bass following itself.")
    j.refused("modulate", {"target": {"fx": "fx_4", "param": "gain"}, "source": "follower"},
              "command.invalid_args", "A follower without a track to follow.")
    return lfo, duck, rand, macro


def edit_routes(j, macro):
    """Change a depth, disable a route, remove one."""
    j.exec("modulation.set", {"modulation": macro, "depth": 0.2},
           "Change a depth.")
    j.exec("modulation.set", {"modulation": macro, "enabled": False},
           "Disable one.")
    states = {m["modulation"]: m for m in j.exec("modulation.list")["modulations"]}
    assert states[macro]["depth"] == 0.2
    assert states[macro]["enabled"] is False
    j.exec("modulation.remove", {"modulation": macro}, "Remove it.")
    assert macro not in [
        m["modulation"] for m in j.exec("modulation.list")["modulations"]]


def only(j, active, every):
    """Enable exactly the routes in `active` among `every`."""
    for route in every:
        j.exec("modulation.set", {"modulation": route, "enabled": route in active})


def swing(series):
    return max(series) - min(series)


def crossings(series):
    mean = sum(series) / len(series)
    signs = [v > mean for v in series]
    return sum(1 for a, b in zip(signs, signs[1:]) if a != b)


def measure(j, lfo_made, duck_made, rand_made, macro_made):
    lfo, duck, rand, macro = (m["modulation"] for m in (lfo_made, duck_made, rand_made, macro_made))
    every = [lfo, duck, rand, macro]
    # The follower hears the drums before their fader, so they can be pulled out of the mix.
    j.exec("route.set", {"route": duck_made["send"], "tap": "pre_fader"}, "The follower hears the drums "
           "before the fader.")
    j.exec("track.set", {"track": "tr_1", "volume_db": -80}, "Take the drums out of the mix.")

    # The LFO: only the keys play; 0.5 Hz, so two cycles in four seconds.
    j.exec("track.set", {"track": "tr_2", "mute": True})
    j.exec("module.set", {"module": lfo_made["module"], "params": {"rate": 0.5, "shape": "sine"}},
           "Slow the LFO to 0.5 Hz.")
    only(j, [], every)
    flat = j.render()
    only(j, [lfo], every)
    waved = j.render()
    # The level difference with and without the LFO swings by its depth, four times in the
    # eight seconds (0.5 Hz).
    wave = [a - b for a, b in zip(momentary(waved), momentary(flat))]
    assert swing(wave) > 8.0, wave
    assert 6 <= crossings(wave) <= 10, ("four LFO cycles in eight seconds", wave)

    # Random steps on the send level: the reverb's share moves from step to step.
    only(j, [rand], every)
    stepped = j.render()
    steps = [a - b for a, b in zip(momentary(stepped), momentary(flat))]
    assert max(abs(v) for v in steps) > 0.5, steps
    # The macro is a knob at 0 until moved; at 100 % it adds its depth of the gain's range.
    j.exec("module.set", {"module": macro_made["module"], "params": {"macro_1": 100}},
           "Turn the macro all the way up.")
    only(j, [macro], every)
    raised = j.render()
    lift = [a - b for a, b in zip(momentary(raised), momentary(flat))]
    assert all(5.0 < v < 8.0 for v in lift), lift  # 0.1 of a 64 dB range

    # The follower: only the bass plays, ducked by the hidden drums at each kick.
    j.exec("track.set", {"track": "tr_2", "mute": False})
    j.exec("track.set", {"track": "tr_3", "mute": True})
    only(j, [], every)
    steady = j.render()
    only(j, [duck], every)
    ducked = j.render()
    dip = [a - b for a, b in zip(momentary(steady), momentary(ducked))]
    # The level falls on each kick (one a bar, four in the eight seconds) and comes back.
    assert max(dip) > 2.5 and min(dip) < 0.5, dip
    assert dip_count(dip) == 4, dip
    j.exec("track.set", {"track": "tr_3", "mute": False})
    return flat, waved


def stem_series(j, name):
    """The momentary loudness of one track's stem, from a stems render of the first 16 beats."""
    files = j.exec("render.stems", {"start_beat": 0, "end_beat": 16},
                   "Render the stems.")["files"]
    stem = next(f for f in files if f["path"].endswith(f"-{name}.wav"))
    report = j.exec("analyze.render", {"render": stem["render"], "detail": "full"},
                    f"Read the {name} stem's loudness over time.")
    return [v for v in report["loudness"]["momentary_lufs"] if v is not None]


def stems(j, lfo, duck, rand):
    """A stem or a track analysis keeps what modulates the track (with the drums back in the mix)."""
    every = [lfo, duck, rand]
    j.exec("track.set", {"track": "tr_1", "volume_db": 0}, "Put the drums back in the mix.")
    only(j, [lfo, duck, rand], every)
    tap = j.exec("analyze.track", {"track": "tr_3", "start_beat": 0, "end_beat": 8},
                 "Analyze the modulated Keys track.")
    assert tap["loudness"]["integrated_lufs"] is not None, tap

    # The Bass stem, ducked by the audible drums: it falls at each of the four kicks.
    only(j, [], every)
    steady = stem_series(j, "Bass")
    only(j, [duck], every)
    ducked = stem_series(j, "Bass")
    dip = [a - b for a, b in zip(steady, ducked)]
    assert dip_count(dip) == 4 and max(dip) > 2.5, dip

    # The Keys stem carries the LFO on the effect: its level swings as the LFO does.
    only(j, [], every)
    flat = stem_series(j, "Keys")
    only(j, [lfo], every)
    waved = stem_series(j, "Keys")
    wave = [a - b for a, b in zip(waved, flat)]
    assert swing(wave) > 8.0 and 6 <= crossings(wave) <= 10, wave


def dip_count(dip):
    """How many separate falls of at least 2.5 LU a difference series holds."""
    padded = [0.0] + dip + [0.0]
    return sum(1 for i in range(1, len(padded) - 1)
               if padded[i] > 2.5 and padded[i - 1] <= padded[i] > padded[i + 1])


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
            j = Journey(client)
            build(j)
            context(j)
            spelling(j)
            scale_edits(j)
            lfo, duck, rand, macro = modulate(j)
            measure(j, lfo, duck, rand, macro)
            edit_routes(j, macro["modulation"])
            lfo, duck, rand = (m["modulation"] for m in (lfo, duck, rand))
            stems(j, lfo, duck, rand)

            # Everything on, then twice the same, then saved and reopened.
            only(j, [lfo, duck, rand], [lfo, duck, rand])
            first = j.render()
            second = j.render()
            assert digest(first) == digest(second), "the same project renders the same"
            before = (j.exec("context.get", {"from": 0, "to": 32})["points"],
                      j.exec("modulation.list")["modulations"])
            j.exec("project.save", {"path": "journey.json"}, "Save.")
            j.exec("project.open", {"path": "journey.json"}, "Reopen.")
            after = (j.exec("context.get", {"from": 0, "to": 32})["points"],
                     j.exec("modulation.list")["modulations"])
            assert before == after, "chords, key and routes survive the round trip"
            reopened = j.render()
            assert digest(first) == digest(reopened), "a reopened project renders identically"
            client.close()
        finally:
            if client.process.poll() is None:
                client.process.kill()
                client.process.wait(timeout=5)
        if args.transcript:
            lines = ["# The context track and modulation, together", ""]
            for note, tool, arguments, structured in client.transcript:
                text = json.dumps(structured, indent=2).replace(workdir, "<workdir>")
                lines += [f"## {tool}", "", note, "", "```json", json.dumps(arguments, indent=2),
                          "```", "", "```json", text, "```", ""]
            args.transcript.parent.mkdir(parents=True, exist_ok=True)
            args.transcript.write_text("\n".join(lines), encoding="utf-8")
    print("MCP journey: context, scale edits, modulation, renders, save and reopen")


if __name__ == "__main__":
    main()
