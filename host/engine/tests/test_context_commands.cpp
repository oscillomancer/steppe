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
