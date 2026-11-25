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
struct PitchClass { char letter; int accidental; };      // 'A'..'G', -2..+2
struct Key        { PitchClass tonic; Scale scale; };
enum class Scale  { Major, Minor, HarmonicMinor, MelodicMinor, Dorian, Phrygian,
                    Lydian, Mixolydian, Locrian, MajorPentatonic, MinorPentatonic,
                    Blues, Chromatic };
struct Chord      { PitchClass root; std::string quality; std::optional<PitchClass> bass; };
```

`accidental` is -2..+2 (ADR-0100). A parsed name (a key's tonic, a chord's root or bass, a
command's argument) has -1, 0 or +1; only a spelling, a chord tone named by its degree, produces `##` and
`bb`. `printPitchClass` writes one mark per step.

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
  below. With no key, the default table. The same two functions take a `Place`
  (key and chord, §5's struct, declared with the spelling) instead of a key: a
  chord tone is named by its degree and a note of the chord's step by the chord
  (§3.1); a place with no chord is the key's rule. A name whose octave would fall
  outside -1..9 (`B#` at pitch 0 is `B#-2`, which the notation refuses) is replaced by
  the key's table, or the default table (`C-1`); this holds for a key alone as well.
- `chordTones(chord)`: the tones of a chord with their degrees, root first, then the quality's tones in
  table order, then a slash bass last: `ChordTone{interval, degree, name, byDegree}`. `degree` is
  `1`, `b3`, `5`, `b7`, `9`, `#5`, `bb7`, `#9` and so on (`bass` for the slash bass,
  whose interval is its distance above the root, 0..11, and whose name is as written);
  `name` is the tone as §3.1 spells it; `byDegree` is false for the three tones that
  would need three accidentals, whose `name` is then the default table's. Empty for a
  quality this build does not know.
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

4. **A chord** (ADR-0100). With a chord that this build can read at the place, the
   rule is, in order:
   1. a **slash bass** whose class is the pitch's: the bass as written (`E/Ab` names the
      class `Ab` although the chord's own third is `G#`);
   2. a **chord tone**: its degree's name (§3.1), whatever the key (the third of E major is
      `G#`, never `Ab`; the seventh of C7 is `Bb`). A tone whose degree needs three accidentals
      goes on to steps 3 and 5, never to 4;
   3. a note of the **key's scale**: the key's spelling, as rule 1 or 2 gives it (a scale note
      keeps its spelling over any chord: `F` over F#7 in C major, not `E#`);
   4. any other note, by its **step above the chord's root**, `i` semitones: 1 is the flat
      second (`Db` over C), 3 the flat third (`Eb`), 6 the sharp fourth over a major or absent third
      and the flat fifth over a minor one (`F#` over C7, `Gb` over Cm), 8 the flat sixth (`Ab`), 10 the flat seventh (`Bb`),
      11 the major seventh (`D#` over E, `G#` over Am); 0, 2, 4, 5, 7 and 9 are the major scale on the
      root. The letter is the step's, the accidental whatever gives the pitch class; a name with
      more than two accidentals goes to step 5;
   5. the key's direction table, or the default table with no key, as rule 1 gives it.

   A place with no chord, and a chord whose quality this build does not know (callers pass none),
   follow rule 1, 2 or 3 as before; every output with no chord is what it was.

Examples: E-flat major `Eb F G Ab Bb C D`; C-sharp minor `C# D# E F# G# A B`;
A blues `A C D Eb E G`; F-sharp major needs `E#`, a letter with one sharp, and
is supported; G-sharp major needs `F##` and is not.

### 3.1 Degrees

A chord tone is named by its degree: the letter is the root's letter moved by the degree's number
(a third is two letters on, a fifth four, a seventh six, a ninth one, an eleventh three, a thirteenth five),
and the accidental is whatever gives the pitch class, root plus interval. The degree is a column of
the quality table beside the intervals (`kQualityRows`), so no quality exists without it:

| interval | degree | qualities |
|---|---|---|
| 0 | 1 | all |
| 2 | 2 | sus2 |
| 3 | b3 | m, dim, m6, m7, mMaj7, dim7, m7b5, m9 |
| 4 | 3 | major, aug, 6, 7, maj7, add9, 9, maj9, 7b9, 7#9, 13 |
| 5 | 4 | sus4, 7sus4 |
| 6 | b5 | dim, dim7, m7b5 |
| 7 | 5 | all but aug, dim, dim7, m7b5, 13 |
| 8 | #5 | aug |
| 9 | 6, or bb7 | 6 and m6 take the sixth; dim7, the only quality with 3, 6 and 9, the double-flat seventh |
| 10 | b7 | 7, m7, m7b5, 9, m9, 11, 13, 7sus4, 7b9, 7#9 |
| 11 | 7 | maj7, mMaj7, maj9 |
| 13 | b9 | 7b9 |
| 14 | 9 | add9, 9, maj9, m9, 11, 13 |
| 15 | #9, **lettered as the b3** | 7#9 |
| 17 | 11 | 11 |
| 21 | 13 | 13 |

The sharp nine takes the third's letter, as printed music writes it: `G` over E7#9, `Eb` over C7#9,
never `F##` or `D#`. Added tones (add9, 6, m6) and sus tones are named by their own degree, and the
tones the table lacks (the third of 11, the fifth of 13) are simply absent.

Where the degrees need a double accidental (every chord, every one of the 21 root spellings): the tones
of `Cdim7` are `C Eb Gb Bbb`, `Bdim7` `B D F Ab`, `C#aug` `C# E# G##`, `Ebdim` `Eb Gb Bbb`; with the twelve
usual roots (`C C# D Eb E F F# G Ab A Bb B`) the doubles are `Ebdim:Bbb`, `Abdim:Ebb`, `C#aug:G##`,
`F#aug:C##`, `Baug:F##`, `Cdim7:Bbb`, `Ebdim7:Bbb,Dbb`, `Fdim7:Ebb`, `Abdim7:Ebb,Gbb`, `Bbdim7:Abb`,
`Ebm7b5:Bbb`, `Abm7b5:Ebb` and `Ab7b9:Bbb`. Three tones over all roots would need three: the fifth of
`B#aug` (`F###`) and the sevenths of `Cbdim7` (`Bbbb`) and `Fbdim7` (`Ebbb`). They are named by
the key's rule, the default table when no key holds (`byDegree` false); a chord is never refused for
its spelling. Input is unchanged: the notation reads `##` and `bb` in a pitch, a chord root or
bass takes one accidental (`chord.bad_root`, `chord.bad_bass`).

### 3.2 Who spells by place

The place of a note is the key and chord that hold where it is heard. `midi.get`, the MIDI
editor (roll labels and messages, the notation panel, the event list) and `rack.probe` (its note
names, at the beat each is heard at, `specs/rack.md` §7.2) spell by it. These do not, on purpose:
a MIDI-control label (`Note C2 · ch 10`, in `*.learn.set` results and the learn dialog) names a
key on a device, which has no song position, so it uses the default table whatever the context;
`midi.notes`, `midi.edit` and `midi.get {format: events}` give MIDI numbers and name nothing; the
roll's keyboard C labels are octave marks.

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

Chord-tone spelling: the tones of every quality by hand (the examples above and one line per double);
for each of the 21 roots and every quality name, each tone's letter, class and accidental agree with its
degree, no two tones share a class, and the degree's alteration against the major scale gives the
interval; a tone's name is the same in every key and with no key; a slash bass as written; each note
of the step table; a scale note keeping its spelling; the three triples falling back and the unknown
quality holding no chord; `B#` at pitch 0 replaced, with a chord and with a key alone; every name
`spell` gives for every root, quality, five keys and pitch 0..127 has a letter, at most two
accidentals and an octave in -1..9 and equals the pitch, and the notation compiles each
back to the same pitch (`test_notation.cpp`); `printPitchClass` for -2..+2 and `parsePitchClass`
refusing doubles.

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
     reference `pitch + iv(from.root, to.root)`. A tie goes down. By `Chords`.
     *(Amended 2026-11-21, ADR-0098 note E1: the reference was the Key-mode
     result, which could split one figure across octaves.)*
     - The reference is the block shift: the written pitch moved by the
       roots' nearest interval. Each note takes its target class nearest its
       own reference.
     - Chord tones give one voicing for one pair of chords whatever the keys:
       their targets depend on the chords only. A note kept by degree depends
       on the keys as well: A4 over Bm in D major sounds A4 under B7 in E
       minor and A#4 under B7 in B major.
     - Neighbouring notes can cross where their targets cross, so a figure
       need not keep its contour: A4 B4 over Cmaj7 in C major sounds Ab4 Gb4
       under Cdim in C minor (A kept by degree, B the major seventh going to
       the diminished fifth).
     - A chord tone lands at most 5 semitones from the block (a sus4's fourth
       on a chord with no third, a major seventh on a diminished fifth). A note
       kept by degree lands at most 2 from it in the diatonic and pentatonic
       scales, and 4 in blues.
     - Known limit: a slash bass that is not a tone lies a tritone from the
       block when the two basses sit a tritone apart above their roots (C/F#
       → G, C/D → G/Eb). It goes down, and so does every note of its class, a
       melody note included: `follow` maps each note on its own and does not
       know which note is lowest.
     - So a Bm figure B4 A4 F#4 D4 written in D major sounds B4 A4 F#4 D#4
       under B7, in A minor as in E minor. A cell A4 C5 E5 G5 over Am7 sounds
       as written over Am7, in any key.
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
  - the octave by the roots (keys C major → F# major, chord C held: C4 stays
    C4; chord C → F#: C4 → F#3, the tie down);
  - a Bm figure B4 A4 F#4 D4 in D major → B7 in A minor, in E minor and with
    no key: B4 A4 F#4 D#4;
  - A4 C5 E5 G5 over Am7, written in any of 24 keys, → Am7 in E minor and in A
    minor: unchanged;
  - a chord tone's result is the same for any keys on either side;
  - a missing chord falls back to Key mode.
