// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Scrolling: a scroll bar, and a virtualized list (only visible rows are
// painted; any number of rows) with selection and keyboard navigation.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "supple/ui/node.h"

namespace supple::ui {

class ScrollBar : public Node {
  public:
    enum class Axis { Vertical, Horizontal };
    ScrollBar(std::string id, Axis axis);
    // Content and visible lengths in design units; the value is the offset.
    void setRange(float content, float shown);
    void setValue(float value);
    float value() const { return value_; }
    float maxValue() const { return std::max(0.0f, content_ - visible_); }
    bool needed() const { return content_ > visible_ + 0.5f; }
    std::function<void(float)> onScroll;  // user drags and track clicks

    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    static constexpr float kThickness = 10;

  private:
    Rect thumb() const;
    Axis axis_;
    float content_ = 0, visible_ = 0, value_ = 0;
    std::optional<float> grab_;  // pointer offset into the thumb while dragging
};

// A drag out of a control, as its owner hears it: Begin once the pointer has
// moved past the slop, Move after it, End on release, Cancel when the gesture
// was taken away.
enum class DragPhase { Begin, Move, End, Cancel };

class List : public Node {
  public:
    explicit List(std::string id);
    float rowHeight = 28;
    std::function<void(Canvas&, std::size_t row, const Rect& r, bool selected)> paintRow;
    std::function<void(std::size_t)> onSelect;    // selection moved (click, keys)
    std::function<void(std::size_t)> onActivate;  // Enter, double click
    // A right press on a row: it is selected first; `at` is the press point.
    std::function<void(std::size_t, Point at)> onContext;
    // A left press on a row that then moves more than kDragSlop units drags
    // that row; the owner draws the feedback and takes the drop. Unset, a
    // press only selects.
    std::function<void(std::size_t row, DragPhase phase, Point at)> onDrag;
    static constexpr float kDragSlop = 4;

    void setCount(std::size_t count);
    std::size_t count() const { return count_; }
    void select(std::optional<std::size_t> row, bool notify = false);
    std::optional<std::size_t> selected() const { return selected_; }
    void scrollTo(std::size_t row);  // minimal scroll that shows it
    float scroll() const { return scroll_; }
    std::size_t rowsPainted() const { return painted_; }

    void arrange(const Canvas& canvas, const Rect& bounds) override;
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;

  private:
    void setScroll(float s);
    std::optional<std::size_t> rowAt(Point p) const;
    std::size_t count_ = 0;
    std::optional<std::size_t> selected_;
    float scroll_ = 0;
    std::size_t painted_ = 0;
    ScrollBar* bar_ = nullptr;
    struct Press {
        std::size_t row;
        Point at;
        bool dragging = false;
    };
    std::optional<Press> press_;  // a left press on a row, until its release
};

}  // namespace supple::ui
