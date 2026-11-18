// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/scroll_view.h"

#include <algorithm>

namespace supple::ui {

TextBlock::TextBlock(std::string id, std::string text, Font font, std::string sizeToken,
                     std::string colorToken)
    : Node(std::move(id), Role::Label),
      text_(std::move(text)),
      font_(font),
      size_(std::move(sizeToken)),
      color_(std::move(colorToken)) {
    layout.height = SizeRule::content();
}

void TextBlock::setText(std::string text) {
    text_ = std::move(text);
    invalidate();
    relayout();
}

void TextBlock::append(std::string_view more) {
    text_ += more;
    invalidate();
    relayout();
}

Size TextBlock::measure(const Canvas& c, Size available) {
    const float size = c.theme().type(size_);
    const Insets& p = layout.padding;
    const float width = std::max(1.0f, available.w - p.left - p.right);
    const auto lines = c.wrap(text_, font_, size, width);
    return {available.w, static_cast<float>(std::max<std::size_t>(1, lines.size())) * size * 1.4f +
                             p.top + p.bottom};
}

void TextBlock::paint(Canvas& c) {
    const float size = c.theme().type(size_);
    const Insets& p = layout.padding;
    const Rect b = bounds();
    float y = b.y + p.top;
    for (const std::string& line :
         c.wrap(text_, font_, size, std::max(1.0f, b.w - p.left - p.right))) {
        c.text({b.x + p.left, y, b.w - p.left - p.right, size * 1.4f}, line, font_, size,
               c.theme().color(color_));
        y += size * 1.4f;
    }
}

ScrollView::ScrollView(std::string id) : Node(std::move(id), Role::Group) {
    interactive = true;
    clipsChildren = true;
    content_ = &emplace<Node>(this->id() + ".content");
    content_->layout.dir = Layout::Dir::Column;
    bar_ = &emplace<ScrollBar>(this->id() + ".scroll", ScrollBar::Axis::Vertical);
    bar_->onScroll = [this](float v) { scrollTo(v); };
}

void ScrollView::scrollTo(float y) {
    const float max = std::max(0.0f, contentHeight_ - bounds().h);
    const float v = std::clamp(y, 0.0f, max);
    atEnd_ = v >= max - 1;
    if (v == scroll_) return;
    scroll_ = v;
    relayout();
    invalidate();
}

void ScrollView::scrollToEnd() { scrollTo(contentHeight_); }

Size ScrollView::measure(const Canvas& c, Size available) {
    const Size inner =
        content_->measure(c, {std::max(0.0f, available.w - ScrollBar::kThickness), available.h});
    return {inner.w + ScrollBar::kThickness, inner.h};
}

void ScrollView::arrange(const Canvas& c, const Rect& r) {
    setBounds(r);
    const float width = r.w - ScrollBar::kThickness;
    contentHeight_ = content_->measure(c, {width, r.h}).h;
    const float max = std::max(0.0f, contentHeight_ - r.h);
    if (stickToEnd && atEnd_) scroll_ = max;  // follow growing content
    scroll_ = std::clamp(scroll_, 0.0f, max);
    content_->arrange(c, {r.x, r.y - scroll_, width, std::max(contentHeight_, r.h)});
    bar_->arrange(c, {r.right() - ScrollBar::kThickness, r.y, ScrollBar::kThickness, r.h});
    bar_->setRange(contentHeight_, r.h);
    bar_->setValue(scroll_);
}

bool ScrollView::onPointer(const PointerEvent& e) {
    if (e.type != PointerEvent::Type::Wheel) return false;
    scrollTo(scroll_ - e.wheelY * 48);
    return true;
}

}  // namespace supple::ui
