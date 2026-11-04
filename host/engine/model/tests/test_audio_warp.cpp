// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include "supple/model/audio_warp.h"
#include "supple/model/schemas.h"
#include "supple/model/versions.h"
using namespace supple;
using namespace supple::model;

TEST_CASE("audio warp maps original source seconds and survives slip split and rate edits") {
    Take t;
    t.startOffset = 2;
    t.playrate = 2;
    t.stretchMarkers = {{2, 0, 0}, {3, 0.75, 0}, {4, 1, 0}};
    CHECK(audioSourceAt(t, 0.375) == doctest::Approx(2.5));
    CHECK(audioItemAt(t, 2.5) == doctest::Approx(0.375));
    CHECK(audioSourceAt(t, -0.5) == doctest::Approx(1));
    CHECK(audioSourceAt(t, 1.5) == doctest::Approx(5));
    const auto original = t;
    rebaseAudioTake(t, 0.5);
    for (double x : {-0.25, 0., 0.125, 0.5, 1.})
        CHECK(audioSourceAt(t, x) == doctest::Approx(audioSourceAt(original, x + 0.5)));
    slipAudioTake(t, 1.25);
    CHECK(audioSourceAt(t, 0) == doctest::Approx(audioSourceAt(original, 0.5) + 1.25));
    rateAudioTake(t, 1);
    CHECK(audioSourceAt(t, 0.5) == doctest::Approx(audioSourceAt(original, 0.75) + 1.25));
    t.stretchMarkers[1].itemPos = t.stretchMarkers[0].itemPos;
    CHECK_FALSE(validateWarpMap(t));
    CHECK_FALSE(activeWarp(t));
}

TEST_CASE("an unwarped take without preserved pitch maps source time at playback speed") {
    // Playback reads the source at playrate x pitch ratio; a split rebases there.
    Take t;
    t.startOffset = 1;
    t.playrate = 1.5;
    t.preservePitch = false;
    t.pitch = 12;  // twice as fast again
    CHECK(audioSpeed(t) == doctest::Approx(3));
    CHECK(audioSourceAt(t, 2) == doctest::Approx(7));
    CHECK(audioItemAt(t, 7) == doctest::Approx(2));
    rebaseAudioTake(t, 2);
    CHECK(t.startOffset == doctest::Approx(7));
    t.preservePitch = true;
    CHECK(audioSpeed(t) == doctest::Approx(1.5));
}

TEST_CASE("audio warp schema 2.2 round trip keeps object markers hits and typed peer references") {
    EditTxn tx(Session::create(), 1);
    Track tr;
    tr.parent = kMaster;
    const auto track = tx.create(tr);
    tx.modify<Track>(kMaster, [&](auto& m) { m.children.push_back(track); });
    MediaSource md;
    md.path = "hits.wav";
    md.sampleRate = 48000;
    md.frames = 96000;
    const auto source = tx.create(md);
    Take t;
    t.source = source;
    t.stretchMarkers = {{0, 0, 0}, {0.6, 0.5, 0}, {2, 2, 0}};
    t.transients = Transients{"flux-v1", "identity", .7, {.1, .6}};
    const auto take = tx.create(t);
    Item item;
    item.track = track;
    item.length = 4;
    item.takes = {take};
    item.activeTake = take;
    tx.create(item);
    auto session = tx.commit();
    REQUIRE(session);
    const auto saved = serializeProject(*session);
    CHECK(saved.find("supple.project/2.5") != std::string::npos);
    CHECK(saved.find("source_pos") != std::string::npos);
    auto reopened = parseProject(saved);
    REQUIRE(reopened);
    CHECK(serializeProject(*reopened) == saved);
    CHECK(reopened->takes.find(take)->transients == t.transients);
    auto old = projectToJson(*session);
    old["schema"] = "supple.project/2.1";
    old["takes"][take.str()].asObject().erase("transients");
    REQUIRE(projectFromJson(old));
    t.warpPeers = {take};
    const Id fresh{Kind::Take, take.n + 10};
    const auto remapped = remapIds(toJson(t), schemaOf<Take>().jsonSchema(), {{take, fresh}});
    CHECK(remapped.find("warp_peers")->at(0).asString() == fresh.str());
}
