// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The module library (ADR-0062, rack.md §7): modules written by commands or
// found in the folder join the catalog; an edit rebuilds the modules playing
// it, in the running engine and in renders.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>

#include "rack_runtime.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/write.h"
#include "supple/model/schemas.h"

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
    explicit Fixture(const std::function<void(const std::filesystem::path&)>& before = {},
                     AppOptions options = {}) {
        std::filesystem::create_directories(dir);
        if (before) before(dir / "modules");
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

TEST_CASE("rack presets: a patch saved from one rack plays in another") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");  // mo_1 Out
    f.run("module.add",
          R"({"rack": "rk_1", "type": "vca", "label": "Amp", "params": {"level": -6}})");  // mo_2
    f.run("module.add", R"({"rack": "rk_1", "type": "lfo", "params": {"rate": 3}})");      // mo_3
    f.run("module.add", R"({"rack": "rk_1", "plugin": "com.suppleaudio.test.gain"})");     // mo_4
    f.run("fx.patch.merge", R"({"fx": "fx_1", "patch": {"params": {"gain": "-9 dB"}}})");
    f.run("cable.add", R"({"from": "mo_3.out", "to": "mo_2.level", "amount": 4})");
    f.run("cable.add", R"({"from": "mo_2.out", "to": "mo_4.in_l"})");
    f.run("cable.add", R"({"from": "mo_3.out", "to": "fx_1.gain", "amount": 0.3})");
    f.run("cable.add", R"({"from": "mo_4.out_l", "to": "mo_1.l", "color": "teal"})");

    json::Value saved = f.run("rack.preset.save",
                              R"({"rack": "rk_1", "name": "Tremolo", "description": "A wobble"})");
    CHECK(saved["modules"].asInt() == 4);
    CHECK(saved["cables"].asInt() == 4);
    auto again = f.call("rack.preset.save", *json::parse(R"({"rack": "rk_1", "name": "Tremolo"})"));
    REQUIRE_FALSE(again);
    CHECK(again.error().code == "rack.preset_exists");
    auto badName = f.call("rack.preset.save", *json::parse(R"({"rack": "rk_1", "name": "a/b"})"));
    REQUIRE_FALSE(badName);
    CHECK(badName.error().code == "rack.preset_name");
    // The file is canonical: saving the same patch again writes the same bytes.
    const auto file = std::filesystem::path(saved["path"].asString());
    auto first = readTextFile(file, 1 << 20);
    REQUIRE(first);
    f.run("rack.preset.save", R"({"rack": "rk_1", "name": "Tremolo", "description": "A wobble",
                                  "replace": true})");
    CHECK(*readTextFile(file, 1 << 20) == *first);

    json::Value list = f.run("rack.preset.list", R"({"query": "wobble"})");
    REQUIRE(list["presets"].asArray().size() == 1);
    CHECK(list["presets"].at(0).find("name")->asString() == "Tremolo");
    CHECK(list["presets"].at(0).find("modules")->asInt() == 4);
    CHECK(f.run("rack.preset.list", R"({"query": "nothing"})")["presets"].asArray().empty());

    // Into a second rack: its Out takes the preset Out's cable (io: join).
    f.run("track.add", R"({"name": "Lead"})");
    f.run("rack.add", R"({"track": "tr_2"})");  // rk_2, mo_5 Out
    json::Value loaded = f.run("rack.preset.load", R"({"rack": "rk_2", "name": "Tremolo"})");
    REQUIRE(loaded["modules"].asArray().size() == 3);  // mo_6 VCA, mo_7 LFO, mo_8 plugin
    REQUIRE(loaded["cables"].asArray().size() == 4);
    CHECK(loaded.find("warnings") == nullptr);
    const model::Session& s = f.app->head();
    const model::RackModule* vca = s.modules.find({model::Kind::Module, 6});
    REQUIRE(vca != nullptr);
    CHECK(vca->rack == model::Id{model::Kind::Rack, 2});
    CHECK(vca->label == "Amp");
    CHECK(json::writeCompact(vca->params) == R"({"level":-6.0})");
    const model::RackModule* plugin = s.modules.find({model::Kind::Module, 8});
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->fx);
    CHECK(*plugin->fx != model::Id{model::Kind::Fx, 1});
    CHECK(s.fx.find(*plugin->fx)->plugin.id == "com.suppleaudio.test.gain");
    CHECK(json::writeCompact(f.run("fx.patch.get", "{\"fx\": \"" + plugin->fx->str() + "\"}")) ==
          json::writeCompact(f.run("fx.patch.get", R"({"fx": "fx_1"})")));
    bool toOut = false, toKnob = false, toPlugin = false;
    for (const auto& e : s.cables) {
        const model::Cable& cb = *e.value;
        toOut |= cb.from.str() == "mo_8.out_l" && cb.to.str() == "mo_5.l" && cb.color == "teal";
        toKnob |= cb.from.str() == "mo_7.out" && cb.to.str() == "mo_6.level" && cb.amount == 4.0;
        toPlugin |= cb.from.str() == "mo_7.out" && cb.to == model::CableEnd{*plugin->fx, "gain"} &&
                    cb.amount == 0.3;
    }
    CHECK(toOut);
    CHECK(toKnob);
    CHECK(toPlugin);
    // One undo step takes it all away.
    f.run("history.undo");
    CHECK(f.app->head().modules.size() == 5);

    // io: add brings the preset's own Out.
    json::Value added = f.run("rack.preset.load", R"({"rack": "rk_2", "name": "Tremolo",
                                                       "io": "add", "x": 0, "y": 500})");
    CHECK(added["modules"].asArray().size() == 4);

    // A knob the module no longer has is left out with a warning; a type that
    // is not installed refuses the whole preset.
    auto edited = [&](const std::function<void(json::Value&)>& change) {
        json::Value doc = *json::parse(*first);
        for (auto& m : doc["modules"].asArray()) change(m);
        return json::writeCanonical(doc);
    };
    REQUIRE(writeFileAtomic(file.parent_path() / "Old.json", edited([](json::Value& m) {
                                if (m["type"].asString() == "com.suppleaudio.rack.vca")
                                    m["params"]["nope"] = 1;
                            })));
    json::Value old =
        f.run("rack.preset.load", R"({"rack": "rk_2", "path": ")" +
                                      (file.parent_path() / "Old.json").generic_string() + R"("})");
    REQUIRE(old.find("warnings") != nullptr);
    CHECK(old["warnings"].at(0).find("code")->asString() == "rack.preset_knob");
    CHECK(old["modules"].asArray().size() == 3);
    const std::size_t before = f.app->head().modules.size();
    REQUIRE(writeFileAtomic(file.parent_path() / "Missing.json", edited([](json::Value& m) {
                                if (m["type"].asString() == "com.suppleaudio.rack.lfo")
                                    m["type"] = "com.example.not.here";
                            })));
    auto missing =
        f.call("rack.preset.load", *json::parse(R"({"rack": "rk_2", "name": "Missing"})"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "rack.unknown_type");
    CHECK(f.app->head().modules.size() == before);

    json::Value gone = f.run("rack.preset.delete", R"({"name": "Tremolo"})");
    CHECK_FALSE(std::filesystem::exists(gone["path"].asString()));
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

TEST_CASE("rack.capture: a rack's notes become a MIDI item") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("track.add", R"({"name": "Keys"})");
    f.run("rack.add", R"({"track": "tr_1"})");                    // mo_1 Out
    f.run("module.add", R"({"rack": "rk_1", "type": "notes"})");  // mo_2
    const std::size_t modules = f.app->head().modules.size();
    json::Value r = f.run("rack.capture", R"({"rack": "rk_1", "pitch": "mo_2.pitch",
        "gate": "mo_2.gate", "velocity": "mo_2.velocity", "track": "tr_2", "beats": 4,
        "at": 8, "quantize": 0.25, "notes": "voice v; 1: C4:q E4:e r:e [G4 B4]:h"})");
    CHECK(r["notes"].asInt() == 4);
    CHECK(r["voices"].asInt() >= 2);
    CHECK(r["position"].asDouble() == 8);
    const model::Session& s = f.app->head();
    CHECK(s.modules.size() == modules);  // the rack is not changed
    const model::Item* item = s.items.find(*model::Id::parse(r["item"].asString()));
    REQUIRE(item != nullptr);
    CHECK(item->track == model::Id{model::Kind::Track, 2});
    const auto* src = s.midi.find(*model::Id::parse(r["source"].asString()));
    REQUIRE(src != nullptr);
    const auto& notes = src->sequence.notes;
    REQUIRE(notes.size() == 4);
    const int ppq = src->sequence.ppq;
    CHECK(notes[0].pitch == 60);
    CHECK(notes[0].start == 0);
    CHECK(notes[0].length == ppq);
    CHECK(notes[1].pitch == 64);
    CHECK(notes[1].start == ppq);
    CHECK(notes[1].length == ppq / 2);
    std::set<int> chord;
    for (std::size_t k = 2; k < 4; ++k) {
        chord.insert(notes[k].pitch);
        CHECK(notes[k].start == 2 * ppq);
        CHECK(notes[k].length == 2 * ppq);
        CHECK(notes[k].velocity > 0);
    }
    CHECK(chord == std::set<int>{67, 71});
    // The same capture again is the same notes.
    json::Value again = f.run("rack.capture", R"({"rack": "rk_1", "pitch": "mo_2.pitch",
        "gate": "mo_2.gate", "velocity": "mo_2.velocity", "track": "tr_2", "beats": 4,
        "at": 16, "quantize": 0.25, "notes": "voice v; 1: C4:q E4:e r:e [G4 B4]:h"})");
    CHECK(f.app->head().midi.find(*model::Id::parse(again["source"].asString()))->sequence.notes ==
          notes);
    // One undo step removes it.
    f.run("history.undo", "{}");
    CHECK(f.app->head().items.find(*model::Id::parse(again["item"].asString())) == nullptr);
    CHECK(f.app->head().items.find(*model::Id::parse(r["item"].asString())) != nullptr);

    auto master = f.call("rack.capture", *json::parse(R"({"rack": "rk_1",
        "pitch": "mo_2.pitch", "gate": "mo_2.gate", "track": "tr_0"})"));
    REQUIRE_FALSE(master);
    CHECK(master.error().code == "track.strip_type");
    auto input = f.call("rack.capture", *json::parse(R"({"rack": "rk_1",
        "pitch": "mo_1.l", "gate": "mo_2.gate", "track": "tr_2"})"));
    REQUIRE_FALSE(input);
    CHECK(input.error().code == "rack.port");
    CHECK(input.error().path == "/args/pitch");
}

TEST_CASE("rack.patch: named modules and cables in one step, all or nothing") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");  // mo_1 Out
    json::Value made = f.run("rack.patch", R"({"rack": "rk_1",
        "modules": {"keys": {"type": "notes"}, "osc": {"type": "vco"},
                    "env": {"type": "adsr", "params": {"release": 200}},
                    "amp": {"type": "vca"}, "fx": {"plugin": "com.suppleaudio.test.gain"},
                    "wobble": {"type": "lfo", "params": {"rate": 4}}},
        "cables": [["keys.pitch", "osc.pitch"], ["keys.gate", "env.gate"],
                   ["osc.saw", "amp.in"], ["env.envelope", "amp.cv"],
                   ["amp.out", "fx.in_l"], ["wobble.out", "fx.gain", 0.25],
                   {"from": "fx.out_l", "to": "mo_1.l", "color": "teal"}]})");
    const auto& ids = made["modules"];
    REQUIRE(ids.find("fx") != nullptr);
    CHECK(made["cables"].asArray().size() == 7);
    const model::Session& s = f.app->head();
    auto module = [&](const char* name) {
        return s.modules.find(*model::Id::parse(ids.find(name)->asString()));
    };
    CHECK(json::writeCompact(module("env")->params) == R"({"release":200.0})");
    // Laid out by signal flow: each stage right of what feeds it.
    CHECK(module("keys")->x < module("osc")->x);
    CHECK(module("osc")->x < module("amp")->x);
    CHECK(module("amp")->x < module("fx")->x);
    // A plugin module's knob is its plug-in's.
    bool knob = false;
    for (const auto& e : s.cables)
        knob |=
            e.value->to == model::CableEnd{*module("fx")->fx, "gain"} && e.value->amount == 0.25;
    CHECK(knob);

    // One bad cable: nothing changes.
    const std::size_t modules = s.modules.size(), cables = s.cables.size();
    auto bad = f.call("rack.patch", *json::parse(R"({"rack": "rk_1",
        "modules": {"osc2": {"type": "vco"}}, "cables": [["osc2.nope", "mo_1.l"]]})"));
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "rack.port");
    CHECK(bad.error().path.rfind("/args/cables/0", 0) == 0);
    CHECK(f.app->head().modules.size() == modules);
    CHECK(f.app->head().cables.size() == cables);
    auto unknown = f.call("rack.patch", *json::parse(R"({"rack": "rk_1",
        "modules": {"osc2": {"type": "vcoo"}}})"));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "rack.unknown_type");
    CHECK(unknown.error().path == "/args/modules/osc2/type");
}

TEST_CASE("rack.patch: a module's name is its label") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");
    json::Value made = f.run("rack.patch", R"({"rack": "rk_1",
        "modules": {"vco": {"type": "vco"}, "vco2": {"type": "vco"}, "adsr_3": {"type": "adsr"},
                    "kick": {"type": "drum"}, "bass_env": {"type": "adsr"},
                    "osc1": {"type": "vco"}, "lfo": {"type": "function"},
                    "named": {"type": "vca", "label": "Mine"},
                    "fx": {"plugin": "com.suppleaudio.test.gain"}}})");
    const model::Session& s = f.app->head();
    auto label = [&](const char* name) {
        return s.modules.find(*model::Id::parse(made["modules"].find(name)->asString()))->label;
    };
    CHECK(label("vco") == "");
    CHECK(label("vco2") == "VCO 2");
    CHECK(label("adsr_3") == "ADSR 3");
    CHECK(label("kick") == "Kick");
    CHECK(label("bass_env") == "Bass env");
    CHECK(label("osc1") == "Osc 1");
    CHECK(label("lfo") == "LFO");  // a type's key, spelled as that type's name
    CHECK(label("named") == "Mine");
    CHECK(label("fx") == "");  // a plugin module keeps its plug-in's name
}

namespace {

struct Placed {
    std::string name;
    double x, y, w, h;
};

std::vector<Placed> placesOf(const Fixture& f, const json::Value& ids) {
    std::vector<Placed> out;
    for (const auto& m : ids.asObject()) {
        const model::RackModule* r =
            f.app->head().modules.find(*model::Id::parse(m.value.asString()));
        auto type = f.app->modules()->find(r->type);
        REQUIRE(type != nullptr);
        out.push_back({m.key, r->x, r->y, type->type->layout().width, type->type->layout().height});
    }
    return out;
}

bool overlap(const Placed& a, const Placed& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

}  // namespace

TEST_CASE("rack.patch: a big patch is laid out near a window's shape, by signal flow") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");  // mo_1 Out
    // Two voices, drums and a modulation web: 24 modules.
    const char* patch = R"({"rack": "rk_1",
        "modules": {"clock": {"type": "clock"}, "seq": {"type": "sequencer"},
                    "quant": {"type": "quantizer"}, "osc1": {"type": "vco"},
                    "osc2": {"type": "vco"}, "env1": {"type": "adsr"}, "env2": {"type": "adsr"},
                    "amp1": {"type": "vca"}, "amp2": {"type": "vca"}, "lfo1": {"type": "lfo"},
                    "lfo2": {"type": "lfo"}, "kick": {"type": "drum"}, "snare": {"type": "drum"},
                    "hat": {"type": "drum"}, "euclid1": {"type": "euclid"},
                    "euclid2": {"type": "euclid"}, "euclid3": {"type": "euclid"},
                    "mix": {"type": "mixer"}, "drift": {"type": "drift"},
                    "chaos": {"type": "chaos"}, "slew": {"type": "slew"},
                    "noise": {"type": "noise"}, "turing": {"type": "turing"},
                    "chance": {"type": "chance"}},
        "cables": [["clock.out_1", "seq.clock"], ["seq.pitch", "quant.pitch"],
                   ["quant.out", "osc1.pitch"], ["turing.out", "osc2.pitch"],
                   ["clock.out_2", "turing.clock"], ["seq.gate", "env1.gate"],
                   ["clock.out_3", "chance.gate"], ["chance.a", "env2.gate"],
                   ["osc1.saw", "amp1.in"], ["osc2.saw", "amp2.in"],
                   ["env1.envelope", "amp1.cv"], ["env2.envelope", "amp2.cv"],
                   ["lfo1.out", "osc1.pulse_width"], ["lfo2.out", "osc2.pulse_width"],
                   ["clock.out_1", "euclid1.clock"], ["clock.out_1", "euclid2.clock"],
                   ["clock.out_1", "euclid3.clock"], ["euclid1.gate", "kick.trigger"],
                   ["euclid2.gate", "snare.trigger"], ["euclid3.gate", "hat.trigger"],
                   ["amp1.out", "mix.in_1"], ["amp2.out", "mix.in_2"], ["kick.out", "mix.in_3"],
                   ["snare.out", "mix.in_4"], ["drift.out", "lfo1.rate"],
                   ["chaos.x", "slew.in"], ["slew.out", "lfo2.rate"],
                   ["noise.pink", "osc2.lin_fm"], ["hat.out", "mo_1.r"], ["mix.left", "mo_1.l"]]})";
    json::Value made = f.run("rack.patch", patch);
    const auto placed = placesOf(f, made["modules"]);
    REQUIRE(placed.size() == 24);
    double left = 1e9, top = 1e9, right = -1e9, bottom = -1e9;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        left = std::min(left, placed[i].x);
        top = std::min(top, placed[i].y);
        right = std::max(right, placed[i].x + placed[i].w);
        bottom = std::max(bottom, placed[i].y + placed[i].h);
        for (std::size_t k = i + 1; k < placed.size(); ++k) {
            INFO(placed[i].name << " and " << placed[k].name);
            CHECK_FALSE(overlap(placed[i], placed[k]));
        }
    }
    // Near a window's shape rather than a tall column.
    const double aspect = (right - left) / (bottom - top);
    INFO("the patch is " << right - left << " by " << bottom - top);
    CHECK(aspect > 0.8);
    CHECK(aspect < 3.2);
    // Left to right by signal flow, and a modulator just left of its target.
    auto at = [&](const char* name) {
        for (const auto& p : placed)
            if (p.name == name) return p;
        FAIL("no " << name);
        return placed[0];
    };
    CHECK(at("clock").x < at("seq").x);
    CHECK(at("seq").x < at("quant").x);
    CHECK(at("osc1").x < at("amp1").x);
    CHECK(at("amp1").x < at("mix").x);
    CHECK(at("lfo1").x < at("osc1").x);
    CHECK(at("drift").x < at("lfo1").x);
    CHECK(at("lfo1").x > at("clock").x);  // pulled right, toward what it modulates

    // The same patch into the same rack lays out the same way.
    Fixture g;
    g.run("track.add", R"({"name": "Synth"})");
    g.run("rack.add", R"({"track": "tr_1"})");
    const auto again = placesOf(g, g.run("rack.patch", patch)["modules"]);
    REQUIRE(again.size() == placed.size());
    for (std::size_t i = 0; i < placed.size(); ++i) {
        CHECK(again[i].x == placed[i].x);
        CHECK(again[i].y == placed[i].y);
    }
}

TEST_CASE("rack.arrange: lays out a rack's modules in one undo step") {
    Fixture f;
    f.run("track.add", R"({"name": "Synth"})");
    f.run("rack.add", R"({"track": "tr_1"})");  // mo_1 Out
    json::Value made = f.run("rack.patch", R"({"rack": "rk_1",
        "modules": {"osc": {"type": "vco", "x": 900, "y": 700},
                    "amp": {"type": "vca", "x": 20, "y": 20},
                    "lfo": {"type": "lfo", "x": 500, "y": 0},
                    "env": {"type": "adsr", "x": 0, "y": 600}},
        "cables": [["osc.saw", "amp.in"], ["lfo.out", "osc.exp_fm"], ["env.envelope", "amp.cv"],
                   ["amp.out", "mo_1.l"]]})");
    auto module = [&](const char* name) {
        return f.app->head().modules.find(
            *model::Id::parse(made["modules"].find(name)->asString()));
    };
    const double before = module("osc")->x;
    json::Value moved = f.run("rack.arrange", R"({"rack": "rk_1"})");
    CHECK(moved["modules"].asObject().size() == 5);
    CHECK(module("lfo")->x < module("osc")->x);
    CHECK(module("osc")->x < module("amp")->x);
    CHECK(module("env")->x < module("amp")->x);
    CHECK(f.app->head().modules.find({model::Kind::Module, 1})->x > module("amp")->x);  // Out
    // From the modules' top-left corner.
    double left = 1e9;
    for (const auto& e : f.app->head().modules) left = std::min(left, e.value->x);
    CHECK(left == 0);
    f.run("history.undo", "{}");
    CHECK(module("osc")->x == before);
    CHECK(module("amp")->x == 20);

    // Some modules only, from a given corner.
    f.run("rack.arrange", R"({"rack": "rk_1", "modules": ["mo_2", "mo_3"], "x": 100, "y": 100})");
    CHECK(module("lfo")->x == 500);  // not named, not moved
    CHECK(std::min(module("osc")->x, module("amp")->x) == 100);
    CHECK(module("osc")->x < module("amp")->x);

    f.run("rack.add", R"({"track": "tr_1"})");  // rk_2, mo_6 Out
    auto other = f.call("rack.arrange", *json::parse(R"({"rack": "rk_1", "modules": ["mo_6"]})"));
    REQUIRE_FALSE(other);
    CHECK(other.error().code == "rack.module");
    CHECK(other.error().path == "/args/modules/0");
}

TEST_CASE("rack presets: a patch brings its own modules to another library") {
    Fixture here;
    here.write("level", kLevel);
    here.run("track.add", R"({"name": "Synth"})");
    here.run("rack.add", R"({"track": "tr_1"})");  // mo_1 Out
    here.run("rack.patch", R"({"rack": "rk_1", "modules": {"lvl": {"type": "com.example.level"}},
                               "cables": [["lvl.out", "mo_1.l"]]})");
    json::Value saved = here.run("rack.preset.save", R"({"rack": "rk_1", "name": "Constant"})");
    auto text = readTextFile(saved["path"].asString(), 1 << 20);
    REQUIRE(text);
    CHECK(text->find("\"sources\"") != std::string::npos);

    // Another machine: an empty library. The preset installs the module and plays it.
    Fixture there;
    there.run("project.set", R"({"console9": false})");
    there.run("track.add", R"({"name": "Synth"})");
    there.run("rack.add", R"({"track": "tr_1"})");
    json::Value args = json::Value::object();
    args["rack"] = "rk_1";
    args["path"] = saved["path"].asString();
    auto loaded = there.call("rack.preset.load", args);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
    REQUIRE((*loaded).find("installed") != nullptr);
    CHECK((*loaded)["installed"].at(0).asString() == "level");
    CHECK(there.level() == doctest::Approx(0.25 * 0.472135955).epsilon(1e-3));
    // Loaded again, the library has it: nothing to install, no warning.
    auto again = there.call("rack.preset.load", args);
    REQUIRE(again);
    CHECK((*again).find("installed") == nullptr);
    CHECK((*again).find("warnings") == nullptr);
    // A different module under the same id: the installed one plays, with a warning.
    there.run("module.edit", R"({"name": "level", "edits": [{"old": "0.25", "new": "0.5"}]})");
    auto differs = there.call("rack.preset.load", args);
    REQUIRE(differs);
    REQUIRE((*differs).find("warnings") != nullptr);
    CHECK((*differs)["warnings"].at(0).find("code")->asString() == "rack.preset_module");
}

TEST_CASE("module library: a project keeps copies of the modules it plays") {
    Fixture here;
    here.write("level", kLevel);
    here.run("project.set", R"({"console9": false})");
    here.run("track.add", R"({"name": "Synth"})");
    here.run("rack.add", R"({"track": "tr_1"})");
    here.run("rack.patch", R"({"rack": "rk_1", "modules": {"lvl": {"type": "com.example.level"}},
                               "cables": [["lvl.out", "mo_1.l"]]})");
    const auto song = here.dir / "song" / "song.json";
    json::Value args = json::Value::object();
    args["path"] = song.string();
    REQUIRE(here.call("project.save", args));
    const auto copy = song.parent_path() / "modules" / "level" / "level.cpp";
    REQUIRE(std::filesystem::exists(copy));

    // Opened where the library lacks it: the project's copy plays.
    Fixture there;
    REQUIRE(there.call("project.open", args));
    CHECK(there.level() == doctest::Approx(0.25 * 0.472135955).epsilon(1e-3));
    json::Value got = there.run("module.get", R"({"type": "com.example.level"})");
    CHECK(got["origin"].asString() == "project");
    // Edited there, it goes into that library and plays; the next save copies it.
    json::Value edited =
        there.run("module.edit",
                  R"({"type": "com.example.level", "edits": [{"old": "0.25", "new": "0.5"}]})");
    CHECK(edited["ok"].asBool());
    CHECK(there.level() == doctest::Approx(0.5 * 0.472135955).epsilon(1e-3));
    REQUIRE(there.call("project.save", args));
    auto saved = readTextFile(copy, 1 << 20);
    REQUIRE(saved);
    CHECK(saved->find("0.5") != std::string::npos);
}

TEST_CASE("rack presets: a malformed file is skipped with a warning") {
    Fixture f;
    REQUIRE(
        writeFileAtomic(f.app->rackPresetDir() / "bad.json",
                        R"({"schema": "supple.rack-preset/1.0", "name": "Bad", "description": 42,
                                "modules": [{"type": "com.suppleaudio.rack.out"}], "cables": []})"));
    json::Value list = f.run("rack.preset.list", R"({"factory": false})");
    CHECK(list["presets"].asArray().empty());
    REQUIRE(list.find("warnings") != nullptr);
    CHECK(list["warnings"].at(0).find("code")->asString() == "rack.preset_invalid");
}

namespace {
std::string otherId(std::string text, const std::string& id) {
    auto at = text.find("com.example.level");
    text.replace(at, std::string("com.example.level").size(), id);
    return text;
}
std::filesystem::path portableProject(Fixture& f, const char* library = "level") {
    f.write(library, kLevel);
    f.run("track.add", R"({"name":"Synth"})");
    f.run("rack.add", R"({"track":"tr_1"})");
    f.run("module.add", R"({"rack":"rk_1","type":"com.example.level"})");
    const auto song = f.dir / "song" / "song.json";
    json::Value a = json::Value::object();
    a["path"] = song.string();
    REQUIRE(f.call("project.save", a));
    return song;
}
void openPortable(Fixture& f, const std::filesystem::path& song) {
    json::Value a = json::Value::object();
    a["path"] = song.string();
    REQUIRE(f.call("project.open", a));
}
}  // namespace

TEST_CASE("module library: project edits preserve another library type with the same name") {
    Fixture here;
    const auto song = portableProject(here);
    Fixture there;
    const auto original = otherId(kLevel, "com.example.other");
    there.write("level", original);
    openPortable(there, song);
    auto result = there.call(
        "module.edit",
        *json::parse(R"({"type":"com.example.level","edits":[{"old":"0.25","new":"0.5"}]})"));
    REQUIRE(result);
    CHECK((*result)["name"].asString() == "level-2");
    const auto saved = readTextFile(there.app->moduleDir() / "level" / "level.cpp", 1 << 20);
    REQUIRE(saved);
    CHECK(*saved == original);
    CHECK(there.app->modules()->find("com.example.other") != nullptr);
}

TEST_CASE("module library: project edits resolve an existing library type by id") {
    Fixture here;
    const auto song = portableProject(here, "portable");
    Fixture there;
    there.write("global", kLevel);
    openPortable(there, song);
    auto result = there.call(
        "module.edit",
        *json::parse(R"({"type":"com.example.level","edits":[{"old":"0.25","new":"0.5"}]})"));
    REQUIRE(result);
    CHECK((*result)["name"].asString() == "global");
    const auto saved = readTextFile(there.app->moduleDir() / "global" / "global.cpp", 1 << 20);
    REQUIRE(saved);
    CHECK(saved->find("0.5") != std::string::npos);
}

TEST_CASE("module library: project snapshots keep different types with the same name") {
    Fixture here;
    const auto song = portableProject(here);
    Fixture there;
    there.write("level", otherId(kLevel, "com.example.other"));
    openPortable(there, song);
    REQUIRE(there.app->modules()->find("com.example.level"));
    REQUIRE(there.app->modules()->find("com.example.other"));
    there.run("module.add", R"({"rack":"rk_1","type":"com.example.other"})");
    const auto exported = there.dir / "export" / "song.json";
    json::Value a = json::Value::object();
    a["path"] = exported.string();
    REQUIRE(there.call("project.save", a));
    Fixture reopened;
    openPortable(reopened, exported);
    CHECK(reopened.app->modules()->find("com.example.level") != nullptr);
    CHECK(reopened.app->modules()->find("com.example.other") != nullptr);
}

TEST_CASE("module library: project snapshots preserve occupied destination files") {
    Fixture f;
    const auto song = portableProject(f);
    const auto destination = f.dir / "export" / "song.json";
    const auto occupied = destination.parent_path() / "modules" / "level" / "level.cpp";
    const auto unrelated = otherId(kLevel, "com.example.other");
    REQUIRE(writeFileAtomic(occupied, unrelated));
    json::Value args = json::Value::object();
    args["path"] = destination.string();
    REQUIRE(f.call("project.save", args));
    const auto kept = readTextFile(occupied, 1 << 20);
    REQUIRE(kept);
    CHECK(*kept == unrelated);
    const auto copy = occupied.parent_path().parent_path() / "level-2" / "level-2.cpp";
    REQUIRE(std::filesystem::exists(copy));
    // Saving an edited implementation updates the same id's suffix, without making another copy.
    f.run("module.edit", R"({"name":"level","edits":[{"old":"0.25","new":"0.5"}]})");
    REQUIRE(f.call("project.save", args));
    const auto updated = readTextFile(copy, 1 << 20);
    REQUIRE(updated);
    CHECK(updated->find("0.5") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(copy.parent_path().parent_path() / "level-3"));
    Fixture reopened;
    openPortable(reopened, destination);
    REQUIRE(reopened.app->modules()->find("com.example.level"));
    CHECK(reopened.app->modules()->find("com.example.other") != nullptr);
}

TEST_CASE("rack presets: embedded source ids are checked before installation") {
    Fixture f;
    f.run("track.add", R"({"name":"Synth"})");
    f.run("rack.add", R"({"track":"tr_1"})");
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.rack-preset/1.0";
    doc["name"] = "Wrong id";
    json::Value m = json::Value::object();
    m["type"] = "com.example.requested";
    doc["modules"] = json::Value::array();
    doc["modules"].push_back(m);
    json::Value src = json::Value::object();
    src["name"] = "unrelated";
    src["source"] = kLevel;
    doc["sources"] = json::Value::object();
    doc["sources"]["com.example.first"] = src;
    doc["sources"]["com.example.first"]["source"] = otherId(kLevel, "com.example.first");
    doc["sources"]["com.example.first"]["name"] = "first";
    doc["sources"]["com.example.requested"] = src;
    SUBCASE("mismatched id") {}
    SUBCASE("mismatched id for an installed type") {
        f.write("requested", otherId(kLevel, "com.example.requested"));
    }
    SUBCASE("source fails to compile") {
        doc["sources"]["com.example.requested"]["source"] = "broken module";
    }
    const auto path = f.dir / "bad-source.json";
    REQUIRE(writeFileAtomic(path, json::writeCanonical(doc)));
    json::Value a = json::Value::object();
    a["rack"] = "rk_1";
    a["path"] = path.string();
    auto loaded = f.call("rack.preset.load", a);
    REQUIRE_FALSE(loaded);
    CHECK(f.app->modules()->find("com.example.level") == nullptr);
    CHECK(loaded.error().code == "rack.preset_module");
    CHECK(loaded.error().path == "/args/sources/com.example.requested");
    CHECK_FALSE(loaded.error().message.empty());
    CHECK_FALSE(loaded.error().hint.empty());
    CHECK(f.app->modules()->find("com.example.first") == nullptr);
    CHECK_FALSE(std::filesystem::exists(f.app->moduleDir() / "first" / "first.cpp"));
    CHECK_FALSE(std::filesystem::exists(f.app->moduleDir() / "unrelated" / "unrelated.cpp"));
}

TEST_CASE("module library: project open warns when a broken copy falls back to the library") {
    Fixture here;
    const auto song = portableProject(here);
    REQUIRE(
        writeFileAtomic(song.parent_path() / "modules" / "level" / "level.cpp", "broken module"));
    Fixture there;
    std::string changed = kLevel;
    changed.replace(changed.find("0.25"), 4, "0.5");
    there.write("level", changed);
    json::Value a = json::Value::object();
    a["path"] = song.string();
    auto opened = there.call("project.open", a);
    REQUIRE(opened);
    const auto* selected = there.app->modules()->find("com.example.level");
    REQUIRE(selected);
    REQUIRE(opened->find("warnings"));
    const auto warning = json::writeCompact((*opened)["warnings"]);
    CHECK(warning.find("com.example.level") != std::string::npos);
    CHECK(warning.find("level.cpp") != std::string::npos);
    CHECK(warning.find("library version") != std::string::npos);
}

TEST_CASE("module library: project open reports unreadable copies with affected type ids") {
    Fixture here;
    const auto song = portableProject(here);
    // A source exceeding the read limit fails deterministically, including as root and on Windows.
    REQUIRE(writeFileAtomic(song.parent_path() / "modules" / "level" / "level.cpp",
                            std::string((1 << 20) + 1, ' ')));
    Fixture there;
    json::Value args = json::Value::object();
    args["path"] = song.string();
    auto opened = there.call("project.open", args);
    REQUIRE(opened);
    REQUIRE(opened->find("warnings"));
    const std::string warning = json::writeCompact((*opened)["warnings"]);
    CHECK(warning.find("com.example.level") != std::string::npos);
    CHECK(warning.find("level.cpp") != std::string::npos);
    CHECK(warning.find("unavailable") != std::string::npos);
}

TEST_CASE("Note Out routes: object endpoints, channel maps, round trip and cycles") {
    Fixture f;
    f.run("track.add", R"({"name":"Generator"})");
    f.run("rack.add", R"({"track":"tr_1"})");
    auto patch = f.run(
        "rack.patch",
        R"({"rack":"rk_1","modules":{"clock":{"type":"transport"},"notes":{"type":"notes_out","params":{"channel":3}}},"cables":[["clock.clock","notes.gate"],["clock.beat_phase","notes.pitch"]]})");
    const auto noteOut = patch["modules"]["notes"].asString();
    f.run("track.add", R"({"name":"Instrument"})");
    json::Value args = json::Value::object();
    args["from"] = json::Value::object();
    args["from"]["module"] = noteOut;
    args["to"] = json::Value::object();
    args["to"]["track"] = "tr_2";
    args["midi"] = json::Value::object();
    auto result = f.call("route.add", args);
    REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
    const auto route = result->find("id")->asString();
    auto id = model::Id::parse(route);
    REQUIRE(id);
    const auto* made = f.app->head().routes.find(*id);
    REQUIRE(made);
    REQUIRE(made->midi);
    CHECK(made->midi->fromChannel == 0);
    CHECK(made->midi->toChannel == 0);
    json::Value edits = json::Value::object();
    edits["route"] = route;
    edits["midi"] = json::parse(R"({"from_channel":3,"to_channel":5})").value();
    auto edited = f.call("route.set", edits);
    REQUIRE(edited);
    auto reopened = model::parseProject(model::serializeProject(f.app->head()));
    REQUIRE(reopened);
    REQUIRE(reopened->routes.find(*id)->midi);
    CHECK(reopened->routes.find(*id)->midi->fromChannel == 3);
    CHECK(reopened->routes.find(*id)->midi->toChannel == 5);
    auto input = f.run("module.add", R"({"rack":"rk_1","type":"in"})");
    json::Value feedback = json::Value::object();
    feedback["from"] = "tr_2";
    feedback["to"] = input["module"];
    const auto before = model::serializeProject(f.app->head());
    auto cycle = f.call("route.add", feedback);
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "route.cycle");
    CHECK_FALSE(cycle.error().hint.empty());
    CHECK(model::serializeProject(f.app->head()) == before);
    args.asObject().erase("midi");
    auto missing = f.call("route.add", args);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "route.endpoint");
    args["midi"] = json::Value::object();
    args["volume_db"] = -3;
    auto audio = f.call("route.add", args);
    REQUIRE_FALSE(audio);
    CHECK(audio.error().code == "route.midi");
    json::Value remove = json::Value::object();
    remove["module"] = noteOut;
    REQUIRE(f.call("module.remove", remove));
    CHECK_FALSE(f.app->head().routes.contains(*id));
}

TEST_CASE("command warnings: a silent apply-FX render reaches the caller") {
    Fixture f;
    f.run("project.set", R"({"console9":false})");
    f.run("track.add", R"({"name":"No instrument"})");
    f.run("item.add", R"({"track":"tr_1","position":0,"length":1,"notation":"1: C4:w"})");
    auto rendered = f.run("item.apply-fx", R"({"item":"it_1"})");
    REQUIRE(rendered.find("warnings"));
    bool silent = false;
    for (const auto& warning : rendered["warnings"].asArray()) {
        if (const auto* code = warning.find("code"); code && *code == "render.silent") {
            silent = true;
            CHECK_FALSE(warning.find("message")->asString().empty());
            CHECK(warning.find("path")->asString().empty());
        }
    }
    CHECK(silent);
    CHECK_FALSE(rendered["take"].asString().empty());
    CHECK(std::filesystem::exists(rendered["path"].asString()));
    auto next = f.run("track.list");
    CHECK(next.find("warnings") == nullptr);
}

namespace {
midi::Sequence noteRecordingScene(Fixture& f, bool instrument) {
    f.run("project.set", R"({"console9":false})");
    f.run("track.add", R"({"name":"Generator"})");
    f.run("track.add", R"({"name":"Recorded"})");
    f.run("track.add", R"({"name":"Capture reference"})");
    if (instrument) f.run("fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.sine"})");
    f.run("rack.add", R"({"track":"tr_1"})");
    f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
    f.run("module.add", R"({"rack":"rk_1","type":"notes_out"})");
    for (const auto* port : {"pitch", "gate", "velocity"})
        f.run("cable.add",
              std::string(R"({"from":"mo_2.)") + port + R"(","to":"mo_3.)" + port + R"("})");
    f.run("route.add", R"({"from":{"module":"mo_3"},"to":{"track":"tr_2"},"midi":{}})");
    const char* phrase = "voice v; 1: C4:q E4:e r:e [G4 B4]:h";
    auto sourceItem =
        f.run("item.add", std::string(R"({"track":"tr_1","position":0,"length":4,"notation":")") +
                              phrase + R"("})");
    // MIDI files may contain equal pitches in separate voices, although a
    // notation chord deliberately rejects duplicate pitches.
    const auto itemId = *model::Id::parse(sourceItem["item"].asString());
    const auto source =
        *f.app->head().takes.find(*f.app->head().items.find(itemId)->activeTake)->source;
    auto transaction = f.app->beginTransaction(origin, "Equal-pitch voices");
    REQUIRE(transaction);
    f.app->txn().modify<model::MidiSource>(source, [](auto& midi) {
        for (auto& note : midi.sequence.notes)
            if (note.pitch == 71) {
                note.pitch = 67;
                note.velocity = 70;  // keep the equal-pitch notes distinct in canonical MIDI
            }
    });
    REQUIRE(f.app->commitTransaction(*transaction));
    f.run("track.set",
          R"({"track":"tr_2","armed":true,"record_mode":"midi_replace","monitor":"off"})");
    auto reference = f.run(
        "rack.capture",
        R"({"rack":"rk_1","pitch":"mo_2.pitch","gate":"mo_2.gate","velocity":"mo_2.velocity","track":"tr_3","beats":4,"at":8})");
    auto expected =
        f.app->head().midi.find(*model::Id::parse(reference["source"].asString()))->sequence;
    REQUIRE(expected.notes.size() == 4);
    return expected;
}
}  // namespace

TEST_CASE("Note Out recording: a take equals rack.capture in both executors" *
          doctest::description("rt")) {
    EngineOptions options{.anticipative = false, .workers = 0};
    bool device = false;
    SUBCASE("callback") {}
    SUBCASE("anticipative") { options.anticipative = true; }
    SUBCASE("anticipative with workers") { options = {.workers = 2}; }
    SUBCASE("device and Note Out together") {
        options = {.workers = 2};
        device = true;
    }
    for (bool instrument : {false, true}) {
        INFO("instrument " << instrument);
        Fixture f;
        auto expected = noteRecordingScene(f, instrument);
        DummyBackend::Options dummy{.mode = DummyBackend::Mode::Manual};
        if (device) {
            f.run("track.set", R"({"track":"tr_2","record_midi":"*"})");
            dummy.midiPorts = {"Keyboard"};
            dummy.midi = [](auto& events, std::uint32_t frames, std::int64_t pos) {
                for (auto at : {std::int64_t{0}, std::int64_t{96000}})
                    if (at >= pos && at < pos + frames) {
                        MidiInputEvent event;
                        event.frame = static_cast<std::uint32_t>(at - pos);
                        event.size = 3;
                        event.bytes[0] = at == 0 ? 0x90 : 0x80;
                        event.bytes[1] = 48;
                        event.bytes[2] = at == 0 ? 70 : 0;
                        events.push_back(event);
                    }
            };
            expected.notes.push_back({0, 4 * expected.ppq, 0, 48, 70, 0});
            expected.canonicalize();
        }
        DummyBackend backend(std::move(dummy));
        REQUIRE(f.app->startEngine(
            backend, {.sampleRate = 48000, .blockSize = 37, .midiInputs = device}, options));
        f.run("transport.record");
        for (int left = 2600; left > 0;) {
            if (options.anticipative) REQUIRE(f.app->engine()->awaitRendered(4096));
            const int count = std::min(left, 4096 / 37);
            backend.run(count);
            left -= count;
        }
        auto stopped = f.run("transport.stop");
        CHECK(f.app->engine()->processErrors() == 0);
        f.app->stopEngine();
        REQUIRE(stopped["recorded"].size() == 1);
        const auto item = *model::Id::parse(stopped["recorded"].at(0).find("item")->asString());
        const auto& session = f.app->head();
        const auto* take = session.takes.find(*session.items.find(item)->activeTake);
        REQUIRE(take);
        REQUIRE(take->source);
        const auto& actual = session.midi.find(*take->source)->sequence;
        CHECK(actual.ppq == expected.ppq);
        CHECK(actual.notes == expected.notes);
        CHECK(stopped["recorded"].at(0).find("lost_frames")->asInt() == 0);
    }
}

TEST_CASE("Note Out recording: loop takes, punch clipping and overdub reuse the recorder" *
          doctest::description("rt")) {
    enum class Mode { Loop, Punch, Overdub };
    Mode mode = Mode::Loop;
    SUBCASE("loop") {}
    SUBCASE("punch") { mode = Mode::Punch; }
    SUBCASE("overdub") { mode = Mode::Overdub; }
    for (const auto options :
         {EngineOptions{.anticipative = false, .workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative);
        Fixture f;
        auto expected = noteRecordingScene(f, true);
        std::optional<model::Id> previous;
        if (mode == Mode::Loop) f.run("transport.loop", R"({"start_beat":0,"end_beat":4})");
        if (mode == Mode::Punch) f.run("transport.punch", R"({"start_beat":0.5,"end_beat":2.5})");
        if (mode == Mode::Overdub) {
            previous = *model::Id::parse(
                f.run("item.add",
                      R"({"track":"tr_2","position":0,"length":4,"notation":"1: C3:w"})")["item"]
                    .asString());
            f.run("track.set", R"({"track":"tr_2","record_mode":"midi_overdub"})");
            expected.notes.push_back({0, 4 * expected.ppq, 0, 48, 96, 0});
            expected.canonicalize();
        }
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 96}, options));
        f.run("transport.record");
        for (int left = mode == Mode::Loop ? 2000 : 1000; left > 0;) {
            if (options.anticipative) REQUIRE(f.app->engine()->awaitRendered(4096));
            const int count = std::min(left, 4096 / 96);
            backend.run(count);
            left -= count;
        }
        auto stopped = f.run("transport.stop");
        CHECK(f.app->engine()->processErrors() == 0);
        f.app->stopEngine();
        REQUIRE(stopped["recorded"].size() == 1);
        const auto item = *model::Id::parse(stopped["recorded"].at(0).find("item")->asString());
        const auto& session = f.app->head();
        const auto* recorded = session.items.find(item);
        REQUIRE(recorded);
        CHECK(recorded->takes.size() == (mode == Mode::Punch ? 1 : 2));
        if (previous) CHECK(item == *previous);
        for (std::size_t i = mode == Mode::Overdub ? 1 : 0; i < recorded->takes.size(); ++i) {
            const auto& seq =
                session.midi.find(*session.takes.find(recorded->takes[i])->source)->sequence;
            if (mode != Mode::Punch)
                CHECK(seq.notes == expected.notes);
            else {
                REQUIRE(seq.notes.size() == 4);
                CHECK(seq.notes[0].start == 0);
                CHECK(seq.notes[1].start == seq.ppq / 2);
                CHECK(seq.notes[2].start == 3 * seq.ppq / 2);
                for (const auto& note : seq.notes) CHECK(note.length == seq.ppq / 2);
            }
        }
        f.run("history.undo");
        CHECK((previous ? f.app->head().items.find(*previous)->takes.size() == 1
                        : !f.app->head().items.contains(item)));
    }
}

TEST_CASE("rack Out recording: the file equals offline output in both executors" *
          doctest::description("rt")) {
    bool inputFx = false, loop = false;
    SUBCASE("plain input") {}
    SUBCASE("input FX latency") { inputFx = true; }
    SUBCASE("loop passes") { loop = true; }
    for (const auto options : {EngineOptions{.anticipative = false, .workers = 0},
                               EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        Fixture f;
        f.run("project.set", R"({"console9":false})");
        f.run("track.add", R"({"name":"Recorded rack"})");
        f.run("rack.add", R"({"track":"tr_1"})");
        f.run("module.add", R"({"rack":"rk_1","type":"transport"})");
        f.run("cable.add", R"({"from":"mo_2.beat_phase","to":"mo_1.l"})");
        const auto expected =
            f.app->renderSession(f.app->head(), {.endSeconds = 2.0,
                                                 .blockSize = 37,
                                                 .threads = 1,
                                                 .track = model::Id{model::Kind::Track, 1},
                                                 .tap = model::Tap::PreFx});
        REQUIRE(expected);
        f.run("track.set", R"({"track":"tr_1","armed":true,"monitor":"off","volume_db":-12})");
        if (inputFx) {
            f.run("fx.add",
                  R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"input"})");
            auto transaction = f.app->beginTransaction(origin, "Input latency");
            REQUIRE(transaction);
            f.app->txn().modify<model::FxInstance>({model::Kind::Fx, 1}, [](auto& fx) {
                fx.patch =
                    json::parse(
                        R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                        .value();
            });
            REQUIRE(f.app->commitTransaction(*transaction));
        }
        if (loop) f.run("transport.loop", R"({"start_beat":0,"end_beat":2})");
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 96, .inputs = 0},
                                   options));
        f.run("transport.record");
        for (int left = 1000; left > 0;) {
            if (options.anticipative) REQUIRE(f.app->engine()->awaitRendered(4096));
            const int count = std::min(left, 4096 / 96);
            backend.run(count);
            left -= count;
        }
        auto stopped = f.run("transport.stop");
        CHECK(f.app->engine()->processErrors() == 0);
        f.app->stopEngine();
        REQUIRE(stopped["recorded"].size() == 1);
        auto& result = stopped["recorded"].asArray().front();
        auto audio = audio::readAudio(result["file"].asString());
        REQUIRE(audio);
        REQUIRE(audio->channels.size() == 2);
        const std::size_t frames = 96000;
        const std::size_t content = inputFx ? frames - 64 : frames;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            REQUIRE(audio->channels[channel].size() == frames);
            CHECK(std::memcmp(audio->channels[channel].data(),
                              expected->audio.channels[channel].data(),
                              content * sizeof(float)) == 0);
            CHECK(std::all_of(
                audio->channels[channel].begin() + static_cast<std::ptrdiff_t>(content),
                audio->channels[channel].end(), [](float sample) { return sample == 0; }));
        }
        CHECK(result["lost_frames"] == 0);
        if (loop) CHECK(result["takes"].size() == 2);
        f.run("history.undo");
        CHECK(f.app->head().items.size() == 0);
    }
}

TEST_CASE("rack.get: prepared latency includes oversampling and reports individual outputs") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("cable.add", R"({"from":"mo_2.out_l","to":"mo_1.l"})");
    auto set = [&](int os, bool bypass = false) {
        auto tx = f.app->beginTransaction(origin, "Latency mode");
        REQUIRE(tx);
        f.app->txn().modify<model::FxInstance>({model::Kind::Fx, 1}, [&](auto& fx) {
            fx.patch = *json::parse(
                R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})");
            fx.oversampling = os;
        });
        f.app->txn().modify<model::RackModule>({model::Kind::Module, 2},
                                               [&](auto& m) { m.bypass = bypass; });
        REQUIRE(f.app->commitTransaction(*tx));
    };
    set(1);
    auto rack = f.run("rack.get", R"({"rack":"rk_1","detail":"full"})");
    CHECK(rack["latency_samples"].asInt() == 64);
    CHECK(rack["outs"].asArray()[0]["latency_samples"].asInt() == 64);
    set(2);
    rack = f.run("rack.get", R"({"rack":"rk_1","detail":"full"})");
    CHECK(rack["latency_samples"].asInt() > 32);  // plug-in latency / 2 plus resampler
    MediaCache media;
    auto plan = supple::engine::detail::compile(f.app->head(), f.app->instances(), 0, 128);
    REQUIRE(plan);
    plan->media = &media;
    REQUIRE(supple::engine::detail::prepare(*plan, f.app->head()));
    for (const auto& node : plan->nodes)
        if (node.rack) CHECK(node.latChain == rack["latency_samples"].asInt());
    set(1, true);
    CHECK(f.run("rack.get", R"({"rack":"rk_1","detail":"full"})")["latency_samples"].asInt() == 0);
}

TEST_CASE("rack.capture: an async snapshot completes once, matches sync bytes and undoes") {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    AppOptions options;
    options.jobFinishedHook = [&] {
        std::lock_guard lock(mutex);
        finished = true;
        cv.notify_all();
    };
    Fixture sync, async({}, options);
    auto prepare = [](Fixture& f) {
        f.run("track.add");
        f.run("track.add");
        f.run("rack.add", R"({"track":"tr_1"})");
        f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
        f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
        auto tx = f.app->beginTransaction(origin, "Case-insensitive plugin reference");
        REQUIRE(tx);
        f.app->txn().modify<model::FxInstance>(
            {model::Kind::Fx, 1}, [](auto& fx) { fx.plugin.id = "COM.SUPPLEAUDIO.TEST.GAIN"; });
        REQUIRE(f.app->commitTransaction(*tx));
    };
    prepare(sync);
    prepare(async);
    async.run("branch.create", R"({"name":"capture"})");
    auto args = *json::parse(R"({"rack":"rk_1","track":"tr_2","pitch":"mo_2.pitch",
        "gate":"mo_2.gate","velocity":"mo_2.velocity","beats":4,"at":8,"quantize":0.25,
        "notes":"voice v; 1: C4:q E4:e r:e [G4 B4]:h"})");
    const auto expected = sync.call("rack.capture", args);
    REQUIRE(expected);
    const auto version = async.app->head().version;
    args["async"] = true;
    auto job = async.call("rack.capture", args);
    REQUIRE(job);
    REQUIRE(job->find("job"));
    CHECK(async.app->head().version == version);
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return finished; });
    }
    async.bus->flushJobEvents();
    auto state = async.bus->jobs().status((*job)["job"].asString());
    REQUIRE(state);
    REQUIRE((*state)["status"].asString() == "done");
    auto& actual = (*state)["result"];
    CHECK(json::writeCompact(actual) == json::writeCompact(*expected));
    const auto source = *model::Id::parse(actual["source"].asString());
    CHECK(json::writeCompact(model::toJson(*async.app->head().midi.find(source))) ==
          json::writeCompact(model::toJson(*sync.app->head().midi.find(source))));
    async.run("history.undo");
    CHECK(async.app->head().items.empty());
    CHECK(async.app->head().midi.empty());
    async.run("history.redo");
    CHECK(async.app->head().items.size() == 1);
    async.run("branch.switch", R"({"name":"main"})");
    async.run("track.add");
    async.run("branch.merge", R"({"from":"capture"})");
    CHECK(async.app->head().items.size() == 1);
    REQUIRE(async.app->head().midi.size() == 1);
    CHECK(json::writeCompact(model::toJson(*async.app->head().midi.begin()->value)) ==
          json::writeCompact(model::toJson(*sync.app->head().midi.find(source))));
}

TEST_CASE("rack.capture: cancelling or changing the target before completion publishes no notes") {
    for (int change = 0; change <= 16; ++change) {
        CAPTURE(change);
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false, release = false, finished = false;
        AppOptions options;
        options.jobStartedHook = [&] {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        };
        options.jobFinishedHook = [&] {
            std::lock_guard lock(mutex);
            finished = true;
            cv.notify_all();
        };
        Fixture f({}, options);
        struct Unblock {
            std::mutex& mutex;
            std::condition_variable& cv;
            bool& release;
            ~Unblock() {
                std::lock_guard lock(mutex);
                release = true;
                cv.notify_all();
            }
        } unblock{mutex, cv, release};
        f.run("track.add");
        f.run("track.add");
        f.run("rack.add", R"({"track":"tr_1"})");
        f.run("module.add", R"({"rack":"rk_1","type":"notes"})");

        auto addItem = [&](model::Id track, double at) {
            auto tx = f.app->beginTransaction(origin, "Target item");
            REQUIRE(tx);
            model::MidiSource source;
            source.sequence.length = 960;
            source.sequence.notes = {{0, 960, 0, 60, 96, 0}};
            const auto sourceId = f.app->txn().create(source);
            model::Take take;
            take.source = sourceId;
            const auto takeId = f.app->txn().create(take);
            model::Item item;
            item.track = track;
            item.position = at;
            item.length = 1;
            item.takes = {takeId};
            item.activeTake = takeId;
            const auto id = f.app->txn().create(item);
            REQUIRE(f.app->commitTransaction(*tx));
            return id;
        };
        model::Id existing;
        if (change == 15 || change == 16)
            existing = addItem({model::Kind::Track, 2}, change == 15 ? 16 : 0);
        if (change == 11)
            f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
        if (change == 12) {
            auto tx = f.app->beginTransaction(origin, "Automation item");
            REQUIRE(tx);
            model::Envelope env;
            env.target = {{model::Kind::Module, 2}, "voices"};
            const auto envId = f.app->txn().create(env);
            model::AutomationItem item;
            item.envelope = envId;
            item.points = {{0, 8}};
            f.app->txn().create(item);
            REQUIRE(f.app->commitTransaction(*tx));
        }
        if (change == 14) {
            f.run("track.add");
            f.run("module.add", R"({"rack":"rk_1","type":"in"})");
        }
        auto job = f.run("rack.capture", R"({"rack":"rk_1","track":"tr_2",
            "pitch":"mo_2.pitch","gate":"mo_2.gate","beats":4,"async":true,
            "notes":"voice v; 1: C4:w"})");
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }
        if (change == 0) REQUIRE(f.bus->jobs().cancel(job["job"].asString()));
        if (change == 1) f.run("rack.remove", R"({"rack":"rk_1"})");
        if (change == 2) f.run("track.remove", R"({"track":"tr_2"})");
        if (change == 3) f.run("module.set", R"({"module":"mo_2","label":"Edited"})");
        if (change == 4) f.run("tempo.set", R"({"points":[{"beat":0,"bpm":90}]})");

        if (change == 7) f.run("track.set", R"({"track":"tr_2","volume_db":-6})");
        if (change == 8 || change == 9 || change == 13)
            addItem({model::Kind::Track, change == 13 ? 1u : 2u}, change == 8 ? 16 : 0);
        if (change == 10 || change == 11 || change == 12 || change == 14 || change == 15 ||
            change == 16) {
            auto tx = f.app->beginTransaction(origin, "Concurrent edit");
            REQUIRE(tx);
            if (change == 10 || change == 11) {
                model::Envelope env;
                env.target = {change == 10 ? model::Id{model::Kind::Module, 2}
                                           : model::Id{model::Kind::Fx, 1},
                              change == 10 ? "voices" : "gain"};
                env.points = {{0, change == 10 ? 4. : -6.}};
                f.app->txn().create(env);
            }
            if (change == 12)
                for (const auto& entry : f.app->head().autoItems)
                    f.app->txn().modify<model::AutomationItem>(
                        entry.id, [](auto& item) { item.points = {{0, 4}}; });
            if (change == 14) {
                model::Route route;
                route.from.track = model::Id{model::Kind::Track, 3};
                route.to.module = model::Id{model::Kind::Module, 3};
                f.app->txn().create(route);
            }
            if (change == 15)
                f.app->txn().modify<model::Item>(existing, [](auto& item) { item.gainDb = -6; });
            if (change == 16)
                for (const auto& entry : f.app->head().midi)
                    f.app->txn().modify<model::MidiSource>(
                        entry.id, [](auto& source) { source.sequence.notes[0].pitch = 67; });
            REQUIRE(f.app->commitTransaction(*tx));
        }
        const auto itemsBefore = f.app->head().items.size(), midiBefore = f.app->head().midi.size();
        const auto version = f.app->head().version;
        {
            std::lock_guard lock(mutex);
            release = true;
            cv.notify_all();
        }
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return finished; });
        }
        if (change == 5) REQUIRE(f.bus->jobs().cancel(job["job"].asString()));
        // An unrelated edit is allowed; the worker uses its captured catalogue and session.
        if (change == 6) f.run("track.add");
        f.bus->flushJobEvents();
        auto state = f.bus->jobs().status(job["job"].asString());
        REQUIRE(state);
        if (change == 6 || change == 7 || change == 8 || change == 13 || change == 14 ||
            change == 15) {
            CHECK((*state)["status"].asString() == "done");
            CHECK(f.app->head().items.size() == itemsBefore + 1);
        } else {
            CHECK(f.app->head().items.size() == itemsBefore);
            CHECK(f.app->head().midi.size() == midiBefore);
            CHECK(f.app->head().version == version);
            CHECK((*state)["status"].asString() ==
                  ((change == 0 || change == 5) ? "cancelled" : "failed"));
            if (change != 0 && change != 5) {
                CHECK((*state)["error"]["code"].asString() == "rack.capture_stale");
                CHECK_FALSE((*state)["error"]["hint"].asString().empty());
            }
        }
    }
}

TEST_CASE("rack.capture: sync joins a batch, async refuses a transaction") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
    auto capture = *json::parse(R"({"rack":"rk_1","track":"tr_1","pitch":"mo_2.pitch",
        "gate":"mo_2.gate","beats":1,"notes":"voice v; 1: C4:q r:*"})");
    auto args = json::Value::object();
    args["steps"] = json::Value::array();
    auto step = json::Value::object();
    step["command"] = "rack.capture";
    step["args"] = capture;
    args["steps"].push_back(step);
    REQUIRE(f.call("batch", args));
    CHECK(f.app->head().items.size() == 1);
    f.run("history.undo");
    CHECK(f.app->head().items.empty());
    args["steps"].asArray()[0]["args"]["async"] = true;
    const auto refused = f.call("batch", args);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "batch.step_failed");
    REQUIRE_FALSE(refused.error().details.empty());
    CHECK(refused.error().details[0].code == "batch.not_transactional");
    CHECK(f.app->head().items.empty());
}

TEST_CASE("rack cables: numeric CLAP ids modulate a raw plugin and survive save and preset reopen" *
          doctest::description("rt")) {
    Fixture f;
    f.run("project.set", R"({"console9":false})");
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","type":"transport"})");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.raw-gain"})");
    const auto* fx = f.app->head().fx.find({model::Kind::Fx, 1});
    REQUIRE(fx);
    auto schema = f.app->fxSchema(*fx);
    REQUIRE(schema);
    CHECK(schema->isNull());
    f.run("cable.add", R"({"from":"mo_2.playing","to":"mo_3.in_l"})");
    f.run("cable.add", R"({"from":"mo_3.out_l","to":"mo_1.l"})");
    const auto plain = f.level();
    f.run("cable.add", R"({"from":"mo_2.playing","to":"fx_1.0","amount":0.1})");
    CHECK(f.level() > plain);
    for (const auto* invalid : {"12", "4294967296"}) {
        auto args = json::Value::object();
        args["from"] = "mo_2.playing";
        args["to"] = std::string("fx_1.") + invalid;
        const auto bad = f.call("cable.add", args);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == "rack.port");
        CHECK(bad.error().path == "/args/to");
        CHECK(bad.error().hint ==
              "available parameter keys and numeric ids: 0, 1; fx.describe lists their ranges");
    }
    auto nonmod = f.call("cable.add", *json::parse(R"({"from":"mo_2.playing","to":"fx_1.1"})"));
    REQUIRE_FALSE(nonmod);
    CHECK(nonmod.error().code == "rack.modulation");
    auto saved = f.run("rack.preset.save", R"({"rack":"rk_1","name":"Numeric"})");
    auto args = json::Value::object();
    args["path"] = (f.dir / "song.json").string();
    REQUIRE(f.call("project.save", args));
    Fixture reopened;
    REQUIRE(reopened.call("project.open", args));
    CHECK(reopened.level() == f.level());
    bool numeric = false;
    for (const auto& cable : reopened.app->head().cables)
        numeric |= cable.value->to.object.kind == model::Kind::Fx && cable.value->to.key == "0";
    CHECK(numeric);
    Fixture preset;
    preset.run("project.set", R"({"console9":false})");
    preset.run("rack.add");
    args["rack"] = "rk_1";
    args["path"] = saved["path"];
    REQUIRE(preset.call("rack.preset.load", args));
    CHECK(preset.level() == f.level());
    OfflineSettings settings;
    settings.endSeconds = .05;
    settings.threads = 1;
    auto offline = f.app->renderSession(f.app->head(), settings);
    REQUIRE(offline);
    for (bool anticipative : {false, true})
        for (int workers : {0, 2}) {
            CAPTURE(anticipative);
            CAPTURE(workers);
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
            REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 37},
                                       {.anticipative = anticipative, .workers = workers}));
            f.run("transport.play");
            for (int i = 0; i < 180; ++i) {
                if (anticipative) REQUIRE(f.app->engine()->awaitRendered(37));
                backend.run(1);
            }
            const auto delay = f.app->engine()->outputLatency();
            f.app->stopEngine();
            CHECK(std::equal(offline->audio.channels[0].begin(), offline->audio.channels[0].end(),
                             backend.capture().channels[0].begin() + delay));
        }
}

TEST_CASE("rack cables: numeric keys work in rack.patch and unknown ids name available keys") {
    Fixture f;
    f.run("rack.add");
    f.run("rack.patch", R"({"rack":"rk_1","modules":{"clock":{"type":"transport"},
        "effect":{"plugin":"com.suppleaudio.test.gain"}},
        "cables":[["clock.playing","effect.0",0.1]]})");
    const auto bad = f.call("cable.add", *json::parse(R"({"from":"mo_2.playing","to":"fx_1.12"})"));
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "rack.port");
    CHECK(bad.error().hint ==
          "available parameter keys and numeric ids: gain, latency, 0, 1; fx.describe lists their "
          "ranges");
    const auto version = f.app->head().version;
    const auto patch = f.call("rack.patch", *json::parse(R"({"rack":"rk_1",
        "modules":{"other":{"type":"transport"}},"cables":[["other.playing","fx_1.999"]]})"));
    REQUIRE_FALSE(patch);
    CHECK(f.app->head().version == version);
    // An invalid serialized target is refused before replacing the open project.
    auto doc = model::projectToJson(f.app->head());
    doc["cables"]["cb_1"]["to"] = "fx_1.12";
    const auto path = f.dir / "invalid.json";
    REQUIRE(writeFileAtomic(path, json::writeCanonical(doc)));
    std::vector<schema::Warning> warnings;
    REQUIRE(f.app->open(path, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].code == "rack.port");
    CHECK(f.app->head().cables.find({model::Kind::Cable, 1})->to.key == "12");
    // A preset's numeric target is validated too.
    auto saved = f.run("rack.preset.save", R"({"rack":"rk_1","name":"Bad numeric"})");
    auto text = readTextFile(saved["path"].asString(), 1 << 20);
    REQUIRE(text);
    auto preset = json::parse(*text);
    REQUIRE(preset);
    (*preset)["cables"].asArray()[0]["to"]["port"] = "999";
    REQUIRE(writeFileAtomic(saved["path"].asString(), json::writeCanonical(*preset)));
    auto args = json::Value::object();
    args["rack"] = "rk_1";
    args["path"] = saved["path"];
    auto loaded = f.call("rack.preset.load", args);
    REQUIRE(loaded);
    REQUIRE((*loaded)["warnings"].isArray());
    CHECK((*loaded)["warnings"].asArray().size() == 1);
    CHECK((*loaded)["warnings"].asArray()[0]["code"].asString() == "rack.preset_cable");
}

TEST_CASE("rack cables: validation uses restored state and live parameter rescans") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","type":"transport"})");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.edge"})");
    auto tx = f.app->beginTransaction(origin, "Dynamic parameter state");
    REQUIRE(tx);
    const std::string hash = "sha256:dynamic-parameter";
    f.app->txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{{53, 0, 0, 0}}));
    f.app->txn().modify<model::FxInstance>({model::Kind::Fx, 1},
                                           [&](auto& fx) { fx.state = hash; });
    REQUIRE(f.app->commitTransaction(*tx));
    f.run("cable.add", R"({"from":"mo_2.playing","to":"fx_1.105"})");
    auto missing = f.call("cable.add", *json::parse(R"({"from":"mo_2.playing","to":"fx_1.7"})"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "rack.port");
    const auto path = f.dir / "dynamic.json";
    REQUIRE(f.app->save(path));
    Fixture reopened;
    std::vector<schema::Warning> warnings;
    REQUIRE(reopened.app->open(path, &warnings));
    CHECK(warnings.empty());
    REQUIRE(reopened.app->head().cables.size() == 1);
    CHECK(reopened.app->head().cables.find({model::Kind::Cable, 1})->to.key == "105");
    f.run("cable.remove", R"({"from":"mo_2.playing","to":"fx_1.105"})");
    // The model is now in the default state, but a live instance changes its parameter list.
    tx = f.app->beginTransaction(origin, "Default parameter state");
    REQUIRE(tx);
    f.app->txn().modify<model::FxInstance>({model::Kind::Fx, 1},
                                           [&](auto& fx) { fx.state.reset(); });
    REQUIRE(f.app->commitTransaction(*tx));
    const auto* fx = f.app->head().fx.find({model::Kind::Fx, 1});
    auto instance = f.app->instances().get({model::Kind::Fx, 1}, *fx, f.app->head());
    REQUIRE(instance);
    REQUIRE((*instance)->loadState(std::vector<std::uint8_t>{53, 0, 0, 0}));
    f.run("cable.add", R"({"from":"mo_2.playing","to":"fx_1.105"})");
}

TEST_CASE("rack cables: loading probes each FX once and skips probes without warnings") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
    for (int i = 0; i < 4; ++i) {
        f.run("module.add", R"({"rack":"rk_1","type":"transport"})");
        auto args = json::Value::object();
        args["from"] = "mo_" + std::to_string(i + 3) + ".playing";
        args["to"] = "fx_1.0";
        REQUIRE(f.call("cable.add", args));
    }
    const auto info = f.app->findPlugin("com.suppleaudio.test.gain");
    REQUIRE(info);
    // Keep the library loaded while opening replaces the app's instance pool.
    auto keeper = host::Instance::create(*info, f.app->options().identity);
    REQUIRE(keeper);
    struct Control {
        void (*edit)(const clap_plugin_t*, double, std::uint32_t);
        std::uint32_t (*deliveries)(const clap_plugin_t*);
        void (*latency)(const clap_plugin_t*, bool);
        std::uint32_t (*creates)();
    };
    const auto* raw = (*keeper)->raw();
    const auto* control =
        static_cast<const Control*>(raw->get_extension(raw, "supple.test.gain-control"));
    REQUIRE(control);
    const auto path = f.dir / "many-cables.json";
    REQUIRE(f.app->save(path));
    auto before = control->creates();
    REQUIRE(f.app->open(path));
    CHECK(control->creates() == before);
    std::vector<schema::Warning> warnings;
    before = control->creates();
    REQUIRE(f.app->open(path, &warnings));
    CHECK(warnings.empty());
    CHECK(control->creates() == before + 1);
    REQUIRE(f.app->head().cables.size() == 4);
    auto saved = f.run("rack.preset.save", R"({"rack":"rk_1","name":"Many cables"})");
    // The preset's normal patch preparation reuses a scratch instance; count
    // only the additional restored probe used to check its four cables.
    REQUIRE(f.app->fxSchema(*f.app->head().fx.find({model::Kind::Fx, 1})));
    auto args = json::Value::object();
    args["rack"] = "rk_1";
    args["path"] = saved["path"];
    before = control->creates();
    REQUIRE(f.call("rack.preset.load", args));
    CHECK(control->creates() == before + 1);
}

TEST_CASE("rack cables: numeric ids reject leading zeros with a canonical hint") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","type":"transport"})");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
    for (auto key : {"00", "012"}) {
        auto args = json::Value::object();
        args["from"] = "mo_2.playing";
        args["to"] = std::string("fx_1.") + key;
        const auto bad = f.call("cable.add", args);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == "rack.port");
        CHECK(bad.error().hint == std::string("use the canonical parameter id ") +
                                      (key == std::string("00") ? "0" : "12"));
    }
    f.run("cable.add", R"({"from":"mo_2.playing","to":"fx_1.0"})");
}

TEST_CASE("rack.get: brief uses prepared timing and full alone probes a cold plugin") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("cable.add", R"({"from":"mo_2.out_l","to":"mo_1.l"})");
    auto tx = f.app->beginTransaction(origin, "Latency");
    REQUIRE(tx);
    f.app->txn().modify<model::FxInstance>({model::Kind::Fx, 1}, [](auto& fx) {
        fx.patch =
            *json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})");
    });
    REQUIRE(f.app->commitTransaction(*tx));
    const auto* fx = f.app->head().fx.find({model::Kind::Fx, 1});
    auto instance = f.app->instances().get({model::Kind::Fx, 1}, *fx, f.app->head());
    REQUIRE(instance);
    struct Control {
        void (*edit)(const clap_plugin_t*, double, std::uint32_t);
        std::uint32_t (*deliveries)(const clap_plugin_t*);
        void (*latency)(const clap_plugin_t*, bool);
        std::uint32_t (*creates)();
    };
    const auto* raw = (*instance)->raw();
    const auto* control =
        static_cast<const Control*>(raw->get_extension(raw, "supple.test.gain-control"));
    REQUIRE(control);
    const auto before = control->creates();
    for (int i = 0; i < 3; ++i)
        CHECK(f.run("rack.get", R"({"rack":"rk_1"})")["latency_samples"].isNull());
    CHECK(control->creates() == before);
    CHECK(f.run("rack.get", R"({"rack":"rk_1","detail":"full"})")["latency_samples"].asInt() == 64);
    CHECK(control->creates() > before);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(f.app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128},
                               {.anticipative = false, .workers = 0}));
    const auto prepared = control->creates();
    CHECK(f.run("rack.get", R"({"rack":"rk_1"})")["latency_samples"].asInt() == 64);
    CHECK(f.run("rack.get", R"({"rack":"rk_1","detail":"full"})")["latency_samples"].asInt() == 64);
    CHECK(control->creates() == prepared);
    auto txEdit = f.app->beginTransaction(origin, "Preview output");
    REQUIRE(txEdit);
    model::RackModule output;
    output.rack = {model::Kind::Rack, 1};
    output.type = std::string(model::kOutModule);
    f.app->txn().create(output);
    REQUIRE(f.call("rack.get", *json::parse(R"({"rack":"rk_1"})")));
    REQUIRE(f.app->rollbackTransaction(*txEdit));
    f.run("module.add", R"({"rack":"rk_1","type":"notes_out"})");
    f.run("cable.add", R"({"from":"mo_2.out_l","to":"mo_3.gate"})");
    f.run("route.add", R"({"from":{"module":"mo_3"},"to":{"track":"tr_1"},"midi":{}})");
    auto txMidi = f.app->beginTransaction(origin, "Only Note Out routed");
    REQUIRE(txMidi);
    for (const auto& route : f.app->head().routes)
        if (route.value->from.module == model::Id{model::Kind::Module, 1})
            f.app->txn().remove<model::Route>(route.id);
    f.app->txn().modify<model::Track>({model::Kind::Track, 1},
                                      [](auto& tr) { tr.record.input.reset(); });
    REQUIRE(f.app->commitTransaction(*txMidi));
    auto midiTiming = f.run("rack.get", R"({"rack":"rk_1"})");
    CHECK(midiTiming["latency_samples"].asInt() == 64);
    CHECK(midiTiming["note_outs"].asArray()[0]["latency_samples"].asInt() == 64);
    f.app->stopEngine();
}

TEST_CASE("rack.get: a deferred structural edit cannot use the older prepared plan") {
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        Fixture f;
        f.run("transport.playrate.set", R"({"rate":0.8})");
        f.run("rack.add");
        f.run("module.add", R"({"rack":"rk_1","plugin":"com.suppleaudio.test.gain"})");
        f.run("cable.add", R"({"from":"mo_2.out_l","to":"mo_1.l"})");
        auto tx = f.app->beginTransaction(origin, "Latency");
        REQUIRE(tx);
        f.app->txn().modify<model::FxInstance>({model::Kind::Fx, 1}, [](auto& fx) {
            fx.patch = *json::parse(
                R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})");
        });
        REQUIRE(f.app->commitTransaction(*tx));
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        REQUIRE(f.app->startEngine(backend, {.sampleRate = 8000, .blockSize = 128},
                                   {.anticipative = anticipative, .workers = 0}));
        auto& engine = *f.app->engine();
        engine.play();
        for (int i = 0; i < 32; ++i) {
            if (anticipative) REQUIRE(engine.awaitRendered(1024));
            backend.run(1);
            engine.service();
        }
        CHECK(f.run("rack.get", R"({"rack":"rk_1"})")["latency_samples"].asInt() == 64);
        // Prepare a future non-unity plan, then edit inside its deferred span.
        f.run("env.add", R"({"track":"tr_0","param":"playrate",
                             "points":[{"seconds":0,"value":1.25}]})");
        const auto generation = engine.planGeneration();
        tx = f.app->beginTransaction(origin, "Deferred output and bypass");
        REQUIRE(tx);
        model::RackModule output;
        output.rack = {model::Kind::Rack, 1};
        output.type = std::string(model::kOutModule);
        f.app->txn().create(output);
        f.app->txn().modify<model::RackModule>({model::Kind::Module, 2},
                                               [](auto& module) { module.bypass = true; });
        REQUIRE(f.app->commitTransaction(*tx));
        REQUIRE(engine.planGeneration() == generation);
        CHECK_FALSE(engine.rackLatencies({model::Kind::Rack, 1}, f.app->head().version));
        auto rack = f.run("rack.get", R"({"rack":"rk_1"})");
        CHECK(rack["latency_samples"].asInt() == 0);
        REQUIRE(rack["outs"].asArray().size() == 2);
        for (auto& entry : rack["outs"].asArray()) CHECK(entry["latency_samples"].asInt() == 0);
        f.app->stopEngine();
    }
}

TEST_CASE(
    "rack.capture: sync does not scan unused plugins and snapshot lookup keeps name fallback") {
    host::setBridgeExecutable(std::filesystem::temp_directory_path() / "no-such-plugin-scanner");
    struct ResetScanner {
        ~ResetScanner() { host::setBridgeExecutable(BRIDGE_PATH); }
    } reset;
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
    CHECK(f.app->pluginScanReport().warnings.empty());
    f.run("rack.capture", R"({"rack":"rk_1","track":"tr_1","pitch":"mo_2.pitch",
        "gate":"mo_2.gate","beats":1,"notes":"voice v; 1: C4:q r:*"})");
    CHECK(f.app->pluginScanReport().warnings.empty());
    REQUIRE(f.app->plugins());
    CHECK_FALSE(f.app->pluginScanReport().warnings.empty());
    host::PluginInfo plugin;
    plugin.id = "com.example.effect";
    plugin.name = "Some Effect";
    const std::vector<host::PluginInfo> catalogue{plugin};
    REQUIRE(EngineApp::findPlugin(catalogue, "COM.EXAMPLE.EFFECT"));
    REQUIRE(EngineApp::findPlugin(catalogue, "some effect"));
}

TEST_CASE("rack inputs: selecting MIDI preserves rack audio in either order and across reopen") {
    for (bool midiFirst : {false, true}) {
        CAPTURE(midiFirst);
        Fixture f;
        f.run("project.set", R"({"console9":false})");
        f.run("track.add");
        if (midiFirst) f.run("track.set", R"({"track":"tr_1","record_midi":"*"})");
        f.run("rack.add", R"({"track":"tr_1"})");
        f.run("module.add", R"({"rack":"rk_1","type":"com.suppleaudio.rack.vco"})");
        f.run("cable.add", R"({"from":"mo_2.sine","to":"mo_1.l"})");
        const auto input = f.app->head().tracks.find({model::Kind::Track, 1})->record.input;
        auto render = [](Fixture& app) {
            auto result =
                app.app->renderSession(app.app->head(), {.endSeconds = .05, .blockSize = 37});
            REQUIRE(result);
            return result->audio.channels;
        };
        const auto before = render(f);
        double peak = 0;
        for (float sample : before[0]) peak = std::max(peak, std::abs(double(sample)));
        REQUIRE(peak > .01);
        f.run("track.set", R"({"track":"tr_1","record_midi":"*"})");
        const auto& track = *f.app->head().tracks.find({model::Kind::Track, 1});
        CHECK(track.record.input == input);
        REQUIRE(track.record.midiInput);
        CHECK(track.record.midiInput != input);
        CHECK(render(f) == before);
        const auto path = f.dir / "rack-midi.json";
        REQUIRE(f.app->save(path));
        Fixture reopened;
        std::vector<schema::Warning> warnings;
        REQUIRE(reopened.app->open(path, &warnings));
        CHECK(warnings.empty());
        CHECK(render(reopened) == before);
        REQUIRE(model::midiInputRoute(reopened.app->head(), {model::Kind::Track, 1}));
        f.run("track.set", R"({"track":"tr_1","record_midi":null})");
        CHECK_FALSE(model::midiInputRoute(f.app->head(), {model::Kind::Track, 1}));
        CHECK(f.app->head().tracks.find({model::Kind::Track, 1})->record.input == input);
        CHECK(render(f) == before);
    }
}

TEST_CASE("rack inputs: home Notes hears monitored device MIDI in both executors") {
    for (bool recording : {false, true})
        for (bool anticipative : {false, true})
            for (int workers : {0, 2}) {
                INFO("recording " << recording << ", anticipative " << anticipative << ", workers "
                                  << workers);
                Fixture f;
                f.run("project.set", R"({"console9":false})");
                f.run("rack.add");
                CHECK(f.app->head().tracks.find({model::Kind::Track, 1})->record.monitor ==
                      f.app->defaultMonitor());
                f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
                f.run("cable.add", R"({"from":"mo_2.gate","to":"mo_1.l"})");
                f.run("track.set", R"({"track":"tr_1","record_midi":"key","midi_channel":1,
                                   "armed":true,"monitor":"on"})");
                // Offline MIDI at the same frames is an exact reference for the
                // injected keyboard, including callback fragments of the rack grid.
                model::EditTxn t(f.app->head(), f.app->head().version + 1);
                model::MidiSource midi;
                midi.sequence.ppq = 24000;
                midi.sequence.length = 4800;
                midi.sequence.notes = {{128, 896, 0, 72, 100, 0}};
                model::Take take;
                take.source = t.create(midi);
                const auto tk = t.create(take);
                model::Item item;
                item.track = {model::Kind::Track, 1};
                item.position = 0;
                item.length = .2;
                item.timebase = model::ItemTimebase::Beats;
                item.takes = {tk};
                item.activeTake = tk;
                t.create(item);
                auto state = t.commit();
                REQUIRE(state);
                auto expected = f.app->renderSession(*state, {.endSeconds = .05, .blockSize = 37});
                REQUIRE(expected);
                REQUIRE(std::any_of(expected->audio.channels[0].begin(),
                                    expected->audio.channels[0].end(),
                                    [](float value) { return std::abs(value) > .01f; }));
                DummyBackend backend(
                    {.mode = DummyBackend::Mode::Manual,
                     .captureFrames = 8192,
                     .midiPorts = {"Keyboard", "Pads"},
                     .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n,
                                std::int64_t frame) {
                         for (const auto& e :
                              std::array<std::array<int, 5>, 5>{{{64, 1, 0x90, 67, 100},
                                                                 {90, 0, 0x91, 69, 100},
                                                                 {128, 0, 0x90, 72, 100},
                                                                 {1024, 0, 0x80, 72, 0},
                                                                 {2600, 0, 0x90, 60, 100}}})
                             if (e[0] >= frame && e[0] < frame + n) {
                                 MidiInputEvent event;
                                 event.frame = static_cast<std::uint32_t>(e[0] - frame);
                                 event.port = static_cast<std::uint16_t>(e[1]);
                                 event.size = 3;
                                 for (int k = 0; k < 3; ++k)
                                     event.bytes[k] = static_cast<std::uint8_t>(e[k + 2]);
                                 out.push_back(event);
                             }
                     }});
                REQUIRE(f.app->startEngine(
                    backend, {.sampleRate = 48000, .blockSize = 37, .midiInputs = true},
                    {.anticipative = anticipative,
                     .renderBlock = 128,
                     .renderAheadMs = 10,
                     .workers = workers}));
                REQUIRE(f.app->engine()->awaitPlan());
                if (recording) f.run("transport.record");
                for (int i = 0; i < 90; ++i) {
                    if (anticipative) REQUIRE(f.app->engine()->awaitRendered(37));
                    backend.run(1);
                }
                const auto offset = f.app->engine()->outputLatency();
                REQUIRE(backend.capture().frames() >= offset + expected->audio.frames());
                for (std::size_t channel = 0; channel < 2; ++channel)
                    CHECK(std::equal(expected->audio.channels[channel].begin(),
                                     expected->audio.channels[channel].end(),
                                     backend.capture().channels[channel].begin() + offset));
                CHECK(f.app->engine()->processErrors() == 0);
                REQUIRE(std::abs(backend.capture().channels[0][90 * 37 - 1]) > .01f);
                f.run("track.set", R"({"track":"tr_1","monitor":"off"})");
                for (int i = 0; i < 64; ++i) {
                    if (anticipative) REQUIRE(f.app->engine()->awaitRendered(37));
                    backend.run(1);
                }
                CHECK(backend.capture().channels[0][154 * 37 - 1] == 0);
                f.run("transport.stop");
                if (recording) {
                    REQUIRE(f.app->head().items.size() == 1);
                    REQUIRE(f.app->head().midi.size() == 1);
                    CHECK(f.app->head().midi.begin()->value->sequence.notes.size() == 2);
                } else
                    CHECK(f.app->head().items.empty());
                f.app->stopEngine();
            }
}

TEST_CASE("module.set: null clears Notes device and source while omission retains them") {
    Fixture f;
    f.run("rack.add");
    f.run("module.add", R"({"rack":"rk_1","type":"notes","device":"","source":"tr_0"})");
    f.run("module.set", R"({"module":"mo_2","label":"Keys"})");
    const auto* notes = f.app->head().modules.find({model::Kind::Module, 2});
    REQUIRE(notes->device);
    CHECK(notes->device->empty());
    CHECK(notes->source == model::kMaster);
    f.run("module.set", R"({"module":"mo_2","device":null,"source":null})");
    notes = f.app->head().modules.find({model::Kind::Module, 2});
    CHECK_FALSE(notes->device);
    CHECK_FALSE(notes->source);
    f.run("history.undo");
    REQUIRE(f.app->head().modules.find({model::Kind::Module, 2})->device);
    f.run("history.redo");
    CHECK_FALSE(f.app->head().modules.find({model::Kind::Module, 2})->device);
    auto wrong = f.call("module.set", *json::parse(R"({"module":"mo_1","device":null})"));
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code == "rack.param");
}

TEST_CASE("rack inputs: hardware audio and MIDI replace each other on a channel") {
    Fixture f;
    f.run("track.add");
    const model::Id tr1{model::Kind::Track, 1};
    auto record = [&] { return f.app->head().tracks.find(tr1)->record; };
    f.run("track.set", R"({"track":"tr_1","record_midi":"*"})");
    const auto midi = record().midiInput;
    REQUIRE(midi);
    CHECK(record().input == midi);  // the MIDI-only alias older views read
    auto legacy = model::projectToJson(f.app->head());
    legacy["schema"] = "supple.project/2.7";
    legacy["tracks"]["tr_1"]["record"].asObject().erase("midi_input");
    auto parsed = model::projectFromJson(legacy);
    REQUIRE(parsed);
    REQUIRE(model::midiInputRoute(*parsed, tr1));
    // Hardware audio takes the MIDI route over, as before independent inputs.
    f.run("track.set", R"({"track":"tr_1","record_input":[1,2]})");
    CHECK(record().input == midi);
    CHECK_FALSE(record().midiInput);
    CHECK_FALSE(model::midiInputRoute(f.app->head(), tr1));
    CHECK(f.app->head().routes.find(*midi)->from.hwChannels);
    f.run("track.set", R"({"track":"tr_1","record_midi":"Keys"})");
    CHECK(record().input == midi);
    CHECK(record().midiInput == midi);
    CHECK_FALSE(f.app->head().routes.find(*midi)->from.hwChannels);
    CHECK(f.app->head().routes.size() == 1);
    auto both = f.call("track.set", *json::parse(R"({"track":"tr_1","record_midi":"Keys",
        "record_input":[3,4]})"));
    REQUIRE_FALSE(both);
    CHECK(both.error().code == "track.record_input");
    // A MIDI-only channel copies as one aliased route.
    f.run("track.duplicate", R"({"tracks":["tr_1"]})");
    const auto* copy = f.app->head().tracks.find({model::Kind::Track, 2});
    REQUIRE(copy);
    REQUIRE(copy->record.midiInput);
    CHECK(copy->record.input == copy->record.midiInput);
    CHECK(copy->record.midiInput != midi);
    CHECK(*model::midiInputRoute(f.app->head(), {model::Kind::Track, 2})->from.midiDevice ==
          "Keys");
    // Validation: midi_input is a MIDI route, and a MIDI record.input is that route.
    auto invalid = model::projectToJson(f.app->head());
    invalid["tracks"]["tr_1"]["record"]["midi_input"] = copy->record.midiInput->str();
    CHECK_FALSE(model::projectFromJson(invalid));
    f.run("rack.add", R"({"track":"tr_1"})");
    REQUIRE(record().input != midi);
    CHECK(record().midiInput == midi);
    auto aliased = model::projectToJson(f.app->head());
    const auto nextRoute = f.app->head().next[model::Kind::Route];
    const std::string second = model::Id{model::Kind::Route, nextRoute}.str();
    aliased["routes"][second] = aliased["routes"][midi->str()];
    aliased["project"]["next_ids"]["rt"] = static_cast<std::int64_t>(nextRoute + 1);
    aliased["tracks"]["tr_1"]["record"]["input"] = second;
    auto refused = model::projectFromJson(aliased);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().path == "/tracks/tr_1/record/input");
}

TEST_CASE("rack inputs: a project saved with MIDI in place of the rack's Out is repaired") {
    for (bool ambiguous : {false, true}) {
        CAPTURE(ambiguous);
        Fixture f;
        f.run("project.set", R"({"console9":false})");
        f.run("track.add");
        f.run("rack.add", R"({"track":"tr_1"})");
        f.run("module.add", R"({"rack":"rk_1","type":"com.suppleaudio.rack.vco"})");
        f.run("cable.add", R"({"from":"mo_2.sine","to":"mo_1.l"})");
        if (ambiguous) f.run("module.add", R"({"rack":"rk_1","type":"out"})");
        const model::Id tr1{model::Kind::Track, 1};
        const auto out = *f.app->head().tracks.find(tr1)->record.input;
        const auto audible = f.app->renderSession(f.app->head(), {.endSeconds = .05});
        REQUIRE(audible);
        // What record_midi did before independent inputs: the Out's route
        // became the MIDI one, and the patch fell silent.
        auto doc = model::projectToJson(f.app->head());
        doc["schema"] = "supple.project/2.7";
        json::Value from = json::Value::object();
        from["midi_device"] = "*";
        doc["routes"][out.str()]["from"] = std::move(from);
        doc["routes"][out.str()]["midi"] = *json::parse(R"({"from_channel":0,"to_channel":0})");
        std::vector<schema::Warning> warnings;
        auto loaded = model::projectFromJson(doc, &warnings);
        REQUIRE(loaded);
        const auto& record = loaded->tracks.find(tr1)->record;
        const auto repaired = std::count_if(warnings.begin(), warnings.end(), [](const auto& w) {
            return w.code == "project.rack_input_repaired";
        });
        if (ambiguous) {
            CHECK(repaired == 0);
            CHECK(record.input == out);
            CHECK_FALSE(record.midiInput);
            continue;
        }
        CHECK(repaired == 1);
        CHECK(record.midiInput == out);
        REQUIRE(record.input);
        REQUIRE(record.input != out);
        CHECK(loaded->routes.find(*record.input)->from.module == model::Id{model::Kind::Module, 1});
        auto rendered = f.app->renderSession(*loaded, {.endSeconds = .05});
        REQUIRE(rendered);
        CHECK(rendered->audio.channels == audible->audio.channels);
        // Saved again, it reopens as is, with no repair.
        warnings.clear();
        auto again = model::parseProject(model::serializeProject(*loaded), &warnings);
        REQUIRE(again);
        CHECK(warnings.empty());
        CHECK(model::serializeProject(*again) == model::serializeProject(*loaded));
    }
}

TEST_CASE("rack inputs: a channel records the input it was switched to") {
    for (bool toAudio : {true, false}) {
        CAPTURE(toAudio);
        Fixture f;
        f.run("track.add");
        f.run("track.set", toAudio ? R"({"track":"tr_1","record_midi":"*"})"
                                   : R"({"track":"tr_1","record_input":[1,2]})");
        f.run("track.set", toAudio ? R"({"track":"tr_1","record_input":[1,2],"armed":true})"
                                   : R"({"track":"tr_1","record_midi":"*","armed":true})");
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual,
             .input =
                 [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
                     for (std::uint32_t c = 0; c < channels; ++c)
                         for (std::uint32_t i = 0; i < n; ++i) in[c][i] = .25f;
                 },
             .midiPorts = {"Keyboard"},
             .midi =
                 [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
                     for (const std::array<int, 3> e :
                          {std::array{512, 0x90, 64}, std::array{4096, 0x80, 64}})
                         if (e[0] >= frame && e[0] < frame + n) {
                             MidiInputEvent event;
                             event.frame = static_cast<std::uint32_t>(e[0] - frame);
                             event.size = 3;
                             event.bytes[0] = static_cast<std::uint8_t>(e[1]);
                             event.bytes[1] = static_cast<std::uint8_t>(e[2]);
                             event.bytes[2] = 100;
                             out.push_back(event);
                         }
                 }});
        REQUIRE(f.app->startEngine(
            backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .midiInputs = true},
            {.anticipative = false}));
        REQUIRE(f.app->engine()->awaitPlan());
        f.run("transport.record");
        backend.run(80);
        auto stopped = f.run("transport.stop");
        f.app->stopEngine();
        REQUIRE(stopped["recorded"].size() == 1);
        CHECK(f.app->head().media.size() == (toAudio ? 1u : 0u));
        CHECK(f.app->head().midi.size() == (toAudio ? 0u : 1u));
    }
}

TEST_CASE("rack inputs: arming during playback keeps timeline notes and ends device notes" *
          doctest::description("rt")) {
    for (bool anticipative : {false, true})
        for (int workers : {0, 2}) {
            INFO("anticipative " << anticipative << ", workers " << workers);
            Fixture f;
            f.run("project.set", R"({"console9":false})");
            f.run("rack.add");
            f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
            f.run("cable.add", R"({"from":"mo_2.gate","to":"mo_1.l"})");
            f.run("track.set", R"({"track":"tr_1","record_midi":"*","monitor":"on"})");
            auto tx = f.app->beginTransaction(origin, "Held timeline note");
            REQUIRE(tx);
            model::MidiSource midi;
            midi.sequence.ppq = 960;
            midi.sequence.length = 3840;
            midi.sequence.notes = {{0, 3840, 0, 60, 96, 0}};
            model::Take take;
            take.source = f.app->txn().create(midi);
            const auto tk = f.app->txn().create(take);
            model::Item item;
            item.track = {model::Kind::Track, 1};
            item.length = 4;
            item.timebase = model::ItemTimebase::Beats;
            item.takes = {tk};
            item.activeTake = tk;
            f.app->txn().create(item);
            REQUIRE(f.app->commitTransaction(*tx));
            std::atomic<bool> press{false};
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual,
                 .captureFrames = 16384,
                 .midiPorts = {"Keyboard"},
                 .midi = [&press](std::vector<MidiInputEvent>& out, std::uint32_t, std::int64_t) {
                     if (!press.exchange(false)) return;
                     MidiInputEvent event;
                     event.size = 3;
                     event.bytes[0] = 0x90;
                     event.bytes[1] = 72;
                     event.bytes[2] = 100;
                     out.push_back(event);  // held: no note-off follows
                 }});
            REQUIRE(f.app->startEngine(backend,
                                       {.sampleRate = 48000, .blockSize = 37, .midiInputs = true},
                                       {.anticipative = anticipative,
                                        .renderBlock = 128,
                                        .renderAheadMs = 10,
                                        .workers = workers}));
            REQUIRE(f.app->engine()->awaitPlan());
            f.run("transport.play");
            int cycles = 0;
            auto run = [&](int count) {
                for (int i = 0; i < count; ++i) {
                    if (anticipative) REQUIRE(f.app->engine()->awaitRendered(37));
                    backend.run(1);
                }
                cycles += count;
                return backend.capture().channels[0][static_cast<std::size_t>(cycles * 37 - 1)];
            };
            const float timeline = run(64);
            REQUIRE(std::abs(timeline) > .01f);
            // Arming starts monitoring: the timeline note keeps playing.
            f.run("track.set", R"({"track":"tr_1","armed":true})");
            CHECK(run(64) == timeline);
            press = true;
            CHECK(double(run(64)) == doctest::Approx(2.0 * double(timeline)));
            // Disarming ends the held device note only.
            f.run("track.set", R"({"track":"tr_1","armed":false})");
            CHECK(run(64) == timeline);
            CHECK(f.app->engine()->processErrors() == 0);
            f.app->stopEngine();
        }
}

// Another armed channel keeps the live region open, so arming the rack's
// channel moves the rack into it beside that one. When the callback takes
// the plan late (the renderer stalled before the note's end and the callback
// played past it), the rack cannot continue from the renderer: its Notes
// would keep the key whose note-off it skipped. It starts afresh.
TEST_CASE("rack inputs: a late handoff into a live region restarts the rack: no note hangs" *
          doctest::description("rt")) {
    for (const std::uint32_t block : {256u, 37u})
        for (int workers : {0, 2}) {
            INFO("block " << block << ", workers " << workers);
            Fixture f;
            f.run("project.set", R"({"console9":false})");
            f.run("rack.add");
            f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
            f.run("cable.add", R"({"from":"mo_2.gate","to":"mo_1.l"})");
            f.run("track.set", R"({"track":"tr_1","record_midi":"*","monitor":"on"})");
            f.run("track.add");
            f.run("track.set", R"({"track":"tr_2","record_midi":"*","armed":true,"monitor":"on"})");
            auto tx = f.app->beginTransaction(origin, "Held timeline note");
            REQUIRE(tx);
            model::MidiSource midi;
            midi.sequence.ppq = 960;
            midi.sequence.length = 3840;
            midi.sequence.notes = {{0, 1920, 0, 60, 96, 0}};  // ends at 1 s, frame 48000
            model::Take take;
            take.source = f.app->txn().create(midi);
            const auto tk = f.app->txn().create(take);
            model::Item item;
            item.track = {model::Kind::Track, 1};
            item.length = 4;
            item.timebase = model::ItemTimebase::Beats;
            item.takes = {tk};
            item.activeTake = tk;
            f.app->txn().create(item);
            REQUIRE(f.app->commitTransaction(*tx));
            DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                                  .captureFrames = 96000 + 4096,
                                  .midiPorts = {"Keyboard"}});
            REQUIRE(f.app->startEngine(
                backend, {.sampleRate = 48000, .blockSize = block, .midiInputs = true},
                {.anticipative = true,
                 .renderBlock = 128,
                 .renderAheadMs = 10,
                 .workers = workers}));
            auto& engine = *f.app->engine();
            REQUIRE(engine.awaitPlan());
            f.run("transport.play");
            std::int64_t played = 0;
            auto playTo = [&](std::int64_t to, bool ahead) {
                for (; played < to; played += block) {
                    if (ahead) REQUIRE(engine.awaitRendered(block));
                    backend.run(1);
                }
            };
            playTo(24000, true);
            engine.holdRenderer(true);
            REQUIRE(engine.awaitRendererHeld());
            playTo(60000, false);  // past the note's end, the renderer stalled
            f.run("track.set", R"({"track":"tr_1","armed":true})");
            REQUIRE(engine.awaitPlan());  // the renderer adopts it where it stalled
            playTo(68000, false);         // the callback, past that handoff, takes it at once
            engine.holdRenderer(false);
            playTo(96000, true);
            CHECK(engine.processErrors() == 0);
            f.app->stopEngine();
            const auto& L = backend.capture().channels[0];
            CHECK(std::abs(L[12000]) > .01f);
            float after = 0;
            for (std::size_t i = 72000; i < 96000; ++i) after = std::max(after, std::abs(L[i]));
            CHECK(after == 0.0f);
        }
}

TEST_CASE("rack inputs: an event both the Notes device and the channel input match arrives once" *
          doctest::description("rt")) {
    std::vector<float> levels;
    for (bool overlap : {false, true}) {
        CAPTURE(overlap);
        Fixture f;
        f.run("project.set", R"({"console9":false})");
        f.run("rack.add");
        f.run("module.add", R"({"rack":"rk_1","type":"notes","device":"Keyboard"})");
        f.run("cable.add", R"({"from":"mo_2.gate","to":"mo_1.l"})");
        if (overlap)
            f.run("track.set", R"({"track":"tr_1","record_midi":"*","armed":true,"monitor":"on"})");
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual,
             .captureFrames = 8192,
             .midiPorts = {"Keyboard"},
             .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
                 if (frame <= 370 && 370 < frame + n) {
                     MidiInputEvent event;
                     event.frame = static_cast<std::uint32_t>(370 - frame);
                     event.size = 3;
                     event.bytes[0] = 0x90;
                     event.bytes[1] = 64;
                     event.bytes[2] = 100;
                     out.push_back(event);
                 }
             }});
        REQUIRE(f.app->startEngine(backend,
                                   {.sampleRate = 48000, .blockSize = 37, .midiInputs = true},
                                   {.anticipative = false, .workers = 0}));
        REQUIRE(f.app->engine()->awaitPlan());
        backend.run(64);
        levels.push_back(backend.capture().channels[0][64 * 37 - 1]);
        CHECK(f.app->engine()->processErrors() == 0);
        f.app->stopEngine();
    }
    REQUIRE(std::abs(levels[0]) > .01f);
    CHECK(levels[1] == levels[0]);
}

TEST_CASE("rack inputs: selecting inactive MIDI preserves a held timeline note") {
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        Fixture f;
        f.run("project.set", R"({"console9":false})");
        f.run("rack.add");
        f.run("module.add", R"({"rack":"rk_1","type":"notes"})");
        f.run("cable.add", R"({"from":"mo_2.gate","to":"mo_1.l"})");
        auto tx = f.app->beginTransaction(origin, "Held timeline note");
        REQUIRE(tx);
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        midi.sequence.length = 3840;
        midi.sequence.notes = {{0, 3840, 0, 60, 96, 0}};
        model::Take take;
        take.source = f.app->txn().create(midi);
        const auto tk = f.app->txn().create(take);
        model::Item item;
        item.track = {model::Kind::Track, 1};
        item.length = 4;
        item.timebase = model::ItemTimebase::Beats;
        item.takes = {tk};
        item.activeTake = tk;
        f.app->txn().create(item);
        REQUIRE(f.app->commitTransaction(*tx));
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
        REQUIRE(f.app->startEngine(
            backend, {.sampleRate = 48000, .blockSize = 37},
            {.anticipative = anticipative, .renderBlock = 128, .renderAheadMs = 10, .workers = 0}));
        REQUIRE(f.app->engine()->awaitPlan());
        f.run("transport.play");
        auto run = [&] {
            for (int i = 0; i < 64; ++i) {
                if (anticipative) REQUIRE(f.app->engine()->awaitRendered(37));
                backend.run(1);
            }
        };
        run();
        REQUIRE(std::abs(backend.capture().channels[0][64 * 37 - 1]) > .01f);
        f.run("track.set", R"({"track":"tr_1","record_midi":"*","monitor":"on"})");
        run();
        CHECK(backend.capture().channels[0][128 * 37 - 1] ==
              backend.capture().channels[0][64 * 37 - 1]);
        f.app->stopEngine();
    }
}
