// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The context track's commands and what agents read of it (ADR-0095).
#include <doctest/doctest.h>

#include <unistd.h>
#include <filesystem>

#include <supple/engine/app.h>
#include <supple/model/context.h>
#include <supple/model/schemas.h>

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

struct Workdir {
    fs::path path =
        fs::temp_directory_path() / ("supple-engine-context-" + std::to_string(::getpid()));
    Workdir() {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Workdir() { fs::remove_all(path); }
};

std::unique_ptr<EngineApp> makeApp(const fs::path& path) {
    AppOptions options;
    options.workdir = path;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto app = EngineApp::create(options, {.console9 = false});
    REQUIRE(app);
    return std::move(*app);
}

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "context", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }

struct Bus {
    Workdir dir;
    std::unique_ptr<EngineApp> app = makeApp(dir.path);
    cmd::Registry registry;
    Bus() { registerEngineCommands(registry); }
    cmd::Dispatcher bus{registry, *app};
    json::Value run(std::string name, std::string_view args = "{}") {
        auto result = bus.execute({std::move(name), j(args), kOrigin});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    }
    Error fail(std::string name, std::string_view args) {
        auto result = bus.execute({std::move(name), j(args), kOrigin});
        REQUIRE_FALSE(result);
        return result.error();
    }
    std::string describe() { return run("session.describe")["text"].asString(); }
};

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("key.set and chord.set add, replace, move and refuse") {
    Bus b;
    auto key = b.run("key.set", R"({"at":0,"tonic":"eb","scale":"major"})");
    CHECK(key["id"].asString() == "cx_1");
    const auto* point = b.app->head().context.find({model::Kind::Context, 1});
    REQUIRE(point);
    CHECK(point->tonic == "Eb");  // written canonically
    CHECK(point->scale == "major");
    // The same beat replaces and keeps the id; a part of a point changes only that part.
    CHECK(b.run("key.set", R"({"at":0,"scale":"minor"})")["id"].asString() == "cx_1");
    CHECK(b.app->head().context.find({model::Kind::Context, 1})->tonic == "Eb");
    CHECK(b.app->head().context.find({model::Kind::Context, 1})->scale == "minor");
    CHECK(b.app->head().context.size() == 1);
    // Positions round to 1/960 beat.
    CHECK(b.run("chord.set", R"({"at":3.9996,"symbol":"Cmin7"})")["id"].asString() == "cx_2");
    const auto* chord = b.app->head().context.find({model::Kind::Context, 2});
    CHECK(chord->beat == doctest::Approx(4.0));
    CHECK(chord->symbol == "Cm7");  // canonical quality
    // `to` moves a point and keeps its id; an occupied beat is refused.
    CHECK(b.run("chord.set", R"({"at":4,"to":8})")["id"].asString() == "cx_2");
    CHECK(b.app->head().context.find({model::Kind::Context, 2})->beat == 8);
    CHECK(b.app->head().context.find({model::Kind::Context, 2})->symbol == "Cm7");
    b.run("chord.set", R"({"at":0,"symbol":"G"})");
    const auto occupied = b.fail("chord.set", R"({"at":8,"to":0})");
    CHECK(occupied.code == "context.occupied");
    CHECK_FALSE(occupied.hint.empty());
    CHECK(b.fail("chord.set", R"({"at":50,"to":60})").code == "context.no_point");
    // "" ends the chord from here on.
    b.run("chord.set", R"({"at":16,"symbol":""})");
    CHECK(b.app->head().context.find({model::Kind::Context, 4})->symbol.empty());
    // Removal.
    b.run("chord.remove", R"({"at":16})");
    CHECK(b.fail("chord.remove", R"({"at":16})").code == "context.no_point");
    b.run("key.remove", R"({"at":0})");
    CHECK(b.fail("key.remove", R"({"at":0})").code == "context.no_point");
}

TEST_CASE("key.set and chord.set refuse what the harmony library does not take, with a hint") {
    Bus b;
    auto tonic = b.fail("key.set", R"({"at":0,"tonic":"H","scale":"major"})");
    CHECK(tonic.code == "key.tonic");
    CHECK(tonic.path == "/args/tonic");
    auto scale = b.fail("key.set", R"({"at":0,"tonic":"C","scale":"whole_tone"})");
    CHECK(scale.code == "scale.unknown");
    CHECK(scale.hint.find("harmonic_minor") != std::string::npos);
    auto unsupported = b.fail("key.set", R"({"at":0,"tonic":"G#","scale":"major"})");
    CHECK(unsupported.code == "key.unsupported");
    CHECK(unsupported.hint.find("Ab major") != std::string::npos);
    CHECK(b.fail("key.set", R"({"at":0,"tonic":"C"})").code == "key.args");
    auto quality = b.fail("chord.set", R"({"at":0,"symbol":"Cm11"})");
    CHECK(quality.code == "chord.unknown_quality");
    CHECK(quality.hint.find("maj7") != std::string::npos);
    CHECK(b.fail("chord.set", R"({"at":0,"symbol":"H7"})").code == "chord.bad_root");
    CHECK(b.fail("chord.set", R"({"at":0,"symbol":"C/H"})").code == "chord.bad_bass");
    CHECK(b.fail("chord.set", R"({"at":0})").code == "chord.args");
    CHECK(b.app->head().context.empty());  // nothing was added
}

TEST_CASE("context.at and context.get say what holds") {
    Bus b;
    b.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    b.run("key.set", R"({"at":16,"tonic":"C","scale":"harmonic_minor"})");
    b.run("chord.set", R"({"at":0,"symbol":"Ebmaj7"})");
    b.run("chord.set", R"({"at":4,"symbol":"Cm7"})");
    b.run("chord.set", R"({"at":8,"symbol":""})");
    b.run("region.add", R"({"start":0,"end":16,"name":"Verse"})");
    b.run("region.add", R"({"start":4,"end":8,"name":"Stems","section":false})");
    auto at = b.run("context.at", R"({"bar":2})");
    CHECK(at["beat"].asDouble() == 4);
    CHECK(at["bar"].asInt() == 2);
    CHECK(at["key"]["tonic"].asString() == "Eb");
    CHECK(at["key"]["scale"].asString() == "major");
    CHECK(at["chord"]["symbol"].asString() == "Cm7");
    CHECK(at["chord"]["from_beat"].asDouble() == 4);
    CHECK(at["section"]["name"].asString() == "Verse");  // the not-a-section region is passed over
    auto later = b.run("context.at", R"({"at":20})");
    CHECK(later["key"]["scale"].asString() == "harmonic_minor");
    CHECK_FALSE(later.find("chord"));    // "no chord from here"
    CHECK_FALSE(later.find("section"));  // a gap
    CHECK(b.fail("context.at", "{}").code == "context.args");
    CHECK(b.fail("context.at", R"({"at":1,"bar":1})").code == "context.args");
    auto all = b.run("context.get", "{}");
    REQUIRE(all["points"].size() == 5);
    CHECK(all["points"].at(0).find("beat")->asDouble() == 0);
    CHECK(all["points"].at(4).find("beat")->asDouble() == 16);
    CHECK(all["key"]["tonic"].asString() == "Eb");
    auto range = b.run("context.get", R"({"from":4,"to":9})");
    CHECK(range["points"].size() == 2);
    CHECK(range["chord"]["symbol"].asString() == "Cm7");  // what holds at `from`
}

TEST_CASE("a point this build cannot read is listed and says known false") {
    Bus b;
    auto tx = b.app->beginTransaction(kOrigin, "later build's points");
    REQUIRE(tx);
    model::ContextPoint chord;
    chord.type = model::ContextType::Chord;
    chord.symbol = "Cm11";
    b.app->txn().create(chord);
    model::ContextPoint key;
    key.tonic = "C";
    key.scale = "whole_tone";
    b.app->txn().create(key);
    REQUIRE(b.app->commitTransaction(*tx));
    auto at = b.run("context.at", R"({"at":1})");
    CHECK(at["chord"]["symbol"].asString() == "Cm11");
    CHECK_FALSE(at["chord"]["known"].asBool());
    CHECK_FALSE(at["key"]["known"].asBool());
    const auto text = b.describe();
    CHECK(has(text, "Cm11?"));
    CHECK(has(text, "C whole tone?"));
    // Changing the key's tonic alone cannot make its unknown scale known.
    CHECK(b.fail("key.set", R"({"at":0,"tonic":"D"})").code == "scale.unknown");
}

TEST_CASE("session.describe names the key, the context, the later tempo and meter changes") {
    Bus b;
    const auto plain = b.describe();
    CHECK_FALSE(has(plain, "context:"));
    CHECK_FALSE(has(plain, "tempo:"));
    CHECK_FALSE(has(plain, "meters:"));
    b.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    b.run("key.set", R"({"at":16,"tonic":"C","scale":"harmonic_minor"})");
    b.run("chord.set", R"({"at":0,"symbol":"Ebmaj7"})");
    b.run("chord.set", R"({"at":4,"symbol":"Cm7"})");
    b.run("chord.set", R"({"at":8,"symbol":""})");
    b.run("region.add", R"({"start":0,"end":4,"name":"Stems","section":false})");
    b.run("tempo.set", R"({"points":[{"beat":0,"bpm":120},{"beat":32,"bpm":100,"ramp":true},
                                      {"beat":64,"bpm":140}]})");
    b.run("timesig.set", R"({"bar":17,"meter":"3/4"})");
    const auto text = b.describe();
    CHECK(has(text, "120 BPM 4/4"));
    CHECK(has(text, "· Eb major\n"));  // the header
    CHECK(has(text, "tempo: bar 9 100 BPM (ramps on) · bar 17 140 BPM\n"));
    CHECK(has(text, "meters: bar 17 3/4\n"));
    CHECK(has(text, "context:\n  keys: bar 1 Eb major · bar 5 C harmonic minor\n"));
    CHECK(has(text, "  chords: bar 1 Ebmaj7 · bar 2 Cm7 · bar 3 (no chord)\n"));
    CHECK(has(text, "(not a section)"));
}

TEST_CASE("midi.get spells by the key at the item and lists the key and chords over it") {
    Bus b;
    b.run("track.add", R"({"name":"Keys"})");
    b.run("item.add",
          R"({"track":"tr_1","bar":3,"notation":"voice a; 1: Bb3:q Db4:q F4:h; 2: C4:w"})");
    auto none = b.run("midi.get", R"({"item":"it_1"})");
    CHECK(has(none["text"].asString(), "Bb3"));
    CHECK(has(none["text"].asString(), "C#4"));  // no key: the default table
    CHECK_FALSE(none.find("context"));
    b.run("key.set", R"({"at":0,"tonic":"F","scale":"major"})");
    b.run("chord.set", R"({"at":8,"symbol":"Bbmaj7"})");  // bar 3: the item's first bar
    b.run("chord.set", R"({"at":12,"symbol":"C7"})");     // bar 4: its second
    b.run("chord.set", R"({"at":40,"symbol":"F"})");      // after the item
    auto inF = b.run("midi.get", R"({"item":"it_1"})");
    CHECK(has(inF["text"].asString(), "Db4"));  // out of F major: one flat, flats
    auto ctx = *inF.find("context");
    REQUIRE(ctx["keys"].size() == 1);
    CHECK(ctx["keys"].at(0).find("what")->asString() == "F major");
    CHECK(ctx["keys"].at(0).find("item_bar")->asInt() == 1);
    CHECK(ctx["keys"].at(0).find("project_bar")->asInt() == 3);
    REQUIRE(ctx["chords"].size() == 2);
    CHECK(ctx["chords"].at(0).find("what")->asString() == "Bbmaj7");
    CHECK(ctx["chords"].at(0).find("item_bar")->asInt() == 1);
    CHECK(ctx["chords"].at(0).find("project_bar")->asInt() == 3);
    CHECK(ctx["chords"].at(1).find("what")->asString() == "C7");
    CHECK(ctx["chords"].at(1).find("item_bar")->asInt() == 2);
    CHECK(ctx["chords"].at(1).find("project_bar")->asInt() == 4);
    // In a sharp key the same pitch is spelled with a sharp, and the text compiles back.
    b.run("key.set", R"({"at":0,"tonic":"D","scale":"major"})");
    auto inD = b.run("midi.get", R"({"item":"it_1"})");
    CHECK(has(inD["text"].asString(), "A#3"));
    CHECK(has(inD["text"].asString(), "C#4"));
    b.run("midi.write", R"({"item":"it_1","notation":)" + json::writeCompact(inD["text"]) + "}");
    CHECK(b.run("midi.get", R"({"item":"it_1"})")["text"].asString() == inD["text"].asString());
}

TEST_CASE("context points undo and merge by command replay") {
    Bus b;
    b.run("chord.set", R"({"at":0,"symbol":"C"})");
    b.run("history.undo");
    CHECK(b.app->head().context.empty());
    b.run("history.redo");
    CHECK(b.app->head().context.size() == 1);
    // Two branches that add points at different beats merge, ids remapped.
    b.run("branch.create", R"({"name":"agent/test/chords"})");
    b.run("chord.set", R"({"at":4,"symbol":"F"})");
    b.run("chord.set", R"({"at":8,"symbol":"Am"})");
    b.run("key.set", R"({"at":0,"tonic":"C","scale":"major"})");
    b.run("branch.switch", R"({"name":"main"})");
    b.run("chord.set", R"({"at":12,"symbol":"G7"})");
    b.run("branch.merge", R"({"from":"agent/test/chords"})");
    const auto& head = b.app->head();
    CHECK(head.context.size() == 5);  // C, F, Am, G7 and the key
    CHECK(model::keyAt(head, 0));
    CHECK(harmony::printChord(*model::chordAt(head, 4)) == "F");
    CHECK(harmony::printChord(*model::chordAt(head, 8)) == "Am");
    CHECK(harmony::printChord(*model::chordAt(head, 12)) == "G7");
    // The same beat set on both sides is a conflict, never a second point at the beat.
    b.run("branch.create", R"({"name":"agent/test/same"})");
    b.run("chord.set", R"({"at":16,"symbol":"Dm"})");
    b.run("branch.switch", R"({"name":"main"})");
    b.run("chord.set", R"({"at":16,"symbol":"Em"})");
    const auto size = b.app->head().context.size();
    CHECK(b.fail("branch.merge", R"({"from":"agent/test/same"})").code == "branch.conflict");
    CHECK(b.app->head().context.size() == size);
    CHECK(harmony::printChord(*model::chordAt(b.app->head(), 16)) == "Em");
}

TEST_CASE("region.add and region.set take section, and region.list shows it") {
    Bus b;
    b.run("region.add", R"({"start":0,"end":4,"name":"Verse"})");
    b.run("region.add", R"({"start":0,"end":4,"name":"Stems","section":false})");
    CHECK(b.app->head().regions.find({model::Kind::Region, 1})->section);
    CHECK_FALSE(b.app->head().regions.find({model::Kind::Region, 2})->section);
    b.run("region.set", R"({"id":"rg_1","section":false})");
    CHECK_FALSE(b.app->head().regions.find({model::Kind::Region, 1})->section);
    auto list = b.run("region.list");
    CHECK_FALSE(list["regions"].at(0).find("section")->asBool());
    auto markers = b.run("marker.add", R"({"pos":1,"name":"m"})");
    CHECK_FALSE(b.run("marker.list")["markers"].at(0).find("section"));
}

// ---------------------------------------------------------------------------------------
// The lead sheet (ADR-0095): context.get's text and context.write.

namespace {
const char* const kSong =
    "1: key Eb major {Ebmaj7}:w\n"
    "2: {Cm7}:h {Fm7}:h\n"
    "3: ~:w\n"
    "4: {Bb7}:h r:h\n";

void seedSong(Bus& b) {
    b.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    b.run("chord.set", R"({"at":0,"symbol":"Ebmaj7"})");
    b.run("chord.set", R"({"at":4,"symbol":"Cm7"})");
    b.run("chord.set", R"({"at":6,"symbol":"Fm7"})");
    b.run("chord.set", R"({"at":12,"symbol":"Bb7"})");
    b.run("chord.set", R"({"at":14,"symbol":""})");
}

std::string sheet(Bus& b, std::string_view args = "{}") {
    return b.run("context.get", args)["text"].asString();
}

json::Value writeArgs(const std::string& text) {
    json::Value args = json::Value::object();
    args["text"] = text;
    return args;
}

Result<json::Value> write(Bus& b, const std::string& text) {
    return b.bus.execute({"context.write", writeArgs(text), kOrigin});
}

std::string commandsOfLastStep(Bus& b) {
    const auto* v = b.app->versions().history(1).front();
    std::string out;
    for (const auto& inv : v->txn.invocations)
        out += (out.empty() ? "" : " ") + inv.command + json::writeCompact(inv.args);
    return out;
}
}  // namespace

TEST_CASE("context.get prints the lead sheet one line per bar, and no text when there is nothing") {
    Bus b;
    CHECK_FALSE(b.run("context.get").find("text"));
    seedSong(b);
    CHECK(sheet(b) == kSong);
    // A range: the bar holding `from` to the bar holding `to`, exclusive at a bar line.
    CHECK(sheet(b, R"({"from":4,"to":8})") == "2: {Cm7}:h {Fm7}:h\n");
    CHECK(sheet(b, R"({"from":5,"to":9})") == "2: {Cm7}:h {Fm7}:h\n3: ~:w\n");
    // A bar mid-hold starts with a hold; a range of bars with no points still prints.
    CHECK(sheet(b, R"({"from":13,"to":15})") == "4: {Bb7}:h r:h\n");
    CHECK(sheet(b, R"({"from":40,"to":41})") == "11: ~:w\n");
    CHECK(b.fail("context.get", R"({"from":0,"to":100000})").code == "context.too_long");
}

TEST_CASE("the lead sheet follows the meter and prints the shortest durations") {
    Bus b;
    b.run("timesig.set", R"({"bar":2,"meter":"3/4"})");
    b.run("timesig.set", R"({"bar":3,"meter":"7/8"})");
    b.run("chord.set", R"({"at":0,"symbol":"C"})");
    b.run("chord.set", R"({"at":5,"symbol":"G"})");     // bar 2, beat 2
    b.run("chord.set", R"({"at":7.5,"symbol":"Am"})");  // bar 3 starts at beat 7
    b.run("chord.set", R"({"at":8.25,"symbol":"F"})");
    b.run("key.set", R"({"at":8.25,"tonic":"A","scale":"minor"})");
    const auto text = sheet(b);
    CHECK(text ==
          "1: {C}:w\n"
          "2: ~:q {G}:h\n"
          "3: ~:e {Am}:e. key A minor {F}:h+s\n");
    // And reads back the same, bar lengths from the same meters.
    const auto before = b.app->head().version;
    auto result = write(b, text);
    REQUIRE(result);
    CHECK((*result)["unchanged"].asInt() == 5);
    CHECK((*result)["added"].asInt() + (*result)["replaced"].asInt() +
              (*result)["removed"].asInt() ==
          0);
    CHECK(b.app->head().version == before);  // nothing changed: no step
    CHECK(sheet(b) == text);
}

TEST_CASE("sharps are not comments, and a comment ends the line") {
    Bus b;
    REQUIRE(write(b, "1: key F# minor {F#m7}:h {C#7#9/E#}:h # a comment {X}\n"));
    CHECK(sheet(b) == "1: key F# minor {F#m7}:h {C#7#9/E#}:h\n");
    CHECK(b.app->head().context.size() == 3);
}

TEST_CASE("context.write is a diff: equal points keep their ids, omitted ones go") {
    Bus b;
    seedSong(b);
    const auto idAt = [&](double beat, const char* type) {
        for (const auto& e : b.app->head().context)
            if (e.value->beat == beat &&
                e.value->type == (std::string_view(type) == "key" ? model::ContextType::Key
                                                                  : model::ContextType::Chord))
                return e.id;
        return model::Id{};
    };
    const auto cm7 = idAt(4, "chord"), bb7 = idAt(12, "chord");
    REQUIRE(cm7.n != 0);
    // Rewrite bars 2 to 4 (a comment, a blank line and CRLF are fine): Fm7 becomes Fm,
    // Cm7 stays; bar 4 moves its rest a beat later. Bar 1 is not named: left alone.
    auto result = write(b,
                        "2: {Cm7}:h {Fm}:h\n"
                        "# bar 3 holds: no points of its own\n"
                        "3: ~:w\r\n"
                        "\n"
                        "4: {Bb7}:h. r:q\n");
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    CHECK((*result)["first_bar"].asInt() == 2);
    CHECK((*result)["last_bar"].asInt() == 4);
    CHECK((*result)["unchanged"].asInt() == 2);  // Cm7 and Bb7
    CHECK((*result)["replaced"].asInt() == 1);   // Fm7 -> Fm
    CHECK((*result)["added"].asInt() == 1);      // the rest at beat 15
    CHECK((*result)["removed"].asInt() == 1);    // the rest at beat 14
    CHECK(idAt(4, "chord") == cm7);
    CHECK(idAt(12, "chord") == bb7);
    CHECK(idAt(0, "chord").n != 0);  // bar 1 untouched
    CHECK(idAt(0, "key").n != 0);
    CHECK(sheet(b) ==
          "1: key Eb major {Ebmaj7}:w\n"
          "2: {Cm7}:h {Fm}:h\n"
          "3: ~:w\n"
          "4: {Bb7}:h. r:q\n");
}

TEST_CASE("context.write is recorded as the points it resolved to, and branches merge") {
    Bus b;
    seedSong(b);
    // One step, recorded as the operations (removes first, then the sets), not the text.
    REQUIRE(write(b, "2: {Cm7}:h {Fm}:h\n3: {Dm}:w\n"));
    const std::string recorded = commandsOfLastStep(b);
    CHECK(recorded == R"(chord.set{"at":6.0,"symbol":"Fm"} chord.set{"at":8.0,"symbol":"Dm"})");
    CHECK_FALSE(has(recorded, "context.write"));
    b.run("history.undo");
    CHECK(sheet(b) == kSong);
    // Undo and redo go through the model's changes; the points come back with their ids.
    b.run("history.redo");
    CHECK(has(sheet(b), "3: {Dm}:w"));
    // Removes are recorded before sets.
    REQUIRE(write(b, "4: {Am}:w\n"));
    CHECK(commandsOfLastStep(b) == R"(chord.remove{"at":14.0} chord.set{"at":12.0,"symbol":"Am"})");

    // Two branches write different bars; the merge keeps both.
    Bus m;
    seedSong(m);
    m.run("branch.create", R"({"name":"agent/test/lead"})");
    REQUIRE(write(m, "2: {Cm7}:h {Fm}:h\n"));
    m.run("branch.switch", R"({"name":"main"})");
    REQUIRE(write(m, "4: {Bb7}:h {Ab}:h\n"));
    m.run("branch.merge", R"({"from":"agent/test/lead"})");
    CHECK(sheet(m) ==
          "1: key Eb major {Ebmaj7}:w\n"
          "2: {Cm7}:h {Fm}:h\n"
          "3: ~:w\n"
          "4: {Bb7}:h {Ab}:h\n");
}

TEST_CASE("context.write refuses what is wrong, naming the bar, and changes nothing") {
    Bus b;
    seedSong(b);
    const auto before = b.app->head().version;
    auto refuses = [&](const std::string& text, const char* code, const char* in = "") {
        auto result = write(b, text);
        REQUIRE_FALSE(result);
        CHECK_MESSAGE(result.error().code == code, text << " -> " << result.error().toString());
        CHECK(result.error().path == "/args/text");
        CHECK_FALSE(result.error().hint.empty());
        CHECK_MESSAGE(result.error().message.find(in) != std::string::npos,
                      text << " -> " << result.error().message);
        CHECK(b.app->head().version == before);
    };
    refuses("", "context.empty");
    refuses("# only a comment\n\n", "context.empty");
    refuses("{C}:w\n", "context.syntax");
    refuses("0: {C}:w\n", "context.syntax");
    refuses("2: {C}:h\n", "context.bar_length", "bar 2 holds 1920");  // short
    refuses("2: {C}:w {G}:q\n", "context.bar_length", "bar 2");       // long
    refuses("2: {C}:w\n4: {C}:w\n", "context.bar_order", "bar 4 follows bar 2");
    refuses("2: {C}:w\n2: {C}:w\n", "context.bar_order");
    refuses("2: {Cm11}:w\n", "chord.unknown_quality", "bar 2");
    refuses("2: {H7}:w\n", "chord.bad_root", "bar 2");
    refuses("2: Cm7:w\n", "context.syntax", "Cm7:w");
    refuses("2: {Cm7}:z\n", "context.syntax", "bar 2");
    refuses("2: {Cm7}:\n", "context.syntax");
    refuses("2: key C {C}:w\n", "context.syntax", "scale");
    refuses("2: key\n", "context.syntax", "tonic and a scale");
    refuses("2: key H major {C}:w\n", "context.syntax", "H");
    refuses("2: key C whole_tone {C}:w\n", "context.syntax", "whole_tone");
    refuses("2: key G# major {C}:w\n", "context.syntax", "double accidental");
    refuses("2: key C major key D major {C}:w\n", "context.syntax", "second key");
    refuses("2: ~:w ~:w\n", "context.bar_length");
    refuses("2: {C}:0t {C}:w\n", "context.syntax");
    // A write that would pass the point limit changes nothing either.
    Bus many;
    std::string text;
    for (int bar = 1; bar <= 1030; ++bar) {
        text += std::to_string(bar) + ": {C}:q {D}:q {E}:q {F}:q\n";
    }
    auto first = write(many, text);  // 4120 chord points: over the limit of 4096 per type
    REQUIRE_FALSE(first);
    CHECK(first.error().code == "context.limit");
    CHECK(many.app->head().context.empty());
}

// ---------------------------------------------------------------------------------------
// midi.edit: transpose_degrees, snap_to_scale, fit_to_chord (ADR-0095).

namespace {

std::vector<int> pitches(Bus& b, const char* item = "it_1") {
    std::vector<int> out;
    auto notes = b.run("midi.notes", std::string(R"({"item":")") + item + "\"}");
    for (const auto& n : notes["notes"].asArray())
        out.push_back(static_cast<int>(n.find("pitch")->asInt()));
    return out;
}

std::string allNotes(int count) {
    std::string out = "[";
    for (int i = 0; i < count; ++i) out += (i ? "," : "") + std::to_string(i);
    return out + "]";
}

Error editFails(Bus& b, std::string_view ops, const char* item = "it_1") {
    return b.fail("midi.edit",
                  std::string(R"({"item":")") + item + R"(","ops":)" + std::string(ops) + "}");
}

void editOk(Bus& b, std::string_view ops, const char* item = "it_1") {
    b.run("midi.edit", std::string(R"({"item":")") + item + R"(","ops":)" + std::string(ops) + "}");
}

}  // namespace

TEST_CASE("transpose_degrees moves by scale degrees in an explicit key") {
    Bus b;
    b.run("track.add");
    b.run(
        "item.add",
        R"({"track":"tr_1","bar":1,"notation":"voice a; 1: C4:q D4:q E4:q F4:q; 2: G4:q A4:q B4:q C5:q"})");
    CHECK(pitches(b) == std::vector<int>{60, 62, 64, 65, 67, 69, 71, 72});
    editOk(b, R"([{"op":"transpose_degrees","notes":)" + allNotes(8) +
                  R"(,"degrees":2,"key":"C major"}])");
    CHECK(pitches(b) == std::vector<int>{64, 65, 67, 69, 71, 72, 74, 76});
    editOk(b, R"([{"op":"transpose_degrees","notes":)" + allNotes(8) +
                  R"(,"degrees":-9,"key":"C major"}])");  // an octave and two degrees down
    CHECK(pitches(b) == std::vector<int>{48, 50, 52, 53, 55, 57, 59, 60});
    // A pitch outside the scale goes down to the scale first: C# is C, one degree up is D.
    b.run("midi.edit", R"({"item":"it_1","ops":[{"op":"add","pitch":61,"start":0,"length":1}]})");
    const auto now = pitches(b);
    const auto index = std::find(now.begin(), now.end(), 61) - now.begin();
    editOk(b, R"([{"op":"transpose_degrees","notes":[)" + std::to_string(index) +
                  R"(],"degrees":1,"key":"C major"}])");
    const auto after = pitches(b);
    CHECK(std::count(after.begin(), after.end(), 62) == 1);
    CHECK(std::count(after.begin(), after.end(), 61) == 0);
    // Refusals.
    CHECK(editFails(b, R"([{"op":"transpose_degrees","notes":[0],"key":"C major"}])").code ==
          "midi.degrees");
    CHECK(editFails(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"H major"}])")
              .code == "key.tonic");
    CHECK(
        editFails(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"C whole_tone"}])")
            .code == "scale.unknown");
    CHECK(editFails(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"G# major"}])")
              .code == "key.unsupported");
    CHECK(editFails(b, R"([{"op":"transpose_degrees","degrees":1,"key":"C major"}])").code ==
          "midi.no_notes");  // notes are required for a transpose
    CHECK(editFails(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":100,"key":"C major"}])")
              .code == "midi.pitch_range");
    CHECK(editFails(
              b,
              R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"C major","mode":"up"}])")
              .code == "midi.args");
    CHECK(editFails(
              b,
              R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"C major","chord":"C"}])")
              .code == "midi.args");
}

TEST_CASE("the scale ops read the key at each note's project beat") {
    Bus b;
    b.run("track.add");
    // Four bars from bar 3: beats 8 to 24. The key changes at bar 5 (beat 16).
    b.run("item.add",
          R"({"track":"tr_1","bar":3,"notation":"voice a; 1: G4:w; 2: G4:w; 3: G4:w; 4: G4:w"})");
    b.run("key.set", R"({"at":0,"tonic":"C","scale":"major"})");
    b.run("key.set", R"({"at":16,"tonic":"Eb","scale":"major"})");
    editOk(b, R"([{"op":"transpose_degrees","notes":[0,1,2,3],"degrees":1}])");
    CHECK(pitches(b) == std::vector<int>{69, 69, 68, 68});  // A in C major, Ab in Eb major
    // Ops run in order: a semitone move, then a snap sees the moved pitch.
    editOk(
        b,
        R"([{"op":"move","notes":[0,1,2,3],"semitones":1},{"op":"snap_to_scale","mode":"down"}])");
    // Bb (70) down in C major is A; A (69) down in Eb major is Ab.
    CHECK(pitches(b) == std::vector<int>{69, 69, 68, 68});
}

TEST_CASE("snap_to_scale modes and fit_to_chord") {
    Bus b;
    b.run("track.add");
    b.run(
        "item.add",
        R"({"track":"tr_1","bar":3,"notation":"voice a; 1: C#4:q F#4:q G4:q Bb4:q; 2: D4:q G4:q Bb4:q A4:q"})");
    b.run("key.set", R"({"at":0,"tonic":"C","scale":"major"})");
    auto run = [&](std::string_view ops) {
        // Each case starts from the same notes.
        b.run(
            "midi.write",
            R"({"item":"it_1","notation":"voice a; 1: C#4:q F#4:q G4:q Bb4:q; 2: D4:q G4:q Bb4:q A4:q"})");
        editOk(b, ops);
        return pitches(b);
    };
    CHECK(run(R"([{"op":"snap_to_scale"}])") == std::vector<int>{60, 65, 67, 69, 62, 67, 69, 69});
    CHECK(run(R"([{"op":"snap_to_scale","mode":"nearest"}])") ==
          run(R"([{"op":"snap_to_scale"}])"));
    CHECK(run(R"([{"op":"snap_to_scale","mode":"down"}])") ==
          std::vector<int>{60, 65, 67, 69, 62, 67, 69, 69});
    CHECK(run(R"([{"op":"snap_to_scale","mode":"up"}])") ==
          std::vector<int>{62, 67, 67, 71, 62, 67, 71, 69});
    // Only the notes named.
    CHECK(run(R"([{"op":"snap_to_scale","notes":[0]}])") ==
          std::vector<int>{60, 66, 67, 70, 62, 67, 70, 69});
    // An explicit key beats the track; a pentatonic one leaves fewer pitches.
    CHECK(run(R"([{"op":"snap_to_scale","key":"A minor_pentatonic"}])") ==
          std::vector<int>{60, 67, 67, 69, 62, 67, 69, 69});
    // fit_to_chord: the chord at each note's beat. Cm7 {C Eb G Bb} in bar 3, F {F A C} in bar 4.
    b.run("chord.set", R"({"at":0,"symbol":"Cm7"})");
    b.run("chord.set", R"({"at":12,"symbol":"F"})");
    CHECK(run(R"([{"op":"fit_to_chord"}])") == std::vector<int>{60, 67, 67, 70, 60, 65, 69, 69});
    // An explicit chord replaces the context; a slash bass is a tone.
    CHECK(run(R"([{"op":"fit_to_chord","chord":"C/D"}])") ==
          std::vector<int>{60, 67, 67, 72, 62, 67, 72, 67});
    CHECK(editFails(b, R"([{"op":"fit_to_chord","chord":"Cm11"}])").code ==
          "chord.unknown_quality");
    CHECK(editFails(b, R"([{"op":"fit_to_chord","key":"C major"}])").code == "midi.args");
    CHECK(editFails(b, R"([{"op":"snap_to_scale","chord":"C"}])").code == "midi.args");
}

TEST_CASE("a scale op with no key or chord holding says so") {
    Bus b;
    b.run("track.add");
    b.run("item.add", R"({"track":"tr_1","bar":3,"notation":"voice a; 1: C#4:w"})");
    auto noKey = editFails(b, R"([{"op":"snap_to_scale"}])");
    CHECK(noKey.code == "midi.no_key");
    CHECK(noKey.message.find("bar 3") != std::string::npos);
    CHECK(noKey.hint.find("key.set") != std::string::npos);
    auto noChord = editFails(b, R"([{"op":"fit_to_chord"}])");
    CHECK(noChord.code == "midi.no_chord");
    CHECK(noChord.hint.find("chord.set") != std::string::npos);
    b.run("key.set", R"({"at":8,"tonic":"C","scale":"major"})");  // from bar 3 only
    b.run("chord.set", R"({"at":8,"symbol":""})");                // no chord from here
    CHECK(editFails(b, R"([{"op":"fit_to_chord"}])").code == "midi.no_chord");
    editOk(b, R"([{"op":"snap_to_scale"}])");
    CHECK(pitches(b) == std::vector<int>{60});
    // A point this build cannot read is named as such.
    Bus o;
    o.run("track.add");
    o.run("item.add", R"({"track":"tr_1","bar":1,"notation":"voice a; 1: C#4:w"})");
    auto tx = o.app->beginTransaction(kOrigin, "later build");
    REQUIRE(tx);
    model::ContextPoint odd;
    odd.tonic = "C";
    odd.scale = "whole_tone";
    o.app->txn().create(odd);
    REQUIRE(o.app->commitTransaction(*tx));
    auto unreadable = editFails(o, R"([{"op":"snap_to_scale"}])");
    CHECK(unreadable.code == "midi.no_key");
    CHECK(unreadable.message.find("cannot read") != std::string::npos);
}

TEST_CASE("a note that sounds in two keys is refused unless the key is given") {
    // Looping: one bar of G4 played over two bars; the key changes between the passes.
    Bus b;
    b.run("track.add");
    b.run("item.add", R"({"track":"tr_1","bar":1,"notation":"voice a; 1: G4:w"})");
    b.run("item.set", R"({"item":"it_1","loop":true,"length":8})");
    b.run("key.set", R"({"at":0,"tonic":"C","scale":"major"})");
    b.run("key.set", R"({"at":4,"tonic":"Eb","scale":"major"})");
    for (const char* op :
         {R"({"op":"transpose_degrees","notes":[0],"degrees":1})", R"({"op":"snap_to_scale"})"}) {
        const auto refused = editFails(b, std::string("[") + op + "]");
        CHECK(refused.code == "midi.context_varies");
        CHECK(refused.message.find("bar 1") != std::string::npos);
        CHECK(refused.message.find("bar 2") != std::string::npos);
        CHECK_FALSE(refused.hint.empty());
        CHECK(pitches(b) == std::vector<int>{67});  // nothing changed
    }
    // fit_to_chord reads chords, not keys: it does not care that the keys differ.
    b.run("chord.set", R"({"at":0,"symbol":"C"})");
    b.run("chord.set", R"({"at":4,"symbol":"G7"})");
    CHECK(editFails(b, R"([{"op":"fit_to_chord"}])").code == "midi.context_varies");
    b.run("chord.set", R"({"at":4,"symbol":"C"})");  // the same chord both passes
    editOk(b, R"([{"op":"fit_to_chord"}])");
    // An explicit key says which; the context is not read.
    editOk(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"C major"}])");
    CHECK(pitches(b) == std::vector<int>{69});
    // The same key for every pass is not a variation.
    b.run("key.set", R"({"at":4,"tonic":"C","scale":"major"})");
    editOk(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1}])");
    CHECK(pitches(b) == std::vector<int>{71});
}

TEST_CASE("a pooled sharer in another key refuses the same way") {
    Bus b;
    b.run("track.add");
    b.run("item.add", R"({"track":"tr_1","bar":1,"notation":"voice a; 1: G4:w"})");
    b.run("key.set", R"({"at":0,"tonic":"C","scale":"major"})");
    b.run("key.set", R"({"at":16,"tonic":"Eb","scale":"major"})");
    // A second item playing the same MIDI source at bar 5 (a pooled sharer).
    auto tx = b.app->beginTransaction(kOrigin, "pooled sharer");
    REQUIRE(tx);
    model::Item sharer = *b.app->head().items.find({model::Kind::Item, 1});
    model::Take take = *b.app->head().takes.find(*sharer.activeTake);
    const model::Id newTake = b.app->txn().create(take);
    sharer.takes = {newTake};
    sharer.activeTake = newTake;
    sharer.position = 16;
    b.app->txn().create(sharer);
    REQUIRE(b.app->commitTransaction(*tx));
    const auto refused = editFails(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1}])");
    CHECK(refused.code == "midi.context_varies");
    CHECK(refused.message.find("bar 5") != std::string::npos);
    editOk(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1,"key":"Eb major"}])");
    CHECK(pitches(b) == std::vector<int>{68});
    // Where both sharers sit in one key, it works without the key.
    b.run("key.remove", R"({"at":16})");
    editOk(b, R"([{"op":"transpose_degrees","notes":[0],"degrees":1}])");
    CHECK(pitches(b) == std::vector<int>{69});  // Ab (68) is not in C major: down to G, up one: A
}
