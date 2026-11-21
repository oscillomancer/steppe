// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Keys, scales, chords and pitch spelling, and the index that answers "what key
// and chord hold at beat t" over plain structs (docs/specs/harmony.md, ADR-0095).
// No model, engine or notation dependency. It allocates: never call it from the
// audio path.
#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <supple/base/result.h>

namespace supple::harmony {

// A spelled pitch class: `Eb` is not `D#`. `letter` is 'A'..'G', `accidental`
// -1, 0 or +1.
struct PitchClass {
    char letter = 'C';
    int accidental = 0;
    friend bool operator==(const PitchClass&, const PitchClass&) = default;
};

// A letter (either case) and at most one `#` or `b`, and nothing after.
std::optional<PitchClass> parsePitchClass(std::string_view text);
std::string printPitchClass(PitchClass pc);  // "Eb", "F#", "C"
// Semitones above C with the accidental applied, not reduced: Cb is -1, B# is 12.
int pitchClassSemitone(PitchClass pc);
int pitchClassNumber(PitchClass pc);  // 0..11

enum class Scale {
    Major,
    Minor,
    HarmonicMinor,
    MelodicMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    MajorPentatonic,
    MinorPentatonic,
    Blues,
    Chromatic,
};

std::span<const Scale> allScales();
std::string_view scaleName(Scale scale);  // "harmonic_minor"
std::optional<Scale> parseScale(std::string_view name);
std::span<const int> scaleIntervals(Scale scale);  // semitones above the tonic

struct Key {
    PitchClass tonic;
    Scale scale = Scale::Major;
    friend bool operator==(const Key&, const Key&) = default;
};

// False when spelling the scale needs a double accidental (G# major).
bool keySupported(const Key& key);

// --- chords -------------------------------------------------------------------------------------

struct Chord {
    PitchClass root;
    std::string quality;  // canonical (printChord's); "" is major
    std::optional<PitchClass> bass;
    friend bool operator==(const Chord&, const Chord&) = default;
};

// Every quality the grammar takes (aliases included), with its intervals above the
// root in semitones; ordered by name. The notation compiler and the commands share it.
const std::map<std::string, std::vector<int>>& chordQualities();
// The canonical name for a quality or one of its aliases; the input when unknown.
std::string_view canonicalQuality(std::string_view quality);

// `Cm7`, `Bb`, `F#7#9`, `C/E`. Errors: `chord.bad_root`, `chord.unknown_quality`
// (the message names the quality; `hint` lists them) and `chord.bad_bass`.
Result<Chord> parseChord(std::string_view text);
std::string printChord(const Chord& chord);  // canonical: parse(print(c)) == c

// The chord voiced as the notation does: the root in `octave`, each further tone
// the quality's interval above it, a slash bass in the octave below its own letter
// (`octave - 1`) placed first, then ascending. Errors: `chord.bad_root`,
// `chord.bad_bass` (not a MIDI pitch) and `chord.pitch_range`.
Result<std::vector<int>> chordPitches(const Chord& chord, int octave);

// --- spelling -----------------------------------------------------------------------------------

// The name of a pitch class (0..11) or a MIDI pitch (with its octave, C4 = 60), by
// the key's rule (specs/harmony.md §3), or the notation's default table with no key:
// `C C# D Eb E F F# G Ab A Bb B`.
std::string spellPitchClass(int pitchClass, const std::optional<Key>& key = std::nullopt);
std::string spell(int pitch, const std::optional<Key>& key = std::nullopt);

// A MIDI pitch moved by scale degrees in the key's scale. An in-scale pitch moves to
// the degree `degrees` away (octaves carry); an out-of-scale pitch first goes to the
// nearest scale pitch below. Nothing when the result is outside 0..127.
std::optional<int> transposeDegrees(int pitch, const Key& key, int degrees);

// The pitch classes (0..11, ascending) of a key's scale and of a chord's tones (the
// root, each of its quality's intervals, a slash bass).
std::vector<int> keyPitchClasses(const Key& key);
std::vector<int> chordPitchClasses(const Chord& chord);

// Which way a pitch outside the set goes.
enum class SnapMode { Nearest, Down, Up };

// A MIDI pitch moved to a pitch whose class is in `classes`: itself when it is
// already there; else the nearest such pitch (a tie goes down), the nearest below,
// or the nearest above. Nothing when `classes` is empty or no such pitch lies in
// 0..127 in that direction.
std::optional<int> snapToPitchClasses(int pitch, std::span<const int> classes,
                                      SnapMode mode = SnapMode::Nearest);

// --- following (ADR-0098, specs/harmony.md §5) --------------------------------------------------

// The key and chord that hold at a place; a part that does not hold, or that this
// build cannot read, is absent.
struct Place {
    std::optional<Key> key;
    std::optional<Chord> chord;
};
enum class FollowMode { Key, Chords };
enum class FollowedBy { Transpose, Degree, Chords, Copy };
struct Followed {
    int pitch = 0;  // always in 0..127
    FollowedBy by = FollowedBy::Copy;
    bool scaleShape = false;  // the scales had different sizes and the note was transposed
    bool folded = false;      // moved by octaves into range
};

// The nearest interval in semitones from tonic `a` to tonic `b`, -5..6 with the tritone
// going down: -6.
int tonicInterval(PitchClass a, PitchClass b);

// A MIDI pitch (0..127) written against `from`, mapped to `to` (docs/specs/harmony.md §5).
// It never fails: what cannot be mapped is copied (`Copy`), and a result outside 0..127
// is folded by octaves. Deterministic and allocation-free: it reads constant tables and
// holds nothing on the heap, so a caller needs no setup before it (X4 builds tables from it).
Followed follow(int pitch, const Place& from, const Place& to, FollowMode mode);

// --- context ------------------------------------------------------------------------------------

struct KeyPoint {
    double beat = 0;
    Key key;
};
struct ChordPoint {
    double beat = 0;
    std::optional<Chord> chord;  // empty: no chord from here
};

// Immutable once built. Two points of one type at one beat are a caller error:
// the later one in input order wins.
class ContextIndex {
  public:
    ContextIndex() = default;
    ContextIndex(std::vector<KeyPoint> keys, std::vector<ChordPoint> chords);

    // The last point at or before `beat`; none before the first (or for an empty
    // chord point).
    std::optional<Key> keyAt(double beat) const;
    std::optional<Chord> chordAt(double beat) const;

    std::span<const KeyPoint> keyPoints() const { return keys_; }  // by beat
    std::span<const ChordPoint> chordPoints() const { return chords_; }

  private:
    std::vector<KeyPoint> keys_;
    std::vector<ChordPoint> chords_;
};

}  // namespace supple::harmony
