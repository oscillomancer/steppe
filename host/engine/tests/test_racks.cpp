// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modular racks in the render plan (ADR-0062, rack.md §6): Out modules feed
// their channels, sends reach In modules, Notes plays a track's MIDI, knob
// envelopes move module knobs, and the output does not depend on the block.
#include <doctest/doctest.h>

#include <supple/audio/audio_file.h>
#include <chrono>
#include <cmath>

#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>

#include <clap/ext/params.h>
#include "plan.h"
#include "rack_runtime.h"
#include "supple/model/groups.h"
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
    const double g = 1.0;
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
    CHECK(l[12000] == 0.0f);                                        // before beat 1 (0.5 s)
    CHECK(double(l[30000]) == doctest::Approx(1.0).epsilon(1e-3));  // the note
    CHECK(l[60000] == 0.0f);                                        // after beat 2 (1 s)
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
    std::uint32_t block = 96;
    SUBCASE("playback rack") {}
    SUBCASE("live rack with host blocks coprime to the rack grid") {
        // An explicitly configured MIDI device puts the rack on the live
        // callback path when anticipation is enabled; the timeline still
        // supplies the notes, so no real MIDI device is needed.
        t.modify<model::RackModule>(notes, [](model::RackModule& m) { m.device = ""; });
        block = 37;
    }
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
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
        REQUIRE(engine.load(s));
        engine.play();
        for (std::int64_t cycles = (96000 + 4096) / block; cycles > 0;) {
            if (options.anticipative) REQUIRE(engine.awaitRendered(4096));
            const std::int64_t n = std::min<std::int64_t>(cycles, 4096 / block);
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
    CHECK(double(l[12000]) == doctest::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(1e-3));
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
    CHECK(during == doctest::Approx(0.25).epsilon(0.02));
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
    const double base = 1.0;  // the channel's fader law at unity
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
    CHECK(bool(renderLeft(s, 0.1, 512) == renderLeft(s, 0.1, 37)));
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

namespace {
struct NoteRouteScene {
    model::Session session, itemReference;
    Id source, target, rack, notes, noteOut;
    NoteRouteScene(bool live, bool compensated = false, bool rackLatency = false) {
        model::Project project;
        project.seed = 456789;
        project.console9 = !rackLatency;
        model::EditTxn t(model::Session::create(project), 1);
        source = addChannel(t);
        target = addChannel(t);
        model::FxInstance synth;
        synth.plugin.id = "com.suppleaudio.test.sine";
        const auto fx = t.create(synth);
        t.modify<model::Track>(target, [&](auto& tr) { tr.chain = {fx}; });
        std::optional<Id> delayed;
        if (compensated) {
            delayed = addChannel(t);
            const auto instrument = t.create(synth);
            model::FxInstance latency;
            latency.plugin.id = "com.suppleaudio.test.gain";
            latency.patch =
                json::parse(
                    R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                    .value();
            const auto effect = t.create(latency);
            t.modify<model::Track>(*delayed, [&](auto& tr) { tr.chain = {instrument, effect}; });
        }
        rack = t.create(model::Rack{source, "Generator"});
        notes = addModule(t, rack, model::kNotesModule);
        if (live) t.modify<model::RackModule>(notes, [](auto& m) { m.device = ""; });
        noteOut = addModule(t, rack, model::kNotesOutModule);
        cable(t, notes, "pitch", noteOut, "pitch");
        cable(t, notes, "gate", noteOut, "gate");
        cable(t, notes, "velocity", noteOut, "velocity");
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        midi.sequence.length = 3840;
        midi.sequence.notes = {{17, 263, 0, 60, 96, 0},
                               {341, 319, 0, 67, 64, 0},
                               {813, 411, 0, 72, 127, 0},
                               {1500, 217, 0, 48, 31, 0},
                               {1901, 803, 0, 65, 110, 0}};
        const auto mi = t.create(midi);
        model::Take take;
        take.source = mi;
        const auto tk = t.create(take);
        model::Item item;
        item.track = source;
        item.length = 4;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        if (rackLatency) {
            const auto latencyPlugin = addPlugin(
                t, rack, "com.suppleaudio.test.gain",
                json::parse(
                    R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                    .value());
            const auto latencyOut = addModule(t, rack, model::kOutModule);
            cable(t, latencyPlugin, "out_l", latencyOut, "l");
            // An unpatched audio input stays silent; the Out still sets the rack clock.
            const auto silentTarget = addChannel(t);
            listen(t, latencyOut, silentTarget);
        }
        auto reference = t.preview();
        model::EditTxn ref(reference, 2);
        item.track = target;
        take.source = ref.create(midi);
        const auto refTake = ref.create(take);
        item.takes = {refTake};
        item.activeTake = refTake;
        ref.create(item);
        if (delayed) {
            item.track = *delayed;
            take.source = ref.create(midi);
            const auto delayedTake = ref.create(take);
            item.takes = {delayedTake};
            item.activeTake = delayedTake;
            ref.create(item);
        }
        itemReference = commit(ref);
        model::Route route;
        route.from.module = noteOut;
        route.to.track = target;
        route.midi = model::MidiRouting{};
        t.create(route);
        if (delayed) {
            route.to.track = *delayed;
            t.create(route);
        }
        session = commit(t);
    }
};
}  // namespace

TEST_CASE("racks: Note Out matches MIDI items offline and in both real-time executors" *
          doctest::description("rt")) {
    bool live = false, compensated = false, rackLatency = false;
    SUBCASE("delay compensation and fan-out") { compensated = true; }
    SUBCASE("rack latency pads Note Out") { rackLatency = true; }
    SUBCASE("anticipative rack") {}
    SUBCASE("live rack") { live = true; }
    NoteRouteScene scene(live, compensated, rackLatency);
    const auto reference = renderLeft(scene.itemReference, 2.0, 512);
    const auto stem = renderLeft(scene.itemReference, 0.2, 37, scene.target);
    CHECK(bool(renderLeft(scene.session, 0.2, 37, scene.target) == stem));
    CHECK(bool(renderLeft(scene.session, 2.0, 512) == reference));
    CHECK(bool(renderLeft(scene.session, 2.0, 37) == reference));
    for (const auto& options : {EngineOptions{.anticipative = false, .workers = 0},
                                EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
        INFO("live " << live << ", anticipative " << options.anticipative << ", workers "
                     << options.workers);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 100096});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 37}));
        REQUIRE(engine.load(scene.session));
        engine.play();
        for (std::int64_t left = 100096 / 37; left > 0;) {
            if (options.anticipative) REQUIRE(engine.awaitRendered(4096));
            const auto cycles = std::min<std::int64_t>(left, 4096 / 37);
            backend.run(cycles);
            left -= cycles;
        }
        engine.stop();
        CHECK(engine.processErrors() == 0);
        const auto& cap = backend.capture().channels[0];
        const auto delay = engine.outputLatency();
        REQUIRE(cap.size() >= reference.size() + delay);
        double worst = 0;
        for (std::size_t i = 0; i < reference.size(); ++i)
            worst = std::max(worst, std::abs(double(cap[delay + i]) - double(reference[i])));
        CHECK(worst == 0);
    }
}

TEST_CASE("racks: Note Out addresses do not overlap item notes after a nonzero render start" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const auto home = addChannel(t), target = addChannel(t);
    model::FxInstance synth;
    synth.plugin.id = "com.suppleaudio.test.sine";
    const auto fx = t.create(synth);
    t.modify<model::Track>(target, [&](auto& tr) { tr.chain = {fx}; });
    const auto rack = t.create(model::Rack{home, ""});
    const auto clock = addModule(t, rack, "com.suppleaudio.rack.transport");
    const auto output = addModule(t, rack, model::kNotesOutModule);
    cable(t, clock, "playing", output, "gate");
    model::Route route;
    route.from.module = output;
    route.to.track = target;
    route.midi = model::MidiRouting{};
    t.create(route);
    model::MidiSource midi;
    midi.sequence.ppq = 960;
    midi.sequence.length = 7680;
    // The first three notes are omitted from events after the two-second
    // start. The remaining note keeps its original compiled source ID (3).
    midi.sequence.notes = {{0, 1, 0, 60, 96, 0},
                           {120, 1, 0, 60, 96, 0},
                           {240, 1, 0, 60, 96, 0},
                           {3841, 1, 0, 61, 96, 0}};
    model::Take take;
    take.source = t.create(midi);
    const auto tk = t.create(take);
    model::Item item;
    item.track = target;
    item.length = 8;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const auto session = commit(t);
    auto pool = makePool();
    auto compiled = d::compile(session, pool, 2.0, 37);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    REQUIRE(d::prepare(plan, session));
    for (auto* instance : plan.instances) REQUIRE(instance->startProcessing());
    const auto node = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                   [&](const auto& n) { return n.id == target; });
    REQUIRE(node != plan.nodes.end());
    REQUIRE(node->noteTarget >= 0);
    auto& stage = node->chain[static_cast<std::size_t>(node->noteTarget)];
    REQUIRE(d::processBlock(plan, 0, 37));
    REQUIRE(stage.voices.size() == 2);
    CHECK(std::count_if(stage.voices.begin(), stage.voices.end(),
                        [](const auto& voice) { return voice.live; }) == 1);
    CHECK(stage.voices[0].sourceId != stage.voices[1].sourceId);
    REQUIRE(d::processBlock(plan, 37, 37));
    REQUIRE(stage.voices.size() == 1);
    CHECK(stage.voices.front().live);
    for (auto* instance : plan.instances) instance->stopProcessing();
}

TEST_CASE("racks: recording mixes routed and device MIDI on their own clocks across a loop" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    for (std::int64_t shift : {0, 7, -7}) {
        INFO("destination shift " << shift);
        const Id track{model::Kind::Track, 1}, route{model::Kind::Route, 1};
        RecordTarget target;
        target.track = track;
        target.midi = RecordTarget::Midi{1, 0, 0, true, shift};
        d::Recorder recorder({target}, 48000, 5);
        REQUIRE(recorder.open());
        MidiInputEvent device;
        device.frame = 3;
        device.size = 3;
        device.bytes[0] = 0x90;
        device.bytes[1] = 61;
        device.bytes[2] = 80;
        d::RoutedMidiEvent routed;
        routed.frame = 3;
        routed.bytes[0] = 0x90;
        routed.bytes[1] = 60;
        routed.bytes[2] = 96;
        routed.route = route;
        routed.voice = 4;
        recorder.capture(nullptr, 0, 50, 0);
        recorder.captureMidi({&device, 1}, 0, 50, 0);
        recorder.captureRoutedMidi(track, {&routed, 1}, 50);
        device.bytes[0] = routed.bytes[0] = 0x80;
        recorder.capture(nullptr, 0, 50, 0);  // the next loop pass
        recorder.captureMidi({&device, 1}, 0, 50, 0);
        recorder.captureRoutedMidi(track, {&routed, 1}, 50);
        const auto takes = recorder.finish({0, 50});
        REQUIRE(takes.size() == 1);
        const auto& take = takes.front();
        const auto clock = std::max<std::int64_t>(5, shift);
        CHECK(take.startSample == -clock);
        CHECK(take.frames == 100 + clock - std::min<std::int64_t>(5, shift));
        REQUIRE(take.midi.size() == 4);
        auto event = [&](int voice, bool on) -> const RecordedMidiEvent& {
            auto found = std::find_if(take.midi.begin(), take.midi.end(), [&](const auto& e) {
                return e.voice == voice && (e.bytes[0] & 0xf0) == (on ? 0x90 : 0x80);
            });
            REQUIRE(found != take.midi.end());
            return *found;
        };
        CHECK(event(-1, true).timeline == -2);
        CHECK(event(-1, false).timeline == 48);  // device off stays in the previous pass
        CHECK(event(-1, false).run == 0);
        CHECK(event(4, true).timeline == 3 - shift);
        CHECK(event(4, true).route == route);
        CHECK(event(4, false).timeline == (shift <= 3 ? 3 - shift : 53 - shift));
        CHECK(event(4, false).run == (shift <= 3 ? 1 : 0));
        CHECK(take.overflowFrames == 0);
    }
}

TEST_CASE("racks: Note Out fan-in has distinct note addresses and survives routing edits" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const auto home = addChannel(t), first = addChannel(t), second = addChannel(t);
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const auto a = t.create(fx), b = t.create(fx);
    t.modify<model::Track>(first, [&](auto& tr) { tr.chain = {a}; });
    t.modify<model::Track>(second, [&](auto& tr) { tr.chain = {b}; });
    const auto rack = t.create(model::Rack{home, ""});
    const auto clock = addModule(t, rack, "com.suppleaudio.rack.transport");
    const auto one =
        addModule(t, rack, model::kNotesOutModule, json::parse(R"({"channel":3})").value());
    const auto two =
        addModule(t, rack, model::kNotesOutModule, json::parse(R"({"channel":4})").value());
    cable(t, clock, "playing", one, "gate");
    cable(t, clock, "playing", two, "gate");
    model::Route route;
    route.from.module = one;
    route.to.track = first;
    route.midi = model::MidiRouting{3, 5};
    const auto r1 = t.create(route);
    route.to.track = second;
    route.midi = model::MidiRouting{};
    t.create(route);
    route.from.module = two;
    route.to.track = first;
    const auto r2 = t.create(route);
    route.midi = model::MidiRouting{3, 0};
    t.create(route);  // channel 4 is filtered out
    auto session = commit(t);
    auto pool = makePool();
    auto compiled = d::compile(session, pool, 0, 37);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    REQUIRE(d::prepare(plan, session));
    std::vector<host::Instance*> started;
    auto start = [&](d::Plan& p) {
        for (auto* in : p.instances)
            if (!in->processing()) {
                REQUIRE(in->startProcessing());
                started.push_back(in);
            }
    };
    start(plan);
    auto stage = [&](d::Plan& p, Id id) -> d::Stage& {
        auto n = std::find_if(p.nodes.begin(), p.nodes.end(),
                              [&](const auto& node) { return node.id == id; });
        REQUIRE(n != p.nodes.end());
        REQUIRE(n->noteTarget >= 0);
        return n->chain[static_cast<std::size_t>(n->noteTarget)];
    };
    REQUIRE(d::processBlock(plan, 0, 37));
    REQUIRE(stage(plan, first).voices.size() == 2);
    CHECK(stage(plan, second).voices.size() == 1);
    CHECK(stage(plan, first).voices[0].noteId != stage(plan, first).voices[1].noteId);
    CHECK(stage(plan, first).voices[0].channel == 4);
    CHECK(stage(plan, first).voices[1].channel == 3);
    const auto heldId = stage(plan, second).voices.front().noteId;
    std::int64_t pos = 37;
    auto adopt = [&](model::EditTxn& edit) {
        session = commit(edit);
        auto next = d::compile(session, pool, 0, 37);
        REQUIRE(next);
        REQUIRE(d::prepare(*next, session));
        start(*next);
        d::carryMix(plan, *next, -1);
        d::locate(*next, pos, false);
        plan = std::move(*next);
        REQUIRE(d::processBlock(plan, pos, 37));
        pos += 37;
    };
    model::EditTxn label(session, 2);
    label.modify<model::RackModule>(clock, [](auto& m) { m.label = "Clock"; });
    adopt(label);
    CHECK(stage(plan, first).voices.size() == 2);
    CHECK(stage(plan, second).voices.front().noteId == heldId);
    model::EditTxn mute(session, 3);
    mute.modify<model::Route>(r1, [](auto& r) { r.mute = true; });
    adopt(mute);
    REQUIRE(stage(plan, first).voices.size() == 1);
    CHECK(stage(plan, first).voices.front().channel == 3);
    CHECK(stage(plan, second).voices.front().noteId == heldId);
    model::EditTxn unmute(session, 4);
    unmute.modify<model::Route>(r1, [](auto& r) { r.mute = false; });
    adopt(unmute);
    CHECK(stage(plan, first).voices.size() == 2);
    model::EditTxn remove(session, 5);
    remove.remove<model::Route>(r2);
    adopt(remove);
    CHECK(stage(plan, first).voices.size() == 1);
    model::EditTxn added(session, 6);
    route.midi = model::MidiRouting{};
    added.create(route);
    adopt(added);
    CHECK(stage(plan, first).voices.size() == 2);  // a new route receives a gate already held
    model::EditTxn replace(session, 7);
    const auto replacement = replace.create(fx);
    replace.modify<model::Track>(first, [&](auto& tr) { tr.chain = {replacement}; });
    replace.remove<model::FxInstance>(a);
    adopt(replace);
    CHECK(stage(plan, first).voices.size() == 2);  // a new instrument receives it too
    REQUIRE(d::processBlock(plan, pos, 37, false));
    CHECK(stage(plan, first).voices.empty());
    CHECK(stage(plan, second).voices.empty());
    d::locate(plan, 0);
    REQUIRE(d::processBlock(plan, 0, 37));
    CHECK(stage(plan, first).voices.size() == 2);
    CHECK(stage(plan, second).voices.size() == 1);
    d::locate(plan, 17);
    REQUIRE(d::processBlock(plan, 17, 37));
    CHECK(stage(plan, first).voices.size() == 2);
    CHECK(stage(plan, second).voices.size() == 1);
    for (auto* in : started) in->stopProcessing();
}

TEST_CASE("racks: feed-forward latency aligns independent Outs with reference impulses" *
          doctest::description("rt")) {
    bool bridged = false;
    SUBCASE("in-process effect") {}
    SUBCASE("shared bridge effect") {
        host::setBridgeExecutable(BRIDGE_PATH);
        bridged = true;
    }
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("rack-pdc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::filesystem::remove_all(dir); }
    } cleanup{dir};
    audio::AudioData audio;
    audio.sampleRate = 48000;
    audio.channels.assign(2, std::vector<float>(8192));
    for (auto at : {17, 479, 480, 2027, 4095}) audio.channels[0][at] = audio.channels[1][at] = .25f;
    const auto path = dir / "impulses.wav";
    REQUIRE(audio::writeWav(path, audio));
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const auto source = addChannel(t), wet = addChannel(t), dry = addChannel(t),
               reference = addChannel(t);
    const auto mediaId = t.create(model::MediaSource{path.string(), "", "wav", 48000, 2, 8192});
    model::Take take;
    take.source = mediaId;
    const auto tk = t.create(take);
    model::Item item;
    item.track = source;
    item.length = 8192. / 24000.;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    t.modify<model::Track>(source, [](auto& tr) { tr.mute = true; });
    const auto rack = t.create(model::Rack{wet, "Latency"});
    const auto in = addModule(t, rack, model::kInModule);
    const auto delayed = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value());
    if (bridged) {
        const auto fx = *t.preview().modules.find(delayed)->fx;
        t.modify<model::FxInstance>(
            fx, [](auto& effect) { effect.isolation = model::Isolation::SharedBridge; });
    }
    const auto out1 = addModule(t, rack, model::kOutModule),
               out2 = addModule(t, rack, model::kOutModule);
    cable(t, in, "l", delayed, "in_l");
    cable(t, delayed, "out_l", out1, "l");
    cable(t, in, "l", out2, "l");
    listen(t, out1, wet);
    listen(t, out2, dry);
    model::Route send;
    send.from.track = source;
    send.to.module = in;
    send.tap = model::Tap::PreFader;
    t.create(send);
    const auto referenceRack = t.create(model::Rack{reference, "Reference"});
    const auto referenceIn = addModule(t, referenceRack, model::kInModule);
    const auto referenceOut = addModule(t, referenceRack, model::kOutModule);
    cable(t, referenceIn, "l", referenceOut, "l");
    listen(t, referenceOut, reference);
    send.to.module = referenceIn;
    t.create(send);
    const auto s = commit(t);
    auto pool = makePool();
    MediaCache media;
    auto plan = supple::engine::detail::compile(s, pool, 0, 128);
    REQUIRE(plan);
    plan->media = &media;
    REQUIRE(supple::engine::detail::prepare(*plan, s));
    for (const auto& node : plan->nodes)
        if (node.rack && node.rack->rack == rack) {
            CHECK(node.latChain == 64);
            CHECK(node.rack->outLatencies == std::vector<std::int64_t>{64, 0});
        }
    const auto ref = renderLeft(s, .15, 512, reference);
    REQUIRE(*std::max_element(ref.begin(), ref.end()) > .02f);
    for (auto target : {wet, dry}) {
        const auto heard = renderLeft(s, .15, 37, target);
        CHECK(bool(heard == ref));
    }
    const auto offline = renderLeft(s, .15, 37);
    for (bool anticipative : {false, true})
        for (int workers : {0, 2}) {
            CAPTURE(anticipative);
            CAPTURE(workers);
            auto livePool = makePool();
            MediaCache liveMedia;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 12288});
            Engine engine(livePool, liveMedia, {.anticipative = anticipative, .workers = workers});
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 37}));
            REQUIRE(engine.load(s));
            engine.play();
            for (int i = 0; i < 320; ++i) {
                if (anticipative) REQUIRE(engine.awaitRendered(37));
                backend.run(1);
            }
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const auto latency = engine.outputLatency();
            const auto& cap = backend.capture().channels[0];
            REQUIRE(cap.size() >= latency + offline.size());
            CHECK(std::equal(offline.begin(), offline.end(), cap.begin() + latency));
        }
}

TEST_CASE("racks: removing scheduler feedback edges retains forward plug-in latency") {
    model::EditTxn t(model::Session::create(), 1);
    const auto channel = addChannel(t), rack = t.create(model::Rack{channel, ""});
    const auto a = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value());
    const auto b = addModule(t, rack, "com.suppleaudio.rack.vca");
    const auto out = addModule(t, rack, model::kOutModule);
    cable(t, a, "out_l", b, "in");
    cable(t, b, "out", a, "in_l");
    cable(t, a, "out_l", out, "l");
    listen(t, out, channel);
    const auto s = commit(t);
    auto pool = makePool();
    MediaCache media;
    auto plan = supple::engine::detail::compile(s, pool, 0, 128);
    REQUIRE(plan);
    plan->media = &media;
    REQUIRE(supple::engine::detail::prepare(*plan, s));
    for (const auto& node : plan->nodes)
        if (node.rack) CHECK(node.latChain == 64);
    CHECK(supple::engine::detail::rackLatencies(s, rack, {{a, 64}}).latency == 64);
    model::EditTxn forward(s, 2);
    for (const auto& entry : s.cables)
        if (entry.value->to.object == a) forward.remove<model::Cable>(entry.id);
    const auto feed = supple::engine::detail::rackLatencies(forward.preview(), rack, {{a, 64}});
    CHECK(feed.latency == 64);
    CHECK(feed.outputs.at(out) == 64);
}

TEST_CASE("racks: a running plug-in latency change rebuilds compensation" *
          doctest::description("rt")) {
    const auto none = modulatedGain("playing");
    model::EditTxn edit(none, 2);
    const Id fx{model::Kind::Fx, 1};
    edit.modify<model::FxInstance>(
        fx, [](auto& effect) { (*effect.patch)["params"]["latency"] = "short"; });
    const auto delayed = commit(edit);
    for (bool anticipative : {false, true}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, {.anticipative = anticipative, .workers = 2});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(none));
        REQUIRE(engine.awaitPlan());
        engine.play();
        auto run = [&] {
            if (anticipative) REQUIRE(engine.awaitRendered(128));
            backend.run(1);
            engine.service();
        };
        for (int i = 0; i < 10; ++i) run();
        auto* instance = pool.find(fx);
        REQUIRE(instance);
        CHECK(instance->latency() == 0);
        CHECK(engine.outputLatency() == 0);
        REQUIRE(engine.load(delayed));
        for (int i = 0; i < 160 && instance->latency() != 64; ++i) run();
        CHECK(instance->latency() == 64);
        for (int i = 0; i < 20; ++i) run();
        CHECK(pool.rememberedLatency(fx) == 64);
        CHECK(engine.outputLatency() == 64);
        CHECK_FALSE(pool.restarting(fx));
        CHECK(engine.processErrors() == 0);
        engine.stop();
    }
}

TEST_CASE("racks: numeric semantic parameter keys take precedence over CLAP ids") {
    std::vector<host::ParamInfo> params = {{0, "Gain", "", 0, 1, 0, CLAP_PARAM_IS_MODULATABLE},
                                           {1, "Other", "", 0, 1, 0, CLAP_PARAM_IS_MODULATABLE}};
    const auto schema = *json::parse(R"({"params":{"1":{"id":0}}})");
    const auto semantic = supple::engine::detail::resolvePluginMod(params, &schema, "1");
    REQUIRE(semantic);
    CHECK(semantic->param == 0);
    const auto numeric = supple::engine::detail::resolvePluginMod(params, nullptr, "1");
    REQUIRE(numeric);
    CHECK(numeric->param == 1);
}

TEST_CASE("racks: modulation back edges retain latency and only routed outputs set the node") {
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const auto channel = addChannel(t), target = addChannel(t);
    const auto rack = t.create(model::Rack{channel, "Timing"});
    const auto input = addModule(t, rack, model::kInModule);
    const auto effect = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        *json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})"));
    const auto follower = addModule(t, rack, "com.suppleaudio.rack.follower");
    const auto output = addModule(t, rack, model::kOutModule);
    const auto fx = *t.preview().modules.find(effect)->fx;
    cable(t, input, "l", effect, "in_l");
    cable(t, effect, "out_l", output, "l");
    cable(t, effect, "out_l", follower, "in");
    cable(t, follower, "envelope", fx, "gain", .1);
    listen(t, output, channel);
    const auto s = commit(t);
    auto prepared = [&](const model::Session& state) {
        auto pool = makePool();
        MediaCache media;
        auto plan = supple::engine::detail::compile(state, pool, 0, 128);
        REQUIRE(plan);
        plan->media = &media;
        REQUIRE(supple::engine::detail::prepare(*plan, state));
        for (const auto& node : plan->nodes)
            if (node.rack) return node.latChain;
        return std::int64_t{-1};
    };
    CHECK(prepared(s) == 64);
    // The same effect is inaudible after its Out route is removed.
    model::EditTxn unused(s, 2);
    for (const auto& route : s.routes)
        if (route.value->from.module == output) {
            unused.remove<model::Route>(route.id);
            unused.modify<model::Track>(channel, [](auto& tr) { tr.record.input.reset(); });
        }
    const auto direct = addModule(unused, rack, model::kOutModule);
    listen(unused, direct, channel);
    const auto silent = commit(unused);
    CHECK(prepared(silent) == 0);
    // A routed Note Out is later than the direct audio Out and sets the node.
    model::EditTxn notes(silent, 3);
    const auto noteOut = addModule(notes, rack, model::kNotesOutModule);
    cable(notes, effect, "out_l", noteOut, "gate");
    model::Route noteRoute;
    noteRoute.from.module = noteOut;
    noteRoute.to.track = target;
    noteRoute.midi = model::MidiRouting{};
    notes.create(noteRoute);
    const auto withNotes = commit(notes);
    CHECK(prepared(withNotes) == 64);
    model::EditTxn midiOnly(withNotes, 4);
    for (const auto& route : withNotes.routes)
        if (route.value->from.module == direct) {
            midiOnly.remove<model::Route>(route.id);
            midiOnly.modify<model::Track>(channel, [](auto& tr) { tr.record.input.reset(); });
        }
    CHECK(prepared(commit(midiOnly)) == 64);
}

TEST_CASE(
    "racks: oversampled stages stay stable across service and plugin-requested restart aligns "
    "outputs" *
    doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    struct Control {
        void (*edit)(const clap_plugin_t*, double, std::uint32_t);
        std::uint32_t (*deliveries)(const clap_plugin_t*);
        void (*latency)(const clap_plugin_t*, bool);
        std::uint32_t (*creates)();
    };
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const auto left = addChannel(t), right = addChannel(t);
    t.modify<model::Track>(left, [](auto& tr) { tr.pan = -100; });
    t.modify<model::Track>(right, [](auto& tr) { tr.pan = 100; });
    const auto rack = t.create(model::Rack{left, "Oversampling"});
    const auto clock = addModule(t, rack, "com.suppleaudio.rack.transport");
    const auto dynamic = addPlugin(t, rack, "com.suppleaudio.test.gain");
    const auto fixed = addPlugin(
        t, rack, "com.suppleaudio.test.gain",
        *json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})"));
    const auto a = addModule(t, rack, model::kOutModule), b = addModule(t, rack, model::kOutModule);
    cable(t, clock, "clock", dynamic, "in_l");
    cable(t, clock, "clock", fixed, "in_l");
    cable(t, dynamic, "out_l", a, "l");
    cable(t, fixed, "out_l", b, "l");
    listen(t, a, left);
    listen(t, b, right);
    const auto fx = *t.preview().modules.find(dynamic)->fx;
    for (const auto& entry : t.preview().fx)
        t.modify<model::FxInstance>(entry.id, [](auto& effect) { effect.oversampling = 4; });
    const auto state = commit(t);
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        auto pool = makePool();
        MediaCache media;
        // Room for every cycle below, the restart's longest wait included.
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = (20 + 40 + 160 + 180) * 128});
        Engine engine(pool, media, {.anticipative = anticipative, .workers = 2});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(state));
        REQUIRE(engine.awaitPlan());
        auto run = [&] {
            if (anticipative) REQUIRE(engine.awaitRendered(128));
            backend.run(1);
            engine.service();
        };
        const auto first = engine.planGeneration();
        for (int i = 0; i < 20; ++i) run();
        REQUIRE(engine.planGeneration() == first);
        engine.play();
        for (int i = 0; i < 40; ++i) run();
        REQUIRE(engine.planGeneration() == first);
        auto* instance = pool.find(fx);
        REQUIRE(instance);
        const auto* plugin = instance->raw();
        const auto* control =
            static_cast<const Control*>(plugin->get_extension(plugin, "supple.test.gain-control"));
        REQUIRE(control);
        control->latency(plugin, true);
        for (int i = 0; i < 160 && (instance->latency() != 64 || !instance->processing()); ++i)
            run();
        CHECK(engine.planGeneration() > first);
        REQUIRE(instance->latency() == 64);
        const auto settled = engine.planGeneration();
        // The anticipative renderer takes the compensating plan where it has
        // rendered to, up to its render-ahead span past what has played: the
        // switch's transient can land that much later.
        const auto ahead =
            anticipative ? static_cast<std::size_t>(EngineOptions{}.renderAheadMs / 1000. * 48000)
                         : 0;
        const auto compareFrom = static_cast<std::size_t>(backend.cycles() * 128) + ahead + 2048;
        for (int i = 0; i < 180; ++i) run();
        CHECK(engine.planGeneration() == settled);
        CHECK(engine.processErrors() == 0);
        engine.stop();
        const auto& audio = backend.capture();
        const auto end = static_cast<std::size_t>(backend.cycles() * 128);
        REQUIRE(end > compareFrom);
        REQUIRE(end <= audio.frames());
        CHECK(std::equal(audio.channels[0].begin() + compareFrom, audio.channels[0].begin() + end,
                         audio.channels[1].begin() + compareFrom));
        CHECK(*std::max_element(audio.channels[0].begin() + compareFrom,
                                audio.channels[0].begin() + end) > .1f);
    }
}

TEST_CASE("racks: In sends preserve tap level and pan without Console 9 trim" *
          doctest::description("rt")) {
    for (bool console : {false, true}) {
        CAPTURE(console);
        model::Project project;
        project.console9 = console;
        model::EditTxn t(model::Session::create(project), 1);
        const auto source = addChannel(t), target = addChannel(t);
        t.modify<model::Track>(source, [](auto& tr) { tr.parentSend.enabled = false; });
        const auto sourceRack = t.create(model::Rack{source, "Source"});
        const auto clock = addModule(t, sourceRack, "com.suppleaudio.rack.transport");
        const auto sourceOut = addModule(t, sourceRack, model::kOutModule);
        cable(t, clock, "playing", sourceOut, "l");
        cable(t, clock, "tempo", sourceOut, "r");
        listen(t, sourceOut, source);
        const auto rack = t.create(model::Rack{target, "Through"});
        const auto in = addModule(t, rack, model::kInModule),
                   out = addModule(t, rack, model::kOutModule);
        cable(t, in, "l", out, "l");
        cable(t, in, "r", out, "r");
        listen(t, out, target);
        model::Route route;
        route.from.track = source;
        route.to.module = in;
        route.tap = model::Tap::PreFader;
        route.pan = -37;
        const auto send = t.create(route);
        model::Envelope envelope;
        envelope.target = {send, "volume"};
        envelope.timebase = model::Timebase::Time;
        envelope.points = {{0, -12}, {.01, -3}};
        t.create(envelope);
        const auto state = commit(t);
        auto pool = makePool();
        MediaCache media;
        auto plan = supple::engine::detail::compile(state, pool, 0, 128);
        REQUIRE(plan);
        plan->media = &media;
        REQUIRE(supple::engine::detail::prepare(*plan, state));
        REQUIRE(supple::engine::detail::processBlock(*plan, 0, 128));
        const supple::engine::detail::Node* sourceNode = nullptr;
        const supple::engine::detail::Node* rackNode = nullptr;
        for (const auto& node : plan->nodes) {
            if (node.id == source) sourceNode = &node;
            if (node.rack && node.rack->rack == rack) rackNode = &node;
        }
        REQUIRE(sourceNode);
        REQUIRE(rackNode);
        REQUIRE(rackNode->rackIns.size() == 1);
        // The source strip's law: 0 dB by default (ADR-0066).
        const auto [panLeft, panRight] = supple::engine::detail::panSides(-.37, {});
        REQUIRE(sourceNode->preFader.l[64] != 0);
        REQUIRE(sourceNode->preFader.r[64] != 0);
        CHECK(sourceNode->preFader.l[64] != sourceNode->preFader.r[64]);
        auto gainAt = [](std::uint32_t frame) {
            return console9::faderGain(-12 + 9 * double(frame) / 480.);
        };
        for (std::uint32_t i = 0; i < 128; ++i) {
            CHECK(rackNode->rackIns[0].l[i] ==
                  static_cast<float>(double(sourceNode->preFader.l[i]) * panLeft * gainAt(i)));
            CHECK(rackNode->rackIns[0].r[i] ==
                  static_cast<float>(double(sourceNode->preFader.r[i]) * panRight * gainAt(i)));
        }
        CHECK(rackNode->rackIns[0].l[127] > rackNode->rackIns[0].l[0]);
        const auto offline = renderLeft(state, .1, 37);
        for (bool anticipative : {false, true})
            for (int workers : {0, 2}) {
                INFO("anticipative " << anticipative << ", workers " << workers);
                auto livePool = makePool();
                MediaCache liveMedia;
                DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
                Engine engine(livePool, liveMedia,
                              {.anticipative = anticipative, .workers = workers});
                REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 37}));
                REQUIRE(engine.load(state));
                engine.play();
                for (int i = 0; i < 210; ++i) {
                    if (anticipative) REQUIRE(engine.awaitRendered(37));
                    backend.run(1);
                }
                engine.stop();
                CHECK(engine.processErrors() == 0);
                const auto offset = engine.outputLatency();
                REQUIRE(backend.capture().frames() >= offset + offline.size());
                CHECK(std::equal(offline.begin(), offline.end(),
                                 backend.capture().channels[0].begin() + offset));
            }
    }
}

// The rack is not a summing stage (rack.md §2.4): a buss's send into an In
// carries the buss's own signal, after the decode of the sum it receives and
// before its encode to its parent, with the route's level and pan alone.
TEST_CASE("racks: In sends from a buss carry its signal without Console 9 encoding" *
          doctest::description("rt")) {
    for (bool console : {false, true})
        for (model::Tap tap : {model::Tap::PreFader, model::Tap::PostFader}) {
            const bool post = tap == model::Tap::PostFader;
            CAPTURE(console);
            CAPTURE(post);
            model::Project project;
            project.console9 = console;
            model::EditTxn t(model::Session::create(project), 1);
            model::Track buss;
            buss.type = model::StripType::Buss;
            buss.parent = kMaster;
            buss.volumeDb = -4.5;
            const Id bus = t.create(buss);
            t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(bus); });
            model::Track channel;
            channel.parent = bus;
            const Id source = t.create(channel);
            t.modify<model::Track>(bus, [&](model::Track& b) { b.children.push_back(source); });
            const auto target = addChannel(t);
            const auto sourceRack = t.create(model::Rack{source, "Source"});
            const auto clock = addModule(t, sourceRack, "com.suppleaudio.rack.transport");
            const auto sourceOut = addModule(t, sourceRack, model::kOutModule);
            cable(t, clock, "playing", sourceOut, "l");
            cable(t, clock, "tempo", sourceOut, "r");
            listen(t, sourceOut, source);
            const auto rack = t.create(model::Rack{target, "Through"});
            const auto in = addModule(t, rack, model::kInModule),
                       out = addModule(t, rack, model::kOutModule);
            cable(t, in, "l", out, "l");
            cable(t, in, "r", out, "r");
            listen(t, out, target);
            model::Route route;
            route.from.track = bus;
            route.to.module = in;
            route.tap = tap;
            route.volumeDb = -6;
            route.pan = -37;
            t.create(route);
            const auto state = commit(t);
            auto pool = makePool();
            MediaCache media;
            auto plan = supple::engine::detail::compile(state, pool, 0, 128);
            REQUIRE(plan);
            plan->media = &media;
            REQUIRE(supple::engine::detail::prepare(*plan, state));
            REQUIRE(supple::engine::detail::processBlock(*plan, 0, 128));
            const supple::engine::detail::Node* busNode = nullptr;
            const supple::engine::detail::Node* rackNode = nullptr;
            for (const auto& node : plan->nodes) {
                if (node.id == bus) busNode = &node;
                if (node.rack && node.rack->rack == rack) rackNode = &node;
            }
            REQUIRE(busNode);
            REQUIRE(rackNode);
            REQUIRE(rackNode->rackIns.size() == 1);
            REQUIRE(busNode->preFader.l[64] != 0);
            CHECK(busNode->preFader.l[64] != busNode->preFader.r[64]);
            const auto [panLeft, panRight] = supple::engine::detail::panSides(-.37, {});
            const double gain = console9::faderGain(-6 + (post ? -4.5 : 0));
            for (std::uint32_t i = 0; i < 128; ++i) {
                CHECK(rackNode->rackIns[0].l[i] ==
                      static_cast<float>(double(busNode->preFader.l[i]) * panLeft * gain));
                CHECK(rackNode->rackIns[0].r[i] ==
                      static_cast<float>(double(busNode->preFader.r[i]) * panRight * gain));
            }
        }
}

TEST_CASE("MIDI sends: channels without instruments forward exact timeline events in chains" *
          doctest::description("rt")) {
    bool compensated = false, rate = false, sourceLatency = false;
    SUBCASE("plain") {}
    SUBCASE("PDC and fan-out") { compensated = true; }
    SUBCASE("master playrate") { rate = true; }
    SUBCASE("post-FX source latency") { sourceLatency = true; }
    NoteRouteScene scene(false, compensated);
    // Compare the destinations alone: the generator's otherwise silent
    // Console 9 parent send has its own noise, shifted by the changed PDC graph.
    for (auto* session : {&scene.session, &scene.itemReference}) {
        model::EditTxn edit(*session, 2);
        edit.modify<model::Track>(scene.source,
                                  [](auto& track) { track.parentSend.enabled = false; });
        if (sourceLatency) {
            model::FxInstance fx;
            fx.plugin.id = "com.suppleaudio.test.gain";
            fx.patch =
                json::parse(
                    R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                    .value();
            const auto id = edit.create(fx);
            const auto track = session == &scene.session ? scene.source : scene.target;
            edit.modify<model::Track>(track, [&](auto& value) { value.chain.push_back(id); });
        }
        if (rate) {
            auto project = edit.project();
            project.playrate.rate = 1.5;
            edit.setProject(project);
        }
        *session = commit(edit);
    }
    model::EditTxn t(scene.session, 3);
    const auto relay = addChannel(t);
    t.modify<model::Track>(relay, [](auto& track) { track.parentSend.enabled = false; });
    for (const auto& entry : scene.session.routes) {
        if (!entry.value->midi) continue;
        t.modify<model::Route>(entry.id, [&](auto& route) {
            route.from = {};
            route.from.track = relay;
        });
    }
    model::Route route;
    route.from.track = scene.source;
    route.to.track = relay;
    route.midi = model::MidiRouting{};
    t.create(route);
    const auto session = commit(t);
    const auto reference = renderLeft(scene.itemReference, 2.0, 512);
    CHECK(bool(renderLeft(session, 2.0, 37) == reference));
    CHECK(bool(renderLeft(session, 2.0, 512) == reference));
    CHECK(bool(renderLeft(session, .2, 37, scene.target) ==
               renderLeft(scene.itemReference, .2, 37, scene.target)));
}

TEST_CASE("MIDI sends: route filter, mapping, mute automation and source mute respect taps" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    NoteRouteScene scene(false);
    model::EditTxn t(scene.session, 2);
    Id send;
    for (const auto& entry : scene.session.routes) {
        if (!entry.value->midi) continue;
        send = entry.id;
        t.modify<model::Route>(entry.id, [&](auto& r) {
            r.from = {};
            r.from.track = scene.source;
        });
    }
    bool muted = false;
    model::Tap tap = model::Tap::PostFader;
    model::MidiRouting routing{1, 7};
    SUBCASE("remap") {}
    SUBCASE("filter") { routing.fromChannel = 2; }
    SUBCASE("post-fader muted") { muted = true; }
    SUBCASE("pre-FX ignores mute") {
        muted = true;
        tap = model::Tap::PreFx;
    }
    SUBCASE("post-FX ignores mute") {
        muted = true;
        tap = model::Tap::PreFader;
    }
    t.modify<model::Track>(scene.source, [&](auto& tr) { tr.mute = muted; });
    t.modify<model::Route>(send, [&](auto& r) {
        r.tap = tap;
        r.midi = routing;
    });
    const auto session = commit(t);
    auto pool = makePool();
    auto plan = d::compile(session, pool, 0, 1024);
    REQUIRE(plan);
    REQUIRE(d::prepare(*plan, session));
    for (auto* instance : plan->instances) REQUIRE(instance->startProcessing());
    REQUIRE(d::processBlock(*plan, 0, 1024));
    const auto target = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                     [&](const auto& n) { return n.id == scene.target; });
    REQUIRE(target != plan->nodes.end());
    if (routing.fromChannel == 2 || (muted && tap == model::Tap::PostFader))
        CHECK(target->routedMidi.empty());
    else {
        REQUIRE_FALSE(target->routedMidi.empty());
        CHECK(target->routedMidi.front().frame == 425);  // tick 17 at 120 BPM, 48 kHz, PPQ 960
        CHECK((target->routedMidi.front().bytes[0] & 15) == 6);
    }
    for (auto* instance : plan->instances) instance->stopProcessing();
}

TEST_CASE("MIDI sends: Notes accepts direct sends and its source track's incoming sends" *
          doctest::description("rt")) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const auto source = addChannel(t), target = addChannel(t);
    const auto rack = t.create(model::Rack{target, ""});
    const auto notes = addModule(t, rack, model::kNotesModule);
    const auto clock = addModule(t, rack, "com.suppleaudio.rack.transport");
    const auto vca = addModule(t, rack, "com.suppleaudio.rack.vca");
    const auto out = addModule(t, rack, model::kOutModule);
    cable(t, clock, "playing", vca, "in");
    cable(t, notes, "gate", vca, "cv");
    cable(t, vca, "out", out, "l");
    listen(t, out, target);
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 1920;
    src.sequence.notes = {{19, 941, 0, 60, 100, 0}};
    model::Take take;
    take.source = t.create(src);
    const auto tk = t.create(take);
    model::Item item;
    item.track = source;
    item.length = 2;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    auto base = commit(t);
    model::EditTxn ref(base, 2);
    ref.modify<model::Item>({model::Kind::Item, 1}, [&](auto& it) { it.track = target; });
    const auto reference = renderLeft(commit(ref), 1.0, 37);
    model::EditTxn send(base, 3);
    model::Route route;
    route.from.track = source;
    route.midi = model::MidiRouting{};
    bool heard = true;
    SUBCASE("direct with source off") {
        send.modify<model::RackModule>(notes, [](auto& m) { m.sourceOff = true; });
        route.to.module = notes;
    }
    SUBCASE("through source track") { route.to.track = target; }
    SUBCASE("through source track, not heard") {  // a pre-2.10 project's Notes
        send.modify<model::RackModule>(notes, [](auto& m) { m.sourceSends = false; });
        route.to.track = target;
        heard = false;
    }
    SUBCASE("Note Out with source off") {
        send.modify<model::RackModule>(notes, [](auto& m) { m.sourceOff = true; });
        const auto generator = send.create(model::Rack{source, "Generator"});
        const auto input = addModule(send, generator, model::kNotesModule);
        const auto output = addModule(send, generator, model::kNotesOutModule);
        cable(send, input, "pitch", output, "pitch");
        cable(send, input, "gate", output, "gate");
        cable(send, input, "velocity", output, "velocity");
        route.from = {};
        route.from.module = output;
        route.to.module = notes;
    }
    send.create(route);
    const auto session = commit(send);
    const auto expected = heard ? reference : renderLeft(base, 1.0, 37);
    CHECK(heard != bool(expected == renderLeft(base, 1.0, 37)));
    CHECK(bool(renderLeft(session, 1.0, 37) == expected));
    CHECK(bool(renderLeft(session, 1.0, 512) == expected));
}

TEST_CASE("MIDI sends: fan-in addresses, held notes, route mute and source mute survive adoption" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    NoteRouteScene scene(false);
    model::EditTxn t(scene.session, 2);
    Id send;
    for (const auto& entry : scene.session.routes) {
        if (!entry.value->midi) continue;
        send = entry.id;
        t.modify<model::Route>(entry.id, [&](auto& r) {
            r.from = {};
            r.from.track = scene.source;
        });
    }
    model::Route second;
    second.from.track = scene.source;
    second.to.track = scene.target;
    second.midi = model::MidiRouting{};
    t.create(second);
    auto session = commit(t);
    auto pool = makePool();
    auto compiled = d::compile(session, pool, 0, 1024);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    REQUIRE(d::prepare(plan, session));
    std::vector<host::Instance*> started;
    auto start = [&](d::Plan& p) {
        for (auto* instance : p.instances)
            if (!instance->processing()) {
                REQUIRE(instance->startProcessing());
                started.push_back(instance);
            }
    };
    auto stage = [&](d::Plan& p) -> d::Stage& {
        const auto n = std::find_if(p.nodes.begin(), p.nodes.end(),
                                    [&](const auto& node) { return node.id == scene.target; });
        REQUIRE(n != p.nodes.end());
        REQUIRE(n->noteTarget >= 0);
        return n->chain[static_cast<std::size_t>(n->noteTarget)];
    };
    start(plan);
    REQUIRE(d::processBlock(plan, 0, 1024));
    REQUIRE(stage(plan).voices.size() == 2);
    CHECK(stage(plan).voices[0].noteId != stage(plan).voices[1].noteId);
    const auto held = stage(plan).voices.front().noteId;
    std::int64_t pos = 1024;
    auto adopt = [&](model::EditTxn& edit) {
        session = commit(edit);
        auto next = d::compile(session, pool, 0, 1024);
        REQUIRE(next);
        REQUIRE(d::prepare(*next, session));
        start(*next);
        d::carryMix(plan, *next, -1);
        d::locate(*next, pos, false);
        plan = std::move(*next);
        REQUIRE(d::processBlock(plan, pos, 37));
        pos += 37;
    };
    model::EditTxn rename(session, 3);
    rename.modify<model::Track>(scene.source, [](auto& tr) { tr.name = "Renamed"; });
    adopt(rename);
    REQUIRE(stage(plan).voices.size() == 2);
    CHECK(stage(plan).voices.front().noteId == held);
    // The sine fixture deliberately ends all equal channel/key voices on an
    // off. Check the engine's independent pre-FX gates after that point;
    // initial delivery above verifies the instrument got distinct note ids.
    const auto streamHeld = [&] {
        const auto target = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                         [&](const auto& n) { return n.id == scene.target; });
        REQUIRE(target != plan.nodes.end());
        return std::count_if(target->midiHeld.begin(), target->midiHeld.end(),
                             [](const auto& v) { return v.high; });
    };
    model::EditTxn mute(session, 4);
    mute.modify<model::Route>(send, [](auto& route) { route.mute = true; });
    adopt(mute);
    CHECK(streamHeld() == 1);
    model::EditTxn unmute(session, 5);
    unmute.modify<model::Route>(send, [](auto& route) { route.mute = false; });
    adopt(unmute);
    CHECK(streamHeld() == 2);
    model::EditTxn sourceMute(session, 6);
    sourceMute.modify<model::Track>(scene.source, [](auto& tr) { tr.mute = true; });
    adopt(sourceMute);
    CHECK(streamHeld() == 0);
    model::EditTxn early(session, 7);
    early.modify<model::Route>(send, [](auto& route) { route.tap = model::Tap::PreFx; });
    adopt(early);
    CHECK(streamHeld() == 1);
    for (auto* instance : started)
        if (instance->processing()) instance->stopProcessing();
}

TEST_CASE("MIDI sends: adding and removing another relay input preserves existing held notes" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    NoteRouteScene scene(false);
    model::EditTxn t(scene.session, 2);
    const auto relay = addChannel(t), spare = addChannel(t);
    t.modify<model::Track>(relay, [](auto& track) { track.parentSend.enabled = false; });
    t.modify<model::Track>(spare, [](auto& track) { track.parentSend.enabled = false; });
    auto midi = *scene.session.midi.begin()->value;
    for (auto& note : midi.sequence.notes) note.pitch = static_cast<std::uint8_t>(note.pitch + 7);
    model::Take take;
    take.source = t.create(midi);
    const auto tk = t.create(take);
    model::Item item;
    item.track = spare;
    item.length = 4;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    for (const auto& entry : scene.session.routes)
        if (entry.value->midi)
            t.modify<model::Route>(entry.id, [&](auto& route) {
                route.from = {};
                route.from.track = relay;
            });
    model::Route send;
    send.from.track = spare;
    send.to.track = relay;
    send.midi = model::MidiRouting{};
    t.create(send);
    auto session = commit(t);
    auto pool = makePool();
    auto compiled = d::compile(session, pool, 0, 1024);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    REQUIRE(d::prepare(plan, session));
    for (auto* instance : plan.instances) REQUIRE(instance->startProcessing());
    const auto stage = [&](d::Plan& p) -> d::Stage& {
        const auto node = std::find_if(p.nodes.begin(), p.nodes.end(),
                                       [&](const auto& n) { return n.id == scene.target; });
        REQUIRE(node != p.nodes.end());
        return node->chain[static_cast<std::size_t>(node->noteTarget)];
    };
    REQUIRE(d::processBlock(plan, 0, 1024));
    REQUIRE(stage(plan).voices.size() == 1);
    const auto address = stage(plan).voices.front().noteId;
    const auto adopt = [&](model::EditTxn& edit, std::int64_t pos) {
        session = commit(edit);
        auto next = d::compile(session, pool, 0, 1024);
        REQUIRE(next);
        REQUIRE(d::prepare(*next, session));
        d::carryMix(plan, *next, -1);
        d::locate(*next, pos, false);
        plan = std::move(*next);
        REQUIRE(d::processBlock(plan, pos, 37));
    };
    model::EditTxn add(session, 3);
    send.from.track = scene.source;
    const auto added = add.create(send);
    adopt(add, 1024);
    REQUIRE(stage(plan).voices.size() == 2);
    const auto held = std::find_if(stage(plan).voices.begin(), stage(plan).voices.end(),
                                   [](const auto& voice) { return voice.key == 67; });
    REQUIRE(held != stage(plan).voices.end());
    CHECK(held->noteId == address);
    model::EditTxn remove(session, 4);
    remove.remove<model::Route>(added);
    adopt(remove, 1061);
    REQUIRE(stage(plan).voices.size() == 1);
    CHECK(stage(plan).voices.front().noteId == address);
    for (auto* instance : plan.instances) instance->stopProcessing();
}

TEST_CASE("MIDI sends: an edit to an upstream send releases and retriggers downstream" *
          doctest::description("rt")) {
    // Source -> relay -> target (an instrument). Changing the first send
    // mid-note changes the note downstream too: one release, one onset,
    // never a second voice for the same note.
    namespace d = supple::engine::detail;
    std::string edit;
    SUBCASE("from channel") { edit = "from"; }
    SUBCASE("to channel") { edit = "to"; }
    SUBCASE("tap") { edit = "tap"; }
    SUBCASE("unrelated") { edit = "rename"; }
    NoteRouteScene scene(false);
    model::EditTxn t(scene.session, 2);
    const auto relay = addChannel(t);
    t.modify<model::Track>(relay, [](auto& track) { track.parentSend.enabled = false; });
    for (const auto& entry : scene.session.routes)
        if (entry.value->midi)
            t.modify<model::Route>(entry.id, [&](auto& route) {
                route.from = {};
                route.from.track = relay;
            });
    model::Route first;
    first.from.track = scene.source;
    first.to.track = relay;
    first.midi = model::MidiRouting{};
    const auto send = t.create(first);
    auto session = commit(t);
    auto pool = makePool();
    auto compiled = d::compile(session, pool, 0, 1024);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    REQUIRE(d::prepare(plan, session));
    for (auto* instance : plan.instances) REQUIRE(instance->startProcessing());
    const auto stage = [&]() -> d::Stage& {
        const auto node = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                       [&](const auto& n) { return n.id == scene.target; });
        REQUIRE(node != plan.nodes.end());
        return node->chain[static_cast<std::size_t>(node->noteTarget)];
    };
    std::int64_t pos = 0;
    const auto run = [&](std::int64_t until) {
        while (pos < until) {
            const auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(256, until - pos));
            REQUIRE(d::processBlock(plan, pos, n));
            pos += n;
            CHECK(stage().voices.size() <= 1);
        }
    };
    run(1024);  // the first note (60) sounds from frame 425 to 7000
    REQUIRE(stage().voices.size() == 1);
    const auto before = stage().voices.front().noteId;
    model::EditTxn change(session, 3);
    if (edit == "from")
        change.modify<model::Route>(send, [](auto& r) { r.midi = model::MidiRouting{1, 0}; });
    if (edit == "to")
        change.modify<model::Route>(send, [](auto& r) { r.midi = model::MidiRouting{0, 1}; });
    if (edit == "tap")
        change.modify<model::Route>(send, [](auto& r) { r.tap = model::Tap::PreFx; });
    if (edit == "rename") change.modify<model::Track>(relay, [](auto& r) { r.name = "Relay"; });
    session = commit(change);
    auto next = d::compile(session, pool, 0, 1024);
    REQUIRE(next);
    REQUIRE(d::prepare(*next, session));
    for (auto* instance : next->instances)
        if (!instance->processing()) REQUIRE(instance->startProcessing());
    d::carryMix(plan, *next, -1);
    d::locate(*next, pos, false);
    plan = std::move(*next);
    run(3000);
    REQUIRE(stage().voices.size() == 1);
    // An unrelated edit keeps the voice; a changed send replays it.
    CHECK((stage().voices.front().noteId == before) == (edit == "rename"));
    CHECK(stage().voices.front().key == 60);
    run(8192);  // past the note's end, before the next one
    CHECK(stage().voices.empty());
    for (const auto& node : plan.nodes)
        CHECK(std::none_of(node.midiHeld.begin(), node.midiHeld.end(),
                           [](const auto& v) { return v.high; }));
    for (auto* instance : plan.instances) instance->stopProcessing();
}

TEST_CASE("MIDI sends: a rack's Notes does not hear its own Note Out through its source track") {
    namespace d = supple::engine::detail;
    NoteRouteScene scene(false);
    model::EditTxn t(scene.session, 2);
    for (const auto& entry : scene.session.routes)
        if (entry.value->midi)
            t.modify<model::Route>(entry.id, [&](auto& route) { route.to.track = scene.source; });
    const auto session = commit(t);
    auto pool = makePool();
    auto plan = d::compile(session, pool, 0, 512);
    REQUIRE_MESSAGE(plan, (plan ? "" : plan.error().toString()));
    REQUIRE(d::prepare(*plan, session));
    std::size_t sends = 0;
    for (const auto& edge : plan->edges) {
        if (!edge.midi) continue;
        ++sends;
        CHECK(plan->nodes[edge.from].id == scene.rack);
        CHECK(plan->nodes[edge.to].id == scene.source);
    }
    CHECK(sends == 1);
}

namespace {
// A source with `count` item notes that never overlap, a MIDI send to an
// empty target channel; `mutePoints` of mute automation on the source.
model::Session manyNotes(int count, int mutePoints, Id& source, Id& target) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    source = addChannel(t);
    target = addChannel(t);
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 240 * count + 960;
    for (int i = 0; i < count; ++i)
        src.sequence.notes.push_back(
            {240 * i, 120, 0, static_cast<std::uint8_t>(36 + i % 48), 100, 0});
    model::Take take;
    take.source = t.create(src);
    const auto tk = t.create(take);
    model::Item item;
    item.track = source;
    item.length = count / 4.0 + 2;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    model::Route route;
    route.from.track = source;
    route.to.track = target;
    route.midi = model::MidiRouting{};
    t.create(route);
    if (mutePoints) {
        model::Envelope env;
        env.target = {source, "mute"};
        for (int i = 0; i < mutePoints; ++i) env.points.push_back({double(i), double(i % 2)});
        t.create(env);
    }
    return commit(t);
}
}  // namespace

TEST_CASE("MIDI sends: voice storage follows the notes held at once, not the notes" *
          doctest::description("rt")) {
    // Adoption walks the voice pools and send voices (carryRackMidi), and
    // every budget multiplies them: none may grow with the timeline.
    namespace d = supple::engine::detail;
    for (int count : {100, 10000}) {
        CAPTURE(count);
        Id source, target;
        const auto session = manyNotes(count, 200, source, target);
        auto pool = makePool();
        auto plan = d::compile(session, pool, 0, 512);
        REQUIRE(plan);
        const auto prepared = d::prepare(*plan, session);
        REQUIRE_MESSAGE(prepared, (prepared ? "" : prepared.error().toString()));
        const auto node = [&](Id id) -> d::Node& {
            const auto n = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                        [&](const auto& x) { return x.id == id; });
            REQUIRE(n != plan->nodes.end());
            return *n;
        };
        const auto& from = node(source);
        const auto& to = node(target);
        // One item note at a time, a margin for late releases.
        CHECK(from.midiHeld.size() <= 32);
        CHECK(to.midiHeld.size() <= 2 * from.midiHeld.size());
        for (const auto& edge : plan->edges)
            if (edge.midi) {
                CHECK(edge.midiVoices.size() == from.midiHeld.size());
                CHECK(edge.midiQueue.size() < 100000);
            }
        CHECK(to.midiPreFx.capacity() < 100000);
        // Adoption still finds a held note by its musical identity.
        REQUIRE(d::processBlock(*plan, 0, 512));
        auto next = d::compile(session, pool, 0, 512);
        REQUIRE(next);
        REQUIRE(d::prepare(*next, session));
        d::carryMix(*plan, *next, -1);
        const auto held = [](const d::Node& n) {
            return std::count_if(n.midiHeld.begin(), n.midiHeld.end(),
                                 [](const auto& v) { return v.high && !v.orphan; });
        };
        CHECK(held(from) == 1);  // the first note, ticks 0 to 120
        const auto carried = std::find_if(next->nodes.begin(), next->nodes.end(),
                                          [&](const auto& n) { return n.id == source; });
        REQUIRE(carried != next->nodes.end());
        CHECK(held(*carried) == 1);
    }
}

namespace {
// A track's MIDI stream alone: `pool` voice slots for item notes 0..inputs-1.
supple::engine::detail::Node midiStream(std::size_t pool, std::size_t inputs) {
    supple::engine::detail::Node node;
    node.midiHeld.assign(pool, {});
    for (std::size_t s = pool; s-- > 0;) node.midiFree.push_back(static_cast<std::int32_t>(s));
    node.midiFree.reserve(pool);
    node.midiInput.assign(inputs, -1);
    node.midiLiveBase = node.midiRoutedBase = static_cast<std::int32_t>(inputs);
    node.midiStage.blockEvents.reserve(16);
    node.midiSortScratch.reserve(16);
    node.midiOrder.prepare(16, 64);
    node.midiPreFx.reserve(16);
    node.midiPostFx.reserve(16);
    node.midiInstrument.reserve(16);
    return node;
}
host::Event noteEvent(host::Event::Kind kind, std::uint32_t time, int id, int key = 60) {
    host::Event e;
    e.kind = kind;
    e.time = time;
    e.noteId = id;
    e.key = static_cast<std::int16_t>(key);
    e.velocity = kind == host::Event::Kind::NoteOn ? 1 : 0;
    return e;
}
}  // namespace

TEST_CASE("MIDI sends: all-notes-off releases precede a same-frame reattack" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    using K = host::Event::Kind;
    for (int controller : {120, 123}) {
        auto node = midiStream(2, 1);
        node.midiStage.blockEvents = {noteEvent(K::NoteOn, 3, 0)};
        d::finishTrackMidi(node);
        REQUIRE(node.midiHeld[0].high);
        host::Event reset;
        reset.kind = K::Midi;
        reset.time = 7;
        reset.midi[0] = 0xb0;
        reset.midi[1] = static_cast<std::uint8_t>(controller);
        node.midiStage.blockEvents = {reset, noteEvent(K::NoteOn, 7, 0)};
        d::finishTrackMidi(node);
        REQUIRE(node.midiPreFx.size() == 3);
        CHECK(node.midiPreFx[0].kind == K::NoteOff);
        CHECK(node.midiPreFx[1].kind == K::Midi);
        CHECK(node.midiPreFx[2].kind == K::NoteOn);
        CHECK(node.midiPreFx[0].time == 7);
        CHECK(node.midiPreFx[2].time == 7);
        // The note target hears the same, under the item note's id.
        CHECK(node.midiInstrument[0] == 0);
        CHECK(node.midiInstrument[2] == 0);
        CHECK(std::count_if(node.midiHeld.begin(), node.midiHeld.end(),
                            [](const auto& v) { return v.high; }) == 1);
    }
}

TEST_CASE("MIDI sends: solo follows a MIDI send downstream only") {
    namespace d = supple::engine::detail;
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const auto source = addChannel(t), target = addChannel(t), other = addChannel(t);
    model::Route route;
    route.from.track = source;
    route.to.track = target;
    route.midi = model::MidiRouting{};
    t.create(route);
    const auto base = commit(t);
    const auto silenced = [&](Id soloed) {
        model::EditTxn edit(base, 2);
        edit.modify<model::Track>(soloed, [](auto& x) { x.solo = model::Solo::On; });
        const auto session = commit(edit);
        auto pool = makePool();
        auto plan = d::compile(session, pool, 0, 256);
        REQUIRE(plan);
        std::map<Id, bool> out;
        for (const auto& node : plan->nodes) out[node.id] = node.soloSilenced;
        for (Id id : {source, target, other})
            CHECK(model::effectiveTrackLevel(session, id, 0).soloSilenced == out[id]);
        return out;
    };
    auto soloTarget = silenced(target);  // the source's sound stays silenced
    CHECK(soloTarget[source]);
    CHECK_FALSE(soloTarget[target]);
    CHECK(soloTarget[other]);
    auto soloSource = silenced(source);  // what it plays stays audible
    CHECK_FALSE(soloSource[source]);
    CHECK_FALSE(soloSource[target]);
    CHECK(soloSource[other]);
}

TEST_CASE("MIDI sends: channel filter and remap leave system messages alone" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    using K = host::Event::Kind;
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    const auto source = addChannel(t), target = addChannel(t);
    model::Route route;
    route.from.track = source;
    route.to.track = target;
    route.midi = model::MidiRouting{2, 5};  // channel 2 only, onto channel 5
    t.create(route);
    const auto session = commit(t);
    auto pool = makePool();
    auto plan = d::compile(session, pool, 0, 64);
    REQUIRE(plan);
    REQUIRE(d::prepare(*plan, session));
    const auto node = [&](Id id) -> d::Node& {
        const auto n = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                    [&](const auto& x) { return x.id == id; });
        REQUIRE(n != plan->nodes.end());
        return *n;
    };
    const auto midi = [](std::uint32_t time, std::uint8_t status, std::uint8_t data) {
        host::Event e;
        e.kind = K::Midi;
        e.time = time;
        e.midi[0] = status;
        e.midi[1] = data;
        return e;
    };
    auto& from = node(source);
    from.midiPreFx = from.midiPostFx = {midi(3, 0xf8, 0), midi(4, 0xb1, 7), midi(5, 0xb0, 7)};
    auto& to = node(target);
    to.midiStage.blockEvents.clear();
    d::addRackMidi(*plan, to, 0, 64, true);
    const auto& got = to.midiStage.blockEvents;
    REQUIRE(got.size() == 2);
    CHECK(got[0].midi[0] == 0xf8);  // a clock has no channel to filter or remap
    CHECK(got[1].midi[0] == 0xb4);  // channel 2 moved to 5; channel 1 filtered out
}

TEST_CASE("MIDI sends: an onset on a held input ends the held note first" *
          doctest::description("rt")) {
    namespace d = supple::engine::detail;
    using K = host::Event::Kind;
    auto node = midiStream(4, 1);
    node.midiStage.blockEvents = {noteEvent(K::NoteOn, 1, 0), noteEvent(K::NoteOn, 5, 0, 62)};
    d::finishTrackMidi(node);
    REQUIRE(node.midiPreFx.size() == 3);
    CHECK(node.midiPreFx[0].kind == K::NoteOn);
    CHECK(node.midiPreFx[1].kind == K::NoteOff);
    CHECK(node.midiPreFx[1].key == 60);
    CHECK(node.midiPreFx[1].noteId == node.midiPreFx[0].noteId);
    CHECK(node.midiPreFx[2].kind == K::NoteOn);
    CHECK(node.midiPreFx[2].key == 62);
    CHECK(std::count_if(node.midiHeld.begin(), node.midiHeld.end(),
                        [](const auto& v) { return v.high; }) == 1);
    // A full pool drops the onset and counts it, and the note target does not hear it.
    auto full = midiStream(1, 2);
    full.midiStage.blockEvents = {noteEvent(K::NoteOn, 1, 0), noteEvent(K::NoteOn, 2, 1, 64)};
    d::finishTrackMidi(full);
    CHECK(full.midiPreFx.size() == 1);
    CHECK(full.midiDropped == 1);
}

TEST_CASE("MIDI sends: a relay chain's reservations grow by each hop, not twice" *
          doctest::description("rt")) {
    // Chords of 32 notes every eighth of a beat, relayed through six
    // channels without instruments. Each hop's buffers once took twice the
    // previous hop's; now each adds its own voices.
    namespace d = supple::engine::detail;
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    std::vector<Id> chain{addChannel(t)};
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 960 * 8;
    for (int tick = 0; tick < 960 * 8; tick += 120)
        for (int k = 0; k < 32; ++k)
            src.sequence.notes.push_back({tick, 60, 0, static_cast<std::uint8_t>(40 + k), 100, 0});
    model::Take take;
    take.source = t.create(src);
    const auto tk = t.create(take);
    model::Item item;
    item.track = chain.front();
    item.length = 8;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    for (int hop = 0; hop < 6; ++hop) {
        chain.push_back(addChannel(t));
        model::Route route;
        route.from.track = chain[chain.size() - 2];
        route.to.track = chain.back();
        route.midi = model::MidiRouting{};
        t.create(route);
    }
    const auto session = commit(t);
    auto pool = makePool();
    auto plan = d::compile(session, pool, 0, 512);
    REQUIRE(plan);
    const auto prepared = d::prepare(*plan, session);
    REQUIRE_MESSAGE(prepared, (prepared ? "" : prepared.error().toString()));
    const auto node = [&](Id id) -> d::Node& {
        const auto n = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                                    [&](const auto& x) { return x.id == id; });
        REQUIRE(n != plan->nodes.end());
        return *n;
    };
    std::vector<std::size_t> capacity, queue;
    for (Id id : chain) capacity.push_back(node(id).midiPreFx.capacity());
    for (std::size_t hop = 1; hop < chain.size(); ++hop)
        for (const auto& edge : plan->edges)
            if (edge.midi && plan->nodes[edge.to].id == chain[hop])
                queue.push_back(edge.midiQueue.size());
    REQUIRE(queue.size() == 6);
    // The sixth hop adds a little more than the first did (each pool holds
    // its source's voices and a margin); doubling would add 32 times more.
    CAPTURE(capacity);
    CAPTURE(queue);
    CHECK(capacity[6] - capacity[5] < 3 * (capacity[1] - capacity[0]));
    CHECK(queue[5] - queue[4] < 3 * (queue[1] - queue[0]));
    CHECK(capacity[6] < 2000);  // 1767; doubling per hop made it 18063
    // And nothing is dropped: the last hop carries every source event.
    std::size_t sent = 0, received = 0;
    for (std::int64_t pos = 0; pos < 48000 * 4; pos += 512) {
        REQUIRE(d::processBlock(*plan, pos, 512));
        sent += node(chain.front()).midiPreFx.size();
        received += node(chain.back()).midiPreFx.size();
    }
    CHECK(sent > 1000);
    CHECK(received == sent);
    for (Id id : chain) CHECK(node(id).midiDropped == 0);
}
