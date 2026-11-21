// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Following at placement (ADR-0098 "Following (X2)", session-model "Clip context"): a clip
// laid on the timeline as an item has its notes mapped from the key (or chords) it was
// written against to the key (or chords) of the place it lands. `followPlacedItem` runs
// after the item and its copied takes are in the transaction.
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
    std::optional<bool> unrolled;
    std::optional<std::string> copied;    // why a clip that could not follow was copied
    std::optional<KeyMismatch> mismatch;  // with context.mismatch: the keys and the bar
};

schema::Schema<FollowedInfo> followedSchema();

// The `follow` argument as the commands take it.
schema::Schema<model::ClipFollow> followArgSchema();

// Maps the notes of `itemId` (just laid from `clip`, its takes copied) by the context, as
// `requested` (else the clip's own `follow`) says, and returns the answer's block. `s` is the
// session the command started from: the project's context is read from it. A clip that cannot
// follow, or one placed as `none`, is left as it is, with the `context.mismatch` note when its
// key is not the project's under the item. Warnings go to `notes` (`context.no_key`,
// `context.scale_shape`, `context.mismatch`). The one refusal is `clip.follow_pooled`: the
// item shares the clip's MIDI (`pooled`) and the mapping would move a note. `path` is where
// errors and warnings point.
Result<FollowedInfo> followPlacedItem(const model::Session& s, model::EditTxn& t, model::Id clipId,
                                      const model::Clip& clip, model::Id itemId,
                                      std::optional<model::ClipFollow> requested, bool pooled,
                                      const std::string& path, std::vector<ClipNote>& notes);

}  // namespace supple::engine::detail
