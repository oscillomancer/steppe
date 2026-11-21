// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The context's lead sheet (ADR-0095): the key and chord points as text in the
// notation's vocabulary, one line per project bar.
//
//   1: key Eb major {Ebmaj7}:w
//   2: {Cm7}:h {Fm7}:h
//   3: ~:w
//   4: {Bb7}:h r:h
//
// `{SYMBOL}:DUR` is a chord point at the cursor, `r:DUR` a chord point with no chord,
// `~:DUR` holds what holds (no point), `key TONIC SCALE` is a key point at the cursor
// and takes no time. A bar's durations fill the bar.
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/objects.h"
#include "supple/model/session.h"

namespace supple::engine::detail {

// The text for bars `firstBar` to `lastBar` (inclusive, 1-based). Canonical: the same
// points print the same bytes, and parsing the text gives the same points.
std::string printLeadSheet(const model::Session& s, int firstBar, int lastBar);

// The same for points that are not the project's (a clip's, in clip beats, ADR-0098):
// bar numbers run in `map`'s meters from bar 1 at beat 0.
std::string printLeadSheetPoints(const std::vector<model::ContextPoint>& points,
                                 const time::TimeMap& map, int firstBar, int lastBar);

struct LeadPoint {
    double beat = 0;  // project beats, on the 1/960 grid
    model::ContextType type = model::ContextType::Chord;
    std::string tonic, scale;  // a key point: canonical names
    std::string symbol;        // a chord point: canonical; empty is "no chord from here"
};

struct LeadSheet {
    int firstBar = 1, lastBar = 1;
    double fromBeat = 0, toBeat = 0;  // the range the text covers: [from, to)
    std::vector<LeadPoint> points;    // by beat, a key before a chord at one beat
};

// Reads the text against the session's meters. Errors name the bar and the token
// (`context.syntax`, `context.bar_length`, `context.bar_order`, `context.empty`,
// `context.too_long`, and the chord and key errors the commands give).
Result<LeadSheet> parseLeadSheet(const model::Session& s, std::string_view text);
// The same against a time map alone (a take's bars, ADR-0098 amendment: the meter at its item).
Result<LeadSheet> parseLeadSheet(const time::TimeMap& map, std::string_view text);

inline constexpr int kLeadSheetMaxBars = 4096;
inline constexpr std::size_t kLeadSheetMaxBytes = 1u << 20;

}  // namespace supple::engine::detail
