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
    if (pc.accidental > 0) out += '#';
    if (pc.accidental < 0) out += 'b';
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

const std::map<std::string, std::vector<int>>& chordQualities() {
    static const std::map<std::string, std::vector<int>> qualities = {{"", {0, 4, 7}},
                                                                      {"maj", {0, 4, 7}},
                                                                      {"m", {0, 3, 7}},
                                                                      {"min", {0, 3, 7}},
                                                                      {"dim", {0, 3, 6}},
                                                                      {"aug", {0, 4, 8}},
                                                                      {"+", {0, 4, 8}},
                                                                      {"sus2", {0, 2, 7}},
                                                                      {"sus4", {0, 5, 7}},
                                                                      {"sus", {0, 5, 7}},
                                                                      {"6", {0, 4, 7, 9}},
                                                                      {"m6", {0, 3, 7, 9}},
                                                                      {"7", {0, 4, 7, 10}},
                                                                      {"maj7", {0, 4, 7, 11}},
                                                                      {"M7", {0, 4, 7, 11}},
                                                                      {"m7", {0, 3, 7, 10}},
                                                                      {"min7", {0, 3, 7, 10}},
                                                                      {"mMaj7", {0, 3, 7, 11}},
                                                                      {"dim7", {0, 3, 6, 9}},
                                                                      {"m7b5", {0, 3, 6, 10}},
                                                                      {"ø", {0, 3, 6, 10}},
                                                                      {"7sus4", {0, 5, 7, 10}},
                                                                      {"add9", {0, 4, 7, 14}},
                                                                      {"9", {0, 4, 7, 10, 14}},
                                                                      {"maj9", {0, 4, 7, 11, 14}},
                                                                      {"m9", {0, 3, 7, 10, 14}},
                                                                      {"7b9", {0, 4, 7, 10, 13}},
                                                                      {"7#9", {0, 4, 7, 10, 15}},
                                                                      {"11", {0, 7, 10, 14, 17}},
                                                                      {"13", {0, 4, 10, 14, 21}},
                                                                      {"5", {0, 7}}};
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

std::string name(Spelled s) { return printPitchClass({s.letter, s.accidental}); }

}  // namespace

std::string spellPitchClass(int pitchClass, const std::optional<Key>& key) {
    pitchClass = floorMod(pitchClass, 12);
    if (!key) return std::string(kDefaultNames[static_cast<std::size_t>(pitchClass)]);
    if (const auto s = scaleSpelling(pitchClass, *key)) return name(*s);
    const int d = direction(*key);
    const auto& names = d < 0 ? kFlatNames : (d > 0 ? kSharpNames : kDefaultNames);
    return std::string(names[static_cast<std::size_t>(pitchClass)]);
}

std::string spell(int pitch, const std::optional<Key>& key) {
    const int pc = floorMod(pitch, 12);
    const std::string text = spellPitchClass(pc, key);
    // The octave follows the letter, so B#3 is the pitch of C4.
    const auto parsed = parsePitchClass(text);
    const int octave =
        (pitch - pitchClassSemitone(*parsed) + 12 * 100) / 12 - 100 - 1;  // exact division
    return text + std::to_string(octave);
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

}  // namespace supple::harmony
