// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's model commands over the bus (ADR-0097, step L1):
// scene.*, clip.*, item.add {clip}, the cascades and session.describe.
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>
#include <supple/json/write.h>
#include <supple/midi/midi.h>
#include <supple/model/follow.h>
#include <supple/model/launcher.h>
#include <supple/model/schemas.h>
#include <supple/model/take_owner.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <span>

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "clip launcher", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }
model::Id id(const char* text) { return *model::Id::parse(text); }

// A project with channel tr_1 holding it_1 (tk_1, unpooled mi_1, four
// quarter notes) and scenes sc_1 "Verse" and sc_2 "Chorus".
struct Launcher {
    // Unique per fixture without a platform call (several test processes
    // may run at once).
    fs::path root = fs::temp_directory_path() /
                    ("supple-clips-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;

    Launcher() {
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
        run("item.add",
            R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q E2:q G2:q C3:q"})");
        run("scene.add", R"({"name": "Verse"})");
        run("scene.add", R"({"name": "Chorus"})");
    }
    ~Launcher() { fs::remove_all(root); }

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
    Error fails(std::string name, std::string_view args) {
        auto result = bus->execute({std::move(name), j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().hint.empty());
        return result.error();
    }
    const model::Session& head() const { return app->head(); }
    // A mono WAV at 48 kHz; `level` tells files of one length apart (the
    // same bytes are one media source).
    fs::path wav(const char* name, double seconds = 1.0, float level = .25f) {
        audio::AudioData a;
        a.sampleRate = 48000;
        a.channels.assign(1, std::vector<float>(static_cast<std::size_t>(seconds * 48000), level));
        const auto path = root / name;
        REQUIRE(audio::writeWav(path, a));
        return path;
    }
};

std::vector<std::string> sceneNames(const model::Session& s) {
    std::vector<std::pair<int, std::string>> byOrder;
    for (const auto& e : s.scenes) byOrder.emplace_back(e.value->order, e.value->name);
    std::sort(byOrder.begin(), byOrder.end());
    std::vector<std::string> out;
    for (auto& [order, name] : byOrder) out.push_back(name);
    return out;
}

std::string warningCodes(const json::Value& result) {
    std::string out;
    if (const auto* w = result.find("warnings"))
        for (const auto& e : w->asArray())
            if (const auto* code = e.find("code")) out += code->asString() + " ";
    return out;
}

}  // namespace

// --- scenes --------------------------------------------------------------------

TEST_CASE("scene.add appends a column, or inserts one, and keeps orders 0 .. n-1") {
    Launcher l;
    CHECK(sceneNames(l.head()) == std::vector<std::string>{"Verse", "Chorus"});
    CHECK(l.run("scene.add", R"({"name": "Intro", "at": 0})")["id"].asString() == "sc_3");
    CHECK(sceneNames(l.head()) == std::vector<std::string>{"Intro", "Verse", "Chorus"});
    CHECK(l.head().scenes.find(id("sc_3"))->order == 0);
    CHECK(l.head().scenes.find(id("sc_2"))->order == 2);
    auto listed = l.run("scene.list", "{}");
    REQUIRE(listed["scenes"].size() == 3);
    CHECK(listed["scenes"].at(0).find("id")->asString() == "sc_3");
    CHECK(listed["scenes"].at(2).find("clips")->asInt() == 0);
    // One undo step puts the orders back.
    l.run("history.undo", "{}");
    CHECK(sceneNames(l.head()) == std::vector<std::string>{"Verse", "Chorus"});
    CHECK(l.head().scenes.find(id("sc_2"))->order == 1);
    const Error past = l.fails("scene.add", R"({"at": 3})");
    CHECK(past.code == "scene.index");
    CHECK(past.path == "/args/at");
}

TEST_CASE("scene.set renames and scene.move reorders") {
    Launcher l;
    l.run("scene.set", R"({"scene": "sc_1", "name": "Verse 1", "color": "#406080"})");
    CHECK(l.head().scenes.find(id("sc_1"))->name == "Verse 1");
    CHECK(l.head().scenes.find(id("sc_1"))->color == "#406080");
    l.run("scene.add", R"({"name": "Bridge"})");
    l.run("scene.move", R"({"scene": "sc_3", "to": 0})");
    CHECK(sceneNames(l.head()) == std::vector<std::string>{"Bridge", "Verse 1", "Chorus"});
    l.run("scene.move", R"({"scene": "sc_3", "to": 2})");
    CHECK(sceneNames(l.head()) == std::vector<std::string>{"Verse 1", "Chorus", "Bridge"});
    CHECK(l.fails("scene.move", R"({"scene": "sc_3", "to": 3})").code == "scene.index");
    CHECK(l.fails("scene.set", R"({"scene": "sc_9", "name": "x"})").code == "sc.unknown");
}

TEST_CASE("scene.remove removes its clips with their takes, and undo brings them back") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    const std::string before = model::serializeProject(l.head());
    const model::Id take = l.head().clips.find(id("cl_1"))->takes.front();
    const model::Id source = *l.head().takes.find(take)->source;
    auto removed = l.run("scene.remove", R"({"scene": "sc_1"})");
    CHECK(warningCodes(removed) == "clip.removed ");
    CHECK(removed["warnings"].at(0).find("message")->asString() ==
          "removed clip cl_1 with their takes");
    CHECK(l.head().clips.empty());
    CHECK_FALSE(l.head().takes.contains(take));
    CHECK_FALSE(l.head().midi.contains(source));
    CHECK(sceneNames(l.head()) == std::vector<std::string>{"Chorus"});
    CHECK(l.head().scenes.find(id("sc_2"))->order == 0);
    l.run("history.undo", "{}");
    CHECK(model::serializeProject(l.head()) == before);
}

TEST_CASE("scene.add refuses the 1025th scene") {
    Launcher l;
    for (int k = 2; k < 1024; ++k) l.run("scene.add", "{}");
    REQUIRE(l.head().scenes.size() == 1024);
    const Error full = l.fails("scene.add", "{}");
    CHECK(full.code == "scene.limit");
}

// --- clip.add -------------------------------------------------------------------

TEST_CASE("clip.add makes a clip from notation, a MIDI file, an audio file or nothing") {
    Launcher l;
    SUBCASE("notation: its bars") {
        auto r = l.run(
            "clip.add",
            R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w | C2:h r:h"})");
        CHECK(r["kind"].asString() == "midi");
        CHECK(r["length"].asDouble() == 8.0);
        const model::Clip& c = *l.head().clips.find(id(r["clip"].asString().c_str()));
        CHECK(c.kind == model::ClipKind::Midi);
        CHECK(c.length == 8.0);
        CHECK(c.takes.size() == 1);
        CHECK(c.activeTake == c.takes.front());
        CHECK(model::takeOwner(l.head(), c.takes.front()) == id("cl_1"));
    }
    SUBCASE("nothing: one empty bar of MIDI") {
        auto r = l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2"})");
        CHECK(r["kind"].asString() == "midi");
        CHECK(r["length"].asDouble() == 4.0);
        const model::Clip& c = *l.head().clips.find(id("cl_1"));
        const model::MidiSource* m = l.head().midi.find(*l.head().takes.find(c.takes[0])->source);
        REQUIRE(m);
        CHECK(m->sequence.notes.empty());
        CHECK(m->sequence.length == 4 * m->sequence.ppq);
    }
    SUBCASE("a MIDI file") {
        midi::Sequence seq;
        seq.ppq = 960;
        seq.notes.push_back({0, 960 * 5, 0, 40, 100, 64});
        seq.length = 960 * 5;
        auto bytes = midi::writeSmf(seq, {}, {});
        REQUIRE(bytes);
        const auto path = l.root / "five.mid";
        REQUIRE(writeFileAtomic(path, std::span<const std::uint8_t>(*bytes)));
        auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
        args["midi_file"] = path.string();
        auto r = l.runArgs("clip.add", args);
        CHECK(r["length"].asDouble() == 8.0);  // five beats, two bars
    }
    SUBCASE("an audio file: seconds") {
        auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
        args["audio_file"] = l.wav("pad.wav", 1.5).string();
        auto r = l.runArgs("clip.add", args);
        CHECK(r["kind"].asString() == "audio");
        CHECK(r["length"].asDouble() == doctest::Approx(1.5));
        CHECK(l.head().clips.find(id("cl_1"))->kind == model::ClipKind::Audio);
    }
    // Clips never move the project length.
    CHECK(model::lastItemBeat(l.head()) == 4.0);
}

TEST_CASE("clip.add {item} copies the item's takes; MIDI is the clip's own unless pooled") {
    Launcher l;
    const std::string item = model::serializeProject(l.head());
    SUBCASE("unpooled") {
        auto r = l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
        const model::Clip& c = *l.head().clips.find(id("cl_1"));
        REQUIRE(c.takes.size() == 1);
        CHECK(c.takes[0] != id("tk_1"));
        CHECK(model::takeOwner(l.head(), id("tk_1")) == id("it_1"));
        const auto source = *l.head().takes.find(c.takes[0])->source;
        CHECK(source != id("mi_1"));
        CHECK(l.head().midi.find(source)->sequence.notes ==
              l.head().midi.find(id("mi_1"))->sequence.notes);
        CHECK_FALSE(l.head().midi.find(id("mi_1"))->pooled);
        CHECK(c.length == 4.0);
        CHECK(r["source"].asString() == source.str());
    }
    SUBCASE("pooled") {
        l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1", "pooled": true})");
        const model::Clip& c = *l.head().clips.find(id("cl_1"));
        CHECK(*l.head().takes.find(c.takes[0])->source == id("mi_1"));
        CHECK(l.head().midi.find(id("mi_1"))->pooled);
    }
    SUBCASE("a looped item loops its whole content") {
        l.run("item.set", R"({"item": "it_1", "loop": true, "length": 8})");
        l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
        const model::Clip& c = *l.head().clips.find(id("cl_1"));
        CHECK(c.length == 8.0);
        CHECK(c.loop == model::ClipLoop{true, 0.0, 4.0});
    }
}

TEST_CASE("clip.add refuses a buss, a master, an occupied cell, a missing scene, two sources") {
    Launcher l;
    l.run("track.add", R"({"name": "Bus", "type": "buss"})");
    CHECK(l.fails("clip.add", R"({"track": "tr_2", "scene": "sc_1"})").code == "clip.track");
    CHECK(l.fails("clip.add", R"({"track": "tr_0", "scene": "sc_1"})").code == "clip.track");
    const Error scene = l.fails("clip.add", R"({"track": "tr_1", "scene": "sc_9"})");
    CHECK(scene.code == "clip.scene");
    CHECK(scene.path == "/args/scene");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");
    const Error occupied = l.fails("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");
    CHECK(occupied.code == "clip.occupied");
    CHECK(occupied.message == "cl_1 is already in tr_1's sc_1");
    CHECK(l.fails("clip.add",
                  R"({"track": "tr_1", "scene": "sc_2", "item": "it_1", "notation": "x"})")
              .code == "clip.source");
    CHECK(l.fails("clip.add", R"({"track": "tr_1", "scene": "sc_2", "pooled": true})").code ==
          "clip.source");
}

TEST_CASE("clip.add {item} refuses a warped take") {
    Launcher l;
    auto args = j(R"({"track": "tr_1", "bar": 3})");
    args["audio_file"] = l.wav("hits.wav", 2.0).string();
    l.runArgs("item.add", args);  // it_2, tk_2
    l.run("take.set", R"({"take": "tk_2", "preserve_pitch": true, "pitch_mode": "signalsmith"})");
    l.run("take.stretch.add", R"({"take": "tk_2", "item_pos": 0.5})");
    const Error warped =
        l.fails("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_2"})");
    CHECK(warped.code == "clip.unsupported");
    CHECK(l.head().clips.empty());
}

// --- clip.set, move, duplicate, remove ------------------------------------------------

TEST_CASE("clip.set edits fields; a null quantize returns to the project's") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");
    l.run("clip.set",
          R"({"clip": "cl_1", "name": "Riff", "color": "#804020", "mute": true, "gain_db": -6,
              "length": 8, "loop": {"on": true, "start": 1, "length": 2}, "mode": "once",
              "quantize": "1/16"})");
    const model::Clip& c = *l.head().clips.find(id("cl_1"));
    CHECK(c.name == "Riff");
    CHECK(c.mute);
    CHECK(c.gainDb == -6);
    CHECK(c.length == 8);
    CHECK(c.loop == model::ClipLoop{true, 1, 2});
    CHECK(c.mode == model::ClipMode::Once);
    CHECK(c.quantize == model::LaunchQuantize::Sixteenth);
    l.run("clip.set", R"({"clip": "cl_1", "quantize": null, "loop": {"on": false}})");
    CHECK_FALSE(l.head().clips.find(id("cl_1"))->quantize);
    CHECK(l.head().clips.find(id("cl_1"))->loop == model::ClipLoop{false, 1, 2});
    const Error length = l.fails("clip.set", R"({"clip": "cl_1", "length": 0})");
    CHECK(length.code == "clip.length");
    CHECK(length.path == "/args/length");
    CHECK(l.fails("clip.set", R"({"clip": "cl_1", "loop": {"on": true, "length": 0}})").code ==
          "clip.length");
}

TEST_CASE("clip.move and clip.duplicate need an empty cell; a duplicate owns its takes") {
    Launcher l;
    l.run("track.add", R"({"name": "Keys"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    l.run("clip.move", R"({"clip": "cl_1", "scene": "sc_2"})");
    CHECK(model::clipAt(l.head(), id("tr_1"), id("sc_2")) == id("cl_1"));
    l.run("clip.move", R"({"clip": "cl_1"})");  // its own cell: nothing changes
    auto copy = l.run("clip.duplicate", R"({"clip": "cl_1", "scene": "sc_1"})");
    const model::Id dup = id(copy["id"].asString().c_str());
    const model::Clip& a = *l.head().clips.find(id("cl_1"));
    const model::Clip& b = *l.head().clips.find(dup);
    CHECK(a.takes != b.takes);
    CHECK(*l.head().takes.find(a.takes[0])->source != *l.head().takes.find(b.takes[0])->source);
    CHECK(l.fails("clip.move", R"({"clip": "cl_1", "scene": "sc_1"})").code == "clip.occupied");
    CHECK(l.fails("clip.duplicate", R"({"clip": "cl_1", "scene": "sc_1"})").code ==
          "clip.occupied");
    l.run("clip.move", R"({"clip": "cl_1", "track": "tr_2", "scene": "sc_1"})");
    CHECK(l.head().clips.find(id("cl_1"))->track == id("tr_2"));
    l.run("track.add", R"({"name": "Bus", "type": "buss"})");
    CHECK(l.fails("clip.move", R"({"clip": "cl_1", "track": "tr_3"})").code == "clip.track");
}

TEST_CASE("clip.remove takes its takes and unpooled sources; pooled MIDI stays") {
    Launcher l;
    // cl_1 gets its own copy; cl_2 then shares mi_1, which becomes pooled.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "item": "it_1", "pooled": true})");
    const model::Id own = *l.head().takes.find(l.head().clips.find(id("cl_1"))->takes[0])->source;
    CHECK(own != id("mi_1"));
    CHECK(*l.head().takes.find(l.head().clips.find(id("cl_2"))->takes[0])->source == id("mi_1"));
    const std::string before = model::serializeProject(l.head());
    l.run("clip.remove", R"({"clip": "cl_1"})");
    l.run("clip.remove", R"({"clip": "cl_2"})");
    CHECK(l.head().clips.empty());
    CHECK(l.head().midi.contains(id("mi_1")));  // the item still plays it
    CHECK_FALSE(l.head().midi.contains(own));
    CHECK(l.head().takes.size() == 1);
    l.run("history.undo", "{}");
    l.run("history.undo", "{}");
    CHECK(model::serializeProject(l.head()) == before);
}

// --- tracks -------------------------------------------------------------------------

TEST_CASE("track.remove removes its clips and says so; track.set keeps clips on channels") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");
    const Error buss = l.fails("track.set", R"({"track": "tr_1", "type": "buss"})");
    CHECK(buss.code == "track.strip_type");
    CHECK(buss.path == "/args/type");
    CHECK(buss.message.find("cl_1") != std::string::npos);
    auto removed = l.run("track.remove", R"({"track": "tr_1"})");
    CHECK(warningCodes(removed).find("clip.removed") != std::string::npos);
    CHECK(l.head().clips.empty());
    CHECK(l.head().takes.empty());
    CHECK(l.head().scenes.size() == 2);  // scenes stay
}

TEST_CASE("clips on a frozen track stay editable") {
    Launcher l;
    l.run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    l.run("track.freeze", R"({"track": "tr_1"})");
    REQUIRE(l.head().tracks.find(id("tr_1"))->freeze);
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    l.run("clip.set", R"({"clip": "cl_1", "name": "Frozen bass"})");
    CHECK(l.head().clips.find(id("cl_1"))->name == "Frozen bass");
    CHECK(l.fails("item.add", R"({"track": "tr_1", "bar": 3, "clip": "cl_1"})").code ==
          "track.frozen");
}

// --- item.add {clip} and the conversion rule ---------------------------------------------

TEST_CASE("item.add {clip} lays a copy of a MIDI clip on the timeline") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    auto r = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    const model::Id it = id(r["item"].asString().c_str());
    const model::Item& item = *l.head().items.find(it);
    CHECK(model::itemSpan(l.head(), item).startBeat == 16.0);
    CHECK(model::itemLengthBeats(l.head(), item) == 4.0);
    CHECK(item.takes.size() == 1);
    CHECK(model::takeOwner(l.head(), l.head().clips.find(id("cl_1"))->takes[0]) == id("cl_1"));
    CHECK(model::takeOwner(l.head(), item.takes[0]) == it);
    CHECK(l.fails("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_1", "length": 2})").code ==
          "item.source");
    // A loop window an item cannot express is refused; the whole content converts.
    l.run("clip.set",
          R"({"clip": "cl_1", "length": 8, "loop": {"on": true, "start": 1, "length": 2}})");
    CHECK(l.fails("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_1"})").code ==
          "clip.unsupported");
    l.run("clip.set", R"({"clip": "cl_1", "loop": {"on": true, "start": 0, "length": 4}})");
    auto looped = l.run("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_1"})");
    const model::Item& li = *l.head().items.find(id(looped["item"].asString().c_str()));
    CHECK(li.loop);
    CHECK(model::itemLengthBeats(l.head(), li) == 8.0);
}

TEST_CASE("an audio item → clip → item round trip keeps its length and envelope points") {
    // At 90 BPM with a step to 150 inside the item's span: the points land
    // exactly through the tempo map, both ways.
    Launcher l;
    l.run("tempo.set", R"({"points": [{"beat": 0, "bpm": 90}, {"beat": 10, "bpm": 150}]})");
    auto args = j(R"({"track": "tr_1", "position": 8})");
    args["audio_file"] = l.wav("loop.wav", 3.0).string();
    auto added = l.runArgs("item.add", args);
    const model::Id item = id(added["item"].asString().c_str());
    const model::Id take = id(added["take"].asString().c_str());
    l.runArgs("env.add", json::parse(R"({"take": ")" + take.str() +
                                     R"(", "param": "volume", "points": [{"beat": 0, "value": 0},
                                 {"beat": 1.5, "value": -12}, {"beat": 4, "value": 0}]})")
                             .value());
    auto pointsOn = [&](model::Id tk) {
        std::vector<double> out;
        for (const auto& e : l.head().envelopes)
            if (e.value->target.object == tk)
                for (const auto& p : e.value->points) out.push_back(p.pos);
        return out;
    };
    const auto span = model::itemSpan(l.head(), *l.head().items.find(item));
    l.runArgs("clip.add",
              json::parse(R"({"track": "tr_1", "scene": "sc_1", "item": ")" + item.str() + "\"}")
                  .value());
    const model::Clip& clip = *l.head().clips.find(id("cl_1"));
    CHECK(clip.kind == model::ClipKind::Audio);
    CHECK(clip.length == doctest::Approx(span.endSec - span.startSec).epsilon(1e-12));
    const auto inClip = pointsOn(clip.takes[0]);
    REQUIRE(inClip.size() == 3);
    const auto& map = *l.head().timeMap;
    CHECK(inClip[1] == doctest::Approx(map.secondsAt(9.5) - map.secondsAt(8)).epsilon(1e-12));
    CHECK(inClip[2] == doctest::Approx(map.secondsAt(12) - map.secondsAt(8)).epsilon(1e-12));
    auto back = l.run("item.add", R"({"track": "tr_1", "position": 8, "clip": "cl_1"})");
    const model::Item& copy = *l.head().items.find(id(back["item"].asString().c_str()));
    const auto copySpan = model::itemSpan(l.head(), copy);
    CHECK(copySpan.startSec == doctest::Approx(span.startSec).epsilon(1e-12));
    CHECK(copySpan.endSec == doctest::Approx(span.endSec).epsilon(1e-12));
    const auto again = pointsOn(copy.takes[0]);
    REQUIRE(again.size() == 3);
    CHECK(again[0] == doctest::Approx(0.0));
    CHECK(again[1] == doctest::Approx(1.5).epsilon(1e-12));
    CHECK(again[2] == doctest::Approx(4.0).epsilon(1e-12));
}

// --- describe, memory, merges ---------------------------------------------------------

TEST_CASE("session.describe lists the scenes with their clips") {
    Launcher l;
    l.run("track.add", R"({"name": "Pad"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1", "name": "Bass line"})");
    l.run("clip.set", R"({"clip": "cl_1", "length": 8, "loop": {"on": true, "length": 4}})");
    auto args = j(R"({"track": "tr_2", "scene": "sc_1"})");
    args["audio_file"] = l.wav("pad.wav", 6.0).string();
    l.runArgs("clip.add", args);
    l.run(
        "clip.set",
        R"({"clip": "cl_2", "name": "Pad swell", "mode": "once", "quantize": "beat", "mute": true, "gain_db": -6})");
    const std::string text = l.run("session.describe", "{}")["text"].asString();
    const auto at = text.find("Scenes (2):");
    REQUIRE(at != std::string::npos);
    const model::Id mi = *l.head().takes.find(l.head().clips.find(id("cl_1"))->takes[0])->source;
    const model::Id md = *l.head().takes.find(l.head().clips.find(id("cl_2"))->takes[0])->source;
    CHECK(text.substr(at) ==
          "Scenes (2):\n"
          "  sc_1 Verse\n"
          "    tr_1 Bass: cl_1 \"Bass line\" 8 beats · loop 0–4 · MIDI " +
              mi.str() +
              ", 4 notes\n"
              "    tr_2 Pad: cl_2 \"Pad swell\" 6.0 s once · audio " +
              md.str() +
              " pad.wav · quantize beat · -6 dB · muted\n"
              "  sc_2 Chorus (no clips)\n");
    // The project's length is the arrangement's.
    CHECK(text.find("· 2.0 s\n") != std::string::npos);
}

TEST_CASE("clip.memory refuses clip audio that would not fit beside the arrangement") {
    // Two sources of 2^26 frames each (hand-edited lengths: the estimate
    // reads the model, not the file) fill the 2^28-sample budget, decoded
    // and resampled once each. A third, real file does not fit.
    Launcher l;
    auto args = j(R"({"track": "tr_1", "bar": 3})");
    args["audio_file"] = l.wav("a.wav").string();
    l.runArgs("item.add", args);
    args = j(R"({"track": "tr_1", "scene": "sc_1"})");
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
    const auto resident = residentSamples(l.head(), l.app->projectDir(), 48000);
    CHECK(resident.arrangement == double(1 << 27));
    CHECK(resident.clips == double(1 << 27));
    // A copy of the same audio adds nothing; new audio does not fit.
    l.run("clip.duplicate", R"({"clip": "cl_1", "scene": "sc_2"})");
    args = j(R"({"track": "tr_1", "scene": "sc_2"})");
    l.run("track.add", "{}");
    args["track"] = "tr_2";
    args["audio_file"] = l.wav("c.wav", 1.0, .125f).string();
    const auto refused = l.bus->execute({"clip.add", args, kOrigin});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "clip.memory");
    CHECK(refused.error().message.find("268435456") != std::string::npos);
    CHECK(l.head().clips.size() == 2);
}

TEST_CASE("a clip added on a branch merges with remapped scene, clip and take ids") {
    Launcher l;
    l.run("branch.create", R"({"name": "agent/test/clips"})");
    l.run("scene.add", R"({"name": "Bridge"})");  // sc_3 on the branch
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_3", "notation": "voice bass; 1: C2:w"})");
    l.run("branch.switch", R"({"name": "main"})");
    l.run("scene.add", R"({"name": "Outro"})");                  // sc_3 on main
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_3"})");  // cl_1 on main
    l.run("branch.merge", R"({"from": "agent/test/clips"})");
    const model::Session& s = l.head();
    CHECK(s.scenes.size() == 4);
    CHECK(s.clips.size() == 2);
    std::set<std::string> names;
    for (const auto& c : s.clips) names.insert(s.scenes.find(c.value->scene)->name);
    CHECK(names == std::set<std::string>{"Outro", "Bridge"});
    CHECK(model::checkInvariants(s));
}

// --- the content commands on clips ------------------------------------------------------

TEST_CASE("midi.write, midi.get, midi.notes and midi.edit take a clip") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    l.run("clip.set", R"({"clip": "cl_1", "length": 8})");
    l.run("midi.write", R"({"item": "cl_1", "notation": "voice bass; 1: C2:q D2:q E2:q F2:q"})");
    CHECK(l.head().clips.find(id("cl_1"))->length == 8);  // the clip keeps its length
    CHECK(model::lastItemBeat(l.head()) == 4.0);
    auto notes = l.run("midi.notes", R"({"item": "cl_1"})");
    REQUIRE(notes["notes"].size() == 4);
    CHECK(notes["notes"].at(1).find("start")->asDouble() == 1.0);  // from the clip's start
    CHECK(notes["length"].asDouble() == 8.0);
    CHECK_FALSE(notes.find("position"));
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "remove", "notes": [0]}]})");
    CHECK(l.run("midi.notes", R"({"item": "cl_1"})")["notes"].size() == 3);
    CHECK(l.run("midi.get", R"({"item": "cl_1"})")["text"].asString().find("D2") !=
          std::string::npos);
    CHECK(l.fails("midi.get", R"({"item": "cl_9"})").code == "cl.unknown");
}

TEST_CASE("midi.write warns when the notation runs past the clip's or item's length") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    const auto fits = l.run("midi.write", R"({"item": "cl_1", "notation": "voice bass; 1: D2:w"})");
    CHECK(warningCodes(fits).empty());
    const auto over =
        l.run("midi.write", R"({"item": "cl_1", "notation": "voice bass; 1: C2:w; 2: D2:w"})");
    CHECK(warningCodes(over) == "midi.beyond_length ");
    CHECK(over.find("warnings")->at(0).find("path")->asString() == "/notation");
    CHECK(l.head().clips.find(id("cl_1"))->length == 4);  // it keeps its length
    // After lengthening it, the same notation fits.
    l.run("clip.set", R"({"clip": "cl_1", "length": 8})");
    CHECK(warningCodes(l.run("midi.write", R"({"item": "cl_1", "notation":
        "voice bass; 1: C2:w; 2: D2:w"})"))
              .empty());
    // An item says the same.
    l.run("item.add", R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:w"})");
    CHECK(warningCodes(l.run("midi.write", R"({"item": "it_1", "notation":
        "voice bass; 1: C2:w; 2: D2:w"})")) == "midi.beyond_length ");
}

TEST_CASE("midi.edit's scale ops on a clip read the clip's own context, never the project's") {
    Launcher l;
    // The project is in C minor from the start; the clip was written in D minor (defect D1:
    // its A1 used to be read in C minor and snapped to Ab1).
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Cm"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w"})");
    // Without a context of its own the clip has no key, whatever the project holds.
    const Error noKey =
        l.fails("midi.edit", R"({"item": "cl_1", "ops": [{"op": "snap_to_scale"}]})");
    CHECK(noKey.code == "midi.no_key");
    CHECK(noKey.hint.find("clip.set {context}") != std::string::npos);
    const Error noChord =
        l.fails("midi.edit", R"({"item": "cl_1", "ops": [{"op": "fit_to_chord"}]})");
    CHECK(noChord.code == "midi.no_chord");
    CHECK(noChord.hint.find("clip.set {context}") != std::string::npos);
    const auto pitch = [&] {
        return l.run("midi.notes", R"({"item": "cl_1"})")["notes"].at(0).find("pitch")->asInt();
    };
    // With its own key, the A stays: it is in D minor.
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor {Dm}:w"})");
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "snap_to_scale", "mode": "up"}]})");
    CHECK(pitch() == 33);  // A1, not Ab1
    // transpose_degrees moves it by D minor's degrees: A up one is Bb.
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "transpose_degrees", "notes": [0],
                           "degrees": 1}]})");
    CHECK(pitch() == 34);  // Bb1
    // fit_to_chord reads the clip's chord (Dm): Bb is nearest A (down one) or D (up four).
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "fit_to_chord"}]})");
    CHECK(pitch() == 33);
    // key and chord still replace the reading.
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "fit_to_chord", "chord": "G"}]})");
    CHECK(pitch() == 31);  // G1, the nearest G tone below
    // A clip with a key and no chord: fit_to_chord is midi.no_chord.
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w"})");
    CHECK(l.fails("midi.edit", R"({"item": "cl_1", "ops": [{"op": "fit_to_chord"}]})").code ==
          "midi.no_chord");
}

TEST_CASE("midi.edit on a clip reads the context at the clip beat each pass sounds at") {
    Launcher l;
    // Two bars (8 beats), looped over its first bar: the note at beat 0 sounds at 0 and at 4.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w"})");
    l.run("clip.set",
          R"({"clip": "cl_1", "length": 8, "loop": {"on": true, "start": 0, "length": 4}})");
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w\n2: ~:w"})");
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "snap_to_scale"}]})");  // one key
    // A key change at the second pass: the same note is in two keys.
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w\n2: key F major ~:w"})");
    const Error varies =
        l.fails("midi.edit", R"({"item": "cl_1", "ops": [{"op": "snap_to_scale"}]})");
    CHECK(varies.code == "midi.context_varies");
    CHECK(varies.message.find("keys of the clip") != std::string::npos);
    // key says which one.
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "snap_to_scale", "key": "D minor"}]})");
    // Without the loop the note sounds once, at beat 0.
    l.run("clip.set", R"({"clip": "cl_1", "loop": {"on": false}})");
    l.run("midi.edit", R"({"item": "cl_1", "ops": [{"op": "snap_to_scale"}]})");
    // Chords vary alike.
    l.run("clip.set", R"({"clip": "cl_1", "loop": {"on": true}, "context":
                          "1: key D minor {Dm}:w\n2: {Gm}:w"})");
    CHECK(l.fails("midi.edit", R"({"item": "cl_1", "ops": [{"op": "fit_to_chord"}]})").code ==
          "midi.context_varies");
}

TEST_CASE("midi.get on a clip spells by the clip's own key, and lists its context") {
    Launcher l;
    l.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: F#2:h C#3:h"})");
    // The project's key is not read: F major would spell F# as Gb.
    l.run("key.set", R"({"at": 0, "tonic": "F", "scale": "major"})");
    auto none = l.run("midi.get", R"({"item": "cl_1"})");
    CHECK(none.find("text")->asString().find("F#2") != std::string::npos);  // the default table
    CHECK_FALSE(none.find("context"));                 // a clip with no context has none to list
    CHECK(none.find("follow")->asString() == "none");  // a clip's, always present
    // The clip's own key decides: B major has sharps, F minor flats.
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key F minor ~:w"})");
    auto flats = l.run("midi.get", R"({"item": "cl_1"})");
    CHECK(flats.find("text")->asString().find("Gb2") != std::string::npos);
    CHECK(flats.find("text")->asString().find("Db3") != std::string::npos);
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key B major ~:w"})");
    CHECK(l.run("midi.get", R"({"item": "cl_1"})").find("text")->asString().find("F#2") !=
          std::string::npos);
    // The timeline item beside it still lists the project's key and chord, in its own shape.
    l.run("chord.set", R"({"at": 0, "symbol": "F"})");
    const auto item = l.run("midi.get", R"({"item": "it_1"})");
    const json::Value* context = item.find("context");
    REQUIRE(context);
    CHECK(context->find("keys")->at(0).find("project_bar"));
    CHECK_FALSE(context->find("keys")->at(0).find("beat"));
    CHECK_FALSE(context->find("text"));
    CHECK_FALSE(item.find("follow"));
}

TEST_CASE("take.list, take.add, take.activate and take.remove take a clip") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    const model::Id first = l.head().clips.find(id("cl_1"))->takes[0];
    auto added = l.runArgs(
        "take.add", json::parse(R"({"item": "cl_1", "from": ")" + first.str() + "\"}").value());
    const model::Id second = id(added["id"].asString().c_str());
    auto listed = l.run("take.list", R"({"item": "cl_1"})");
    REQUIRE(listed["takes"].size() == 2);
    CHECK(listed["takes"].at(0).find("active")->asBool());
    l.runArgs("take.activate",
              json::parse(R"({"item": "cl_1", "take": ")" + second.str() + "\"}").value());
    CHECK(l.head().clips.find(id("cl_1"))->activeTake == second);
    l.run("take.activate", R"({"item": "cl_1", "index": 0})");
    CHECK(l.head().clips.find(id("cl_1"))->activeTake == first);
    auto removed =
        l.runArgs("take.remove", json::parse(R"({"take": ")" + first.str() + "\"}").value());
    CHECK(removed["item"].asString() == "cl_1");
    CHECK_FALSE(removed["item_removed"].asBool());
    CHECK(l.head().clips.find(id("cl_1"))->activeTake == second);
    removed = l.runArgs("take.remove", json::parse(R"({"take": ")" + second.str() + "\"}").value());
    CHECK(removed["item_removed"].asBool());
    CHECK(l.head().clips.empty());
}

TEST_CASE("take.extract moves a clip's take onto the timeline at a position") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    l.run("take.add", R"({"item": "cl_1"})");
    const model::Clip clip = *l.head().clips.find(id("cl_1"));
    const model::Id heard = *clip.activeTake;
    const Error where =
        l.fails("take.extract",
                json::writeCompact(json::parse(R"({"take": ")" + heard.str() + "\"}").value()));
    CHECK(where.code == "take.position");
    auto r =
        l.runArgs("take.extract",
                  json::parse(R"({"take": ")" + heard.str() + R"(", "position": 16})").value());
    const model::Item& item = *l.head().items.find(id(r["item"].asString().c_str()));
    CHECK(item.track == id("tr_1"));  // the clip's track
    CHECK(item.takes == std::vector<model::Id>{heard});
    CHECK(model::itemSpan(l.head(), item).startBeat == 16.0);
    CHECK(model::itemLengthBeats(l.head(), item) == 4.0);
    CHECK(l.head().clips.find(id("cl_1"))->takes.size() == 1);
    CHECK(model::takeOwner(l.head(), heard) == id(r["item"].asString().c_str()));
    const model::Id last = l.head().clips.find(id("cl_1"))->takes[0];
    l.runArgs("take.extract",
              json::parse(R"({"take": ")" + last.str() + R"(", "position": 32})").value());
    CHECK(l.head().clips.empty());  // the clip goes with its last take
}

TEST_CASE("envelopes on a clip take's FX are in clip time, and convert with the take") {
    // ADR-0097, amended 2026-11-20 (C2).
    Launcher l;
    const auto fxOn = [&](model::Id take) {
        auto r = l.runArgs("fx.add", json::parse(R"({"take": ")" + take.str() +
                                                 R"(", "plugin": "com.suppleaudio.test.gain"})")
                                         .value());
        return id(r["fx"].asString().c_str());
    };
    const auto envOn = [&](model::Id fx) -> const model::Envelope* {
        for (const auto& e : l.head().envelopes)
            if (e.value->target.object == fx) return e.value.get();
        return nullptr;
    };
    const auto fxOf = [&](model::Id take) { return l.head().takes.find(take)->chain.at(0); };
    // An audio clip's: seconds from its start, never beats or bars.
    auto args = j(R"({"track": "tr_1", "scene": "sc_2"})");
    args["audio_file"] = l.wav("pad.wav", 3.0).string();
    l.runArgs("clip.add", args);
    const model::Id audioTake = l.head().clips.find(id("cl_1"))->takes[0];
    const model::Id audioFx = fxOn(audioTake);
    CHECK(l.fails("env.add",
                  json::writeCompact(
                      json::parse(R"({"fx": ")" + audioFx.str() +
                                  R"(", "param": "gain", "points": [{"beat": 1, "value": -6}]})")
                          .value()))
              .code == "clip.unsupported");
    l.runArgs("env.add",
              json::parse(R"({"fx": ")" + audioFx.str() +
                          R"(", "param": "gain", "points": [{"seconds": 0.5, "value": -6},
                                                                       {"seconds": 1, "value": 0}]})")
                  .value());
    const model::Envelope* inClip = envOn(audioFx);
    REQUIRE(inClip);
    CHECK(inClip->timebase == model::Timebase::Time);
    CHECK(inClip->points.at(0).pos == 0.5);
    // No automation items in clip time.
    model::Id envId;
    for (const auto& e : l.head().envelopes)
        if (e.value->target.object == audioFx) envId = e.id;
    CHECK(
        l.fails("autoitem.add", json::writeCompact(json::parse(R"({"env": ")" + envId.str() +
                                                               R"(", "position": 0, "length": 4})")
                                                       .value()))
            .code == "autoitem.take");
    // No write pass: it would write timeline positions.
    CHECK(l.fails("env.set",
                  json::writeCompact(
                      json::parse(R"({"env": ")" + envId.str() + R"(", "mode": "touch"})").value()))
              .code == "env.mode");
    // A range is in the clip's seconds: only the point at 1 s is replaced.
    l.runArgs("env.points.set",
              json::parse(
                  R"({"env": ")" + envId.str() +
                  R"(", "from_beat": 0.75, "to_beat": 2, "points": [{"seconds": 1, "value": -3}]})")
                  .value());
    REQUIRE(envOn(audioFx)->points.size() == 2);
    CHECK(envOn(audioFx)->points.at(0).value == -6.0);
    CHECK(envOn(audioFx)->points.at(1).value == -3.0);
    l.runArgs("env.points.set",
              json::parse(
                  R"({"env": ")" + envId.str() +
                  R"(", "from_beat": 0.75, "to_beat": 2, "points": [{"seconds": 1, "value": 0}]})")
                  .value());
    // Onto the timeline at beat 8 (120 BPM): timeline beats, 1 beat per 0.5 s.
    auto placed = l.run("item.add", R"({"track": "tr_1", "position": 8, "clip": "cl_1"})");
    const model::Item& copy = *l.head().items.find(id(placed["item"].asString().c_str()));
    const model::Envelope* onItem = envOn(fxOf(copy.takes[0]));
    REQUIRE(onItem);
    CHECK(onItem->timebase == model::Timebase::Beats);
    CHECK(onItem->points.at(0).pos == doctest::Approx(9.0));
    CHECK(onItem->points.at(1).pos == doctest::Approx(10.0));
    // And back into a clip: seconds from its start again.
    l.runArgs("clip.add", json::parse(R"({"track": "tr_1", "scene": "sc_1", "item": ")" +
                                      placed["item"].asString() + "\"}")
                              .value());
    const model::Id back = l.head().clips.find(id("cl_2"))->takes[0];
    const model::Envelope* again = envOn(fxOf(back));
    REQUIRE(again);
    CHECK(again->timebase == model::Timebase::Time);
    CHECK(again->points.at(0).pos == doctest::Approx(0.5));
    CHECK(again->points.at(1).pos == doctest::Approx(1.0));
    // A MIDI clip's: beats from its start; extracted to beat 16, the
    // timeline's beat 16 on.
    l.run("track.add", R"({"name": "Keys"})");
    l.run("clip.add", R"({"track": "tr_2", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    const model::Id midiTake = l.head().clips.find(id("cl_3"))->takes[0];
    l.runArgs("fx.add", json::parse(R"({"take": ")" + midiTake.str() +
                                    R"(", "plugin": "com.suppleaudio.test.sine"})")
                            .value());
    const model::Id gain = fxOn(midiTake);
    l.runArgs("env.add", json::parse(R"({"fx": ")" + gain.str() +
                                     R"(", "param": "gain", "points": [{"beat": 2, "value": -6}]})")
                             .value());
    CHECK(envOn(gain)->points.at(0).pos == 2.0);
    l.runArgs("take.extract",
              json::parse(R"({"take": ")" + midiTake.str() + R"(", "position": 16})").value());
    CHECK(envOn(gain)->timebase == model::Timebase::Beats);
    CHECK(envOn(gain)->points.at(0).pos == doctest::Approx(18.0));
}

TEST_CASE("env.add on a clip's take measures in the clip's unit; fx.add works") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    const model::Id midiTake = l.head().clips.find(id("cl_1"))->takes[0];
    l.runArgs("fx.add", json::parse(R"({"take": ")" + midiTake.str() +
                                    R"(", "plugin": "com.suppleaudio.test.sine"})")
                            .value());
    l.runArgs("env.add",
              json::parse(R"({"take": ")" + midiTake.str() +
                          R"(", "param": "volume", "points": [{"beat": 2, "value": -6}]})")
                  .value());
    const Error bar = l.fails(
        "env.add",
        json::writeCompact(json::parse(R"({"take": ")" + midiTake.str() +
                                       R"(", "param": "pan", "points": [{"bar": 1, "value": 0}]})")
                               .value()));
    CHECK(bar.code == "clip.unsupported");
    auto args = j(R"({"track": "tr_1", "scene": "sc_2"})");
    args["audio_file"] = l.wav("pad.wav").string();
    l.runArgs("clip.add", args);
    const model::Id audioTake = l.head().clips.find(id("cl_2"))->takes[0];
    l.runArgs("env.add",
              json::parse(R"({"take": ")" + audioTake.str() +
                          R"(", "param": "volume", "points": [{"seconds": 0.5, "value": -6}]})")
                  .value());
    double pos = -1;
    for (const auto& e : l.head().envelopes)
        if (e.value->target.object == audioTake) pos = e.value->points.at(0).pos;
    CHECK(pos == 0.5);
    CHECK(l.fails("env.add",
                  json::writeCompact(
                      json::parse(R"({"take": ")" + audioTake.str() +
                                  R"(", "param": "pan", "points": [{"beat": 1, "value": 0}]})")
                          .value()))
              .code == "clip.unsupported");
    // take.hits work on any take; stretch markers are refused on a clip's.
    l.runArgs(
        "take.hits.set",
        json::parse(R"({"take": ")" + audioTake.str() + R"(", "positions": [0.25]})").value());
    l.runArgs("take.set", json::parse(R"({"take": ")" + audioTake.str() +
                                      R"(", "preserve_pitch": true, "pitch_mode": "signalsmith"})")
                              .value());
    CHECK(
        l.fails(
             "take.stretch.add",
             json::writeCompact(
                 json::parse(R"({"take": ")" + audioTake.str() + R"(", "item_pos": 0.5})").value()))
            .code == "clip.unsupported");
}

TEST_CASE("item.export writes a clip's MIDI") {
    Launcher l;
    l.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:q D2:q E2:q r:q"})");
    auto args = j(R"({"item": "cl_1"})");
    args["path"] = (l.root / "riff.mid").string();
    auto r = l.runArgs("item.export", args);
    CHECK(r["format"].asString() == "mid");
    CHECK(r["notes"].asInt() == 3);
}

TEST_CASE("item.export of a following item writes what sounds, as after a bake (round 85 G4)") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    const auto exported = [&](const char* name) {
        auto args = j(R"({"item": "it_1"})");
        args["path"] = (l.root / name).string();
        l.runArgs("item.export", args);
        auto seq = midi::readSmf(readFile(l.root / name).value());
        REQUIRE(seq);
        std::string out;
        for (const auto& n : seq->notes)
            out += (out.empty() ? "" : " ") + std::to_string(n.pitch) + "@" +
                   std::to_string(n.start * 960 / seq->ppq);
        return out;
    };
    const std::string following = exported("following.mid");
    // The stored notes are C major's (36 40 43 48); the file has the pitches that sound.
    CHECK(following == "39@0 43@960 46@1920 51@2880");
    l.run("take.bake", R"({"take": "tk_1"})");
    CHECK(exported("baked.mid") == following);
}

TEST_CASE("timeline commands refuse a clip with item.kind") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");
    const std::pair<const char*, const char*> calls[] = {
        {"item.move", R"({"item": "cl_1", "position": 4})"},
        {"item.set", R"({"item": "cl_1", "mute": true})"},
        {"item.split", R"({"item": "cl_1", "at": 2})"},
        {"item.duplicate", R"({"item": "cl_1"})"},
        {"item.remove", R"({"item": "cl_1"})"},
        {"item.paste", R"({"items": [{"item": "cl_1", "track": "tr_1", "position": 8}]})"},
        {"item.consolidate", R"({"items": ["cl_1"]})"},
        {"item.heal", R"({"items": ["cl_1", "it_1"]})"},
        {"item.mix-to-track", R"({"items": ["cl_1"]})"},
        {"audio.quantize", R"({"items": ["cl_1"], "grid": 1})"},
        {"audio.warp.reset", R"({"items": ["cl_1"]})"},
        {"audio.warp.detach", R"({"items": ["cl_1"]})"},
        {"group.targets", R"({"item": "cl_1", "attribute": "edit"})"},
        {"item.explode-takes", R"({"item": "cl_1"})"},
        {"item.apply-fx", R"({"item": "cl_1"})"},
    };
    for (const auto& [name, args] : calls) {
        CAPTURE(name);
        auto result = l.bus->execute({name, j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "item.kind");
        CHECK(result.error().path.find("/args/item") == 0);
        CHECK_FALSE(result.error().hint.empty());
    }
    CHECK(l.head().clips.size() == 1);
    CHECK(l.head().items.size() == 1);
}

TEST_CASE("a content edit on a branch's clip merges with the clip's id remapped") {
    Launcher l;
    l.run("branch.create", R"({"name": "agent/test/riff"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");  // cl_1 on the branch
    l.run("midi.write", R"({"item": "cl_1", "notation": "voice bass; 1: C2:q D2:q E2:q F2:q"})");
    l.run("branch.switch", R"({"name": "main"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2"})");  // cl_1 on main
    l.run("branch.merge", R"({"from": "agent/test/riff"})");
    const model::Session& s = l.head();
    REQUIRE(s.clips.size() == 2);
    const auto verse = model::clipAt(s, id("tr_1"), id("sc_1"));
    REQUIRE(verse);
    CHECK(*verse != id("cl_1"));  // remapped past main's
    const model::MidiSource* m = s.midi.find(*s.takes.find(s.clips.find(*verse)->takes[0])->source);
    CHECK(m->sequence.notes.size() == 4);
    const model::MidiSource* other =
        s.midi.find(*s.takes.find(s.clips.find(id("cl_1"))->takes[0])->source);
    CHECK(other->sequence.notes.empty());
}

TEST_CASE("clip audio is prepared after the arrangement, in launch order, as far as it fits") {
    // ADR-0097 "Memory". Three mono sources of 2^26 frames (hand-edited, as
    // above): each costs 2^27 decoded and resampled, so two fill the budget.
    // Launch order is scene, then track display order: cl_3, cl_1, cl_2,
    // cl_4. cl_2's source does not fit and is skipped; cl_4 shares cl_1's
    // source and costs nothing, so it still fits after the skip.
    Launcher l;
    l.run("item.remove", R"({"item": "it_1"})");
    l.run("track.add", R"({"name": "Pad"})");  // tr_2
    auto args = j(R"({"track": "tr_2", "scene": "sc_1"})");
    args["audio_file"] = l.wav("a.wav").string();
    l.runArgs("clip.add", args);  // cl_1
    args = j(R"({"track": "tr_1", "scene": "sc_2"})");
    args["audio_file"] = l.wav("b.wav", 1.0, .5f).string();
    l.runArgs("clip.add", args);  // cl_2
    args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = l.wav("c.wav", 1.0, .125f).string();
    l.runArgs("clip.add", args);                                      // cl_3
    l.run("clip.duplicate", R"({"clip": "cl_1", "scene": "sc_2"})");  // cl_4
    const auto path = l.root / "order.steppe";
    auto save = json::Value::object();
    save["path"] = path.string();
    l.runArgs("project.save", save);
    auto doc = json::parse(*readTextFile(path)).value();
    for (auto& m : doc["sources"]["media"].asObject()) m.value["frames"] = 1 << 26;
    REQUIRE(writeFileAtomic(path, std::string_view(json::writeCanonical(doc))));
    l.runArgs("project.open", save);
    const auto order = clipsInLaunchOrder(l.head());
    CHECK(order == std::vector<model::Id>{id("cl_3"), id("cl_1"), id("cl_2"), id("cl_4")});
    const auto resident = residentSamples(l.head(), l.app->projectDir(), 48000);
    CHECK(resident.arrangement == 0.0);
    CHECK(resident.clipsSkipped == std::vector<model::Id>{id("cl_2")});
    MediaCache cache;
    auto prepared = cache.prepareSources(l.head(), l.app->projectDir(), 48000);
    REQUIRE(prepared);
    CHECK((*prepared)->clipsSkipped == std::vector<model::Id>{id("cl_2")});
    const auto takeOf = [&](const char* clip) {
        return *l.head().clips.find(id(clip))->activeTake;
    };
    CHECK((*prepared)->takes.contains(takeOf("cl_1")));
    CHECK((*prepared)->takes.contains(takeOf("cl_3")));
    CHECK((*prepared)->takes.contains(takeOf("cl_4")));
    CHECK_FALSE((*prepared)->takes.contains(takeOf("cl_2")));
    {
        // The running plan skips it too: it cannot be launched, and a scene
        // launch leaves it out with a warning.
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(l.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                                   EngineOptions{.anticipative = false, .workers = 0}));
        CHECK(l.fails("launch.clip", R"({"clip": "cl_2"})").code == "launch.memory");
        auto scene = l.run("launch.scene", R"({"scene": "sc_2"})");
        REQUIRE(scene["tracks"].size() == 1);
        CHECK(scene["tracks"].at(0).find("clip")->asString() == "cl_4");
        CHECK(scene["warnings"].at(0).find("code")->asString() == "launch.memory");
        l.app->stopEngine();
    }
    // Moving cl_2 to the first column puts it before cl_1: now cl_1 is skipped.
    l.run("scene.move", R"({"scene": "sc_2", "to": 0})");
    CHECK(residentSamples(l.head(), l.app->projectDir(), 48000).clipsSkipped ==
          std::vector<model::Id>{id("cl_3")});
}

// --- clip context (ADR-0098, step X1) ------------------------------------------------

namespace {

const model::Clip& clipOf(const Launcher& l, const char* clip) {
    const model::Clip* c = l.head().clips.find(id(clip));
    REQUIRE(c);
    return *c;
}

// The clip's points as text, beat first: "key@0 D minor, chord@0 Dm".
std::string pointsOf(const model::Clip& c) {
    std::string out;
    for (const auto& p : c.context) {
        char at[32];
        std::snprintf(at, sizeof at, "%g", p.beat);
        out += std::string(out.empty() ? "" : ", ") +
               (p.type == model::ContextType::Key
                    ? "key@" + std::string(at) + " " + p.tonic + " " + p.scale
                    : "chord@" + std::string(at) + " " + p.symbol);
    }
    return out;
}

// What the last undo step recorded of its command: the arguments history keeps.
json::Value lastRecorded(const Launcher& l) {
    const auto* v = l.app->versions().history(1).front();
    return v->txn.invocations.back().args;
}

// A project whose context track reads C minor / Cm, then from beat 16 D minor / Dm and
// A7 at 20.
void seedContext(Launcher& l) {
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 16, "tonic": "D", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Cm"})");
    l.run("chord.set", R"({"at": 16, "symbol": "Dm"})");
    l.run("chord.set", R"({"at": 20, "symbol": "A7"})");
}

const char* kTwoBars = R"("notation": "voice bass; 1: D2:w; 2: A1:w")";

}  // namespace

TEST_CASE("clip.add {context} keeps the key and chords as a lead sheet in clip bars") {
    Launcher l;
    l.run("clip.add",
          std::string(R"({"track": "tr_1", "scene": "sc_1", )") + kTwoBars +
              R"(, "context": "1: key D minor {Dm}:w\n2: {A7}:h {Dm}:h", "follow": "key"})");
    const model::Clip& c = clipOf(l, "cl_1");
    CHECK(pointsOf(c) == "key@0 D minor, chord@0 Dm, chord@4 A7, chord@6 Dm");
    CHECK(c.follow == model::ClipFollow::Key);
    // midi.get answers it in the item's shape: runs by clip bar and beat, the text and follow.
    const auto got = l.run("midi.get", R"({"item": "cl_1"})");
    const json::Value* context = got.find("context");
    REQUIRE(context);
    CHECK(context->find("text")->asString() == "1: key D minor {Dm}:w\n2: {A7}:h {Dm}:h\n");
    CHECK(context->find("keys")->at(0).find("what")->asString() == "D minor");
    CHECK(context->find("keys")->at(0).find("beat")->asDouble() == 0.0);
    CHECK_FALSE(context->find("keys")->at(0).find("project_bar"));
    const auto& chords = *context->find("chords");
    REQUIRE(chords.size() == 3);
    CHECK(chords.at(1).find("what")->asString() == "A7");
    CHECK(chords.at(1).find("item_bar")->asInt() == 2);
    CHECK(chords.at(1).find("beat")->asDouble() == 4.0);
    CHECK(got.find("follow")->asString() == "key");
    // The names follow the clip's own key: the D minor line reads in flats' absence.
    CHECK(got.find("text")->asString().find("D2") != std::string::npos);
    // session.describe names the key and the follow.
    const std::string text = l.run("session.describe", "{}")["text"].asString();
    CHECK(text.find("cl_1 in D minor ") != std::string::npos);
    CHECK(text.find("· follows the key") != std::string::npos);
    // The text reads back as the same points, and the file keeps them.
    const std::string printed = context->find("text")->asString();
    l.run("clip.set", R"({"clip": "cl_1", "context": ""})");
    CHECK(clipOf(l, "cl_1").context.empty());
    auto args = j(R"({"clip": "cl_1"})");
    args["context"] = printed;
    l.runArgs("clip.set", args);
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm, chord@4 A7, chord@6 Dm");
    auto reopened = model::parseProject(model::serializeProject(l.head()));
    REQUIRE(reopened);
    CHECK(reopened->clips.find(id("cl_1"))->context == clipOf(l, "cl_1").context);
}

TEST_CASE("clip.add context errors name the clip bar and change nothing") {
    Launcher l;
    const auto before = l.head().version;
    const auto refuses = [&](std::string extra, const char* code, const char* in = "") {
        const Error e = l.fails("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") +
                                                kTwoBars + ", " + extra + "}");
        CHECK_MESSAGE(e.code == code, e.toString());
        CHECK_MESSAGE(e.message.find(in) != std::string::npos, e.toString());
        CHECK(l.head().version == before);
        return e;
    };
    auto both = refuses(R"("context": "1: {Dm}:w", "context_at": {"bar": 1})", "clip.context_args");
    CHECK(both.path == "/args/context_at");
    const auto syntax = refuses(R"("context": "{Dm}:w")", "clip.context_syntax");
    CHECK(syntax.path == "/args/context");
    CHECK(refuses(R"("context": "1: {Dm}:h")", "context.bar_length", "bar 1").path ==
          "/args/context");
    // The clip is two bars (8 beats): a point in bar 3 is past its end, though bars may run on.
    CHECK(
        refuses(R"("context": "1: ~:w\n2: ~:w\n3: {Dm}:w")", "clip.context_range", "bar 3").path ==
        "/args/context");
    // follow needs a MIDI clip with a key at its start.
    CHECK(refuses(R"("follow": "key")", "clip.no_key").path == "/args/follow");
    CHECK(refuses(R"("follow": "chords", "context": "1: {Dm}:w")", "clip.no_key").code ==
          "clip.no_key");
    // context_at takes a bar or beats, one of them.
    CHECK(refuses(R"("context_at": {})", "clip.context_args").path == "/args/context_at");
    CHECK(refuses(R"("context_at": {"bar": 1, "at": 4})", "clip.context_args").code ==
          "clip.context_args");
    // Bars may run past the clip's end; only a point may not.
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") + kTwoBars +
                          R"(, "context": "1: key D minor {Dm}:w\n2: ~:w\n3: ~:w"})");
}

TEST_CASE("clip.add on an audio clip: a key at the start only, and no follow") {
    Launcher l;
    auto args = j(R"({"track": "tr_1", "scene": "sc_1"})");
    args["audio_file"] = l.wav("pad.wav", 2.0).string();
    const auto add = [&](const char* extra) {
        auto a = args;
        const auto extras = j(extra);
        for (const auto& m : extras.asObject()) a[m.key] = m.value;
        return l.bus->execute({"clip.add", a, kOrigin});
    };
    const auto refused = [&](const char* extra, const char* code) {
        const auto r = add(extra);
        REQUIRE_FALSE(r);
        CHECK_MESSAGE(r.error().code == code, r.error().toString());
        CHECK_FALSE(r.error().hint.empty());
    };
    refused(R"({"context": "1: key D minor {Dm}:w"})", "clip.context_audio");
    refused(R"({"context": "1: key D minor r:w"})",
            "clip.context_audio");  // a rest is a chord point
    refused(R"({"context": "1: ~:h key D minor ~:h"})", "clip.context_audio");  // not at the start
    refused(R"({"follow": "key"})", "clip.follow_audio");
    refused(R"({"context": "1: key D minor ~:w", "follow": "key"})", "clip.follow_audio");
    const auto ok = add(R"({"context": "1: key D minor ~:w"})");
    REQUIRE_MESSAGE(ok, (ok ? "" : ok.error().toString()));
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor");
    // describe names an audio clip's key too.
    CHECK(l.run("session.describe", "{}")["text"].asString().find("cl_1 in D minor ") !=
          std::string::npos);
    // Setting follow on it later is refused the same way.
    const Error e = l.fails("clip.set", R"({"clip": "cl_1", "follow": "key"})");
    CHECK(e.code == "clip.follow_audio");
}

TEST_CASE("clip.add {context_at} reads the project's context for the clip's length") {
    Launcher l;
    seedContext(l);
    // Bar 5 is beat 16: D minor and Dm at the start, the A7 change at beat 4 of the clip.
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") + kTwoBars +
                          R"(, "context_at": {"bar": 5}})");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm, chord@4 A7");
    // History records the context the user saw, not the place it was read from.
    const json::Value recorded = lastRecorded(l);
    CHECK_FALSE(recorded.find("context_at"));
    REQUIRE(recorded.find("context"));
    CHECK(recorded.find("context")->asString() == "1: key D minor {Dm}:w\n2: {A7}:w\n");
    // The same place in beats; the key holding mid-clip is carried to its start.
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_2", )") + kTwoBars +
                          R"(, "context_at": {"at": 18}})");
    CHECK(pointsOf(clipOf(l, "cl_2")) == "key@0 D minor, chord@0 Dm, chord@2 A7");
    // Before any key, no key: the clip has none and the answer warns.
    Launcher bare;
    const auto none = bare.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") +
                                               kTwoBars + R"(, "context_at": {"bar": 1}})");
    CHECK(warningCodes(none).find("context.no_key") != std::string::npos);
    CHECK(clipOf(bare, "cl_1").context.empty());
}

TEST_CASE("clip.add {item} takes the context under the item; a stated one wins") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Cm"})");
    l.run("chord.set", R"({"at": 2, "symbol": "Fm"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 C minor, chord@0 Cm, chord@2 Fm");
    // An item is taken by reference, so history keeps the reference and replay reads the
    // context again with the notes; only context_at records the text it resolved to.
    const json::Value recorded = lastRecorded(l);
    CHECK_FALSE(recorded.find("context"));
    CHECK(recorded.find("item")->asString() == "it_1");
    // A stated context, even none, is the clip's.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "item": "it_1", "context": ""})");
    CHECK(clipOf(l, "cl_2").context.empty());
    CHECK(lastRecorded(l).find("context")->asString().empty());
    // No key under the item: nothing is taken, and no warning (the project may have no context).
    Launcher bare;
    const auto r = bare.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    CHECK(clipOf(bare, "cl_1").context.empty());
    CHECK(warningCodes(r).empty());
}

TEST_CASE(
    "a replay re-reads what an item is taken by; context_at replays the text it resolved to") {
    // ADR-0098, the history rule: a command that takes project material by reference
    // re-reads all of it on replay; context_at, which states a place for the context alone,
    // records the resolved text.
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Cm"})");
    l.run("branch.create", R"({"name": "agent/test/ctx"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");  // cl_1 (item)
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_2", )") + kTwoBars +
                          R"(, "context_at": {"bar": 1}})");  // cl_2 (context_at)
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 C minor, chord@0 Cm");
    CHECK(pointsOf(clipOf(l, "cl_2")) == "key@0 C minor, chord@0 Cm");
    // Main moves: the key under bar 1 changes. The merge replays both steps there.
    l.run("branch.switch", R"({"name": "main"})");
    l.run("key.set", R"({"at": 0, "tonic": "E", "scale": "minor"})");
    l.run("branch.merge", R"({"from": "agent/test/ctx"})");
    const model::Session& s = l.head();
    const auto fromItem = model::clipAt(s, id("tr_1"), id("sc_1"));
    REQUIRE(fromItem);
    // The item case reads the context under the item on this branch: E minor, as its notes.
    CHECK(pointsOf(*s.clips.find(*fromItem)) == "key@0 E minor, chord@0 Cm");
    // context_at replays the context the user saw: C minor.
    const auto fromPlace = model::clipAt(s, id("tr_1"), id("sc_2"));
    REQUIRE(fromPlace);
    CHECK(pointsOf(*s.clips.find(*fromPlace)) == "key@0 C minor, chord@0 Cm");
}

TEST_CASE(
    "a key or chord this build cannot read is not taken, and nothing unreadable is recorded") {
    // ADR-0098: unreadable is absent. The clip does not take the point, the answer names it
    // (clip.context_dropped), and the resolved text history keeps holds none of it, so a
    // merge replays it.
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Cm"})");
    auto tx = l.app->beginTransaction(kOrigin, "a later build's points");
    REQUIRE(tx);
    model::ContextPoint key;  // a scale from a later build, at beat 16 (bar 5)
    key.beat = 16;
    key.tonic = "D";
    key.scale = "zz_future_scale";
    l.app->txn().create(key);
    model::ContextPoint chord;  // a chord quality from a later build, at beat 20
    chord.beat = 20;
    chord.type = model::ContextType::Chord;
    chord.symbol = "Cm11";
    l.app->txn().create(chord);
    REQUIRE(l.app->commitTransaction(*tx));
    const auto dropped = [](const json::Value& r) {
        std::string out;
        if (const auto* w = r.find("warnings"))
            for (const auto& e : w->asArray())
                if (e.find("code")->asString() == "clip.context_dropped")
                    out += e.find("message")->asString() + "\n";
        return out;
    };
    l.run("branch.create", R"({"name": "agent/test/later"})");
    // The span from beat 14 holds both: C minor and Cm are taken, the later build's two named.
    const auto spanned = l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") +
                                               kTwoBars + R"(, "context_at": {"at": 14}})");
    // The chord the later build wrote at beat 6 is "no chord" in the clip, as it reads in the
    // project: Cm does not carry on over it (H4); the key, which has no such point, says that
    // the earlier one does.
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 C minor, chord@0 Cm, chord@6 ");
    CHECK_FALSE(model::chordAt(clipOf(l, "cl_1"), 7).has_value());
    CHECK(model::chordAt(clipOf(l, "cl_1"), 5).has_value());
    CHECK(dropped(spanned).find("no chord from there") != std::string::npos);
    CHECK(dropped(spanned).find("earlier key carries on") != std::string::npos);
    CHECK(dropped(spanned).find("zz_future_scale") != std::string::npos);
    CHECK(dropped(spanned).find("Cm11") != std::string::npos);
    const std::string text = lastRecorded(l).find("context")->asString();
    CHECK(text.find("zz_future_scale") == std::string::npos);
    CHECK(text.find("Cm11") == std::string::npos);
    // The unreadable key holds at the start: the clip has no key (so it cannot follow), the
    // answer names the key and the later chord, and no context.no_key is added to it.
    const auto held = l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_2", )") +
                                            kTwoBars + R"(, "context_at": {"bar": 5}})");
    CHECK(pointsOf(clipOf(l, "cl_2")) == "chord@0 Cm, chord@4 ");
    CHECK(dropped(held).find("zz_future_scale") != std::string::npos);
    CHECK(warningCodes(held).find("context.no_key") == std::string::npos);
    // The same under an item, and by scene.capture: named, not taken.
    l.run("item.add", R"({"track": "tr_1", "bar": 5, "notation": "voice bass; 1: C2:w"})");
    l.run("scene.add", R"({"name": "Bridge"})");
    const auto under = l.run("clip.add", R"({"track": "tr_1", "scene": "sc_3", "item": "it_2"})");
    CHECK(pointsOf(clipOf(l, "cl_3")) == "chord@0 Cm");
    CHECK(dropped(under).find("zz_future_scale") != std::string::npos);
    l.run("region.add", R"({"start": 16, "end": 20, "name": "Bridge"})");
    l.run("scene.add", R"({"name": "Take"})");
    const auto captured = l.run("scene.capture", R"({"region": "rg_1", "name": "Taken"})");
    CHECK(dropped(captured).find("zz_future_scale") != std::string::npos);
    // The merge replays the branch's steps on a main that moved, with none refused.
    l.run("branch.switch", R"({"name": "main"})");
    l.run("key.set", R"({"at": 0, "tonic": "E", "scale": "minor"})");
    const auto merged =
        l.bus->execute({"branch.merge", j(R"({"from": "agent/test/later"})"), kOrigin});
    REQUIRE_MESSAGE(merged, (merged ? "" : merged.error().toString()));
    const auto first = model::clipAt(l.head(), id("tr_1"), id("sc_1"));
    REQUIRE(first);
    CHECK(pointsOf(*l.head().clips.find(*first)) ==
          "key@0 C minor, chord@0 Cm, chord@6 ");  // as it saw
}

TEST_CASE("clip.add records the chord symbols of its notation, by precedence") {
    Launcher l;
    const std::string base = R"({"track": "tr_1", "scene": "sc_1", "notation": )";
    const std::string text = R"("voice bass; 1: {Dm}:w; 2: {A7}:h {Dm}:h")";
    // No stated context: the symbols' chords, at their clip beats; no key.
    const auto plain = l.run("clip.add", base + text + "}");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "chord@0 Dm, chord@4 A7, chord@6 Dm");
    CHECK(warningCodes(plain).empty());
    // A stated key only takes the symbols' chords too.
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_2", "notation": )") + text +
                          R"(, "context": "1: key D minor ~:w"})");
    CHECK(pointsOf(clipOf(l, "cl_2")) == "key@0 D minor, chord@0 Dm, chord@4 A7, chord@6 Dm");
    // A stated context holding chords wins; the symbols are ignored, with a warning.
    l.run("scene.add", R"({"name": "Bridge"})");
    const auto ignored =
        l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_3", "notation": )") + text +
                              R"(, "context": "1: key D minor {Gm}:w"})");
    CHECK(pointsOf(clipOf(l, "cl_3")) == "key@0 D minor, chord@0 Gm");
    CHECK(warningCodes(ignored).find("clip.chords_ignored") != std::string::npos);
    // context_at gives the place's points as a stated context would: a place with chords
    // wins over the symbols, a place with a key only takes them.
    seedContext(l);
    l.run("scene.add", R"({"name": "Outro"})");
    const auto atChords =
        l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_4", "notation": )") + text +
                              R"(, "context_at": {"bar": 5}})");
    CHECK(pointsOf(clipOf(l, "cl_4")) == "key@0 D minor, chord@0 Dm, chord@4 A7");
    CHECK(warningCodes(atChords).find("clip.chords_ignored") != std::string::npos);
    Launcher keyOnly;
    keyOnly.run("key.set", R"({"at": 0, "tonic": "D", "scale": "minor"})");
    const auto atKey =
        keyOnly.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", "notation": )") +
                                    text + R"(, "context_at": {"bar": 1}})");
    CHECK(pointsOf(clipOf(keyOnly, "cl_1")) == "key@0 D minor, chord@0 Dm, chord@4 A7, chord@6 Dm");
    CHECK(warningCodes(atKey).find("clip.chords_ignored") == std::string::npos);
    // ... and the history of the last replays to the same points: key only, symbols taken.
    CHECK(lastRecorded(keyOnly).find("context")->asString() == "1: key D minor ~:w\n2: ~:w\n");
    // Equal neighbours collapse, a tie adds none, and of two voices the earlier wins.
    l.run("scene.add", R"({"name": "Coda"})");
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_5", "notation": )") +
                          R"x("voice bass; 1: {Dm}:h {Dm}:h; 2: {Gm}:h~ {Gm}:h"})x");
    CHECK(pointsOf(clipOf(l, "cl_5")) == "chord@0 Dm, chord@4 Gm");
    l.run("scene.add", R"({"name": "Fin"})");
    const auto conflict =
        l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_6", "notation": )") +
                              R"x("voice a; 1: {Dm}:w; voice b; 1: {Gm}:w"})x");
    CHECK(pointsOf(clipOf(l, "cl_6")) == "chord@0 Dm");
    CHECK(warningCodes(conflict).find("clip.chord_conflict") != std::string::npos);
    // Symbols that say what the stated chords say are not news: no warning when every symbol
    // is the chord holding at its beat (a stated chord may hold across several symbols).
    l.run("scene.add", R"({"name": "Same"})");
    const auto agreeing =
        l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_7", "notation": )") +
                              R"x("voice bass; 1: {Dm}:w; 2: {Dm}:h {Dm}:h", )x" +
                              R"x("context": "1: key D minor {Dm}:w\n2: ~:w"})x");
    CHECK(pointsOf(clipOf(l, "cl_7")) == "key@0 D minor, chord@0 Dm");
    CHECK(warningCodes(agreeing).find("clip.chords_ignored") == std::string::npos);
    // One symbol that differs from the chord holding at its beat brings the warning back.
    l.run("scene.add", R"({"name": "Differ"})");
    const auto differing =
        l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_8", "notation": )") +
                              R"x("voice bass; 1: {Dm}:w; 2: {Dm}:h {Gm}:h", )x" +
                              R"x("context": "1: key D minor {Dm}:w\n2: ~:w"})x");
    CHECK(warningCodes(differing).find("clip.chords_ignored") != std::string::npos);
}

TEST_CASE("clip.set: context, follow, and the edits that keep the invariants in the same step") {
    Launcher l;
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") + kTwoBars +
                          R"(, "context": "1: key D minor {Dm}:w\n2: {A7}:w"})");
    l.run("clip.set", R"({"clip": "cl_1", "follow": "chords"})");
    CHECK(clipOf(l, "cl_1").follow == model::ClipFollow::Chords);
    // Shortening drops the points at or past the new length, and says so.
    const auto shorter = l.run("clip.set", R"({"clip": "cl_1", "length": 4})");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm");
    CHECK(warningCodes(shorter).find("clip.context_dropped") != std::string::npos);
    CHECK(shorter.find("warnings")->at(0).find("message")->asString().find("A7") !=
          std::string::npos);
    l.run("history.undo", "{}");  // the points and the length come back together
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm, chord@4 A7");
    CHECK(clipOf(l, "cl_1").length == 8.0);
    // Clearing the context resets a follow that needs its key, in the same step.
    const auto cleared = l.run("clip.set", R"({"clip": "cl_1", "context": ""})");
    CHECK(clipOf(l, "cl_1").context.empty());
    CHECK(clipOf(l, "cl_1").follow == model::ClipFollow::None);
    CHECK(warningCodes(cleared).find("clip.follow_reset") != std::string::npos);
    l.run("history.undo", "{}");
    CHECK(clipOf(l, "cl_1").follow == model::ClipFollow::Chords);
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm, chord@4 A7");
    // A text with no key does the same.
    const auto keyless = l.run("clip.set", R"({"clip": "cl_1", "context": "1: {Dm}:w\n2: ~:w"})");
    CHECK(warningCodes(keyless).find("clip.follow_reset") != std::string::npos);
    CHECK(clipOf(l, "cl_1").follow == model::ClipFollow::None);
    // follow asked for without a key is refused, not reset.
    CHECK(l.fails("clip.set", R"({"clip": "cl_1", "follow": "key"})").code == "clip.no_key");
    CHECK(
        l.fails("clip.set", R"({"clip": "cl_1", "context": "", "context_at": {"bar": 1}})").code ==
        "clip.context_args");
    // context_at on a set is recorded as the context it read.
    seedContext(l);
    l.run("clip.set", R"({"clip": "cl_1", "context_at": {"bar": 5}, "follow": "key"})");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm, chord@4 A7");
    CHECK(clipOf(l, "cl_1").follow == model::ClipFollow::Key);
    CHECK_FALSE(lastRecorded(l).find("context_at"));
    CHECK(lastRecorded(l).find("context")->asString() == "1: key D minor {Dm}:w\n2: {A7}:w\n");
    // A point past a shorter clip is refused as range, not dropped.
    CHECK(l.fails("clip.set", R"({"clip": "cl_1", "length": 4, "context": "2: {C}:w"})").code ==
          "clip.context_range");
}

TEST_CASE("clip.set context.limit: a clip holds at most 4096 chord points") {
    Launcher l;
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1"})");
    l.run("clip.set", R"({"clip": "cl_1", "length": 9000})");
    std::string text;
    for (int bar = 1; bar <= 2049; ++bar) text += std::to_string(bar) + ": {C}:h {G}:h\n";
    auto args = j(R"({"clip": "cl_1"})");
    args["context"] = text;  // 4098 chord points
    const auto result = l.bus->execute({"clip.set", args, kOrigin});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "context.limit");
    CHECK_FALSE(result.error().hint.empty());
}

TEST_CASE("clip.duplicate and clip.move keep the context and follow") {
    Launcher l;
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") + kTwoBars +
                          R"(, "context": "1: key D minor {Dm}:w\n2: {A7}:w", "follow": "key"})");
    l.run("clip.duplicate", R"({"clip": "cl_1", "scene": "sc_2"})");
    CHECK(clipOf(l, "cl_2").context == clipOf(l, "cl_1").context);
    CHECK(clipOf(l, "cl_2").follow == model::ClipFollow::Key);
    l.run("scene.add", R"({"name": "Bridge"})");
    l.run("clip.move", R"({"clip": "cl_2", "scene": "sc_3"})");
    CHECK(pointsOf(clipOf(l, "cl_2")) == "key@0 D minor, chord@0 Dm, chord@4 A7");
    CHECK(clipOf(l, "cl_2").follow == model::ClipFollow::Key);
    // An item made from the clip carries the clip's context on its take (and keeps no link: a
    // later clip edit does not reach it).
    const auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_1"})");
    CHECK(l.head().items.size() == 2);
    const model::Take& carried = *l.head().takes.find(id(laid.find("take")->asString().c_str()));
    CHECK(carried.context.size() == 3);
    CHECK(carried.follow == model::ClipFollow::Key);
    CHECK(carried.contextPeriod == std::optional<double>(clipOf(l, "cl_1").length));
}

TEST_CASE("midi.write on a clip records its chord symbols, replacing the chords when they differ") {
    Launcher l;
    l.run("clip.add", std::string(R"({"track": "tr_1", "scene": "sc_1", )") + kTwoBars +
                          R"(, "context": "1: key D minor {Dm}:w\n2: {A7}:w"})");
    const auto write = [&](const char* notation) {
        auto args = j(R"({"item": "cl_1"})");
        args["notation"] = notation;
        return l.runArgs("midi.write", args);
    };
    // Symbols that differ replace the chords, keep the key, and say so.
    const auto replaced = write("voice bass; 1: {Gm}:w; 2: {Dm}:w");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Gm, chord@4 Dm");
    CHECK(warningCodes(replaced).find("clip.chords_set") != std::string::npos);
    // The same symbols again: nothing replaced, nothing said.
    const auto same = write("voice bass; 1: {Gm}:w; 2: {Dm}:w");
    CHECK(warningCodes(same).empty());
    // A text without symbols leaves the context alone.
    write("voice bass; 1: D2:w; 2: A1:w");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Gm, chord@4 Dm");
    // A clip with no chords takes them too, and says so.
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key D minor ~:w"})");
    CHECK(warningCodes(write("voice bass; 1: {Am}:w; 2: {Dm}:w")).find("clip.chords_set") !=
          std::string::npos);
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Am, chord@4 Dm");
    // Symbols at or past the clip's length are not recorded, and the answer says so.
    l.run("clip.set", R"({"clip": "cl_1", "length": 4})");
    const auto beyond = write("voice bass; 1: {Em}:w; 2: {Dm}:w");
    CHECK(warningCodes(beyond).find("clip.chords_beyond_length") != std::string::npos);
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Em");
}

namespace {

// The pitches of an item's notes by start beat, "31@0 31@4".
std::string pitchesOf(Launcher& l, const std::string& item) {
    json::Value notes = l.run("midi.notes", R"({"item": ")" + item + R"("})");
    std::string out;
    for (const auto& n : notes["notes"].asArray()) {
        char at[32];
        std::snprintf(at, sizeof at, "%g", n.find("start")->asDouble());
        out += std::string(out.empty() ? "" : " ") + std::to_string(n.find("pitch")->asInt()) +
               "@" + at;
    }
    return out;
}

// What an item sounds as (ADR-0098: notes are stored as written and mapped when they sound):
// "31@0 36@4", each heard note's pitch at its item beat, in sounding order (start, then note).
std::string soundsOf(Launcher& l, const std::string& item) {
    const model::Session& s = l.head();
    const model::Id itemId = id(item.c_str());
    const double start = model::itemSpan(s, *s.items.find(itemId)).startBeat;
    auto heard = model::soundingNotes(s, itemId, -1e9, 1e9);
    std::stable_sort(heard.begin(), heard.end(),
                     [](const model::SoundingNote& a, const model::SoundingNote& b) {
                         if (a.songBeat != b.songBeat) return a.songBeat < b.songBeat;
                         return a.note < b.note;
                     });
    std::string out;
    for (const auto& n : heard) {
        char at[32];
        std::snprintf(at, sizeof at, "%g", n.songBeat - start);
        out += std::string(out.empty() ? "" : " ") + std::to_string(n.sounds.pitch) + "@" + at;
    }
    return out;
}

// "5-6 transpose D minor>C minor -2, 7-8 transpose D minor>F minor 3": a followed block's regions.
std::string regionsOf(const json::Value& followed) {
    std::string out;
    const json::Value* regions = followed.find("regions");
    if (!regions) return out;
    for (const auto& r : regions->asArray()) {
        auto text = [&](const char* key) {
            const json::Value* v = r.find(key);
            return v ? v->asString() : std::string("-");
        };
        out += std::string(out.empty() ? "" : ", ") + std::to_string(r.find("from_bar")->asInt()) +
               "-" + std::to_string(r.find("to_bar")->asInt()) + " " + text("by") + " " +
               text("from_key") + ">" + text("to_key");
        if (const json::Value* iv = r.find("interval")) out += " " + std::to_string(iv->asInt());
    }
    return out;
}

}  // namespace

TEST_CASE("laying a clip copies its notes and warns when its key is not the project's") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "name": "Break bass"})");
    // item.add {clip, follow: "none"}: the notes are copied as they are, with a warning that
    // names the clip, both keys and the first project bar where they differ.
    const auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    const auto codes = warningCodes(laid);
    CHECK(codes.find("context.mismatch") != std::string::npos);
    const std::string message = laid.find("warnings")->at(0).find("message")->asString();
    CHECK(message.find("cl_1 \"Break bass\"") != std::string::npos);
    CHECK(message.find("D minor") != std::string::npos);
    CHECK(message.find("C minor") != std::string::npos);
    CHECK(message.find("bar 5") != std::string::npos);
    CHECK(l.run("midi.notes", R"({"item": ")" + laid.find("item")->asString() + R"("})")["notes"]
              .at(0)
              .find("pitch")
              ->asInt() == 33);  // copied: A1
    CHECK(laid.find("followed")->find("mode")->asString() == "none");
    // The followed block says the same for a program: the two keys and the bar.
    const json::Value* mismatch = laid.find("followed")->find("mismatch");
    REQUIRE(mismatch);
    CHECK(json::writeCompact(*mismatch) ==
          R"({"kind":"key","clip_key":"D minor","key":"C minor","bar":5})");
    // The clip's own follow is the default: a clip that follows the key is mapped, with no
    // mismatch warning (the notes were moved), and follow: none overrides it.
    l.run("clip.set", R"({"clip": "cl_1", "follow": "key"})");
    const auto followed = l.run("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_1"})");
    CHECK(warningCodes(followed).find("context.mismatch") == std::string::npos);
    CHECK(followed.find("followed")->find("mode")->asString() == "key");
    // The notes stay as written (A1); they sound as C minor's G1, D minor's fifth.
    CHECK(pitchesOf(l, followed.find("item")->asString()) == "33@0");
    CHECK(soundsOf(l, followed.find("item")->asString()) == "31@0");
    CHECK(followed.find("followed")->find("mismatch") == nullptr);
    CHECK(warningCodes(
              l.run("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_1", "follow": "none"})"))
              .find("context.mismatch") != std::string::npos);
    // The same key, an enharmonic spelling of it, a clip with no key and a clip of drums or
    // nothing give none.
    l.run("clip.set", R"({"clip": "cl_1", "follow": "none", "context": "1: key C minor ~:w"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 13, "clip": "cl_1"})")).empty());
    l.run("clip.set", R"({"clip": "cl_1", "context": ""})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 17, "clip": "cl_1"})")).empty());
    l.run("key.set", R"({"at": 0, "tonic": "C#", "scale": "major"})");
    l.run("clip.set", R"({"clip": "cl_1", "context": "1: key Db major ~:w"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 21, "clip": "cl_1"})")).empty());
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "notation": "voice kick ch 10; 1: C2:w",
                          "context": "1: key D minor ~:w"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 25, "clip": "cl_2"})")).empty());
    l.run("scene.add", R"({"name": "Empty"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_3", "context": "1: key D minor ~:w"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 29, "clip": "cl_3"})")).empty());
}

TEST_CASE("the mismatch warning finds a key change inside the span, and comes from scene.insert") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "D", "scale": "minor"})");
    l.run("key.set", R"({"at": 40, "tonic": "C", "scale": "minor"})");  // bar 11
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w"})");
    l.run("clip.set", R"({"clip": "cl_1", "length": 16})");
    // Laid at bar 9 for four bars, the clip's D minor agrees with the project's for two bars
    // and parts from it at bar 11.
    const auto inserted = l.run("scene.insert", R"({"scene": "sc_1", "bar": 9, "bars": 4})");
    REQUIRE(warningCodes(inserted).find("context.mismatch") != std::string::npos);
    CHECK(inserted.find("warnings")->at(0).find("message")->asString().find("from bar 11") !=
          std::string::npos);
    CHECK(inserted.find("warnings")->size() == 1);  // once per item
    // arrangement.build gives it as well.
    l.run("history.undo", "{}");
    const auto built =
        l.run("arrangement.build", R"({"sequence": [{"scene": "sc_1", "bars": 4}], "bar": 9})");
    CHECK(warningCodes(built).find("context.mismatch") != std::string::npos);
    l.run("history.undo", "{}");
    // A clip that changes key itself at its bar 3 agrees with the project throughout.
    l.run("clip.set",
          R"({"clip": "cl_1", "context": "1: key D minor ~:w\n2: ~:w\n3: key C minor ~:w"})");
    const auto agrees = l.run("scene.insert", R"({"scene": "sc_1", "bar": 9, "bars": 4})");
    CHECK(warningCodes(agrees).empty());
}

// --- following at placement (ADR-0098 X2) ------------------------------------------------------

TEST_CASE("laying a clip as written warns when its chords are not the project's, key or no key") {
    Launcher l;
    // The project: D minor over Dm | Gm | A7 | Dm.
    l.run("context.write", R"({"text": "1: key D minor {Dm}:w\n2: {Gm}:w\n3: {A7}:w\n4: {Dm}:w"})");
    // A clip written over Dm | Dm | A7 | Dm, in the same key.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "name": "Verse bass",
                          "notation": "voice bass; 1: D2:w; 2: D2:w; 3: A1:w; 4: D2:w",
                          "context": "1: key D minor {Dm}:w\n2: ~:w\n3: {A7}:w\n4: {Dm}:w"})");
    const auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 1, "clip": "cl_1"})");
    REQUIRE(laid.find("warnings") != nullptr);
    const std::string message = laid.find("warnings")->at(0).find("message")->asString();
    CHECK(warningCodes(laid) == "context.mismatch ");
    CHECK(message.find("cl_1 \"Verse bass\" was written over Dm and sits under Gm from bar 2") !=
          std::string::npos);
    // `followed.mismatch` names what differs: here, in one key, the chords and the bar.
    REQUIRE(laid.find("followed") != nullptr);
    const json::Value* chordMismatch = laid.find("followed")->find("mismatch");
    REQUIRE(chordMismatch);
    CHECK(json::writeCompact(*chordMismatch) ==
          R"({"kind":"chord","clip_chord":"Dm","chord":"Gm","bar":2})");
    // One warning for the item, whatever the number of bars that differ; the same chords do not.
    l.run("item.remove", R"({"item": ")" + laid.find("item")->asString() + R"("})");
    l.run("context.write", R"({"text": "2: {Dm}:w"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 1, "clip": "cl_1"})")).empty());
    // A clip with chords and no key says it too; one with no chords does not.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "name": "Keyless",
                          "notation": "voice bass; 1: {Gm}:w; 2: {Gm}:w"})");
    CHECK(warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 9, "clip": "cl_2"})")) ==
          "context.mismatch ");
    l.run("scene.add", R"({"name": "Plain"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_3", "notation": "voice bass; 1: D2:w"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 13, "clip": "cl_3"})")).empty());
    // A clip that follows the chords is mapped, so there is nothing to warn of.
    l.run("context.write", R"({"text": "2: {Gm}:w"})");
    l.run("clip.set", R"({"clip": "cl_1", "follow": "chords"})");
    CHECK(
        warningCodes(l.run("item.add", R"({"track": "tr_1", "bar": 17, "clip": "cl_1"})")).empty());
}

TEST_CASE("item.add {clip} maps a clip to the key under it, by bar, and says how") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");  // bar 7
    l.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "context": "1: key D minor ~:w", "follow": "key",
              "notation": "voice bass; 1: A1:w; 2: A1:w; 3: A1:w; 4: A1:w"})");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    const json::Value& followed = *laid.find("followed");
    CHECK(followed.find("mode")->asString() == "key");
    CHECK(regionsOf(followed) ==
          "5-6 transpose D minor>C minor -2, 7-8 transpose D minor>F minor 3");
    CHECK(followed.find("unrolled") == nullptr);
    // D minor's fifth is C minor's G and F minor's C; the notes themselves are as written.
    CHECK(pitchesOf(l, laid["item"].asString()) == "33@0 33@4 33@8 33@12");
    CHECK(soundsOf(l, laid["item"].asString()) == "31@0 31@4 36@8 36@12");
    CHECK(warningCodes(laid).find("context.mismatch") == std::string::npos);
    // A clip's key that is the project's scale a tonic away is a transpose, with its interval.
    Launcher t;
    t.run("key.set", R"({"at": 0, "tonic": "E", "scale": "minor"})");
    t.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
              "context": "1: key D minor ~:w"})");
    auto up = t.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1", "follow": "key"})");
    CHECK(regionsOf(*up.find("followed")) == "5-5 transpose D minor>E minor 2");
    CHECK(soundsOf(t, up["item"].asString()) == "35@0");
    // Scales of the same size map by degree: D minor's third (F) is C major's (E).
    Launcher d;
    d.run("key.set", R"({"at": 0, "tonic": "C", "scale": "major"})");
    d.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: F2:w",
              "context": "1: key D minor ~:w"})");
    auto deg = d.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1", "follow": "key"})");
    CHECK(regionsOf(*deg.find("followed")) == "5-5 degree D minor>C major");
    CHECK(soundsOf(d, deg["item"].asString()) == "40@0");
}

TEST_CASE("following copies what cannot follow, and says why") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    auto laidAt = [&](int bar, const std::string& clip) {
        return l.run("item.add", R"({"track": "tr_1", "bar": )" + std::to_string(bar) +
                                     R"(, "clip": ")" + clip + R"(", "follow": "key"})");
    };
    auto copied = [](const json::Value& laid) {
        const json::Value* c = laid.find("followed")->find("copied");
        return c ? c->asString() : std::string();
    };
    // No key at the clip's start.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w"})");
    auto noKey = laidAt(5, "cl_1");
    CHECK(copied(noKey) == "no_key");
    CHECK(noKey.find("followed")->find("mode")->asString() == "none");
    CHECK(pitchesOf(l, noKey["item"].asString()) == "33@0");
    // Drums only (channel 10).
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_2", "notation": "voice kick ch 10; 1: C2:w",
                          "context": "1: key D minor ~:w"})");
    CHECK(copied(laidAt(9, "cl_2")) == "drums");
    // A MIDI clip with no notes.
    l.run("scene.add", R"({"name": "Bare"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_3", "context": "1: key D minor ~:w"})");
    CHECK(copied(laidAt(13, "cl_3")) == "no_notes");
    // Audio.
    l.run("scene.add", R"({"name": "Wav"})");
    auto args = j(R"({"track": "tr_1", "scene": "sc_4"})");
    args["audio_file"] = l.wav("a.wav").string();
    l.runArgs("clip.add", args);
    CHECK(copied(laidAt(17, "cl_4")) == "audio");
}

TEST_CASE("following with no key under the item copies the notes, with context.no_key") {
    Launcher l;  // the project has no key
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(pitchesOf(l, laid["item"].asString()) == "33@0");
    CHECK(regionsOf(*laid.find("followed")) == "5-5 copy D minor>-");
    CHECK(warningCodes(laid) == "context.no_key ");
}

TEST_CASE("following between scales of different sizes transposes, with context.scale_shape") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor_pentatonic"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(soundsOf(l, laid["item"].asString()) == "31@0");
    CHECK(regionsOf(*laid.find("followed")) == "5-5 transpose D minor>C minor pentatonic -2");
    CHECK(warningCodes(laid) == "context.scale_shape ");
}

TEST_CASE("following by chords maps each chord tone to its counterpart") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("chord.set", R"({"at": 0, "symbol": "Gm"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice pad; 1: {Dm}:w",
                          "context": "1: key D minor ~:w", "follow": "chords"})");
    CHECK(pointsOf(clipOf(l, "cl_1")) == "key@0 D minor, chord@0 Dm");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(laid.find("followed")->find("mode")->asString() == "chords");
    // Dm's D, F, A become Gm's G, Bb, D (as pitch classes, whatever the octave).
    std::set<int> classes;
    for (const auto& n :
         model::soundingNotes(l.head(), id(laid["item"].asString().c_str()), -1e9, 1e9))
        classes.insert(n.sounds.pitch % 12);
    CHECK(classes == std::set<int>{2, 7, 10});
    CHECK(regionsOf(*laid.find("followed")).find("chords") != std::string::npos);
}

TEST_CASE("a looped item stays looped and as written, whatever the passes sound as") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    // One key for the four bars: one looped item, four passes alike.
    auto steady = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const json::Value& laid = steady["items"].at(0);
    const std::string item = laid.find("item")->asString();
    CHECK(l.head().items.find(id(item.c_str()))->loop);
    CHECK(laid.find("followed")->find("unrolled") == nullptr);
    CHECK(pitchesOf(l, item) == "33@0");
    CHECK(soundsOf(l, item) == "31@0 31@4 31@8 31@12");
    const model::Take& take =
        *l.head().takes.find(*l.head().items.find(id(item.c_str()))->activeTake);
    CHECK(take.follow == model::ClipFollow::Key);
    CHECK(take.contextPeriod == std::optional<double>(4.0));  // the clip's length
    l.run("history.undo", "{}");
    // The key changes at bar 7: the passes sound differently, and the item is still one looped
    // item over the notes as written.
    l.run("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");
    auto moving = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const json::Value& out = moving["items"].at(0);
    const std::string again = out.find("item")->asString();
    CHECK(out.find("followed")->find("unrolled") == nullptr);
    CHECK(l.head().items.find(id(again.c_str()))->loop);
    CHECK(pitchesOf(l, again) == "33@0");
    CHECK(soundsOf(l, again) == "31@0 31@4 36@8 36@12");
    CHECK(regionsOf(*out.find("followed")) ==
          "5-6 transpose D minor>C minor -2, 7-8 transpose D minor>F minor 3");
    // Undo and redo take the item away and back as one step.
    l.run("history.undo", "{}");
    CHECK(l.head().items.size() == 1);
    l.run("history.redo", "{}");
    CHECK(soundsOf(l, again) == "31@0 31@4 36@8 36@12");
    // A context edit moves what it follows: nothing laid is stale (key change moved to bar 8).
    l.run("key.set", R"({"at": 28, "tonic": "F", "scale": "minor"})");
    l.run("key.remove", R"({"at": 24})");
    CHECK(soundsOf(l, again) == "31@0 31@4 31@8 36@12");
}

TEST_CASE("a pooled placement may follow: each take reads its own context over one source") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    const auto first =
        l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 1, "pooled": true})");
    CHECK(first.find("items")->at(0).find("followed")->find("mode")->asString() == "key");
    const std::string a = first.find("items")->at(0).find("item")->asString();
    CHECK(soundsOf(l, a) == "31@0");
    // A second placement under D minor shares the source and sounds as written there.
    l.run("key.set", R"({"at": 32, "tonic": "D", "scale": "minor"})");  // bar 9
    const auto second =
        l.run("scene.insert", R"({"scene": "sc_1", "bar": 9, "bars": 1, "pooled": true})");
    const std::string b = second.find("items")->at(0).find("item")->asString();
    CHECK(soundsOf(l, b) == "33@0");
    CHECK(soundsOf(l, a) == "31@0");  // the first is unmoved by the second
    const model::Session& s = l.head();
    CHECK(s.takes.find(*s.items.find(id(a.c_str()))->activeTake)->source ==
          s.takes.find(*s.items.find(id(b.c_str()))->activeTake)->source);
    CHECK(pitchesOf(l, a) == "33@0");  // written once, for both
}

TEST_CASE("arrangement.build follows too, and a replay maps against the branch it lands on") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    l.run("branch.create", R"({"name": "agent/test/follow"})");
    auto built =
        l.run("arrangement.build", R"({"sequence": [{"scene": "sc_1", "bars": 1}], "bar": 5})");
    const json::Value& item = built["segments"].at(0).find("items")->at(0);
    CHECK(item.find("followed")->find("mode")->asString() == "key");
    CHECK(soundsOf(l, item.find("item")->asString()) == "31@0");
    // Main's key moves under the same bar; merging replays the placement there.
    l.run("branch.switch", R"({"name": "main"})");
    l.run("key.set", R"({"at": 0, "tonic": "E", "scale": "minor"})");
    l.run("branch.merge", R"({"from": "agent/test/follow"})");
    const model::Session& s = l.head();
    std::string merged;
    for (const auto& e : s.items)
        if (e.value->position >= 16) merged = e.id.str();
    REQUIRE_FALSE(merged.empty());
    CHECK(soundsOf(l, merged) == "35@0");  // D minor's A1 under E minor
}

TEST_CASE("following folds a pitch pushed out of range, and counts it") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "D", "scale": "major"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice hi; 1: G9:w",
                          "context": "1: key C major ~:w", "follow": "key"})");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(pitchesOf(l, laid["item"].asString()) == "127@0");  // as written
    CHECK(soundsOf(l, laid["item"].asString()) == "117@0");   // G9 (127) + 2, folded an octave
    CHECK(laid.find("followed")->find("folded")->asInt() == 1);
}

TEST_CASE("notes on channel 10 are never mapped by a follow, in a looped item too") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");  // bar 7
    l.run("clip.add",
          R"({"track": "tr_1", "scene": "sc_1", "context": "1: key D minor ~:w", "follow": "key",
              "notation": "voice bass; 1: A1:w; voice kick ch 10; 1: C2:w"})");
    // One pass in one key: the bass moves, the kick stays.
    auto once = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(soundsOf(l, once["item"].asString()) == "31@0 36@0");
    l.run("history.undo", "{}");
    // Four passes over two keys: the kick sounds C2 four times.
    auto four = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const std::string item = four["items"].at(0).find("item")->asString();
    CHECK(soundsOf(l, item) ==
          "31@0 36@0 31@4 36@4 36@8 36@8 36@12 36@12");  // bass: G G C C; kick: C2 four times
}

TEST_CASE("every MIDI take of a laid item carries the clip's context") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    const model::Id first = l.head().clips.find(id("cl_1"))->takes[0];
    l.runArgs("take.add",
              json::parse(R"({"item": "cl_1", "from": ")" + first.str() + "\"}").value());
    auto four = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const std::string item = four["items"].at(0).find("item")->asString();
    const model::Item& laid = *l.head().items.find(id(item.c_str()));
    REQUIRE(laid.takes.size() == 2);
    CHECK(laid.loop);
    for (const model::Id t : laid.takes) {
        const model::Take& take = *l.head().takes.find(t);
        CHECK(take.follow == model::ClipFollow::Key);
        CHECK(take.context.size() == 1);
        CHECK(take.contextPeriod == std::optional<double>(4.0));
    }
    CHECK(soundsOf(l, item) == "31@0 31@4 36@8 36@12");
    // Activating the other take sounds the same: it follows too.
    l.run("take.activate", R"({"item": ")" + item + R"(", "index": 1})");
    CHECK(soundsOf(l, item) == "31@0 31@4 36@8 36@12");
}

TEST_CASE("notes that follow to one pitch sound as they are mapped, and are stored as written") {
    // harmony.md section 5 "Duplicates": Cmaj7's C E G B under C5 map to C C G G. The notes
    // are stored as written, so an edit loses none; take.bake and the engine's compile keep
    // one of identical notes.
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "major"})");
    l.run("chord.set", R"({"at": 0, "symbol": "C5"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice pad; 1: {Cmaj7}:w",
                          "context": "1: key C major ~:w", "follow": "chords"})");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    const std::string item = laid.find("item")->asString();
    json::Value before = l.run("midi.notes", R"({"item": ")" + item + R"("})");
    CHECK(before["notes"].size() == 4);
    std::multiset<int> sounding;
    for (const auto& n : model::soundingNotes(l.head(), id(item.c_str()), -1e9, 1e9))
        sounding.insert(n.sounds.pitch);
    CHECK(sounding.size() == 4);
    CHECK(std::set<int>(sounding.begin(), sounding.end()).size() == 2);
    l.run("midi.edit",
          R"({"item": ")" + item + R"(", "ops": [{"op": "move", "notes": [0], "semitones": 0}]})");
    json::Value after = l.run("midi.notes", R"({"item": ")" + item + R"("})");
    CHECK(after["notes"].size() == before["notes"].size());
}

TEST_CASE("a note held into the item's start is mapped from where it was written") {
    // The clip's second bar is in C major; its first, in D minor. The take starts at beat 6,
    // inside the second bar's whole note, so that note sounds from the item's start as a
    // fragment. It was written in C major, so under C major it does not move (mapped from
    // the clip's beat 0, D minor, its E would become D).
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "major"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "follow": "key",
                          "notation": "voice lead; 1: r:w; 2: E3:w",
                          "context": "1: key D minor ~:w\n2: key C major ~:w"})");
    const model::Id take = l.head().clips.find(id("cl_1"))->takes[0];
    l.runArgs("take.set",
              json::parse(R"({"take": ")" + take.str() + R"(", "start_offset": 6})").value());
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    // As written (A note at clip beat 4 begins two beats before the item); it sounds as C major's
    // E, by the place where it started, not as D minor's (which would give D, 50).
    CHECK(pitchesOf(l, laid.find("item")->asString()) == "52@-2");
    CHECK(soundsOf(l, laid.find("item")->asString()) == "52@-2");
}

TEST_CASE("a held-in fragment keeps the pitch of the place where it started") {
    // As above, looped over four bars from bar 5, with the project in C major at bar 5 and in
    // D major from bar 6: the fragment at the item's start began in bar 4 (C major) and sounds
    // as C major's E; every whole pass starts in D major (+2). One looped item.
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "major"})");
    l.run("key.set", R"({"at": 20, "tonic": "D", "scale": "major"})");  // bar 6
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "follow": "key",
                          "notation": "voice lead; 1: r:w; 2: E3:w",
                          "context": "1: key C major ~:w"})");
    const model::Id take = l.head().clips.find(id("cl_1"))->takes[0];
    l.runArgs("take.set",
              json::parse(R"({"take": ")" + take.str() + R"(", "start_offset": 6})").value());
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const json::Value& row = laid["items"].at(0);
    CHECK(row.find("followed")->find("unrolled") == nullptr);
    const std::string item = row.find("item")->asString();
    CHECK(l.head().items.find(id(item.c_str()))->loop);
    CHECK(soundsOf(l, item) == "52@-2 54@6 54@14");
}

TEST_CASE("followed.regions run across a bar where no note starts") {
    // session-model: a region is a maximal run of project bars over which the keys and the
    // way notes were mapped stay the same. A rest bar between two notes does not split it.
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "E", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "follow": "key",
                          "notation": "voice bass; 1: A1:w; 2: r:w; 3: A1:w",
                          "context": "1: key D minor ~:w"})");
    auto laid = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(regionsOf(*laid.find("followed")) == "5-7 transpose D minor>E minor 2");
    // A key change inside the rest bar still splits the run: the keys differ.
    l.run("history.undo", "{}");
    l.run("key.set", R"({"at": 20, "tonic": "F", "scale": "minor"})");  // bar 6
    auto split = l.run("item.add", R"({"track": "tr_1", "bar": 5, "clip": "cl_1"})");
    CHECK(regionsOf(*split.find("followed")) ==
          "5-5 transpose D minor>E minor 2, 7-7 transpose D minor>F minor 3");
}

// --- X4d: a take's own context and follow (ADR-0098, following is playback) ------------------

TEST_CASE("take.set gives a take a context and a follow: the notes stay as written") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    // Follow needs a key in the take's own context.
    CHECK(l.fails("take.set", R"({"take": "tk_1", "follow": "key"})").code == "take.no_key");
    CHECK(l.fails("take.set", R"({"take": "tk_1", "context": "1: key Q major ~:w"})").code ==
          "take.context_syntax");
    auto set = l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w",
                                      "follow": "key"})");
    CHECK(pitchesOf(l, "it_1") == "36@0 40@1 43@2 48@3");
    CHECK(soundsOf(l, "it_1") == "39@0 43@1 46@2 51@3");
    const model::Take& take = *l.head().takes.find(id("tk_1"));
    CHECK(take.follow == model::ClipFollow::Key);
    CHECK_FALSE(take.contextPeriod);
    CHECK(take.contextPhase == 0.0);
    // midi.get answers written notes by default, says it follows, and counts what differs.
    auto written = l.run("midi.get", R"({"item": "it_1"})");
    CHECK(written["follow"].asString() == "key");
    CHECK(written["sounding_differs"].asInt() == 4);
    CHECK(written["take_context"].asString() == "1: key C major ~:w\n");
    CHECK(written["text"].asString().find("C2:q E2:q G2:q C3:q") != std::string::npos);
    auto sounding = l.run("midi.get", R"({"item": "it_1", "sounding": true})");
    CHECK(sounding["text"].asString().find("Eb2:q G2:q Bb2:q Eb3:q") != std::string::npos);
    // from and to answer the heard notes of that range.
    auto range = l.run("midi.get", R"({"item": "it_1", "sounding": true, "from": 1, "to": 3})");
    CHECK(range["text"].asString().find("G2:q Bb2:q") != std::string::npos);
    CHECK(l.fails("midi.get", R"({"item": "it_1", "from": 1})").code == "midi.range_args");
    // Turning follow off returns the notes as written; the context stays.
    l.run("take.set", R"({"take": "tk_1", "follow": "none"})");
    CHECK(soundsOf(l, "it_1") == "36@0 40@1 43@2 48@3");
    CHECK_FALSE(l.head().takes.find(id("tk_1"))->context.empty());
    // Clearing the context of a following take sets its follow back, and says so.
    l.run("take.set", R"({"take": "tk_1", "follow": "key"})");
    auto cleared = l.run("take.set", R"({"take": "tk_1", "context": ""})");
    CHECK(warningCodes(cleared) == "take.follow_reset ");
    CHECK(l.head().takes.find(id("tk_1"))->follow == model::ClipFollow::None);
    CHECK(l.head().takes.find(id("tk_1"))->context.empty());
    CHECK_FALSE(l.head().takes.find(id("tk_1"))->contextPeriod);
    CHECK(l.head().takes.find(id("tk_1"))->contextPhase == 0.0);
    // A clip's take holds none of the four.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    const model::Id clipTake = l.head().clips.find(id("cl_1"))->takes[0];
    CHECK(l.fails("take.set",
                  R"({"take": ")" + clipTake.str() + R"(", "context": "1: key C major ~:w"})")
              .code == "take.in_clip");
    // session.describe names a following take's first key.
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    CHECK(l.run("session.describe", "{}")["text"].asString().find("follows key C major") !=
          std::string::npos);
}

TEST_CASE(
    "take.set context is read in bars of the meter at the item, and refuses past the period") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const std::string item = laid["items"].at(0).find("item")->asString();
    const model::Id take = *l.head().items.find(id(item.c_str()))->activeTake;
    REQUIRE(l.head().takes.find(take)->contextPeriod == std::optional<double>(4.0));
    // The period is four beats: a point in bar 2 is out of range, naming the bar.
    auto err =
        l.fails("take.set", R"({"take": ")" + take.str() +
                                R"(", "context": "1: key D minor ~:w\n2: key F minor ~:w"})");
    CHECK(err.code == "take.context_range");
    CHECK(err.message.find("bar 2") != std::string::npos);
    l.run("take.set", R"({"take": ")" + take.str() + R"(", "context": "1: key D minor {Dm}:w"})");
    CHECK(l.run("midi.get", R"({"item": ")" + item + R"("})")["take_context"].asString() ==
          "1: key D minor {Dm}:w\n");
}

TEST_CASE("take.bake writes what sounds, and one undo takes it back") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    CHECK(l.fails("take.bake", R"({"take": "tk_1"})").code == "take.not_following");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    const std::string before = soundsOf(l, "it_1");
    auto baked = l.run("take.bake", R"({"take": "tk_1"})");
    CHECK(baked["unrolled"].asBool() == false);
    CHECK(baked["followed"]["mode"].asString() == "key");
    CHECK(pitchesOf(l, "it_1") == "39@0 43@1 46@2 51@3");
    const model::Take& take = *l.head().takes.find(id("tk_1"));
    CHECK(take.follow == model::ClipFollow::None);
    CHECK_FALSE(take.context.empty());  // for display
    CHECK(soundsOf(l, "it_1") == before);
    CHECK(l.fails("take.bake", R"({"take": "tk_1"})").code == "take.not_following");
    l.run("history.undo", "{}");
    CHECK(pitchesOf(l, "it_1") == "36@0 40@1 43@2 48@3");
    CHECK(l.head().takes.find(id("tk_1"))->follow == model::ClipFollow::Key);
    CHECK(soundsOf(l, "it_1") == before);
    // A take that is not the item's active take is refused: what sounds is the active take's.
    auto added = l.run("take.add", R"({"item": "it_1", "from": "tk_1"})");
    const std::string second = added["id"].asString();
    CHECK(l.head().takes.find(id(second.c_str()))->follow == model::ClipFollow::Key);  // copied
    CHECK(l.fails("take.bake", R"({"take": ")" + second + R"("})").code == "take.not_active");
    // A clip's copy of the take holds none of the four.
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    const model::Clip& clip = *l.head().clips.find(id("cl_1"));
    CHECK(clip.follow == model::ClipFollow::Key);
    CHECK_FALSE(clip.context.empty());
    for (const model::Id t : clip.takes) {
        CHECK(l.head().takes.find(t)->context.empty());
        CHECK(l.head().takes.find(t)->follow == model::ClipFollow::None);
    }
    auto clipAdded =
        l.run("take.add", R"({"item": "cl_1", "from": ")" + clip.takes[0].str() + R"("})");
    CHECK(l.head().takes.find(id(clipAdded["id"].asString().c_str()))->follow ==
          model::ClipFollow::None);
    CHECK(l.fails("midi.get", R"({"item": "cl_1", "sounding": true})").code ==
          "midi.sounding_clip");
}

TEST_CASE("take.bake unrolls a looped item whose passes sound differently") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const std::string item = laid["items"].at(0).find("item")->asString();
    const model::Id take = *l.head().items.find(id(item.c_str()))->activeTake;
    const std::string before = soundsOf(l, item);
    CHECK(before == "31@0 31@4 36@8 36@12");
    auto baked = l.run("take.bake", R"({"take": ")" + take.str() + R"("})");
    CHECK(baked["unrolled"].asBool());
    CHECK_FALSE(l.head().items.find(id(item.c_str()))->loop);
    CHECK(pitchesOf(l, item) == "31@0 31@4 36@8 36@12");
    CHECK(soundsOf(l, item) == before);
    CHECK(l.head().takes.find(take)->follow == model::ClipFollow::None);
    // The take now holds the song's under the item: C minor from its start, F minor at beat 8
    // (song beat 24), nothing repeating; following again sounds as before.
    {
        const model::Take& t = *l.head().takes.find(take);
        REQUIRE(t.context.size() == 2);
        CHECK(t.context[0].tonic == "C");
        CHECK(t.context[1].tonic == "F");
        CHECK(t.context[1].beat == 8);
        CHECK_FALSE(t.contextPeriod);
    }
    l.run("take.set", R"({"take": ")" + take.str() + R"(", "follow": "key"})");
    CHECK(soundsOf(l, item) == before);
    // Undo the follow edit above, then the bake: one step puts the looped item and its written
    // note back.
    l.run("history.undo", "{}");
    l.run("history.undo", "{}");
    CHECK(l.head().items.find(id(item.c_str()))->loop);
    CHECK(pitchesOf(l, item) == "33@0");
    CHECK(soundsOf(l, item) == before);
    // The same key all through: the loop stays, and the note is written as it sounds.
    l.run("key.remove", R"({"at": 24})");
    auto steady = l.run("take.bake", R"({"take": ")" + take.str() + R"("})");
    CHECK_FALSE(steady["unrolled"].asBool());
    CHECK(l.head().items.find(id(item.c_str()))->loop);
    CHECK(pitchesOf(l, item) == "31@0");
}

TEST_CASE("take.bake unrolls when a note held into the item's start sounds other than its passes") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 15, "tonic": "F", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const std::string item = laid["items"].at(0).find("item")->asString();
    const model::Id take = *l.head().items.find(id(item.c_str()))->activeTake;
    // Two beats into the note: it is held in at the item's start, heard from beat 14 under C
    // minor, while every whole pass (from beat 18) is under F minor.
    l.run("take.set", R"({"take": ")" + take.str() + R"(", "start_offset": 2})");
    const std::string before = soundsOf(l, item);
    auto baked = l.run("take.bake", R"({"take": ")" + take.str() + R"("})");
    CHECK(baked["unrolled"].asBool());
    CHECK_FALSE(l.head().items.find(id(item.c_str()))->loop);
    // The held-in fragment keeps its pitch (C minor's, 31) and now starts at the item's start;
    // the passes keep F minor's 36. Written as one pitch per note it would sound 36 first.
    CHECK(before == "31@-2 36@2 36@6 36@10 36@14");
    CHECK(soundsOf(l, item) == "31@0 36@2 36@6 36@10 36@14");
    l.run("history.undo", "{}");
    CHECK(soundsOf(l, item) == before);
}

TEST_CASE("take.bake refuses a replay over other notes than it was recorded over") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    // The recorded pitches are positional: each names the written pitch and the place it was
    // heard at. Four notes sound here (36@0 40@1 43@2 48@3); these name other ones.
    const std::string wrongCount =
        R"({"take": "tk_1", "pitches": [{"was": 36, "at": 0, "pitch": 39}]})";
    CHECK(l.fails("take.bake", wrongCount).code == "take.bake_replay");
    const std::string wrongNotes =
        R"({"take": "tk_1", "pitches": [{"was": 37, "at": 0, "pitch": 40},
            {"was": 40, "at": 960, "pitch": 43}, {"was": 43, "at": 1920, "pitch": 46},
            {"was": 48, "at": 2880, "pitch": 51}]})";
    CHECK(l.fails("take.bake", wrongNotes).code == "take.bake_replay");
    CHECK(l.head().takes.find(id("tk_1"))->follow == model::ClipFollow::Key);
    // The record history keeps replays.
    const std::string right =
        R"({"take": "tk_1", "pitches": [{"was": 36, "at": 0, "pitch": 39},
            {"was": 40, "at": 960, "pitch": 43}, {"was": 43, "at": 1920, "pitch": 46},
            {"was": 48, "at": 2880, "pitch": 51}]})";
    l.run("take.bake", right);
    CHECK(pitchesOf(l, "it_1") == "39@0 43@1 46@2 51@3");
}

TEST_CASE("take.bake leaves the song's context under the item as the take's own") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    const std::string before = soundsOf(l, "it_1");
    CHECK(before == "39@0 43@1 46@2 51@3");
    l.run("take.bake", R"({"take": "tk_1"})");
    // What the notes are written in now: Eb major, from the item's start, not repeating.
    const model::Take& take = *l.head().takes.find(id("tk_1"));
    REQUIRE(take.context.size() == 1);
    CHECK(take.context[0].type == model::ContextType::Key);
    CHECK(take.context[0].tonic == "Eb");
    CHECK(take.context[0].beat == 0);
    CHECK_FALSE(take.contextPeriod);
    CHECK(take.contextPhase == 0);
    CHECK(take.follow == model::ClipFollow::None);
    // Following again sounds as before while the song is unchanged...
    l.run("take.set", R"({"take": "tk_1", "follow": "key"})");
    CHECK(soundsOf(l, "it_1") == before);
    // ...and moves with a later edit of the song, from the written notes.
    l.run("key.set", R"({"at": 0, "tonic": "F", "scale": "major"})");
    CHECK(pitchesOf(l, "it_1") == "39@0 43@1 46@2 51@3");
    CHECK(soundsOf(l, "it_1") == "41@0 45@1 48@2 53@3");
}

TEST_CASE("take.bake of a song with a key change and one with none") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 2, "tonic": "F", "scale": "minor"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key D minor ~:w", "follow": "key"})");
    const std::string before = soundsOf(l, "it_1");
    l.run("take.bake", R"({"take": "tk_1"})");
    {
        const model::Take& take = *l.head().takes.find(id("tk_1"));
        REQUIRE(take.context.size() == 2);
        CHECK(take.context[0].tonic == "C");
        CHECK(take.context[1].tonic == "F");
        CHECK(take.context[1].beat == 2);
    }
    l.run("take.set", R"({"take": "tk_1", "follow": "key"})");
    CHECK(soundsOf(l, "it_1") == before);
    // No key under the item: the context is dropped.
    l.run("take.set", R"({"take": "tk_1", "follow": "none"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key D minor ~:w", "follow": "key"})");
    l.run("key.remove", R"({"at": 0})");
    l.run("key.remove", R"({"at": 2})");
    l.run("take.bake", R"({"take": "tk_1"})");
    CHECK(l.head().takes.find(id("tk_1"))->context.empty());
    CHECK_FALSE(l.head().takes.find(id("tk_1"))->contextPeriod);
}

TEST_CASE("take.bake makes a pooled source the take's own copy") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w",
                          "context": "1: key C major ~:w", "follow": "key"})");
    auto one = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 1, "pooled": true})");
    auto two = l.run("scene.insert", R"({"scene": "sc_1", "bar": 9, "bars": 1, "pooled": true})");
    const std::string a = one["items"].at(0).find("item")->asString();
    const std::string b = two["items"].at(0).find("item")->asString();
    CHECK(l.head().takes.find(*l.head().items.find(id(a.c_str()))->activeTake)->source ==
          l.head().takes.find(*l.head().items.find(id(b.c_str()))->activeTake)->source);
    CHECK(soundsOf(l, a) == "39@0");
    l.run("take.bake",
          R"({"take": ")" + l.head().items.find(id(a.c_str()))->activeTake->str() + R"("})");
    CHECK(pitchesOf(l, a) == "39@0");  // written now
    CHECK(pitchesOf(l, b) == "36@0");  // the other item's notes are untouched
    CHECK(soundsOf(l, b) == "39@0");   // and still follow
}

TEST_CASE("take.bake of an unrolled pooled item leaves no unused source behind") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("key.set", R"({"at": 24, "tonic": "F", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto one = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4, "pooled": true})");
    auto two = l.run("scene.insert", R"({"scene": "sc_1", "bar": 9, "bars": 4, "pooled": true})");
    const auto takeOf = [&](const json::Value& laid) {
        return *l.head()
                    .items.find(id(laid.find("items")->at(0).find("item")->asString().c_str()))
                    ->activeTake;
    };
    const model::Id shared = *l.head().takes.find(takeOf(one))->source;
    REQUIRE(l.head().takes.find(takeOf(two))->source == shared);
    // The clip's take shares the source too; with the clip gone only the two items read it.
    l.run("clip.remove", R"({"clip": "cl_1"})");
    REQUIRE(l.head().midi.find(shared) != nullptr);
    auto baked = l.run("take.bake", R"({"take": ")" + takeOf(one).str() + R"("})");
    CHECK(baked.find("unrolled")->asBool());
    CHECK(l.head().midi.find(shared) != nullptr);  // the other item still reads it
    l.run("take.bake", R"({"take": ")" + takeOf(two).str() + R"("})");
    CHECK(l.head().midi.find(shared) == nullptr);  // nothing reads it now
}

TEST_CASE("midi.edit acts on written notes, reading a following take's own context") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    // C#2 is outside C major (the take's own key), and outside Eb major too; snap reads C major.
    l.run("midi.edit", R"({"item": "it_1", "ops": [{"op": "add", "pitch": 37, "start": 3.5,
                                                    "length": 0.25}]})");
    auto snapped = l.run("midi.edit", R"({"item": "it_1", "ops": [{"op": "snap_to_scale",
                                                                  "mode": "down"}]})");
    CHECK(snapped["acts_on"].asString() == "written");
    CHECK(pitchesOf(l, "it_1") == "36@0 40@1 43@2 48@3 36@3.5");
    // Written notes only: an edit that reads no pitch does not say it.
    auto moved = l.run("midi.edit", R"({"item": "it_1", "ops": [{"op": "velocity", "notes": [0],
                                                                "velocity": 90}]})");
    CHECK(moved.find("acts_on") == nullptr);
}

TEST_CASE("midi.write on a following take keeps its context and says how it follows") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    auto written = l.run("midi.write", R"({"item": "it_1", "notation": "voice bass; 1: C2:w"})");
    CHECK(written["follows"].asString() == "key");
    CHECK(soundsOf(l, "it_1") == "39@0");
    CHECK(pitchesOf(l, "it_1") == "36@0");
}

TEST_CASE("clip.add {item} and scene.capture take a following take's context and follow") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major {C}:w", "follow": "chords"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    const model::Clip& clip = *l.head().clips.find(id("cl_1"));
    CHECK(clip.follow == model::ClipFollow::Chords);
    REQUIRE_FALSE(clip.context.empty());
    CHECK(clip.context.front().beat == 0.0);
    // The take-to-clip conversion clears the four members on the clip's copy.
    CHECK(l.head().takes.find(clip.takes[0])->context.empty());
    // An item whose take does not follow gives the clip the project's context under it.
    Launcher p;
    p.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    p.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "item": "it_1"})");
    CHECK(p.head().clips.find(id("cl_1"))->follow == model::ClipFollow::None);
    CHECK(p.head().clips.find(id("cl_1"))->context.front().tonic == "Eb");
}

TEST_CASE("a context edit names the following items that lost their key") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    CHECK(soundsOf(l, "it_1") == "39@0 43@1 46@2 51@3");
    auto removed = l.run("key.remove", R"({"at": 0})");
    CHECK(warningCodes(removed) == "context.no_key ");
    CHECK(soundsOf(l, "it_1") == "36@0 40@1 43@2 48@3");  // as written
    CHECK(warningCodes(l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})")).empty());
}

TEST_CASE("item.split, trims and duplicate carry a take's context with each note's place") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    const std::string before = soundsOf(l, "it_1");
    auto split = l.run("item.split", R"({"item": "it_1", "at": 2})");
    (void)split;
    // Split keeps every note's song beat and pitch, and each part its take's context.
    std::string all;
    for (const auto& e : l.head().items) {
        const auto s = soundsOf(l, e.id.str());
        all += (all.empty() ? "" : " | ") + s;
        const model::Take& tk = *l.head().takes.find(*e.value->activeTake);
        CHECK(tk.follow == model::ClipFollow::Key);
        CHECK(tk.context.size() == 1);
    }
    CHECK(all.find("39@0 43@1") != std::string::npos);
    auto dup = l.run("item.duplicate", R"({"item": "it_1", "to": 16})");
    (void)dup;
    CHECK(l.head().items.size() == 3);
}

TEST_CASE("take.set context on a part cut from a take reads bar 1 at the item's start") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    l.run("item.split", R"({"item": "it_1", "at": 2})");
    // The right part's take begins two beats into its source: its phase is not 0 now.
    model::Id right;
    for (const auto& e : l.head().items)
        if (e.id != id("it_1")) right = e.id;
    REQUIRE(right.n != 0);
    const model::Id take = *l.head().items.find(right)->activeTake;
    REQUIRE(l.head().takes.find(take)->startOffset == 2.0);
    l.run("take.set", R"({"take": ")" + take.str() + R"(", "context": "1: key D minor ~:w"})");
    CHECK(l.head().takes.find(take)->contextPhase == -2.0);
    const double start = model::itemSpan(l.head(), *l.head().items.find(right)).startBeat;
    const auto place = model::placeAt(l.head(), right, start);
    REQUIRE(place.has_value());
    CHECK(place->contextBeat == doctest::Approx(0.0));
    // The text's key holds at the part's first beat: the same text on a fresh item says the same.
    CHECK(place->take.key.has_value());
}

TEST_CASE("take.set names every lead-sheet fault take.context_syntax (round 85 G13)") {
    Launcher l;
    for (const char* text : {"2: {C}:w\n1: {G}:w\n", "{C}:w\n", "1: {C}:w\n1: {G}:w\n"}) {
        json::Value args = j(R"({"take": "tk_1"})");
        args["context"] = text;
        const auto err = l.fails("take.set", json::writeCompact(args));
        CHECK_MESSAGE(err.code == "take.context_syntax", text, ": ", err.code);
    }
}

TEST_CASE("take.set {context: \"\"} clears the period and the phase with the points") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "C", "scale": "minor"})");
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: A1:w",
                          "context": "1: key D minor ~:w", "follow": "key"})");
    auto laid = l.run("scene.insert", R"({"scene": "sc_1", "bar": 5, "bars": 4})");
    const std::string item = laid.find("items")->at(0).find("item")->asString();
    const model::Id take = *l.head().items.find(id(item.c_str()))->activeTake;
    REQUIRE(l.head().takes.find(take)->contextPeriod.has_value());
    l.run("take.set", R"({"take": ")" + take.str() + R"(", "context": ""})");
    const model::Take& t = *l.head().takes.find(take);
    CHECK(t.context.empty());
    CHECK_FALSE(t.contextPeriod.has_value());
    CHECK(t.contextPhase == 0.0);
    CHECK(t.follow == model::ClipFollow::None);
}

TEST_CASE("take.bake replays on another branch with the pitches the user heard") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    l.run("branch.create", R"({"name": "agent/test/bake"})");
    l.run("take.bake", R"({"take": "tk_1"})");
    CHECK(pitchesOf(l, "it_1") == "39@0 43@1 46@2 51@3");
    // Main's key moves meanwhile; the merge replays the bake, writing what the user heard on
    // the branch (Eb), not what the key says there now (F).
    l.run("branch.switch", R"({"name": "main"})");
    l.run("key.set", R"({"at": 0, "tonic": "F", "scale": "major"})");
    CHECK(soundsOf(l, "it_1") == "41@0 45@1 48@2 53@3");
    l.run("branch.merge", R"({"from": "agent/test/bake"})");
    CHECK(pitchesOf(l, "it_1") == "39@0 43@1 46@2 51@3");
    CHECK(l.head().takes.find(id("tk_1"))->follow == model::ClipFollow::None);
}

TEST_CASE("midi.get {sounding} answers in item time, and counts what it answered (G9, G10)") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    // The middle two notes (E2 G2 sounding as G2 Bb2): the take starts a beat in, the item
    // lasts two.
    l.run("take.set", R"({"take": "tk_1", "start_offset": 1})");
    l.run("item.set", R"({"item": "it_1", "length": 2})");
    const auto whole = l.run("midi.get", R"({"item": "it_1", "sounding": true})");
    const std::string text = whole.find("text")->asString();
    // Item time: the first heard note is at the item's start, not a beat in.
    CHECK(text.find("G2:q Bb2:q") != std::string::npos);
    CHECK(text.find("r:q") == std::string::npos);
    CHECK(whole.find("sounding_differs")->asInt() == 2);
    // A range counts the notes of the range.
    const auto range =
        l.run("midi.get", R"({"item": "it_1", "sounding": true, "from": 0, "to": 1})");
    CHECK(range.find("sounding_differs")->asInt() == 1);
    // A take that does not follow answers in item time too.
    l.run("take.set", R"({"take": "tk_1", "follow": "none"})");
    const auto plain = l.run("midi.get", R"({"item": "it_1", "sounding": true})");
    CHECK(plain.find("text")->asString().find("E2:q G2:q") != std::string::npos);
    CHECK(plain.find("text")->asString().find("r:q") == std::string::npos);
}

TEST_CASE("midi.notes {sounding} adds each note's sounding pitch beside the written one") {
    Launcher l;
    l.run("key.set", R"({"at": 0, "tonic": "Eb", "scale": "major"})");
    l.run("take.set", R"({"take": "tk_1", "context": "1: key C major ~:w", "follow": "key"})");
    auto notes = l.run("midi.notes", R"({"item": "it_1", "sounding": true})");
    std::string written, sounds;
    for (const auto& n : notes["notes"].asArray()) {
        written += std::to_string(n.find("pitch")->asInt()) + " ";
        sounds += std::to_string(n.find("sounds")->asInt()) + " ";
    }
    CHECK(written == "36 40 43 48 ");
    CHECK(sounds == "39 43 46 51 ");
    CHECK(notes.find("differs_by_pass") == nullptr);
    CHECK(l.run("midi.notes", R"({"item": "it_1"})")["notes"].at(0).find("sounds") == nullptr);
    l.run("clip.add", R"({"track": "tr_1", "scene": "sc_1", "notation": "voice bass; 1: C2:w"})");
    CHECK(l.fails("midi.notes", R"({"item": "cl_1", "sounding": true})").code ==
          "midi.sounding_clip");
}
