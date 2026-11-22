// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/harmony/harmony.h>
#include <supple/notation/notation.h>

#include <doctest/doctest.h>
#include <supple/base/environment.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sstream>

using namespace supple::notation;

namespace {
std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string dump(const supple::midi::Sequence& sequence) {
    std::ostringstream out;
    out << "ppq " << sequence.ppq << " length " << sequence.length << '\n';
    for (const auto& n : sequence.notes)
        out << "note " << n.start << ' ' << n.length << ' ' << int(n.channel) << ' ' << int(n.pitch)
            << ' ' << int(n.velocity) << ' ' << int(n.offVelocity) << '\n';
    for (const auto& c : sequence.controls)
        out << "control " << c.tick << ' ' << int(c.kind) << ' ' << int(c.channel) << ' '
            << int(c.number) << ' ' << c.value << '\n';
    for (const auto& sx : sequence.sysex) {
        out << "sysex " << sx.tick;
        for (auto byte : sx.bytes) out << ' ' << int(byte);
        out << '\n';
    }
    return out.str();
}
}  // namespace

TEST_CASE("notation rejects overflowing positions and invalid compile contexts") {
    CompileContext context;
    context.ppq = 0;
    CHECK_FALSE(compile("voice a; 1: C4:q", context));
    CHECK_FALSE(compile("meter -4294967292/4; voice a; 1: C4:q"));
    CHECK_FALSE(compile("length 1+9223372036854775807t; voice a; 1: C4:q"));
    CHECK_FALSE(compile("lane cc 1 ch 1; 1.1+9223372036854775807t 80"));
}

TEST_CASE("notation lane ramps near the tick limit reject without overflowing") {
    const auto pickup = std::to_string(std::numeric_limits<std::int64_t>::max() - 1000);
    for (const auto shape : {"lin", "slow", "fast"}) {
        auto result =
            compile("pickup " + pickup + "t; lane cc 1 ch 1; 1.1 0; 1.2+30t 127 " + shape);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "notation.bad_duration");
    }
}

TEST_CASE("notation decompiler validates event ranges and every used meter") {
    supple::midi::Sequence sequence;
    sequence.length = 7680;
    SUBCASE("note end overflow") {
        sequence.notes = {{std::numeric_limits<std::int64_t>::max(), 1, 0, 60, 100, 0}};
        CHECK_FALSE(decompile(sequence));
    }
    SUBCASE("control kind") {
        sequence.controls = {{0, static_cast<supple::midi::ControlKind>(255), 0, 0, 1}};
        CHECK_FALSE(decompile(sequence));
    }
    SUBCASE("control value") {
        sequence.controls = {{0, supple::midi::ControlKind::CC, 0, 1, 128}};
        CHECK_FALSE(decompile(sequence));
    }
    SUBCASE("sysex framing") {
        sequence.sysex = {{0, {0xf0, 0x01}}};
        CHECK_FALSE(decompile(sequence));
    }
    SUBCASE("invalid later meter") {
        DecompileOptions options;
        options.meterAt = [](int bar) { return supple::midi::Meter{4, bar == 1 ? 4 : 0}; };
        CHECK_FALSE(decompile(sequence, options));
    }
}

TEST_CASE("notation duration sums require a term after every plus") {
    for (const auto duration : {"q+", "q+e+", "700t+"}) {
        auto result = compile(std::string("voice a; 1: C4:") + duration + " r:*");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "notation.bad_duration");
        CHECK_FALSE(result.error().hint.empty());
    }
    auto valid = compile("voice a; 1: C4:q+e r:*");
    REQUIRE(valid);
    REQUIRE(valid->sequence.notes.size() == 1);
    CHECK(valid->sequence.notes.front().length == 1440);
}

TEST_CASE("notation round trips the final supported bar boundary") {
    supple::midi::Sequence sequence;
    sequence.length = 100000LL * 3840;
    auto written = decompile(sequence);
    REQUIRE(written);
    auto compiled = compile(*written);
    REQUIRE(compiled);
    CHECK(compiled->sequence == sequence);
}

TEST_CASE("notation inline bars respect the supported bar limit") {
    REQUIRE(compile("voice a; 99999: r:* | r:*"));
    for (const auto text : {"voice a; 100000: r:* | r:*", "voice a; 100000: r:* | 100001: r:*"}) {
        auto result = compile(text);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "notation.syntax");
    }
}

TEST_CASE("valid notation corpus matches event listings") {
    const auto root = std::filesystem::path(NOTATION_CORPUS_DIR) / "valid";
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".note") continue;
        ++count;
        auto result = compile(readText(entry.path()));
        INFO(entry.path().string());
        std::string errorInfo;
        if (!result) {
            errorInfo = result.error().toString();
            for (const auto& detail : result.error().details) errorInfo += "\n" + detail.toString();
        }
        INFO(errorInfo);
        REQUIRE(result);
        const auto expectedPath =
            entry.path().parent_path() / (entry.path().stem().string() + ".events");
        const auto actual = dump(result->sequence);
        if (supple::environmentFlagEnabled("SUPPLE_UPDATE_NOTATION_GOLDENS")) {
            std::ofstream output(expectedPath);
            output << actual;
        } else
            CHECK(actual == readText(expectedPath));
    }
    CHECK(count >= 40);
}

TEST_CASE("invalid notation corpus has precise errors") {
    const auto root = std::filesystem::path(NOTATION_CORPUS_DIR) / "invalid";
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".note") continue;
        ++count;
        const auto expectPath =
            entry.path().parent_path() / (entry.path().stem().string() + ".expect");
        std::istringstream expectation(readText(expectPath));
        std::string code, hint;
        int line = 0, column = 0;
        expectation >> code >> line >> column >> hint;
        auto result = compile(readText(entry.path()));
        INFO(entry.path().string());
        REQUIRE_FALSE(result.ok());
        std::vector<supple::Error> errors;
        if (result.error().details.empty())
            errors.push_back(result.error());
        else
            errors = result.error().details;
        bool matched = false;
        for (const auto& e : errors)
            if (e.code == code && e.loc && e.loc->line == line && e.loc->column == column &&
                !e.hint.empty() && e.hint.find(hint) != std::string::npos)
                matched = true;
        CHECK(matched);
    }
    CHECK(count >= 30);
}

TEST_CASE("compile a simple voice") {
    auto result =
        compile("notation 1\nppq 960\nmeter 4/4\nvoice bass ch 1\n1: C4:q D4:q E4:q F4:q\n");
    REQUIRE(result);
    CHECK(result->sequence.notes.size() == 4);
    CHECK(result->sequence.length == 3840);
}

TEST_CASE("thirty-second duration is distinct from exact tick suffix") {
    auto result = compile("voice lead\n1: C4:t D4:120t r:*\n");
    REQUIRE(result);
    REQUIRE(result->sequence.notes.size() == 2);
    CHECK(result->sequence.notes[0].length == 120);
    CHECK(result->sequence.notes[1].length == 120);
}

TEST_CASE("implicit main voice matches an explicit declaration") {
    const auto implicit = compile("notation 1\n1: C4:q E4:q G4:h\n");
    const auto explicitVoice = compile("notation 1\nvoice main\n1: C4:q E4:q G4:h\n");
    REQUIRE(implicit);
    REQUIRE(explicitVoice);
    CHECK(implicit->sequence == explicitVoice->sequence);
    auto written = decompile(implicit->sequence);
    REQUIRE(written);
    CHECK(written->find("voice ") != std::string::npos);
}

TEST_CASE("numbered bar outside a voice suggests a voice declaration") {
    auto result = compile("lane cc 74\n1: C4:w\n");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().hint.find("voice NAME") != std::string::npos);
}

TEST_CASE("a bar number after a pipe is accepted when it is the next bar") {
    const std::string numbered =
        "notation 1\nmeter 3/4\n"
        "1: C4:q E4:q G4:q | 2: A4:q G4:q E4:q | 3: F4:q A4:q C5:q | 4: G4:q E4:q C4:q\n";
    const std::string byLine =
        "notation 1\nmeter 3/4\n"
        "1: C4:q E4:q G4:q\n2: A4:q G4:q E4:q\n3: F4:q A4:q C5:q\n4: G4:q E4:q C4:q\n";
    auto withNumbers = compile(numbered);
    REQUIRE(withNumbers);
    auto withNewlines = compile(byLine);
    REQUIRE(withNewlines);
    CHECK(withNumbers->sequence == withNewlines->sequence);
}

TEST_CASE("a pipe number that is not the next bar is an error") {
    auto result = compile("voice lead\n1: C4:w | 3: D4:w\n");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == "notation.bar_after_pipe");
    REQUIRE(result.error().loc.has_value());
    CHECK(result.error().loc->line == 2);
    CHECK(result.error().loc->column == 11);
    CHECK(result.error().message.find("Bar 3 follows a pipe where bar 2 comes next") !=
          std::string::npos);
    CHECK(result.error().hint.find("new line") != std::string::npos);
}

TEST_CASE("a pipe without a number still continues the next bar") {
    auto result = compile("voice lead\n1: C4:w | D4:w\n");
    REQUIRE(result);
    REQUIRE(result->sequence.notes.size() == 2);
    CHECK(result->sequence.length == 7680);
    auto numbered = compile("voice lead\n1: C4:w | 2: D4:w\n");
    REQUIRE(numbered);
    CHECK(numbered->sequence == result->sequence);
}

TEST_CASE("exact item length and repeated lane point") {
    auto result = compile("notation 1\nppq 960\nlength 0+1t\nlane cc 1\n1.1 10\n1.1 20\n");
    REQUIRE(result);
    CHECK(result->sequence.length == 1);
    CHECK(result->sequence.controls.size() == 2);
}

TEST_CASE("note round trip") {
    supple::midi::Sequence s;
    s.length = 900;
    s.notes = {{50, 800, 0, 60, 100, 20}, {100, 200, 0, 64, 70, 0}};
    auto written = decompile(s);
    REQUIRE(written);
    auto compiled = compile(*written);
    REQUIRE(compiled);
    s.canonicalize();
    CHECK(compiled->sequence == s);
}

TEST_CASE("decompiler emits grid and copy lines when exact") {
    supple::midi::Sequence drums;
    drums.length = 3840;
    drums.notes = {{0, 240, 9, 36, 100, 0}, {960, 240, 9, 36, 100, 0}};
    auto grid = decompile(drums);
    REQUIRE(grid);
    CHECK(grid->find("voice drums ch 10 grid s") != std::string::npos);
    auto compiledGrid = compile(*grid);
    REQUIRE(compiledGrid);
    drums.canonicalize();
    CHECK(compiledGrid->sequence == drums);

    supple::midi::Sequence repeated;
    repeated.length = 11520;
    repeated.notes = {
        {0, 3840, 0, 60, 96, 0}, {3840, 3840, 0, 60, 96, 0}, {7680, 3840, 0, 60, 96, 0}};
    auto copied = decompile(repeated);
    REQUIRE(copied);
    CHECK(copied->find("2-3 = 1") != std::string::npos);
    auto compiledCopy = compile(*copied);
    REQUIRE(compiledCopy);
    repeated.canonicalize();
    CHECK(compiledCopy->sequence == repeated);
}

TEST_CASE("same-tick lane writes retain order through decompile") {
    supple::midi::Sequence s;
    s.length = 1;
    s.controls = {{0, supple::midi::ControlKind::CC, 0, 74, 20},
                  {0, supple::midi::ControlKind::CC, 0, 74, 90}};
    auto written = decompile(s);
    REQUIRE(written);
    CHECK(written->find("1.1 20\n1.1 90") != std::string::npos);
    auto compiled = compile(*written);
    REQUIRE(compiled);
    CHECK(compiled->sequence == s);
}

TEST_CASE("fractional denominator beats retain integer tick controls") {
    for (int ppq : {1, 3}) {
        supple::midi::Sequence s;
        s.ppq = ppq;
        s.length = ppq * 3;
        s.controls = {{2, supple::midi::ControlKind::CC, 0, 1, 80}};
        DecompileOptions options;
        options.meterAt = [](int) { return supple::midi::Meter{6, 8}; };
        CompileContext context;
        context.meterAt = options.meterAt;
        auto written = decompile(s, options);
        REQUIRE(written);
        CHECK(written->find("1.1+2t 80") != std::string::npos);
        auto compiled = compile(*written, context);
        REQUIRE(compiled);
        CHECK(compiled->sequence == s);
    }
}

TEST_CASE("round-trip random canonical sequences") {
    constexpr std::uint32_t seed = 0x5a17c0de;
    std::mt19937 rng(seed);
    const int ppqs[] = {96, 480, 960, 1920};
    const supple::midi::Meter meters[] = {{4, 4}, {3, 4}, {6, 8}, {5, 8}};
    for (int iteration = 0; iteration < 2000; ++iteration) {
        supple::midi::Sequence s;
        s.ppq = ppqs[rng() % 4];
        const auto meter = meters[rng() % 4];
        const auto span = s.ppq * 4 * meter.numerator / meter.denominator;
        const int noteCount = rng() % 9;
        for (int i = 0; i < noteCount; ++i)
            s.notes.push_back({static_cast<std::int64_t>(rng() % (span * 3)),
                               static_cast<std::int64_t>(rng() % (span * 2)),
                               static_cast<std::uint8_t>(rng() % 16),
                               static_cast<std::uint8_t>(rng() % 128),
                               static_cast<std::uint8_t>(1 + rng() % 127),
                               static_cast<std::uint8_t>(rng() % 128)});
        const int controlCount = rng() % 9;
        for (int i = 0; i < controlCount; ++i) {
            const auto kind = static_cast<supple::midi::ControlKind>(rng() % 5);
            s.controls.push_back(
                {static_cast<std::int64_t>(rng() % (span * 3)), kind,
                 static_cast<std::uint8_t>(rng() % 16),
                 static_cast<std::uint8_t>(kind == supple::midi::ControlKind::CC ||
                                                   kind == supple::midi::ControlKind::PolyPressure
                                               ? rng() % 128
                                               : 0),
                 kind == supple::midi::ControlKind::PitchBend
                     ? static_cast<int>(rng() % 16384) - 8192
                     : static_cast<int>(rng() % 128)});
        }
        if (rng() % 4 == 0)
            s.sysex.push_back({static_cast<std::int64_t>(rng() % (span * 3)),
                               {0xf0, 0x7e, static_cast<std::uint8_t>(rng() % 128), 0xf7}});
        std::int64_t end = 0;
        for (const auto& n : s.notes) end = std::max(end, n.start + n.length);
        for (const auto& c : s.controls) end = std::max(end, c.tick);
        for (const auto& x : s.sysex) end = std::max(end, x.tick);
        s.length = end + static_cast<std::int64_t>(rng() % (span + 1));
        s.canonicalize();
        DecompileOptions options;
        options.meterAt = [meter](int) { return meter; };
        CompileContext context;
        context.meterAt = options.meterAt;
        auto text = decompile(s, options);
        INFO("seed=", seed, " iteration=", iteration);
        REQUIRE(text);
        auto compiled = compile(*text, context);
        INFO(*text);
        std::string errorInfo;
        if (!compiled) {
            errorInfo = compiled.error().toString();
            for (const auto& detail : compiled.error().details)
                errorInfo += "\n" + detail.toString();
        }
        INFO(errorInfo);
        REQUIRE(compiled);
        CHECK(compiled->sequence == s);
    }
}

TEST_CASE("notation indexes long bar timelines without repeated meter walks") {
    supple::midi::Sequence sequence;
    sequence.length = 2000 * 3840;
    for (int bar = 0; bar < 2000; ++bar) sequence.notes.push_back({bar * 3840, 960, 0, 60, 100, 0});
    int calls = 0;
    DecompileOptions options;
    options.meterAt = [&](int) {
        ++calls;
        return supple::midi::Meter{4, 4};
    };
    auto written = decompile(sequence, options);
    REQUIRE(written);
    CHECK(calls <= 2002);
    CompileContext context;
    context.meterAt = [](int) { return supple::midi::Meter{4, 4}; };
    auto roundtrip = compile(*written, context);
    REQUIRE(roundtrip);
    CHECK(roundtrip->sequence == sequence);
}

TEST_CASE("chord symbols compile to the pitches harmony voices") {
    for (const auto& [quality, intervals] : supple::harmony::chordQualities())
        for (const std::string bass : {"", "/E", "/Bb"}) {
            const std::string symbol = "C" + quality + bass;
            const auto compiled = compile("voice a; 1: {" + symbol + "}:w");
            REQUIRE_MESSAGE(compiled, symbol);
            std::vector<int> got;
            for (const auto& n : compiled->sequence.notes) got.push_back(n.pitch);
            std::sort(got.begin(), got.end());
            const auto chord = supple::harmony::parseChord(symbol);
            REQUIRE(chord);
            auto want = supple::harmony::chordPitches(*chord, 3);
            REQUIRE(want);
            std::sort(want->begin(), want->end());
            CHECK_MESSAGE(got == *want, symbol);
        }
}

TEST_CASE("chord symbol errors keep the compiler's codes") {
    auto codes = [](const std::string& text) {
        std::vector<std::string> out;
        const auto result = compile(text);
        if (result) return out;
        if (result.error().details.empty()) out.push_back(result.error().code);
        for (const auto& e : result.error().details) out.push_back(e.code);
        return out;
    };
    CHECK(codes("voice a; 1: {H7}:w") == std::vector<std::string>{"notation.bad_pitch"});
    CHECK(codes("voice a; 1: {Cmajj7}:w") == std::vector<std::string>{"notation.unknown_chord"});
    CHECK(codes("voice a; 1: {C/H}:w") == std::vector<std::string>{"notation.bad_pitch"});
    CHECK(codes("voice a octave 8; 1: {C13}:w") ==
          std::vector<std::string>{"notation.pitch_range"});
}

TEST_CASE("decompile spells pitches by the key it is given") {
    supple::midi::Sequence sequence;
    sequence.ppq = 960;
    sequence.length = 3840;
    sequence.notes = {{0, 960, 0, 70, 100, 0}, {960, 960, 0, 61, 100, 0}};  // A#4/Bb4, C#4
    const auto plain = decompile(sequence);
    REQUIRE(plain);
    CHECK(plain->find("Bb4") != std::string::npos);  // no key: the default table, as always
    CHECK(plain->find("C#4") != std::string::npos);
    supple::notation::DecompileOptions options;
    options.key =
        supple::harmony::Key{*supple::harmony::parsePitchClass("F"), supple::harmony::Scale::Major};
    const auto inF = decompile(sequence, options);
    REQUIRE(inF);
    CHECK(inF->find("Bb4") != std::string::npos);
    CHECK(inF->find("Db4") != std::string::npos);  // one flat: out-of-scale notes take flats
    options.key =
        supple::harmony::Key{*supple::harmony::parsePitchClass("A"), supple::harmony::Scale::Major};
    const auto inA = decompile(sequence, options);
    REQUIRE(inA);
    CHECK(inA->find("A#4") != std::string::npos);
    CHECK(inA->find("C#4") != std::string::npos);
    // What it prints compiles back to the same notes in every spelling.
    const auto back = compile(*inF);
    REQUIRE(back);
    CHECK(back->sequence.notes.size() == 2);
    CHECK(back->sequence.notes[0].pitch == 70);
}

namespace {
std::string chordsOf(const Compiled& c) {
    std::string out;
    for (const auto& s : c.chords)
        out += std::to_string(s.tick) + " " + s.voice + " " + supple::harmony::printChord(s.chord) +
               "\n";
    return out;
}
}  // namespace

TEST_CASE("notation hands over the chord symbols as compiled (ADR-0098)") {
    SUBCASE("two voices, a tie, a slash chord") {
        const auto c = compile(
            "voice bass\n1: {Cm7}:h~ {Cm7}:h\n2: {F/A}:w\nvoice lead\n1: {Cm7}:w\n2: C4:w\n");
        REQUIRE(c);
        // The tied continuation adds none; at tick 0 the earlier-declared voice is first.
        CHECK(chordsOf(*c) == "0 bass Cm7\n0 lead Cm7\n3840 bass F/A\n");
    }
    SUBCASE("a text with no voice line has one implicit voice") {
        const auto c = compile("1: {C}:h {G7}:h\n");
        REQUIRE(c);
        CHECK(chordsOf(*c) == "0 main C\n1920 main G7\n");
    }
    SUBCASE("a bad quality fails the compile, so no entries are handed over") {
        const auto c = compile("voice bass\n1: {Czz}:w\n");
        REQUIRE_FALSE(c);
        CHECK(c.error().code == "notation.unknown_chord");
    }
    SUBCASE("a copy line gives entries for the bars it repeats") {
        // A bar range (2-4: ...) is for grid voices only, which hold no chord symbols.
        const auto c = compile("voice a\n1: {C}:w\n2: {G}:w\n3-4 = 1-2\n5: {F}:w\n6-7 = 5\n");
        REQUIRE(c);
        CHECK(chordsOf(*c) ==
              "0 a C\n3840 a G\n7680 a C\n11520 a G\n15360 a F\n19200 a F\n"
              "23040 a F\n");
    }
    SUBCASE("declaration order decides a tie, whatever order the events were written in") {
        const auto c = compile("voice first\n2: {Am}:w\nvoice second\n1: {C}:w\n2: {Dm}:w\n");
        REQUIRE(c);
        CHECK(chordsOf(*c) == "0 second C\n3840 first Am\n3840 second Dm\n");
    }
    SUBCASE("notes, length and the other members are as before") {
        const auto withSymbol = compile("voice a\n1: {Cm7}:w\n");
        const auto plain = compile("voice a\n1: [C3 Eb3 G3 Bb3]:w\n");
        REQUIRE(withSymbol);
        REQUIRE(plain);
        CHECK(dump(withSymbol->sequence) == dump(plain->sequence));
        CHECK(plain->chords.empty());
    }
}

TEST_CASE("every name harmony::spell produces for a chord place compiles back to its pitch") {
    // For every root spelling, every quality, a few keys and every pitch 0..127: the name is
    // one note of a bar and compiles to that pitch (doubles, `B#` and the octave edges included).
    const std::vector<std::optional<supple::harmony::Key>> keys = {
        std::nullopt, supple::harmony::Key{{'C', 0}, supple::harmony::Scale::Major},
        supple::harmony::Key{{'G', 1}, supple::harmony::Scale::Minor},
        supple::harmony::Key{{'G', -1}, supple::harmony::Scale::Major},
        supple::harmony::Key{{'C', 1}, supple::harmony::Scale::Chromatic}};
    std::set<std::pair<int, std::string>> seen;
    for (const char letter : {'A', 'B', 'C', 'D', 'E', 'F', 'G'})
        for (const int accidental : {-1, 0, 1})
            for (const auto& [quality, intervals] : supple::harmony::chordQualities()) {
                (void)intervals;
                const supple::harmony::Chord chord{{letter, accidental}, quality, std::nullopt};
                for (const auto& key : keys)
                    for (int pitch = 0; pitch < 128; ++pitch)
                        seen.insert({pitch, supple::harmony::spell(
                                                pitch, supple::harmony::Place{key, chord})});
            }
    CHECK(seen.size() > 128);
    for (const auto& [pitch, name] : seen) {
        CAPTURE(name);
        const auto compiled = compile("voice a\n1: " + name + ":q r:*\n");
        REQUIRE(compiled);
        REQUIRE(compiled->sequence.notes.size() == 1);
        CHECK(compiled->sequence.notes[0].pitch == pitch);
    }
}
