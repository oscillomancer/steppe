// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include "../../../tests/file_platform.h"
#include "hardware_midi.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/engine/offline.h"
#include "supple/model/schemas.h"
using namespace supple;
using namespace supple::engine;
namespace {
InstancePool hardwarePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path path = FIXTURE_CLAP_PATH;
        auto all = host::scan(std::span<const std::filesystem::path>(&path, 1));
        if (!all) return all.error();
        for (const auto& p : *all)
            if (p.id == ref.id) return p;
        return Error("test.plugin", "fixture missing");
    });
}
struct Session {
    model::Session value;
    model::Id track, route;
    explicit Session(model::Tap tap = model::Tap::PostFader, bool mute = false,
                     bool latency = true) {
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        model::Track t;
        t.parent = model::kMaster;
        t.name = "Keys";
        t.mute = mute;
        track = edit.create(t);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(track); });
        for (const auto* plugin :
             {"com.suppleaudio.test.sine", latency ? "com.suppleaudio.test.gain" : nullptr}) {
            if (!plugin) continue;
            model::FxInstance fx;
            fx.plugin.id = plugin;
            if (std::string_view(plugin) == "com.suppleaudio.test.gain")
                fx.patch =
                    json::parse(
                        R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                        .value();
            const auto id = edit.create(fx);
            edit.modify<model::Track>(track, [&](auto& tr) { tr.chain.push_back(id); });
        }
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        midi.sequence.length = 1920;
        midi.sequence.notes.push_back({0, 960, 2, 60, 100, 0});
        model::Take take;
        take.source = edit.create(midi);
        model::Item item;
        item.track = track;
        item.length = 2;
        item.takes = {edit.create(take)};
        item.activeTake = item.takes[0];
        edit.create(item);
        model::Route send;
        send.from.track = track;
        send.to.midiDevice = "sYnTh";
        send.midi = model::MidiRouting{3, 5};
        send.tap = tap;
        route = edit.create(send);
        auto result = edit.commit();
        REQUIRE(result);
        value = *result;
    }
};
void run(Engine& engine, DummyBackend& backend, bool anticipative, unsigned cycles) {
    for (unsigned i = 0; i < cycles; ++i) {
        if (anticipative) REQUIRE(engine.awaitRendered(4096));
        backend.run(1);
    }
}
std::vector<std::int64_t> events(const DummyBackend& backend, std::uint8_t status) {
    std::vector<std::int64_t> frames;
    for (const auto& e : backend.midiCapture())
        if (e.event.bytes[0] == status) frames.push_back(e.frame);
    return frames;
}
MidiPortPreferences outputSettings(double offset = 0, bool enabled = true, bool reset = true) {
    auto j = json::Value::object();
    j["offset_ms"] = offset;
    j["enabled"] = enabled;
    j["reset_on_stop"] = reset;
    return {{"Synth", j}};
}
}  // namespace
TEST_CASE(
    "hardware MIDI: timing, negative lead, compensated taps and audio null in both executors") {
    for (bool anticipative : {false, true})
        for (auto tap : {model::Tap::PreFx, model::Tap::PreFader, model::Tap::PostFader})
            for (int latency : {48, -96})
                for (double offset : {-.5, 1.5}) {
                    CAPTURE(anticipative);
                    CAPTURE(tap);
                    CAPTURE(latency);
                    Session session(tap);
                    auto pool = hardwarePool();
                    MediaCache media;
                    auto referencePool = hardwarePool();
                    auto offline = renderOffline(session.value, referencePool,
                                                 {.startSeconds = 0, .endSeconds = .1});
                    REQUIRE(offline);
                    DummyBackend backend({.captureFrames = 8192,
                                          .midiOutputPorts = {"Synth"},
                                          .midiOutputLatency = {latency}});
                    Engine engine(pool, media,
                                  {.anticipative = anticipative,
                                   .renderBlock = 64,
                                   .renderAheadMs = 10,
                                   .workers = 0});
                    REQUIRE(engine.start(
                        backend, {.sampleRate = 48000, .blockSize = 64, .midiOutputs = true}));
                    engine.setMidiPreferences({}, outputSettings(offset));
                    REQUIRE(engine.load(session.value));
                    engine.play();
                    run(engine, backend, anticipative, 100);
                    const auto notes = events(backend, 0x94);
                    REQUIRE(notes.size() == 1);
                    // Source latency is compensated before the port's signed latency/offset.
                    CHECK(notes[0] == std::int64_t(engine.outputLatency()) + latency +
                                          time::samplesAt(offset / 1000., 48000));
                    const auto delay = engine.outputLatency();
                    for (std::size_t c = 0; c < 2; ++c)
                        for (std::size_t f = 0; f < 4800; ++f)
                            if (backend.capture().channels[c][f + delay] !=
                                offline->audio.channels[c][f]) {
                                CHECK(backend.capture().channels[c][f + delay] ==
                                      offline->audio.channels[c][f]);
                                break;
                            }
                    engine.stop();
                    CHECK_FALSE(events(backend, 0x84).empty());
                }
}
TEST_CASE("hardware MIDI: mute, route removal, disable, stop, locate and missing ports") {
    for (bool anticipative : {false, true})
        for (int action = 0; action < 6; ++action) {
            CAPTURE(anticipative);
            CAPTURE(action);
            Session session;
            auto pool = hardwarePool();
            MediaCache media;
            DummyBackend backend({.midiOutputPorts = {"Synth"}});
            Engine engine(pool, media,
                          {.anticipative = anticipative,
                           .renderBlock = 64,
                           .renderAheadMs = 10,
                           .workers = 0});
            REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
            REQUIRE(engine.load(session.value));
            engine.play();
            run(engine, backend, anticipative, 5);
            REQUIRE(events(backend, 0x94).size() == 1);
            if (action == 0) {
                model::EditTxn edit(session.value, 2);
                edit.remove<model::Route>(session.route);
                REQUIRE(engine.load(edit.commit().value()));
            }
            if (action == 1) {
                engine.setMidiPreferences({}, outputSettings(0, false));
                REQUIRE(engine.load(session.value));
                backend.run(1);  // disable gates the next callback, before the render horizon
                CHECK_FALSE(events(backend, 0x84).empty());
            }
            if (action == 2) engine.pause();
            if (action == 3) engine.seek(1.0);
            if (action == 4) {
                model::EditTxn edit(session.value, 2);
                edit.modify<model::Route>(session.route, [](auto& r) { r.mute = true; });
                REQUIRE(engine.load(edit.commit().value()));
            }
            if (action == 5) {
                model::EditTxn edit(session.value, 2);
                auto other = *session.value.tracks.find(session.track);
                other.chain.clear();
                other.name = "Other source";
                for (const auto fx : session.value.tracks.find(session.track)->chain)
                    other.chain.push_back(edit.create(*session.value.fx.find(fx)));
                const auto source = edit.create(other);
                edit.modify<model::Track>(model::kMaster,
                                          [&](auto& m) { m.children.push_back(source); });
                edit.modify<model::Route>(session.route, [&](auto& r) { r.from.track = source; });
                REQUIRE(engine.load(edit.commit().value()));
            }
            if (anticipative && (action == 0 || action == 1 || action == 4 || action == 5))
                REQUIRE(engine.awaitPlan());
            run(engine, backend, anticipative, 100);
            CHECK_FALSE(events(backend, 0x84).empty());
            engine.stop();
        }
    for (auto tap : {model::Tap::PostFader, model::Tap::PreFx}) {
        Session session(tap, true);
        auto pool = hardwarePool();
        MediaCache media;
        DummyBackend backend({.midiOutputPorts = {"Synth"}});
        Engine engine(pool, media, {.anticipative = false, .workers = 0});
        REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
        REQUIRE(engine.load(session.value));
        engine.play();
        backend.run(5);
        CHECK(events(backend, 0x94).empty() == (tap == model::Tap::PostFader));
        engine.stop();
    }
    Session session;
    auto pool = hardwarePool();
    MediaCache media;
    DummyBackend backend({});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {}));
    REQUIRE(engine.load(session.value));
    CHECK(std::any_of(engine.warnings().begin(), engine.warnings().end(),
                      [](const auto& w) { return w.find("is absent") != std::string::npos; }));
    engine.stop();
}
TEST_CASE("hardware MIDI: reset preference retains held notes until engine shutdown") {
    Session session;
    auto pool = hardwarePool();
    MediaCache media;
    DummyBackend backend({.midiOutputPorts = {"Synth"}});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
    engine.setMidiPreferences({}, outputSettings(0, true, false));
    REQUIRE(engine.load(session.value));
    engine.play();
    backend.run(5);
    engine.pause();
    backend.run(5);
    CHECK(events(backend, 0x84).empty());
    engine.stop();
    CHECK_FALSE(events(backend, 0x84).empty());
}
TEST_CASE("hardware MIDI: input offsets carry across cycles, clamp advances and respect disable") {
    auto runtime = std::make_unique<detail::HardwareMidiRuntime>();
    detail::Plan plan;
    plan.rate = 48000;
    DummyBackend backend({});
    REQUIRE(backend.start({.blockSize = 64}, [](auto, auto, auto) {}));
    const std::array<MidiInputEvent, 2> raw{
        {{10, 0, 3, {0x90, 60, 100}}, {20, 1, 3, {0x90, 61, 100}}}};
    plan.midiInputsSettings[0].offsetMs = 2;
    plan.midiInputsSettings[1].offsetMs = -1;
    runtime->begin(backend, 64);
    auto first = runtime->input(raw, &plan);
    REQUIRE(first.size() == 1);
    CHECK(first[0].frame == 0);
    CHECK(first[0].port == 1);
    runtime->finish(nullptr, 0, 64, false);
    runtime->begin(backend, 64);
    auto second = runtime->input({}, &plan);
    REQUIRE(second.size() == 1);
    CHECK(second[0].frame == 42);
    plan.midiInputsSettings[0].enabled = false;
    CHECK(runtime->input(raw, &plan).size() == 1);
    backend.stop();
}
TEST_CASE("hardware MIDI: machine preferences, commands and project save/reopen") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("midi-hardware-" + std::to_string(test::processId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    AppOptions options;
    options.workdir = dir;
    options.midiPreferencesFile = dir / "preferences.json";
    auto app = EngineApp::create(options);
    REQUIRE(app);
    REQUIRE((*app)->setMidiPreferences({{"Keys", json::Value::object()}},
                                       {{"Synth", json::Value::object()}}, false));
    {
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, **app);
        auto call = [&](const char* command, const std::string& args) {
            auto result = bus.execute(
                {command, json::parse(args).value(), {cmd::Origin::Type::Ui, "test", {}}});
            REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
            return *result;
        };
        call("midi.outputs.set",
             R"({"port":"sYnTh","enabled":false,"offset_ms":-2,"reset_on_stop":false})");
        call("midi.inputs.set", R"({"port":"Keys","offset_ms":3})");
        CHECK_FALSE((*app)->midiOutputsPreferences().contains("sYnTh"));
        auto unknown = bus.execute({"midi.outputs.set",
                                    json::parse(R"({"port":"typo","enabled":false})").value(),
                                    {cmd::Origin::Type::Ui, "test", {}}});
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().hint.find("Synth") != std::string::npos);
        auto ambiguous = (*app)->midiOutputsPreferences();
        ambiguous["synth"] = json::Value::object();
        REQUIRE((*app)->setMidiPreferences((*app)->midiInputsPreferences(), ambiguous, false));
        CHECK_FALSE((*app)->setMidiPort(true, "Synth", json::Value::object()));
        ambiguous.erase("synth");
        REQUIRE((*app)->setMidiPreferences((*app)->midiInputsPreferences(), ambiguous, false));
        const auto track = call("track.add", R"({"name":"Hardware source"})")["track"].asString();
        const auto send =
            call(
                "route.add",
                R"({"from":")" + track +
                    R"(","to_midi":"Synth","midi":{"from_channel":3,"to_channel":5},"tap":"pre_fx"})")
                ["id"]
                    .asString();
        const auto file = dir / "hardware.json";
        const auto pathArgs = json::writeCanonical([&] {
            auto args = json::Value::object();
            args["path"] = file.string();
            return args;
        }());
        call("project.save", pathArgs);
        call("project.new", "{}");
        call("project.open", pathArgs);
        const auto* route = (*app)->head().routes.find(*model::Id::parse(send));
        REQUIRE(route);
        CHECK(route->to.midiDevice == "Synth");
        CHECK(route->midi->toChannel == 5);
        auto records = (*app)->midiOutputsPreferences();
        records["Synth"]["clock"] = true;
        REQUIRE((*app)->setMidiPreferences((*app)->midiInputsPreferences(), records));
    }
    const auto saved = readFile(options.midiPreferencesFile);
    REQUIRE(saved);
    const auto prefs =
        json::parse(std::string_view(reinterpret_cast<const char*>(saved->data()), saved->size()));
    REQUIRE(prefs);
    CHECK(prefs->find("schema")->asString() == kPreferencesSchema);
    app->reset();
    app = EngineApp::create(options);
    REQUIRE(app);
    CHECK(midiPortSettings((*app)->midiOutputsPreferences(), "Synth").offsetMs == -2);
    CHECK((*app)->midiOutputsPreferences().at("Synth").find("clock")->asBool());
    CHECK_FALSE(
        parseMidiPortPreferences(json::parse(R"({"Synth":{"offset_ms":1001}})").value(), true));
    // A canonical session round trip retains the hardware endpoint and MIDI channel controls.
    Session session;
    auto encoded = model::projectToJson(session.value);
    auto decoded = model::projectFromJson(encoded);
    REQUIRE(decoded);
    CHECK(decoded->routes.find(session.route)->to.midiDevice ==
          session.value.routes.find(session.route)->to.midiDevice);
    encoded["routes"][session.route.str()]["from"] =
        json::parse(R"({"midi_device":"Keys"})").value();
    CHECK_FALSE(model::projectFromJson(encoded));
    app->reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("hardware MIDI: rack Note Out reaches a named output in both executors") {
    for (bool anticipative : {false, true}) {
        Session base(model::Tap::PreFx, false, false);
        model::EditTxn edit(base.value, 2);
        const auto rack = edit.create(model::Rack{base.track, "Hardware rack"});
        auto module = [&](std::string_view type) {
            model::RackModule m;
            m.rack = rack;
            m.type = std::string(type);
            m.version = "1.0.0";
            return edit.create(m);
        };
        const auto notes = module(model::kNotesModule), out = module(model::kNotesOutModule);
        edit.modify<model::RackModule>(out, [](auto& m) { m.params["channel"] = 3; });
        for (const auto* port : {"pitch", "gate", "velocity"})
            edit.create(model::Cable{{notes, port}, {out, port}, std::nullopt, ""});
        edit.modify<model::Route>(base.route, [&](auto& r) {
            r.from = {};
            r.from.module = out;
        });
        const auto s = edit.commit();
        REQUIRE(s);
        auto pool = hardwarePool();
        MediaCache media;
        DummyBackend backend({.midiOutputPorts = {"Synth"}});
        Engine engine(
            pool, media,
            {.anticipative = anticipative, .renderBlock = 64, .renderAheadMs = 10, .workers = 0});
        REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
        REQUIRE(engine.load(*s));
        engine.play();
        run(engine, backend, anticipative, 10);
        REQUIRE(events(backend, 0x94).size() == 1);
        CHECK(events(backend, 0x94)[0] == engine.outputLatency());
        engine.stop();
        CHECK_FALSE(events(backend, 0x84).empty());
    }
}

TEST_CASE("hardware MIDI: route and source mute automation follow compensated MIDI time") {
    for (bool anticipative : {false, true})
        for (bool sourceMute : {false, true}) {
            Session base;
            model::EditTxn edit(base.value, 2);
            // A slower sibling pads this source's input by 64 frames.
            model::Track slow;
            slow.parent = model::kMaster;
            const auto sibling = edit.create(slow);
            edit.modify<model::Track>(model::kMaster,
                                      [&](auto& master) { master.children.push_back(sibling); });
            for (const auto& fx : base.value.fx)
                if (fx.value->plugin.id == "com.suppleaudio.test.gain")
                    for (int i = 0; i < 2; ++i) {
                        const auto id = edit.create(*fx.value);
                        edit.modify<model::Track>(sibling, [&](auto& t) { t.chain.push_back(id); });
                    }
            model::Envelope lane;
            lane.target = {sourceMute ? base.track : base.route, "mute"};
            lane.timebase = model::Timebase::Time;
            lane.points = {{0, 0, model::PointShape::Square, 0},
                           {96. / 48000, 1, model::PointShape::Square, 0}};
            edit.create(lane);
            const auto s = edit.commit();
            REQUIRE(s);
            auto pool = hardwarePool();
            MediaCache media;
            DummyBackend backend({.midiOutputPorts = {"Synth"}});
            Engine engine(pool, media,
                          {.anticipative = anticipative,
                           .renderBlock = 64,
                           .renderAheadMs = 10,
                           .workers = 0});
            REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
            REQUIRE(engine.load(*s));
            engine.play();
            run(engine, backend, anticipative, 30);
            REQUIRE(events(backend, 0x94).size() == 1);
            CHECK(events(backend, 0x94)[0] == engine.outputLatency());
            const auto offs = events(backend, 0x84);
            REQUIRE(offs.size() == 1);
            CHECK(offs[0] == engine.outputLatency() + 96);
            engine.stop();
        }
}

TEST_CASE("hardware MIDI: existing channel input monitoring reaches output while stopped") {
    for (bool anticipative : {false, true}) {
        Session base(model::Tap::PreFx, false, false);
        model::EditTxn edit(base.value, 2);
        model::Route input;
        input.from.midiDevice = "Keyboard";
        input.to.track = base.track;
        input.midi = model::MidiRouting{0, 3};
        const auto id = edit.create(input);
        edit.modify<model::Track>(base.track, [&](auto& t) {
            t.record.armed = true;
            t.record.monitor = model::Monitor::On;
            t.record.input = id;
            t.record.midiInput = id;
        });
        const auto s = edit.commit();
        REQUIRE(s);
        auto pool = hardwarePool();
        MediaCache media;
        DummyBackend backend({.midiOutputPorts = {"Synth"},
                              .midiPorts = {"Keyboard"},
                              .midi = [](auto& events, unsigned, std::int64_t frame) {
                                  if (frame == 0) events.push_back({7, 0, 3, {0x90, 60, 100}});
                                  if (frame == 64) events.push_back({9, 0, 3, {0x80, 60, 0}});
                              }});
        Engine engine(
            pool, media,
            {.anticipative = anticipative, .renderBlock = 64, .renderAheadMs = 10, .workers = 0});
        REQUIRE(engine.start(backend, {.blockSize = 64, .midiInputs = true, .midiOutputs = true}));
        REQUIRE(engine.load(*s));
        REQUIRE(engine.awaitPlan());
        run(engine, backend, anticipative, 3);
        REQUIRE(events(backend, 0x94).size() == 1);
        CHECK(events(backend, 0x94)[0] == 7);
        REQUIRE(events(backend, 0x84).size() == 1);
        CHECK(events(backend, 0x84)[0] == 73);
        engine.stop();
    }
}
