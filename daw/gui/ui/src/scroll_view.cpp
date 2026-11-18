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

namespace {
struct Word {
    std::size_t run;
    std::string text;
    bool spaceBefore;
};
struct Placed {
    std::size_t word;
    float x;
    int line;
};
std::vector<Word> wordsOf(const std::vector<TextRun>& runs) {
    std::vector<Word> words;
    bool space = false;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const std::string& t = runs[r].text;
        std::size_t i = 0;
        while (i < t.size()) {
            if (t[i] == ' ') {
                space = true;
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < t.size() && t[j] != ' ') ++j;
            words.push_back({r, t.substr(i, j - i), space});
            space = false;
            i = j;
        }
    }
    return words;
}
// Greedy line breaking over the words: a word moves down when it would cross `width`.
int placeWords(const Canvas& c, const std::vector<TextRun>& runs, const std::vector<Word>& words,
               float size, float width, std::vector<Placed>* out) {
    float x = 0;
    int line = 0;
    bool first = true;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const Word& w = words[i];
        const Font font = runs[w.run].font;
        const float gap = (!first && w.spaceBefore) ? c.textWidth(" ", font, size) : 0.0f;
        const float len = c.textWidth(w.text, font, size);
        if (!first && x + gap + len > width) {
            ++line;
            x = 0;
        } else {
            x += gap;
        }
        if (out) out->push_back({i, x, line});
        x += len;
        first = false;
    }
    return line + 1;
}
}  // namespace

RichText::RichText(std::string id, std::vector<TextRun> runs, std::string sizeToken)
    : Node(std::move(id), Role::Label), runs_(std::move(runs)), size_(std::move(sizeToken)) {
    layout.height = SizeRule::content();
}

void RichText::setRuns(std::vector<TextRun> runs) {
    runs_ = std::move(runs);
    invalidate();
    relayout();
}

std::string RichText::valueText() const {
    std::string all;
    for (const TextRun& r : runs_) all += r.text;
    return all;
}

Size RichText::measure(const Canvas& c, Size available) {
    const float size = c.theme().type(size_);
    const Insets& p = layout.padding;
    const float width = std::max(1.0f, available.w - p.left - p.right);
    const int lines = placeWords(c, runs_, wordsOf(runs_), size, width, nullptr);
    return {available.w, static_cast<float>(std::max(1, lines)) * size * 1.4f + p.top + p.bottom};
}

void RichText::paint(Canvas& c) {
    const float size = c.theme().type(size_);
    const Insets& p = layout.padding;
    const Rect b = bounds();
    const auto words = wordsOf(runs_);
    std::vector<Placed> placed;
    placeWords(c, runs_, words, size, std::max(1.0f, b.w - p.left - p.right), &placed);
    for (const Placed& at : placed) {
        const Word& w = words[at.word];
        const TextRun& run = runs_[w.run];
        const float len = c.textWidth(w.text, run.font, size);
        c.text({b.x + p.left + at.x, b.y + p.top + static_cast<float>(at.line) * size * 1.4f,
                len + 2.0f, size * 1.4f},
               w.text, run.font, size, c.theme().color(run.colorToken));
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
