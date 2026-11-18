// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>

namespace supple::views {

// Shared by TCP drawing, resize hit testing and the layout's height clamp.
struct TcpRowGeometry {
    static constexpr float kNameTop = 2, kNameHeight = 18;
    // A row's buttons are cells of one size on three lines (gui.md §4.2): the
    // name line with arm, monitor, mute and solo at its right; the mix line
    // (from kMixHeight); the connections line (from kFullHeight).
    static constexpr float kCellWidth = 18, kCellHeight = 14, kCellGap = 2;
    static constexpr float kStateTop = 4, kMixTop = 22, kLinkTop = 42;
    static constexpr float kMixHeight = 40, kFullHeight = 58;
    static constexpr float kLeft = 8;   // where a row's content starts, after the colour stripe
    static constexpr float kMark = 13;  // a type mark or a snowflake before the name
    static constexpr float kResizeGrip = 4;
    static constexpr float kMeterWidth = 8;  // the row's meter, at its right edge
    static constexpr float kTopLineBottom =
        std::max(kNameTop + kNameHeight, kStateTop + kCellHeight);
    static constexpr float kMinHeight = kTopLineBottom + kResizeGrip;
    // The lowest row that shows all three lines clear of the resize grip:
    // a new project's row height.
    static constexpr float kAllLinesHeight = kLinkTop + kCellHeight + kResizeGrip;
};

}  // namespace supple::views
