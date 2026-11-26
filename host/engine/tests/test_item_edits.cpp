// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include "../../../tests/file_platform.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/midi/midi.h"
#include "supple/model/follow.h"
#include "supple/model/item_edits.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace {
const cmd::Origin origin{cmd::Origin::Type::Test, "item-edits", {}};
struct Fixture {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::filesystem::path dir = supple::test::uniqueTempDir("item-edits-");
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    explicit Fixture(bool seedSong = true) {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        options.jobFinishedHook = [&] {
            std::lock_guard lock(mutex);
            finished = true;
            done.notify_all();
        };
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        if (seedSong) app->setupFixture(*bus, "song_with_audio");
    }
    ~Fixture() {
        bus.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    Result<json::Value> call(const char* command, std::string_view args = "{}") {
        return bus->execute({command, *json::parse(args), origin});
    }
    json::Value run(const char* command, std::string_view args = "{}") {
        auto result = call(command, args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    void checkUnknown(const char* command, std::string_view args, std::string_view code,
                      std::string_view path, std::string_view hint) {
        auto result = call(command, args);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK(result.error().path == path);
        CHECK(result.error().hint == hint);
    }
    json::Value finish(const json::Value& job) {
        {
            std::unique_lock lock(mutex);
            done.wait(lock, [&] { return finished; });
        }
        bus->flushJobEvents();
        auto status = bus->jobs().status(job.find("job")->asString());
        REQUIRE(status);
        REQUIRE_MESSAGE((*status)["status"].asString() == "done", json::writeCompact(*status));
        return (*status)["result"];
    }
};
}  // namespace

TEST_CASE("track.move permits non-root reorders and sibling folders but rejects descendants") {
    Fixture f(false);
    f.run("track.add", R"({"name":"A","type":"buss"})");                      // tr_1
    f.run("track.add", R"({"name":"B","type":"buss"})");                      // tr_2
    f.run("track.add", R"({"name":"Child","type":"buss","parent":"tr_1"})");  // tr_3
    f.run("track.add", R"({"name":"Deep","type":"buss","parent":"tr_3"})");   // tr_4
    f.run("track.add", R"({"name":"First","parent":"tr_1"})");                // tr_5
    f.run("track.add", R"({"name":"Second","parent":"tr_1"})");               // tr_6
    const auto initial = model::serializeProject(f.app->head());
    const auto track = [&](unsigned n) -> const model::Track& {
        const auto* result = f.app->head().tracks.find({model::Kind::Track, n});
        REQUIRE(result);
        return *result;
    };
    const auto children = [&](unsigned parent, std::initializer_list<unsigned> expected) {
        std::vector<model::Id> ids;
        for (auto n : expected) ids.push_back({model::Kind::Track, n});
        CHECK(track(parent).children == ids);
        REQUIRE(model::checkInvariants(f.app->head()));
    };
    // Explicit and inferred current parents must both permit real reorders.
    f.run("track.move", R"({"track":"tr_6","parent":"tr_1","index":0})");
    children(1, {6, 3, 5});
    CHECK(track(6).parent == model::Id{model::Kind::Track, 1});
    const auto reordered = model::serializeProject(f.app->head());
    f.run("track.move", R"({"track":"tr_5","index":0})");
    children(1, {5, 6, 3});
    f.run("history.undo");
    CHECK(model::serializeProject(f.app->head()) == reordered);
    f.run("history.undo");
    CHECK(model::serializeProject(f.app->head()) == initial);

    // Reparent a whole subtree between sibling folders, then undo it exactly.
    f.run("track.move", R"({"track":"tr_3","parent":"tr_2","index":0})");
    children(1, {5, 6});
    children(2, {3});
    children(3, {4});
    CHECK(track(3).parent == model::Id{model::Kind::Track, 2});
    CHECK(track(4).parent == model::Id{model::Kind::Track, 3});
    const auto moved = model::serializeProject(f.app->head());
    const auto version = f.app->head().version;
    for (unsigned parent : {2, 3, 4}) {
        CAPTURE(parent);
        auto refused = f.call(
            "track.move", "{\"track\":\"tr_2\",\"parent\":\"tr_" + std::to_string(parent) + "\"}");
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "tr.cycle");
        CHECK(refused.error().path == "/args/parent");
        CHECK_FALSE(refused.error().hint.empty());
        CHECK(refused.error().message ==
              (parent == 2 ? "a track cannot be moved under itself"
                           : "a track cannot be moved under one of its own children"));
        CHECK(f.app->head().version == version);
        CHECK(model::serializeProject(f.app->head()) == moved);
    }
    f.run("history.undo");
    CHECK(model::serializeProject(f.app->head()) == initial);
    children(0, {1, 2});
    children(1, {3, 5, 6});
    children(2, {});
}

TEST_CASE("unknown track IDs list the existing tracks, including the empty project's master") {
    Fixture f(false);
    auto check = [&](std::string_view hint) {
        f.checkUnknown("track.remove", R"({"track":"tr_999"})", "tr.unknown", "/args/track", hint);
    };
    check("existing: tr_0");
    f.run("track.add");
    check("existing: tr_0, tr_1");
    for (int i = 0; i < 6; ++i) f.run("track.add");
    check("existing: tr_0, tr_1, tr_2, tr_3, tr_4, tr_5, tr_6, tr_7");
    f.run("track.add");
    check("existing: tr_0, tr_1, tr_2, tr_3, tr_4, tr_5, tr_6, tr_7, …");
}

TEST_CASE("unknown item IDs list existing items or explain that there are none") {
    Fixture f(false);
    auto check = [&](std::string_view hint) {
        f.checkUnknown("item.remove", R"({"item":"it_999"})", "it.unknown", "/args/item", hint);
    };
    check("there are none yet");
    f.run("track.add");
    const auto add = [&] {
        f.run("item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    };
    add();
    check("existing: it_1");
    for (int i = 0; i < 7; ++i) add();
    check("existing: it_1, it_2, it_3, it_4, it_5, it_6, it_7, it_8");
    add();
    check("existing: it_1, it_2, it_3, it_4, it_5, it_6, it_7, it_8, …");
}

TEST_CASE("unknown FX IDs list existing instances or explain that there are none") {
    Fixture f(false);
    auto check = [&](std::string_view hint) {
        f.checkUnknown("fx.remove", R"({"fx":"fx_999"})", "fx.unknown", "/args/fx", hint);
    };
    check("there are none yet");
    f.run("track.add");
    const auto add = [&] {
        f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    };
    add();
    check("existing: fx_1");
    for (int i = 0; i < 7; ++i) add();
    check("existing: fx_1, fx_2, fx_3, fx_4, fx_5, fx_6, fx_7, fx_8");
    add();
    check("existing: fx_1, fx_2, fx_3, fx_4, fx_5, fx_6, fx_7, fx_8, …");
}

TEST_CASE("consolidation bakes item and take FX with silence in gaps, leaving track FX live") {
    Fixture f;
    f.run("item.set",
          R"({"item":"it_1","length":0.5,"gain_db":-3,"fade_in":0.01,"fade_out":0.02})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("fx.param.set", R"({"fx":"fx_1","values":{"gain":6}})");
    f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("fx.param.set", R"({"fx":"fx_2","values":{"gain":-6}})");
    f.run("env.add", R"({"take":"tk_1","param":"volume","points":[{"beat":0,"value":-3}]})");
    f.run("env.add", R"({"fx":"fx_2","param":"gain","points":[{"beat":0,"value":-6}]})");
    f.run("autoitem.add",
          R"({"env":"en_2","position":0,"length":0.5,"points":[{"beat":0,"value":-6}]})");
    f.run("item.duplicate", R"({"item":"it_1","to":2})");
    const auto original = f.app->head();
    OfflineSettings settings{.endSeconds = 1.25,
                             .threads = 1,
                             .track = model::Id{model::Kind::Track, 1},
                             .tap = model::Tap::PreFader};
    auto full = f.app->renderSession(original, settings);
    REQUIRE(full);
    model::EditTxn source(original, original.version);
    source.modify<model::Track>({model::Kind::Track, 1}, [](auto& track) { track.chain.clear(); });
    source.remove<model::FxInstance>({model::Kind::Fx, 1});
    auto isolated = source.commit();
    REQUIRE(isolated);
    auto expected = f.app->renderSession(*isolated, settings);
    REQUIRE(expected);
    const auto result = f.finish(f.run("item.consolidate", R"({"items":["it_1","it_2"]})"));
    REQUIRE(result.find("files")->size() == 1);
    auto audio = audio::readAudio(result.find("files")->asArray()[0].asString());
    REQUIRE(audio);
    CHECK(bool(audio->channels == expected->audio.channels));
    CHECK(audio->channels[0][24000] == 0);
    REQUIRE(f.app->head().items.size() == 1);
    const auto& item = *f.app->head().items.find({model::Kind::Item, 1});
    const auto& take = *f.app->head().takes.find(*item.activeTake);
    CHECK(take.chain.empty());
    CHECK(f.app->head().autoItems.empty());
    CHECK(item.fadeIn.length == 0);
    CHECK(item.gainDb == 0);
    CHECK(f.app->head().tracks.find({model::Kind::Track, 1})->chain.size() == 1);
    auto after = f.app->renderSession(f.app->head(), settings);
    REQUIRE(after);
    CHECK(bool(after->audio.channels == full->audio.channels));
    CHECK(f.app->head().version == original.version + 1);
    REQUIRE(f.app->undo());
    CHECK(f.app->head().items.size() == 2);
    REQUIRE(f.app->redo());
    CHECK(f.app->head().items.size() == 1);
    CHECK(std::filesystem::exists(result.find("files")->asArray()[0].asString()));
}

TEST_CASE("consolidation partitions mixed items by track and active kind") {
    Fixture f;
    f.run("item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    f.run("track.add");
    f.run("item.add", R"({"track":"tr_2","notation":"1: D4:w"})");
    f.run("item.add", R"({"track":"tr_1","position":4,"notation":"1: E4:w"})");
    const std::vector<model::Id> ids{{model::Kind::Item, 1},
                                     {model::Kind::Item, 2},
                                     {model::Kind::Item, 3},
                                     {model::Kind::Item, 4}};
    auto groups = model::consolidationGroups(f.app->head(), ids);
    REQUIRE(groups);
    REQUIRE(groups->size() == 3);
    CHECK((*groups)[0].items.size() == 2);
    CHECK_FALSE((*groups)[0].audio);
    CHECK((*groups)[1].items.size() == 1);
    CHECK((*groups)[1].audio);
    CHECK((*groups)[2].track == model::Id{model::Kind::Track, 2});
    auto result = f.finish(f.run("item.consolidate", R"({"items":["it_1","it_2","it_3","it_4"]})"));
    CHECK(result["items"].size() == 3);
    CHECK(f.app->head().items.size() == 3);
    CHECK(model::itemLengthBeats(f.app->head(),
                                 *f.app->head().items.find({model::Kind::Item, 2})) == 8);
}

TEST_CASE("consolidation joins MIDI items across a gap, and layers a lone item's takes") {
    Fixture f;
    f.run("track.add", R"({"name":"Keys"})");
    f.run("item.add", R"({"track":"tr_2","bar":1,"notation":"voice keys; 1: C2:q r:*"})");
    f.run("item.add", R"({"track":"tr_2","bar":5,"notation":"voice keys; 1: E2:q r:*"})");
    f.finish(f.run("item.consolidate", R"({"items":["it_2","it_3"]})"));
    CHECK_FALSE(f.app->head().items.find({model::Kind::Item, 3}));
    auto notes = f.run("midi.notes", R"({"item":"it_2"})");
    CHECK(notes.find("length")->asDouble() == doctest::Approx(20));
    const auto& list = notes.find("notes")->asArray();
    REQUIRE(list.size() == 2);
    CHECK(list[0].find("pitch")->asInt() == 36);
    CHECK(list[1].find("pitch")->asInt() == 40);
    CHECK(list[1].find("start")->asDouble() == doctest::Approx(16));
    // A lone MIDI item's two takes become one take with both.
    const auto& item = *f.app->head().items.find({model::Kind::Item, 2});
    f.run("take.add", R"({"item":"it_2","from":")" + item.activeTake->str() + R"("})");
    f.run("midi.write", R"({"item":"it_2","notation":"voice keys; 1: G2:q r:*"})");
    REQUIRE(f.app->head().items.find({model::Kind::Item, 2})->takes.size() == 2);
    f.finished = false;
    f.finish(f.run("item.consolidate", R"({"items":["it_2"]})"));
    CHECK(f.app->head().items.find({model::Kind::Item, 2})->takes.size() == 1);
    CHECK(f.run("midi.notes", R"({"item":"it_2"})").find("notes")->asArray().size() == 3);
}

namespace {
// What an item's active take sounds as: "pitch@beat", by beat (ADR-0098).
std::vector<std::pair<long long, int>> heardBy(const model::Session& s, model::Id item) {
    std::vector<std::pair<long long, int>> out;
    const double start = model::itemSpan(s, *s.items.find(item)).startBeat;
    for (const auto& n : model::soundingNotes(s, item, -1e9, 1e9))
        out.emplace_back(std::llround(std::max(n.songBeat, start) * 960.0), n.sounds.pitch);
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

TEST_CASE(
    "consolidating following MIDI items keeps what sounds, and refuses what would change it") {
    Fixture f;
    f.run("track.add", R"({"name":"Keys"})");  // tr_2
    f.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    f.run("item.add", R"({"track":"tr_2","bar":1,"notation":"voice keys; 1: C2:q r:*"})");
    f.run("item.add", R"({"track":"tr_2","bar":5,"notation":"voice keys; 1: E2:q r:*"})");
    const model::Id one{model::Kind::Item, 2}, two{model::Kind::Item, 3};
    const auto takeOf = [&](model::Id item) { return *f.app->head().items.find(item)->activeTake; };
    for (const model::Id item : {one, two})
        f.run("take.set", (R"({"take":")" + takeOf(item).str() +
                           R"(","context":"1: key C major ~:w","follow":"key"})")
                              .c_str());
    const auto before1 = heardBy(f.app->head(), one);
    const auto before2 = heardBy(f.app->head(), two);
    REQUIRE(before1.size() == 1);
    CHECK(before1[0].second == 39);  // C to Eb
    CHECK(before2[0].second == 43);  // E to G
    f.finish(f.run("item.consolidate", R"({"items":["it_2","it_3"]})"));
    REQUIRE(f.app->head().items.find(one));
    const model::Take& merged = *f.app->head().takes.find(takeOf(one));
    CHECK(merged.follow == model::ClipFollow::Key);
    CHECK_FALSE(merged.context.empty());
    CHECK_FALSE(merged.contextPeriod);
    const auto after = heardBy(f.app->head(), one);
    REQUIRE(after.size() == 2);
    CHECK(after[0] == before1[0]);
    CHECK(after[1] == before2[0]);
    // The result holds the notes as written.
    auto notes = f.run("midi.notes", R"({"item":"it_2"})");
    CHECK(notes.find("notes")->asArray()[0].find("pitch")->asInt() == 36);
    f.run("history.undo");
    CHECK(f.app->head().items.size() == 3);
    CHECK(heardBy(f.app->head(), two) == before2);
}

TEST_CASE("consolidating MIDI items refuses when only some follow, or a chased note would change") {
    const auto failedWith = [](Fixture& f, const char* args) {
        {
            std::unique_lock lock(f.mutex);
            f.finished = false;
        }
        auto job = f.run("item.consolidate", args);
        {
            std::unique_lock lock(f.mutex);
            f.done.wait(lock, [&] { return f.finished; });
        }
        f.bus->flushJobEvents();
        auto status = f.bus->jobs().status(job.find("job")->asString());
        REQUIRE(status);
        return json::writeCompact(*status);
    };
    Fixture f;
    f.run("track.add", R"({"name":"Keys"})");
    f.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    f.run("item.add", R"({"track":"tr_2","bar":1,"notation":"voice keys; 1: C2:q r:*"})");
    f.run("item.add", R"({"track":"tr_2","bar":5,"notation":"voice keys; 1: E2:q r:*"})");
    const auto take = *f.app->head().items.find({model::Kind::Item, 2})->activeTake;
    f.run("take.set",
          (R"({"take":")" + take.str() + R"(","context":"1: key C major ~:w","follow":"key"})")
              .c_str());
    const auto text = failedWith(f, R"({"items":["it_2","it_3"]})");
    CHECK(text.find("item.follow_merge") != std::string::npos);
    CHECK(text.find("take.bake") != std::string::npos);
    CHECK(f.app->head().items.size() == 3);  // nothing changed
}

TEST_CASE("consolidating an item whose context is longer than a take holds refuses (round 91 M3)") {
    Fixture f(false);
    f.run("track.add", R"({"name":"Keys"})");  // tr_1
    f.run("scene.add", R"({"name":"One"})");   // sc_1
    f.run("key.set", R"({"at":0,"tonic":"C","scale":"major"})");
    f.run("item.add", R"({"track":"tr_1","bar":1,"notation":"voice keys; 1: E2:q r:*"})");
    f.run("take.set",
          (R"({"take":")" + f.app->head().items.find({model::Kind::Item, 1})->activeTake->str() +
           R"(","context":"1: key C major ~:w","follow":"key"})")
              .c_str());
    // A one-bar clip with sixteen chord changes laid over 260 bars repeats them 4160 times, past
    // the 4096 a take holds; consolidating it with another item cannot carry them.
    f.run("clip.add", R"({"track":"tr_1","scene":"sc_1","notation":"voice keys; 1: C2:w",
          "context":"1: key C major {C}:s {D}:s {E}:s {F}:s {G}:s {A}:s {B}:s {C}:s {D}:s {E}:s {F}:s {G}:s {A}:s {B}:s {C}:s {D}:s",
          "follow":"key"})");
    const auto laid = f.run("scene.insert", R"({"scene":"sc_1","bar":9,"bars":260})");
    const std::string item = laid.find("items")->at(0).find("item")->asString();
    f.finished = false;
    const auto job = f.run("item.consolidate", (R"({"items":["it_1",")" + item + R"("]})").c_str());
    {
        std::unique_lock lock(f.mutex);
        f.done.wait(lock, [&] { return f.finished; });
    }
    f.bus->flushJobEvents();
    const auto status = f.bus->jobs().status(job.find("job")->asString());
    REQUIRE(status);
    const std::string text = json::writeCompact(*status);
    CHECK(text.find("item.follow_merge") != std::string::npos);
    CHECK(text.find("context too long") != std::string::npos);
}

TEST_CASE("consolidating a lone following item keeps a note held into its start (round 85 G11)") {
    Fixture f;
    f.run("track.add", R"({"name":"Keys"})");  // tr_2
    f.run("key.set", R"({"at":0,"tonic":"Eb","scale":"major"})");
    f.run("key.set", R"({"at":7,"tonic":"F","scale":"major"})");
    f.run("item.add", R"({"track":"tr_2","bar":3,"notation":"voice keys; 1: C2:w"})");
    const model::Id item{model::Kind::Item, 2};
    f.run("take.add", R"({"item":"it_2","from":"tk_2"})");
    REQUIRE(f.app->head().items.find(item)->takes.size() == 2);
    // Both takes follow alike, and start two beats into their note: it began two beats before
    // the item does, under Eb major, and the item starts under F.
    for (const model::Id take : f.app->head().items.find(item)->takes)
        f.run("take.set", (R"({"take":")" + take.str() +
                           R"(","context":"1: key C major ~:w","follow":"key","start_offset":2})")
                              .c_str());
    f.run("take.activate", R"({"item":"it_2","index":0})");
    f.run("item.set", R"({"item":"it_2","length":2})");
    const auto before = heardBy(f.app->head(), item);
    REQUIRE(before.size() == 1);
    f.finish(f.run("item.consolidate", R"({"items":["it_2"]})"));
    CHECK(f.app->head().items.find(item)->takes.size() == 1);
    const auto after = heardBy(f.app->head(), item);
    CHECK(std::count(after.begin(), after.end(), before[0]) == 1);
}

TEST_CASE("healing restores pre-split and pre-slice audio bit for bit without take FX") {
    Fixture f;
    bool slice = false, warp = false;
    SUBCASE("ordinary split") {}
    SUBCASE("unchanged slices with generated overlaps") { slice = true; }
    SUBCASE("warped source split") { warp = true; }
    f.run("item.set", R"({"item":"it_1","fade_in":0.01,"fade_out":0.02,"gain_db":-3})");
    if (warp) f.run("take.stretch.add", R"({"take":"tk_1","source_pos":0.6,"item_pos":0.5})");
    const OfflineSettings settings{.endSeconds = 2, .threads = 1};
    auto before = f.app->renderSession(f.app->head(), settings);
    REQUIRE(before);
    if (slice) {
        f.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
        f.run("audio.quantize", R"({"items":["it_1"],"grid":0.25,"mode":"slice","strength":0})");
    } else {
        f.run("item.split", R"({"item":"it_1","at":0.6})");
        f.run("item.split", R"({"item":"it_2","at":1.6})");
    }
    const auto cut = f.app->head().version;
    f.run("item.heal", R"({"items":["it_1","it_2","it_3"]})");
    CHECK(f.app->head().version == cut + 1);
    REQUIRE(f.app->head().items.size() == 1);
    auto healed = f.app->renderSession(f.app->head(), settings);
    REQUIRE(healed);
    CHECK(bool(healed->audio.channels == before->audio.channels));
    REQUIRE(f.app->undo());
    CHECK(f.app->head().items.size() == 3);
}

TEST_CASE("healing refuses moved pieces and differing stored take FX") {
    Fixture f;
    bool fx = false;
    SUBCASE("moved piece") {}
    SUBCASE("stored FX differ") { fx = true; }
    if (fx) f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("item.split", R"({"item":"it_1","at":1})");
    if (fx)
        f.run("fx.param.set", R"({"fx":"fx_2","values":{"gain":-6}})");
    else
        f.run("item.move", R"({"item":"it_2","position":1.25})");
    auto result = f.call("item.heal", R"({"items":["it_1","it_2"]})");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == (fx ? "item.heal_fx" : "item.heal_incompatible"));
    CHECK_FALSE(result.error().hint.empty());
    CHECK(f.app->head().items.size() == 2);
}

TEST_CASE("item.export writes the raw source span a trimmed, split or looping item covers") {
    Fixture f;
    const auto tone = audio::readAudio(f.dir / "media" / "tone.wav").value();
    auto exported = [&](const char* item, const char* name) {
        auto args = json::Value::object();
        args["item"] = item;
        args["path"] = (f.dir / name).string();
        auto result = f.run("item.export", json::writeCompact(args));
        const std::filesystem::path written = result["path"].asString();
        CHECK(written.stem() == std::filesystem::path(name).stem());
        CHECK(written.extension() == ".wav");
        return audio::readAudio(written).value();
    };
    // Trimmed at both ends: half a second from 0.25 s, gain and fades not applied.
    f.run("item.set", R"({"item":"it_1","length":1,"gain_db":-6,"fade_in":0.1})");
    f.run("take.set", R"({"take":"tk_1","start_offset":0.25})");
    auto trimmed = exported("it_1", "trimmed.wav");
    REQUIRE(trimmed.channelCount() == 2);
    REQUIRE(trimmed.frames() == 24000);
    for (std::size_t i = 0; i < trimmed.frames(); ++i)
        REQUIRE(trimmed.channels[0][i] == tone.channels[0][12000 + i]);
    // Looping past the source's end: the source repeats as heard.
    f.run("take.set", R"({"take":"tk_1","start_offset":0})");
    f.run("item.set", R"({"item":"it_1","length":10,"loop":true})");
    auto looped = exported("it_1", "looped.wav");
    REQUIRE(looped.frames() == 5 * 48000);
    for (std::size_t i = 0; i < looped.frames(); i += 997)
        REQUIRE(looped.channels[1][i] == tone.channels[1][i % tone.frames()]);
    // Split: each half exports its own part only, and together they are the source.
    f.run("item.set", R"({"item":"it_1","length":4,"loop":false})");
    f.run("item.split", R"({"item":"it_1","at":2})");
    auto left = exported("it_1", "left.wav");
    auto right = exported("it_2", "right");  // the extension is added
    REQUIRE(left.frames() == 48000);
    REQUIRE(right.frames() == 48000);
    CHECK(std::equal(left.channels[0].begin(), left.channels[0].end(), tone.channels[0].begin()));
    CHECK(std::equal(right.channels[0].begin(), right.channels[0].end(),
                     tone.channels[0].begin() + 48000));
    // An audio item does not become a MIDI file, nor overwrite the project's media.
    CHECK(f.call("item.export", R"({"item":"it_1","path":"x.mid"})").error().code ==
          "item.export_extension");
    auto media = json::Value::object();
    media["item"] = "it_1";
    media["path"] = (f.dir / "media" / "tone.wav").string();
    CHECK(f.call("item.export", json::writeCompact(media)).error().code == "item.export_path");
}

TEST_CASE("item.export writes a trimmed MIDI item as a Standard MIDI File from zero") {
    Fixture f;
    f.run("item.add", R"({"track":"tr_1","bar":3,"notation":"voice x; 1: C2:q D2:q E2:q F2:q"})");
    // The middle two notes: the take starts a beat in and the item lasts two.
    f.run("take.set", R"({"take":"tk_2","start_offset":1})");
    f.run("item.set", R"({"item":"it_2","length":2})");
    auto args = json::Value::object();
    args["item"] = "it_2";
    args["path"] = (f.dir / "middle.mid").string();
    CHECK(f.run("item.export", json::writeCompact(args))["notes"].asInt() == 2);
    midi::SmfInfo info;
    auto seq = midi::readSmf(readFile(f.dir / "middle.mid").value(), &info);
    REQUIRE(seq);
    REQUIRE(seq->notes.size() == 2);
    CHECK(seq->notes[0].pitch == 38);
    CHECK(seq->notes[0].start == 0);
    CHECK(seq->notes[1].pitch == 40);
    CHECK(seq->notes[1].start == seq->ppq);
    CHECK(seq->length == 2 * seq->ppq);
    REQUIRE(info.tempos.size() == 1);
    CHECK(info.tempos[0].second == doctest::Approx(120.0));
    CHECK(info.meters == std::vector<std::pair<std::int64_t, midi::Meter>>{{0, midi::Meter{4, 4}}});
    // item.add imports it back: the same two notes, as long as the file.
    auto add = json::Value::object();
    add["track"] = "tr_1";
    add["bar"] = 5;
    add["midi_file"] = (f.dir / "middle.mid").string();
    auto added = f.run("item.add", json::writeCompact(add));
    CHECK(added["length"].asDouble() == doctest::Approx(2.0));
    CHECK_FALSE(added.find("warnings"));  // 120 BPM, as the project
    const auto& s = f.app->view();
    const auto* take = s.takes.find(*model::Id::parse(added["take"].asString()));
    REQUIRE(take);
    const auto& imported = s.midi.find(*take->source)->sequence;
    REQUIRE(imported.notes.size() == 2);
    CHECK(imported.notes[0].pitch == 38);
    CHECK(imported.notes[1].start == imported.ppq);
    // MIDI never goes onto a master.
    add["track"] = "tr_0";
    CHECK(f.call("item.add", json::writeCompact(add)).error().code == "track.strip_type");
}

TEST_CASE("item.mix-to-track sums the selected items' strips onto a new muted channel") {
    Fixture f;
    // tr_2 plays the tone from 1 s, quieter and panned; tr_1 plays it from 0.
    f.run("track.add", R"({"name":"Other"})");
    auto add = json::Value::object();
    add["track"] = "tr_2";
    add["position"] = 2;
    add["audio_file"] = (f.dir / "media" / "tone.wav").string();
    f.run("item.add", json::writeCompact(add));
    f.run("track.set", R"({"track":"tr_2","volume_db":-6,"pan":30})");
    // With Console 9 off (this fixture) the stages are linear: the mix is the
    // sum of the two strips' stems, each decoded as a buss would.
    auto stem = [&](const char* track) {
        OfflineSettings settings;
        settings.applyMasterPlayrate = false;
        settings.track = *model::Id::parse(track);
        settings.startSeconds = 0;
        settings.endSeconds = 3;
        return f.app->renderMix(settings).value().audio;
    };
    const auto one = stem("tr_1"), two = stem("tr_2");
    // Neither the mute switch nor an unselected item on a source channel is heard.
    f.run("track.set", R"({"track":"tr_1","mute":true})");
    add["position"] = 0;
    f.run("item.add", json::writeCompact(add));  // it_3, not selected
    const auto tracks = f.app->head().tracks.size();
    {
        std::lock_guard lock(f.mutex);
        f.finished = false;
    }
    const auto result = f.finish(f.run("item.mix-to-track", R"({"items":["it_1","it_2"]})"));
    const auto s = f.app->head();
    REQUIRE(s.tracks.size() == tracks + 1);
    const auto track = *model::Id::parse(result.find("track")->asString());
    const auto& made = *s.tracks.find(track);
    CHECK(made.type == model::StripType::Channel);
    CHECK(made.mute);
    CHECK(made.name == "Mix");
    CHECK(made.parent == model::kMaster);
    const auto& children = s.tracks.find(model::kMaster)->children;
    CHECK(children.back() == track);  // after tr_2, the last source
    const auto& item = *s.items.find(*model::Id::parse(result.find("item")->asString()));
    const auto span = model::itemSpan(s, item);
    CHECK(span.startSec == doctest::Approx(0));
    CHECK(span.endSec == doctest::Approx(3));
    CHECK(s.items.size() == 4);  // the sources stay
    const auto mix = audio::readAudio(result.find("file")->asString()).value();
    REQUIRE(mix.channelCount() == 2);
    REQUIRE(mix.frames() == 3 * 48000);
    REQUIRE(one.frames() == mix.frames());
    float peak = 0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < mix.frames(); ++i) {
            const double expected = double(one.channels[c][i]) + double(two.channels[c][i]);
            REQUIRE(double(mix.channels[c][i]) == doctest::Approx(expected).epsilon(1e-5));
            peak = std::max(peak, std::abs(mix.channels[c][i]));
        }
    CHECK(peak > 0.1f);
    // One undo step removes the channel and its item.
    REQUIRE(f.app->undo());
    CHECK(f.app->head().tracks.size() == tracks);
}

TEST_CASE("healing joins take FX routes that match, and refuses ones that differ (M5)") {
    Fixture f;
    f.run("rack.add", R"({"track":"tr_1"})");                    // rk_1, mo_1 its Out
    f.run("module.add", R"({"rack":"rk_1","type":"mod_out"})");  // mo_2
    f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");  // fx_1
    f.run("modulation.add", R"({"source":{"module":"mo_2","input":"in_1"},
        "target":{"fx":"fx_1","param":"gain"},"depth":0.25})");                  // mr_1
    // Both halves keep the route: the right one on its copied FX.
    f.run("item.split", R"({"item":"it_1","at":1})");
    REQUIRE(f.app->head().modulations.size() == 2);
    const model::Id copy{model::Kind::Modulation, 2};
    CHECK(f.app->head().modulations.find(copy)->target.object == model::Id::parse("fx_2"));
    // A depth that differs: refused, its own words and hint.
    f.run("modulation.set", R"({"modulation":"mr_2","depth":0.5})");
    auto refused = f.call("item.heal", R"({"items":["it_1","it_2"]})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "item.heal_fx");
    CHECK(refused.error().message.find("modulation routes") != std::string::npos);
    CHECK(refused.error().hint.find("modulation.set") != std::string::npos);
    // Matching again: healed, the right half's route goes with its piece and
    // no route is reported removed (it lives on in the left one).
    f.run("modulation.set", R"({"modulation":"mr_2","depth":0.25})");
    const auto healed = f.run("item.heal", R"({"items":["it_1","it_2"]})");
    if (const json::Value* warnings = healed.find("warnings"))
        for (const auto& w : warnings->asArray())
            CHECK(w.find("code")->asString() != "modulation.removed");
    REQUIRE(f.app->head().modulations.size() == 1);
    CHECK(f.app->head().modulations.begin()->value->target.object == model::Id::parse("fx_1"));
    REQUIRE(f.app->undo());
    CHECK(f.app->head().modulations.size() == 2);
}

namespace {
// rk_1 on tr_1 (its Out uncabled): Transport's beat phase into the Mod Out
// mo_2, routed into a gain added to tk_1's chain (fx_1 when tr_1 has none).
void beatModulated(Fixture& f) {
    f.run("rack.add", R"({"track":"tr_1"})");                      // rk_1, mo_1 its Out
    f.run("module.add", R"({"rack":"rk_1","type":"mod_out"})");    // mo_2
    f.run("module.add", R"({"rack":"rk_1","type":"transport"})");  // mo_3
    f.run("cable.add", R"({"from":"mo_3.beat_phase","to":"mo_2.in_1"})");
    const auto fx = f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
    json::Value add = *json::parse(R"({"source":{"module":"mo_2","input":"in_1"},"depth":0.3})");
    add["target"] = json::Value::object();
    add["target"]["fx"] = fx.find("fx")->asString();
    add["target"]["param"] = "gain";
    auto added = f.bus->execute({"modulation.add", std::move(add), origin});
    REQUIRE_MESSAGE(added, (added ? "" : added.error().toString()));
}
}  // namespace

TEST_CASE("item.apply-fx prints a modulated take FX exactly as it plays (M5, F10)") {
    // The take's gain moved by Transport's beat phase: the track over the
    // item's span sounds the same before the bake and after it, when the
    // printed take plays without the FX (mode replace removes them, and the
    // route with them, after printing).
    Fixture f;
    beatModulated(f);
    const model::ItemSpan span =
        model::itemSpan(f.app->head(), *f.app->head().items.find(*model::Id::parse("it_1")));
    OfflineSettings settings;
    settings.applyMasterPlayrate = false;
    settings.track = *model::Id::parse("tr_1");
    settings.tap = model::Tap::PreFader;
    settings.startSeconds = span.startSec;
    settings.endSeconds = span.endSec;
    settings.threads = 1;
    auto before = f.app->renderSession(f.app->head(), settings);
    REQUIRE(before);
    // The route moves something: without it the track sounds otherwise.
    {
        model::EditTxn t(f.app->head(), f.app->head().version + 1);
        for (const auto& m : f.app->head().modulations) t.remove<model::Modulation>(m.id);
        auto plain = t.commit();
        REQUIRE(plain);
        auto unmodulated = f.app->renderSession(*plain, settings);
        REQUIRE(unmodulated);
        CHECK_FALSE(bool(unmodulated->audio.channels == before->audio.channels));
    }
    f.run("item.apply-fx", R"({"item":"it_1","mode":"replace"})");
    CHECK(f.app->head().modulations.empty());
    auto after = f.app->renderSession(f.app->head(), settings);
    REQUIRE(after);
    CHECK(bool(after->audio.channels == before->audio.channels));
}

TEST_CASE("item.consolidate prints modulated take FX exactly as they play (M5, F10)") {
    // Two items whose takes' gains a route moves (the duplicate copies it):
    // the track sounds the same before and after, the routes going with the
    // baked FX.
    Fixture f;
    beatModulated(f);
    f.run("item.set", R"({"item":"it_1","length":0.5})");
    f.run("item.duplicate", R"({"item":"it_1","to":2})");
    REQUIRE(f.app->head().modulations.size() == 2);
    OfflineSettings settings{.endSeconds = 1.25,
                             .threads = 1,
                             .track = model::Id{model::Kind::Track, 1},
                             .tap = model::Tap::PreFader};
    auto before = f.app->renderSession(f.app->head(), settings);
    REQUIRE(before);
    f.finish(f.run("item.consolidate", R"({"items":["it_1","it_2"]})"));
    REQUIRE(f.app->head().items.size() == 1);
    CHECK(f.app->head().modulations.empty());
    auto after = f.app->renderSession(f.app->head(), settings);
    REQUIRE(after);
    CHECK(bool(after->audio.channels == before->audio.channels));
}

TEST_CASE("item.consolidate.commit is stale when a route into a baked FX changed (M5, G6)") {
    // The render is taken with one depth; the route changes before the
    // commit, so what was printed is no longer what plays: refused as stale.
    Fixture f;
    beatModulated(f);
    f.run("item.set", R"({"item":"it_1","length":0.5})");
    f.run("item.duplicate", R"({"item":"it_1","to":2})");
    const auto job = f.run("item.consolidate", R"({"items":["it_1","it_2"]})");
    f.run("modulation.set", R"({"modulation":"mr_1","depth":-0.3})");
    {
        std::unique_lock lock(f.mutex);
        f.done.wait(lock, [&] { return f.finished; });
    }
    f.bus->flushJobEvents();
    auto status = f.bus->jobs().status(job.find("job")->asString());
    REQUIRE(status);
    INFO(json::writeCompact(*status));
    CHECK((*status)["status"].asString() != "done");
    CHECK(json::writeCompact(*status).find("item.consolidate_stale") != std::string::npos);
    CHECK(f.app->head().items.size() == 2);
    CHECK(f.app->head().modulations.size() == 2);
}

namespace {
// tk_1's gain modulated by `source` ("lfo", or "follower" from `from`)
// through modulate, which makes the rack on a new channel, Modulators; plus
// tr_3 "Elsewhere", which nothing the bakes render reads. Returns the
// source module.
std::string sourced(Fixture& f, const char* source, const char* from = nullptr) {
    const auto fx = f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
    json::Value args = json::Value::object();
    args["target"] = json::Value::object();
    args["target"]["fx"] = fx.find("fx")->asString();
    args["target"]["param"] = "gain";
    args["source"] = source;
    if (from) args["from"] = from;
    args["depth"] = 0.2;
    auto made = f.bus->execute({"modulate", std::move(args), origin});
    REQUIRE_MESSAGE(made, (made ? "" : made.error().toString()));
    f.run("track.add", R"({"name":"Elsewhere"})");
    return made->find("module")->asString();
}
// The job `job`'s status once it finished, its events flushed.
json::Value settled(Fixture& f, const json::Value& job) {
    {
        std::unique_lock lock(f.mutex);
        f.done.wait(lock, [&] { return f.finished; });
    }
    f.bus->flushJobEvents();
    auto status = f.bus->jobs().status(job.find("job")->asString());
    REQUIRE(status);
    return *status;
}
// A consolidate of it_1 and its copy it_2, with `edit` made while it renders.
json::Value consolidated(Fixture& f, const std::function<void()>& edit) {
    f.run("item.set", R"({"item":"it_1","length":0.5})");
    f.run("item.duplicate", R"({"item":"it_1","to":2})");
    const auto job = f.run("item.consolidate", R"({"items":["it_1","it_2"]})");
    if (edit) edit();
    return settled(f, job);
}
}  // namespace

TEST_CASE("item.consolidate is stale when the route's source changed during it (round 192 F4)") {
    // The stale check covers what the render read, not only the route: the
    // LFO's rate on another channel's rack changes what the gain printed.
    for (const bool edited : {false, true}) {
        INFO("edited " << edited);
        Fixture f;
        const std::string lfo = sourced(f, "lfo");
        const auto status = consolidated(f, [&] {
            if (!edited) return;
            json::Value set = json::Value::object();
            set["module"] = lfo;
            set["params"] = *json::parse(R"({"rate": 3})");
            REQUIRE(f.bus->execute({"module.set", std::move(set), origin}));
        });
        INFO(json::writeCompact(status));
        if (edited) {
            CHECK(status.find("status")->asString() != "done");
            CHECK(json::writeCompact(status).find("item.consolidate_stale") != std::string::npos);
            CHECK(f.app->head().items.size() == 2);
        } else {
            CHECK(status.find("status")->asString() == "done");
        }
    }
}

TEST_CASE("item.consolidate is stale when a followed track changed during it (round 192 F4)") {
    // A Follower source reads tr_2's signal: an edit to tr_2 is an edit to
    // what the render read. tr_3 is read by nothing: its edit is accepted.
    for (const char* edit : {"followed", "elsewhere"}) {
        INFO(edit);
        Fixture f;
        f.run("track.add", R"({"name":"Followed"})");  // tr_2
        sourced(f, "follower", "tr_2");
        const std::string elsewhere =
            f.app->head().tracks.find(model::kMaster)->children.back().str();
        const auto status = consolidated(f, [&] {
            json::Value set = json::Value::object();
            set["track"] = std::string(edit) == "followed" ? std::string("tr_2") : elsewhere;
            set["volume_db"] = -6.0;
            REQUIRE(f.bus->execute({"track.set", std::move(set), origin}));
        });
        INFO(json::writeCompact(status));
        if (std::string(edit) == "followed") {
            CHECK(status.find("status")->asString() != "done");
            CHECK(json::writeCompact(status).find("item.consolidate_stale") != std::string::npos);
        } else {
            CHECK(status.find("status")->asString() == "done");
        }
    }
}

TEST_CASE("item.consolidate is stale when a Notes source's item moves during it (round 193 R1)") {
    // Other is tr_2; Keys (tr_3) holds a MIDI item; the rack (on Modulators,
    // tr_4, which modulate makes) has a Notes module playing Keys, its sends
    // off, into the Mod Out input that modulates tk_1's gain. Moving that item onto
    // Modulators (in the closure) or onto a new track (outside it) leaves
    // the module playing nothing: both are edits to what the render read.
    // Renumbering the closure's tracks in order (the master, tr_1 and tr_4
    // as tr_1, tr_2 and tr_3; Keys, outside it, kept as written) once hashed
    // the first the same: the item's track went from Keys's raw "tr_3" to
    // Modulators renumbered "tr_3" (P9).
    for (const char* to : {"rack", "new"}) {
        INFO("to " << std::string(to));
        Fixture f;
        f.run("track.add", R"({"name":"Other"})");  // tr_2
        f.run("track.add", R"({"name":"Keys"})");   // tr_3
        const std::string keys = f.app->head().tracks.find(model::kMaster)->children.back().str();
        REQUIRE(keys == "tr_3");
        const auto fx = f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
        json::Value args = *json::parse(R"({"source":"lfo","depth":0.2})");
        args["target"] = json::Value::object();
        args["target"]["fx"] = fx.find("fx")->asString();
        args["target"]["param"] = "gain";
        auto made = f.bus->execute({"modulate", std::move(args), origin});
        REQUIRE_MESSAGE(made, (made ? "" : made.error().toString()));
        const std::string rack = made->find("rack")->asString();
        const std::string modOut = made->find("mod_out")->asString();
        const std::string input = made->find("input")->asString();
        json::Value item = *json::parse(R"({"bar":1,"notation":"voice v; 1: C4:w"})");
        item["track"] = keys;
        const auto exec = [&](const char* name, json::Value request) {
            auto r = f.bus->execute({name, std::move(request), origin});
            REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
            return *r;
        };
        const std::string midi = exec("item.add", item).find("item")->asString();
        json::Value notes = *json::parse(R"({"type":"notes"})");
        notes["rack"] = rack;
        notes["source"] = keys;
        const std::string module = exec("module.add", notes).find("module")->asString();
        // Without its sends the source track is not in the closure, only its
        // items (with sends, the default, it is, and nothing collided).
        json::Value sends = *json::parse(R"({"source_sends":false})");
        sends["module"] = module;
        exec("module.set", sends);
        json::Value cable = json::Value::object();
        cable["from"] = module + ".gate";
        cable["to"] = modOut + "." + input;
        exec("cable.add", cable);
        std::string target;  // the rack's track
        for (const auto& e : f.app->head().racks)
            if (e.id.str() == rack) target = e.value->track.str();
        REQUIRE(target == "tr_4");
        if (std::string(to) == "new") {
            f.run("track.add", R"({"name":"Other"})");
            target = f.app->head().tracks.find(model::kMaster)->children.back().str();
        }
        const auto status = consolidated(f, [&] {
            json::Value move = json::Value::object();
            move["item"] = midi;
            move["track"] = target;
            REQUIRE(f.bus->execute({"item.move", std::move(move), origin}));
        });
        INFO(json::writeCompact(status));
        CHECK(status.find("status")->asString() != "done");
        CHECK(json::writeCompact(status).find("item.consolidate_stale") != std::string::npos);
    }
}

TEST_CASE("item.mix-to-track is not stale for an edit its render does not read (round 193)") {
    // A track added (and another's volume) during the mix: its private view's
    // buss and routes take numbers from counters that creation moves, so they
    // are hashed in a range of their own; the result commits.
    Fixture f;
    sourced(f, "lfo");
    const std::string elsewhere = f.app->head().tracks.find(model::kMaster)->children.back().str();
    const auto job = f.run("item.mix-to-track", R"({"items":["it_1"]})");
    f.run("track.add", R"({"name":"Later"})");
    json::Value set = json::Value::object();
    set["track"] = elsewhere;
    set["volume_db"] = -6.0;
    REQUIRE(f.bus->execute({"track.set", std::move(set), origin}));
    const auto status = settled(f, job);
    INFO(json::writeCompact(status));
    CHECK(status.find("status")->asString() == "done");
}

TEST_CASE(
    "item.mix-to-track is stale when a warp peer of its take changes during it "
    "(round 195 F3)") {
    // it_1 and its copy it_2, moved onto Peer (tr_2), quantized together:
    // tk_1 and tk_2 are warp peers, and the render of it_1 reads tk_2's
    // bundle (offline.cpp keeps a selected take's peers). tr_2 is outside
    // the mix's closure, so only the peers' hashing sees an edit to tk_2's
    // transients; without it the mix committed over the edit.
    for (const bool edited : {false, true}) {
        INFO("edited " << edited);
        Fixture f;
        f.run("track.add", R"({"name":"Peer"})");  // tr_2
        f.run("take.hits.set", R"({"take":"tk_1","positions":[0.3,0.8]})");
        f.run("item.duplicate", R"({"item":"it_1","to":0})");
        f.run("item.move", R"({"item":"it_2","track":"tr_2"})");
        f.run("audio.quantize", R"({"items":["it_1","it_2"],"grid":0.25})");
        const model::Session before = f.app->head();
        REQUIRE(before.takes.find({model::Kind::Take, 1})->warpPeers.size() == 2);
        REQUIRE(before.items.find({model::Kind::Item, 2})->track ==
                model::Id{model::Kind::Track, 2});
        const auto job = f.run("item.mix-to-track", R"({"items":["it_1"]})");
        if (edited) f.run("take.hits.set", R"({"take":"tk_2","positions":[0.2,0.6]})");
        const auto status = settled(f, job);
        INFO(json::writeCompact(status));
        if (edited) {
            CHECK(status.find("status")->asString() != "done");
            CHECK(json::writeCompact(status).find("item.mix_stale") != std::string::npos);
        } else {
            CHECK(status.find("status")->asString() == "done");
        }
    }
}

TEST_CASE("item.mix-to-track is stale when the route's source changed during it (round 192 F4)") {
    for (const bool edited : {false, true}) {
        INFO("edited " << edited);
        Fixture f;
        const std::string lfo = sourced(f, "lfo");
        const auto job = f.run("item.mix-to-track", R"({"items":["it_1"]})");
        if (edited) {
            json::Value set = json::Value::object();
            set["module"] = lfo;
            set["params"] = *json::parse(R"({"rate": 3})");
            REQUIRE(f.bus->execute({"module.set", std::move(set), origin}));
        }
        const auto status = settled(f, job);
        INFO(json::writeCompact(status));
        if (edited)
            CHECK(json::writeCompact(status).find("item.mix_stale") != std::string::npos);
        else
            CHECK(status.find("status")->asString() == "done");
    }
}

TEST_CASE("item.mix-to-track prints a modulated take FX as it plays (M5, F10)") {
    // One modulated item through its strip: with Console 9 off the mix is
    // that strip's stem, the take's gain moving as it does in playback.
    Fixture f;
    beatModulated(f);
    const auto& head = f.app->head();
    const auto span = model::itemSpan(head, *head.items.find(*model::Id::parse("it_1")));
    OfflineSettings settings;
    settings.applyMasterPlayrate = false;
    settings.track = *model::Id::parse("tr_1");
    settings.startSeconds = span.startSec;
    settings.endSeconds = span.endSec;
    const auto stem = f.app->renderMix(settings).value().audio;
    const auto result = f.finish(f.run("item.mix-to-track", R"({"items":["it_1"]})"));
    const auto mix = audio::readAudio(result.find("file")->asString()).value();
    REQUIRE(mix.channelCount() == 2);
    REQUIRE(stem.frames() == mix.frames());
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < mix.frames(); ++i)
            REQUIRE(double(mix.channels[c][i]) ==
                    doctest::Approx(double(stem.channels[c][i])).epsilon(1e-5));
    // The source keeps its route: mix-to-track leaves the item as it is.
    CHECK(f.app->head().modulations.size() == 1);
}
