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
    // Notes and a knob envelope on the timeline, Console 9 on.
    model::Project project;
    project.seed = 9182736;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t);
    const Id rack = t.create(model::Rack{channel, ""});
    const Id notes = addModule(t, rack, "com.suppleaudio.rack.notes");
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id vca = addModule(t, rack, "com.suppleaudio.rack.vca");
    const Id out = addModule(t, rack, "com.suppleaudio.rack.out");
    cable(t, transport, "beat_phase", vca, "in");
    cable(t, notes, "gate", vca, "cv");
    cable(t, vca, "out", out, "l");
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
