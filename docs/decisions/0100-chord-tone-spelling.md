# ADR-0100: Chord tones are spelled by their degree

**Status:** Accepted, 2026-11-22. Amends ADR-0095.

Contracts: `specs/harmony.md` §1, §2, §3 (rule 4) and §3.1; `specs/notation.md` ("Decompile");
`specs/session-model.md` (`midi.get`). The roll's note names (`specs/gui.md`) follow in a later
step, which updates that spec.

## Context

ADR-0095 spells a pitch from the key alone: the third of E major, shown in the context of an E
chord in C major, reads `Ab`, and the seventh of a diminished seventh chord on C reads `A`. A chord
tone has a name that depends on its degree, not on the key: the third of E major is `G#`, the seventh
of C7 is `Bb`, a diminished seventh on B has `Ab` and on C it has `Bbb`. The library had no chord
among the inputs to spelling, and `PitchClass` could not hold a double accidental.

## Decision

`spell` and `spellPitchClass` take a place, a key and a chord, as well as a key alone. At a place
with a chord, in order:

1. a slash bass is named as written for its class;
2. a chord tone is named by its degree: the root's letter moved by the degree, the accidental whatever
   gives the class. The degree is a column of the quality table (`kQualityRows`), so no quality exists
   without one. The **sharp nine takes the third's letter**: `G` over E7#9, `Eb` over C7#9, as printed
   music writes it, never `F##` or `D#`;
3. a note of the key's scale keeps the key's spelling;
4. any other note is named by its step above the chord's root (`D#` over E, `G#` over Am, `F#` over
   C7, `Gb` over Cm);
5. else the key's rule, or the default table, as before.

A place with no chord is exactly ADR-0095's rule.

`PitchClass::accidental` widens to -2..+2. Only a spelling produces a double: a parsed name (a
key's tonic, a chord's root or bass) keeps one accidental, and `parsePitchClass` still refuses
`F##`. Double flats and sharps are allowed where a degree needs them; with all 21 root spellings
and every quality that is 188 chord tones, of which 15 on the twelve usual roots (`Cdim7` has
`Bbb`, `Baug` has `F##`). Three tones would need three accidentals (`B#aug`'s fifth, `Cbdim7`'s
and `Fbdim7`'s seventh): they use the key's rule, and no chord is refused for its spelling. A name whose
octave the notation would refuse (`B#` at pitch 0 is `B#-2`; ADR-0095's rule produced it for a key
alone too) is replaced by the key's table or the default table.

`notation::DecompileOptions` gains `placeAt`, a function from an item tick to a place. When set,
`decompile` names each note by the place at the note's start, as the piano roll does, so `midi.get`
follows a key or chord change inside an item (it used one key per item before). A tied
continuation keeps its first piece's names.

## Consequences

- Nothing stored changes: notes are MIDI numbers, context points hold symbols, and every produced name
  compiles back to the same pitch (the notation reads `##` and `bb`; a test compiles every name for every
  root, quality, five keys and pitch 0..127).
- `midi.get` text over a chord changes (chord tones read by degree). An item with no chord and one
  key reads as before, byte for byte; an item that crosses a key change now names each note by the key
  holding at its start, and pitch 0 under a key that spelled it `B#-2` now reads `C-1`.
  `midi.get {format: events}` carries numbers and does not change.
- The roll's note labels follow in a later step. The keyboard's C labels, the MIDI event list, MIDI-control names, the
  rack probe, and (unless it is the same decompile call) the notation pane stay on the default table:
  a known limit in `STATUS.md`.
- A chord's tone is named the same in every key, so the same pitch may read `G#` under E and `Ab` under
  Ab: the name says what the note is in the chord.

Amends the spelling rule of [ADR-0095](0095-context-track.md).

*Note, 2026-11-24:* `rack.probe` now spells by place, at the project beat each note is heard at
(`specs/rack.md` §7.2, `specs/harmony.md` §3.2), so it no longer stays on the default table as the
Consequences say. MIDI-control labels stay on it on purpose, and so do the roll's keyboard C labels;
`midi.notes` and `midi.edit` name nothing.
