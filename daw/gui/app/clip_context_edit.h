// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Editing a launcher clip's own key and chords from the piano roll (ADR-0098
// X3, gui.md §4.7). clip.set takes a clip's context as a lead sheet in clip
// bars, so a point edit prints the clip's points again as that text and the
// engine reads, checks and keeps them.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "supple/harmony/harmony.h"

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

// A clip's context runs in clip beats (the time it sounds, from its launch);
// the roll's axis is its take's content beats ((tick / ppq - startOffset) /
// playrate). A looped clip plays its loop window [from, from + window) of
// the content once per pass, so a note there sounds at clip beat pass *
// window + content - from. This is the engine's clipSoundingBeats
// (midi_context.cpp), which midi.edit reads a note's key and chord by; the
// roll reads them the same way so its preview, shading and outline agree.
struct ClipContextAxis {
    bool looping = false;
    double from = 0;    // the loop's start in content beats; 0 without a loop
    double window = 0;  // the loop's length; the clip's without a loop
    double length = 0;  // the clip's
    // The clip beats a note at `content` sounds at, one per pass; none when
    // it never sounds (outside the loop window).
    std::vector<double> soundingBeats(double content) const;
    // The clip beat whose key and chord a note at `content` reads, as
    // midi.edit: its first sounding beat, else (it never sounds) its content
    // beat kept inside the clip.
    double contextBeat(double content) const;
    // The clip beats the roll draws a context point in, first pass only:
    // [0, min(window, length)); a point there shows at content beat
    // clip + from.
    double shownEnd() const { return looping ? std::min(window, length) : length; }
};
ClipContextAxis clipContextAxis(const model::Clip& clip);

// The key a note reads at each content beat, as spans (content beat, key)
// from the first, which holds from before any note: the key at
// contextBeat(content) for every content beat.
std::vector<std::pair<double, std::optional<harmony::Key>>> clipKeySpans(const model::Clip& clip);

}  // namespace supple::gui
