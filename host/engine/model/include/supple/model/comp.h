// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A comp is adjacent pieces of one track that hold the same takes, each playing one of them
// (session-model §1.5.1). Nothing in the model marks one: the run is found from the items.
#pragma once
#include <optional>
#include <span>
#include <vector>

#include "supple/model/session.h"

namespace supple::model {

// One piece of a run, in project samples (the rate of the project).
struct CompPiece {
    Id item;
    std::int64_t start = 0, end = 0;          // as stored: a crossfade's extensions included
    std::int64_t coreStart = 0, coreEnd = 0;  // with every crossfade taken out
};

// The pieces of the comp that `item` belongs to, or exactly `items` (in any order), by start.
// Two items of a track continue each other when the right one starts after the left one starts
// and no later than it ends, ends after it, and the two are take-compatible (same lane and
// timebase, not looped, the same count of at least two takes, and at each index the same kind
// of source, name and, for audio, media). An overlap is allowed only between audio items; MIDI
// pieces touch. The core of a seam is the middle sample of the overlap.
// Errors: comp.takes (an item with fewer than two takes), comp.loop, comp.mismatch (`items`
// that do not continue one another, or two items that could continue the run), each with the
// path of the argument (`/item`, `/items/k`).
Result<std::vector<CompPiece>> compRun(const Session&, std::optional<Id> item,
                                       std::span<const Id> items);

// Moves an item's start (end) to a time, keeping its end (start), in the item's own domain.
void setItemStartSeconds(const Session&, Item&, double startSec);
void setItemEndSeconds(const Session&, Item&, double endSec);

}  // namespace supple::model
