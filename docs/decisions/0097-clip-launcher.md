# ADR-0097: Clip launcher: scenes and clips beside the arrangement

**Status:** Proposed, 2026-11-19, for review by opus-reviewer and the lead.
Revised the same day after review round 55 (F1 to F9), with the
co-director's rulings.
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
  length under `midi.write`, as an item keeps its position and length.
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
