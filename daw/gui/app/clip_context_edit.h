// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Editing a launcher clip's own key and chords from the piano roll (ADR-0098
// X3, gui.md §4.7). clip.set takes a clip's context as a lead sheet in clip
// bars, so a point edit prints the clip's points again as that text and the
// engine reads, checks and keeps them.
#pragma once

#include <string>
#include <vector>

#include "supple/model/objects.h"
#include "supple/time/timeline.h"

namespace supple::gui {

// The lead sheet for a clip's `points` (clip beats): one line per bar from
// bar 1 at beat 0, in `map`'s meters, through the bar holding the clip's last
// beat (`lengthBeats`) or its last point, whichever is later. Each line is
// "N:" and, from each point's tick to the next cut, "key TONIC SCALE" before
// "{SYMBOL}:DUR", "r:DUR" (no chord from here) or "~:DUR" (a hold).
std::string clipLeadSheet(const std::vector<model::ContextPoint>& points, const time::TimeMap& map,
                          double lengthBeats);

// `points` with `point` in place of any of its type at its beat (on the
// 1/960 grid), or with that one removed when `remove`; sorted by beat, a key
// before a chord at one beat, as a clip keeps them.
std::vector<model::ContextPoint> withContextPoint(std::vector<model::ContextPoint> points,
                                                  const model::ContextPoint& point,
                                                  bool remove = false);

}  // namespace supple::gui
