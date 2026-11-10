// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/base/sha256.h>
#include <supple/engine/app.h>
#include <supple/engine/jack.h>
#include <supple/host/clap_host.h>
#include <supple/model/schemas.h>

#include <supple/analysis/analysis.h>
#include <atomic>
#include "fake_jack.h"
#include "history_sequence.h"
#include "recording.h"
#include "render_commands.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;

struct Root {
    fs::path path = fs::temp_directory_path() / ("supple-engine-app-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::unique_ptr<EngineApp> makeApp(const fs::path& dir, bool player = false) {
    AppOptions o;
    o.workdir = dir;
    o.chainDir = dir / "chains";  // never the user's config
    o.presetDir = dir / "presets";
    o.templateDir = dir / "templates";
    o.projectTemplateDir = dir / "project-templates";
    o.pluginPath = {kFixture};
    if (player) o.pluginPath.push_back(fs::path(STOCK_CLAP_DIR) / "Script Player.clap");
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    return std::move(*app);
}

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

}  // namespace

TEST_CASE("seeded command histories preserve invariants, undo and saved projects") {
    Root root;
    for (std::uint32_t seed : {1U, 7U, 42U, 2026U}) {
        cmd::Registry registry;
        registerEngineCommands(registry);
        auto app = makeApp(root.path / std::to_string(seed));
        cmd::Dispatcher bus(registry, *app);
        std::vector<std::uint8_t> stream;
        std::uint32_t state = seed;
        for (int i = 0; i < 64; ++i) {
            state = state * 1664525U + 1013904223U;
            stream.push_back(static_cast<std::uint8_t>(state >> 24));
        }
        // Ensure every command family is exercised even when a seeded stream
        // happens to miss it. Vary the high bits to select existing or bad IDs.
        for (int op = 0; op < 19; ++op) stream.push_back(static_cast<std::uint8_t>(op));
        auto result = test::historySequence(*app, bus, stream);
        INFO("seed: " << seed);
        INFO((result ? std::string() : result.error().toString()));
        CHECK(result);
    }
}

TEST_CASE("every engine command example runs against its fixture") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    int n = 0;
    // The dsp_player fixtures' examples need the script player built, and
    // dsp_player_editor's its editor, which builds without a GUI lack.
    auto st = cmd::runExamples(
        registry,
        [&](std::string_view fixture) -> std::unique_ptr<cmd::Host> {
            return makeApp(root.path / std::to_string(n++), fixture.starts_with("dsp_player"));
        },
        [](std::string_view fixture) { return SUPPLE_EDITORS || fixture != "dsp_player_editor"; });
    INFO((st ? std::string() : st.error().toString()));
    CHECK(st);
    CHECK(n >= 25);
}

TEST_CASE("an LLM builds a song over the bus: batch, render, undo, save and reopen") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    auto song = call("batch", R"({
      "label": "bass part",
      "steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "bass"},
        {"command": "fx.add", "args": {"track": "${bass.track}", "plugin": "Test Sine"}},
        {"command": "fx.add", "args": {"track": "${bass.track}", "plugin": "com.suppleaudio.test.gain",
                                        "patch": {"params": {"gain": "-6 dB"}}}, "as": "gain"},
        {"command": "item.add", "args": {"track": "${bass.track}", "bar": 1,
                                          "notation": "voice bass; 1: A2:q A2:q E3:q A2:q | D3:h A2:h"}},
        {"command": "session.describe", "args": {}}
      ]})");
    REQUIRE_MESSAGE(
        song,
        (song ? std::string()
              : song.error().toString() + " / " +
                    (song.error().details.empty() ? "" : song.error().details[0].toString())));
    const std::string described = (*song)["results"].at(4).find("text")->asString();
    CHECK(described.find("tr_1 Bass") != std::string::npos);
    CHECK(described.find("it_1 bar 1–bar 3") != std::string::npos);
    CHECK(app->versions().history(10).size() == 2);  // one batch = one version

    auto mix = call("render.mix", R"({"name": "take1"})");
    REQUIRE_MESSAGE(mix, (mix ? std::string() : mix.error().toString()));
    CHECK((*mix)["seconds"].asDouble() == doctest::Approx(4.0));  // two bars at 120 BPM
    // velocity 96 sine (0.25 * 96/127), centred (-3 dB), gain -6 dB.
    CHECK((*mix)["peak_dbfs"].asDouble() ==
          doctest::Approx(20 * std::log10(0.25 * 96 / 127) - 6).epsilon(0.01));
    CHECK(fs::exists((*mix)["path"].asString()));

    auto inlineMix = call("render.mix", R"({"name":"analyzed","analyze":"summary"})");
    REQUIRE(inlineMix);
    CHECK((*inlineMix)["analysis"]["schema"].asString() == "supple.analysis-report/1.0");
    CHECK((*inlineMix)["analysis"]["summary"].isString());
    auto byId = call("analyze.render", R"({"render":"rd_1"})");
    REQUIRE(byId);
    CHECK((*byId)["schema"].asString() == "supple.analysis-report/1.0");
    auto byPath =
        call("analyze.render", json::writeCompact(j(std::string("{\"render\":") +
                                                    json::writeCompact((*mix)["path"]) + "}")));
    REQUIRE(byPath);
    CHECK((*byPath)["loudness"] == (*byId)["loudness"]);
    REQUIRE(call("track.set", R"({"track":"tr_1","volume_db":-6})"));
    auto trackPost = call("analyze.track", R"({"track":"tr_1","tap":"post_fader"})");
    auto trackPre = call("analyze.track", R"({"track":"tr_1","tap":"pre_fader"})");
    REQUIRE(trackPost);
    REQUIRE(trackPre);
    CHECK((*trackPost)["schema"].asString() == "supple.analysis-report/1.0");
    CHECK((*trackPost)["loudness"]["integrated_lufs"].asDouble() <
          (*trackPre)["loudness"]["integrated_lufs"].asDouble() - 5.5);
    auto compared = call("analyze.compare", R"({"reference":"rd_1","test":"rd_1"})");
    REQUIRE(compared);
    CHECK((*compared)["notable"].size() == 0);
    CHECK((*compared)["ltas_rms_db"].asDouble() == doctest::Approx(0));
    auto masked = call("analyze.masking", R"({"masker_track":"tr_1","maskee_track":"tr_1"})");
    REQUIRE(masked);
    CHECK(masked->isObject());
    auto unknown = call("analyze.render", R"({"render":"rd_999"})");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "engine.unknown_render");

    // An invalid step rolls back the whole batch.
    auto bad = call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Keys"}},
        {"command": "fx.param.set", "args": {"fx": "fx_2", "values": {"gian": 1}}}]})");
    REQUIRE(!bad);
    CHECK(bad.error().details[0].path == "/steps/1/args/values/gian");
    CHECK(app->head().tracks.size() == 2);

    // Parameter edits through the model, undo/redo.
    REQUIRE(call("fx.param.set", R"({"fx": "fx_2", "values": {"gain": -12}})"));
    CHECK(app->head()
              .fx.find(*model::Id::parse("fx_2"))
              ->patch->find("params")
              ->find("gain")
              ->asDouble() == -12.0);
    REQUIRE(call("history.undo", "{}"));
    CHECK(app->head()
              .fx.find(*model::Id::parse("fx_2"))
              ->patch->find("params")
              ->find("gain")
              ->asDouble() == -6.0);
    REQUIRE(call("history.redo", "{}"));

    auto notes = call("midi.get", R"({"item": "it_1"})");
    REQUIRE(notes);
    CHECK((*notes)["text"].asString().find("A2") != std::string::npos);

    REQUIRE(call("project.save", R"({"path": "song.json"})"));
    auto app2 = makeApp(root.path);
    cmd::Dispatcher d2(registry, *app2);
    REQUIRE(d2.execute({"project.open", j(R"({"path": "song.json"})"), kTest}));
    CHECK(model::serializeProject(app2->head()) == model::serializeProject(app->head()));
    auto errs = d2.execute({"track.set", j(R"({"track": "tr_9", "mute": true})"), kTest});
    REQUIRE(!errs);
    CHECK(errs.error().path == "/args/track");
    CHECK(errs.error().hint.find("tr_1") != std::string::npos);
}

TEST_CASE("track analysis excludes unrelated tracks and validates ranges") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string name, std::string_view args) {
        return d.execute({std::move(name), j(args), kTest});
    };
    REQUIRE(call("track.add", R"({"name":"A"})"));
    REQUIRE(call("fx.add", R"({"track":"tr_1","plugin":"Test Sine"})"));
    REQUIRE(
        call("item.add", R"({"track":"tr_1","bar":1,"notation":"voice a; 1: A2:q r:q A2:q r:q"})"));
    auto before = call("analyze.track", R"({"track":"tr_1"})");
    REQUIRE(before);
    REQUIRE(call("track.add", R"({"name":"B"})"));
    REQUIRE(call("fx.add", R"({"track":"tr_2","plugin":"Test Sine"})"));
    REQUIRE(
        call("item.add", R"({"track":"tr_2","bar":1,"notation":"voice b; 1: C4:q r:q C4:q r:q"})"));
    auto after = call("analyze.track", R"({"track":"tr_1"})");
    REQUIRE(after);
    CHECK((*before)["loudness"] == (*after)["loudness"]);
    auto masking = call("analyze.masking", R"({"masker_track":"tr_1","maskee_track":"tr_2"})");
    REQUIRE(masking);
    CHECK(masking->isObject());
    auto empty = call("analyze.track", R"({"track":"tr_1","start_beat":2,"end_beat":1})");
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "engine.range");
}

TEST_CASE("agent branches: parallel edits merge by replay; conflicts are reported") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        return r;
    };
    REQUIRE(call("track.add", R"({"name": "Drums"})"));
    REQUIRE(call("branch.create", R"({"name": "agent/claude/bass"})"));
    // On the branch: a bass track (tr_2 here) and a level change on it.
    REQUIRE(call("track.add", R"({"name": "Bass"})"));
    REQUIRE(call("track.set", R"({"track": "tr_2", "volume_db": -4})"));
    // Meanwhile on main: a keys track (also tr_2 there) and a drums tweak.
    REQUIRE(call("branch.switch", R"({"name": "main"})"));
    REQUIRE(call("track.add", R"({"name": "Keys"})"));
    REQUIRE(call("track.set", R"({"track": "tr_1", "mute": true})"));
    auto diff = call("branch.diff", R"({"a": "main", "b": "agent/claude/bass"})");
    REQUIRE(diff);
    CHECK((*diff)["changes"].size() >= 2);

    auto merged = call("branch.merge", R"({"from": "agent/claude/bass"})");
    REQUIRE_MESSAGE(merged, (merged ? std::string() : merged.error().toString()));
    CHECK_FALSE((*merged)["fast_forward"].asBool());
    CHECK((*merged)["remapped"]["tr_2"].asString() == "tr_3");
    const auto& s = app->head();
    CHECK(s.tracks.find(*model::Id::parse("tr_2"))->name == "Keys");
    CHECK(s.tracks.find(*model::Id::parse("tr_3"))->name == "Bass");
    CHECK(s.tracks.find(*model::Id::parse("tr_3"))->volumeDb == -4.0);  // followed the remap
    CHECK(s.tracks.find(*model::Id::parse("tr_1"))->mute);

    // A conflict: both sides set the drums level.
    REQUIRE(call("branch.create", R"({"name": "agent/claude/drums"})"));
    REQUIRE(call("track.set", R"({"track": "tr_1", "volume_db": -2})"));
    REQUIRE(call("branch.switch", R"({"name": "main"})"));
    REQUIRE(call("track.set", R"({"track": "tr_1", "volume_db": -6})"));
    const auto before = app->versions().headVersion("main");
    auto conflict = call("branch.merge", R"({"from": "agent/claude/drums"})");
    REQUIRE(!conflict);
    CHECK(conflict.error().code == "branch.conflict");
    REQUIRE(conflict.error().details.size() == 1);
    CHECK(conflict.error().details[0].code == "merge.concurrent_edit");
    CHECK(conflict.error().details[0].message.find("volume_db") != std::string::npos);
    CHECK(app->versions().headVersion("main") == before);

    // Only ID-typed arguments follow a remap: a track *named* "tr_4" keeps its name.
    REQUIRE(call("branch.create", R"({"name": "agent/claude/names"})"));
    REQUIRE(call("track.add", R"({"name": "Pad"})"));  // tr_4 on the branch
    REQUIRE(call("track.set", R"({"track": "tr_4", "name": "tr_4", "notes": "copy of tr_1"})"));
    REQUIRE(call("branch.switch", R"({"name": "main"})"));
    REQUIRE(call("track.add", R"({"name": "Lead"})"));  // tr_4 on main
    auto names = call("branch.merge", R"({"from": "agent/claude/names"})");
    REQUIRE_MESSAGE(names, (names ? std::string() : names.error().toString()));
    CHECK((*names)["remapped"]["tr_4"].asString() == "tr_5");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_4"))->name == "Lead");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_5"))->name == "tr_4");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_5"))->notes == "copy of tr_1");
}

TEST_CASE("an LLM plays the song in real time over the bus (a stand-in libjack)") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    AppOptions o;
    o.workdir = root.path;
    o.pluginPath = {kFixture};
    o.jackLibrary = FakeJack::kPath;
    auto created = EngineApp::create(o, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "bass"},
        {"command": "fx.add", "args": {"track": "${bass.track}", "plugin": "Test Sine"}},
        {"command": "item.add", "args": {"track": "${bass.track}", "bar": 1,
                                          "notation": "voice bass; 1: A2:w | D3:w"}}]})");

    auto status = call("engine.start", "{}");  // jack by default
    CHECK(status["backend"].asString() == "jack");
    CHECK(status["period"].asInt() == 1024);
    CHECK(status["device_latency"].asInt() == 512);
    CHECK(call("transport.play", "{}")["playing"].asBool());
    for (int i = 0; i < 5000 && fake.outputPeak("") <= 0.01f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(fake.outputPeak("") > 0.01f);
    CHECK(call("transport.get", "{}")["sample"].asInt() > 0);

    call("track.set", R"({"track": "tr_1", "volume_db": -6})");  // a new plan while playing
    fake.xrun();
    status = call("engine.status", "{}");
    CHECK(status["xruns"].asInt() == 1);
    CHECK(status["process_errors"].asInt() == 0);
    CHECK_FALSE(status["restart_needed"].asBool());

    auto t = call("transport.stop", "{}");  // back to where play started
    CHECK_FALSE(t["playing"].asBool());
    CHECK(t["sample"].asInt() == 0);
    call("engine.stop", "{}");
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    CHECK(fake.open() == 0);

    // A device at another rate than the project: the engine runs at the device's.
    fake.setRate(44100);
    auto other = call("engine.start", "{}");
    CHECK(other["sample_rate"].asDouble() == 44100.0);
    CHECK(std::string(fake.rateEnv()).empty());  // no rate given: nothing asked of the graph
    call("engine.stop", "{}");
    CHECK(fake.open() == 0);

    // A rate given is asked of the graph; force_graph holds it and the block.
    call("engine.start", R"({"rate": 96000, "block": 128, "force_graph": true})");
    CHECK(std::string(fake.rateEnv()) == "1/96000");
    CHECK(std::string(fake.propsEnv()) == "{ node.force-quantum=128 node.force-rate=96000 }");
    call("engine.stop", "{}");
    fake.reset();
}

TEST_CASE("an isolated plugin that crashes is reported and restarted over the bus") {
    host::setBridgeExecutable(BRIDGE_PATH);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Lead"}, "as": "t"},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "com.suppleaudio.test.crash"}, "as": "c"},
        {"command": "fx.set", "args": {"fx": "${c.fx}", "isolation": "dedicated"}},
        {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice lead; 1: C4:w"}}]})");
    const model::Id crashFx = *model::Id::parse("fx_2");
    call("engine.start", R"({"backend": "dummy"})");
    call("transport.play", "{}");
    host::Instance* crash = app->instanceOf(crashFx);
    REQUIRE(crash);
    CHECK(crash->isolated());
    REQUIRE(crash->setParam(1, 1.0));  // crash in its next process()
    for (int i = 0; i < 2000 && !crash->failed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto status = call("engine.status", "{}");
    REQUIRE(status["failed_fx"].size() == 1);
    CHECK(status["failed_fx"].at(0).asString() == "fx_2");
    REQUIRE(status["failed_errors"].size() == 1);
    const auto& failure = status["failed_errors"].at(0);
    CHECK(failure.find("fx")->asString() == "fx_2");
    CHECK(failure.find("error")->find("code")->asString() == "engine.bridge_failed");
    CHECK(failure.find("reason")->asString() == failure.find("error")->find("message")->asString());
    CHECK(status["running"].asBool());  // the engine plays on
    // Its description still answers, with the unreadable status as a problem.
    auto described = call("fx.describe", R"({"fx": "fx_2"})");
    CHECK_FALSE(described["status_available"].asBool());
    REQUIRE(described["problems"].size() == 1);
    CHECK(described["problems"].at(0).find("scope")->asString() == "host");
    bool reported = false;
    for (const auto& [topic, payload] : app->takeEvents())
        if (topic == "fx.failed") {
            reported = payload.find("fx")->asString() == "fx_2";
            REQUIRE(payload.find("error"));
            CHECK(payload.find("error")->find("code")->asString() == "engine.bridge_failed");
        }
    CHECK(reported);
    app->tick();
    for (const auto& event : app->takeEvents()) CHECK(event.first != "fx.failed");
    call("fx.restart", R"({"fx": "fx_2"})");
    CHECK(call("engine.status", "{}")["failed_fx"].size() == 0);
    host::Instance* again = app->instanceOf(crashFx);
    REQUIRE(again);
    CHECK(again != crash);
    CHECK_FALSE(again->failed());
    call("engine.stop", "{}");
}

TEST_CASE("shared bridge crash reports both fx and both restart in one new process") {
    host::setBridgeExecutable(BRIDGE_PATH);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher dispatcher(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto result = dispatcher.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    };
    call("batch", R"({"steps": [
        {"command":"track.add","args":{"name":"Shared"},"as":"t"},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"Test Sine"}},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"com.suppleaudio.test.gain"},"as":"a"},
        {"command":"fx.set","args":{"fx":"${a.fx}","isolation":"shared_bridge"}},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"com.suppleaudio.test.crash"},"as":"b"},
        {"command":"fx.set","args":{"fx":"${b.fx}","isolation":"shared_bridge"}},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"com.suppleaudio.test.gain"},"as":"d"},
        {"command":"fx.set","args":{"fx":"${d.fx}","isolation":"dedicated"}},
        {"command":"item.add","args":{"track":"${t.track}","bar":1,"notation":"voice lead; 1: C4:w"}}]})");
    const auto firstId = *model::Id::parse("fx_2");
    const auto crashId = *model::Id::parse("fx_3");
    const auto dedicatedId = *model::Id::parse("fx_4");
    call("engine.start", R"({"backend":"dummy"})");
    call("transport.play", "{}");
    auto* first = app->instanceOf(firstId);
    auto* crash = app->instanceOf(crashId);
    auto* dedicated = app->instanceOf(dedicatedId);
    REQUIRE(first);
    REQUIRE(crash);
    REQUIRE(dedicated);
    const int oldPid = first->bridgePid();
    CHECK(oldPid > 0);
    CHECK(crash->bridgePid() == oldPid);
    CHECK(dedicated->bridgePid() != oldPid);
    REQUIRE(crash->setParam(1, 1.0));
    for (int i = 0; i < 2000 && !crash->failed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(crash->failed());
    CHECK(first->failed());
    CHECK_FALSE(dedicated->failed());
    auto status = call("engine.status", "{}");
    CHECK(status["running"].asBool());
    REQUIRE(status["failed_fx"].size() == 2);
    std::set<std::string> reported;
    for (const auto& [topic, payload] : app->takeEvents())
        if (topic == "fx.failed") reported.insert(payload.find("fx")->asString());
    CHECK((reported == std::set<std::string>{"fx_2", "fx_3"}));
    call("fx.restart", R"({"fx":"fx_2"})");
    call("fx.restart", R"({"fx":"fx_3"})");
    first = app->instanceOf(firstId);
    crash = app->instanceOf(crashId);
    REQUIRE(first);
    REQUIRE(crash);
    CHECK_FALSE(first->failed());
    CHECK_FALSE(crash->failed());
    CHECK(first->bridgePid() > 0);
    CHECK(first->bridgePid() == crash->bridgePid());
    CHECK(first->bridgePid() != oldPid);
    CHECK(dedicated->bridgePid() != first->bridgePid());
    CHECK(call("engine.status", "{}")["failed_fx"].size() == 0);
    call("engine.stop", "{}");
}

TEST_CASE("an LLM automates a plugin parameter over the bus and hears it in the render") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    auto made = call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Pad"}, "as": "t"},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "com.suppleaudio.test.gain"}, "as": "g"},
        {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice pad; 1: C4:w"}},
        {"command": "env.add", "args": {"fx": "${g.fx}", "param": "gain",
            "points": [{"beat": 0, "value": "0 dB", "shape": "square"}, {"beat": 2, "value": "-60 dB", "shape": "square"}]}}]})");
    REQUIRE_MESSAGE(made, (made ? std::string() : made.error().toString()));
    auto mix = call("render.mix", R"({"name": "automated", "end_beat": 4})");
    REQUIRE_MESSAGE(mix, (mix ? std::string() : mix.error().toString()));
    auto audio = audio::readAudio((*mix)["path"].asString());
    REQUIRE(audio);
    const auto& l = audio->channels[0];
    double before = 0, after = 0;
    for (std::size_t i = 40000; i < 48000; ++i) before = std::max(before, std::fabs(double(l[i])));
    for (std::size_t i = 48000; i < 90000; ++i) after = std::max(after, std::fabs(double(l[i])));
    CHECK(before > 0.05);
    CHECK(after == 0.0);  // silent from beat 2 (sample 48000), off at min

    auto listed = call("env.list", "{}");
    REQUIRE(listed);
    CHECK((*listed)["envelopes"].size() == 1);
    CHECK((*listed)["envelopes"].at(0).find("visible")->asBool());
    // Hiding the lane is UI state in the project; the envelope still plays.
    REQUIRE(call("env.set", R"({"env": "en_1", "visible": false})"));
    CHECK_FALSE(app->head().envelopes.find(*model::Id::parse("en_1"))->visible);
    auto hidden = call("env.list", "{}");
    REQUIRE(hidden);
    CHECK_FALSE((*hidden)["envelopes"].at(0).find("visible")->asBool());
    REQUIRE(call("env.set", R"({"env": "en_1", "visible": true})"));
    REQUIRE(call("history.undo", "{}"));  // both visibility edits, so the undo below
    REQUIRE(call("history.undo", "{}"));  // still takes the batch
    // Errors say what to do.
    auto twice = call("env.add", R"({"fx": "fx_2", "param": "gain"})");
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == "env.exists");
    auto typo = call("env.add", R"({"fx": "fx_2", "param": "gian"})");
    REQUIRE_FALSE(typo);
    CHECK(typo.error().code == "env.param");
    CHECK(typo.error().hint.find("gain") != std::string::npos);
    auto both = call("env.add", R"({"fx": "fx_2", "track": "tr_1", "param": "gain"})");
    REQUIRE_FALSE(both);
    CHECK(both.error().code == "env.target");
    auto badValue =
        call("env.points.set", R"({"env": "en_1", "points": [{"beat": 0, "value": "loud"}]})");
    REQUIRE_FALSE(badValue);
    CHECK(badValue.error().path == "/args/points/0/value");
    // Undo removes the envelope with the batch.
    REQUIRE(call("history.undo", "{}"));
    CHECK(app->head().envelopes.size() == 0);
}

TEST_CASE("an LLM records a take over the bus, hears it back and undoes it") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "v"},
        {"command": "track.set", "args": {"track": "${v.track}", "armed": true, "record_input": [1, 2]}}]})");
    // A device whose inputs carry a 0.5 sine at 441 Hz.
    DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                          .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                      std::int64_t frame) {
                              for (std::uint32_t c = 0; c < channels; ++c)
                                  for (std::uint32_t i = 0; i < n; ++i)
                                      in[c][i] =
                                          0.5f * static_cast<float>(std::sin(
                                                     2.0 * 3.14159265358979 * 441.0 *
                                                     static_cast<double>(frame + i) / 48000.0));
                          }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    auto started = call("transport.record", "{}");
    CHECK(started["recording"].asBool());
    backend.run(1);  // apply the queued transport command at the callback
    CHECK(call("transport.get", "{}")["playing"].asBool());
    backend.run(74);
    auto stopped = call("transport.stop", "{}");
    CHECK_FALSE(stopped["recording"].asBool());
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value& take = stopped["recorded"].at(0);
    CHECK(take.find("track")->asString() == "tr_1");
    CHECK(take.find("seconds")->asDouble() == doctest::Approx(0.4));
    CHECK(take.find("lost_frames")->asInt() == 0);
    CHECK(fs::exists(take.find("file")->asString()));
    CHECK(app->head().items.size() == 1);
    REQUIRE(app->head().media.size() == 1);  // hash completion covered below
    app->stopEngine();

    // The take plays back in a render: the sine, centred (-3 dB).
    auto mix = call("render.mix", R"({"name": "take", "end_beat": 0.5})");
    auto audio = audio::readAudio(mix["path"].asString());
    REQUIRE(audio);
    double peak = 0;
    for (float x : audio->channels[0]) peak = std::max(peak, std::fabs(double(x)));
    CHECK(peak == doctest::Approx(0.5).epsilon(0.01));

    // One version: undo removes the take.
    call("history.undo", "{}");
    CHECK(app->head().items.size() == 0);
}

TEST_CASE("an LLM prints the mix onto the master over the bus (ADR-0057)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    auto ok = [&](std::string command, std::string_view args) {
        auto r = call(std::move(command), args);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    ok("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Pad"}, "as": "t"},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
        {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice pad; 1: C4:w"}},
        {"command": "track.set", "args": {"track": "tr_0", "armed": true}}]})");
    // A master records its output; inputs, monitoring and MIDI stay on channels.
    for (auto [args, path] : {std::pair{R"({"track": "tr_0", "monitor": "on"})", "/track"},
                              std::pair{R"({"track": "tr_0", "record_input": [1]})", "/track"},
                              std::pair{R"({"track": "tr_1", "lane_mute": true})", "/lane_mute"}}) {
        auto refused = call("track.set", args);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "track.strip_type");
        CHECK(refused.error().path == std::string("/args") + path);
        CHECK_FALSE(refused.error().hint.empty());
    }
    auto midi = call("item.add", R"({"track": "tr_0", "bar": 1, "notation": "voice x; 1: C4:w"})");
    REQUIRE_FALSE(midi);
    CHECK(midi.error().code == "track.strip_type");
    CHECK(midi.error().path == "/args/notation");
    json::Value listed = ok("track.list", "{}");
    CHECK(listed["tracks"].at(0).find("armed")->asBool());
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256}, {.anticipative = false})
            .ok());
    CHECK(ok("transport.record", "{}")["recording"].asBool());
    backend.run(75);
    json::Value stopped = ok("transport.stop", "{}");
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value& take = stopped["recorded"].at(0);
    CHECK(take.find("track")->asString() == "tr_0");
    CHECK(take.find("lost_frames")->asInt() == 0);
    const model::Item* print = nullptr;
    for (const auto& e : app->head().items)
        if (e.value->track == model::kMaster) print = e.value.get();
    REQUIRE(print);
    app->stopEngine();
    // The print plays beside the live mix; the lane mute silences it alone.
    auto peakOf = [&](std::string_view args) {
        auto audio = audio::readAudio(ok("render.mix", args)["path"].asString());
        REQUIRE(audio);
        double peak = 0;
        for (std::size_t i = 4800; i < 14400; ++i)
            peak = std::max(peak, std::fabs(double(audio->channels[0][i])));
        return peak;
    };
    const double both = peakOf(R"({"name": "both", "end_beat": 1})");
    ok("track.set", R"({"track": "tr_0", "lane_mute": true})");
    const double live = peakOf(R"({"name": "live", "end_beat": 1})");
    CHECK(live > 0.05);
    CHECK(both == doctest::Approx(2 * live).epsilon(0.01));  // the print doubles the mix
    ok("history.undo", "{}");                                // the lane mute
    ok("history.undo", "{}");                                // the take
    for (const auto& e : app->head().items) CHECK(e.value->track != model::kMaster);
}

TEST_CASE("recording commits before hashing and completes without an undo step") {
    Root root;
    std::atomic<bool> entered{false}, release{false};
    std::atomic<int> completed{0}, startedJobs{0};
    std::atomic<bool> holdFinalizer{false}, finalizeRelease{false};
    AppOptions options;
    options.workdir = root.path;
    options.scratchDir = root.path / "scratch";
    options.pluginPath = {kFixture};
    options.jobStartedHook = [&] {
        const int number = startedJobs.fetch_add(1) + 1;
        startedJobs.notify_all();
        if (number == 1) {
            entered.store(true);
            entered.notify_one();
            release.wait(false);
        } else if (holdFinalizer.load()) {
            finalizeRelease.wait(false);
        }
    };
    options.jobFinishedHook = [&] {
        completed.fetch_add(1);
        completed.notify_all();
    };
    auto waitCompleted = [&](int count) {
        for (int value = completed.load(); value < count; value = completed.load())
            completed.wait(value);
    };
    // Release the barrier even if an assertion unwinds before the job is released.
    struct Release {
        std::atomic<bool>& flag;
        std::atomic<bool>& finalizer;
        ~Release() {
            flag.store(true);
            flag.notify_all();
            finalizer.store(true);
            finalizer.notify_all();
        }
    };
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    auto created = EngineApp::create(std::move(options), {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *app);
    Release unblock{release, finalizeRelease};
    auto call = [&](std::string name, json::Value args = json::Value::object()) {
        auto result = d.execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    };
    call("track.add", j(R"({"name":"Vocal"})"));
    call("track.set", j(R"({"track":"tr_1","armed":true,"record_input":[1]})"));
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 1},
                             {.anticipative = false}));
    const auto before = app->versions().history().size();
    call("transport.record");
    backend.run(8);
    auto stopped = call("transport.stop");  // returns while worker is held
    entered.wait(false);
    REQUIRE(stopped["recorded"].size() == 1);
    REQUIRE(app->head().items.size() == 1);
    REQUIRE(app->head().media.size() == 1);
    CHECK(app->head().media.begin()->value->hash.empty());
    CHECK(app->versions().history().size() == before + 1);
    CHECK(app->versions().history(1).front()->txn.label == "record");
    const auto recordedVersion = app->head().version;
    const auto immutable = app->versions().pin(recordedVersion);
    const auto source = app->head().media.begin()->id;
    bool dropped = false, saveDuringFinalization = false;
    SUBCASE("hash reaches every stored version, including redo and other branches") {
        call("branch.create", j(R"({"name":"copy","switch":false})"));
        call("track.set", j(R"({"track":"tr_1","name":"Edited"})"));
        call("history.undo");  // redo references another version with the source
    }
    SUBCASE("undo before the job finishes drops the metadata") {
        call("history.undo");
        CHECK(app->head().items.empty());
        dropped = true;
    }
    SUBCASE("deleting the take drops the metadata even if its media source remains") {
        call("item.remove", j(R"({"item":"it_1"})"));
        REQUIRE(app->head().media.find(source));
        dropped = true;
    }
    SUBCASE("a first save before hashing relocates the file and writes a valid empty hash") {
        call("project.save", j(R"({"path":"saved/project.supple"})"));
        auto reopened = makeApp(root.path);
        cmd::Dispatcher reader(registry, *reopened);
        REQUIRE(reader.execute({"project.open", j(R"({"path":"saved/project.supple"})"), kTest}));
        CHECK(reopened->head().media.find(source)->hash.empty());
    }
    SUBCASE("a first save during sidecar finalization retries at the relocated path") {
        holdFinalizer.store(true);
        saveDuringFinalization = true;
    }
    SUBCASE("a replaced project drops the old completion") {
        release.store(true);
        release.notify_all();
        waitCompleted(1);
        call("project.new");
        dropped = true;
    }
    const auto history = app->versions().history().size();
    const auto next = app->versions().nextVersion();
    release.store(true);
    release.notify_all();
    waitCompleted(1);
    d.flushJobEvents();  // hash result schedules finalization if the source still exists
    if (saveDuringFinalization) {
        for (int value = startedJobs.load(); value < 2; value = startedJobs.load())
            startedJobs.wait(value);
        call("project.save", j(R"({"path":"saved/project.supple"})"));
        CHECK(app->head().media.find(source)->hash.empty());
        finalizeRelease.store(true);
        finalizeRelease.notify_all();
    }
    if (app->head().media.find(source)) {
        waitCompleted(2);
        d.flushJobEvents();
        if (saveDuringFinalization) {
            waitCompleted(3);
            d.flushJobEvents();
            CHECK_FALSE(fs::exists(root.path / "scratch"));
        }
    }
    CHECK(app->versions().history().size() == history);
    CHECK(app->versions().nextVersion() == next);
    if (!dropped) {
        const auto* media = app->head().media.find(source);
        const auto file = fs::path(media->path).is_absolute()
                              ? fs::path(media->path)
                              : app->projectDir() / app->head().project->mediaDir / media->path;
        const auto digest = sha256File(file);
        REQUIRE(digest);
        CHECK(media->hash == *digest);
        CHECK(app->versions().version(recordedVersion)->session->media.find(source)->hash ==
              *digest);
        app->versions().forEachVersion([&](const model::Version& version) {
            if (const auto* stored = version.session->media.find(source))
                CHECK(stored->hash == *digest);
        });
        auto side = readTextFile(detail::sidecarPath(file));
        REQUIRE(side);
        CHECK(j(*side)["hash"].asString() == *digest);
        CHECK(immutable->media.find(source)->hash.empty());  // no reader mutation
        call("history.undo");
        CHECK(app->head().items.empty());
    } else {
        CHECK(app->head().items.empty());
        app->versions().forEachVersion([&](const model::Version& version) {
            if (const auto* stored = version.session->media.find(source))
                CHECK(stored->hash.empty());
        });
    }
    app->stopEngine();
}

TEST_CASE("recordings a crash left out are listed and recovered in place, whole") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    // Record a take for real (the Recorder writes the sidecar), then lose it
    // the way a crash before saving would: undo, save, reopen.
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "v"},
        {"command": "track.set", "args": {"track": "${v.track}", "armed": true, "record_input": [1]}}]})");
    call("project.save", R"({"path": "song.json"})");
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.5f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    call("transport.seek", R"({"seconds": 0.5})");
    call("transport.record", "{}");
    // While it records, its file is not offered.
    backend.run(19);
    CHECK(call("project.recoverable", "{}")["recordings"].size() == 0);
    backend.run(38);
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value take = stopped["recorded"].at(0);
    const model::Session recorded = app->head();
    REQUIRE(recorded.items.size() == 1);
    const model::Item original = *recorded.items.begin()->value;
    CHECK(call("project.get", "{}")["recoverable"].asInt() == 0);
    call("history.undo", "{}");
    CHECK(call("project.get", "{}")["recoverable"].asInt() == 0);  // redo still has it
    call("project.save", "{}");
    auto opened = call("project.open", R"({"path": "song.json"})");
    CHECK(opened["recoverable"].asInt() == 1);

    auto list = call("project.recoverable", "{}");
    REQUIRE(list["recordings"].size() == 1);
    const json::Value& rec = list["recordings"].at(0);
    CHECK(rec.find("file")->asString() == "rec/Vocal-1.wav");
    CHECK(rec.find("track")->asString() == "tr_1");
    CHECK(rec.find("channels")->asInt() == 1);
    CHECK(rec.find("seconds")->asDouble() == doctest::Approx(take.find("seconds")->asDouble()));

    auto bad = d.execute({"project.recover", j(R"({"files": ["rec/Other-1.wav"]})"), kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "recover.unknown_file");
    CHECK(bad.error().path == "/args/files/0");

    auto recovered = call("project.recover", R"({"files": ["rec/Vocal-1.wav"]})");
    REQUIRE(recovered["recovered"].size() == 1);
    const model::Session s = app->head();
    REQUIRE(s.items.size() == 1);
    const model::Item& back = *s.items.begin()->value;
    CHECK(back.track == original.track);
    CHECK(back.position == doctest::Approx(original.position));
    CHECK(back.length == doctest::Approx(original.length));
    CHECK(call("project.recoverable", "{}")["recordings"].size() == 0);
    auto nothing = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE_FALSE(nothing);
    CHECK(nothing.error().code == "recover.nothing");
    // One undo step.
    call("history.undo", "{}");
    CHECK(app->head().items.size() == 0);
}

TEST_CASE("recovering a crashed recording repairs its header and plays all of it") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "recording_interrupted");  // tr_1 + rec/Vocal-1.wav: 1 s at 1 s
    const fs::path file = app->recordDir() / "Vocal-1.wav";
    // As the crash left it, a reader sees only up to the last header rewrite.
    auto before = audio::readAudio(file);
    REQUIRE(before);
    CHECK(before->frames() == 24000);

    // The track it was recorded on is gone: it must be told where to go.
    REQUIRE(d.execute({"track.remove", j(R"({"track": "tr_1"})"), kTest}));
    REQUIRE(d.execute({"track.add", j(R"({"name": "Vocal 2"})"), kTest}));
    auto gone = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == "recover.no_track");
    auto r = d.execute({"project.recover", j(R"({"track": "tr_2"})"), kTest});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK((*r)["recovered"].at(0).find("seconds")->asDouble() == doctest::Approx(1.0));
    auto after = audio::readAudio(file);
    REQUIRE(after);
    CHECK(after->frames() == 48000);

    // It plays from 1 s to 2 s (120 BPM: beats 2 to 4), at 0.25 panned centre.
    auto mix = d.execute({"render.mix", j(R"({"name": "recovered", "end_beat": 5})"), kTest});
    REQUIRE(mix);
    auto audio = audio::readAudio((*mix)["path"].asString());
    REQUIRE(audio);
    const auto& left = audio->channels[0];
    REQUIRE(left.size() >= 110000);
    CHECK(double(left[47000]) == 0.0);
    CHECK(double(left[49000]) == doctest::Approx(0.25).epsilon(0.01));
    CHECK(double(left[95000]) == doctest::Approx(0.25).epsilon(0.01));
    CHECK(double(left[97000]) == 0.0);
}

TEST_CASE("an unsaved project records into its scratch folder; the first save moves the files") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto open = [&](const fs::path& scratch) {
        AppOptions o;
        o.workdir = root.path / "work";
        o.scratchDir = scratch;
        o.chainDir = root.path / "chains";
        o.templateDir = root.path / "templates";
        o.projectTemplateDir = root.path / "project-templates";
        o.pluginPath = {kFixture};
        auto app = EngineApp::create(o, {.console9 = false});
        REQUIRE(app);
        return std::move(*app);
    };
    // Saved: the recording moves beside the project, under the same relative path.
    {
        const fs::path scratch = root.path / "unsaved-1";
        auto app = open(scratch);
        cmd::Dispatcher d(registry, *app);
        app->setupFixture(d, "recording_interrupted");  // rec/Vocal-1.wav in the scratch media
        CHECK(app->recordDir() == scratch / "media" / "rec");
        REQUIRE(d.execute({"project.recover", j("{}"), kTest}));
        const auto& media = app->head().media;
        REQUIRE(media.size() == 1);
        CHECK(media.begin()->value->path == "rec/Vocal-1.wav");  // relative to the media folder
        REQUIRE(app->save(root.path / "song" / "song.json"));
        CHECK(fs::exists(root.path / "song" / "media" / "rec" / "Vocal-1.wav"));
        CHECK_FALSE(fs::exists(scratch));
        auto mix = d.execute({"render.mix", j(R"({"name": "after-save", "end_beat": 5})"), kTest});
        REQUIRE(mix);
        CHECK((*mix)["peak_dbfs"].asDouble() > -20.0);  // the moved file still plays
        app->clearScratch();  // quitting a saved project deletes nothing of it
        CHECK(fs::exists(root.path / "song" / "media" / "rec" / "Vocal-1.wav"));
    }
    // Discarded: quitting, or starting a new project, deletes the recordings.
    {
        const fs::path scratch = root.path / "unsaved-2";
        auto app = open(scratch);
        cmd::Dispatcher d(registry, *app);
        app->setupFixture(d, "recording_interrupted");
        REQUIRE(fs::exists(scratch / "media" / "rec" / "Vocal-1.wav"));
        REQUIRE(app->newProject(model::Project{}));
        CHECK_FALSE(fs::exists(scratch));
        app->setupFixture(d, "recording_interrupted");
        REQUIRE(fs::exists(scratch / "media" / "rec" / "Vocal-1.wav"));
        app->clearScratch();
        CHECK_FALSE(fs::exists(scratch));
        CHECK(fs::exists(root.path / "work"));
    }
}

TEST_CASE("two armed tracks with one name record to two files (review round 8)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "a"},
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "b"},
        {"command": "track.set", "args": {"track": "${a.track}", "armed": true, "record_input": [1]}},
        {"command": "track.set", "args": {"track": "${b.track}", "armed": true, "record_input": [2]}}]})");
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = c ? -0.5f : 0.25f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    call("transport.record", "{}");
    backend.run(38);
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 2);
    const std::string f0 = stopped["recorded"].at(0).find("file")->asString();
    const std::string f1 = stopped["recorded"].at(1).find("file")->asString();
    CHECK(f0 != f1);
    for (const auto& [file, value] : {std::pair{f0, 0.25f}, std::pair{f1, -0.5f}}) {
        auto audio = audio::readAudio(file);
        REQUIRE(audio);
        REQUIRE(audio->frames() > 1000);
        CHECK(audio->channels[0][500] == value);  // each file holds its own input
        CHECK(fs::exists(fs::path(file).replace_extension(".rec.json")));
    }
}

TEST_CASE("recovery touches only files in rec/, once each (review round 8)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "recording_interrupted");  // rec/Vocal-1.wav, recoverable
    const fs::path rec = app->recordDir();
    // An eligible WAV outside the project, linked into rec/ with a sidecar.
    const fs::path outside = root.path / "elsewhere.wav";
    fs::copy_file(rec / "Vocal-1.wav", outside);
    fs::copy_file(rec / "Vocal-1.rec.json", rec / "Link-1.rec.json");
    fs::create_symlink(outside, rec / "Link-1.wav");
    auto bytes = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    const std::string before = bytes(outside);

    auto list = d.execute({"project.recoverable", j("{}"), kTest});
    REQUIRE(list);
    REQUIRE((*list)["recordings"].size() == 1);
    CHECK((*list)["recordings"].at(0).find("file")->asString() == "rec/Vocal-1.wav");
    auto link = d.execute({"project.recover", j(R"({"files": ["rec/Link-1.wav"]})"), kTest});
    REQUIRE_FALSE(link);
    CHECK(link.error().code == "recover.unknown_file");
    auto twice = d.execute(
        {"project.recover", j(R"({"files": ["rec/Vocal-1.wav", "rec/Vocal-1.wav"]})"), kTest});
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == "recover.duplicate_file");
    CHECK(app->head().items.size() == 0);
    auto all = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE(all);
    CHECK((*all)["recovered"].size() == 1);
    CHECK(bytes(outside) == before);  // never rewritten through the link
}

TEST_CASE("an LLM loop-records over the bus: one item, a take per pass, punch trims it") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "v"},
        {"command": "track.set", "args": {"track": "${v.track}", "armed": true, "record_input": [1]}}]})");
    // Loop beats 1..2 (0.5 s .. 1 s at 120 BPM), set before the engine runs.
    auto loop = call("transport.loop", R"({"start_beat": 1, "end_beat": 2})");
    CHECK(loop["enabled"].asBool());
    CHECK(loop["start_seconds"].asDouble() == 0.5);
    auto bad = d.execute({"transport.loop", j(R"({"start_beat": 2, "end_beat": 1})"), kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "transport.range");

    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.25f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    call("transport.seek", R"({"seconds": 0.5})");
    auto got = call("transport.get", "{}");
    CHECK(got["loop"].find("enabled")->asBool());
    call("transport.record", "{}");
    backend.run(244);  // two whole passes and a bit
    auto stopped = call("transport.stop", "{}");
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value& rec = stopped["recorded"].at(0);
    const std::size_t passes = rec.find("takes")->size();
    CHECK(passes == 3);  // two whole passes and one partial
    CHECK(rec.find("seconds")->asDouble() == doctest::Approx(0.5));
    const model::Session s = app->head();
    REQUIRE(s.items.size() == 1);
    const model::Item& item = *s.items.begin()->value;
    CHECK(item.position == doctest::Approx(1.0));
    CHECK(item.length == doctest::Approx(1.0));
    REQUIRE(item.takes.size() == passes);
    for (std::size_t k = 0; k < passes; ++k)  // pass k starts k * 0.5 s into the file
        CHECK(s.takes.find(item.takes[k])->startOffset == doctest::Approx(0.5 * double(k)));
    // The newest whole pass is active: the last, or the one before it when
    // the stop cut the last one short.
    const std::string active = rec.find("take")->asString();
    CHECK((active == rec.find("takes")->at(passes - 1).asString() ||
           active == rec.find("takes")->at(passes - 2).asString()));
    CHECK(item.activeTake->str() == active);

    // With a punch range, the same kind of recording keeps 0.6 s .. 0.8 s.
    call("history.undo", "{}");
    call("transport.punch", R"({"start_seconds": 0.6, "end_seconds": 0.8})");
    call("transport.seek", R"({"seconds": 0.5})");
    call("transport.record", "{}");
    backend.run(150);
    auto punched = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(punched["recorded"].size() == 1);
    CHECK(punched["recorded"].at(0).find("seconds")->asDouble() == doctest::Approx(0.2));
    const model::Session p = app->head();
    REQUIRE(p.items.size() == 1);
    const model::Item& pi = *p.items.begin()->value;
    CHECK(pi.position == doctest::Approx(1.2));  // 0.6 s
    const model::Take* first = p.takes.find(pi.takes[0]);
    CHECK(first->startOffset == doctest::Approx(0.1));  // 0.1 s into the first pass
}

TEST_CASE("an LLM records a MIDI performance over the bus and reads it back as notes") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Keys"}, "as": "k"},
        {"command": "fx.add", "args": {"track": "${k.track}", "plugin": "com.suppleaudio.test.sine"}},
        {"command": "track.set", "args": {"track": "${k.track}", "armed": true, "record_midi": "*", "midi_channel": 1}}]})");
    const model::Session armed = app->head();
    const model::Route* route =
        armed.routes.find(*armed.tracks.find(model::Id{model::Kind::Track, 1})->record.input);
    REQUIRE(route);
    CHECK(route->from.midiDevice == std::optional<std::string>("*"));
    CHECK(route->midi->fromChannel == 1);
    // A keyboard: every 24000 device frames (0.5 s) a note on, off 12000 later.
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .midiPorts = {"USB Keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
             for (std::int64_t f = frame; f < frame + n; ++f) {
                 if (f % 12000 != 0) continue;
                 MidiInputEvent m;
                 m.frame = static_cast<std::uint32_t>(f - frame);
                 m.size = 3;
                 const bool on = f % 24000 == 0;
                 m.bytes[0] = on ? 0x90 : 0x80;
                 m.bytes[1] = 60;
                 m.bytes[2] = on ? 100 : 0;
                 out.push_back(m);
             }
         }});
    REQUIRE(
        app->startEngine(backend,
                         {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .midiInputs = true},
                         {.anticipative = false})
            .ok());
    call("transport.record", "{}");
    backend.run(263);
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 1);
    const std::string itemId = stopped["recorded"].at(0).find("item")->asString();
    const model::Session s = app->head();
    const model::Item& item = *s.items.find(*model::Id::parse(itemId));
    const model::Take& take = *s.takes.find(*item.activeTake);
    REQUIRE(take.source);
    const midi::Sequence& seq = s.midi.find(*take.source)->sequence;
    REQUIRE(seq.notes.size() >= 2);
    for (std::size_t i = 0; i < seq.notes.size(); ++i) {
        CHECK(seq.notes[i].pitch == 60);
        CHECK(seq.notes[i].velocity == 100);
        if (i + 1 < seq.notes.size()) {
            CHECK(seq.notes[i].length == 480);                          // half a beat
            CHECK(seq.notes[i + 1].start - seq.notes[i].start == 960);  // one beat apart
        }
    }
    // Read back as notation.
    auto text = call("midi.get", std::string(R"({"item": ")") + itemId + R"("})");
    CHECK(text["text"].asString().find("C4") != std::string::npos);
}

TEST_CASE("a batch keeps text positions of nested errors (not pointer-prefixed)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto r = d.execute({"batch", j(R"({"steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "b"},
        {"command": "item.add", "args": {"track": "${b.track}", "bar": 1, "notation": "voice v; 1: C4:q | 2: D4:q | 3: E4:q"}}]})"),
                        kTest});
    REQUIRE_FALSE(r);
    REQUIRE(r.error().details.size() == 1);
    const Error& step = r.error().details[0];
    CHECK(step.path == "/steps/1/args/notation");
    REQUIRE_FALSE(step.details.empty());
    CHECK(step.details[0].path.front() != '/');  // "1:16": a position in the notation text
    CHECK(step.details[0].path.find("/steps") == std::string::npos);
}

TEST_CASE("recorded MIDI notes are clipped at punch and overdub item edges") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto result = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    };
    call("batch", R"({"steps":[
        {"command":"track.add","args":{"name":"Keys"},"as":"k"},
        {"command":"fx.add","args":{"track":"${k.track}","plugin":"com.suppleaudio.test.sine"}}]})");
    const model::Id track{model::Kind::Track, 1};
    RecordedTake r;
    r.track = track;
    r.isMidi = true;
    r.sampleRate = 48000;
    r.frames = 48000;
    r.segments = {{0, 0}};
    r.midi = {{9600, 0, {0x90, 72, 100}}, {38400, 0, {0x80, 72, 0}}};
    app->setPunch({true, 0.4, 0.6});
    REQUIRE(app->commitTakes({r}));
    const auto& punched = app->head();
    REQUIRE(punched.items.size() == 1);
    const model::Item& item = *punched.items.begin()->value;
    const midi::Sequence& seq =
        punched.midi.find(*punched.takes.find(*item.activeTake)->source)->sequence;
    REQUIRE(seq.notes.size() == 1);
    CHECK(seq.notes[0].start == 0);
    CHECK(seq.notes[0].length == 384);  // 0.2 seconds at 120 bpm and 960 PPQ
    auto rendered = app->renderMix({.startSeconds = 0, .endSeconds = 0.8});
    REQUIRE(rendered);
    const auto& audio = rendered->audio.channels[0];
    double during = 0, before = 0, after = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const double v = std::fabs(double(audio[i]));
        if (i < 19200)
            before = std::max(before, v);
        else if (i < 28800)
            during = std::max(during, v);
        else
            after = std::max(after, v);
    }
    CHECK(before == 0.0);
    CHECK(during > 0.1);
    CHECK(after == 0.0);

    auto overdub = makeApp(root.path / "overdub");
    cmd::Dispatcher overdubBus(registry, *overdub);
    auto setup = overdubBus.execute({"batch", j(R"({"steps":[
        {"command":"track.add","args":{"name":"Keys"},"as":"k"},
        {"command":"item.add","args":{"track":"${k.track}","bar":1,"notation":"voice v; 1: C4:w"}},
        {"command":"track.set","args":{"track":"${k.track}","record_mode":"midi_overdub"}}]})"),
                                     kTest});
    REQUIRE_MESSAGE(setup, (setup ? std::string() : setup.error().toString()));
    RecordedTake edge;
    edge.track = track;
    edge.isMidi = true;
    edge.sampleRate = 48000;
    edge.frames = 144000;
    edge.segments = {{0, 0}};
    edge.midi = {{91200, 0, {0x90, 74, 100}}, {105600, 0, {0x80, 74, 0}}};
    REQUIRE(overdub->commitTakes({edge}));
    const auto& merged = overdub->head();
    REQUIRE(merged.items.size() == 1);
    const model::Item& oldItem = *merged.items.begin()->value;
    const midi::Sequence& notes =
        merged.midi.find(*merged.takes.find(*oldItem.activeTake)->source)->sequence;
    const auto clipped = std::find_if(notes.notes.begin(), notes.notes.end(),
                                      [](const midi::Note& n) { return n.pitch == 74; });
    REQUIRE(clipped != notes.notes.end());
    CHECK(clipped->start == 3648);
    CHECK(clipped->length == 192);
}

TEST_CASE("midi_overdub merges a performance into the item under it, as a new take") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    // Four bars of a bass line on beat 1 of each bar (at 120 BPM a bar is 2 s).
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Keys"}, "as": "k"},
        {"command": "fx.add", "args": {"track": "${k.track}", "plugin": "com.suppleaudio.test.sine"}},
        {"command": "item.add", "args": {"track": "${k.track}", "bar": 1, "notation": "voice v; 1: C3:w | C3:w | C3:w | C3:w"}},
        {"command": "track.set", "args": {"track": "${k.track}", "armed": true, "record_midi": "*", "record_mode": "midi_overdub"}}]})");
    call("take.set", R"({"take":"tk_1","playrate":2})");
    call("fx.add", R"({"take":"tk_1","plugin":"Test Gain"})");
    call("env.add", R"({"fx":"fx_2","param":"gain","points":[{"beat":0,"value":-6}]})");
    const model::Session before = app->head();
    REQUIRE(before.items.size() == 1);
    const model::Id item = before.items.begin()->id;
    const model::Id oldTake = *before.items.begin()->value->activeTake;
    const std::size_t oldNotes =
        before.midi.find(*before.takes.find(oldTake)->source)->sequence.notes.size();
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .midiPorts = {"Keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
             for (std::int64_t f = frame; f < frame + n; ++f) {
                 if (f % 12000 != 0) continue;
                 MidiInputEvent m;
                 m.frame = static_cast<std::uint32_t>(f - frame);
                 m.size = 3;
                 const bool on = f % 24000 == 0;
                 m.bytes[0] = on ? 0x90 : 0x80;
                 m.bytes[1] = 72;
                 m.bytes[2] = on ? 90 : 0;
                 out.push_back(m);
             }
         }});
    REQUIRE(
        app->startEngine(backend,
                         {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .midiInputs = true},
                         {.anticipative = false})
            .ok());
    call("transport.record", "{}");
    backend.run(225);  // exactly 1.2 s at 48 kHz/256, no clock criterion
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 1);
    CHECK(stopped["recorded"].at(0).find("item")->asString() == item.str());  // no new item
    const model::Session s = app->head();
    CHECK(s.items.size() == 1);
    const model::Item& it = *s.items.find(item);
    REQUIRE(it.takes.size() == 2);
    CHECK(it.takes[0] == oldTake);  // the old take stays
    const midi::Sequence& merged = s.midi.find(*s.takes.find(*it.activeTake)->source)->sequence;
    std::size_t c5 = 0, c3 = 0;
    for (const midi::Note& n : merged.notes) (n.pitch == 72 ? c5 : c3) += 1;
    CHECK(c3 == oldNotes);
    CHECK(c5 >= 2);
    for (const auto& note : merged.notes)
        if (note.pitch == 72 && note.start == 0) CHECK(note.length == 960);
    const auto* fresh = s.takes.find(*it.activeTake);
    REQUIRE(fresh->chain.size() == 1);
    CHECK(fresh->chain[0] != before.takes.find(oldTake)->chain[0]);
    CHECK(s.envelopes.size() == before.envelopes.size() + 1);
    call("history.undo", "{}");
    CHECK(app->head().items.find(item)->takes.size() == 1);
}

TEST_CASE("linear Console 9 stems sum to the master-less mix, and regions match dry run") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_region");
    auto call = [&](std::string name, std::string_view args) {
        auto r = d.execute({std::move(name), j(args), kTest});
        INFO((r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    };
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    call("track.add", R"({"name":"Lead"})");
    call("item.add", R"({"track":"tr_2","bar":1,"notation":"voice lead; 1: E3:q r:q E3:q r:q"})");
    call("fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.sine"})");
    auto mix = call("render.mix", R"({"name":"sum","end_beat":4})");
    auto plannedMix = call("render.dry-run", R"({"operation":"mix","name":"sum","end_beat":4})");
    CHECK(plannedMix.find("files")->at(0).find("path")->asString() == mix.find("path")->asString());
    auto stems = call("render.stems", R"({"end_beat":4,"pattern":"stem-{track_number:02}"})");
    const auto& files = *stems.find("files");
    REQUIRE(files.size() == 2);
    auto whole = audio::readAudio(mix.find("path")->asString());
    auto left = audio::readAudio(files.at(0).find("path")->asString());
    auto right = audio::readAudio(files.at(1).find("path")->asString());
    REQUIRE(whole);
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(whole->frames() == left->frames());
    REQUIRE(whole->frames() == right->frames());
    for (std::size_t c = 0; c < whole->channelCount(); ++c)
        for (std::size_t i = 0; i < whole->frames(); ++i)
            CHECK(static_cast<double>(whole->channels[c][i]) ==
                  doctest::Approx(static_cast<double>(left->channels[c][i]) +
                                  static_cast<double>(right->channels[c][i]))
                      .epsilon(1e-5));
    auto through = call("render.stems", R"({"tracks":["tr_1"],"mode":"through_master",
        "pattern":"through-{track}"})");
    auto mastered = audio::readAudio(through.find("files")->at(0).find("path")->asString());
    REQUIRE(mastered);
    CHECK(mastered->channels == left->channels);
    auto dry =
        call("render.dry-run", R"({"regions":["rg_1"],"pattern":"region-{region_number:02}"})");
    auto rendered =
        call("render.regions", R"({"regions":["rg_1"],"pattern":"region-{region_number:02}"})");
    CHECK(dry.find("files")->at(0).find("path")->asString() ==
          rendered.find("files")->at(0).find("path")->asString());
    auto regionAudio = audio::readAudio(rendered.find("files")->at(0).find("path")->asString());
    REQUIRE(regionAudio);
    CHECK(regionAudio->frames() == 96000);
    call(
        "project.set",
        R"({"render":{"matrix":{"project":["tr_0"],"rg_1":["tr_1","tr_0"]},"pattern":"matrix-{region}-{track}-{counter:02}"}})");
    auto matrix = call("render.regions", R"({})");
    CHECK(matrix.find("files")->size() == 3);
}

TEST_CASE("branch render uses its own head and leaves the current branch alone") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    auto call = [&](std::string name, std::string_view args) {
        auto r = d.execute({std::move(name), j(args), kTest});
        INFO((r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    };
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    call("branch.create", R"({"name":"alternate","switch":true})");
    call("track.set", R"({"track":"tr_1","volume_db":-12})");
    call("branch.switch", R"({"name":"main"})");
    auto other = call("render.mix", R"({"branch":"alternate","name":"other","end_beat":4})");
    CHECK(app->versions().branch() == "main");
    call("branch.switch", R"({"name":"alternate"})");
    auto direct = call("render.mix", R"({"name":"direct","end_beat":4})");
    auto a = audio::readAudio(other.find("path")->asString());
    auto b = audio::readAudio(direct.find("path")->asString());
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->channels == b->channels);
    auto compared = call("branch.render-ab", R"({"a":"main","b":"alternate","end_beat":4})");
    CHECK(compared.find("a")->isString());
    CHECK(compared.find("b")->isString());
}

TEST_CASE("render job cancellation is observable before worker renders") {
    Root root;
    std::atomic<bool> entered{false}, release{false};
    AppOptions options;
    options.workdir = root.path;
    options.pluginPath = {kFixture};
    options.jobStartedHook = [&] {
        entered.store(true);
        entered.notify_one();
        release.wait(false);
    };
    auto created = EngineApp::create(std::move(options));
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    auto started = d.execute({"render.mix", j(R"({"end_beat":4,"job":true})"), kTest});
    REQUIRE(started);
    entered.wait(false);
    const auto id = started->find("job")->asString();
    json::Value args = json::Value::object();
    args["job"] = id;
    auto cancelled = d.execute({"job.cancel", std::move(args), kTest});
    release.store(true);
    release.notify_one();
    REQUIRE(cancelled);
    CHECK(d.jobs().status(id)->find("status")->asString() == "cancelled");
}

TEST_CASE("add_to_project puts renders on new tracks as one undo step (sync and job)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    // Items whose take plays `path` (and where the last one starts).
    double position = -1;
    auto itemsWithFile = [&](const std::string& path) {
        const model::Session& h = app->head();
        int n = 0;
        for (const auto& e : h.items)
            for (const model::Id& takeId : e.value->takes)
                if (const auto* take = h.takes.find(takeId); take && take->source)
                    if (const auto* src = h.media.find(*take->source);
                        src && fs::path(src->path).filename() == fs::path(path).filename()) {
                        ++n;
                        position = e.value->position;
                    }
        return n;
    };
    const std::size_t tracks = app->head().tracks.size();
    auto mix = d.execute(
        {"render.mix", j(R"({"end_beat":2,"start_beat":1,"name":"bounce","add_to_project":true})"),
         kTest});
    REQUIRE(mix);
    // The follow-up ran before execute() returned: a new track, the file on it at start_beat.
    CHECK(app->head().tracks.size() == tracks + 1);
    const std::string path = mix->find("path")->asString();
    CHECK(itemsWithFile(path) == 1);
    CHECK(position == doctest::Approx(1.0));
    CHECK(app->versions().history(1).front()->txn.label == "Add renders to project");
    REQUIRE(d.execute({"history.undo", json::Value::object(), kTest}));
    CHECK(app->head().tracks.size() == tracks);  // the import alone is undone
    CHECK(fs::exists(path));

    auto started = d.execute(
        {"render.stems",
         j(R"({"end_beat":2,"job":true,"pattern":"stem-{track}","add_to_project":true})"), kTest});
    REQUIRE(started);
    json::Value args = json::Value::object();
    args["job"] = started->find("job")->asString();
    args["timeout_s"] = 30;
    auto done = d.execute({"job.wait", args, kTest});
    REQUIRE(done);
    // Whoever sees the job done sees its files in the project.
    const auto& files = *done->find("result")->find("files");
    REQUIRE(files.size() >= 1);
    for (const auto& f : files.asArray()) CHECK(itemsWithFile(f.find("path")->asString()) == 1);

    // A batch that waits for the job does not take the import into its own
    // undo step: the import follows as its own step once the batch closes.
    started = d.execute(
        {"render.stems",
         j(R"({"end_beat":3,"job":true,"pattern":"late-{track}","add_to_project":true})"), kTest});
    REQUIRE(started);
    const json::Value batch = j(R"({"label":"waits","steps":[
        {"command":"track.add","args":{"name":"Mine"}},
        {"command":"job.wait","args":{"timeout_s":30,"job":")" +
                                started->find("job")->asString() + R"("}}]})");
    const std::size_t before = app->head().tracks.size();
    auto waited = d.execute({"batch", batch, kTest});
    REQUIRE(waited);
    const auto& late = *waited->find("results")->asArray().back().find("result")->find("files");
    REQUIRE(late.size() >= 1);
    // One new track per file (identical silent stems share one media entry).
    CHECK(app->head().tracks.size() == before + 1 + late.size());
    CHECK(app->versions().history(1).front()->txn.label == "Add renders to project");
    REQUIRE(d.execute({"history.undo", json::Value::object(), kTest}));
    CHECK(app->head().tracks.size() == before + 1);  // the batch's own track stays
}

TEST_CASE("stem render job publishes files and analysis summaries") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    auto started = d.execute(
        {"render.stems", j(R"({"end_beat":4,"job":true,"pattern":"queued-{track}"})"), kTest});
    REQUIRE(started);
    const auto id = started->find("job")->asString();
    json::Value args = json::Value::object();
    args["job"] = id;
    args["timeout_s"] = 30;
    auto done = d.execute({"job.wait", args, kTest});
    INFO((done ? std::string() : done.error().toString()));
    REQUIRE(done);
    CHECK(done->find("status")->asString() == "done");
    const auto& file = done->find("result")->find("files")->at(0);
    CHECK(fs::exists(file.find("path")->asString()));
    CHECK(file.find("analysis") != nullptr);
    json::Value get = json::Value::object();
    get["job"] = id;
    auto status = d.execute({"job.get", get, kTest});
    REQUIRE(status);
    CHECK(status->find("status")->asString() == "done");
}

TEST_CASE("offline render reports progress and honors cancellation between blocks") {
    Root root;
    auto app = makeApp(root.path);
    OfflineSettings settings;
    settings.endSeconds = 1.0;
    settings.progressFlag = std::make_shared<std::atomic<double>>(0.0);
    auto rendered = app->renderMix(settings);
    REQUIRE(rendered);
    CHECK(settings.progressFlag->load() == doctest::Approx(1.0));
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    settings.cancelFlag = cancel;
    auto stopped = app->renderMix(settings);
    CHECK_FALSE(stopped);
    if (!stopped) CHECK(stopped.error().code == "job.cancelled");
}

TEST_CASE("render patterns cover the closed tokens, filters, empty separators and escapes") {
    RenderPatternContext c;
    c.project = "Big Song";
    c.track = "Lead Vocal";
    c.folderPath = "Band/Vocals";
    c.region = "Verse";
    c.marker = "Start";
    c.item = "it_1";
    c.take = "Main";
    c.trackNumber = 4;
    c.regionNumber = 2;
    c.counter = 7;
    c.startBeats = 8;
    c.startSeconds = 4;
    c.sampleRate = 48000;
    c.bitDepth = 24;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"{project}", "Big Song"},
        {"{track}", "Lead Vocal"},
        {"{track_number:02}", "04"},
        {"{folder_path}", "Band/Vocals"},
        {"{region}", "Verse"},
        {"{region_number:02}", "02"},
        {"{marker}", "Start"},
        {"{item}", "it_1"},
        {"{take}", "Main"},
        {"{start:beats}", "8"},
        {"{start:seconds}", "4"},
        {"{start:timecode}", "00-00-04"},
        {"{format}", "wav"},
        {"{sample_rate}", "48000"},
        {"{bit_depth}", "24"},
        {"{counter:03}", "007"},
        {"{track|lower}", "lead vocal"},
        {"{region|upper}", "VERSE"},
        {"{track|replace(\" \",\"_\")}", "Lead_Vocal"},
        {"{track|max(4)}", "Lead"},
    };
    for (const auto& [pattern, expected] : cases) {
        auto out = expandRenderPattern(pattern, c);
        INFO(pattern);
        REQUIRE(out);
        CHECK(*out == expected);
    }
    CHECK(expandRenderPattern("x-{marker}-y", RenderPatternContext{}).value() == "x-y");
    CHECK_FALSE(expandRenderPattern("../escape", c));
    CHECK_FALSE(expandRenderPattern("{unknown}", c));
    CHECK_FALSE(expandRenderPattern("{track|bogus}", c));
    CHECK(expandRenderPattern("{date:YYYY-MM-DD}", c)->size() == 10);
    CHECK(expandRenderPattern("{time:HH-mm-ss}", c)->size() == 8);
}

TEST_CASE("render dry run rejects paths through symlinks outside renders") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    fs::create_directories(app->rendersDir());
    fs::create_directory_symlink(root.path.parent_path(), app->rendersDir() / "escape");
    auto result =
        d.execute({"render.dry-run", j(R"({"end_beat":1,"pattern":"escape/out"})"), kTest});
    CHECK_FALSE(result);
    if (!result) CHECK(result.error().code == "render.path");
}

TEST_CASE("render mix honors normalization and PCM export settings") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_fx");
    auto render = d.execute({"render.mix", j(R"({"name":"mastered","end_beat":4,
        "sample_rate":44100,"channels":1,"bit_depth":16,
        "normalize":{"mode":"lufs_i","target":-20,"limit_true_peak_db":-1}})"),
                             kTest});
    INFO((render ? std::string() : render.error().toString()));
    REQUIRE(render);
    auto audio = audio::readAudio(render->find("path")->asString());
    REQUIRE(audio);
    CHECK(audio->sampleRate == 44100);
    CHECK(audio->channelCount() == 1);
    const float* mono[] = {audio->channels[0].data()};
    auto report = analysis::analyze({mono, static_cast<std::int64_t>(audio->frames()),
                                     audio->sampleRate, analysis::Layout::Mono});
    REQUIRE(report);
    CHECK(report->loudness->integrated == doctest::Approx(-20).epsilon(0.04));
    CHECK(report->loudness->truePeak <= -0.9);
}

TEST_CASE("true peak limiting keeps distant audio at the normalization gain") {
    audio::AudioData original;
    original.sampleRate = 48000;
    original.channels.assign(1, std::vector<float>(96000));
    for (std::size_t i = 0; i < original.frames(); ++i)
        original.channels[0][i] = static_cast<float>(
            0.05 * std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / 48000.0));
    original.channels[0][48000] = 1.0f;
    auto unlimited = original, limited = original;
    REQUIRE(normalizeRenderAudio(unlimited, "lufs_i", -20));
    REQUIRE(normalizeRenderAudio(limited, "lufs_i", -20, -1));
    const std::size_t far = 10000;
    CHECK(std::fabs(static_cast<double>(limited.channels[0][far])) >=
          0.8 * std::fabs(static_cast<double>(unlimited.channels[0][far])));
    const float* channels[] = {limited.channels[0].data()};
    auto report = analysis::analyze({channels, static_cast<std::int64_t>(limited.frames()),
                                     limited.sampleRate, analysis::Layout::Mono});
    REQUIRE(report);
    CHECK(report->loudness->truePeak <= -0.95);
}

TEST_CASE("the tick autosaves the project, rotates five, and open reports a newer one") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    app->setFixedClock(1'000'000);
    call("project.set", R"({"name": "Autosaved"})");
    const fs::path project =
        call("project.save", R"({"path": "autosave-project.json"})")["path"].asString();
    REQUIRE(fs::exists(project));
    // The project file is older than the autosaves written below (mtime
    // granularity must not decide the report).
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(project, now - std::chrono::seconds(60));

    CHECK_FALSE(app->nextServiceDelayMs());
    // 60 s with no change: nothing is written.
    app->setFixedClock(1'060'000);
    app->tick();
    CHECK_FALSE(fs::exists(app->autosaveDir()));

    CHECK_FALSE(app->nextServiceDelayMs());
    // A change plus an interval: one autosave, then six more (rotating to five).
    for (int i = 0; i < 6; ++i) {
        call("project.set", std::string(R"({"name": "Take )") + std::to_string(i) + "\"}");
        if (i == 0)
            CHECK(app->nextServiceDelayMs() == 0);
        else
            CHECK(app->nextServiceDelayMs() == 60'000);
        app->setFixedClock(1'120'000 + static_cast<std::uint64_t>(i) * 60'000);
        app->tick();
    }
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(app->autosaveDir()))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"autosave-2.json", "autosave-3.json", "autosave-4.json",
                                            "autosave-5.json", "autosave-6.json"});
    // The newest autosave is a valid project file with the last name.
    auto text = readTextFile(app->autosaveDir() / "autosave-6.json");
    REQUIRE(text);
    auto parsed = model::parseProject(*text);
    REQUIRE_MESSAGE(parsed, (parsed ? std::string() : parsed.error().toString()));
    CHECK(parsed->project->name == "Take 5");

    // A fresh app opening the file is told a newer autosave exists, and does
    // not load it: the head is still what was saved.
    auto other = makeApp(root.path / "fresh");
    cmd::Dispatcher d2(registry, *other);
    auto openArgs = json::Value::object();
    openArgs["path"] = project.string();
    auto opened = d2.execute({"project.open", std::move(openArgs), kTest});
    REQUIRE_MESSAGE(opened, (opened ? std::string() : opened.error().toString()));
    CHECK((*opened)["autosave_available"].asBool());
    CHECK(other->head().project->name == "Autosaved");
}

TEST_CASE("the tick restarts the engine after a device change, then gives up and reports") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    AppOptions o;
    o.workdir = root.path;
    o.pluginPath = {kFixture};
    o.jackLibrary = FakeJack::kPath;
    auto created = EngineApp::create(o, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    app->setFixedClock(1'000'000);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "bass"},
        {"command": "item.add", "args": {"track": "${bass.track}", "bar": 1,
                                         "notation": "voice bass; 1: A2:w | D3:w"}}]})");
    app->takeEvents();
    call("engine.start", R"({"backend": "jack", "block": 256})");
    call("transport.seek", R"({"seconds": 1.0})");

    // The device changes rate: the tick stops the engine, announces it, and
    // starts it again at the new rate, stopped, at the same position.
    fake.setRate(44100);
    app->tick();
    bool announced = false;
    for (auto& [topic, payload] : app->takeEvents())
        if (topic == "engine.restarting") {
            announced = true;
            CHECK(payload.find("reason")->asString().find("rate") != std::string::npos);
        }
    CHECK(announced);
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    CHECK(app->nextServiceDelayMs() == 0);
    app->tick();  // the retry fires at once (backoff starts at zero)
    auto status = call("engine.status", "{}");
    REQUIRE(status["running"].asBool());
    CHECK(status["sample_rate"].asDouble() == 44100.0);
    // The meter rings report their drops (P5.3c); a quiet engine drops nothing.
    REQUIRE(status.find("meter_drops") != nullptr);
    CHECK(status["meter_drops"]["renderer"].asInt() == 0);
    CHECK(status["meter_drops"]["callback"].asInt() == 0);
    auto t = call("transport.get", "{}");
    CHECK_FALSE(t["playing"].asBool());
    CHECK(t["seconds"].asDouble() == doctest::Approx(1.0).epsilon(0.2));

    // The server goes away and no reopen succeeds: after 30 s the tick gives
    // up and reports engine.error.
    fake.shutdown();
    app->tick();
    fake.failOpen(1);
    app->tick();
    CHECK(app->nextServiceDelayMs() == 250);
    fake.failOpen(1);
    app->setFixedClock(1'029'999);
    app->tick();
    CHECK(app->nextServiceDelayMs() == 1);  // give-up precedes the next retry
    app->setFixedClock(1'031'000);
    app->tick();
    bool reported = false;
    for (auto& [topic, payload] : app->takeEvents())
        if (topic == "engine.error" && payload.isObject()) {
            reported = true;
            CHECK(payload["error"].asString().find("JACK") != std::string::npos);
        }
    CHECK(reported);
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    app->tick();  // it stays given up, it does not retry forever
    app->setFixedClock(1'100'000);
    app->tick();
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    fake.reset();
}

TEST_CASE("a UI gesture is one undo step: edits with the same gesture coalesce") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, *app);
    app->attachCommands(dispatcher);
    auto run = [&](const char* command, json::Value args, cmd::Origin origin) {
        auto r = dispatcher.execute({command, std::move(args), std::move(origin)});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    const std::string track = run("track.add", j(R"({"name": "Bass"})"), kTest)["track"].asString();
    const auto versions = [&] {
        return run("history.list", j("{}"), kTest)["versions"].asArray().size();
    };
    const std::size_t before = versions();
    cmd::Origin drag{cmd::Origin::Type::Ui, "gui", "", "fader:" + track + ":1"};
    for (double db : {-1.0, -2.5, -4.0, -6.0}) {
        json::Value args = json::Value::object();
        args["track"] = track;
        args["volume_db"] = db;
        run("track.set", std::move(args), drag);
    }
    CHECK(versions() == before + 1);  // four edits, one step
    CHECK(app->head().tracks.find(*model::Id::parse(track))->volumeDb == doctest::Approx(-6.0));
    // A new gesture is a new step; another origin's same key does not merge.
    drag.gesture = "fader:" + track + ":2";
    run("track.set", j(R"({"track": ")" + track + R"(", "volume_db": -3})"), drag);
    CHECK(versions() == before + 2);
    cmd::Origin other{cmd::Origin::Type::Mcp, "agent", "", drag.gesture};
    run("track.set", j(R"({"track": ")" + track + R"(", "volume_db": -9})"), other);
    CHECK(versions() == before + 3);
    // Undo removes the whole gesture.
    run("history.undo", j("{}"), kTest);
    run("history.undo", j("{}"), kTest);
    run("history.undo", j("{}"), kTest);
    CHECK(app->head().tracks.find(*model::Id::parse(track))->volumeDb == doctest::Approx(0.0));
}

TEST_CASE("midi.notes / midi.edit: ops by original index, one undo step, selection kept") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    REQUIRE(
        d.execute({"midi.write",
                   j(R"({"item":"it_1","notation":"voice v; 1: C4:q D4:q E4:q F4:q"})"), kTest}));
    auto notes = [&] {
        auto r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
        REQUIRE(r);
        return *r->find("notes");
    };
    auto n0 = notes();
    REQUIRE(n0.size() == 4);
    CHECK(n0.at(0).find("pitch")->asInt() == 60);
    CHECK(n0.at(1).find("start")->asDouble() == doctest::Approx(1.0));
    CHECK(n0.at(1).find("length")->asDouble() == doctest::Approx(1.0));
    const auto before = app->head().version;

    // Remove D (1), move E (2) up an octave and later, add a note at 0.5: all
    // indices refer to the list before the command.
    auto edit = d.execute({"midi.edit", j(R"({"item":"it_1","ops":[
        {"op":"remove","notes":[1]},
        {"op":"move","notes":[2],"semitones":12,"beats":0.25},
        {"op":"add","pitch":67,"start":0.5,"length":0.5,"velocity":80},
        {"op":"velocity","notes":[0],"velocity_delta":-127}]})"),
                           kTest});
    INFO((edit ? std::string() : edit.error().toString()));
    REQUIRE(edit);
    auto n1 = notes();
    REQUIRE(n1.size() == 4);  // C, G (added), E' (moved), F
    CHECK(n1.at(0).find("pitch")->asInt() == 60);
    CHECK(n1.at(0).find("velocity")->asInt() == 1);  // clamped
    CHECK(n1.at(1).find("pitch")->asInt() == 67);
    CHECK(n1.at(2).find("pitch")->asInt() == 76);
    CHECK(n1.at(2).find("start")->asDouble() == doctest::Approx(2.25));
    // The touched notes' new indices: C (velocity), G (added), E' (moved).
    const auto& sel = *edit->find("notes");
    REQUIRE(sel.size() == 3);
    CHECK(sel.at(0).asInt() == 0);
    CHECK(sel.at(1).asInt() == 1);
    CHECK(sel.at(2).asInt() == 2);
    CHECK(app->head().version == before + 1);  // one undo step

    // Quantize to the beat: E' at 2.25 snaps to 2.
    REQUIRE(
        d.execute({"midi.edit",
                   j(R"({"item":"it_1","ops":[{"op":"quantize","notes":[2],"grid":1}]})"), kTest}));
    CHECK(notes().at(2).find("start")->asDouble() == doctest::Approx(2.0));
    // Resize the end and the start.
    REQUIRE(d.execute(
        {"midi.edit",
         j(R"({"item":"it_1","ops":[{"op":"resize","notes":[3],"length_delta":1},{"op":"resize","notes":[0],"start_delta":0.5}]})"),
         kTest}));
    auto n2 = notes();
    CHECK(n2.at(3).find("length")->asDouble() == doctest::Approx(2.0));
    // CC points in and out.
    REQUIRE(d.execute(
        {"midi.edit",
         j(R"({"item":"it_1","ops":[{"op":"cc.set","number":1,"points":[{"beat":0,"value":10},{"beat":2,"value":100}]}]})"),
         kTest}));
    auto r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
    REQUIRE(r->find("cc")->size() == 2);
    REQUIRE(
        d.execute({"midi.edit",
                   j(R"({"item":"it_1","ops":[{"op":"cc.clear","number":1,"from":1}]})"), kTest}));
    r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
    CHECK(r->find("cc")->size() == 1);

    // Errors are structured and change nothing.
    const auto v = app->head().version;
    auto bad = d.execute(
        {"midi.edit", j(R"({"item":"it_1","ops":[{"op":"move","notes":[9],"beats":1}]})"), kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "midi.note_index");
    CHECK(bad.error().path == "/args/ops/0/notes");
    CHECK(app->head().version == v);
    REQUIRE(d.execute({"history.undo", json::Value::object(), kTest}));  // the cc.clear
    r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
    CHECK(r->find("cc")->size() == 2);
}

TEST_CASE("track.add: numbered names by type and the app's default monitoring") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto add = [&](const char* args) {
        auto r = d.execute({"track.add", j(args), kTest});
        REQUIRE(r);
        return *model::Id::parse(r->find("track")->asString());
    };
    const model::Id ch1 = add("{}"), ch2 = add("{}"), buss = add(R"({"type":"buss"})");
    const model::Id master = add(R"({"type":"master"})"), named = add(R"({"name":"Vox"})");
    const model::Session& s = app->head();
    CHECK(s.tracks.find(ch1)->name == "ch_1");
    CHECK(s.tracks.find(ch2)->name == "ch_2");
    CHECK(s.tracks.find(buss)->name == "buss_1");
    CHECK(s.tracks.find(master)->name == "mstr_1");
    CHECK(s.tracks.find(named)->name == "Vox");
    CHECK(model::trackLabel(s, model::kMaster) == "Master");
    // A new channel monitors (once armed) by default; the preference changes it.
    CHECK(s.tracks.find(ch1)->record.monitor == model::Monitor::On);
    CHECK(s.tracks.find(buss)->record.monitor == model::Monitor::Off);
    app->setDefaultMonitor(model::Monitor::Auto);
    const model::Id ch3 = add("{}");
    CHECK(app->head().tracks.find(ch3)->record.monitor == model::Monitor::Auto);
    CHECK(app->head().tracks.find(ch3)->name == "ch_3");
    // An unnamed track shows a label by type; a freed number is used again.
    REQUIRE(d.execute({"track.set", j(R"({"track":")" + ch2.str() + R"(","name":""})"), kTest}));
    CHECK(model::trackLabel(app->head(), ch2) == "ch_2");
    REQUIRE(d.execute({"track.remove", j(R"({"track":")" + ch1.str() + R"("})"), kTest}));
    const model::Id again = add(R"({"type":"buss"})");
    CHECK(app->head().tracks.find(again)->name == "buss_2");
    REQUIRE(d.execute({"track.remove", j(R"({"track":")" + buss.str() + R"("})"), kTest}));
    const model::Id reused = add(R"({"type":"buss"})");
    CHECK(app->head().tracks.find(reused)->name == "buss_1");
}

TEST_CASE("route.matrix: type rules, cycles and existing sends") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto add = [&](const char* args) {
        auto r = d.execute({"track.add", j(args), kTest});
        REQUIRE(r);
        return r->find("track")->asString();
    };
    const std::string a = add(R"({"type":"buss","name":"A"})");
    const std::string b = add(R"({"type":"buss","name":"B"})");
    const std::string ch = add(R"({"name":"Vox"})");
    REQUIRE(d.execute(
        {"track.move", j(R"({"track":")" + ch + R"(","parent":")" + a + R"("})"), kTest}));
    auto send =
        d.execute({"route.add", j(R"({"from":")" + a + R"(","to":")" + b + R"("})"), kTest});
    REQUIRE(send);
    auto m = d.execute({"route.matrix", json::Value::object(), kTest});
    REQUIRE(m);
    // Sources in the views' order (the master's tree, a parent's channels before
    // its busses, the masters last) with depth and the views' labels; the
    // master's rule; a channel's rule as a destination.
    const auto& sources = *m->find("sources");
    REQUIRE(sources.size() == 4);
    CHECK(sources.at(0).find("id")->asString() == a);
    CHECK(sources.at(1).find("id")->asString() == ch);
    CHECK(sources.at(2).find("id")->asString() == b);
    CHECK(sources.at(3).find("id")->asString() == "tr_0");
    CHECK(sources.at(3).find("name")->asString() == "Master");
    CHECK(sources.at(3).find("rule")->find("code")->asString() == "route.strip_type");
    CHECK(sources.at(1).find("depth")->asInt() == 1);
    CHECK(sources.at(1).find("parent")->asString() == a);
    for (const auto& dst : m->find("destinations")->asArray())
        if (dst.find("id")->asString() == ch) CHECK(dst.find("rule") != nullptr);
    // A -> B exists, so B -> A (and Vox -> B -> A is fine; B -> A feeds back) is listed.
    REQUIRE(m->find("sends")->size() == 1);
    CHECK(m->find("sends")->at(0).find("from")->asString() == a);
    bool cycle = false;
    for (const auto& c : m->find("cycles")->asArray())
        if (c.find("from")->asString() == b && c.find("to")->asString() == a) cycle = true;
    CHECK(cycle);
    // What the matrix allows, route.add accepts; what it lists as a cycle, route.add refuses.
    CHECK(d.execute({"route.add", j(R"({"from":")" + ch + R"(","to":")" + b + R"("})"), kTest}));
    auto refused =
        d.execute({"route.add", j(R"({"from":")" + b + R"(","to":")" + a + R"("})"), kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "route.cycle");
}

TEST_CASE("route.matrix: a rack's In modules are destinations; its own feed is a cycle") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](const char* name, const std::string& args) {
        auto r = d.execute({name, j(args), kTest});
        REQUIRE(r);
        return *r;
    };
    const std::string synth = call("track.add", R"({"name":"Synth"})").find("track")->asString();
    const std::string gtr = call("track.add", R"({"name":"Gtr"})").find("track")->asString();
    const std::string fx =
        call("track.add", R"({"type":"buss","name":"FX"})").find("track")->asString();
    const std::string rack =
        call("rack.add", R"({"track":")" + synth + R"("})").find("rack")->asString();
    const std::string in1 =
        call("module.add", R"({"rack":")" + rack + R"(","type":"in"})").find("module")->asString();
    const std::string in2 =
        call("module.add", R"({"rack":")" + rack + R"(","type":"in","label":"Side"})")
            .find("module")
            ->asString();
    // Synth feeds FX, so FX hears the rack too.
    call("route.add", R"({"from":")" + synth + R"(","to":")" + fx + R"("})");
    const std::string send = call("route.add", R"({"from":")" + gtr + R"(","to":")" + in1 + R"("})")
                                 .find("id")
                                 ->asString();
    const json::Value m = call("route.matrix", "{}");
    // The In modules follow their home channel, in id order, named after the
    // rack as the input menu names an Out.
    const auto& dst = m.find("destinations")->asArray();
    std::vector<std::string> ids;
    for (const auto& x : dst) ids.push_back(x.find("id")->asString());
    const auto home = std::find(ids.begin(), ids.end(), synth);
    REQUIRE(home != ids.end());
    REQUIRE(home + 2 < ids.end());
    CHECK(*(home + 1) == in1);
    CHECK(*(home + 2) == in2);
    const json::Value& first = dst.at(static_cast<std::size_t>(home - ids.begin()) + 1);
    CHECK(first.find("kind")->asString() == "module");
    CHECK(first.find("name")->asString() == "Synth \u00bb In 1");
    CHECK(first.find("rack")->asString() == rack);
    CHECK(first.find("track")->asString() == synth);
    CHECK(first.find("rule") == nullptr);
    CHECK(dst.at(static_cast<std::size_t>(home - ids.begin()) + 2).find("name")->asString() ==
          "Synth \u00bb Side");
    // The send into In 1 is listed; the rack's Out feeding its channel is not a send.
    bool listed = false;
    for (const auto& x : m.find("sends")->asArray()) {
        CHECK(x.find("from")->asString().starts_with("tr_"));
        if (x.find("id")->asString() == send) {
            listed = true;
            CHECK(x.find("from")->asString() == gtr);
            CHECK(x.find("to")->asString() == in1);
        }
    }
    CHECK(listed);
    // The rack's Outs reach Synth and, through it, FX: both are cycles; Gtr
    // is not; the master is left to its rule.
    auto cycleOf = [&](const std::string& from, const std::string& to) {
        for (const auto& c : m.find("cycles")->asArray())
            if (c.find("from")->asString() == from && c.find("to")->asString() == to) return true;
        return false;
    };
    CHECK(cycleOf(synth, in2));
    CHECK(cycleOf(fx, in2));
    CHECK_FALSE(cycleOf(gtr, in2));
    CHECK_FALSE(cycleOf("tr_0", in2));
    // The path by id and by name (the rack unnamed: its home's, marked).
    for (const auto& c : m.find("cycles")->asArray())
        if (c.find("from")->asString() == fx && c.find("to")->asString() == in2) {
            CHECK(c.find("path")->asString() ==
                  rack + " -> " + synth + " -> " + fx + " -> " + rack);
            CHECK(c.find("names")->asString() == "Synth (rack) -> Synth -> FX -> Synth (rack)");
        }
    // What the matrix allows, route.add accepts; what it blocks, route.add refuses,
    // naming the path with its ids.
    CHECK(d.execute({"route.add", j(R"({"from":")" + gtr + R"(","to":")" + in2 + R"("})"), kTest}));
    for (const std::string& from : {synth, fx}) {
        auto refused = d.execute(
            {"route.add", j(R"({"from":")" + from + R"(","to":")" + in2 + R"("})"), kTest});
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "route.cycle");
        CHECK(refused.error().message.starts_with("route creates a cycle: Synth (" + rack +
                                                  ") -> Synth (" + synth + ")"));
    }
    auto master = d.execute({"route.add", j(R"({"from":"tr_0","to":")" + in2 + R"("})"), kTest});
    REQUIRE_FALSE(master);
    CHECK(master.error().code == "route.strip_type");
}

TEST_CASE("track.remove: a rack another channel hears moves home there; others go") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](const char* name, const std::string& args) {
        auto r = d.execute({name, j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    auto add = [&](const char* name) {
        return call("track.add", std::string(R"({"name":")") + name + R"("})")
            .find("track")
            ->asString();
    };
    const std::string synth = add("Synth"), lead = add("Lead"), pad = add("Pad"), seq = add("Seq"),
                      bass = add("Bass");
    // Synth's rack also plays into Lead (Lead's input is its Out).
    const json::Value made = call("rack.add", R"({"track":")" + synth + R"("})");
    const std::string rack = made.find("rack")->asString(), out = made.find("out")->asString();
    call("track.set", R"({"track":")" + lead + R"(","record_rack":")" + out + R"("})");
    // Pad's rack plays only into Pad, its home.
    const std::string alone =
        call("rack.add", R"({"track":")" + pad + R"("})").find("rack")->asString();
    // Seq's rack plays only MIDI into Bass, through a Note Out.
    const std::string notes =
        call("rack.add", R"({"track":")" + seq + R"("})").find("rack")->asString();
    const std::string noteOut =
        call("module.add", R"({"rack":")" + notes + R"(","type":"notes_out"})")
            .find("module")
            ->asString();
    call("route.add",
         R"({"from":{"module":")" + noteOut + R"("},"to":{"track":")" + bass + R"("},"midi":{}})");
    const auto at = [](const std::string& id) { return *model::Id::parse(id); };

    call("track.remove", R"({"track":")" + synth + R"("})");
    const model::Rack* moved = app->head().racks.find(at(rack));
    REQUIRE(moved);
    CHECK(moved->track == at(lead));
    CHECK(moved->name == "Synth");  // the name it showed, not its new home's
    CHECK(app->head().modules.find(at(out)));
    const model::Track* l = app->head().tracks.find(at(lead));
    REQUIRE(l->record.input);
    CHECK(app->head().routes.find(*l->record.input)->from.module == at(out));
    // One undo step brings Synth and the rack's old home back.
    call("history.undo", "{}");
    CHECK(app->head().tracks.find(at(synth)));
    CHECK(app->head().racks.find(at(rack))->track == at(synth));
    CHECK(app->head().racks.find(at(rack))->name.empty());
    call("history.redo", "{}");

    call("track.remove", R"({"track":")" + seq + R"("})");
    REQUIRE(app->head().racks.find(at(notes)));
    CHECK(app->head().racks.find(at(notes))->track == at(bass));

    call("track.remove", R"({"track":")" + pad + R"("})");
    CHECK_FALSE(app->head().racks.find(at(alone)));
    // Removing the rack's last channel removes it.
    call("track.remove", R"({"track":")" + lead + R"("})");
    CHECK_FALSE(app->head().racks.find(at(rack)));
    CHECK_FALSE(app->head().modules.find(at(out)));
}

TEST_CASE("track.set: null takes off hardware audio or a rack's Out, each alone") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](const char* name, const std::string& args) {
        auto r = d.execute({name, j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    const std::string mic = call("track.add", R"({"name":"Mic"})").find("track")->asString();
    const json::Value made = call("rack.add", R"({"name":"Synth"})");
    const std::string synth = made.find("track")->asString(), out = made.find("out")->asString();
    const auto track = [&](const std::string& id) {
        return *app->head().tracks.find(*model::Id::parse(id));
    };
    const auto routes = [&] { return app->head().routes.size(); };
    const auto set = [&](const std::string& id, const std::string& fields) {
        call("track.set", R"({"track":")" + id + R"(",)" + fields + "}");
    };

    set(mic, R"("record_input":[1,2])");
    const auto withMic = routes();
    // A rack's Out is not hardware audio: record_rack: null leaves Mic's input.
    set(mic, R"("record_rack":null)");
    CHECK(track(mic).record.input);
    set(mic, R"("record_input":null)");
    CHECK_FALSE(track(mic).record.input);
    CHECK_FALSE(track(mic).record.midiInput);
    CHECK(routes() == withMic - 1);
    set(mic, R"("record_input":null)");  // nothing to take off
    CHECK(routes() == withMic - 1);

    // Synth: its rack's Out and a keyboard. Hardware audio's null leaves both;
    // the rack's takes off the Out, keeping the keyboard.
    set(synth, R"("record_midi":"*")");
    REQUIRE(track(synth).record.midiInput);
    set(synth, R"("record_input":null)");
    REQUIRE(track(synth).record.input);
    CHECK(app->head().routes.find(*track(synth).record.input)->from.module ==
          model::Id::parse(out));
    set(synth, R"("record_rack":null)");
    const model::Track after = track(synth);
    REQUIRE(after.record.midiInput);
    CHECK(app->head().routes.find(*after.record.midiInput)->from.midiDevice == "*");
    CHECK(after.record.input == after.record.midiInput);  // the MIDI-only alias
    // The rack stays, homed on Synth; then no MIDI input either: blank.
    CHECK(app->head().racks.find(*model::Id::parse(made.find("rack")->asString())));
    set(synth, R"("record_midi":null)");
    CHECK_FALSE(track(synth).record.input);
    CHECK_FALSE(track(synth).record.midiInput);
    // A value with a null is still one audio input at a time.
    auto both = d.execute(
        {"track.set",
         j(R"({"track":")" + synth + R"(","record_rack":")" + out + R"(","record_input":[1]})"),
         kTest});
    REQUIRE_FALSE(both);
    CHECK(both.error().code == "track.record_input");
    set(synth, R"("record_rack":")" + out + R"(","record_input":null)");
    CHECK(app->head().routes.find(*track(synth).record.input)->from.module ==
          model::Id::parse(out));
}

TEST_CASE("log.list: who did what, queries left out, since, origin filter, bounded") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    const cmd::Origin gui{cmd::Origin::Type::Ui, "gui", {}};
    const cmd::Origin agent{cmd::Origin::Type::Agent, "panel", "claude-x"};
    REQUIRE(d.execute({"track.add", j(R"({"name":"A"})"), gui}));
    REQUIRE(d.execute({"track.list", j("{}"), gui}));  // a query
    REQUIRE(d.execute({"track.add", j(R"({"name":"B"})"), agent}));
    REQUIRE_FALSE(d.execute({"track.set", j(R"({"track":"tr_99","mute":true})"), agent}));
    auto log = d.execute({"log.list", j("{}"), kTest});
    REQUIRE(log);
    const auto& e = *log->find("entries");
    REQUIRE(e.size() == 3);  // the query and log.list itself are left out
    CHECK(e.at(0).find("origin")->asString() == "ui:gui");
    CHECK(e.at(1).find("origin")->asString() == "agent:panel/claude-x");
    CHECK(e.at(2).find("ok")->asBool() == false);
    CHECK(e.at(2).find("error")->asString() == "tr.unknown");
    // since, origin, queries.
    const auto seq = e.at(1).find("seq")->asInt();
    json::Value since = json::Value::object();
    since["since"] = seq;
    CHECK(d.execute({"log.list", since, kTest})->find("entries")->size() == 1);
    CHECK(d.execute({"log.list", j(R"({"origin":"agent"})"), kTest})->find("entries")->size() == 2);
    CHECK(d.execute({"log.list", j(R"({"queries":true})"), kTest})->find("entries")->size() == 4);
    // Bounded.
    for (int i = 0; i < 2100; ++i) (void)d.execute({"project.get", j("{}"), kTest});
    CHECK(app->records().size() == engine::EngineApp::kMaxRecords);
}

TEST_CASE("track.set parent_send: off, a loop it would close is refused, back on") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto ok = [&](const char* command, const std::string& args) {
        auto r = d.execute({command, j(args), kTest});
        INFO(command << " " << args << ": " << (r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    };
    const std::string a = ok("track.add", R"({"type":"buss","name":"A"})")["track"].asString();
    const std::string b = ok("track.add", R"({"type":"buss","name":"B"})")["track"].asString();
    ok("track.move", R"({"track":")" + b + R"(","parent":")" + a + R"("})");
    // With B -> A as the parent send, A -> B would loop.
    CHECK_FALSE(
        d.execute({"route.add", j(R"({"from":")" + a + R"(","to":")" + b + R"("})"), kTest}));
    ok("track.set", R"({"track":")" + b + R"(","parent_send":false})");
    CHECK_FALSE(app->head().tracks.find(*model::Id::parse(b))->parentSend.enabled);
    const std::string send =
        ok("route.add", R"({"from":")" + a + R"(","to":")" + b + R"("})")["id"].asString();
    auto loop =
        d.execute({"track.set", j(R"({"track":")" + b + R"(","parent_send":true})"), kTest});
    REQUIRE_FALSE(loop);
    CHECK(loop.error().code == "route.cycle");
    CHECK_FALSE(app->head().tracks.find(*model::Id::parse(b))->parentSend.enabled);  // unchanged
    ok("route.remove", R"({"route":")" + send + R"("})");
    ok("track.set", R"({"track":")" + b + R"(","parent_send":true})");
    CHECK(app->head().tracks.find(*model::Id::parse(b))->parentSend.enabled);
    auto master = d.execute({"track.set", j(R"({"track":"tr_0","parent_send":false})"), kTest});
    REQUIRE_FALSE(master);
    CHECK(master.error().code == "track.parent_send");
}

TEST_CASE("engine.inputs: the record inputs while running, empty while stopped") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto stopped = d.execute({"engine.inputs", j("{}"), kTest});
    REQUIRE(stopped);
    CHECK_FALSE(stopped->find("running")->asBool());
    CHECK(stopped->find("audio")->size() == 0);
    REQUIRE(d.execute({"engine.start", j(R"({"backend":"dummy"})"), kTest}));
    auto inputs = d.execute({"engine.inputs", j("{}"), kTest});
    REQUIRE(inputs);
    CHECK(inputs->find("backend")->asString() == "dummy");
    REQUIRE(inputs->find("audio")->size() == 2);
    CHECK(inputs->find("audio")->at(1).find("channel")->asInt() == 2);
    REQUIRE(d.execute({"engine.stop", j("{}"), kTest}));
}

TEST_CASE(
    "automation writing: touch and latch record a strip's control while the transport plays") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name": "Lead"})");
    call("env.add",
         R"({"track": "tr_1", "param": "volume", "points": [{"beat": 0, "value": -6}]})");
    call("env.add", R"({"track": "tr_1", "param": "pan", "points": [{"beat": 0, "value": 0}]})");
    call("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain"})");
    call("env.add", R"({"fx": "fx_1", "param": "gain", "points": [{"beat": 0, "value": "0 dB"}]})");
    auto points = [&](const char* env) {
        return app->head().envelopes.find(*model::Id::parse(env))->points;
    };
    // Read mode, or stopped: nothing is written.
    call("track.set", R"({"track": "tr_1", "volume_db": -3})");
    CHECK(points("en_1").size() == 1);
    call("env.set", R"({"env": "en_1", "mode": "touch"})");
    call("env.set", R"({"env": "en_2", "mode": "latch"})");
    call("env.set", R"({"env": "en_3", "mode": "latch"})");
    call("track.set", R"({"track": "tr_1", "volume_db": -3})");  // stopped
    CHECK(points("en_1").size() == 1);

    call("engine.start", R"({"backend": "dummy"})");
    call("transport.play", "{}");
    auto waitBeats = [&](double beats) {
        const double until = call("transport.get", "{}")["beat"].asDouble() + beats;
        for (int i = 0; i < 4000 && call("transport.get", "{}")["beat"].asDouble() < until; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    waitBeats(0.5);
    call("track.set", R"({"track": "tr_1", "volume_db": -12, "pan": -50})");
    call("fx.param.set", R"({"fx": "fx_1", "values": {"gain": "-12 dB"}})");  // text, in its unit
    waitBeats(0.5);
    call("track.set", R"({"track": "tr_1", "volume_db": -20, "pan": 40})");
    waitBeats(0.5);
    const double stopBeat = call("transport.get", "{}")["beat"].asDouble();
    call("transport.stop", "{}");

    // Touch: the hold point before the first write, then the written values.
    const auto vol = points("en_1");
    REQUIRE(vol.size() == 4);  // 0 dB start point, hold, -12, -20
    CHECK(vol[1].value == doctest::Approx(-6));
    CHECK(vol[2].value == doctest::Approx(-12));
    CHECK(vol[3].value == doctest::Approx(-20));
    CHECK(vol[2].pos > 0.4);
    CHECK(vol[3].pos > vol[2].pos);
    // Latch: holds the last value to where the transport stopped.
    const auto pan = points("en_2");
    REQUIRE(pan.size() == 5);
    CHECK(pan.back().value == doctest::Approx(40));
    CHECK(pan.back().pos >= stopBeat - 1e-6);
    CHECK(pan.back().pos > pan[pan.size() - 2].pos);
    // A plugin parameter set through fx.param.set is recorded too (in its unit).
    const auto gain = points("en_3");
    REQUIRE(gain.size() == 4);  // start, hold, -12 dB, held to the stop
    CHECK(gain[2].value == doctest::Approx(-12));
    CHECK(gain.back().value == doctest::Approx(-12));
    // The stop's completion is one undo step of its own.
    auto history = call("history.list", "{}");
    CHECK(history["versions"].at(0).find("label")->asString() ==
          "Automation written");  // newest first
    call("engine.stop", "{}");
}

TEST_CASE("item.paste copies items as they were in a version, even after a cut") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name": "Keys"})");
    call("track.add", R"({"name": "Bass"})");
    call("item.add", R"({"track": "tr_1", "bar": 1, "notation": "voice v; 1: C4:q E4:q G4:h"})");
    call("item.set", R"({"item": "it_1", "fade_in": 0.05})");
    const std::uint64_t copied = app->head().version;  // "Ctrl+C" remembers this
    call("item.remove", R"({"item": "it_1"})");        // the cut
    REQUIRE_FALSE(app->head().items.find(*model::Id::parse("it_1")));
    auto pasted =
        call("item.paste", std::string(R"({"version": )") + std::to_string(copied) +
                               R"(, "items": [{"item": "it_1", "track": "tr_2", "position": 8}]})");
    REQUIRE(pasted["items"].size() == 1);
    const model::Session& s = app->head();
    const model::Item& item = *s.items.find(*model::Id::parse(pasted["items"].at(0).asString()));
    CHECK(item.track == *model::Id::parse("tr_2"));
    CHECK(item.position == doctest::Approx(8));
    CHECK(item.fadeIn.length == doctest::Approx(0.05));
    const model::Take& take = *s.takes.find(*item.activeTake);
    REQUIRE(take.source);
    CHECK(s.midi.find(*take.source)->sequence.notes.size() == 3);
    // Errors: a version that does not exist, a buss, an item not in that version.
    CHECK_FALSE(d.execute({"item.paste", j(R"({"version": 999999, "items": []})"), kTest}));
    call("track.add", R"({"name": "Bus", "type": "buss"})");
    auto onBus = d.execute({"item.paste",
                            j(std::string(R"({"version": )") + std::to_string(copied) +
                              R"(, "items": [{"item": "it_1", "track": "tr_3", "position": 0}]})"),
                            kTest});
    REQUIRE_FALSE(onBus);
    CHECK(onBus.error().code == "track.strip_type");
}

TEST_CASE("track templates: a strip's settings and chains saved, then added as a new strip") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name": "Lead Vox"})");
    call(
        "track.set",
        R"({"track": "tr_1", "volume_db": -4.5, "pan": -20, "color": "#ff8800", "monitor": "auto"})");
    call(
        "track.set",
        R"({"track": "tr_1", "pan_mode": "width", "width": 35, "pan_law": {"db": -4.5, "boost": true}})");
    call(
        "fx.add",
        R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain", "patch": {"params": {"gain": -6}}})");
    call(
        "fx.add",
        R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain", "chain": "input", "patch": {"params": {"gain": -3}}})");
    auto saved = call("track.template.save", R"({"track": "tr_1", "name": "Vocal"})");
    CHECK(saved["effects"].asInt() == 2);
    CHECK(call("track.template.list", "{}")["templates"].size() == 1);
    // Saving again without replace is refused.
    CHECK_FALSE(
        d.execute({"track.template.save", j(R"({"track": "tr_1", "name": "Vocal"})"), kTest}));
    const auto before = app->versions().history(1000).size();
    auto loaded = call("track.template.load", R"({"name": "Vocal"})");
    CHECK(app->versions().history(1000).size() == before + 1);  // one undo step
    const model::Session& s = app->head();
    const model::Track& strip = *s.tracks.find(*model::Id::parse(loaded["track"].asString()));
    CHECK(strip.name == "Lead Vox");
    CHECK(strip.volumeDb == doctest::Approx(-4.5));
    CHECK(strip.pan == doctest::Approx(-20));
    CHECK(strip.panMode == model::PanMode::Width);
    CHECK(strip.width == doctest::Approx(35));
    REQUIRE(strip.panLaw);
    CHECK(*strip.panLaw == model::PanLaw{-4.5, true});
    CHECK(strip.color == "#ff8800");
    CHECK(strip.record.monitor == model::Monitor::Auto);
    REQUIRE(strip.chain.size() == 1);
    REQUIRE(strip.inputChain.size() == 1);
    CHECK(loaded["effects"].size() == 2);
    auto gainOf = [&](model::Id fx) {
        auto patch = call("fx.patch.get", std::string(R"({"fx": ")") + fx.str() + R"("})");
        return patch["patch"]["params"]["gain"].asDouble();
    };
    CHECK(gainOf(strip.chain[0]) == doctest::Approx(-6));
    CHECK(gainOf(strip.inputChain[0]) == doctest::Approx(-3));
    // Errors: neither or both sources, an unknown name.
    CHECK_FALSE(d.execute({"track.template.load", j("{}"), kTest}));
    CHECK_FALSE(d.execute({"track.template.load", j(R"({"name": "Nope"})"), kTest}));
}

TEST_CASE("undo and redo reload the running plan and emit object version events") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto call = [&](std::string name, std::string_view args) {
        auto r = bus.execute({std::move(name), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", "{}");
    call("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    DummyBackend backend({});
    REQUIRE(app->startEngine(backend, {}, {.anticipative = false}));
    CHECK(app->engine()->outputLatency() == 0);
    call("fx.param.set", R"({"fx":"fx_1","values":{"latency":"short"}})");
    const auto latency = app->engine()->outputLatency();
    REQUIRE(latency > 0);
    app->takeEvents();
    call("history.undo", "{}");
    CHECK(app->engine()->outputLatency() == 0);
    call("history.redo", "{}");
    CHECK(app->engine()->outputLatency() == latency);
    int changes = 0;
    for (const auto& [topic, payload] : app->takeEvents()) {
        if (topic != "session.changed") continue;
        REQUIRE(payload.isObject());
        CHECK(*payload.find("branch") == "main");
        CHECK(payload.find("version")->isInt());
        ++changes;
    }
    CHECK(changes == 2);
    app->stopEngine();
}

TEST_CASE("project replacement joins jobs and drops stale render completions") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j(R"({"name":"Vocal"})"), kTest}));
    REQUIRE(bus.execute(
        {"track.set", j(R"({"track":"tr_1","armed":true,"record_input":[1]})"), kTest}));
    REQUIRE(app->save(root.path / "old.json"));
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app->startEngine(backend, {.blockSize = 128, .inputs = 1}, {.anticipative = false}));
    REQUIRE(bus.execute({"transport.record", j("{}"), kTest}));
    backend.run(8);
    auto target = renderTarget(*app);
    const auto id = bus.jobs().create();
    std::atomic<bool> joined{false};
    app->startJob([&](supple::StopToken stop) {
        while (!stop.stop_requested()) std::this_thread::yield();
        joined.store(true);
    });
    bool published = false;
    json::Value result = json::Value::object();
    result["path"] = "retained.wav";
    completeRenderJob(*app, bus, bus.jobs(), id, target, result, [&] { published = true; });
    REQUIRE(app->newProject({.name = "Fresh"}));
    auto recording = audio::readAudio(root.path / "media" / "rec" / "Vocal-1.wav");
    REQUIRE(recording);
    CHECK(recording->frames() == 1024);
    CHECK(app->head().items.empty());
    REQUIRE(app->engine());
    CHECK_FALSE(app->engine()->recording());
    CHECK(joined.load());
    CHECK(app->projectGeneration() == target.generation + 1);
    CHECK(app->engine()->config().blockSize == 128);
    bus.flushJobEvents();
    CHECK_FALSE(published);
    auto replaced = bus.jobs().status(id);
    REQUIRE(replaced);
    CHECK(*replaced->find("status") == "cancelled");
    REQUIRE(replaced->find("result"));
    CHECK(*replaced->find("result")->find("status") == "render.stale_target");
    bool stale = false;
    for (const auto& [topic, payload] : app->takeEvents())
        if (topic == "render.stale_target") stale = true;
    CHECK(stale);
    const auto branchTarget = renderTarget(*app);
    REQUIRE(app->versions().createBranch("other", "main"));
    const auto branchJob = bus.jobs().create();
    completeRenderJob(*app, bus, bus.jobs(), branchJob, branchTarget, result,
                      [&] { published = true; });
    REQUIRE(app->switchBranch("other"));
    bus.flushJobEvents();
    CHECK_FALSE(published);
    auto status = bus.jobs().status(branchJob);
    REQUIRE(status);
    CHECK(*status->find("result")->find("status") == "render.stale_target");
    CHECK(*status->find("result")->find("path") == "retained.wav");
    app->stopEngine();
}

TEST_CASE("first save rolls back staged media when project publication fails") {
    Root root;
    AppOptions options;
    options.workdir = root.path / "work";
    options.scratchDir = root.path / "scratch";
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    REQUIRE(writeFileAtomic(app->mediaDir() / "a.wav", "audio bytes"));
    const auto project = root.path / "saved" / "project.json";
    fs::create_directories(project);  // rename over a directory must fail
    REQUIRE_FALSE(app->save(project));
    CHECK(app->projectPath().empty());
    CHECK(fs::exists(app->mediaDir() / "a.wav"));
    CHECK_FALSE(fs::exists(project.parent_path() / "media" / "a.wav"));
    fs::remove(project);
    REQUIRE(app->save(project));
    CHECK(app->projectPath() == project);
    CHECK(fs::exists(project.parent_path() / "media" / "a.wav"));
    CHECK_FALSE(fs::exists(options.scratchDir / "media" / "a.wav"));
}

TEST_CASE("latency probes restore semantic patch and standard params") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j("{}"), kTest}));
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test Gain"})"), kTest}));
    REQUIRE(
        bus.execute({"fx.param.set", j(R"({"fx":"fx_1","values":{"latency":"short"}})"), kTest}));
    auto latency = app->fxLatencySamples(*model::Id::parse("fx_1"), *app->head().fx.begin()->value);
    REQUIRE(latency);
    CHECK(*latency > 0);
}

TEST_CASE("plain plugin parameter setters validate names ids text and saved state") {
    Root root;
    AppOptions options;
    options.workdir = root.path;
    options.pluginPath = {kFixture, FIXTURE_VST3_PATH};
    auto made = EngineApp::create(options, {.console9 = false});
    REQUIRE(made);
    auto app = std::move(*made);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j("{}"), kTest}));
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test VST3 Gain"})"), kTest}));
    REQUIRE(bus.execute({"fx.param.set", j(R"({"fx":"fx_1","values":{"Gain":0.25}})"), kTest}));
    const auto* fx = app->head().fx.find(*model::Id::parse("fx_1"));
    REQUIRE(fx->state);
    CHECK(fx->params.at(1) == doctest::Approx(0.25));
    auto info = app->findPlugin(fx->plugin.id);
    REQUIRE(info);
    auto instance = host::Instance::create(*info, {});
    REQUIRE(instance);
    REQUIRE((*instance)->loadState(app->head().blobs.at(*fx->state)->data));
    CHECK((*instance)->paramValue(1).value() == doctest::Approx(0.25));
    auto text = (*instance)->paramText(1, 0.5);
    REQUIRE(text);
    json::Value args = j(R"({"fx":"fx_1","values":{}})");
    args["values"]["1"] = *text;
    REQUIRE(bus.execute({"fx.param.set", args, kTest}));
    CHECK(app->head().fx.find(*model::Id::parse("fx_1"))->params.at(1) == doctest::Approx(0.5));
    const auto before = model::serializeProject(app->head());
    for (const auto* values : {R"({"Gain":2})", R"({"Missing":0})", R"({"Gain":false})"}) {
        args["values"] = j(values);
        REQUIRE_FALSE(bus.execute({"fx.param.set", args, kTest}));
        CHECK(model::serializeProject(app->head()) == before);
    }
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test Edge"})"), kTest}));
    auto readonly = bus.execute({"fx.param.set", j(R"({"fx":"fx_2","values":{"7":1}})"), kTest});
    REQUIRE_FALSE(readonly);
    CHECK(readonly.error().code == "fx.param_readonly");
}

TEST_CASE("recovery reports corrupt sidecars and recovers the valid selection") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "recording_interrupted");
    const auto corrupt = app->recordDir() / "Broken-1.wav";
    fs::copy_file(app->recordDir() / "Vocal-1.wav", corrupt);
    auto sidecar = corrupt;
    sidecar.replace_extension(".rec.json");
    REQUIRE(writeFileAtomic(sidecar, "{broken"));
    auto listed = d.execute({"project.recoverable", j("{}"), kTest});
    REQUIRE(listed);
    REQUIRE((*listed)["recordings"].size() == 1);
    REQUIRE((*listed)["warnings"].size() == 1);
    CHECK((*listed)["warnings"].at(0).find("code")->asString() == "record.sidecar");
    CHECK((*listed)["warnings"].at(0).find("file")->asString() == "rec/Broken-1.wav");
    auto recovered = d.execute(
        {"project.recover", j(R"({"files":["rec/Broken-1.wav","rec/Vocal-1.wav"]})"), kTest});
    REQUIRE(recovered);
    REQUIRE((*recovered)["recovered"].size() == 1);
    REQUIRE((*recovered)["warnings"].size() == 1);
    CHECK((*recovered)["warnings"].at(0).find("code")->asString() == "record.sidecar");
    CHECK_FALSE((*recovered)["warnings"].at(0).find("hint")->asString().empty());
    CHECK(app->head().items.size() == 1);
}

TEST_CASE("recording loop capacity warning reaches transport.stop and retained passes commit") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *app);
    REQUIRE(d.execute({"track.add", j(R"({"name":"Input"})"), kTest}));
    REQUIRE(
        d.execute({"track.set", j(R"({"track":"tr_1","armed":true,"record_input":[1]})"), kTest}));
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 1},
                             {.anticipative = false}));
    REQUIRE(d.execute({"transport.loop",
                       j(R"({"start_seconds":0,"end_seconds":0.0013333333333333333})"), kTest}));
    REQUIRE(d.execute({"transport.record", j("{}"), kTest}));
    backend.run(4100);
    auto stopped = d.execute({"transport.stop", j("{}"), kTest});
    REQUIRE_MESSAGE(stopped, (stopped ? "" : stopped.error().toString()));
    REQUIRE((*stopped)["recorded"].size() == 1);
    auto info = (*stopped)["recorded"].at(0);
    REQUIRE(info["warnings"].size() == 1);
    auto warning = info["warnings"].at(0);
    CHECK(warning["code"].asString() == "recording.loop_runs_full");
    CHECK(warning["path"].asString() == "/recording");
    CHECK_FALSE(warning["hint"].asString().empty());
    CHECK(warning["track"].asString() == "tr_1");
    CHECK_FALSE(warning["file"].asString().empty());
    CHECK(info["takes"].size() > 1);
    CHECK(app->head().items.size() == 1);
    app->stopEngine();
}

TEST_CASE("recovery skips an overflowing recording start beside a valid file") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "recording_interrupted");
    const auto invalid = app->recordDir() / "Overflow-1.wav";
    fs::copy_file(app->recordDir() / "Vocal-1.wav", invalid);
    auto sidecar = invalid;
    sidecar.replace_extension(".rec.json");
    REQUIRE(writeFileAtomic(
        sidecar,
        R"({"schema":"supple.recording/1.0","track":"tr_1","sample_rate":48000,"start_sample":9223372036854775807})"));
    auto r = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE(r);
    REQUIRE((*r)["warnings"].size() == 1);
    CHECK((*r)["warnings"].at(0).find("code")->asString() == "record.sidecar");
    CHECK((*r)["warnings"].at(0).find("path")->asString() == "/start_sample");
    REQUIRE((*r)["recovered"].size() == 1);
    CHECK(app->head().items.size() == 1);
}

TEST_CASE("fx.copy takes the running plugin's state, not the model's") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = bus.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name":"A"})");
    call("track.add", R"({"name":"B"})");
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    backend.run(1);
    app->tick();
    auto* instance = app->instanceOf(*model::Id::parse("fx_1"));
    REQUIRE(instance);
    // A change only the plugin knows about (the model still has no state).
    const std::vector<std::uint8_t> live{0, 9, 0, 0};
    REQUIRE(instance->loadState(live));
    REQUIRE_FALSE(app->head().fx.find(*model::Id::parse("fx_1"))->state);
    auto copied = call("fx.copy", R"({"fx":"fx_1","track":"tr_2"})");
    const auto* copy = app->head().fx.find(*model::Id::parse(copied["fx"].asString()));
    REQUIRE(copy);
    REQUIRE(copy->state);
    CHECK(app->head().blobs.at(*copy->state)->data == live);
    app->stopEngine();  // before the backend goes out of scope
}

TEST_CASE("app consumes editor gestures as one undo step without echoing to the live instance") {
    for (std::uint8_t mode : {30, 31, 32, 35}) {
        Root root;
        auto app = makeApp(root.path);
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, *app);
        auto call = [&](std::string command, std::string_view args) {
            auto r = bus.execute({std::move(command), j(args), kTest});
            REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
            return *r;
        };
        call("track.add", R"({"name":"Editor"})");
        call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})");
        const auto fx = *model::Id::parse("fx_1");
        auto setup = app->beginTransaction(kTest, "Fixture mode");
        REQUIRE(setup);
        std::vector<std::uint8_t> state{mode, 0, 0, 0};
        const auto hash = "sha256:" + sha256Hex(state);
        app->txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{state}));
        app->txn().modify<model::FxInstance>(fx, [&](auto& f) { f.state = hash; });
        REQUIRE(app->commitTransaction(*setup));
        if (mode == 31 || mode == 35) {
            call("env.add", R"({"fx":"fx_1","param":"7","points":[{"beat":0,"value":0}]})");
            call("env.set", R"({"env":"en_1","mode":"touch"})");
        }
        if (mode == 35) call("branch.create", R"({"name":"editor"})");
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64},
                                 {.anticipative = false}));
        REQUIRE(app->transport("play"));
        backend.run(1);
        app->tick();
        auto* instance = app->instanceOf(fx);
        REQUIRE(instance);
        REQUIRE(instance->openEditor(1234, 1));  // display-free fixture editor
        const auto version = app->head().version;
        const auto history = app->versions().history().size();
        const auto deliveries = *instance->paramValue(8);
        backend.run(1);
        app->tick();
        if (mode != 32) CHECK(app->head().version == version);  // unfinished gesture
        backend.run(1);
        app->tick();
        REQUIRE(app->head().fx.find(fx)->params.count(7));
        CHECK(app->head().fx.find(fx)->params.at(7) == 10);
        CHECK(app->instanceOf(fx) == instance);  // saved output state was acknowledged
        if (mode != 32) CHECK(app->versions().history().size() == history + 1);
        if (mode == 30) CHECK(*instance->paramValue(8) == deliveries);
        if (mode == 31) {
            REQUIRE(app->head().envelopes.size() == 1);
            CHECK(app->head().envelopes.begin()->value->points.size() == 1);
        }
        if (mode == 35) {
            REQUIRE(app->head().envelopes.size() == 1);
            const auto& points = app->head().envelopes.begin()->value->points;
            REQUIRE(points.size() > 1);
            CHECK(points.back().value == 10);
            CHECK(points.back().pos == doctest::Approx(128.0 / 24000));
        }
        if (mode == 32) CHECK(app->engineStatus().dropped > 0);
        instance->closeEditor();
        app->stopEngine();
        if (mode == 35) {
            call("branch.switch", R"({"name":"main"})");
            call("track.set", R"({"track":"tr_1","name":"Concurrent unrelated edit"})");
            call("branch.merge", R"({"from":"editor"})");
            CHECK(app->head().fx.find(fx)->params.at(7) == 10);
            CHECK(app->head().envelopes.begin()->value->points.back().value == 10);
        }
        if (mode != 32) {
            call("history.undo", "{}");
            CHECK(app->head().fx.find(fx)->params.count(7) == 0);
            call("history.redo", "{}");
            CHECK(app->head().fx.find(fx)->params.at(7) == 10);
        }
    }
}

TEST_CASE("app applies stopped editor flushes immediately and discards obsolete gesture outputs") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j(R"({"name":"Editor"})"), kTest}));
    REQUIRE(bus.execute(
        {"fx.add", j(R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})"), kTest}));
    const auto fx = *model::Id::parse("fx_1");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    auto* instance = app->instanceOf(fx);
    instance->deactivate();  // inactive params.flush is the main-thread idle path
    REQUIRE(instance->loadState(std::array<std::uint8_t, 4>{30, 0, 0, 0}));
    REQUIRE(instance->openEditor(1234, 1));
    const auto version = app->head().version;
    app->tick();
    CHECK(app->head().version != version);
    REQUIRE(app->head().fx.find(fx)->params.count(7));
    CHECK(app->head().fx.find(fx)->params.at(7) == 10);
    CHECK(app->instanceOf(fx) == instance);
    CHECK(*instance->paramValue(8) == 0);
    instance->closeEditor();
    REQUIRE(app->transport("play"));
    backend.run(1);
    REQUIRE(instance->openEditor(1234, 1));
    backend.run(1);  // queued begin/value, then a seek abandons this epoch
    REQUIRE(app->transport("seek", 2));
    const auto afterSeek = app->head().version;
    app->tick();
    CHECK(app->head().version == afterSeek);
    instance->closeEditor();
    app->stopEngine();
}

TEST_CASE("app keeps read-only plug-in outputs out of the model and history") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j(R"({"name":"Meter"})"), kTest}));
    REQUIRE(bus.execute(
        {"fx.add", j(R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})"), kTest}));
    const auto fx = *model::Id::parse("fx_1");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    auto* instance = app->instanceOf(fx);
    instance->deactivate();
    REQUIRE(instance->loadState(std::array<std::uint8_t, 4>{33, 0, 0, 0}));
    REQUIRE(instance->openEditor(1234, 1));
    const auto version = app->head().version;
    app->tick();  // the editor's idle flush reports the read-only parameter
    CHECK(app->head().version == version);
    CHECK_FALSE(app->head().fx.find(fx)->params.count(7));
    instance->closeEditor();
    app->stopEngine();
}

TEST_CASE("app converts semantic output values to physical patches without setter feedback") {
    struct Control {
        void (*edit)(const clap_plugin_t*, double, std::uint32_t);
        std::uint32_t (*deliveries)(const clap_plugin_t*);
    };
    for (bool playing : {false, true}) {
        Root root;
        auto app = makeApp(root.path);
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, *app);
        REQUIRE(bus.execute({"track.add", j(R"({"name":"Gain"})"), kTest}));
        REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test Gain"})"), kTest}));
        const auto fx = *model::Id::parse("fx_1");
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64},
                                 {.anticipative = false}));
        if (playing) {
            REQUIRE(app->transport("play"));
            backend.run(1);
            app->tick();
        }
        auto* instance = app->instanceOf(fx);
        const auto* plugin = instance->raw();
        const auto* control =
            static_cast<const Control*>(plugin->get_extension(plugin, "supple.test.gain-control"));
        REQUIRE(control);
        const auto deliveries = control->deliveries(plugin);
        const auto history = app->versions().history().size();
        control->edit(plugin, -12, host::OutputEvent::IsLive | host::OutputEvent::DontRecord);
        backend.run(1);  // active stopped flushes also need their audio owner
        app->tick();
        for (const auto& [topic, payload] : app->takeEvents()) {
            if (topic == "engine.error") MESSAGE(json::writeCompact(payload));
        }
        CAPTURE(playing);
        REQUIRE(app->head().fx.find(fx)->patch);
        CHECK(app->head().fx.find(fx)->patch->find("params")->find("gain")->asDouble() ==
              doctest::Approx(-12));
        CHECK(app->versions().history().size() == history + 1);
        CHECK(control->deliveries(plugin) == deliveries);
        app->stopEngine();
    }
}

TEST_CASE("app rejects plug-in outputs from a replaced plan and a removed FX") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j(R"({"name":"Editor"})"), kTest}));
    REQUIRE(bus.execute(
        {"fx.add", j(R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})"), kTest}));
    const auto fx = *model::Id::parse("fx_1");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    auto* instance = app->instanceOf(fx);
    REQUIRE(instance->loadState(std::array<std::uint8_t, 4>{30, 0, 0, 0}));
    REQUIRE(app->transport("play"));
    backend.run(1);
    REQUIRE(instance->openEditor(1234, 1));
    backend.run(1);  // an undrained begin/value belongs to the older plan
    REQUIRE(bus.execute({"track.set", j(R"({"track":"tr_1","name":"New plan"})"), kTest}));
    const auto version = app->head().version;
    app->tick();
    CHECK(app->head().version == version);
    CHECK(app->head().fx.find(fx)->params.empty());
    backend.run(1);
    REQUIRE(bus.execute({"fx.remove", j(R"({"fx":"fx_1"})"), kTest}));
    const auto removed = app->head().version;
    app->tick();
    CHECK(app->head().version == removed);
    CHECK_FALSE(app->head().fx.contains(fx));
    app->stopEngine();
}

TEST_CASE("app holds anticipative plug-in edits until their stream position has played") {
    struct Control {
        void (*edit)(const clap_plugin_t*, double, std::uint32_t);
        std::uint32_t (*deliveries)(const clap_plugin_t*);
    };
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j(R"({"name":"Ahead"})"), kTest}));
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test Gain"})"), kTest}));
    const auto fx = *model::Id::parse("fx_1");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app->startEngine(
        backend, {.sampleRate = 48000, .blockSize = 64},
        {.anticipative = true, .renderBlock = 64, .renderAheadMs = 4, .workers = 0}));
    REQUIRE(app->transport("play"));
    auto& engine = *app->engine();
    REQUIRE(engine.awaitRendered(1 << 20));
    engine.holdRenderer(true);
    REQUIRE(engine.awaitRendererHeld());
    backend.run(1);
    auto* instance = app->instanceOf(fx);
    const auto* plugin = instance->raw();
    const auto* control =
        static_cast<const Control*>(plugin->get_extension(plugin, "supple.test.gain-control"));
    REQUIRE(control);
    control->edit(plugin, -9, host::OutputEvent::IsLive);
    engine.holdRenderer(false);
    REQUIRE(engine.awaitRendered(1 << 20));
    engine.holdRenderer(true);
    REQUIRE(engine.awaitRendererHeld());
    const auto version = app->head().version;
    app->tick();
    CHECK(app->head().version == version);  // output was rendered ahead
    backend.run(16);                        // renderer clamps render-ahead to at least 10 ms
    app->tick();
    REQUIRE(app->head().fx.find(fx)->patch);
    CHECK(app->head().fx.find(fx)->patch->find("params")->find("gain")->asDouble() ==
          doctest::Approx(-9));
    engine.holdRenderer(false);
    app->stopEngine();
}

TEST_CASE("a plug-in's state change beyond its parameters reaches the project, and every save") {
    // A sampler loading a file from its editor changes no parameter; it calls
    // clap.state mark_dirty (the edge fixture's markDirty does the same).
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", "{}");
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    backend.run(2);
    const auto fx = *model::Id::parse("fx_1");
    host::Instance* instance = app->instanceOf(fx);
    REQUIRE(instance);
    struct Control {
        void (*requestProcess)(const clap_plugin_t*);
        void (*rescan)(const clap_plugin_t*, bool);
        void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
        void (*restart)(const clap_plugin_t*);
        void (*markDirty)(const clap_plugin_t*, std::uint8_t);
    };
    const auto* control = static_cast<const Control*>(
        instance->raw()->get_extension(instance->raw(), "supple.test.edge-control"));
    REQUIRE(control);
    auto blob = [&] {
        const auto& state = app->head().fx.find(fx)->state;
        REQUIRE(state);
        return app->head().blobs.at(*state)->data;
    };
    const auto steps = [&] { return call("history.list", "{}")["versions"].size(); };
    const auto before = steps();
    control->markDirty(instance->raw(), 7);
    app->tick();
    CHECK(blob() == std::vector<std::uint8_t>{0, 0, 0, 7});
    CHECK(steps() == before + 1);
    CHECK(app->instanceOf(fx) == instance);  // the same instance: nothing reloaded
    // Another change to the same plug-in joins that undo step.
    control->markDirty(instance->raw(), 8);
    app->tick();
    CHECK(blob() == std::vector<std::uint8_t>{0, 0, 0, 8});
    CHECK(steps() == before + 1);
    // A change it did not report is saved with the project anyway.
    REQUIRE(instance->loadState(std::vector<std::uint8_t>{0, 0, 0, 9}));
    call("project.save", R"({"path":"state.json"})");
    CHECK(blob() == std::vector<std::uint8_t>{0, 0, 0, 9});
    auto text = readTextFile(root.path / "state.json");
    REQUIRE(text);
    auto reopened = model::parseProject(*text);
    REQUIRE(reopened);
    const auto& saved = reopened->fx.find(fx)->state;
    REQUIRE(saved);
    CHECK(reopened->blobs.at(*saved)->data == std::vector<std::uint8_t>{0, 0, 0, 9});
    // A state the plug-in cannot report (the fixture refuses with 7 first) is
    // named in the save's warnings, and the file keeps the state it had.
    REQUIRE(instance->loadState(std::vector<std::uint8_t>{7, 0, 0, 0}));
    const auto result = call("project.save", "{}");
    const json::Value* warnings = result.find("warnings");
    REQUIRE(warnings);
    REQUIRE(warnings->size() == 1);
    const json::Value& warning = warnings->asArray()[0];
    CHECK(warning.find("code")->asString() == "fx.state");
    CHECK(warning.find("path")->asString() == "/fx/fx_1");
    CHECK(blob() == std::vector<std::uint8_t>{0, 0, 0, 9});
    // The same for a project template.
    const auto templ = call("project.template.save", R"({"name":"stuck"})");
    REQUIRE(templ.find("warnings"));
    CHECK(templ.find("warnings")->size() == 1);
    app->stopEngine();
}

TEST_CASE("automation timebases govern evaluation erasure guards and stop holds") {
    for (auto timebase : {model::Timebase::Beats, model::Timebase::Time})
        for (auto mode : {model::EnvMode::Touch, model::EnvMode::Latch, model::EnvMode::Write}) {
            Root root;
            auto app = makeApp(root.path);
            cmd::Registry registry;
            registerEngineCommands(registry);
            cmd::Dispatcher bus(registry, *app);
            auto call = [&](const char* command, const char* args) {
                auto result = bus.execute({command, j(args), kTest});
                REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
            };
            call("track.add", "{}");
            call("env.add", R"({"track":"tr_1","param":"volume"})");
            const model::Id env{model::Kind::Envelope, 1};
            const double domain = timebase == model::Timebase::Time ? 1 : 2;
            auto setup = app->beginTransaction(kTest, "Envelope clock");
            REQUIRE(setup);
            app->txn().modify<model::Envelope>(env, [&](auto& e) {
                e.timebase = timebase;
                e.mode = mode;
                e.points = {
                    {0, -6}, {1.25 * domain, -16}, {1.75 * domain, -18}, {2.75 * domain, -21}};
            });
            REQUIRE(app->commitTransaction(*setup));
            DummyBackend backend({.mode = DummyBackend::Mode::Manual});
            REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 480},
                                     {.anticipative = false}));
            call("transport.play", "{}");
            backend.run(100);
            if (mode != model::EnvMode::Write) {
                call("track.set", R"({"track":"tr_1","volume_db":-12})");
                const auto& points = app->head().envelopes.find(env)->points;
                const auto hold = std::find_if(points.begin(), points.end(), [&](const auto& p) {
                    return p.pos > .99 * domain && p.pos < domain;
                });
                CHECK(hold != points.end());
                if (hold != points.end()) CHECK(hold->value == doctest::Approx(-14));
            }
            backend.run(50);
            if (mode != model::EnvMode::Write)
                call("track.set", R"({"track":"tr_1","volume_db":-20})");
            backend.run(50);
            call("transport.stop", "{}");
            const auto& points = app->head().envelopes.find(env)->points;
            auto has = [&](double pos, double value) {
                return std::any_of(points.begin(), points.end(), [&](const auto& p) {
                    return std::abs(p.pos - pos) < 1e-9 && p.value == value;
                });
            };
            CHECK(has(2.75 * domain, -21));
            CHECK_FALSE(has(1.25 * domain, -16));
            if (mode == model::EnvMode::Touch) {
                CHECK(has(1.5 * domain, -20));
                CHECK_FALSE(has(2 * domain, -20));
                CHECK(has(1.75 * domain, -18));
            } else {
                CHECK_FALSE(has(1.75 * domain, -18));
                CHECK(has(2 * domain, mode == model::EnvMode::Write ? 0 : -20));
            }
            app->stopEngine();
        }
}

TEST_CASE("automation timebases preserve plugin output positions through branch replay") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto call = [&](const char* command, const char* args) {
        auto result = bus.execute({command, j(args), kTest});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    };
    call("track.add", "{}");
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.edge"})");
    call("env.add", R"({"fx":"fx_1","param":"7","points":[{"beat":0,"value":0}]})");
    const model::Id fx{model::Kind::Fx, 1}, env{model::Kind::Envelope, 1};
    auto setup = app->beginTransaction(kTest, "Timed editor output");
    REQUIRE(setup);
    std::vector<std::uint8_t> state{35, 0, 0, 0};
    const auto hash = "sha256:" + sha256Hex(state);
    app->txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{state}));
    app->txn().modify<model::FxInstance>(fx, [&](auto& f) { f.state = hash; });
    app->txn().modify<model::Envelope>(env, [](auto& e) {
        e.timebase = model::Timebase::Time;
        e.mode = model::EnvMode::Touch;
    });
    REQUIRE(app->commitTransaction(*setup));
    call("branch.create", R"({"name":"timed"})");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    REQUIRE(app->transport("play"));
    backend.run(1);
    app->tick();
    auto* instance = app->instanceOf(fx);
    REQUIRE(instance);
    REQUIRE(instance->openEditor(1234, 1));
    backend.run(1);
    app->tick();
    backend.run(1);
    app->tick();
    CHECK(app->head().envelopes.find(env)->points.back().pos == doctest::Approx(128.0 / 48000));
    instance->closeEditor();
    app->stopEngine();
    call("branch.switch", R"({"name":"main"})");
    call("track.set", R"({"track":"tr_1","name":"Unrelated"})");
    call("branch.merge", R"({"from":"timed"})");
    CHECK(app->head().envelopes.find(env)->points.back().pos == doctest::Approx(128.0 / 48000));
}

TEST_CASE("audio thread preference applies at next start and explicit options win") {
    Root root;
    auto app = makeApp(root.path);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app->setAudioThreadLimit(2));
    REQUIRE(app->startEngine(backend, {}));
    CHECK(app->engine()->options().workers == 1);
    REQUIRE(app->setAudioThreadLimit(8));
    CHECK(app->engine()->options().workers == 1);  // no mid-job resize
    auto invalid = app->setAudioThreadLimit(65);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().path == "/audio_threads");
    CHECK_FALSE(invalid.error().hint.empty());
    CHECK(app->audioThreadLimit() == 8);
    REQUIRE(app->newProject({.console9 = false}));  // this restarts the engine too
    CHECK(app->engine()->options().workers == 7);
    app->stopEngine();
    REQUIRE(app->startEngine(backend, {}));
    CHECK(app->engine()->options().workers == 7);
    app->stopEngine();
    REQUIRE(app->startEngine(backend, {}, {.workers = 0}));
    CHECK(app->engine()->options().workers == 0);
    CHECK(app->engineStatus().liveHelpers == 0);
    REQUIRE(app->newProject({.console9 = false}));
    CHECK(app->engine()->options().workers == 0);
    CHECK(app->engineStatus().liveHelpers == 0);  // explicit stays explicit
    app->stopEngine();
    REQUIRE(app->setAudioThreadLimit(0));
    REQUIRE(app->startEngine(backend, {}));
    CHECK(app->engine()->options().workers == -1);
    app->stopEngine();
    // engine.start (including the GUI's startup) uses the same app default.
    REQUIRE(app->setAudioThreadLimit(1));
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"engine.start", j(R"({"backend":"dummy"})"), kTest}));
    CHECK(app->engine()->options().workers == 0);
    CHECK(app->engineStatus().liveHelpers == 0);
    REQUIRE(bus.execute({"engine.stop", j("{}"), kTest}));
}

TEST_CASE("device recovery preserves an Automatic thread preference on a supplied backend") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    Root root;
    auto app = makeApp(root.path);
    JackBackend backend({.library = FakeJack::kPath});
    app->setFixedClock(1'000'000);
    REQUIRE(app->setAudioThreadLimit(2));
    REQUIRE(app->startEngine(backend, {}));
    CHECK(app->engine()->options().workers == 1);
    REQUIRE(app->setAudioThreadLimit(4));
    fake.setRate(44100);
    app->tick();
    CHECK_FALSE(app->engine());
    app->tick();
    REQUIRE(app->engine());
    CHECK(app->engine()->options().workers == 3);
    app->stopEngine();
    fake.reset();
}

TEST_CASE("failed initial load rolls back a supplied backend and permits another start") {
    Root root;
    auto app = makeApp(root.path);
    const auto good = app->head();
    auto bad = good;
    bad.version = app->versions().nextVersion();
    // Inject an invalid graph below the model boundary to force an initial-load error.
    model::IdMap<model::Route>::Builder routes(bad.routes);
    model::Route route;
    route.from.track = model::Id{model::Kind::Track, 0};
    route.to.track = route.from.track;
    const model::Id id{model::Kind::Route, 1};
    routes.put(id, bad.version, std::make_shared<const model::Route>(route));
    bad.routes = routes.build();
    REQUIRE(app->versions().commit(std::move(bad), {}, {}, {}));
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    const auto failed = app->startEngine(backend, {}, {.anticipative = false});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "engine.cycle");
    CHECK_FALSE(app->engine());
    CHECK_FALSE(app->backend());
    CHECK_FALSE(app->engineStatus().running);
    auto repaired = good;
    repaired.version = app->versions().nextVersion();
    REQUIRE(app->versions().commit(std::move(repaired), {}, {}, {}));
    REQUIRE(app->startEngine(backend, {}, {.anticipative = false}));
    CHECK(app->engineStatus().running);
    app->stopEngine();
}

TEST_CASE("app service subscriptions keep local instance callbacks alive after engine stop") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    app->setupFixture(bus, "song_with_fx");
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    const model::Id fx{model::Kind::Fx, 1};
    auto* instance = app->instanceOf(fx);
    REQUIRE(instance);
    app->stopEngine();
    CHECK_FALSE(app->nextServiceDelayMs());
    // Use an edge fixture so its host callbacks and timer extension are visible.
    auto info = app->findPlugin("com.suppleaudio.test.edge");
    REQUIRE(info);
    auto transaction = app->beginTransaction(kTest, "Local subscriptions");
    REQUIRE(transaction);
    app->txn().modify<model::FxInstance>(fx, [&](auto& value) {
        value.plugin.id = info->id;
        value.patch.reset();
        value.state.reset();
    });
    REQUIRE(app->commitTransaction(*transaction));
    REQUIRE(
        app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64}, {.anticipative = false}));
    instance = app->instanceOf(fx);
    REQUIRE(instance);
    app->stopEngine();
    const auto* plugin = instance->raw();
    const auto* host =
        static_cast<const clap_host_t*>(plugin->get_extension(plugin, "supple.test.edge-host"));
    REQUIRE(host);
    const auto* timers = static_cast<const clap_host_timer_support_t*>(
        host->get_extension(host, CLAP_EXT_TIMER_SUPPORT));
    const auto* descriptors = static_cast<const clap_host_posix_fd_support_t*>(
        host->get_extension(host, CLAP_EXT_POSIX_FD_SUPPORT));
    clap_id timer = CLAP_INVALID_ID;
    REQUIRE(timers->register_timer(host, 3'600'000, &timer));
    REQUIRE(app->nextServiceDelayMs());
    REQUIRE(timers->unregister_timer(host, timer));
    CHECK_FALSE(app->nextServiceDelayMs());
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    REQUIRE(descriptors->register_fd(host, fds[0], CLAP_POSIX_FD_READ));
    auto watches = app->serviceFds();
    REQUIRE(watches.size() == 1);
    CHECK(watches[0].owner == instance);
    CHECK(watches[0].fd == fds[0]);
    REQUIRE(descriptors->unregister_fd(host, fds[0]));
    CHECK(app->serviceFds().empty());
    app->serviceFdReady(watches[0], POLLIN);  // stale snapshots are harmless
    // Main-thread callback request is delivered by explicit service, with no device/editor.
    host->request_callback(host);
    app->tick();
    CHECK_FALSE(instance->serviceMainThread());
    ::close(fds[0]);
    ::close(fds[1]);
}
