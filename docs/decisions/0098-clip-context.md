# ADR-0098: Clips know the context

**Status:** Accepted, 2026-11-20. Written by the lead from the
co-director's outline (`designs/headline-core.md` §7); the co-director
accepted it with the seven differences listed below. Revised the same day
after the design review (round 71, F1 to F19), with the co-director's
decisions on F1, F2, F5, F7 and F8: chord tones map by function, following
never refuses, and a laid note reads the clip's context at its place in the
clip's period.
It extends ADR-0095 (the context track) and ADR-0097 (the clip launcher).
It is built in steps X1 to X5 (below); X1 and X2 are specified here and in
the contracts precisely enough to build without asking.

Contracts: `specs/session-model.md` "Clip context (schema 2.20, ADR-0098)"
and the clip rows of "Scenes and clips", `specs/harmony.md` §5 "Following",
`specs/notation.md` "Chord symbols in the compiled result". Each is marked
as the contract for X1 or X2.

## Context

The builder's song journey (`reports/builder-song-journey.md`) wrote one
24-bar song with the context track, modulators and the launcher, and found
the seam between them: the harmony is written twice, once as the lead sheet
and once in the clips, and nothing connects the two.
- A chord symbol in a clip's notation is voiced into notes at once. The clip
  keeps no key and no chord.
- A scene laid under a different key (`scene.insert`, `arrangement.build`) is
  copied note for note, and nothing says so (friction F4).
- `midi.edit`'s scale ops on a clip read the key at the *project's* start
  (ADR-0095 amendment C-e). The builder's Break bass, written in D minor in a
  project that starts in C minor, had its A1 snapped to Ab1, silently
  (defect D1). `fit_to_chord` on a clip is refused without `chord`.

A clip has no place in time, so it must carry the harmony it was written
against. The context track says what the music is *at a place*; a clip's own
context says what the clip *was written against*. Following is the mapping
from the second to the first when the clip is given a place.

## What the code has today (the three points the outline left to the lead)

**1. `libs/harmony`** (`harmony.h`, MIT, no model dependency) has:
- `Key`, `Chord`, their parsers and printers, `keySupported`, `allScales`,
  `scaleIntervals`, `chordQualities` (each quality's intervals above the root,
  in order), `chordPitches` (the notation's voicing);
- `transposeDegrees(pitch, key, degrees)`: an in-scale pitch moves by scale
  degrees, an out-of-scale one first goes to the nearest scale pitch below;
- `keyPitchClasses`, `chordPitchClasses` (pitch classes, *ascending 0..11*,
  so the chord's tone order is lost) and `snapToPitchClasses(pitch, classes,
  mode)`;
- `ContextIndex` (key and chord at a beat over plain points).

`midi.edit`'s three ops are thin wrappers over these (`midi_commands.cpp`).
**Following cannot be those ops with the clip's context as the source.**
- `snap_to_scale` and `fit_to_chord` move a note *to a set*. They lose which
  degree or chord tone the note was, and a note already in the target set
  stays even when following should move it (C major to D major moves every
  note).
- `transposeDegrees` moves within one key. It does not map a degree of one
  key onto the same degree of another.
- `chordPitchClasses` sorts its classes, so "the third of the chord" cannot
  be found from it.

So X2 adds one function to `libs/harmony`, `follow` (below and
`harmony.md` §5), built from `scaleIntervals`, `chordQualities` and the
spelling-free semitone arithmetic the library already uses. The `midi.edit`
ops stay as they are and read the clip's own context (X1).

**2. Notation and chord symbol positions.** The compiler voices `{SYMBOL}:DUR`
in `compileVoice` (`libs/notation/src/notation.cpp`), where the event's
start tick (`cursor`) and the parsed `harmony::Chord` are both in hand, and
then forgets the symbol. It can hand the symbols over **without a format
change**:
- an additive member of the public result, `Compiled::chords`: one
  `ChordSymbol {std::int64_t tick; harmony::Chord chord; std::string voice;}`
  per symbol event as compiled (the bars a copy line or a bar range repeats
  included), ordered by tick and then by voice declaration;
- the continuation of a tied chord (`{Cm7}:h~ {Cm7}:h`) adds no entry;
- a symbol that fails to parse already raises a warning and adds no notes;
  it adds no entry either.

The text format, its parser and `decompile` are unchanged.

**3. The mapping rules.** These are in `harmony.md` §5, as one
deterministic function with its tie rules and the only test list, and are
summarized below under "The mapping".

## Decision

### Model (schema 2.20)

- **`Clip.context`** (optional): the clip's own key and chord points, in the
  context track's shapes (`ContextPoint`: `beat`, `type`, `tonic`/`scale` or
  `symbol`), with `beat` counted from the clip's start in beats.
  - A MIDI clip: points in `[0, length)`, at most one of a type at a beat, at
    most 4096 of a type.
  - An audio clip: at most one point, a key at 0 (for display and for the
    mismatch warning).
  - The strings are data, as on the context track. An unknown scale or
    chord quality from a later build is kept, and it reads as no key or no
    chord at that beat everywhere: the scale ops, following and the
    warning.
- **`Clip.follow`**: `none` (default), `key` or `chords`. With the default,
  nothing that plays today changes.
- **The invariants live on the clip** and are checked at commit:
  - the points rule above;
  - `follow` other than `none` needs a MIDI clip with a readable key at
    beat 0.
  - `clip.add` and `clip.set` refuse a `follow` that breaks the second
    (`clip.no_key`, `clip.follow_audio`).
  - **Edits keep both in the same step and say so:**
    - shortening a clip removes the points at or past its new length
      (`clip.context_dropped`);
    - a context edit that leaves no readable key at 0 resets `follow` to
      `none` (`clip.follow_reset`).
  - Undo restores both.
- **Items keep no link.** An item is plain notes. Following happens when a
  clip becomes an item, and in X4 when it is launched.
- **File:**
  - a clip gains `"context": [{"beat": 0, "type": "key", "tonic": "D",
    "scale": "minor"}, {"beat": 0, "type": "chord", "symbol": "Dm"}, …]`,
    sorted by beat then type (key first), positions rounded to 1/960 beat,
    written only when not empty;
  - it gains `"follow": "key"`, written only when not `none`.
- **Older files** load unchanged; nothing migrates. A 2.19 build drops both
  members with its unknown-field warnings.
- **Load:**
  - points against the rules are dropped with `clip.context_dropped`;
  - a `follow` the invariant refuses reads as `none`, with
    `clip.follow_reset`.
  - The open never fails for any of this.

### How a clip gets its context (X1)

- **`clip.add {context: TEXT}`.** The lead sheet text form, in clip bars.
  Bar 1 is the clip's start, and bars run in the project's meters from bar
  1, which is how the clip's notation compiles. The bars are full bars and
  may run past the clip's end. What is checked is the points
  (`clip.context_range`), so a clip that is not whole bars can still state
  a change in its last part bar. An audio clip's text holds only a key at
  the start of bar 1.
- **`clip.add {context_at: {bar} | {at}}`.** The project's context read from
  that place for the clip's length:
  - the key and chord holding there become points at 0;
  - the points inside the span follow, shifted.
  - No key there gives the clip no key, and the answer warns
    `context.no_key`.
  - `context` and `context_at` together: `clip.context_args`.
- **`clip.add {item}` and `scene.capture`.** The project's context under the
  item, from its start, by the same function as `context_at`, unless
  `context` or `context_at` is given, which wins. A clip remembers where it
  came from. An audio item gives a key only.
- **Chord symbols in the clip's notation** (`clip.add {notation}`,
  `midi.write` on a clip) are recorded from `Compiled::chords` at their clip
  beats, through the active take's start offset and playrate.
  - Repeated bars (copy lines and bar ranges) count.
  - Symbols at or past the clip's length are not recorded
    (`clip.chords_beyond_length`).
  - At one tick the earlier-declared voice wins (`clip.chord_conflict`).
  - Equal neighbours collapse, and a rest does not end a chord.
  - **Precedence:**
    - a stated `context` with chords, and `context_at`, win over the
      symbols, which are ignored with `clip.chords_ignored`;
    - a stated `context` with only a key takes the symbols' chords;
    - `midi.write` replaces the chord points only when the symbols differ,
      with `clip.chords_replaced`, and a text without symbols leaves the
      context alone.
- **History** records `context_at`, and the context taken under an item, as
  the resolved `context` text. So a replay on another branch gives the clip
  the points the user saw, as the lead sheet's history does.
- **`clip.set {context, context_at, follow}`**: `context: ""` clears it. One
  undo step, as any `clip.set`.
- **Copies.** `clip.duplicate` and `clip.move` keep `context` and `follow`.
  `take.extract` from a clip and `item.export` copy the notes, with no
  following.
- **Reading it back.**
  - **`midi.get` on a MIDI clip** answers `context` in the shape it already
    has for items (`midi_context`), one shape under one name: runs `{item_bar,
    what, beat}` with the clip bar as `item_bar` and no `project_bar`, plus
    `text` and `follow`.
  - Its pitch names follow the clip's own key at its start.
  - There is no `clip.get`. `session.describe` names a clip's key at 0 and
    its `follow`, audio clips included.

### Editing against it (X1, fixes D1)

- `midi.edit`'s `transpose_degrees`, `snap_to_scale` and `fit_to_chord` on a
  clip read the clip's own context at each note's clip beat: its sounding
  beat through the take's start offset, playrate and the clip's loop
  window, modulo the clip's length. Where those beats disagree for one
  note, the op is refused with `midi.context_varies`, as on items.
- Without a key: `midi.no_key`, hint "give the clip a context (clip.set
  {context} or {context_at}) or pass key". Without a chord: `midi.no_chord`.
- The ops never read the project's context for a clip. ADR-0095 amendment
  C-e (the key at the project's start) is superseded for clips.
- `key` and `chord` in the op still replace the reading.

### Following (X2)

**Following itself never refuses and never fails.** X4 must give the same
notes live, and a refusal has no live meaning. So the conditions live on the
clip (above), and placement copies what cannot follow, saying why.
- **Where.** `item.add {clip}`, `scene.insert` and `arrangement.build` take
  `follow: none | key | chords`, defaulting to each clip's own `follow`.
  `launch.commit` does not take it (difference 1).
- **Which clips follow.** A `follow` other than `none`, given or the clip's
  own, applies to each MIDI clip with a readable key at 0 and at least one
  note off channel 10. Any other clip (a drum clip, an audio clip, a keyless
  clip) is copied, and its item's answer says why: `copied: "drums" |
  "audio" | "no_key"`.
- **A note's place and its clip beat.** A note's sounding beat is where it
  starts from the item's start, through the take's start offset and
  playrate and the item's loop.
  - Its place is the item's start plus its sounding beat.
  - Its clip beat, where the clip's context is read, is its sounding beat
    modulo the clip's length: the clip's own period, as it plays when
    launched.
  - Only the active take is followed; the item's other takes are copied.
  - Notes the item never plays are copied, and do not count toward "every
    pass maps alike".
- **Never followed:** notes on MIDI channel 10 (stored as 9; General MIDI
  drums, as `voice drums` writes them). Controllers, bend and pressure are
  copied as they are.
- **`none`.** Notes are copied. A `context.mismatch` warning is given once
  per item when, at some beat of the item, the clip's key at the clip beat
  and the project's key both hold and differ in tonic pitch class or scale.
  It names the clip, both keys and the first project bar. A drum-only clip
  gives none, and neither does an enharmonic spelling (Eb major under D#
  major).
- **`key`.** Each note is mapped by `harmony::follow` from the clip's key at
  its clip beat to the key at its place. Where no readable key holds at the
  place, the note is copied, with one `context.no_key` warning per item.
- **`chords`.** Each note is mapped from the clip's chord at its clip beat to
  the chord at its place. A note with either chord missing or unreadable
  follows by key (difference 7).
- **The item that results.**
  - If every pass of a looped item maps every played note alike, it stays
    looped over the mapped source.
  - Otherwise it is laid unrolled (loop off, its passes written out, each
    mapped at its place), with `unrolled: true`.
  - `pooled: true` with a mapping that moves any note is refused
    (`clip.follow_pooled`): a shared source cannot be two keys. This is the
    one refusal, and it concerns the argument, not the mapping.
- **Range.** A mapped pitch outside 0..127 is folded by octaves inside
  `harmony::follow`. X4 folds alike, and the answer counts such notes
  (`folded`).
- **The answer.** Each laid item gains `followed: {mode, regions, folded?,
  unrolled?, copied?}`.
  - A region is a maximal run of project bars over which the clip's key, the
    place's key and the way notes were mapped stay the same: `{from_bar,
    to_bar (inclusive), from_key?, to_key?, by, interval?}`.
  - `by` is `transpose`, `degree`, `chords` or `copy`.
  - A `chords` follow lists its key fallbacks as their own regions.
- **History.** A placement with `follow` re-maps against the branch it
  replays on, as `scene.insert` re-reads the clip.
- **Until X4** a launched clip plays unfollowed. Launching a clip whose
  `follow` is not `none` warns `launch.unfollowed`, and STATUS says so, so
  live and the timeline never differ silently.

### The mapping, `harmony::follow`

The function, its formulas and **the only test list** are in `harmony.md` §5.
In short:
- **The tonic interval** is the nearest one, with the tritone down.
- **The same scale** transposes.
- **Scales with as many degrees** map by degree, keeping a note's alteration
  above the degree below it. So distinct notes may merge across a gap that
  narrows (harmonic minor's G and G# both go to B in C major), and a
  relative key moves every note, tonic to tonic.
- **Scales of different sizes** transpose, with `context.scale_shape`.
- **Chord tones map by function**, through a slot table: root, third (sus
  tones stand in), fifth, seventh (a sixth stands in), ninth, eleventh,
  thirteenth.
  - When the target chord has no tone in a slot, an extension goes to the
    root, third or fifth, and any other tone to the highest slot below it.
    So a seventh over a triad lands on the triad's fifth, and a power
    chord's missing third on its root.
  - Two notes that land on one pitch are both kept.
  - A slash bass that is also a tone maps as the tone; a bass that is not a
    tone maps to the target's bass, or root.
- **Other notes** keep their degree distance above the root, as a class (a
  rotation of the scale's degrees). They move by the root interval when the
  scales differ in size or a root is outside its scale.
- **The octave** is the one nearest the key-followed pitch, with a tie down.
- **Out of range** is folded by octaves.

### The other direction (X5)

`arrangement.build {write_context: true}`: where the context track is empty
under a section, the scene's clips' context (when they agree) is written
there. Not in X1 or X2.

### Steps

| Step | Who | What |
|---|---|---|
| X1 | builder (model, commands) | The fields, schema 2.20, the invariants and their edits, and the round trip; `clip.add {context, context_at, follow}`, chord symbols recorded (with `Compiled::chords`), `clip.set`, `midi.get` on a clip (one shape; spelling by the clip's key), describe; `clip.add {item}` and `scene.capture` take the context under the item; history records the resolved text; the `midi.edit` ops on clips (D1); the `none` mismatch warning at placement. It updates what states the old reading: the tests in test_clip_launcher.cpp and test_context_commands.cpp, `skill/guides/launcher.md`, `skill/guides/harmony.md`, `docs/guides/manual.md`, midi.get's description, and the generated commands. |
| X2 | builder (harmony, commands) | `harmony::follow` with `harmony.md` §5's tests; `follow` on `item.add {clip}`, `scene.insert` and `arrangement.build`, with the copies and their reasons, unrolling, `clip.follow_pooled` and the answer; `launch.unfollowed` and its STATUS line; the song journey writes its Verse once, in C minor, and it lands right in D minor beside its drums. |
| X3 | gui-lead | A clip's roll shows and edits its context (chord strip, scale shading); the clip inspector has Follow; a mismatch shows on a laid item's roll. |
| X4 | lead (engine) | A launched clip follows live through pitch tables built at compile from `harmony::follow`, per key region and chord region. A note is looked up by the song position of its *start* (its clip beat by the launch's phase), and keeps that pitch until its note-off, so a note held across a change cannot stick. A note out of range folds as on the timeline, and a clip that cannot follow plays unfollowed, as placement copies it. Context edits then publish the tables while a clip with `follow` ≠ `none` exists, which amends engine.md "Context edits". `launch.commit` lays what was heard, followed, and `launch.unfollowed` goes. |
| X5 | later | `write_context`; modulation sources that read the context (idea I7); scenes carrying macro state (I5). |

## Where this differs from the outline, and why

1. **`launch.commit` takes no `follow`.** A commit lays what was heard (L-f:
   the committed timeline renders as the performance played). Until X4 a
   launched clip plays unfollowed, so a followed commit would lay notes
   nobody heard. Until X4 the commit copies, with the `none` warning. After
   X4 it lays what live followed, by the same function, so it needs no
   argument then either.
2. **Drums are never followed** (channel 10). The outline did not say, and a
   key follow of a drum clip would move the kick to another drum.
3. **Looped items unroll when the map varies by pass**, and `pooled` with a
   changing follow is refused. The outline's "a key change inside the span
   splits the mapping" cannot be represented by a looped item over one
   source otherwise.
4. **The tritone goes down (−6),** as every tie in `libs/harmony` does, and
   scales of different shapes transpose with a warning instead of mapping
   degrees that do not correspond.
5. **Precedence between a stated context and the notation's chord symbols**
   is spelled out, with warnings, where the outline listed the sources side
   by side.
6. **The ops on a clip read the clip's context, never the key at the
   project's start.** That supersedes ADR-0095 amendment C-e for clips. The
   outline said the same in other words, and D1 is fixed by it.
7. **`chords` falls back per note,** not per item: a clip with chords for
   its first bars only still follows by key after them.

## Consequences

- The harmony is written once: a clip written against D minor knows it, says
  so when laid under C minor, and can follow.
- **One mapping function**, in an MIT library, is shared by the commands now
  and the engine in X4, so offline and live cannot differ. It never fails,
  and following never refuses, so there is nothing X4 could not do live.
- **Clips grow by their context points.** No limit changes except the new
  per-clip 4096.
- **Items stay plain notes.** Nothing that plays today changes, since
  `follow` defaults to `none`.
- **D1 is fixed by X1.** It needs no separate fix.
