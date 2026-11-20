// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Scenes and clips in the model (ADR-0097, session-model "Scenes and clips").
#include <doctest/doctest.h>
#include <supple/base/sha256.h>
#include <supple/json/parse.h>
#include <supple/json/write.h>
#include <supple/model/freeze.h>
#include <supple/model/launcher.h>
#include <supple/model/project_file.h>
#include <supple/model/schemas.h>
#include <supple/model/take_owner.h>
#include <supple/model/versions.h>

#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace supple;
using namespace supple::model;

namespace {

// Channel A and a buss; scenes Verse (order 0) and Chorus (order 1); a MIDI
// clip on A in Verse holding one take of an unpooled source, and an item on
// A holding another take.
struct Fixture {
    Session s;
    Id a, buss, verse, chorus, clip, clipTake, midi, item, itemTake;
};

Id addTrack(EditTxn& t, StripType type = StripType::Channel) {
    Track tr;
    tr.type = type;
    tr.parent = kMaster;
    const Id id = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(id); });
    return id;
}

Fixture fixture() {
    Fixture f;
    EditTxn t(Session::create(), 1);
    f.a = addTrack(t);
    f.buss = addTrack(t, StripType::Buss);
    f.verse = t.create(Scene{"Verse", "", 0});
    f.chorus = t.create(Scene{"Chorus", "#c08040", 1});
    f.midi = t.create(MidiSource{});
    Take take;
    take.source = f.midi;
    f.clipTake = t.create(take);
    Clip clip;
    clip.track = f.a;
    clip.scene = f.verse;
    clip.takes = {f.clipTake};
    clip.activeTake = f.clipTake;
    clip.length = 8;
    clip.loop = {true, 0, 4};
    clip.name = "Bass line";
    f.clip = t.create(clip);
    f.itemTake = t.create(Take{});
    Item item;
    item.track = f.a;
    item.length = 4;
    item.takes = {f.itemTake};
    item.activeTake = f.itemTake;
    f.item = t.create(item);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    f.s = *s;
    return f;
}

template <class T>
Result<Session> edit(const Session& s, Id id, std::function<void(T&)> f) {
    EditTxn t(s, s.version + 1);
    t.modify<T>(id, std::move(f));
    return t.commit();
}

// Every invariant path and message in `e`, one per line.
std::string messages(const Error& e) {
    std::string out = e.path + " " + e.message + "\n";
    for (const auto& d : e.details) out += d.path + " " + d.message + "\n";
    return out;
}
bool says(const Result<Session>& r, const std::string& text) {
    return !r && messages(r.error()).find(text) != std::string::npos;
}

}  // namespace

// --- ids and the file ----------------------------------------------------------

TEST_CASE("launcher: sc and cl ids parse and allocate") {
    const Fixture f = fixture();
    CHECK(f.verse.str() == "sc_1");
    CHECK(f.clip.str() == "cl_1");
    CHECK(Id::parse("sc_1") == f.verse);
    CHECK(Id::parse("cl_1") == f.clip);
    CHECK(prefix(Kind::Scene) == "sc");
    CHECK(prefix(Kind::Clip) == "cl");
}

TEST_CASE("launcher: scenes and clips save, reopen and serialize the same") {
    const Fixture f = fixture();
    const std::string bytes = serializeProject(f.s);
    CHECK(bytes.find("\"supple.project/2.18\"") != std::string::npos);
    auto doc = json::parse(bytes).value();
    json::Value& sc = doc["scenes"]["sc_2"];
    CHECK(sc["name"] == "Chorus");
    CHECK(sc["color"] == "#c08040");
    CHECK(sc["order"] == 1);
    CHECK_FALSE(doc["scenes"]["sc_1"].find("color"));
    json::Value& cl = doc["clips"]["cl_1"];
    CHECK(cl["track"] == f.a.str());
    CHECK(cl["scene"] == f.verse.str());
    CHECK(cl["kind"] == "midi");  // always written: it is required
    CHECK(cl["active_take"] == f.clipTake.str());
    CHECK(cl["length"] == 8.0);
    CHECK(cl["loop"]["on"] == true);
    CHECK(cl["loop"]["length"] == 4.0);
    CHECK_FALSE(cl["loop"].find("start"));  // 0 is omitted
    CHECK_FALSE(cl.find("mode"));           // loop is the default
    CHECK_FALSE(cl.find("quantize"));       // the project's
    CHECK_FALSE(cl.find("mute"));
    CHECK_FALSE(doc["project"].find("launch_quantize"));  // bar is the default
    CHECK(doc["project"]["next_ids"]["sc"] == 3);
    CHECK(doc["project"]["next_ids"]["cl"] == 2);
    // After the modulation routes' place, before the blobs.
    CHECK(bytes.find("\"cables\"") == std::string::npos);
    CHECK(bytes.find("\"scenes\"") < bytes.find("\"clips\""));
    CHECK(bytes.find("\"clips\"") < bytes.find("\"blobs\""));
    std::vector<schema::Warning> warnings;
    auto back = parseProject(bytes, &warnings);
    REQUIRE_MESSAGE(back, (back ? "" : back.error().toString()));
    CHECK(warnings.empty());
    CHECK(*back->clips.find(f.clip) == *f.s.clips.find(f.clip));
    CHECK(*back->scenes.find(f.chorus) == *f.s.scenes.find(f.chorus));
    CHECK(back->next[Kind::Scene] == 3);
    CHECK(back->next[Kind::Clip] == 2);
    CHECK(serializeProject(*back) == bytes);
}

TEST_CASE("launcher: every clip field and the project quantize round-trip") {
    const Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    t.modify<Clip>(f.clip, [](Clip& c) {
        c.mode = ClipMode::Once;
        c.quantize = LaunchQuantize::Sixteenth;
        c.mute = true;
        c.gainDb = -3.5;
        c.color = "#406080";
        c.loop = {false, 1.5, 2};
    });
    Clip pad;
    pad.track = f.a;
    pad.scene = f.chorus;
    pad.kind = ClipKind::Audio;
    pad.length = 6.5;
    const Id padId = t.create(pad);
    Project p = t.project();
    p.launchQuantize = LaunchQuantize::None;
    t.setProject(p);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    const std::string bytes = serializeProject(*s);
    auto doc = json::parse(bytes).value();
    CHECK(doc["clips"]["cl_1"]["mode"] == "once");
    CHECK(doc["clips"][padId.str()]["kind"] == "audio");
    CHECK(doc["clips"][padId.str()]["length"] == 6.5);
    CHECK(doc["clips"]["cl_1"]["quantize"] == "1/16");
    CHECK(doc["clips"]["cl_1"]["gain_db"] == -3.5);
    CHECK(doc["project"]["launch_quantize"] == "none");
    auto back = parseProject(bytes);
    REQUIRE(back);
    CHECK(*back->clips.find(f.clip) == *s->clips.find(f.clip));
    CHECK(*back->clips.find(padId) == *s->clips.find(padId));
    CHECK(back->project->launchQuantize == LaunchQuantize::None);
    CHECK(serializeProject(*back) == bytes);
    // Every quantize name reads back as itself.
    for (const auto& [value, name] : kLaunchQuantizeNames) {
        CHECK(launchQuantizeName(value) == name);
        auto one = edit<Clip>(*s, f.clip, [v = value](Clip& c) { c.quantize = v; });
        REQUIRE(one);
        auto again = parseProject(serializeProject(*one));
        REQUIRE(again);
        CHECK(again->clips.find(f.clip)->quantize == value);
    }
}

TEST_CASE("launcher: a project without scenes writes no sections or counters") {
    EditTxn t(Session::create(), 1);
    addTrack(t);
    const std::string bytes = serializeProject(t.commit().value());
    CHECK(bytes.find("\"scenes\"") == std::string::npos);
    CHECK(bytes.find("\"clips\"") == std::string::npos);
    auto doc = json::parse(bytes).value();
    CHECK_FALSE(doc["project"]["next_ids"].find("sc"));
    CHECK_FALSE(doc["project"]["next_ids"].find("cl"));
}

TEST_CASE("launcher: a 2.17 file loads unchanged and saves as 2.18") {
    EditTxn t(Session::create(), 1);
    addTrack(t);
    const Session s = t.commit().value();
    auto doc = json::parse(serializeProject(s)).value();
    doc["schema"] = "supple.project/2.17";
    std::vector<schema::Warning> warnings;
    auto opened = parseProject(json::writeCompact(doc), &warnings);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    CHECK(warnings.empty());
    CHECK(opened->next[Kind::Scene] == 1);
    CHECK(opened->next[Kind::Clip] == 1);
    CHECK(serializeProject(*opened) == serializeProject(s));
}

// --- the take owner --------------------------------------------------------------

TEST_CASE("launcher: a clip owns its takes") {
    const Fixture f = fixture();
    CHECK(takeOwner(f.s, f.clipTake) == f.clip);
    CHECK(takeOwner(f.s, f.itemTake) == f.item);
    REQUIRE(ownerTakes(f.s, f.clip));
    CHECK(*ownerTakes(f.s, f.clip) == std::vector<Id>{f.clipTake});
    const TakeOwners owners(f.s);
    CHECK(owners.of(f.clipTake) == f.clip);
    CHECK(owners.size() == 2);
    CHECK(clipAt(f.s, f.a, f.verse) == f.clip);
    CHECK_FALSE(clipAt(f.s, f.a, f.chorus));
}

TEST_CASE("launcher: a take held by an item and a clip is refused") {
    const Fixture f = fixture();
    auto r = edit<Clip>(f.s, f.clip, [&](Clip& c) { c.takes.push_back(f.itemTake); });
    CHECK(says(r, "/clips/cl_1/takes " + f.itemTake.str() + " also belongs to " + f.item.str()));
}

TEST_CASE("launcher: a take no item or clip holds is an orphan") {
    const Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    t.modify<Clip>(f.clip, [](Clip& c) {
        c.takes.clear();
        c.activeTake.reset();
    });
    CHECK(says(t.commit(), f.clipTake.str() + " belongs to no item or clip"));
}

// --- invariants --------------------------------------------------------------------

TEST_CASE("launcher: clips live on channels only") {
    const Fixture f = fixture();
    CHECK(says(edit<Clip>(f.s, f.clip, [&](Clip& c) { c.track = f.buss; }),
               "/clips/cl_1/track " + f.buss.str() + " is not a channel"));
    CHECK(says(edit<Clip>(f.s, f.clip, [&](Clip& c) { c.track = kMaster; }),
               "/clips/cl_1/track tr_0 is not a channel"));
    CHECK(says(edit<Clip>(f.s, f.clip, [&](Clip& c) { c.track = Id{Kind::Track, 99}; }),
               "/clips/cl_1/track track tr_99 does not exist"));
}

TEST_CASE("launcher: a clip's scene must exist, one clip per track and scene") {
    const Fixture f = fixture();
    CHECK(says(edit<Clip>(f.s, f.clip, [](Clip& c) { c.scene = Id{Kind::Scene, 99}; }),
               "/clips/cl_1/scene scene sc_99 does not exist"));
    EditTxn t(f.s, f.s.version + 1);
    Clip second;
    second.track = f.a;
    second.scene = f.verse;
    second.length = 4;
    t.create(second);
    CHECK(says(t.commit(), "/clips/cl_2 cl_1 is already in " + f.a.str() + "'s sc_1"));
}

TEST_CASE("launcher: lengths and loops must be positive and finite") {
    const Fixture f = fixture();
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()})
        CHECK(
            says(edit<Clip>(f.s, f.clip, [&](Clip& c) { c.length = bad; }), "/clips/cl_1/length"));
    CHECK(
        says(edit<Clip>(f.s, f.clip, [](Clip& c) { c.loop = {true, 0, 0}; }), "/clips/cl_1/loop"));
    CHECK(
        says(edit<Clip>(f.s, f.clip, [](Clip& c) { c.loop = {true, -1, 2}; }), "/clips/cl_1/loop"));
    CHECK(edit<Clip>(f.s, f.clip, [](Clip& c) { c.loop = {false, 0, 0}; }));  // off: unchecked
}

TEST_CASE("launcher: a clip's active take is one of its takes") {
    const Fixture f = fixture();
    CHECK(says(edit<Clip>(f.s, f.clip, [&](Clip& c) { c.activeTake = f.itemTake; }),
               "/clips/cl_1/active_take"));
    CHECK(says(edit<Clip>(f.s, f.clip, [&](Clip& c) { c.activeTake.reset(); }),
               "/clips/cl_1/active_take a clip with takes needs an active take"));
}

TEST_CASE("launcher: each take's source matches the clip's kind") {
    const Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    MediaSource media;
    media.path = "a.wav";
    media.sampleRate = 48000;
    media.frames = 96000;
    Take audio;
    audio.source = t.create(media);
    const Id audioTake = t.create(audio);
    const Id empty = t.create(Take{});
    t.modify<Clip>(f.clip, [&](Clip& c) { c.takes.push_back(empty); });  // no source: fine
    REQUIRE(t.preview().clips.find(f.clip)->takes.size() == 2);
    t.modify<Clip>(f.clip, [&](Clip& c) { c.takes.push_back(audioTake); });
    CHECK(says(t.commit(),
               "/clips/cl_1/takes " + audioTake.str() + " holds audio; the clip is midi"));
    // The kind is stored, so a clip whose takes are all MIDI cannot claim audio.
    CHECK(says(edit<Clip>(f.s, f.clip, [](Clip& c) { c.kind = ClipKind::Audio; }),
               f.clipTake.str() + " holds midi; the clip is audio"));
    // An audio clip with no takes, or a sourceless one, is fine.
    EditTxn u(f.s, f.s.version + 1);
    Clip pad;
    pad.track = f.a;
    pad.scene = f.chorus;
    pad.kind = ClipKind::Audio;
    pad.length = 6;
    const Id sourceless = u.create(Take{});
    pad.takes = {sourceless};
    pad.activeTake = sourceless;
    u.create(pad);
    CHECK(u.commit());
}

TEST_CASE("launcher: scene orders are distinct and not negative") {
    const Fixture f = fixture();
    CHECK(says(edit<Scene>(f.s, f.chorus, [](Scene& s) { s.order = 0; }),
               "/scenes/sc_2/order order 0 is also sc_1's"));
    CHECK(says(edit<Scene>(f.s, f.chorus, [](Scene& s) { s.order = -1; }), "/scenes/sc_2/order"));
}

TEST_CASE("launcher: at most 1024 scenes and 16384 clips") {
    EditTxn t(Session::create(), 1);
    std::vector<Id> tracks;
    for (int k = 0; k < 17; ++k) tracks.push_back(addTrack(t));
    std::vector<Id> scenes;
    for (int k = 0; k < 1024; ++k) scenes.push_back(t.create(Scene{"", "", k}));
    REQUIRE(t.preview().scenes.size() == kMaxScenes);
    Clip c;
    c.length = 1;
    std::size_t made = 0;
    for (Id track : tracks)
        for (Id scene : scenes)
            if (made++ < kMaxClips) {
                c.track = track;
                c.scene = scene;
                t.create(c);
            }
    auto full = t.commit();
    REQUIRE_MESSAGE(full, (full ? "" : full.error().toString()));
    EditTxn more(*full, 2);
    more.create(Scene{"", "", 1024});
    CHECK(says(more.commit(), "/scenes more than 1024 scenes"));
    EditTxn extra(*full, 2);
    c.track = tracks.back();
    c.scene = scenes.back();
    extra.create(c);
    CHECK(says(extra.commit(), "/clips more than 16384 clips"));
}

// --- freeze, diff, older builds -------------------------------------------------------

TEST_CASE("launcher: clips are outside a frozen track's freeze hash") {
    const Fixture f = fixture();
    const std::string before = freezeDependencyHash(f.s, f.a);
    auto renamed = edit<Clip>(f.s, f.clip, [](Clip& c) { c.name = "Other"; });
    REQUIRE(renamed);
    CHECK(freezeDependencyHash(*renamed, f.a) == before);
    auto moved = edit<Item>(f.s, f.item, [](Item& it) { it.position = 2; });
    REQUIRE(moved);
    CHECK(freezeDependencyHash(*moved, f.a) != before);
}

TEST_CASE("launcher: diffs list scene and clip changes by field") {
    const Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    t.modify<Clip>(f.clip, [](Clip& c) { c.mute = true; });
    t.modify<Scene>(f.verse, [](Scene& s) { s.name = "Verse 1"; });
    const Id bridge = t.create(Scene{"Bridge", "", 2});
    auto s = t.commit();
    REQUIRE(s);
    bool clip = false, scene = false, added = false;
    for (const auto& d : diff(f.s, *s)) {
        if (d.id == f.clip && d.op == Change::Op::Update)
            clip = d.fields == std::vector<std::string>{"mute"};
        if (d.id == f.verse && d.op == Change::Op::Update)
            scene = d.fields == std::vector<std::string>{"name"};
        if (d.id == bridge) added = d.op == Change::Op::Add;
    }
    CHECK(clip);
    CHECK(scene);
    CHECK(added);
}

TEST_CASE("launcher: a file that went through an older build still opens") {
    // An older build keeps scenes and clips as unknown sections and drops
    // their counters (G-c). What it can leave behind is a clip that refers
    // to something it deleted: here its track, and (by hand edits that stand
    // in for the rest) its scene, a take, or its active take.
    const Fixture f = fixture();
    EditTxn t(f.s, f.s.version + 1);
    const Id b = addTrack(t);
    const Id tB = t.create(Take{}), tC = t.create(Take{}), tD = t.create(Take{}),
             tE = t.create(Take{}), tE2 = t.create(Take{});
    Clip c;
    c.length = 4;
    c.track = b;  // cl_2: its track goes
    c.scene = f.chorus;
    c.takes = {tB};
    c.activeTake = tB;
    t.create(c);
    const Id bridge = t.create(Scene{"Bridge", "", 2});
    c.track = f.a;  // cl_3: its scene goes
    c.scene = bridge;
    c.takes = {tC};
    c.activeTake = tC;
    t.create(c);
    c.scene = f.chorus;  // cl_4: its take goes
    c.takes = {tD};
    c.activeTake = tD;
    t.create(c);
    const Id b2 = addTrack(t);
    c.track = b2;  // cl_5: its active take goes, its other take stays
    c.takes = {tE, tE2};
    c.activeTake = tE;
    t.create(c);
    Envelope env;  // an envelope on tB goes with it
    env.target = {tB, "volume"};
    env.points = {{0, 0}};
    t.create(env);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));

    auto doc = json::parse(serializeProject(*s)).value();
    doc["project"]["next_ids"].asObject().erase("sc");
    doc["project"]["next_ids"].asObject().erase("cl");
    doc["tracks"].asObject().erase(b.str());
    json::Value children = json::Value::array();
    for (const auto& child : doc["tracks"]["tr_0"]["children"].asArray())
        if (child != b.str()) children.push_back(child);
    doc["tracks"]["tr_0"]["children"] = std::move(children);
    doc["scenes"].asObject().erase(bridge.str());
    doc["takes"].asObject().erase(tD.str());
    doc["takes"].asObject().erase(tE.str());
    doc["clips"]["cl_5"]["takes"] = json::parse(R"([")" + tE2.str() + R"("])").value();

    std::vector<schema::Warning> warnings;
    auto opened = parseProject(json::writeCompact(doc), &warnings);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    CHECK(opened->clips.size() == 1);
    CHECK(opened->clips.contains(f.clip));
    CHECK(opened->next[Kind::Clip] == 6);     // past cl_5, the highest the file held
    CHECK(opened->next[Kind::Scene] == 3);    // past sc_2 (sc_3 went)
    CHECK_FALSE(opened->takes.contains(tB));  // the dropped clips' takes went with them
    CHECK_FALSE(opened->takes.contains(tC));
    CHECK_FALSE(opened->takes.contains(tE2));
    CHECK(opened->envelopes.empty());  // the envelope on tB
    std::vector<std::string> dropped, derived;
    for (const auto& w : warnings) {
        if (w.code == "project.clip_dropped") {
            dropped.push_back(w.path + ": " + w.message);
        }
        if (w.code == "project.counter_derived") derived.push_back(w.path);
    }
    CHECK(dropped ==
          std::vector<std::string>{
              "/clips/cl_2: dropped clip cl_2: track " + b.str() + " does not exist",
              "/clips/cl_3: dropped clip cl_3: scene " + bridge.str() + " does not exist",
              "/clips/cl_4: dropped clip cl_4: active take " + tD.str() + " does not exist",
              "/clips/cl_5: dropped clip cl_5: active take " + tE.str() + " does not exist"});
    CHECK(derived == std::vector<std::string>{"/project/next_ids/sc", "/project/next_ids/cl"});
    // The kept project saves cleanly.
    auto again = parseProject(serializeProject(*opened));
    REQUIRE_MESSAGE(again, (again ? "" : again.error().toString()));
}

TEST_CASE("launcher: a clip's missing take is dangling; a clip fault is not") {
    const Fixture f = fixture();
    // A take listed but gone (the active one kept) is dropped as dangling.
    {
        EditTxn t(f.s, f.s.version + 1);
        const Id extra = t.create(Take{});
        t.modify<Clip>(f.clip, [&](Clip& c) { c.takes.push_back(extra); });
        auto s = t.commit();
        REQUIRE(s);
        auto doc = json::parse(serializeProject(*s)).value();
        doc["takes"].asObject().erase(extra.str());
        std::vector<schema::Warning> warnings;
        auto opened = parseProject(json::writeCompact(doc), &warnings);
        REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
        CHECK(opened->clips.empty());
        CHECK_FALSE(opened->takes.contains(f.clipTake));
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].message == "dropped clip cl_1: take " + extra.str() + " does not exist");
    }
    // Two clips in one cell, or a take an item also holds, is no older
    // build's doing: the open fails with the invariant (F5's ruling).
    auto doc = json::parse(serializeProject(f.s)).value();
    doc["clips"]["cl_5"] = json::parse(R"({"track": ")" + f.a.str() +
                                       R"(", "scene": "sc_1", "kind": "midi", "length": 2})")
                               .value();
    doc["project"]["next_ids"]["cl"] = 6;
    auto shared = parseProject(json::writeCompact(doc));
    REQUIRE_FALSE(shared);
    CHECK(messages(shared.error()).find("/clips/cl_5 cl_1 is already in") != std::string::npos);
    doc = json::parse(serializeProject(f.s)).value();
    doc["items"][f.item.str()]["takes"].push_back(f.clipTake.str());
    auto claimed = parseProject(json::writeCompact(doc));
    REQUIRE_FALSE(claimed);
    CHECK(messages(claimed.error()).find("also belongs to") != std::string::npos);
    doc = json::parse(serializeProject(f.s)).value();
    doc["tracks"][f.a.str()]["type"] = "buss";
    doc["items"].asObject().erase(f.item.str());  // items have their own strip rule
    doc["takes"].asObject().erase(f.itemTake.str());
    auto buss = parseProject(json::writeCompact(doc));
    REQUIRE_FALSE(buss);
    CHECK(messages(buss.error()).find("/clips/cl_1/track " + f.a.str() + " is not a channel") !=
          std::string::npos);
}

TEST_CASE("launcher: a bad length in a file still fails the open") {
    const Fixture f = fixture();
    auto doc = json::parse(serializeProject(f.s)).value();
    doc["clips"]["cl_1"]["length"] = 0.0;
    auto opened = parseProject(json::writeCompact(doc));
    REQUIRE_FALSE(opened);
    CHECK(messages(opened.error()).find("/clips/cl_1/length") != std::string::npos);
}

TEST_CASE("launcher: a pre-2.18 build's view: clip takes are orphans") {
    // Documents the ADR's cost: without the clips section (an older build
    // keeps it as unknown, so to it the section is not there), the
    // invariant refuses the clip's take.
    const Fixture f = fixture();
    auto doc = json::parse(serializeProject(f.s)).value();
    doc.asObject().erase("clips");
    auto opened = parseProject(json::writeCompact(doc));
    REQUIRE_FALSE(opened);
    CHECK(messages(opened.error()).find(f.clipTake.str() + " belongs to no item") !=
          std::string::npos);
}

TEST_CASE("launcher: lost sc and cl counters never reuse an id another branch holds") {
    // As for mr (cfa2fee5): main removed a scene and its clip after a branch
    // forked with them; with the counters lost, main's own objects alone
    // would give sc_3 and cl_2 out again under the branch's ids.
    const Fixture f = fixture();
    VersionStore vs(f.s);
    const auto step = [&](auto&& edit) {
        EditTxn t(*vs.head(), vs.nextVersion());
        std::vector<Id> created;
        edit(t, created);
        auto changes = t.changes();
        auto s = t.commit();
        REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
        REQUIRE(vs.commit(std::move(*s), TxnRecord{"clip", "test", {}, {}}, std::move(changes),
                          std::move(created)));
    };
    Id bridge, clip;
    step([&](EditTxn& t, std::vector<Id>& c) {
        bridge = t.create(Scene{"Bridge", "", 2});
        Clip cl;
        cl.track = f.a;
        cl.scene = bridge;
        cl.length = 4;
        clip = t.create(cl);
        c = {bridge, clip};
    });
    REQUIRE(vs.createBranch("agent/claude/keep", "main"));
    step([&](EditTxn& t, std::vector<Id>&) {
        t.remove<Clip>(clip);
        t.remove<Scene>(bridge);
    });
    auto doc = json::parse(serializeProjectFile(vs, true)).value();
    for (const char* k : {"sc", "cl"}) {
        doc["project"]["next_ids"].asObject().erase(k);
        for (auto& ref : doc["branches"]["refs"].asObject())
            ref.value["next_ids"].asObject().erase(k);
    }
    auto main = doc;
    main.asObject().erase("branches");
    doc["branches"]["main_hash"] = "sha256:" + sha256Hex(json::writeCanonical(main));
    auto opened = projectFileFromJson(doc);
    REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
    REQUIRE_MESSAGE(!opened->branchesDropped,
                    (opened->branchesDropped ? opened->branchesDropped->toString() : ""));
    CHECK(opened->store.head("agent/claude/keep")->clips.contains(clip));
    CHECK(opened->store.head("main")->next[Kind::Scene] > bridge.n);
    CHECK(opened->store.head("main")->next[Kind::Clip] > clip.n);
    // A new scene and clip on main get ids the branch does not hold.
    EditTxn t(*opened->store.head("main"), opened->store.nextVersion());
    const Id freshScene = t.create(Scene{"Outro", "", 2});
    CHECK(freshScene != bridge);
}
