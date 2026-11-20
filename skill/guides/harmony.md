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

## Sections

Regions are sections unless told otherwise. `region.add {"section": false}` or
`region.set {"id": "rg_1", "section": false}` marks a region that is only a
span to loop or label. `context.at` names the section at a beat: the region
that starts latest and covers it.

## Rules of thumb

- Set the key once at beat 0, and again only where it changes; set chords at
  every change, and `""` where the harmony stops.
- After a key change, read a part back with `midi.get` before writing more:
  the spelling follows the key, and what you read is what compiles back.
- Chords and keys are information. Changing them moves no note and costs no
  recompile of the audio.
