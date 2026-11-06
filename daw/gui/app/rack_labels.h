// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The labels under a module face's knobs and jacks (gui.md §4.12): each in a
// cell that stops short of its neighbours' controls and labels, at its size
// when it fits, else a little smaller, else on two lines when the space
// under it is free, else cut with an ellipsis. Whatever a module's layout,
// automatic or its own panel(), no label runs into another or under a
// control.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/rack/module_type.h"
#include "supple/ui/geometry.h"

namespace supple::gui::racks {

inline constexpr float kLabelSize = 8.5f;   // a label's size when it fits
inline constexpr float kLabelMin = 7.25f;   // the smallest it shrinks to
inline constexpr float kLabelLine = 11;     // a line's height
inline constexpr float kLabelLine2 = 9.5f;  // the second line's, when wrapped

struct FaceLabel {
    std::size_t item = 0;            // in the layout's items
    std::string text;                // the whole label
    std::vector<std::string> lines;  // one, or two; an elided line ends in "…"
    float size = kLabelSize;
    ui::Rect cell;  // the room it has on its first line (a knob's value shows there)
    ui::Rect box;   // where its lines are, all of them
};

// A text's width at a size, in the face's units (the canvas's textWidth).
using MeasureText = std::function<float(std::string_view text, float size)>;

// What an item covers on the face, with the marks around it (a knob's
// steps, a jack's ring), in module units. An output's plate is under the
// labels, not counted.
ui::Rect footprint(const rack::PanelItem& item);

// Every knob's and jack's label on `face`, the layout of `type`.
std::vector<FaceLabel> faceLabels(const rack::ModuleType& type, const rack::PanelLayout& face,
                                  const MeasureText& measure);

}  // namespace supple::gui::racks
