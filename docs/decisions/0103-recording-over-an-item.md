# ADR-0103: Recording over an item makes a take of it

**Status:** Accepted, 2026-11-23. Amends ADR-0015 §6 and ADR-0016 §4. Designed in
`reports/builder-takes-design.md` §8 from Nate's answer (Q6); the co-director accepted it with the
differences listed under "Decisions".

Contracts: `specs/session-model.md` "Recording over an item (ADR-0103)" and "Recording" under
"Clip context"; `specs/engine.md` (`record.commit`); the commands `record.get` / `record.set`.

## Context

A recording pass was always laid as new items (one per run, or one on the loop with a take per pass,
ADR-0015 §6; ADR-0016 §4 for MIDI). Recording over an existing item layered them: both played, and
fixing a bar meant a second item to mute, trim and comp by hand. `take.comp` (session-model §1.5.1)
chooses between the takes of an item, so the natural place for a second pass over the same bars is
another take of the item that is already there. `midi_overdub` already did that for MIDI, merging the
notes into the item under the pass.

## Decision

A pass that starts inside an item becomes a **take of that item**, when the setting `stack_takes` is
on (the default), per armed track and per placed span (the pass after the punch range and the
timeline start have clipped it).

1. **The target** is the item of the track, on the main lane, whose span contains the span's first
   heard sample (the anchor `midi_overdub` uses). The pass is laid as a new item, as before, when
   there is none. When the pass starts before an item and runs into it, or the target is locked,
   looped, or holds the other kind of material (MIDI under an audio pass), the new item carries a
   `record.stack_skipped` warning naming the item and why, so a doubling is never silent.
2. **The item does not change.** Its position and length stay; the new take reads the recording by the
   timeline mapping (`start_offset` negative when the pass starts after the item, so the first part
   is silent; positive past the end of a short pass). What lies beyond the item stays in the file,
   hidden, and the commit warns `record.take_beyond_item`.
3. **A comp run** (adjacent pieces holding the same takes) gets the take on every piece, so it stays
   take-compatible and `take.comp` keeps working; a piece the pass does not touch is silent there
   and keeps its heard take. A locked piece makes the whole pass a new item.
4. **Which take is heard**: on the pieces the pass touches, the newest whole pass, else the last (the
   loop rule). The new take is heard also when the target's active take followed the context
   (ADR-0098): the following take stays as another take, untouched, and `record.follow_kept` says so.
   Loop passes are appended in order, one take per pass.
5. **MIDI**: the take is a new source with the played notes only, in a copy for each piece of a run.
   `midi_overdub` keeps its own rule (the played notes merged into the active take's, a new take),
   and its `record.follow_kept` leaves a following item alone as before.
6. **The setting** lives in the app's preferences (`stack_recorded_takes`, default true), is pushed into
   the engine (`EngineApp::setStackRecordedTakes`, default true) and is read and set by agents with
   `record.get` and `record.set {stack_takes}` (a machine setting, not an undo step, not in the
   project). A session with no preference file (headless, tests) uses the default for the process;
   `record.set` lasts for the process. Off: every pass is laid as items, as before.
7. **Replays** do not read the setting. The decision is made at commit time and stored in
   `record.commit`'s arguments (`stack`); a history without it replays laid as items. A replay on a
   session where the target is gone lays a new item. A recovery (`project.recover`) never stacks: it
   puts files back, and the user decides what they are.

## Consequences

- Recording a fix over a bar leaves one item with another take, ready for `take.comp` or
  `take.activate`; nothing needs muting.
- A punch-in inside an item is heard as the new take for the whole item (silent outside the punch):
  comp it with `take.comp`. The GUI step may add a comp of the punch range on top.
- Existing recordings, projects and histories are unchanged; the default changes what a new
  recording over an item does, and `record.set {stack_takes: false}` restores the old behaviour.
- The result of a recording lists `stacked: true` when the takes went into an existing item.
