// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A comp as the arrange view sees it (session-model §1.5.1, gui.md "Takes"):
// the run of pieces model::compRun finds, the take each plays and the seams
// between pieces that play different takes. The swipe, a lane click and a
// seam drag build their take.comp calls from it; the lanes draw its seams.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/model/comp.h"
#include "supple/model/session.h"

namespace supple::views {

struct CompView {
    std::vector<model::CompPiece> pieces;  // in time order
    std::vector<std::size_t> index;        // the take index each piece plays
    // A seam between pieces `left` and `left + 1` that play different takes,
    // at its core (the overlap's middle sample), in seconds.
    struct Seam {
        std::size_t left = 0;
        double seconds = 0;
    };
    std::vector<Seam> seams;
};

// The comp `item` belongs to (a lone item is a run of one piece); nullopt for
// an item with fewer than two takes, one compsWithTakeComp turns away (looped:
// no seams are drawn or dragged there), or one compRun refuses.
std::optional<CompView> compOf(const model::Session& s, model::Id item);

// A seam of a track, for the lanes and the hit test.
struct CompSeam {
    model::Id left, right;
    double seconds = 0;
    // Its comp differs from its edit group's, as the engine reports it
    // (compDiffersFrom; the lanes fill it in when the app gives a query).
    bool differs = false;
};
// The seams of the comps among `items` (one track's, any order).
std::vector<CompSeam> compSeams(const model::Session& s, const std::vector<model::Id>& items);

// One span per stretch of one take, in beats, from the core seams: what
// take.comp.get's pieces say, joined where neighbours play the same take.
struct CompSpan {
    double from = 0, to = 0;
    std::size_t index = 0;
};
std::vector<CompSpan> compSpans(const model::Session& s, const CompView& comp);

// The crossfade a re-comp keeps, since take.comp gives every seam of a run
// one length and shape: nullopt for a run with no seam between different
// takes (a first comp takes take.comp's default), else what take.comp.get
// reads back (model::compCrossfade: the longest seam-side fade, which a
// clamped seam does not shrink, and its shape; 0 for a hard-cut comp).
std::optional<model::CompCrossfade> compCrossfade(const model::Session& s, const CompView& comp);

// Whether the GUI comps `item` with take.comp: any item but a looped one,
// which keeps the older split and take.activate, with hard cuts (take.comp
// refuses it, comp.loop). An item edited together with others (a track's
// edit group) is comped with every member of the group by take.comp.
bool compsWithTakeComp(const model::Session& s, model::Id item);
// Whether `item`'s track is edited together with others (an edit group).
bool compsGroup(const model::Session& s, model::Id item);

// The comp whose piece `item` the GUI switches with take.comp: one of two or
// more pieces, or a lone item of an edit group (take.activate would switch it
// alone; take.comp switches the group's members with it). nullopt: a lone
// item outside a group, or one compsWithTakeComp turns away, switches its take
// with take.activate.
std::optional<CompView> compPieceOf(const model::Session& s, model::Id item);

// take.comp.get with these arguments, run by the app through the command bus
// (nullopt when it is refused).
using CompQuery = std::function<std::optional<json::Value>(const json::Value& args)>;
// Whether the comp `item` belongs to differs from its edit group's, as the
// engine reports it: take.comp.get asked for the group's first track (in the
// order take.comp takes members, model::linkedTracks's) with a run there lists
// `item`'s track in differing_members (other spans, takes, crossfade or shape,
// or items that cannot follow). Returns that first track; nullopt when it does
// not differ, is that track, is in no group, or without a query.
std::optional<model::Id> compDiffersFrom(const model::Session& s, model::Id item,
                                         const CompQuery& query);
// The take.comp call that switches piece `item` of `comp` to take `index`:
// its core span with that take and the comp's crossfade, so the seams are
// rebuilt and it joins a neighbour that plays the same take. (take.activate
// would keep the seam fades: a piece switched to its neighbour's take would
// sum one source with itself through an equal-power crossfade.)
json::Value compPieceArgs(const model::Session& s, const CompView& comp, model::Id item,
                          std::size_t index);
// The take index piece `item` of `comp` plays.
std::size_t compPieceIndex(const CompView& comp, model::Id item);

// A take.comp refusal as the GUI shows it. One from a group track that cannot
// follow (its path is /args/members/<track>[/items/<item>]: comp.group_takes,
// comp.group_mismatch, item.locked) names the argument bypass_groups, or only
// unlocking, in its hint; a GUI user sends it by holding Alt, which the hint
// says. The comped track's own refusals are returned as they are.
Error compRefusal(Error error);

// The warnings of a command's result, their messages joined for the status
// line ("" when there are none).
std::string resultWarnings(const json::Value& result);

// take.comp's arguments: `item`, the spans, and the crossfade when given.
json::Value compArgs(model::Id item, const std::vector<CompSpan>& spans,
                     const std::optional<model::CompCrossfade>& crossfade);

}  // namespace supple::views
