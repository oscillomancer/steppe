# ADR-0057: Masters record their output and play their own items

**Status:** Accepted, 2026-11-03. Contracts: session-model.md §1.4, §3, §5,
engine.md §1.1, §1.3, §2.3, §3 and §4, gui.md §4.2–§4.4. Amends ADR-0019 §1,
§2 and §10 (masters gain items, arm and a stage after the plain gain/pan) and
ADR-0014 §1 (a master's recording captures its output, not device input).

## Context

A master's mix can be rendered offline (`render.mix`) but not printed in real
time. Printing a mix as it plays (a pass through outboard gear, a performance
on the master's controls, a take of a live set, a quick bounce while working)
needs a master that can be armed and recorded, and somewhere for the take to
live. ADR-0019 made channels the only strips with items, takes and record arm,
and gave masters no arrange row.

A print placed on a master must not be processed again: it is already the
master's decoded, processed and faded output. Played at the master's input
sum it would be decoded a second time (ADR-0019 §3 allows only encoded signals
there) and pass through the master's inserts and fader again.

## Decision

1. **Masters can be armed.** `record.armed` is valid on a master; its other
   record settings (input, monitor, mode) and `input_chain` stay channel-only.
   Arming a master records its output when `transport.record` runs, through
   the existing recorder: one WAV per armed master, sidecars, loop passes,
   punch, recovery and the one "record" undo step work as for channels
   (ADR-0014, ADR-0015). Several masters can be armed; each records its own
   take.

2. **Masters hold audio items.** Items, takes and the usual item edits are
   valid on masters; their takes must be audio (or empty). A MIDI source on a
   master is refused with `track.strip_type` and a hint. Busses still hold no
   items.

3. **The master stage order gains one stage.** First to last:
   input sum → C9 Buss → `pre_fx` tap → inserts → mono/polarity →
   `pre_fader` tap → plain gain and pan → **the master's own items** →
   `post_fader` tap → hardware outputs, renders, monitor chain.
   The items are added after the plain stage: they are not decoded, not
   processed by the master's inserts and not changed by its fader or pan. The
   master's mute (and its automation, VCA and solo silencing) ramps them with
   the master's output. Everything that reads a master's `post_fader` tap
   (device outputs, renders, meters, key routes) hears them. Take FX and take
   envelopes on a master's item run as on a channel, with the item.

4. **Lane mute.** A master's `lane_mute` (bool, masters only, default false)
   silences its items and nothing else, so a print can be compared with the
   live mix or kept silent. Like an item's mute it is a plan rebuild, not a
   ramp. Items play alongside the live mix; nothing is replaced.

5. **What is recorded.** The master's output before its own items join: the
   plain stage's output, the signal `render.mix` writes for that master when
   it has no items in the range. Nothing reaches a take from a master's
   earlier prints, ever. While a master records, its items do not play inside
   the span being recorded: within the punch range when punch is on,
   everywhere otherwise; so what the user hears there is what is recorded.
   Outside the punch range they play as usual (heard, not recorded). The
   engine applies this as a plan rebuild when recording starts and stops.

6. **Capture and placement.** The audio callback stays the recorder's only
   producer and captures a master's frames as they are played, so a seek, a
   dropout or a flushed render-ahead never reaches a take. Where the master
   runs in the callback (the in-callback engine, or a live master), the
   callback copies its node buffer. Where the renderer runs it, the renderer
   publishes the master's output per block into a record ring of the plan
   (epoch- and stream-tagged, like a frontier ring, ADR-0014), published
   before the plan's root or frontier chunk; the callback pulls it at the
   stream position it plays, whether or not it is recording, so the ring
   never fills. A frame the master produced at processing position P belongs
   to timeline P − (S + chain), the master's own timeline shift and chain
   latency (ADR-0051): no device latency is involved. The recorder starts
   every file of a recording at the same timeline position (the play position
   minus the round trip, as today) and writes a master's frames
   (round trip − S − chain) frames later in its file, the mechanism input FX
   already use, so loop runs and punch need nothing new. A take is therefore
   bit-identical to `render.mix` of that master over the same span when the
   master has no items there.

7. **Renders.** `render.mix` of a master includes its items, so a render
   matches playback, and region renders follow it. Stems of channels and
   busses (isolated or through the master) leave every master's items out:
   they belong to no channel. A stem whose track is a master is that
   master's mix and keeps its items.

8. **Commands.** `track.set {track, armed}` accepts masters;
   `track.set {track, lane_mute}` is masters only (`track.strip_type`
   otherwise). `item.add` with `audio_file`, `item.move`, `item.paste`,
   `take.*` and `project.recover` accept masters; MIDI there is
   `track.strip_type`. `transport.record` records armed masters as well as
   channels. Retyping a master clears `lane_mute`; a master with items cannot
   become a buss.

9. **GUI.** A master strip in the mixer gets the arm button, in the row where
   channels have it. A master that is armed or holds an item gets an arrange
   row: its lane, at the top of its subtree where its automation lanes already
   appear (above its children, which keep their indentation), with a TCP
   header holding its type badge, name, arm button and the lane mute. Items on
   it are edited as on a channel's lane. A master with neither has no row, as
   before.

10. **Project file 2.5.** `supple.project/2.5` adds `lane_mute`. A 2.4 file
    loads unchanged (no master has items, arm or lane mute).

## Consequences

- A mix can be printed in real time onto its own master, and the print sits
  beside the live mix with a one-click comparison (lane mute).
- The plan checker accepts items on masters only at the new stage, never at
  the input sum, and still rejects items on busses and anything between an
  encode and its decode.
- An armed master that the renderer runs costs one stereo ring and a copy per
  render block, consumed by the callback while it plays.
- After a print the master plays the take alongside the live mix, which
  doubles it until the user mutes the lane or the live sources; this is the
  intended comparison, not a replacement.
- Older applications cannot open a project that uses the feature: their strip
  rules refuse items and arm on masters. A project that does not use it
  opens there as before.
- A plan change that moves a master's latency while it records shifts the
  rest of the take by that much, as an output-latency change does for a
  channel's take.
- Starting a recording while already playing lets an earlier print be heard
  for up to the render-ahead span before the rebuild silences it; it is never
  recorded.
- Masters wider than two channels record their main pair.

## Alternatives considered

- **Record the output including the master's items** (what the device
  plays). Inside the recorded span this is the same signal (the items are
  silent there); outside it the file would carry the old print, which a take
  extended later, or a crash recovery (which has no punch range), would
  bring back doubled.
- **Make an armed master live** so the callback computes it. Simpler capture,
  but every armed master's inserts would move to the callback, and bridged
  plug-ins on live nodes pass through (engine.md §5), so the take would differ
  from the render.
- **Record in the renderer.** The renderer works up to 200 ms ahead; audio
  flushed by a seek or played across a dropout would reach the take.
- **Items at the master's input sum.** A print would be decoded again and
  processed by the master's chain and fader twice, and ADR-0019 §3 would
  break.
- **A separate "print" track type.** More model, commands and GUI for what is
  an item on the master it came from.
- **`record.mode: output` on channels** (REAPER's output recording) is a
  different feature and stays unsupported (`record.output_unsupported`).
