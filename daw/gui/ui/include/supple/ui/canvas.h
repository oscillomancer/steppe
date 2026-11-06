// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Drawing for chrome (widgets, text, outlines) over NanoVG, in design units
// and window coordinates. Bulk data (waveforms, notes) uses the GL layers of
// P5.3, never this (ADR-0005 §3).
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "supple/gui/window.h"
#include "supple/ui/geometry.h"
#include "supple/ui/theme.h"

struct NVGcontext;

namespace supple::ui {

enum class Font { Ui, UiBold, Mono, MonoBold };

enum Align : int {
    kLeft = 1 << 0,
    kCenter = 1 << 1,
    kRight = 1 << 2,
    kTop = 1 << 3,
    kMiddle = 1 << 4,
    kBottom = 1 << 5,
    kBaseline = 1 << 6,
};

class Canvas {
  public:
    Canvas(NVGcontext* vg, const gui::Fonts& fonts, const Theme& theme)
        : vg_(vg), fonts_(fonts), theme_(theme) {}
    NVGcontext* vg() const { return vg_; }
    const Theme& theme() const { return theme_; }
    const gui::Fonts& fonts() const { return fonts_; }  // for code that draws with NanoVG itself

    void fillRect(const Rect& r, Color c);
    void fillRound(const Rect& r, float radius, Color c);
    void strokeRect(const Rect& r, Color c, float width = 1.0f);
    void strokeRound(const Rect& r, float radius, Color c, float width = 1.0f);
    void line(Point a, Point b, Color c, float width = 1.0f);
    void fillCircle(Point centre, float radius, Color c);
    void strokeCircle(Point centre, float radius, Color c, float width = 1.0f);
    // A polygon through `points` (icons): filled, or outlined (closed or open).
    void fillPath(const std::vector<Point>& points, Color c);
    void strokePath(const std::vector<Point>& points, Color c, float width = 1.0f,
                    bool closed = true);
    // An arc around `centre` from angle a0 to a1 (radians, clockwise from +x).
    void strokeArc(Point centre, float radius, float a0, float a1, Color c, float width = 1.0f);
    // Text inside `box`, aligned with `align`; clipped to the box when it
    // does not fit (an ellipsis replaces the cut).
    void text(const Rect& box, std::string_view s, Font font, float size, Color c,
              int align = kLeft | kMiddle);
    float textWidth(std::string_view s, Font font, float size) const;
    // `s` broken into lines no wider than `width` (at spaces where possible;
    // explicit newlines break).
    std::vector<std::string> wrap(std::string_view s, Font font, float size, float width) const;
    // The x of every caret position (each UTF-8 boundary, 0…size) of `s`
    // drawn left-aligned at `x0`: front.size() == number of boundaries.
    std::vector<std::pair<std::size_t, float>> carets(std::string_view s, Font font, float size,
                                                      float x0) const;

    // Scissor stack: clip() intersects with the current clip.
    void save();
    void restore();
    void clip(const Rect& r);

  private:
    int face(Font f) const;
    NVGcontext* vg_;
    gui::Fonts fonts_;
    const Theme& theme_;
};

}  // namespace supple::ui
