// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI learn (ADR-0027): reading controller messages, and plug-in parameters
// moved by them through EngineApp::pollControl.
#include <doctest/doctest.h>
#include <unistd.h>

#include <deque>
#include <filesystem>
#include <mutex>

#include "supple/base/fader.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/engine/midi_control.h"
#include "supple/host/clap_host.h"
#include "supple/json/parse.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }

ControlMidiEvent msg(std::uint8_t status, std::uint8_t a, std::uint8_t b = 0,
                     std::uint16_t port = 0) {
    ControlMidiEvent e;
    e.port = port;
    e.bytes[0] = status;
    e.bytes[1] = a;
    e.bytes[2] = b;
    return e;
}

struct Root {
    fs::path path =
        fs::temp_directory_path() / ("supple-engine-learn-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

// An app with the test plug-ins (CLAP and VST3), a bus, and a manual dummy
// device with two MIDI inputs whose messages the test queues.
struct Rig {
    Root root;
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    std::mutex lock;
    std::deque<MidiInputEvent> queued;
    std::unique_ptr<DummyBackend> backend;

    Rig() {
        AppOptions o;
        o.workdir = root.path;
        o.chainDir = root.path / "chains";
        o.templateDir = root.path / "templates";
        o.projectTemplateDir = root.path / "project-templates";
        o.pluginPath = {kFixture, FIXTURE_VST3_PATH};
        auto made = EngineApp::create(o, {.console9 = false});
        REQUIRE(made);
        app = std::move(*made);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*bus);
        app->setFixedClock(1'000'000);
    }
    ~Rig() {
        if (app) app->stopEngine();
    }
    json::Value call(std::string command, std::string_view args) {
        auto r = bus->execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    }
    void start() {
        backend = std::make_unique<DummyBackend>(DummyBackend::Options{
            .mode = DummyBackend::Mode::Manual,
            .midiPorts = {"nanoKONTROL2 CTRL", "USB Keyboard"},
            .midi = [this](std::vector<MidiInputEvent>& out, std::uint32_t, std::int64_t) {
                std::lock_guard<std::mutex> g(lock);
                while (!queued.empty()) {
                    out.push_back(queued.front());
                    queued.pop_front();
                }
            }});
        REQUIRE(app->startEngine(*backend,
                                 {.sampleRate = 48000, .blockSize = 256, .midiInputs = true},
                                 {.anticipative = false})
                    .ok());
    }
    // A device sends `bytes` on `port`; a cycle hears it and the app polls.
    void send(std::uint8_t status, std::uint8_t a, std::uint8_t b, std::uint16_t port = 0) {
        {
            std::lock_guard<std::mutex> g(lock);
            MidiInputEvent e;
            e.port = port;
            e.size = 3;
            e.bytes[0] = status;
            e.bytes[1] = a;
            e.bytes[2] = b;
            queued.push_back(e);
        }
        backend->run(1);
        app->pollControl();
    }
    double gain(const char* fx) {
        const model::FxInstance* f = app->head().fx.find(*model::Id::parse(fx));
        REQUIRE(f);
        if (!f->patch) return 0;
        const json::Value* p = f->patch->find("params");
        const json::Value* g = p ? p->find("gain") : nullptr;
        return g && g->isNumber() ? g->asDouble() : 0;
    }
};

}  // namespace

TEST_CASE("MIDI learn: what a controller message is and says") {
    // Which control: kind, channel (1-based), number, device.
    auto cc = controlOf(msg(0xb3, 21, 64), "nanoKONTROL2");
    REQUIRE(cc);
    CHECK(cc->kind == model::ControlKind::Cc);
    CHECK(cc->channel == 4);
    CHECK(cc->number == 21);
    CHECK(cc->device == "nanoKONTROL2");
    CHECK(controlOf(msg(0x80, 60), "")->kind == model::ControlKind::Note);
    CHECK(controlOf(msg(0xe0, 0, 64), "")->number == 0);
    CHECK_FALSE(controlOf(msg(0xa0, 60, 10), ""));  // aftertouch is not a control
    CHECK(describe(*cc) == "CC 21 · ch 4");
    CHECK(describe(*controlOf(msg(0x99, 36, 100), "")) == "Note C2 · ch 10");

    // The device matches as a substring, any case; empty matches any.
    model::MidiControl c{model::ControlKind::Cc, 1, 21, "kontrol"};
    CHECK(matches(c, msg(0xb0, 21, 1), "nanoKONTROL2 CTRL"));
    CHECK_FALSE(matches(c, msg(0xb0, 21, 1), "USB Keyboard"));
    CHECK_FALSE(matches(c, msg(0xb1, 21, 1), "nanoKONTROL2"));  // channel 2
    CHECK_FALSE(matches(c, msg(0xb0, 22, 1), "nanoKONTROL2"));
    c.device.clear();
    CHECK(matches(c, msg(0xb0, 21, 1), "USB Keyboard"));
    model::MidiControl bend{model::ControlKind::PitchBend, 1, 0, ""};
    CHECK(matches(bend, msg(0xe0, 5, 99), ""));

    // The CC modes.
    using M = model::ControlMode;
    CHECK(read(msg(0xb0, 1, 127), M::Absolute).value == doctest::Approx(1.0));
    CHECK_FALSE(read(msg(0xb0, 1, 127), M::Absolute).relative);
    CHECK(read(msg(0xb0, 1, 127), M::Relative1).steps == -1);
    CHECK(read(msg(0xb0, 1, 1), M::Relative1).steps == 1);
    CHECK(read(msg(0xb0, 1, 63), M::Relative2).steps == -1);
    CHECK(read(msg(0xb0, 1, 65), M::Relative2).steps == 1);
    CHECK(read(msg(0xb0, 1, 65), M::Relative3).steps == -1);
    CHECK(read(msg(0xb0, 1, 3), M::Relative3).steps == 3);
    CHECK(read(msg(0xb0, 1, 1), M::Toggle).press);
    CHECK_FALSE(read(msg(0xb0, 1, 0), M::Toggle).press);
    CHECK(read(msg(0x90, 60, 127), M::Absolute).press);
    CHECK_FALSE(read(msg(0x90, 60, 0), M::Absolute).press);  // velocity 0: a release
    CHECK(read(msg(0xe0, 0x7f, 0x7f), M::Absolute).value == doctest::Approx(1.0));

    // The parameter's next value.
    Takeover t;
    CHECK(*nextValue(0.2, read(msg(0xb0, 1, 127), M::Absolute), M::Absolute, false, 0.01, t) ==
          doctest::Approx(1.0));
    CHECK(*nextValue(0.5, read(msg(0xb0, 1, 2), M::Relative1), M::Relative1, false, 0.1, t) ==
          doctest::Approx(0.7));
    CHECK(*nextValue(0.95, read(msg(0xb0, 1, 3), M::Relative1), M::Relative1, false, 0.1, t) ==
          doctest::Approx(1.0));  // held at the top
    CHECK(*nextValue(0.8, read(msg(0xb0, 1, 127), M::Toggle), M::Toggle, false, 1, t) == 0.0);
    CHECK(*nextValue(0.2, read(msg(0xb0, 1, 127), M::Toggle), M::Toggle, false, 1, t) == 1.0);
    CHECK_FALSE(nextValue(0.2, read(msg(0xb0, 1, 0), M::Toggle), M::Toggle, false, 1, t));
    // Soft takeover: the knob far below the value does nothing until it
    // reaches it, then follows.
    Takeover soft;
    auto at = [](int v) { return read(msg(0xb0, 1, static_cast<std::uint8_t>(v)), M::Absolute); };
    CHECK_FALSE(nextValue(0.5, at(10), M::Absolute, true, 0.01, soft));
    CHECK_FALSE(nextValue(0.5, at(40), M::Absolute, true, 0.01, soft));
    auto caught = nextValue(0.5, at(70), M::Absolute, true, 0.01, soft);  // crossed 0.5
    REQUIRE(caught);
    soft.set = *caught;
    CHECK(nextValue(*caught, at(20), M::Absolute, true, 0.01, soft));  // held: follows
    // Moved elsewhere since: waits again.
    CHECK_FALSE(nextValue(0.9, at(30), M::Absolute, true, 0.01, soft));
}

TEST_CASE("MIDI learn: fx.learn.set binds a parameter, saved with the project") {
    Rig rig;
    rig.call("track.add", "{}");
    rig.call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    auto set = rig.call(
        "fx.learn.set",
        R"({"fx":"fx_1","param":"gain","midi":{"kind":"cc","channel":2,"number":7,"device":"kontrol"},"mode":"relative2","soft_takeover":true})");
    CHECK(set["control"].asString() == "CC 7 · ch 2");
    // A second binding for the same parameter replaces the first.
    rig.call("fx.learn.set",
             R"({"fx":"fx_1","param":"gain","midi":{"kind":"note","channel":10,"number":36}})");
    const model::FxInstance& fx = *rig.app->head().fx.find(*model::Id::parse("fx_1"));
    REQUIRE(fx.learn.size() == 1);
    CHECK(fx.learn[0].control.kind == model::ControlKind::Note);
    CHECK(fx.learn[0].mode == model::ControlMode::Absolute);
    rig.call(
        "fx.learn.set",
        R"({"fx":"fx_1","param":"latency","midi":{"kind":"cc","channel":1,"number":20},"mode":"toggle","when_selected":true})");
    // Unknown parameters and unbound removals say what to do instead.
    auto bad = rig.bus->execute(
        {"fx.learn.set", j(R"({"fx":"fx_1","param":"gian","midi":{"kind":"cc","channel":1}})"),
         kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "fx.param_unknown");
    CHECK(bad.error().path == "/args/param");
    auto channel = rig.bus->execute(
        {"fx.learn.set", j(R"({"fx":"fx_1","param":"gain","midi":{"kind":"cc","channel":17}})"),
         kTest});
    REQUIRE_FALSE(channel);

    // Save and reopen: the bindings come back as they were.
    const auto file = rig.root.path / "learn.json";
    REQUIRE(rig.app->save(file));
    const std::string text = model::serializeProject(rig.app->head());
    CHECK(text.find("\"learn\"") != std::string::npos);
    REQUIRE(rig.app->open(file));
    const model::FxInstance& back = *rig.app->head().fx.find(*model::Id::parse("fx_1"));
    REQUIRE(back.learn.size() == 2);
    CHECK(back.learn[0].param == "gain");
    CHECK(back.learn[0].control == model::MidiControl{model::ControlKind::Note, 10, 36, ""});
    CHECK(back.learn[1].param == "latency");
    CHECK(back.learn[1].mode == model::ControlMode::Toggle);
    CHECK(back.learn[1].whenSelected);
    CHECK(model::serializeProject(rig.app->head()) == text);

    auto listed = rig.call("fx.learn.list", "{}");
    CHECK(listed["learn"].size() == 2);
    rig.call("fx.learn.remove", R"({"fx":"fx_1","param":"gain"})");
    auto again = rig.bus->execute({"fx.learn.remove", j(R"({"fx":"fx_1","param":"gain"})"), kTest});
    REQUIRE_FALSE(again);
    CHECK(again.error().code == "fx.learn_unknown");
    CHECK(rig.call("fx.learn.list", R"({"fx":"fx_1"})")["learn"].size() == 1);
}

TEST_CASE("MIDI learn: a controller moves a semantic parameter, one undo step per movement") {
    Rig rig;
    rig.call("track.add", "{}");
    rig.call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    rig.call(
        "fx.learn.set",
        R"({"fx":"fx_1","param":"gain","midi":{"kind":"cc","channel":1,"number":21,"device":"kontrol"}})");
    std::vector<EngineApp::ControlEvent> heard;
    rig.app->onControl = [&](const std::vector<EngineApp::ControlEvent>& events) {
        heard.insert(heard.end(), events.begin(), events.end());
    };
    rig.start();
    const std::size_t before = rig.app->versions().history(1000).size();
    rig.send(0xb0, 21, 127);
    CHECK(rig.gain("fx_1") == doctest::Approx(12.0));
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].device == "nanoKONTROL2 CTRL");
    rig.send(0xb0, 21, 0);
    CHECK(rig.gain("fx_1") == doctest::Approx(-60.0));
    // The same knob a moment later: still the same undo step.
    CHECK(rig.app->versions().history(1000).size() == before + 1);
    // Another channel, CC or device leaves it.
    rig.send(0xb1, 21, 127);
    rig.send(0xb0, 22, 127);
    rig.send(0xb0, 21, 127, 1);  // the keyboard
    CHECK(rig.gain("fx_1") == doctest::Approx(-60.0));
    CHECK(heard.size() == 5);
    // While a learn dialog listens, nothing moves.
    rig.app->setLearning(true);
    rig.send(0xb0, 21, 127);
    CHECK(rig.gain("fx_1") == doctest::Approx(-60.0));
    rig.app->setLearning(false);
    // After a pause, a new movement is a new undo step; undo goes back.
    rig.app->setFixedClock(1'005'000);
    rig.send(0xb0, 21, 127);
    CHECK(rig.gain("fx_1") == doctest::Approx(12.0));
    CHECK(rig.app->versions().history(1000).size() == before + 2);
    rig.call("history.undo", "{}");
    CHECK(rig.gain("fx_1") == doctest::Approx(-60.0));

    // An encoder: relative steps, each one 1/128 of the range.
    rig.call(
        "fx.learn.set",
        R"({"fx":"fx_1","param":"gain","midi":{"kind":"cc","channel":1,"number":21},"mode":"relative1"})");
    rig.send(0xb0, 21, 4);
    CHECK(rig.gain("fx_1") == doctest::Approx(-60.0 + 72.0 * 4 / 128).epsilon(0.01));
    rig.send(0xb0, 21, 126);  // -2
    CHECK(rig.gain("fx_1") == doctest::Approx(-60.0 + 72.0 * 2 / 128).epsilon(0.01));

    // A condition the window app decides: without it the binding waits.
    rig.call(
        "fx.learn.set",
        R"({"fx":"fx_1","param":"gain","midi":{"kind":"cc","channel":1,"number":21},"when_selected":true})");
    const double held = rig.gain("fx_1");
    rig.send(0xb0, 21, 127);
    CHECK(rig.gain("fx_1") == doctest::Approx(held));
    bool selected = false;
    rig.app->learnGate = [&](model::Id, const model::ParamLearn& l) {
        return l.whenSelected && selected;
    };
    rig.send(0xb0, 21, 127);
    CHECK(rig.gain("fx_1") == doctest::Approx(held));
    selected = true;
    rig.send(0xb0, 21, 127);
    CHECK(rig.gain("fx_1") == doctest::Approx(12.0));
}

TEST_CASE("MIDI learn: a plain parameter moves the running plug-in, then the project at rest") {
    Rig rig;
    rig.call("track.add", "{}");
    rig.call("fx.add", R"({"track":"tr_1","plugin":"Test VST3 Gain"})");
    auto set =
        rig.call("fx.learn.set",
                 R"({"fx":"fx_1","param":"Gain","midi":{"kind":"cc","channel":1,"number":1}})");
    CHECK(set["param"].asString() == "1");  // a plain parameter by its id
    rig.start();
    const model::Id fx = *model::Id::parse("fx_1");
    host::Instance* live = rig.app->instanceOf(fx);
    REQUIRE(live);
    const std::size_t before = rig.app->versions().history(1000).size();
    rig.send(0xb0, 1, 127);
    rig.send(0xb0, 1, 32);
    rig.backend->run(2);  // the audio thread takes the change
    CHECK(live->paramValue(1).value() == doctest::Approx(32.0 / 127).epsilon(0.001));
    CHECK(rig.app->nextServiceDelayMs() == 300);
    // Not yet in the project: the knob has not rested.
    CHECK(rig.app->versions().history(1000).size() == before);
    rig.app->setFixedClock(1'000'299);
    CHECK(rig.app->nextServiceDelayMs() == 1);
    auto transaction = rig.app->beginTransaction({}, "Defer learned value");
    REQUIRE(transaction);
    CHECK_FALSE(rig.app->nextServiceDelayMs());
    REQUIRE(rig.app->rollbackTransaction(*transaction));
    rig.app->setFixedClock(1'000'500);
    CHECK(rig.app->nextServiceDelayMs() == 0);
    rig.app->pollControl();
    CHECK_FALSE(rig.app->nextServiceDelayMs());
    const model::FxInstance& after = *rig.app->head().fx.find(fx);
    CHECK(after.params.at(1) == doctest::Approx(32.0 / 127).epsilon(0.001));
    CHECK(rig.app->versions().history(1000).size() == before + 1);
    // The saved state holds it too.
    REQUIRE(after.state);
    auto info = rig.app->findPlugin(after.plugin.id);
    REQUIRE(info);
    auto probe = host::Instance::create(*info, {});
    REQUIRE(probe);
    REQUIRE((*probe)->loadState(rig.app->head().blobs.at(*after.state)->data));
    CHECK((*probe)->paramValue(1).value() == doctest::Approx(32.0 / 127).epsilon(0.001));
}

TEST_CASE("MIDI learn: the last touched parameter is the window's last edit") {
    Rig rig;
    rig.call("track.add", "{}");
    rig.call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    rig.call("fx.add", R"({"track":"tr_1","plugin":"Test VST3 Gain"})");
    CHECK_FALSE(rig.app->lastTouched());
    // Agents' and tests' edits do not count; the window's do.
    rig.call("fx.param.set", R"({"fx":"fx_1","values":{"gain":-6}})");
    CHECK_FALSE(rig.app->lastTouched());
    const cmd::Origin window{cmd::Origin::Type::Ui, "gui", ""};
    REQUIRE(rig.bus->execute({"fx.param.set", j(R"({"fx":"fx_1","values":{"gain":-3}})"), window}));
    auto t = rig.app->lastTouched();
    REQUIRE(t);
    CHECK(t->fx.str() == "fx_1");
    CHECK(t->param == "gain");
    CHECK(rig.app->paramName(t->fx, t->param) == "Gain");
    const std::uint64_t serial = t->serial;
    // A plain parameter by name is touched by its id.
    REQUIRE(
        rig.bus->execute({"fx.param.set", j(R"({"fx":"fx_2","values":{"gain":0.5}})"), window}));
    t = rig.app->lastTouched();
    REQUIRE(t);
    CHECK(t->fx.str() == "fx_2");
    CHECK(t->param == "1");
    CHECK(t->serial > serial);
    CHECK(rig.app->paramName(t->fx, t->param) == "Gain");

    // moveParam moves it as a control would, without touching anything.
    rig.start();
    const std::uint64_t before = rig.app->lastTouched()->serial;
    auto set = rig.app->moveParam(*model::Id::parse("fx_1"), "gain",
                                  [](double, double) { return std::optional<double>(1.0); });
    REQUIRE(set);
    CHECK(*set == doctest::Approx(1.0));
    CHECK(rig.gain("fx_1") == doctest::Approx(12.0));
    // A plain one: live at once, committed at rest; the commit is no touch.
    set = rig.app->moveParam(*model::Id::parse("fx_2"), "1", [](double current, double step) {
        return std::optional<double>(current - 10 * step);
    });
    REQUIRE(set);
    rig.backend->run(2);
    rig.app->setFixedClock(1'000'500);
    rig.app->pollControl();
    CHECK(rig.app->head().fx.find(*model::Id::parse("fx_2"))->params.at(1) ==
          doctest::Approx(0.5 - 10.0 / 128).epsilon(0.001));
    CHECK(rig.app->lastTouched()->serial == before);
    // Unknown parameters and effects are left alone.
    CHECK_FALSE(rig.app->moveParam(*model::Id::parse("fx_1"), "nope",
                                   [](double, double) { return std::optional<double>(0.5); }));
    rig.call("fx.remove", R"({"fx":"fx_2"})");
    CHECK_FALSE(rig.app->lastTouched());  // gone with its effect
}

TEST_CASE("a control action may stop the engine during event-driven service") {
    Rig rig;
    rig.start();
    int received = 0;
    rig.app->onControl = [&](const auto& events) {
        received += static_cast<int>(events.size());
        rig.app->stopEngine();
    };
    {
        std::lock_guard lock(rig.lock);
        MidiInputEvent event;
        event.size = 3;
        event.bytes[0] = 0xb0;
        event.bytes[1] = 1;
        event.bytes[2] = 64;
        rig.queued.push_back(event);
    }
    rig.backend->run(1);
    rig.app->tick();
    CHECK(received == 1);
    CHECK(rig.app->engine() == nullptr);
    CHECK_FALSE(rig.app->nextServiceDelayMs());
}

TEST_CASE("MIDI learn: track.learn and route.learn bind a strip's and a send's controls") {
    Rig rig;
    rig.call("track.add", R"({"name":"Bass"})");
    rig.call("track.add", R"({"name":"Verb","type":"buss"})");
    rig.call("route.add", R"({"from":"tr_1","to":"tr_2"})");
    auto set = rig.call(
        "track.learn.set",
        R"({"track":"tr_1","param":"volume","midi":{"kind":"cc","channel":1,"number":7},"soft_takeover":true})");
    CHECK(set["control"].asString() == "CC 7 · ch 1");
    rig.call("track.learn.set",
             R"({"track":"tr_1","param":"mute","midi":{"kind":"note","channel":10,"number":36}})");
    rig.call(
        "route.learn.set",
        R"({"route":"sd_1","param":"tap:pre_fx","midi":{"kind":"cc","channel":1,"number":40},"mode":"toggle"})");
    // What a strip lacks, a misspelt control and a toggled knob say what to do.
    const auto fails = [&](std::string command, std::string_view args) {
        auto r = rig.bus->execute({std::move(command), j(args), kTest});
        REQUIRE_FALSE(r);
        return r.error();
    };
    CHECK(fails("track.learn.set",
                R"({"track":"tr_2","param":"monitor","midi":{"kind":"cc","channel":1}})")
              .code == "track.strip_type");
    const Error misspelt = fails(
        "track.learn.set", R"({"track":"tr_1","param":"vol","midi":{"kind":"cc","channel":1}})");
    CHECK(misspelt.code == "track.learn_param");
    CHECK(misspelt.hint.find("volume") != std::string::npos);
    CHECK(
        fails("route.learn.set",
              R"({"route":"sd_1","param":"pan","midi":{"kind":"cc","channel":1},"mode":"toggle"})")
            .code == "learn.mode");
    CHECK(fails("route.learn.set",
                R"({"route":"tr_1","param":"pan","midi":{"kind":"cc","channel":1}})")
              .code == "command.invalid_args");

    // Saved with the project and back as they were.
    const auto file = rig.root.path / "mix-learn.json";
    REQUIRE(rig.app->save(file));
    const std::string text = model::serializeProject(rig.app->head());
    REQUIRE(rig.app->open(file));
    const model::Track& bass = *rig.app->head().tracks.find(*model::Id::parse("tr_1"));
    REQUIRE(bass.learn.size() == 2);
    CHECK(bass.learn[0].param == "mute");
    CHECK(bass.learn[1].softTakeover);
    const model::Route& send = *rig.app->head().routes.find(*model::Id::parse("sd_1"));
    REQUIRE(send.learn.size() == 1);
    CHECK(send.learn[0].mode == model::ControlMode::Toggle);
    CHECK(model::serializeProject(rig.app->head()) == text);

    CHECK(rig.call("track.learn.list", "{}")["learn"].size() == 2);
    CHECK(rig.call("route.learn.list", R"({"route":"sd_1"})")["learn"]
              .at(0)
              .find("control")
              ->asString() == "CC 40 · ch 1");
    rig.call("track.learn.remove", R"({"track":"tr_1","param":"mute"})");
    CHECK(fails("track.learn.remove", R"({"track":"tr_1","param":"mute"})").code ==
          "track.learn_unknown");
    // A send goes with its bindings.
    rig.call("route.remove", R"({"route":"sd_1"})");
    CHECK(rig.call("route.learn.list", "{}")["learn"].size() == 0);
}

TEST_CASE("MIDI learn: controllers move many strips' faders, buttons and sends at once") {
    Rig rig;
    rig.call("track.add", R"({"name":"Bass"})");
    rig.call("track.add", R"({"name":"Verb","type":"buss"})");
    rig.call("route.add", R"({"from":"tr_1","to":"tr_2"})");
    rig.call("track.learn.set",
             R"({"track":"tr_1","param":"volume","midi":{"kind":"cc","channel":1,"number":0}})");
    rig.call("track.learn.set",
             R"({"track":"tr_2","param":"volume","midi":{"kind":"cc","channel":1,"number":1}})");
    rig.call("track.learn.set",
             R"({"track":"tr_2","param":"pan","midi":{"kind":"cc","channel":1,"number":16}})");
    rig.call("track.learn.set",
             R"({"track":"tr_1","param":"mute","midi":{"kind":"note","channel":1,"number":60}})");
    rig.call("track.learn.set",
             R"({"track":"tr_1","param":"solo","midi":{"kind":"cc","channel":1,"number":32}})");
    rig.call(
        "track.learn.set",
        R"({"track":"tr_1","param":"monitor","midi":{"kind":"cc","channel":1,"number":48},"mode":"toggle"})");
    rig.call(
        "route.learn.set",
        R"({"route":"sd_1","param":"volume","midi":{"kind":"cc","channel":1,"number":2},"mode":"relative2"})");
    rig.call(
        "route.learn.set",
        R"({"route":"sd_1","param":"tap:pre_fader","midi":{"kind":"note","channel":1,"number":61}})");
    CHECK(rig.app->hasLearn());
    rig.start();
    const auto track = [&](const char* id) {
        return *rig.app->head().tracks.find(*model::Id::parse(id));
    };
    const auto send = [&] { return *rig.app->head().routes.find(*model::Id::parse("sd_1")); };
    const std::size_t before = rig.app->versions().history(1000).size();

    // Two faders on two knobs, each along the fader's travel.
    rig.send(0xb0, 0, 127);
    rig.send(0xb0, 0, 100);  // the same movement: the same undo step
    rig.send(0xb0, 1, 0);
    CHECK(track("tr_1").volumeDb == doctest::Approx(fader::db(100 / 127.0)));
    CHECK(track("tr_2").volumeDb == model::kSilenceDb);
    CHECK(rig.app->versions().history(1000).size() == before + 2);  // one step per fader
    // A pan knob's centre is the centre.
    rig.send(0xb0, 16, 64);
    CHECK(track("tr_2").pan == 0);
    rig.send(0xb0, 16, 0);
    CHECK(track("tr_2").pan == -100);

    // A pad flips the mute on each press; its release leaves it.
    rig.send(0x90, 60, 100);
    CHECK(track("tr_1").mute);
    rig.send(0x80, 60, 0);
    CHECK(track("tr_1").mute);
    rig.send(0x90, 60, 100);
    CHECK_FALSE(track("tr_1").mute);
    // An absolute CC button holds the solo while it is down.
    rig.send(0xb0, 32, 127);
    CHECK(track("tr_1").solo == model::Solo::On);
    rig.send(0xb0, 32, 0);
    CHECK(track("tr_1").solo == model::Solo::Off);
    // The monitor goes off, on, auto in turn.
    rig.call("track.set", R"({"track":"tr_1","monitor":"off"})");
    rig.send(0xb0, 48, 127);
    CHECK(track("tr_1").record.monitor == model::Monitor::On);
    rig.send(0xb0, 48, 127);
    CHECK(track("tr_1").record.monitor == model::Monitor::Auto);

    // A send's level on an encoder: 1/128 of the travel per step.
    const double level = send().volumeDb;
    rig.send(0xb0, 2, 63);  // -1
    CHECK(send().volumeDb == doctest::Approx(fader::db(fader::position(level) - 1.0 / 128)));
    // Its tap on a pad.
    rig.send(0x90, 61, 90);
    CHECK(send().tap == model::Tap::PreFader);

    // Undo takes back the last movement only.
    rig.call("history.undo", "{}");
    CHECK(send().tap == model::Tap::PostFader);
    CHECK(send().volumeDb < level);
}
