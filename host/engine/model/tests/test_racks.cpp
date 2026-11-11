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
    CHECK(bytes.find("\"supple.project/2.10\"") != std::string::npos);
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

TEST_CASE("MIDI sends: channel endpoints, Notes source off and old projects round trip") {
    Patch p = patch();
    EditTxn t(p.s, 2);
    Route route;
    route.from.track = p.other;
    route.to.track = p.channel;
    route.midi = MidiRouting{2, 5};
    const auto send = t.create(route);
    t.modify<RackModule>(p.notes, [](auto& m) { m.sourceOff = true; });
    auto s = t.commit();
    REQUIRE(s);
    const auto bytes = serializeProject(*s);
    CHECK(bytes.find("supple.project/2.10") != std::string::npos);
    auto reopened = parseProject(bytes);
    REQUIRE(reopened);
    CHECK(reopened->modules.find(p.notes)->sourceOff);
    CHECK(reopened->routes.find(send)->midi->toChannel == 5);
    CHECK(serializeProject(*reopened) == bytes);
    auto old = projectToJson(p.s);
    old["schema"] = "supple.project/2.9";
    auto loaded = projectFromJson(old);
    REQUIRE(loaded);
    CHECK_FALSE(loaded->modules.find(p.notes)->sourceOff);
    CHECK(serializeProject(*loaded) == serializeProject(p.s));
}

TEST_CASE("MIDI sends: a Note Out may play its own Notes source; older projects with one open") {
    // The arpeggiator on its own track: the rack's Notes reads the home
    // channel, its Note Out plays that channel's instrument. The Notes module
    // does not hear its own rack's Note Out, so this is no cycle.
    Patch p = patch();
    EditTxn t(p.s, 2);
    RackModule noteOut;
    noteOut.rack = p.rack;
    noteOut.type = std::string(kNotesOutModule);
    noteOut.version = "1.0.0";
    const Id output = t.create(noteOut);
    t.create(Cable{{p.notes, "gate"}, {output, "gate"}, std::nullopt, ""});
    Route back;
    back.from.module = output;
    back.to.track = p.channel;
    back.midi = MidiRouting{};
    t.create(back);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    auto old = projectToJson(*s);
    old["schema"] = "supple.project/2.9";
    auto loaded = projectFromJson(old);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
    CHECK(serializeProject(*loaded) == serializeProject(*s));
    // Another rack's Notes still hears it: two racks playing each other's
    // Notes source tracks feed back.
    EditTxn cross(*s, 3);
    const Id rack = cross.create(Rack{p.other, "Echo"});
    RackModule notes;
    notes.rack = rack;
    notes.type = std::string(kNotesModule);
    notes.version = "1.0.0";
    cross.create(notes);
    noteOut.rack = rack;
    const Id echo = cross.create(noteOut);
    back.from.module = output;
    back.to.track = p.other;
    cross.create(back);
    back.from.module = echo;
    back.to.track = p.channel;
    cross.create(back);
    const auto refused = cross.commit();
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "route.cycle");
}

TEST_CASE("MIDI sends: model rejects invalid endpoints, audio controls, learn and mixed cycles") {
    Patch p = patch();
    Route send;
    send.from.track = p.other;
    send.to.track = p.channel;
    send.midi = MidiRouting{};
    auto valid = [&](Route route) {
        EditTxn t(p.s, 2);
        t.create(route);
        return t.commit();
    };
    SUBCASE("channel to channel") { CHECK(valid(send)); }
    SUBCASE("channel to Notes") {
        send.to = {};
        send.to.module = p.notes;
        CHECK(valid(send));
    }
    SUBCASE("master source") {
        send.from.track = kMaster;
        CHECK_FALSE(valid(send));
    }
    SUBCASE("master destination") {
        send.to.track = kMaster;
        CHECK_FALSE(valid(send));
    }
    SUBCASE("In destination") {
        send.to = {};
        send.to.module = p.in;
        CHECK_FALSE(valid(send));
    }
    SUBCASE("audio level") {
        send.volumeDb = -3;
        CHECK_FALSE(valid(send));
    }
    SUBCASE("audio channels") {
        send.channels = {{1}, {2}};
        CHECK_FALSE(valid(send));
    }
    SUBCASE("audio pan") {
        send.pan.percent = 0;
        CHECK_FALSE(valid(send));
    }
    SUBCASE("learn tap") {
        send.learn.push_back({{}, "tap:pre_fx"});
        CHECK_FALSE(valid(send));
    }
    SUBCASE("mixed cycle") {
        // Other -> rack (audio), rack -> home (audio), home -> other (MIDI).
        send.from.track = p.channel;
        send.to.track = p.other;
        CHECK_FALSE(valid(send));
    }
    SUBCASE("implicit Notes reader cycle") {
        // Home -> Notes on home: its rack already feeds home with audio.
        send.from.track = p.channel;
        send.to = {};
        send.to.module = p.notes;
        CHECK_FALSE(valid(send));
    }
}
