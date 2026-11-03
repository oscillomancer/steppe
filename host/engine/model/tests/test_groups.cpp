// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/model/groups.h>
#include <supple/model/schemas.h>
#include <supple/model/versions.h>
#include <cmath>
using namespace supple;
using namespace supple::model;

TEST_CASE("track groups round trip, overlap and nested VCA ancestry") {
    EditTxn t(Session::create(), 1);
    Track track;
    track.parent = kMaster;
    const auto a = t.create(track), b = t.create(track), c = t.create(track);
    t.modify<Track>(kMaster, [&](auto& master) { master.children = {a, b, c}; });
    TrackGroup first;
    first.name = "Drums";
    first.members = {a, b};
    first.vca = Vca{6, false, false, {}};
    const auto g1 = t.create(first);
    TrackGroup second;
    second.members = {b, c};
    second.vca = Vca{6, false, false, {g1}};
    const auto g2 = t.create(second);
    TrackGroup third;
    third.members = {a};
    third.vca = Vca{6, false, false, {g1, g2}};
    const auto g3 = t.create(third);
    auto s = t.commit();
    REQUIRE(s);
    CHECK(linkedTracks(*s, a, "volume") == std::vector<Id>{a, b, c});
    CHECK(vcaControllers(*s, a) == std::vector<Id>{g1, g2, g3});
    CHECK(vcaControllers(*s, g1) == std::vector<Id>{g2, g3});
    auto level = effectiveTrackLevel(*s, a, 0);
    CHECK(level.requestedDb == 18);
    CHECK(level.appliedDb == doctest::Approx(20 * std::log10(4.0)));
    auto bytes = serializeProject(*s);
    auto loaded = parseProject(bytes);
    REQUIRE(loaded);
    CHECK(serializeProject(*loaded) == bytes);
    EditTxn suspend(*loaded, 2);
    suspend.modify<TrackGroup>(g1, [](auto& g) { g.active = false; });
    auto after = suspend.commit();
    REQUIRE(after);
    CHECK(linkedTracks(*after, a, "volume") == std::vector<Id>{a});
    CHECK(effectiveTrackLevel(*after, a, 0).requestedDb == 18);
    auto changes = diff(*loaded, *after);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].id == g1);
    EditTxn cycle(*s, 3);
    cycle.modify<TrackGroup>(g1, [&](auto& g) { g.vca->followers = {g3}; });
    auto bad = cycle.commit();
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "vca.cycle");
}
TEST_CASE("legacy opaque groups retain their value and never become active") {
    auto doc = projectToJson(Session::create());
    doc["schema"] = "supple.project/2.1";
    doc["groups"] = json::parse(R"({"gr_9":{"future":true}})").value();
    doc["extensions"]["legacy_track_groups"] = "already present";
    std::vector<schema::Warning> warnings;
    auto s = projectFromJson(doc, &warnings);
    REQUIRE(s);
    CHECK(s->groups.empty());
    CHECK(s->next[Kind::Group] > 9);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].code == "project.groups_legacy");
    auto saved = projectToJson(*s);
    CHECK(saved["extensions"]["legacy_track_groups"] == "already present");
    CHECK(saved["extensions"]["legacy_track_groups_1"] == doc["groups"]);
    CHECK(saved["schema"] == "supple.project/2.3");
    auto again = parseProject(serializeProject(*s));
    REQUIRE(again);
    CHECK(serializeProject(*again) == serializeProject(*s));
}
TEST_CASE("edit links match spans and preserve independent item-group symbols") {
    EditTxn t(Session::create(), 1);
    Track tr;
    tr.parent = kMaster;
    auto a = t.create(tr), b = t.create(tr);
    t.modify<Track>(kMaster, [&](auto& m) { m.children = {a, b}; });
    TrackGroup g;
    g.members = {a, b};
    g.links = {"edit"};
    auto gid = t.create(g);
    Item one;
    one.track = a;
    one.length = 4;
    one.group = Id{Kind::Group, 99};
    const auto i = t.create(one);
    t.create(one);  // Same-track overlapping items are not implicit peers.
    Item two = one;
    two.track = b;
    two.timebase = ItemTimebase::Time;
    two.length = 2;
    two.group.reset();
    const auto j = t.create(two);
    two.position = 1;
    const auto k = t.create(two);
    (void)k;
    auto s = t.commit();
    REQUIRE(s);
    CHECK(linkedItems(*s, i) == std::vector<Id>{i, j});
    auto bytes = serializeProject(*s);
    auto reloaded = parseProject(bytes);
    REQUIRE(reloaded);
    CHECK(reloaded->items.find(i)->group == one.group);
    CHECK(reloaded->groups.contains(gid));
    CHECK(reloaded->next[Kind::Group] > 99);
}
TEST_CASE("VCA envelopes combine at query time and invalid values refuse loading") {
    EditTxn t(Session::create(), 1);
    TrackGroup g;
    g.members = {kMaster};
    g.vca = Vca{};
    auto id = t.create(g);
    Envelope e;
    e.target = {id, "volume"};
    e.points = {{0, 0}, {4, -12}};
    t.create(e);
    auto s = t.commit();
    REQUIRE(s);
    CHECK(effectiveTrackLevel(*s, kMaster, 1).requestedDb == doctest::Approx(-6));
    auto doc = projectToJson(*s);
    doc["groups"][id.str()]["vca"]["volume_db"] = 13;
    CHECK_FALSE(projectFromJson(doc));
}
