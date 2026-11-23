# ADR-0015: Transport loop, punch, and loop recording into takes

**Status:** Accepted, 2026-09-27. Spec: `docs/specs/engine.md` §4 (recording:
"punch in/out and loop recording (one take per pass)"),
`docs/specs/session-model.md` §7 (`transport.loop {range}`). Builds on
ADR-0012 (anticipation), ADR-0014 (recording, the live region).

**Amendment, 2026-10-16:** [ADR-0045](0045-master-playrate.md) defines master
playrate with a shared timeline clock and source conversion in device-time
graphs; model/command support precedes DSP integration.

## Context

Phase 4 asks for punch-in and loop recording into takes. Playback has no
loop today: the timeline position only moves forward, except when seeking.
Three executors care about the position: the in-callback engine
(`cycle`), the renderer rendering ahead (chunks carry their timeline
start), and the callback's live region (it reads frontier rings by
timeline sample with `ChunkRing::pullAt`, which pops chunks that lie
behind the position it asks for). A wrap from the loop end back to its
start moves the position backwards, which `pullAt` would read as "every
chunk is behind".

## Decision

1. **Loop and punch are transport state, not session state.** Like the
   play position, they are not versioned (no undo step for moving the loop)
   and are not saved in the project file yet (a later view-state file can
   hold them). `transport.loop {start, end, enabled}` and `transport.punch
   {start, end, enabled}` (positions as seconds, beat or bar like
   `transport.seek`); `transport.get` reports both.
2. **Loop ranges are epoch-scoped.** The main thread writes the loop range
   and bumps the epoch, exactly as a seek does, so the renderer and the
   callback always agree on the range for the audio they handle: a reader
   takes the range with a seqlock read against the epoch. Changing the loop
   while playing flushes what was rendered ahead (the same short
   re-priming as a seek). A play position already past the loop end plays
   on (as in REAPER).
3. **Wrap without a gap; compare stream positions, not timeline ones.**
   Every executor splits its block at the loop end and continues from the
   loop start in the same block (a block that starts at or after the loop
   end never wraps, so playing from past the loop plays on):
   - the in-callback engine splits `processBlock` at the end and moves its
     event and automation cursors to the loop start (no plugin reset, no
     cleared delay lines: tails ring on); notes still held at the end get
     note-offs at the wrap (each node tracks its held notes);
   - the renderer never renders a chunk across the end: it emits a short
     chunk up to the end, wraps, and goes on in the same epoch, so the ring
     stays full and playback does not re-prime;
   - the live callback splits its cycle at the end too.
   Positions compared *across* executors become **stream positions**:
   frames since the epoch began, which only grow. Chunks carry their
   stream position next to their timeline start; how far the renderer is
   ahead, the plan hand-over point (`handAt`, ADR-0014) and
   `ChunkRing::pullAt` use stream positions; MIDI, clips, automation and
   the reported transport use timeline positions. Without a loop the two
   differ by the epoch's start, so nothing changes.
4. **Punch** is applied when the takes are committed: the recording keeps
   running and the file keeps everything, but the items cover only
   `[punch start, punch end)` (a take can be extended later to reveal the
   rest). Capture itself needs no punch logic on the audio thread.
5. **Segments.** The recorder captures only what is heard (ADR-0014 review
   round 8) and now also where on the timeline each captured run lies: a
   new segment starts whenever the captured position does not continue the
   previous one (a loop wrap, the edges of the punch range). The audio
   thread appends `(file frame, timeline sample)` to a preallocated segment
   table (4096 entries; overflow merges into the last segment and is
   reported). The WAV file stays one contiguous file per armed track.
6. **Takes per pass.** On stop, a recording without wraps commits as today
   (one item per segment). A recording with wraps commits, per armed track,
   **one item covering the loop range** (intersected with the punch range
   when on), with one take per pass: each take references the same media
   source, its `start_offset` placing the pass's first frame at its
   timeline position (negative when a pass starts after the item start, so
   the part before it is silent; the clip reader already plays negative
   offsets as silence). The newest complete pass is the active take (the
   last pass if none is complete). The recorded answer lists every take.
7. **Recovery** keeps working: the sidecar gains the segment table when the
   recording ends; a crash before that recovers the file as one take at its
   first position (as today).

## Consequences

- Loop playback also works without recording (a musician looping a
  section), in both modes and with monitoring.
- The CLAP transport gets loop flags and range (`CLAP_TRANSPORT_IS_LOOP_ACTIVE`,
  loop start/end in beats and seconds).
- Offline renders ignore the loop (a render covers its range once).

## Alternatives considered

Timeline positions only, with every consumer splitting at the wrap (the
hand-over point and "how far ahead" become ambiguous once the position can
go back: a plan adopted just after the wrap would be taken up by the
callback a whole pass early); looping only in the renderer and letting the
callback re-prime at each wrap (audible gaps at every loop pass); one file
per pass (more files, and a take change mid-recording would have to open
files on the writer thread on demand; one file with offsets is simpler and
matches recovery); resetting plugins at each wrap like a seek (cuts reverb
and delay tails at every pass).

## Implementation notes (2026-09-27)

- `LoopRange` (realtime.h): `until(pos, n)` (frames before the loop end),
  `next(from, to)` (where playback continues). Every epoch bump (seek,
  pause, `setLoop`) happens inside one sequence lock together with the
  seek target and the loop (`writeTransport`, main thread only), so a
  reader gets an epoch with exactly its seek and loop (review round 13: a
  loop published before its epoch bump could pair with the previous
  epoch). The renderer reads the snapshot at each relocation and takes its
  epoch from it; the callback reads it when it sees a new epoch
  (anticipative) or a new sequence (in-callback) with at most four tries —
  a write in progress keeps the old epoch and loop for another cycle
  instead of spinning on the main thread (a priority inversion). The
  callback also takes the loop at `start` (it used to miss a loop set
  before the engine started).
- Stream positions: `Chunk::stream`, the renderer's `streamLocal_`, the
  callback's `rtStream_`/`liveStream_`; `handAt`, render-ahead and
  `ChunkRing::pullAt` use them. `pullAhead` stops at a timeline
  discontinuity (a wrap) so each pull is one contiguous run.
- `detail::wrapTo(plan, pos, class)` moves event and automation cursors
  (delay lines and plugins untouched); `Node::held` counts delivered notes
  per channel/key and `releaseHeld` sends their note-offs at the start of
  the next block. Seeks still reset plugins as before.
- The in-callback engine now also offsets the monitored input per part of a
  split cycle (a bug the loop test found: the second part read the cycle's
  first frames).
- Recorder: a new run whenever the captured position does not continue the
  previous one (4096 preallocated entries); runs start `latency` frames
  later in the file than where the output wrapped. `RecordedTake` carries
  `segments` and the loop; the sidecar is rewritten with both when the
  recording ends. `commitTakes`: passes → one item on the loop range with a
  take per pass (`start_offset` negative for a late first pass), the newest
  whole pass active; other runs (e.g. a seek while recording) → an item
  each; everything clipped to the punch range and to the timeline start.
- Bus: `transport.loop` / `transport.punch {enabled, start_*/end_*}`
  (seconds, beat or bar; kept in `EngineApp`, re-applied when the engine
  starts); `transport.*` results include `loop` and `punch`; recorded takes
  list every pass (`takes`).
- Tests: loop playback equals the offline render unrolled (in-callback and
  anticipative, with and without plugin latency, wraps mid-block and short
  chunks); a note held across the loop end is released; anticipative
  monitoring with a loop is bit-identical to in-callback; loop recording
  yields contiguous files and the expected runs; over the bus, a take per
  pass on one item, newest whole pass active, punch trims.
- Plugins see the loop: `host::Transport` carries it and the CLAP
  transport sets `CLAP_TRANSPORT_IS_LOOP_ACTIVE` with the loop's beats and
  seconds (each executor passes its own loop).
- Not yet: loop passes that span a loop change, MIDI recording.

**Note, 2026-11-23:** a pass that starts inside an existing item is now a take of that item, not a
new item layered over it (ADR-0103; `record.set {stack_takes: false}` keeps the layering described
in §6).
