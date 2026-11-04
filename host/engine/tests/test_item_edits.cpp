// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/midi/midi.h"
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
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("item-edits-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    Fixture() {
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
        app->setupFixture(*bus, "song_with_audio");
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
