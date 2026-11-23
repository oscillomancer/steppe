// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/harmony/harmony.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <random>
#include <set>
#include <sstream>

using namespace supple::harmony;

// Counts the global allocations, to show `follow` makes none. Not under a sanitizer, which
// brings its own allocator.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define HARMONY_NO_ALLOCATION_COUNT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(realtime_sanitizer)
#define HARMONY_NO_ALLOCATION_COUNT 1
#endif
#endif
#ifndef HARMONY_NO_ALLOCATION_COUNT
namespace {
std::atomic<long> gAllocations{0};
}
void* operator new(std::size_t size) {
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

Key key(std::string_view tonic, Scale scale) { return Key{*parsePitchClass(tonic), scale}; }

// The names of a key's scale degrees, space separated, from its tonic upward.
std::string degrees(const Key& k) {
    std::string out;
    const int tonic = pitchClassNumber(k.tonic);
    for (int interval : scaleIntervals(k.scale)) {
        if (!out.empty()) out += ' ';
        out += spellPitchClass(tonic + interval, k);
    }
    return out;
}

// Signed count of accidentals in the spelled scale: sharps minus flats.
int signature(const Key& k) {
    int n = 0;
    const int tonic = pitchClassNumber(k.tonic);
    for (int interval : scaleIntervals(k.scale)) {
        const auto pc = *parsePitchClass(spellPitchClass(tonic + interval, k));
        n += pc.accidental;
    }
    return n;
}

}  // namespace

TEST_CASE("pitch classes parse, print and number") {
    CHECK(printPitchClass(*parsePitchClass("Eb")) == "Eb");
    CHECK(printPitchClass(*parsePitchClass("f#")) == "F#");
    CHECK(printPitchClass(*parsePitchClass("bb")) == "Bb");  // B flat, not a second b
    CHECK(printPitchClass(*parsePitchClass("b")) == "B");
    CHECK(*parsePitchClass("Eb") != *parsePitchClass("D#"));
    CHECK(pitchClassNumber(*parsePitchClass("Eb")) == pitchClassNumber(*parsePitchClass("D#")));
    CHECK(pitchClassSemitone(*parsePitchClass("Cb")) == -1);
    CHECK(pitchClassNumber(*parsePitchClass("Cb")) == 11);
    CHECK(pitchClassSemitone(*parsePitchClass("B#")) == 12);
    CHECK(pitchClassNumber(*parsePitchClass("B#")) == 0);
    for (const char* bad : {"", "H", "C##", "Cbb", "C4", "E ", "#C", "Ab#"})
        CHECK_FALSE(parsePitchClass(bad));
}

TEST_CASE("scale names round trip and every scale starts on the tonic") {
    CHECK(allScales().size() == 13);
    for (Scale s : allScales()) {
        CHECK(parseScale(scaleName(s)) == s);
        const auto intervals = scaleIntervals(s);
        CHECK(intervals.front() == 0);
        CHECK(std::is_sorted(intervals.begin(), intervals.end()));
        CHECK(intervals.back() < 12);
    }
    CHECK_FALSE(parseScale("Major"));
    CHECK_FALSE(parseScale("whole_tone"));
    CHECK(scaleIntervals(Scale::HarmonicMinor).size() == 7);
    CHECK(scaleIntervals(Scale::Blues).size() == 6);
    CHECK(scaleIntervals(Scale::Chromatic).size() == 12);
}

TEST_CASE("the fifteen major and fifteen minor key signatures spell and the rest are refused") {
    // Signed sharps minus flats, by tonic.
    const std::vector<std::pair<const char*, int>> major = {
        {"C", 0},  {"G", 1},   {"D", 2},   {"A", 3},   {"E", 4},   {"B", 5},   {"F#", 6}, {"C#", 7},
        {"F", -1}, {"Bb", -2}, {"Eb", -3}, {"Ab", -4}, {"Db", -5}, {"Gb", -6}, {"Cb", -7}};
    const std::vector<std::pair<const char*, int>> minor = {
        {"A", 0},  {"E", 1},  {"B", 2},  {"F#", 3}, {"C#", 4},  {"G#", 5},  {"D#", 6}, {"A#", 7},
        {"D", -1}, {"G", -2}, {"C", -3}, {"F", -4}, {"Bb", -5}, {"Eb", -6}, {"Ab", -7}};
    std::set<std::string> supportedMajor, supportedMinor;
    for (const auto& [tonic, count] : major) {
        const Key k = key(tonic, Scale::Major);
        CHECK(keySupported(k));
        CHECK_MESSAGE(signature(k) == count, tonic << " major");
        supportedMajor.insert(tonic);
    }
    for (const auto& [tonic, count] : minor) {
        const Key k = key(tonic, Scale::Minor);
        CHECK(keySupported(k));
        CHECK_MESSAGE(signature(k) == count, tonic << " minor");
        supportedMinor.insert(tonic);
    }
    int refusedMajor = 0, refusedMinor = 0;
    for (char letter : std::string("ABCDEFG"))
        for (const char* accidental : {"", "#", "b"}) {
            const std::string tonic = std::string(1, letter) + accidental;
            CHECK(keySupported(key(tonic, Scale::Major)) == (supportedMajor.count(tonic) > 0));
            CHECK(keySupported(key(tonic, Scale::Minor)) == (supportedMinor.count(tonic) > 0));
            refusedMajor += !keySupported(key(tonic, Scale::Major));
            refusedMinor += !keySupported(key(tonic, Scale::Minor));
            CHECK(keySupported(key(tonic, Scale::Chromatic)));
        }
    CHECK(refusedMajor == 6);  // Fb, E#, B#, D#, G#, A#
    CHECK(refusedMinor == 6);  // Cb, Gb, Db, Fb, E#, B#
}

TEST_CASE("a supported diatonic key names each degree with its own letter") {
    for (Scale s :
         {Scale::Major, Scale::Minor, Scale::HarmonicMinor, Scale::MelodicMinor, Scale::Dorian,
          Scale::Phrygian, Scale::Lydian, Scale::Mixolydian, Scale::Locrian})
        for (char letter : std::string("ABCDEFG"))
            for (const char* accidental : {"", "#", "b"}) {
                const Key k = key(std::string(1, letter) + accidental, s);
                if (!keySupported(k)) continue;
                std::set<char> letters;
                std::istringstream names(degrees(k));
                std::string name;
                int degree = 0;
                while (names >> name) {
                    letters.insert(name[0]);
                    const int pc = pitchClassNumber(*parsePitchClass(name));
                    CHECK(pc == (pitchClassNumber(k.tonic) + scaleIntervals(s)[degree]) % 12);
                    ++degree;
                }
                CHECK_MESSAGE(letters.size() == 7, degrees(k));
            }
}

TEST_CASE("spelling by hand") {
    CHECK(degrees(key("Eb", Scale::Major)) == "Eb F G Ab Bb C D");
    CHECK(degrees(key("F#", Scale::Major)) == "F# G# A# B C# D# E#");
    CHECK(degrees(key("Cb", Scale::Major)) == "Cb Db Eb Fb Gb Ab Bb");
    CHECK(degrees(key("C#", Scale::Minor)) == "C# D# E F# G# A B");
    CHECK(degrees(key("A", Scale::HarmonicMinor)) == "A B C D E F G#");
    CHECK(degrees(key("A", Scale::MelodicMinor)) == "A B C D E F# G#");
    CHECK(degrees(key("D", Scale::Dorian)) == "D E F G A B C");
    CHECK(degrees(key("E", Scale::Phrygian)) == "E F G A B C D");
    CHECK(degrees(key("B", Scale::Locrian)) == "B C D E F G A");
    CHECK(degrees(key("Eb", Scale::MajorPentatonic)) == "Eb F G Bb C");
    CHECK(degrees(key("C", Scale::MinorPentatonic)) == "C Eb F G Bb");
    CHECK(degrees(key("A", Scale::Blues)) == "A C D Eb E G");
    CHECK(degrees(key("C", Scale::Blues)) == "C Eb F Gb G Bb");
    CHECK_FALSE(keySupported(key("G#", Scale::Major)));
    CHECK(keySupported(key("G#", Scale::MinorPentatonic)));
    CHECK(degrees(key("G#", Scale::MinorPentatonic)) == "G# B C# D# F#");
}

TEST_CASE("support is decided by the scale's own degrees, not its parent's") {
    // G# major needs F## for its seventh; the pentatonic scale leaves the seventh out.
    CHECK_FALSE(keySupported(key("G#", Scale::Major)));
    CHECK(keySupported(key("G#", Scale::MajorPentatonic)));
    CHECK(degrees(key("G#", Scale::MajorPentatonic)) == "G# A# B# D# E#");
    // D# major pentatonic keeps the third, F##.
    CHECK_FALSE(keySupported(key("D#", Scale::MajorPentatonic)));
    // Chromatic has no letters: every tonic is supported, even where the major is not.
    for (char letter : std::string("ABCDEFG"))
        for (const char* accidental : {"", "#", "b"})
            CHECK(keySupported(key(std::string(1, letter) + accidental, Scale::Chromatic)));
    // Blues (minor pentatonic plus the lowered fifth): refused when one of its own
    // degrees needs a double accidental, not when only the parent's would.
    CHECK(keySupported(key("G#", Scale::Blues)));
    CHECK_FALSE(keySupported(key("Cb", Scale::Blues)));            // Cb Ebb ...
    CHECK_FALSE(keySupported(key("Gb", Scale::MinorPentatonic)));  // Gb Bbb ...
}

TEST_CASE("pitch classes outside the scale take the key's direction") {
    CHECK(spellPitchClass(1, key("C", Scale::Major)) == "C#");  // equal: the default table
    CHECK(spellPitchClass(3, key("C", Scale::Major)) == "Eb");
    CHECK(spellPitchClass(1, key("F", Scale::Major)) == "Db");  // one flat: flats
    CHECK(spellPitchClass(6, key("F", Scale::Major)) == "Gb");
    CHECK(spellPitchClass(3, key("D", Scale::Major)) == "D#");  // two sharps: sharps
    CHECK(spellPitchClass(10, key("G", Scale::Major)) == "A#");
    CHECK(spellPitchClass(1, key("Eb", Scale::Major)) == "Db");
    CHECK(spellPitchClass(6, key("Eb", Scale::Major)) == "Gb");
    CHECK(spellPitchClass(8, key("A", Scale::Minor)) == "Ab");  // default table
    // A chromatic key takes the direction of the major scale on its tonic.
    CHECK(spellPitchClass(1, key("F", Scale::Chromatic)) == "Db");
    CHECK(spellPitchClass(1, key("D", Scale::Chromatic)) == "C#");
    CHECK(spellPitchClass(3, key("E", Scale::Chromatic)) == "D#");
    CHECK(spellPitchClass(0, key("G#", Scale::Chromatic)) == "C");
    // An unsupported key has no scale spelling but still has a direction.
    CHECK(spellPitchClass(1, key("G#", Scale::Major)) == "C#");
}

TEST_CASE("without a key the notation's default table") {
    const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    for (int pc = 0; pc < 12; ++pc) CHECK(spellPitchClass(pc) == names[pc]);
    for (int pitch = 0; pitch < 128; ++pitch)
        CHECK(spell(pitch) == std::string(names[pitch % 12]) + std::to_string(pitch / 12 - 1));
}

TEST_CASE("the octave follows the letter") {
    CHECK(spell(60, key("C", Scale::Major)) == "C4");
    CHECK(spell(70, key("F", Scale::Major)) == "Bb4");
    CHECK(spell(60, key("C#", Scale::Major)) == "B#3");  // the pitch of C4
    CHECK(spell(59, key("Cb", Scale::Major)) == "Cb4");  // the pitch of B3
    CHECK(spell(0, key("C", Scale::Major)) == "C-1");
    CHECK(spell(127, key("G", Scale::Major)) == "G9");
}

TEST_CASE("chord symbols parse to a canonical form and print back") {
    CHECK(printChord(*parseChord("Cmaj")) == "C");
    CHECK(printChord(*parseChord("Cmin")) == "Cm");
    CHECK(printChord(*parseChord("C+")) == "Caug");
    CHECK(printChord(*parseChord("Csus")) == "Csus4");
    CHECK(printChord(*parseChord("CM7")) == "Cmaj7");
    CHECK(printChord(*parseChord("Cmin7")) == "Cm7");
    CHECK(printChord(*parseChord("Cø")) == "Cm7b5");
    CHECK(printChord(*parseChord("Bb7#9")) == "Bb7#9");
    CHECK(printChord(*parseChord("F#m/C#")) == "F#m/C#");
    CHECK(printChord(*parseChord("ebmaj7/bb")) == "Ebmaj7/Bb");
    CHECK(*parseChord("D#m") != *parseChord("Ebm"));  // spelling is kept
    // Every name the table takes round trips through the canonical form.
    for (const auto& [quality, intervals] : chordQualities()) {
        const auto chord = parseChord("A" + quality);
        REQUIRE(chord);
        CHECK(chord->quality == canonicalQuality(quality));
        CHECK(*parseChord(printChord(*chord)) == *chord);
        // Aliases share intervals with their canonical name.
        CHECK(chordQualities().at(std::string(canonicalQuality(quality))) == intervals);
    }
    CHECK(chordQualities().size() == 31);
}

TEST_CASE("chord symbols that do not parse say why") {
    CHECK(parseChord("").error().code == "chord.bad_root");
    CHECK(parseChord("H7").error().code == "chord.bad_root");
    const auto quality = parseChord("Cfoo");
    REQUIRE_FALSE(quality);
    CHECK(quality.error().code == "chord.unknown_quality");
    CHECK(quality.error().message.find("'foo'") != std::string::npos);
    CHECK(quality.error().hint.find("maj7") != std::string::npos);
    CHECK(parseChord("C/H").error().code == "chord.bad_bass");
    CHECK(parseChord("C/").error().code == "chord.bad_bass");
}

TEST_CASE("a chord is voiced in close position from its root") {
    CHECK(*chordPitches(*parseChord("C"), 3) == std::vector<int>{48, 52, 55});
    CHECK(*chordPitches(*parseChord("Cm7"), 3) == std::vector<int>{48, 51, 55, 58});
    CHECK(*chordPitches(*parseChord("C9"), 4) == std::vector<int>{60, 64, 67, 70, 74});
    // A slash bass sits in the octave below, first.
    CHECK(*chordPitches(*parseChord("C/E"), 3) == std::vector<int>{40, 48, 52, 55});
    CHECK(*chordPitches(*parseChord("Eb/Bb"), 4) == std::vector<int>{58, 63, 67, 70});
    // Flats and sharps are the same pitch; Cb and B# keep their octave.
    CHECK(*chordPitches(*parseChord("Cb"), 4) == std::vector<int>{59, 63, 66});
    CHECK(*chordPitches(*parseChord("B#"), 3) == std::vector<int>{60, 64, 67});
    CHECK(chordPitches(*parseChord("C13"), 9).error().code == "chord.pitch_range");
    CHECK(chordPitches(*parseChord("C"), 10).error().code == "chord.bad_root");
    CHECK(chordPitches(*parseChord("C/E"), -1).error().code == "chord.bad_bass");
}

TEST_CASE("transposing by scale degrees") {
    const Key cMajor = key("C", Scale::Major);
    CHECK(transposeDegrees(60, cMajor, 0) == 60);
    CHECK(transposeDegrees(60, cMajor, 1) == 62);
    CHECK(transposeDegrees(60, cMajor, 2) == 64);
    CHECK(transposeDegrees(64, cMajor, 1) == 65);  // E to F, a semitone
    CHECK(transposeDegrees(60, cMajor, 7) == 72);  // an octave
    CHECK(transposeDegrees(60, cMajor, -1) == 59);
    CHECK(transposeDegrees(60, cMajor, -7) == 48);
    CHECK(transposeDegrees(60, cMajor, 15) == 60 + 24 + 2);
    // Out of the scale: first to the nearest scale pitch below, then moved.
    CHECK(transposeDegrees(61, cMajor, 0) == 60);
    CHECK(transposeDegrees(61, cMajor, 1) == 62);
    CHECK(transposeDegrees(70, cMajor, 1) == 69 + 2);  // Bb falls to A, then B
    // The tonic's octave is not the pitch's: a key off C.
    const Key dMinor = key("D", Scale::Minor);
    CHECK(transposeDegrees(62, dMinor, 2) == 65);  // D to F
    CHECK(transposeDegrees(57, dMinor, 1) == 58);  // A to Bb
    CHECK(transposeDegrees(58, dMinor, -1) == 57);
    // A pentatonic scale has five degrees per octave.
    const Key cPent = key("C", Scale::MinorPentatonic);
    CHECK(transposeDegrees(60, cPent, 5) == 72);
    CHECK(transposeDegrees(60, cPent, 1) == 63);
    // A chromatic scale's degrees are semitones.
    const Key chromatic = key("C", Scale::Chromatic);
    for (int p = 1; p < 127; ++p) {
        CHECK(transposeDegrees(p, chromatic, 1) == p + 1);
        CHECK(transposeDegrees(p, chromatic, -1) == p - 1);
    }
    // The result is a MIDI pitch or nothing.
    CHECK_FALSE(transposeDegrees(127, cMajor, 1));
    CHECK_FALSE(transposeDegrees(0, cMajor, -1));
    CHECK(transposeDegrees(127, cMajor, 0) == 127);  // G9 is in C major
}

TEST_CASE("transposing there and back lands on scale pitches") {
    for (Scale s : allScales()) {
        const Key k = key("Eb", s);
        const auto intervals = scaleIntervals(s);
        for (int p = 40; p < 90; ++p) {
            const int rel = ((p - pitchClassNumber(k.tonic)) % 12 + 12) % 12;
            const bool inScale =
                std::find(intervals.begin(), intervals.end(), rel) != intervals.end();
            if (!inScale) continue;
            for (int d : {-9, -3, 1, 4, 11}) {
                const auto up = transposeDegrees(p, k, d);
                REQUIRE(up);
                CHECK(transposeDegrees(*up, k, -d) == p);
            }
        }
    }
}

TEST_CASE("the context index answers what holds at a beat") {
    const Key c = key("C", Scale::Major), eb = key("Eb", Scale::Major);
    const auto cmaj = *parseChord("Cmaj7"), fm = *parseChord("Fm");
    ContextIndex index({{8, eb}, {0, c}}, {{4, fm}, {0, cmaj}, {6, std::nullopt}});
    CHECK(index.keyPoints().size() == 2);
    CHECK(index.keyPoints()[0].beat == 0);  // sorted
    CHECK(index.keyAt(0) == c);
    CHECK(index.keyAt(7.999) == c);
    CHECK(index.keyAt(8) == eb);
    CHECK(index.keyAt(1000) == eb);
    CHECK_FALSE(index.keyAt(-0.5));
    CHECK(index.chordAt(3.99) == cmaj);
    CHECK(index.chordAt(4) == fm);
    CHECK_FALSE(index.chordAt(6));  // "no chord from here"
    CHECK_FALSE(index.chordAt(100));
    // Nothing before the first point.
    ContextIndex late({{4, c}}, {{4, cmaj}});
    CHECK_FALSE(late.keyAt(3.9));
    CHECK_FALSE(late.chordAt(3.9));
    CHECK(late.keyAt(4) == c);
    // Two points of a type at one beat: the later one given wins.
    ContextIndex twice({{2, c}, {2, eb}}, {});
    CHECK(twice.keyPoints().size() == 1);
    CHECK(twice.keyAt(2) == eb);
    ContextIndex empty;
    CHECK_FALSE(empty.keyAt(0));
    CHECK_FALSE(empty.chordAt(0));
}

TEST_CASE("the index agrees with a scan of the points") {
    std::mt19937 rng(7);
    const std::vector<Key> keys = {key("C", Scale::Major), key("A", Scale::Minor),
                                   key("F#", Scale::Dorian), key("Bb", Scale::Blues)};
    const std::vector<Chord> chords = {*parseChord("C"), *parseChord("Am7"), *parseChord("G7/B"),
                                       *parseChord("Dsus4")};
    for (int round = 0; round < 20; ++round) {
        std::vector<KeyPoint> kp;
        std::vector<ChordPoint> cp;
        for (int i = 0; i < 12; ++i) {
            kp.push_back({static_cast<double>(rng() % 40), keys[rng() % keys.size()]});
            cp.push_back({static_cast<double>(rng() % 40) / 2, chords[rng() % chords.size()]});
        }
        ContextIndex index(kp, cp);
        for (double beat = -1; beat < 42; beat += 0.25) {
            std::optional<Key> wantKey;
            double bestKey = -1e9;
            for (const auto& p : kp)  // the last given of the latest beat at or before
                if (p.beat <= beat && p.beat >= bestKey) bestKey = p.beat, wantKey = p.key;
            std::optional<Chord> wantChord;
            double bestChord = -1e9;
            for (const auto& p : cp)
                if (p.beat <= beat && p.beat >= bestChord) bestChord = p.beat, wantChord = p.chord;
            CHECK(index.keyAt(beat) == wantKey);
            CHECK(index.chordAt(beat) == wantChord);
        }
    }
}

TEST_CASE("pitch classes of a key and of a chord") {
    CHECK(keyPitchClasses(key("C", Scale::Major)) == std::vector<int>{0, 2, 4, 5, 7, 9, 11});
    CHECK(keyPitchClasses(key("Eb", Scale::Minor)) == std::vector<int>{1, 3, 5, 6, 8, 10, 11});
    CHECK(keyPitchClasses(key("A", Scale::MinorPentatonic)) == std::vector<int>{0, 2, 4, 7, 9});
    CHECK(keyPitchClasses(key("F#", Scale::Chromatic)).size() == 12);
    CHECK(keyPitchClasses(key("Eb", Scale::Major)) == std::vector<int>{0, 2, 3, 5, 7, 8, 10});
    CHECK(chordPitchClasses(*parseChord("Cm7")) == std::vector<int>{0, 3, 7, 10});
    CHECK(chordPitchClasses(*parseChord("Bb")) == std::vector<int>{2, 5, 10});
    CHECK(chordPitchClasses(*parseChord("C/E")) == std::vector<int>{0, 4, 7});  // bass is a tone
    CHECK(chordPitchClasses(*parseChord("C/D")) == std::vector<int>{0, 2, 4, 7});
    CHECK(chordPitchClasses(*parseChord("Cmin7")) == chordPitchClasses(*parseChord("Cm7")));
}

TEST_CASE("snapping a pitch to a set of classes") {
    const std::vector<int> cMajor = keyPitchClasses(key("C", Scale::Major));
    using M = SnapMode;
    // In the set: unchanged in every mode.
    for (M m : {M::Nearest, M::Down, M::Up}) CHECK(snapToPitchClasses(60, cMajor, m) == 60);
    // C# (61): a tie between C and D goes down for nearest.
    CHECK(snapToPitchClasses(61, cMajor, M::Nearest) == 60);
    CHECK(snapToPitchClasses(61, cMajor, M::Down) == 60);
    CHECK(snapToPitchClasses(61, cMajor, M::Up) == 62);
    // Eb (63): D is below by one, E above by one: down again.
    CHECK(snapToPitchClasses(63, cMajor, M::Nearest) == 62);
    // A pentatonic gap: C (60) in A minor pentatonic {A C D E G}: in set; F (65): E below 1, G
    // above 2.
    const std::vector<int> aMinPent = keyPitchClasses(key("A", Scale::MinorPentatonic));
    CHECK(snapToPitchClasses(65, aMinPent, M::Nearest) == 64);
    CHECK(snapToPitchClasses(65, aMinPent, M::Up) == 67);
    CHECK(snapToPitchClasses(70, aMinPent, M::Nearest) == 69);  // Bb: A below 1, C above 2
    // Range edges: nothing below 0 or above 127.
    const std::vector<int> onlyCsharp{1};
    CHECK_FALSE(snapToPitchClasses(0, onlyCsharp, M::Down));
    CHECK(snapToPitchClasses(0, onlyCsharp, M::Up) == 1);
    CHECK(snapToPitchClasses(127, onlyCsharp, M::Nearest) == 121);  // G (127) up has no C#
    CHECK_FALSE(snapToPitchClasses(127, onlyCsharp, M::Up));
    // One class: the farthest gap is a whole octave less one.
    const std::vector<int> onlyC{0};
    CHECK(snapToPitchClasses(66, onlyC, M::Nearest) == 60);  // F#: six below and six above: down
    CHECK(snapToPitchClasses(67, onlyC, M::Nearest) == 72);
    CHECK_FALSE(snapToPitchClasses(60, std::vector<int>{}, M::Nearest));
    CHECK_FALSE(snapToPitchClasses(-1, cMajor));
    CHECK_FALSE(snapToPitchClasses(128, cMajor));
    // Every pitch of every scale in every key lands in the scale and moves by at most 6.
    for (Scale scale : allScales())
        for (int tonic = 0; tonic < 12; ++tonic) {
            const Key k{*parsePitchClass(spellPitchClass(tonic)), scale};
            const auto classes = keyPitchClasses(k);
            for (int p = 12; p < 116; ++p) {
                const auto got = snapToPitchClasses(p, classes);
                REQUIRE(got);
                CHECK(std::find(classes.begin(), classes.end(), *got % 12) != classes.end());
                CHECK(std::abs(*got - p) <= 6);
            }
        }
}

// --- following (specs/harmony.md §5) ----------------------------------------------------------

namespace {

Place keyOnly(std::string_view tonic, Scale scale) {
    return Place{key(tonic, scale), std::nullopt};
}
Place keyChord(std::string_view tonic, Scale scale, std::string_view chord) {
    return Place{key(tonic, scale), *parseChord(chord)};
}
Place chordOnly(std::string_view chord) { return Place{std::nullopt, *parseChord(chord)}; }
Followed keyFollow(int pitch, const Place& from, const Place& to) {
    return follow(pitch, from, to, FollowMode::Key);
}
Followed chordFollow(int pitch, const Place& from, const Place& to) {
    return follow(pitch, from, to, FollowMode::Chords);
}
// C4 = 60: a note named like "E4".
int note(std::string_view name) {
    const char letter = name[0];
    int semitone = pitchClassSemitone(PitchClass{letter, 0});
    std::size_t i = 1;
    if (name[i] == '#')
        semitone += 1, ++i;
    else if (name[i] == 'b')
        semitone -= 1, ++i;
    return 12 * (std::atoi(std::string(name.substr(i)).c_str()) + 1) + semitone;
}

}  // namespace

TEST_CASE("follow in key mode: the same scale transposes by the nearest interval") {
    const Place c = keyOnly("C", Scale::Major), d = keyOnly("D", Scale::Major);
    for (int p = 0; p <= 125; ++p) {
        const Followed f = keyFollow(p, c, d);
        CHECK(f.pitch == p + 2);
        CHECK(f.by == FollowedBy::Transpose);
        CHECK_FALSE(f.folded);
    }
    for (int p : {126, 127}) {
        const Followed f = keyFollow(p, c, d);
        CHECK(f.pitch == p + 2 - 12);  // 116, 117
        CHECK(f.folded);
    }
    CHECK(keyFollow(60, c, keyOnly("F#", Scale::Major)).pitch == 54);  // the tritone goes down
    CHECK(tonicInterval(PitchClass{'C', 0}, PitchClass{'F', 1}) == -6);
    CHECK(tonicInterval(PitchClass{'C', 0}, PitchClass{'G', 0}) == -5);
    CHECK(tonicInterval(PitchClass{'G', 0}, PitchClass{'C', 0}) == 5);
}

TEST_CASE("follow in key mode: scales of the same size map by degree") {
    const Place cMajor = keyOnly("C", Scale::Major), dMinor = keyOnly("D", Scale::Minor);
    CHECK(keyFollow(note("E4"), cMajor, dMinor).pitch == note("F4"));
    CHECK(keyFollow(note("E4"), cMajor, dMinor).by == FollowedBy::Degree);
    CHECK(keyFollow(note("F#4"), cMajor, dMinor).pitch == note("G#4"));
    const Place aMinor = keyOnly("A", Scale::Minor);
    CHECK(keyFollow(note("A4"), aMinor, cMajor).pitch == note("C5"));
    CHECK(keyFollow(note("C5"), aMinor, cMajor).pitch == note("E5"));
    CHECK(keyFollow(note("G4"), aMinor, cMajor).pitch == note("B4"));
    const Place cMinor = keyOnly("C", Scale::Minor), eb = keyOnly("Eb", Scale::Major);
    CHECK(keyFollow(note("C4"), cMinor, eb).pitch == note("Eb4"));
    CHECK(keyFollow(note("Eb4"), cMinor, eb).pitch == note("G4"));
    CHECK(keyFollow(note("G4"), cMinor, eb).pitch == note("Bb4"));
    // Harmonic minor's G and G# both land on B in C major: the rule keeps degrees.
    const Place aHarmonic = keyOnly("A", Scale::HarmonicMinor);
    CHECK(keyFollow(note("G4"), aHarmonic, cMajor).pitch == note("B4"));
    CHECK(keyFollow(note("G#4"), aHarmonic, cMajor).pitch == note("B4"));
    // Pentatonic to pentatonic.
    CHECK(keyFollow(note("F#4"), keyOnly("C", Scale::MajorPentatonic),
                    keyOnly("A", Scale::MinorPentatonic))
              .pitch == note("E4"));
    // Low pitches, where the scale pitch below lies under 0 (pitch 0 in B major).
    const Followed low = keyFollow(0, keyOnly("B", Scale::Major), keyOnly("C", Scale::Minor));
    CHECK(low.pitch >= 0);
    CHECK(low.pitch <= 127);
}

TEST_CASE("follow in key mode: scales of different sizes transpose, and a missing key copies") {
    const Followed shape =
        keyFollow(note("E4"), keyOnly("C", Scale::Major), keyOnly("C", Scale::MinorPentatonic));
    CHECK(shape.pitch == note("E4"));
    CHECK(shape.by == FollowedBy::Transpose);
    CHECK(shape.scaleShape);
    CHECK_FALSE(keyFollow(60, keyOnly("C", Scale::Major), keyOnly("D", Scale::Major)).scaleShape);
    const Place none;
    for (const auto& [from, to] :
         {std::pair{keyOnly("C", Scale::Major), none}, std::pair{none, keyOnly("D", Scale::Major)},
          std::pair{none, none}}) {
        const Followed f = keyFollow(61, from, to);
        CHECK(f.pitch == 61);
        CHECK(f.by == FollowedBy::Copy);
    }
}

TEST_CASE("follow in chords mode: tones map by function") {
    const Place cmaj7 = chordOnly("Cmaj7"), dm7 = chordOnly("Dm7");
    // Cmaj7 -> Dm7, tone by tone, in each octave: the root, third, fifth and seventh go to
    // their counterparts (D, F, A, C), each in the octave nearest.
    for (int octave = 1; octave <= 7; ++octave) {
        const int c = 12 * (octave + 1);
        CHECK(chordFollow(c, cmaj7, dm7).pitch == c + 2);        // C -> D
        CHECK(chordFollow(c + 4, cmaj7, dm7).pitch == c + 5);    // E -> F
        CHECK(chordFollow(c + 7, cmaj7, dm7).pitch == c + 9);    // G -> A
        CHECK(chordFollow(c + 11, cmaj7, dm7).pitch == c + 12);  // B -> C
    }
    CHECK(chordFollow(60, cmaj7, dm7).by == FollowedBy::Chords);
    // At the top, with keys: G9 (127) goes to A9 (129) by the nearest rule and folds to A8.
    const Place from = keyChord("C", Scale::Major, "Cmaj7"),
                to = keyChord("D", Scale::Major, "Dm7");
    const Followed top = chordFollow(127, from, to);
    CHECK(top.pitch == 117);
    CHECK(top.folded);
    // C11 -> Dm7 maps C, G, Bb, D, F to D, A, C, D, F.
    const Place c11 = chordOnly("C11");
    const auto cls = [](int p) { return ((p % 12) + 12) % 12; };
    CHECK(cls(chordFollow(note("C4"), c11, dm7).pitch) == 2);
    CHECK(cls(chordFollow(note("G4"), c11, dm7).pitch) == 9);
    CHECK(cls(chordFollow(note("Bb4"), c11, dm7).pitch) == 0);
    CHECK(cls(chordFollow(note("D5"), c11, dm7).pitch) == 2);
    CHECK(cls(chordFollow(note("F5"), c11, dm7).pitch) == 5);
    // C5 -> Dm maps G to A.
    CHECK(cls(chordFollow(note("G4"), chordOnly("C5"), chordOnly("Dm")).pitch) == 9);
    // C13 -> D maps C, E, Bb, D, A to D, F#, A, D, A.
    const Place c13 = chordOnly("C13"), dMajor = chordOnly("D");
    CHECK(cls(chordFollow(note("C4"), c13, dMajor).pitch) == 2);
    CHECK(cls(chordFollow(note("E4"), c13, dMajor).pitch) == 6);
    CHECK(cls(chordFollow(note("Bb4"), c13, dMajor).pitch) == 9);
    CHECK(cls(chordFollow(note("D5"), c13, dMajor).pitch) == 2);
    CHECK(cls(chordFollow(note("A5"), c13, dMajor).pitch) == 9);
    // C11 -> D5: the eleventh F goes to D (no third, so the root).
    CHECK(cls(chordFollow(note("F5"), c11, chordOnly("D5")).pitch) == 2);
    // Cmaj7 -> C5 keeps both of two notes that land together: C4 E4 G4 B4 -> C4 C4 G4 G4.
    const Place c5 = chordOnly("C5");
    CHECK(chordFollow(note("C4"), cmaj7, c5).pitch == note("C4"));
    CHECK(chordFollow(note("E4"), cmaj7, c5).pitch == note("C4"));
    CHECK(chordFollow(note("G4"), cmaj7, c5).pitch == note("G4"));
    CHECK(chordFollow(note("B4"), cmaj7, c5).pitch == note("G4"));
}

TEST_CASE("follow in chords mode: slash basses, other notes, the octave, and the key fallback") {
    const auto cls = [](int p) { return ((p % 12) + 12) % 12; };
    // A slash bass that is not a tone maps to the target's bass: C/D -> G/A maps D to A.
    CHECK(cls(chordFollow(note("D3"), chordOnly("C/D"), chordOnly("G/A")).pitch) == 9);
    // One that is a tone maps as the tone: C/E -> G/D maps E to B.
    CHECK(cls(chordFollow(note("E3"), chordOnly("C/E"), chordOnly("G/D")).pitch) == 11);
    // C over Dm in C major, to Em in D major, gives D (the seventh to the seventh).
    CHECK(cls(chordFollow(note("C4"), keyChord("C", Scale::Major, "Dm"),
                          keyChord("D", Scale::Major, "Em"))
                  .pitch) == 2);
    // A over G7 in C major, to A7 in D minor, gives Bb.
    CHECK(cls(chordFollow(note("A4"), keyChord("C", Scale::Major, "G7"),
                          keyChord("D", Scale::Minor, "A7"))
                  .pitch) == 10);
    // The octave follows the roots, not the keys: keys C major to F# major, the
    // chord C held, C4 stays C4. A tie between the roots goes down: C to F#
    // takes C4 to F#3.
    CHECK(
        chordFollow(note("C4"), keyChord("C", Scale::Major, "C"), keyChord("F#", Scale::Major, "C"))
            .pitch == note("C4"));
    CHECK(chordFollow(note("C4"), chordOnly("C"), chordOnly("F#")).pitch == note("F#3"));
    // A chord missing on either side falls back to key mode for the note.
    const Followed missing =
        chordFollow(note("E4"), keyOnly("C", Scale::Major), keyChord("D", Scale::Minor, "Dm"));
    CHECK(missing.pitch == note("F4"));
    CHECK(missing.by == FollowedBy::Degree);
    const Followed neither = chordFollow(61, Place{}, Place{});
    CHECK(neither.pitch == 61);
    CHECK(neither.by == FollowedBy::Copy);
}

TEST_CASE("follow in chords mode: a figure moves as a block by its roots' interval") {
    // E1 (the GUI lead's third walk): the octave came from the key-followed
    // pitch, so a Bm figure written in D major, under B7 in A minor (the keys
    // -5 apart, a tritone from three of its four targets), split across
    // octaves: B3 A4 F#3 D#3. Under B7 in E minor it was B4 A4 F#4 D#4. The
    // reference is now the pitch moved by the roots' interval, so the figure
    // keeps its contour whatever the keys.
    const std::vector<int> figure = {note("B4"), note("A4"), note("F#4"), note("D4")};
    const std::vector<int> heard = {note("B4"), note("A4"), note("F#4"), note("D#4")};
    const Place written = keyChord("D", Scale::Major, "Bm");
    for (const Place& to :
         {keyChord("A", Scale::Minor, "B7"), keyChord("E", Scale::Minor, "B7"), chordOnly("B7")}) {
        std::vector<int> got;
        for (int p : figure) got.push_back(chordFollow(p, written, to).pitch);
        CHECK(got == heard);
    }
    // A cell over Am7 plays one voicing over Am7 whatever the keys: A4 C5 E5
    // G5 stays itself, from any key it was written in, under E minor as under
    // A minor (it played an octave apart).
    const std::vector<int> cell = {note("A4"), note("C5"), note("E5"), note("G5")};
    for (const char* tonic : {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"})
        for (const Scale scale : {Scale::Major, Scale::Minor}) {
            const Place from = keyChord(tonic, scale, "Am7");
            const std::string writtenIn = tonic;
            CAPTURE(writtenIn);
            for (const Place& to :
                 {keyChord("E", Scale::Minor, "Am7"), keyChord("A", Scale::Minor, "Am7")})
                for (int p : cell) CHECK(chordFollow(p, from, to).pitch == p);
        }
    // So for a chord tone in general: one pair of chords, any keys on either
    // side, gives the pitch the chords alone give.
    const std::vector<Place> keys = {keyOnly("C", Scale::Major), keyOnly("F#", Scale::Minor),
                                     keyOnly("Bb", Scale::Dorian), keyOnly("E", Scale::Minor),
                                     keyOnly("A", Scale::HarmonicMinor)};
    for (const auto& [fromChord, toChord] :
         {std::pair{"Bm", "B7"}, std::pair{"Cmaj7", "F#m7b5"}, std::pair{"G7", "Db9"},
          std::pair{"Esus4", "A5"}, std::pair{"C13", "D"}}) {
        const std::string pair = std::string(fromChord) + " to " + toChord;
        CAPTURE(pair);
        const auto chordTone = [&](int p) {
            for (int interval : chordQualities().at(parseChord(fromChord)->quality))
                if ((p - pitchClassNumber(parseChord(fromChord)->root) - interval) % 12 == 0)
                    return true;
            return false;
        };
        for (const Place& fk : keys)
            for (const Place& tk : keys)
                for (int p = 24; p <= 100; ++p) {
                    if (!chordTone(p)) continue;
                    const Place from{fk.key, *parseChord(fromChord)};
                    const Place to{tk.key, *parseChord(toChord)};
                    CHECK(chordFollow(p, from, to).pitch ==
                          chordFollow(p, chordOnly(fromChord), chordOnly(toChord)).pitch);
                }
    }
}

TEST_CASE("follow never fails: any pitch, any pair of places, lands in range") {
    const std::vector<Place> places = {
        Place{},
        keyOnly("C", Scale::Major),
        keyOnly("F#", Scale::HarmonicMinor),
        keyChord("Eb", Scale::Minor, "Cm7"),
        keyChord("B", Scale::Blues, "F#7#9"),
        chordOnly("G11"),
        keyChord("A", Scale::Chromatic, "Asus2/E"),
    };
    for (const Place& from : places)
        for (const Place& to : places)
            for (int p = 0; p <= 127; ++p)
                for (const FollowMode mode : {FollowMode::Key, FollowMode::Chords}) {
                    const Followed f = follow(p, from, to, mode);
                    CHECK(f.pitch >= 0);
                    CHECK(f.pitch <= 127);
                }
}

#ifndef HARMONY_NO_ALLOCATION_COUNT
TEST_CASE("follow allocates nothing, the first call included") {
    // harmony.md section 5: X4 builds its tables from follow, so it must not touch the heap.
    // The places are built first; only the calls are counted.
    const Place cmaj7 = keyChord("C", Scale::Major, "Cmaj7");
    const Place dm7 = keyChord("D", Scale::Minor, "Dm7");
    const Place c11 = keyChord("Eb", Scale::Major, "C11/Bb");
    const Place none;
    const long before = gAllocations.load();
    int sum = 0;
    for (int pitch = 0; pitch <= 127; ++pitch)
        for (const FollowMode mode : {FollowMode::Key, FollowMode::Chords})
            for (const Place* from : {&cmaj7, &c11, &none})
                for (const Place* to : {&dm7, &cmaj7, &none})
                    sum += follow(pitch, *from, *to, mode).pitch;
    const long made = gAllocations.load() - before;
    CHECK(made == 0);
    CHECK(sum > 0);
}
#endif

// --- chord-tone spelling (specs/harmony.md §3.1) ------------------------------------------------

namespace {

Chord chord(std::string_view text) { return *parseChord(text); }

// The tones of a chord by degree, space separated, root first; the slash bass is not a tone.
std::string toneNames(std::string_view text) {
    std::string out;
    for (const ChordTone& t : chordTones(chord(text))) {
        if (t.degree == "bass") continue;
        if (!out.empty()) out += ' ';
        out += printPitchClass(t.name);
    }
    return out;
}

// The 21 root spellings.
std::vector<PitchClass> allRoots() {
    std::vector<PitchClass> roots;
    for (const char letter : {'A', 'B', 'C', 'D', 'E', 'F', 'G'})
        for (const int accidental : {-1, 0, 1}) roots.push_back({letter, accidental});
    return roots;
}

int degreeNumber(std::string_view degree) {
    int n = 0;
    for (const char c : degree)
        if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
    return n;
}

}  // namespace

TEST_CASE("a chord tone is named by its degree") {
    CHECK(toneNames("E") == "E G# B");           // never Ab
    CHECK(toneNames("C7") == "C E G Bb");        // the seventh is Bb
    CHECK(toneNames("Bdim7") == "B D F Ab");     // A flat
    CHECK(toneNames("Cdim7") == "C Eb Gb Bbb");  // B double flat
    CHECK(toneNames("Cm") == "C Eb G");
    CHECK(toneNames("Csus4") == "C F G");
    CHECK(toneNames("Fsus2") == "F G C");
    CHECK(toneNames("Bbsus4") == "Bb Eb F");
    CHECK(toneNames("Cadd9") == "C E G D");
    CHECK(toneNames("C6") == "C E G A");
    CHECK(toneNames("Cm6") == "C Eb G A");
    CHECK(toneNames("C5") == "C G");
    CHECK(toneNames("Cmaj7") == "C E G B");
    CHECK(toneNames("CmMaj7") == "C Eb G B");
    CHECK(toneNames("Cm7b5") == "C Eb Gb Bb");
    CHECK(toneNames("C7sus4") == "C F G Bb");
    CHECK(toneNames("C9") == "C E G Bb D");
    CHECK(toneNames("Cmaj9") == "C E G B D");
    CHECK(toneNames("Cm9") == "C Eb G Bb D");
    CHECK(toneNames("C11") == "C G Bb D F");  // no third
    CHECK(toneNames("C13") == "C E Bb D A");  // no fifth
    CHECK(toneNames("Caug") == "C E G#");
    CHECK(toneNames("Cdim") == "C Eb Gb");
    SUBCASE("altered ninths") {
        CHECK(toneNames("C7b9") == "C E G Bb Db");
        CHECK(toneNames("B7b9") == "B D# F# A C");
        CHECK(toneNames("Ab7b9") == "Ab C Eb Gb Bbb");
        CHECK(toneNames("C7#9") == "C E G Bb Eb");
        // The sharp nine takes the b3 letter: G over E7#9, never F##.
        CHECK(toneNames("E7#9") == "E G# B D G");
        CHECK(toneNames("G7#9") == "G B D F Bb");
    }
    SUBCASE("the fifteen doubles of the twelve usual roots") {
        CHECK(toneNames("Ebdim") == "Eb Gb Bbb");
        CHECK(toneNames("Abdim") == "Ab Cb Ebb");
        CHECK(toneNames("C#aug") == "C# E# G##");
        CHECK(toneNames("F#aug") == "F# A# C##");
        CHECK(toneNames("Baug") == "B D# F##");
        CHECK(toneNames("Ebdim7") == "Eb Gb Bbb Dbb");
        CHECK(toneNames("Fdim7") == "F Ab Cb Ebb");
        CHECK(toneNames("Abdim7") == "Ab Cb Ebb Gbb");
        CHECK(toneNames("Bbdim7") == "Bb Db Fb Abb");
        CHECK(toneNames("Ebm7b5") == "Eb Gb Bbb Db");
        CHECK(toneNames("Abm7b5") == "Ab Cb Ebb Gb");
    }
    SUBCASE("a root with an accidental") {
        CHECK(toneNames("F#") == "F# A# C#");
        CHECK(toneNames("Db7") == "Db F Ab Cb");
        CHECK(toneNames("G#m7") == "G# B D# F#");
    }
}

TEST_CASE("the three triples fall back, and no tone ever has three accidentals") {
    // B#aug's fifth would be F###, Cbdim7's seventh Bbbb, Fbdim7's seventh Ebbb.
    struct Case {
        const char* symbol;
        const char* degree;
    };
    for (const Case c : {Case{"B#aug", "#5"}, Case{"Cbdim7", "bb7"}, Case{"Fbdim7", "bb7"}}) {
        int fallbacks = 0;
        for (const ChordTone& t : chordTones(chord(c.symbol)))
            if (!t.byDegree) {
                ++fallbacks;
                CHECK(t.degree == c.degree);
                CHECK(t.name.accidental >= -1);  // the default table's: at most one accidental
                CHECK(t.name.accidental <= 1);
            }
        CHECK(fallbacks == 1);
    }
    int total = 0;
    for (const PitchClass& root : allRoots())
        for (const auto& [quality, intervals] : chordQualities()) {
            (void)intervals;
            for (const ChordTone& t : chordTones(Chord{root, quality, std::nullopt}))
                if (!t.byDegree) ++total;
        }
    // Each of the three symbols, once for every quality name that is its canonical or alias.
    CHECK(total == 4);  // B#aug and B#+, Cbdim7, Fbdim7
}

TEST_CASE("every quality row's tones: degree, letter and class agree with the interval") {
    static constexpr int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
    for (const PitchClass& root : allRoots())
        for (const auto& [quality, intervals] : chordQualities()) {
            const Chord c{root, quality, std::nullopt};
            const auto tones = chordTones(c);
            REQUIRE(tones.size() == intervals.size());
            std::set<int> classes;
            for (std::size_t i = 0; i < tones.size(); ++i) {
                const ChordTone& t = tones[i];
                CAPTURE(printChord(c));
                CAPTURE(t.degree);
                CHECK(t.interval == intervals[i]);
                // The degree's alteration against the major scale gives the interval.
                const int number = degreeNumber(t.degree);
                int alteration = 0;
                for (const char ch : t.degree) alteration += ch == '#' ? 1 : (ch == 'b' ? -1 : 0);
                const int major = kMajor[(number - 1) % 7] + ((number - 1) / 7) * 12;
                CHECK(((major + alteration - intervals[i]) % 12) == 0);
                const int cls = ((pitchClassSemitone(root) + intervals[i]) % 12 + 12) % 12;
                CHECK(classes.insert(cls).second);  // no two tones share a class
                if (t.byDegree) {
                    CHECK(pitchClassNumber(t.name) == cls);
                    CHECK(t.name.accidental >= -2);
                    CHECK(t.name.accidental <= 2);
                    // The letter is the root's letter moved by the degree; the sharp nine
                    // takes the third's letter.
                    const int offset = t.degree == "#9" ? 2 : (number - 1) % 7;
                    CHECK(t.name.letter == char('A' + (root.letter - 'A' + offset) % 7));
                }
            }
            // The tones, as classes, are chordPitchClasses.
            std::vector<int> sorted(classes.begin(), classes.end());
            CHECK(sorted == chordPitchClasses(c));
        }
}

TEST_CASE("a chord tone's name does not depend on the key") {
    const std::vector<std::string> chords = {"E",      "C7",    "Bdim7", "Cdim7",
                                             "F#m7b5", "Abaug", "Bb7#9"};
    for (const std::string& symbol : chords) {
        const Chord c = chord(symbol);
        for (const ChordTone& t : chordTones(c)) {
            if (t.degree == "bass" || !t.byDegree) continue;
            const int cls = pitchClassNumber(t.name);
            const std::string want = printPitchClass(t.name);
            CHECK(spellPitchClass(cls, Place{std::nullopt, c}) == want);
            for (const Scale scale : allScales())
                for (const PitchClass& tonic : allRoots()) {
                    const Key k{tonic, scale};
                    CHECK(spellPitchClass(cls, Place{k, c}) == want);
                }
        }
    }
}

TEST_CASE("a slash bass as written wins for its class") {
    CHECK(spellPitchClass(4, Place{std::nullopt, chord("C/E")}) == "E");
    // The chord's own third is G#; the writer spelled the bass Ab.
    CHECK(spellPitchClass(8, Place{std::nullopt, chord("E/Ab")}) == "Ab");
    CHECK(spellPitchClass(8, Place{std::nullopt, chord("E")}) == "G#");
    // A bass that is not a tone is as written and changes nothing else.
    CHECK(spellPitchClass(2, Place{std::nullopt, chord("C/D")}) == "D");
    CHECK(spellPitchClass(3, Place{std::nullopt, chord("C/D#")}) == "D#");
    CHECK(spellPitchClass(7, Place{std::nullopt, chord("C/D#")}) == "G");
    const auto tones = chordTones(chord("C/Eb"));
    REQUIRE(tones.size() == 4);
    CHECK(tones.back().degree == "bass");
    CHECK(tones.back().interval == 3);
    CHECK(printPitchClass(tones.back().name) == "Eb");
}

TEST_CASE("a note outside the chord: the key's scale first, then its step above the root") {
    const Key cMajor = key("C", Scale::Major);
    // The key's scale keeps its spelling: F is not E# over F#7.
    CHECK(spellPitchClass(5, Place{cMajor, chord("F#7")}) == "F");
    // Chromatic neighbours are named from the chord's root.
    CHECK(spellPitchClass(3, Place{cMajor, chord("E")}) == "D#");   // the major seventh
    CHECK(spellPitchClass(8, Place{cMajor, chord("Am")}) == "G#");  // the leading note
    CHECK(spellPitchClass(1, Place{cMajor, chord("Dm")}) == "C#");
    CHECK(spellPitchClass(8, Place{cMajor, chord("G7")}) == "Ab");  // the flat nine
    CHECK(spellPitchClass(10, Place{cMajor, chord("C")}) == "Bb");
    CHECK(spellPitchClass(6, Place{cMajor, chord("C7")}) == "F#");  // the sharp four
    CHECK(spellPitchClass(6, Place{key("C", Scale::Minor), chord("Cm")}) == "Gb");  // the flat five
    CHECK(spellPitchClass(8, Place{cMajor, chord("C")}) == "Ab");                   // the flat six
    CHECK(spellPitchClass(1, Place{cMajor, chord("C")}) == "Db");                   // the flat nine
    CHECK(spellPitchClass(3, Place{cMajor, chord("C")}) == "Eb");  // the flat third
    // With no key the chord still names its neighbours.
    CHECK(spellPitchClass(3, Place{std::nullopt, chord("E")}) == "D#");
    CHECK(spellPitchClass(5, Place{std::nullopt, chord("F#7")}) == "E#");
    // No chord: the key's rule or the default table, unchanged.
    CHECK(spellPitchClass(3, Place{cMajor, std::nullopt}) == "Eb");
    CHECK(spellPitchClass(3, Place{std::nullopt, std::nullopt}) == "Eb");
    // A quality this build does not know holds no chord: callers pass none, and a chord
    // with one names nothing by degree.
    CHECK(spellPitchClass(8, Place{cMajor, Chord{{'E', 0}, "zzz", std::nullopt}}) == "Ab");
    CHECK(chordTones(Chord{{'E', 0}, "zzz", std::nullopt}).empty());
}

TEST_CASE("a place spells a pitch with its octave and the octave stays in the compiler's range") {
    CHECK(spell(68, Place{std::nullopt, chord("E")}) == "G#4");
    CHECK(spell(70, Place{std::nullopt, chord("C7")}) == "Bb4");
    CHECK(spell(57, Place{std::nullopt, chord("Cdim7")}) == "Bbb3");  // the pitch of A3
    CHECK(spell(60, Place{std::nullopt, chord("B#aug")}) == "B#3");   // the root
    CHECK(spell(68, Place{std::nullopt, chord("B#aug")}) == "Ab4");   // F###: the default table's
    // B# at pitch 0 would be octave -2, which the notation refuses: the key's or the default
    // table's name is used instead. B# is the third of G# major.
    CHECK(spell(0, Place{std::nullopt, chord("G#")}) == "C-1");
    CHECK(spell(12, Place{std::nullopt, chord("G#")}) == "B#-1");
    CHECK(spell(60, Place{std::nullopt, chord("G#")}) == "B#3");
    // The same octave rule holds for a key alone (it used to give B#-2).
    CHECK(spell(0, key("C#", Scale::Major)) == "C-1");
    CHECK(spell(60, key("C#", Scale::Major)) == "B#3");
}

TEST_CASE("pitch classes print with up to two accidentals") {
    CHECK(printPitchClass({'F', 2}) == "F##");
    CHECK(printPitchClass({'B', -2}) == "Bbb");
    CHECK(printPitchClass({'C', 1}) == "C#");
    CHECK(printPitchClass({'C', -1}) == "Cb");
    CHECK(printPitchClass({'C', 0}) == "C");
    CHECK(pitchClassSemitone({'F', 2}) == 7);
    CHECK(pitchClassNumber({'B', -2}) == 9);
    CHECK(pitchClassNumber({'C', -2}) == 10);
    // Input still takes one accidental: roots and tonics are names a person writes.
    CHECK_FALSE(parsePitchClass("F##"));
    CHECK_FALSE(parsePitchClass("Bbb"));
    CHECK_FALSE(parseChord("F##m"));
    CHECK_FALSE(parseChord("C/Ebb"));
}

TEST_CASE(
    "every name a place produces has a letter, at most two accidentals and an octave the notation "
    "reads") {
    // The notation's rule for a pitch name: a letter, up to two `#` or `b`, an octave -1..9, the
    // result 0..127. The notation test compiles the same names.
    const std::vector<std::optional<Key>> keys = {std::nullopt, key("C", Scale::Major),
                                                  key("G#", Scale::Minor), key("Gb", Scale::Major),
                                                  key("C#", Scale::Chromatic)};
    for (const PitchClass& root : allRoots())
        for (const auto& [quality, intervals] : chordQualities()) {
            (void)intervals;
            const Chord c{root, quality, std::nullopt};
            for (const auto& k : keys)
                for (int pitch = 0; pitch < 128; ++pitch) {
                    const std::string text = spell(pitch, Place{k, c});
                    CAPTURE(text);
                    CAPTURE(printChord(c));
                    CAPTURE(pitch);
                    static constexpr int kSemitone[7] = {9, 11, 0, 2, 4, 5, 7};
                    REQUIRE(text.size() >= 2);
                    REQUIRE(text[0] >= 'A');
                    REQUIRE(text[0] <= 'G');
                    int semitone = kSemitone[text[0] - 'A'];
                    std::size_t i = 1;
                    int marks = 0;
                    for (; i < text.size() && (text[i] == '#' || text[i] == 'b'); ++i, ++marks)
                        semitone += text[i] == '#' ? 1 : -1;
                    CHECK(marks <= 2);
                    const int octave = std::stoi(text.substr(i));
                    CHECK(octave >= -1);
                    CHECK(octave <= 9);
                    CHECK((octave + 1) * 12 + semitone == pitch);
                }
        }
}
