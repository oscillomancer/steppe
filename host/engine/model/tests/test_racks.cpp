// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Racks, modules and cables in the session (ADR-0062, rack.md §2).
#include <doctest/doctest.h>
#include <supple/model/groups.h>
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
    CHECK(bytes.find("\"supple.project/2.13\"") != std::string::npos);
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
    SUBCASE("a rack send with a strip send's controls") {
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
                r.pan = 30.0;
                t.create(r);
            },
            "level and mute only");
    }
    SUBCASE("an Out module routed to a channel it is not the input of") {
        refused(
            [&](EditTxn& t) {
                Route r;
                r.from.module = p.out;
                r.to.track = p.other;
                t.create(r);
            },
            "record input");
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

TEST_CASE("racks: a rack send to a buss saves, reopens and follows its home (ADR-0075)") {
    Patch p = patch();
    EditTxn t(p.s, 2);
    Track buss;
    buss.type = StripType::Buss;
    buss.parent = kMaster;
    const Id b = t.create(buss);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(b); });
    Route send;
    send.from.module = p.out;
    send.to.track = b;
    send.volumeDb = -6;
    send.mute = true;
    const Id id = t.create(send);
    // A rack send may learn level and mute.
    auto learn = [](std::string param, int cc) {
        ControlLearn l;
        l.param = std::move(param);
        l.control.number = cc;
        return l;
    };
    t.modify<Route>(id, [&](Route& r) {
        r.learn.push_back(learn("volume", 20));
        r.learn.push_back(learn("mute", 21));
    });
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    CHECK(rackSendHome(*s, *s->routes.find(id)) == p.channel);
    // The Out stays its channel's input: that route is not a rack send.
    CHECK_FALSE(rackSendHome(*s, *s->routes.find(*s->tracks.find(p.channel)->record.input)));
    const std::string bytes = serializeProject(*s);
    CHECK(bytes.find("\"supple.project/2.13\"") != std::string::npos);
    auto loaded = parseProject(bytes);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
    CHECK(serializeProject(*loaded) == bytes);
    const Route* reopened = loaded->routes.find(id);
    REQUIRE(reopened);
    CHECK(reopened->volumeDb == -6);
    CHECK(reopened->mute);
    CHECK(reopened->from.module == p.out);
    CHECK(reopened->to.track == b);
    // Solo: soloing the buss keeps the home channel (the send's source) audible.
    EditTxn solo(*loaded, 3);
    solo.modify<Track>(b, [](Track& tr) { tr.solo = Solo::On; });
    auto soloed = solo.commit();
    REQUIRE(soloed);
    CHECK_FALSE(effectiveTrackLevel(*soloed, p.channel, 0).soloSilenced);
    // Other learned controls are refused.
    EditTxn pan(*loaded, 3);
    pan.modify<Route>(id, [&](Route& r) { r.learn.push_back(learn("pan", 22)); });
    CHECK_FALSE(pan.commit());
    // A send from the buss into the rack's In closes a loop through the rack.
    EditTxn loop(*loaded, 3);
    Route back;
    back.from.track = b;
    back.to.module = p.in;
    loop.create(back);
    auto cycle = loop.commit();
    REQUIRE_FALSE(cycle);
    std::string all = cycle.error().message;
    for (const auto& x : cycle.error().details) all += " | " + x.message;
    CHECK_MESSAGE(all.find("routing cycle") != std::string::npos, all);
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
    CHECK(bytes.find("supple.project/2.13") != std::string::npos);
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
    // Before 2.10 its Notes heard no sends into the channel: it still does not.
    CHECK_FALSE(loaded->modules.find(p.notes)->sourceSends);
    EditTxn heard(*loaded, 3);
    heard.modify<RackModule>(p.notes, [](auto& m) { m.sourceSends = true; });
    auto same = heard.commit();
    REQUIRE(same);
    CHECK(serializeProject(*same) == serializeProject(*s));
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

TEST_CASE("MIDI sends: older Notes keep not hearing sends into their source track") {
    // Two racks, each Note Out playing the other rack's channel: valid before
    // 2.10, when a Notes module never heard what was sent into its source.
    EditTxn t(Session::create(), 1);
    const Id a = addChannel(t), b = addChannel(t), c = addChannel(t);
    auto rackOn = [&](Id track) {
        const Id rack = t.create(Rack{track, "R"});
        RackModule m;
        m.rack = rack;
        m.type = std::string(kNotesModule);
        m.version = "1.0.0";
        m.sourceSends = false;
        const Id notes = t.create(m);
        m.type = std::string(kNotesOutModule);
        m.sourceSends = true;
        const Id out = t.create(m);
        return std::pair{notes, out};
    };
    const auto [notesA, outA] = rackOn(a);
    const auto [notesB, outB] = rackOn(b);
    const auto [notesC, outC] = rackOn(c);
    t.modify<RackModule>(notesC, [](auto& m) { m.sourceSends = true; });
    Route send;
    send.midi = MidiRouting{};
    send.from.module = outA;
    send.to.track = b;
    t.create(send);
    send.from.module = outB;
    send.to.track = a;
    t.create(send);
    send.from.module = outC;  // into a channel nobody's Notes reads
    send.to.track = addChannel(t);
    t.create(send);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    // As a 2.9 writer saved it: no source_sends.
    auto old = projectToJson(*s);
    old["schema"] = "supple.project/2.9";
    for (auto& m : old["modules"].asObject()) m.value.asObject().erase("source_sends");
    std::vector<schema::Warning> warnings;
    auto loaded = projectFromJson(old, &warnings);
    REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
    CHECK_FALSE(loaded->modules.find(notesA)->sourceSends);
    CHECK_FALSE(loaded->modules.find(notesB)->sourceSends);
    CHECK(loaded->modules.find(notesC)->sourceSends);  // its channel receives no MIDI
    CHECK(warnings.empty());
    // Saved at 2.10 and reopened, unchanged.
    const auto bytes = serializeProject(*loaded);
    CHECK(bytes == serializeProject(*s));
    CHECK(bytes.find("\"source_sends\": false") != std::string::npos);
    auto reopened = parseProject(bytes);
    REQUIRE(reopened);
    CHECK(serializeProject(*reopened) == bytes);
    // A 2.10 file means what it says: hearing the sends is the loop.
    old["schema"] = "supple.project/2.10";
    auto current = projectFromJson(old);
    REQUIRE_FALSE(current);
    CHECK(current.error().code == "route.cycle");
    EditTxn one(*reopened, 2);
    one.modify<RackModule>(notesA, [](auto& m) { m.sourceSends = true; });
    auto half = one.commit();
    REQUIRE(half);  // one direction is no loop
    EditTxn both(*half, 3);
    both.modify<RackModule>(notesB, [](auto& m) { m.sourceSends = true; });
    const auto loop = both.commit();
    REQUIRE_FALSE(loop);
    CHECK(loop.error().code == "route.cycle");
    // Only a Notes module has the switch.
    EditTxn wrong(*reopened, 2);
    wrong.modify<RackModule>(outA, [](auto& m) { m.sourceSends = false; });
    CHECK_FALSE(wrong.commit());
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

TEST_CASE(
    "track inputs: current conflicts fail; old files retain rack, then instrument MIDI, then "
    "audio") {
    for (bool rackInput : {false, true})
        for (bool mainChain : {false, true}) {
            CAPTURE(rackInput);
            CAPTURE(mainChain);
            Patch p = patch();
            EditTxn tx(p.s, 2);
            tx.modify<Track>(p.channel, [](auto& t) { t.name = "Voice"; });
            const Id input = *p.s.tracks.find(p.channel)->record.input;
            if (!rackInput)
                tx.modify<Route>(input, [](auto& r) {
                    r.from = RouteEnd{};
                    r.from.hwChannels = std::vector<int>{1, 2};
                });
            if (mainChain) {
                FxInstance fx;
                fx.plugin.id = "com.example.instrument";
                const Id id = tx.create(fx);
                tx.modify<Track>(p.channel, [&](auto& t) { t.chain.push_back(id); });
            }
            auto base = tx.commit();
            REQUIRE(base);
            EditTxn invalid(*base, 3);
            Route midi;
            midi.from.midiDevice = "Keyboard";
            midi.to.track = p.channel;
            midi.midi = MidiRouting{};
            const Id midiId = invalid.create(midi);
            invalid.modify<Track>(p.channel, [&](auto& t) { t.record.midiInput = midiId; });
            auto refused = invalid.commit();
            REQUIRE_FALSE(refused);
            CHECK(refused.error().code == "model.invariant");
            CHECK(refused.error().path == "/tracks/" + p.channel.str() + "/record/input");
            CHECK_FALSE(refused.error().hint.empty());
            auto doc = projectToJson(*base);
            doc["tracks"][p.channel.str()]["record"]["midi_input"] = midiId.str();
            doc["routes"][midiId.str()] = toJson(midi);
            doc["project"]["next_ids"]["sd"] = static_cast<std::int64_t>(midiId.n + 1);
            // From 2.12 (ADR-0074) a conflict is invalid, not migrated; 2.13
            // (rack sends, ADR-0075) keeps that rule.
            for (const char* version : {"supple.project/2.12", "supple.project/2.13"}) {
                doc["schema"] = version;
                CHECK_FALSE(projectFromJson(doc));
            }
            const bool midiWins = !rackInput && mainChain;
            const Id dropped = midiWins ? input : midiId;
            Envelope env;
            env.target.object = dropped;
            env.target.param = "mute";
            const Id envId{Kind::Envelope, base->next[Kind::Envelope]};
            doc["envelopes"][envId.str()] = toJson(env);
            doc["project"]["next_ids"]["en"] = static_cast<std::int64_t>(envId.n + 1);
            AutomationItem automation;
            automation.envelope = envId;
            automation.length = 1;
            const Id autoId{Kind::AutoItem, base->next[Kind::AutoItem]};
            doc["automation_items"][autoId.str()] = toJson(automation);
            doc["project"]["next_ids"]["ai"] = static_cast<std::int64_t>(autoId.n + 1);
            for (const char* version :
                 {"supple.project/2.8", "supple.project/2.10", "supple.project/2.11"}) {
                doc["schema"] = version;
                const bool timecode = std::string_view(version) == "supple.project/2.11";
                if (timecode)
                    doc["project"]["timecode"] =
                        *json::parse(R"({"rate":"25","start":"01:00:00:00"})");
                else
                    doc["project"].asObject().erase("timecode");
                std::vector<schema::Warning> warnings;
                auto loaded = projectFromJson(doc, &warnings);
                REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
                if (timecode)
                    CHECK(loaded->project->timecode ==
                          sync::TimecodeMapping{sync::FrameRate::Fps25, {1, 0, 0, 0}});
                const bool keepMidi = !rackInput && mainChain;
                const auto& record = loaded->tracks.find(p.channel)->record;
                CHECK(record.input == (keepMidi ? midiId : input));
                CHECK(bool(record.midiInput) == keepMidi);
                CHECK_FALSE(loaded->routes.contains(keepMidi ? input : midiId));
                CHECK_FALSE(loaded->envelopes.contains(envId));
                CHECK_FALSE(loaded->autoItems.contains(autoId));
                REQUIRE(warnings.size() == 1);
                CHECK(warnings.front().code == "project.input_conflict");
                CHECK(warnings.front().message.find("Voice") != std::string::npos);
                CHECK(warnings.front().message.find(keepMidi ? "dropped audio input 1 2"
                                                             : "dropped MIDI input Keyboard") !=
                      std::string::npos);
                CHECK(warnings.front().message.find("another channel") != std::string::npos);
                CHECK(warnings.front().message.find("Notes") != std::string::npos);
                CHECK(warnings.front().message.find(p.channel.str()) != std::string::npos);
                const auto bytes = serializeProject(*loaded);
                warnings.clear();
                auto again = parseProject(bytes, &warnings);
                REQUIRE(again);
                CHECK(warnings.empty());
                CHECK(serializeProject(*again) == bytes);
            }
        }
}

TEST_CASE("track inputs: old files warn when multiple rack Outs are unrouted") {
    for (int count : {2, 3}) {
        Patch p = patch();
        EditTxn tx(p.s, 2);
        tx.modify<Track>(p.channel, [](auto& track) { track.name = "Ambiguous voice"; });
        for (int i = 1; i < count; ++i) {
            RackModule out;
            out.rack = p.rack;
            out.type = std::string(kOutModule);
            tx.create(out);
        }
        auto base = tx.commit();
        REQUIRE(base);
        const auto input = *base->tracks.find(p.channel)->record.input;
        auto midi = *base->routes.find(input);
        midi.from = RouteEnd{};
        midi.from.midiDevice = "Keyboard";
        midi.midi = MidiRouting{};
        auto doc = projectToJson(*base);
        doc["routes"][input.str()] = toJson(midi);
        for (const char* format : {"supple.project/2.7", "supple.project/2.8"}) {
            doc["schema"] = format;
            std::vector<schema::Warning> warnings;
            auto loaded = projectFromJson(doc, &warnings);
            REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
            CHECK(loaded->tracks.find(p.channel)->record.input == input);
            REQUIRE(midiInputRoute(*loaded, p.channel));
            CHECK(midiInputRoute(*loaded, p.channel)->from.midiDevice == "Keyboard");
            CHECK(loaded->routes.size() == base->routes.size());
            REQUIRE(warnings.size() == 1);
            CHECK(warnings.front().code == "project.rack_input_unrepaired");
            CHECK(warnings.front().path == "/tracks/" + p.channel.str() + "/record");
            CHECK(warnings.front().message.find("Ambiguous voice") != std::string::npos);
            CHECK(warnings.front().message.find("Keyboard") != std::string::npos);
            CHECK(warnings.front().message.find("Choose") != std::string::npos);
            CHECK(warnings.front().message.find("Out") != std::string::npos);
            const auto saved = serializeProject(*loaded);
            warnings.clear();
            auto reopened = parseProject(saved, &warnings);
            REQUIRE(reopened);
            CHECK(warnings.empty());
            CHECK(serializeProject(*reopened) == saved);
        }
    }
}
