# harmony

Status: contract for `libs/harmony` (ADR-0095). MIT, namespace
`supple::harmony`, header `supple/harmony/harmony.h`. No dependency on the
model, the engine or the notation; it links `core-base` only. The notation
compiler, the session model, the engine commands and the GUI link it.

The library knows keys, scales, chords and spelling, and answers "what holds
at beat t" over plain structs. It allocates, so it is never called on the
audio path.

## 1. Types

```cpp
struct PitchClass { char letter; int accidental; };      // 'A'..'G', -1 | 0 | +1
struct Key        { PitchClass tonic; Scale scale; };
enum class Scale  { Major, Minor, HarmonicMinor, MelodicMinor, Dorian, Phrygian,
                    Lydian, Mixolydian, Locrian, MajorPentatonic, MinorPentatonic,
                    Blues, Chromatic };
struct Chord      { PitchClass root; std::string quality; std::optional<PitchClass> bass; };
```

Scale names (file and command spelling): `major`, `minor`, `harmonic_minor`,
`melodic_minor`, `dorian`, `phrygian`, `lydian`, `mixolydian`, `locrian`,
`major_pentatonic`, `minor_pentatonic`, `blues`, `chromatic`. `parseScale`
returns nothing for any other name; a file keeps an unknown name and only the
commands refuse it.

Intervals above the tonic, in semitones:

| scale | intervals | scale | intervals |
|---|---|---|---|
| major | 0 2 4 5 7 9 11 | dorian | 0 2 3 5 7 9 10 |
| minor | 0 2 3 5 7 8 10 | phrygian | 0 1 3 5 7 8 10 |
| harmonic minor | 0 2 3 5 7 8 11 | lydian | 0 2 4 6 7 9 11 |
| melodic minor | 0 2 3 5 7 9 11 | mixolydian | 0 2 4 5 7 9 10 |
| major pentatonic | 0 2 4 7 9 | locrian | 0 1 3 5 6 8 10 |
| minor pentatonic | 0 3 5 7 10 | blues | 0 3 5 6 7 10 |
| chromatic | 0 1 2 3 4 5 6 7 8 9 10 11 | | |

Melodic minor is the ascending form in both directions.

## 2. Functions

- `parsePitchClass(text)`, `printPitchClass(pc)`: a letter (either case on
  input, upper case on output) and at most one `#` or `b`. `Eb` and `D#` are
  different values with the same sounding pitch; `pitchClassNumber(pc)` is 0
  to 11 and `pitchClassSemitone(pc)` is the unreduced value (`Cb` is -1, `B#`
  is 12).
- `parseChord(text)` / `printChord(chord)`: the notation's chord grammar
  (notation spec, "Chord symbols"): root, quality, optional `/bass`.
  `parseChord` fails with the quality list. `printChord` writes the
  canonical quality (the first of its aliases: empty for `maj`, `m` for
  `min`, `aug` for `+`, `sus4` for `sus`, `maj7` for `M7`, `m7` for `min7`,
  `m7b5` for `ø`) and the root and bass as written. `chordPitches(chord,
  octave)` voices it as the notation does: close position, the root in
  `octave`, each further tone the table's interval above the root, a slash
  bass first, in `octave - 1` (so the pitches are ascending). It fails with
  `chord.bad_root`, `chord.bad_bass` (not a MIDI pitch) or `chord.pitch_range`;
  `parseChord` fails with `chord.bad_root`, `chord.unknown_quality` (the hint
  lists the qualities) or `chord.bad_bass`. `chordQualities()` is the one table
  (31 names) the notation compiler and the commands share.
- `spellPitchClass(pc, key?)` and `spell(pitch, key?)`: the name of a pitch
  class, or of a MIDI pitch with its octave (`Bb3`; scientific, C4 = 60; the
  octave follows the letter, so `B#3` is the pitch of C4), as in the rule
  below. With no key, the default table.
- `transposeDegrees(pitch, key, degrees)`: a MIDI pitch moved by scale
  degrees. An in-scale pitch moves to the degree `degrees` away (octaves
  carry, negative degrees go down); an out-of-scale pitch first goes to the
  nearest scale pitch below, then moves. A chromatic scale has twelve
  degrees. The result is clamped to 0..127 by returning nothing when it falls
  outside.
- `keySupported(key)`: false when spelling needs a double accidental.
- `ContextIndex(keyPoints, chordPoints)` from plain `KeyPoint{beat, key}` and
  `ChordPoint{beat, optional chord}` vectors (any order; an empty chord is "no
  chord from here"); `keyAt(beat)`, `chordAt(beat)`
  (the last point at or before `beat`, none before the first), `keyPoints()`,
  `chordPoints()`. Two points of one type at one beat are a caller error; the
  index keeps the later in input order. Immutable once built.

## 3. The spelling rule

`spell(pitch, key)` names the pitch class of `pitch` from the key:

1. **Diatonic family** (major, minor, harmonic and melodic minor, the five
   other modes): degree *k* of the scale takes the *k*-th letter after the
   tonic's letter, with the accidental that gives its pitch class. A key that
   needs a double accidental for a degree is not supported
   (`keySupported` false; `key.set` refuses it with `key.unsupported`).
   Pitch classes outside the scale take the key's *direction*: the flat table
   `C Db D Eb E F Gb G Ab A Bb B` when the spelled scale has more flats than
   sharps, the sharp table `C C# D D# E F F# G G# A A# B` when it has more
   sharps, otherwise the default table below (a scale with no accidentals, such as C major, A
   minor or D dorian, or with equally many).
2. **Pentatonic** takes the letters of its parent (major pentatonic: major;
   minor pentatonic: natural minor). **Blues** is minor pentatonic plus the
   lowered fifth, spelled as the fifth's letter with a flat. **Chromatic**
   takes the direction of the major scale on the same tonic and the tables
   above for every pitch class.
3. **No key:** the default table `C C# D Eb E F F# G Ab A Bb B`, which is the
   notation's printer today.

Examples: E-flat major `Eb F G Ab Bb C D`; C-sharp minor `C# D# E F# G# A B`;
A blues `A C D Eb E G`; F-sharp major needs `E#`, a letter with one sharp, and
is supported; G-sharp major needs `F##` and is not.

## 4. Tests required

Every scale's spelling for all 21 tonic spellings (seven letters, each plain,
`#` and `b`), supported or refused, with the 15 major and 15 minor key
signatures checked by hand; out-of-scale direction; the default table equal to
the notation printer's output for 0..127; chord parse and print round trips
for every alias; `chordPitches` equal to the compiler's output for every
quality and a slash bass; `transposeDegrees` over each scale across octave
edges and out of scale; the index against a brute-force scan.
