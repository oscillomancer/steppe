// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A comp as the arrange view sees it (session-model §1.5.1, gui.md "Takes"):
// the run of pieces model::compRun finds, the take each plays and the seams
// between pieces that play different takes. The swipe, a lane click and a
// seam drag build their take.comp calls from it; the lanes draw its seams.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

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
// an item with fewer than two takes, a looped one, or one compRun refuses.
std::optional<CompView> compOf(const model::Session& s, model::Id item);

// A seam of a track, for the lanes and the hit test.
struct CompSeam {
    model::Id left, right;
    double seconds = 0;
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

// Whether the GUI comps `item` with take.comp. A looped item, or one edited
// together with others (a track group), keeps the older split and
// take.activate, with hard cuts: take.comp refuses both (comp.loop,
// comp.linked).
bool compsWithTakeComp(const model::Session& s, model::Id item);

// The piece `item` of a comp (two or more pieces) that the GUI comps with
// take.comp, or nullopt: a lone item, or one compsWithTakeComp turns away,
// switches its take with take.activate.
std::optional<CompView> compPieceOf(const model::Session& s, model::Id item);
// The take.comp call that switches piece `item` of `comp` to take `index`:
// its core span with that take and the comp's crossfade, so the seams are
// rebuilt and it joins a neighbour that plays the same take. (take.activate
// would keep the seam fades: a piece switched to its neighbour's take would
// sum one source with itself through an equal-power crossfade.)
json::Value compPieceArgs(const model::Session& s, const CompView& comp, model::Id item,
                          std::size_t index);
// The take index piece `item` of `comp` plays.
std::size_t compPieceIndex(const CompView& comp, model::Id item);

// take.comp's arguments: `item`, the spans, and the crossfade when given.
json::Value compArgs(model::Id item, const std::vector<CompSpan>& spans,
                     const std::optional<model::CompCrossfade>& crossfade);

}  // namespace supple::views
