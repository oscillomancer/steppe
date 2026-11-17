// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>

namespace supple::views {

// Shared by TCP drawing, resize hit testing and the layout's height clamp.
struct TcpRowGeometry {
    static constexpr float kNameTop = 2, kNameHeight = 18;
    static constexpr float kFxTop = 3, kFxHeight = 15;
    static constexpr float kBadgeTop = 4, kBadgeSize = 16;
    static constexpr float kResizeGrip = 4;
    static constexpr float kMeterWidth = 8;  // the row's meter, at its right edge
    static constexpr float kTopLineBottom =
        std::max({kNameTop + kNameHeight, kFxTop + kFxHeight, kBadgeTop + kBadgeSize});
    static constexpr float kMinHeight = kTopLineBottom + kResizeGrip;
};

}  // namespace supple::views
