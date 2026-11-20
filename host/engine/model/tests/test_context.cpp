// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The context track in the model (ADR-0095): points, the index, sections, the file
// and the rules for a file an older or newer build touched.
#include <doctest/doctest.h>

#include <supple/base/sha256.h>
#include <supple/model/context.h>
#include <supple/model/project_file.h>
#include <supple/model/schemas.h>
#include <supple/model/session.h>
#include <supple/model/versions.h>

using namespace supple;
using namespace supple::model;

namespace {

ContextPoint key(double beat, const char* tonic, const char* scale) {
    ContextPoint p;
    p.beat = beat;
    p.type = ContextType::Key;
    p.tonic = tonic;
    p.scale = scale;
    return p;
}

ContextPoint chord(double beat, const char* symbol) {
    ContextPoint p;
    p.beat = beat;
    p.type = ContextType::Chord;
    p.symbol = symbol;
    return p;
}

Session withPoints(const std::vector<ContextPoint>& points, std::uint64_t version = 1) {
    EditTxn t(Session::create(), version);
    for (const auto& p : points) t.create(p);
    return t.commit().value();
}

bool hasWarning(const std::vector<schema::Warning>& warnings, const std::string& code) {
    for (const auto& w : warnings)
        if (w.code == code) return true;
    return false;
}

}  // namespace

TEST_CASE("the index answers what holds at a beat") {
    const Session s = withPoints({key(0, "C", "major"), key(8, "Eb", "minor"), chord(0, "Cmaj7"),
                                  chord(4, "Fm7"), chord(6, "")});
    CHECK(keyAt(s, 0)->tonic == harmony::PitchClass{'C', 0});
    CHECK(keyAt(s, 7.99)->scale == harmony::Scale::Major);
    CHECK(keyAt(s, 8)->tonic == harmony::PitchClass{'E', -1});
    CHECK(keyAt(s, 8)->scale == harmony::Scale::Minor);
    CHECK(harmony::printChord(*chordAt(s, 3)) == "Cmaj7");
    CHECK(harmony::printChord(*chordAt(s, 4)) == "Fm7");
    CHECK_FALSE(chordAt(s, 6));  // an empty symbol: no chord from here
    CHECK_FALSE(chordAt(s, 100));
    CHECK(chordPointAt(s, 100).has_value());  // the point is there, and says "no chord"
    const Session empty = Session::create();
    CHECK_FALSE(keyAt(empty, 0));
    CHECK_FALSE(keyPointAt(empty, 0));
    const Session late = withPoints({key(4, "G", "major")});
    CHECK_FALSE(keyAt(late, 3.9));
    CHECK(keyAt(late, 4));
}

TEST_CASE("the view follows the points' storage, not every commit") {
    const Session a = withPoints({key(0, "C", "major")});
    // An edit that does not touch the points shares the view.
    EditTxn other(a, 2);
    other.modify<Track>(kMaster, [](Track& t) { t.name = "Main"; });
    const Session b = other.commit().value();
    CHECK(b.contextView == a.contextView);
    // One that does, builds a new one that sees the new point.
    EditTxn edit(b, 3);
    edit.create(chord(0, "Am"));
    const Session c = edit.commit().value();
    CHECK(c.contextView != b.contextView);
    CHECK(chordAt(c, 0));
    CHECK_FALSE(chordAt(b, 0));
    // The preview sees an uncommitted edit.
    EditTxn pending(c, 4);
    pending.create(key(2, "D", "dorian"));
    CHECK(keyAt(pending.preview(), 2)->scale == harmony::Scale::Dorian);
    CHECK(keyAt(c, 2)->scale == harmony::Scale::Major);
}

TEST_CASE("an unparsable point is data: it is kept, listed and answers nothing") {
    const Session s =
        withPoints({key(0, "C", "wholetone"), chord(0, "Cm11"), key(4, "H", "major")});
    CHECK(keyPointAt(s, 0).has_value());
    CHECK_FALSE(keyAt(s, 0));
    CHECK(keyWords(*s.context.find(*keyPointAt(s, 0))) == "C wholetone?");
    CHECK(chordPointAt(s, 0).has_value());
    CHECK_FALSE(chordAt(s, 0));
    CHECK_FALSE(keyAt(s, 5));
}

TEST_CASE("the section at a beat: latest start, then shorter, then lower id") {
    EditTxn t(Session::create(), 1);
    auto region = [&](double start, double end, const char* name, bool section = true) {
        Region r;
        r.start = start;
        r.end = end;
        r.name = name;
        r.section = section;
        return t.create(r);
    };
    const Id song = region(0, 32, "Song");
    const Id verse = region(0, 16, "Verse");
    const Id fill = region(12, 16, "Fill");
    const Id twin = region(12, 20, "Twin");
    const Id hidden = region(14, 15, "Stems", false);
    const Session s = t.commit().value();
    CHECK(sectionAt(s, 1) == verse);    // same start as Song, the shorter
    CHECK(sectionAt(s, 13) == fill);    // latest start; the shorter of Fill and Twin
    CHECK(sectionAt(s, 14.5) == fill);  // the hidden region is not a section
    CHECK(sectionAt(s, 17) == twin);    // Verse and Fill ended
    CHECK(sectionAt(s, 25) == song);
    CHECK_FALSE(sectionAt(s, 32));  // the end is not inside
    CHECK_FALSE(sectionAt(s, -1));
    (void)hidden;
    // A time-based region moves with the tempo.
    EditTxn u(Session::create(), 1);
    Region r;
    r.timebase = Timebase::Time;
    r.start = 2;  // seconds: beat 4 at 120 BPM
    r.end = 4;
    r.name = "Timed";
    u.create(r);
    const Session timed = u.commit().value();
    CHECK(sectionAt(timed, 3.9) == std::nullopt);
    CHECK(sectionAt(timed, 4));
    CHECK(sectionAt(timed, 7.9));
    CHECK_FALSE(sectionAt(timed, 8));
    EditTxn slower(timed, 2);
    slower.modify<TempoPointObj>({Kind::TempoPoint, 1}, [](TempoPointObj& tp) { tp.bpm = 60; });
    const Session half = slower.commit().value();
    CHECK_FALSE(sectionAt(half, 1.9));  // 2 s to 4 s is now beats 2 to 4
    CHECK(sectionAt(half, 2));
    CHECK(sectionAt(half, 3.9));
    CHECK_FALSE(sectionAt(half, 4));
    CHECK_FALSE(sectionAt(half, 5));  // inside before the tempo change
}

TEST_CASE("the invariants refuse two points of a type at a beat, a bad beat and too many") {
    {
        EditTxn t(Session::create(), 1);
        t.create(key(0, "C", "major"));
        t.create(key(0, "D", "major"));
        const auto bad = t.commit();
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == "model.invariant");
    }
    {  // a key and a chord may share a beat
        EditTxn t(Session::create(), 1);
        t.create(key(0, "C", "major"));
        t.create(chord(0, "C"));
        CHECK(t.commit());
    }
    for (double beat : {-1.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
        EditTxn t(Session::create(), 1);
        t.create(chord(beat, "C"));
        CHECK_FALSE(t.commit());
    }
    EditTxn many(Session::create(), 1);
    for (std::size_t i = 0; i <= kMaxContextPoints; ++i)
        many.create(chord(static_cast<double>(i), "C"));
    CHECK_FALSE(many.commit());
}

TEST_CASE("context points and a region's section flag save, reopen and serialize the same") {
    EditTxn t(Session::create(), 1);
    t.create(key(0, "Eb", "major"));
    t.create(chord(0, "Ebmaj7"));
    t.create(chord(4, "Cm7"));
    Region r;
    r.end = 8;
    r.name = "Stems";
    r.section = false;
    t.create(r);
    const Session s = t.commit().value();
    json::Value doc = projectToJson(s);
    CHECK(doc["schema"].asString() == std::string(kProjectSchema));
    REQUIRE(doc.find("context"));
    CHECK(doc["context"]["points"]["cx_1"]["tonic"].asString() == "Eb");
    CHECK(doc["context"]["points"]["cx_2"]["symbol"].asString() == "Ebmaj7");
    CHECK_FALSE(doc["context"]["points"]["cx_2"].find("tonic"));  // defaults are omitted
    CHECK(doc["project"]["next_ids"]["cx"].asDouble() == 4);
    CHECK_FALSE(doc["rulers"]["regions"]["rg_1"]["section"].asBool());
    std::vector<schema::Warning> warnings;
    auto back = projectFromJson(doc, &warnings);
    REQUIRE(back);
    CHECK(warnings.empty());
    CHECK(projectToJson(*back) == doc);
    CHECK(back->context.size() == 3);
    CHECK(back->next[Kind::Context] == 4);
    CHECK_FALSE(back->regions.find({Kind::Region, 1})->section);
    CHECK(harmony::printChord(*chordAt(*back, 5)) == "Cm7");
    // A project without points writes nothing new: no section, no counter.
    json::Value plain = projectToJson(Session::create());
    CHECK_FALSE(plain.find("context"));
    CHECK_FALSE(plain["project"]["next_ids"].find("cx"));
    CHECK_FALSE(plain["rulers"]["regions"].find("rg_1"));
}

TEST_CASE("a region without a section member is a section") {
    auto doc = projectToJson(Session::create());
    doc["rulers"]["regions"]["rg_1"] = json::parse(R"({"start":0,"end":4,"name":"Verse"})").value();
    doc["project"]["next_ids"]["rg"] = 2;
    auto s = projectFromJson(doc);
    REQUIRE(s);
    CHECK(s->regions.find({Kind::Region, 1})->section);
}

// A file that went through an older build comes back with the points and without the
// counter; one from a later build may hold things this build does not read. None of
// it fails the open (amendment G-c; round 53 F1).
TEST_CASE("a hand-edited context section opens: derived counter, repaired duplicates, data kept") {
    auto base = [] {
        const Session s = withPoints({key(0, "C", "major"), chord(0, "C"), chord(4, "G")});
        return projectToJson(s);
    };
    SUBCASE("the counter is missing") {
        auto doc = base();
        doc["project"]["next_ids"].asObject().erase("cx");
        std::vector<schema::Warning> warnings;
        auto s = projectFromJson(doc, &warnings);
        REQUIRE(s);
        CHECK(s->next[Kind::Context] == 4);
        CHECK(hasWarning(warnings, "project.counter_derived"));
        // The next point does not reuse an id.
        EditTxn t(*s, 9);
        CHECK(t.create(chord(8, "D")) == Id{Kind::Context, 4});
    }
    SUBCASE("the counter is low") {
        auto doc = base();
        doc["project"]["next_ids"]["cx"] = 2;
        std::vector<schema::Warning> warnings;
        auto s = projectFromJson(doc, &warnings);
        REQUIRE(s);
        CHECK(s->next[Kind::Context] == 4);
        CHECK(hasWarning(warnings, "project.counter_derived"));
    }
    SUBCASE("the counter is right: no warning") {
        std::vector<schema::Warning> warnings;
        REQUIRE(projectFromJson(base(), &warnings));
        CHECK_FALSE(hasWarning(warnings, "project.counter_derived"));
    }
    SUBCASE("two points of a type at one beat: the lowest id stays") {
        auto doc = base();
        doc["context"]["points"]["cx_9"] =
            json::parse(R"({"beat":4,"type":"chord","symbol":"F"})").value();
        doc["context"]["points"]["cx_5"] =
            json::parse(R"({"beat":0,"type":"key","tonic":"D","scale":"minor"})").value();
        std::vector<schema::Warning> warnings;
        auto s = projectFromJson(doc, &warnings);
        REQUIRE(s);
        CHECK(s->context.size() == 3);
        CHECK(s->context.contains({Kind::Context, 2}));
        CHECK_FALSE(s->context.contains({Kind::Context, 9}));
        CHECK_FALSE(s->context.contains({Kind::Context, 5}));
        CHECK(harmony::printChord(*chordAt(*s, 4)) == "G");
        CHECK(hasWarning(warnings, "project.context_duplicate"));
        CHECK(s->next[Kind::Context] == 4);  // the dropped ids' numbers do not move it
    }
    SUBCASE("a chord quality and a scale this build lacks are kept as data") {
        auto doc = base();
        doc["context"]["points"]["cx_1"]["scale"] = "whole_tone";
        doc["context"]["points"]["cx_3"]["symbol"] = "Cm11";
        std::vector<schema::Warning> warnings;
        auto s = projectFromJson(doc, &warnings);  // the open never fails for these
        REQUIRE(s);
        CHECK(s->context.find({Kind::Context, 3})->symbol == "Cm11");
        CHECK(s->context.find({Kind::Context, 1})->scale == "whole_tone");
        CHECK_FALSE(chordAt(*s, 5));
        CHECK_FALSE(keyAt(*s, 0));
        // and it is written back as it was read
        CHECK(projectToJson(*s) == doc);
    }
    SUBCASE("an unknown member of a point is ignored with a warning") {
        auto doc = base();
        doc["context"]["points"]["cx_2"]["voicing"] = "drop2";
        std::vector<schema::Warning> warnings;
        REQUIRE(projectFromJson(doc, &warnings));
        CHECK(hasWarning(warnings, "schema.unknown_member"));
    }
}

TEST_CASE("diff reports context points like any other object") {
    const Session a = Session::create();
    EditTxn t(a, 2);
    const Id id = t.create(chord(0, "C"));
    const Session b = t.commit().value();
    const auto added = diff(a, b);
    REQUIRE(added.size() == 1);
    CHECK(added[0].id == id);
    CHECK(added[0].op == Change::Op::Add);
    EditTxn u(b, 3);
    u.put(id, chord(0, "G"));
    const auto changed = diff(b, u.commit().value());
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].op == Change::Op::Update);
}

namespace {
// What a build that does not know the kind writes: no cx counter anywhere.
void stripCounters(json::Value& v, bool inNextIds = false) {
    if (v.isObject()) {
        if (inNextIds) v.asObject().erase("cx");
        for (auto& m : v.asObject()) stripCounters(m.value, m.key == "next_ids");
    } else if (v.isArray()) {
        for (auto& e : v.asArray()) stripCounters(e);
    }
}

std::uint64_t addPoint(VersionStore& vs, ContextPoint p) {
    EditTxn t(*vs.head(), vs.nextVersion());
    const Id id = t.create(p);
    auto changes = t.changes();
    auto s = t.commit();
    REQUIRE(s);
    TxnRecord txn{"chord.set", "test", {{"chord.set", json::Value::object()}}, {}};
    auto v = vs.commit(std::move(*s), txn, std::move(changes), {id});
    REQUIRE(v);
    return *v;
}
}  // namespace

// Saved branches carry the high-water mark of the ids their lineage allocated, so a
// merge never meets two different objects under one id. A file an older build
// rewrote has no cx counter anywhere; the mark is then rebuilt from the versions the
// branch can still reach, including an undone one waiting in redo (round 53).
TEST_CASE("a saved branch without the cx counter is not handed an id its lineage holds") {
    VersionStore vs(Session::create());
    addPoint(vs, chord(0, "C"));
    addPoint(vs, chord(4, "F"));  // cx_2, then undone below
    REQUIRE(vs.createBranch("other", "main"));
    REQUIRE(vs.undo());
    REQUIRE(vs.head()->context.size() == 1);
    json::Value doc = projectFileToJson(vs, true);
    REQUIRE(doc.find("branches"));
    stripCounters(doc);
    {  // the older build wrote the file, so main's hash is of the document as it left it
        json::Value branches = *doc.find("branches");
        doc.asObject().erase("branches");
        branches["main_hash"] = "sha256:" + sha256Hex(json::writeCanonical(doc));
        doc["branches"] = std::move(branches);
    }
    auto opened = projectFileFromJson(doc);
    REQUIRE(opened);
    REQUIRE_FALSE(opened->branchesDropped);
    VersionStore& back = opened->store;
    REQUIRE(back.branches().size() == 2);
    // Both branches continue past cx_2, which "other" still holds and main's redo does.
    EditTxn t(*back.head("main"), back.nextVersion());
    CHECK(t.create(chord(8, "G")) == Id{Kind::Context, 3});
    EditTxn u(*back.head("other"), back.nextVersion());
    CHECK(u.create(chord(8, "G")) == Id{Kind::Context, 3});
}
