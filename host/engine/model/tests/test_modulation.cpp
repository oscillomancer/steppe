// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes in the session (ADR-0096, session-model "Modulation").
#include <doctest/doctest.h>
#include <supple/base/sha256.h>
#include <supple/json/write.h>
#include <supple/model/freeze.h>
#include <supple/model/project_file.h>
#include <supple/model/schemas.h>
#include <supple/model/versions.h>

#include <algorithm>
#include <string>

using namespace supple;
using namespace supple::model;

namespace {

// Channel A homes a rack with an LFO into a Mod Out; channel B has a
// plug-in in its chain and one in its input chain; channel A sends to a
// buss. Nothing is modulated yet.
struct Fixture {
    Session s;
    Id a, b, buss, rack, lfo, modOut, fx, inputFx, send;
};

Id addTrack(EditTxn& t, StripType type = StripType::Channel) {
    Track tr;
    tr.type = type;
    tr.parent = kMaster;
    const Id id = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(id); });
    return id;
}

Id addFx(EditTxn& t, Id track, bool input = false) {
    FxInstance fx;
    fx.plugin = {"clap", "test.filter", "", "", ""};
    const Id id = t.create(fx);
    t.modify<Track>(track, [&](Track& tr) { (input ? tr.inputChain : tr.chain).push_back(id); });
    return id;
}

Fixture fixture() {
    Fixture f;
    EditTxn t(Session::create(), 1);
    f.a = addTrack(t);
    f.b = addTrack(t);
    f.buss = addTrack(t, StripType::Buss);
    f.rack = t.create(Rack{f.a, "Modulators"});
    RackModule lfo;
    lfo.rack = f.rack;
    lfo.type = "com.suppleaudio.rack.lfo";
    f.lfo = t.create(lfo);
    RackModule modOut;
    modOut.rack = f.rack;
    modOut.type = std::string(kModOutModule);
    f.modOut = t.create(modOut);
    t.create(Cable{{f.lfo, "out"}, {f.modOut, "in_1"}, std::nullopt, ""});
    f.fx = addFx(t, f.b);
    f.inputFx = addFx(t, f.b, true);
    Route send;
    send.from.track = f.a;
    send.to.track = f.buss;
    f.send = t.create(send);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    f.s = *s;
    return f;
}

Modulation route(Id module, Id target, std::string param, double depth,
                 std::string input = "in_1") {
    Modulation m;
    m.source = {module, std::move(input)};
    m.target = {target, std::move(param)};
    m.depth = depth;
    return m;
}

// Adds `m` to `s` and commits.
Result<Session> with(const Session& s, Modulation m, Id* id = nullptr) {
    EditTxn t(s, s.version + 1);
    const Id made = t.create(std::move(m));
    if (id) *id = made;
    return t.commit();
}

std::string invariantPath(const Result<Session>& r) {
    REQUIRE_FALSE(r);
    return r.error().path;
}

}  // namespace

TEST_CASE("modulation: a route saves, reopens and serializes the same") {
    Fixture f = fixture();
    Id id;
    auto s = with(f.s, route(f.modOut, f.fx, "cutoff", 0.25), &id);
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    CHECK(id == Id{Kind::Modulation, 1});
    CHECK(id.str() == "mr_1");
    CHECK(Id::parse("mr_1") == id);
    CHECK(Id::parse("mo_1")->kind == Kind::Module);

    const std::string bytes = serializeProject(*s);
    CHECK(bytes.find("\"supple.project/2.20\"") != std::string::npos);
    auto doc = json::parse(bytes).value();
    REQUIRE(doc.find("modulations"));
    json::Value& m = doc["modulations"]["mr_1"];
    CHECK(m["source"]["module"] == f.modOut.str());
    CHECK(m["source"]["input"] == "in_1");
    CHECK(m["target"]["object"] == f.fx.str());
    CHECK(m["target"]["param"] == "cutoff");
    CHECK(m["depth"] == 0.25);
    CHECK_FALSE(m.find("enabled"));  // defaults are omitted
    CHECK_FALSE(m.find("lane"));
    CHECK(doc["project"]["next_ids"]["mr"] == 2);
    // After the rack collections, before the blobs.
    CHECK(bytes.find("\"cables\"") < bytes.find("\"modulations\""));
    CHECK(bytes.find("\"modulations\"") < bytes.find("\"blobs\""));

    auto reopened = parseProject(bytes);
    REQUIRE_MESSAGE(reopened, (reopened ? "" : reopened.error().toString()));
    const Modulation* back = reopened->modulations.find(id);
    REQUIRE(back);
    CHECK(*back == *s->modulations.find(id));
    CHECK(reopened->next[Kind::Modulation] == 2);
    CHECK(serializeProject(*reopened) == bytes);

    // Non-default switches round-trip too.
    EditTxn t(*s, s->version + 1);
    t.modify<Modulation>(id, [](Modulation& x) {
        x.enabled = false;
        x.lane = false;
    });
    auto off = t.commit();
    REQUIRE(off);
    const std::string offBytes = serializeProject(*off);
    auto offBack = parseProject(offBytes);
    REQUIRE(offBack);
    CHECK_FALSE(offBack->modulations.find(id)->enabled);
    CHECK_FALSE(offBack->modulations.find(id)->lane);
    CHECK(serializeProject(*offBack) == offBytes);
}

TEST_CASE("modulation: a project without routes writes no section or counter") {
    Fixture f = fixture();
    const std::string bytes = serializeProject(f.s);
    auto doc = json::parse(bytes).value();
    CHECK_FALSE(doc.find("modulations"));
    CHECK_FALSE(doc["project"]["next_ids"].find("mr"));
    auto reopened = parseProject(bytes);
    REQUIRE(reopened);
    CHECK(reopened->next[Kind::Modulation] == 1);
    CHECK(serializeProject(*reopened) == bytes);
}

TEST_CASE("modulation: an older file loads with the counter at its start") {
    Fixture f = fixture();
    auto doc = json::parse(serializeProject(f.s)).value();
    doc["schema"] = "supple.project/2.15";
    auto reopened = parseProject(json::writeCompact(doc));
    REQUIRE_MESSAGE(reopened, (reopened ? "" : reopened.error().toString()));
    CHECK(reopened->modulations.empty());
    CHECK(reopened->next[Kind::Modulation] == 1);
    auto s = with(*reopened, route(f.modOut, f.fx, "cutoff", 0.5));
    REQUIRE(s);
    CHECK(s->modulations.contains({Kind::Modulation, 1}));
}

TEST_CASE("modulation: targets the invariants accept") {
    Fixture f = fixture();
    CHECK(with(f.s, route(f.modOut, f.fx, "cutoff", -1.0)));
    CHECK(with(f.s, route(f.modOut, f.inputFx, "1234", 1.0)));
    CHECK(with(f.s, route(f.modOut, f.b, "volume", -48.0)));
    CHECK(with(f.s, route(f.modOut, f.b, "pan", 200.0)));
    CHECK(with(f.s, route(f.modOut, f.buss, "width", -200.0)));
    CHECK(with(f.s, route(f.modOut, kMaster, "volume", 6.0)));
    CHECK(with(f.s, route(f.modOut, f.send, "volume", 12.0)));
    CHECK(with(f.s, route(f.modOut, f.fx, "cutoff", 0.5, "in_16")));
}

TEST_CASE("modulation: the invariants refuse a bad source") {
    Fixture f = fixture();
    CHECK(invariantPath(with(f.s, route(f.lfo, f.fx, "cutoff", 0.5))) ==
          "/modulations/mr_1/source");
    CHECK(invariantPath(with(f.s, route(f.modOut, f.fx, "cutoff", 0.5, "in_17"))) ==
          "/modulations/mr_1/source");
    CHECK(invariantPath(with(f.s, route(f.modOut, f.fx, "cutoff", 0.5, "in_0"))) ==
          "/modulations/mr_1/source");
    CHECK(invariantPath(with(f.s, route(f.modOut, f.fx, "cutoff", 0.5, "out"))) ==
          "/modulations/mr_1/source");
    CHECK(invariantPath(with(f.s, route({Kind::Module, 99}, f.fx, "cutoff", 0.5))) ==
          "/modulations/mr_1/source");
}

TEST_CASE("modulation: the invariants refuse a bad target or depth") {
    Fixture f = fixture();
    auto refused = [&](Modulation m, const std::string& field) {
        const auto r = with(f.s, std::move(m));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == "model.invariant");
        CHECK(r.error().path == "/modulations/mr_1/" + field);
    };
    refused(route(f.modOut, {Kind::Fx, 99}, "cutoff", 0.5), "target");
    refused(route(f.modOut, f.b, "mute", 1.0), "target");
    refused(route(f.modOut, f.b, "playrate", 1.0), "target");
    refused(route(f.modOut, kMaster, "width", 10.0), "target");  // a master has no width
    refused(route(f.modOut, f.send, "pan", 10.0), "target");
    refused(route(f.modOut, f.rack, "volume", 1.0), "target");
    refused(route(f.modOut, f.lfo, "rate", 1.0), "target");  // rack knobs: later
    refused(route(f.modOut, f.fx, "cutoff", 1.5), "depth");
    refused(route(f.modOut, f.b, "volume", 48.5), "depth");
    refused(route(f.modOut, f.b, "pan", -201.0), "depth");

    // A plug-in in a take's chain or a plugin module: later.
    EditTxn t(f.s, f.s.version + 1);
    RackModule plugin;
    plugin.rack = f.rack;
    plugin.type = std::string(kPluginModule);
    FxInstance hosted;
    hosted.plugin = {"clap", "test.filter", "", "", ""};
    plugin.fx = t.create(hosted);
    t.create(plugin);
    t.create(route(f.modOut, *plugin.fx, "cutoff", 0.5));
    const auto inModule = t.commit();
    REQUIRE_FALSE(inModule);
    CHECK(inModule.error().path.find("/target") != std::string::npos);

    // A MIDI route has no level to modulate.
    EditTxn m(f.s, f.s.version + 1);
    Route midi;
    midi.from.track = f.a;
    midi.to.track = f.b;
    midi.midi = MidiRouting{};
    const Id midiRoute = m.create(midi);
    m.create(route(f.modOut, midiRoute, "volume", 1.0));
    const auto midiResult = m.commit();
    REQUIRE_FALSE(midiResult);
    CHECK(midiResult.error().path.find("/target") != std::string::npos);
}

TEST_CASE("modulation: two routes with the same source and target are refused") {
    Fixture f = fixture();
    auto one = with(f.s, route(f.modOut, f.fx, "cutoff", 0.5));
    REQUIRE(one);
    CHECK(invariantPath(with(*one, route(f.modOut, f.fx, "cutoff", -0.5))) == "/modulations/mr_2");
    // Another input, or another parameter, is a different route.
    CHECK(with(*one, route(f.modOut, f.fx, "cutoff", 0.5, "in_2")));
    CHECK(with(*one, route(f.modOut, f.fx, "resonance", 0.5)));
}

TEST_CASE("modulation: a route that closes a cycle is refused with its id") {
    Fixture f = fixture();
    // Channel B sends into the rack's In: the rack hears B, so modulating
    // B's plug-in from it would feed back.
    EditTxn t(f.s, f.s.version + 1);
    RackModule in;
    in.rack = f.rack;
    in.type = std::string(kInModule);
    const Id inModule = t.create(in);
    Route send;
    send.from.track = f.b;
    send.to.module = inModule;
    t.create(send);
    auto fed = t.commit();
    REQUIRE_MESSAGE(fed, (fed ? "" : fed.error().toString()));

    const auto cycle = with(*fed, route(f.modOut, f.fx, "cutoff", 0.5));
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "modulation.cycle");
    CHECK(cycle.error().path == "/modulations/mr_1");
    CHECK(cycle.error().message.find("mr_1") != std::string::npos);
    CHECK_FALSE(cycle.error().hint.empty());
    // The input chain's plug-in is on the same node.
    const auto input = with(*fed, route(f.modOut, f.inputFx, "cutoff", 0.5));
    REQUIRE_FALSE(input);
    CHECK(input.error().code == "modulation.cycle");
    // Its own home's fader is no cycle: nothing the rack hears comes from A.
    CHECK(with(*fed, route(f.modOut, f.a, "volume", 3.0)));
    // A rack send's level is applied at the rack: a cycle of one node.
    EditTxn r(*fed, fed->version + 1);
    RackModule out;
    out.rack = f.rack;
    out.type = std::string(kOutModule);
    const Id outModule = r.create(out);
    Route rackSend;
    rackSend.from.module = outModule;
    rackSend.to.track = f.buss;
    const Id rackSendId = r.create(rackSend);
    r.create(route(f.modOut, rackSendId, "volume", 3.0));
    const auto self = r.commit();
    REQUIRE_FALSE(self);
    CHECK(self.error().code == "modulation.cycle");
}

TEST_CASE("modulation: removing its source or target removes the route") {
    Fixture f = fixture();
    Id onFx, onTrack, onSend;
    auto s = with(f.s, route(f.modOut, f.fx, "cutoff", 0.5), &onFx);
    REQUIRE(s);
    s = with(*s, route(f.modOut, f.b, "volume", 3.0), &onTrack);
    REQUIRE(s);
    s = with(*s, route(f.modOut, f.send, "volume", 3.0), &onSend);
    REQUIRE(s);

    auto removed = [](const EditTxn& t, Id id) {
        for (const Change& c : t.changes())
            if (c.id == id && c.op == Change::Op::Remove) return true;
        return false;
    };
    {
        EditTxn t(*s, s->version + 1);
        t.modify<Track>(f.b, [&](Track& tr) { tr.chain.clear(); });
        t.remove<FxInstance>(f.fx);
        CHECK(removed(t, onFx));
        CHECK_FALSE(removed(t, onTrack));
        REQUIRE(t.commit());
    }
    {
        EditTxn t(*s, s->version + 1);
        t.remove<Route>(f.send);
        CHECK(removed(t, onSend));
        CHECK_FALSE(removed(t, onFx));
        REQUIRE(t.commit());
    }
    {
        EditTxn t(*s, s->version + 1);
        t.remove<RackModule>(f.modOut);
        for (const auto& e : s->cables) t.remove<Cable>(e.id);
        CHECK(removed(t, onFx));
        CHECK(removed(t, onTrack));
        CHECK(removed(t, onSend));
        auto after = t.commit();
        REQUIRE_MESSAGE(after, (after ? "" : after.error().toString()));
        CHECK(after->modulations.empty());
    }
    {
        // A route made in the same transaction goes with its target too.
        EditTxn t(*s, s->version + 1);
        const Id fresh = t.create(route(f.modOut, f.fx, "resonance", 0.1));
        t.modify<Track>(f.b, [&](Track& tr) { tr.chain.clear(); });
        t.remove<FxInstance>(f.fx);
        CHECK(removed(t, onFx));
        CHECK_FALSE(t.exists<Modulation>(fresh));
        REQUIRE(t.commit());
    }
}

TEST_CASE("modulation: at most 4096 routes") {
    Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    for (std::size_t i = 0; i < kMaxModulations; ++i)
        t.create(route(f.modOut, f.fx, std::to_string(i), 0.1));
    auto full = t.commit();
    REQUIRE_MESSAGE(full, (full ? "" : full.error().toString()));
    CHECK(invariantPath(with(*full, route(f.modOut, f.fx, "one more", 0.1))) == "/modulations");
}

TEST_CASE("modulation: 32 Mod Out modules per rack") {
    Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    RackModule modOut;
    modOut.rack = f.rack;
    modOut.type = std::string(kModOutModule);
    for (std::size_t i = 1; i < kMaxRackModOuts; ++i) t.create(modOut);
    auto full = t.commit();
    REQUIRE_MESSAGE(full, (full ? "" : full.error().toString()));
    EditTxn more(*full, full->version + 1);
    more.create(modOut);
    auto over = more.commit();
    REQUIRE_FALSE(over);
    CHECK(over.error().path == "/racks/" + f.rack.str());
}

TEST_CASE("modulation: routes into a track's plug-ins are part of its freeze hash") {
    Fixture f = fixture();
    const auto hash = freezeDependencyHash(f.s, f.b);
    Id id;
    auto s = with(f.s, route(f.modOut, f.fx, "cutoff", 0.5), &id);
    REQUIRE(s);
    const auto modulated = freezeDependencyHash(*s, f.b);
    CHECK(modulated != hash);
    // The source track's print does not change.
    CHECK(freezeDependencyHash(*s, f.a) == freezeDependencyHash(f.s, f.a));
    // Depth and enabled are sound; the lane is not.
    {
        EditTxn t(*s, s->version + 1);
        t.modify<Modulation>(id, [](Modulation& m) { m.depth = 0.75; });
        CHECK(freezeDependencyHash(*t.commit(), f.b) != modulated);
    }
    {
        EditTxn t(*s, s->version + 1);
        t.modify<Modulation>(id, [](Modulation& m) { m.enabled = false; });
        CHECK(freezeDependencyHash(*t.commit(), f.b) != modulated);
    }
    {
        EditTxn t(*s, s->version + 1);
        t.modify<Modulation>(id, [](Modulation& m) { m.lane = false; });
        CHECK(freezeDependencyHash(*t.commit(), f.b) == modulated);
    }
    // The fader acts after the print, so modulating it is no dependency.
    auto fader = with(f.s, route(f.modOut, f.b, "volume", 3.0));
    REQUIRE(fader);
    CHECK(freezeDependencyHash(*fader, f.b) == hash);
}

TEST_CASE("modulation: diffs list route changes by field") {
    Fixture f = fixture();
    Id id;
    auto s = with(f.s, route(f.modOut, f.fx, "cutoff", 0.5), &id);
    REQUIRE(s);
    auto added = diff(f.s, *s);
    REQUIRE(added.size() == 1);
    CHECK(added[0].op == Change::Op::Add);
    CHECK(added[0].id == id);
    EditTxn t(*s, s->version + 1);
    t.modify<Modulation>(id, [](Modulation& m) { m.depth = -0.5; });
    auto changed = t.commit();
    REQUIRE(changed);
    auto updated = diff(*s, *changed);
    REQUIRE(updated.size() == 1);
    CHECK(updated[0].op == Change::Op::Update);
    CHECK(updated[0].fields == std::vector<std::string>{"depth"});
}

TEST_CASE("modulation: a file that went through an older build still opens") {
    Fixture f = fixture();
    // mr_1 targets the plug-in an older build deleted; mr_2 stays valid;
    // mr_3 lost its Mod Out input's module; mr_4 closes a cycle the older
    // build made with a send from B into the rack's In.
    EditTxn t(f.s, f.s.version + 1);
    t.create(route(f.modOut, f.fx, "cutoff", 0.5));
    t.create(route(f.modOut, f.a, "volume", 3.0));
    RackModule spare;
    spare.rack = f.rack;
    spare.type = std::string(kModOutModule);
    const Id spareId = t.create(spare);
    t.create(route(spareId, f.inputFx, "cutoff", 0.5));
    t.create(route(f.modOut, f.inputFx, "drive", 0.5));
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    auto doc = json::parse(serializeProject(*s)).value();

    // What the older build wrote: no mr counter, mr_1's plug-in and the
    // spare Mod Out gone, a send from B into a new In module.
    doc["project"]["next_ids"].asObject().erase("mr");
    doc["fx"].asObject().erase(f.fx.str());
    doc["tracks"][f.b.str()].asObject().erase("chain");
    doc["modules"].asObject().erase(spareId.str());
    const Id inModule{Kind::Module, s->next[Kind::Module]};
    doc["modules"][inModule.str()] =
        json::parse(R"({"rack": ")" + f.rack.str() + R"(", "type": ")" + std::string(kInModule) +
                    R"("})")
            .value();
    doc["project"]["next_ids"]["mo"] = static_cast<int>(inModule.n + 1);
    const Id send{Kind::Route, s->next[Kind::Route]};
    doc["routes"][send.str()] = json::parse(R"({"from": {"track": ")" + f.b.str() +
                                            R"("}, "to": {"module": ")" + inModule.str() + R"("}})")
                                    .value();
    doc["project"]["next_ids"]["sd"] = static_cast<int>(send.n + 1);

    std::vector<schema::Warning> warnings;
    auto opened = parseProject(json::writeCompact(doc), &warnings);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    CHECK(opened->modulations.size() == 1);
    CHECK(opened->modulations.contains({Kind::Modulation, 2}));
    CHECK(opened->next[Kind::Modulation] == 5);  // past the highest id the file held
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const auto& w) {
        return w.code == "project.counter_derived" && w.path == "/project/next_ids/mr";
    }));
    std::vector<std::string> dropped;
    for (const auto& w : warnings)
        if (w.code == "project.modulation_dropped") dropped.push_back(w.path);
    CHECK(dropped ==
          std::vector<std::string>{"/modulations/mr_1", "/modulations/mr_3", "/modulations/mr_4"});
    for (const auto& w : warnings)
        if (w.code == "project.modulation_dropped")
            CHECK(w.message.find(w.path.substr(std::string("/modulations/").size())) !=
                  std::string::npos);
    // A new route takes a fresh id.
    auto more = with(*opened, route(f.modOut, f.a, "pan", 10.0));
    REQUIRE(more);
    CHECK(more->modulations.contains({Kind::Modulation, 5}));
}

TEST_CASE("modulation: a low mr counter is raised to the highest id") {
    Fixture f = fixture();
    auto s = with(f.s, route(f.modOut, f.fx, "cutoff", 0.5));
    REQUIRE(s);
    s = with(*s, route(f.modOut, f.b, "volume", 1.0));
    REQUIRE(s);
    auto doc = json::parse(serializeProject(*s)).value();
    doc["project"]["next_ids"]["mr"] = 1;
    std::vector<schema::Warning> warnings;
    auto opened = parseProject(json::writeCompact(doc), &warnings);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    CHECK(opened->next[Kind::Modulation] == 3);
    CHECK(opened->modulations.size() == 2);
    // The same code as ADR-0095's cx repair, so agents read one warning.
    const auto derived = std::count_if(warnings.begin(), warnings.end(), [](const auto& w) {
        return w.code == "project.counter_derived" && w.path == "/project/next_ids/mr";
    });
    CHECK(derived == 1);
    // A counter that is right says nothing.
    warnings.clear();
    REQUIRE(parseProject(serializeProject(*s), &warnings));
    for (const auto& w : warnings) CHECK(w.code != "project.counter_derived");
}

TEST_CASE("modulation: a rack modulating its own rack send's level is told why") {
    Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    RackModule out;
    out.rack = f.rack;
    out.type = std::string(kOutModule);
    const Id outModule = t.create(out);
    Route rackSend;
    rackSend.from.module = outModule;
    rackSend.to.track = f.buss;
    const Id rackSendId = t.create(rackSend);
    t.create(route(f.modOut, rackSendId, "volume", 3.0));
    const auto self = t.commit();
    REQUIRE_FALSE(self);
    CHECK(self.error().code == "modulation.cycle");
    CHECK(self.error().hint.find("rack send") != std::string::npos);
}

TEST_CASE("modulation: a frozen target keeps its routes locked, but their source can go") {
    // Review round 52, F4: the source lies outside the print, as a
    // sidechain's does (ADR-0096 "Freeze").
    Fixture f = fixture();
    Id id;
    auto modulated = with(f.s, route(f.modOut, f.fx, "cutoff", 0.5), &id);
    REQUIRE(modulated);
    EditTxn freeze(*modulated, modulated->version + 1);
    const Id media = freeze.create(MediaSource{"freeze.wav", "sha256:" + std::string(64, '1'),
                                               "wav", modulated->project->sampleRate, 2, 96000});
    const TrackFreeze cache{media, 48000, 48000, 0, freezeDependencyHash(*modulated, f.b)};
    freeze.modify<Track>(f.b, [&](Track& t) { t.freeze = cache; });
    auto frozen = freeze.commit();
    REQUIRE_MESSAGE(frozen, (frozen ? "" : frozen.error().toString()));
    // Editing or removing the route itself changes the frozen track's sound.
    {
        EditTxn t(*frozen, frozen->version + 1);
        t.modify<Modulation>(id, [](Modulation& m) { m.depth = 0.75; });
        auto s = t.commit();
        REQUIRE_FALSE(s);
        CHECK(s.error().code == "track.frozen");
    }
    {
        EditTxn t(*frozen, frozen->version + 1);
        t.remove<Modulation>(id);
        auto s = t.commit();
        REQUIRE_FALSE(s);
        CHECK(s.error().code == "track.frozen");
    }
    // Removing the source module elsewhere takes the route with it.
    {
        EditTxn t(*frozen, frozen->version + 1);
        for (const auto& c : frozen->cables)
            if (c.value->to.object == f.modOut) t.remove<Cable>(c.id);
        t.remove<RackModule>(f.modOut);
        auto s = t.commit();
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        CHECK(s->modulations.empty());
        CHECK(s->tracks.find(f.b)->freeze);
    }
    // So does removing the source's home track, with its rack and send, as
    // track.remove does.
    {
        EditTxn t(*frozen, frozen->version + 1);
        for (const auto& c : frozen->cables) t.remove<Cable>(c.id);
        t.remove<RackModule>(f.lfo);
        t.remove<RackModule>(f.modOut);
        t.remove<Rack>(f.rack);
        t.remove<Route>(f.send);
        t.remove<Track>(f.a);
        t.modify<Track>(kMaster, [&](Track& m) { std::erase(m.children, f.a); });
        auto s = t.commit();
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        CHECK(s->modulations.empty());
    }
}

TEST_CASE("modulation: a branch whose saved counters lost mr still allocates fresh ids") {
    // Review round 53, Q1: a file that went through an older build keeps the
    // routes but its branches' next_ids lose the mr counter. A branch head's
    // counters come from those (VersionStore::head), not from the document.
    Fixture f = fixture();
    VersionStore vs(f.s);
    const auto add = [&](Id target, const char* param) {
        EditTxn t(*vs.head(), vs.nextVersion());
        const Id id = t.create(route(f.modOut, target, param, 0.5));
        auto changes = t.changes();
        auto s = t.commit();
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        REQUIRE(vs.commit(std::move(*s), TxnRecord{"modulation.add", "test", {}, {}},
                          std::move(changes), {id}));
        return id;
    };
    const Id onMain = add(f.fx, "cutoff");
    REQUIRE(vs.createBranch("agent/claude/wobble", "main"));
    REQUIRE(vs.switchBranch("agent/claude/wobble"));
    const Id onBranch = add(f.b, "volume");
    REQUIRE(vs.switchBranch("main"));
    auto doc = json::parse(serializeProjectFile(vs, true)).value();
    doc["project"]["next_ids"].asObject().erase("mr");
    for (auto& ref : doc["branches"]["refs"].asObject())
        ref.value["next_ids"].asObject().erase("mr");
    // An older build that keeps branches writes main's hash for what it wrote.
    auto main = doc;
    main.asObject().erase("branches");
    doc["branches"]["main_hash"] = "sha256:" + sha256Hex(json::writeCanonical(main));
    auto opened = projectFileFromJson(doc);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    REQUIRE_MESSAGE(!opened->branchesDropped,
                    (opened->branchesDropped ? opened->branchesDropped->toString() : ""));
    VersionStore& back = opened->store;
    for (const char* branch : {"main", "agent/claude/wobble"}) {
        CAPTURE(branch);
        const auto head = back.head(branch);
        REQUIRE(head);
        for (const auto& e : head->modulations) CHECK(head->next[Kind::Modulation] > e.id.n);
    }
    CHECK(back.head("main")->modulations.contains(onMain));
    CHECK(back.head("agent/claude/wobble")->modulations.contains(onBranch));
}

TEST_CASE("modulation: a lost mr counter never reuses an id another branch holds") {
    // An older build drops the high-water mark: main removed mr_2 after the
    // branch forked with it, so main's own routes alone would give mr_2 out
    // again, a different route under the branch's id, which a merge would
    // take for the same one.
    Fixture f = fixture();
    VersionStore vs(f.s);
    const auto step = [&](auto&& edit) {
        EditTxn t(*vs.head(), vs.nextVersion());
        std::vector<Id> created;
        edit(t, created);
        auto changes = t.changes();
        auto s = t.commit();
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        REQUIRE(vs.commit(std::move(*s), TxnRecord{"modulation", "test", {}, {}},
                          std::move(changes), std::move(created)));
    };
    Id second;
    step([&](EditTxn& t, std::vector<Id>& c) {
        c.push_back(t.create(route(f.modOut, f.fx, "cutoff", 0.5)));
        second = t.create(route(f.modOut, f.b, "volume", 1.0));
        c.push_back(second);
    });
    REQUIRE(vs.createBranch("agent/claude/keep", "main"));
    step([&](EditTxn& t, std::vector<Id>&) { t.remove<Modulation>(second); });
    auto doc = json::parse(serializeProjectFile(vs, true)).value();
    doc["project"]["next_ids"].asObject().erase("mr");
    for (auto& ref : doc["branches"]["refs"].asObject())
        ref.value["next_ids"].asObject().erase("mr");
    auto main = doc;
    main.asObject().erase("branches");
    doc["branches"]["main_hash"] = "sha256:" + sha256Hex(json::writeCanonical(main));
    auto opened = projectFileFromJson(doc);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    REQUIRE_MESSAGE(!opened->branchesDropped,
                    (opened->branchesDropped ? opened->branchesDropped->toString() : ""));
    CHECK(opened->store.head("agent/claude/keep")->modulations.contains(second));
    CHECK(opened->store.head("main")->next[Kind::Modulation] > second.n);
}
