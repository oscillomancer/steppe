// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include "supple/model/freeze.h"
#include "supple/model/schemas.h"
using namespace supple;
using namespace supple::model;

namespace {
Session frozenSession() {
    EditTxn edit(Session::create(), 1);
    Track track;
    track.parent = kMaster;
    const auto id = edit.create(track);
    edit.modify<Track>(kMaster, [&](auto& master) { master.children.push_back(id); });
    const auto media = edit.create(
        MediaSource{"freeze.wav", "sha256:" + std::string(64, '1'), "wav", 48000, 2, 96000});
    auto before = edit.commit();
    REQUIRE(before);
    EditTxn freeze(*before, 2);
    TrackFreeze cache{media, 48000, 48000, 37, freezeDependencyHash(*before, id)};
    freeze.modify<Track>(id, [&](auto& t) { t.freeze = cache; });
    auto s = freeze.commit();
    REQUIRE(s);
    return *s;
}
}  // namespace
TEST_CASE("freeze model: canonical round trip and older files") {
    auto s = frozenSession();
    auto text = serializeProject(s);
    CHECK(text.find("supple.project/2.8") != std::string::npos);
    auto loaded = parseProject(text);
    REQUIRE(loaded);
    CHECK(serializeProject(*loaded) == text);
    auto old = projectToJson(Session::create());
    old["schema"] = "supple.project/2.1";
    std::vector<schema::Warning> warnings;
    auto migrated = projectFromJson(old, &warnings);
    REQUIRE(migrated);
    CHECK(warnings.empty());
    CHECK_FALSE(migrated->tracks.find(kMaster)->freeze);
    auto invalid = projectToJson(s);
    invalid["tracks"]["tr_1"]["freeze"]["content_frames"] = -1;
    CHECK_FALSE(projectFromJson(invalid));
    invalid = projectToJson(s);
    invalid["tracks"]["tr_1"]["freeze"]["source"] = "md_99";
    CHECK_FALSE(projectFromJson(invalid));
}
TEST_CASE("freeze model: mix controls stay live and timing and sound stay locked") {
    auto s = frozenSession();
    const Id track{Kind::Track, 1};
    const auto hash = freezeDependencyHash(s, track);
    EditTxn mix(s, 3);
    mix.modify<Track>(track, [](auto& t) {
        t.volumeDb = -12;
        t.pan = 40;
        t.name = "New name";
    });
    auto changed = mix.commit();
    REQUIRE(changed);
    CHECK(freezeDependencyHash(*changed, track) == hash);
    EditTxn sound(s, 3);
    sound.modify<Track>(track, [](auto& t) { t.mono = true; });
    auto denied = sound.commit();
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == "track.frozen");
    EditTxn tempo(s, 3);
    tempo.modify<TempoPointObj>(Id{Kind::TempoPoint, 1}, [](auto& t) { t.bpm = 80; });
    CHECK_FALSE(tempo.commit());
    EditTxn unfreeze(s, 3);
    unfreeze.modify<Track>(track, [](auto& t) { t.freeze.reset(); });
    // A batch may unfreeze the final frozen track before changing global timing.
    unfreeze.modify<TempoPointObj>(Id{Kind::TempoPoint, 1}, [](auto& t) { t.bpm = 80; });
    auto unfrozen = unfreeze.commit();
    REQUIRE(unfrozen);
    CHECK(unfrozen->timeMap->bpmAt(0) == 80);
}
