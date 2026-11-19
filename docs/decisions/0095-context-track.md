# ADR-0095: The context track: sections, key and chords on the timeline

**Status:** Proposed, 2026-11-19.

Contracts: `specs/session-model.md` "Context track (schema 2.17)" (the model, the
file, errors, describe), §1.10, §3, §7 (commands); `specs/harmony.md` (the library); `specs/notation.md`
("Chord symbols"). Part of the headline design in `designs/headline-core.md`
§0, §1 and §6; the survey that grounds it is `reports/survey-context.md`.

## Context

Nothing in Steppe knows what key a song is in, where its sections are or
which chord is sounding. Markers and regions name places on the timeline and
nothing reads them. Notation spells every pitch from one key-blind table
(`C C# D Eb E F F# G Ab A Bb B`) and reads chord symbols but never prints or
detects them. The piano roll, `midi.edit` and `session.describe` know pitches
and beats only. Five more pitch-name tables sit in the GUI and the engine.

Agents and people both think in sections, keys and chords. The same answer
should reach notation, the piano roll, note-editing commands, the session
description and, later, racks and the clip launcher.

## Decision

**Sections are regions.** `Region` gains `section` (boolean, default true).
The section at a beat is the region with `section` true that covers the beat
and starts latest (ties: the shorter, then the lower id), found at query time
through the current time map, because a region may be time-based. A beat no
section covers has no section. Every region, section or not, stays a
render-matrix row (`render.*` defaults to all regions): the ADR does not
change that, and `session.describe` and `context.at` name a region that is
not a section only by its `section: false`. `region.add` and `region.set`
take `section`.

**One new kind, the context point** (`cx_*`, prefix `cx`):
`{beat, type: key | chord, …}`, beats only (no timebase).

- A `key` point holds `tonic` (a spelled pitch class: a letter and at most
  one `#` or `b`; `Eb` is not `D#`) and `scale`, a name from the fixed table
  in `specs/harmony.md`. A key holds from its beat until the next key point.
  Before the first key point no key holds.
- A `chord` point holds `symbol` in the notation's chord grammar (root,
  quality, optional slash bass), stored canonically: the root and bass as
  written, the quality as the first name in the table below. An empty symbol
  means "no chord from here". A chord holds until the next chord point.
- Invariants: at most one point of a type at a beat; beats finite and ≥ 0;
  the symbol parses; the number of points is at most 4096 per type.
  `at` and `to` are rounded to 1/960 beat (the MIDI sequence's ppq), so a
  point always lies on the grid the lead sheet and notation use.
- **Project level only.** There is no per-track key in this work.

**Stored per entry**, so branches merge: each point is its own object in the
`cx` collection of the session, with `rev`, diff and merge like a marker. The
file has a top-level `context` section (`points`, in id order, each
`{id, beat, type, …}` with defaults omitted) and writes the `cx` counter in
`next_ids` once used. The schema is **2.17**: modulation (ADR-0096)
lands first and takes 2.16 (design amendment G-d; numbers follow the order of
landing on integration). A build that does not know the
section keeps it verbatim as an unknown top-level section, so a file that
passes through it returns with the points intact and the `cx` counter
missing.

**A file that went through an older build never fails to open for this.** On
load a missing or low `cx` counter is raised to the highest `cx` id plus one,
with a `project.counter_derived` warning. A point with a duplicate beat of
its type keeps the lowest id and drops the rest with a warning naming them; a
point whose symbol or scale name the loader does not know is kept (an unknown
scale name is data; only the commands refuse it). Points hold no references
to other objects, so nothing else can dangle. A hand-edited file tests each
rule.

**The index.** A derived, immutable sorted index over the points answers
`holdsAt(beat)` (key, chord) in O(log n). It is rebuilt only when the
collection's storage changes and reused when storage is shared, like the
`TimeMap` but not part of it. It indexes points only. The section is a scan
of the regions through the current `TimeMap`: the count is small and a tempo
edit moves time-based regions.

**`libs/harmony` is MIT** and has no model dependency. It holds the key,
scale and chord types, the scale table, chord parse, print and pitches
(lifted out of the notation compiler, which then calls it),
`spell(pitch, key)`, `transposeDegrees(pitch, key, degrees)` and the index
over plain structs. Notation, the model, the engine commands and the GUI link
it. The rack modules and the analysis library may link it later: that is why
it is MIT.

**One pitch-name function.** The tables in the notation printer, the piano
roll (`midi_editor.cpp`), the MIDI event list, `midi_control.cpp` and
`rack_probe.cpp` become calls to `spell`. With no key `spell` returns the
notation's table, so the piano roll's `A#` becomes `Bb` and every agent-facing
name changes with it. The analysis library's label for a detected frequency
(`analysis.cpp`, with cents) stays: it has no key to give. Input keeps
accepting both spellings.

### The spelling rule

`spell(pitch, key)` names a pitch class from the key's tonic and scale:

1. **Diatonic family** (major, natural minor, harmonic minor, melodic minor,
   the modes): each scale degree takes the next letter after the tonic's
   letter, with the accidental that makes its pitch class (so C-sharp minor
   has `C# D# E F# G# A B`). If a degree would need a double accidental, the
   key is refused by `key.set` with `key.unsupported` and a hint naming the
   enharmonic key (G-sharp major asks for A-flat major). Pitch classes the
   scale does not contain take the *direction* of the key: flats
   (`C Db D Eb E F Gb G Ab A Bb B`) when the spelled scale has more flats
   than sharps, sharps (`C C# D D# E F F# G G# A A# B`) when it has more
   sharps, the notation's default table when it has equally many (a scale with
   no accidentals, such as C major, A minor or D dorian).
2. **Pentatonic** takes the letters of its parent scale (major pentatonic:
   major; minor pentatonic: natural minor); **blues** is minor pentatonic
   plus a lowered fifth spelled as the fifth's letter with a flat (A blues
   has `Eb`); **chromatic** uses the direction of the major scale on the
   same tonic.
3. With no key, the notation's default table.

`transposeDegrees(pitch, key, degrees)` moves a pitch by scale degrees in the
key's scale: an in-scale pitch moves to the degree `degrees` away (octaves
carry); an out-of-scale pitch is first moved to the nearest scale pitch below
(a tie goes down), then moved. A chromatic scale has twelve degrees, so
degrees are semitones.

### Chord vocabulary

A chord symbol is exactly what the notation parser takes: a root (letter and
at most one `#` or `b`), a quality from its table of 31 names and an optional
`/bass`. The canonical quality for each interval set is the first name below;
the others are accepted and stored as the canonical one.

| canonical | accepted aliases |
|---|---|
| (empty) | `maj` |
| `m` | `min` |
| `aug` | `+` |
| `sus4` | `sus` |
| `maj7` | `M7` |
| `m7` | `min7` |
| `m7b5` | `ø` |
| all others | none |

`chord.set` and `key.set` refuse what does not parse with the quality list
(or the scale list) in the error, as the compiler does. Qualities the table
lacks (`m11`, `6/9`, alterations beyond `7b9` and `7#9`, inversions other than
a slash bass) are a later extension of the one table in `libs/harmony`; the
compiler and the commands follow it together.

### Commands and what agents see

- `key.set {at, tonic, scale, to?}` adds a key point at `at` or replaces the
  one there; with `to` it moves the point that is at `at` to `to`, keeping
  its id. `key.remove {at}`. `chord.set {at, symbol, to?}` and
  `chord.remove {at}` likewise. Moving onto a beat that holds a point of the
  type is `context.occupied`, with a hint to remove it first.
- `context.at {at}` (q): `{beat, bar, key?, chord?, section?}` for the
  project beat or `bar`. `context.get {from?, to?}` (q): the points in a
  range, and (C2) the lead sheet text.
- `session.describe` gains the key in its header line and a `context:` block
  after `regions:` (key changes and chord runs, by project bar). In the same
  change it lists the later tempo and meter changes it omits today. `midi.get`
  gains a `context` result field: the key and chords over the item's span,
  with item bars and project bars both named. Notation gets no `key` header
  in this work.
- `midi.edit` gains `transpose_degrees`, `snap_to_scale` and
  `fit_to_chord` ops (C2). They take optional explicit `key` and `chord`
  arguments; without them they read the context at each note's project beat
  in the item's first pass, refuse with `midi.context_varies` (and a hint)
  when a loop pass or a pooled sharer would see a different key or chord, and
  apply in order within one `midi.edit`. A clip reads the key that holds at
  the project's start and no chords.

### The lead sheet (C2: proposed here, built in the next step)

`context.get` prints and `context.write {text}` reads the context in the
notation's vocabulary, in project bars, one line per bar:

```
1: key Eb major {Ebmaj7}:w
2: {Cm7}:h {Fm7}:h
3: ~:w
4: {Bb7}:h r:h
```

`{SYMBOL}:DUR` is a chord point at the cursor; `~:DUR` holds what holds (no
point); `r:DUR` is a chord point with an empty symbol; `key TONIC SCALE` is a
key point at the cursor and takes no time. A bar's durations fill the bar.
Printing is canonical and round-trips. **A write is a diff:** it covers from
the first bar named to the end of the last; points equal in beat and content
keep their ids and are not touched, points in that range the text omits are
removed, the others are added or replaced. It is recorded in history as the
`chord.set`, `key.set` and remove operations it resolved to, not as its text
(the `take.hits.detect` to `take.hits.set` pattern is the precedent), so two
branches that write different bars merge.

### Engine

None in the first steps. The compile decision matters, though: the plan does
not read the context, so a context edit must not compile one. The three
equivalence checks (`scalarOnly`, `mixOnly`, `patchOnly`) qualify fast paths
only when something they handle changed, so a context-only edit falls through
them to a full compile. `Engine::loadNow` therefore gains a fourth shortcut,
`contextOnly`: when only the context collection differs between the running
and the new session, nothing is compiled and `latest_` takes the new session.
When a note source or a rack module reads the context, an edit compiles only
if something in the project reads it.

## Alternatives not taken

- **Sections as a new kind.** Regions are named, coloured ranges that the
  render matrix already uses; a flag says which are sections. A second kind
  would cost a dozen hand-kept sites and two ways to say the same thing.
- **A blob or a replaced list, as `tempo.set` does.** Two branches that edit
  any two points would conflict on all of them; per-entry objects merge.
- **The tempo map's home.** `TimeMap` is rebuilt on every commit and joins
  seconds and beats; the context has no seconds and changes far less often.
- **A per-track key.** Not in this work; the model leaves room (a `track`
  field on a point later).
- **A notation `key` header.** Whether it means an item-local key or sets the
  project's is not settled; the engine already drops the `tempo` header, and
  a dropped `key` header would be worse. Not in this work.

## Consequences

- Agents and people read one answer for "what holds at beat t".
- Every pitch name an agent sees can follow the key. The default table
  changes the piano roll and MIDI-control names from sharps (`A#`) to the
  notation's (`Bb`); generated docs and golden images that show them change.
- A file that passed through an older build loses nothing but the counter,
  which is derived.
- The first steps add no engine work, and a context edit compiles nothing.
- The lead sheet and the scale ops are separate, later steps (C2); the chord
  row as a live note source, `chords.render`, `chords.detect` and a rack
  Context module are later (design §1.13).
- Limits and the marker/region spec mismatches this change fixes are in
  `specs/session-model.md` §1.10.
