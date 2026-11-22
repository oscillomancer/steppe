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

Names go with the material: the items `scene.insert`, `arrangement.build`, `item.add {clip}`
and `launch.commit` make from a clip are named as it is, and the clips `clip.add {item}` and
`scene.capture` make from an item are named as it is (an item's name is its active take's,
`item.set {name}`); `session.describe` shows a named item as `it_3 "Bass line" bar 5–bar 9`.

Content commands take a clip as they take an item: `midi.get`, `midi.write`,
`midi.edit`, `midi.notes`, take FX and the take's own envelopes. **A clip keeps
its length under `midi.write`**: notation longer than it is kept but never plays,
and the warning `midi.beyond_length` says so; set `clip.set {length}` first. A
clip has no place in time, so its `midi.get` spells pitches by the clip's own
key (see "A clip knows its key and chords" below) and its `context` is the clip's.

Timeline commands refuse a clip with `item.kind` (`item.set`, `item.remove` …) and say
which command to use instead. A clip take has its own FX chain (`fx.add {take}`) and
envelopes on it (`env.add {fx}`): they play with the clip, and an envelope's points are in
**clip time** (beats on a MIDI clip, seconds on an audio one), so a clip launched at two
places, or on its second pass, hears the same values. `modulate` does not reach a clip
take's FX yet (`modulation.target`): modulate the track's FX instead.

## A clip knows its key and chords

A clip has no place in time, so it keeps the harmony it was **written against** (its
`context`, with beats counted from the clip's start), as the project's context track
keeps the harmony at a place. Say it when you make the clip:

- `clip.add {..., context: "1: key D minor {Dm}:w\n2: {A7}:h {Dm}:h"}`: a lead sheet in
  **clip bars** (bar 1 is the clip's start; `guides/harmony.md` has the text form).
- `clip.add {..., context_at: {bar: 5}}` (or `{at: <beat>}`): read it from the project
  there, for the clip's length. `clip.add {item}` and `scene.capture` take the context
  under the item the same way, and `context: ""` takes none.
- The `{Dm}` chord symbols in a clip's notation (`clip.add {notation}`, `midi.write`)
  are recorded as its chords, unless you stated chords, which win (`clip.chords_ignored` warns
  only when the symbols differ from the stated chords: symbols and `context_at` that agree are fine).
  An audio clip's context is a key at the start of bar 1 and holds (`~`) only.
- `clip.set {context, context_at, follow}` changes it (`context: ""` clears it).
  `follow` (`none`, `key`, `chords`) says how the clip's notes are to be mapped
  when it is laid under another key; it needs a MIDI clip with a key at its start
  (`clip.no_key`, `clip.follow_audio`). Laying it on the timeline maps its notes
  (see "Following when a clip is laid").
- `midi.get` on a clip answers `context` (its runs by clip bar and beat, and `text`, the
  lead sheet) and `follow`, and spells pitch names by the clip's own key.
  `session.describe` shows `cl_3 "Break bass" in D minor …`.
- `midi.edit`'s `snap_to_scale`, `transpose_degrees` and `fit_to_chord` on a clip read
  **the clip's** key and chord, never the project's: a bass line written in D minor
  snaps within D minor whatever key the song is in at the bar it is laid at. A clip
  with no context says `midi.no_key` / `midi.no_chord` (give it one, or pass `key` /
  `chord`).
- Laying a clip (`item.add {clip}`, `scene.insert`, `arrangement.build`, `launch.commit`)
  with its notes played as written (follow none) gives a `context.mismatch` warning when the clip's key is
  not the project's under the item, or, in the same key, when its chord at some beat is not the
  chord the project has there (once per item, naming the first bar that differs; once per
  launched clip in `launch.commit`). A clip with no chords has none to compare. The answer's
  `followed.mismatch` says what differs without reading the message: `kind: "key"` with
  `clip_key` and `key`, or `kind: "chord"` with `clip_chord` and `chord`, and the `bar`.
- Shortening a clip drops the context points past its end (`clip.context_dropped`); a
  context change that leaves no key at the start resets `follow` (`clip.follow_reset`).

### Following when a clip is laid

`item.add {clip}`, `scene.insert` and `arrangement.build` take `follow` (`none`, `key`,
`chords`), by default the clip's own. The laid item's MIDI takes get the clip's context
and `follow`, and the notes are copied **as written**: what sounds is worked out when the
item plays (**following is playback**), from the key it was written against (or its chord at
that beat) to the key (or chord) under it. Write a clip once in D minor with `follow: key`,
and it lands in C minor, or F minor from bar 7, with no second copy. Because nothing is
written into the notes, nothing laid goes stale: edit the context track and what follows moves
with it. `take.bake` writes the sounding pitches into the notes when you want them fixed.
Each laid item's answer has `followed: {mode, regions, folded?, copied?}`; `regions` is a list
of runs, each `{from_bar, to_bar, from_key, to_key, by, interval?}` (project bars, inclusive;
`by` is `transpose` with the `interval` in semitones, `degree`, `chords` or `copy`): the run
`5-6 transpose D minor>C minor -2` is `{from_bar: 5, to_bar: 6, from_key: "D minor", to_key:
"C minor", by: "transpose", interval: -2}`. A `chords` run names no chords and no interval, and
is the same whether the chords under it differ from the clip's or match them.

- Same scale: a transpose by the nearest interval. Scales of the same size (D minor to C
  major): by degree. Different sizes (a pentatonic): a transpose, with `context.scale_shape`.
- `chords`: each note goes from its chord tone (root, third, fifth, seventh, extensions) to
  the same tone of the chord under it; a note with no chord on either side follows by key.
  A figure moves as a block by the interval between the two roots, each note to its tone
  nearest where that puts it. Chord tones give one voicing for one pair of chords, whatever
  the keys; a note that is not a chord tone keeps its scale degree, so it depends on the keys
  too. Neighbouring notes can cross where their targets cross: A4 B4 over Cmaj7 in C major
  sounds Ab4 Gb4 under Cdim in C minor.
- Notes on channel 10 (drums) are never moved. A drum-only, empty, keyless or audio clip is
  laid without following, and `followed.copied` says why (`drums`, `no_notes`, `no_key`,
  `audio`).
- Where no key holds under a note, it sounds as written, with `context.no_key`.
- A looped item stays one looped item: each pass sounds as the key under it says, so a key
  change between passes (or, in `chords` mode, chords that differ from one pass to the next)
  changes what the passes sound as and nothing else. A note sounds as the key where it
  **starts**, to its end.
- Under a bar the context track marks "no chord" (`""`), `chords` falls back to a transpose by
  the key (`by: transpose` for that bar).
- `pooled: true` may follow too: the mapping belongs to each take that reads the source.
- A laid item that follows plays as its take maps it (`followed` and `midi.get {sounding}`
  say how); `take.bake` writes those pitches into the notes.
- A launched clip follows live as laid ones do, by the place where each note starts; a note
  keeps its pitch to its note-off. `launch.state` gives each track's `followed` (`none`,
  `key` or `chords`), and `follow_budget: true` when a clip's context and the project's are
  too varied to follow fully (a chords clip then follows its key, or plays as written).
- `launch.commit` lays notes as written and gives each laid take the clip's context and the
  follow that was heard, so a performance of followed clips is laid as played.

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
master playrate (`transport.playrate.set`) does not stop launches, but while it keeps the pitch an audio clip's
launch is refused (`launch.playrate`), and `launch.capture` is refused under any active
playrate, and while the rate heard (`effective_rate`) is still gliding back to 1. A clip's FX and envelopes play through its own chain; a rack's Notes module
and the track's MIDI sends follow launches.

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
- `write_context: true` (on both): the scenes' clips' own key and chords are written onto the
  context track under each section, in the same undo step and before the clips are laid, so
  a song built from scenes has a lead sheet (`context.get` reads it back) and `follow` finds
  the key. Each segment's (or the result's) `context` says `{written, keys, chords}` or why
  not: `present` (the track already has points under that section: left alone), `disagree`
  (two clips say different keys or chords at one beat: nothing written; `clips` and `bar` name
  them, and `context.not_written` warns), `holds` (the track already says it), `no_context`
  (no clip has any). A looping clip's points repeat at its length; a key that is already
  holding is not written again.
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
