// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes in the plan (ADR-0096, engine.md §1.4.1): every route is
// an edge that orders its source rack first, runs source and target in one
// executor, and is compensated as a sidechain into its target stage.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>

#include "bridge_patience.h"
#include "plan.h"
#include "plan_dump.h"
#include "rack_runtime.h"
#include "realtime_common.h"
#include "supple/console9/console9.h"
#include "supple/model/freeze.h"
#include "supple/model/schemas.h"
#include "supple/params/params.h"

namespace {
namespace d = supple::engine::detail;

constexpr const char* kGain = "com.suppleaudio.test.gain";

Id addChannel(model::EditTxn& t) {
    model::Track tr;
    tr.parent = kMaster;
    const Id id = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(id); });
    return id;
}

Id addModule(model::EditTxn& t, Id rack, std::string_view type) {
    model::RackModule m;
    m.rack = rack;
    m.type = std::string(type);
    m.version = "1.0.0";
    return t.create(m);
}

Id addFx(model::EditTxn& t, Id track, bool latency = false) {
    model::FxInstance fx;
    fx.plugin.id = kGain;
    if (latency)
        fx.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                .value();
    const Id id = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& tr) { tr.chain.push_back(id); });
    return id;
}

void cable(model::EditTxn& t, Id from, std::string fromKey, Id to, std::string toKey) {
    t.create(model::Cable{{from, std::move(fromKey)}, {to, std::move(toKey)}, std::nullopt, ""});
}

Id modulate(model::EditTxn& t, Id modOut, Id target, std::string param, double depth = 0.5) {
    model::Modulation m;
    m.source = {modOut, "in_1"};
    m.target = {target, std::move(param)};
    m.depth = depth;
    return t.create(m);
}

model::Session commit(model::EditTxn& t) {
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    return std::move(*s);
}

// Channels A and B; B has a latent gain then a plain one. A rack on A
// (created last, so its id sorts after both) runs Transport into a Mod Out,
// and its Out is A's input, which fixes its timing.
struct Patch {
    model::Session s;
    Id b, a, rack, modOut, out, latent, plain;
    Patch() {
        model::Project project;
        project.console9 = false;
        model::EditTxn t(model::Session::create(project), 1);
        b = addChannel(t);
        a = addChannel(t);
        latent = addFx(t, b, true);
        plain = addFx(t, b);
        rack = t.create(model::Rack{a, ""});
        const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
        modOut = addModule(t, rack, model::kModOutModule);
        out = addModule(t, rack, model::kOutModule);
        cable(t, transport, "beat_phase", modOut, "in_1");
        cable(t, transport, "bar_phase", out, "l");
        model::Route listen;
        listen.from.module = out;
        listen.to.track = a;
        const Id route = t.create(listen);
        t.modify<model::Track>(a, [&](model::Track& tr) { tr.record.input = route; });
        s = commit(t);
    }
};

struct Prepared {
    InstancePool pool = makePool();
    MediaCache media;
    d::Plan plan;
    explicit Prepared(const model::Session& s) {
        auto compiled = d::compile(s, pool, 0, 128);
        REQUIRE_MESSAGE(compiled, (compiled ? "" : compiled.error().toString()));
        plan = std::move(*compiled);
        plan.media = &media;
        auto ok = d::prepare(plan, s);
        REQUIRE_MESSAGE(ok, (ok ? "" : ok.error().toString()));
    }
    std::size_t node(Id id) const {
        for (std::size_t i = 0; i < plan.nodes.size(); ++i)
            if (plan.nodes[i].id == id) return i;
        FAIL("no node " << id.str());
        return 0;
    }
};

// The value the rack computed for timeline sample t reaches the target as
// the target plays t: the edge's delay closes the gap between the two nodes'
// timeline shifts, its tap and the target stage's position.
std::int64_t before(const d::Plan& p, const d::ModEdge& me) {
    const d::Node& n = p.nodes[me.to];
    return me.stage >= 0 ? n.chain[static_cast<std::size_t>(me.stage)].before
           : me.inputStage >= 0 || me.target.object.kind == model::Kind::Fx
               ? 0
               : n.latChain - std::int64_t{d::kAutomationStep};
}

}  // namespace

TEST_CASE("modulation: the source rack runs before its target") {
    Patch patch;
    {
        Prepared without(patch.s);
        // Racks sort after tracks when nothing orders them.
        CHECK(without.node(patch.rack) > without.node(patch.b));
        CHECK(without.plan.modEdges.empty());
    }
    model::EditTxn t(patch.s, 2);
    const Id route = modulate(t, patch.modOut, patch.plain, "gain");
    Prepared with(commit(t));
    CHECK(with.node(patch.rack) < with.node(patch.b));
    REQUIRE(with.plan.modEdges.size() == 1);
    const d::ModEdge& me = with.plan.modEdges[0];
    CHECK(me.route == route);
    CHECK(me.from == with.node(patch.rack));
    CHECK(me.to == with.node(patch.b));
    CHECK(me.modOut == 0);
    CHECK(me.input == 0);
    CHECK(me.stage == 1);
    CHECK_FALSE(me.inert);
    const auto& in = with.plan.nodes[me.to].modIn;
    CHECK(in == std::vector<std::size_t>{0});
    // The task graph waits for the rack too.
    const auto& rack = *with.plan.nodes[me.from].rack;
    REQUIRE(rack.modValues.size() == static_cast<std::size_t>(rack::host::kModOutInputs));
    CHECK(rack.modValues[0].size() == with.plan.block);  // in_1 is read
    CHECK(rack.modValues[1].empty());                    // nothing reads in_2
}

TEST_CASE("modulation: a disabled route is still an edge, a mixer target binds to its node") {
    Patch patch;
    model::EditTxn t(patch.s, 2);
    const Id off = modulate(t, patch.modOut, patch.plain, "gain");
    t.modify<model::Modulation>(off, [](model::Modulation& m) { m.enabled = false; });
    const Id fader = modulate(t, patch.modOut, patch.b, "volume", 3.0);
    Prepared p(commit(t));
    REQUIRE(p.plan.modEdges.size() == 2);
    CHECK(p.plan.modEdges[0].route == off);
    CHECK_FALSE(p.plan.modEdges[0].enabled);
    CHECK_FALSE(p.plan.modEdges[0].inert);
    CHECK(p.plan.modEdges[1].route == fader);
    CHECK_FALSE(p.plan.modEdges[1].inert);
    const d::Node& b = p.plan.nodes[p.node(patch.b)];
    REQUIRE(b.modVolume == 0);
    CHECK(b.modPan == -1);
    CHECK(b.mixMods.at(0).routes == std::vector<std::uint32_t>{1});
    CHECK(b.mixMods.at(0).offsets.size() == 128);
    CHECK(p.node(patch.rack) < p.node(patch.b));
}

TEST_CASE("modulation: the edge is compensated as a sidechain into its target stage") {
    Patch patch;
    model::EditTxn t(patch.s, 2);
    modulate(t, patch.modOut, patch.latent, "gain");
    modulate(t, patch.modOut, patch.plain, "gain");
    modulate(t, patch.modOut, patch.b, "pan", 10.0);
    Prepared p(commit(t));
    REQUIRE(p.plan.modEdges.size() == 3);
    const d::Node& b = p.plan.nodes[p.node(patch.b)];
    const std::int64_t latency = b.chain[1].before;
    REQUIRE(latency > 0);  // the latent gain sits ahead of the plain one
    for (const d::ModEdge& me : p.plan.modEdges) {
        CAPTURE(me.route.str());
        const d::Node& from = p.plan.nodes[me.from];
        const d::Node& to = p.plan.nodes[me.to];
        CHECK(me.delay.length >= 0);
        CHECK(me.delay.length ==
              to.timelineShift + before(p.plan, me) - from.timelineShift - me.tap);
    }
    // The rack's Out fixes its timing beside B's, so the edge into the
    // plain gain waits out the latent one.
    CHECK(p.plan.modEdges[1].delay.length - p.plan.modEdges[0].delay.length == latency);
}

TEST_CASE("modulation: a live target pulls its source live, and a live source its target") {
    Patch patch;
    SUBCASE("an armed, monitored target") {
        model::EditTxn t(patch.s, 2);
        modulate(t, patch.modOut, patch.plain, "gain");
        model::Route input;
        input.from.hwChannels = std::vector<int>{1};
        input.to.track = patch.b;
        const Id route = t.create(input);
        t.modify<model::Track>(patch.b, [&](model::Track& tr) {
            tr.record.input = route;
            tr.record.armed = true;
            tr.record.monitor = model::Monitor::On;
        });
        Prepared p(commit(t));
        CHECK(p.plan.nodes[p.node(patch.b)].live);
        CHECK(p.plan.nodes[p.node(patch.rack)].live);
        CHECK(p.plan.nodes[p.node(patch.a)].live);  // what the rack feeds
        CHECK(std::any_of(p.plan.warnings.begin(), p.plan.warnings.end(), [](const std::string& w) {
            return w.find("modulation.live") != std::string::npos;
        }));
    }
    SUBCASE("a rack playing a MIDI device") {
        model::EditTxn t(patch.s, 2);
        modulate(t, patch.modOut, patch.plain, "gain");
        const Id notes = addModule(t, patch.rack, model::kNotesModule);
        t.modify<model::RackModule>(notes, [](model::RackModule& m) { m.device = ""; });
        Prepared p(commit(t));
        CHECK(p.plan.nodes[p.node(patch.rack)].live);
        CHECK(p.plan.nodes[p.node(patch.b)].live);
    }
    SUBCASE("nothing live") {
        model::EditTxn t(patch.s, 2);
        modulate(t, patch.modOut, patch.plain, "gain");
        Prepared p(commit(t));
        CHECK_FALSE(p.plan.nodes[p.node(patch.rack)].live);
        CHECK_FALSE(p.plan.nodes[p.node(patch.b)].live);
    }
}

TEST_CASE("modulation: a Mod Out with a route sets its rack's latency") {
    model::Project project;
    project.console9 = false;
    model::EditTxn t(model::Session::create(project), 1);
    const Id channel = addChannel(t), target = addChannel(t);
    const Id plain = addFx(t, target);
    const Id rack = t.create(model::Rack{channel, ""});
    model::FxInstance fx;
    fx.plugin.id = kGain;
    fx.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value();
    model::RackModule plugin;
    plugin.rack = rack;
    plugin.type = std::string(model::kPluginModule);
    plugin.version = "1.0.0";
    plugin.fx = t.create(fx);
    const Id effect = t.create(plugin);
    const Id follower = addModule(t, rack, "com.suppleaudio.rack.follower");
    const Id modOut = addModule(t, rack, model::kModOutModule);
    cable(t, effect, "out_l", follower, "in");
    cable(t, follower, "envelope", modOut, "in_1");
    const auto unrouted = commit(t);
    {
        Prepared p(unrouted);
        CHECK(p.plan.nodes[p.node(rack)].latChain == 0);  // no way out yet
    }
    model::EditTxn routed(unrouted, 2);
    modulate(routed, modOut, plain, "gain");
    Prepared p(commit(routed));
    const d::Node& node = p.plan.nodes[p.node(rack)];
    CHECK(node.latChain == 64);
    REQUIRE(node.rack->modOutLatencies.size() == 1);
    CHECK(node.rack->modOutLatencies[0] == 64);
    CHECK(p.plan.modEdges[0].tap == 64);
}

TEST_CASE("modulation: the plan dump lists the edges, the same for the same session") {
    Patch patch;
    model::EditTxn t(patch.s, 2);
    modulate(t, patch.modOut, patch.plain, "gain");
    const auto s = commit(t);
    Prepared one(s), two(s);
    const std::string dump = d::dumpPlan(one.plan);
    CHECK(dump == d::dumpPlan(two.plan));
    CHECK(dump.find("plan modEdges 1") != std::string::npos);
    CHECK(dump.find("m0 route mr_1") != std::string::npos);
    CHECK(dump.find("modIn [0]") != std::string::npos);
}

namespace {

// Channel B hears a rack's Out: Transport's `playing`, 1 while playing,
// through the gain fixture at -6 dB in B's chain. The rack's Mod Out takes
// `source` (an output of Transport) on in_1; `depth` routes it into the
// gain (CLAP param_mod, its CLAP range 0..1). A linear console: levels are
// exact.
struct Emit {
    model::Session s;
    Id a, b, rack, modOut, gain, route;
    Emit(const char* source, std::optional<double> depth, bool enabled = true) {
        model::Project project;
        project.console9 = false;
        model::EditTxn t(model::Session::create(project), 1);
        a = addChannel(t);
        b = addChannel(t);
        model::FxInstance fx;
        fx.plugin.id = kGain;
        fx.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}})").value();
        gain = t.create(fx);
        t.modify<model::Track>(b, [&](model::Track& tr) { tr.chain.push_back(gain); });
        rack = t.create(model::Rack{a, ""});
        const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
        modOut = addModule(t, rack, model::kModOutModule);
        const Id out = addModule(t, rack, model::kOutModule);
        cable(t, transport, "playing", out, "l");
        cable(t, transport, source, modOut, "in_1");
        model::Route listen;
        listen.from.module = out;
        listen.to.track = b;
        const Id heard = t.create(listen);
        t.modify<model::Track>(b, [&](model::Track& tr) { tr.record.input = heard; });
        if (depth) {
            route = modulate(t, modOut, gain, "gain", *depth);
            if (!enabled)
                t.modify<model::Modulation>(route, [](model::Modulation& m) { m.enabled = false; });
        }
        s = commit(t);
    }
};

struct Render {
    std::vector<float> left;
    std::vector<std::string> warnings;
};
Render render(const model::Session& s, double seconds, std::uint32_t block = 512) {
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = seconds;
    settings.blockSize = block;
    auto r = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return {r->audio.channels[0], r->warnings};
}

// The fixture's gain: -60..12 dB, its CLAP value normalized (ADR-0010).
double gainAt(double clapOffset) {
    params::ParamSpec spec;
    spec.type = params::Type::Float;
    spec.unit = units::Unit::dB;
    spec.min = -60;
    spec.max = 12;
    const double db =
        params::fromClap(spec, std::clamp(params::toClap(spec, -6) + clapOffset, 0.0, 1.0));
    return std::pow(10.0, db / 20.0);
}

bool warns(const std::vector<std::string>& warnings, const std::string& what) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [&](const std::string& w) { return w.find(what) != std::string::npos; });
}

}  // namespace

TEST_CASE("modulation: a route moves its plug-in parameter by signal x depth x range" *
          doctest::description("rt")) {
    const auto plain = render(Emit("playing", std::nullopt).s, 0.5);
    CHECK(double(plain.left[12000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
    const auto up = render(Emit("playing", 0.1).s, 0.5);
    CHECK(double(up.left[12000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
    const auto down = render(Emit("playing", -0.25).s, 0.5);
    CHECK(double(down.left[12000]) == doctest::Approx(gainAt(-0.25)).epsilon(1e-4));
    // A disabled route adds 0.
    const auto off = render(Emit("playing", 0.1, false).s, 0.5);
    CHECK(off.left == plain.left);
}

TEST_CASE("modulation: the events fall on the stage's grid whatever the block size" *
          doctest::description("rt")) {
    const Emit emit("beat_phase", 0.2);
    const auto at512 = render(emit.s, 1.0, 512);
    CHECK(at512.left == render(emit.s, 1.0, 37).left);
    CHECK(at512.left == render(emit.s, 1.0, 128).left);
    // The level follows the beat phase: it changes, and only at grid points
    // of the absolute 32-frame grid.
    std::size_t changes = 0;
    for (std::size_t i = 1; i < at512.left.size(); ++i)
        if (at512.left[i] != at512.left[i - 1]) {
            ++changes;
            CHECK(i % d::kAutomationStep == 0);
        }
    CHECK(changes > 10);
}

TEST_CASE("modulation: a target that cannot take it is inert, with a warning") {
    Emit emit("playing", std::nullopt);
    model::EditTxn t(emit.s, 2);
    const Id route = modulate(t, emit.modOut, emit.gain, "latency", 0.5);
    const auto r = render(commit(t), 0.5);
    CHECK(warns(r.warnings, route.str()));
    CHECK(warns(r.warnings, "modulation.target"));
    CHECK(r.left == render(emit.s, 0.5).left);
}

TEST_CASE("modulation: a running engine carries the memo, and a route removed sends its 0") {
    const Emit on("playing", 0.1), half("playing", 0.05), none("playing", std::nullopt);
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    REQUIRE(engine.load(on.s));
    engine.play();
    backend.run(100);  // 9600 frames
    REQUIRE(engine.load(half.s));
    backend.run(100);
    REQUIRE(engine.load(none.s));
    backend.run(100);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto& cap = backend.capture();
    CHECK(double(cap.channels[0][9000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
    CHECK(double(cap.channels[0][19000]) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
    // No plan modulates the gain now: the memo's 0 reached it.
    CHECK(double(cap.channels[0][28000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
}

TEST_CASE("modulation: binding gives each stage its slots and their events") {
    Patch patch;
    model::EditTxn t(patch.s, 2);
    const Id input = addFx(t, patch.b);
    t.modify<model::Track>(patch.b, [&](model::Track& tr) {
        std::erase(tr.chain, input);
        tr.inputChain.push_back(input);
    });
    modulate(t, patch.modOut, patch.plain, "gain");
    const Id second = modulate(t, patch.modOut, patch.plain, "gain", -0.25);
    t.modify<model::Modulation>(second, [](model::Modulation& m) { m.source.input = "in_2"; });
    modulate(t, patch.modOut, input, "gain");
    Prepared p(commit(t));
    const d::Node& b = p.plan.nodes[p.node(patch.b)];
    const d::Stage& plain = b.chain[1];
    REQUIRE(plain.mods.size() == 1);  // two routes, one parameter
    CHECK(plain.mods[0].clapId == 0);
    CHECK(plain.mods[0].range == 1.0);
    CHECK(plain.mods[0].count == 2);
    CHECK(plain.modRoutes.size() == 2);
    const std::size_t perParam = p.plan.block / d::kAutomationStep + 2;
    CHECK(plain.modEvents == perParam);
    CHECK(plain.blockEvents.capacity() >= plain.modEvents);
    CHECK(b.chain[0].mods.empty());  // the latent gain is not modulated
    // The input chain's plug-in, which hears no notes, still has room.
    REQUIRE(b.inputChain.size() == 1);
    CHECK(b.inputChain[0].mods.size() == 1);
    CHECK(b.inputChain[0].blockEvents.capacity() >= perParam);
    for (const d::ModEdge& me : p.plan.modEdges) {
        CHECK_FALSE(me.inert);
        CHECK(me.values.size() == p.plan.block);
    }
}

TEST_CASE("modulation: an input-chain plug-in has room for its automation and modulation" *
          doctest::description("rt")) {
    // B's input is the rack's Out, armed, so its input chain runs every
    // block. Its gain is automated to -20 dB; then modulated as well.
    Emit emit("playing", std::nullopt);
    model::EditTxn t(emit.s, 2);
    t.modify<model::Track>(emit.b, [&](model::Track& tr) {
        tr.chain.clear();
        tr.inputChain.push_back(emit.gain);
        tr.record.armed = true;
    });
    model::Envelope env;
    env.target = {emit.gain, "gain"};
    env.points = {{0.0, -20.0}};
    t.create(env);
    const auto automated = commit(t);
    const auto a = render(automated, 0.5);
    CHECK(double(a.left[12000]) == doctest::Approx(std::pow(10.0, -20.0 / 20.0)).epsilon(1e-4));
    model::EditTxn more(automated, 3);
    modulate(more, emit.modOut, emit.gain, "gain", 0.1);
    const auto m = render(commit(more), 0.5);
    CHECK(double(m.left[12000]) > double(a.left[12000]) * 1.5);
}

TEST_CASE("modulation: a target whose latency moves while modulated turns its routes off") {
    // ADR-0096, amendment M-d: the plug-in reports latency only after it has
    // changed, so the engine turns the routes into that FX off until one of
    // them is edited. Here a patch edit moves the gain's latency.
    const Emit on("playing", 0.1);
    model::EditTxn t(on.s, 2);
    t.modify<model::FxInstance>(on.gain, [](model::FxInstance& fx) {
        fx.patch =
            json::parse(
                R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6,"latency":"short"}})")
                .value();
    });
    const auto latent = commit(t);
    model::EditTxn u(latent, 3);  // an unrelated edit that compiles
    addChannel(u);
    const auto unrelated = commit(u);
    model::EditTxn e(unrelated, 4);
    e.modify<model::Modulation>(on.route, [](model::Modulation& m) { m.depth = 0.05; });
    const auto edited = commit(e);

    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, {.anticipative = false, .workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    const auto settle = [&](int blocks) {
        for (int i = 0; i < blocks; i += 10) {
            engine.service();
            backend.run(10);
        }
    };
    REQUIRE(engine.load(on.s));
    engine.play();
    settle(100);  // to 9600
    CHECK_FALSE(pool.modulationDisabled(on.gain));
    REQUIRE(engine.load(latent));
    settle(100);  // to 19200: the restart, the latency, the reload
    CHECK(pool.modulationDisabled(on.gain));
    CHECK(warns(engine.warnings(), "modulation.latency"));  // the latest plan's
    REQUIRE(engine.load(unrelated));
    settle(20);
    CHECK(pool.modulationDisabled(on.gain));  // only a route edit ends it
    REQUIRE(engine.load(edited));
    settle(100);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK_FALSE(pool.modulationDisabled(on.gain));
    const auto& cap = backend.capture();
    CHECK(double(cap.channels[0][9000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
    CHECK(double(cap.channels[0][19000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
    CHECK(double(cap.channels[0][21000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
    CHECK(double(cap.channels[0][30500]) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
    CHECK_FALSE(warns(engine.warnings(), "modulation.latency"));
}

TEST_CASE(
    "modulation: a route into a frozen track's FX is inert, and the plan has no chain to read") {
    // Review round 54's probe: the frozen node's plan chain is empty, so the
    // route must not index it (the latency solve, the binding).
    Patch patch;
    model::EditTxn t(patch.s, 2);
    const Id route = modulate(t, patch.modOut, patch.plain, "gain");
    const Id media = t.create(
        model::MediaSource{"frozen.wav", "sha256:" + std::string(64, '2'), "wav", 48000, 2, 48000});
    auto modulated = t.preview();
    const model::TrackFreeze cache{media, 48000, 0, 0,
                                   model::freezeDependencyHash(modulated, patch.b)};
    t.modify<model::Track>(patch.b, [&](model::Track& tr) { tr.freeze = cache; });
    const auto frozen = commit(t);
    InstancePool pool = makePool();
    MediaCache media2;
    auto compiled = d::compile(frozen, pool, 0, 128);
    REQUIRE_MESSAGE(compiled, (compiled ? "" : compiled.error().toString()));
    REQUIRE(compiled->modEdges.size() == 1);
    const d::ModEdge& me = compiled->modEdges[0];
    CHECK(me.route == route);
    CHECK(me.inert);
    CHECK(me.stage == -1);
    CHECK(compiled->nodes[me.to].chain.empty());
    compiled->media = &media2;
    // The media file is not there, so prepare stops reading it, after the
    // latency solve has visited the route.
    auto prepared = d::prepare(*compiled, frozen);
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error().code == "audio.open");
}

namespace {

// Emit with the beat phase, and a latent gain ahead of the modulated one,
// modulated too from the same input: its route's delay is the latent stage's
// own compensation, the plain one's is shifted by it. `isolation` for both.
// B's volume and pan are modulated too, after the whole chain.
model::Session latentPair(model::Isolation isolation) {
    const Emit emit("beat_phase", 0.2);
    model::EditTxn t(emit.s, 2);
    model::FxInstance fx;
    fx.plugin.id = kGain;
    fx.isolation = isolation;
    fx.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value();
    const Id latent = t.create(fx);
    t.modify<model::Track>(emit.b,
                           [&](model::Track& tr) { tr.chain.insert(tr.chain.begin(), latent); });
    t.modify<model::FxInstance>(emit.gain, [&](model::FxInstance& f) { f.isolation = isolation; });
    modulate(t, emit.modOut, latent, "gain", -0.1);
    // And B's volume and pan, interpolated between grid points (M-i).
    modulate(t, emit.modOut, emit.b, "volume", -9.0);
    modulate(t, emit.modOut, emit.b, "pan", 40.0);
    return commit(t);
}

// Plays `s` live from 0 for `frames`: the left channel and the output latency.
struct Live {
    std::vector<float> left;
    std::uint32_t latency = 0;
};
Live playLive(const model::Session& s, const EngineOptions& options, std::uint32_t block,
              std::int64_t frames) {
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
    Engine engine(pool, media, options);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = block}));
    REQUIRE(engine.load(s));
    engine.play();
    const std::int64_t cycles = frames / block;
    if (options.anticipative)
        runAheadOnState(engine, backend, cycles, block);
    else
        backend.run(cycles);
    CHECK(engine.anticipationMisses() == 0);
    CHECK(engine.bridgeLateCycles() == 0);  // a late live block is silent by design
    engine.stop();
    CHECK(engine.processErrors() == 0);
    return {backend.capture().channels[0], engine.outputLatency()};
}

}  // namespace

TEST_CASE(
    "modulation: real-time output equals the offline render, in both executors, at 128 "
    "and 512" *
    doctest::description("rt")) {
    const model::Session s = latentPair(model::Isolation::InProcess);
    const std::int64_t seconds = 2, frames = 48000 * seconds + 4096;
    for (const std::uint32_t block : {128u, 512u}) {
        const auto offline = render(s, double(seconds), block);
        // The routes move the level, on the grid, so equality says something.
        CHECK(offline.left != render(Emit("beat_phase", std::nullopt).s, double(seconds)).left);
        for (const EngineOptions& options :
             {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 2}}) {
            INFO("block " << block << ", anticipative " << options.anticipative << ", workers "
                          << options.workers);
            const Live live = playLive(s, options, block, frames);
            CHECK(live.latency == 64);
            CHECK(maxDiff(live.left, live.latency, offline.left, 48000 * seconds) == 0.0);
        }
    }
}

TEST_CASE("modulation: through the bridge, real-time output equals the in-process render" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    const std::int64_t seconds = 1, frames = 48000 * seconds + 4096;
    const auto reference = render(latentPair(model::Isolation::InProcess), double(seconds), 128);
    for (const auto isolation : {model::Isolation::SharedBridge, model::Isolation::Dedicated}) {
        INFO("isolation " << static_cast<int>(isolation));
        const model::Session s = latentPair(isolation);
        CHECK(render(s, double(seconds), 128).left == reference.left);
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("anticipative " << options.anticipative);
            // A deadline no healthy plug-in misses under load (bridge_patience.h).
            const Live live = playLive(s, test::patient(options), 128, frames);
            CHECK(maxDiff(live.left, live.latency, reference.left, 48000 * seconds) == 0.0);
        }
    }
}

TEST_CASE("modulation: from a seek and over loop wraps, both executors play it as offline" *
          doctest::description("rt")) {
    const model::Session s = latentPair(model::Isolation::InProcess);
    constexpr std::uint32_t kBlock = 128;
    constexpr std::size_t kPass = 24000;  // the loop, 0.5 s to 1 s
    constexpr int kPasses = 4;
    auto pool = makePool();
    // From 1 s, after a seek: the offline render from there.
    const auto fromOne =
        renderOffline(s, pool, {.startSeconds = 1.0, .endSeconds = 2.0, .blockSize = kBlock})
            .value();
    const auto whole = render(s, 1.0, kBlock);  // no loop: up to the first wrap
    std::vector<std::vector<float>> loops;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        for (const bool looped : {false, true}) {
            INFO("looped " << looped);
            auto rtPool = makePool();
            MediaCache media;
            const std::size_t frames = looped ? kPass * (1 + kPasses) : 48000;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                                  .captureFrames = static_cast<std::int64_t>(frames) + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
            REQUIRE(engine.load(s));
            REQUIRE(engine.awaitPlan());
            if (looped)
                engine.setLoop(0.5, 1.0, true);
            else
                engine.seek(1.0);
            engine.play();
            const auto cycles = static_cast<std::int64_t>((frames + 4096) / kBlock);
            if (options.anticipative)
                runAheadOnState(engine, backend, cycles, kBlock);
            else
                backend.run(cycles);
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const std::size_t L = engine.outputLatency();
            const auto& got = backend.capture().channels[0];
            if (!looped) {
                CHECK(maxDiff(got, L, fromOne.audio.channels[0], 48000 - 4096) == 0.0);
                continue;
            }
            // Up to the first wrap, the render without a loop; every pass
            // after a wrap plays as the first such pass.
            CHECK(maxDiff(got, L, whole.left, 2 * kPass) == 0.0);
            const std::vector<float> second(
                got.begin() + static_cast<std::ptrdiff_t>(L + 2 * kPass),
                got.begin() + static_cast<std::ptrdiff_t>(L + 3 * kPass));
            for (int k = 3; k <= kPasses; ++k) {
                INFO("pass " << k);
                CHECK(maxDiff(got, L + static_cast<std::size_t>(k) * kPass, second, kPass) == 0.0);
            }
            loops.emplace_back(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(frames));
        }
    }
    REQUIRE(loops.size() == 2);
    CHECK(loops[0] == loops[1]);  // the two executors, sample for sample
}

TEST_CASE("modulation: a target handed to the live executor mid-play keeps its offsets" *
          doctest::description("rt")) {
    // Arming B makes it live (ADR-0096, M-f): its plug-ins and the rack that
    // modulates them move to the callback, carrying the memo and held values.
    const model::Session s = latentPair(model::Isolation::InProcess);
    model::EditTxn t(s, 3);
    const Id b = s.tracks.find(kMaster)->children.at(1);
    t.modify<model::Track>(b, [](model::Track& tr) { tr.record.armed = true; });
    const model::Session armed = commit(t);
    constexpr std::uint32_t kBlock = 128;
    const auto offline = render(s, 2.0, kBlock);
    CHECK(render(armed, 2.0, kBlock).left == offline.left);  // arming changes no sound
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
        const auto run = [&](std::int64_t frames) {
            if (options.anticipative)
                runAheadOnState(engine, backend, frames / kBlock, kBlock);
            else
                backend.run(frames / kBlock);
        };
        REQUIRE(engine.load(s));
        engine.play();
        run(32000);
        REQUIRE(engine.load(armed));
        run(32000);
        REQUIRE(engine.load(s));
        run(32000 + 4096);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(engine.anticipationMisses() == 0);
        const std::size_t L = engine.outputLatency();
        CHECK(maxDiff(backend.capture().channels[0], L, offline.left, 96000) == 0.0);
    }
}

TEST_CASE("modulation: depth and enabled reach a running plan without a compile" *
          doctest::description("rt")) {
    // ADR-0096, M-h: a depth drag is a mix value, as a fader is; lane compiles.
    const Emit emit("playing", 0.1);
    const auto edited = [&](const model::Session& from, auto&& change) {
        model::EditTxn t(from, from.version + 1);
        t.modify<model::Modulation>(emit.route, change);
        return commit(t);
    };
    const auto half = edited(emit.s, [](model::Modulation& m) { m.depth = 0.05; });
    const auto off = edited(half, [](model::Modulation& m) { m.enabled = false; });
    const auto hidden = edited(off, [](model::Modulation& m) { m.lane = false; });
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
        const auto run = [&](std::int64_t blocks) {
            if (options.anticipative)
                runAheadOnState(engine, backend, blocks, 96);
            else
                backend.run(blocks);
        };
        REQUIRE(engine.load(emit.s));
        engine.play();
        run(100);  // 9600 frames
        const auto generation = engine.planGeneration();
        REQUIRE(engine.load(half));
        CHECK(engine.planGeneration() == generation);
        run(100);
        REQUIRE(engine.load(off));
        CHECK(engine.planGeneration() == generation);
        run(100);
        REQUIRE(engine.load(hidden));
        CHECK(engine.planGeneration() > generation);  // lane is a route edit
        run(100);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        // The anticipative renderer takes a value up to its render-ahead
        // (9600 frames here) later: read each step's end.
        const auto& cap = backend.capture().channels[0];
        CHECK(double(cap[9000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
        CHECK(double(cap[19000]) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
        CHECK(double(cap[28500]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
        CHECK(double(cap[38000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
    }
}

namespace {

// Emit without a plug-in route, plus mixer routes added by `edit`; B's
// level reaches the master (the rack's Out is 1.0 while playing).
model::Session mixed(const Emit& emit, const std::function<void(model::EditTxn&)>& edit) {
    model::EditTxn t(emit.s, 2);
    edit(t);
    return commit(t);
}

double ratio(const std::vector<float>& a, const std::vector<float>& b, std::size_t at) {
    return double(a[at]) / double(b[at]);
}

}  // namespace

TEST_CASE("modulation: a track's volume moves by signal x depth in dB, clamped to its range" *
          doctest::description("rt")) {
    const Emit emit("playing", std::nullopt);
    const auto plain = render(emit.s, 0.5);
    const auto down = render(
        mixed(emit, [&](model::EditTxn& t) { modulate(t, emit.modOut, emit.b, "volume", -6.0); }),
        0.5);
    CHECK(ratio(down.left, plain.left, 12000) ==
          doctest::Approx(console9::faderGain(-6) / console9::faderGain(0)).epsilon(1e-6));
    // Base +10 dB and +6 dB of modulation: the sum stops at the top, 12 dB.
    const auto loud = [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) { tr.volumeDb = 10; });
    };
    const auto base = render(mixed(emit, loud), 0.5);
    const auto top = render(mixed(emit,
                                  [&](model::EditTxn& t) {
                                      loud(t);
                                      modulate(t, emit.modOut, emit.b, "volume", 6.0);
                                  }),
                            0.5);
    CHECK(ratio(top.left, base.left, 12000) ==
          doctest::Approx(console9::faderGain(12) / console9::faderGain(10)).epsilon(1e-6));
    // A disabled route adds 0.
    const auto off = render(mixed(emit,
                                  [&](model::EditTxn& t) {
                                      const Id r = modulate(t, emit.modOut, emit.b, "volume", -6.0);
                                      t.modify<model::Modulation>(
                                          r, [](model::Modulation& m) { m.enabled = false; });
                                  }),
                            0.5);
    CHECK(off.left == plain.left);
}

TEST_CASE("modulation: pan and width move by signal x depth in percent" *
          doctest::description("rt")) {
    const Emit emit("playing", std::nullopt);
    InstancePool pool = makePool();
    const auto stereo = [&](const model::Session& s) {
        OfflineSettings settings;
        settings.endSeconds = 0.5;
        auto r = renderOffline(s, pool, settings);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::array{double(r->audio.channels[0][12000]), double(r->audio.channels[1][12000])};
    };
    const auto panned = stereo(
        mixed(emit, [&](model::EditTxn& t) { modulate(t, emit.modOut, emit.b, "pan", 50.0); }));
    const auto fixed = stereo(mixed(emit, [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) { tr.pan = 50; });
    }));
    CHECK(panned[0] == doctest::Approx(fixed[0]).epsilon(1e-6));
    CHECK(panned[1] == doctest::Approx(fixed[1]).epsilon(1e-6));
    CHECK(panned[1] > panned[0]);
    // Clamped at 100.
    const auto hard = stereo(mixed(emit, [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) { tr.pan = 80; });
        modulate(t, emit.modOut, emit.b, "pan", 60.0);
    }));
    const auto right = stereo(mixed(emit, [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) { tr.pan = 100; });
    }));
    CHECK(hard[0] == doctest::Approx(right[0]).epsilon(1e-6));
    CHECK(hard[1] == doctest::Approx(right[1]).epsilon(1e-6));
    const auto narrow = stereo(mixed(emit, [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) { tr.pan = 50; });
        modulate(t, emit.modOut, emit.b, "width", -60.0);
    }));
    const auto narrowFixed = stereo(mixed(emit, [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) {
            tr.pan = 50;
            tr.width = 40;
        });
    }));
    CHECK(narrow[0] == doctest::Approx(narrowFixed[0]).epsilon(1e-6));
    CHECK(narrow[1] == doctest::Approx(narrowFixed[1]).epsilon(1e-6));
}

TEST_CASE("modulation: a send's level moves by signal x depth, and the send is encoded for it" *
          doctest::description("rt")) {
    // B is silent at its fader; a pre-fader send carries it to a buss.
    const Emit emit("playing", std::nullopt);
    Id send;
    const auto sending = [&](model::EditTxn& t) {
        t.modify<model::Track>(emit.b, [](model::Track& tr) { tr.volumeDb = model::kSilenceDb; });
        model::Track buss;
        buss.type = model::StripType::Buss;
        buss.parent = kMaster;
        const Id verb = t.create(buss);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(verb); });
        model::Route r;
        r.from.track = emit.b;
        r.to.track = verb;
        r.tap = model::Tap::PreFader;
        send = t.create(r);
    };
    const auto plain = render(mixed(emit, sending), 0.5);
    const auto down = render(mixed(emit,
                                   [&](model::EditTxn& t) {
                                       sending(t);
                                       modulate(t, emit.modOut, send, "volume", -6.0);
                                   }),
                             0.5);
    REQUIRE(std::abs(plain.left[12000]) > 0.01f);
    CHECK(ratio(down.left, plain.left, 12000) ==
          doctest::Approx(console9::faderGain(-6) / console9::faderGain(0)).epsilon(1e-5));
}

TEST_CASE("modulation: a mixer target is linear between grid points, a plug-in steps on them" *
          doctest::description("rt")) {
    const Emit emit("beat_phase", std::nullopt);
    const auto level =
        render(mixed(emit,
                     [&](model::EditTxn& t) { modulate(t, emit.modOut, emit.b, "volume", -12.0); }),
               1.0)
            .left;
    // Over a beat the phase ramps, so the level changes at almost every
    // frame (not only every 32), and the same at any block size.
    std::size_t changes = 0;
    for (std::size_t i = 4800; i < 24000; ++i) changes += level[i] != level[i - 1];
    CHECK(changes > 19000 * 9 / 10);
    for (const std::uint32_t block : {37u, 128u})
        CHECK(render(mixed(emit,
                           [&](model::EditTxn& t) {
                               modulate(t, emit.modOut, emit.b, "volume", -12.0);
                           }),
                     1.0, block)
                  .left == level);
}
