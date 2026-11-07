// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The module library (ADR-0062, rack.md §7): modules written by commands or
// found in the folder join the catalog; an edit rebuilds the modules playing
// it, in the running engine and in renders.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <functional>

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
