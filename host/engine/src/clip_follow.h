// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Following at placement (ADR-0098 "Following (X2)", amended "following is playback"): a clip
// laid on the timeline as an item carries its context and `follow` onto its takes, and its
// notes sound as the key (or chords) of the place they land says. `followPlacedItem` runs after
// the item and its copied takes are in the transaction.
#include <optional>
#include <string>
#include <vector>

#include "clip_context.h"
#include "supple/base/result.h"
#include "supple/model/session.h"
#include "supple/schema/schema.h"

namespace supple::engine::detail {

// A run of project bars over which the clip's key, the place's key and the way notes were
// mapped stay the same (`to_bar` is inclusive).
struct FollowRegion {
    std::int64_t fromBar = 0, toBar = 0;
    std::optional<std::string> fromKey, toKey;
    std::string by;                        // transpose | degree | chords | copy
    std::optional<std::int64_t> interval;  // semitones, for transpose
};

// The answer's `followed` block for one laid item.
struct FollowedInfo {
    std::string mode = "none";  // none | key | chords: what was done
    std::optional<std::vector<FollowRegion>> regions;
    std::optional<std::int64_t> folded;  // notes that were folded into range
    std::optional<std::string> copied;  // why a clip that could not follow was copied
};

schema::Schema<FollowedInfo> followedSchema();

// The `follow` argument as the commands take it.
schema::Schema<model::ClipFollow> followArgSchema();

// The `followed` block for `itemId`'s active take as `now` holds it: its mode, the regions of
// project bars over which the context's key, the project's key and the way notes sound agree,
// and the folded count, all read from `model::soundingNotes`, with the `context.no_key` and
// `context.scale_shape` notes in `notes`. A take that does not follow is `mode: "none"`.
// Placement, `take.bake` (read before it writes) and `midi.get` answer through it.
FollowedInfo followedBlock(const model::Session& now, model::Id itemId, const std::string& path,
                           std::vector<ClipNote>& notes);

// Lays the clip's context on every MIDI take of `itemId` (just laid from `clip`, its takes
// copied): each take gets the clip's context in its own beats, its period and phase, and the
// `follow` that `requested` (else the clip's own) says, or none when the clip cannot follow
// (ADR-0098 "following is playback": the notes stay as written and are mapped when they
// sound). Returns the answer's block, read from `model::soundingNotes`: how the notes sound
// as, by bar. A clip that cannot follow, or one placed as `none`, is left as it is, with the
// `context.mismatch` note when its key is not the project's under the item. `s` is the
// session the command started from. Warnings go to `notes` (`context.no_key`,
// `context.scale_shape`, `context.mismatch`). Placement never refuses: a pooled item may
// follow, since the mapping belongs to each take. `path` is where warnings point.
Result<FollowedInfo> followPlacedItem(const model::Session& s, model::EditTxn& t, model::Id clipId,
                                      const model::Clip& clip, model::Id itemId,
                                      std::optional<model::ClipFollow> requested,
                                      const std::string& path, std::vector<ClipNote>& notes);

}  // namespace supple::engine::detail
