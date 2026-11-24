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
// pieces touch. At the core of the seam every take plays the same source on both sides, to a
// sample (a copy placed after the original does not, and is not the next piece).
// The core of a seam is the middle sample of the overlap.
// Errors: comp.takes (an item with fewer than two takes), comp.loop, comp.mismatch (`items`
// that do not continue one another, or two items that could continue the run; candidates that
// continue one another in turn are one chain, and the nearest is the next piece), each with the
// path of the argument (`/item`, `/items/k`).
Result<std::vector<CompPiece>> compRun(const Session&, std::optional<Id> item,
                                       std::span<const Id> items);

// Whether two continuing pieces (left, right: from compRun) are one piece cut in two: the items
// agree in everything but where they lie, their fades and their takes, and the takes agree in
// everything but the source offset (which compRun already found continuous), with the same FX
// and no envelopes, take markers or stretch markers. Audio takes name one file; MIDI takes
// hold the same notes (a split gives each half its own copy; a pooled source is one object).
// Joining them loses nothing: a note held across the boundary sounds once.
bool joinablePieces(const Session&, Id left, Id right);

// The crossfade a run was made with (take.comp's `crossfade` and `crossfade_shape`), read back
// from the stored pieces for take.comp.get and for a GUI that edits a comp: the longest
// seam-side fade of the run, and that fade's shape (a seam clamped by short pieces does not
// shrink it). Only seams between different takes count: a run with none (one piece, or an item
// split in two) gives the default; a run whose seams between takes are all hard cuts gives 0. A
// fade of a shape take.comp does not offer reads as cos.
inline constexpr double kDefaultCompCrossfade = 0.01;  // seconds
struct CompCrossfade {
    double seconds = kDefaultCompCrossfade;
    FadeShape shape = FadeShape::Cos;
};
CompCrossfade compCrossfade(const Session&, std::span<const CompPiece> run);
// The seams (0 = between the first two pieces) that `take.comp` called with compCrossfade's values
// (the longest seam fade and its shape, as the read-back reports them) and the run's spans would
// rewrite: a seam between two takes has, on both sides, the fade 2h samples long, h = min(that
// crossfade in samples / 2, half of each piece's core), in that shape; a seam inside one take has
// none. A hand-set length or shape, or a hard cut among crossfaded seams, is listed. Empty: that
// rebuild gives the run byte for byte.
std::vector<std::size_t> compDifferingSeams(const Session&, std::span<const CompPiece> run);

// Moves an item's start (end) to a time, keeping its end (start), in the item's own domain.
void setItemStartSeconds(const Session&, Item&, double startSec);
void setItemEndSeconds(const Session&, Item&, double endSec);
// Moves an item's end to another item's end, exactly as stored (no round trip through seconds).
void setItemEndToItemEnd(const Session&, Item&, const Item& other);

}  // namespace supple::model
