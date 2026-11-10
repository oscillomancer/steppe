// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/environment.h>

#include <supple/model/render_settings.h>
#include <supple/model/schemas.h>
#include <supple/model/session.h>
#include <supple/model/versions.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <ostream>

using namespace supple;
using namespace supple::model;

TEST_CASE("render matrix remaps region keys and track cells but preserves text") {
    const auto project = json::parse(R"({"name":"tr_1","render":{"pattern":"rg_1",
        "matrix":{"rg_1":["tr_1","tr_0","tr_3"],"project":["tr_1"]}}})")
                             .value();
    const auto expected = json::parse(R"({"name":"tr_1","render":{"pattern":"rg_1",
        "matrix":{"rg_2":["tr_2","tr_0","tr_3"],"project":["tr_2"]}}})")
                              .value();
    const std::map<Id, Id> remap{{*Id::parse("tr_1"), *Id::parse("tr_2")},
                                 {*Id::parse("rg_1"), *Id::parse("rg_2")}};
    CHECK(remapIds(project, schemaOf<Project>().jsonSchema(), remap) == expected);
    auto decoded = schemaOf<Project>().decode(project);
    REQUIRE(decoded);
    CHECK(toJson(*decoded) == project);
}

TEST_CASE("project FX oversampling accepts only 1 2 4 or 8") {
    json::Value valid;
    Id fxId;
    for (const int factor : {1, 2, 4, 8}) {
        CAPTURE(factor);
        EditTxn txn(Session::create(), 1);
        FxInstance fx;
        fx.plugin = {"clap", "test.oversampling", "", "", ""};
        fx.oversampling = factor;
        fxId = txn.create(fx);
        txn.modify<Track>(kMaster, [&](auto& track) { track.chain.push_back(fxId); });
        auto session = txn.commit();
        REQUIRE(session);
        const auto saved = serializeProject(*session);
        auto reopened = parseProject(saved);
        REQUIRE(reopened);
        CHECK(reopened->fx.find(fxId)->oversampling == factor);
        CHECK(serializeProject(*reopened) == saved);
        valid = projectToJson(*reopened);
    }
    for (const int factor : {3, 5, 6, 7, 0, 9}) {
        CAPTURE(factor);
        auto bad = valid;
        bad["fx"][fxId.str()]["oversampling"] = factor;
        auto loaded = parseProject(json::writeCanonical(bad));
        CHECK_FALSE(loaded);
        if (!loaded) {
            CHECK(loaded.error().code == "fx.oversampling");
            CHECK(loaded.error().path == "/fx/" + fxId.str() + "/oversampling");
            CHECK(loaded.error().message.find("1, 2, 4 or 8") != std::string::npos);
            CHECK(loaded.error().hint.find("1, 2, 4 or 8") != std::string::npos);
        }
    }
    const auto exported = schemaOf<FxInstance>().jsonSchema();
    const auto* choices = exported.find("properties")->find("oversampling")->find("enum");
    REQUIRE(choices);
    CHECK(*choices == *json::parse("[1,2,4,8]"));
}

TEST_CASE("FX parameter keys are complete unsigned 32-bit decimal ids") {
    FxInstance fx;
    fx.plugin = {"clap", "test.params", "", "", ""};
    auto object = toJson(fx);
    for (const std::string& key :
         {std::string("-0"), std::string("+1"), std::string(" 1"), std::string("1\0suffix", 8),
          std::string("4294967296"), std::string("18446744073709551616")}) {
        CAPTURE(key);
        object["params"] = json::Value::object();
        object["params"][key] = 0.5;
        auto decoded = schemaOf<FxInstance>().decode(object);
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().toString().find("schema.key") != std::string::npos);
    }
    fx.params = {{0, 0.25}, {UINT32_MAX, 0.75}};
    auto decoded = schemaOf<FxInstance>().decode(toJson(fx));
    REQUIRE(decoded);
    CHECK(decoded->params == fx.params);
    EditTxn txn(Session::create(), 1);
    const auto id = txn.create(fx);
    txn.modify<Track>(kMaster, [&](auto& track) { track.chain.push_back(id); });
    auto session = txn.commit();
    REQUIRE(session);
    auto reopened = parseProject(serializeProject(*session));
    REQUIRE(reopened);
    CHECK(reopened->fx.find(id)->params == fx.params);
}

namespace {

Id addTrack(EditTxn& t, std::string name, Id parent = kMaster) {
    Track tr;
    tr.name = std::move(name);
    tr.parent = parent;
    const Id id = t.create(tr);
    t.modify<Track>(parent, [&](Track& p) { p.children.push_back(id); });
    return id;
}

// A session with a bit of everything, for round trips.
Session richSession() {
    Session s = Session::create({.name = "Demo", .sampleRate = 48000});
    EditTxn t(s, 1);
    const Id drums = addTrack(t, "Drums");
    t.modify<Track>(drums, [](Track& tr) { tr.type = StripType::Buss; });
    const Id kick = addTrack(t, "Kick", drums);
    const Id bass = addTrack(t, "Bass");
    t.modify<Track>(kick, [](Track& k) { k.volumeDb = -3.0; });
    MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 3840;
    src.sequence.notes = {{0, 480, 0, 36, 110, 0}, {960, 480, 9, 38, 90, 64}};
    src.sequence.controls = {{0, midi::ControlKind::CC, 0, 74, 64},
                             {480, midi::ControlKind::PitchBend, 0, 0, -8192}};
    src.sequence.sysex = {{0, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}};
    const Id mi = t.create(src);
    Take take;
    take.name = "kick loop";
    take.source = mi;
    const Id tk = t.create(take);
    Item item;
    item.track = kick;
    item.position = 0;
    item.length = 16;
    item.takes = {tk};
    item.activeTake = tk;
    item.fadeIn = {0.01, FadeShape::Cos, 0};
    t.create(item);
    FxInstance synth;
    synth.plugin = {"clap", "org.example.synth", "Example Synth", "Example", "0.1.0"};
    synth.patch =
        json::parse(R"({"schema": "org.example.synth-patch/1.0", "params": {"cutoff": 800}})")
            .value();
    const Id fx = t.create(synth);
    FxInstance third;
    third.plugin = {"clap", "org.example.comp", "", "", ""};
    third.state = "sha256:abc";
    third.params = {{0, 0.5}, {7, 1.0}};
    const Id fx2 = t.create(third);
    t.putBlob("sha256:abc", std::make_shared<const Blob>(Blob{{1, 2, 3, 250}}));
    t.modify<Track>(bass, [&](Track& b) { b.chain = {fx, fx2}; });
    Route side;
    side.from.track = kick;
    side.to.fx = fx2;
    side.tap = Tap::PreFader;
    side.channels = {{1, 2}, {3, 4}};
    t.create(side);
    Envelope env;
    env.target = {bass, "volume"};
    env.points = {{0, -6, PointShape::Linear, 0}, {4, 0, PointShape::Slow, 0.2}};
    t.create(env);
    t.create(Marker{4.0, Timebase::Beats, "Verse", ""});
    t.create(Region{16.0, 32.0, Timebase::Beats, "Chorus", "#ff8800"});
    t.create(TempoPointObj{16.0, 140.0, true});
    t.create(TempoPointObj{32.0, 150.0, false});
    t.create(MeterObj{9, 3, 4});
    auto out = t.commit();
    REQUIRE_MESSAGE(out, (out ? std::string() : out.error().toString()));
    return std::move(*out);
}

}  // namespace

TEST_CASE("ids: canonical text form only") {
    CHECK(Id::parse("tr_0") == kMaster);
    CHECK(Id::parse("fx_12")->str() == "fx_12");
    CHECK(Id::parse("rj_4294967295").has_value());
    for (const char* bad :
         {"tr_01", "xx_1", "tr_", "tr_-1", "tr1", "TR_1", "tr_1x", "tr_4294967296"})
        CHECK_MESSAGE(!Id::parse(bad).has_value(), bad);
    CHECK(Id{Kind::Track, 2} < Id{Kind::Track, 10});
    CHECK(Id{Kind::Track, 99} < Id{Kind::Item, 1});
}

TEST_CASE("a new session: master, one tempo point, one meter, counters past them") {
    const Session s = Session::create();
    REQUIRE(s.tracks.find(kMaster));
    CHECK(s.tracks.find(kMaster)->name == "Master");
    CHECK(s.tempo.contains(Id{Kind::TempoPoint, 1}));
    CHECK(s.meters.contains(Id{Kind::Meter, 1}));
    CHECK(s.next[Kind::Track] == 1);
    CHECK(s.next[Kind::TempoPoint] == 2);
    CHECK(s.timeMap->secondsAt(4) == 2.0);
    CHECK(checkInvariants(s));
}

TEST_CASE("transactions: new IDs, change lists, revisions, structural sharing") {
    const Session s0 = Session::create();
    EditTxn t(s0, 7);
    const Id a = addTrack(t, "A");
    const Id b = addTrack(t, "B");
    CHECK(a == Id{Kind::Track, 1});
    CHECK(b == Id{Kind::Track, 2});
    CHECK(t.get<Track>(a)->name == "A");  // reads see the transaction's edits
    const Id c = addTrack(t, "C");
    t.modify<Track>(kMaster, [&](Track& m) { m.children.erase(m.children.end() - 1); });
    t.remove<Track>(c);  // created and removed: no change recorded
    auto changes = t.changes();
    REQUIRE(changes.size() == 3);
    CHECK(changes[0] == Change{Change::Op::Update, kMaster});
    CHECK(changes[1] == Change{Change::Op::Add, a});
    CHECK(changes[2] == Change{Change::Op::Add, b});
    auto s1 = t.commit();
    REQUIRE(s1);
    CHECK(s1->version == 7);
    CHECK(s1->tracks.entry(a)->rev == 7);
    CHECK(s1->next[Kind::Track] == 4);       // tr_3 was used (then removed): never reused
    CHECK(s1->items.sameStorage(s0.items));  // untouched collections are shared
    CHECK_FALSE(s1->tracks.sameStorage(s0.tracks));
    CHECK(s0.tracks.size() == 1);  // the base is unchanged
    CHECK(t.commit().error().code == "model.txn");
}

TEST_CASE("invariants catch structural bugs") {
    const Session s0 = Session::create();
    {
        EditTxn t(s0, 1);
        Track orphan;
        orphan.parent = Id{Kind::Track, 42};
        t.create(orphan);
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().code == "model.invariant");
    }
    {
        EditTxn t(s0, 1);
        const Id x = addTrack(t, "X");
        const Id y = addTrack(t, "Y");
        t.modify<Track>(x, [](Track& tr) { tr.type = StripType::Buss; });
        t.modify<Track>(y, [](Track& tr) { tr.type = StripType::Buss; });
        Route r1, r2;
        r1.from.track = x;
        r1.to.track = y;
        r2.from.track = y;
        r2.to.track = x;
        t.create(r1);
        t.create(r2);
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().message.find("cycle") != std::string::npos);
    }
    {
        EditTxn t(s0, 1);
        FxInstance f;
        f.plugin.id = "x";
        t.create(f);  // in no chain
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().path == "/fx/fx_1");
    }
    {
        EditTxn t(s0, 1);
        const Id tr = addTrack(t, "T");
        const Id tk = t.create(Take{});
        Item i1, i2;
        i1.track = i2.track = tr;
        i1.takes = i2.takes = {tk};
        i1.activeTake = i2.activeTake = tk;
        t.create(i1);
        t.create(i2);
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().message.find("also belongs") != std::string::npos);
    }
    {
        EditTxn t(s0, 1);
        t.modify<TempoPointObj>(Id{Kind::TempoPoint, 1}, [](TempoPointObj& p) { p.beat = 1; });
        auto r = t.commit();
        REQUIRE(!r);  // no tempo point at beat 0
        CHECK(r.error().code == "model.invariant");
    }
}

TEST_CASE("project file: canonical, defaults omitted, byte-identical round trip") {
    const Session s = richSession();
    const std::string text = serializeProject(s);
    if (supple::environmentFlagEnabled("SUPPLE_DUMP_PROJECT")) std::fputs(text.c_str(), stdout);
    auto back = parseProject(text);
    REQUIRE_MESSAGE(back, (back ? std::string() : back.error().toString()));
    CHECK(serializeProject(*back) == text);
    CHECK(diff(s, *back).empty());
    CHECK(back->timeMap->bpmAt(24) == doctest::Approx(145.0));  // ramp 140 -> 150 over 16..32
    CHECK(back->timeMap->meterAt(10).numerator == 3);
    CHECK(back->blobs.at("sha256:abc")->data == std::vector<std::uint8_t>{1, 2, 3, 250});

    const json::Value doc = projectToJson(s);
    const json::Value* kick = doc.find("tracks")->find("tr_2");
    REQUIRE(kick);
    CHECK(kick->find("volume_db")->asDouble() == -3.0);
    CHECK(kick->find("pan") == nullptr);  // default: omitted
    CHECK(kick->find("channels") == nullptr);
    // Member order follows the spec.
    std::vector<std::string> order;
    for (const auto& m : doc.asObject()) order.push_back(m.key);
    CHECK(order == std::vector<std::string>{"schema", "project", "tempo", "rulers", "tracks",
                                            "items", "takes", "fx", "sources", "routes",
                                            "envelopes", "blobs"});
    // MIDI in compact rows, channels 1-based.
    const json::Value* notes =
        doc.find("sources")->find("midi")->find("mi_1")->find("sequence")->find("notes");
    CHECK(json::writeCompact(*notes) == "[[0,480,1,36,110,0],[960,480,10,38,90,64]]");
}

TEST_CASE("project file errors carry paths") {
    std::string text = serializeProject(richSession());
    auto bad = parseProject(
        std::string(text).replace(text.find("\"volume_db\": -3.0"), 17, "\"volume_db\": 99"));
    REQUIRE(!bad);
    CHECK(bad.error().path == "/tracks/tr_2/volume_db");
    auto wrongKey = json::parse(text).value();
    wrongKey["tracks"]["it_9"] = json::Value::object();
    auto r = projectFromJson(wrongKey);
    REQUIRE(!r);
    CHECK(r.error().path == "/tracks/it_9");
    auto future = json::parse(text).value();
    future["schema"] = "supple.project/9.0";
    CHECK(projectFromJson(future).error().code == "project.version");
}

TEST_CASE("diff: object-level, deterministic, field names") {
    const Session a = richSession();
    EditTxn t(a, 2);
    t.modify<Track>(Id{Kind::Track, 2}, [](Track& k) {
        k.volumeDb = -6.0;
        k.mute = true;
    });
    addTrack(t, "New");
    t.remove<Marker>(Id{Kind::Marker, 1});
    auto b = t.commit();
    REQUIRE(b);
    const auto d = diff(a, *b);
    REQUIRE(d.size() == 4);
    CHECK(d[0].op == Change::Op::Remove);  // markers come before tracks
    CHECK(d[0].id == Id{Kind::Marker, 1});
    CHECK(d[1].op == Change::Op::Update);  // the master's children
    CHECK(d[1].id == kMaster);
    CHECK(d[1].fields == std::vector<std::string>{"children"});
    CHECK(d[2].id == Id{Kind::Track, 2});
    CHECK(d[2].fields == std::vector<std::string>{"mute", "volume_db"});
    CHECK(d[3].op == Change::Op::Add);
}

TEST_CASE("typed strips: master forest, follow pan and edit validation") {
    EditTxn t(Session::create(), 1);
    const Id buss = addTrack(t, "Group");
    t.modify<Track>(buss, [](auto& tr) { tr.type = StripType::Buss; });
    const Id channel = addTrack(t, "Source", buss);
    Track master;
    master.type = StripType::Master;
    master.name = "Alt";
    const Id alt = t.create(master);
    auto p = t.project();
    p.masters.push_back(alt);
    p.console9 = false;
    t.setProject(p);
    Route send;
    send.from.track = channel;
    send.to.track = alt;
    const Id route = t.create(send);
    auto s = t.commit();
    REQUIRE(s);
    CHECK(s->routes.find(route)->pan.resolve(34) == 34);
    CHECK(toJson(*s->routes.find(route)).find("pan") == nullptr);
    auto parsed = parseProject(serializeProject(*s));
    REQUIRE(parsed);
    CHECK(parsed->project->masters == std::vector<Id>{kMaster, alt});
    CHECK_FALSE(parsed->project->console9);
    CHECK(serializeProject(*parsed) == serializeProject(*s));
    auto rejected = [&](auto mutate, const char* code) {
        EditTxn bad(*s, 2);
        mutate(bad);
        auto result = validateStripRules(bad.preview());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().hint.empty());
    };
    rejected(
        [&](EditTxn& bad) {
            bad.modify<Track>(buss, [](auto& tr) { tr.type = StripType::Channel; });
        },
        "track.strip_type");
    rejected(
        [&](EditTxn& bad) {
            Item i;
            i.track = buss;
            bad.create(i);
        },
        "track.strip_type");
    rejected(
        [&](EditTxn& bad) { bad.modify<Track>(buss, [](auto& tr) { tr.record.armed = true; }); },
        "track.strip_type");
    rejected(
        [&](EditTxn& bad) { bad.modify<Route>(route, [&](auto& r) { r.to.track = channel; }); },
        "route.strip_type");
    rejected(
        [&](EditTxn& bad) { bad.modify<Route>(route, [&](auto& r) { r.from.track = kMaster; }); },
        "route.strip_type");
    rejected(
        [&](EditTxn& bad) {
            bad.modify<Route>(route, [&](auto& r) {
                r.from.track.reset();
                r.from.hwChannels = std::vector<int>{1, 2};
            });
        },
        "route.strip_type");
    rejected(
        [&](EditTxn& bad) {
            bad.modify<Route>(route, [&](auto& r) {
                r.to.track.reset();
                r.to.hwChannels = std::vector<int>{1, 2};
            });
        },
        "route.hw_output");
    rejected(
        [&](EditTxn& bad) {
            bad.modify<Track>(buss, [](auto& tr) { tr.hwOutput = std::vector<int>{1, 2}; });
        },
        "track.strip_type");
    rejected(
        [&](EditTxn& bad) {
            bad.modify<Track>(alt, [](auto& tr) { tr.hwOutput = std::vector<int>{3, 3}; });
        },
        "track.hw_output");
    rejected(
        [&](EditTxn& bad) {
            Envelope e;
            e.target = {channel, "width"};
            bad.create(e);
        },
        "track.strip_type");
    auto pan = routePanSchema().decode(json::Value("follow"));
    REQUIRE(pan);
    CHECK_FALSE(pan->percent);
    CHECK(routePanSchema().decode(json::Value(-21))->resolve(0) == -21);
}

TEST_CASE("project 1.x migration: folders and receives become busses with child sources") {
    auto doc = projectToJson(Session::create());
    doc["schema"] = "supple.project/1.7";
    doc["project"]["pan_law_db"] = -3;
    doc["project"]["next_ids"]["tr"] = 4;
    doc["project"]["next_ids"]["sd"] = 4;
    doc["project"]["next_ids"]["it"] = 2;
    doc["project"]["next_ids"]["en"] = 2;
    doc["tracks"] = json::parse(R"({
      "tr_0":{"name":"Master","children":["tr_1","tr_3"]},
      "tr_1":{"name":"Folder","parent":"tr_0","children":["tr_2"],"width":50,"phase_invert":true,
                "record":{"armed":true,"input":"sd_2","monitor":"on","mode":"input"}},
      "tr_2":{"name":"Source","parent":"tr_1"},
      "tr_3":{"name":"Receive","parent":"tr_0"}
    })")
                        .value();
    Item item;
    item.track = Id{Kind::Track, 1};
    doc["items"]["it_1"] = toJson(item);
    doc["routes"] = json::parse(R"({
       "sd_1":{"from":{"track":"tr_2"},"to":{"track":"tr_3"}},
       "sd_2":{"from":{"hw_channels":[1,2]},"to":{"track":"tr_1"}},
       "sd_3":{"from":{"track":"tr_0"},"to":{"track":"tr_3"}}
    })")
                        .value();
    Envelope env;
    env.target = {Id{Kind::Track, 1}, "width"};
    doc["envelopes"]["en_1"] = toJson(env);
    std::vector<schema::Warning> warnings;
    auto migrated = projectFromJson(doc, &warnings);
    REQUIRE_MESSAGE(migrated, (migrated ? std::string() : migrated.error().toString()));
    CHECK(migrated->project->console9);
    CHECK(migrated->project->masters == std::vector<Id>{kMaster});
    CHECK(migrated->tracks.find(Id{Kind::Track, 1})->type == StripType::Buss);
    CHECK(migrated->tracks.find(Id{Kind::Track, 3})->type == StripType::Buss);
    CHECK(
        migrated->tracks.find(Id{Kind::Track, 1})->phaseInvert);  // a strip field again (ADR-0022)
    const Id child{Kind::Track, 4};
    CHECK(migrated->tracks.find(child)->name == "Folder items");
    CHECK(migrated->tracks.find(Id{Kind::Track, 1})->children.front() == child);
    CHECK(migrated->items.find(Id{Kind::Item, 1})->track == child);
    CHECK(migrated->tracks.find(child)->record.armed);
    CHECK(migrated->routes.find(Id{Kind::Route, 2})->to.track == child);
    CHECK(migrated->next[Kind::Track] == 5);
    CHECK_FALSE(migrated->routes.contains(Id{Kind::Route, 3}));
    CHECK(migrated->envelopes.empty());
    CHECK(warnings.size() == 6);
    bool staging = false;
    for (const auto& w : warnings) {
        CHECK_FALSE(w.path.empty());
        staging |= w.path == "/project/console9" && w.message.find("levels") != std::string::npos;
    }
    CHECK(staging);
    const auto text = serializeProject(*migrated);
    CHECK(text.find(std::string(kProjectSchema)) != std::string::npos);
    auto back = parseProject(text);
    REQUIRE(back);
    CHECK(serializeProject(*back) == text);
}

TEST_CASE("masters' hardware outputs: defaults, round trip and the 2.3 route form") {
    EditTxn t(Session::create(), 1);
    Track alt;
    alt.type = StripType::Master;
    const Id cue = t.create(alt);
    auto project = t.project();
    project.masters.push_back(cue);
    t.setProject(project);
    auto s = t.commit();
    REQUIRE(s);
    CHECK(hardwareOutputs(*s->tracks.find(kMaster), kMaster) == std::vector<int>{1, 2});
    CHECK(hardwareOutputs(*s->tracks.find(cue), cue).empty());
    EditTxn edit(*s, 2);
    edit.modify<Track>(cue, [](auto& tr) { tr.hwOutput = std::vector<int>{3, 4}; });
    edit.modify<Track>(kMaster, [](auto& tr) { tr.hwOutput = std::vector<int>{}; });
    auto set = edit.commit();
    REQUIRE(set);
    auto back = parseProject(serializeProject(*set));
    REQUIRE(back);
    CHECK(hardwareOutputs(*back->tracks.find(cue), cue) == std::vector<int>{3, 4});
    CHECK(hardwareOutputs(*back->tracks.find(kMaster), kMaster).empty());
    // Before 2.4 the same was a route from the master to hw_channels.
    auto doc = projectToJson(*s);
    doc["schema"] = "supple.project/2.3";
    doc["routes"]["sd_9"] = *json::parse(R"({"from": {"track": ")" + cue.str() +
                                         R"("}, "to": {"hw_channels": [5, 6]}})");
    std::vector<schema::Warning> warnings;
    auto migrated = projectFromJson(doc, &warnings);
    REQUIRE(migrated);
    CHECK(migrated->routes.size() == 0);
    CHECK(hardwareOutputs(*migrated->tracks.find(cue), cue) == std::vector<int>{5, 6});
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].code == "project.hw_output_route");
}

TEST_CASE("masters: arm, audio items and lane mute round trip (ADR-0057)") {
    EditTxn t(Session::create(), 1);
    Track alt;
    alt.type = StripType::Master;
    alt.record.armed = true;
    const Id cue = t.create(alt);
    auto project = t.project();
    project.masters.push_back(cue);
    t.setProject(project);
    t.modify<Track>(kMaster, [](auto& tr) { tr.laneMute = true; });
    MediaSource md;
    md.path = "print.wav";
    md.sampleRate = 48000;
    md.channels = 2;
    md.frames = 48000;
    const Id media = t.create(md);
    Take take;
    take.source = media;
    const Id tk = t.create(take);
    Item print;
    print.track = kMaster;
    print.length = 2;
    print.takes = {tk};
    print.activeTake = tk;
    const Id it = t.create(print);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    REQUIRE(validateStripRules(*s));
    const std::string saved = serializeProject(*s);
    CHECK(saved.find("\"supple.project/2.9\"") != std::string::npos);
    auto back = parseProject(saved);
    REQUIRE(back);
    CHECK(back->tracks.find(kMaster)->laneMute);
    CHECK(back->tracks.find(cue)->record.armed);
    CHECK(back->items.find(it)->track == kMaster);
    CHECK(serializeProject(*back) == saved);
    // A 2.4 file has none of it and loads unchanged.
    auto old = projectToJson(Session::create());
    old["schema"] = "supple.project/2.4";
    std::vector<schema::Warning> warnings;
    auto loaded = projectFromJson(old, &warnings);
    REQUIRE(loaded);
    CHECK(warnings.empty());
    CHECK_FALSE(loaded->tracks.find(kMaster)->laneMute);
    auto rejected = [&](auto mutate) {
        EditTxn bad(*s, 2);
        mutate(bad);
        auto result = validateStripRules(bad.preview());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "track.strip_type");
        CHECK_FALSE(result.error().hint.empty());
    };
    rejected([&](EditTxn& bad) {  // MIDI on a master
        MidiSource mi;
        const Id midi = bad.create(mi);
        bad.modify<Take>(tk, [&](auto& x) { x.source = midi; });
    });
    rejected([&](EditTxn& bad) {  // a master records its output, not an input
        bad.modify<Track>(cue, [](auto& tr) { tr.record.monitor = Monitor::On; });
    });
    rejected([&](EditTxn& bad) {
        Track ch;
        ch.parent = kMaster;
        ch.laneMute = true;
        const Id id = bad.create(ch);
        bad.modify<Track>(kMaster, [&](auto& tr) { tr.children.push_back(id); });
    });
}

TEST_CASE("project newer minor ignores unknown fields with load warnings") {
    auto doc = projectToJson(Session::create());
    doc["schema"] = "supple.project/2.10";
    doc["tracks"]["tr_0"]["future"] = true;
    std::vector<schema::Warning> warnings;
    auto parsed = projectFromJson(doc, &warnings);
    REQUIRE(parsed);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].path == "/schema");
    CHECK(warnings[1].path == "/tracks/tr_0/future");
    doc["schema"] = "supple.project/2.0";
    warnings.clear();
    REQUIRE(projectFromJson(doc, &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].path == "/tracks/tr_0/future");
}

TEST_CASE("project 1.x migration drops automation items with their removed envelopes") {
    auto doc = projectToJson(Session::create());
    doc["schema"] = "supple.project/1.7";
    doc["project"]["next_ids"]["en"] = 3;
    doc["project"]["next_ids"]["ai"] = 3;
    Envelope removed;
    removed.target = {kMaster, "width"};
    doc["envelopes"]["en_1"] = toJson(removed);
    Envelope retained;
    retained.target = {kMaster, "volume"};
    doc["envelopes"]["en_2"] = toJson(retained);
    AutomationItem item;
    item.envelope = Id{Kind::Envelope, 1};
    item.length = item.sourceLength = 1;
    doc["automation_items"]["ai_1"] = toJson(item);
    item.envelope = Id{Kind::Envelope, 2};
    doc["automation_items"]["ai_2"] = toJson(item);
    std::vector<schema::Warning> warnings;
    auto migrated = projectFromJson(doc, &warnings);
    REQUIRE_MESSAGE(migrated, (migrated ? std::string() : migrated.error().toString()));
    CHECK_FALSE(migrated->autoItems.contains(Id{Kind::AutoItem, 1}));
    CHECK(migrated->autoItems.contains(Id{Kind::AutoItem, 2}));
    CHECK(std::any_of(warnings.begin(), warnings.end(),
                      [](const schema::Warning& w) { return w.path == "/automation_items/ai_1"; }));
    const auto saved = serializeProject(*migrated);
    auto reopened = parseProject(saved);
    REQUIRE(reopened);
    CHECK(serializeProject(*reopened) == saved);
}

TEST_CASE("automation items replace the envelope under them: loop, scale, overlap, file") {
    model::EditTxn t(model::Session::create(), 1);
    model::Envelope env;
    env.target = {model::kMaster, "volume"};
    env.points = {{0.0, 0.0}, {16.0, -16.0}};  // -1 dB a beat
    const model::Id en = t.create(env);
    model::AutomationItem ai;
    ai.envelope = en;
    ai.position = 4.0;
    ai.length = 4.0;  // two passes of a two-beat source
    ai.sourceLength = 2.0;
    ai.points = {{0.0, -20.0}, {1.0, -10.0}};
    ai.loop = true;
    const model::Id a = t.create(ai);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    const auto pts = model::effectivePoints(*s, en);
    auto at = [&](double beat) { return model::pointsValueAt(pts, beat, 0.0); };
    CHECK(at(2.0) == doctest::Approx(-2.0));   // the envelope before the item
    CHECK(at(4.0) == doctest::Approx(-20.0));  // the item from its first beat (a step)
    CHECK(at(4.5) == doctest::Approx(-15.0));  // its ramp
    CHECK(at(5.5) == doctest::Approx(-10.0));  // its last value held to the source end
    CHECK(at(6.0) == doctest::Approx(-20.0));  // the second pass
    CHECK(at(6.5) == doctest::Approx(-15.0));
    CHECK(at(8.0) == doctest::Approx(-8.0));  // back on the envelope after it
    CHECK(at(12.0) == doctest::Approx(-12.0));
    // Baseline and amplitude scale its values; unlooped, its last value holds.
    model::EditTxn t2(*s, 2);
    t2.modify<model::AutomationItem>(a, [](model::AutomationItem& x) {
        x.loop = false;
        x.baseline = -10.0;
        x.amplitude = 0.5;
    });
    auto s2 = t2.commit();
    REQUIRE(s2);
    const auto pts2 = model::effectivePoints(*s2, en);
    CHECK(model::pointsValueAt(pts2, 4.0, 0.0) == doctest::Approx(-15.0));  // -10 + 0.5 x -10
    CHECK(model::pointsValueAt(pts2, 7.0, 0.0) == doctest::Approx(-10.0));  // held, unlooped
    // A later item cuts an earlier one it overlaps.
    model::EditTxn t3(*s2, 3);
    model::AutomationItem b;
    b.envelope = en;
    b.position = 6.0;
    b.length = 1.0;
    b.sourceLength = 1.0;
    b.points = {{0.0, 6.0}};
    t3.create(b);
    auto s3 = t3.commit();
    REQUIRE(s3);
    const auto pts3 = model::effectivePoints(*s3, en);
    CHECK(model::pointsValueAt(pts3, 6.5, 0.0) == doctest::Approx(6.0));
    CHECK(model::pointsValueAt(pts3, 4.5, 0.0) == doctest::Approx(-12.5));  // the first, scaled
    CHECK(model::pointsValueAt(pts3, 7.5, 0.0) == doctest::Approx(-7.5));   // the envelope again
    // Through the project file and back; a project without them has no key.
    auto back = model::parseProject(model::serializeProject(*s3));
    REQUIRE_MESSAGE(back, (back ? std::string() : back.error().toString()));
    CHECK(back->autoItems.size() == 2);
    CHECK(model::serializeProject(*back) == model::serializeProject(*s3));
    CHECK(model::serializeProject(model::Session::create()).find("automation_items") ==
          std::string::npos);
    // An item on a take's envelope, or on no envelope, is invalid.
    model::EditTxn t4(*s3, 4);
    model::AutomationItem stray;
    stray.envelope = model::Id{model::Kind::Envelope, 99};
    t4.create(stray);
    CHECK_FALSE(t4.commit());
}

TEST_CASE("project MIDI decoding rejects malformed rows without truncation or exceptions") {
    auto doc = projectToJson(richSession());
    auto& seq = doc["sources"]["midi"]["mi_1"]["sequence"];
    auto rejects = [&](const char* key, const char* input, const char* field) {
        auto bad = doc;
        bad["sources"]["midi"]["mi_1"]["sequence"][key] = json::parse(input).value();
        Result<Session> result = Error("test", "not decoded");
        REQUIRE_NOTHROW(result = projectFromJson(bad));
        REQUIRE_FALSE(result);
        CHECK(result.error().path == std::string("/sources/midi/mi_1/sequence/") + field);
    };
    rejects("ppq", "0", "ppq");
    rejects("ppq", "960.5", "ppq");
    rejects("length", "-1", "length");
    rejects("notes", "false", "notes");
    rejects("notes", "[[0.5,480,1,60,96]]", "notes/0/0");
    rejects("notes", "[[1e100,480,1,60,96]]", "notes/0/0");
    rejects("notes", "[[0,480,1,60,96,0,123]]", "notes/0");
    rejects("controls", "[[0,\"cc\",1,7,-1]]", "controls/0/4");
    rejects("controls", "[[0,\"program\",1,0,128]]", "controls/0/4");
    rejects("controls", "[[0,\"bend\",1,0,8192]]", "controls/0/4");
    rejects("sysex", "[[0,\"GG\"]]", "sysex/0/1");
    rejects("sysex", "[[0,\"F0 7E F7 junk\"]]", "sysex/0/1");
    rejects("sysex", "[[0,\"F0 FF F7\"]]", "sysex/0/1");
    rejects("sysex", "[[-1,\"F0 F7\"]]", "sysex/0/0");
    seq["notes"] = json::parse("[[9007199254740993,480,1,60,96]]").value();
    auto exact = projectFromJson(doc);
    REQUIRE(exact);
    CHECK(exact->midi.find(Id{Kind::Midi, 1})->sequence.notes[0].start == 9007199254740993LL);
}

TEST_CASE("project ID counters refuse narrowing and fractional values") {
    for (const char* value : {"-1", "4294967297", "1.5", "1e100", "\"1\""}) {
        auto doc = projectToJson(Session::create());
        doc["project"]["next_ids"]["tr"] = json::parse(value).value();
        auto parsed = projectFromJson(doc);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().path == "/project/next_ids/tr");
    }
    auto doc = projectToJson(Session::create());
    doc["project"]["next_ids"] = false;
    auto parsed = projectFromJson(doc);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().path == "/project/next_ids");
}

TEST_CASE("automation item lists are derived from the authoritative map") {
    auto s = richSession();
    EditTxn txn(s, s.version + 1);
    AutomationItem ai;
    ai.envelope = s.envelopes.begin()->id;
    ai.length = 4;
    ai.sourceLength = 4;
    const auto id = txn.create(ai);
    auto committed = txn.commit();
    REQUIRE(committed);
    auto doc = projectToJson(*committed);
    auto& items = doc["envelopes"][ai.envelope.str()]["items"];
    REQUIRE(items.isArray());
    CHECK(items.at(0) == id.str());
    items.push_back("ai_999");
    std::vector<schema::Warning> warnings;
    auto loaded = projectFromJson(doc, &warnings);
    REQUIRE(loaded);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].code == "project.derived_items");
    CHECK(serializeProject(*loaded) == serializeProject(*committed));
}

TEST_CASE(
    "project sections: strict containers, preserved extensions, ignored UI and unsupported "
    "groups") {
    const auto base = projectToJson(Session::create());
    for (const char* name : {"tempo", "rulers", "sources", "blobs", "extensions"}) {
        auto bad = base;
        bad[name] = json::Value::array();
        auto result = projectFromJson(bad);
        REQUIRE_FALSE(result);
        CHECK(result.error().path == std::string("/") + name);
    }
    auto doc = base;
    doc["future"] = json::parse(R"({"uninterpreted":[1,true,{"x":"keep"}]})").value();
    doc["extensions"] = json::parse(R"({"vendor.example":{"data":[null,2.0]}})").value();
    const auto bytes = json::writeCanonical(doc) + "\n";
    auto parsed = parseProject(bytes);
    REQUIRE(parsed);
    CHECK(serializeProject(*parsed) == bytes);
    doc["ui"] = json::Value::array();
    doc["schema"] = "supple.project/2.1";
    doc["groups"] = json::parse(R"({"gr_1":{"future":true}})").value();
    std::vector<schema::Warning> warnings;
    auto withWarnings = projectFromJson(doc, &warnings);
    REQUIRE(withWarnings);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[1].path == "/ui");
    CHECK(warnings[1].message.find(".ui.json") != std::string::npos);
    CHECK(warnings[0].path == "/groups");
    auto saved = projectToJson(*withWarnings);
    CHECK_FALSE(saved.find("ui"));
    CHECK(saved["extensions"]["legacy_track_groups"] == doc["groups"]);
    saved["extensions"].asObject().erase("legacy_track_groups");
    CHECK(saved["extensions"] == doc["extensions"]);
}

TEST_CASE("model and project loading reject unsafe numeric ends and interpolation") {
    for (bool automation : {false, true}) {
        auto base = Session::create();
        EditTxn t(base, 1);
        Envelope env;
        env.target = {kMaster, "volume"};
        if (!automation) env.points = {{0, -1e308}, {1, 1e308}};
        const auto id = t.create(env);
        if (automation) {
            AutomationItem ai;
            ai.envelope = id;
            ai.points = {{0, -1e308}, {1, 1e308}};
            t.create(ai);
        }
        auto doc = projectToJson(t.preview());
        auto loaded = projectFromJson(doc);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().path.find("/points/") != std::string::npos);
        CHECK_FALSE(t.commit());
    }
    auto base = Session::create();
    EditTxn t(base, 1);
    t.create(Marker{1e300, Timebase::Beats, "Huge", ""});
    auto loaded = projectFromJson(projectToJson(t.preview()));
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().path.find("/pos") != std::string::npos);
    CHECK_FALSE(t.commit());
    EditTxn midiTxn(base, 2);
    MidiSource source;
    source.sequence.length = INT64_MAX;
    midiTxn.create(source);
    CHECK_FALSE(midiTxn.commit());
}

TEST_CASE("known nested project fields reject wrong shapes and drop unknowns with paths") {
    auto base = projectToJson(Session::create());
    for (const auto& [section, field] :
         std::vector<std::pair<std::string, std::string>>{{"tempo", "points"},
                                                          {"tempo", "meters"},
                                                          {"rulers", "markers"},
                                                          {"sources", "media"}}) {
        auto doc = base;
        doc[section][field] = json::Value::array();
        auto loaded = projectFromJson(doc);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().path == "/" + section + "/" + field);
    }
    for (const char* field : {"render"}) {
        auto doc = base;
        doc["project"][field] = false;
        auto loaded = projectFromJson(doc);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().path == std::string("/project/") + field);
    }
    auto doc = base;
    doc["project"]["render"] =
        json::parse(R"({"normalize":{"mode":"peak","future":true},"future":42})").value();
    std::vector<schema::Warning> warnings;
    auto loaded = projectFromJson(doc, &warnings);
    REQUIRE(loaded);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].path == "/project/render/normalize/future");
    CHECK(warnings[1].path == "/project/render/future");
    CHECK_FALSE(loaded->project->render.find("future"));
    CHECK_FALSE(loaded->project->render.find("normalize")->find("future"));
    doc["project"]["render"] = json::parse(R"({"normalize":{"mode":true}})").value();
    auto bad = projectFromJson(doc);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().path == "/project/render/normalize/mode");
}

TEST_CASE("master playrate: old projects default to unity without preserving pitch") {
    for (const char* format :
         {"supple.project/1.7", "supple.project/2.0", "supple.project/2.1", "supple.project/2.2"}) {
        auto doc = projectToJson(Session::create());
        CHECK(doc["project"].find("playrate") == nullptr);
        doc["schema"] = format;
        std::vector<schema::Warning> warnings;
        auto loaded = projectFromJson(doc, &warnings);
        REQUIRE(loaded);
        CHECK(loaded->project->playrate == Playrate{});
        CHECK(std::none_of(warnings.begin(), warnings.end(),
                           [](const auto& w) { return w.code == "project.newer_minor"; }));
        CHECK(projectToJson(*loaded)["schema"].asString() == kProjectSchema);
    }
}

TEST_CASE("master playrate: save reopen includes the first master envelope and automation") {
    for (double rate : {kMinPlayrate, 1.137, kMaxPlayrate}) {
        EditTxn edit(Session::create(), 1);
        auto project = edit.project();
        project.playrate = {rate, true};
        edit.setProject(project);
        Envelope env;
        env.target = {kMaster, "playrate"};
        env.timebase = Timebase::Time;
        env.points = {{0, kMinPlayrate}, {2, kMaxPlayrate, PointShape::Bezier, .8}};
        const Id id = edit.create(env);
        AutomationItem item;
        item.envelope = id;
        item.position = 1;
        item.points = {{0, 1}, {4, 2}};
        item.baseline = 1;
        item.amplitude = .5;
        edit.create(item);
        auto session = edit.commit();
        REQUIRE(session);
        const auto bytes = serializeProject(*session);
        auto reopened = parseProject(bytes);
        REQUIRE(reopened);
        CHECK(serializeProject(*reopened) == bytes);
        CHECK(reopened->project->playrate == project.playrate);
        CHECK(reopened->envelopes.find(id)->target.param == "playrate");
        CHECK(reopened->envelopes.find(id)->timebase == Timebase::Time);
        CHECK(effectivePoints(*reopened, id).size() > 2);
        CHECK(diff(*session, *reopened).empty());
        CHECK(diff(Session::create(), *session).front().fields ==
              std::vector<std::string>{"playrate"});
    }
}

TEST_CASE("master playrate: reject invalid settings and scaled automation on edits and load") {
    for (double rate : {0.0, .249, 4.001, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        EditTxn edit(Session::create(), 1);
        auto project = edit.project();
        project.playrate.rate = rate;
        edit.setProject(project);
        auto rejected = edit.commit();
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "playrate.range");
        CHECK(rejected.error().path == "/project/playrate/rate");
        auto doc = projectToJson(Session::create());
        doc["project"]["playrate"] = toJson(project.playrate);
        CHECK_FALSE(projectFromJson(doc));
    }
    for (bool scaled : {false, true}) {
        EditTxn edit(Session::create(), 1);
        Envelope env;
        env.target = {kMaster, "playrate"};
        env.points = {{0, scaled ? 1.0 : 0.0}};
        const Id id = edit.create(env);
        if (scaled) {
            AutomationItem item;
            item.envelope = id;
            item.points = {{0, 2}, {4, 3}};
            item.baseline = 1;
            item.amplitude = 2;  // effective endpoint is 5, outside the range
            edit.create(item);
        }
        const auto doc = projectToJson(edit.preview());
        auto result = edit.commit();
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "playrate.range");
        CHECK_FALSE(result.error().hint.empty());
        CHECK_FALSE(projectFromJson(doc));
    }
}

TEST_CASE("master playrate: only one non-plugin envelope on tr_0") {
    EditTxn setup(Session::create(), 1);
    const Id channel = addTrack(setup, "Channel");
    Route route;
    route.from.track = channel;
    route.to.track = kMaster;
    const Id send = setup.create(route);
    const Id take = setup.create(Take{});
    Item item;
    item.track = channel;
    item.length = 1;
    item.takes = {take};
    item.activeTake = take;
    setup.create(item);
    auto ready = setup.commit();
    REQUIRE_MESSAGE(ready, (ready ? "" : ready.error().toString()));
    const auto base = *ready;
    for (Id target : {channel, send, take}) {
        EditTxn edit(base, 2);
        Envelope env;
        env.target = {target, "playrate"};
        edit.create(env);
        CHECK_FALSE(edit.commit());
    }
    EditTxn duplicate(base, 2);
    Envelope env;
    env.target = {kMaster, "playrate"};
    duplicate.create(env);
    duplicate.create(env);
    CHECK_FALSE(duplicate.commit());
}

TEST_CASE("automation items on a time envelope convert beat positions into seconds") {
    model::EditTxn edit(model::Session::create(), 1);
    model::Envelope env;
    env.target = {model::kMaster, "playrate"};
    env.timebase = model::Timebase::Time;
    env.points = {{0, 1}, {2, 1}};
    const auto lane = edit.create(env);
    model::AutomationItem item;
    item.envelope = lane;
    item.position = 1;
    item.length = 1;
    item.sourceLength = .5;
    item.loop = true;
    item.points = {{0, 2}, {.25, 3}};
    edit.create(item);
    const auto session = edit.commit();
    REQUIRE(session);
    const auto points = model::effectivePoints(*session, lane);
    auto at = [&](double seconds) { return model::pointsValueAt(points, seconds, 1); };
    CHECK(at(.4) == 1);
    CHECK(at(.5) == 2);
    CHECK(at(.5625) == 2.5);
    CHECK(at(.75) == 2);
    CHECK(at(.8125) == 2.5);
    CHECK(at(1) == 1);
    const auto reopened = model::parseProject(model::serializeProject(*session));
    REQUIRE(reopened);
    CHECK(model::serializeProject(*reopened) == model::serializeProject(*session));
    CHECK(model::pointsValueAt(model::effectivePoints(*reopened, lane), .5625, 1) == 2.5);
}

TEST_CASE("render settings: a 2.2 file migrates to schema 2.3 and reopens byte-identical") {
    auto doc = projectToJson(Session::create());
    doc["schema"] = "supple.project/2.2";
    doc["project"]["render"] = json::parse(R"({
        "format": {"container": "flac", "bit_depth": 16, "dither": false},
        "secondary": {"format": "wav", "bit_depth": 24},
        "tail_ms": 1500, "sample_rate": 44100, "channels": 1, "pattern": "{project}-{track}",
        "normalize": {"mode": "lufs_i", "target": -16, "limit_true_peak_db": -1},
        "matrix": {"project": ["tr_0"]}})")
                                   .value();
    auto loaded = projectFromJson(doc);
    REQUIRE(loaded);
    const auto expected = json::parse(R"({
        "outputs": [{"format": "flac", "bit_depth": 16, "dither": "none"},
                    {"format": "wav", "bit_depth": 24}],
        "tail_s": 1.5, "sample_rate": 44100, "channels": 1, "pattern": "{project}-{track}",
        "normalize": {"mode": "lufs_i", "target": -16}, "limit_true_peak_db": -1,
        "matrix": {"project": ["tr_0"]}})")
                              .value();
    CHECK(loaded->project->render ==
          render::toJson(render::settingsSchema().decode(expected).value()));
    const std::string saved = serializeProject(*loaded);
    CHECK(saved.find("\"supple.project/2.9\"") != std::string::npos);
    CHECK(saved.find("tail_ms") == std::string::npos);
    auto reopened = parseProject(saved);
    REQUIRE(reopened);
    CHECK(serializeProject(*reopened) == saved);
    // A string format and normalize, and a bare bit depth.
    doc["project"]["render"] =
        json::parse(R"({"format": "wav", "bit_depth": 16, "normalize": "peak"})").value();
    loaded = projectFromJson(doc);
    REQUIRE(loaded);
    CHECK(json::writeCompact(loaded->project->render) ==
          R"({"outputs":[{"format":"wav","bit_depth":16}],"normalize":{"mode":"peak"}})");
    // Every member of the new shape round-trips.
    doc["schema"] = std::string(kProjectSchema);
    doc["project"]["render"] = json::parse(R"({
        "source": "stems", "bounds": "custom", "start_beat": 4, "end_beat": 12, "tail_s": 2,
        "pattern": {"mix": "{project}/{project}-v{version}", "stems": "{track}"},
        "channels": "track", "seamless_loop": false, "handles_s": 0.5, "if_exists": "number",
        "outputs": [{"format": "wav", "bit_depth": 24, "dither": "shaped"},
                    {"format": "mp3", "mp3": {"mode": "vbr", "vbr_quality": 2}, "suffix": "-web"},
                    {"format": "vorbis", "vorbis_quality": 5, "sample_rate": 44100}],
        "normalize": {"mode": "lufs_i", "target": -14, "only_reduce": true, "scope": "master"},
        "trim": {"start": true, "end": true, "threshold_db": -70},
        "stems": {"mode": "through_master", "tap": "pre_fader", "tracks": ["tr_0"],
                  "skip_silent": true},
        "metadata": {"title": "{project}", "artist": "A", "track_number": 3, "date": "2026-11-03",
                     "isrc": "USRC17607839", "bwf": true, "markers": true},
        "report": true, "add_to_project": true})")
                                   .value();
    loaded = projectFromJson(doc);
    REQUIRE(loaded);
    auto again = parseProject(serializeProject(*loaded));
    REQUIRE(again);
    CHECK(again->project->render ==
          render::toJson(render::settingsSchema().decode(doc["project"]["render"]).value()));
    CHECK(again->project->render.size() == doc["project"]["render"].size());
    // Values outside the contract are refused where they are.
    for (const auto& [render, path] : std::vector<std::pair<const char*, const char*>>{
             {R"({"outputs": []})", "/project/render/outputs"},
             {R"({"outputs": [{"format": "ogg"}]})", "/project/render/outputs/0/format"},
             {R"({"outputs": [{"format": "mp3", "mp3": {"bitrate_kbps": 100}}]})",
              "/project/render/outputs/0/mp3/bitrate_kbps"},
             {R"({"channels": 3})", "/project/render/channels"},
             {R"({"tail_ms": 1e300})", "/project/render/tail_s"},
             {R"({"metadata": {"date": "Nov 3"}})", "/project/render/metadata/date"}}) {
        doc["project"]["render"] = json::parse(render).value();
        auto refused = projectFromJson(doc);
        REQUIRE_FALSE(refused);
        CHECK_MESSAGE(refused.error().path == path, refused.error().toString());
    }
}

TEST_CASE("pan modes and laws save, reopen and are refused on masters (ADR-0066)") {
    EditTxn t(Session::create(), 1);
    auto project = t.project();
    project.panLaw = {-4.5, true};
    t.setProject(project);
    Track ch;
    ch.parent = kMaster;
    ch.panMode = PanMode::Dual;
    ch.panLeft = -30;
    ch.panRight = 60;
    ch.width = -40;
    ch.panLaw = PanLaw{-6, false};
    const Id id = t.create(ch);
    t.modify<Track>(kMaster, [&](auto& tr) { tr.children.push_back(id); });
    auto s = t.commit();
    REQUIRE(s);
    auto doc = projectToJson(*s);
    CHECK(doc["project"]["pan_law"]["db"].asDouble() == -4.5);
    CHECK(doc["project"]["pan_law"]["boost"].asBool());
    CHECK(doc["tracks"][id.str()]["pan_mode"].asString() == "dual");
    CHECK(doc["tracks"][id.str()]["pan_law"]["db"].asDouble() == -6);
    CHECK_FALSE(doc["tracks"][id.str()]["pan_law"].find("boost"));  // false is the default
    // Defaults are not written: an untouched strip's bytes are what 2.8 wrote.
    CHECK_FALSE(doc["tracks"]["tr_0"].find("pan_mode"));
    CHECK_FALSE(doc["tracks"]["tr_0"].find("width"));
    CHECK_FALSE(doc["tracks"]["tr_0"].find("pan_law"));
    auto reopened = projectFromJson(json::parse(json::writeCanonical(doc)).value());
    REQUIRE(reopened);
    CHECK(reopened->project->panLaw == s->project->panLaw);
    CHECK(*reopened->tracks.find(id) == *s->tracks.find(id));
    CHECK(json::writeCanonical(projectToJson(*reopened)) == json::writeCanonical(doc));
    // A law off the list is refused with its path.
    auto bad = doc;
    bad["project"]["pan_law"]["db"] = -4.0;
    auto refused = projectFromJson(bad);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "pan_law.db");
    // A master balances: no pan mode or law.
    EditTxn m(*s, 2);
    m.modify<Track>(kMaster, [](auto& tr) { tr.panMode = PanMode::Width; });
    CHECK_FALSE(m.commit());
}
