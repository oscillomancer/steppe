// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// A launched clip follows live (ADR-0098 X4): pitch tables built at compile from
// harmony::follow, one per pair of distinct places (a key and a chord), the clip's
// and the project's, looked up on the audio path by a note's clip beat and the song
// beat where it starts. The commit (launch.commit) maps what it lays with the same
// tables, so what is laid is what was heard.
#include <cstdint>
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
// chords (`full`) and by key alone (`keys`).
struct ProjectFollow {
    FollowRegions full, keys;
    std::vector<harmony::Place> fullPlaces, keyPlaces;
};
ProjectFollow projectFollow(const model::Session& s);

// How a clip plays when launched: as written, following the key, or the chords.
enum class FollowOutcome : std::uint8_t { None, Key, Chords };

// A following clip's tables. `keyOnly`: they index the project's `keys` regions.
struct ClipFollowTables {
    bool keyOnly = true;
    FollowRegions clip;  // clip beats
    std::uint32_t projectPlaces = 0;
    std::vector<std::uint8_t> pitch;  // [(from * projectPlaces + to) * 128 + key]
};

// Pairs of places one clip's tables may hold (128 bytes each): past it, a
// `chords` clip falls back to its key, and past that it plays as written.
inline constexpr std::size_t kFollowPairs = 1024;

struct ClipFollowBuild {
    FollowOutcome outcome = FollowOutcome::None;
    std::optional<ClipFollowTables> tables;
    bool overBudget = false;  // the outcome is a fallback the budget forced
};
// The tables for `clip` (a launched MIDI clip whose notes `pitched` says hold one
// off channel 10): none when its follow is none, it has no readable key at 0, or
// no pitched note, as placement copies those.
ClipFollowBuild buildClipFollow(const model::Clip& clip, bool pitched,
                                const ProjectFollow& project);

// The key a note of a following clip plays: written `key` at clip beat `clipBeat`,
// starting at song beat `songBeat`. Channel 10 (stored 9) is never mapped.
int followedKey(const ClipFollowTables& t, const ProjectFollow& project, double clipBeat,
                double songBeat, int channel, int key) SUPPLE_RT;

}  // namespace supple::engine::detail
