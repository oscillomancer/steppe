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
// takes (a first comp takes take.comp's default), else the longest
// seam-side fade of those seams (a clamped seam does not shrink it; 0 for a
// hard-cut comp) and its shape, linear or else equal power.
// Read from the fades until take.comp.get reports the run's value.
struct CompCrossfade {
    double seconds = 0;
    bool linear = false;
};
std::optional<CompCrossfade> compCrossfade(const model::Session& s, const CompView& comp);

// take.comp's arguments: `item`, the spans, and the crossfade when given.
json::Value compArgs(model::Id item, const std::vector<CompSpan>& spans,
                     const std::optional<CompCrossfade>& crossfade);

}  // namespace supple::views
