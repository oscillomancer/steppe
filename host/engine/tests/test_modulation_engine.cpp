// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes in the plan (ADR-0096, engine.md §1.4.1): every route is
// an edge that orders its source rack first, runs source and target in one
// executor, and is compensated as a sidechain into its target stage.
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "plan.h"
#include "plan_dump.h"
#include "rack_runtime.h"
#include "supple/model/schemas.h"

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
