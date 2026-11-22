// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// A launched clip follows live (ADR-0098 X4): pitch tables built at compile from
// harmony::follow, one row per pair of distinct places (a key and a chord), the
// clip's and the project's, read as `model::soundingKey` reads them (the model's
// place readers: a clip's first key and chord hold before its first points), and
// looked up on the audio path by a note's clip beat and the song beat where it
// starts. The rows live in one pool per plan, deduplicated by content across clips
// (engine.md "Following at playback", X4c).
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "supple/base/rt.h"
#include "supple/harmony/harmony.h"
#include "supple/model/session.h"

namespace supple::engine::detail {

// Where the places change, by beat: region i (0 before the first change) holds
// place[i]; `place.size() == beats.size() + 1`.
struct FollowRegions {
    std::vector<double> beats;
    std::vector<std::uint16_t> place;
    std::uint16_t at(double beat) const SUPPLE_RT;  // the last change at or before it
};

// The project's side, for one session: its regions and distinct places, with
// chords (`full`) and by key alone (`keys`); and the plan's pool of pitch rows
// (128 keys each) that every following clip's tables index, with the index that
// deduplicates them by content (compile only).
struct ProjectFollow {
    FollowRegions full, keys;
    std::vector<harmony::Place> fullPlaces, keyPlaces;
    std::shared_ptr<std::vector<std::uint8_t>> pool = std::make_shared<std::vector<std::uint8_t>>();
    std::map<std::array<std::uint8_t, 128>, std::uint32_t> rowIndex;
};
ProjectFollow projectFollow(const model::Session& s);

// How a clip plays when launched: as written, following the key, or the chords.
enum class FollowOutcome : std::uint8_t { None, Key, Chords };

// A following clip's tables. `keyOnly`: they index the project's `keys` regions.
// `pool` is the plan's (a retiring player carried into a later plan keeps the
// rows it was built with).
struct ClipFollowTables {
    bool keyOnly = true;
    FollowRegions clip;  // clip beats
    std::uint32_t projectPlaces = 0;
    std::vector<std::uint32_t> rows;  // [from * projectPlaces + to]: a row of `pool`
    std::shared_ptr<const std::vector<std::uint8_t>> pool;
};

// Pairs of places one clip's tables may hold, counted before the pool's
// deduplication, so a clip's outcome never depends on what else is launched:
// past it, a `chords` clip falls back to its key, and past that it plays as
// written.
inline constexpr std::size_t kFollowPairs = 1024;

struct ClipFollowBuild {
    FollowOutcome outcome = FollowOutcome::None;
    std::optional<ClipFollowTables> tables;
    bool overBudget = false;  // the outcome is a fallback the budget forced
};
// The tables for `clip` (a launched MIDI clip whose notes `pitched` says hold one
// off channel 10): none when its follow is none, it has no readable key at 0, or
// no pitched note, as placement copies those.
ClipFollowBuild buildClipFollow(const model::Clip& clip, bool pitched, ProjectFollow& project);
// Whether launched MIDI clip `clip` plays a note off channel 10 among the notes its
// player compiles (within its length and loop, not its whole source): the `pitched` both the
// compile and launch.commit give buildClipFollow (review round 86 H8).
bool clipPlaysPitched(const model::Session& s, model::Id clip);

// The key a note of a following clip plays: written `key` at clip beat `clipBeat`,
// starting at song beat `songBeat`. Channel 10 (stored 9) is never mapped.
int followedKey(const ClipFollowTables& t, const ProjectFollow& project, double clipBeat,
                double songBeat, int channel, int key) SUPPLE_RT;

}  // namespace supple::engine::detail
