# ADR-0056: Masters play to hardware outputs; a mix renders several masters

**Status:** Accepted, 2026-11-03. Contracts: session-model.md §1.4, §1.8,
§1.10 and §5, engine.md §2.3 and the native device backend section, gui.md
§4.4 and §4.8. Amends ADR-0019 §4 (the `hw_output` route form) and extends
ADR-0055 (the `masters` render setting).

## Context

ADR-0019 allowed several masters and said masters "feed hardware outputs",
with routes `{from: master, to: {hw_output: …}}` as the means. Nothing
realized those routes: the engine wrote only the main master (tr_0) to the
device's first two channels, no command created such a route, and the GUI had
no control for it. A second master was silent while playing and could only be
rendered one at a time with `render.mix {master}`. Typical uses for a second
master (a headphone cue mix, a second pair of monitors, a broadcast feed, a
stem bus sent to another device output) were impossible.

The render dialog's Master mix source rendered tr_0 only.

## Decision

1. **A master's outputs are a field of the master, not a route.** Masters
   carry `hw_output`: the device output channels their main pair plays to,
   1-based: two (left, right), one (mono, (L+R)/2) or none (`[]`). When the
   field is absent the main master plays to 1/2 and every other master to
   none, so existing projects sound the same. A field keeps one obvious place
   for the setting, needs no route id in new projects, and lets "none" be
   said for the main master. Several masters may share channels; they add.
   Masters wider than two channels still send only their main pair.
   `track.set {track, hw_output}` edits it (undoable); `track.list` shows the
   resolved value of every master.
2. **The route form goes.** Routes to `hw_channels` are refused
   (`route.hw_output`). Files before schema 2.4 that contain one from a master
   load with the master's `hw_output` set from it (the first such route) and
   a `project.hw_output_route` warning. The project schema becomes 2.4.
3. **Output masters play in step.** The main master and every master with
   outputs are latency anchors: they share one output latency, the largest of
   theirs, and the latency solve (ADR-0051) schedules each so its output
   reaches the device at that time. With only the main master this is its
   own latency, as before. A key route from one output master into another's
   path can pull the first earlier; the plan warns with the sample count
   rather than adding an output delay line for that rare case.
4. **Every executor writes every device channel.** In-callback, anticipative
   and split (live) paths write the plan's output masters into their channels
   and silence into the rest; the anticipative root ring carries all device
   channels. A channel's first master is copied, later ones add, so a single
   master stays bit-identical to its offline render. Masters with outputs
   are live whenever anything is live, since the callback writes them. The
   metronome stays on outputs 1/2.
5. **The engine opens the device's outputs.** JACK registers one output per
   physical playback port (2 to 64) unless `engine.start {outputs}` says
   otherwise; the GUI asks PortAudio for every output of the chosen device (up
   to 64). `engine.inputs` lists `outputs` with their port names. A master
   whose channels exceed the device's outputs plays nowhere, with an engine
   warning that says so.
6. **The GUI picks outputs from a menu.** A master strip's **out** button in
   the mixer opens a menu of the device's outputs (None, the stereo pairs
   1/2, 3/4…, then each output alone as mono), not the routing matrix: masters
   have no sends. The button names the current choice ("out 3/4").
7. **A mix renders the masters chosen.** The render setting `masters` lists
   the masters the mix renders, one file each (absent: the main master; not
   kept in presets). With more than one, a pattern without `{track}` gets
   `-{track}`, the master's name, so the files differ. `render.mix {master}`
   still renders one master for that call. The dialog shows a masters
   checklist under Master mix when the project has more than one master.

## Consequences

- A cue or monitor mix is a master with its own outputs, faders and FX, heard
  live and rendered on its own or with the main mix.
- Opening every device output costs some buffer memory per channel and a copy
  of silence for unused channels each cycle; 64 channels is the cap.
- Old projects with no `hw_output` load and play unchanged; projects with the
  never-played route form gain the output they asked for.
- Per-route output gain and outputs for auxiliary channels (3+) of wide
  masters are not provided.
