# ADR-0097: Clip launcher: scenes and clips beside the arrangement

**Status:** Proposed, 2026-11-19, for review by opus-reviewer and the lead.
It extends ADR-0057 (master items) and ADR-0093 (values on a running plan),
and touches ADR-0013 (bridge deadlines) only through the executors it
shares. It is built in steps L1 to L4 (below). The engine's "Playing"
section is written from the core design and the lead's core check as they
stand, **for the lead's revision**.

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
| `takes`, `active_take` | Ordinary takes (`tk_*`), as an item's are. A clip with takes has an active take. |
| `length` | The clip's period, > 0. It is in the clip's unit (below). |
| `loop` | `{on, start, length}`: the content window that repeats inside the clip, measured from the take's start offset. Default off. |
| `mode` | `loop` (default): it repeats every `length` until something else is launched. `once`: it plays `length` once, then the track stays held by the clip, silent, until the next launch. |
| `quantize` | Optional override of the project's `launch_quantize` (below). |
| `name`, `color`, `mute`, `gain_db` | As an item's. |

- **The clip's unit.** A clip's `length`, `loop` and take envelopes are in
  the unit its content plays in. That is beats for a MIDI clip, which
  follows the song's tempo, and seconds of take time for an audio clip,
  which plays at the take's own rate (L-c). A clip's takes are therefore all
  MIDI or all audio; takes without a source are allowed in either kind. A
  clip with no source decides by its first source, and an empty clip is in
  beats.
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
`model::ownerTakes(session, owner)`, replaces the three scans that find a
take's item today:
- `takeOwners` (prepared_sources.cpp:25-30);
- `itemOfTake` (take_commands.cpp:34);
- `owner` (audio_commands.cpp:47).

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
  length under `midi.write`, as an item keeps its position and length.
- `item.export`: the clip's raw audio or its MIDI.
- `take.list`, `take.add`, `take.activate`, `take.set` and `take.remove`.
  A clip goes with its last take, as an item does.
- `take.extract`: the take leaves the clip for a new item on the timeline.
- `take.hits.detect` and `take.hits.set`.
- `fx.add {take}`, `env.add {take}`, and the `fx.*` and `env.*` commands on
  a clip take's FX and envelopes. A take envelope on a clip is in the
  clip's unit.
- The editors: the piano roll, the inspector and the take FX window. They
  open a clip as they open an item. A clip in the piano roll uses the
  project's first key (an addition to §5 of the core design).

**Refused on a clip's take in the first steps** (`clip.unsupported`, with a
hint to lay the clip on the timeline first): `take.stretch.add/move/remove`,
because the clip player does not warp. A clip whose take has stretch
markers cannot be made (`clip.add {item}` refuses it, L-g).

**Timeline only; these refuse a clip** (`item.kind`, with a hint naming the
clip command). They place material in time:
- `item.move`, `item.set`, `item.split`, `item.duplicate`, `item.remove`,
  `item.paste`;
- `item.consolidate` and `.commit`, `item.heal`;
- `audio.quantize`, `audio.warp.detach/reset`, `group.targets`;
- `record.commit`;
- render bounds, analysis ends, freeze length and the project length.

### Invariants

Violations are `model.invariant` at the object's path.

- A clip's track exists and is a **channel**. Clips are not allowed on
  busses or masters; a master may hold audio items (ADR-0057), but not
  clips, in this ADR.
- A clip's scene exists. There is at most one clip per (track, scene).
- `length > 0` and finite. `loop.start ≥ 0`, and `loop.length > 0` when
  `loop.on`.
- A clip's takes exist and belong to no other owner. The active take is one
  of them. Its sources are all MIDI or all audio.
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
frozen, but not launched (`launch.clip` fails with `track.frozen`).

### Older builds (G-c, G-d)

The schema is **2.18**, by order of landing (G-d). The collections `scenes`
and `clips` are written only when present, and the project's
`launch_quantize` only when it is not `bar`.

- **A pre-2.18 build refuses a file whose clips hold takes.** It keeps
  `scenes` and `clips` as unknown sections, so to it the clips' takes
  belong to no item, and its invariant (session.cpp:403) refuses the open
  with a path. This follows the precedent of racks (schema 2.7, whose
  module-end routes older builds refuse). A file with scenes and no clip
  takes opens there.
- **A file that comes back from an older build opens.** A missing or low
  `sc` or `cl` counter is derived from the highest id. A clip whose track
  or scene is gone, whose track is no longer a channel, or whose takes are
  gone or claimed by an item is dropped with a `project.clip_dropped`
  warning naming it and why. Two clips on one (track, scene) keep the lower
  id. The open never fails for these, and each case has a test with a
  hand-edited file.

### Memory (L-e)

Every clip's audio is resident, inside the existing in-memory budget of
2^28 samples (prepared_sources.cpp:120-122).

- `clip.add`, `clip.duplicate`, `take.add` and `take.activate` on a clip,
  and `take.set` changing a clip take's source window refuse a set that
  would go over budget (`clip.memory`). The error names the budget and
  what the clips hold now.
- A project already over budget on load plays what fits, in scene order
  then track order. The plan warns (`launch.memory`) naming the clips left
  silent, and the open never fails for it.
- The disk-backed cache (route E) is separate work that the launcher will
  want at scale.

### Commands

- **L1, model.**
  - `scene.add {name?, color?, at?}`, `scene.set {scene, …}`,
    `scene.remove {scene}`, `scene.move {scene, to}` and `scene.list` (q).
  - `clip.add {track, scene, …}`, taking one source: `notation`,
    `audio_file`, `midi_file`, or `item` (a copy of an item's takes), with
    `pooled?`. Without a source it makes an empty MIDI clip of one bar.
  - `clip.set {clip, name?, color?, mute?, gain_db?, length?, loop?, mode?,
    quantize?}`, where `quantize: null` clears the override.
  - `clip.remove {clip}`.
  - `clip.move {clip, track?, scene?}` and `clip.duplicate {clip, track?,
    scene?, pooled?}`. Both need an empty cell (`clip.occupied`).
  - `item.add {clip, track, position | bar}`: a new item from a copy of a
    clip's takes. This is how a clip is dragged onto the timeline.
  - The content commands above accept clips.
  - Errors: `clip.track`, `clip.scene`, `clip.occupied`, `clip.mixed`
    (audio and MIDI takes in one clip), `clip.length`, `clip.memory`,
    `clip.unsupported`, `clip.limit`, `scene.limit` and `item.kind`. Each
    has `code`, `message`, `path` and a `hint`.
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

### Playing (engine; for the lead's revision)

- **A track either follows the arrangement or plays one clip.** All of a
  track's clips are compiled into its node: audio as prepared buffers, MIDI
  as period-relative event lists feeding the track's one note stage. A
  launch never compiles.
- **A launch is a timed message (L-a, B1).** Its target is `{epoch, stream
  sample}`, the coordinate plan handoffs use (`handAt`/`handEpoch`). It is
  not a timeline sample, because the timeline repeats under the transport
  loop and is not the stream at any master playrate other than 1. There is
  one target per launch. For a scene it is the latest floor among its
  tracks (B5), so a scene spanning live and rendered tracks starts together.
- **The queue is the engine's (L-b, B3).** It is fixed capacity and keyed
  by track id. Plans hold only the clip players, so a queued launch
  survives plan swaps and a pending compile.
  - The main thread picks the target: the next quantize boundary at or
    after the renderer's published position plus one render block,
    computed through the playrate timelines.
  - The first executor to reach an entry decides it by compare-and-swap
    (B2). It is taken if that executor's stream is at or before the target;
    otherwise it is missed and goes back to the main thread, which
    re-quantizes it.
  - After a seek starts a new epoch, the main thread re-quantizes every
    pending entry from the new position (B4).
  - The applied sample is acknowledged by both executors. The song position
    at which a launch was applied is what the launch log keeps.
- **Quantize** is project-wide, with a per-clip override. `none` means as
  soon as both executors can agree: about the render-ahead (200 ms) on
  rendered tracks, and the device block on live tracks. That floor is
  accepted in this work and stated in the guide.
- **Phase is a pure function of the song position (L-c, B4),** in the
  clip's own unit. Negative differences use the mathematical modulus.
  - MIDI: the position in the clip is `(song beat − anchor beat) mod
    length`.
  - Audio: it is `(timeline sample − anchor sample) mod length in frames`,
    never beats, so a tempo ramp does not move it.
  - A `once` clip sounds while the position is within its length after the
    anchor, and is silent before the anchor; after a transport wrap it is
    silent until it is launched again.
  - Seeks and wraps apply the same formulas, so clips stay on the grid and
    a performance replays exactly from its log.
- **Switching (L-d, B5).** Leaving the arrangement or a clip ends the
  sounding notes of what is left, at the switch sample. Returning to the
  arrangement does not chase notes (plan.cpp:168). An audio switch has a
  short fixed fade.
- **While stopped.** Launching while stopped starts the transport. State is
  carried across plan swaps by node id.
- **Audio clips play at the take's own rate** in these steps, with the
  take's playrate and pitch as on the timeline, but no warp. A clip whose
  audio does not fit the project tempo gets a warning. Following tempo
  comes later, as a live converter on the shared fractional clock (the
  master-playrate path), not as cached renders.
- **Renders play the arrangement only.** The render dialog says so in one
  line while any track is held by a clip.
- **Lead's call, with L2:** the take `pitch` envelope that is stored but
  never played (core §4.5) is refused or played, for clips and items alike.
- **Tests:** both executors against the offline replay of the same launch
  log, at two block sizes, through the bridge and under rtsan.

### Scenes and the arrangement (L4)

A scene is a section you can play before it has a place in the song.

- **Capturing a performance.** While `launch.capture` is on, launches are
  logged with their applied positions. `launch.commit` writes the
  performance onto the timeline as ordinary items in one undo step, the
  sibling of `record.commit`.
  - The performance is laid out linearly from the position where capture
    began, by running time, whatever the transport loop did (L-f).
  - MIDI content is copied, not pooled, unless `pooled` is asked.
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

### GUI (L3)

`gui.md` §4.15 specifies it, from mock 13.
- Scenes are columns in a pane between the track list and the lanes, on the
  one row model.
- A clip cell is a small item, drawn by the shared item-body functions.
- Playing and queued use `playing`, solid and dashed (L-h).
- A narrow last column per track is lit while it follows the arrangement.
  A track's lanes dim while a clip holds it.
- Clips drag to and from the timeline.
- Dragging the pane's edge all the way makes the launcher the whole view.
- The pane takes a cell in the arrange header row (L-h).

### Implementation checklist (L1, from survey Q5–Q7)

- **Ids.** Two `Kind`s with prefixes `sc` and `cl`, `kKindCount`, the
  counters (the G-a guard fails the build otherwise).
- **Model plumbing.** `Traits`, `TxnBuilders` and the diff; `versions.cpp`
  `entryInfo`/`objectJson`/`objectSchema`; both section lists (schemas.cpp
  load, and project_file.cpp `objectDepth`). The collections join
  `mixOnly`, `patchOnly` and `scalarOnly` (as edits that compile).
- **Commands.** The ID patterns in every argument schema that takes a
  scene, a clip, or an `item` that may be a clip.
- **Migration and tests.** Migration for older files; the round trip for
  every field (save and reopen); and the take-owner abstraction at the three
  sites above, with the orphan invariant.

### Steps

| Step | Who | What |
|---|---|---|
| L1 | gui-lead (model, commands) | The two kinds, schema 2.18, the take owner, invariants, older builds' files, round trip; `scene.*`, `clip.*`, `item.add {clip}`, content commands accepting clips. No engine, no GUI. |
| L2 | lead | The clip player, the launch queue, the phase rules, state carry, `launch.*`, telemetry, the render dialog's line. |
| L3 | gui-lead | The launcher pane (gui.md §4.15). |
| L4 | lead and gui-lead | Capture and commit; scene ↔ section; `arrangement.build`. |

## Consequences

- **Timeline code is untouched by construction.** The cost is the take
  owner, at three sites and one invariant, plus the content commands'
  argument patterns.
- **Older builds.** A pre-2.18 build cannot open a project whose clips hold
  takes. Saving such a project for an older build means laying the clips on
  the timeline first, or removing them.
- **Memory.** Every clip's audio is resident, so a launcher set of long
  audio reaches the budget sooner than an arrangement does, until the disk
  cache exists.
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
