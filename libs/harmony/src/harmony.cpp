// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/harmony/harmony.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace supple::harmony {

namespace {

constexpr std::array<int, 7> kLetterSemitone = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G

int letterIndex(char letter) { return letter - 'A'; }  // A=0 .. G=6

int floorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

}  // namespace

// --- pitch classes ------------------------------------------------------------------------------

std::optional<PitchClass> parsePitchClass(std::string_view text) {
    if (text.empty() || text.size() > 2) return std::nullopt;
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G') return std::nullopt;
    PitchClass pc{letter, 0};
    if (text.size() == 2) {
        if (text[1] == '#')
            pc.accidental = 1;
        else if (text[1] == 'b')
            pc.accidental = -1;
        else
            return std::nullopt;
    }
    return pc;
}

std::string printPitchClass(PitchClass pc) {
    std::string out(1, pc.letter);
    if (pc.accidental > 0) out.append(static_cast<std::size_t>(pc.accidental), '#');
    if (pc.accidental < 0) out.append(static_cast<std::size_t>(-pc.accidental), 'b');
    return out;
}

int pitchClassSemitone(PitchClass pc) {
    return kLetterSemitone[static_cast<std::size_t>(letterIndex(pc.letter))] + pc.accidental;
}

int pitchClassNumber(PitchClass pc) { return floorMod(pitchClassSemitone(pc), 12); }

// --- scales -------------------------------------------------------------------------------------

namespace {

struct ScaleInfo {
    Scale scale;
    std::string_view name;
    std::vector<int> intervals;
    // The letter offset (0..6, from the tonic's letter) each degree takes; empty for
    // the chromatic scale, which has no letters.
    std::vector<int> letters;
};

const std::vector<ScaleInfo>& scaleTable() {
    static const std::vector<ScaleInfo> table = {
        {Scale::Major, "major", {0, 2, 4, 5, 7, 9, 11}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::Minor, "minor", {0, 2, 3, 5, 7, 8, 10}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::HarmonicMinor, "harmonic_minor", {0, 2, 3, 5, 7, 8, 11}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::MelodicMinor, "melodic_minor", {0, 2, 3, 5, 7, 9, 11}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::Dorian, "dorian", {0, 2, 3, 5, 7, 9, 10}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::Phrygian, "phrygian", {0, 1, 3, 5, 7, 8, 10}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::Lydian, "lydian", {0, 2, 4, 6, 7, 9, 11}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::Mixolydian, "mixolydian", {0, 2, 4, 5, 7, 9, 10}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::Locrian, "locrian", {0, 1, 3, 5, 6, 8, 10}, {0, 1, 2, 3, 4, 5, 6}},
        {Scale::MajorPentatonic, "major_pentatonic", {0, 2, 4, 7, 9}, {0, 1, 2, 4, 5}},
        {Scale::MinorPentatonic, "minor_pentatonic", {0, 3, 5, 7, 10}, {0, 2, 3, 4, 6}},
        {Scale::Blues, "blues", {0, 3, 5, 6, 7, 10}, {0, 2, 3, 4, 4, 6}},
        {Scale::Chromatic, "chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, {}},
    };
    return table;
}

const ScaleInfo& info(Scale scale) {
    for (const auto& s : scaleTable())
        if (s.scale == scale) return s;
    return scaleTable().front();
}

// The accidental a degree needs: the tonic's pitch plus the interval, against the
// letter's natural pitch, reduced to -6..5.
int degreeAccidental(const Key& key, int interval, int letterOffset) {
    const int letter = (letterIndex(key.tonic.letter) + letterOffset) % 7;
    const int target = pitchClassSemitone(key.tonic) + interval;
    return floorMod(target - kLetterSemitone[static_cast<std::size_t>(letter)] + 6, 12) - 6;
}

char degreeLetter(const Key& key, int letterOffset) {
    return static_cast<char>('A' + (letterIndex(key.tonic.letter) + letterOffset) % 7);
}

}  // namespace

std::span<const Scale> allScales() {
    static const std::vector<Scale> all = [] {
        std::vector<Scale> v;
        for (const auto& s : scaleTable()) v.push_back(s.scale);
        return v;
    }();
    return all;
}

std::string_view scaleName(Scale scale) { return info(scale).name; }

std::optional<Scale> parseScale(std::string_view name) {
    for (const auto& s : scaleTable())
        if (s.name == name) return s.scale;
    return std::nullopt;
}

std::span<const int> scaleIntervals(Scale scale) { return info(scale).intervals; }

bool keySupported(const Key& key) {
    const auto& s = info(key.scale);
    for (std::size_t k = 0; k < s.letters.size(); ++k) {
        const int a = degreeAccidental(key, s.intervals[k], s.letters[k]);
        if (a < -1 || a > 1) return false;
    }
    return true;
}

// --- chords -------------------------------------------------------------------------------------

namespace {

// The one table of qualities: the map `chordQualities()` builds from it, and `follow` reads it
// directly, so a chord's tones need no allocation (not even the map's first-use build).
struct QualityRow {
    std::string_view name;
    int count;
    std::array<int, 6> intervals;
    // The degree of each tone (specs/harmony.md §3.1): its number gives the letter, its
    // alterations against the major scale give the interval (a test checks both).
    std::array<std::string_view, 6> degrees;
};
using D = std::array<std::string_view, 6>;
constexpr D kTriadMajor = {"1", "3", "5"};
constexpr D kTriadMinor = {"1", "b3", "5"};
constexpr D kSus4 = {"1", "4", "5"};
constexpr D kAug = {"1", "3", "#5"};
constexpr D kSix = {"1", "3", "5", "6"};
constexpr D kDom7 = {"1", "3", "5", "b7"};
constexpr D kMaj7 = {"1", "3", "5", "7"};
constexpr D kMin7 = {"1", "b3", "5", "b7"};
constexpr D kHalfDim = {"1", "b3", "b5", "b7"};
constexpr std::array<QualityRow, 31> kQualityRows = {{
    {"", 3, {0, 4, 7, 0, 0, 0}, kTriadMajor},
    {"maj", 3, {0, 4, 7, 0, 0, 0}, kTriadMajor},
    {"m", 3, {0, 3, 7, 0, 0, 0}, kTriadMinor},
    {"min", 3, {0, 3, 7, 0, 0, 0}, kTriadMinor},
    {"dim", 3, {0, 3, 6, 0, 0, 0}, {"1", "b3", "b5"}},
    {"aug", 3, {0, 4, 8, 0, 0, 0}, kAug},
    {"+", 3, {0, 4, 8, 0, 0, 0}, kAug},
    {"sus2", 3, {0, 2, 7, 0, 0, 0}, {"1", "2", "5"}},
    {"sus4", 3, {0, 5, 7, 0, 0, 0}, kSus4},
    {"sus", 3, {0, 5, 7, 0, 0, 0}, kSus4},
    {"6", 4, {0, 4, 7, 9, 0, 0}, kSix},
    {"m6", 4, {0, 3, 7, 9, 0, 0}, {"1", "b3", "5", "6"}},
    {"7", 4, {0, 4, 7, 10, 0, 0}, kDom7},
    {"maj7", 4, {0, 4, 7, 11, 0, 0}, kMaj7},
    {"M7", 4, {0, 4, 7, 11, 0, 0}, kMaj7},
    {"m7", 4, {0, 3, 7, 10, 0, 0}, kMin7},
    {"min7", 4, {0, 3, 7, 10, 0, 0}, kMin7},
    {"mMaj7", 4, {0, 3, 7, 11, 0, 0}, {"1", "b3", "5", "7"}},
    {"dim7", 4, {0, 3, 6, 9, 0, 0}, {"1", "b3", "b5", "bb7"}},
    {"m7b5", 4, {0, 3, 6, 10, 0, 0}, kHalfDim},
    {"ø", 4, {0, 3, 6, 10, 0, 0}, kHalfDim},
    {"7sus4", 4, {0, 5, 7, 10, 0, 0}, {"1", "4", "5", "b7"}},
    {"add9", 4, {0, 4, 7, 14, 0, 0}, {"1", "3", "5", "9"}},
    {"9", 5, {0, 4, 7, 10, 14, 0}, {"1", "3", "5", "b7", "9"}},
    {"maj9", 5, {0, 4, 7, 11, 14, 0}, {"1", "3", "5", "7", "9"}},
    {"m9", 5, {0, 3, 7, 10, 14, 0}, {"1", "b3", "5", "b7", "9"}},
    {"7b9", 5, {0, 4, 7, 10, 13, 0}, {"1", "3", "5", "b7", "b9"}},
    {"7#9", 5, {0, 4, 7, 10, 15, 0}, {"1", "3", "5", "b7", "#9"}},
    {"11", 5, {0, 7, 10, 14, 17, 0}, {"1", "5", "b7", "9", "11"}},
    {"13", 5, {0, 4, 10, 14, 21, 0}, {"1", "3", "b7", "9", "13"}},
    {"5", 2, {0, 7, 0, 0, 0, 0}, {"1", "5"}},
}};

}  // namespace

const std::map<std::string, std::vector<int>>& chordQualities() {
    static const std::map<std::string, std::vector<int>> qualities = [] {
        std::map<std::string, std::vector<int>> table;
        for (const QualityRow& row : kQualityRows)
            table.emplace(
                std::string(row.name),
                std::vector<int>(row.intervals.begin(), row.intervals.begin() + row.count));
        return table;
    }();
    return qualities;
}

std::string_view canonicalQuality(std::string_view quality) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> aliases = {{
        {"maj", ""},
        {"min", "m"},
        {"+", "aug"},
        {"sus", "sus4"},
        {"M7", "maj7"},
        {"min7", "m7"},
        {"ø", "m7b5"},
    }};
    for (const auto& [alias, canonical] : aliases)
        if (quality == alias) return canonical;
    return quality;
}

Result<Chord> parseChord(std::string_view text) {
    const auto slash = text.find('/');
    const auto symbol = text.substr(0, slash);
    std::size_t i = 1;
    if (i < symbol.size() && (symbol[i] == '#' || symbol[i] == 'b')) ++i;
    const auto root = parsePitchClass(symbol.substr(0, i));
    if (symbol.empty() || !root)
        return Error("chord.bad_root", "Invalid chord root.", {}, "Use a root such as C or Bb.");
    const std::string quality(symbol.substr(i));
    const auto& table = chordQualities();
    if (table.find(quality) == table.end()) {
        std::string names;
        for (const auto& [name, intervals] : table) {
            (void)intervals;
            if (!names.empty()) names += ' ';
            names += name.empty() ? "(major)" : name;
        }
        return Error("chord.unknown_quality", "Unknown chord quality '" + quality + "'.", {},
                     "Qualities: " + names);
    }
    Chord chord{*root, std::string(canonicalQuality(quality)), std::nullopt};
    if (slash != std::string_view::npos) {
        const auto bass = parsePitchClass(text.substr(slash + 1));
        if (!bass)
            return Error("chord.bad_bass", "Invalid slash bass.", {},
                         "Use a bass letter such as C/E.");
        chord.bass = *bass;
    }
    return chord;
}

std::string printChord(const Chord& chord) {
    std::string out = printPitchClass(chord.root);
    out += canonicalQuality(chord.quality);
    if (chord.bass) out += "/" + printPitchClass(*chord.bass);
    return out;
}

namespace {
// The MIDI pitch of a pitch class in a scientific octave, as the notation reads
// `C#4`: octave -1..9, the result 0..127.
std::optional<int> midiPitch(PitchClass pc, int octave) {
    if (octave < -1 || octave > 9) return std::nullopt;
    const int p = (octave + 1) * 12 + pitchClassSemitone(pc);
    if (p < 0 || p > 127) return std::nullopt;
    return p;
}
}  // namespace

Result<std::vector<int>> chordPitches(const Chord& chord, int octave) {
    const auto root = midiPitch(chord.root, octave);
    if (!root)
        return Error("chord.bad_root", "Invalid chord root.", {}, "Use a root such as C or Bb.");
    const auto& table = chordQualities();
    const auto it = table.find(std::string(canonicalQuality(chord.quality)));
    if (it == table.end())
        return Error("chord.unknown_quality", "Unknown chord quality '" + chord.quality + "'.");
    std::vector<int> pitches;
    if (chord.bass) {
        const auto bass = midiPitch(*chord.bass, octave - 1);
        if (!bass)
            return Error("chord.bad_bass", "Invalid slash bass.", {},
                         "Use a bass letter such as C/E.");
        pitches.push_back(*bass);
    }
    for (int interval : it->second) {
        if (*root + interval > 127)
            return Error("chord.pitch_range", "Chord pitch exceeds MIDI range.", {},
                         "Use a lower chord octave.");
        pitches.push_back(*root + interval);
    }
    return pitches;
}

// --- spelling -----------------------------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, 12> kDefaultNames = {"C",  "C#", "D",  "Eb", "E",  "F",
                                                            "F#", "G",  "Ab", "A",  "Bb", "B"};
constexpr std::array<std::string_view, 12> kSharpNames = {"C",  "C#", "D",  "D#", "E",  "F",
                                                          "F#", "G",  "G#", "A",  "A#", "B"};
constexpr std::array<std::string_view, 12> kFlatNames = {"C",  "Db", "D",  "Eb", "E",  "F",
                                                         "Gb", "G",  "Ab", "A",  "Bb", "B"};

struct Spelled {
    char letter;
    int accidental;
};

// Whether the key's scale has more flats (-1), more sharps (+1) or neither (0), by the
// accidentals of its spelled degrees. A chromatic key counts the major scale on its
// tonic; a degree needing a double accidental counts by its sign.
int direction(const Key& key) {
    const auto& s = info(key.scale == Scale::Chromatic ? Scale::Major : key.scale);
    int flats = 0, sharps = 0;
    for (std::size_t k = 0; k < s.letters.size(); ++k) {
        const int a = degreeAccidental(key, s.intervals[k], s.letters[k]);
        if (a < 0) ++flats;
        if (a > 0) ++sharps;
    }
    return flats > sharps ? -1 : (sharps > flats ? 1 : 0);
}

// The scale's own spelling of a pitch class, when it is a scale degree and the key is
// supported.
std::optional<Spelled> scaleSpelling(int pitchClass, const Key& key) {
    const auto& s = info(key.scale);
    if (s.letters.empty() || !keySupported(key)) return std::nullopt;
    for (std::size_t k = 0; k < s.letters.size(); ++k) {
        const int pc = floorMod(pitchClassSemitone(key.tonic) + s.intervals[k], 12);
        if (pc == pitchClass)
            return Spelled{degreeLetter(key, s.letters[k]),
                           degreeAccidental(key, s.intervals[k], s.letters[k])};
    }
    return std::nullopt;
}

// The key's direction table, or the default table with no key: always one accidental at most
// and a letter whose own octave is the pitch's.
Spelled tableSpelling(int pitchClass, const std::optional<Key>& key) {
    const int d = key ? direction(*key) : 0;
    const auto& names = d < 0 ? kFlatNames : (d > 0 ? kSharpNames : kDefaultNames);
    const std::string_view text = names[static_cast<std::size_t>(pitchClass)];
    return Spelled{text[0], text.size() > 1 ? (text[1] == '#' ? 1 : -1) : 0};
}

// The key's rule alone: a scale degree by the scale, else the table.
Spelled keySpelling(int pitchClass, const std::optional<Key>& key) {
    if (key)
        if (const auto s = scaleSpelling(pitchClass, *key)) return *s;
    return tableSpelling(pitchClass, key);
}

// The letter a degree takes, as an offset from the root's letter: its number minus one,
// modulo seven (a ninth is a second's letter). The sharp nine takes the third's letter.
int letterOffsetOf(std::string_view degree) {
    if (degree == "#9") return 2;
    int number = 0;
    for (const char c : degree)
        if (c >= '0' && c <= '9') number = number * 10 + (c - '0');
    return (number - 1) % 7;
}

// A letter `offset` letters above the root's, with the accidental that gives `semitone`
// (reduced), against the letter's own pitch.
Spelled byLetter(PitchClass root, int offset, int semitone) {
    const char letter = static_cast<char>('A' + (letterIndex(root.letter) + offset) % 7);
    const int natural = kLetterSemitone[static_cast<std::size_t>(letterIndex(letter))];
    return Spelled{letter, floorMod(semitone - natural + 6, 12) - 6};
}

bool plausible(Spelled s) { return s.accidental >= -2 && s.accidental <= 2; }

// A chord's tones as spelled things: the quality's row, or none for a quality this build does
// not know.
const QualityRow* rowOf(const Chord& chord) {
    for (const QualityRow& row : kQualityRows)
        if (row.name == chord.quality) return &row;
    return nullptr;
}

// Step four: a note that is neither a chord tone nor in the key's scale is named by its step
// above the chord's root (specs/harmony.md §3).
std::optional<Spelled> stepSpelling(int pitchClass, const Chord& chord, const QualityRow& row) {
    const int i = floorMod(pitchClass - pitchClassSemitone(chord.root), 12);
    int offset = 0;
    switch (i) {
        case 0:
            offset = 0;
            break;
        case 1:
            offset = 1;
            break;  // b2
        case 2:
            offset = 1;
            break;
        case 3:
            offset = 2;
            break;  // b3
        case 4:
            offset = 2;
            break;
        case 5:
            offset = 3;
            break;
        case 6: {  // #4 over a major or absent third, b5 over a minor one
            bool minorThird = false;
            for (int k = 0; k < row.count; ++k)
                if (row.intervals[static_cast<std::size_t>(k)] == 3) minorThird = true;
            offset = minorThird ? 4 : 3;
            break;
        }
        case 7:
            offset = 4;
            break;
        case 8:
            offset = 5;
            break;  // b6
        case 9:
            offset = 5;
            break;
        case 10:
            offset = 6;
            break;  // b7
        default:
            offset = 6;
            break;  // 11: the major seventh
    }
    const Spelled s = byLetter(chord.root, offset, pitchClassSemitone(chord.root) + i);
    if (!plausible(s)) return std::nullopt;
    return s;
}

// The spelling at a place, before the octave is looked at.
Spelled spelledAt(int pitchClass, const Place& place) {
    const Chord* chord = place.chord ? &*place.chord : nullptr;
    const QualityRow* row = chord ? rowOf(*chord) : nullptr;
    if (row) {
        // 1. A slash bass as written.
        if (chord->bass && pitchClassNumber(*chord->bass) == pitchClass)
            return Spelled{chord->bass->letter, chord->bass->accidental};
        // 2. A chord tone, by its degree. A tone that would need three accidentals goes on
        // to the key's rule (steps 3 and 5), not to step 4.
        bool tone = false;
        for (int k = 0; k < row->count; ++k) {
            const int interval = row->intervals[static_cast<std::size_t>(k)];
            if (floorMod(pitchClassSemitone(chord->root) + interval, 12) != pitchClass) continue;
            tone = true;
            const Spelled s =
                byLetter(chord->root, letterOffsetOf(row->degrees[static_cast<std::size_t>(k)]),
                         pitchClassSemitone(chord->root) + interval);
            if (plausible(s)) return s;
            break;
        }
        // 3. A note of the key's scale, by the key.
        if (place.key)
            if (const auto s = scaleSpelling(pitchClass, *place.key)) return *s;
        // 4. A note a step above the chord's root.
        if (!tone)
            if (const auto s = stepSpelling(pitchClass, *chord, *row)) return *s;
        // 5. The key's direction table, or the default table.
        return tableSpelling(pitchClass, place.key);
    }
    return keySpelling(pitchClass, place.key);
}

std::string name(Spelled s) { return printPitchClass({s.letter, s.accidental}); }

// The scientific octave of a pitch spelled `s`: the octave follows the letter.
int octaveOf(int pitch, Spelled s) {
    const int semitone =
        kLetterSemitone[static_cast<std::size_t>(letterIndex(s.letter))] + s.accidental;
    return floorDiv(pitch - semitone, 12) - 1;
}

}  // namespace

std::vector<ChordTone> chordTones(const Chord& chord) {
    std::vector<ChordTone> out;
    const QualityRow* row = rowOf(chord);
    if (!row) return out;
    const int root = pitchClassSemitone(chord.root);
    for (int k = 0; k < row->count; ++k) {
        const int interval = row->intervals[static_cast<std::size_t>(k)];
        const std::string_view degree = row->degrees[static_cast<std::size_t>(k)];
        const Spelled s = byLetter(chord.root, letterOffsetOf(degree), root + interval);
        ChordTone tone;
        tone.interval = interval;
        tone.degree = degree;
        if (plausible(s)) {
            tone.name = {s.letter, s.accidental};
        } else {
            const Spelled plain = tableSpelling(floorMod(root + interval, 12), std::nullopt);
            tone.name = {plain.letter, plain.accidental};
            tone.byDegree = false;
        }
        out.push_back(tone);
    }
    if (chord.bass) {
        ChordTone bass;
        bass.interval = floorMod(pitchClassSemitone(*chord.bass) - root, 12);
        bass.degree = "bass";
        bass.name = *chord.bass;
        out.push_back(bass);
    }
    return out;
}

std::string spellPitchClass(int pitchClass, const Place& place) {
    return name(spelledAt(floorMod(pitchClass, 12), place));
}

std::string spellPitchClass(int pitchClass, const std::optional<Key>& key) {
    return spellPitchClass(pitchClass, Place{key, std::nullopt});
}

std::string spell(int pitch, const Place& place) {
    const int pc = floorMod(pitch, 12);
    Spelled s = spelledAt(pc, place);
    int octave = octaveOf(pitch, s);
    // A name whose octave the notation would refuse (B# at pitch 0 is B#-2) is replaced by the
    // key's or the default table's.
    if (pitch >= 0 && pitch <= 127 && (octave < -1 || octave > 9)) {
        s = tableSpelling(pc, place.key);
        octave = octaveOf(pitch, s);
    }
    return name(s) + std::to_string(octave);
}

std::string spell(int pitch, const std::optional<Key>& key) {
    return spell(pitch, Place{key, std::nullopt});
}

std::optional<int> transposeDegrees(int pitch, const Key& key, int degrees) {
    const auto& intervals = info(key.scale).intervals;
    const int n = static_cast<int>(intervals.size());
    const int tonic = pitchClassNumber(key.tonic);
    const int rel = pitch - tonic;
    const int octave = floorDiv(rel, 12);
    const int r = rel - 12 * octave;
    int k = 0;  // the nearest scale degree at or below
    for (int i = 0; i < n; ++i)
        if (intervals[static_cast<std::size_t>(i)] <= r) k = i;
    const int position = octave * n + k + degrees;
    const int newOctave = floorDiv(position, n);
    const int newDegree = position - newOctave * n;
    const int result = tonic + 12 * newOctave + intervals[static_cast<std::size_t>(newDegree)];
    if (result < 0 || result > 127) return std::nullopt;
    return result;
}

std::vector<int> keyPitchClasses(const Key& key) {
    std::vector<int> out;
    const int tonic = pitchClassNumber(key.tonic);
    for (const int interval : info(key.scale).intervals) out.push_back((tonic + interval) % 12);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<int> chordPitchClasses(const Chord& chord) {
    std::vector<int> out;
    const int root = pitchClassNumber(chord.root);
    const auto& table = chordQualities();
    auto found = table.find(chord.quality);
    if (found == table.end()) found = table.find(std::string(canonicalQuality(chord.quality)));
    if (found != table.end())
        for (const int interval : found->second) out.push_back(((root + interval) % 12 + 12) % 12);
    if (chord.bass) out.push_back(pitchClassNumber(*chord.bass));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<int> snapToPitchClasses(int pitch, std::span<const int> classes, SnapMode mode) {
    if (classes.empty() || pitch < 0 || pitch > 127) return std::nullopt;
    auto in = [&](int p) {
        if (p < 0 || p > 127) return false;
        return std::find(classes.begin(), classes.end(), p % 12) != classes.end();
    };
    if (in(pitch)) return pitch;
    for (int d = 1; d <= 12; ++d) {
        const bool down = in(pitch - d), up = in(pitch + d);
        if (mode != SnapMode::Up && down) return pitch - d;  // nearest ties go down
        if (mode != SnapMode::Down && up) return pitch + d;
    }
    return std::nullopt;
}

// --- context ------------------------------------------------------------------------------------

namespace {
// Sorted by beat; of several at one beat the last one given.
template <class Point>
void normalize(std::vector<Point>& points) {
    std::stable_sort(points.begin(), points.end(),
                     [](const Point& a, const Point& b) { return a.beat < b.beat; });
    std::vector<Point> out;
    for (auto& p : points) {
        if (!out.empty() && out.back().beat == p.beat)
            out.back() = std::move(p);
        else
            out.push_back(std::move(p));
    }
    points = std::move(out);
}

template <class Point>
const Point* holding(const std::vector<Point>& points, double beat) {
    auto it = std::upper_bound(points.begin(), points.end(), beat,
                               [](double b, const Point& p) { return b < p.beat; });
    return it == points.begin() ? nullptr : &*(it - 1);
}
}  // namespace

ContextIndex::ContextIndex(std::vector<KeyPoint> keys, std::vector<ChordPoint> chords)
    : keys_(std::move(keys)), chords_(std::move(chords)) {
    normalize(keys_);
    normalize(chords_);
}

std::optional<Key> ContextIndex::keyAt(double beat) const {
    const auto* p = holding(keys_, beat);
    return p ? std::optional<Key>(p->key) : std::nullopt;
}

std::optional<Chord> ContextIndex::chordAt(double beat) const {
    const auto* p = holding(chords_, beat);
    return p ? p->chord : std::nullopt;
}

// --- following ----------------------------------------------------------------------------------

namespace {

int pcOf(PitchClass pc) { return pitchClassNumber(pc); }

int ivBetween(int fromClass, int toClass) {
    const int d = floorMod(toClass - fromClass, 12);
    return d < 6 ? d : (d > 6 ? d - 12 : -6);
}

// The result of Key mode before folding, or nothing when a key is missing (a copy).
struct KeyStep {
    int pitch = 0;
    FollowedBy by = FollowedBy::Transpose;
    bool scaleShape = false;
};
std::optional<KeyStep> keyStep(int pitch, const std::optional<Key>& from,
                               const std::optional<Key>& to) {
    if (!from || !to) return std::nullopt;
    const int iv = ivBetween(pcOf(from->tonic), pcOf(to->tonic));
    if (from->scale == to->scale) return KeyStep{pitch + iv, FollowedBy::Transpose, false};
    const auto fromIv = scaleIntervals(from->scale);
    const auto toIv = scaleIntervals(to->scale);
    if (fromIv.size() != toIv.size()) return KeyStep{pitch + iv, FollowedBy::Transpose, true};
    // The highest scale pitch at or below `pitch` (it may lie below 0), and the
    // alteration above it.
    const int tonic = pcOf(from->tonic);
    int s = pitch;
    for (int d = 0; d < 12; ++d, --s) {
        const int rel = floorMod(s - tonic, 12);
        if (std::find(fromIv.begin(), fromIv.end(), rel) != fromIv.end()) break;
    }
    const int alt = pitch - s;
    const int r = s - tonic;
    const int o = floorDiv(r, 12);
    const int rem = r - 12 * o;
    const auto k =
        static_cast<std::size_t>(std::find(fromIv.begin(), fromIv.end(), rem) - fromIv.begin());
    return KeyStep{tonic + iv + 12 * o + toIv[k] + alt, FollowedBy::Degree, false};
}

int slotOf(int interval) {
    if (interval == 0) return 0;
    if (interval >= 2 && interval <= 5) return 1;
    if (interval >= 6 && interval <= 8) return 2;
    if (interval >= 9 && interval <= 11) return 3;
    if (interval >= 13 && interval <= 15) return 4;
    if (interval == 17) return 5;
    if (interval == 21) return 6;
    return -1;
}

struct Tone {
    int slot = 0;
    int pitchClass = 0;
};
// A chord's tones, in the table's order (no quality has more than six).
struct Tones {
    std::array<Tone, 6> tone{};
    int count = 0;
    const Tone* begin() const { return tone.data(); }
    const Tone* end() const { return tone.data() + count; }
    bool empty() const { return count == 0; }
};
Tones tonesOf(const Chord& chord) {
    Tones out;
    for (const QualityRow& row : kQualityRows) {
        if (row.name != chord.quality) continue;
        for (int i = 0; i < row.count; ++i) {
            const int interval = row.intervals[static_cast<std::size_t>(i)];
            const int slot = slotOf(interval);
            if (slot < 0) continue;
            out.tone[static_cast<std::size_t>(out.count++)] = {
                slot, floorMod(pcOf(chord.root) + interval, 12)};
        }
        break;
    }
    return out;
}
const Tone* toneInSlot(const Tones& tones, int slot) {
    for (const Tone& t : tones)
        if (t.slot == slot) return &t;
    return nullptr;
}
// The highest tone below `slot`, the root at least.
const Tone* toneBelow(const Tones& tones, int slot) {
    const Tone* best = nullptr;
    for (const Tone& t : tones)
        if (t.slot < slot && (!best || t.slot > best->slot)) best = &t;
    return best ? best : toneInSlot(tones, 0);
}

}  // namespace

int tonicInterval(PitchClass a, PitchClass b) { return ivBetween(pcOf(a), pcOf(b)); }

Followed follow(int pitch, const Place& from, const Place& to, FollowMode mode) {
    Followed out;
    const auto fold = [&](int p) {
        while (p > 127) {
            p -= 12;
            out.folded = true;
        }
        while (p < 0) {
            p += 12;
            out.folded = true;
        }
        return p;
    };
    const auto byKey = [&]() {
        const auto step = keyStep(pitch, from.key, to.key);
        if (!step) {
            out.pitch = pitch;
            out.by = FollowedBy::Copy;
            return;
        }
        out.by = step->by;
        out.scaleShape = step->scaleShape;
        out.pitch = fold(step->pitch);
    };
    if (mode == FollowMode::Key || !from.chord || !to.chord) {
        byKey();
        return out;
    }
    const Tones fromTones = tonesOf(*from.chord), toTones = tonesOf(*to.chord);
    if (fromTones.empty() || toTones.empty()) {  // a quality the table does not hold
        byKey();
        return out;
    }
    const int pitchClass = floorMod(pitch, 12);
    const int fromRoot = pcOf(from.chord->root), toRoot = pcOf(to.chord->root);
    int target = 0;
    const Tone* tone = nullptr;
    for (const Tone& t : fromTones)
        if (t.pitchClass == pitchClass) {
            tone = &t;
            break;
        }
    if (tone) {
        const Tone* landing = toneInSlot(toTones, tone->slot);
        if (!landing) {
            if (tone->slot >= 4) {  // an extension: the root, third or fifth, else below that
                const int base = tone->slot - 4;
                landing = toneInSlot(toTones, base);
                if (!landing) landing = toneBelow(toTones, base);
            } else {
                landing = toneBelow(toTones, tone->slot);
            }
        }
        target = landing ? landing->pitchClass : toRoot;
    } else if (from.chord->bass && pcOf(*from.chord->bass) == pitchClass) {
        target = to.chord->bass ? pcOf(*to.chord->bass) : toRoot;
    } else if (from.key && to.key &&
               scaleIntervals(from.key->scale).size() == scaleIntervals(to.key->scale).size()) {
        const auto fromIv = scaleIntervals(from.key->scale);
        const auto toIv = scaleIntervals(to.key->scale);
        const auto indexOf = [](std::span<const int> iv, int rel) {
            const auto it = std::find(iv.begin(), iv.end(), rel);
            return it == iv.end() ? -1 : static_cast<int>(it - iv.begin());
        };
        const int kr = indexOf(fromIv, floorMod(fromRoot - pcOf(from.key->tonic), 12));
        const int kt = indexOf(toIv, floorMod(toRoot - pcOf(to.key->tonic), 12));
        if (kr >= 0 && kt >= 0) {
            const int tonic = pcOf(from.key->tonic);
            int s = pitch;
            for (int d = 0; d < 12; ++d, --s)
                if (indexOf(fromIv, floorMod(s - tonic, 12)) >= 0) break;
            const int alt = pitch - s;
            const int j = indexOf(fromIv, floorMod(s - tonic, 12));
            const int m = static_cast<int>(fromIv.size());
            target = floorMod(pcOf(to.key->tonic) +
                                  toIv[static_cast<std::size_t>(floorMod(kt + j - kr, m))] + alt,
                              12);
        } else {
            target = floorMod(pitchClass + ivBetween(fromRoot, toRoot), 12);
        }
    } else {
        target = floorMod(pitchClass + ivBetween(fromRoot, toRoot), 12);
    }
    // The octave: the figure moves as a block by the roots' nearest interval,
    // and the note takes its target class nearest to that, a tie down. So one
    // pair of chords gives one voicing whatever the keys (ADR-0098, E1).
    const int reference = pitch + ivBetween(fromRoot, toRoot);
    const int d = floorMod(target - reference, 12);
    out.pitch = fold(reference + (d < 6 ? d : d - 12));  // 6 is a tie: down
    out.by = FollowedBy::Chords;
    return out;
}

}  // namespace supple::harmony
