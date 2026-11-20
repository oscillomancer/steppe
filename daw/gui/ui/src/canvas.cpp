// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/canvas.h"

#include <nanovg.h>

#include <string>
#include <vector>

namespace supple::ui {
namespace {
NVGcolor nvg(Color c) { return nvgRGBAf(c.r, c.g, c.b, c.a); }
}  // namespace

int Canvas::face(Font f) const {
    switch (f) {
        case Font::Ui:
            return fonts_.ui;
        case Font::UiBold:
            return fonts_.uiBold;
        case Font::Mono:
            return fonts_.mono;
        case Font::MonoBold:
            return fonts_.monoBold;
    }
    return fonts_.ui;
}

void Canvas::fillRect(const Rect& r, Color c) {
    nvgBeginPath(vg_);
    nvgRect(vg_, r.x, r.y, r.w, r.h);
    nvgFillColor(vg_, nvg(c));
    nvgFill(vg_);
}

void Canvas::fillRound(const Rect& r, float radius, Color c) {
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, r.x, r.y, r.w, r.h, radius);
    nvgFillColor(vg_, nvg(c));
    nvgFill(vg_);
}

void Canvas::strokeRect(const Rect& r, Color c, float width) {
    nvgBeginPath(vg_);
    // Half-unit inset so a 1-unit stroke covers whole pixels at scale 1.
    nvgRect(vg_, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

void Canvas::strokeRound(const Rect& r, float radius, Color c, float width) {
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, r.x + width / 2, r.y + width / 2, r.w - width, r.h - width, radius);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

void Canvas::line(Point a, Point b, Color c, float width) {
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, a.x, a.y);
    nvgLineTo(vg_, b.x, b.y);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

void Canvas::fillCircle(Point centre, float radius, Color c) {
    nvgBeginPath(vg_);
    nvgCircle(vg_, centre.x, centre.y, radius);
    nvgFillColor(vg_, nvg(c));
    nvgFill(vg_);
}

void Canvas::strokeCircle(Point centre, float radius, Color c, float width) {
    nvgBeginPath(vg_);
    nvgCircle(vg_, centre.x, centre.y, radius - width / 2);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
}

void Canvas::fillPath(const std::vector<Point>& points, Color c) {
    if (points.size() < 3) return;
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, points[0].x, points[0].y);
    for (std::size_t i = 1; i < points.size(); ++i) nvgLineTo(vg_, points[i].x, points[i].y);
    nvgClosePath(vg_);
    nvgFillColor(vg_, nvg(c));
    nvgFill(vg_);
}

void Canvas::strokePath(const std::vector<Point>& points, Color c, float width, bool closed) {
    if (points.size() < 2) return;
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, points[0].x, points[0].y);
    for (std::size_t i = 1; i < points.size(); ++i) nvgLineTo(vg_, points[i].x, points[i].y);
    if (closed) nvgClosePath(vg_);
    nvgLineJoin(vg_, NVG_ROUND);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
    nvgLineJoin(vg_, NVG_MITER);  // the state is shared: later strokes keep the default
}

void Canvas::strokeArc(Point centre, float radius, float a0, float a1, Color c, float width) {
    nvgBeginPath(vg_);
    nvgArc(vg_, centre.x, centre.y, radius, a0, a1, NVG_CW);
    nvgLineCap(vg_, NVG_ROUND);
    nvgStrokeColor(vg_, nvg(c));
    nvgStrokeWidth(vg_, width);
    nvgStroke(vg_);
    nvgLineCap(vg_, NVG_BUTT);  // the default, for the strokes after this one
}

float Canvas::textWidth(std::string_view s, Font font, float size) const {
    nvgFontFaceId(vg_, face(font));
    nvgFontSize(vg_, size);
    return nvgTextBounds(vg_, 0, 0, s.data(), s.data() + s.size(), nullptr);
}

std::vector<std::string> Canvas::wrap(std::string_view s, Font font, float size,
                                      float width) const {
    std::vector<std::string> lines;
    if (s.empty()) return lines;
    nvgFontFaceId(vg_, face(font));
    nvgFontSize(vg_, size);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    const char* at = s.data();
    const char* end = s.data() + s.size();
    NVGtextRow rows[16];
    while (at < end) {
        const int n = nvgTextBreakLines(vg_, at, end, std::max(1.0f, width), rows, 16);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) lines.emplace_back(rows[i].start, rows[i].end);
        at = rows[n - 1].next;
    }
    return lines;
}

std::vector<std::pair<std::size_t, float>> Canvas::carets(std::string_view s, Font font, float size,
                                                          float x0) const {
    std::vector<std::pair<std::size_t, float>> out;
    out.emplace_back(0, x0);
    if (s.empty()) return out;
    nvgFontFaceId(vg_, face(font));
    nvgFontSize(vg_, size);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    std::vector<NVGglyphPosition> glyphs(s.size());
    const int n = nvgTextGlyphPositions(vg_, x0, 0, s.data(), s.data() + s.size(), glyphs.data(),
                                        static_cast<int>(glyphs.size()));
    for (int i = 0; i < n; ++i) {
        const auto end = static_cast<std::size_t>(
            (i + 1 < n ? glyphs[static_cast<std::size_t>(i) + 1].str : s.data() + s.size()) -
            s.data());
        out.emplace_back(end, glyphs[static_cast<std::size_t>(i)].maxx);
    }
    return out;
}

void Canvas::text(const Rect& box, std::string_view s, Font font, float size, Color c, int align) {
    nvgFontFaceId(vg_, face(font));
    nvgFontSize(vg_, size);
    nvgFillColor(vg_, nvg(c));
    int a = 0;
    a |= (align & kCenter) ? NVG_ALIGN_CENTER : (align & kRight) ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT;
    a |= (align & kTop)        ? NVG_ALIGN_TOP
         : (align & kBottom)   ? NVG_ALIGN_BOTTOM
         : (align & kBaseline) ? NVG_ALIGN_BASELINE
                               : NVG_ALIGN_MIDDLE;
    nvgTextAlign(vg_, a);
    const float x = (align & kCenter) ? box.x + box.w / 2 : (align & kRight) ? box.right() : box.x;
    const float y = (align & kTop) ? box.y : (align & kBottom) ? box.bottom() : box.y + box.h / 2;
    std::string shown(s);
    if (textWidth(shown, font, size) > box.w && box.w > 0) {
        // Shorten to fit, ending in an ellipsis (U+2026).
        const std::string ellipsis = "\xE2\x80\xA6";
        while (!shown.empty() && textWidth(shown + ellipsis, font, size) > box.w) {
            std::size_t end = shown.size() - 1;
            while (end > 0 && (static_cast<unsigned char>(shown[end]) & 0xC0) == 0x80) --end;
            shown.resize(end);  // remove the whole UTF-8 character
        }
        shown += ellipsis;
    }
    nvgText(vg_, x, y, shown.c_str(), nullptr);
}

std::string Canvas::elide(std::string_view s, Font font, float size, float width) const {
    std::string shown(s);
    if (textWidth(shown, font, size) <= width) return shown;
    const std::string ellipsis = "\xE2\x80\xA6";
    while (!shown.empty() && textWidth(shown + ellipsis, font, size) > width) {
        std::size_t end = shown.size() - 1;
        while (end > 0 && (static_cast<unsigned char>(shown[end]) & 0xC0) == 0x80) --end;
        shown.resize(end);  // remove the whole UTF-8 character
    }
    return shown.empty() ? std::string() : shown + ellipsis;
}

void Canvas::save() { nvgSave(vg_); }
void Canvas::restore() { nvgRestore(vg_); }
void Canvas::clip(const Rect& r) { nvgIntersectScissor(vg_, r.x, r.y, r.w, r.h); }

}  // namespace supple::ui
