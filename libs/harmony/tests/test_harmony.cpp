// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/harmony/harmony.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <set>
#include <sstream>

using namespace supple::harmony;

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
