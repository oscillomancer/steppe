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
    // When a mix value is heard: from the next block its executor renders.
    // In the callback that is the next cycle; the anticipative renderer may
    // be up to its render-ahead (200 ms, 9600 frames) past the playhead, in
    // a block of up to renderBlock (1024) frames. Each edit is made with the
    // renderer as far ahead as it goes, the latest arrival there is, so the
    // test is decided by that bound and not by how fast the machine renders
    // (CI 44760220473: a Windows runner whose renderer had filled its budget
    // still played depth 0.1 at frame 19000).
    constexpr std::int64_t kStep = 300 * 96;  // frames between edits
    constexpr std::int64_t kLatest = 9600 + 1024;
    static_assert(kLatest < kStep);
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        InstancePool pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4 * kStep});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
        const auto run = [&](std::int64_t blocks) {
            if (options.anticipative)
                runAheadOnState(engine, backend, blocks, 96);
            else
                backend.run(blocks);
        };
        // The renderer at its render-ahead (state alone: no deadline decides).
        const auto fullyAhead = [&] {
            while (options.anticipative && !engine.awaitRendered(1 << 30)) {
            }
        };
        REQUIRE(engine.load(emit.s));
        engine.play();
        run(kStep / 96);
        const auto generation = engine.planGeneration();
        fullyAhead();
        REQUIRE(engine.load(half));
        CHECK(engine.planGeneration() == generation);
        run(kStep / 96);
        fullyAhead();
        REQUIRE(engine.load(off));
        CHECK(engine.planGeneration() == generation);
        run(kStep / 96);
        fullyAhead();
        REQUIRE(engine.load(hidden));
        CHECK(engine.planGeneration() > generation);  // lane is a route edit
        run(kStep / 96);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(engine.anticipationMisses() == 0);
        const auto& cap = backend.capture().channels[0];
        const std::size_t L = engine.outputLatency();
        const auto at = [&](std::int64_t frame) {
            return double(cap[L + static_cast<std::size_t>(frame)]);
        };
        // Before each edit the old value; from the latest arrival on, the new.
        CHECK(at(kStep - 1) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
        CHECK(at(kStep + kLatest) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
        CHECK(at(2 * kStep - 1) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
        CHECK(at(2 * kStep + kLatest) == doctest::Approx(gainAt(0)).epsilon(1e-4));
        CHECK(at(3 * kStep - 1) == doctest::Approx(gainAt(0)).epsilon(1e-4));
        CHECK(at(3 * kStep + kLatest) == doctest::Approx(gainAt(0)).epsilon(1e-4));
        CHECK(at(4 * kStep - 1) == doctest::Approx(gainAt(0)).epsilon(1e-4));
        // In the callback, the next cycle.
        if (!options.anticipative) {
            CHECK(at(kStep) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
            CHECK(at(2 * kStep) == doctest::Approx(gainAt(0)).epsilon(1e-4));
        }
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

TEST_CASE("modulation: removing another Mod Out keeps a latency-disabled route off") {
    // Review round 54, F1: what M-d keeps names the source by module and
    // input, so a shift in the rack's Mod Outs is no route edit. The route
    // comes from the rack's second Mod Out; the first, unrouted, goes.
    const Emit emit("playing", std::nullopt);
    model::EditTxn t(emit.s, 2);
    const Id second = addModule(t, emit.rack, model::kModOutModule);
    Id transport{};
    for (const auto& c : emit.s.cables)
        if (c.value->to.object == emit.modOut) transport = c.value->from.object;
    cable(t, transport, "playing", second, "in_1");
    const Id route = modulate(t, second, emit.gain, "gain", 0.1);
    const auto on = commit(t);
    model::EditTxn l(on, 3);
    l.modify<model::FxInstance>(emit.gain, [](model::FxInstance& fx) {
        fx.patch =
            json::parse(
                R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-6,"latency":"short"}})")
                .value();
    });
    const auto latent = commit(l);
    model::EditTxn r(latent, 4);
    for (const auto& c : latent.cables)
        if (c.value->to.object == emit.modOut) r.remove<model::Cable>(c.id);
    r.remove<model::RackModule>(emit.modOut);
    const auto removed = commit(r);
    REQUIRE(removed.modulations.contains(route));

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
    REQUIRE(engine.load(on));
    engine.play();
    settle(100);
    REQUIRE(engine.load(latent));
    settle(100);
    REQUIRE(pool.modulationDisabled(emit.gain));
    REQUIRE(engine.load(removed));
    settle(100);
    engine.stop();
    CHECK(pool.modulationDisabled(emit.gain));
    CHECK(double(backend.capture().channels[0][28000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
}

TEST_CASE(
    "modulation: an FX moved into its track's input chain with its route removed gets its 0") {
    // Review round 54, F2: the memo follows the instance between the input
    // chain and the main chain, so the removed route's offset is cleared.
    const Emit on("playing", 0.1);
    model::EditTxn a(on.s, 2);
    a.modify<model::Track>(on.b, [](model::Track& tr) { tr.record.armed = true; });
    const auto armed = commit(a);
    model::EditTxn m(armed, 3);
    m.remove<model::Modulation>(on.route);
    m.modify<model::Track>(on.b, [&](model::Track& tr) {
        tr.chain.clear();
        tr.inputChain.push_back(on.gain);
    });
    const auto moved = commit(m);
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    REQUIRE(engine.load(armed));
    engine.play();
    backend.run(100);
    REQUIRE(engine.load(moved));
    backend.run(100);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto& cap = backend.capture().channels[0];
    CHECK(double(cap[9000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
    CHECK(double(cap[19000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
}

TEST_CASE("modulation: a handover that also changes the routes sends the new offsets" *
          doctest::description("rt")) {
    // Review round 54's open question: the memo carried when a track goes
    // live (arming B) must describe what its instance last heard. A constant
    // source makes a stale memo visible: a matching sum would not be resent.
    // Arming B and halving the depth at once, then arming off and removing
    // the route at once: each level is the new one, in both executors.
    const Emit on("playing", 0.1);
    model::EditTxn a(on.s, 2);
    a.modify<model::Track>(on.b, [](model::Track& tr) { tr.record.armed = true; });
    a.modify<model::Modulation>(on.route, [](model::Modulation& m) { m.depth = 0.05; });
    const auto live = commit(a);
    model::EditTxn r(live, 3);
    r.modify<model::Track>(on.b, [](model::Track& tr) { tr.record.armed = false; });
    r.remove<model::Modulation>(on.route);
    const auto gone = commit(r);
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
        REQUIRE(engine.load(on.s));
        engine.play();
        run(100);
        REQUIRE(engine.load(live));
        run(150);
        REQUIRE(engine.load(gone));
        run(150);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        const auto& cap = backend.capture().channels[0];
        CHECK(double(cap[9000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
        // Loads at 9600 and 24000; the anticipative renderer takes each up to
        // its render-ahead later, so read near each segment's end.
        CHECK(double(cap[23000]) == doctest::Approx(gainAt(0.05)).epsilon(1e-4));
        CHECK(double(cap[38000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
    }
}

TEST_CASE("modulation: an event that finds no room waits for the next grid point, counted") {
    // ADR-0096: events dropped for capacity are counted and reported. The
    // emitter keeps a value that does not fit and sends the then-current one
    // at a later grid point; each wait counts.
    const Emit emit("playing", 0.1);
    Prepared p(emit.s);
    d::Stage* stage = nullptr;
    for (d::Node& n : p.plan.nodes)
        for (d::Stage& st : n.chain)
            if (!st.mods.empty()) stage = &st;
    REQUIRE(stage);
    REQUIRE(stage->instance);
    if (!stage->instance->processing()) REQUIRE(stage->instance->startProcessing());
    const d::ModEdge& me = p.plan.modEdges.at(0);
    std::fill(p.plan.modEdges[0].values.begin(), p.plan.modEdges[0].values.end(), 1.0);
    REQUIRE(me.values.size() == 128);
    host::Event filler;
    filler.kind = host::Event::Kind::Param;
    stage->blockEvents.clear();
    while (stage->blockEvents.size() < stage->blockEvents.capacity())
        stage->blockEvents.push_back(filler);
    const auto full = stage->blockEvents.size();
    d::modulateStage(p.plan, *stage, 0, 128, 0, nullptr);
    CHECK(stage->blockEvents.size() == full);  // nothing fits
    CHECK(stage->modDeferred == 4);            // grid points 0, 32, 64 and 96
    stage->blockEvents.clear();
    d::modulateStage(p.plan, *stage, 128, 128, 0, nullptr);
    REQUIRE_FALSE(stage->blockEvents.empty());  // sent at the first point with room
    CHECK(stage->blockEvents.front().kind == host::Event::Kind::ParamMod);
    CHECK(stage->blockEvents.front().time == 0);
    CHECK(stage->modDeferred == 4);
    stage->instance->stopProcessing();
}

TEST_CASE("modulation: telemetry gives each bound route its target's live offset" *
          doctest::description("rt")) {
    // ADR-0096, M2: a plug-in's offset as a fraction of its range, a mixer
    // target's in its unit; routes on one target read the same sum.
    const Emit emit("playing", 0.1);
    const auto s = mixed(emit, [&](model::EditTxn& t) {
        modulate(t, emit.modOut, emit.b, "volume", -6.0);
        model::Modulation second;
        second.source = {emit.modOut, "in_2"};
        second.target = {emit.gain, "gain"};
        second.depth = 0.05;
        t.create(second);  // in_2 has no cable: it adds 0
    });
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 96}));
    REQUIRE(engine.load(s));
    CHECK(engine.modulationOffsets().size() == 3);
    engine.play();
    backend.run(100);
    const auto offsets = engine.modulationOffsets();
    engine.stop();
    REQUIRE(offsets.size() == 3);
    CHECK(offsets.at(emit.route) == doctest::Approx(0.1));  // playing: 1 x 0.1
    CHECK(offsets.at(Id{model::Kind::Modulation, emit.route.n + 2}) == doctest::Approx(0.1));
    CHECK(offsets.at(Id{model::Kind::Modulation, emit.route.n + 1}) == doctest::Approx(-6.0));
}

TEST_CASE("modulation: a stem of a modulated track carries it, its source rack kept unheard" *
          doctest::description("rt")) {
    // B's gain is modulated from a second rack on its own channel C, which
    // feeds nothing B plays. A render of B alone (a stem, a track analysis)
    // keeps that rack as a dependency and applies the modulation exactly as
    // the mix does; C is not heard in it, though the mix hears C.
    Emit emit("playing", std::nullopt);
    model::EditTxn t(emit.s, 2);
    const Id c = addChannel(t);
    const Id rack = t.create(model::Rack{c, "Modulators"});
    const Id transport = addModule(t, rack, "com.suppleaudio.rack.transport");
    const Id modOut = addModule(t, rack, model::kModOutModule);
    cable(t, transport, "beat_phase", modOut, "in_1");
    modulate(t, modOut, emit.gain, "gain", 0.2);
    const model::Session silent = commit(t);
    // The same with C audible: its rack's Out plays the transport's gate.
    model::EditTxn u(silent, 3);
    const Id out = addModule(u, rack, model::kOutModule);
    cable(u, transport, "playing", out, "l");
    model::Route heard;
    heard.from.module = out;
    heard.to.track = c;
    const Id input = u.create(heard);
    u.modify<model::Track>(c, [&](model::Track& tr) { tr.record.input = input; });
    const model::Session s = commit(u);
    InstancePool pool = makePool();
    OfflineSettings settings;
    settings.endSeconds = 1.0;
    auto mix = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(mix, (mix ? std::string() : mix.error().toString()));
    auto quiet = renderOffline(silent, pool, settings);
    REQUIRE_MESSAGE(quiet, (quiet ? std::string() : quiet.error().toString()));
    const bool hearsC = mix->audio.channels[0] != quiet->audio.channels[0];
    CHECK(hearsC);
    settings.track = emit.b;
    auto stem = renderOffline(s, pool, settings);
    REQUIRE_MESSAGE(stem, (stem ? std::string() : stem.error().toString()));
    // B alone, modulated as in the mix, and nothing of C.
    const bool alone = stem->audio.channels[0] == quiet->audio.channels[0];
    CHECK(alone);
    // The modulation moved it: equality says something.
    CHECK(stem->audio.channels[0] != render(emit.s, 1.0).left);
}

namespace {

// ADR-0096 step M4. B's gain becomes the fixed gain, whose gain is
// automatable but not modulatable. With `envelope`, an envelope holds it at
// -6 dB (its CLAP value, the fixture's normalized one), and a route of
// `depth` reaches it through that envelope's lane.
model::Session throughEnvelope(const char* source, std::optional<double> depth,
                               bool envelope = true,
                               model::Isolation isolation = model::Isolation::InProcess) {
    Emit emit(source, std::nullopt);
    model::EditTxn t(emit.s, 2);
    t.modify<model::FxInstance>(emit.gain, [&](model::FxInstance& f) {
        f.plugin.id = "com.suppleaudio.test.fixed-gain";
        f.patch.reset();
        f.isolation = isolation;
    });
    if (envelope) {
        params::ParamSpec spec;
        spec.type = params::Type::Float;
        spec.unit = units::Unit::dB;
        spec.min = -60;
        spec.max = 12;
        model::Envelope env;
        env.target = {emit.gain, "0"};
        env.points = {{0.0, params::toClap(spec, -6)}};
        t.create(env);
    }
    if (depth) modulate(t, emit.modOut, emit.gain, "0", *depth);
    return commit(t);
}

}  // namespace

TEST_CASE("modulation through an envelope: its lane sends base + signal x depth x range, clamped" *
          doctest::description("rt")) {
    const auto plain = render(throughEnvelope("playing", std::nullopt), 0.5);
    CHECK(double(plain.left[12000]) == doctest::Approx(gainAt(0)).epsilon(1e-4));
    const auto up = render(throughEnvelope("playing", 0.1), 0.5);
    CHECK(double(up.left[12000]) == doctest::Approx(gainAt(0.1)).epsilon(1e-4));
    const auto down = render(throughEnvelope("playing", -0.25), 0.5);
    CHECK(double(down.left[12000]) == doctest::Approx(gainAt(-0.25)).epsilon(1e-4));
    // Past the top of the range the sum is clamped to it.
    const auto top = render(throughEnvelope("playing", 1.0), 0.5);
    CHECK(double(top.left[12000]) == doctest::Approx(gainAt(1.0)).epsilon(1e-4));
    CHECK_FALSE(warns(up.warnings, "modulation.target"));
}

TEST_CASE("modulation through an envelope: with no active envelope the route is inert") {
    const model::Session s = throughEnvelope("playing", 0.1, false);
    const auto r = render(s, 0.5);
    CHECK(warns(r.warnings, "modulation.target"));
    CHECK(warns(r.warnings, "no active envelope"));
    CHECK(r.left == render(throughEnvelope("playing", std::nullopt, false), 0.5).left);
}

TEST_CASE("modulation through an envelope: the events fall on the grid whatever the block size" *
          doctest::description("rt")) {
    const model::Session s = throughEnvelope("beat_phase", 0.2);
    const auto at512 = render(s, 1.0, 512);
    CHECK(at512.left == render(s, 1.0, 37).left);
    CHECK(at512.left == render(s, 1.0, 128).left);
    std::size_t changes = 0;
    for (std::size_t i = 1; i < at512.left.size(); ++i)
        if (at512.left[i] != at512.left[i - 1]) {
            ++changes;
            CHECK(i % d::kAutomationStep == 0);
        }
    CHECK(changes > 10);
}

TEST_CASE(
    "modulation through an envelope: across a plan swap the lane holds its sum while the "
    "rack is elsewhere") {
    // M-f: while another executor still owns the source rack's modules, a
    // lane holds its last sum, as Mod Outs and the PARAM_MOD memo do. The
    // sum comes with the plan swap; a fresh one would send its envelope's
    // base alone (review 63, probe P2).
    const model::Session s = throughEnvelope("beat_phase", 0.2);
    InstancePool pool = makePool();
    MediaCache media;
    const auto make = [&] {
        auto compiled = d::compile(s, pool, 0, 128);
        REQUIRE_MESSAGE(compiled, (compiled ? "" : compiled.error().toString()));
        compiled->media = &media;
        auto ok = d::prepare(*compiled, s);
        REQUIRE_MESSAGE(ok, (ok ? "" : ok.error().toString()));
        return compiled;
    };
    const auto lane = [](d::Plan& p) -> d::ParamLane* {
        for (d::Node& n : p.nodes)
            for (d::Stage& st : n.chain)
                for (d::ParamLane& ln : st.lanes)
                    if (ln.modCount) return &ln;
        return nullptr;
    };
    auto a = make();
    const char renderer = 0, other = 0;
    d::adoptRackModules(nullptr, &a->rendererModules, &renderer);
    d::Workers workers(0);
    std::int64_t pos = 0;
    for (; pos < 30000; pos += 128)
        REQUIRE(d::processRendererBlock(&workers, *a, pos, 128, &renderer));
    REQUIRE(lane(*a));
    const double sent = lane(*a)->lastSent;
    REQUIRE(lane(*a)->modSum != 0.0);
    auto b = make();
    d::carryMix(*a, *b, 0);
    // The rack's modules pass to the other executor before this one runs
    // the new plan's first block.
    d::adoptRackModules(&a->rendererModules, nullptr, &renderer);
    d::claimRackModules(b->rendererModules, &other);
    REQUIRE(d::processRendererBlock(&workers, *b, pos, 128, &renderer));
    REQUIRE(lane(*b));
    CHECK(lane(*b)->lastSent == sent);
}

TEST_CASE("modulation through an envelope: a target handed to the live executor keeps its sum" *
          doctest::description("rt")) {
    // Arming B moves it, and the rack that modulates it, to the callback
    // (M-f). While the rack's modules are still the renderer's, the lane
    // holds its last sum, carried with the plan, as the PARAM_MOD memo
    // does; it never falls back to its envelope's base (review 63, P2).
    const model::Session s = throughEnvelope("beat_phase", 0.2);
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

TEST_CASE(
    "modulation through an envelope: real-time output equals the offline render, in both "
    "executors and through the bridge" *
    doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    const std::int64_t seconds = 1, frames = 48000 * seconds + 4096;
    for (const auto isolation : {model::Isolation::InProcess, model::Isolation::SharedBridge})
        for (const std::uint32_t block : {128u, 512u}) {
            const model::Session s = throughEnvelope("beat_phase", 0.2, true, isolation);
            const auto offline = render(s, double(seconds), block);
            CHECK(offline.left !=
                  render(throughEnvelope("beat_phase", std::nullopt), double(seconds)).left);
            for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                INFO("isolation " << static_cast<int>(isolation) << ", block " << block
                                  << ", anticipative " << options.anticipative);
                const Live live = playLive(s, test::patient(options), block, frames);
                CHECK(maxDiff(live.left, live.latency, offline.left, 48000 * seconds) == 0.0);
            }
        }
}

namespace {

// ADR-0061, amended 2026-11-20: B's gain is the fixed gain (automatable, not
// modulatable), its envelope moving from -30 dB to 0 dB over a second, in
// `mode`; `modulated`, a route of depth 0.2 from the beat phase reaches it
// through that envelope (ADR-0096 step M4).
struct HeldLane {
    model::Session s;
    Id gain, b;
};
HeldLane movingLane(bool modulated, model::EnvMode mode = model::EnvMode::Touch) {
    Emit emit("beat_phase", std::nullopt);
    model::EditTxn t(emit.s, 2);
    t.modify<model::FxInstance>(emit.gain, [&](model::FxInstance& f) {
        f.plugin.id = "com.suppleaudio.test.fixed-gain";
        f.patch.reset();
    });
    params::ParamSpec spec;
    spec.type = params::Type::Float;
    spec.unit = units::Unit::dB;
    spec.min = -60;
    spec.max = 12;
    model::Envelope env;
    env.target = {emit.gain, "0"};
    env.mode = mode;
    env.timebase = model::Timebase::Time;
    env.points = {{0.0, params::toClap(spec, -30)}, {1.0, params::toClap(spec, 0)}};
    t.create(env);
    if (modulated) modulate(t, emit.modOut, emit.gain, "0", 0.2);
    return {commit(t), emit.gain, emit.b};
}

// The fixture's test controls (GainControl in fixture_plugins.cpp).
struct GainHand {
    void (*edit)(const clap_plugin_t*, double, std::uint32_t);
    std::uint32_t (*deliveries)(const clap_plugin_t*);
    void (*latency)(const clap_plugin_t*, bool);
    std::uint32_t (*creates)();
    void (*patchRescan)(const clap_plugin_t*, std::uint32_t);
    void (*patchEffect)(const clap_plugin_t*, std::uint32_t);
    void (*hand)(const clap_plugin_t*, double, std::uint32_t);
};
const GainHand& handOf(InstancePool& pool, Id fx) {
    host::Instance* instance = pool.find(fx);
    REQUIRE(instance);
    const auto* plugin = instance->raw();
    const auto* control =
        static_cast<const GainHand*>(plugin->get_extension(plugin, "supple.test.gain-control"));
    REQUIRE(control);
    return *control;
}

double dbGain(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

TEST_CASE(
    "write holds: a touch over a moving lane plays the hand, and the rest the offline render" *
    doctest::description("rt")) {
    // ADR-0061, amended 2026-11-20: from the block after the plug-in's
    // gesture begins to the one it ends in, the lane sends nothing, so the
    // plug-in plays what the hand set, whatever the curve or the modulation
    // does. Released, the lane sends at once and from its next grid point
    // the output is the offline render's again. In the callback, and in an
    // anticipative engine's live executor (B armed); the renderer's own
    // run is the next case.
    constexpr std::uint32_t kBlock = 128;
    constexpr std::int64_t kBefore = 100, kAfter = 100;
    for (const bool modulated : {false, true}) {
        const HeldLane lane = movingLane(modulated);
        model::EditTxn t(lane.s, lane.s.version + 1);
        t.modify<model::Track>(lane.b, [](model::Track& tr) { tr.record.armed = true; });
        const model::Session armed = commit(t);
        const auto offline = render(lane.s, 1.0, kBlock);
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("modulated " << modulated << ", anticipative " << options.anticipative);
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 40000});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
            const auto run = [&](std::int64_t blocks) {
                if (options.anticipative)
                    runAheadOnState(engine, backend, blocks, kBlock);
                else
                    backend.run(blocks);
            };
            REQUIRE(engine.load(options.anticipative ? armed : lane.s));
            engine.play();
            run(kBefore);
            const GainHand& control = handOf(pool, lane.gain);
            const auto* plugin = pool.find(lane.gain)->raw();
            control.hand(plugin, -20, 1);  // grabbed: its gesture begins in this block
            run(1);
            const auto deliveries = control.deliveries(plugin);
            std::vector<double> hand;  // what it set before each held block
            for (int k = 1; k <= 20; ++k) {
                hand.push_back(-20 + 0.5 * k);
                control.hand(plugin, hand.back(), 0);
                run(1);
            }
            hand.push_back(-9);
            control.hand(plugin, hand.back(), 2);  // let go: its gesture ends in this block
            run(1);
            const bool lanesQuiet = control.deliveries(plugin) == deliveries;
            CHECK(lanesQuiet);  // the lane sent the plug-in nothing while held
            run(kAfter);
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const auto& cap = backend.capture().channels[0];
            const std::size_t L = engine.outputLatency();
            const auto same = [&](std::int64_t from, std::int64_t to) {
                double worst = 0;
                for (auto i = from; i < to; ++i)
                    worst = std::max(worst, std::fabs(double(cap[L + std::size_t(i)]) -
                                                      double(offline.left[std::size_t(i)])));
                return worst;
            };
            CHECK(same(0, kBefore * kBlock) == 0.0);
            bool handHeard = true;
            for (std::size_t j = 0; j < hand.size(); ++j) {
                const auto start = (kBefore + 1 + std::int64_t(j)) * std::int64_t(kBlock);
                for (std::int64_t i = start; i < start + kBlock; ++i)
                    handHeard = handHeard && double(cap[L + std::size_t(i)]) ==
                                                 doctest::Approx(dbGain(hand[j])).epsilon(1e-6);
            }
            CHECK(handHeard);
            const std::int64_t released =
                (kBefore + 1 + std::int64_t(hand.size())) * std::int64_t(kBlock);
            CHECK(same(released + d::kAutomationStep, released + kAfter * kBlock) == 0.0);
        }
    }
}

TEST_CASE(
    "write holds: the renderer holds a latch past its gesture, across a plan swap, until "
    "the transport stops") {
    // A latching mode holds the hand's last value after the gesture ends,
    // to the pass's end (ADR-0061). B is the renderer's here. The hold comes
    // with the plan swap the pass's commit makes; a stop ends it and the
    // lane sends again. Each step runs well past the render-ahead, so the
    // renderer has processed what the hand did (decided by its state).
    const HeldLane held = movingLane(false, model::EnvMode::Latch);
    params::ParamSpec spec;
    spec.type = params::Type::Float;
    spec.unit = units::Unit::dB;
    spec.min = -60;
    spec.max = 12;
    model::EditTxn t(held.s, held.s.version + 1);  // what the pass's commit writes
    t.modify<model::Envelope>(held.s.envelopes.begin()->id, [&](model::Envelope& e) {
        e.points.push_back({0.5, params::toClap(spec, -18)});
        std::sort(e.points.begin(), e.points.end(),
                  [](const auto& x, const auto& y) { return x.pos < y.pos; });
    });
    const model::Session written = commit(t);
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, EngineOptions{.workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    const auto run = [&] { runAheadOnState(engine, backend, 200, 128); };
    REQUIRE(engine.load(held.s));
    engine.play();
    run();
    const GainHand& control = handOf(pool, held.gain);
    const auto* plugin = pool.find(held.gain)->raw();
    const auto playing = control.deliveries(plugin);
    run();
    CHECK(control.deliveries(plugin) > playing);  // the moving lane sends
    control.hand(plugin, -20, 1);
    run();
    const auto deliveries = control.deliveries(plugin);
    control.hand(plugin, -18, 2);
    run();
    const auto generation = engine.planGeneration();
    REQUIRE(engine.load(written));
    CHECK(engine.planGeneration() > generation);
    run();
    run();
    CHECK(control.deliveries(plugin) == deliveries);  // held past the gesture and the swap
    REQUIRE(pool.find(held.gain)->paramValue(0));
    CHECK(*pool.find(held.gain)->paramValue(0) == doctest::Approx(params::toClap(spec, -18)));
    // Paused: the pass is over, and the lane plays its curve again.
    engine.pause();
    while (!engine.awaitRendered(0)) {
    }
    engine.play();
    run();
    CHECK(control.deliveries(plugin) > deliveries);
    engine.stop();
    CHECK(engine.processErrors() == 0);
}

TEST_CASE("write holds: the main thread's hold keeps a latch and its release lets it go") {
    // The pass's own hold (EngineApp::writeHolds, through the mix slots)
    // holds the lane with no gesture and no compile; when it lets go before
    // the stop (an undo starts the pass again) the latch goes with it.
    const HeldLane held = movingLane(false, model::EnvMode::Latch);
    InstancePool pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
    REQUIRE(engine.load(held.s));
    engine.play();
    backend.run(20);
    const GainHand& control = handOf(pool, held.gain);
    const auto* plugin = pool.find(held.gain)->raw();
    const auto generation = engine.planGeneration();
    CHECK_FALSE(engine.setWriteHolds({{held.gain, "0"}}));
    CHECK(engine.planGeneration() == generation);
    backend.run(1);
    const auto deliveries = control.deliveries(plugin);
    backend.run(20);
    CHECK(control.deliveries(plugin) == deliveries);
    control.hand(plugin, -20, 1);
    backend.run(1);
    control.hand(plugin, -18, 2);
    backend.run(20);
    CHECK(control.deliveries(plugin) == deliveries);
    CHECK_FALSE(engine.setWriteHolds({}));
    backend.run(2);
    CHECK(control.deliveries(plugin) > deliveries);
    engine.stop();
}
