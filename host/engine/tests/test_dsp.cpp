// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// DSP scripts over the bus (ADR-0032): a script written with dsp.write is a
// plugin like a stock one -- listed, loaded, set in physical units through
// its patch, an instrument playing a track's notes -- and its mistakes come
// back with hints.
#include <doctest/doctest.h>
#include "../../../tests/file_platform.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const fs::path kStock = STOCK_CLAP_DIR;
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

struct Root {
    fs::path path = fs::temp_directory_path() /
                    ("supple-engine-dsp-" + std::to_string(supple::test::processId()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

const char* const kScript = R"(#include <supple/script.h>

struct AgentGain : Effect {
    static constexpr Info info = {.name = "Agent Gain", .id = "com.example.agent-gain"};
    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level change.");
    Param mix = percent("Mix", 50).help("Dry to processed.");

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i)
                io[c][i] = lerp(io[c][i], io[c][i] * dbToGain(gain.at(i)), mix);
    }
    void factoryPatches(Patches& p) {
        p.add("Quiet").set(gain, -9).set(mix, 30);
    }
};
)";

}  // namespace

TEST_CASE("fx.describe reports the live script's problems in process and through the bridge") {
    host::setBridgeExecutable(BRIDGE_PATH);
    REQUIRE(fs::exists(kStock / "Script Player.clap"));
    for (const bool isolated : {false, true}) {
        Root root;
        AppOptions o;
        o.workdir = root.path;
        o.chainDir = root.path / "chains";
        o.pluginPath = {kStock / "Script Player.clap"};
        // Backend outlives the app even if an assertion aborts the test.
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        auto made = EngineApp::create(o, {.console9 = false});
        REQUIRE(made);
        auto app = std::move(*made);
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, *app);
        auto call = [&](std::string name, json::Value args) {
            auto r = bus.execute({std::move(name), std::move(args), kTest});
            REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
            return *r;
        };
        json::Value write = json::Value::object();
        write["name"] = "live_status";
        write["source"] = R"(#include <supple/script.h>
struct LiveStatus : Effect {
    static constexpr Info info = {.name = "Live Status", .id = "com.example.live-status"};
    void process(Block& io) {
        io[0][io.frames] = 1.0;
    }
    void factoryPatches(Patches& p) { p.add("Default"); }
};
)";
        REQUIRE(call("dsp.write", std::move(write))["ok"].asBool());
        call("track.add", json::parse(R"({"name":"Live"})").value());
        call("track.set", json::parse(R"({"track":"tr_1","armed":true})").value());
        auto added =
            call("fx.add",
                 json::parse(R"({"track":"tr_1","plugin":"com.example.live-status"})").value());
        const std::string fx = added["fx"].asString();
        json::Value args = json::Value::object();
        args["fx"] = fx;
        CHECK_FALSE(call("fx.describe", args)["status_available"].asBool());
        if (isolated) {
            auto set = args;
            set["isolation"] = "dedicated";
            call("fx.set", std::move(set));
        }
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64},
                                 {.anticipative = false}));
        auto* live = app->instanceOf(*model::Id::parse(fx));
        REQUIRE(live);
        CHECK(live->isolated() == isolated);
        // Drive the actual pooled instance deterministically. Live callback
        // nodes intentionally bypass synchronous bridges; this query test
        // exercises their ordinary host processing API instead.
        float left[64]{}, right[64]{};
        float* channels[]{left, right};
        REQUIRE(live->startProcessing());
        REQUIRE(live->process({.inputs = channels,
                               .inputChannels = 2,
                               .outputs = channels,
                               .outputChannels = 2,
                               .frames = 64}));
        live->stopProcessing();
        auto described = call("fx.describe", args);
        CHECK(described["status_available"].asBool());
        REQUIRE(described["problems"].size() == 1);
        const auto& p = described["problems"].at(0);
        CHECK(p.find("code")->asString() == "script.runtime.index");
        CHECK(p.find("line")->asInt() == 5);
        CHECK_FALSE(p.find("silent")->asBool());
        // The optional semantic callback has the same view through isolation.
        auto factories = call("fx.patch.list", args);
        REQUIRE(factories["patches"].size() == 1);
        CHECK(factories["patches"].at(0).find("name")->asString() == "Default");
        app->stopEngine();
    }
}

TEST_CASE("a DSP script written over the bus is a plugin an agent sets through its patch") {
    if (!fs::exists(kStock / "Script Player.clap")) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    AppOptions o;
    o.workdir = root.path;
    o.chainDir = root.path / "chains";
    o.presetDir = root.path / "presets";
    o.pluginPath = {kStock};
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    auto call = [&](std::string name, json::Value args) {
        auto r = bus.execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    auto write = [&](const std::string& source) {
        json::Value a = json::Value::object();
        a["name"] = "agent_gain";
        a["source"] = source;
        return call("dsp.write", std::move(a));
    };

    json::Value written = write(kScript);
    CHECK(written["ok"].asBool());
    CHECK(written["listed"].asBool());
    CHECK(written["plugin"]["id"].asString() == "com.example.agent-gain");
    CHECK(written["plugin"]["params"].at(1).find("unit")->asString() == "%");
    CHECK(written["plugin"]["params"].at(1).find("default")->asDouble() == 50.0);

    call("track.add", json::parse(R"({"name": "Vocal"})").value());
    json::Value added = call("fx.add", json::parse(R"({"track": "tr_1",
        "plugin": "com.example.agent-gain", "patch": {"params": {"gain": -6}}})")
                                           .value());
    CHECK(added["semantic"].asBool());
    const std::string fx = added["fx"].asString();
    auto patch = [&] {
        return call("fx.patch.get", json::parse(R"({"fx": ")" + fx + R"("})").value())["patch"];
    };
    CHECK(patch()["params"]["gain"].asDouble() == doctest::Approx(-6));
    CHECK(patch()["params"]["mix"].asDouble() == doctest::Approx(50));
    call("fx.param.set", json::parse(R"({"fx": ")" + fx + R"(", "values": {"mix": 25}})").value());
    CHECK(patch()["params"]["mix"].asDouble() == doctest::Approx(25));

    auto factory = call("fx.patch.list", json::parse(R"({"fx": ")" + fx + R"("})").value());
    REQUIRE(factory["patches"].size() == 1);
    CHECK(factory["patches"].at(0).find("name")->asString() == "Quiet");
    call("fx.patch.set", json::parse(R"({"fx": ")" + fx + R"(", "name":"Quiet"})").value());
    CHECK(patch()["params"]["gain"].asDouble() == doctest::Approx(-9));
    CHECK(patch()["params"]["mix"].asDouble() == doctest::Approx(30));
    call("history.undo", json::Value::object());
    CHECK(patch()["params"]["gain"].asDouble() == doctest::Approx(-6));
    CHECK(patch()["params"]["mix"].asDouble() == doctest::Approx(25));
    // The FX window's preset menu: the factory patch first, read-only.
    const json::Value fxArgs = json::parse(R"({"fx": ")" + fx + R"("})").value();
    auto presets = call("fx.preset.list", fxArgs);
    REQUIRE(presets["presets"].size() == 1);
    CHECK(presets["presets"].at(0).find("name")->asString() == "Quiet");
    CHECK(presets["presets"].at(0).find("source")->asString() == "factory");
    auto named = fxArgs;
    named["name"] = "Quiet";
    for (const char* command : {"fx.preset.save", "fx.preset.delete"}) {
        auto refused = bus.execute({command, named, kTest});
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "fx.preset_factory");
    }
    call("fx.preset.load", named);
    CHECK(patch()["params"]["gain"].asDouble() == doctest::Approx(-9));
    call("history.undo", json::Value::object());

    // The harness, through the player: -6 dB fully wet halves the sine.
    json::Value measured = call("dsp.test", json::parse(R"({"script": "agent_gain",
        "signals": ["sine"], "patch": {"params": {"gain": -6, "mix": 100}}})")
                                                .value());
    CHECK(measured["plugin"].asString() == "com.example.agent-gain");
    CHECK(measured["signals"].at(0).find("gain_db")->asDouble() == doctest::Approx(-6.0));
    CHECK(measured["problems"].size() == 0);

    // A mistake: saved, reported with its line and hint, and no longer a plugin.
    std::string broken = kScript;
    broken.replace(broken.find("dbToGain(gain.at(i))"), 20,
                   "dbToGain(gain.at(i)) / (io.frames / c)");
    json::Value failed = write(broken);
    CHECK_FALSE(failed["ok"].asBool());
    CHECK_FALSE(failed["listed"].asBool());
    REQUIRE(failed["diagnostics"].size() >= 1);
    CHECK(failed["diagnostics"].at(0).find("code")->asString() == "script.ub.division");
    CHECK(failed["diagnostics"].at(0).find("line")->asInt() == 11);
    CHECK_FALSE(failed["diagnostics"].at(0).find("hint")->asString().empty());
    json::Value listed = call("dsp.list", json::Value::object());
    REQUIRE(listed["scripts"].size() == 1);
    CHECK_FALSE(listed["scripts"].at(0).find("ok")->asBool());
    CHECK(call("dsp.get", json::parse(R"({"name": "agent_gain"})").value())["source"].asString() ==
          broken);
}

TEST_CASE("a merge names the values it set that do not apply with the patch it leaves") {
    if (!fs::exists(kStock / "Script Player.clap")) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    AppOptions o;
    o.workdir = root.path;
    o.chainDir = root.path / "chains";
    o.pluginPath = {kStock / "Script Player.clap"};
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    auto call = [&](std::string name, const std::string& args) {
        auto r = bus.execute({std::move(name), json::parse(args).value(), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    json::Value write = json::Value::object();
    write["name"] = "sometimes";
    write["source"] = R"(#include <supple/script.h>
struct Sometimes : Effect {
    static constexpr Info info = {.name = "Sometimes", .id = "com.example.sometimes"};
    Param on = toggle("On", true);
    Param gain = decibels("Gain", -24, 24, 0).activeWhen(on);
    void process(Block& io) {}
};
)";
    auto written = bus.execute({"dsp.write", std::move(write), kTest});
    REQUIRE((written && (*written)["ok"].asBool()));
    call("track.add", R"({"name": "Vocal"})");
    const std::string fx =
        call("fx.add", R"({"track": "tr_1", "plugin": "com.example.sometimes"})")["fx"].asString();
    auto set =
        call("fx.param.set", R"({"fx": ")" + fx + R"(", "values": {"on": false, "gain": -6}})");
    CHECK(json::writeCompact(set["inactive"]) == R"(["gain"])");
    auto described = call("fx.describe", R"({"fx": ")" + fx + R"(", "full": true})");
    CHECK_FALSE(described["params"]["gain"]["active"].asBool());
    auto merged =
        call("fx.patch.merge", R"({"fx": ")" + fx + R"(", "patch": {"params": {"on": true}}})");
    CHECK(merged.find("inactive") == nullptr);
}

TEST_CASE("a scripted instrument plays a track's notes in the mix") {
    if (!fs::exists(kStock / "Script Player.clap")) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    AppOptions o;
    o.workdir = root.path;
    o.chainDir = root.path / "chains";
    o.pluginPath = {kStock / "Script Player.clap"};
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    auto call = [&](std::string name, json::Value args) {
        auto r = bus.execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/synth.cpp");
    std::stringstream source;
    source << in.rdbuf();
    json::Value w = json::Value::object();
    w["name"] = "synth";
    w["source"] = source.str();
    json::Value written = call("dsp.write", std::move(w));
    REQUIRE(written["ok"].asBool());
    CHECK(written["plugin"]["kind"].asString() == "instrument");
    REQUIRE(written["listed"].asBool());

    call("track.add", json::parse(R"({"name": "Keys"})").value());
    call("fx.add", json::parse(R"({"track": "tr_1", "plugin": "com.example.synth"})").value());
    call("item.add", json::parse(R"({"track": "tr_1", "bar": 1,
        "notation": "voice keys; 1: C4:q E4:q G4:h"})")
                         .value());
    json::Value mix = call("render.mix", json::parse(R"({"name": "keys"})").value());
    CHECK(mix["peak_dbfs"].asDouble() > -40.0);
}

TEST_CASE("fx.meters reads a live plugin's readouts in process and through the bridge") {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (const bool isolated : {false, true}) {
        Root root;
        AppOptions o;
        o.workdir = root.path;
        o.chainDir = root.path / "chains";
        o.pluginPath = {kStock / "Script Player.clap"};
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        auto made = EngineApp::create(o, {.console9 = false});
        REQUIRE(made);
        auto app = std::move(*made);
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, *app);
        auto call = [&](std::string name, json::Value args) {
            auto r = bus.execute({std::move(name), std::move(args), kTest});
            REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
            return *r;
        };
        json::Value write = json::Value::object();
        write["name"] = "listen";
        write["source"] = R"(#include <supple/script.h>
struct Listen : Effect {
    static constexpr Info info = {.name = "Listen", .id = "com.example.listen"};
    Param trim = decibels("Trim", -12, 12, 0).help("Unused.");
    Param level = decibels("Level", -120, 0, -120).meter().help("The block's peak.");
    Param state = choice("State", {"Quiet", "Loud"}, 0).meter().help("Above -6 dB.");
    void process(Block& io) {
        double peak = 0.0;
        for (int i = 0; i < io.frames; ++i) peak = max(peak, abs(io[0][i]));
        level.set(gainToDb(peak));
        state.set(peak > 0.5 ? 1 : 0);
    }
};
)";
        REQUIRE(call("dsp.write", std::move(write))["ok"].asBool());
        call("track.add", json::parse(R"({"name":"Live"})").value());
        auto added = call("fx.add",
                          json::parse(R"({"track":"tr_1","plugin":"com.example.listen"})").value());
        json::Value args = json::Value::object();
        args["fx"] = added["fx"].asString();
        auto before = call("fx.meters", args);
        CHECK(before["source"].asString() == "none");
        CHECK(before["meters"].size() == 0);
        if (isolated) {
            auto set = args;
            set["isolation"] = "dedicated";
            call("fx.set", std::move(set));
        }
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 64},
                                 {.anticipative = false}));
        auto* live = app->instanceOf(*model::Id::parse(added["fx"].asString()));
        REQUIRE(live);
        float left[64], right[64];
        std::fill(std::begin(left), std::end(left), 0.75f);
        std::fill(std::begin(right), std::end(right), 0.75f);
        float* channels[]{left, right};
        // A plugin reports its readouts as output events a few dozen times
        // a second, as the engine's blocks give it room to: a tenth of a
        // second of blocks, each with a queue.
        std::vector<host::OutputEvent> storage(64);
        host::OutputEvents events;
        events.storage = storage;
        REQUIRE(live->startProcessing());
        for (int block = 0; block < 75; ++block) {
            std::fill(std::begin(left), std::end(left), 0.75f);
            std::fill(std::begin(right), std::end(right), 0.75f);
            events.clear();
            REQUIRE(live->process({.inputs = channels,
                                   .inputChannels = 2,
                                   .outputs = channels,
                                   .outputChannels = 2,
                                   .frames = 64},
                                  &events));
        }
        live->stopProcessing();
        auto read = call("fx.meters", args);
        CHECK(read["source"].asString() == "live");
        const auto& meters = read["meters"];
        CHECK(meters.size() == 2);  // the readouts only, not the trim
        CHECK(meters.find("level")->asDouble() == doctest::Approx(-2.4988).epsilon(1e-3));
        CHECK(meters.find("state")->asString() == "loud");
        app->stopEngine();
    }
}
