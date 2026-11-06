// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The module library (ADR-0062, rack.md §7): modules written by commands or
// found in the folder join the catalog; an edit rebuilds the modules playing
// it, in the running engine and in renders.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "rack_runtime.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/write.h"

using namespace supple;
using namespace supple::engine;

namespace {

const cmd::Origin origin{cmd::Origin::Type::Test, "modules", ""};

// A constant on its output.
const char* const kLevel = R"(#include <supple/script.h>

struct Level : Module {
    static constexpr ModuleInfo info = {.name = "Level", .id = "com.example.level",
                                        .vendor = "Example", .description = "A constant."};
    Output out = audioOut("Out");

    void process(int frames) {
        for (int i = 0; i < frames; ++i) out[0][i] = 0.25;
    }
};
)";

std::filesystem::path freshDir(const char* what) {
    return std::filesystem::temp_directory_path() /
           (std::string(what) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

struct Fixture {
    std::filesystem::path dir = freshDir("module-library");
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    explicit Fixture(const std::function<void(const std::filesystem::path&)>& before = {}) {
        std::filesystem::create_directories(dir);
        if (before) before(dir / "modules");
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto made = EngineApp::create(options);
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~Fixture() {
        bus.reset();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Result<json::Value> call(std::string name, const json::Value& args) {
        return bus->execute({std::move(name), args, origin});
    }
    json::Value run(std::string name, std::string_view text = "{}") {
        auto args = json::parse(text);
        REQUIRE(args);
        auto result = call(std::move(name), *args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    json::Value write(const std::string& name, const std::string& source) {
        json::Value args = json::Value::object();
        args["name"] = name;
        args["source"] = source;
        auto result = call("module.write", args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    double level() {
        OfflineSettings settings;
        settings.endSeconds = 0.05;
        settings.threads = 1;
        auto r = app->renderSession(app->head(), settings);
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return double(r->audio.channels[0][1200]);
    }
};

}  // namespace

TEST_CASE("module library: a written module plays, and an edit rebuilds it" *
          doctest::description("rt")) {
    Fixture f;
    f.run("project.set", R"({"console9": false})");
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");  // mo_1: the Out, tr_1's input
    json::Value written = f.write("level", kLevel);
    CHECK(written["ok"].asBool());
    CHECK(written["type"]["origin"].asString() == "library");
    f.run("module.add", R"({"rack": "rk_1", "type": "com.example.level"})");  // mo_2
    f.run("cable.add", R"({"from": "mo_2.out", "to": "mo_1.l"})");
    REQUIRE(f.app->startEngine("dummy", 256));
    const model::Id mo2{model::Kind::Module, 2};
    const rack::Instance* before = f.app->instances().racks().find(mo2);
    REQUIRE(before != nullptr);
    const double quiet = f.level();
    CHECK(quiet == doctest::Approx(0.25 * 0.472135955).epsilon(1e-3));

    json::Value edited =
        f.run("module.edit", R"({"name": "level", "edits": [{"old": "0.25", "new": "0.5"}]})");
    CHECK(edited["ok"].asBool());
    CHECK(edited["reloaded"].asInt() == 1);
    // The running engine plays the new code, and so do renders.
    CHECK(f.app->instances().racks().find(mo2) != before);
    CHECK(f.level() == doctest::Approx(2 * quiet).epsilon(1e-3));

    // A version that does not compile is saved, and racks keep the last good one.
    json::Value broken =
        f.run("module.edit",
              R"({"name": "level", "edits": [{"old": "out[0][i] = 0.5;", "new": "out"}]})");
    CHECK_FALSE(broken["ok"].asBool());
    CHECK(broken.find("playing") != nullptr);
    CHECK(f.level() == doctest::Approx(2 * quiet).epsilon(1e-3));
    json::Value got = f.run("module.get", R"({"name": "level"})");
    CHECK(got["source"].asString().find("out[0][i] = 0.5;") == std::string::npos);
}

TEST_CASE("module library: the folder's modules load when the app starts") {
    Fixture f([](const std::filesystem::path& library) {
        REQUIRE(writeFileAtomic(library / "level" / "level.cpp", kLevel));
        REQUIRE(writeFileAtomic(library / "broken" / "broken.cpp", "struct X : Module {};"));
    });
    json::Value list = f.run("module.list", R"({"query": "level"})");
    bool found = false;
    for (auto& e : list["types"].asArray())
        found = found || (e["type"].asString() == "com.example.level" &&
                          e["origin"].asString() == "library");
    CHECK(found);
    json::Value check = f.run("module.check", R"({"name": "broken"})");
    CHECK_FALSE(check["ok"].asBool());
    CHECK(check["diagnostics"].asArray().size() > 0);
}

TEST_CASE("module library: ids stay unique") {
    Fixture f;
    std::string vca = f.run("module.get", R"({"type": "vca"})")["source"].asString();
    json::Value args = json::Value::object();
    args["name"] = "myvca";
    args["source"] = vca;
    auto taken = f.call("module.write", args);
    REQUIRE_FALSE(taken);
    CHECK(taken.error().code == "module.id_taken");
    // A fork takes a new id; a second module with that id is refused.
    json::Value fork = f.run("module.fork", R"({"type": "vca", "id": "com.example.vca"})");
    CHECK(fork["ok"].asBool());
    CHECK(fork["name"].asString() == "vca");
    std::string copy = f.run("module.get", R"({"name": "vca"})")["source"].asString();
    args["source"] = copy;
    auto twice = f.call("module.write", args);
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == "module.id_taken");
    // Built-in modules are forked, not edited.
    auto builtIn = f.call("module.edit", *json::parse(R"({"type": "com.suppleaudio.rack.vca",
        "edits": [{"old": "VCA", "new": "Amp"}]})"));
    REQUIRE_FALSE(builtIn);
    CHECK(builtIn.error().code == "module.built_in");
}

TEST_CASE("module library: factory presets set a module's knobs") {
    Fixture f;
    f.write("tone", R"(#include <supple/script.h>

struct Tone : Module {
    static constexpr ModuleInfo info = {.name = "Tone", .id = "com.example.tone",
                                        .vendor = "Example", .description = "A test."};
    Param level = decibels("Level", -60, 0, -6);
    Param shape = choice("Shape", {"Sine", "Saw"}, 0).keys({"sine", "saw"});
    Output out = audioOut("Out");

    void factoryPatches(Patches& p) {
        p.add("Loud Saw").set(level, 0.0).set(shape, "saw");
    }
    void process(int frames) {
        for (int i = 0; i < frames; ++i) out[0][i] = 0;
    }
};
)");
    json::Value described = f.run("module.describe", R"({"type": "com.example.tone"})");
    REQUIRE(described.find("presets") != nullptr);
    CHECK(described["presets"].asArray().size() == 1);
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");
    f.run("module.add", R"({"rack": "rk_1", "type": "com.example.tone", "preset": "Loud Saw"})");
    json::Value got = f.run("rack.get", R"({"rack": "rk_1"})");
    const model::RackModule* m = f.app->head().modules.find({model::Kind::Module, 2});
    REQUIRE(m != nullptr);
    CHECK(json::writeCompact(m->params) == R"({"level":0.0,"shape":"saw"})");
    // module.set's preset resets the others: params on top.
    f.run("module.set", R"({"module": "mo_2", "preset": "Loud Saw", "params": {"level": -12}})");
    m = f.app->head().modules.find({model::Kind::Module, 2});
    CHECK(json::writeCompact(m->params) == R"({"level":-12.0,"shape":"saw"})");
    auto missing = f.call("module.set", *json::parse(R"({"module": "mo_2", "preset": "Nope"})"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "rack.preset");
    (void)got;
}

TEST_CASE("rack.probe: pitch, gates, notes and a CV's speed") {
    Fixture f;
    f.write("sine", R"(#include <supple/script.h>

struct Sine : Module {
    static constexpr ModuleInfo info = {.name = "Sine", .id = "com.example.sine",
                                        .vendor = "Example", .description = "A test sine."};
    Param freq = frequency("Frequency", 20, 2000, 220);
    Output out = audioOut("Out");
    double phase = 0;

    void process(int frames) {
        for (int i = 0; i < frames; ++i) {
            out[0][i] = 0.5 * sin(2 * pi * phase);
            phase += freq.at(i) / sampleRate();
            phase -= floor(phase);
        }
    }
};
)");
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");                                         // mo_1 Out
    f.run("module.add", R"({"rack": "rk_1", "type": "com.example.sine"})");            // mo_2
    f.run("module.add", R"({"rack": "rk_1", "type": "notes"})");                       // mo_3
    f.run("module.add", R"({"rack": "rk_1", "type": "lfo", "params": {"rate": 2}})");  // mo_4
    json::Value r = f.run("rack.probe", R"({"rack": "rk_1", "seconds": 2,
        "ports": ["mo_2.out", "mo_3.gate", "mo_3.pitch", "mo_4.out"],
        "notes": "voice v; 1: C4:q r:q E4:q r:q"})");
    auto& ports = r["ports"].asArray();
    REQUIRE(ports.size() == 4);
    CHECK(ports[0]["pitch"]["hz"].asDouble() == doctest::Approx(220).epsilon(0.002));
    CHECK(ports[0]["pitch"]["note"].asString() == "A3");
    CHECK(ports[0]["peak"].asDouble() == doctest::Approx(0.5).epsilon(0.01));
    CHECK(ports[1]["gates"]["per_beat"].asDouble() == doctest::Approx(0.5));  // 2 in 4 beats
    CHECK(ports[1]["gates"]["duty"].asDouble() == doctest::Approx(0.5).epsilon(0.02));
    auto& notes = ports[2]["notes"].asArray();
    REQUIRE(notes.size() >= 2);
    bool c4 = false, e4 = false;
    for (auto& n : notes) {
        c4 = c4 || n.asString() == "C4";
        e4 = e4 || n.asString() == "E4";
    }
    CHECK(c4);
    CHECK(e4);
    CHECK(ports[3]["rate_hz"].asDouble() == doctest::Approx(2).epsilon(0.05));
    // Nothing changed.
    CHECK(f.app->head().modules.size() == 4);
}

TEST_CASE("module.duplicate keeps cables into a plugin module's plug-in") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");                                          // mo_1 Out
    f.run("module.add", R"({"rack": "rk_1", "type": "lfo"})");                          // mo_2
    f.run("module.add", R"({"rack": "rk_1", "plugin": "com.suppleaudio.test.gain"})");  // mo_3
    f.run("cable.add", R"({"from": "mo_2.out", "to": "mo_3.in_l"})");
    f.run("cable.add", R"({"from": "mo_2.out", "to": "fx_1.gain", "amount": 0.3})");
    json::Value copies = f.run("module.duplicate", R"({"modules": ["mo_2", "mo_3"]})");
    REQUIRE(copies["cables"].asArray().size() == 2);
    const model::RackModule* copy = f.app->head().modules.find({model::Kind::Module, 5});
    REQUIRE(copy != nullptr);
    REQUIRE(copy->fx);
    bool reached = false;
    for (const auto& e : f.app->head().cables)
        reached |= e.value->from.str() == "mo_4.out" &&
                   e.value->to == model::CableEnd{*copy->fx, "gain"} && e.value->amount == 0.3;
    CHECK(reached);
}
