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
- `keyPitchClasses(key)` and `chordPitchClasses(chord)`: the sorted classes
  0..11 of a scale and of a chord's tones (root, the quality's intervals, a slash
  bass). `snapToPitchClasses(pitch, classes, mode)` with `SnapMode`
  `Nearest` (a tie goes down), `Down`, `Up`: the pitch itself when its class is in
  `classes`, else the nearest such pitch in 0..127 in that direction; nothing for an
  empty set or a pitch outside 0..127 or when none lies in range.
  `snap_to_scale` and `fit_to_chord` are these.
- `keySupported(key)`: false when spelling one of the scale's own degrees needs a
  double accidental (the scale's degrees decide, not its parent's; chromatic is
  always supported).
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
2. **Pentatonic** (support is decided by its own five degrees: G-sharp major
   pentatonic `G# A# B# D# E#` is supported though G-sharp major is not,
   D-sharp major pentatonic needs `F##` and is not) takes the letters of its parent (major pentatonic: major;
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
signatures checked by hand; support by the scale's own degrees (G-sharp major
pentatonic supported, D-sharp major pentatonic refused, chromatic always
supported); out-of-scale direction; the default table equal to
the notation printer's output for 0..127; chord parse and print round trips
for every alias; `chordPitches` equal to the compiler's output for every
quality and a slash bass; `transposeDegrees` over each scale across octave
edges and out of scale; the pitch classes of keys and chords (a slash bass counts);
snapping in each mode, ties, the range edges and a one-class set, and every pitch of
every scale in every key landing in the scale within six semitones; the index
against a brute-force scan.

## 5. Following (ADR-0098; the contract for step X2, built)

*(Amended 2026-11-21, ADR-0098 "Amendment: following is playback": `follow` is
unchanged, and its callers change. Placement no longer writes its results into
notes. `model::soundingKey` (`session-model.md`, "Take context and follow"), in
the model library, calls it for every note that sounds: the engine's compile,
`take.bake`, `midi.get {sounding}` and `item.export`. This library still does not
see the model.)*

```cpp
struct Place { std::optional<Key> key; std::optional<Chord> chord; };
enum class FollowMode { Key, Chords };
enum class FollowedBy { Transpose, Degree, Chords, Copy };
struct Followed {
    int pitch;               // always in 0..127
    FollowedBy by;
    bool scaleShape = false; // step 4 applied: the caller warns context.scale_shape
    bool folded = false;     // step 7 moved it by octaves into range
};
// A MIDI pitch (0..127) written against `from`, mapped to `to`. It never fails:
// what cannot be mapped is copied (`Copy`), and a result out of range is folded.
Followed follow(int pitch, const Place& from, const Place& to, FollowMode mode);
```

`tonicInterval(a, b)` is step 1's `iv` as a public function (a tonic interval from `a` to `b`,
−5..6, the tritone −6), for callers that report the interval a transposition used.

**Notation.**
- `pc(x)` is `pitchClassNumber`.
- `I(scale)` is `scaleIntervals` and `m(scale)` its size.
- A key or a chord that does not parse (a later build's scale or quality) is
  absent: callers pass none.

1. **Tonic interval** `iv(a, b)`: `d = (pc(b) − pc(a)) mod 12`. It is `d` when
   `d < 6`, `d − 12` when `d > 6`, and `−6` when `d = 6` (a tie goes down, as
   in §2's snapping).
2. **Key mode, a key missing on one side or on both:** the pitch unchanged,
   by `Copy`. Steps 3 to 5 need both keys. A clip key with no key at the
   place is the ordinary case (placement's `context.no_key`).
3. **Key mode, same scale:** `pitch + iv(from.tonic, to.tonic)`, by
   `Transpose`.
4. **Key mode, different scales with `m(from) = m(to)`:**
   - Let `s` be the highest pitch `≤ pitch` whose class is in `from`'s
     scale, and `alt = pitch − s`. `s` is an integer that may be below 0
     (pitch 0 in B major gives `s = −1`); it is never searched for within
     0..127 only.
   - Let `r = s − pc(from.tonic)`, `o = floor(r / 12)`, and `k` the index
     with `I(from)[k] = r − 12·o`.
   - The result is `pc(from.tonic) + iv(from.tonic, to.tonic) + 12·o +
     I(to)[k] + alt`, by `Degree`.
   - `alt` is measured from the degree below, so a note two semitones above
     a degree whose next degree is three semitones up (harmonic minor's
     sixth, a pentatonic's gaps) maps to the next degree's image, and
     distinct notes may merge. A harmonic minor's G4 and G#4 both go to B4
     in C major. This is deliberate: the rule keeps degrees, and no rule can
     keep both degrees and every distinction across a gap that narrows.
   - A relative key moves every note, tonic to tonic: C minor to Eb major
     raises C, Eb and G to Eb, G and Bb.
5. **Key mode, `m(from) ≠ m(to)`** (a pentatonic, blues or chromatic scale on
   one side): as step 3, with `scaleShape`.
6. **Chords mode.** With a chord missing on either side, Key mode for that
   note. Otherwise:
   - **Slots.**
     - `tones(chord)` is `chordQualities()`'s list for the chord's quality,
       exactly as stored. It begins with 0, the root, and its intervals may
       exceed 12.
     - Each tone has a slot by its interval: 0 root; 2, 3, 4, 5 third (sus2
       and sus4 stand in for the third); 6, 7, 8 fifth; 9, 10, 11 seventh (a
       sixth takes the seventh's slot); 13, 14, 15 ninth; 17 eleventh; 21
       thirteenth.
     - Its class is `(pc(root) + interval) mod 12`.
     - No quality in the table has two tones in one slot.
   - **A chord tone.** If `pitch`'s class is a tone of `from` in slot S
     (checked from the root's slot upward; the first match), the target
     class is `to`'s tone in slot S. When `to` has none:
     - for a ninth, eleventh or thirteenth, `to`'s root, third or fifth
       respectively, or, when `to` has no tone there either, its tone in
       the highest slot below that one (C11 → D5 maps the eleventh F to D);
     - otherwise `to`'s tone in the highest slot below S that it has, its
       root at least.

     So C11 → Dm7 maps C, G, Bb, D, F to D, A, C, D, F; C5 → Dm maps G to A;
     C13 → D maps C, E, Bb, D, A to D, F#, A, D, A.
   - **A slash bass that is also a tone** maps as that tone: C/E → G/D maps E
     to B. A slash bass that is not a tone (C/D) maps to `to`'s bass, or its
     root without one: C/D → G/A maps D to A.
   - **Any other note** keeps its degree distance above the root, as a class:
     - If both keys are present, `m(from) = m(to)`, and each root's class is
       in its key's scale:
       - let `s` and `alt` be as in step 4, `j` the degree of `s` in `from`'s
         scale, and `kr` and `kt` the degrees of `from`'s root in `from`'s
         scale and of `to`'s root in `to`'s scale;
       - the target class is `(pc(to.tonic) + I(to)[(kt + j − kr) mod m] +
         alt) mod 12`.
       - So C over Dm in C major, to Em in D major, gives D (the seventh to
         the seventh); A over G7 in C major, to A7 in D minor, gives Bb.
     - Otherwise the target class is `(pc(pitch) + iv(from.root, to.root))
       mod 12`.
   - **Octave.** The result is the pitch of the target class nearest to the
     reference: `follow(pitch, from, to, Key)` computed without step 7, or
     `pitch + iv(from.root, to.root)` when that gives `Copy`. A tie goes
     down. By `Chords`.
   - **Duplicates.** `follow` maps each note on its own, so two notes may map
     to one pitch at one start: Cmaj7 voiced C4 E4 G4 B4 → C5 gives C4 C4 G4
     G4. A caller that writes the notes keeps one of identical notes, as every
     sequence does (`Sequence::canonicalize`): placement writes C4 G4, and
     X4's tables must play such a pair once.
7. **Range.** A result above 127 goes down by octaves, and one below 0 up by
   octaves, until it is in 0..127, with `folded`. The live player (X4) folds
   alike, so a laid clip and a launched one give the same notes.

Deterministic. It allocates nothing (not even a first-use table: it reads the
constant quality table `chordQualities()` is built from) and has no model or
engine dependency.
X4 builds the engine's pitch tables from it.

**Tests (X2; this is the only test list, and ADR-0098 points here):**
- **Key mode:**
  - C major → D major is +2 for every pitch 0..125;
  - 126 and 127 fold to 116 and 117 with `folded`;
  - C major → F# major is −6;
  - C major E4 → D minor is F4;
  - C major F#4 → D minor is G#4;
  - A minor → C major: A4 → C5, C5 → E5, G4 → B4;
  - C minor → Eb major: C4, Eb4, G4 → Eb4, G4, Bb4;
  - A harmonic minor → C major: G4 → B4 and G#4 → B4;
  - C major pentatonic → A minor pentatonic: F#4 → E4;
  - C major → C minor pentatonic is a transpose with `scaleShape`;
  - a key missing is `Copy`: on the `from` side only, on the `to` side
    only, and on both.
- **Chords mode:**
  - Cmaj7 → Dm7, tone by tone, in each octave; at the top, G9 (127) with
    keys C major → D major goes to A9 (129) by the nearest rule and folds to
    A8 (117);
  - C11 → Dm7;
  - C5 → Dm;
  - C13 → D;
  - C11 → D5 (the eleventh F → D);
  - Cmaj7 → C5 (the duplicate rule);
  - C/D → G/A (D → A);
  - C/E → G/D (E → B);
  - C over Dm (C major) → Em (D major) gives D;
  - A over G7 (C major) → A7 (D minor) gives Bb;
  - the octave tie (keys C major → F# major, chord C held: C4 → C3);
  - a missing chord falls back to Key mode.
