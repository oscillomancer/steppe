# ADR-0014: Recording, input monitoring and the live region

**Status:** Accepted, 2026-09-27 (recording, input monitoring and crash recovery implemented). Spec: `docs/specs/engine.md` §2.1, §4
(recording), `docs/specs/session-model.md` (`track.record`, routes from
`hw_input`). Builds on ADR-0012 (anticipation) and ADR-0004 (JACK inputs).

**Amendment, 2026-10-16:** [ADR-0045](0045-master-playrate.md) defines master
playrate with a shared timeline clock and source conversion in device-time
graphs; model/command support precedes DSP integration.

**Amendment, 2026-10-28:**
[ADR-0051](0051-read-ahead-latency-compensation.md) replaces decision 6's
unimplemented monitoring-latency promise with a backwards read-ahead solve
and clarifies actual recording placement. The former forward solve
delayed live paths to match playback; read-ahead removes that padding.

**Amendment, 2026-11-03:** [ADR-0057](0057-master-recording.md) adds armed
masters: decision 1's capture also takes a master's output (from its node in
the callback, or from a record ring the renderer fills), placed by the
master's own timeline shift instead of the device's input latency.

## Context

Everything renders ahead today (ADR-0012). Recording needs the device's
input captured without loss, and monitoring needs some nodes to run at the
device block size with input that exists only now — the *live region*.
Phase 4 acceptance asks for robust recording (never drop samples, disk
streaming, crash recovery) and live paths at 64 frames.

## Decision

1. **Recording first, independent of monitoring.** The audio callback copies
   each armed track's input channels (from its `record.input` route's
   `hw_input` channels) into a per-track SPSC capture ring of float frames
   sized for 10 s. The callback only copies and counts; an overflow is
   counted and reported (`record.overflow`), never silent. Capture starts at
   the render position the transport reports for that callback minus the
   device's input latency (JACK capture latency + the period), so takes land
   where they were played.
2. **A writer thread per recording session** drains the rings into WAV
   files (RF64 above 4 GiB) under `<project>/media/rec/`, rewriting the
   header every 5 s (and on stop) so a crash leaves a valid file up to the
   last rewrite. Files are named from the track and take number. On stop the
   main thread commits one version: an audio source, a take (and the item
   if new) per armed track, positioned at the capture start; punch ranges
   and loop passes (one take per pass) come with `transport.loop`. The source
   initially has no hash. A background job rereads the closed file, finalizes
   the sidecar, and posts a model-thread metadata completion. Matching stored
   versions receive the hash without a new version or undo step; leased
   snapshots remain immutable. Undo/deletion on the originating branch or
   project replacement drops the update. Saving before completion is valid
   with an empty hash. The job retains a file handle across scratch relocation
   and resolves the current sidecar location on the model thread before its
   worker writes it. Placement/loop metadata remains recoverable while the
   hash is pending.
3. **Crash recovery:** on project open, files in `media/rec/` not referenced
   by the model are offered as recovered takes (`project.recover` lists and
   imports them); the header rewrite makes them readable.
4. **The live region (monitoring).** A node is live when it monitors input
   (an armed track whose `record.monitor` is on or auto) or is fed by a live node
   (children → folder, routes → targets, and the master). Live nodes run in
   the audio callback at the device block size, in topological order;
   everything else keeps rendering ahead. Where an anticipative node feeds a
   live one (e.g. a playback track into the master while a vocal is
   monitored), the renderer also publishes that edge's tap into an
   epoch-tagged chunk ring (as the master's today), and the callback reads
   it at the same timeline sample — anticipative audio is rendered for the
   play position, so it lines up without extra delay. The master, when live,
   is rendered in the callback from those rings plus the live nodes.
5. **Plans carry the classification.** The compiler marks nodes live or
   anticipative and lists frontier edges; a change of monitoring or arming
   is a plan swap like any other edit (live nodes switch at the swap; the
   first callback after a swap to live of a formerly anticipative node plays
   from the ring until its render position is reached, then from the live
   graph — a short crossfade avoids a click).
6. **PDC:** live paths are not delayed to align with playback (spec §1.3);
   a live node's latency is reported in `engine.graph`; recorded material is
   offset by the input latency plus the monitored chain's latency.
7. **Live multiprocessing** (engine.md §2.2) comes after: the callback
   enqueues independent live nodes to the worker pool and helps, never
   blocks; with one or two live tracks the callback alone is enough.

### Two executors over one plan (refined 2026-09-27)

- **Plans are reclaimed by generation.** The main thread publishes plans
  through an atomic pointer and keeps every published plan; a plan is freed
  once both executors (renderer and callback) have acknowledged a newer
  generation. (RtHandoff returns a plan as soon as one consumer moves on,
  which is too early with two.)
- **The renderer adopts first, the callback follows at the horizon.** The
  renderer adopts plan N at its render position R and republishes
  `(N, R)`; the callback keeps playing plan N−1 (its live nodes and N−1's
  frontier rings, which the renderer filled up to R) until its play
  position reaches R, then switches — so an edit takes effect at the same
  timeline sample on both sides, with no gap. A cycle that straddles R is
  split there.
- **Each instance has one owner at a time**, tagged on the instance when an
  executor starts processing it and cleared when it stops. An executor
  processes a stage only if it owns the instance; it starts an instance of
  its class only when the instance is not processing (the other executor
  has let go), retrying on later cycles and passing the stage through
  meanwhile. A plugin that moves between live and anticipative (arming,
  monitoring) therefore changes owner cleanly, at the cost of a pass-through
  gap of up to the render-ahead time — only at those class changes.
- **Frontier rings belong to a plan**: the renderer fills plan N's rings from
  R on; the callback reads N−1's until it switches at R.

## Consequences

- Recording works before monitoring does; monitoring adds the frontier
  rings and a second (callback) renderer over the same plan.
- The audio callback grows from "copy a ring" to "copy rings + run live
  nodes"; its RT safety is tested under rtsan with a dummy backend feeding
  synthetic input.

## Alternatives considered

Recording from the renderer thread (inputs would have to be queued to it
first — the same ring, one more hop); monitoring by running the whole graph
live while any track monitors (simple, but gives up anticipation exactly
when the machine is busiest, during a recording session).

## Implementation notes (2026-09-27)

- `host/engine/src/recording.{h,cpp}`: `FrameRing` (SPSC interleaved
  frames, 10 s), `Recorder` (capture on the audio thread; a writer thread
  drains into `audio::WavStreamWriter` — float32 WAV, header rewritten
  every 5 s and on close; a partial file is readable). `Engine::
  startRecording/stopRecording`; the callback captures only cycles that
  play timeline audio and places the first at its play position minus the
  plan's output latency and the device's output + input latency. Stopping
  waits two callbacks (none for a manual backend) before draining.
- `DummyBackend::Options::input` feeds synthetic input (tests); JACK
  connects physical capture ports and reports capture latency.
- Bus: `track.set {armed, record_input: [1-based channels], monitor}` keeps
  the hw_input route; `transport.record` records every armed track;
  `transport.stop|pause` commits one version ("record") with a media
  source, take (start offset for material before 0) and item per track, and
  returns `recorded: [{track, item, take, file, seconds, lost_frames}]`.
- Tests: contiguous capture with exact placement in both renderers; a take
  recorded over the bus plays back in a render and undoes as one step.
- Not yet: punch/loop takes, RF64, monitoring (§4–5; since done),
  and replaying a recording in a branch merge (the version has no
  invocation to replay).

## Implementation notes: the live region (2026-09-27)

- Compile marks a node live when it monitors (armed, `monitor: on` or
  `auto`, with a hw_input record route) or is fed by a live node; the
  master is live whenever anything is. Monitoring needs the arm, as in
  REAPER (2026-10-06): `on` hears the input whenever the track is armed;
  `auto` (tape style) only while stopped or recording — `processNode` skips
  the input (audio and device MIDI) while playing without a take
  (`Plan::deviceRecording`, set by the callback from the recorder), so the
  node stays live and the timeline plays instead. track.add gives a new
  channel the app's default monitoring (`EngineApp::defaultMonitor`, `on`
  unless the GUI's preferences say otherwise). Monitored input is added to the
  node's input sum (before the pre-FX tap). Stopped, a plan with live nodes
  still runs them (plugins process, items and MIDI do not play). Since
  2026-10-06 every plugin runs while stopped, as hosts run them (a sampler
  loads the file its editor picked only in process()): the callback runs the
  whole plan in callback mode; the renderer runs its share at the device's
  pace, unheard. Starting playback re-locates the renderer's nodes and resets
  their plugins, so playback still equals the offline render.
- `prepare` splits each plan: renderer nodes with their own task graph,
  live nodes in order, each side's instances, and the frontier edges; the
  anticipative engine gives each frontier edge a chunk ring and sets
  `split`, so live nodes read those rings instead of their sources' taps
  (`ChunkRing::pullAt` aligns by timeline sample).
- Plans are kept in a registry and freed once the renderer
  (`stoppedGen_`) and the callback (`callbackGen_`) have both moved past
  them; instance pruning waits for both too.
- The renderer adopts a plan at its render position R (`handAt`,
  `handEpoch` stored in the plan before it is handed over); the callback
  adopts it when its play position reaches R (splitting the cycle there),
  immediately when stopped or after a seek. Each executor keeps its own
  list of owned instances; `host::Instance` carries the owner token;
  `processNode` skips stages the calling executor does not own;
  `startFree` starts an instance only once nobody owns it (a refused start
  is marked and retried with the next plan).
- Tests: with a monitored track, the anticipative engine's output is
  bit-identical to the in-callback reference over 100k frames; monitoring
  is heard while stopped; toggling monitoring while playing moves the
  plugin between executors with no errors; tsan and rtsan clean.
- Class changes (added after round 12): when a node leaves the live
  region (monitoring off while playing), the renderer could take it only
  at its render horizon while the callback still plays the instance up to
  that point. Waiting for the callback to let go passed the track
  through, unprocessed, for the render-ahead time; a fresh instance
  (round 12) lost the plugin's state (e.g. a sampler's loaded
  instrument) and orphaned its open editor, which crashed plug-ins and
  the app. Since 2026-10-04 `load` keeps every node that was live in the
  live region while the transport plays (`compile`'s `keepLive`; the plan
  is `heldLive`), unmonitored, on the same instance; `service()` reloads
  once the transport stops, and the callback, stopped, lets go at once.
  An instance the pool does replace (fx.restart, a VST3 key change) has
  its editor closed on retirement while its window exists, and the UI
  reopens the editor in that window on the new instance.
  Since 2026-11-08 the other direction is continuous too: a node entering
  the live region while playing (arming, monitoring on) kept its owner
  change but lost its sound, because the renderer reset the instance as
  it let go and the callback restarted the newly live nodes, so a held
  timeline note's voice ended and its note-on lay before the handoff. The
  renderer now lets go without `reset()` and carries the node's stage and
  console state into the new plan's live node (it stopped rendering it at
  R, where the callback continues); the callback places such a plan
  without a restart, but only when it adopts the plan exactly at R,
  playing, in the epoch of the carry; a late adoption (after a frontier
  miss), a pause or seek between, or a rate plan (whose renderer moves to
  its hand position) starts the nodes that joined afresh, plug-ins reset,
  so a note whose end fell between R and the callback cannot hang.
  Separately, a plan that stops hearing a strip's MIDI input ends the
  device's voices alone (it ended every voice).
  While every node is live the renderer renders nothing, but it
  keeps its stream and timeline positions one render-ahead span past the
  callback (advanced together, through the loop), so a plan that gives it
  work takes over at the edit horizon, not at the epoch's start (which
  replayed from there).
- Not yet: live multiprocessing (live nodes run on the callback alone).

## Implementation notes: crash recovery (2026-09-27)

- Each recording gets a sidecar, `<name>.rec.json` next to its WAV
  (`{schema, track, start_sample, sample_rate}`), written atomically by the
  writer thread as soon as capture has started. It stays after the take is
  committed: it records where the file belongs.
- The writer flushes to the OS after every drain (20 ms), so a crash of the
  process loses at most the ring's undrained frames; the header may lag by
  up to 5 s. `audio::inspectWavStream` reads a file's header frames and the
  whole frames actually present; `repairWavStream` rewrites the sizes to
  cover them.
- A recording is *recoverable* when it has a sidecar, is not being recorded
  now, and no version in the store (any branch) uses its file. So a take
  undone in this session is not offered (redo still has it); one committed
  but never saved, or never committed because the program died, is.
  (History is not saved with the project, so after a reopen only the saved
  head counts.)
- `project.recoverable` lists them (file relative to the media folder,
  track, start_seconds, seconds including the unaccounted tail, channels);
  `project.recover {files?, track?}` repairs and commits them as one version
  ("recover") at their recorded positions, on the sidecar's track or
  `track` (`recover.no_track` when the original is gone). `project.get` and
  `project.open` report the count as `recoverable`.
- Also fixed on the way: a failed disk write stopped the take silently; the
  frames after it now count as `lost_frames` and the engine emits
  `record.error {track, file, error}`. Takes carry the engine's sample
  rate, so a device at another rate than the project places them correctly.
- Tests: a take recorded for real, lost by undo + save + reopen, is offered
  and recovered at its original position and length (one undo step); a
  crashed file whose header covers half the audio plays in full after
  recovery onto another track; header repair unit test.

## Review round 8 fixes (2026-09-27)

- `stopRecording` no longer frees the recorder on a timeout: the callback
  marks itself in a cycle (`cycleSeq_` odd, seq_cst with the recorder
  pointer), and the main thread waits for the cycle in progress to end; if
  it has not ended after 2 s the recorder is kept and `record.busy` is
  returned (a later stop, or `engine.stop`, finishes it).
- Two armed tracks whose names sanitize alike get distinct files (paths
  chosen in this recording are reserved), and the writer creates files
  exclusively (`fopen "wbx"`), so no take can truncate another's.
- Recovery considers only regular files directly in a real `rec/`
  directory (no symlinks, for the directory or the file; checked when
  listing and again before repair), and inspect/repair open with
  `O_NOFOLLOW`. A file listed twice is `recover.duplicate_file`.
- Anticipative capture records only the input that went with played audio:
  a pull reports how many frames came from rendered chunks; input during a
  renderer dropout (partly or wholly unplayed cycles, while playing) counts
  as `lost_frames`. The take stays on the timeline: its frames are exactly
  what was heard.
- Media resampling (same commit): see engine.md §Media resampling; the
  real-time engine now runs at the device's rate.
