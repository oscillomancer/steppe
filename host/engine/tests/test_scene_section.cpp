// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Scene <-> section over the bus (ADR-0097, step L4a): scene.insert,
// scene.capture and arrangement.build, their refusals, one undo step each,
// save and reopen, and the round trip between a scene and a section.
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>
#include <supple/json/write.h>
#include <supple/model/launcher.h>
#include <supple/model/schemas.h>
#include <supple/model/take_owner.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include "../../../tests/file_platform.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "scene section", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }
model::Id id(const char* text) { return *model::Id::parse(text); }

// Channels tr_1 "Bass" (it_1: four quarter notes over bar 1) and tr_2 "Pad",
// and scenes sc_1 "Verse" and sc_2 "Chorus".
struct Song {
    fs::path root = supple::test::uniqueTempDir("supple-scene-");
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;

    Song() {
        fs::remove_all(root);
        fs::create_directories(root);
        AppOptions options;
        options.workdir = root;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto made = EngineApp::create(options, {.console9 = false});
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        run("track.add", R"({"name": "Bass"})");
        run("track.add", R"({"name": "Pad"})");
        run("item.add",
            R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q E2:q G2:q C3:q"})");
        run("scene.add", R"({"name": "Verse"})");
        run("scene.add", R"({"name": "Chorus"})");
    }
    ~Song() { fs::remove_all(root); }

    json::Value run(std::string name, std::string_view args) {
        auto result = bus->execute({std::move(name), j(args), kOrigin});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    }
    json::Value runArgs(std::string name, const json::Value& args) {
        auto result = bus->execute({std::move(name), args, kOrigin});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    }
    // A refusal that leaves the project exactly as it was.
    Error fails(std::string name, std::string_view args) {
        const std::string before = model::serializeProject(head());
        auto result = bus->execute({std::move(name), j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().hint.empty());
        CHECK_FALSE(result.error().message.empty());
        CHECK(model::serializeProject(head()) == before);
        return result.error();
    }
    const model::Session& head() const { return app->head(); }
    std::string describe() { return run("session.describe", "{}")["text"].asString(); }
    // A mono WAV at 48 kHz.
    fs::path wav(const char* name, double seconds, float level = .25f) {
        audio::AudioData a;
        a.sampleRate = 48000;
        a.channels.assign(1, std::vector<float>(static_cast<std::size_t>(seconds * 48000), level));
        const auto path = root / name;
        REQUIRE(audio::writeWav(path, a));
        return path;
    }
    // Clip cl_1 on Bass (four beats, loop mode, loop off) and cl_2 on Pad (a
    // once clip of two beats) in the Verse.
    void verseClips() {
        run("clip.add", R"({"track": "tr_1", "scene": "sc_1",
            "notation": "voice bass; 1: C2:q E2:q G2:q C3:q"})");
        run("clip.add", R"({"track": "tr_2", "scene": "sc_1",
            "notation": "voice pad; 1: C3:h r:h"})");
        run("clip.set", R"({"clip": "cl_2", "mode": "once", "length": 2})");
    }
    std::vector<model::Id> itemsOn(const char* track) const {
        std::vector<model::Id> out;
        for (const auto& e : head().items)
            if (e.value->track == id(track)) out.push_back(e.id);
        return out;
    }
};

// A copy: json::Value indexes by key on a non-const value only.
json::Value at(const json::Value& list, std::size_t k) { return list.asArray()[k]; }

double startBeat(const Song& s, model::Id item) {
    return model::itemSpan(s.head(), *s.head().items.find(item)).startBeat;
}
double lengthBeats(const Song& s, model::Id item) {
    return model::itemLengthBeats(s.head(), *s.head().items.find(item));
}

}  // namespace

// --- scene.insert -------------------------------------------------------------------

TEST_CASE("scene.insert lays a loop clip over the span and a once clip for its length") {
    Song l;
    l.verseClips();
    auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 4})");
    CHECK(r["start"].asDouble() == 8.0);
    CHECK(r["end"].asDouble() == 24.0);
    REQUIRE(r["items"].size() == 2);
    CHECK(at(r["items"], 0)["clip"].asString() == "cl_1");  // track display order: Bass, Pad
    CHECK(at(r["items"], 0)["track"].asString() == "tr_1");
    CHECK(at(r["items"], 1)["clip"].asString() == "cl_2");

    const model::Id bass = id(at(r["items"], 0)["item"].asString().c_str());
    const model::Id pad = id(at(r["items"], 1)["item"].asString().c_str());
    CHECK(startBeat(l, bass) == 8.0);
    CHECK(lengthBeats(l, bass) == 16.0);  // the clip repeats: one looping item for the span
    CHECK(l.head().items.find(bass)->loop);
    CHECK(startBeat(l, pad) == 8.0);
    CHECK(lengthBeats(l, pad) == 2.0);  // a once clip plays its length, the rest is silent
    CHECK_FALSE(l.head().items.find(pad)->loop);

    // The section: named after the scene, over the span.
    const model::Region& region = *l.head().regions.find(id(r["region"].asString().c_str()));
    CHECK(region.name == "Verse");
    CHECK(region.start == 8.0);
    CHECK(region.end == 24.0);
    CHECK(region.section);

    // MIDI is copied, not pooled; the clips keep their own takes.
    const model::Take& itemTake = *l.head().takes.find(*l.head().items.find(bass)->activeTake);
    const model::Take& clipTake =
        *l.head().takes.find(*l.head().clips.find(id("cl_1"))->activeTake);
    CHECK(*itemTake.source != *clipTake.source);
    CHECK_FALSE(l.head().midi.find(*itemTake.source)->pooled);
    CHECK(l.head().clips.size() == 2);
}

TEST_CASE("scene.insert pooled shares the MIDI, and name names the section") {
    Song l;
    l.verseClips();
    auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 1, "pooled": true,
        "name": "Intro"})");
    const model::Id bass = id(at(r["items"], 0)["item"].asString().c_str());
    const auto source = *l.head().takes.find(*l.head().items.find(bass)->activeTake)->source;
    CHECK(source == *l.head().takes.find(*l.head().clips.find(id("cl_1"))->activeTake)->source);
    CHECK(l.head().midi.find(source)->pooled);
    CHECK(l.head().regions.find(id(r["region"].asString().c_str()))->name == "Intro");
}

TEST_CASE("scene.insert counts bars through the meter map, and a position is a bar line") {
    Song l;
    l.verseClips();
    l.run("timesig.set", R"({"bar": 3, "meter": "3/4"})");
    // Bars 2 (4 beats), 3 and 4 (3 beats each): bar 5 begins at beat 14.
    auto r = l.run("scene.insert", R"({"scene": "sc_1", "at": 4, "bars": 3})");
    CHECK(r["start"].asDouble() == 4.0);
    CHECK(r["end"].asDouble() == 14.0);
    CHECK(l.fails("scene.insert", R"({"scene": "sc_1", "at": 15, "bars": 1})").code == "scene.at");
}

TEST_CASE("scene.insert is one undo step, and survives save and reopen") {
    Song l;
    l.verseClips();
    const std::string before = l.describe();
    l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");
    CHECK(l.head().items.size() == 3);
    CHECK(l.head().regions.size() == 1);
    const std::string after = l.describe();
    l.run("history.undo", "{}");
    CHECK(l.head().items.size() == 1);
    CHECK(l.head().regions.empty());
    CHECK(l.describe() == before);  // ids already used are not reused, so compare content
    l.run("history.redo", "{}");
    CHECK(l.head().items.size() == 3);
    CHECK(l.head().regions.size() == 1);
    CHECK(l.describe() == after);
    // Save and reopen keep the items, the section and the scenes.
    // The path goes in as a JSON value, not pasted into text: a Windows path has backslashes.
    auto file = json::Value::object();
    file["path"] = (l.root / "song.json").string();
    l.runArgs("project.save", file);
    l.runArgs("project.open", file);
    CHECK(l.describe() == after);
    CHECK(l.head().items.size() == 3);
    CHECK(l.head().regions.size() == 1);
}

TEST_CASE("scene.insert takes a clip as long as the span, a clip that repeats, and one longer") {
    Song l;
    l.run("clip.add", R"({"track": "tr_2", "scene": "sc_1",
        "notation": "voice pad; 1: C3:q E3:q G3:q C4:q"})");
    SUBCASE("a clip no longer than the span repeats when its period is its content") {
        auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 8})");
        const model::Id item = id(at(r["items"], 0)["item"].asString().c_str());
        CHECK(lengthBeats(l, item) == 32.0);
        CHECK(l.head().items.find(item)->loop);
    }
    SUBCASE("a longer clip with its loop off plays its content once, and the span cuts it") {
        l.run("clip.set", R"({"clip": "cl_1", "length": 16})");
        auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");
        const model::Id item = id(at(r["items"], 0)["item"].asString().c_str());
        CHECK(lengthBeats(l, item) == 8.0);
        CHECK_FALSE(l.head().items.find(item)->loop);
    }
    SUBCASE("a period with a gap after the content is refused when it must repeat") {
        l.run("clip.set", R"({"clip": "cl_1", "length": 8})");
        const Error e = l.fails("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 8})");
        CHECK(e.code == "scene.insert.period");
        CHECK(e.path == "/args/scene");
        // The same clip with its loop on over the whole content repeats exactly.
        l.run("clip.set", R"({"clip": "cl_1", "loop": {"on": true, "start": 0, "length": 4}})");
        auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 8})");
        CHECK(lengthBeats(l, id(at(r["items"], 0)["item"].asString().c_str())) == 32.0);
        // Within one period it needs no repeat at all.
        l.run("clip.set", R"({"clip": "cl_1", "loop": {"on": false}})");
        l.run("scene.insert", R"({"scene": "sc_1", "bar": 41, "bars": 2})");
    }
    SUBCASE("a loop window an item cannot express is refused as item.add {clip} refuses it") {
        l.run("clip.set",
              R"({"clip": "cl_1", "length": 8, "loop": {"on": true, "start": 1, "length": 2}})");
        CHECK(l.fails("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})").code ==
              "clip.unsupported");
    }
}

TEST_CASE("scene.insert lays audio clips in seconds") {
    Song l;
    auto add = j(R"({"track": "tr_2", "scene": "sc_1"})");
    add["audio_file"] = l.wav("pad.wav", 2.0).string();
    l.runArgs("clip.add", add);  // cl_1: 2 s, loop mode, loop off, content 2 s
    auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");  // 4 s
    const model::Item& item =
        *l.head().items.find(id(at(r["items"], 0)["item"].asString().c_str()));
    const auto span = model::itemSpan(l.head(), item);
    CHECK(span.startSec == doctest::Approx(4.0));
    CHECK(span.endSec - span.startSec == doctest::Approx(4.0));  // repeats: 2 s twice
    CHECK(item.loop);
    l.run("clip.set", R"({"clip": "cl_1", "mode": "once"})");
    auto once = l.run("scene.insert", R"({"scene": "sc_1", "bar": 9, "bars": 2})");
    const auto s2 = model::itemSpan(
        l.head(), *l.head().items.find(id(at(once["items"], 0)["item"].asString().c_str())));
    CHECK(s2.endSec - s2.startSec == doctest::Approx(2.0));
}

TEST_CASE("scene.insert refuses what it cannot place, and changes nothing") {
    Song l;
    l.verseClips();
    SUBCASE("an item already in the span of a track that gets a clip") {
        const Error e = l.fails("scene.insert", R"({"scene": "sc_1", "bar": 1, "bars": 2})");
        CHECK(e.code == "scene.insert.occupied");
        CHECK(e.message.find("it_1") != std::string::npos);
        CHECK(e.message.find("tr_1") != std::string::npos);
        CHECK(e.path == "/args/bar");
    }
    SUBCASE("an item that ends where the span starts is not in it") {
        l.run("scene.insert", R"({"scene": "sc_1", "bar": 2, "bars": 1})");
    }
    SUBCASE("an item on a track that gets no clip is left alone") {
        l.run("item.add", R"({"track": "tr_2", "bar": 3,
            "notation": "voice pad; 1: C3:w"})");
        l.run("clip.remove", R"({"clip": "cl_2"})");
        l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 1})");
    }
    SUBCASE("an empty scene") {
        const Error e = l.fails("scene.insert", R"({"scene": "sc_2", "bar": 3, "bars": 1})");
        CHECK(e.code == "scene.empty");
        CHECK(e.path == "/args/scene");
    }
    SUBCASE("where it starts must be said once, and on a bar line") {
        CHECK(l.fails("scene.insert", R"({"scene": "sc_1", "bars": 1})").code == "scene.at");
        CHECK(l.fails("scene.insert", R"({"scene": "sc_1", "at": 8, "bar": 3, "bars": 1})").code ==
              "scene.at");
        const Error off = l.fails("scene.insert", R"({"scene": "sc_1", "at": 9, "bars": 1})");
        CHECK(off.code == "scene.at");
        CHECK(off.hint.find("bar 3") != std::string::npos);
    }
    SUBCASE("a scene or a number of bars that is not there") {
        CHECK(l.fails("scene.insert", R"({"scene": "sc_9", "bar": 3, "bars": 1})").code ==
              "sc.unknown");
        (void)l.bus->execute(
            {"scene.insert", j(R"({"scene": "sc_1", "bar": 3, "bars": 0})"), kOrigin});
        CHECK(l.head().regions.empty());
    }
    SUBCASE("a frozen track") {
        l.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
        l.run("track.freeze", R"({"track": "tr_1"})");
        CHECK(l.fails("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 1})").code ==
              "track.frozen");
    }
}

// --- scene.capture ------------------------------------------------------------------

TEST_CASE("scene.capture makes a scene from the items of a section, one clip per track") {
    Song l;
    l.run("region.add", R"({"start": 0, "end": 4, "name": "Hook", "color": "#112233"})");
    l.run("item.add", R"({"track": "tr_2", "position": 0, "notation": "voice pad; 1: C3:h r:h"})");
    const std::string itemsBefore = l.describe();
    auto r = l.run("scene.capture", R"({"region": "rg_1"})");
    const model::Scene& scene = *l.head().scenes.find(id(r["scene"].asString().c_str()));
    CHECK(scene.name == "Hook");
    CHECK(scene.color == "#112233");
    CHECK(scene.order == 2);  // after Verse and Chorus
    REQUIRE(r["clips"].size() == 2);
    const model::Clip& bass =
        *l.head().clips.find(id(at(r["clips"], 0)["clip"].asString().c_str()));
    const model::Clip& pad = *l.head().clips.find(id(at(r["clips"], 1)["clip"].asString().c_str()));
    CHECK(at(r["clips"], 0)["source_item"].asString() == "it_1");
    CHECK(bass.track == id("tr_1"));
    CHECK(bass.length == 4.0);
    CHECK(bass.mode == model::ClipMode::Loop);  // the item fills the section
    CHECK_FALSE(bass.loop.on);
    CHECK(pad.mode == model::ClipMode::Loop);
    // MIDI is copied: the item's takes are untouched, the sources differ.
    CHECK(*l.head().takes.find(bass.takes[0])->source !=
          *l.head().takes.find(l.head().items.find(id("it_1"))->takes[0])->source);
    // The region and the items are unchanged (describe lists them, and the scenes).
    CHECK(l.head().regions.size() == 1);
    CHECK(l.head().items.size() == 2);
    CHECK(l.describe().substr(0, l.describe().find("Scenes")) ==
          itemsBefore.substr(0, itemsBefore.find("Scenes")));
    l.run("history.undo", "{}");
    CHECK(l.head().scenes.size() == 2);
    CHECK(l.head().clips.empty());
}

TEST_CASE("scene.capture gives each clip the context under its item (ADR-0098)") {
    Song l;
    l.run("key.set", R"({"at": 0, "tonic": "D", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Dm"})");
    l.run("chord.set", R"({"at": 2, "symbol": "A7"})");
    l.run("region.add", R"({"start": 0, "end": 4, "name": "Hook"})");
    l.run("item.add", R"({"track": "tr_2", "position": 0, "notation": "voice pad; 1: C3:h r:h"})");
    auto r = l.run("scene.capture", R"({"region": "rg_1"})");
    const model::Clip& bass =
        *l.head().clips.find(id(at(r["clips"], 0)["clip"].asString().c_str()));
    const model::Clip& pad = *l.head().clips.find(id(at(r["clips"], 1)["clip"].asString().c_str()));
    // Each clip remembers what its item sat under, from the item's start, in clip beats.
    for (const model::Clip* c : {&bass, &pad}) {
        REQUIRE(c->context.size() == 3);
        CHECK(c->context[0].type == model::ContextType::Key);
        CHECK(c->context[0].tonic == "D");
        CHECK(c->context[1].symbol == "Dm");
        CHECK(c->context[2].symbol == "A7");
        CHECK(c->context[2].beat == 2.0);
        CHECK(c->follow == model::ClipFollow::None);
    }
    // An audio item gives its clip the key alone.
    auto args = j(R"({"track": "tr_2", "position": 8})");
    args["audio_file"] = l.wav("pad.wav", 2.0).string();
    l.runArgs("item.add", args);
    l.run("region.add", R"({"start": 8, "end": 12, "name": "Wash"})");
    auto audio = l.run("scene.capture", R"({"region": "rg_2"})");
    const model::Clip& wash =
        *l.head().clips.find(id(at(audio["clips"], 0)["clip"].asString().c_str()));
    CHECK(wash.kind == model::ClipKind::Audio);
    REQUIRE(wash.context.size() == 1);
    CHECK(wash.context[0].type == model::ContextType::Key);
    // With no context track there is nothing to take.
    Song bare;
    bare.run("region.add", R"({"start": 0, "end": 4, "name": "Hook"})");
    auto none = bare.run("scene.capture", R"({"region": "rg_1"})");
    CHECK(bare.head()
              .clips.find(id(at(none["clips"], 0)["clip"].asString().c_str()))
              ->context.empty());
}

TEST_CASE("scene.capture: a shorter item is a once clip, a looped item a looped clip") {
    Song l;
    l.run("region.add", R"({"start": 0, "end": 8, "name": "Verse 2"})");
    auto once = l.run("scene.capture", R"({"region": "rg_1"})");
    const model::Clip& c =
        *l.head().clips.find(id(at(once["clips"], 0)["clip"].asString().c_str()));
    CHECK(c.mode == model::ClipMode::Once);  // it played four beats of eight, then fell silent
    CHECK(c.length == 4.0);

    l.run("item.set", R"({"item": "it_1", "loop": true, "length": 8})");
    auto looped = l.run("scene.capture", R"({"region": "rg_1", "name": "Looped"})");
    const model::Clip& m =
        *l.head().clips.find(id(at(looped["clips"], 0)["clip"].asString().c_str()));
    CHECK(m.mode == model::ClipMode::Loop);
    CHECK(m.length == 8.0);
    CHECK(m.loop == model::ClipLoop{true, 0.0, 4.0});
    CHECK(l.head().scenes.find(id(looped["scene"].asString().c_str()))->name == "Looped");
}

TEST_CASE("scene.capture pooled shares the MIDI") {
    Song l;
    l.run("region.add", R"({"start": 0, "end": 4})");
    auto r = l.run("scene.capture", R"({"region": "rg_1", "pooled": true})");
    const model::Clip& c = *l.head().clips.find(id(at(r["clips"], 0)["clip"].asString().c_str()));
    CHECK(*l.head().takes.find(c.takes[0])->source ==
          *l.head().takes.find(l.head().items.find(id("it_1"))->takes[0])->source);
    CHECK(l.head().midi.find(*l.head().takes.find(c.takes[0])->source)->pooled);
}

TEST_CASE("scene.capture refuses several items, an item on an edge, an offset, nothing") {
    Song l;
    SUBCASE("several items of one track inside the section") {
        l.run("item.add", R"({"track": "tr_1", "bar": 2, "notation": "voice bass; 1: C2:w"})");
        l.run("region.add", R"({"start": 0, "end": 8})");
        const Error e = l.fails("scene.capture", R"({"region": "rg_1"})");
        CHECK(e.code == "scene.capture.items");
        CHECK(e.message.find("it_1") != std::string::npos);
        CHECK(e.message.find("it_2") != std::string::npos);
        CHECK(e.path == "/args/region");
    }
    SUBCASE("an item straddling the section's edge") {
        l.run("region.add", R"({"start": 2, "end": 8})");  // it_1 runs 0..4
        const Error e = l.fails("scene.capture", R"({"region": "rg_1"})");
        CHECK(e.code == "scene.capture.edge");
        CHECK(e.message.find("start") != std::string::npos);
        l.run("region.add", R"({"start": 0, "end": 2})");
        CHECK(l.fails("scene.capture", R"({"region": "rg_2"})").message.find("end") !=
              std::string::npos);
    }
    SUBCASE("an item that starts after the section does") {
        l.run("item.add",
              R"({"track": "tr_2", "position": 2, "notation": "voice pad; 1: C3:h r:h"})");
        l.run("region.add", R"({"start": 0, "end": 8})");  // the pad runs 2..6
        const Error e = l.fails("scene.capture", R"({"region": "rg_1"})");
        CHECK(e.code == "scene.capture.offset");
        CHECK(e.message.find("it_2") != std::string::npos);
    }
    SUBCASE("a section with no item") {
        l.run("region.add", R"({"start": 16, "end": 20})");
        CHECK(l.fails("scene.capture", R"({"region": "rg_1"})").code == "scene.capture.empty");
    }
    SUBCASE("an item that only touches the section is not in it") {
        l.run("region.add", R"({"start": 4, "end": 8})");  // it_1 ends where it starts
        CHECK(l.fails("scene.capture", R"({"region": "rg_1"})").code == "scene.capture.empty");
    }
    SUBCASE("a region that is not there") {
        CHECK(l.fails("scene.capture", R"({"region": "rg_7"})").path == "/args/region");
    }
}

TEST_CASE("scene.capture skips a master's items with a warning, and refuses a warped take") {
    Song l;
    SUBCASE("master") {
        auto add = j(R"({"track": "tr_0", "bar": 1})");
        add["audio_file"] = l.wav("print.wav", 2.0).string();
        l.runArgs("item.add", add);  // it_2 on the master, over bars 1..2
        l.run("region.add", R"({"start": 0, "end": 4})");
        auto r = l.run("scene.capture", R"({"region": "rg_1"})");
        CHECK(r["clips"].size() == 1);
        bool warned = false;
        for (auto w : r["warnings"].asArray())
            warned |= w["code"].asString() == "scene.capture.skipped";
        CHECK(warned);
    }
    SUBCASE("warped") {
        auto add = j(R"({"track": "tr_2", "bar": 3})");
        add["audio_file"] = l.wav("hits.wav", 2.0).string();
        l.runArgs("item.add", add);  // it_2 over beats 8..12
        l.run("take.set",
              R"({"take": "tk_2", "preserve_pitch": true, "pitch_mode": "signalsmith"})");
        l.run("take.stretch.add", R"({"take": "tk_2", "item_pos": 0.5})");
        l.run("region.add", R"({"start": 8, "end": 12})");
        CHECK(l.fails("scene.capture", R"({"region": "rg_1"})").code == "clip.unsupported");
        CHECK(l.head().scenes.size() == 2);
    }
}

TEST_CASE("scene.capture of an audio item makes an audio clip in seconds") {
    Song l;
    auto add = j(R"({"track": "tr_2", "bar": 3})");
    add["audio_file"] = l.wav("loop.wav", 2.0).string();
    l.runArgs("item.add", add);  // beats 8..12 at 120 BPM
    l.run("region.add", R"({"start": 8, "end": 12})");
    auto r = l.run("scene.capture", R"({"region": "rg_1"})");
    const model::Clip& c = *l.head().clips.find(id(at(r["clips"], 0)["clip"].asString().c_str()));
    CHECK(c.kind == model::ClipKind::Audio);
    CHECK(c.length == doctest::Approx(2.0));
    CHECK(c.mode == model::ClipMode::Loop);
}

TEST_CASE("scene.capture refuses a project that already has the most scenes") {
    Song l;
    l.run("region.add", R"({"start": 0, "end": 4})");
    for (std::size_t k = 2; k < model::kMaxScenes; ++k) l.run("scene.add", "{}");
    REQUIRE(l.head().scenes.size() == model::kMaxScenes);
    const Error full = l.fails("scene.capture", R"({"region": "rg_1"})");
    CHECK(full.code == "scene.limit");
    CHECK(full.message.find("1024") != std::string::npos);
    // One scene fewer and it is captured.
    l.run("scene.remove", R"({"scene": "sc_2"})");
    auto r = l.run("scene.capture", R"({"region": "rg_1"})");
    CHECK(l.head().scenes.size() == model::kMaxScenes);
    CHECK(at(r["clips"], 0)["track"].asString() == "tr_1");
}

TEST_CASE("scene.capture refuses a capture that would take the project past the most clips") {
    // 17 channels and 964 scenes hold the most clips, 16384, in cells of their own
    // (the captured scene would be the 965th, still under the scene limit). Making
    // that many through the bus takes minutes, so the clips go into a saved project
    // with their takes and empty MIDI sources, and the project is reopened.
    Song l;
    for (int k = 2; k < 17; ++k) l.run("track.add", "{}");
    for (std::size_t k = 2; k < 964; ++k) l.run("scene.add", "{}");
    const auto path = l.root / "full-clips.steppe";
    auto save = json::Value::object();
    save["path"] = path.string();
    l.runArgs("project.save", save);
    auto doc = json::parse(*readTextFile(path)).value();
    auto& ids = doc["project"]["next_ids"];
    auto next = [&](const char* kind) {  // a kind nothing was made of yet is not listed
        const json::Value* v = ids.find(kind);
        return v ? v->asInt() : std::int64_t{1};
    };
    std::int64_t clip = next("cl"), take = next("tk"), midi = next("mi");
    std::size_t made = 0;
    for (std::size_t scene = 1; scene <= 964 && made < model::kMaxClips; ++scene)
        for (int track = 1; track <= 17 && made < model::kMaxClips; ++track, ++made, ++clip) {
            const std::string t = "tk_" + std::to_string(take++),
                              m = "mi_" + std::to_string(midi++);
            auto row = json::Value::object();
            row["track"] = "tr_" + std::to_string(track);
            row["scene"] = "sc_" + std::to_string(scene);
            row["kind"] = "midi";
            auto takes = json::Value::array();
            takes.push_back(t);
            row["takes"] = takes;
            row["active_take"] = t;
            row["length"] = 4.0;
            doc["clips"]["cl_" + std::to_string(clip)] = row;
            auto tk = json::Value::object();
            tk["source"] = m;
            doc["takes"][t] = tk;
            doc["sources"]["midi"][m] =
                j(R"({"sequence": {"ppq": 960, "length": 3840, "notes": []}})");
        }
    ids["cl"] = clip;
    ids["tk"] = take;
    ids["mi"] = midi;
    REQUIRE(writeFileAtomic(path, std::string_view(json::writeCanonical(doc))));
    l.runArgs("project.open", save);
    REQUIRE(l.head().clips.size() == model::kMaxClips);
    l.run("region.add", R"({"start": 0, "end": 4})");
    const Error full = l.fails("scene.capture", R"({"region": "rg_1"})");
    CHECK(full.code == "clip.limit");
    CHECK(full.message.find("16384") != std::string::npos);
    // The same limit holds for the other command that makes a clip from nothing.
    CHECK(l.fails("clip.add", R"({"track": "tr_14", "scene": "sc_964"})").code == "clip.limit");
    // A clip fewer and the capture goes through: it makes one clip, for the one item.
    l.run("clip.remove", R"({"clip": "cl_1"})");
    auto r = l.run("scene.capture", R"({"region": "rg_1"})");
    CHECK(l.head().clips.size() == model::kMaxClips);
    CHECK(r["clips"].asArray().size() == 1);
}

TEST_CASE("scene.capture refuses when a clip's audio would not fit beside the arrangement's") {
    // The estimate reads the model, so hand-edited lengths (2^26 frames each) make
    // the arrangement's audio and one clip's fill the 2^28-sample budget exactly,
    // each decoded and resampled once. A capture copies an item's takes, which add
    // nothing of their own; what it checks is the whole session, so new audio on
    // the timeline that pushes a clip out of the budget is refused at the capture.
    Song l;
    auto args = j(R"({"track": "tr_2", "bar": 3})");
    args["audio_file"] = l.wav("a.wav", 1.0).string();
    l.runArgs("item.add", args);
    args = j(R"({"track": "tr_2", "scene": "sc_1"})");
    args["audio_file"] = l.wav("b.wav", 1.0, .5f).string();
    l.runArgs("clip.add", args);
    const auto path = l.root / "full.steppe";
    auto save = json::Value::object();
    save["path"] = path.string();
    l.runArgs("project.save", save);
    auto doc = json::parse(*readTextFile(path)).value();
    for (auto& m : doc["sources"]["media"].asObject()) m.value["frames"] = 1 << 26;
    REQUIRE(writeFileAtomic(path, std::string_view(json::writeCanonical(doc))));
    l.runArgs("project.open", save);
    auto resident = residentSamples(l.head(), l.app->projectDir(), 48000);
    REQUIRE(resident.arrangement == double(1 << 27));
    REQUIRE(resident.clips == double(1 << 27));
    REQUIRE(resident.clipsSkipped.empty());
    // A second file on the timeline, a real second long: the clip no longer fits.
    args = j(R"({"track": "tr_1", "bar": 3})");
    args["audio_file"] = l.wav("c.wav", 1.0, .125f).string();
    l.runArgs("item.add", args);
    resident = residentSamples(l.head(), l.app->projectDir(), 48000);
    REQUIRE_FALSE(resident.clipsSkipped.empty());
    l.run("region.add", R"({"start": 8, "end": 12})");
    const Error refused = l.fails("scene.capture", R"({"region": "rg_1"})");
    CHECK(refused.code == "clip.memory");
    CHECK(refused.path == "/args/region");
    CHECK(refused.message.find("268435456") != std::string::npos);
    CHECK(l.head().scenes.size() == 2);
}

// --- the round trip -----------------------------------------------------------------

TEST_CASE("a scene laid on the timeline and captured again comes back as the same clips") {
    Song l;
    l.verseClips();  // cl_1: Bass, 4 beats, loop off; cl_2: Pad, once, 2 beats
    // A span as long as cl_1 (one bar): both come back as they were.
    l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 1})");
    auto first = l.run("scene.capture", R"({"region": "rg_1"})");
    const auto clipOf = [&](const json::Value& r, int k) -> const model::Clip& {
        return *l.head().clips.find(
            id(at(*r.find("clips"), static_cast<std::size_t>(k))["clip"].asString().c_str()));
    };
    CHECK(clipOf(first, 0).length == 4.0);
    CHECK(clipOf(first, 0).mode == model::ClipMode::Loop);
    CHECK_FALSE(clipOf(first, 0).loop.on);
    CHECK(clipOf(first, 1).length == 2.0);  // the once clip shorter than the span
    CHECK(clipOf(first, 1).mode == model::ClipMode::Once);

    // A looped clip over a longer span: one looped clip as long as the span.
    l.run("scene.insert", R"({"scene": "sc_1", "bar": 11, "bars": 4})");
    auto second = l.run("scene.capture", R"({"region": "rg_2"})");
    CHECK(clipOf(second, 0).length == 16.0);
    CHECK(clipOf(second, 0).mode == model::ClipMode::Loop);
    CHECK(clipOf(second, 0).loop == model::ClipLoop{true, 0.0, 4.0});

    // A clip longer than the span comes back cut to it, as a loop clip.
    l.run("clip.set", R"({"clip": "cl_1", "length": 16})");
    l.run("scene.insert", R"({"scene": "sc_1", "bar": 31, "bars": 1})");
    auto third = l.run("scene.capture", R"({"region": "rg_3"})");
    CHECK(clipOf(third, 0).length == 4.0);
    CHECK(clipOf(third, 0).mode == model::ClipMode::Loop);
}

// --- arrangement.build --------------------------------------------------------------

TEST_CASE("arrangement.build lays scenes one after the other, each with its section") {
    Song l;
    l.verseClips();
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2",
        "notation": "voice bass; 1: G2:q G2:q G2:q G2:q"})");
    auto r = l.run("arrangement.build",
                   R"({"bar": 3, "sequence": [{"scene": "sc_1", "bars": 2},
                       {"scene": "sc_2", "bars": 4}, {"scene": "sc_1", "bars": 1}]})");
    REQUIRE(r["segments"].size() == 3);
    const double starts[] = {8, 16, 32};
    const double ends[] = {16, 32, 36};
    const char* names[] = {"Verse", "Chorus", "Verse"};
    const std::size_t counts[] = {2, 1, 2};
    for (std::size_t k = 0; k < 3; ++k) {
        CAPTURE(k);
        auto seg = at(r["segments"], k);
        CHECK(seg["start"].asDouble() == starts[k]);
        CHECK(seg["end"].asDouble() == ends[k]);
        CHECK(seg["items"].size() == counts[k]);
        const model::Region& region = *l.head().regions.find(id(seg["region"].asString().c_str()));
        CHECK(region.name == names[k]);
        CHECK(region.start == starts[k]);
        CHECK(region.end == ends[k]);
        for (auto laid : seg["items"].asArray())
            CHECK(startBeat(l, id(laid["item"].asString().c_str())) == starts[k]);
    }
    CHECK(l.head().items.size() == 1 + 5);
    CHECK(l.head().regions.size() == 3);
    // One undo step takes it all back.
    l.run("history.undo", "{}");
    CHECK(l.head().items.size() == 1);
    CHECK(l.head().regions.empty());
    l.run("history.redo", "{}");
    CHECK(l.head().items.size() == 6);
}

// --- write_context (ADR-0098 X5) -------------------------------------------------------

namespace {

std::string codesOf(const json::Value& result) {
    std::string out;
    if (const json::Value* w = result.find("warnings"))
        for (const auto& e : w->asArray()) out += e.find("code")->asString() + " ";
    return out;
}

// "D minor Dm" for the key and chord holding at a bar.
std::string holding(Song& l, int bar) {
    auto at = l.run("context.at", "{\"bar\": " + std::to_string(bar) + "}");
    std::string out;
    if (const json::Value* key = at.find("key"))
        out += key->find("tonic")->asString() + " " + key->find("scale")->asString();
    if (const json::Value* chord = at.find("chord")) out += " " + chord->find("symbol")->asString();
    return out;
}

// Verse (sc_1): a two-bar bass line and a two-bar pad, both written in D minor over Dm | A7
// (the same points from two clips are written once); Chorus (sc_2): a bass in F major over F.
void harmonyScenes(Song& l) {
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1",
        "notation": "voice bass; 1: D2:w; 2: A1:w", "context": "1: key D minor {Dm}:w\n2: {A7}:w"})");
    l.run("clip.add", R"({"track": "tr_2", "scene": "sc_1",
        "notation": "voice pad; 1: D3:w; 2: A3:w", "context": "1: key D minor {Dm}:w\n2: {A7}:w"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2",
        "notation": "voice bass; 1: F2:w; 2: F2:w", "context": "1: key F major {F}:w\n2: ~:w"})");
}

}  // namespace

TEST_CASE("write_context writes each scene's harmony under its section, in one undo step") {
    Song l;
    harmonyScenes(l);
    const std::string empty = l.run("context.get", R"({"from": 0, "to": 64})")["text"].asString();
    auto built = l.run("arrangement.build", R"({"bar": 3, "write_context": true, "sequence": [
        {"scene": "sc_1", "bars": 4}, {"scene": "sc_2", "bars": 2}, {"scene": "sc_1", "bars": 2}]})");
    // Verse over bars 3-6 repeats its two bars: a key, then Dm A7 Dm A7; the Chorus a key and a
    // chord; the Verse again, after the Chorus, its key and Dm A7.
    const struct {
        int keys, chords;
    } expect[] = {{1, 4}, {1, 1}, {1, 2}};
    for (std::size_t k = 0; k < 3; ++k) {
        CAPTURE(k);
        auto note = at(built["segments"], k)["context"];
        CHECK(note["written"].asBool());
        CHECK(note["keys"].asInt() == expect[k].keys);
        CHECK(note["chords"].asInt() == expect[k].chords);
        CHECK(note.find("reason") == nullptr);
    }
    const char* heard[] = {"D minor Dm", "D minor A7", "D minor Dm", "D minor A7",
                           "F major F",  "F major F",  "D minor Dm", "D minor A7"};
    for (int k = 0; k < 8; ++k) {
        CAPTURE(k);
        CHECK(holding(l, 3 + k) == heard[k]);
    }
    const std::string text = l.run("context.get", R"({"from": 8, "to": 40})")["text"].asString();
    CHECK(text ==
          "3: key D minor {Dm}:w\n4: {A7}:w\n5: {Dm}:w\n6: {A7}:w\n7: key F major "
          "{F}:w\n8: ~:w\n9: key D minor {Dm}:w\n10: {A7}:w\n");
    CHECK(l.head().context.size() == 3 + 7);  // three keys, seven chords
    // One undo step takes the points and the items back; redo restores both.
    l.run("history.undo", "{}");
    CHECK(l.head().context.empty());
    CHECK(l.head().items.size() == 1);
    CHECK(l.run("context.get", R"({"from": 0, "to": 64})")["text"].asString() == empty);
    l.run("history.redo", "{}");
    CHECK(l.head().context.size() == 10);
    CHECK(l.run("context.get", R"({"from": 8, "to": 40})")["text"].asString() == text);
    // The harmony is part of the project: save and reopen keep it, and the sections.
    auto file = json::Value::object();
    file["path"] = (l.root / "song.json").string();
    l.runArgs("project.save", file);
    l.runArgs("project.open", file);
    CHECK(l.run("context.get", R"({"from": 8, "to": 40})")["text"].asString() == text);
    CHECK(l.head().regions.size() == 3);
}

TEST_CASE("write_context leaves out what the context track already says") {
    Song l;
    harmonyScenes(l);
    // The Verse twice in a row: the second one's key is what already holds, so only its
    // chords are written.
    auto built = l.run("arrangement.build", R"({"bar": 3, "write_context": true, "sequence": [
        {"scene": "sc_1", "bars": 4}, {"scene": "sc_1", "bars": 2}]})");
    CHECK(at(built["segments"], 0)["context"]["keys"].asInt() == 1);
    CHECK(at(built["segments"], 1)["context"]["keys"].asInt() == 0);
    CHECK(at(built["segments"], 1)["context"]["chords"].asInt() == 2);
    CHECK(at(built["segments"], 1)["context"]["written"].asBool());
    // Laid again over what is already there, nothing is left to write: it says it holds.
    l.run("history.undo", "{}");
    l.run("key.set", R"({"at": 4, "tonic": "D", "scale": "minor"})");
    l.run("chord.set", R"({"at": 4, "symbol": "Dm"})");
    l.run("chord.set", R"({"at": 8, "symbol": "A7"})");
    // Bars 3 and 4 (beats 8-16) hold the points of a section laid earlier: left alone.
    auto again = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2,
        "write_context": true})");
    CHECK_FALSE(again["context"]["written"].asBool());
    CHECK(again["context"]["reason"].asString() == "present");
}

TEST_CASE("write_context: clips that disagree write nothing, and say which") {
    Song l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2",
        "notation": "voice bass; 1: F2:w", "context": "1: key F major {F}:w"})");
    l.run("clip.add", R"({"track": "tr_2", "scene": "sc_2",
        "notation": "voice pad; 1: D3:w", "context": "1: key F major {Dm}:w"})");
    auto built = l.run("arrangement.build", R"({"bar": 3, "write_context": true,
        "sequence": [{"scene": "sc_2", "bars": 2}]})");
    auto note = at(built["segments"], 0)["context"];
    CHECK_FALSE(note["written"].asBool());
    CHECK(note["reason"].asString() == "disagree");
    CHECK(note["bar"].asInt() == 3);
    CHECK(note["clips"].size() == 2);
    CHECK(note["message"].asString().find("different chords") != std::string::npos);
    CHECK(codesOf(built).find("context.not_written") != std::string::npos);
    CHECK(l.head().context.empty());                       // the context track is untouched,
    CHECK(at(built["segments"], 0)["items"].size() == 2);  // and the clips are laid as ever
    // The same scene alone, its clips agreeing (one names the key only): written.
    l.run("clip.set", R"({"clip": "cl_2", "context": "1: key F major ~:w"})");
    auto agreed = l.run("scene.insert", R"({"scene": "sc_2", "bar": 6, "bars": 1,
        "write_context": true})");
    CHECK(agreed["context"]["written"].asBool());
    CHECK(agreed["context"]["keys"].asInt() == 1);
    CHECK(agreed["context"]["chords"].asInt() == 1);
}

TEST_CASE("write_context: a section with points under it is left alone, with a warning") {
    Song l;
    harmonyScenes(l);
    l.run("chord.set", R"({"at": 10, "symbol": "Gm"})");  // inside bar 3 (beats 8-12)
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2,
        "write_context": true})");
    CHECK_FALSE(laid["context"]["written"].asBool());
    CHECK(laid["context"]["reason"].asString() == "present");
    CHECK(codesOf(laid).find("context.not_written") != std::string::npos);
    CHECK(l.head().context.size() == 1);
    // A key holding into the span is not "under" it: bar 7's section is written.
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "major"})");
    auto later = l.run("scene.insert", R"({"scene": "sc_1", "bar": 7, "bars": 2,
        "write_context": true})");
    CHECK(later["context"]["written"].asBool());
    CHECK(later["context"]["keys"].asInt() == 1);  // D minor replaces the C major that held
}

TEST_CASE("write_context writes before the clips are laid, so a following clip finds its key") {
    Song l;
    harmonyScenes(l);
    l.run("clip.set", R"({"clip": "cl_1", "follow": "chords"})");
    // Without the flag the project has no key under the item: the notes are copied.
    auto plain = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");
    CHECK(codesOf(plain).find("context.no_key") != std::string::npos);
    l.run("history.undo", "{}");
    auto written = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2,
        "write_context": true})");
    CHECK(codesOf(written).find("context.no_key") == std::string::npos);
    CHECK(codesOf(written).find("context.mismatch") == std::string::npos);
    auto bass = at(written["items"], 0);
    CHECK(bass["followed"]["mode"].asString() == "chords");
    CHECK(bass["followed"]["regions"].size() == 1);
    CHECK(bass["followed"]["regions"].asArray()[0]["to_key"].asString() == "D minor");
}

TEST_CASE(
    "write_context: nothing to write when no clip has a context, and without the flag no answer") {
    Song l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "notation": "voice bass; 1: F2:w"})");
    auto laid = l.run("scene.insert", R"({"scene": "sc_2", "bar": 3, "bars": 1,
        "write_context": true})");
    CHECK_FALSE(laid["context"]["written"].asBool());
    CHECK(laid["context"]["reason"].asString() == "no_context");
    CHECK(codesOf(laid).find("context.not_written") == std::string::npos);
    l.run("history.undo", "{}");
    auto plain = l.run("scene.insert", R"({"scene": "sc_2", "bar": 3, "bars": 1})");
    CHECK(plain.find("context") == nullptr);
    CHECK(l.head().context.empty());
}

TEST_CASE("arrangement.build refuses an entry, with its index, and changes nothing") {
    Song l;
    l.verseClips();
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2",
        "notation": "voice bass; 1: G2:q G2:q G2:q G2:q"})");
    l.run("scene.add", R"({"name": "Empty"})");  // sc_3
    SUBCASE("an empty scene") {
        const Error e = l.fails("arrangement.build",
                                R"({"bar": 3, "sequence": [{"scene": "sc_1", "bars": 2},
                                    {"scene": "sc_3", "bars": 2}]})");
        CHECK(e.code == "scene.empty");
        CHECK(e.path == "/args/sequence/1/scene");
    }
    SUBCASE("an item in a later entry's span") {
        l.run("item.add", R"({"track": "tr_1", "bar": 6, "notation": "voice bass; 1: C2:w"})");
        const Error e = l.fails("arrangement.build",
                                R"({"bar": 3, "sequence": [{"scene": "sc_1", "bars": 2},
                                    {"scene": "sc_2", "bars": 4}]})");
        CHECK(e.code == "scene.insert.occupied");
        CHECK(e.path == "/args/sequence/1/bar");
    }
    SUBCASE("the default start is bar 1, where the first item is") {
        CHECK(
            l.fails("arrangement.build", R"({"sequence": [{"scene": "sc_1", "bars": 1}]})").code ==
            "scene.insert.occupied");
    }
    SUBCASE("an empty sequence, one too long, a scene that is not there") {
        CHECK(l.fails("arrangement.build", R"({"sequence": []})").code == "scene.sequence");
        std::string many = R"({"bar": 3, "sequence": [)";
        for (int k = 0; k < 257; ++k)
            many += std::string(k ? "," : "") + R"({"scene": "sc_1", "bars": 1})";
        CHECK(l.fails("arrangement.build", many + "]}").code == "scene.sequence");
        CHECK(l.fails("arrangement.build", R"({"bar": 3, "sequence": [{"scene": "sc_9",
            "bars": 1}]})")
                  .path == "/args/sequence/0/scene");
    }
}

TEST_CASE(
    "scene.insert, arrangement.build and scene.capture merge from a branch with remapped ids") {
    Song l;
    l.run("branch.create", R"({"name": "agent/test/arrange"})");
    l.run("scene.add", R"({"name": "Bridge"})");  // sc_3 on the branch
    l.run("clip.add", R"({"track": "tr_2", "scene": "sc_3", "notation": "voice pad; 1: C3:w"})");
    l.run("arrangement.build", R"({"bar": 5, "sequence": [{"scene": "sc_3", "bars": 2}]})");
    l.run("scene.insert", R"({"scene": "sc_3", "bar": 9, "bars": 1})");
    l.run("scene.capture", R"({"region": "rg_1"})");  // the Bridge section laid by the build
    l.run("branch.switch", R"({"name": "main"})");
    l.run("scene.add", R"({"name": "Outro"})");  // sc_3 on main
    l.run("clip.add", R"({"track": "tr_2", "scene": "sc_3", "notation": "voice pad; 1: C3:w"})");
    l.run("region.add", R"({"start": 64, "end": 68, "name": "Tail"})");  // rg_1 on main
    l.run("branch.merge", R"({"from": "agent/test/arrange"})");
    const model::Session& s = l.head();
    CHECK(model::checkInvariants(s));
    CHECK(s.scenes.size() == 5);  // Verse, Chorus, Outro, Bridge, and the captured Bridge
    CHECK(s.clips.size() == 3);
    CHECK(s.regions.size() == 3);  // Tail and the two sections
    std::set<std::string> names;
    for (const auto& e : s.regions) names.insert(e.value->name);
    CHECK(names == std::set<std::string>{"Tail", "Bridge"});
    std::set<double> starts;
    for (const auto& e : s.items)
        if (e.value->track == id("tr_2")) starts.insert(model::itemSpan(s, *e.value).startBeat);
    CHECK(starts == std::set<double>{16.0, 32.0});  // bar 5 and bar 9
}

TEST_CASE("scene.insert lays a clip take's FX envelope in timeline beats, pass after pass") {
    // Review round 69 F2 (the reviewer's probe P2): cl_1, a four-beat loop,
    // has the gain fixture on its take with an envelope from 0 dB at beat 0
    // to -24 dB at beat 3, in clip time (C2). Laid at bar 3 for two bars,
    // the item's copy repeats it each pass from the item's start: the value
    // at the period held to the boundary, the next pass from its first
    // point there, and the span's end taking the next pass's start.
    Song l;
    l.verseClips();  // cl_1: four beats, loop mode
    const model::Id take = *l.head().clips.find(id("cl_1"))->activeTake;
    auto fx = j(R"({"plugin": "com.suppleaudio.test.gain"})");
    fx["take"] = take.str();
    const std::string fxId = l.runArgs("fx.add", fx)["fx"].asString();
    auto env =
        j(R"({"param": "gain", "points": [{"beat": 0, "value": 0}, {"beat": 3, "value": -24}]})");
    env["fx"] = fxId;
    l.runArgs("env.add", env);
    auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");
    const model::Id bass = id(at(r["items"], 0)["item"].asString().c_str());
    const model::Take& itemTake = *l.head().takes.find(*l.head().items.find(bass)->activeTake);
    REQUIRE(itemTake.chain.size() == 1);
    const model::Envelope* laid = nullptr;
    for (const auto& e : l.head().envelopes)
        if (e.value->target.object == itemTake.chain[0]) laid = &*e.value;
    REQUIRE(laid);
    CHECK(laid->timebase == model::Timebase::Beats);
    std::string points;
    for (const auto& p : laid->points)
        points += json::formatDouble(p.pos) + ":" + json::formatDouble(p.value) + " ";
    CHECK(points ==
          "8.0:0.0 11.0:-24.0 12.0:-24.0 12.0:0.0 15.0:-24.0 16.0:-24.0 16.0:0.0 19.0:-24.0 ");
    // The clip's own envelope stays in clip time.
    for (const auto& e : l.head().envelopes)
        if (e.value->target.object == id(fxId.c_str())) {
            REQUIRE(e.value->points.size() == 2);
            CHECK(e.value->points.front().pos == 0.0);
            CHECK(e.value->points.back().pos == 3.0);
        }
}

TEST_CASE("scene.insert lays a clip take's own envelope pass after pass over a looped item") {
    // cl_1, a four-beat loop, has a volume envelope on its take from beat 0 to
    // beat 3, in clip time, which the clip plays from the start of each pass.
    // Laid at bar 3 for two bars as one looped item, whose take envelopes run
    // once from its start, the copy repeats it each pass, as its FX's are
    // (item-relative beats).
    Song l;
    l.verseClips();  // cl_1: four beats, loop mode
    const model::Id take = *l.head().clips.find(id("cl_1"))->activeTake;
    auto fx = j(R"({"plugin": "com.suppleaudio.test.sine"})");  // a MIDI take's envelope needs one
    fx["take"] = take.str();
    l.runArgs("fx.add", fx);
    auto env = j(R"({"param": "volume", "points": [{"beat": 0, "value": 0},
                                                   {"beat": 3, "value": "-12 dB"}]})");
    env["take"] = take.str();
    l.runArgs("env.add", env);
    auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");
    const model::Id bass = id(at(r["items"], 0)["item"].asString().c_str());
    const model::Item& item = *l.head().items.find(bass);
    REQUIRE(item.loop);
    const model::Envelope* laid = nullptr;
    for (const auto& e : l.head().envelopes)
        if (e.value->target.object == *item.activeTake) laid = &*e.value;
    REQUIRE(laid);
    std::string positions;
    for (const auto& p : laid->points) positions += json::formatDouble(p.pos) + " ";
    CHECK(positions == "0.0 3.0 4.0 4.0 7.0 8.0 8.0 11.0 ");
    REQUIRE(laid->points.size() == 8);
    CHECK(laid->points[3].value == laid->points[0].value);  // each pass from its start
    CHECK(laid->points[2].value == laid->points[1].value);  // the end held to the boundary
}

// --- names ---------------------------------------------------------------------------

// An item's name is its active take's: the model has no name on an item.
std::string itemName(const Song& l, model::Id item) {
    const model::Item* it = l.head().items.find(item);
    REQUIRE(it);
    REQUIRE(it->activeTake);
    return l.head().takes.find(*it->activeTake)->name;
}

TEST_CASE("a clip's name becomes the name of the items made from it") {
    Song l;
    l.verseClips();
    l.run("clip.set", R"({"clip": "cl_1", "name": "Bass line"})");  // cl_2 has none
    SUBCASE("scene.insert") {
        auto r = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 2})");
        CHECK(itemName(l, id(at(r["items"], 0)["item"].asString().c_str())) == "Bass line");
        CHECK(itemName(l, id(at(r["items"], 1)["item"].asString().c_str())).empty());
        CHECK(l.describe().find(" \"Bass line\" bar 3") != std::string::npos);
    }
    SUBCASE("arrangement.build") {
        auto r =
            l.run("arrangement.build", R"({"sequence": [{"scene": "sc_1", "bars": 2}], "bar": 3})");
        auto items = at(at(r["segments"], 0)["items"], 0);
        CHECK(itemName(l, id(items["item"].asString().c_str())) == "Bass line");
    }
    SUBCASE("item.add {clip}") {
        auto r = l.run("item.add", R"({"track": "tr_1", "bar": 3, "clip": "cl_1"})");
        CHECK(itemName(l, id(r["item"].asString().c_str())) == "Bass line");
    }
    SUBCASE("a clip without a name leaves the copied take's own") {
        l.run("take.set", R"({"take": "tk_3", "name": "pad take"})");  // cl_2's take
        auto r = l.run("item.add", R"({"track": "tr_2", "bar": 3, "clip": "cl_2"})");
        CHECK(itemName(l, id(r["item"].asString().c_str())) == "pad take");
    }
}

TEST_CASE("an item's name becomes the name of the clip made from it") {
    Song l;
    l.run("item.set", R"({"item": "it_1", "name": "Bass riff"})");
    SUBCASE("clip.add {item} takes the item's name, unless one is given") {
        auto r = l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
        CHECK(l.head().clips.find(id(r["clip"].asString().c_str()))->name == "Bass riff");
        auto named = l.run(
            "clip.add", R"({"track": "tr_1", "scene": "sc_2", "item": "it_1", "name": "Intro"})");
        CHECK(l.head().clips.find(id(named["clip"].asString().c_str()))->name == "Intro");
    }
    SUBCASE("scene.capture") {
        l.run("region.add", R"({"start": 0, "end": 4, "name": "Verse"})");
        auto r = l.run("scene.capture", R"({"region": "rg_1"})");
        CHECK(l.head().clips.find(id(at(r["clips"], 0)["clip"].asString().c_str()))->name ==
              "Bass riff");
    }
}

TEST_CASE("a name makes the whole trip: clip, item, clip, item") {
    Song l;
    l.verseClips();
    l.run("clip.set", R"({"clip": "cl_1", "name": "Hook"})");
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 3, "bars": 1})");
    const std::string item = at(laid["items"], 0)["item"].asString();
    CHECK(itemName(l, id(item.c_str())) == "Hook");
    l.run("item.set", std::string(R"({"item": ")") + item + R"(", "name": "Hook 2"})");
    auto back = l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_2", "item": ")") +
                                      item + R"("})");
    CHECK(l.head().clips.find(id(back["clip"].asString().c_str()))->name == "Hook 2");
    // One undo step takes the name with the item.
    l.run("history.undo", "{}");
    CHECK(l.head().clips.find(id(back["clip"].asString().c_str())) == nullptr);
}
