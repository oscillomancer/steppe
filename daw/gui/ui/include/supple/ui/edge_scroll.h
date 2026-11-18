// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Edge scroll (gui.md §2): what the views that scroll under a drag held at
// their edge share, so that they all move at one speed and by one clock.
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace supple::ui {

// How near an edge a held drag starts to scroll (design units).
inline constexpr float kEdgeScrollZone = 24;

// The scroll's speed along one axis (design units per second, negative
// towards `from`) for a pointer at `at` in a view that spans from..to: none
// in the middle, slow enough at the zone's start to place something just out
// of view, fast enough further out and past the edge to cross a long project.
inline float edgeScrollSpeed(float at, float from, float to) {
    auto speed = [](float depth) {
        return depth <= 0 ? 0.0f : std::min(2400.0f, 120.0f + 30.0f * depth);
    };
    return at > to - kEdgeScrollZone     ? speed(at - (to - kEdgeScrollZone))
           : at < from + kEdgeScrollZone ? -speed(from + kEdgeScrollZone - at)
                                         : 0.0f;
}

// The time one pass of the loop advances an edge scroll by: the time since
// the pass before, but never more than kMaxStep, so that a stalled frame is
// a short step and not a jump. The first pass of a scroll advances nothing.
class EdgeScrollClock {
  public:
    static constexpr double kMaxStep = 0.05;  // seconds
    double step(std::chrono::steady_clock::time_point now) {
        const double seconds =
            at_ ? std::clamp(std::chrono::duration<double>(now - *at_).count(), 0.0, kMaxStep)
                : 0.0;
        at_ = now;
        return seconds;
    }
    void reset() { at_.reset(); }  // the scroll ended

  private:
    std::optional<std::chrono::steady_clock::time_point> at_;
};

}  // namespace supple::ui
