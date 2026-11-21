// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// An item's body: its take's waveform or notes drawn into a rect. The
// drawing knows the take and where its content lands, not the item, so the
// arrangement's items and the launcher's clip cells share it (gui.md §4.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "supple/model/follow.h"
#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/gl.h"

namespace supple::views {

// Where an audio body's content lands across its rect. Content time is in
// seconds from the body's start, as the timeline runs (before the take's
// rate and warp, which the drawing applies).
struct AudioWindow {
    std::function<double(float x)> secondsAt;  // the content second at an x
    double unitsPerSecond = 40;  // design units per content second (picks the peaks' level)
    bool loop = false;           // the take repeats from its offset; else its warp map places it
};

// Where a MIDI body's content lands across its rect. Content time is in
// beats from the body's start.
struct MidiWindow {
    std::function<float(double beat)> xAt;  // where a content beat lands
    double length = 0;                      // the beats the body spans
    double from = 0, to = 0;                // the beats of it in view
    float end = 0;                          // the body's right edge: notes are cut there
    bool loop = false;                      // the take repeats over the length
};

// The waveform, one column per device pixel across the part of `r` inside the
// frame's clip, a lane per channel, scaled by `gain`. Returns the columns drawn.
std::size_t drawAudioBody(ui::StripRenderer& strips, const ui::GlFrame& frame, const ui::Rect& r,
                          const peaks::Pyramid& pyramid, const model::Take& take,
                          const AudioWindow& window, float gain, std::uint32_t rgba);

// The notes, their pitch range fitted to `r` (a free row above and below,
// at most 8 units a row), every loop pass that is in view; at most 8192
// glyphs. Returns the notes drawn.
std::size_t drawMidiBody(ui::RectRenderer& notes, const ui::Rect& r, const model::Take& take,
                         const model::MidiSource& source, const MidiWindow& window,
                         std::uint32_t rgba, std::uint32_t border);

// A following take's notes where they sound (ADR-0098 amended, gui.md §4.3):
// each heard occurrence (model::soundingNotes) at its song beat less
// `itemBeat`, at the pitch it sounds, the range fitted as drawMidiBody fits
// it; at most 8192 glyphs. Returns the notes drawn.
std::size_t drawSoundingBody(ui::RectRenderer& notes, const ui::Rect& r,
                             std::span<const model::SoundingNote> heard, const model::Take& take,
                             const model::MidiSource& source, const MidiWindow& window,
                             double itemBeat, std::uint32_t rgba, std::uint32_t border);

}  // namespace supple::views
