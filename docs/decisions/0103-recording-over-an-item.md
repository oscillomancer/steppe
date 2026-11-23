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
   looped, frozen, or holds the other kind of material (MIDI under an audio pass), the new item
   carries a `record.stack_skipped` warning naming the item and why, so a doubling is never silent.
   A frozen track takes no recording: its pass is left out with `record.frozen_skipped`, the recording is
   kept where it was made, with its journal, for `project.recover` to place after unfreezing, and the
   other tracks' takes are placed (a
   frozen track used to make the whole multitrack commit fail, losing every track's recording;
   arming a frozen track, and freezing an armed one, are refused, so this is a backstop; decided
   2026-11-23, round 132). A commit that fails anywhere (a refused nested `take.comp` included) is
   rolled back whole: the project stays as it was and editable, and the recording is not in it
   (its journals remain for recovery; the refusal's message says so and names project.recover).
2. **The item's extent does not change.** Its position and length stay; the new take reads the
   recording by the timeline mapping (`start_offset` negative when the pass starts after the item,
   so the first part is silent; positive past the end of a short pass). What lies beyond the item
   stays in the file, hidden, and the commit warns `record.take_beyond_item`.
3. **A comp run** (adjacent pieces holding the same takes) gets the take on every piece, so it stays
   take-compatible and `take.comp` keeps working. A locked piece makes the whole pass a new item.
4. **Which take is heard: where it was recorded, and nothing else.** (Decided 2026-11-23 on round
   132, replacing "the new take is heard on every piece the pass touches", which left the rest of
   the item silent: a pass over bar 5 of eight bars made bars 1-4 and 6-8 mute.) In the commit's
   own transaction the pass's stretch is comped to the new take through `take.comp` (the default
   crossfade for a lone item, the comp's own crossfade and shape for an existing comp, hard cuts for
   MIDI, `bypass_groups`), and everything outside it keeps playing what it played, in a comp as
   well: the item becomes the pieces before, the pass, and after, as `take.comp` makes them (one
   undo step). The seams of an existing comp are rebuilt from the crossfade `take.comp.get`
   reports (a user's 40 ms linear comp stays 40 ms linear; seams that differed by hand become
   uniform, as any `take.comp` makes them, and `record.comp_uniform` says so). A pass that covers the whole of a lone item makes the
   new take heard whole with no comp; over a whole comp of several pieces it goes through
   `take.comp` with a single span, which joins the neighbours that now play one take (kept apart
   they would swell by 3 dB across each old equal-power seam). Loop passes are appended in order, one
   take per pass, and the newest pass is heard over each stretch of the loop range, older passes
   staying as takes. The new take is heard also when the target's active take followed the
   context (ADR-0098): the following take stays as another take, untouched, over the rest, and
   `record.follow_kept` says so. A comp the commit made carries `take.comp`'s own warnings.
5. **MIDI**: the take is a new source with the played notes only, in a copy for each piece of a run.
   `midi_overdub` keeps its own rule (the played notes merged into the active take's, a new take),
   and its `record.follow_kept` leaves a following item alone as before.
6. **The setting** is the engine's (`EngineApp::setStackRecordedTakes`, default true) and is read
   and set by agents with `record.get` and `record.set {stack_takes}` (a machine setting, not an
   undo step, not in the project). It lasts for the process. Keeping it between runs is a client's
   job, to be built with the GUI's Preferences row (`stack_recorded_takes`); until then nothing
   does, and a headless session always starts with the default. Off: every pass is laid as items,
   as before.
7. **Replays** do not read the setting. The decision is made at commit time and stored in
   `record.commit`'s arguments (`stack`); a history without it replays laid as items. A replay on a
   session where the target is gone lays a new item. A recovery (`project.recover`) never stacks: it
   puts files back, and the user decides what they are.

## Consequences

- Recording a fix over a bar leaves one item with another take, ready for `take.comp` or
  `take.activate`; nothing needs muting.
- A punch-in inside an item is heard over the punch range and nowhere else: the item plays what it
  did before and after it, with the new take in the middle, and `take.comp` or `take.activate`
  can still choose differently.
- **Edit groups.** Stacking is per recorded track. Recording only some tracks of an edit group
  leaves their take counts out of step (the others get no take), so a later group `take.comp` that
  plays the new index is refused with `comp.group_takes` naming the tracks that lack it; indices
  they have still comp the group. This is a stated limit: the commit warns `record.stack_group`
  naming the tracks left behind, and arming every track of the group keeps them level. (A silent
  placeholder take on the others is a possible later answer.)
- Existing recordings, projects and histories are unchanged; the default changes what a new
  recording over an item does, and `record.set {stack_takes: false}` restores the old behaviour.
- The result of a recording lists `stacked: true` when the takes went into an existing item.
