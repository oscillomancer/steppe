# ADR-0098: Clips know the context

**Status:** Accepted, 2026-11-20. Written by the lead from the
co-director's outline (`designs/headline-core.md` §7); the co-director
accepted it with the seven differences listed below.
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
  per symbol event, in text order;
- the continuation of a tied chord (`{Cm7}:h~ {Cm7}:h`) adds no entry;
- a symbol that fails to parse already raises a warning and adds no notes;
  it adds no entry either.

The text format, its parser and `decompile` are unchanged.

**3. The mapping rules.** These are below under "Following", as one
deterministic function with its tie rules.

## Decision

### Model (schema 2.20)

- **`Clip.context`** (optional): the clip's own key and chord points, in the
  context track's shapes (`ContextPoint`: `beat`, `type`, `tonic`/`scale` or
  `symbol`), with `beat` counted from the clip's start in beats.
  - A MIDI clip:
    - keys and chords at beats in `[0, length)`;
    - a key at beat 0 whenever it has any key;
    - at most one point of a type at a beat;
    - at most 4096 of a type.
  - An audio clip: at most one point, a key at beat 0 (for display and for
    the mismatch warning); chords are refused (`clip.context_audio`).
  - The strings are data, as on the context track: an unknown scale or chord
    quality from a later build is kept and treated as unreadable.
- **`Clip.follow`**: `none` (default), `key` or `chords`. With the default,
  nothing that plays today changes. An audio clip takes only `none`
  (`clip.follow_audio`).
- **Items keep no link.** An item is plain notes. Following happens when a
  clip becomes an item, and in X4 when it is launched.
- **File:**
  - a clip gains `"context": [{"beat": 0, "type": "key", "tonic": "D",
    "scale": "minor"}, {"beat": 0, "type": "chord", "symbol": "Dm"}, …]`,
    sorted by beat then type (key first), positions rounded to 1/960 beat,
    written only when not empty;
  - it gains `"follow": "key"`, written only when not `none`.
- **Older files** load unchanged; nothing migrates. A 2.19 build drops both
  members with its unknown-field warnings (session-model, "Unknown fields
  within known objects").
- **Load:**
  - points out of range or breaking the rules above are dropped, each with a
    `clip.context_dropped` warning naming the clip and the point;
  - a key point after beat 0 with none at 0 is kept and a key at 0 is not
    invented: the clip then has no key before its first key point, and
    `key` following refuses it (`clip.no_key`).
  - The open never fails for any of this.

### How a clip gets its context (X1)

- **`clip.add {context: TEXT}`.** The lead sheet text form (session-model
  "Lead sheet"), read in clip bars:
  - bar 1 is the clip's start, and the bars run in the project's meters from
    bar 1, which is how the clip's own notation compiles (`compileNotation`
    at beat 0);
  - the text may cover less than the clip, never more (`clip.context_range`);
  - the rules above apply (`clip.context_key_first`: a key point after beat 0
    with none at 0).
- **`clip.add {context_at: {bar}}`.** The project's context read from that
  place for the clip's length:
  - the key holding at the bar's start becomes a key at 0;
  - so does the chord holding there;
  - the points inside the span follow, shifted.
  - No key there gives no key point and the warning `context.no_key`, so the
    agent is told before a `key` follow is refused.
  - `context` and `context_at` together: `clip.context_args`.
- **Chord symbols in the clip's notation** (`clip.add {notation}`,
  `midi.write` on a clip) are recorded as the clip's chord points from
  `Compiled::chords`, each at its tick in beats.
  - Two different symbols at one tick from different voices: the first in
    text order wins, with the warning `clip.chord_conflict` naming both.
  - Equal neighbours collapse into one point.
  - A rest does not end a chord; the next symbol does.
  - Precedence:
    - a stated `context` with chord points wins, and the notation's symbols
      are ignored with the warning `clip.chords_ignored`;
    - a stated `context` with only a key takes the notation's chords as well;
    - `context_at` takes the place's chords and ignores the symbols, again
      with `clip.chords_ignored`.
  - `midi.write` on a clip whose text has symbols replaces the clip's chord
    points and leaves its keys. A text without symbols leaves the context
    alone.
- **`clip.add {item}` and `scene.capture`.** The project's context under the
  item (its span from its start, for the clip's length), by the same
  function as `context_at`. A clip remembers where it came from. An audio
  item gives a key only.
- **`clip.set {context, follow}`:**
  - `context: TEXT` replaces the context;
  - `context: ""` clears it;
  - `context_at` is accepted as in `clip.add`.
  - One undo step, as any `clip.set`.
- **`clip.get` and `midi.get` on a clip** answer:
  - `context: {text, keys: [{beat, key}], chords: [{beat, chord}]}`, where
    `text` is the canonical lead sheet in clip bars;
  - `follow`.
  - `session.describe` names a clip's key beside its name (`"Bass" D minor`).

### Editing against it (X1, fixes D1)

- `midi.edit`'s `transpose_degrees`, `snap_to_scale` and `fit_to_chord` on a
  clip read the clip's own context at each note's clip beat.
  - A note's beat is its start within the period: for a looping clip, the
    window's notes at each pass's beat inside `[0, length)`. When the passes
    disagree, the op is refused with `midi.context_varies`, as on items.
  - Without a key (or a chord for `fit_to_chord`):
    - `midi.no_key`, with the hint "give the clip a context (clip.set
      {context} or {context_at}) or pass key";
    - `midi.no_chord`, likewise.
  - The ops never read the project's context for a clip. ADR-0095 amendment
    C-e (the key at the project's start) is superseded for clips.
- `key` and `chord` in the op still replace the reading.

### Following (X2)

**Where.** `item.add {clip}`, `scene.insert` and `arrangement.build` take
`follow: none | key | chords`, defaulting to each clip's own `follow`.
- `launch.commit` does not take it: see "Where this differs from the
  outline", (1).
- **Each note's place** is its start beat on the timeline: the item's
  start, plus its beat in the source, plus the pass offset for a looped item.
- **Notes on MIDI channel 10 are never followed**: General MIDI drums, as the
  notation's `voice drums` writes them. The answer counts them
  (`drums_kept`).
- CC, pitch bend and pressure are copied as they are.

**`none`.** Notes are copied.
- If the clip has a key and a key region the item spans holds a different
  key (tonic or scale differ), the answer carries one `context.mismatch`
  warning per item naming the clip, the clip's key, the place's key and the
  bar where they first differ.
- A clip without a key gives no warning; nothing is known to disagree.
- Never silent when something is known.

**`key`.** Each note is mapped by `harmony::follow` from the clip's key at
its clip beat to the key at its place.
- No key in the clip: refused, `clip.no_key` (hint: give the clip a context,
  or follow `none`).
- No key at the place: the note is copied, with one `context.no_key` warning
  per item.
- A key change inside the item splits the mapping at it, because the map is
  per note.

**`chords`.** Each note is mapped from the clip's chord at its clip beat to
the chord at its place.
- No clip chord at the note's beat, or no chord at its place: `key` for that
  note (if that is impossible too, it is copied with the warnings above).
- A clip with neither chords nor a key: refused, `clip.no_key`.

**The item that results.**
- If every pass of a looped item maps every note the same way (one key
  region, or regions that give the same map), the item stays looped over the
  followed source.
- Otherwise the item is laid unrolled: loop off, its source written out pass
  by pass, each pass mapped at its own place. The answer says `unrolled:
  true`.
- `pooled: true` with a follow that changes any note is refused
  (`clip.follow_pooled`), since a shared source cannot be two keys. A follow
  that changes nothing lays it pooled.
- A result outside 0..127 is refused (`midi.pitch_range`, naming the clip and
  the note's beat), and nothing is laid.

**The answer.** Each laid item gains `followed: {mode, regions: [{from_bar,
to_bar, from_key, to_key, interval?, by}]}`.
- `by` is `transpose`, `degree` or `chords`.
- `interval` is in semitones, for `transpose`.
- `drums_kept` and `unrolled` appear when they apply, and the warnings go in
  the usual list.

**The mapping, `harmony::follow`** (one function; `harmony.md` §5 is its
contract):

`follow(pitch, from, to, mode) → {pitch, by}` where `from` and `to` are each
`{optional Key, optional Chord}`.
1. **Tonic interval.** `d = (pc(to.tonic) − pc(from.tonic)) mod 12` in 0..11.
   The interval is `d` if `d < 6`, `d − 12` if `d > 6`, and **−6 at the
   tritone** (a tie goes down, as every tie in the library does).
2. **Key, same scale** (`from.scale == to.scale`): `pitch + interval`
   (`by: transpose`).
3. **Key, different scale with the same number of degrees** (major, the
   minors and the modes are all seven):
   - Let `s` be the nearest pitch at or below `pitch` that is in `from`'s
     scale, `alt = pitch − s` (0 for an in-scale note, else 1 or 2), and
     `k`, `o` its degree and octave in `from`'s scale.
   - The result is `to's tonic placed at from's tonic + interval, plus
     to.intervals[k], plus 12·o, plus alt` (`by: degree`).
   - So an altered note keeps its alteration above the mapped degree.
4. **Key, different degree counts** (a pentatonic, blues or chromatic scale
   on either side): `pitch + interval`, as step 2 (`by: transpose`). The
   answer then warns `context.scale_shape`, naming both scales, since
   degrees do not correspond.
5. **Chords** (`mode = chords`, both chords present):
   - The from chord's tones in order are its root, then each interval of its
     quality (`chordQualities`, in the table's order), as pitch classes
     relative to the root.
   - If `pitch`'s class is tone `i`, the target class is the to chord's tone
     `i mod n`, where `n` is the to chord's tone count.
   - A tone beyond the target chord's count wraps by design: the target
     chord has no such tone, and the wrap gives every note a target among
     the chord's own tones instead of inventing an extension it does not
     have. So a seventh over a triad (Cmaj7's B, tone 3, to D) lands on the
     triad's root (D), in the octave nearest the key-followed pitch. It is
     not a bug.
   - If it is the slash bass of the from chord and not a tone, the target is
     the to chord's bass, or its root without one.
   - Any other note keeps its distance in scale degrees above the from root:
     - with keys on both sides and both roots in their scales, step 3's
       arithmetic is run with the roots in place of the tonics;
     - else `pitch + (pc(to.root) − pc(from.root))`, folded to −6..+5 as in
       step 1.
   - **Octave:** the result is the pitch of the target class nearest to
     `k = follow(pitch, from, to, key)`, the key-followed pitch (a tie goes
     down). Without keys on both sides, it is nearest to
     `pitch + root interval`.
   - `by: chords`.
6. **Range:** a result outside 0..127 is an error the caller turns into
   `midi.pitch_range`.

The function allocates nothing per call beyond its result, is deterministic,
and has no model dependency. The engine reuses it in X4 to build pitch
tables at compile time.

**Tests the builder writes with it** (`harmony.tests`):
- C major to D major moves every pitch +2;
- C major to F# major is −6;
- C major E4 to D minor is F4 (degree);
- C major F#4 to D minor is G#4 (the alteration kept);
- C major to C minor pentatonic is a transpose by 0, with the warning flag;
- chords Cmaj7 → Dm7, tone by tone;
- C/E's bass to G/B;
- a ninth over a triad, as `i mod n`;
- the octave choice at a tie;
- the range errors.

### The other direction (X5)

`arrangement.build {write_context: true}`: where the context track is empty
under a section, the scene's clips' context (when they agree) is written
there. Not in X1 or X2.

### Steps

| Step | Who | What |
|---|---|---|
| X1 | builder (model, commands) | The fields, schema 2.20 and its round trip; `clip.add {context, context_at}`, chord symbols recorded (with `Compiled::chords`), `clip.set`, `clip.get`, `midi.get`, describe; `clip.add {item}` and `scene.capture` take the context under the item; the `midi.edit` ops on clips (D1); the `none` mismatch warning at placement. |
| X2 | builder (harmony, commands) | `harmony::follow` with its tests; `follow` on `item.add {clip}`, `scene.insert`, `arrangement.build`, with unrolling, `clip.follow_pooled` and the answer; the song journey writes its Verse once, in C minor, and it lands right in D minor. |
| X3 | gui-lead | A clip's roll shows and edits its context (chord strip, scale shading); the clip inspector has Follow; a mismatch shows on a laid item's roll. |
| X4 | lead (engine) | A launched clip follows live: per key region and chord region pitch tables built at compile from `harmony::follow`, looked up by song position in the player without allocating; `launch.commit` lays what was heard, followed. |
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
  and the engine in X4, so offline and live cannot differ.
- **Clips grow by their context points.** No limit changes except the new
  per-clip 4096.
- **Items stay plain notes.** Nothing that plays today changes, since
  `follow` defaults to `none`.
- **D1 is fixed by X1.** It needs no separate fix.
