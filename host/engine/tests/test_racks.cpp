// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modular racks in the render plan (ADR-0062, rack.md §6): Out modules feed
// their channels, sends reach In modules, Notes plays a track's MIDI, knob
// envelopes move module knobs, and the output does not depend on the block.
#include <doctest/doctest.h>

#include <cmath>

#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>

#include "plan.h"
#include "rack_runtime.h"
#include "supple/params/params.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

Id addChannel(model::EditTxn& t) {
    model::Track tr;
    tr.parent = kMaster;
    const Id id = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(id); });
    return id;
}

Id addModule(model::EditTxn& t, Id rack, std::string_view type, json::Value params = {}) {
    model::RackModule m;
    m.rack = rack;
    m.type = std::string(type);
    m.version = "1.0.0";
    if (params.isObject()) m.params = std::move(params);
    return t.create(m);
}

void cable(model::EditTxn& t, Id from, std::string from_key, Id to, std::string to_key,
           std::optional<double> amount = std::nullopt) {
    t.create(model::Cable{{from, std::move(from_key)}, {to, std::move(to_key)}, amount, ""});
}

// The rack's Out as `channel`'s input.
void listen(model::EditTxn& t, Id out, Id channel) {
    model::Route r;
    r.from.module = out;
    r.to.track = channel;
    const Id route = t.create(r);
    t.modify<model::Track>(channel, [&](model::Track& tr) { tr.record.input = route; });
}

model::Session commit(model::EditTxn& t) {
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    return std::move(*s);
}

Id addPlugin(model::EditTxn& t, Id rack, std::string id, std::optional<json::Value> patch = {}) {
    model::FxInstance fx;
    fx.plugin.id = std::move(id);
    fx.patch = std::move(patch);
    const Id fxId = t.create(fx);
    model::RackModule m;
    m.rack = rack;
    m.type = std::string(model::kPluginModule);
    m.version = "1.0.0";
    m.fx = fxId;
    return t.create(m);
}

// A linear console, so levels are exact: Transport's beat phase through a
// VCA into an Out module, the Out as a channel's input.
struct Ramp {
    model::Session s;
    Id channel, rack, transport, vca, out;
    explicit Ramp(bool console9 = false) {
        model::Project project;
        project.console9 = console9;
        model::EditTxn t(model::Session::create(project), 1);
        channel = addChannel(t);
        rack = t.create(model::Rack{channel, ""});
        transport = addModule(t, rack, "com.suppleaudio.rack.transport");
        vca = addModule(t, rack, "com.suppleaudio.rack.vca");
        out = addModule(t, rack, "com.suppleaudio.rack.out");
        cable(t, transport, "beat_phase", vca, "in");
        cable(t, vca, "out", out, "l");
        listen(t, out, channel);
        s = commit(t);
    }
};

std::vector<float> renderLeft(const model::Session& s, double seconds, std::uint32_t block = 512,
                              std::optional<Id> track = std::nullopt) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.blockSize = block;
    settings.track = track;
    auto r = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    for (const auto& w : r->warnings) MESSAGE(w);
    return r->audio.channels[0];
}

}  // namespace

TEST_CASE("racks: an Out module plays into the channel whose input it is") {
    Ramp ramp;
    const auto l = renderLeft(ramp.s, 1.0);
    REQUIRE(l.size() == 48000);
    // The beat phase ramps 0..1 every half second at 120 BPM; the channel
    // reaches the master at the neutral strip's gain (ADR-0019).
    const double g = 0.472135955;
    CHECK(double(l[6000]) / g == doctest::Approx(0.25).epsilon(2e-3));
    CHECK(double(l[23990]) / g == doctest::Approx(1.0).epsilon(2e-3));
    CHECK(double(l[24010]) / g == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE("racks: the render does not depend on the block size") {
    Ramp ramp(true);
    const auto a = renderLeft(ramp.s, 0.7, 512);
    const auto b = renderLeft(ramp.s, 0.7, 37);
    const auto c = renderLeft(ramp.s, 0.7, 1024);
    CHECK(a == b);
    CHECK(a == c);
}

TEST_CASE("racks: a channel's stem keeps the rack that feeds it") {
    Ramp ramp;
    model::EditTxn t(ramp.s, 2);
    const Id other = addChannel(t);
    (void)other;
    const auto s = commit(t);
    const auto stem = renderLeft(s, 0.5, 512, ramp.channel);
    double peak = 0;
    for (float x : stem) peak = std::max(peak, double(std::fabs(x)));
    CHECK(peak > 0.4);
}

TEST_CASE("racks: Notes plays the home track's MIDI into the patch") {
    // Notes' gate opens the VCA on a constant; the note is beats 1..2.
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id notes = addModule(t, rack, "com.suppleaudio.rack.notes");
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id vca = addModule(t, rack, "com.suppleaudio.rack.vca");
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    cable(t, transport, "playing", vca, "in");  // a constant 1 while playing
    cable(t, notes, "gate", vca, "cv");
    cable(t, vca, "out", out, "l");
    listen(t, out, channel);
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 960;
    src.sequence.notes = {{0, 960, 0, 60, 100, 0}};
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = channel;
    item.position = 1;
    item.length = 1;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const auto s = commit(t);
    const auto l = renderLeft(s, 1.5);
    CHECK(l[12000] == 0.0f);                                                // before beat 1 (0.5 s)
    CHECK(double(l[30000]) == doctest::Approx(0.472135955).epsilon(1e-3));  // the note
    CHECK(l[60000] == 0.0f);                                                // after beat 2 (1 s)
}

TEST_CASE("racks: an envelope moves a module's knob") {
    Ramp ramp;
    model::EditTxn t(ramp.s, 2);
    model::Envelope env;
    env.target = {ramp.vca, "level"};
    env.timebase = model::Timebase::Time;
    env.points = {{0.0, -60.0}, {0.5, -60.0, model::PointShape::Square}, {0.5001, 0.0}};
    t.create(env);
    const auto s = commit(t);
    const auto l = renderLeft(s, 1.0);
    CHECK(l[12000] == 0.0f);           // -60 dB is off (offAtMin)
    CHECK(std::fabs(l[30000]) > 0.1);  // 0 dB after half a second
}

TEST_CASE("racks: a send reaches an In module") {
    // Channel A's rack Out feeds channel B's rack In: B's rack passes it on.
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id a = addChannel(t), b = addChannel(t);
    const Id rackA = t.create(model::Rack{a, "A"});
    const Id transport = addModule(t, rackA, "com.suppleaudio.rack.transport");
    const Id outA = addModule(t, rackA, "com.suppleaudio.rack.out");
    cable(t, transport, "playing", outA, "l");
    listen(t, outA, a);
    const Id rackB = t.create(model::Rack{b, "B"});
    const Id in = addModule(t, rackB, "com.suppleaudio.rack.in");
    const Id outB = addModule(t, rackB, "com.suppleaudio.rack.out");
    cable(t, in, "l", outB, "l");
    listen(t, outB, b);
    model::Route send;
    send.from.track = a;
    send.to.module = in;
    send.tap = model::Tap::PreFader;
    t.create(send);
    // A plays only through the send: its own output is muted.
    t.modify<model::Track>(a, [](model::Track& tr) { tr.mute = true; });
    const auto s = commit(t);
    const auto l = renderLeft(s, 0.5);
    double peak = 0;
    for (std::size_t i = 4800; i < l.size(); ++i) peak = std::max(peak, double(std::fabs(l[i])));
    CHECK(peak > 0.2);
}

TEST_CASE("racks: a cycle through a rack is refused") {
    model::EditTxn t(model::Session::create(), 1);
    const Id a = addChannel(t);
    const Id rack = t.create(model::Rack{a, ""});
    const Id in = addModule(t, rack, "com.suppleaudio.rack.in");
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    listen(t, out, a);
    model::Route send;
    send.from.track = a;
    send.to.module = in;
    t.create(send);
    CHECK_FALSE(t.commit());
}

TEST_CASE("racks: real-time output equals the offline render" * doctest::description("rt")) {
    // Notes, a knob envelope and a plug-in, Console 9 on.
    model::Project project;
    project.seed = 9182736;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id notes = addModule(t, rack, "com.suppleaudio.rack.notes");
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id vca = addModule(t, rack, "com.suppleaudio.rack.vca");
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    // A plug-in in the patch: the gain fixture at -6 dB after the VCA.
    const Id gain = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}})").value());
    cable(t, transport, "beat_phase", vca, "in");
    cable(t, notes, "gate", vca, "cv");
    cable(t, vca, "out", gain, "in_l");
    cable(t, gain, "out_l", out, "l");
    // A changing cable into the plug-in's gain (CLAP param_mod).
    cable(t, transport, "beat_phase", *t.preview().modules.find(gain)->fx, "gain", 0.1);
    listen(t, out, channel);
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 4 * 960;
    for (int i = 0; i < 4; ++i) src.sequence.notes.push_back({i * 960, 480, 0, 60, 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = channel;
    item.length = 4;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    model::Envelope env;
    env.target = {vca, "level"};
    env.points = {{0.0, -20.0}, {4.0, 6.0}};
    t.create(env);
    const auto s = commit(t);
    InstancePool offlinePool = makePool();
    auto offline = renderOffline(s, offlinePool, {.startSeconds = 0, .endSeconds = 2.0}).value();
    for (const EngineOptions& options :
         {EngineOptions{.anticipative = false, .workers = 0}, EngineOptions{.workers = 0},
          EngineOptions{.workers = 3}}) {
        INFO("anticipative " << options.anticipative << ", workers " << options.workers);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000 + 4096});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
        REQUIRE(engine.load(s));
        engine.play();
        for (std::int64_t cycles = (96000 + 4096) / 96; cycles > 0;) {
            if (options.anticipative) REQUIRE(engine.awaitRendered(4096));
            const std::int64_t n = std::min<std::int64_t>(cycles, 4096 / 96);
            backend.run(n);
            cycles -= n;
        }
        engine.stop();
        CHECK(engine.processErrors() == 0);
        const auto& cap = backend.capture();
        const std::uint32_t L = engine.outputLatency();
        double worst = 0;
        for (std::size_t i = 0; i < 96000; ++i)
            worst = std::max(worst, std::fabs(double(cap.channels[0][L + i]) -
                                              double(offline.audio.channels[0][i])));
        CHECK(worst == 0.0);
    }
}

TEST_CASE("racks: a knob edit reaches the running rack without a compile") {
    Ramp ramp;
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(ramp.s));
    engine.play();
    backend.run(40);
    model::EditTxn t(ramp.s, 2);
    t.modify<model::RackModule>(ramp.vca, [](model::RackModule& m) {
        m.params = json::parse(R"({"level": -60})").value();
    });
    const auto quiet = commit(t);
    const auto edits = engine.mixEdits();
    REQUIRE(engine.load(quiet));
    CHECK(engine.mixEdits() == edits + 1);
    backend.run(40);
    engine.stop();
    const auto& cap = backend.capture();
    double before = 0, after = 0;
    for (std::size_t i = 2000; i < 5000; ++i)
        before = std::max(before, double(std::fabs(cap.channels[0][i])));
    for (std::size_t i = 6400; i < 10000; ++i)
        after = std::max(after, double(std::fabs(cap.channels[0][i])));
    CHECK(before > 0.01);
    CHECK(after < before * 0.01);  // gliding to off over the knob's 10 ms smoothing
}

TEST_CASE("racks: a plugin module runs its effect in the patch") {
    // Transport's `playing` (1 while playing) through the gain fixture at -6 dB.
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id gain = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}})").value());
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    cable(t, transport, "playing", gain, "in_l");
    cable(t, gain, "out_l", out, "l");
    listen(t, out, channel);
    const auto s = commit(t);
    const auto l = renderLeft(s, 0.5);
    CHECK(double(l[12000]) ==
          doctest::Approx(0.472135955 * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-3));
    CHECK(l == renderLeft(s, 0.5, 37));
}

TEST_CASE("racks: Notes' pitch and gate play a plugin module's instrument") {
    // A4 (key 69) on beats 1..2 into the sine fixture: 440 Hz at velocity * 0.25.
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id notes = addModule(t, rack, "com.suppleaudio.rack.notes");
    const Id sine = addPlugin(t, rack, "com.suppleaudio.test.sine");
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    cable(t, notes, "pitch", sine, "pitch");
    cable(t, notes, "gate", sine, "gate");
    cable(t, notes, "velocity", sine, "velocity");
    cable(t, sine, "out_l", out, "l");
    listen(t, out, channel);
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 960;
    src.sequence.notes = {{0, 960, 0, 69, 127, 0}};
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = channel;
    item.position = 1;
    item.length = 1;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const auto s = commit(t);
    const auto l = renderLeft(s, 1.5);
    double before = 0, during = 0, after = 0;
    int crossings = 0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        const double x = std::fabs(double(l[i]));
        if (i < 24000) before = std::max(before, x);
        if (i >= 28800 && i < 43200) {
            during = std::max(during, x);
            if (l[i - 1] < 0 && l[i] >= 0) ++crossings;
        }
        if (i >= 50000) after = std::max(after, x);
    }
    CHECK(before == 0.0);
    CHECK(during == doctest::Approx(0.25 * 0.472135955).epsilon(0.02));
    CHECK(crossings == doctest::Approx(440 * 0.3).epsilon(0.02));  // 0.3 s of A4
    CHECK(after == 0.0);
    CHECK(l == renderLeft(s, 1.5, 37));
}

TEST_CASE("racks: a cable modulates a plug-in's parameter, and stops when it goes" *
          doctest::description("rt")) {
    // Transport's `playing` (1 while playing) through the gain fixture at -6
    // dB, and into its gain: CLAP param_mod of `amount` in its CLAP units.
    auto build = [](bool modulated) {
        model::Project project;
        project.console9 = false;
        model::EditTxn t(model::Session::create(project), 1);
        const Id channel = addChannel(t);
        const Id rack = t.create(model::Rack{channel, ""});
        const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
        const Id gain = addPlugin(
            t, rack, "com.suppleaudio.test.gain",
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}})").value());
        const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
        cable(t, transport, "playing", gain, "in_l");
        cable(t, gain, "out_l", out, "l");
        listen(t, out, channel);
        auto s = t.preview();
        const Id fx = *s.modules.find(gain)->fx;
        if (modulated) cable(t, transport, "playing", fx, "gain", 0.1);
        return commit(t);
    };
    // The fixture's gain: -60..12 dB, its CLAP value normalized (ADR-0010).
    params::ParamSpec spec;
    spec.type = params::Type::Float;
    spec.unit = units::Unit::dB;
    spec.min = -60;
    spec.max = 12;
    const double modDb = params::fromClap(spec, params::toClap(spec, -6) + 0.1);
    REQUIRE(modDb > -6);
    const double base = 0.472135955;  // the channel's fader law at unity
    const auto modulated = build(true), plain = build(false);
    const auto l = renderLeft(modulated, 0.5);
    CHECK(double(l[12000]) == doctest::Approx(base * std::pow(10.0, modDb / 20.0)).epsilon(1e-3));
    CHECK(l == renderLeft(modulated, 0.5, 37));

    // The running engine: the cable goes, the plug-in goes back to -6 dB.
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    REQUIRE(engine.load(modulated));
    engine.play();
    backend.run(100);
    REQUIRE(engine.load(plain));
    backend.run(100);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto& cap = backend.capture();
    CHECK(double(cap.channels[0][9000]) ==
          doctest::Approx(base * std::pow(10.0, modDb / 20.0)).epsilon(1e-3));
    CHECK(double(cap.channels[0][19000]) ==
          doctest::Approx(base * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-3));
}

namespace {

// Transport's `playing` through the gain fixture at -6 dB; `mod` (an output of
// Transport) into its gain by 0.1.
model::Session modulatedGain(const char* mod) {
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id gain = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}})").value());
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    cable(t, transport, "playing", gain, "in_l");
    cable(t, gain, "out_l", out, "l");
    cable(t, transport, mod, *t.preview().modules.find(gain)->fx, "gain", 0.1);
    listen(t, out, channel);
    return commit(t);
}

}  // namespace

TEST_CASE("racks: a changing modulation does not depend on the block size") {
    // Sampled on the rack's grid, not where the host's blocks split it.
    const auto s = modulatedGain("beat_phase");
    CHECK(renderLeft(s, 0.1, 512) == renderLeft(s, 0.1, 37));
}

TEST_CASE("racks: a plugin module's plug-in keeps its settings and modulation when replaced" *
          doctest::description("rt")) {
    const auto s = modulatedGain("playing");
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(100);
    pool.restart({model::Kind::Fx, 1});  // a new instance of the same plug-in
    REQUIRE(engine.load(s));
    backend.run(100);
    engine.stop();
    const auto& cap = backend.capture();
    CHECK(double(cap.channels[0][19000]) == doctest::Approx(double(cap.channels[0][9000])));
}

TEST_CASE("racks: a plugin module's plug-in is oversampled as its FX settings say") {
    auto s = modulatedGain("playing");
    model::EditTxn t(s, 2);
    t.modify<model::FxInstance>({model::Kind::Fx, 1}, [](model::FxInstance& fx) {
        fx.oversampling = 4;
        fx.autoBypassOnSilence = true;
    });
    s = commit(t);
    InstancePool pool = makePool();
    auto p = supple::engine::detail::compile(s, pool, 0, 512, 48000);
    REQUIRE(p);
    std::size_t stages = 0;
    for (const auto& node : p->nodes)
        for (const auto& st : node.rackStages) {
            CHECK(st.os == 4);
            CHECK(st.autoBypassOnSilence);
            ++stages;
        }
    CHECK(stages == 1);
}

TEST_CASE("racks: a held gate plays again after a seek resets the instrument" *
          doctest::description("rt")) {
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id sine = addPlugin(t, rack, "com.suppleaudio.test.sine");
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    cable(t, transport, "playing", sine, "gate");  // held while playing
    cable(t, sine, "out_l", out, "l");
    listen(t, out, channel);
    const auto s = commit(t);
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    REQUIRE(engine.load(s));
    engine.play();
    backend.run(100);
    engine.seek(0.25);
    backend.run(100);
    engine.stop();
    const auto& cap = backend.capture();
    double before = 0, after = 0;
    for (std::size_t i = 5000; i < 9000; ++i)
        before = std::max(before, std::abs(double(cap.channels[0][i])));
    for (std::size_t i = 15000; i < 19000; ++i)
        after = std::max(after, std::abs(double(cap.channels[0][i])));
    CHECK(before > 0.01);
    CHECK(after == doctest::Approx(before).epsilon(0.05));
}
