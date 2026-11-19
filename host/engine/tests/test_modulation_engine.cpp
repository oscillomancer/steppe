// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes in the plan (ADR-0096, engine.md §1.4.1): every route is
// an edge that orders its source rack first, runs source and target in one
// executor, and is compensated as a sidechain into its target stage.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <supple/engine/offline.h>
#include <supple/engine/realtime.h>

#include "plan.h"
#include "plan_dump.h"
#include "rack_runtime.h"
#include "supple/model/schemas.h"
#include "supple/params/params.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {
namespace d = supple::engine::detail;

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kGain = "com.suppleaudio.test.gain";

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
    return me.stage >= 0        ? n.chain[static_cast<std::size_t>(me.stage)].before
           : me.inputStage >= 0 ? 0
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

TEST_CASE("modulation: a disabled route is still an edge, a mixer target is inert until M2") {
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
    CHECK(p.plan.modEdges[1].inert);
    CHECK(std::any_of(p.plan.warnings.begin(), p.plan.warnings.end(), [&](const std::string& w) {
        return w.find(fader.str()) != std::string::npos && w.find("M2") != std::string::npos;
    }));
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
