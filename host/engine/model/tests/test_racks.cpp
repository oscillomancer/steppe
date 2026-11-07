// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Racks, modules and cables in the session (ADR-0062, rack.md §2).
#include <doctest/doctest.h>
#include <supple/model/schemas.h>
#include <supple/model/versions.h>

using namespace supple;
using namespace supple::model;

namespace {

struct Patch {
    Session s;
    Id channel, other, rack, notes, vco, out, in;
};

Id addChannel(EditTxn& t) {
    Track tr;
    tr.parent = kMaster;
    const Id id = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(id); });
    return id;
}

// A channel with a rack (Notes -> VCO -> Out), the Out as its input, and a
// second channel that sends into the rack's In module.
Patch patch() {
    Patch p;
    EditTxn t(Session::create(), 1);
    p.channel = addChannel(t);
    p.other = addChannel(t);
    p.rack = t.create(Rack{p.channel, "Bells"});
    RackModule notes;
    notes.rack = p.rack;
    notes.type = std::string(kNotesModule);
    notes.version = "1.0.0";
    p.notes = t.create(notes);
    RackModule vco;
    vco.rack = p.rack;
    vco.type = "com.suppleaudio.rack.vco";
    vco.version = "1.0.0";
    vco.x = 180;
    vco.y = 60;
    vco.params = json::parse(R"({"shape": "saw", "fine": -7})").value();
    p.vco = t.create(vco);
    RackModule out;
    out.rack = p.rack;
    out.type = std::string(kOutModule);
    out.x = 520;
    p.out = t.create(out);
    RackModule in;
    in.rack = p.rack;
    in.type = std::string(kInModule);
    p.in = t.create(in);
    t.create(Cable{{p.notes, "pitch"}, {p.vco, "pitch"}, std::nullopt, ""});
    t.create(Cable{{p.vco, "saw"}, {p.out, "l"}, std::nullopt, ""});
    t.create(Cable{{p.notes, "modwheel"}, {p.vco, "fine"}, 12.0, "violet"});
    Route input;
    input.from.module = p.out;
    input.to.track = p.channel;
    const Id route = t.create(input);
    t.modify<Track>(p.channel, [&](Track& tr) { tr.record.input = route; });
    Route send;
    send.from.track = p.other;
    send.to.module = p.in;
    t.create(send);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().message));
    p.s = *s;
    return p;
}

}  // namespace

TEST_CASE("racks: a patch saves, reopens and serializes the same") {
    Patch p = patch();
    const std::string bytes = serializeProject(p.s);
    CHECK(bytes.find("\"supple.project/2.8\"") != std::string::npos);
    CHECK(bytes.find("\"mo_2.saw\"") != std::string::npos);
    auto loaded = parseProject(bytes);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().message));
    CHECK(serializeProject(*loaded) == bytes);
    CHECK(loaded->racks.size() == 1);
    CHECK(loaded->modules.size() == 4);
    CHECK(loaded->cables.size() == 3);
    const Cable* mod = loaded->cables.find({Kind::Cable, 3});
    REQUIRE(mod);
    CHECK(mod->amount == 12.0);
    CHECK(mod->to.str() == "mo_2.fine");
    CHECK(moduleLabel(*loaded, p.out) == "Out 1");
    CHECK(moduleLabel(*loaded, p.in) == "In 1");
    CHECK(rackLabel(*loaded, p.rack) == "Bells");
    CHECK(rackOfEnd(*loaded, mod->from) == p.rack);
}

TEST_CASE("racks: a project without racks writes no rack sections or counters") {
    const std::string bytes = serializeProject(Session::create());
    CHECK(bytes.find("\"racks\"") == std::string::npos);
    CHECK(bytes.find("\"rk\"") == std::string::npos);
    CHECK(bytes.find("\"cb\"") == std::string::npos);
    auto doc = projectToJson(Session::create());
    doc["schema"] = "supple.project/2.6";
    std::vector<schema::Warning> warnings;
    REQUIRE(projectFromJson(doc, &warnings));
    CHECK(warnings.empty());
    auto loaded = projectFromJson(doc);
    REQUIRE(loaded);
    CHECK(loaded->next[Kind::Rack] == 1);
}

TEST_CASE("racks: invariants refuse broken patches") {
    Patch p = patch();
    auto refused = [&](auto&& edit, std::string_view words) {
        EditTxn t(p.s, 2);
        edit(t);
        auto r = t.commit();
        REQUIRE_FALSE(r);
        const std::string all = r.error().message + [&] {
            std::string d;
            for (const auto& x : r.error().details) d += " | " + x.message;
            return d;
        }();
        CHECK_MESSAGE(all.find(words) != std::string::npos, all);
    };
    SUBCASE("a rack on a buss") {
        refused(
            [&](EditTxn& t) {
                Track buss;
                buss.type = StripType::Buss;
                buss.parent = kMaster;
                const Id b = t.create(buss);
                t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(b); });
                t.create(Rack{b, ""});
            },
            "a rack's home is a channel");
    }
    SUBCASE("a cable across racks") {
        refused(
            [&](EditTxn& t) {
                const Id second = t.create(Rack{p.other, ""});
                RackModule m;
                m.rack = second;
                m.type = "com.suppleaudio.rack.vca";
                const Id vca = t.create(m);
                t.create(Cable{{p.vco, "saw"}, {vca, "in"}, std::nullopt, ""});
            },
            "one rack");
    }
    SUBCASE("two cables between the same ends") {
        refused(
            [&](EditTxn& t) { t.create(Cable{{p.vco, "saw"}, {p.out, "l"}, std::nullopt, ""}); },
            "another cable");
    }
    SUBCASE("a plugin module without a plug-in") {
        refused(
            [&](EditTxn& t) {
                RackModule m;
                m.rack = p.rack;
                m.type = std::string(kPluginModule);
                t.create(m);
            },
            "holds a plug-in");
    }
    SUBCASE("an Out module routed to a buss") {
        refused(
            [&](EditTxn& t) {
                Track buss;
                buss.type = StripType::Buss;
                buss.parent = kMaster;
                const Id b = t.create(buss);
                t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(b); });
                Route r;
                r.from.module = p.out;
                r.to.track = b;
                t.create(r);
            },
            "Out module");
    }
    SUBCASE("a track that feeds the rack hears it") {
        // ch 2 sends into the rack's In; feeding ch 2 from the rack's Out
        // closes the loop.
        refused(
            [&](EditTxn& t) {
                Route r;
                r.from.module = p.out;
                r.to.track = p.other;
                const Id route = t.create(r);
                t.modify<Track>(p.other, [&](Track& tr) { tr.record.input = route; });
            },
            "routing cycle");
    }
    SUBCASE("a module far away") {
        refused([&](EditTxn& t) { t.modify<RackModule>(p.vco, [](RackModule& m) { m.x = 2e6; }); },
                "within");
    }
}

TEST_CASE("racks: an envelope can automate a module's knob") {
    Patch p = patch();
    EditTxn t(p.s, 2);
    Envelope env;
    env.target = {p.vco, "fine"};
    env.points = {{0, -7}, {4, 7}};
    t.create(env);
    REQUIRE(t.commit());
}

TEST_CASE("racks: cable ends parse strictly") {
    CHECK(CableEnd::parse("mo_3.out")->key == "out");
    CHECK(CableEnd::parse("fx_9.cutoff")->object == Id{Kind::Fx, 9});
    CHECK_FALSE(CableEnd::parse("tr_1.out"));
    CHECK_FALSE(CableEnd::parse("mo_3"));
    CHECK_FALSE(CableEnd::parse("mo_3."));
    CHECK_FALSE(CableEnd::parse("mo_3.Out"));
    CHECK_FALSE(CableEnd::parse("mo_3.1x"));
    auto doc = projectToJson(patch().s);
    doc["cables"]["cb_1"]["to"] = "mo_2";
    auto bad = projectFromJson(doc);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().path == "/cables/cb_1/to");
}

TEST_CASE("racks: merges remap the module ids inside cable ends") {
    const json::Value schema = json::parse(R"({"type": "object", "properties": {
        "from": {"type": "string", "pattern": "^(mo|fx)_[0-9]+\\.[a-z][a-z0-9_]*$"},
        "module": {"type": "string", "pattern": "^mo_[0-9]+$"}}})")
                                   .value();
    const auto args = json::parse(R"({"from": "mo_3.out", "module": "mo_3"})").value();
    auto out = remapIds(args, schema, {{Id{Kind::Module, 3}, Id{Kind::Module, 7}}});
    CHECK(out["from"] == "mo_7.out");
    CHECK(out["module"] == "mo_7");
}

TEST_CASE("rack model: numeric CLAP cable targets round trip and preserve module key rules") {
    for (const auto* text : {"fx_3.0", "fx_3.12", "fx_3.4294967295", "fx_3.gain", "mo_3.out"}) {
        auto parsed = CableEnd::parse(text);
        REQUIRE(parsed);
        CHECK(parsed->str() == text);
        auto decoded = cableEndSchema().decode(json::Value(text));
        REQUIRE(decoded);
        CHECK(cableEndSchema().encode(*decoded).asString() == text);
    }
    for (const auto* text : {"mo_3.12", "fx_3.-1", "fx_3.+12", "fx_3.12gain", "fx_3.0x12", "fx_3."})
        CHECK_FALSE(CableEnd::parse(text));
}

TEST_CASE("rack model: numeric cable target remapping keeps the parameter id") {
    auto args = *json::parse(R"({"target":"fx_3.12"})");
    auto schema = *json::parse(R"({"type":"object","properties":{"target":{"type":"string"}}})");
    schema["properties"]["target"]["pattern"] = std::string(kCableEndPattern);
    auto remapped = remapIds(args, schema, {{Id{Kind::Fx, 3}, Id{Kind::Fx, 9}}});
    CHECK(remapped["target"].asString() == "fx_9.12");
}
