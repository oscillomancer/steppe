// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's model commands over the bus (ADR-0097, step L1):
// scene.*, clip.*, item.add {clip}, the cascades and session.describe.
#include <doctest/doctest.h>
#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/engine/app.h>
#include <supple/engine/offline.h>
#include <supple/midi/midi.h>
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
        R"({"clip": "cl_2", "name": "Pad swell", "mode": "once", "quantize": "beat", "mute": true})");
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
              " pad.wav · quantize beat · muted\n"
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
