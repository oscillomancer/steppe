// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/list.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace supple::ui {

// --- ScrollBar

ScrollBar::ScrollBar(std::string id, Axis axis) : Node(std::move(id), Role::Slider), axis_(axis) {
    interactive = true;
}

void ScrollBar::setRange(float content, float shown) {
    content = std::max(0.0f, content);
    shown = std::max(0.0f, shown);
    if (content == content_ && shown == visible_) return;
    content_ = content;
    visible_ = shown;
    value_ = std::clamp(value_, 0.0f, maxValue());
    invalidate();
}

void ScrollBar::setValue(float value) {
    const float v = std::clamp(value, 0.0f, maxValue());
    if (v == value_) return;
    value_ = v;
    invalidate();
}

Rect ScrollBar::thumb() const {
    const Rect b = bounds();
    const bool vertical = axis_ == Axis::Vertical;
    const float track = vertical ? b.h : b.w;
    if (!needed() || track <= 0) return {};
    const float length = std::max(24.0f, track * visible_ / content_);
    const float at = maxValue() > 0 ? (track - length) * value_ / maxValue() : 0;
    return vertical ? Rect{b.x + 2, b.y + at, b.w - 4, length}
                    : Rect{b.x + at, b.y + 2, length, b.h - 4};
}

void ScrollBar::paint(Canvas& c) {
    if (!needed()) return;
    const Theme& t = c.theme();
    const Rect th = thumb();
    c.fillRound(th, std::min(th.w, th.h) / 2,
                (hovered() || grab_) ? t.color("ink-quiet").withAlpha(0.8f)
                                     : t.color("ink-faint").withAlpha(0.6f));
}

bool ScrollBar::onPointer(const PointerEvent& e) {
    if (!needed()) return false;
    const bool vertical = axis_ == Axis::Vertical;
    const float along = vertical ? e.pos.y : e.pos.x;
    const Rect th = thumb();
    const Rect b = bounds();
    auto emit = [&](float v) {
        setValue(v);
        auto callback = onScroll;
        if (callback) callback(value_);
    };
    switch (e.type) {
        case PointerEvent::Type::Down: {
            if (e.button != 1) return false;
            const float start = vertical ? th.y : th.x, length = vertical ? th.h : th.w;
            if (along >= start && along < start + length) {
                grab_ = along - start;
            } else {  // page toward the click
                emit(value_ + (along < start ? -visible_ : visible_));
            }
            return true;
        }
        case PointerEvent::Type::Move:
            if (grab_) {
                const float track = vertical ? b.h : b.w, length = vertical ? th.h : th.w;
                const float origin = vertical ? b.y : b.x;
                const float travel = std::max(1.0f, track - length);
                emit((along - *grab_ - origin) / travel * maxValue());
                return true;
            }
            return grab_.has_value();
        case PointerEvent::Type::Cancel:
        case PointerEvent::Type::Up:
            grab_.reset();
            invalidate();
            return true;
        default:
            return false;
    }
}

// --- List

List::List(std::string id) : Node(std::move(id), Role::List) {
    interactive = true;
    focusable = true;
    bar_ = &emplace<ScrollBar>(this->id() + ".scroll", ScrollBar::Axis::Vertical);
    bar_->onScroll = [this](float v) { setScroll(v); };
}

void List::setCount(std::size_t count) {
    count_ = count;
    if (selected_ && *selected_ >= count_)
        selected_ = count_ ? std::optional(count_ - 1) : std::nullopt;
    bar_->setRange(rowHeight * static_cast<float>(count_), bounds().h);
    setScroll(scroll_);
    invalidate();
}

void List::select(std::optional<std::size_t> row, bool notify) {
    if (row && *row >= count_) row = count_ ? std::optional(count_ - 1) : std::nullopt;
    selected_ = row;
    if (row) scrollTo(*row);
    invalidate();
    auto callback = onSelect;
    if (notify && row && callback) callback(*row);
}

void List::scrollTo(std::size_t row) {
    const float top = rowHeight * static_cast<float>(row);
    if (top < scroll_)
        setScroll(top);
    else if (top + rowHeight > scroll_ + bounds().h)
        // A view shorter than a row (or not laid out yet) shows the row's top.
        setScroll(std::min(top, top + rowHeight - bounds().h));
}

void List::setScroll(float s) {
    const float max = std::max(0.0f, rowHeight * static_cast<float>(count_) - bounds().h);
    const float v = std::clamp(s, 0.0f, max);
    if (v != scroll_) invalidate();
    scroll_ = v;
    bar_->setValue(v);
}

void List::arrange(const Canvas& canvas, const Rect& r) {
    setBounds(r);
    bar_->arrange(canvas, {r.right() - ScrollBar::kThickness, r.y, ScrollBar::kThickness, r.h});
    bar_->setRange(rowHeight * static_cast<float>(count_), r.h);
    setScroll(scroll_);
}

Size List::measure(const Canvas&, Size available) {
    return {available.w, rowHeight * static_cast<float>(count_)};
}

void List::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    painted_ = 0;
    if (count_ == 0 || rowHeight <= 0) return;
    c.save();
    c.clip(b);
    const auto first = static_cast<std::size_t>(std::floor(scroll_ / rowHeight));
    const float width = b.w - (bar_->needed() ? ScrollBar::kThickness : 0);
    for (std::size_t i = first; i < count_; ++i) {
        const Rect row{b.x, b.y + static_cast<float>(i) * rowHeight - scroll_, width, rowHeight};
        if (row.y >= b.bottom()) break;
        const bool sel = selected_ == i;
        if (sel)
            c.fillRect(row,
                       focused() ? t.color("selection") : t.color("selection").withAlpha(0.6f));
        if (paintRow) paintRow(c, i, row, sel);
        ++painted_;
    }
    c.restore();
}

std::optional<std::size_t> List::rowAt(Point p) const {
    if (!bounds().contains(p) || rowHeight <= 0) return std::nullopt;
    const auto i = static_cast<std::size_t>(std::floor((p.y - bounds().y + scroll_) / rowHeight));
    return i < count_ ? std::optional(i) : std::nullopt;
}

bool List::onPointer(const PointerEvent& e) {
    switch (e.type) {
        case PointerEvent::Type::Down: {
            if (e.button == 3 && onContext) {
                const auto row = rowAt(e.pos);
                if (!row) return true;
                auto context = onContext;
                selectMods_ = e.mods;
                select(*row, true);
                selectMods_ = 0;
                context(*row, e.pos);
                return true;
            }
            if (e.button != 1) return false;
            const auto row = rowAt(e.pos);
            if (!row) return true;
            auto activate = onActivate;
            selectMods_ = e.mods;
            select(*row, true);
            selectMods_ = 0;
            press_ = e.clicks >= 2 ? std::nullopt : std::optional(Press{*row, e.pos});
            if (e.clicks >= 2 && activate) activate(*row);
            return true;
        }
        case PointerEvent::Type::Move: {
            if (!press_ || !onDrag) return false;
            auto drag = onDrag;  // the owner may rebuild the list
            const std::size_t row = press_->row;
            if (!press_->dragging) {
                if (std::abs(e.pos.x - press_->at.x) <= kDragSlop &&
                    std::abs(e.pos.y - press_->at.y) <= kDragSlop)
                    return true;
                press_->dragging = true;
                drag(row, DragPhase::Begin, e.pos);
                return true;
            }
            drag(row, DragPhase::Move, e.pos);
            return true;
        }
        case PointerEvent::Type::Up:
        case PointerEvent::Type::Cancel: {
            if (!press_) return false;
            const Press press = *std::exchange(press_, std::nullopt);
            if (press.dragging && onDrag) {
                auto drag = onDrag;
                drag(press.row,
                     e.type == PointerEvent::Type::Up ? DragPhase::End : DragPhase::Cancel,
                     e.type == PointerEvent::Type::Up ? e.pos : press.at);
            }
            return true;
        }
        case PointerEvent::Type::Wheel:
            setScroll(scroll_ - e.wheelY * rowHeight * 3);
            return true;
        default:
            return false;
    }
}

bool List::onKey(const KeyEvent& e) {
    if (count_ == 0) return false;
    const std::size_t page = static_cast<std::size_t>(
        std::max(1.0f, rowHeight > 0 ? std::floor(bounds().h / rowHeight) - 1 : 1.0f));
    const std::size_t cur = selected_.value_or(0);
    switch (e.key) {
        case SDLK_DOWN:
            select(selected_ ? std::min(count_ - 1, cur + 1) : 0, true);
            return true;
        case SDLK_UP:
            select(cur > 0 ? cur - 1 : 0, true);
            return true;
        case SDLK_PAGEDOWN:
            select(std::min(count_ - 1, cur + page), true);
            return true;
        case SDLK_PAGEUP:
            select(cur > page ? cur - page : 0, true);
            return true;
        case SDLK_HOME:
            select(0, true);
            return true;
        case SDLK_END:
            select(count_ - 1, true);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            const auto row = selected_;
            auto activate = onActivate;
            if (row && activate) activate(*row);
            return row.has_value();
        }
        default:
            return false;
    }
}

std::string List::valueText() const {
    return selected_ ? std::to_string(*selected_) : std::string();
}

}  // namespace supple::ui
