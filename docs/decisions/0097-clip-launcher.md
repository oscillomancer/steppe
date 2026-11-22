# ADR-0097: Clip launcher: scenes and clips beside the arrangement

**Status:** Proposed, 2026-11-19, for review by opus-reviewer and the lead.
Revised the same day after review round 55 (F1 to F9), with the
co-director's rulings.
It extends ADR-0057 (master items) and ADR-0093 (values on a running plan),
and touches ADR-0013 (bridge deadlines) only through the executors it
shares. It is built in steps L1 to L4 (below). The engine's "Playing"
section was written from the core design and the lead's core check, and
the lead revised it with L2 (2026-11-19).

**Amended 2026-11-20** (below, "Amendment: a clip take's chain"): a clip
take's FX and envelopes play, and a launch's floor grows by the latency of
the track's clip chains; envelopes on a clip take's FX are in clip time
(C2, schema 2.19).

Contracts: `specs/session-model.md` "Scenes and clips (schema 2.18,
ADR-0097)", `specs/engine.md` §1.4.2, `specs/gui.md` §4.15.

## Context

Nate asked for a clip launcher: material that can be played in any order
before it has a place in the song, and laid into the song when it has one.
The working design is `headline-core.md` §3 with amendments L-a to L-h, G-c
and G-d. It is checked against the code in
`reports/survey-pitch-time-clips.md` (the clip model, §2 Q5–Q7 and §3.8–3.11)
and `reports/core-check-lead.md` (B1–B5).

Four things decide the shape:

- **Timeline code assumes items are on the timeline.** About 60 sites walk
  `Session::items` and read a position (survey Q5). Among them are the
  project length (`lastItemBeat`), describe, render ends, freeze length,
  group-linked edits, the plan's `trackEvents`, and the GUI's lanes. An
  item "in a slot" would be read as a timeline item by every one of them
  that is missed, and the invariants require every item to have a finite
  timeline position (session.cpp:778).
- **Takes already carry everything a clip plays:** sources, take FX, take
  envelopes, MIDI content, playrate and pitch. The editors and the content
  commands work on takes through their item.
- **Compiled audio is absolute.** `Clip.start` and `TimedEvent.sample` are
  fixed timeline samples (plan.h:672-693). A launched clip starts at a
  run-time position, so the item compile is not reused as is.
- **The engine's laws.** Renders are deterministic, and real time is
  bit-identical to offline in both executors and through the bridge, at any
  block size. A launch has to be a timed message that both executors
  resolve to the same sample.

## Decision

### Model: two new kinds

A **scene** (`sc_*`, collection `scenes`) is a column: `{name, color,
order}`. A **clip** (`cl_*`, collection `clips`) is one cell: a track's
material for one scene.

| Clip field | Meaning |
|---|---|
| `track` | A channel (required). |
| `scene` | Its scene (required). |
| `kind` | `midi` or `audio` (required), fixed when the clip is made. |
| `takes`, `active_take` | Ordinary takes (`tk_*`), as an item's are. A clip with takes has an active take. |
| `length` | The clip's period, > 0. It is in the clip's unit (below). |
| `loop` | `{on, start, length}`: the content window that repeats inside the clip, measured from the take's start offset. Default off. |
| `mode` | `loop` (default): it repeats every `length` until something else is launched. `once`: it plays `length` once, then the track stays held by the clip, silent, until the next launch. |
| `quantize` | Optional override of the project's `launch_quantize` (below). |
| `name`, `color`, `mute`, `gain_db` | As an item's. |

- **The clip's kind and unit.** A clip stores its `kind`, `midi` or
  `audio`, chosen by `clip.add` and never changed after. Its `length`,
  `loop` and take envelopes are in the kind's unit: beats for a MIDI clip,
  which follows the song's tempo, and seconds of played take time for an
  audio clip, which plays at the take's own rate (L-c). Played time is
  source time divided by the take's playrate. A take whose source does not
  match the kind is refused (`clip.mixed`); takes without a source are
  allowed in either kind. So the unit never changes under a clip: no take
  edit rereads its numbers. `clip.add` with no source makes a MIDI clip.
- **The project** gains `launch_quantize`: `bar` (default), `beat`, `1/2`,
  `1/4`, `1/8`, `1/16`, `1/32` or `none`.
- **A clip is not an `Item`.** Timeline code stays correct by construction;
  survey §3.8's cost is moved onto takes, below.
- **What is playing is transport state,** like the loop range. It is not
  saved, not in history, and not in branches or merges. Scene and clip
  edits are ordinary model edits: undo, history, diff, merge by replay and
  branches treat them as they treat items.
- **The words.** The user-facing words are "clip", "scene" and "clip
  launcher", always in full, because "the launcher" already names the
  process that starts the app (ADR-0091). The plan's existing `Clip`
  (prepared item audio, plan.h:673) may be renamed inside the engine if the
  lead finds the clash confusing; no command or file key changes for it.

### The take owner

A take belongs to exactly one **owner**, an item or a clip. One
abstraction, `model::takeOwner(session, take) → it_* | cl_*` with
`model::ownerTakes(session, owner)` and `model::TakeOwners` (one scan for
many lookups), replaces the five scans that found a take's item (L1a):
- `takeOwners` (prepared_sources.cpp);
- `itemOfTake` (take_commands.cpp);
- `owner` (audio_commands.cpp);
- `checkUnlockedTake` (command_validation.h);
- the warp-peer check in `validateAudioWarp` (audio_warp.cpp).

The orphan-take invariant (session.cpp:403) counts both owners. Moving
material between an item and a clip **copies** takes, never shares them. A
copied MIDI take gets its own copy of an unpooled source; a pooled source,
or one copied with `pooled: true`, is shared. Audio media sources are shared
as they are between items.

**Commands that accept a clip.** These are the content commands. They take
a `cl_*` where they take an item today (the same `item` field, whose ID
pattern becomes `^(it|cl)_[0-9]+$` so that merges remap it), or a clip's
take:
- `midi.get`, `midi.write`, `midi.edit` and `midi.notes`. A clip keeps its
  length under `midi.write`, as an item keeps its position and length; notation
  longer than it is kept but does not play, and `midi.write` warns
  (`midi.beyond_length`).
- `item.export`: the clip's raw audio or its MIDI, as the item it would
  make at the project's start (one period, looped as an item loops).
- `take.list`, `take.add`, `take.activate`, `take.set` and `take.remove`.
  A clip goes with its last take, as an item does.
- `take.extract {take, position, track?}`: the take leaves the clip for a
  new item at `position` (beats, required for a clip's take, since a clip
  has no place in time) on `track` (default: the clip's track). Its length,
  loop and envelopes convert as below. A clip goes with its last take.
- `take.hits.detect` and `take.hits.set`.
- `fx.add {take}`, `env.add {take}`, and the `fx.*` and `env.*` commands on
  a clip take's FX and envelopes. A take envelope on a clip is in the
  clip's unit from the clip's start: its points give `beat` on a MIDI clip
  and `seconds` on an audio clip. `bar`, and the other unit, are refused
  (`clip.unsupported`), since a clip has no place in time.
- The editors: the piano roll, the inspector and the take FX window. They
  open a clip as they open an item. A clip in the piano roll uses the
  project's first key (an addition to §5 of the core design).

**Refused on a clip's take in the first steps** (`clip.unsupported`, with a
hint to lay the clip on the timeline first): `take.stretch.add/move/remove`,
because the clip player does not warp. A clip whose take has stretch
markers cannot be made (`clip.add {item}` refuses it, L-g).

**Converting between an item and a clip.** `clip.add {item}` copies an
item's takes into a clip; `item.add {clip}` and `take.extract` copy or move
a clip's takes onto the timeline. One rule covers all three. It converts
through the tempo map at the item's span: where the item sits, or where the
new item lands (`itemSpan`, so the item's timebase is resolved first).
- **Kind.** The clip's kind is its takes' content: `midi` for MIDI sources,
  `audio` for media. An item whose takes have no source makes a MIDI clip.
- **Length.** A MIDI clip's length is the item's length in beats (its end
  beat minus its start beat). An audio clip's is the item's length in
  seconds (end second minus start second). Back on the timeline, the new
  item spans the clip's length from its position, in beats or in seconds.
- **Notes and audio** do not convert: a MIDI source is in beats, and an
  audio take plays its source at its own playrate in both places. The
  playrate, pitch, start offset and take FX are copied as they are. The
  item's gain, mute and colour become the clip's, and back; its fades have
  no place on a clip and are not kept.
- **Loop.** An item's `loop` repeats its take's content from the start
  offset. It becomes a clip loop that is on, from 0, one content length
  long (the source's length in beats for MIDI, its played seconds for
  audio). Back on the timeline, a clip loop converts only in that form, or
  when it is off; any other loop window has no item equivalent and is
  refused (`clip.unsupported`, hint: set the loop off or to the whole
  content first).
- **Take envelopes.** An item's are in beats from its start. On a MIDI clip
  the points keep their numbers. On an audio clip each point converts
  through the tempo map at the item's span: seconds from the item's start
  second to the point's beat. Back on the timeline the same conversion runs
  the other way at the landing position.
- **Exactness.** The conversion is exact at the tempo where the item sits
  or lands: an item → clip → item round trip at the same position gives the
  same length and points (up to rounding), and each case has a test.
- **What a tempo change inside the span loses.** Points convert one by one,
  so the points land exactly, but the segment between two points is
  straight in the new unit. Across a tempo change (a step or a ramp) inside
  the span, the shape between those points differs slightly from the
  original. An audio clip then keeps its seconds whatever the tempo map
  does later; a MIDI clip keeps its beats.
- **Refused:** a warped take (above), a loop window an item cannot
  express, and, between beats and seconds, a take envelope with automation
  items (they would need converting too). Nothing else is guessed.

**Timeline only; these refuse a clip** (`item.kind`, with a hint naming the
clip command). They place material in time:
- `item.move`, `item.set`, `item.split`, `item.duplicate`, `item.remove`,
  `item.paste`;
- `item.consolidate`, `item.heal`, `item.mix-to-track`,
  `item.explode-takes`, `item.apply-fx`;
- `audio.quantize`, `audio.warp.detach/reset`, `group.targets`;
- render bounds, analysis ends, freeze length and the project length.
Their `item` fields accept a clip's id only to refuse it. (`record.commit`
takes no item, so a clip never reaches it.)

### Invariants

The commands check these first and refuse with their own errors and hints;
the model checks them again on every commit, where a violation is
`model.invariant` at the object's path (a bug, session-model §3).

- A clip's track exists and is a **channel**. Clips are not allowed on
  busses or masters; a master may hold audio items (ADR-0057), but not
  clips, in this ADR. The commands check it as they check items' strip
  rules: `clip.add` and `clip.move` with `clip.track`, and `track.set
  {type}` on a channel that holds clips with `track.strip_type` (hint: move
  or remove its clips first).
- A clip's scene exists. There is at most one clip per (track, scene).
- `length > 0` and finite. `loop.start ≥ 0`, and `loop.length > 0` when
  `loop.on`.
- A clip's takes exist and belong to no other owner. The active take is one
  of them. Each take's source matches the clip's `kind`.
- Scene `order` values are distinct integers. `scene.move` keeps them
  0 .. n−1.
- At most 1024 scenes and 16384 clips per project (`scene.limit`,
  `clip.limit`).

**Cascades.** Removing a track removes its clips. Removing a scene removes
its clips. Removing a clip removes its takes, and unpooled sources go too,
as with `item.remove`. Each command reports the clips it removed in a
`clip.removed` warning.

**Freeze.** Clips are not part of a frozen track's print or its freeze hash,
since the print is the arrangement. They can be edited while the track is
frozen, but not launched:
- freezing a track returns it to the arrangement first, and drops its
  queued launch;
- `launch.clip` on a frozen track fails with `track.frozen`;
- `launch.scene` launches the scene's other clips and skips each frozen
  track with a `track.frozen` warning naming it;
- `launch.stop` and `launch.arrangement` on a frozen track change nothing,
  since it already follows the arrangement.

### Older builds (G-c, G-d)

The schema is **2.18**, by order of landing (G-d). The collections `scenes`
and `clips` are written only when present, and the project's
`launch_quantize` only when it is not `bar`.

- **A project that holds clips does not open in a build older than
  2.18.** There is no backward path, and none is planned. (An older build
  keeps `scenes` and `clips` as unknown sections, so to it a clip's takes
  belong to no item, and it refuses the open; it loads every saved version,
  so a clip with takes anywhere in the history is enough. Nothing is done to
  make the other cases open there.)
- **A file that comes back through an older build anyway opens,** for the
  two things such a build can leave behind. A missing or low `sc` or `cl`
  counter is derived from the highest id (`project.counter_derived`). A
  clip that refers to something gone (its track, its scene, a take or its
  active take) is dropped, with its remaining takes, by a
  `project.clip_dropped` warning naming it and why. Each case has a test
  with a hand-edited file. Any other fault in a clip fails the open with
  `model.invariant`, as for every kind.

*Note (2026-11-20, on merging the context track):* a saved branch's `sc`
and `cl` counters follow the one lineage rule ADR-0096's note of 2026-11-19
describes, not the raise past every saved version that L1 (e4e5f841) first
built: `VersionStore::restore` raises a branch's counters, for every kind, to
those of each version its lineage reaches (its head's history and the steps
waiting in redo). The checklist's "branch counter derivation" is that one
loop now, which needs no per-kind list.

### Memory (L-e)

Every clip's audio is resident, inside the in-memory budget of 2^28
samples (prepared_sources.cpp). Today that budget is all or nothing: past
it, the whole preparation fails with `media.budget` and nothing plays.
Clips replace that with an **ordered budget**:
- **The order.** Arrangement items are budgeted first, since the
  arrangement is what renders. Then clips, in scene order, and within a
  scene in track order: the track tree's display order (the masters list,
  each track followed depth-first by its children).
- **What does not fit.** Clips past the budget are not loaded. They stay in
  the project, cannot be launched, and are reported by a `launch.memory`
  warning naming them. The arrangement alone past the budget still fails
  as today.
- **When.** The rule applies at every load and every recompile, not only on
  open, since an arrangement edit can push clips out.
- **Timeline commands are never refused because of clips.** `item.add`, a
  longer take or a playrate change may leave fewer clips loaded; the
  warning says which.
- **Clip commands check.** `clip.add`, `clip.duplicate`, `take.add` and
  `take.activate` on a clip, and `take.set` changing a clip take's source
  window, refuse a set whose clips would not all fit beside the arrangement
  (`clip.memory`). The error names the budget and what the arrangement and
  the clips hold now.
- **Who.** The ordered budget is the engine's (L2, the lead). L1 states it
  and adds the `clip.memory` checks.
- The disk-backed cache (route E) is separate work that the launcher will
  want at scale.

### Commands

- **L1, model.**
  - `scene.add {name?, color?, at?}`, `scene.set {scene, …}`,
    `scene.remove {scene}`, `scene.move {scene, to}` and `scene.list` (q).
  - `clip.add {track, scene, …}`, taking one source: `notation`,
    `audio_file`, `midi_file`, or `item` (a copy of an item's takes, as
    above), with `pooled?`. Without a source it makes an empty MIDI clip of
    one bar. Its length: the notation's or MIDI file's length rounded up to
    whole bars, the audio file's played seconds, or one bar; a bar is the
    first meter's.
  - `clip.set {clip, name?, color?, mute?, gain_db?, length?, loop?, mode?,
    quantize?}`, where `quantize: null` clears the override.
  - `clip.remove {clip}`.
  - `clip.move {clip, track?, scene?}` and `clip.duplicate {clip, track?,
    scene?, pooled?}`. Both need an empty cell (`clip.occupied`).
  - `item.add {clip, track, position | bar}`: a new item from a copy of a
    clip's takes. This is how a clip is dragged onto the timeline.
  - The content commands above accept clips.
  - Errors: `clip.track`, `clip.scene`, `clip.occupied`, `clip.mixed`
    (a take whose source does not match the clip's kind), `clip.length`,
    `clip.memory`,
    `clip.unsupported`, `clip.limit`, `clip.source` (more than one source),
    `scene.limit`, `scene.index` (a column past the last), `take.position`
    (`take.extract` from a clip without a position) and `item.kind`, and
    `track.strip_type` from `track.set`. Each has `code`, `message`, `path`
    and a `hint`.
- **L2, lead.** `launch.clip {clip}`, `launch.scene {scene}`,
  `launch.stop {track?}`, `launch.arrangement {track?}`, and
  `launch.state` (q): what each track plays (`arrangement`, a clip, held
  silent, stopped), what is queued and for when, and each playing clip's
  position in its period. Telemetry publishes the same for the GUI.
- **L4.** `launch.capture {on}` and `launch.commit` (§ Scenes and the
  arrangement); `scene.insert {scene, at | bar, bars}`, `scene.capture
  {region, name?}` and `arrangement.build {sequence: [{scene, bars}…],
  at?}`.
- **`session.describe`** lists scenes with their clips after the tracks
  (session-model.md shows the text). Clips never count toward the project
  length.

### Playing (engine; revised by the lead with L2, 2026-11-19)

- **A track either follows the arrangement or plays one clip.** Every
  clip of a channel is compiled into the channel's node as a *clip
  player*. An audio clip's player holds the take's prepared buffer, as an
  item's does. A MIDI clip's holds its notes, sorted by their beat within
  the content. A launch never compiles. A frozen track's node compiles no
  players: it has no chain or note stage and plays its print.
- **What a track plays is transport state,** the node's *launch state*:
  `arrangement`, `clip` (with the clip's player and anchor), `held` (a
  `once` clip past its length) or `stopped`. Plans carry it across swaps
  by node id and clip id (§1.6 of engine.md). A clip the new plan no
  longer has (removed, or now past the memory budget) ends at that swap
  and its track returns to the arrangement, without the switch fade: the
  new plan holds no player to fade out. (Amended 2026-11-20: an audio
  clip without a take chain now fades out from a copy the new plan keeps;
  see engine.md §1.4.2.)
- **A launch is a timed message (L-a, B1).** Its target is `{epoch,
  stream sample}`, the coordinate plan handoffs use (`handAt`,
  `handEpoch`). A scene launch has one target for all its tracks: the
  latest of their floors (B5).
- **The floor is per track.** It is the position of the executor that
  runs the track plus that executor's block: the renderer's published
  position plus one render block for a rendered track, the callback's
  stream plus one device block for a live one. The target is the next
  quantize boundary at or after the floor, through the playrate
  timelines.
- **The queue is the engine's (L-b).** It has fixed capacity and holds one
  entry per track; a newer launch on a track replaces its pending entry.
  Plans hold only the players, so an entry survives plan swaps and a plan
  waiting for adoption.
  - **Who applies it:** the executor that runs the track's node when its
    block reaches the target. It splits the block at the target, as at a
    loop wrap, and takes the entry by compare-and-swap, so a node handed
    between executors is applied exactly once. A handoff carries the
    applied state with the node.
  - An executor whose block already starts past the target marks the
    entry missed, and the main thread re-quantizes it. With a bar or beat
    quantize and a clip length in whole bars, a re-quantized track keeps
    the same phase (below).
  - **Epochs:** an executor never takes or misses an entry from another
    epoch. After a seek's `epochDone_` the main thread re-quantizes every
    pending entry from the new position (B4).
  - Each applied entry comes back to the main thread with its stream
    sample, its running frame and its song position.
  - *(2026-11-20, revised with the co-director: several targets per
    launch; engine.md §1.4.2.)* A launch has one target node per thing
    that follows the track: its channel, and every rack Notes module whose
    source is that track. Each switches at the launch's stream sample plus
    its own node's timeline shift, by the executor that runs that node, so
    a track and the rack it feeds may switch on two executors. Whether the
    launch is taken is decided once, for all of them:
    - The main thread publishes the entry, then reads every executor's
      published horizon (the furthest its current block may reach,
      stored before it reads the queue). If each executor that runs a
      target is at or before the target, the entry is committed: each
      will read it before it passes its frames. Otherwise it is missed.
    - An executor that reaches a frame of an entry still undecided marks
      it missed. Nothing of a missed entry switches anywhere; the main
      thread places it again, as before.
    - The first target of a committed entry to switch takes it by
      compare-and-swap, against the main thread voiding an entry whose
      epoch ended first. Every other target then switches at its own
      frame, or, if its executor relocated before reaching it, at that
      executor's next block, as at a relocation. The slot is reused only
      once each executor is done with it.
    This replaces "Who applies it" above: an entry is no longer taken by
    the track's executor alone.
- **While stopped,** a launch is not quantized. It starts the transport
  and applies at the play start's first sample, so the clip's anchor is
  the play position.
- **Quantize** is project-wide (`launch_quantize`) with a per-clip
  override. `none` means the floor: about the render-ahead (200 ms) for a
  rendered track, one device block for a live one. That is accepted for
  this work and stated in the guide.
- **Phase is a pure function of the song position (L-c),** in the clip's
  unit, with the mathematical modulus for negative differences.
  - MIDI: the position in the clip is `(song beat − anchor beat) mod
    length`. The player converts beats to samples per block through the
    plan's time map, without allocating, as timeline MIDI does.
  - Audio: it is `(timeline sample − anchor sample) mod length in frames`,
    never beats, so a tempo ramp does not move it.
  - Inside the period, a loop window that is on maps the position into
    the content, as an item's looped source does.
  - A `once` clip sounds while its position is within its length after
    the anchor. It is silent before the anchor, and silent after a
    transport wrap until it is launched again.
  - Seeks and wraps use the same formulas, so clips stay on the grid and
    a performance replays exactly from its log.
- **Switching (L-d, B5).** Leaving the arrangement or a clip ends what
  was left's sounding notes at the switch sample. Returning to the
  arrangement does not chase notes (`trackEvents`, engine.md §1.1). An
  audio switch crossfades over a fixed 5 ms of timeline samples, in the
  source like an item's fade, in both executors and offline.
- **The launch log and the offline replay.** The engine logs each applied
  launch with its running frame (stream frames since the play start, in
  the epoch's terms) and its song position, and logs seeks and transport
  wraps beside them. Renders and the render dialog play the arrangement
  only, and the dialog says so in one line while any track is held by a
  clip. An internal offline replay applies a log at its logged running
  frames. The tests use it, and L4's capture will.
  *(2026-11-20, the lead with L4's engine half: seeks and wraps are not
  logged yet; capture and commit as built are in engine.md §1.4.2, and a
  capture lives in one transport epoch.)*
- **Memory (L-e).** The prepared-source budget counts sample values
  across channels: the decoded source plus each converted variant, with
  a source or variant shared by several takes counted once. A clip on
  audio the arrangement already plays at the same rate therefore costs
  nothing. The same preparation (`prepareSources`, off the audio thread)
  adds the clips' takes after the arrangement's, in scene order and then
  track order. It skips any clip that would pass 2^28 instead of failing,
  and names the skipped clips in a `launch.memory` warning. A launch of a
  skipped clip is refused with `launch.memory`.
- **Frozen tracks.** Freezing returns the track to the arrangement at the
  swap that adopts the frozen plan, in both executors, and drops its
  queued launch. There is no switch fade: the frozen node holds no
  players to fade out, and the print replaces the live chain at that swap
  in any case. `launch.clip` on a frozen track
  fails with `track.frozen`, and `launch.scene` skips it with a
  `track.frozen` warning.
- **Audio clips play at the take's own rate,** with its playrate, pitch
  and channel mode as on the timeline, and no warp. A clip whose audio
  does not fit the project tempo gets a `launch.tempo` warning.
- **Staged within L2, each stated in STATUS until done:**
  - *Master playrate.* Clip audio is a source on the playrate path
    (ADR-0045 §2: converted before take FX and mixing), so it speeds up
    with the master playrate. Until that lands, a plan with the master
    playrate active compiles no players, its tracks follow the
    arrangement, and a launch is refused with `launch.playrate`.
    (2026-11-21: launches play under a master playrate; see the revision
    of that date below.)
  - *A clip take's FX and envelopes.* They will play as an item's take
    chain does. Until then a clip plays without them, and the plan warns
    with `launch.take_chain`, naming the clip.
- **The take `pitch` envelope** that is stored but never played (core
  §4.5) stays unplayed for clips as for items. Whether to play it is
  decided with the stretch path, for both.
- **Tests:** both executors against the offline replay of the engine's
  own launch log, at two block sizes, through the bridge, and under
  rtsan.

### Scenes and the arrangement (L4)

A scene is a section you can play before it has a place in the song.

- **Capturing a performance.** While `launch.capture` is on, launches are
  logged with their applied positions. `launch.commit` writes the
  performance onto the timeline as ordinary items in one undo step, the
  sibling of `record.commit`.
  - The performance is laid out linearly from the position where capture
    began, by running time, whatever the transport loop did (L-f).
  - MIDI content is copied, not pooled, unless `pooled` is asked.
  - *(2026-11-20, as built by the lead, engine.md §1.4.2.)* Each piece of
    a clip is laid at the phase the song gave it, so a loop wrap
    restarts a piece where the phase jumped. On each track that left the
    arrangement, its clip, stopped and held stretches replace the
    arrangement there; the 5 ms switch crossfades become item fades. A
    stop, seek or loop change ends a capture that has run.
    `launch.commit` records the performance it laid out in history
    (`performance`), so a merge replays it without an engine.
  - *(2026-11-20, I3.)* A scene launched whole also gets a section, a
    region named after the scene as `scene.insert` makes one, in the same
    undo step, so a committed performance and a built song are the same
    shape and `context.at` names the section over either (engine.md
    §1.4.2).
- **`scene.insert {scene, at, bars}`** lays the scene's clips on the
  timeline as items for that length and names a region after the scene.
- **`scene.capture {region, name?}`** makes a scene from the items inside a
  section. A new scene takes the region's name.
- **`arrangement.build {sequence}`** lays out a whole song from scenes in
  one command.
- **Refused rather than guessed (L-g),** each with a hint:
  - several items of one track inside the region (`scene.capture.items`);
  - an item straddling the region's edge (`scene.capture.edge`);
  - a looped clip or item whose take is warped (`clip.unsupported`).

#### Scene ↔ section: the rules (L4a, 2026-11-20)

These three commands touch the model only: no launch, no player. Each is one
undo step, and a refusal changes nothing. The contract is in
`specs/session-model.md`, "Scene ↔ section"; the rules and why:

- **A section is a region, and a scene is its columns of clips.** `scene.insert`
  and `arrangement.build` go from scene to timeline; `scene.capture` from
  timeline to scene. Going there and back lands where it started (below).
- **Spans are whole bars from a bar line.** `scene.insert {scene, at | bar,
  bars}` takes the span from the start of the bar (`bar`, or `at` in beats,
  which must be on a bar line) for `bars` bars, counted through the meter map.
  An `at` between bar lines is refused (`scene.at`), because "n bars from the
  middle of a bar" has two readings.
- **Where an item stops.** Each clip of the scene becomes one item on its own
  track at the span's start, converted as `item.add {clip}` converts it:
  - `once`: one item as long as the clip, or as the span if that is shorter.
    The rest of the span is silent on that track.
  - `loop`, clip no longer than the span: the item loops its content over the
    span (the same sound as the clip repeating) when the clip's period is the
    content: its loop off and its length the content, or its loop on over the
    whole content (start 0) with a length of whole repeats of it. A loop window
    that is not the whole content cannot be an item and is `clip.unsupported`,
    as `item.add {clip}` refuses it. A clip whose period is anything else (a gap
    after the content, a length shorter than the content) cannot be one item
    without a guess about the gap, and is refused (`scene.insert.period`, hint:
    set the clip's length to its content or its loop on over the whole content,
    clip.set, or ask for no more bars than the clip is long).
  - `loop`, clip at least as long as the span: one item for the span, the first
    part of the clip.
- **Existing items in the span.** An item on a track that receives a clip and
  overlaps the span is refused (`scene.insert.occupied`, naming the items; the
  hint says to move or remove them). Items on other tracks are left alone. The
  command never overwrites, trims or splits.
- **An empty scene.** A scene with no clip is refused (`scene.empty`) by all
  three commands that read one; a region with no item to capture is
  `scene.capture.empty`.
- **MIDI is copied, not pooled,** unless `pooled: true`; the conversion is the
  one `clip.add {item}` and `item.add {clip}` already use (audio media are
  shared, fades are not kept).
- **The region.** `scene.insert` adds a section named after the scene (or
  `name`), the scene's colour, over the span. It adds a new region every time
  and never edits an existing one.
- **Capture is one clip per track, with nothing guessed.** `scene.capture
  {region, name?}` reads the items of each channel that lie wholly inside the
  region:
  - an item that starts before the region or ends after it:
    `scene.capture.edge` (checked first);
  - more than one item of one track inside it: `scene.capture.items`;
  - the one item starting after the region's start: `scene.capture.offset` (a
    clip begins at its own start, so the entry point would be lost);
  - a warped take: `clip.unsupported`, as `clip.add {item}`;
  - items on a master (they belong to no channel) are skipped, and a warning
    `scene.capture.skipped` names them.
  The clip is `loop` when its item fills the region and `once` when it is
  shorter (it played once and fell silent in the section); the item's own loop
  becomes the clip's. The new scene is appended after the last, named after the
  region unless `name` is given, and takes its colour. The region and the
  items are untouched.
- **A song from scenes.** `arrangement.build {sequence: [{scene, bars}…], at |
  bar?}` is `scene.insert` for each entry, one after the other from the start
  (default: bar 1), a region for each. It checks every entry before changing
  anything, and refuses an empty sequence, one past 256 entries, or `bars` < 1.
- **Round trip.** `scene.capture` of a section that `scene.insert` laid gives a
  scene whose clips play the same material, with these differences, each held
  by a test: a clip as long as the span, or a `once` clip shorter than it, comes
  back the same (length, loop, mode); a looped clip laid over a longer span
  comes back as one looped clip as long as the span; a clip longer than the span
  comes back cut to the span, as a `loop` clip.
- **Limits and memory.** `scene.limit` and `clip.limit` apply to capture, and
  `clip.memory` to audio it adds; items laid on the timeline are in the
  arrangement's budget as `item.add`'s are.

### GUI (L3)

`gui.md` §4.15 specifies it, from mock 13.
- Scenes are columns in a pane between the track list and the lanes, on the
  one row model.
- A clip cell is a small item, drawn by the shared item-body functions.
- Playing and queued use `playing`, solid and dashed (L-h).
- A narrow last column per track is lit while it follows the arrangement.
  A track's lanes dim while a clip holds it.
- Clips drag to and from the timeline.
- A clip launches from its launch zone, the ▶ at the left of its name bar.
  A click on its body selects it, and a double-click opens its editor; no
  click does both.
- Dragging the pane's edge all the way makes the launcher the whole view.
- The pane takes a cell in the arrange header row (L-h).

### Implementation checklist (L1, from survey Q5–Q7)

- **Ids.** Two `Kind`s with prefixes `sc` and `cl`, `kKindCount`, the
  counters (the G-a guard fails the build otherwise).
- **Model plumbing.** `Traits`, `TxnBuilders` and the diff; `versions.cpp`
  `entryInfo`/`objectJson`/`objectSchema`; both section lists (schemas.cpp
  load, and project_file.cpp `objectDepth`). The collections join
  `mixOnly`, `patchOnly` and `scalarOnly` (as edits that compile).
- **Lists no guard covers** (round 53 G4; a member missing from them
  compiles and silently loses data): `EditTxn::preview` and so `commit`
  (`apply<T>`: a missing one drops the collection's edits); `shareRevisions`
  in project_file.cpp; `RetainedSize` in versions.cpp; `Checker::counters`
  (the clip kinds check theirs in `checkLauncher`); the branch counter
  derivation in `VersionStore::restore`.
- **Commands.** The ID patterns in every argument schema that takes a
  scene, a clip, or an `item` that may be a clip.
- **Migration and tests.** Migration for older files; the round trip for
  every field (save and reopen); and the take-owner abstraction at the five
  sites above, with the orphan invariant.

### Steps

| Step | Who | What |
|---|---|---|
| L1 | gui-lead (model, commands) | The two kinds, schema 2.18, the take owner, invariants, older builds' files, round trip; `scene.*`, `clip.*`, `item.add {clip}`, content commands accepting clips. No engine, no GUI. |
| L2 | lead | The clip player, the launch queue, the phase rules, state carry, `launch.*`, telemetry, the render dialog's line. |
| L3 | gui-lead | The launcher pane (gui.md §4.15). |
| L4 | lead and gui-lead | Capture and commit; scene ↔ section; `arrangement.build`. |

## Amendment (2026-11-20): a clip take's chain

**Context.** L2 staged a clip take's FX and envelopes: a clip played dry
and the plan warned with `launch.take_chain`. A timeline item's take with
FX plays through a chain whose input runs its latency ahead, so it is
heard on time; a clip needs the same, but its start is a launch decided at
run time, not a position in the timeline.

**Decision.**
- **A clip's take chain** is compiled like an item's (the take's FX, its
  volume, pan and mute envelopes), linked to the clip's player. The clip's
  audio, or its notes for an instrument in the chain, go into the chain;
  the chain's output goes where the player's went, weighed by the switch's
  crossfade, with the clip's and the take's gain and pan after the chain.
- **Latency and run-ahead.** A track's launch lead is the most latency
  among its clips' chains. Every clip chain on the track reads its input
  that far ahead and is delayed back by the lead less its own latency, so
  the clip is heard at its switch frame. The incoming clip's chain must
  therefore know of the launch the lead before the frame. A launch reaches
  such a track twice: its early half at the frame less the lead (from
  there the chains' input takes it) and the switch at the frame, as
  before.
- **C1: the floor grows by the lead.** The target is fixed when queued, at
  or after every owning executor's horizon *plus the target track's
  launch lead*. The decision is still made once, at the first frame of
  any target, which for such a track is its early half; the horizon check
  uses that frame. An early half before the epoch's first frame (a launch
  placed while stopped, whose target is the play start) is that first
  frame, so the chain warms up through its first frames *(review round 69:
  it counted as passed, and a chained track started a bar after a dry
  one)*. The alternative, keeping the floor and starting the
  chain at the switch, would make the clip heard its chain's latency late,
  or lose its first frames to compensation: a clip would no longer sound
  at its boundary, and a capture would not commit as it played. Quantize
  `none` waits that much longer on such a track. The lead is the track's
  (the most of all its clip chains), not only the clips a launch
  switches: the outgoing chain needs the early half too, so its notes end
  where the switch is heard.
- **Running.** A chain runs while its clip is coming in, plays or fades
  out. It starts afresh at each launch that starts its clip, and runs on,
  unreset, across the clip's passes and a relaunch of the playing clip.
  At the switch away its tail is cut after the crossfade, as an item's is
  at its end.
- **Envelopes.** The take's own envelopes are already in clip time in the
  model (beats on a MIDI clip, seconds on an audio clip): they play from
  the clip's start, pass after pass, as `launch.commit` lays out the
  copies.
- **C2: envelopes on a clip take's FX** move to clip time too, relative
  to the clip's start and looping with it. (It changes the meaning of those
  points in the model, schema 2.19, with a migration; no project has them,
  since L1 shipped with this limit stated.) Landed 2026-11-20:
  - In the model they are beats on a MIDI clip and seconds
    (`timebase: time`) on an audio clip, from its start, as the take's own
    envelopes are; they take no automation items and no writing mode. The conversions between
    an item and a clip carry them across point by point.
  - The engine compiles their lanes in the clip's domain (the domain of
    the take envelopes' clip position), unshifted by the chain's latency,
    and evaluates them at the clip position each stage's input reads: the
    running lead less the stages before it. A lane sends on the absolute
    automation grid, at each point it crosses and afresh at each new pass
    or anchor; while the input reads nothing it holds.
  - `launch.commit` writes each copy's FX envelopes in timeline position,
    repeated pass after pass over the copy, so a looped item of many passes
    keeps one chain as the clip did. Each pass's value at the period is
    held to the boundary and the next pass starts from its first value
    there; a copy ending on a boundary also takes the next pass's start,
    which a latent FX hears within its latency, as it did live.
- **Freeze is unchanged:** a frozen track plays its print and refuses
  launches, so its clip chains never play.
- **Neither C1 nor C2 changes** the single decision, the launch log (the
  early half follows from the entry's frame and the plan's lead), or the
  phase rules.

**Consequences.**
- Live equals the offline replay with clip chains, in both executors,
  through the bridge.
- A launch on a track with a latent clip chain is placed that much later;
  a rendered track's floor was already about the render-ahead.
- A plan swap that raises a track's lead after a launch was queued starts
  the incoming chain's input late for that launch (its first frames go
  through the chain's warm-up). It is the rare case of a latency change
  racing a queued launch, and live and the replay agree on it.

## Revision (2026-11-20, batch 3): the replay loops; a capture stays in one epoch

**Context.** The offline replay of a launch log played one linear run, so
no live-versus-replay test could loop (review round 66). The draft for
batch 3 proposed logging seeks as well, so that a capture could span them
and a replay follow them, with an inexact window after each seek where the
anticipative renderer's flush and the plug-ins' state may not reproduce.

**Decision (the co-director's).**
- **Wraps, exactly.** An internal replay is given the transport loop
  (`OfflineSettings::loop`) and wraps where the live executors do: no block
  crosses the loop end, the song continues at the loop start (`wrapTo`),
  and the take chains running ahead wrap early in the same loop. The log's
  running frames count on across wraps, as the live executors' do within an
  epoch; a wrap is not logged, since it follows from the loop.
- **No seeks in a capture.** A seek, a loop change or a stop still ends a
  capture, so a capture and its replay never cross a relocation, no window
  is inexact, and the law that live equals the replay holds everywhere. The
  performance schema gains nothing.
- A capture already records its loop and the commit lays wraps out (L-f);
  an exact wrap needed nothing else.

**Consequences.**
- Every live-versus-replay launcher test runs once more under a loop that
  wraps before an anchor, in both executors, through the bridge too.
- A looped replay under a master playrate is refused (`render.loop`) until
  launches play under one (the next revision).

## Revision (2026-11-21, batch 3, S3 step 1): launches under a master playrate

**Context.** L2 staged the master playrate: a plan with it active compiled
no players, its tracks followed the arrangement, and a launch was refused
with `launch.playrate`. Batch 3's S3 put clips on the playrate path. This
revision records step 1 as built.

**Decision.**
- **The target stays a stream frame.** The queue protocol, the launch log
  and the replay by running frame are unchanged.
  - *Placement.* On a playrate plan, while playing, `placeLaunch` seeds a
    copy of the running rate timeline from the executor's published clock,
    with the scalar edits after it and the plan's envelope. It takes the
    song position at the floor, finds the quantize boundary as on the unity
    path (folding over the loop), and targets the first frame whose
    unwrapped song position reaches it. With quantize `none` the anchor is
    the floor's song sample, rounded down.
  - *A rate control in flight.* While the running plan's scalar or envelope
    differ from the newest plan's, placement answers `launch.later`, and
    service places a queued launch again later.
  - *Edits.* A scalar edit voids the pending launches whose target lies
    past the edit's frame. A plan with other rate controls (the playrate on
    or off, the scalar, the envelope, a sync chase) voids those that switch
    after its handover. Service places them again on the new trajectory, on
    the same boundary unless the floor has passed it.
  - *A launch missed while it was placed* (every executor's horizon passed
    it before it was decided) is placed again at once by `launch()`, up to
    three times, instead of at the next service. A placement on a playrate
    plan integrates the rate up to the boundary, which takes milliseconds.
    Three, because a miss means the placement took longer than the time
    from the floor to the executors' horizon, which happens when the main
    thread is starved (a debug build under load): one more try almost
    always lands, and the bound keeps `launch()` from spinning on the main
    thread while the starvation lasts. After the third, the missed slots
    stay missed and the next service places them again, as it did before
    the retry; `launch()` answers with the third placement.
- **Audio clips** play through a tape converter (`PlayerReader`: per
  channel one for what plays and one for what fades out, and one per clip
  chain).
  - The converter's positions are the song positions of the frames (the
    rate timeline's unwrapped phase). Its source folds them over the
    transport loop and maps them by the player's phase rule: the anchor,
    `once`, the loop window and the take's offset.
  - Before the anchor it is silent, unless the clip is unanchored or the
    frame is read in a later loop pass than the output's (a clip chain
    reading ahead of a wrap).
  - The tape converter keeps no state between quanta, so a launch primes
    nothing.
- **With the pitch preserved**, a playrate plan reads the timeline through
  stretch converters, which a launch would have to seek and prime. In
  step 1 an audio clip then has no player: compile warns
  `launch.playrate`, and its launch is refused with `launch.playrate` (hint:
  turn off the playrate's preserve pitch, or set it to 1 without an
  envelope). MIDI clips launch. Step 1b plays these audio clips.
- **MIDI clips** play each event at the first frame whose song position
  reaches it, by the rule timeline MIDI follows on the playrate path
  (`rateEvents`).
  - What the track holds ends where the song enters a new loop pass.
  - A switch's first frame reaches back to the anchor, which at a slow rate
    can lie behind the previous frame's song position.
  - While anything but the arrangement holds the track, the timeline's
    notes pass unplayed, as on the unity path.
  - A clip chain's instrument (its input, the lead ahead) and a rack's
    Notes feed follow the same rule.
- **The 5 ms switch crossfade is in device frames, as heard.** On a
  playrate plan `LaunchState::fadeStart` is the node's frame at the switch
  (`launchClock`). A crossfade in progress when a plan changes between the
  unity and playrate paths ends there.
- **Clip time (C2).** Clip-time lanes and a clip chain's take envelopes
  are not bound to the rate timeline. They read the clip position from the
  song position, now fractional.
- **Capture.** `launch.capture` is refused at arming while a master
  playrate is active: a rate other than a steady 1, an envelope, or a sync
  chase. The error is `launch.playrate`, with the hint "set the master
  playrate to 1 and turn off its envelope, then capture". A playrate that
  becomes active ends a running capture, with a warning. Committing a
  capture by the song positions heard (S3, Q2) needs the scalar history
  logged with it, and is left for later.
- **Still refused:** a looped replay under a master playrate
  (`render.loop`). The replay does not wrap on the playrate path.

**Consequences.**
- Live equals the replay under a constant 0.75 and under an envelope, in
  both executors, at two block sizes. This holds for audio and MIDI clips,
  clips with take chains, and a rack's Notes feed. Further tests cover the
  switch frame at the boundary, a scalar edit placing a launch again, the
  crossfade in device frames, a note on the anchor at 0.25, a clip lane in
  clip time, and the refused capture.
- A placement integrates the rate up to the boundary on the main thread,
  on a 64k-frame copy of the timeline.
- Limits until later steps:
  - audio clips under a pitch-keeping playrate (step 1b);
  - a frozen track's print cuts in, not crossfaded, on a playrate plan;
  - when a plan swap changes the trajectory, launched audio reads the new
    one at once, where timeline clips crossfade between the two;
  - a looped transport under a playrate has no replay to compare with.

## Revision (2026-11-22, S3 design A): a capture can end on a quantize boundary

**Context.** `launch.capture {on: false}` ended a capture at the position
heard when it arrived, so a performance ended wherever the caller happened
to be: a script polling every 20 ms ended it one to three blocks past the
bar it meant, and the commit's last item was cut there.

**Decision.**
- `launch.capture {on: false, quantize}` with a quantize other than `none`,
  while playing, ends the capture on that quantize's next boundary. The
  boundary is placed as a launch of every track with clips made at that
  moment would be (the same floors, the loop fold and, on a playrate plan,
  the rate placement), so a scene launch made with it lands on the same
  boundary. One track's launch can land a boundary earlier when its floor
  is nearer than the latest of them (a live track in the anticipative
  engine, or one whose clip chains take less lead); it is then in the
  capture.
- The end is main-thread state: the capture runs until the heard position
  reaches the boundary's stream frame, then service closes it there. Its
  running time ends exactly on the boundary, and a launch applied on or
  after it is not in the performance. Nothing changes on the audio path.
- Before the boundary, `on: true` cancels the end, another `on: false`
  replaces it, and a relocation (a seek, a stop, a loop change) ends the
  capture at once, as before. Stopped, or without `quantize` (or with
  `none`), it ends at once, as before.
- It ends on the sample a launch on the boundary switches on (the nearest
  to the boundary). `ends_at` is that sample's beat, the same number as the
  commit's `to`: the boundary's own beat when it falls on a whole sample,
  otherwise within half a sample of it (at 117 BPM, 7.99999… for beat 8).
  After a transport loop wrap, `ends_at` is the song's beat in the loop,
  while the commit lays out by running time, so its `to` is further on.
- `launch.commit` while an end is pending is refused with
  `launch.capture.ending`; the capture is kept.
- The GUI's Capture toggle, turned off while playing, ends the capture on
  the project's launch quantize and commits it once the end is heard.

**Consequences.** A performance can end on the grid it was launched on, and
its commit ends on the sample a launch on that bar switches on, with `to`
equal to the `ends_at` it was given, in both executors at any block size. The tests end a capture on bar 3 with a launch placed on the same
boundary (left out), and cover cancelling, replacing and a seek first.

## Consequences

- **Timeline code is untouched by construction.** The cost is the take
  owner, at five sites and one invariant, plus the content commands'
  argument patterns.
- **Older builds.** A project that holds clips does not open in a build
  older than 2.18, and there is no way back.
- **Memory.** Every clip's audio is resident, so a launcher set of long
  audio reaches the budget sooner than an arrangement does, until the disk
  cache exists. The budget becomes ordered: the arrangement first, then
  clips, and clips that do not fit are left out and reported, never the
  arrangement.
- **Conversion.** Moving material between an item and a clip is exact at
  the tempo where the item sits; across a tempo change inside its span, the
  envelope shape between points may differ slightly.
- **Latency.** Unquantized launches on rendered tracks start about 0.2 s
  after the click.
- **Not saved.** What is playing is not saved with the project. A
  performance is kept only by capturing it.
- **Audio does not follow tempo** in these steps.
- **Later:** recording into a cell; tempo-following audio; follow actions;
  per-scene tempo; controller mapping of launches; clips on busses or
  masters.
- **Determinism.** Renders stay deterministic and real time stays equal to
  offline in both executors, at any block size and through the bridge. A
  launch resolves to one stream sample for both executors, and every clip
  position is a function of the song position.
- **Platforms.** No platform-specific code is added. Native CI must pass on
  Linux, macOS and Windows before each step merges.
