// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Scene <-> section over the bus (ADR-0097, step L4a): scene.insert,
// scene.capture and arrangement.build, their refusals, one undo step each,
// save and reopen, and the round trip between a scene and a section.
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/engine/app.h>
#include <supple/json/write.h>
#include <supple/model/launcher.h>
#include <supple/model/schemas.h>
#include <supple/model/take_owner.h>
#include <chrono>
#include <cmath>
#include <filesystem>

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
    fs::path root = fs::temp_directory_path() /
                    ("supple-scene-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    const auto file = (l.root / "song.json").string();
    l.run("project.save", std::string(R"({"path": ")") + file + R"("})");
    l.run("project.open", std::string(R"({"path": ")") + file + R"("})");
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
