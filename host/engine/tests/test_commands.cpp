// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/product.h>

#include <unistd.h>
#include <cmath>
#include <filesystem>
#include <limits>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/base/paths.h>
#include <supple/base/sha256.h>
#include <supple/engine/app.h>
#include <supple/model/schemas.h>

#include "midi_ticks.h"
#include "render_commands.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
struct Workdir {
    fs::path path =
        fs::temp_directory_path() / ("supple-engine-commands-" + std::to_string(::getpid()));
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

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "commands", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }
json::Value run(cmd::Dispatcher& bus, std::string name, std::string_view args) {
    auto result = bus.execute({std::move(name), j(args), kOrigin});
    REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
    return *result;
}
void checkUndoContent(const model::Session& current, const std::string& before) {
    auto previous = model::parseProject(before);
    REQUIRE(previous);
    for (std::size_t k = 0; k < model::kKindCount; ++k)
        CHECK(current.next.next[k] >= previous->next.next[k]);
    auto content = current;
    content.next = previous->next;
    CHECK(model::serializeProject(content) == before);
}
double peak(const fs::path& path) {
    auto audio = audio::readAudio(path);
    REQUIRE(audio);
    double max = 0;
    for (float x : audio->channels[0]) max = std::max(max, std::fabs(double(x)));
    return max;
}
}  // namespace

TEST_CASE("project.set schema remaps render matrix references") {
    cmd::Registry registry;
    registerEngineCommands(registry);
    const auto* command = registry.find("project.set");
    REQUIRE(command);
    const auto args = j(R"({"render":{"pattern":"tr_1",
        "matrix":{"rg_1":["tr_1","tr_0"],"project":["tr_1"]}}})");
    const auto expected = j(R"({"render":{"pattern":"tr_1",
        "matrix":{"rg_2":["tr_2","tr_0"],"project":["tr_2"]}}})");
    const std::map<model::Id, model::Id> remap{
        {*model::Id::parse("tr_1"), *model::Id::parse("tr_2")},
        {*model::Id::parse("rg_1"), *model::Id::parse("rg_2")}};
    CHECK(model::remapIds(args, command->argsSchema, remap) == expected);
}

TEST_CASE("render patterns reject embedded NUL bytes before planning files") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    for (const char* command : {"render.mix", "render.stems", "render.dry-run"}) {
        if (std::string_view(command) == "render.stems") run(bus, "track.add", "{}");
        auto result =
            bus.execute({command, j(R"({"pattern":"name\u0000suffix","end_beat":1})"), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "render.path");
        CHECK(result.error().path == "/args/pattern");
        CHECK_FALSE(result.error().hint.empty());
        CHECK_FALSE(fs::exists(app->rendersDir() / "name"));
    }
    RenderPatternContext context;
    context.track = std::string("part\0hidden", 11);
    CHECK_FALSE(expandRenderPattern("{track}", context));
}

TEST_CASE("unknown MIDI items and copied FX suggest existing commands") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto item = bus.execute({"midi.notes", j(R"({"item":"it_999"})"), kOrigin});
    REQUIRE_FALSE(item);
    CHECK(item.error().hint.find("track.list") != std::string::npos);
    CHECK(item.error().hint.find("item.list") == std::string::npos);
    auto fx = bus.execute({"fx.copy", j(R"({"fx":"fx_999","track":"tr_0"})"), kOrigin});
    REQUIRE_FALSE(fx);
    CHECK(fx.error().hint.find("fx.add") != std::string::npos);
    CHECK(fx.error().hint.find("fx.list") == std::string::npos);
    CHECK(registry.find("track.list"));
    CHECK(registry.find("fx.add"));
}

TEST_CASE("invalid project, tempo, marker and normalization calls explain how to fix them") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    for (const auto& [command, args] : std::vector<std::pair<std::string, std::string>>{
             {"project.open", "{}"},
             {"tempo.set", R"({"points":[{"beat":1,"bpm":120}]})"},
             {"marker.add", "{}"},
             {"render.mix", R"({"end_beat":1,"normalize":{"mode":"peak","target":"bad"}})"}}) {
        auto result = bus.execute({command, j(args), kOrigin});
        REQUIRE_FALSE(result);
        INFO(command, result.error().toString());
        CHECK_FALSE(result.error().hint.empty());
    }
}

TEST_CASE("auxiliary channels over the bus: channels, routes to 3/4, key pins (ADR-0020)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Source"})");
    run(bus, "track.add", R"({"name":"Bus","type":"buss"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.keymix"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    run(bus, "track.set", R"({"track":"tr_1","parent_send":false})");
    // A stereo buss has no channel 3.
    auto narrow = bus.execute(
        {"route.add", j(R"({"from":"tr_1","to":"tr_2","channels":{"from":[1,2],"to":[3,4]}})"),
         kOrigin});
    REQUIRE_FALSE(narrow);
    CHECK(narrow.error().code == "route.channels");
    CHECK(narrow.error().path == "/args/channels/to/0");
    auto pins = bus.execute({"fx.set", j(R"({"fx":"fx_2","pins":{"key":[3,4]}})"), kOrigin});
    REQUIRE_FALSE(pins);
    CHECK(pins.error().code == "fx.pins");
    // Four channels: the route lands on 3/4, unheard until a pin keys from it.
    run(bus, "track.set", R"({"track":"tr_2","channels":4})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_2"))->channels == 4);
    run(bus, "route.add", R"({"from":"tr_1","to":"tr_2","channels":{"from":[1,2],"to":[3,4]}})");
    const double unkeyed =
        peak(run(bus, "render.mix", R"({"name":"unkeyed","end_beat":2})")["path"].asString());
    CHECK(unkeyed < 1e-6);
    run(bus, "fx.set", R"({"fx":"fx_2","pins":{"key":[3,4]}})");
    CHECK(app->head().fx.find(*model::Id::parse("fx_2"))->pins.key == std::vector<int>{3, 4});
    const double keyed =
        peak(run(bus, "render.mix", R"({"name":"keyed","end_beat":2})")["path"].asString());
    CHECK(keyed > 0.05);
    // {} restores the default pins.
    run(bus, "fx.set", R"({"fx":"fx_2","pins":{}})");
    CHECK(app->head().fx.find(*model::Id::parse("fx_2"))->pins.empty());
    // Strips stay within 1-128 channels.
    run(bus, "track.set", R"({"track":"tr_2","channels":9})");
    CHECK_FALSE(bus.execute({"track.set", j(R"({"track":"tr_2","channels":129})"), kOrigin}));
}

TEST_CASE("a multi-out instrument grows its track and pins its outputs (ADR-0021)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto track = [&](const char* id) { return *app->head().tracks.find(*model::Id::parse(id)); };
    auto fx = [&](const char* id) { return *app->head().fx.find(*model::Id::parse(id)); };
    run(bus, "track.add", R"({"name":"Drums"})");
    run(bus, "track.add", R"({"name":"Kit","channels":4})");
    run(bus, "track.add", R"({"name":"Kick","type":"buss"})");
    // Main, Kick and Snare: two stereo outputs past the main pair.
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.multiout"})");
    CHECK(track("tr_1").channels == 6);
    CHECK(fx("fx_1").pins.out == std::vector<int>{1, 2, 3, 4, 5, 6});
    // After a track's own auxiliary channels.
    run(bus, "fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.multiout"})");
    CHECK(track("tr_2").channels == 8);
    CHECK(fx("fx_2").pins.out == std::vector<int>{1, 2, 5, 6, 7, 8});
    // One undo step takes both back.
    run(bus, "history.undo", "{}");
    CHECK(track("tr_2").channels == 4);
    CHECK(track("tr_2").chain.empty());
    // 48 stereo outputs past the main pair: 98 channels.
    run(bus, "track.add", R"({"name":"Big"})");
    run(bus, "fx.add", R"({"track":"tr_4","plugin":"com.suppleaudio.test.multiout-x48"})");
    CHECK(track("tr_4").channels == 98);
    CHECK(fx("fx_3").pins.out.size() == 98);
    CHECK(fx("fx_3").pins.out.back() == 98);
    // The project saves and reopens with them.
    auto reopened = model::parseProject(model::serializeProject(app->head()));
    REQUIRE_MESSAGE(reopened, (reopened ? "" : reopened.error().message));
    CHECK(reopened->tracks.find(*model::Id::parse("tr_4"))->channels == 98);
    CHECK(reopened->fx.find(*model::Id::parse("fx_3"))->pins.out.size() == 98);
    // An effect's extra outputs (the key mix's) stay unconnected; the input
    // chain stays stereo.
    run(bus, "fx.add", R"({"track":"tr_3","plugin":"com.suppleaudio.test.keymix"})");
    CHECK(track("tr_3").channels == 2);
    run(bus, "fx.add",
        R"({"track":"tr_2","plugin":"com.suppleaudio.test.multiout","chain":"input"})");
    CHECK(track("tr_2").channels == 4);
    // The kick's pair, sent to the buss, is what the buss hears.
    run(bus, "route.add", R"({"from":"tr_1","to":"tr_3","channels":{"from":[3,4],"to":[1,2]}})");
    // fx.set: 0 leaves an auxiliary output unconnected, never a main one.
    run(bus, "fx.set", R"({"fx":"fx_1","pins":{"out":[1,2,0,0,5,6]}})");
    auto zero = bus.execute({"fx.set", j(R"({"fx":"fx_1","pins":{"out":[0,2]}})"), kOrigin});
    REQUIRE_FALSE(zero);
    CHECK(zero.error().code == "fx.pins");
    auto beyond =
        bus.execute({"fx.set", j(R"({"fx":"fx_1","pins":{"out":[1,2,3,4,7,8]}})"), kOrigin});
    REQUIRE_FALSE(beyond);
    CHECK(beyond.error().path == "/args/pins/out/4");
}

TEST_CASE("tempo and meter edits see previous steps in the same batch") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    const auto before = model::serializeProject(app->head());
    run(bus, "batch", R"({"steps":[
        {"command":"tempo.set","args":{"points":[{"beat":0,"bpm":90}]}},
        {"command":"tempo.set","args":{"points":[{"beat":0,"bpm":100}]}},
        {"command":"timesig.set","args":{"bar":9,"meter":"3/4"}},
        {"command":"timesig.set","args":{"bar":9,"meter":"5/4"}},
        {"command":"timesig.set","args":{"bar":17,"meter":"7/4"}},
        {"command":"timesig.remove","args":{"bar":17}}
    ]})");
    CHECK(app->head().tempo.size() == 1);
    CHECK(app->head().timeMap->bpmAt(0) == 100);
    CHECK(app->head().meters.size() == 2);
    CHECK(app->head().timeMap->meterAt(9).numerator == 5);
    run(bus, "history.undo", "{}");
    checkUndoContent(app->head(), before);
}

TEST_CASE("FX descriptions reset shared scratch instances") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"FX"})");
    run(bus, "fx.add",
        R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-6}}})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    auto defaults = run(bus, "fx.describe", R"({"fx":"fx_2"})")["description"];
    auto changed = run(bus, "fx.describe", R"({"fx":"fx_1"})")["description"];
    CHECK(changed != defaults);
    CHECK(run(bus, "fx.describe", R"({"fx":"fx_2"})")["description"] == defaults);
}

TEST_CASE("route sends through a gain bus, rejects cycles and replays in a branch merge") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Source"})");
    run(bus, "track.add", R"({"name":"Bus","type":"buss"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "fx.add",
        R"({"track":"tr_2","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-6}}})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const double direct =
        peak(run(bus, "render.mix", R"({"name":"direct","end_beat":2})")["path"].asString());
    run(bus, "route.add", R"({"from":"tr_1","to":"tr_2"})");
    auto listed = run(bus, "route.list", "{}");
    REQUIRE(listed["routes"].size() == 1);
    CHECK(listed["routes"].at(0).find("id")->asString() == "sd_1");
    const double sent =
        peak(run(bus, "render.mix", R"({"name":"sent","end_beat":2})")["path"].asString());
    CHECK(sent > direct * 1.2);
    auto cycle = bus.execute({"route.add", j(R"({"from":"tr_2","to":"tr_1"})"), kOrigin});
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "route.cycle");
    auto self = bus.execute({"route.add", j(R"({"from":"tr_1","to":"tr_1"})"), kOrigin});
    REQUIRE_FALSE(self);
    CHECK(self.error().code == "route.cycle");

    run(bus, "branch.create", R"({"name":"agent/test/route"})");
    run(bus, "route.set", R"({"route":"sd_1","volume_db":-3})");
    run(bus, "branch.switch", R"({"name":"main"})");
    run(bus, "marker.add", R"({"pos":1,"name":"Check"})");
    run(bus, "branch.merge", R"({"from":"agent/test/route"})");
    CHECK(app->head().routes.find(*model::Id::parse("sd_1"))->volumeDb == -3);
    CHECK(app->head().markers.size() == 1);
}

TEST_CASE("audio split is sample exact and take activation changes the render") {
    Workdir dir;
    audio::AudioData source;
    source.sampleRate = 48000;
    source.channels = {std::vector<float>(48000)};
    for (std::size_t i = 0; i < source.frames(); ++i)
        source.channels[0][i] =
            static_cast<float>(0.25 * std::sin(2 * 3.141592653589793 * 440 * double(i) / 48000));
    REQUIRE(audio::writeWav(dir.path / "source.wav", source));
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Audio"})");
    const std::string add = "{\"track\":\"tr_1\",\"audio_file\":" +
                            json::writeCompact(json::Value((dir.path / "source.wav").string())) +
                            "}";
    run(bus, "item.add", add);
    auto before = audio::readAudio(
        run(bus, "render.mix", R"({"name":"before","end_beat":2})")["path"].asString());
    REQUIRE(before);
    run(bus, "item.split", R"({"item":"it_1","at":1})");
    auto after = audio::readAudio(
        run(bus, "render.mix", R"({"name":"after","end_beat":2})")["path"].asString());
    REQUIRE(after);
    REQUIRE(before->frames() == after->frames());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < before->frames(); ++i)
        if (before->channels[0][i] != after->channels[0][i]) ++mismatches;
    CHECK(mismatches == 0);
    CHECK(app->head().items.size() == 2);
    CHECK(app->head().takes.size() == 2);
    run(bus, "take.add", R"({"item":"it_1","from":"tk_1"})");
    run(bus, "take.set", R"({"take":"tk_3","gain_db":-24})");
    const double loud =
        peak(run(bus, "render.mix", R"({"name":"loud","end_beat":1})")["path"].asString());
    run(bus, "take.activate", R"({"item":"it_1","take":"tk_3"})");
    const double quiet =
        peak(run(bus, "render.mix", R"({"name":"quiet","end_beat":1})")["path"].asString());
    CHECK(quiet < loud * 0.1);

    // take.set: the rate and pitch fields round-trip, and an unknown pitch
    // mode is rejected with the valid choices.
    run(bus, "take.set",
        R"({"take":"tk_1","playrate":2,"preserve_pitch":false,"pitch":-3,"pitch_mode":"resample"})");
    auto takes = run(bus, "take.list", R"({"item":"it_1"})");
    REQUIRE(takes["takes"].size() == 2);
    CHECK(takes["takes"].at(0).find("playrate")->asDouble() == 2.0);
    CHECK_FALSE(takes["takes"].at(0).find("preserve_pitch")->asBool());
    CHECK(takes["takes"].at(0).find("pitch")->asDouble() == -3.0);
    CHECK(takes["takes"].at(0).find("pitch_mode")->asString() == "resample");
    CHECK(takes["takes"].at(1).find("playrate")->asDouble() == 1.0);
    auto bad =
        bus.execute({"take.set", j(R"({"take":"tk_1","pitch_mode":"phase-vocoder"})"), kOrigin});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "command.invalid_args");
    REQUIRE(bad.error().details.size() == 1);
    CHECK(bad.error().details[0].hint.find("signalsmith") != std::string::npos);
}

TEST_CASE("FX order is audible and rulers survive save and reopen") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Tone"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "fx.add",
        R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-12}}})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const double low =
        peak(run(bus, "render.mix", R"({"name":"low","end_beat":2})")["path"].asString());
    run(bus, "fx.move", R"({"fx":"fx_2","index":0})");
    const double high =
        peak(run(bus, "render.mix", R"({"name":"high","end_beat":2})")["path"].asString());
    CHECK(high > low * 3.5);
    run(bus, "marker.add", R"({"pos":4,"name":"Verse"})");
    run(bus, "region.add", R"({"start":4,"end":8,"name":"Verse"})");
    const std::string save =
        "{\"path\":" + json::writeCompact(json::Value((dir.path / "project.json").string())) + "}";
    run(bus, "project.save", save);
    auto reopened = makeApp(dir.path / "reopened");
    REQUIRE(reopened->open(dir.path / "project.json"));
    CHECK(reopened->head().markers.find(*model::Id::parse("mk_1"))->name == "Verse");
    CHECK(reopened->head().regions.find(*model::Id::parse("rg_1"))->end == 8);

    run(bus, "branch.create", R"({"name":"agent/test/arrangement"})");
    run(bus, "item.set", R"({"item":"it_1","gain_db":-3})");
    run(bus, "region.set", R"({"id":"rg_1","end":10})");
    run(bus, "fx.move", R"({"fx":"fx_2","index":1})");
    run(bus, "branch.switch", R"({"name":"main"})");
    run(bus, "marker.set", R"({"id":"mk_1","name":"Start"})");
    run(bus, "branch.merge", R"({"from":"agent/test/arrangement"})");
    CHECK(app->head().items.find(*model::Id::parse("it_1"))->gainDb == -3);
    CHECK(app->head().regions.find(*model::Id::parse("rg_1"))->end == 10);
    CHECK(app->head().tracks.find(*model::Id::parse("tr_1"))->chain[1] ==
          *model::Id::parse("fx_2"));
    CHECK(app->head().markers.find(*model::Id::parse("mk_1"))->name == "Start");
}

TEST_CASE("MIDI split keeps held notes audible on both sides") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Tone"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:h r:h"})");
    run(bus, "item.split", R"({"item":"it_1","at":1})");
    const auto rendered = run(bus, "render.mix", R"({"name":"midi-split","end_beat":2})");
    auto audio = audio::readAudio(rendered.find("path")->asString());
    REQUIRE(audio);
    double left = 0, right = 0;
    for (std::size_t i = 10000; i < 20000; ++i)
        left = std::max(left, std::fabs(double(audio->channels[0][i])));
    for (std::size_t i = 30000; i < 40000; ++i)
        right = std::max(right, std::fabs(double(audio->channels[0][i])));
    CHECK(left > 0.05);
    CHECK(right > 0.05);
    const auto leftSource = app->head().takes.find(*model::Id::parse("tk_1"))->source;
    const auto rightSource = app->head().takes.find(*model::Id::parse("tk_2"))->source;
    REQUIRE(leftSource);
    REQUIRE(rightSource);
    CHECK(leftSource != rightSource);  // non-pooled MIDI stays independent
    const std::string leftNotation = run(bus, "midi.get", R"({"item":"it_1"})")["text"].asString();
    run(bus, "midi.write", R"({"item":"it_2","notation":"voice a; 1: r:w"})");
    CHECK(run(bus, "midi.get", R"({"item":"it_1"})")["text"].asString() == leftNotation);
    CHECK(app->head().takes.find(*model::Id::parse("tk_2"))->startOffset == 0);
}

TEST_CASE("a VST3 is listed with its format and describes its plain parameters") {
    Workdir dir;
    AppOptions options;
    options.workdir = dir.path;
    options.pluginPath = {FIXTURE_VST3_PATH};
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    json::Value listed = run(bus, "plugin.list", R"({"filter":"Sidechain"})");
    REQUIRE(listed["plugins"].asArray().size() == 1);
    json::Value plugin = listed["plugins"].asArray()[0];
    CHECK(plugin["format"].asString() == "vst3");
    run(bus, "track.add", R"({"name":"Fx"})");
    const std::string args = R"({"track":"tr_1","plugin":")" + plugin["id"].asString() + R"("})";
    json::Value added = run(bus, "fx.add", args);
    const std::string fx = added["fx"].asString();
    json::Value full = run(bus, "fx.describe", R"({"fx":")" + fx + R"(","full":true})");
    REQUIRE(full["params"].isArray());
    REQUIRE(full["params"].asArray().size() == 1);
    CHECK(full["params"].asArray()[0]["name"].asString() == "Gain");
    CHECK(full["params_total"].asDouble() == 1);
    json::Value none =
        run(bus, "fx.describe", R"({"fx":")" + fx + R"(","full":true,"filter":"cutoff"})");
    CHECK(none["params"].asArray().empty());
    CHECK(none["params_total"].asDouble() == 0);
    const auto param = std::to_string(full["params"].asArray()[0]["id"].asInt());
    auto doc = model::projectToJson(app->head());
    doc["fx"][fx]["params"][param] = 0.25;
    REQUIRE(writeFileAtomic(dir.path / "plain.json", json::writeCanonical(doc)));
    run(bus, "project.open", R"({"path":"plain.json"})");
    CHECK(run(bus, "fx.param.get",
              R"({"fx":")" + fx + R"(","param":")" + param + R"("})")["value"] == 0.25);
}

namespace {
// Runs a command that must fail and returns its error code ("" when it worked).
std::string fails(cmd::Dispatcher& bus, std::string name, std::string_view args) {
    auto result = bus.execute({std::move(name), j(args), kOrigin});
    return result ? std::string() : result.error().code;
}
std::vector<model::Id> childrenOf(const model::Session& s, std::string_view track) {
    const auto id = model::Id::parse(track);
    REQUIRE(id);
    const model::Track* t = s.tracks.find(*id);
    REQUIRE(t);
    return t->children;
}
}  // namespace

TEST_CASE("track.move re-parents, reorders, and refuses a cycle (review 14 D5)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A","type":"buss"})");  // tr_1
    run(bus, "track.add", R"({"name":"B","type":"buss"})");  // tr_2
    run(bus, "track.add", R"({"name":"C","index":0})");      // tr_3, first under the master
    const auto first = childrenOf(app->view(), "tr_0");
    REQUIRE(first.size() == 3);
    CHECK(first[0].str() == "tr_3");
    // A pure reorder: parent defaults to the current one.
    run(bus, "track.move", R"({"track":"tr_1","index":0})");
    const auto reordered = childrenOf(app->view(), "tr_0");
    REQUIRE(reordered.size() == 3);
    CHECK(reordered[0].str() == "tr_1");
    CHECK(reordered[1].str() == "tr_3");
    // Into a folder: the old parent loses it, the new one gains it.
    run(bus, "track.move", R"({"track":"tr_2","parent":"tr_1"})");
    CHECK(app->view().tracks.find(*model::Id::parse("tr_2"))->parent == *model::Id::parse("tr_1"));
    CHECK(childrenOf(app->view(), "tr_1")[0].str() == "tr_2");
    const auto master = childrenOf(app->view(), "tr_0");
    REQUIRE(master.size() == 2);
    // And the model stays consistent: the track is in exactly one parent.
    std::size_t listed = 0;
    for (const auto& e : app->view().tracks)
        for (model::Id child : e.value->children)
            if (child == *model::Id::parse("tr_2")) ++listed;
    CHECK(listed == 1);
    // A track cannot move under its own child, under itself, or move the master.
    CHECK(fails(bus, "track.move", R"({"track":"tr_1","parent":"tr_2"})") == "tr.cycle");
    CHECK(fails(bus, "track.move", R"({"track":"tr_1","parent":"tr_1"})") == "tr.cycle");
    CHECK(fails(bus, "track.move", R"({"track":"tr_0"})") == "track.strip_type");
    CHECK(fails(bus, "track.move", R"({"track":"tr_9"})") == "tr.unknown");
    // Sorting an index past the end appends, like track.add.
    run(bus, "track.move", R"({"track":"tr_2","parent":"tr_0","index":99})");
    const auto back = childrenOf(app->view(), "tr_0");
    CHECK(back.size() == 3);
    CHECK(back[2].str() == "tr_2");
}

TEST_CASE("item.set rejects a conflicting position and a zero length (review 14 D1)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    CHECK(fails(bus, "item.set", R"({"item":"it_1","position":1,"bar":2})") == "item.position");
    CHECK(fails(bus, "item.set", R"({"item":"it_1","length":0})") == "item.length");
    // A negative length is refused by the schema before the command runs.
    CHECK(fails(bus, "item.set", R"({"item":"it_1","length":-1})") == "command.invalid_args");
    run(bus, "item.set", R"({"item":"it_1","name":"Intro"})");  // it_1 has an active take
    run(bus, "item.set", R"({"item":"it_1","bar":2})");         // one of them is fine
    CHECK(app->view().items.find(*model::Id::parse("it_1"))->position > 0);
}

TEST_CASE("fx.move names a real command in its hint, and stops at the first chain (D2)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    // The "not in a track chain" rejection (whose hint used to name a command
    // that does not exist) needs an fx that only a take chain holds, and no
    // command can put one there yet: the fix is covered by fx.move on a track
    // chain (the first chain that holds it) staying correct.
    run(bus, "take.add", R"({"item":"it_1","from":"tk_1"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    CHECK(app->view().tracks.find(*model::Id::parse("tr_1"))->chain.size() == 2);
    run(bus, "fx.move", R"({"fx":"fx_1","index":1})");
    const auto& chain = app->view().tracks.find(*model::Id::parse("tr_1"))->chain;
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].str() == "fx_2");
    CHECK(chain[1].str() == "fx_1");
    CHECK(fails(bus, "fx.move", R"({"fx":"fx_1","index":9})") == "fx.index");
}

TEST_CASE("fx.add {chain:\"input\"} fills the input chain; fx.move/remove work on it") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    auto strip = [&] { return *app->view().tracks.find(*model::Id::parse("tr_1")); };
    // The input chain is separate from the track's chain.
    auto added = run(bus, "fx.add",
                     R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"input"})");
    CHECK(added["chain"].asArray().size() == 1);
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    CHECK(strip().inputChain.size() == 1);
    CHECK(strip().inputChain[0].str() == "fx_1");
    REQUIRE(strip().chain.size() == 1);
    CHECK(strip().chain[0].str() == "fx_2");
    // Reorder inside the input chain.
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"input"})");
    REQUIRE(strip().inputChain.size() == 2);
    CHECK(strip().inputChain[0].str() == "fx_1");
    CHECK(strip().inputChain[1].str() == "fx_3");
    run(bus, "fx.move", R"({"fx":"fx_3","index":0})");
    CHECK(strip().inputChain[0].str() == "fx_3");
    CHECK(strip().inputChain[1].str() == "fx_1");
    CHECK(fails(bus, "fx.move", R"({"fx":"fx_3","index":9})") == "fx.index");
    // An input-chain effect stays on its track: moving it elsewhere is refused.
    run(bus, "track.add", R"({"name":"B"})");
    CHECK(fails(bus, "fx.move", R"({"fx":"fx_3","track":"tr_2","index":0})") == "fx.chain");
    // Remove empties it from the input chain, leaving the track's chain alone.
    run(bus, "fx.remove", R"({"fx":"fx_3"})");
    CHECK(strip().inputChain.size() == 1);
    CHECK(strip().chain.size() == 1);
    // A bad chain name is rejected by the schema.
    CHECK_FALSE(
        fails(bus, "fx.add",
              R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"sidechain"})")
            .empty());
}

TEST_CASE("key routes over the bus: route.add {fx}, validation, listing, removal (ADR-0018)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Kick"})");
    run(bus, "track.add", R"({"name":"Bass"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","bar":1,"notation":"voice k; 1: A4:w"})");
    json::Value added =
        run(bus, "fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.keymix"})");
    const std::string keyMix = added["fx"].asString();
    added = run(bus, "fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.gain"})");
    const std::string gain = added["fx"].asString();
    json::Value described = run(bus, "fx.describe", R"({"fx":")" + keyMix + R"("})");
    CHECK(described["sidechain_channels"].asInt() == 2);
    auto noKey =
        bus.execute({"route.add", j(R"({"from":"tr_1","fx":")" + gain + R"("})"), kOrigin});
    REQUIRE_FALSE(noKey);
    CHECK(noKey.error().code == "route.no_sidechain");
    auto wrongTrack = bus.execute(
        {"route.add", j(R"({"from":"tr_1","to":"tr_1","fx":")" + keyMix + R"("})"), kOrigin});
    REQUIRE_FALSE(wrongTrack);
    CHECK(wrongTrack.error().code == "route.endpoint");
    json::Value made =
        run(bus, "route.add", R"({"from":"tr_1","fx":")" + keyMix + R"(","tap":"pre_fader"})");
    const std::string route = made["id"].asString();
    json::Value listed = run(bus, "route.list", "{}");
    REQUIRE(listed["routes"].asArray().size() == 1);
    json::Value first = listed["routes"].asArray()[0];
    CHECK(first["fx"].asString() == keyMix);
    CHECK(first["to"].asString() == "tr_2");
    // A key from Bass into a plug-in on Kick would close a loop.
    added = run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.keymix"})");
    const std::string kickKey = added["fx"].asString();
    auto cycle =
        bus.execute({"route.add", j(R"({"from":"tr_2","fx":")" + kickKey + R"("})"), kOrigin});
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "route.cycle");
    // Audible: the keyed track now carries the kick.
    json::Value keyed = run(bus, "render.mix", R"({"end_beat":2})");
    run(bus, "route.set", R"({"route":")" + route + R"(","mute":true})");
    json::Value unkeyed = run(bus, "render.mix", R"({"end_beat":2})");
    CHECK(peak(keyed["path"].asString()) > peak(unkeyed["path"].asString()) * 1.3);
    // Removing the keyed plug-in removes its key route.
    run(bus, "fx.remove", R"({"fx":")" + keyMix + R"("})");
    json::Value after = run(bus, "route.list", "{}");
    CHECK(after["routes"].asArray().empty());
}

TEST_CASE("typed strip commands: rejected edits roll back and masters render independently") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Alt","type":"master","parent":null})");
    run(bus, "track.add", R"({"name":"Group","type":"buss","parent":"tr_1"})");
    run(bus, "track.add", R"({"name":"Tone","parent":"tr_2"})");
    run(bus, "fx.add", R"({"track":"tr_3","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_3","notation":"voice a; 1: C4:w"})");
    const auto masters = app->head().project->masters;
    CHECK(masters == std::vector<model::Id>{model::kMaster, *model::Id::parse("tr_1")});
    auto rejected = [&](const char* command, const char* args, const char* code) {
        const auto version = app->head().version;
        auto result = bus.execute({command, j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().hint.empty());
        CHECK(app->head().version == version);
        CHECK(model::checkInvariants(app->head()));
    };
    rejected("track.add", R"({"parent":"tr_3"})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_3","type":"buss"})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_0","type":"buss"})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_2","armed":true})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_3","color":"pink"})", "track.color");
    rejected("track.set", R"({"track":"tr_3","color":"#12345g"})", "track.color");
    run(bus, "track.set", R"({"track":"tr_3","color":"teal"})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_3"))->color == "teal");
    run(bus, "track.set", R"({"track":"tr_3","color":"#a1b2c3"})");
    run(bus, "track.set", R"({"track":"tr_3","color":""})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_3"))->color.empty());
    rejected("item.move", R"({"item":"it_1","track":"tr_2"})", "track.strip_type");
    rejected("item.add", R"({"track":"tr_2"})", "track.strip_type");  // a buss
    // A master holds audio items only (ADR-0057).
    rejected("item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})", "track.strip_type");
    rejected("item.move", R"({"item":"it_1","track":"tr_1"})", "track.strip_type");
    rejected("route.add", R"({"from":"tr_0","to":"tr_2"})", "route.strip_type");
    rejected("route.add", R"({"from":"tr_0","to":"tr_1"})", "route.strip_type");
    rejected("route.add", R"({"from":"tr_2","to":"tr_3"})", "route.cycle");
    rejected("env.add", R"({"track":"tr_3","param":"trim"})", "track.strip_type");
    rejected("render.mix", R"({"master":"tr_2","end_beat":1})", "track.strip_type");
    auto list = run(bus, "track.list", "{}");
    CHECK(list["tracks"].at(0).find("type")->asString() == "master");
    CHECK(list["tracks"].at(2).find("type")->asString() == "buss");
    CHECK(describeSession(app->head()).find("[master]") != std::string::npos);
    CHECK(describeSession(app->head()).find("[buss]") != std::string::npos);
    const auto main = run(bus, "render.mix", R"({"name":"main","end_beat":1})");
    const auto alt = run(bus, "render.mix", R"({"master":"tr_1","name":"alt","end_beat":1})");
    CHECK(peak(main.find("path")->asString()) == 0);
    CHECK(peak(alt.find("path")->asString()) > 0.01);
    // Hardware outputs and several masters in one mix (ADR-0056).
    rejected("track.set", R"({"track":"tr_2","hw_output":[3,4]})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_1","hw_output":[3,3]})", "track.hw_output");
    run(bus, "track.set", R"({"track":"tr_1","hw_output":[3,4]})");
    list = run(bus, "track.list", "{}");
    CHECK(*list["tracks"].at(0).find("hw_output") == j("[1,2]"));
    CHECK(*list["tracks"].at(1).find("hw_output") == j("[3,4]"));
    CHECK(list["tracks"].at(2).find("hw_output") == nullptr);
    rejected("project.set", R"({"render":{"masters":["tr_2"]}})", "render.masters");
    auto both = run(bus, "render.mix", R"({"masters":["tr_0","tr_1"],"end_beat":1,
        "pattern":"both"})");
    REQUIRE(both["files"].size() == 2);
    CHECK(both["files"].at(1).find("path")->asString().ends_with("both-Alt.wav"));
    CHECK(peak(both["files"].at(1).find("path")->asString()) > 0.01);
    run(bus, "project.set",
        R"({"console9":false,"render":{"matrix":{"project":["tr_0","tr_1"]},"pattern":"master-{track}"}})");
    auto matrix = run(bus, "render.dry-run", R"({"operation":"regions","end_beat":1})");
    CHECK(matrix["files"].size() == 2);
    auto actual = run(bus, "render.regions", R"({"end_beat":1})");
    CHECK(actual["files"].size() == 2);
    run(bus, "route.add", R"({"from":"tr_3","to":"tr_0","pan":"follow"})");
    CHECK(run(bus, "route.list", "{}")["routes"].at(0).find("pan")->asString() == "follow");
    run(bus, "route.set", R"({"route":"sd_1","pan":-25})");
    CHECK(app->head().routes.find(*model::Id::parse("sd_1"))->pan.percent == -25);
    run(bus, "route.remove", R"({"route":"sd_1"})");
    run(bus, "track.add", R"({"name":"Empty"})");
    run(bus, "track.set", R"({"track":"tr_4","type":"master"})");
    CHECK(app->head().project->masters.back().str() == "tr_4");
    run(bus, "track.move", R"({"track":"tr_4","index":1})");
    CHECK(app->head().project->masters[1].str() == "tr_4");
    run(bus, "track.set", R"({"track":"tr_4","type":"buss"})");
    CHECK(app->head().project->masters == masters);
    run(bus, "track.set", R"({"track":"tr_4","type":"master"})");
    run(bus, "track.remove", R"({"track":"tr_4"})");
    CHECK(app->head().project->masters == masters);
}

TEST_CASE("project.open returns migration warnings in its load report") {
    Workdir dir;
    auto app = makeApp(dir.path);
    auto doc = model::projectToJson(app->head());
    doc["schema"] = "supple.project/1.0";
    doc["project"]["pan_law_db"] = -3;
    REQUIRE(writeFileAtomic(dir.path / "legacy.json", json::writeCanonical(doc)));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto loaded = run(bus, "project.open", R"({"path":"legacy.json"})");
    REQUIRE(loaded.find("warnings"));
    CHECK(loaded["warnings"].size() == 2);
    CHECK(loaded["warnings"].at(1).find("path")->asString() == "/project/console9");
}

namespace {
struct ChainFixture {
    fs::path dir = fs::temp_directory_path() / ("supple-chain-tests-" + std::to_string(::getpid()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    // scratch: the GUI's layout, an unsaved project with a folder of its own.
    explicit ChainFixture(bool scratch = false) {
        fs::remove_all(dir);
        AppOptions options;
        options.workdir = dir;
        if (scratch) options.scratchDir = dir / "scratch";
        options.chainDir = dir / "chains";
        options.presetDir = dir / "presets";
        options.templateDir = dir / "track-templates";
        options.projectTemplateDir = dir / "templates";
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto result = EngineApp::create(options, {.console9 = false});
        REQUIRE(result);
        app = std::move(*result);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~ChainFixture() {
        bus.reset();
        app.reset();
        fs::remove_all(dir);
    }
    json::Value run(const char* name, std::string_view args = "{}") {
        return ::run(*bus, name, args);
    }
    std::size_t history() { return app->versions().history(1000).size(); }
    json::Value document(const char* name = "Tone") {
        auto text = readTextFile(dir / "chains" / (std::string(name) + ".json"));
        REQUIRE(text);
        auto value = json::parse(*text);
        REQUIRE(value);
        return *value;
    }
    void write(json::Value doc, const char* name = "Tone") {
        REQUIRE(writeFileAtomic(dir / "chains" / (std::string(name) + ".json"),
                                json::writeCanonical(doc)));
    }
};
}  // namespace

TEST_CASE("FX chain presets round-trip ordered patches, blobs and options in one edit") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("track.add", R"({"name":"Target"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","patch":{"params":{"gain":-9}}})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.set",
          R"({"fx":"fx_1","label":"Quiet","bypass":true,"wet":37,"isolation":"in_process"})");
    f.run("fx.set", R"({"fx":"fx_2","offline":true,"wet":82})");
    const auto beforeSave = f.history();
    auto saved = f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    CHECK(saved["effects"].asInt() == 2);
    CHECK(f.history() == beforeSave);
    auto text = readTextFile(saved.find("path")->asString());
    REQUIRE(text);
    auto doc = f.document();
    CHECK(doc["schema"].asString() == "supple.fx-chain/1.0");
    REQUIRE(doc["effects"].size() == 2);
    CHECK(doc["effects"].at(1).find("state")->asString() == "AAAAAA==");
    // Oversampling is part of the preset even though fx.set has no such option.
    doc["effects"].asArray()[0]["oversampling"] = 4;
    f.write(doc);
    auto listed = f.run("fx.chain.list");
    REQUIRE(listed["chains"].size() == 1);
    CHECK(listed["chains"].at(0).find("effects")->asInt() == 2);
    const auto beforeLoad = f.history();
    auto loaded = f.run("fx.chain.load", R"({"track":"tr_2","name":"Tone"})");
    CHECK(f.history() == beforeLoad + 1);
    REQUIRE(loaded["effects"].size() == 2);
    const auto& target = f.app->head().tracks.find(*model::Id::parse("tr_2"))->chain;
    REQUIRE(target.size() == 2);
    const auto& first = *f.app->head().fx.find(target[0]);
    const auto& second = *f.app->head().fx.find(target[1]);
    CHECK(first.plugin.id == "com.suppleaudio.test.gain");
    CHECK(second.plugin.id == "com.suppleaudio.test.edge");
    CHECK(first.label == "Quiet");
    CHECK(first.bypass);
    CHECK(first.wet == 37);
    CHECK(first.isolation == model::Isolation::InProcess);
    CHECK(first.oversampling == 4);
    CHECK(second.offline);
    CHECK(second.wet == 82);
    REQUIRE(second.state);
    CHECK(*second.state ==
          "sha256:df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119");
    REQUIRE(f.app->head().blobs.contains(*second.state));
    CHECK(f.app->head().blobs.at(*second.state)->data == std::vector<std::uint8_t>(4, 0));
    const auto sourcePatch = f.run("fx.patch.get", R"({"fx":"fx_1"})");
    const auto targetPatch = f.run("fx.patch.get", "{\"fx\":\"" + target[0].str() + "\"}");
    CHECK(sourcePatch == targetPatch);
    f.run("history.undo");
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_2"))->chain.empty());
    f.run("history.redo");
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_2"))->chain.size() == 2);
    f.run("fx.chain.save", R"({"track":"tr_2","name":"Copy"})");
    auto copy = f.document("Copy");
    CHECK(copy["effects"] == doc["effects"]);
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone","replace":true})");
    CHECK(readTextFile(saved.find("path")->asString()).value() == *text);
}

TEST_CASE("a plugin's user presets save, load in one edit and delete; other plugins' stay apart") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","patch":{"params":{"gain":-9}}})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    const auto beforeSave = f.history();
    f.run("fx.preset.save", R"({"fx":"fx_1","name":"Quiet"})");
    f.run("fx.preset.save", R"({"fx":"fx_3","name":"Plain"})");
    CHECK(f.history() == beforeSave);
    CHECK(fails(*f.bus, "fx.preset.save", R"({"fx":"fx_1","name":"Quiet"})") == "fx.preset_exists");
    CHECK(fails(*f.bus, "fx.preset.save", R"({"fx":"fx_1","name":"a:b"})") == "fx.preset_name");
    // Every instance of the plugin sees its presets, and only its own.
    auto listed = f.run("fx.preset.list", R"({"fx":"fx_2"})");
    REQUIRE(listed["presets"].size() == 1);
    CHECK(listed["presets"].at(0).find("name")->asString() == "Quiet");
    CHECK(listed["presets"].at(0).find("source")->asString() == "user");
    auto plain = f.run("fx.preset.list", R"({"fx":"fx_3"})");
    REQUIRE(plain["presets"].size() == 1);
    CHECK(plain["presets"].at(0).find("name")->asString() == "Plain");

    f.run("fx.preset.load", R"({"fx":"fx_2","name":"Quiet"})");
    CHECK(f.history() == beforeSave + 1);
    CHECK(f.run("fx.patch.get", R"({"fx":"fx_2"})") == f.run("fx.patch.get", R"({"fx":"fx_1"})"));
    f.run("history.undo");
    CHECK(f.run("fx.param.get", R"({"fx":"fx_2","param":"gain"})")["value"].asDouble() == 0);
    f.run("fx.preset.load", R"({"fx":"fx_3","name":"Plain","source":"user"})");
    CHECK(f.app->head().fx.find(*model::Id::parse("fx_3"))->state ==
          "sha256:df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119");
    CHECK(fails(*f.bus, "fx.preset.load", R"({"fx":"fx_2","name":"Plain"})") ==
          "fx.unknown_preset");

    f.run("fx.preset.delete", R"({"fx":"fx_2","name":"Quiet"})");
    CHECK(f.run("fx.preset.list", R"({"fx":"fx_1"})")["presets"].size() == 0);
    CHECK(fails(*f.bus, "fx.preset.delete", R"({"fx":"fx_2","name":"Quiet"})") ==
          "fx.unknown_preset");
}

TEST_CASE("presets in the folder earlier versions used still list, load and go") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","patch":{"params":{"gain":-9}}})");
    const fs::path saved =
        f.run("fx.preset.save", R"({"fx":"fx_1","name":"Quiet"})")["path"].asString();
    CHECK(saved.parent_path().filename() == paths::idFolder("com.suppleaudio.test.gain"));
    const fs::path older =
        f.dir / "presets" / paths::legacyIdFolder("com.suppleaudio.test.gain") / "Quiet.json";
    const auto moveBack = [&] {
        fs::create_directories(older.parent_path());
        fs::rename(saved, older);
    };
    moveBack();
    auto listed = f.run("fx.preset.list", R"({"fx":"fx_1"})");
    REQUIRE(listed["presets"].size() == 1);
    CHECK(listed["presets"].at(0).find("path")->asString() == older.string());
    f.run("fx.preset.load", R"({"fx":"fx_1","name":"Quiet"})");
    CHECK(fails(*f.bus, "fx.preset.save", R"({"fx":"fx_1","name":"Quiet"})") == "fx.preset_exists");
    // Replaced, it moves to the plugin's own folder.
    f.run("fx.preset.save", R"({"fx":"fx_1","name":"Quiet","replace":true})");
    CHECK(fs::exists(saved));
    CHECK(!fs::exists(older));
    CHECK(f.run("fx.preset.list", R"({"fx":"fx_1"})")["presets"].size() == 1);
    // Deleted, no copy of it is left to come back.
    fs::copy_file(saved, older);
    f.run("fx.preset.delete", R"({"fx":"fx_1","name":"Quiet"})");
    CHECK(!fs::exists(saved));
    CHECK(!fs::exists(older));
}

TEST_CASE("fx.copy puts a copy of a plugin and its settings last in another track's chain") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("track.add", R"({"name":"Target"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","patch":{"params":{"gain":-9}}})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.add", R"({"track":"tr_2","plugin":"Test Gain"})");
    f.run("fx.set", R"({"fx":"fx_1","label":"Quiet","bypass":true,"wet":37})");
    const auto track = [&](const char* id) {
        return *f.app->head().tracks.find(*model::Id::parse(id));
    };
    const auto before = f.history();
    auto copied = f.run("fx.copy", R"({"fx":"fx_1","track":"tr_2"})");
    CHECK(f.history() == before + 1);  // one undo step
    REQUIRE(track("tr_2").chain.size() == 2);
    CHECK(track("tr_2").chain[0].str() == "fx_3");
    CHECK(track("tr_2").chain[1].str() == copied["fx"].asString());
    CHECK(copied["chain"].size() == 2);
    CHECK(track("tr_1").chain.size() == 2);  // the original stays
    const auto& source = *f.app->head().fx.find(*model::Id::parse("fx_1"));
    const auto& copy = *f.app->head().fx.find(track("tr_2").chain[1]);
    CHECK(copy.plugin.id == source.plugin.id);
    CHECK(copy.label == "Quiet");
    CHECK(copy.bypass);
    CHECK(copy.wet == 37);
    CHECK(copy.patch == source.patch);
    CHECK(f.run("fx.patch.get", "{\"fx\":\"" + copied["fx"].asString() + "\"}") ==
          f.run("fx.patch.get", R"({"fx":"fx_1"})"));
    // An index places it; the input chain takes one too.
    auto edge = f.run("fx.copy", R"({"fx":"fx_2","track":"tr_2","index":0})");
    CHECK(track("tr_2").chain[0].str() == edge["fx"].asString());
    CHECK(f.app->head().fx.find(track("tr_2").chain[0])->plugin.id == "com.suppleaudio.test.edge");
    f.run("fx.copy", R"({"fx":"fx_2","track":"tr_1","chain":"input"})");
    CHECK(track("tr_1").inputChain.size() == 1);
    // Onto its own track too.
    f.run("fx.copy", R"({"fx":"fx_1","track":"tr_1"})");
    CHECK(track("tr_1").chain.size() == 3);
    f.run("history.undo");
    CHECK(track("tr_1").chain.size() == 2);
    // Refusals leave the model alone.
    const auto version = f.app->head().version;
    auto fails = [&](std::string_view args) {
        auto r = f.bus->execute({"fx.copy", j(args), kOrigin});
        REQUIRE_FALSE(r);
        return r.error().code;
    };
    CHECK(fails(R"({"fx":"fx_99","track":"tr_2"})") == "fx.unknown");
    CHECK(fails(R"({"fx":"fx_1","track":"tr_9"})") == "track.unknown");
    CHECK(fails(R"({"fx":"fx_1","track":"tr_2","index":9})") == "fx.index");
    CHECK(f.app->head().version == version);
}

TEST_CASE("FX chain replace removes envelopes and routes and restores them on undo") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("track.add", R"({"name":"Target"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","chain":"input"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone","chain":"input"})");
    f.run("fx.add", R"({"track":"tr_2","plugin":"Test Key Mix"})");
    f.run("route.add", R"({"from":"tr_1","fx":"fx_2"})");
    f.run("env.add", R"({"route":"sd_1","param":"volume"})");
    const auto before = f.history();
    auto loaded = f.run("fx.chain.load", R"({"track":"tr_2","name":"Tone","mode":"replace"})");
    CHECK(f.history() == before + 1);
    const auto& track = *f.app->head().tracks.find(*model::Id::parse("tr_2"));
    CHECK(track.inputChain.empty());
    REQUIRE(track.chain.size() == 1);
    CHECK(track.chain[0].str() == loaded["effects"].at(0).asString());
    CHECK_FALSE(f.app->head().fx.find(*model::Id::parse("fx_2")));
    CHECK(f.app->head().routes.empty());
    CHECK(f.app->head().envelopes.empty());
    f.run("history.undo");
    CHECK(f.app->head().fx.find(*model::Id::parse("fx_2")));
    CHECK(f.app->head().routes.size() == 1);
    CHECK(f.app->head().envelopes.size() == 1);
    f.run("fx.add", R"({"track":"tr_2","plugin":"Test Gain","chain":"input"})");
    const auto path = json::writeCompact(json::Value((f.dir / "chains/Tone.json").string()));
    f.run("fx.chain.load", "{\"track\":\"tr_2\",\"path\":" + path + ",\"chain\":\"input\"}");
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_2"))->inputChain.size() == 2);
}

TEST_CASE("FX chain presets reject invalid names, overwrites and invalid loads atomically") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    auto empty = f.bus->execute({"fx.chain.save", j(R"({"track":"tr_1","name":"Tone"})"), kOrigin});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "chain.empty");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    auto original = f.document();
    for (const char* name : {"../Tone", "sub/Tone", "sub\\Tone", ".", ".."}) {
        auto args = json::Value::object();
        args["track"] = "tr_1";
        args["name"] = name;
        auto result = f.bus->execute({"fx.chain.save", args, kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "chain.name");
    }
    auto overwrite =
        f.bus->execute({"fx.chain.save", j(R"({"track":"tr_1","name":"Tone"})"), kOrigin});
    REQUIRE_FALSE(overwrite);
    CHECK(overwrite.error().code == "chain.exists");
    CHECK(f.document() == original);
    const auto before = f.history();
    auto bad = original;
    bad["effects"].asArray()[1]["plugin"]["id"] = "not-installed";
    bad["effects"].asArray()[1]["plugin"]["name"] = "Missing Space Echo";
    f.write(bad);
    auto missing = f.bus->execute(
        {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","mode":"replace"})"), kOrigin});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "chain.plugin_missing");
    CHECK(missing.error().message.find("Missing Space Echo") != std::string::npos);
    CHECK(f.history() == before);
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_1"))->chain.size() == 2);
    bad = original;
    bad["effects"].asArray()[1]["state"] = "not base64";
    f.write(bad);
    auto invalid = f.bus->execute(
        {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","mode":"replace"})"), kOrigin});
    REQUIRE_FALSE(invalid);
    CHECK(f.history() == before);
    CHECK(f.app->head().fx.size() == 2);
    auto ambiguous = f.bus->execute(
        {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","path":"Tone.json"})"), kOrigin});
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().code == "chain.source");
    REQUIRE(writeFileAtomic(f.dir / "chains/broken.json", "{}"));
    auto listed = f.run("fx.chain.list");
    CHECK(listed["chains"].size() == 1);
}

TEST_CASE("fx.chain.delete removes only the named preset file") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Keep"})");
    const auto before = f.history();
    auto deleted = f.run("fx.chain.delete", R"({"name":"Tone"})");
    CHECK(deleted["path"].asString() == (f.dir / "chains" / "Tone.json").string());
    CHECK_FALSE(fs::exists(f.dir / "chains" / "Tone.json"));
    auto listed = f.run("fx.chain.list");
    REQUIRE(listed["chains"].size() == 1);
    CHECK(listed["chains"].asArray()[0]["name"].asString() == "Keep");
    CHECK(f.history() == before);  // a file action, not an edit
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_1"))->chain.size() == 1);
    auto again = f.bus->execute({"fx.chain.delete", j(R"({"name":"Tone"})"), kOrigin});
    REQUIRE_FALSE(again);
    CHECK(again.error().code == "chain.unknown");
    auto escape = f.bus->execute({"fx.chain.delete", j(R"({"name":"../Keep"})"), kOrigin});
    REQUIRE_FALSE(escape);
    CHECK(escape.error().code == "chain.name");
    CHECK(fs::exists(f.dir / "chains" / "Keep.json"));
}

namespace {
// A second of a constant tone, as a WAV file.
void writeTone(const fs::path& file, float level) {
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000, level));
    REQUIRE(audio::writeWav(file, tone));
}
std::string mediaPath(EngineApp& app) { return app.head().media.begin()->value->path; }
}  // namespace

TEST_CASE("project templates save the whole project and start new unsaved projects from it") {
    ChainFixture f;
    f.run("project.set", R"({"name":"Band"})");
    f.run("track.add", R"({"name":"Drums"})");
    f.run("track.add", R"({"name":"Bass"})");
    f.run("fx.add", R"({"track":"tr_2","plugin":"Test Gain","patch":{"params":{"gain":-6}}})");
    auto saved = f.run("project.template.save", R"({"name":"Band setup"})");
    const fs::path folder = f.dir / "templates" / "Band setup";
    CHECK(saved["path"].asString() == folder.string());
    CHECK(fs::exists(folder / "project.json"));
    CHECK(f.app->projectPath().empty());  // the project keeps (no) file of its own
    auto exists = f.bus->execute({"project.template.save", j(R"({"name":"Band setup"})"), kOrigin});
    REQUIRE_FALSE(exists);
    CHECK(exists.error().code == "template.exists");
    f.run("project.template.save", R"({"name":"Band setup","replace":true})");
    CHECK_FALSE(fs::exists(f.dir / "templates" / ".staging"));
    auto listed = f.run("project.template.list");
    REQUIRE(listed["templates"].size() == 1);
    CHECK(listed["templates"].asArray()[0]["name"].asString() == "Band setup");
    auto dotted = f.bus->execute({"project.template.save", j(R"({"name":".hidden"})"), kOrigin});
    REQUIRE_FALSE(dotted);
    CHECK(dotted.error().code == "chain.name");

    REQUIRE(f.app->save(f.dir / "song.json"));
    f.run("track.add", R"({"name":"Extra"})");
    auto loaded = f.run("project.template.load", R"({"name":"Band setup"})");
    CHECK(loaded["name"].asString() == "Band");
    CHECK(f.app->projectPath().empty());
    CHECK(f.app->head().tracks.size() == 3);  // master, Drums, Bass
    CHECK(f.app->head().fx.size() == 1);
    auto missing = f.bus->execute({"project.template.load", j(R"({"name":"Nope"})"), kOrigin});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "template.unknown");

    // An older single-file template still lists, loads and deletes.
    REQUIRE(fs::copy_file(folder / "project.json", f.dir / "templates" / "Old.json"));
    CHECK(f.run("project.template.list")["templates"].size() == 2);
    CHECK(f.run("project.template.load", R"({"name":"Old"})")["name"].asString() == "Band");
    f.run("project.template.delete", R"({"name":"Old"})");
    CHECK_FALSE(fs::exists(f.dir / "templates" / "Old.json"));

    f.run("project.template.delete", R"({"name":"Band setup"})");
    CHECK_FALSE(fs::exists(folder));
    CHECK(f.run("project.template.list")["templates"].size() == 0);
    auto gone = f.bus->execute({"project.template.delete", j(R"({"name":"Band setup"})"), kOrigin});
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == "template.unknown");
}

TEST_CASE("project templates carry copies of their audio, and a new project gets its own") {
    for (bool scratch : {false, true}) {
        CAPTURE(scratch);
        ChainFixture f(scratch);
        f.app->setCopyImportedMedia(true);
        const fs::path original = f.dir / "loop.wav";
        writeTone(original, 0.25f);
        f.run("track.add", R"({"name":"Loop"})");
        json::Value add = json::Value::object();
        add["track"] = "tr_1";
        add["position"] = 0.0;
        add["audio_file"] = original.string();
        f.run("item.add", json::write(add).value());
        f.run("project.template.save", R"({"name":"Loops"})");
        const fs::path folder = f.dir / "templates" / "Loops";
        CHECK(fs::exists(folder / "media" / "loop.wav"));
        auto doc = json::parse(*readTextFile(folder / "project.json"));
        REQUIRE(doc);
        CHECK(json::write(*doc).value().find(f.dir.string()) == std::string::npos);  // no paths out

        fs::remove(original);  // the template no longer needs it
        f.run("project.template.load", R"({"name":"Loops"})");
        const fs::path media = scratch ? f.dir / "scratch" / "media" : f.dir / "media";
        CHECK(fs::exists(media / "loop.wav"));
        const fs::path named = mediaPath(*f.app);
        CHECK((named.is_absolute() ? named : media / named) == media / "loop.wav");
        // The new project's audio is its own: deleting the template keeps it.
        f.run("project.template.delete", R"({"name":"Loops"})");
        CHECK(fs::exists(media / "loop.wav"));
        if (scratch) {  // the first save moves it beside the project
            fs::create_directories(f.dir / "song");
            REQUIRE(f.app->save(f.dir / "song" / "song.json"));
            CHECK(fs::exists(f.dir / "song" / "media" / "loop.wav"));
        }
    }
}

TEST_CASE("a template refuses to save when an audio file it uses is gone") {
    ChainFixture f;
    const fs::path original = f.dir / "gone.wav";
    writeTone(original, 0.25f);
    f.run("track.add", R"({"name":"Loop"})");
    json::Value add = json::Value::object();
    add["track"] = "tr_1";
    add["position"] = 0.0;
    add["audio_file"] = original.string();
    f.run("item.add", json::write(add).value());  // copying off: the project names the original
    fs::remove(original);
    auto saved = f.bus->execute({"project.template.save", j(R"({"name":"Broken"})"), kOrigin});
    REQUIRE_FALSE(saved);
    CHECK(saved.error().code == "media.missing");
    CHECK(saved.error().message.find(original.string()) != std::string::npos);
    CHECK_FALSE(fs::exists(f.dir / "templates" / "Broken"));
    CHECK(f.run("project.template.list")["templates"].size() == 0);
}

TEST_CASE("imported audio is copied into the project's media folder when the setting is on") {
    ChainFixture f(true);
    f.app->setCopyImportedMedia(true);
    const fs::path original = f.dir / "in" / "kick.wav";
    fs::create_directories(original.parent_path());
    writeTone(original, 0.5f);
    f.run("track.add", R"({"name":"Kick"})");
    json::Value add = json::Value::object();
    add["track"] = "tr_1";
    add["position"] = 0.0;
    add["audio_file"] = original.string();
    auto first = f.run("item.add", json::write(add).value());
    const fs::path media = f.dir / "scratch" / "media";
    CHECK(mediaPath(*f.app) == "kick.wav");
    CHECK(fs::exists(media / "kick.wav"));
    // The same bytes again: the same source, no second copy.
    add["position"] = 8.0;
    CHECK(f.run("item.add", json::write(add).value())["source"] == first["source"]);
    CHECK_FALSE(fs::exists(media / "kick-2.wav"));
    // Other bytes under the same name: a copy of its own.
    const fs::path other = f.dir / "other" / "kick.wav";
    fs::create_directories(other.parent_path());
    writeTone(other, 0.125f);
    add["audio_file"] = other.string();
    f.run("item.add", json::write(add).value());
    CHECK(fs::exists(media / "kick-2.wav"));
    fs::remove_all(f.dir / "in");
    fs::remove_all(f.dir / "other");
    REQUIRE(f.app->save(f.dir / "song" / "song.json"));
    CHECK(fs::exists(f.dir / "song" / "media" / "kick.wav"));
    CHECK(fs::exists(f.dir / "song" / "media" / "kick-2.wav"));
    // Off: the project names the file where it is.
    f.app->setCopyImportedMedia(false);
    const fs::path outside = f.dir / "snare.wav";
    writeTone(outside, 0.75f);
    add["audio_file"] = outside.string();
    auto named = f.run("item.add", json::write(add).value());
    const auto* source = f.app->head().media.find(*model::Id::parse(named["source"].asString()));
    REQUIRE(source);
    CHECK(source->path == outside.string());
}

TEST_CASE("save as into another folder copies the project's media there") {
    ChainFixture f(true);
    f.app->setCopyImportedMedia(true);
    writeTone(f.dir / "pad.wav", 0.5f);
    f.run("track.add", R"({"name":"Pad"})");
    json::Value add = json::Value::object();
    add["track"] = "tr_1";
    add["position"] = 0.0;
    add["audio_file"] = (f.dir / "pad.wav").string();
    f.run("item.add", json::write(add).value());
    REQUIRE(f.app->save(f.dir / "a" / "a.json"));
    REQUIRE(f.app->save(f.dir / "b" / "b.json"));
    CHECK(fs::exists(f.dir / "a" / "media" / "pad.wav"));  // the first project keeps its own
    CHECK(fs::exists(f.dir / "b" / "media" / "pad.wav"));
    REQUIRE(f.app->save(f.dir / "b" / "b.json"));  // in place: nothing to copy
    // A folder whose media holds another file of that name is refused.
    fs::create_directories(f.dir / "c" / "media");
    writeTone(f.dir / "c" / "media" / "pad.wav", 0.125f);
    auto clash = f.app->save(f.dir / "c" / "c.json");
    REQUIRE_FALSE(clash);
    CHECK(clash.error().code == "media.conflict");
    CHECK_FALSE(fs::exists(f.dir / "c" / "c.json"));
    CHECK(f.app->projectPath() == f.dir / "b" / "b.json");
}

namespace {
using model::Id;
// tr_1 "Bass" with it_1: a MIDI item of four beats with takes tk_1..tk_4.
void fourTakes(ChainFixture& f) {
    f.run("track.add", R"({"name":"Bass"})");
    f.run("item.add", R"({"track":"tr_1","bar":1,"notation":"voice bass; 1: C2:q r:q C2:q r:q"})");
    for (int k = 0; k < 3; ++k) f.run("take.add", R"({"item":"it_1","from":"tk_1"})");
    for (int k = 1; k <= 4; ++k) {
        json::Value set = json::Value::object();
        set["take"] = "tk_" + std::to_string(k);
        set["name"] = "pass " + std::to_string(k);
        f.run("take.set", json::write(set).value());
    }
}
std::vector<std::string> takesOf(ChainFixture& f, const char* item) {
    std::vector<std::string> out;
    for (Id tk : f.app->head().items.find(*Id::parse(item))->takes) out.push_back(tk.str());
    return out;
}
}  // namespace

TEST_CASE("take.remove deletes one take; the heard one moves on; the last takes the item") {
    ChainFixture f;
    fourTakes(f);
    f.run("take.activate", R"({"item":"it_1","take":"tk_2"})");
    f.run("fx.add", R"({"take":"tk_2","plugin":"Test Gain"})");  // fx_1, a take FX
    f.run("env.add", R"({"fx":"fx_1","param":"gain","points":[{"beat":0,"value":-6}]})");
    const auto before = f.history();
    auto removed = f.run("take.remove", R"({"take":"tk_2"})");
    CHECK_FALSE(removed["item_removed"].asBool());
    CHECK(f.history() == before + 1);
    CHECK(takesOf(f, "it_1") == std::vector<std::string>{"tk_1", "tk_3", "tk_4"});
    CHECK(f.app->head().items.find(*Id::parse("it_1"))->activeTake->str() == "tk_3");
    CHECK_FALSE(f.app->head().takes.find(*Id::parse("tk_2")));
    CHECK(f.app->head().fx.size() == 0);         // its FX went with it
    CHECK(f.app->head().envelopes.size() == 0);  // and the FX's envelope
    f.run("take.remove", R"({"take":"tk_4"})");  // the last one heard: the one before
    f.run("take.activate", R"({"item":"it_1","take":"tk_3"})");
    f.run("take.remove", R"({"take":"tk_3"})");
    CHECK(f.app->head().items.find(*Id::parse("it_1"))->activeTake->str() == "tk_1");
    CHECK(f.run("take.remove", R"({"take":"tk_1"})")["item_removed"].asBool());
    CHECK(f.app->head().items.size() == 0);
    REQUIRE(f.run("history.undo").isObject());
    CHECK(f.app->head().items.size() == 1);
    f.run("item.set", R"({"item":"it_1","locked":true})");
    auto locked = f.bus->execute({"take.remove", j(R"({"take":"tk_1"})"), kOrigin});
    REQUIRE_FALSE(locked);
    CHECK(locked.error().code == "item.locked");
}

TEST_CASE("take.extract moves a take into an item of its own on any channel") {
    ChainFixture f;
    fourTakes(f);
    f.run("track.add", R"({"name":"Other"})");
    auto out = f.run("take.extract", R"({"take":"tk_3","track":"tr_2","position":8})");
    const auto made = *Id::parse(out["item"].asString());
    const model::Item& item = *f.app->head().items.find(made);
    CHECK(item.track.str() == "tr_2");
    CHECK(item.takes == std::vector<Id>{*Id::parse("tk_3")});
    CHECK(model::itemSpan(f.app->head(), item).startBeat == doctest::Approx(8));
    CHECK(model::itemEndBeat(f.app->head(), item) == doctest::Approx(12));  // the item's length
    CHECK(takesOf(f, "it_1") == std::vector<std::string>{"tk_1", "tk_2", "tk_4"});
    // An item's only take moves the item itself.
    auto alone = f.run("take.extract", R"({"take":"tk_3","track":"tr_1","position":16})");
    CHECK(alone["item"] == out["item"]);
    CHECK(f.app->head().items.find(made)->track.str() == "tr_1");
    f.run("track.add", R"({"name":"Bus","type":"buss"})");
    auto buss = f.bus->execute({"take.extract", j(R"({"take":"tk_2","track":"tr_3"})"), kOrigin});
    REQUIRE_FALSE(buss);
    CHECK(buss.error().code == "track.strip_type");
}

TEST_CASE("item.explode-takes spreads takes onto tracks or along the track, in order") {
    {
        ChainFixture f;
        fourTakes(f);
        f.run("track.add", R"({"name":"After"})");
        auto out = f.run("item.explode-takes", R"({"item":"it_1"})");
        REQUIRE(out["tracks"].size() == 3);
        const model::Session s = f.app->head();
        const auto& order = s.tracks.find(model::kMaster)->children;
        std::vector<std::string> names;
        for (Id t : order) names.push_back(s.tracks.find(t)->name);
        CHECK(names == std::vector<std::string>{"Bass", "pass 2", "pass 3", "pass 4", "After"});
        for (std::size_t k = 0; k < 4; ++k) {
            const model::Item& it = *s.items.find(*Id::parse(out["items"].asArray()[k].asString()));
            CHECK(it.takes.size() == 1);
            CHECK(it.takes[0].str() == "tk_" + std::to_string(k + 1));
            CHECK(model::itemSpan(s, it).startBeat == doctest::Approx(0));
        }
    }
    {
        ChainFixture f;
        fourTakes(f);
        auto out = f.run("item.explode-takes", R"({"item":"it_1","mode":"serial"})");
        CHECK(out["tracks"].size() == 0);
        const model::Session s = f.app->head();
        for (std::size_t k = 0; k < 4; ++k) {
            const model::Item& it = *s.items.find(*Id::parse(out["items"].asArray()[k].asString()));
            CHECK(it.track.str() == "tr_1");
            CHECK(it.takes[0].str() == "tk_" + std::to_string(k + 1));
            CHECK(model::itemSpan(s, it).startBeat ==
                  doctest::Approx(4.0 * static_cast<double>(k)));
        }
        auto single = f.bus->execute({"item.explode-takes", j(R"({"item":"it_1"})"), kOrigin});
        REQUIRE_FALSE(single);
        CHECK(single.error().code == "take.single");
    }
}

TEST_CASE("item.apply-fx renders an item through its FX into a new take or in its place") {
    ChainFixture f(true);
    f.app->setCopyImportedMedia(true);
    writeTone(f.dir / "tone.wav", 0.5f);
    f.run("track.add", R"({"name":"Gtr"})");
    json::Value add = json::Value::object();
    add["track"] = "tr_1";
    add["position"] = 0.0;
    add["length"] = 2.0;
    add["audio_file"] = (f.dir / "tone.wav").string();
    f.run("item.add", json::write(add).value());
    add["position"] = 2.0;  // a neighbour on the track, not in the render
    f.run("item.add", json::write(add).value());
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","patch":{"params":{"gain":-6}}})");
    f.run("item.set", R"({"item":"it_1","gain_db":-12})");  // stays for playback
    auto out = f.run("item.apply-fx", R"({"item":"it_1"})");
    const fs::path file = out["path"].asString();
    CHECK(file.parent_path() == f.dir / "scratch" / "media");
    auto audio = audio::readAudio(file);
    REQUIRE(audio);
    CHECK(audio->frames() == 48000);  // two beats at 120 bpm
    float peak = 0;
    for (const auto& ch : audio->channels)
        for (float v : ch) peak = std::max(peak, std::fabs(v));
    CHECK(peak == doctest::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).epsilon(0.01));
    CHECK(takesOf(f, "it_1").size() == 2);
    const model::Item& it = *f.app->head().items.find(*Id::parse("it_1"));
    CHECK(it.activeTake->str() == out["take"].asString());
    CHECK(it.gainDb == -12);
    // replace: the new take takes the heard one's place.
    auto again = f.run("item.apply-fx", R"({"item":"it_2","mode":"replace"})");
    CHECK(takesOf(f, "it_2") == std::vector<std::string>{again["take"].asString()});
    REQUIRE(f.app->save(f.dir / "song" / "song.json"));  // the render moves with the media
    CHECK(fs::exists(f.dir / "song" / "media" / file.filename()));
}

TEST_CASE("FX chain opaque states have content hashes across SHA-256 block boundaries") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    auto doc = f.document();
    // Standard SHA-256 vectors for bytes 0..n-1, independently checked with hashlib.
    for (const auto& [size, hash] : std::vector<std::pair<std::size_t, std::string>>{
             {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
             {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
             {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
             {120, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"}}) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(i);
        doc["effects"].asArray()[0]["state"] = base64Encode(bytes);
        f.write(doc);
        auto loaded = f.run("fx.chain.load", R"({"track":"tr_1","name":"Tone","mode":"replace"})");
        auto id = model::Id::parse(loaded["effects"].at(0).asString());
        REQUIRE(id);
        const auto& fx = *f.app->head().fx.find(*id);
        REQUIRE(fx.state);
        CHECK(*fx.state == "sha256:" + hash);
        CHECK(f.app->head().blobs.at(*fx.state)->data == bytes);
    }
}

TEST_CASE("item.add hashes the file, reads its header, and adds one source per file") {
    Workdir dir;
    audio::AudioData source;
    source.sampleRate = 48000;
    source.channels.assign(2, std::vector<float>(48000));  // 1 s stereo
    for (std::size_t i = 0; i < source.frames(); ++i)
        source.channels[0][i] = source.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000));
    const fs::path file = dir.path / "source.wav";
    REQUIRE(audio::writeWav(file, source));
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Audio"})");
    const std::string add =
        "{\"track\":\"tr_1\",\"audio_file\":" + json::writeCompact(json::Value(file.string())) +
        "}";

    audio::resetDecodeCountForTests();
    const auto first = run(bus, "item.add", add);
    CHECK(first.find("source")->asString() == "md_1");
    for (int i = 0; i < 100; ++i) run(bus, "item.add", add);
    // One source for one file, and no decode to learn its metadata.
    CHECK(app->head().media.size() == 1);
    CHECK(app->head().items.size() == 101);
    CHECK(audio::decodeCountForTests() == 0);
    const auto* md = app->head().media.find(model::Id{model::Kind::Media, 1});
    REQUIRE(md != nullptr);
    CHECK(md->hash == sha256File(file).value());
    CHECK(md->sampleRate == 48000);
    CHECK(md->channels == 2);
    CHECK(md->frames == 48000);
    CHECK(md->format == "wav");
    // The item's length came from the header (1 s at 120 bpm = 2 beats), and
    // every add used the same source.
    const auto* item = app->head().items.find(model::Id{model::Kind::Item, 1});
    REQUIRE(item != nullptr);
    CHECK(item->length == doctest::Approx(2.0));

    // The hash survives a save/open round trip: the schema accepts the hashed
    // form (and the empty form old projects carry — see the fuzz corpus).
    const std::string save =
        "{\"path\":" + json::writeCompact(json::Value((dir.path / "project.json").string())) + "}";
    run(bus, "project.save", save);
    auto reopened = makeApp(dir.path / "reopened");
    REQUIRE(reopened->open(dir.path / "project.json"));
    const auto* again = reopened->head().media.find(model::Id{model::Kind::Media, 1});
    REQUIRE(again != nullptr);
    CHECK(again->hash == md->hash);
}

TEST_CASE("item.add reuses a pre-hash source for the same file and backfills its hash") {
    Workdir dir;
    audio::AudioData source;
    source.sampleRate = 48000;
    source.channels.assign(2, std::vector<float>(48000));  // 1 s stereo
    for (std::size_t i = 0; i < source.frames(); ++i)
        source.channels[0][i] = source.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000));
    const fs::path file = dir.path / "source.wav";
    REQUIRE(audio::writeWav(file, source));
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Audio"})");
    const std::string add =
        "{\"track\":\"tr_1\",\"audio_file\":" + json::writeCompact(json::Value(file.string())) +
        "}";
    run(bus, "item.add", add);

    // A project saved before MediaSource::hash existed: the source keeps the
    // file's path and header, and has no hash.
    auto doc = model::projectToJson(app->head());
    doc["schema"] = "supple.project/1.0";
    doc["sources"]["media"]["md_1"]["hash"] = "";
    REQUIRE(writeFileAtomic(dir.path / "legacy.json", json::writeCanonical(doc)));
    REQUIRE(app->open(dir.path / "legacy.json"));
    REQUIRE(app->head().media.size() == 1);
    const auto media = [&] { return app->head().media.find(model::Id{model::Kind::Media, 1}); };
    REQUIRE(media() != nullptr);
    CHECK(media()->hash.empty());

    // Adding the same file again converges on that source and fills its hash,
    // instead of growing a second source for one file.
    const auto again = run(bus, "item.add", add);
    CHECK(again.find("source")->asString() == "md_1");
    CHECK(app->head().media.size() == 1);
    CHECK(app->head().items.size() == 2);  // the item is still added
    CHECK(media()->hash == sha256File(file).value());

    // A different file of the same length is not conflated with it: the path
    // has to be the same file, not just the same shape.
    audio::AudioData other = source;
    for (float& v : other.channels[0]) v *= 0.5f;
    const fs::path otherFile = dir.path / "other.wav";
    REQUIRE(audio::writeWav(otherFile, other));
    const auto third = run(bus, "item.add",
                           "{\"track\":\"tr_1\",\"audio_file\":" +
                               json::writeCompact(json::Value(otherFile.string())) + "}");
    CHECK(third.find("source")->asString() == "md_2");
    CHECK(app->head().media.size() == 2);
}

TEST_CASE("timebase: items keep their place when it changes, and follow it when the tempo does") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");  // tr_1
    run(bus, "track.add", R"({"name":"B"})");  // tr_2
    // At 120 BPM: beat 4 = 2 s, two beats long = 1 s.
    run(bus, "item.add", R"({"track":"tr_1","position":4,"length":2,"notation":"1: C4:h r:h"})");
    run(bus, "item.add", R"({"track":"tr_1","position":8,"length":2,"notation":"1: C4:h r:h"})");
    const model::Id a = *model::Id::parse("it_1"), b = *model::Id::parse("it_2");
    auto span = [&](model::Id id) {
        return model::itemSpan(app->head(), *app->head().items.find(id));
    };
    // it_1 follows time from now on: the model holds seconds, nothing moves.
    run(bus, "item.set", R"({"item":"it_1","timebase":"time"})");
    CHECK(app->head().items.find(a)->position == doctest::Approx(2.0));
    CHECK(app->head().items.find(a)->length == doctest::Approx(1.0));
    CHECK(span(a).startBeat == doctest::Approx(4.0));
    // Positions given in beats land in its domain.
    run(bus, "item.set", R"({"item":"it_1","position":6})");
    CHECK(app->head().items.find(a)->position == doctest::Approx(3.0));
    CHECK(app->head().items.find(a)->length == doctest::Approx(1.0));  // seconds kept
    run(bus, "item.set", R"({"item":"it_1","length":4})");
    CHECK(app->head().items.find(a)->length == doctest::Approx(2.0));
    // Half the tempo: the time item stays at 3 s, the beats item doubles its time.
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":60}]})");
    CHECK(span(a).startSec == doctest::Approx(3.0));
    CHECK(span(a).endSec == doctest::Approx(5.0));
    CHECK(span(b).startSec == doctest::Approx(8.0));
    CHECK(span(b).endSec == doctest::Approx(10.0));
    // Moved to B by beat, it keeps its seconds length.
    run(bus, "item.move", R"({"item":"it_1","track":"tr_2","position":1})");
    CHECK(span(a).startSec == doctest::Approx(1.0));
    CHECK(span(a).endSec == doctest::Approx(3.0));
    // A track's timebase converts its inheriting items, keeping them in place.
    run(bus, "track.set", R"({"track":"tr_1","timebase":"time"})");
    CHECK(app->head().items.find(b)->position == doctest::Approx(8.0));  // seconds now
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":120}]})");
    CHECK(span(b).startSec == doctest::Approx(8.0));
    CHECK(span(b).endSec == doctest::Approx(10.0));
    // beats_position: the start follows the beat, the length stays in seconds.
    run(bus, "item.set", R"({"item":"it_2","timebase":"beats_position"})");
    CHECK(app->head().items.find(b)->position == doctest::Approx(16.0));  // beat at 8 s
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":60}]})");
    CHECK(span(b).startSec == doctest::Approx(16.0));
    CHECK(span(b).endSec - span(b).startSec == doctest::Approx(2.0));
    // The project's timebase converts what inherits it (items on tracks that
    // follow the project); a duplicate lands right after the original.
    run(bus, "item.add", R"({"track":"tr_2","position":2,"length":1,"notation":"1: C4:q r:*"})");
    const model::Id c = *model::Id::parse("it_3");
    run(bus, "project.set", R"({"timebase":"time"})");
    CHECK(app->head().project->timebase == model::Timebase::Time);
    CHECK(app->head().items.find(c)->position == doctest::Approx(2.0));  // beat 2 at 60 BPM
    const auto dup = run(bus, "item.duplicate", R"({"item":"it_3"})");
    const model::Id d = *model::Id::parse(dup.find("id")->asString());
    CHECK(span(d).startSec == doctest::Approx(span(c).endSec));
    // The describe text says which items do not follow beats.
    const auto text = run(bus, "session.describe", "{}");
    CHECK(json::writeCompact(text).find("[time]") != std::string::npos);
    // One undo step undoes the project change and its conversions together.
    run(bus, "history.undo", "{}");
    run(bus, "history.undo", "{}");
    CHECK(app->head().project->timebase == model::Timebase::Beats);
    CHECK(app->head().items.find(c)->position == doctest::Approx(2.0));
}

TEST_CASE("renders: FLAC files, a secondary format, and a queue pinned to its versions") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Synth"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","position":0,"length":4,"notation":"1: C4:w"})");
    // A FLAC mix with a 16-bit WAV beside it.
    const auto mix = run(bus, "render.mix",
                         R"({"name":"song","end_beat":2,"format":"flac","bit_depth":24,
                             "secondary":{"format":"wav","bit_depth":16}})");
    const fs::path flac = mix.find("path")->asString();
    const fs::path wav = mix.find("secondary_path")->asString();
    CHECK(flac.extension() == ".flac");
    CHECK(wav.filename() == "song.wav");
    auto a = audio::readAudio(flac);
    auto b = audio::readAudio(wav);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(audio::inspect(flac)->container == audio::Container::Flac);
    CHECK(a->frames() == b->frames());
    CHECK(peak(flac) == doctest::Approx(peak(wav)).epsilon(1e-3));
    CHECK(peak(flac) > 0.05);
    // Stems as FLAC; a same-container secondary carries its depth in the name.
    const auto stems = run(bus, "render.stems",
                           R"({"pattern":"stem-{track}","end_beat":2,"format":"flac","bit_depth":16,
                               "secondary":{"format":"flac","bit_depth":24}})");
    const auto& file = stems.find("files")->at(0);
    CHECK(fs::path(file.find("path")->asString()).filename() == "stem-Synth.flac");
    CHECK(fs::path(file.find("secondary_path")->asString()).filename() == "stem-Synth.24.flac");
    CHECK(fs::exists(file.find("secondary_path")->asString()));
    // FLAC is 16 or 24 bit: a contradiction in the settings (engine.md §3).
    auto refused = bus.execute({"render.mix", j(R"({"format":"flac","bit_depth":32})"), kOrigin});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "render.args");
    CHECK(refused.error().path == "/args/outputs/0/bit_depth");
    // The queue: q1 now, then the track 12 dB down and q2; q1 still renders loud.
    const auto q1 = run(bus, "render.queue.add",
                        R"({"command":"render.mix","args":{"name":"before","end_beat":2}})");
    CHECK(q1.find("id")->asString() == "q1");
    run(bus, "track.set", R"({"track":"tr_1","volume_db":-12})");
    run(bus, "render.queue.add",
        R"({"command":"render.stems","args":{"pattern":"after-{track}","end_beat":2},"label":"stems"})");
    CHECK(run(bus, "render.queue.list", "{}").find("queue")->size() == 2);
    const auto ran = run(bus, "render.queue.run", "{}");
    REQUIRE(ran.find("renders")->size() == 2);
    const fs::path before =
        ran.find("renders")->at(0).find("files")->at(0).find("path")->asString();
    const fs::path after = ran.find("renders")->at(1).find("files")->at(0).find("path")->asString();
    CHECK(before.filename() == "before.wav");
    CHECK(ran.find("renders")->at(1).find("label")->asString() == "stems");
    CHECK(peak(before) / peak(after) == doctest::Approx(std::pow(10.0, 12.0 / 20.0)).epsilon(0.02));
    CHECK(run(bus, "render.queue.list", "{}").find("queue")->size() == 0);  // done: off the queue
    // Remove and clear.
    run(bus, "render.queue.add", R"({"command":"render.mix","args":{"end_beat":1}})");
    run(bus, "render.queue.add", R"({"command":"render.mix","args":{"end_beat":1,"name":"x"}})");
    run(bus, "render.queue.remove", R"({"id":"q3"})");
    CHECK(run(bus, "render.queue.list", "{}").find("queue")->size() == 1);
    CHECK(run(bus, "render.queue.remove", R"({"all":true})").find("removed")->asInt() == 1);
    CHECK(bus.execute({"render.queue.run", j("{}"), kOrigin}).error().code == "render.queue_empty");
}

TEST_CASE("take FX and take envelopes over the bus: add, reorder, copy with the item, remove") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Synth"})");
    run(bus, "item.add", R"({"track":"tr_1","position":0,"length":4,"notation":"1: C4:w"})");
    // A MIDI take's envelope needs an instrument in its take FX.
    auto refused = bus.execute({"env.add", j(R"({"take":"tk_1","param":"volume"})"), kOrigin});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "env.take_midi");
    run(bus, "fx.add", R"({"take":"tk_1","plugin":"Test Sine"})");                  // fx_1
    run(bus, "fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");  // fx_2
    const model::Id tk1 = *model::Id::parse("tk_1");
    CHECK(app->head().takes.find(tk1)->chain.size() == 2);
    run(bus, "fx.move", R"({"fx":"fx_2","index":0})");
    CHECK(app->head().takes.find(tk1)->chain.front() == *model::Id::parse("fx_2"));
    CHECK(bus.execute({"fx.move", j(R"({"fx":"fx_2","track":"tr_1","index":0})"), kOrigin})
              .error()
              .code == "fx.chain");
    // Points are beats from the item's start; bars are refused.
    CHECK(bus.execute({"env.add",
                       j(R"({"take":"tk_1","param":"volume","points":[{"bar":1,"value":0}]})"),
                       kOrigin})
              .error()
              .code == "env.position");
    run(bus, "env.add",
        R"({"take":"tk_1","param":"volume","points":[{"beat":0,"value":0},{"beat":4,"value":-12}]})");
    run(bus, "env.add", R"({"fx":"fx_2","param":"gain","points":[{"beat":0,"value":0}]})");
    // The item renders through its take chain (the track has no instrument).
    const auto mix = run(bus, "render.mix", R"({"name":"take","end_beat":4})");
    CHECK(peak(mix.find("path")->asString()) > 0.01);
    // A split's right side gets copies: its take FX and its envelope, from its start.
    const auto split = run(bus, "item.split", R"({"item":"it_1","at":2})");
    const model::Id right = *model::Id::parse(split.find("right")->asString());
    const model::Session& s = app->head();
    const model::Id rightTake = *s.items.find(right)->activeTake;
    CHECK(s.takes.find(rightTake)->chain.size() == 2);
    CHECK(s.takes.find(rightTake)->chain != s.takes.find(tk1)->chain);  // copies, not shared
    const model::Envelope* copied = nullptr;
    for (const auto& e : s.envelopes)
        if (e.value->target.object == rightTake) copied = e.value.get();
    REQUIRE(copied);
    REQUIRE(copied->points.size() == 2);
    CHECK(copied->points[0].pos == doctest::Approx(0.0));
    CHECK(copied->points[0].value == doctest::Approx(-6.0));  // halfway down the ramp
    CHECK(copied->points[1].pos == doctest::Approx(2.0));
    std::size_t fxEnvelopes = 0;
    for (const auto& e : s.envelopes) fxEnvelopes += e.value->target.object.kind == model::Kind::Fx;
    CHECK(fxEnvelopes == 2);  // the take FX envelope was copied with its plug-in
    // Removing the item removes its takes' FX and envelopes.
    const std::string removeArgs = R"({"item":")" + split.find("right")->asString() + R"("})";
    run(bus, "item.remove", removeArgs);
    std::size_t left = 0;
    for (const auto& e : app->head().envelopes) left += e.value->target.object == rightTake;
    CHECK(left == 0);
}

TEST_CASE("automation items: an item replaces the envelope in a render; a pool edits together") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Synth"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","position":0,"length":8,"notation":"1: A4:w | A4:w"})");
    run(bus, "env.add", R"({"track":"tr_1","param":"volume","points":[{"beat":0,"value":0}]})");
    // Beats 2-4 (1-2 s at 120 BPM) silenced by an item over a flat 0 dB envelope.
    const auto item =
        run(bus, "autoitem.add",
            R"({"env":"en_1","position":2,"length":2,"points":[{"beat":0,"value":"-150 dB"}]})");
    CHECK(item.find("id")->asString() == "ai_1");
    const auto mix = run(bus, "render.mix", R"({"name":"items","end_beat":8})");
    auto audio = audio::readAudio(mix.find("path")->asString());
    REQUIRE(audio);
    auto windowPeak = [&](double from, double to) {
        double p = 0;
        for (std::size_t i = static_cast<std::size_t>(from * 48000);
             i < static_cast<std::size_t>(to * 48000); ++i)
            p = std::max(p, std::fabs(double(audio->channels[0][i])));
        return p;
    };
    CHECK(windowPeak(0.2, 0.9) > 0.05);    // before it
    CHECK(windowPeak(1.05, 1.95) < 1e-4);  // under it (past the fader's 10 ms ramp)
    CHECK(windowPeak(2.1, 2.9) > 0.05);    // after it
    // A pooled copy: rewriting the copy's points rewrites the original's.
    const auto copy = run(bus, "autoitem.duplicate", R"({"item":"ai_1","position":6})");
    CHECK(copy.find("pool")->asInt() == 1);
    run(bus, "autoitem.set",
        R"({"item":"ai_2","points":[{"beat":0,"value":"-6 dB"},{"beat":1,"value":0}]})");
    const auto listed = run(bus, "autoitem.list", R"({"env":"en_1"})");
    REQUIRE(listed.find("items")->size() == 2);
    for (const auto& e : listed.find("items")->asArray()) {
        REQUIRE(e.find("points")->size() == 2);
        CHECK(e.find("points")->at(0).find("value")->asDouble() == doctest::Approx(-6.0));
    }
    // Unpooled, an edit stays with its item.
    run(bus, "autoitem.unpool", R"({"item":"ai_2"})");
    run(bus, "autoitem.set", R"({"item":"ai_2","points":[{"beat":0,"value":-3}]})");
    CHECK(app->head().autoItems.find(*model::Id::parse("ai_1"))->points.size() == 2);
    // An item on a take envelope is refused; removing the envelope removes its items.
    run(bus, "env.remove", R"({"env":"en_1"})");
    CHECK(app->head().autoItems.empty());
    run(bus, "history.undo", "{}");
    CHECK(app->head().autoItems.size() == 2);
}

TEST_CASE("track removal cascades envelopes and automation items on input FX") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "fx.add", R"({"track":"tr_1","chain":"input","plugin":"com.suppleaudio.test.gain"})");
    run(bus, "env.add", R"({"fx":"fx_1","param":"gain","points":[{"beat":0,"value":-3}]})");
    run(bus, "autoitem.add", R"({"env":"en_1","position":0,"length":4})");
    const auto before = model::serializeProject(app->head());
    run(bus, "track.remove", R"({"track":"tr_1"})");
    CHECK(app->head().fx.empty());
    CHECK(app->head().envelopes.empty());
    CHECK(app->head().autoItems.empty());
    CHECK(model::checkInvariants(app->head()));
    run(bus, "history.undo", "{}");
    checkUndoContent(app->head(), before);
    run(bus, "history.redo", "{}");
    CHECK(app->head().autoItems.empty());
}

TEST_CASE("render planning refuses collisions between primary and secondary files") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"one"})");
    run(bus, "track.add", R"({"name":"one.16"})");
    for (const char* command : {"render.dry-run", "render.stems"}) {
        CHECK(
            fails(
                bus, command,
                R"({"operation":"stems","end_beat":1,"pattern":"{track}","format":"wav","bit_depth":32,"secondary":{"format":"wav","bit_depth":16}})") ==
            "render.path");
        CHECK_FALSE(fs::exists(app->rendersDir() / "one.wav"));
    }
    run(bus, "render.dry-run",
        R"({"operation":"stems","end_beat":1,"pattern":"{counter}-{track}","secondary":{"format":"wav","bit_depth":16}})");
}

TEST_CASE("audio item splits advance source offsets by the take play rate") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","length":8})");
    run(bus, "take.set", R"({"take":"tk_1","start_offset":0.25,"playrate":2})");
    const auto before = model::serializeProject(app->head());
    run(bus, "item.split", R"({"item":"it_1","at":2})");
    const auto* right = app->head().takes.find(*model::Id::parse("tk_2"));
    REQUIRE(right);
    CHECK(right->startOffset == doctest::Approx(2.25));
    run(bus, "history.undo", "{}");
    checkUndoContent(app->head(), before);
}

TEST_CASE("MIDI queries report item length in beats for time-based items") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    run(bus, "item.set", R"({"item":"it_1","timebase":"time"})");
    CHECK(run(bus, "midi.notes", R"({"item":"it_1"})")["length"].asDouble() == 4.0);
    // Imported or project-file notes can be shorter than the editor's minimum.
    run(bus, "midi.edit", R"({"item":"it_1","ops":[{"op":"remove","notes":[0]}]})");
    auto doc = model::projectToJson(app->head());
    doc["sources"]["midi"]["mi_1"]["sequence"]["notes"] = j("[[0,1,1,60,96]]");
    REQUIRE(writeFileAtomic(dir.path / "short.json", json::writeCanonical(doc)));
    run(bus, "project.open", R"({"path":"short.json"})");
    run(bus, "midi.edit", R"({"item":"it_1","ops":[{"op":"resize","notes":[0],"start_delta":1}]})");
    const auto& note = app->head().midi.find(*model::Id::parse("mi_1"))->sequence.notes[0];
    CHECK(note.start == 0);
    CHECK(note.length == 1);
}

TEST_CASE("merge remaps creations within a batch and versions project metadata") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "branch.create", R"({"name":"agent","switch":false})");
    run(bus, "track.add", R"({"name":"Human"})");
    run(bus, "project.set", R"({"author":"Human"})");
    run(bus, "branch.switch", R"({"name":"agent"})");
    run(bus, "batch", R"({"steps":[
      {"command":"track.add","args":{"name":"Agent"}},
      {"command":"track.set","args":{"track":"tr_1","volume_db":-9}},
      {"command":"project.set","args":{"title":"Song"}}]})");
    auto changes = run(bus, "branch.diff", R"({"a":"main","b":"agent"})");
    CHECK(json::writeCompact(changes).find("project") != std::string::npos);
    run(bus, "branch.merge", R"({"from":"agent","into":"main"})");
    run(bus, "branch.switch", R"({"name":"main"})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_1"))->volumeDb == 0);
    CHECK(app->head().tracks.find(*model::Id::parse("tr_2"))->volumeDb == -9);
    CHECK(app->head().project->author == "Human");
    CHECK(app->head().project->title == "Song");
    run(bus, "branch.create", R"({"name":"other","switch":false})");
    run(bus, "project.set", R"({"name":"Mine"})");
    run(bus, "branch.switch", R"({"name":"other"})");
    run(bus, "project.set", R"({"name":"Theirs"})");
    auto conflict = bus.execute({"branch.merge", j(R"({"from":"other","into":"main"})"), kOrigin});
    REQUIRE_FALSE(conflict);
    CHECK(conflict.error().code == "branch.conflict");
    REQUIRE(app->undo());
    run(bus, "project.set", R"({"name":"Mine"})");
    run(bus, "branch.merge", R"({"from":"other","into":"main"})");
}

TEST_CASE("track add accepts auxiliary channels and rejects mono busses") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"channels":8})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_1"))->channels == 8);
    auto bad = bus.execute({"track.add", j(R"({"type":"buss","channels":1})"), kOrigin});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "track.channels");
}

TEST_CASE("MIDI extreme numbers and automation expansions refuse without edits") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const auto before = model::serializeProject(app->head());
    for (
        const auto* args :
        {R"({"item":"it_1","ops":[{"op":"add","pitch":60,"start":1e300,"length":1}]})",
         R"({"item":"it_1","ops":[{"op":"move","notes":[0],"beats":1e300}]})",
         R"({"item":"it_1","ops":[{"op":"velocity","notes":[0],"velocity_delta":9223372036854775807}]})",
         R"({"item":"it_1","ops":[{"op":"quantize","grid":1e-300}]})"}) {
        REQUIRE_FALSE(bus.execute({"midi.edit", j(args), kOrigin}));
        CHECK(model::serializeProject(app->head()) == before);
    }
    run(bus, "env.add", R"({"track":"tr_1","param":"volume"})");
    auto bad = bus.execute(
        {"autoitem.add",
         j(R"({"env":"en_1","position":0,"length":1e9,"source_length":0.000001,"loop":true})"),
         kOrigin});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "automation.budget");
    CHECK(app->head().autoItems.empty());
}

TEST_CASE("take copies clone FX envelopes and pooled items and split shaped boundaries") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const auto source = app->head();
    auto transaction = app->beginTransaction(kOrigin, "take automation");
    REQUIRE(transaction);
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    const auto fxId = app->txn().create(fx);
    const auto takeId = *source.items.begin()->value->activeTake;
    app->txn().modify<model::Take>(takeId, [&](auto& take) { take.chain = {fxId}; });
    model::Envelope env;
    env.target = {takeId, "volume"};
    env.points = {{0, -12, model::PointShape::FastStart, 0}, {4, 0}};
    const auto envId = app->txn().create(env);
    env.target = {fxId, "gain"};
    const auto fxEnv = app->txn().create(env);
    model::AutomationItem ai;
    ai.envelope = fxEnv;
    ai.length = 4;
    ai.sourceLength = 4;
    ai.pool = 7;
    app->txn().create(ai);
    REQUIRE(app->commitTransaction(*transaction));
    const double boundary = model::pointsValueAt(app->head().envelopes.find(envId)->points, 2, 0);
    run(bus, "item.duplicate", R"({"item":"it_1"})");
    CHECK(app->head().fx.size() == 2);
    CHECK(app->head().autoItems.size() == 2);
    for (const auto& e : app->head().autoItems) CHECK(e.value->pool == 7);
    run(bus, "item.split", R"({"item":"it_1","at":2})");
    CHECK(app->head().fx.size() == 3);
    CHECK(app->head().autoItems.size() == 3);
    CHECK(app->head().envelopes.find(envId)->points.back().value == doctest::Approx(boundary));
    CHECK(app->head().envelopes.find(envId)->points.back().shape == model::PointShape::FastStart);
    int right = 0;
    for (const auto& e : app->head().envelopes)
        if (e.value->target.object.kind == model::Kind::Take &&
            e.value->points.front().value == boundary) {
            CHECK(e.value->points.front().pos == 0);
            CHECK(e.value->points.front().shape == model::PointShape::FastStart);
            ++right;
        }
    CHECK(right == 1);
    REQUIRE(model::checkInvariants(app->head()));
}

TEST_CASE("ruler lists expose timebase and descriptions convert seconds to beats") {
    Workdir dir;
    auto app = makeApp(dir.path);
    auto transaction = app->beginTransaction(kOrigin, "time rulers");
    REQUIRE(transaction);
    app->txn().create(model::Marker{2, model::Timebase::Time, "Second bar", ""});
    app->txn().create(model::Region{2, 4, model::Timebase::Time, "Region", ""});
    REQUIRE(app->commitTransaction(*transaction));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    const auto markers = run(bus, "marker.list", "{}");
    CHECK(*markers.find("markers")->at(0).find("timebase") == "time");
    const auto description = describeSession(app->head());
    CHECK(description.find("Second bar\" bar 2") != std::string::npos);
    CHECK(description.find("Region\" bar 2") != std::string::npos);
}

TEST_CASE("unheaded notation follows the item's project meter through later changes") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Meter"})");
    run(bus, "timesig.set", R"({"bar":1,"meter":"3/4"})");
    run(bus, "timesig.set", R"({"bar":3,"meter":"5/8"})");
    auto item = run(bus, "item.add",
                    R"({"track":"tr_1","bar":2,"notation":"voice a; 1: C4:q r:* | D4:e r:*"})");
    CHECK(item["position"].asDouble() == 3);
    CHECK(item["length"].asDouble() == 5.5);
    const auto text = run(bus, "midi.get", R"({"item":"it_1"})")["text"].asString();
    json::Value write = json::Value::object();
    write["item"] = "it_1";
    write["notation"] = text;
    auto rewritten = bus.execute({"midi.write", write, kOrigin});
    REQUIRE_MESSAGE(rewritten, (rewritten ? "" : rewritten.error().toString()));
    CHECK(model::itemLengthBeats(app->head(), *app->head().items.find(*model::Id::parse("it_1"))) ==
          5.5);
}

TEST_CASE("commands reject numeric overflow and impossible derived work without edits") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Numbers"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    run(bus, "env.add", R"({"track":"tr_1","param":"volume"})");
    run(bus, "autoitem.add",
        R"({"env":"en_1","position":0,"length":4,"points":[{"beat":0,"value":0}]})");
    for (const auto& [command, args] : std::vector<std::pair<std::string, std::string>>{
             {"time.convert", R"({"seconds":1e300})"},
             {"time.convert", R"({"bar":2147483648})"},
             {"transport.seek", R"({"beat":1e300})"},
             {"transport.loop", R"({"start_seconds":0,"end_seconds":1e300})"},
             {"transport.punch", R"({"start_bar":1,"end_bar":2147483648})"},
             {"item.add", R"({"track":"tr_1","bar":2147483648})"},
             {"item.add", R"({"track":"tr_1","position":1e300})"},
             {"item.set", R"({"item":"it_1","length":1e300})"},
             {"item.move", R"({"item":"it_1","position":1e300})"},
             {"item.duplicate", R"({"item":"it_1","to":1e300})"},
             {"item.set", R"({"item":"it_1","fade_in":1e300})"},
             {"take.set", R"({"take":"tk_1","start_offset":1e300})"},
             {"timesig.set", R"({"bar":2147483648,"meter":"4/4"})"},
             {"tempo.set", R"({"points":[{"beat":0,"bpm":120},{"beat":1e300,"bpm":120}]})"},
             {"marker.add", R"({"pos":1e300})"},
             {"region.add", R"({"start":0,"end":1e300})"},
             {"env.points.set", R"({"env":"en_1","points":[{"beat":1e300,"value":0}]})"},
             {"env.points.set", R"({"env":"en_1","points":[{"beat":0,"value":1e300}]})"},
             {"autoitem.set", R"({"item":"ai_1","length":1e300})"},
             {"autoitem.duplicate", R"({"item":"ai_1","position":1e300})"},
             {"render.mix", R"({"end_beat":1e300})"},
             {"render.dry-run", R"({"end_beat":1e300})"},
             {"render.mix", R"({"end_beat":20000,"sample_rate":384000})"},
             {"project.set", R"({"render":{"sample_rate":1e300}})"},
             {"project.set", R"({"render":{"tail_ms":1e300}})"}}) {
        const auto before = model::serializeProject(app->head());
        auto result = bus.execute({command, j(args), kOrigin});
        REQUIRE_MESSAGE(!result, (command + " accepted " + args));
        CHECK_MESSAGE(!result.error().path.empty(), (command + ": " + result.error().toString()));
        CHECK(model::serializeProject(app->head()) == before);
    }
}

TEST_CASE("a queued render keeps a discarded redo version until the queue releases it") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Pinned"})");
    run(bus, "item.add", R"({"track":"tr_1","length":1})");
    const auto queuedVersion = app->head().version;
    std::weak_ptr<const model::Session> weak = app->versions().pin(queuedVersion);
    run(bus, "render.queue.add",
        R"({"command":"render.mix","args":{"end_beat":1,"name":"pinned"}})");
    run(bus, "history.undo", "{}");
    run(bus, "track.set", R"({"track":"tr_1","name":"Discard redo"})");
    REQUIRE(app->versions().version(queuedVersion));
    REQUIRE(run(bus, "render.queue.run", "{}").find("renders"));
    app->versions().collectUnused();
    CHECK_FALSE(app->versions().version(queuedVersion));
    CHECK(weak.expired());
    auto nested = bus.execute(
        {"render.queue.add", j(R"({"command":"render.mix","args":{"end_beat":1e300}})"), kOrigin});
    REQUIRE_FALSE(nested);
    CHECK(nested.error().path == "/args/args/end_beat");
}

TEST_CASE("numeric sample bounds use the actual rate and recheck device-rate changes") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "project.new", R"({"sample_rate":8000})");
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":1}]})");
    run(bus, "timesig.set", R"({"bar":1,"meter":"64/1"})");
    // Fits signed samples at 8 kHz and int bars; exceeds samples at 384 kHz.
    run(bus, "marker.add", R"({"pos":500000000000})");
    auto converted = run(bus, "time.convert", R"({"beat":500000000000})");
    CHECK(converted["seconds"].asDouble() == 30000000000000.0);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    auto started =
        app->startEngine(backend, {.sampleRate = 384000, .blockSize = 64}, {.anticipative = false});
    REQUIRE_FALSE(started);
    CHECK(started.error().path == "/rulers/markers/mk_1/pos");
    CHECK_FALSE(app->engine());
    run(bus, "project.new", R"({"sample_rate":8000})");
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":1}]})");
    run(bus, "timesig.set", R"({"bar":1,"meter":"64/1"})");
    REQUIRE(app->startEngine(backend, {.sampleRate = 384000, .blockSize = 64},
                             {.anticipative = false}));
    const auto before = model::serializeProject(app->head());
    auto edit = bus.execute({"marker.add", j(R"({"pos":500000000000})"), kOrigin});
    REQUIRE_FALSE(edit);
    CHECK(edit.error().path == "/args/pos");
    CHECK(model::serializeProject(app->head()) == before);
    app->stopEngine();
}

TEST_CASE("automation duplication refuses pool-id overflow without changing either item") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Pools"})");
    run(bus, "env.add", R"({"track":"tr_1","param":"volume"})");
    run(bus, "autoitem.add", R"({"env":"en_1","position":0,"length":4})");
    run(bus, "autoitem.add", R"({"env":"en_1","position":4,"length":4})");
    auto txn = app->beginTransaction(kOrigin, "pool fixture");
    REQUIRE(txn);
    app->txn().modify<model::AutomationItem>(
        *model::Id::parse("ai_1"), [](auto& ai) { ai.pool = std::numeric_limits<int>::max(); });
    REQUIRE(app->commitTransaction(*txn));
    const auto before = model::serializeProject(app->head());
    auto r = bus.execute({"autoitem.duplicate", j(R"({"item":"ai_2"})"), kOrigin});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "autoitem.pool");
    CHECK(model::serializeProject(app->head()) == before);
}

TEST_CASE("project history budget is persisted and history lists an earlier-history step") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "project.set", R"({"history_budget_bytes":1})");
    run(bus, "track.add", R"({"name":"A"})");
    run(bus, "track.set", R"({"track":"tr_1","name":"B"})");
    auto history = run(bus, "history.list", "{}");
    CHECK(history["truncated"].asBool());
    CHECK(history["budget_bytes"].asInt() == 1);
    CHECK(history["retained_bytes"].asInt() > 1);  // mandatory current/root payloads
    CHECK(history["versions"].asArray()[0]["label"] == "earlier history");
    CHECK(history["versions"].asArray()[0]["squashed_steps"].asInt() > 0);
    auto saved = model::parseProject(model::serializeProject(app->head()));
    REQUIRE(saved);
    CHECK(saved->project->historyBudgetBytes == 1);
    run(bus, "history.undo", "{}");
    CHECK_FALSE(app->head().tracks.contains(model::Id{model::Kind::Track, 1}));
    run(bus, "history.redo", "{}");
    CHECK(app->head().tracks.find(model::Id{model::Kind::Track, 1})->name == "B");
}

TEST_CASE(
    "engine catalogue commands retain failed-module cache reports with no available plugins") {
    Workdir dir;
    const auto cache = dir.path / "cache";
    struct Environment {
        std::optional<std::string> previous;
        ~Environment() {
            if (previous)
                ::setenv("XDG_CACHE_HOME", previous->c_str(), 1);
            else
                ::unsetenv("XDG_CACHE_HOME");
        }
    } env;
    if (const char* previous = std::getenv("XDG_CACHE_HOME")) env.previous = previous;
    REQUIRE(::setenv("XDG_CACHE_HOME", cache.c_str(), 1) == 0);
    struct Bridge {
        fs::path previous =
            fs::path(FIXTURE_CLAP_PATH).parent_path() / (std::string(product::kSlug) + "-bridge");
        ~Bridge() { host::setBridgeExecutable(previous); }
    } bridge;
    const auto bad = dir.path / "bad.clap";
    REQUIRE(writeFileAtomic(bad, "invalid plugin"));
    AppOptions options;
    options.workdir = dir.path;
    options.pluginPath = {bad};
    auto opened = EngineApp::create(options);
    REQUIRE(opened);
    auto& app = **opened;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, app);
    auto first = run(bus, "plugin.list", "{}");
    CHECK(first["plugins"].size() == 0);
    auto failure = first["scan"]["failures"].asArray()[0];
    CHECK(failure["module"] == fs::canonical(bad).string());
    CHECK(failure["reason"]["code"] == "host.load");
    CHECK(failure["reason"]["path"].isString());
    CHECK(failure["reason"]["hint"].isString());
    CHECK_FALSE(failure["cached"].asBool());
    host::setBridgeExecutable(dir.path / "missing-bridge");
    auto second = run(bus, "plugin.scan", "{}");
    CHECK(second["plugins"].size() == 0);
    CHECK(second["scan"]["warnings"].asArray()[0]["code"] == "host.scan.unsupervised");
    CHECK(second["scan"]["failures"].asArray()[0]["cached"].asBool());
    CHECK(second["scan"]["cached_modules"].asInt() == 1);
    const std::vector<fs::path> empty;
    REQUIRE(host::scan(empty));
    auto retained = run(bus, "plugin.list", R"({"filter":"nothing"})");
    CHECK(retained["scan"] == second["scan"]);
    CHECK(retained["scan"]["warnings"].isArray());
}

TEST_CASE("MIDI selections apply each original note once") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    run(bus, "midi.edit",
        R"({"item":"it_1","ops":[{"op":"move","notes":[0,0],"beats":1,"semitones":1},{"op":"resize","notes":[0,0],"length_delta":1},{"op":"velocity","notes":[0,0],"velocity_delta":-1}]})");
    const auto notes = run(bus, "midi.notes", R"({"item":"it_1"})");
    const auto* source = app->head().midi.find(*app->head().takes.begin()->value->source);
    REQUIRE(source);
    CHECK(source->sequence.notes[0].start == 960);
    CHECK(source->sequence.notes[0].pitch == 61);
    CHECK(source->sequence.notes[0].length == 5 * 960);
    CHECK(source->sequence.notes[0].velocity == 95);
    CHECK(notes.find("notes")->size() == 1);
}

TEST_CASE("normalization rejects nonfinite derived gain and samples before mutation") {
    for (double target : {1e308, 800.0}) {
        audio::AudioData data;
        data.sampleRate = 48000;
        data.channels.assign(2, std::vector<float>(4800, 0.25f));
        const auto before = data.channels;
        auto status = normalizeRenderAudio(data, "peak", target);
        CHECK_FALSE(status);
        if (!status) {
            CHECK(status.error().code == "render.normalize");
            CHECK_FALSE(status.error().hint.empty());
        }
        CHECK(bool(data.channels == before));
    }
    for (float sample :
         {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        audio::AudioData data;
        data.sampleRate = 48000;
        data.channels.assign(2, std::vector<float>(4800, 0.25f));
        data.channels[1][100] = sample;
        auto status = normalizeRenderAudio(data, "peak", -1);
        REQUIRE_FALSE(status);
        CHECK(status.error().code == "render.normalize");
        CHECK(status.error().path == "/normalize");
        CHECK_FALSE(status.error().hint.empty());
        CHECK(data.channels[0][0] == 0.25f);
        CHECK(data.channels[1][0] == 0.25f);
    }
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    auto result = bus.execute(
        {"render.mix",
         j(R"({"name":"overflow","end_beat":1,"normalize":{"mode":"peak","target":1e308}})"),
         kOrigin});
    // Targets above 0 are outside the settings' range (session-model §1.10).
    CHECK_FALSE(result);
    if (!result) {
        const auto& error =
            result.error().details.empty() ? result.error() : result.error().details.front();
        CHECK(error.path == "/args/normalize/target");
        CHECK_FALSE(error.hint.empty());
    }
    CHECK_FALSE(fs::exists(app->rendersDir() / "overflow.wav"));
}

TEST_CASE("render aliases collide by parent identity and filename") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    fs::create_directories(app->rendersDir() / "real");
    fs::create_directory_symlink("real", app->rendersDir() / "alias");
    run(bus, "track.add", R"({"name":"real/same"})");
    run(bus, "track.add", R"({"name":"alias/same"})");
    auto result = bus.execute(
        {"render.dry-run",
         j(R"({"operation":"stems","end_beat":1,"pattern":"{track}","secondary":{"format":"flac","bit_depth":16}})"),
         kOrigin});
    CHECK_FALSE(result);
    if (!result) {
        CHECK(result.error().code == "render.path");
        CHECK(result.error().path == "/args/pattern");
        CHECK_FALSE(result.error().hint.empty());
    }
    // Replacing distinct final symlinks is safe: their shared target is not
    // the write destination of an atomic rename.
    fs::create_symlink("shared.wav", app->rendersDir() / "real/same.wav");
    fs::create_symlink("shared.wav", app->rendersDir() / "real/other.wav");
    run(bus, "track.set", R"({"track":"tr_2","name":"real/other"})");
    auto distinct =
        bus.execute({"render.dry-run",
                     j(R"({"operation":"stems","end_beat":1,"pattern":"{track}"})"), kOrigin});
    CHECK(distinct);
}

TEST_CASE("routes map channel 128 and survive saving and reopening") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"channels":128})");
    run(bus, "track.add", R"({"type":"buss","channels":128})");
    auto result = bus.execute(
        {"route.add",
         j(R"({"from":"tr_1","to":"tr_2","channels":{"from":[65,128],"to":[128,65]}})"), kOrigin});
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    const auto project = dir.path / "mapped.json";
    auto args = json::Value::object();
    args["path"] = project.string();
    REQUIRE(bus.execute({"project.save", args, kOrigin}));
    const auto canonical = model::serializeProject(app->head());
    run(bus, "project.new", "{}");
    REQUIRE(bus.execute({"project.open", args, kOrigin}));
    const auto* route = app->head().routes.find(*model::Id::parse("sd_1"));
    REQUIRE(route);
    CHECK(route->channels.from == std::vector<int>{65, 128});
    CHECK(route->channels.to == std::vector<int>{128, 65});
    CHECK(model::serializeProject(app->head()) == canonical);
    auto tooWide = bus.execute(
        {"route.add", j(R"({"from":"tr_1","to":"tr_2","channels":{"from":[129],"to":[1]}})"),
         kOrigin});
    CHECK_FALSE(tooWide);
}

TEST_CASE("track templates preserve channel count and accept legacy stereo templates") {
    Workdir dir;
    AppOptions options;
    options.workdir = dir.path;
    options.templateDir = dir.path / "templates";
    options.projectTemplateDir = dir.path / "project-templates";
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"channels":4})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    run(bus, "fx.set", R"({"fx":"fx_1","pins":{"in":[3,4]}})");
    const auto saved = run(bus, "track.template.save", R"({"track":"tr_1","name":"Wide"})");
    const auto restored = run(bus, "track.template.load", R"({"name":"Wide"})");
    const auto* strip =
        app->head().tracks.find(*model::Id::parse(restored.find("track")->asString()));
    REQUIRE(strip);
    CHECK(strip->channels == 4);
    REQUIRE(strip->chain.size() == 1);
    CHECK(app->head().fx.find(strip->chain[0])->pins.in == std::vector<int>{3, 4});
    const auto reopened = model::parseProject(model::serializeProject(app->head()));
    REQUIRE(reopened);
    CHECK(reopened->tracks.find(*model::Id::parse(restored.find("track")->asString()))->channels ==
          4);
    auto doc = json::parse(readTextFile(saved.find("path")->asString()).value()).value();
    doc["track"].asObject().erase("channels");
    doc["chain"] = json::Value::array();
    const auto legacy = dir.path / "legacy-template.json";
    REQUIRE(writeFileAtomic(legacy, json::writeCanonical(doc)));
    auto args = json::Value::object();
    args["path"] = legacy.string();
    const auto old = bus.execute({"track.template.load", args, kOrigin});
    REQUIRE(old);
    CHECK(app->head().tracks.find(*model::Id::parse(old->find("track")->asString()))->channels ==
          2);
}

TEST_CASE("track.duplicate copies a strip after itself with its FX, items, sends and automation") {
    Workdir dir;
    AppOptions options;
    options.workdir = dir.path;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Bass"})");                // tr_1
    run(bus, "track.add", R"({"name":"Keys"})");                // tr_2
    run(bus, "track.add", R"({"type":"buss","name":"Verb"})");  // tr_3
    run(bus, "route.add", R"({"from":"tr_1","to":"tr_3","volume_db":-9})");
    run(bus, "fx.add",
        R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-6}}})");
    run(bus, "item.add", R"({"track":"tr_1","bar":1,"notation":"voice bass; 1: C2:q r:*"})");
    run(bus, "env.add",
        R"({"fx":"fx_1","param":"gain","points":[{"beat":0,"value":-12},{"beat":4,"value":0}]})");
    const auto made = run(bus, "track.duplicate", R"({"tracks":["tr_1"]})");
    const model::Id copy = *model::Id::parse(made.find("tracks")->asArray().front().asString());
    const model::Session& s = app->head();
    const model::Id one = *model::Id::parse("tr_1");
    CHECK(s.tracks.find(model::kMaster)->children ==
          std::vector<model::Id>{one, copy, *model::Id::parse("tr_2"), *model::Id::parse("tr_3")});
    const model::Track& t = *s.tracks.find(copy);
    CHECK(t.name == "Bass");
    // Its own plugin, with the original's settings, and the automation on it.
    REQUIRE(t.chain.size() == 1);
    const model::Id fx = t.chain[0];
    CHECK(fx != s.tracks.find(one)->chain[0]);
    CHECK(json::writeCanonical(*s.fx.find(fx)->patch) ==
          json::writeCanonical(*s.fx.find(s.tracks.find(one)->chain[0])->patch));
    int lanes = 0;
    for (const auto& e : s.envelopes)
        if (e.value->target.object == fx) {
            ++lanes;
            CHECK(e.value->points.size() == 2);
        }
    CHECK(lanes == 1);
    // Its own item and MIDI, and the send.
    std::vector<const model::Item*> items;
    for (const auto& e : s.items)
        if (e.value->track == copy) items.push_back(e.value.get());
    REQUIRE(items.size() == 1);
    const model::Take& take = *s.takes.find(*items[0]->activeTake);
    CHECK(take.source != s.takes.find(*model::Id::parse("tk_1"))->source);
    int sends = 0;
    for (const auto& e : s.routes)
        if (e.value->from.track == copy && e.value->to.track == model::Id::parse("tr_3")) {
            ++sends;
            CHECK(e.value->volumeDb == -9);
        }
    CHECK(sends == 1);
    CHECK(model::parseProject(model::serializeProject(s)));
    // One undo step; a master is refused.
    run(bus, "history.undo", "{}");
    CHECK_FALSE(app->head().tracks.find(copy));
    const auto master = bus.execute({"track.duplicate", j(R"({"tracks":["tr_0"]})"), kOrigin});
    REQUIRE_FALSE(master);
    CHECK(master.error().code == "track.strip_type");
}

TEST_CASE("render filters respect pipes and braces inside quoted replacements") {
    RenderPatternContext context;
    context.track = "A|{B},C";
    const auto result = expandRenderPattern(
        R"({track|replace("|","-")|replace("{","[")|replace("}","]")|lower})", context);
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    CHECK(*result == "a-[b],c");
    context.track = "a,b";
    CHECK(expandRenderPattern(R"({track|replace(",","|")})", context).value() == "a|b");
    auto malformed = expandRenderPattern(R"({track|replace("|,"-")})", context);
    CHECK_FALSE(malformed);
    if (!malformed) CHECK_FALSE(malformed.error().hint.empty());
}

TEST_CASE("masking errors identify the selector that needs repair") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    for (const char* field : {"masker_track", "maskee_track"}) {
        auto args = j(R"({"masker_track":"tr_0","maskee_track":"tr_0","end_beat":1})");
        args[field] = "tr_999";
        auto result = bus.execute({"analyze.masking", std::move(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "track.unknown");
        CHECK(result.error().path == std::string("/args/") + field);
        CHECK_FALSE(result.error().hint.empty());
    }
}

TEST_CASE("command repairs name the invalid fields and valid alternatives") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    for (const auto& [command, args] : std::vector<std::pair<std::string, std::string>>{
             {"midi.edit", R"({"item":"it_1","ops":[{"op":"move","notes":[0],"beats":-1}]})"},
             {"midi.edit", R"({"item":"it_1","ops":[{"op":"move","notes":[0],"semitones":127}]})"},
             {"midi.edit", R"({"item":"it_1","ops":[{"op":"cc.clear"}]})"},
             {"tempo.set",
              R"({"points":[{"beat":0,"bpm":120},{"beat":2,"bpm":120},{"beat":1,"bpm":120}]})"},
             {"render.mix", R"({"end_beat":1,"normalize":{}})"},
             {"render.mix",
              R"({"end_beat":1,"normalize":{"mode":"peak","limit_true_peak_db":"bad"}})"}}) {
        auto result = bus.execute({command, j(args), kOrigin});
        REQUIRE_FALSE(result);
        INFO(command, result.error().toString());
        CHECK_FALSE(result.error().path.empty());
        CHECK_FALSE(result.error().hint.empty());
    }
}

TEST_CASE("fx.copy declares the same usable ID hint it returns") {
    cmd::Registry registry;
    registerEngineCommands(registry);
    const auto* command = registry.find("fx.copy");
    REQUIRE(command);
    bool found = false;
    for (const auto& error : command->errors)
        if (error.code == "fx.unknown") {
            found = true;
            CHECK(error.hint == "use an fx id returned by fx.add, fx.copy or fx.chain.load");
        }
    CHECK(found);
}

TEST_CASE("MIDI tick arithmetic reports overflow at the operation field") {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    for (auto [a, b] : {std::pair{max, std::int64_t{1}},
                        {min, std::int64_t{-1}},
                        {std::int64_t{1}, max},
                        {std::int64_t{-1}, min}}) {
        auto result = detail::addMidiTicks(a, b, "/ops/0/length_delta");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "midi.range");
        CHECK(result.error().path == "/ops/0/length_delta");
        CHECK_FALSE(result.error().hint.empty());
    }
    CHECK(detail::addMidiTicks(max - 1, 1, "/ops").value() == max);
    CHECK(detail::addMidiTicks(min + 1, -1, "/ops").value() == min);
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    const auto before = model::serializeProject(app->head());
    auto bad = bus.execute(
        {"midi.edit",
         j(R"({"item":"it_1","ops":[{"op":"move","notes":[0],"beats":1},{"op":"resize","notes":[0],"length_delta":1e308}]})"),
         kOrigin});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "midi.range");
    CHECK(bad.error().path == "/args/ops/1/length_delta");
    CHECK_FALSE(bad.error().hint.empty());
    CHECK(model::serializeProject(app->head()) == before);
}

TEST_CASE("chain repairs name missing patch state and valid oversampling") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    const auto original = f.document();
    const auto before = model::serializeProject(f.app->head());
    for (const auto& [index, field] : {std::pair{0, "patch"}, {1, "state"}, {0, "oversampling"}}) {
        auto bad = original;
        auto& effect = bad["effects"].asArray()[static_cast<std::size_t>(index)];
        if (std::string_view(field) == "oversampling")
            effect[field] = 3;
        else {
            effect.asObject().erase(field);
            // Keep exactly one serialized state representation so readDocument
            // accepts the shape; prepareEffects then diagnoses the plugin's
            // specific patch/state requirement.
            if (std::string_view(field) == "patch")
                effect["state"] = "";
            else
                effect["patch"] = json::Value::object();
        }
        f.write(bad);
        auto result = f.bus->execute(
            {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","mode":"replace"})"), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "chain.invalid");
        CHECK(result.error().path == "/args/effects/" + std::to_string(index) + "/" + field);
        CHECK(result.error().hint.find(field) != std::string::npos);
        CHECK(model::serializeProject(f.app->head()) == before);
    }
}

TEST_CASE("transport range repairs point to the supplied end domain") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    for (const char* domain : {"seconds", "beat", "bar"}) {
        auto args = json::Value::object();
        args[std::string("start_") + domain] = 2;
        args[std::string("end_") + domain] = 1;
        auto result = bus.execute({"transport.loop", args, kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "transport.range");
        CHECK(result.error().path == std::string("/args/end_") + domain);
        CHECK(result.error().hint.find(std::string("end_") + domain) != std::string::npos);
    }
}

TEST_CASE("a locked item refuses every edit until it is unlocked") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    const std::string take = app->view().items.find(*model::Id::parse("it_1"))->takes[0].str();
    run(bus, "item.set", R"({"item":"it_1","locked":true})");
    const auto version = app->versions().headVersion("main");
    for (const auto& [command, args] : std::vector<std::pair<std::string, std::string>>{
             {"item.set", R"({"item":"it_1","position":2})"},
             {"item.move", R"({"item":"it_1","position":2})"},
             {"item.split", R"({"item":"it_1","at":1})"},
             {"item.remove", R"({"item":"it_1"})"},
             {"midi.edit", R"({"item":"it_1","ops":[{"op":"move","notes":[0],"semitones":12}]})"},
             {"midi.write", R"({"item":"it_1","notation":"1: D4:w"})"},
             {"take.add", R"({"item":"it_1"})"},
             {"take.activate", R"({"item":"it_1","index":0})"},
             {"take.set", R"({"take":")" + take + R"(","gain_db":-3})"}}) {
        INFO(command);
        auto result = bus.execute({command, j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "item.locked");
        CHECK(result.error().hint.find("locked: false") != std::string::npos);
    }
    CHECK(app->versions().headVersion("main") == version);  // nothing changed
    run(bus, "item.set", R"({"item":"it_1","locked":false})");
    run(bus, "item.move", R"({"item":"it_1","position":2})");
}

TEST_CASE("MIDI query values read back land on the same ticks at a fast playrate") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", "{}");
    run(bus, "item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    run(bus, "take.set", R"({"take":"tk_1","playrate":100})");
    run(bus, "midi.edit",
        R"({"item":"it_1","ops":[{"op":"resize","notes":[0],"length":0.00015625}]})");
    const auto midi = (*app->head().midi.begin()).id;
    const auto before = app->head().midi.find(midi)->sequence.notes.at(0).length;
    REQUIRE(before == 15);  // 15 ticks at ppq 960 and playrate 100
    auto shown = run(bus, "midi.notes", R"({"item":"it_1"})");
    auto args = j(R"({"item":"it_1","ops":[{"op":"resize","notes":[0],"length":1}]})");
    args["ops"].asArray()[0]["length"] = *shown["notes"].at(0).find("length");
    REQUIRE(bus.execute({"midi.edit", std::move(args), kOrigin}));
    CHECK(app->head().midi.find(midi)->sequence.notes.at(0).length == before);
}

TEST_CASE("plugin.scan in the background returns at once and installs the catalogue when done") {
    namespace fs = std::filesystem;
    Workdir dir;
    host::setBridgeExecutable(fs::path(FIXTURE_CLAP_PATH).parent_path() /
                              (std::string(product::kSlug) + "-bridge"));
    AppOptions options;
    options.workdir = dir.path;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto opened = EngineApp::create(options);
    REQUIRE(opened);
    auto& app = **opened;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, app);
    const auto before = run(bus, "plugin.list", "{}")["plugins"].size();
    REQUIRE(before > 0);
    auto started = run(bus, "plugin.scan", R"({"wait": false})");
    REQUIRE(started.find("job"));
    const std::string job = started["job"].asString();
    CHECK(started["plugins"].size() == before);  // the current list, at once
    auto again = run(bus, "plugin.scan", R"({"wait": false})");
    // A cached scan may finish before the second request starts. In that case
    // the second request legitimately creates a new job; settle both jobs.
    for (const auto& id : {job, again["job"].asString()}) {
        std::string status;
        for (int i = 0; i < 2000 && status != "done" && status != "failed"; ++i) {
            bus.flushJobEvents();  // the worker posts the catalogue to this thread
            status = run(bus, "job.status", R"({"job": ")" + id + R"("})")["status"].asString();
            if (status == "running") std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(status == "done");
    }
    CHECK(run(bus, "plugin.list", "{}")["plugins"].size() == before);
}

TEST_CASE("MIDI sends: commands validate endpoints, controls, cycles and mute-only learn") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Source"})");
    run(bus, "track.add", R"({"name":"Target"})");
    run(bus, "track.add", R"({"name":"Buss","type":"buss"})");
    const auto rack = run(bus, "rack.add", R"({"track":"tr_2"})")["rack"].asString();
    const auto notes =
        run(bus, "module.add",
            "{\"rack\":\"" + rack + "\",\"type\":\"notes\",\"source\":\"none\"}")["module"]
            .asString();
    const auto output =
        run(bus, "module.add", "{\"rack\":\"" + rack + "\",\"type\":\"notes_out\"}")["module"]
            .asString();
    auto send =
        run(bus, "route.add", R"({"from":{"track":"tr_1"},"to":{"track":"tr_2"},"midi":{}})");
    const auto route = send["id"].asString();
    auto refused = [&](const char* command, json::Value args, const char* code) {
        const auto version = app->head().version;
        const auto result = bus.execute({command, std::move(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().hint.empty());
        CHECK(app->head().version == version);
    };
    for (const char* key : {"volume_db", "pan", "mono", "phase_invert", "channels"}) {
        auto args = j(R"({"from":"tr_1","to":"tr_2","midi":{}})");
        args[key] = std::string_view(key) == "channels" ? j(R"({"from":[1,2],"to":[1,2]})")
                    : std::string_view(key) == "mono" || std::string_view(key) == "phase_invert"
                        ? json::Value(false)
                        : json::Value(0);
        refused("route.add", std::move(args), "route.midi");
    }
    refused("route.add", j(R"({"from":"tr_3","to":"tr_2","midi":{}})"), "route.endpoint");
    refused("route.add", j(R"({"from":"tr_1","to":"tr_0","midi":{}})"), "route.endpoint");
    refused("route.add", j(R"({"from":"tr_2","to":"tr_1","midi":{}})"), "route.cycle");
    auto learn = j(R"({"param":"volume","midi":{"kind":"cc","channel":1,"number":7}})");
    learn["route"] = route;
    refused("route.learn.set", learn, "route.midi");
    learn["param"] = "mute";
    REQUIRE(bus.execute({"route.learn.set", learn, kOrigin}));
    auto direct = j(R"({"from":"tr_1","midi":{}})");
    direct["to"] = notes;
    REQUIRE(bus.execute({"route.add", direct, kOrigin}));
    direct["from"] = output;
    direct["to"] = "tr_1";
    refused("route.add", direct, "route.cycle");  // Target already follows Source.
    auto set = json::Value::object();
    set["module"] = notes;
    set["source"] = json::Value();
    REQUIRE(bus.execute({"module.set", set, kOrigin}));
    CHECK_FALSE(app->head().modules.find(*model::Id::parse(notes))->sourceOff);
    // Notes reads Target again; its own rack's Note Out may play Target.
    direct["to"] = "tr_2";
    const auto own = bus.execute({"route.add", direct, kOrigin});
    REQUIRE_MESSAGE(own, (own ? "" : own.error().toString()));
    const auto cycles = run(bus, "route.matrix", R"({"mode":"midi"})");
    for (const auto& cycle : cycles.find("cycles")->asArray())
        CHECK_FALSE(
            (cycle.find("from")->asString() == output && cycle.find("to")->asString() == "tr_2"));
    set["source"] = "none";
    REQUIRE(bus.execute({"module.set", set, kOrigin}));
    CHECK(app->head().modules.find(*model::Id::parse(notes))->sourceOff);
    auto old = model::parseProject(model::serializeProject(app->head()));
    REQUIRE(old);
    CHECK(old->modules.find(*model::Id::parse(notes))->sourceOff);
    const auto matrix = run(bus, "route.matrix", R"({"mode":"midi"})");
    CHECK(matrix.find("sources")->size() == 3);       // two channels and Note Out
    CHECK(matrix.find("destinations")->size() == 3);  // two channels and Notes
    // source_sends: false, and Notes no longer hears Source's send into
    // Target, so its Note Out may play Source (once Source's direct send to
    // Notes is gone).
    set["source"] = json::Value();
    REQUIRE(bus.execute({"module.set", set, kOrigin}));
    auto remove = json::Value::object();
    for (const auto& r : app->head().routes)
        if (r.value->to.module == model::Id::parse(notes)) remove["route"] = r.id.str();
    REQUIRE(remove.find("route"));
    REQUIRE(bus.execute({"route.remove", remove, kOrigin}));
    direct["to"] = "tr_1";
    refused("route.add", direct, "route.cycle");
    auto quiet = json::Value::object();
    quiet["module"] = notes;
    quiet["source_sends"] = false;
    REQUIRE(bus.execute({"module.set", quiet, kOrigin}));
    CHECK_FALSE(app->head().modules.find(*model::Id::parse(notes))->sourceSends);
    REQUIRE(bus.execute({"route.add", direct, kOrigin}));
    bool listed = false;
    const auto got = run(bus, "rack.get", "{\"rack\":\"" + rack + "\"}");
    for (const auto& m : got.find("modules")->asArray())
        if (m.find("module")->asString() == notes)
            listed = m.find("source_sends") && *m.find("source_sends") == json::Value(false);
    CHECK(listed);
    quiet["source_sends"] = true;
    refused("module.set", quiet, "route.cycle");
    quiet["module"] = output;
    refused("module.set", quiet, "rack.param");
}

TEST_CASE("MIDI sends: Notes source None does not prevent branch id remapping") {
    cmd::Registry registry;
    registerEngineCommands(registry);
    const std::map<model::Id, model::Id> remap{
        {*model::Id::parse("tr_1"), *model::Id::parse("tr_7")}};
    for (const char* name : {"module.add", "module.set"}) {
        const auto* command = registry.find(name);
        REQUIRE(command);
        auto args = j(R"({"source":"tr_1"})");
        CHECK(model::remapIds(args, command->argsSchema, remap) == j(R"({"source":"tr_7"})"));
        args["source"] = "none";
        CHECK(model::remapIds(args, command->argsSchema, remap) == args);
    }
}
