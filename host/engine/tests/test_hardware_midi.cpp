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
    for (bool anticipative : {false, true})
        for (bool reset : {false, true})
            for (double offset : {0., 100.}) {
                CAPTURE(anticipative);
                CAPTURE(reset);
                CAPTURE(offset);
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
                                          if (frame == 0)
                                              events.push_back({7, 0, 3, {0x90, 60, 100}});
                                          if (frame == 64)
                                              events.push_back({9, 0, 3, {0x80, 60, 0}});
                                      }});
                Engine engine(pool, media,
                              {.anticipative = anticipative,
                               .renderBlock = 64,
                               .renderAheadMs = 10,
                               .workers = 0});
                REQUIRE(engine.start(backend,
                                     {.blockSize = 64, .midiInputs = true, .midiOutputs = true}));
                engine.setMidiPreferences({}, outputSettings(offset, true, reset));
                REQUIRE(engine.load(*s));
                REQUIRE(engine.awaitPlan());
                run(engine, backend, anticipative, 100);
                REQUIRE(events(backend, 0x94).size() == 1);
                CHECK(events(backend, 0x94)[0] == 7 + time::samplesAt(offset / 1000., 48000));
                REQUIRE(events(backend, 0x84).size() == 1);
                CHECK(events(backend, 0x84)[0] == 73 + time::samplesAt(offset / 1000., 48000));
                engine.stop();
            }
}

TEST_CASE("hardware MIDI: negative lead preserves the audio tail across a seek") {
    for (bool anticipative : {false, true}) {
        Session session(model::Tap::PreFx, false, false);
        auto pool = hardwarePool();
        MediaCache media;
        DummyBackend backend({.captureFrames = 14000, .midiOutputPorts = {"Synth"}});
        Engine engine(
            pool, media,
            {.anticipative = anticipative, .renderBlock = 64, .renderAheadMs = 10, .workers = 0});
        REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
        engine.setMidiPreferences({}, outputSettings(-20));
        REQUIRE(engine.load(session.value));
        CHECK(engine.midiLeadForOffset(0, -20) == 960);
        engine.play();
        run(engine, backend, anticipative, 100);
        engine.seek(0);
        run(engine, backend, anticipative, 100);
        engine.stop();
        std::size_t longest = 0, zeros = 0;
        const auto& audio = backend.capture().channels[0];
        for (std::size_t f = 6000; f < 8500; ++f) {
            zeros = audio[f] == 0 ? zeros + 1 : 0;
            longest = std::max(longest, zeros);
        }
        CHECK(longest <= 1);
    }
}
TEST_CASE("hardware MIDI: prepared audio history survives stop, wrap and lead changes") {
    auto runtime = std::make_unique<detail::HardwareMidiRuntime>();
    runtime->prepare(48000, 1);
    runtime->ensureHistory();
    DummyBackend backend({});
    REQUIRE(backend.start({.blockSize = 64}, [](auto, auto, auto) {}));
    detail::Plan plan;
    plan.hardwareLead = 960;
    plan.generation = 1;
    std::array<float, 64> signal;
    float* out[] = {signal.data()};
    auto cycle = [&](bool playing, std::uint64_t epoch) {
        signal.fill(1);
        runtime->begin(backend, 64);
        runtime->collect(plan, 0, 64, 0, playing, epoch);
        runtime->finish(out, 1, 64, false);
    };
    for (int i = 0; i < 100; ++i) cycle(true, 1);
    for (const auto epoch : {2, 3, 4}) {
        cycle(false, epoch);
        for (float sample : signal) CHECK(sample == 1);
        cycle(true, epoch);
        for (float sample : signal) CHECK(sample == 1);
    }
    plan.hardwareLead = 1920;
    ++plan.generation;
    for (int i = 0; i < 8; ++i) {
        cycle(true, 4);
        for (float sample : signal) CHECK(sample == 1);
    }
    backend.stop();
}

TEST_CASE("hardware MIDI: mix edits retain the plan and update route, source and VCA mutes") {
    for (bool anticipative : {false, true}) {
        Session base;
        model::EditTxn setup(base.value, 2);
        model::TrackGroup controller;
        controller.members = {base.track};
        controller.vca = model::Vca{};
        const auto group = setup.create(controller);
        auto session = setup.commit().value();
        auto pool = hardwarePool();
        MediaCache media;
        DummyBackend backend({.midiOutputPorts = {"Synth"}});
        Engine engine(
            pool, media,
            {.anticipative = anticipative, .renderBlock = 64, .renderAheadMs = 10, .workers = 0});
        REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
        REQUIRE(engine.load(session));
        engine.play();
        run(engine, backend, anticipative, 5);
        const auto generation = engine.planGeneration();
        auto apply = [&](auto change) {
            model::EditTxn edit(session, session.version + 1);
            change(edit);
            session = edit.commit().value();
            REQUIRE(engine.load(session));
            CHECK(engine.planGeneration() == generation);
            run(engine, backend, anticipative, 5);
        };
        apply([&](auto& e) {
            e.template modify<model::Track>(base.track, [](auto& t) { t.volumeDb = -6; });
        });
        CHECK(engine.mixEdits() == 1);
        for (int kind = 0; kind < 3; ++kind) {
            const auto before = events(backend, 0x84).size();
            auto mute = [&](bool on) {
                apply([&](auto& e) {
                    if (kind == 0)
                        e.template modify<model::Route>(base.route, [&](auto& r) { r.mute = on; });
                    else if (kind == 1)
                        e.template modify<model::Track>(base.track, [&](auto& t) { t.mute = on; });
                    else
                        e.template modify<model::TrackGroup>(group,
                                                             [&](auto& g) { g.vca->mute = on; });
                });
            };
            mute(true);
            CHECK(events(backend, 0x84).size() > before);
            mute(false);
        }
        engine.stop();
    }
}
TEST_CASE("hardware MIDI: scalar playrate edits retain an existing rate plan") {
    Session base;
    model::EditTxn initial(base.value, 2);
    auto project = initial.project();
    project.playrate.rate = .9;
    project.playrate.preservePitch = false;
    initial.setProject(project);
    const auto session = initial.commit().value();
    auto pool = hardwarePool();
    MediaCache media;
    DummyBackend backend({.midiOutputPorts = {"Synth"}});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
    REQUIRE(engine.load(session));
    const auto generation = engine.planGeneration();
    model::EditTxn changed(session, 3);
    project.playrate.rate = 1.1;
    changed.setProject(project);
    REQUIRE(engine.load(changed.commit().value()));
    CHECK(engine.planGeneration() == generation);
    engine.stop();
}

TEST_CASE("hardware MIDI: plug-in bus output reaches compensated taps and keeps fast mix edits") {
    for (bool anticipative : {false, true})
        for (auto tap : {model::Tap::PreFx, model::Tap::PreFader, model::Tap::PostFader})
            for (int variant : {0, 1, 2, 3}) {
                CAPTURE(anticipative);
                CAPTURE(int(tap));
                CAPTURE(variant);
                Session base(tap);
                model::EditTxn setup(base.value, 2);
                model::FxInstance echo;
                echo.plugin.id = "com.suppleaudio.test.note-echo";
                echo.midiOutput =
                    variant == 1 ? model::MidiOutputMode::Merge : model::MidiOutputMode::Replace;
                echo.bypass = variant == 2;
                const auto fx = setup.create(echo);
                setup.modify<model::Track>(base.track, [&](auto& t) {
                    t.chain.insert(t.chain.begin(), fx);
                    if (variant == 3) t.chainMidi = model::ChainMidi::Instrument;
                });
                auto session = setup.commit().value();
                auto pool = hardwarePool();
                MediaCache media;
                DummyBackend backend({.midiOutputPorts = {"Synth"}});
                Engine engine(pool, media,
                              {.anticipative = anticipative,
                               .renderBlock = 64,
                               .renderAheadMs = 10,
                               .workers = 0});
                REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
                REQUIRE(engine.load(session));
                engine.play();
                run(engine, backend, anticipative, 8);
                const bool generated = tap != model::Tap::PreFx && variant < 2;
                const std::vector<int> expected =
                    generated ? (variant == 1 ? std::vector<int>{60, 67} : std::vector<int>{67})
                              : std::vector<int>{60};
                auto keys = [&](std::uint8_t status) {
                    std::vector<int> result;
                    for (const auto& e : backend.midiCapture())
                        if (e.event.bytes[0] == status) result.push_back(e.event.bytes[1]);
                    std::sort(result.begin(), result.end());
                    return result;
                };
                CHECK(keys(0x94) == expected);  // channel filter 3, remap to 5
                for (const auto frame : events(backend, 0x94))
                    CHECK(frame == engine.outputLatency());
                if (generated) {
                    const auto& capture = backend.midiCapture();
                    CHECK(std::any_of(capture.begin(), capture.end(), [&](const auto& e) {
                        return e.event.bytes[0] == 0xb4 && e.event.bytes[1] == 74 &&
                               e.event.bytes[2] == 100 && e.frame == engine.outputLatency();
                    }));
                }
                const auto generation = engine.planGeneration();
                auto apply = [&](auto change) {
                    model::EditTxn edit(session, session.version + 1);
                    change(edit);
                    session = edit.commit().value();
                    REQUIRE(engine.load(session));
                    CHECK(engine.planGeneration() == generation);
                    run(engine, backend, anticipative, 8);
                };
                apply([&](auto& e) {
                    e.template modify<model::Track>(base.track, [](auto& t) { t.volumeDb = -6; });
                });
                apply([&](auto& e) {
                    e.template modify<model::Route>(base.route, [](auto& r) { r.mute = true; });
                });
                CHECK(keys(0x84) == expected);
                apply([&](auto& e) {
                    e.template modify<model::Route>(base.route, [](auto& r) { r.mute = false; });
                });
                auto restarted = expected;
                restarted.insert(restarted.end(), expected.begin(), expected.end());
                std::sort(restarted.begin(), restarted.end());
                CHECK(keys(0x94) == restarted);
                engine.stop();
                CHECK(keys(0x84) == restarted);
            }
}

TEST_CASE("hardware MIDI: adopting a post-FX route restarts the bus's held output") {
    for (bool anticipative : {false, true})
        for (auto tap : {model::Tap::PreFader, model::Tap::PostFader}) {
            CAPTURE(anticipative);
            CAPTURE(int(tap));
            Session base(model::Tap::PreFx);
            model::EditTxn setup(base.value, 2);
            model::FxInstance echo;
            echo.plugin.id = "com.suppleaudio.test.note-echo";
            const auto fx = setup.create(echo);
            setup.modify<model::Track>(base.track,
                                       [&](auto& t) { t.chain.insert(t.chain.begin(), fx); });
            const auto session = setup.commit().value();
            auto pool = hardwarePool();
            MediaCache media;
            DummyBackend backend({.midiOutputPorts = {"Synth"}});
            Engine engine(pool, media,
                          {.anticipative = anticipative,
                           .renderBlock = 64,
                           .renderAheadMs = 10,
                           .workers = 0});
            REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
            REQUIRE(engine.load(session));
            engine.play();
            run(engine, backend, anticipative, 8);
            REQUIRE(events(backend, 0x94).size() == 1);
            model::EditTxn change(session, 3);
            change.modify<model::Route>(base.route, [&](auto& r) { r.tap = tap; });
            REQUIRE(engine.load(change.commit().value()));
            REQUIRE(engine.awaitPlan());
            // Consume the old render-ahead horizon before inspecting the new route.
            run(engine, backend, anticipative, 100);
            std::vector<int> keys;
            for (const auto& e : backend.midiCapture())
                if (e.event.bytes[0] == 0x94) keys.push_back(e.event.bytes[1]);
            CHECK(keys == std::vector<int>{60, 67});
            engine.stop();
        }
}

TEST_CASE("hardware MIDI: reset-off boundaries preserve delayed releases and drop stale onsets") {
    for (bool anticipative : {false, true})
        for (bool locate : {false, true})
            for (bool beforeOnset : {false, true}) {
                CAPTURE(anticipative);
                CAPTURE(locate);
                CAPTURE(beforeOnset);
                Session base;
                auto pool = hardwarePool();
                MediaCache media;
                DummyBackend backend({.midiOutputPorts = {"Synth"}});
                Engine engine(pool, media,
                              {.anticipative = anticipative,
                               .renderBlock = 64,
                               .renderAheadMs = 10,
                               .workers = 0});
                REQUIRE(engine.start(backend, {.blockSize = 64, .midiOutputs = true}));
                engine.setMidiPreferences({}, outputSettings(100, true, false));
                REQUIRE(engine.load(base.value));
                engine.play();
                run(engine, backend, anticipative, beforeOnset ? 8 : 383);
                CHECK(events(backend, 0x94).size() == (beforeOnset ? 0 : 1));
                CHECK(events(backend, 0x84).empty());
                if (locate)
                    engine.seek(1);  // beyond this note: no fresh onset after the locate
                else
                    engine.pause();
                run(engine, backend, anticipative, 400);
                CHECK(events(backend, 0x94).size() == (beforeOnset ? 0 : 1));
                CHECK(events(backend, 0x84).size() == (beforeOnset ? 0 : 1));
                for (const auto& e : backend.midiCapture())
                    CHECK_FALSE((e.event.bytes[0] == 0xb4 && e.event.bytes[1] == 123));
                engine.stop();
                CHECK(events(backend, 0x84).size() == (beforeOnset ? 0 : 1));
            }
}

TEST_CASE("hardware MIDI: startup skips malformed preference records and reports each warning") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("midi-bad-record-" + std::to_string(test::processId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    AppOptions options;
    options.workdir = dir;
    options.midiPreferencesFile = dir / "preferences.json";
    REQUIRE(writeFileAtomic(
        options.midiPreferencesFile,
        R"({"schema":"supple.preferences/1.6","midi_outputs":{"Synth":{"offset_ms":2000},"Drums":{"enabled":false}},"midi_inputs":{"Broken":{"enabled":3},"Keyboard":{"offset_ms":5}}})"));
    auto app = EngineApp::create(options);
    REQUIRE(app);
    CHECK_FALSE((*app)->midiOutputsPreferences().contains("Synth"));
    CHECK_FALSE((*app)->midiInputsPreferences().contains("Broken"));
    CHECK_FALSE(midiPortSettings((*app)->midiOutputsPreferences(), "Drums").enabled);
    CHECK(midiPortSettings((*app)->midiInputsPreferences(), "Keyboard").offsetMs == 5);
    {
        cmd::Registry registry;
        registerEngineCommands(registry);
        cmd::Dispatcher bus(registry, **app);
        for (bool output : {false, true}) {
            const auto state = bus.execute({output ? "midi.outputs.get" : "midi.inputs.get",
                                            json::Value::object(),
                                            {cmd::Origin::Type::Ui, "test", {}}});
            REQUIRE(state);
            bool found = false;
            for (const auto& port : state->find("ports")->asArray()) {
                if (port.find("name")->asString() != (output ? "Synth" : "Broken")) continue;
                CHECK(port.find("offset_ms")->asDouble() == 0);
                for (const auto& warning : port.find("warnings")->asArray())
                    if (const auto* code = warning.find("code");
                        code && code->asString() == "midi.preferences") {
                        found = true;
                        CHECK(warning.find("path")->asString().find(
                                  output ? "/midi_outputs/Synth/offset_ms"
                                         : "/midi_inputs/Broken/enabled") == 0);
                    }
            }
            CHECK(found);
        }
        auto edits = json::Value::object();
        edits["offset_ms"] = 2;
        REQUIRE((*app)->setMidiPort(true, "synth", edits));
        CHECK(midiPortSettings((*app)->midiOutputsPreferences(), "Synth").offsetMs == 2);
        const auto corrected = (*app)->midiPortsState(true);
        for (const auto& port : corrected.asArray())
            for (const auto& warning : port.find("warnings")->asArray())
                CHECK(warning.find("code") == nullptr);
    }
    app->reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("hardware MIDI: lead changes crossfade two fixed history taps") {
    auto runtime = std::make_unique<detail::HardwareMidiRuntime>();
    runtime->prepare(48000, 1);
    runtime->ensureHistory();
    DummyBackend backend({});
    REQUIRE(backend.start({.blockSize = 64}, [](auto, auto, auto) {}));
    detail::Plan plan;
    plan.hardwareLead = 64;
    plan.generation = 1;
    std::array<float, 64> signal;
    float* out[] = {signal.data()};
    std::uint32_t frame = 0;
    auto sample = [](std::uint32_t f) { return float((f * 17) % 101) / 101; };
    auto cycle = [&] {
        for (std::uint32_t f = 0; f < 64; ++f) signal[f] = sample(frame + f);
        runtime->begin(backend, 64);
        runtime->collect(plan, 0, 64, 0, true, 1);
        runtime->finish(out, 1, 64, false);
        frame += 64;
    };
    for (int i = 0; i < 32; ++i) cycle();
    plan.hardwareLead = 128;
    ++plan.generation;
    const auto start = frame;
    for (int i = 0; i < 5; ++i) {
        const auto first = frame;
        cycle();
        for (std::uint32_t f = 0; f < 64; ++f) {
            const auto at = first + f;
            const double mix = std::min(1., double(at - start + 1) / 240.);
            const float expected = static_cast<float>((1 - mix) * double(sample(at - 64)) +
                                                      mix * double(sample(at - 128)));
            CHECK(signal[f] == doctest::Approx(double(expected)).epsilon(.000001));
        }
    }
    backend.stop();
}

TEST_CASE("hardware MIDI: zero-lead plans skip history allocation and processing") {
    auto runtime = std::make_unique<detail::HardwareMidiRuntime>();
    runtime->prepare(48000, 1);
    CHECK(runtime->historySamples() == 0);
    DummyBackend backend({});
    REQUIRE(backend.start({.blockSize = 64}, [](auto, auto, auto) {}));
    detail::Plan plan;
    plan.generation = 1;
    std::array<float, 64> signal;
    float* out[] = {signal.data()};
    auto cycle = [&] {
        signal.fill(1);
        runtime->begin(backend, 64);
        runtime->collect(plan, 0, 64, 0, true, 1);
        runtime->finish(out, 1, 64, false);
    };
    for (int i = 0; i < 100; ++i) cycle();
    CHECK(runtime->historySamples() == 0);
    for (float x : signal) CHECK(x == 1);
    runtime->ensureHistory();  // owner prepares before publishing the first negative-offset plan
    CHECK(runtime->historySamples() == 48001);
    plan.hardwareLead = 960;
    ++plan.generation;
    cycle();
    for (float x : signal) CHECK(x == 0);  // prime once; zero-lead history was never running
    CHECK_FALSE(runtime->releaseHistory());
    for (int i = 0; i < 100; ++i) cycle();
    for (float x : signal) CHECK(x == 1);
    plan.hardwareLead = 0;
    ++plan.generation;
    cycle();
    CHECK_FALSE(runtime->releaseHistory());  // crossfade still uses the history
    for (int i = 0; i < 8; ++i) cycle();
    REQUIRE(runtime->releaseHistory());  // all retained plans are zero-lead in this probe
    CHECK(runtime->historySamples() == 0);
    cycle();
    for (float x : signal) CHECK(x == 1);
    backend.stop();
}

TEST_CASE("hardware MIDI: delayed live onsets survive reset-off boundaries") {
    for (bool anticipative : {false, true})
        for (auto tap : {model::Tap::PreFx, model::Tap::PreFader, model::Tap::PostFader})
            for (bool reset : {false, true})
                for (bool shortNote : {false, true})
                    for (int boundary : {0, 1, 2}) {
                        CAPTURE(anticipative);
                        CAPTURE(int(tap));
                        CAPTURE(reset);
                        CAPTURE(shortNote);
                        CAPTURE(boundary);
                        Session base(tap, false, false);
                        model::EditTxn edit(base.value, 2);
                        model::Route input;
                        input.from.midiDevice = "Keyboard";
                        input.to.track = base.track;
                        input.midi = model::MidiRouting{0, 3};
                        const auto id = edit.create(input);
                        edit.modify<model::Track>(base.track, [&](auto& t) {
                            t.record.armed = true;
                            t.record.monitor = model::Monitor::On;
                            t.record.input = t.record.midiInput = id;
                        });
                        const auto session = edit.commit().value();
                        auto pool = hardwarePool();
                        MediaCache media;
                        DummyBackend backend(
                            {.midiOutputPorts = {"Synth"},
                             .midiPorts = {"Keyboard"},
                             .midi = [shortNote](auto& events, unsigned, std::int64_t frame) {
                                 if (frame == 256) {
                                     events.push_back({7, 0, 3, {0x90, 61, 100}});
                                     if (shortNote) events.push_back({9, 0, 3, {0x80, 61, 0}});
                                 }
                                 if (!shortNote && frame == 1024)
                                     events.push_back({9, 0, 3, {0x80, 61, 0}});
                             }});
                        Engine engine(pool, media,
                                      {.anticipative = anticipative,
                                       .renderBlock = 64,
                                       .renderAheadMs = 10,
                                       .workers = 0});
                        REQUIRE(engine.start(
                            backend, {.blockSize = 64, .midiInputs = true, .midiOutputs = true}));
                        engine.setMidiPreferences({}, outputSettings(100, true, reset));
                        REQUIRE(engine.load(session));
                        engine.play();
                        run(engine, backend, anticipative, 6);
                        CHECK(events(backend, 0x94).empty());
                        if (boundary == 1) engine.pause();
                        if (boundary == 2) engine.seek(1.5);
                        run(engine, backend, anticipative, 100);
                        std::vector<std::int64_t> on, off;
                        for (const auto& e : backend.midiCapture()) {
                            if (e.event.bytes[1] != 61) continue;
                            if (e.event.bytes[0] == 0x94) on.push_back(e.frame);
                            if (e.event.bytes[0] == 0x84) off.push_back(e.frame);
                        }
                        if (boundary == 0 || !reset) {
                            CHECK(on == std::vector<std::int64_t>{5063});
                            CHECK(off == std::vector<std::int64_t>{shortNote ? 5065 : 5833});
                        } else
                            CHECK(on.empty());
                        engine.stop();
                    }
}

TEST_CASE("hardware MIDI: unrelated preference writes retain malformed raw records") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("midi-raw-record-" + std::to_string(test::processId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    AppOptions options;
    options.workdir = dir;
    options.midiPreferencesFile = dir / "preferences.json";
    const auto original =
        json::parse(
            R"({"schema":"supple.preferences/1.6","midi_outputs":{"Synth":{"offset_ms":2000,"enabled":false,"reset_on_stop":false,"extra":{"keep":[1,"two"]}},"WrongType":17,"Drums":{"enabled":false}},"midi_inputs":{"Broken":{"enabled":"bad","offset_ms":5},"Keyboard":{"offset_ms":3}}})")
            .value();
    REQUIRE(writeFileAtomic(options.midiPreferencesFile, json::writeCanonical(original)));
    auto app = EngineApp::create(options);
    REQUIRE(app);
    auto read = [&](const std::filesystem::path& file) {
        const auto bytes = readFile(file);
        REQUIRE(bytes);
        auto value = json::parse(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        REQUIRE(value);
        return *value;
    };
    auto checkRaw = [&](json::Value saved) {
        CHECK(saved["midi_outputs"]["Synth"] == *original.find("midi_outputs")->find("Synth"));
        CHECK(saved["midi_outputs"]["WrongType"] == json::Value(17));
        CHECK(saved["midi_inputs"]["Broken"] == *original.find("midi_inputs")->find("Broken"));
    };
    // Writing a new file also retains the raw records: no reread can supply them.
    REQUIRE((*app)->writeMidiPreferences(dir / "copy.json"));
    checkRaw(read(dir / "copy.json"));
    auto edit = json::Value::object();
    edit["offset_ms"] = 7;
    REQUIRE((*app)->setMidiPort(true, "Drums", edit));
    checkRaw(read(options.midiPreferencesFile));
    edit["offset_ms"] = 9;
    REQUIRE((*app)->setMidiPort(false, "Keyboard", edit));
    checkRaw(read(options.midiPreferencesFile));
    edit["offset_ms"] = 2;
    REQUIRE((*app)->setMidiPort(true, "synth", edit));
    auto corrected = read(options.midiPreferencesFile);
    CHECK_FALSE(corrected["midi_outputs"]["Synth"]["enabled"].asBool());
    CHECK_FALSE(corrected["midi_outputs"]["Synth"]["reset_on_stop"].asBool());
    CHECK(corrected["midi_outputs"]["Synth"]["offset_ms"].asDouble() == 2);
    CHECK(corrected["midi_outputs"]["Synth"]["extra"] ==
          *original.find("midi_outputs")->find("Synth")->find("extra"));
    CHECK(corrected["midi_inputs"]["Broken"] == *original.find("midi_inputs")->find("Broken"));
    CHECK(corrected["midi_outputs"]["WrongType"] == json::Value(17));
    app->reset();
    app = EngineApp::create(options);
    REQUIRE(app);
    CHECK_FALSE(midiPortSettings((*app)->midiOutputsPreferences(), "Synth").enabled);
    CHECK(midiPortSettings((*app)->midiOutputsPreferences(), "Synth").offsetMs == 2);
    CHECK_FALSE((*app)->midiInputsPreferences().contains("Broken"));
    app->reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("hardware MIDI: a stopped live release may reuse a pre-block timeline slot") {
    for (bool anticipative : {false, true})
        for (auto tap : {model::Tap::PreFx, model::Tap::PreFader, model::Tap::PostFader})
            for (double offset : {0., 100.}) {
                CAPTURE(anticipative);
                CAPTURE(int(tap));
                CAPTURE(offset);
                Session base(tap, false, false);
                model::EditTxn edit(base.value, 2);
                model::Route input;
                input.from.midiDevice = "Keyboard";
                input.to.track = base.track;
                input.midi = model::MidiRouting{0, 3};
                const auto id = edit.create(input);
                edit.modify<model::Track>(base.track, [&](auto& t) {
                    t.record.armed = true;
                    t.record.monitor = model::Monitor::On;
                    t.record.input = t.record.midiInput = id;
                });
                auto pool = hardwarePool();
                MediaCache media;
                DummyBackend backend({.midiOutputPorts = {"Synth"},
                                      .midiPorts = {"Keyboard"},
                                      .midi = [](auto& events, unsigned, std::int64_t frame) {
                                          if (frame == 256) {
                                              events.push_back({7, 0, 3, {0x90, 61, 100}});
                                              events.push_back({9, 0, 3, {0x80, 61, 0}});
                                          }
                                      }});
                Engine engine(pool, media,
                              {.anticipative = anticipative,
                               .renderBlock = 64,
                               .renderAheadMs = 10,
                               .workers = 0});
                REQUIRE(engine.start(backend,
                                     {.blockSize = 64, .midiInputs = true, .midiOutputs = true}));
                engine.setMidiPreferences({}, outputSettings(offset, true, false));
                REQUIRE(engine.load(edit.commit().value()));
                engine.play();
                run(engine, backend, anticipative, 4);  // timeline key 60 occupies the slot
                engine.pause();
                run(engine, backend, anticipative, 100);
                std::vector<std::int64_t> on, off;
                for (const auto& e : backend.midiCapture()) {
                    if (e.event.bytes[1] != 61) continue;
                    if (e.event.bytes[0] == 0x94) on.push_back(e.frame);
                    if (e.event.bytes[0] == 0x84) off.push_back(e.frame);
                }
                const auto delay = time::samplesAt(offset / 1000., 48000);
                CHECK(on == std::vector<std::int64_t>{263 + delay});
                CHECK(off == std::vector<std::int64_t>{265 + delay});
                engine.stop();
            }
}
