# Key, chords and sections: the context track

A project can say what key it is in and what chord is playing, bar by bar.
Agents and people both read it; the tracks do not follow it by themselves.
Spelling is the first thing that does: notes in `midi.get` and the piano roll
are named by the key at the item's start (`Bb` in F major, `A#` in D major).

## Reading it

- `session.describe` shows the key in its header and a `context:` block
  (`keys:` and `chords:` by bar), plus the later tempo and meter changes. A
  region that is not a section is marked `(not a section)`.
- `context.at {"bar": 5}` (or `"at": <beat>`): the key, the chord and the
  section that hold there. A point this build cannot read (an unknown scale,
  a chord quality it lacks) comes back with `known: false`; treat it as text
  to show, not to compute with.
- `context.get {"from": 0, "to": 64}`: the points in a range, by beat, with
  what holds at `from`.
- `midi.get` returns a `context` field with the keys and chords over the item,
  each with its bar in the item and in the project.

## Writing it

- `key.set {"at": 0, "tonic": "Eb", "scale": "major"}`. Scales: major, minor,
  harmonic_minor, melodic_minor, dorian, phrygian, lydian, mixolydian,
  locrian, major_pentatonic, minor_pentatonic, blues, chromatic. A key that
  needs a double accidental (G# major) is refused: write Ab major.
- `chord.set {"at": 8, "symbol": "Cm7"}`. The symbol is notation's grammar:
  root, quality, optional `/bass` (`Bb/D`). Aliases are written back in the
  canonical quality (`Cmin7` becomes `Cm7`). `""` means "no chord from here".
  An unknown quality (`Cm11`) is refused with the list of qualities.
- Setting a beat that already has a point of that type replaces it and keeps
  its id; give only the part to change. `to` moves a point and keeps its id.
- `key.remove {"at": 0}` and `chord.remove {"at": 8}` take a point away; the
  one before it continues.
- Positions are beats, rounded to 1/960. One key and one chord at most per
  beat; at most 4096 points of each type.

## The lead sheet: a whole progression in one call

`context.get` also returns `text`: the points as a lead sheet, one line per
project bar. `context.write {text}` writes one back. It reads bars as notation does: a
newline, a `;` or a ` | ` token (surrounded by spaces) ends a bar, and the number after `|`
may be left out (`1: key C major {C}:w | {F}:w` is bars 1 and 2); what it prints is always
one line per bar. The same goes for a clip's `context`.

```
1: key Eb major {Ebmaj7}:w
2: {Cm7}:h {Fm7}:h
3: ~:w
4: {Bb7}:h r:h
```

- `{SYMBOL}:DUR` is a chord from the cursor; `r:DUR` is "no chord from here";
  `~:DUR` holds whatever holds (no point); `key Eb major` is a key from the
  cursor and takes no time. DUR is notation's duration (`w h q e`, `h.`,
  `h+q`, `480t`). **Each bar's durations must fill the bar** (3/4 is `h.`);
  the error says by how much.
- Bars run in order without gaps. A `#` starts a comment only at the start of a
  word (`F#` is a sharp).
- **A write is a diff over the bars it names**, keys and chords both: points that
  are the same stay (ids kept), points in those bars that the text leaves out are
  removed, the rest are added or changed. To change one bar, send just that bar:
  `context.get {"from": 4, "to": 8}` prints it. To leave a bar's key alone, keep
  its `key` token in the text.
- History records the individual `chord.set`, `key.set` and remove steps, so two
  branches that rewrite different bars merge.

## Edits that read the key and chord

`midi.edit` ops (see `generated/engine-commands.md`), applied in order:

- `{"op": "transpose_degrees", "notes": [0, 1], "degrees": 2}` moves notes by
  scale degrees in the key at each note's beat (a note outside the scale first
  goes down to the scale). A third up in C major turns C4 into E4.
- `{"op": "snap_to_scale", "mode": "nearest"}` moves notes outside the scale to
  it; `mode` is `nearest` (a tie goes down), `down` or `up`. Without `notes` it
  means every note.
- `{"op": "fit_to_chord"}` moves each note to the nearest tone of the chord at
  its beat (a tie goes down; a slash bass counts).
- `key` (`"Eb major"`) and `chord` (`"Cm7"`) in an op replace what the context
  says, for that op.

On a launcher clip (`guides/launcher.md`) the ops read **the clip's own** key and chord,
the ones it was written against (`clip.set {context}`), never the project's.

If no key or chord holds there, you get `midi.no_key` or `midi.no_chord` and a
hint. If a loop or another item sharing the same MIDI puts one note in two keys
(or over two chords), you get `midi.context_varies`: pass `key` or `chord`, or
unloop or unshare the item first.

## Clips and their context

A clip keeps its own key and chords, written in the same lead sheet text over
**clip bars** (bar 1 is the clip's start): `clip.add {context: "1: key D minor
{Dm}:w\n2: {A7}:h {Dm}:h"}`, or `context_at: {bar: 5}` to read them from the
project, and `clip.set {context}` to change them (`""` clears). A text for a clip
may run past its end, but a key or chord point may not (`clip.context_range`); an
audio clip's text is a key at the start of bar 1 with `~` holds only. The chord
symbols in a clip's notation (`{Dm}:w`) are recorded as its chords too. `midi.get`
shows them as `context` (runs by clip bar and beat, and `text`) with the clip's
`follow`. `guides/launcher.md` has the rest.

## Sections

Regions are sections unless told otherwise. `region.add {"section": false}` or
`region.set {"id": "rg_1", "section": false}` marks a region that is only a
span to loop or label. `context.at` names the section at a beat: the region
that starts latest and covers it. `scene.insert` and `arrangement.build` add a
section for every scene they lay out, named after it (`guides/launcher.md`). With
`write_context: true` they also write the scenes' clips' key and chords onto the context
track under each section that has none, so a song built from scenes gets its lead sheet
without writing it twice.

## Rules of thumb

- Set the key once at beat 0, and again only where it changes; set chords at
  every change, and `""` where the harmony stops.
- After a key change, read a part back with `midi.get` before writing more:
  the spelling follows the key, and what you read is what compiles back.
- Chords and keys are information. Changing them moves no note and costs no
  recompile of the audio.
