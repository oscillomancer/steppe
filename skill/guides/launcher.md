# The clip launcher: scenes, clips, launching, and scenes as a song

Besides its timeline, a channel can hold **clips**: short pieces of MIDI or audio,
one per scene. A **scene** is a column of clips that play together. You can try
ideas by launching scenes, then turn them into a song. Everything here is a
command; there is no pane to look at, so read the session back with
`session.describe` (its `Scenes` block lists each clip with its length, mode,
loop, quantize, gain and content).

## Making scenes and clips

- `scene.add {name}`, `scene.set`, `scene.move`, `scene.remove`, `scene.list`.
- `clip.add {track, scene, notation}`: MIDI from notation (as `item.add`'s). Other
  sources: `midi_file`, `audio_file`, or `item` (a copy of an item). With none it is an
  empty one-bar MIDI clip. One clip per track and scene, on a channel (not a buss
  or master). A MIDI clip is measured in **beats**, as long as its notes rounded
  up to whole bars; an audio clip in **seconds** of take time.
- `clip.set {clip, name, color, mute, gain_db, length, loop, mode, quantize}`.
  `mode` is `loop` (the default: it repeats every `length`) or `once`. `loop:
  {on, start, length}` repeats only a window of the clip. `quantize` (`bar`,
  `beat`, `1/2` … `1/32`, `none`) overrides the project's `launch_quantize`; `null`
  goes back to it.
- `clip.move`, `clip.duplicate` (the copy's MIDI is its own unless `pooled`),
  `clip.remove`.

Content commands take a clip as they take an item: `midi.get`, `midi.write`,
`midi.edit`, `midi.notes`, take FX and the take's own envelopes. **A clip keeps
its length under `midi.write`**: notation longer than it is kept but never plays,
and the warning `midi.beyond_length` says so; set `clip.set {length}` first. A
clip has no place in time, so `midi.get` spells pitches by the key at the
project's start and gives no `context`.

Timeline commands refuse a clip with `item.kind` (`item.set`, `item.remove` …) and say
which command to use instead. A clip take has its own FX chain (`fx.add {take}`) and
envelopes on it (`env.add {fx}`): they play with the clip, and an envelope's points are in
**clip time** (beats on a MIDI clip, seconds on an audio one), so a clip launched at two
places, or on its second pass, hears the same values. `modulate` does not reach a clip
take's FX yet (`modulation.target`): modulate the track's FX instead.

## Launching

Launching needs the engine running: `engine.start {"backend": "dummy"}` is a
device-free one that is paced like a device. The transport can be stopped.

- `launch.clip {clip}`, `launch.scene {scene}` (every clip of the scene on the same
  beat; tracks without a clip in it are untouched), `launch.stop {track?}`,
  `launch.arrangement {track?}` (back to the timeline). Without `track`, the last two
  act on every track that holds a clip.
- A launch lands on the next **quantize boundary**: the clip's `quantize`, else
  `launch_quantize` (a bar by default). The result says where: `beat`, `seconds`,
  and `started: true` if it started a stopped transport (from the play position).
- `launch.state` reads what each track with clips plays *as heard*:
  `plays` is `arrangement`, `clip` (with its `clip` and `position` in its period),
  `held` (a `once` clip that has finished: silent until the next launch) or
  `stopped`; `queued` is a launch not yet heard, with the beat it applies at.
  Tracks without clips are not listed.

**Do not decide by a clock.** The dummy backend runs in real time. After a launch,
poll `launch.state` until it shows what you expect; a launch's `beat` is a song
position, so check that (a bar launch's `beat` is a multiple of the bar), not how
long you waited. Set a high tempo (`tempo.set {points: [{beat: 0, bpm: 240}]}`) to
make bars short.

A frozen track plays its print: `launch.clip` refuses it (`track.frozen`) and
`launch.scene` skips it with a warning. `launch.stop` on a track with no clips is
`launch.track`; `launch.scene` on a scene nothing can play is `launch.scene`. A
master playrate refuses launches (`launch.playrate`). A clip's take FX and
their envelopes play, and so do a rack's Notes module and the track's MIDI sends.

Two things to expect when polling. A launch given just as the transport started (the
first launch while stopped, or right after a seek) can be refused with `launch.later`
("the transport is relocating"): launch again in a moment. And a stopped transport does
not advance, so a `launch.stop` or `launch.arrangement` given while it is stopped stays
in `queued` until playing resumes: send it while the transport runs.

**Renders play the arrangement only.** `render.mix` while a clip plays gives the
same audio as without it. To hear (measure) a scene, put it on the timeline.

## Capturing a performance

- `launch.capture {on: true}` keeps every launch with the running time it applied at
  (from where playing starts, when stopped); `{on: false}` ends it and answers
  `{on, from, to}`, the span in beats. A stop, a seek or a loop change ends a capture
  that has run. `launch.state` has `capturing`: whether one is on.
- `launch.commit {pooled?}` lays the capture on the timeline as items, in one undo
  step, by running time from `from`: a launch heard s seconds in lands s seconds after
  it, however the transport loop jumped meanwhile. On each track that left the
  arrangement, its clips and its stopped or held stretches replace what the timeline
  had there; tracks that stayed on the arrangement are untouched. Each clip becomes
  copies of its takes at the phase heard, MIDI copied (shared with `pooled: true`),
  and the 5 ms switches are item fades. A scene launched whole (`launch.scene`, every
  clip that could play started on one boundary) also writes a **section** named after
  the scene, from that boundary until one of its tracks is launched again or the capture
  ends, so `context.at` names it as it does a built song's; clips launched one by one
  write none. It answers `{tracks, items, sections, from, to}`. Nothing captured is
  `launch.capture`. Committing again writes the same performance over itself (a section
  already there stays).
- A committed performance renders as what was launched: unlike a clip playing live,
  its items are on the timeline. To check, render the span, or lay the same scenes
  with `arrangement.build` and compare away from the switches.

## From scenes to a song

- `scene.insert {scene, bars, bar | at}`: lays each clip of a scene on the timeline as
  an item over whole bars, and adds a **section** named after the scene (`name`
  overrides). A `once` clip lasts its length (or the span if shorter); a `loop` clip
  lasts the span as one looping item. MIDI is copied, not shared, unless `pooled`.
- `arrangement.build {sequence: [{scene, bars}, …], bar?}`: `scene.insert` for each
  entry, one after another from `bar` (default 1), a scene as often as you like;
  one undo step, and an entry that cannot be laid changes nothing, with its
  index in the error's `path`.
- `scene.capture {region, name?}`: the other way: a scene from the items in a
  section, one clip per channel, named after the region. Items are copied;
  the region and its items are unchanged.

What they refuse, each with a `code`, `path` and a `hint`:

| code | when |
|---|---|
| `scene.empty` | the scene has no clips |
| `scene.insert.occupied` | an item already lies in the span on a track that would get a clip (nothing is overwritten, trimmed or split) |
| `scene.insert.period` | a clip's repeat is not its content (a gap after the notes, a length shorter than them): make its length its content (`clip.set`) or ask for no more bars than it is long |
| `clip.unsupported` | a loop window that is not the whole content, or a warped take |
| `scene.capture.items` / `.edge` / `.offset` / `.empty` | a track has several items in the region; an item crosses its edge; the one item starts after the region does; the region holds none |
| `scene.sequence`, `scene.bars` | an empty or over-long sequence, `bars` below 1 |

Chords and keys are written over a built song with `context.write` (see
`guides/harmony.md`); the sections show in `context.at`.

## Checking it

1. `session.describe` after each step: the `regions:` line, the items per track,
   the `Scenes` block.
2. Render the song and measure it (`guides/analysis.md`): the chorus should
   measure louder than a quiet verse, and the same song laid by hand
   (`item.add {clip}` then `item.set {length, loop: true}`) renders bit for bit the
   same.
3. Each command is one undo step; `history.undo` / `redo` cross them, and a saved
   project reopens with its scenes, clips, items and sections.
