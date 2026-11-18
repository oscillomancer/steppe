// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/text_area.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "supple/ui/root.h"

namespace supple::ui {
namespace {

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::size_t nextBoundary(const std::string& s, std::size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && continuation(s[i])) ++i;
    return i;
}

std::size_t prevBoundary(const std::string& s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && continuation(s[i])) --i;
    return i;
}

bool wordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '#' ||
           (static_cast<unsigned char>(c) & 0x80);
}

std::size_t nextWord(const std::string& s, std::size_t i) {
    while (i < s.size() && !wordChar(s[i]) && s[i] != '\n') i = nextBoundary(s, i);
    if (i < s.size() && s[i] == '\n') return i + 1;
    while (i < s.size() && wordChar(s[i])) i = nextBoundary(s, i);
    return i;
}

std::size_t prevWord(const std::string& s, std::size_t i) {
    if (i > 0 && s[i - 1] == '\n') return i - 1;
    while (i > 0 && !wordChar(s[prevBoundary(s, i)]) && s[i - 1] != '\n') i = prevBoundary(s, i);
    while (i > 0 && wordChar(s[prevBoundary(s, i)])) i = prevBoundary(s, i);
    return i;
}

constexpr float kPad = 6.0f;
constexpr auto kBurst = std::chrono::milliseconds(1000);

}  // namespace

TextArea::TextArea(std::string id, std::string text)
    : Node(std::move(id), Role::TextField), text_(std::move(text)) {
    focusable = true;
    interactive = true;
    reindex();
}

void TextArea::reindex() {
    lines_.assign(1, 0);
    for (std::size_t i = 0; i < text_.size(); ++i)
        if (text_[i] == '\n') lines_.push_back(i + 1);
}

int TextArea::lineOf(std::size_t byte) const {
    const auto it = std::upper_bound(lines_.begin(), lines_.end(), byte);
    return static_cast<int>(it - lines_.begin()) - 1;
}

std::size_t TextArea::lineEnd(int line) const {
    const auto next = static_cast<std::size_t>(line) + 1;
    return next < lines_.size() ? lines_[next] - 1 : text_.size();
}

int TextArea::columnOf(std::size_t byte) const {
    int col = 0;
    for (std::size_t i = lineStart(lineOf(byte)); i < byte && i < text_.size(); ++i)
        col += !continuation(text_[i]);
    return col;
}

std::size_t TextArea::byteAt(int line, int column) const {
    line = std::clamp(line, 0, lineCount() - 1);
    std::size_t i = lineStart(line);
    const std::size_t end = lineEnd(line);
    while (column > 0 && i < end) {
        i = nextBoundary(text_, i);
        --column;
    }
    return std::min(i, end);
}

std::pair<int, int> TextArea::caretLineColumn() const {
    const int line = lineOf(caret_);
    return {line + 1, static_cast<int>(caret_ - lineStart(line)) + 1};
}

void TextArea::setText(std::string text) {
    if (text == text_) return;
    const int line = lineOf(caret_), column = columnOf(caret_);
    text_ = std::move(text);
    composing_.clear();
    reindex();
    caret_ = anchor_ = byteAt(line, column);
    undo_.clear();
    redo_.clear();
    typingBurst_ = false;
    invalidate();
}

void TextArea::setMarks(std::vector<Mark> marks) {
    marks_ = std::move(marks);
    invalidate();
}

float TextArea::gutterWidth() const {
    if (!lineNumbers) return 0;
    int digits = 1;
    for (int n = lineCount(); n >= 10; n /= 10) ++digits;
    return static_cast<float>(std::max(digits, 2)) * charW_ + 2 * kPad + 6;
}

Rect TextArea::textBox() const {
    const Rect b = bounds();
    const float g = gutterWidth();
    return {b.x + g + kPad, b.y + kPad, std::max(0.0f, b.w - g - 2 * kPad),
            std::max(0.0f, b.h - 2 * kPad)};
}

Size TextArea::measure(const Canvas&, Size available) { return available; }

void TextArea::ensureCaretVisible() {
    const Rect box = textBox();
    const int line = lineOf(caret_);
    const float y = static_cast<float>(line) * lineH_;
    if (y < scrollY_) scrollY_ = y;
    if (y + lineH_ > scrollY_ + box.h) scrollY_ = y + lineH_ - box.h;
    const float x = static_cast<float>(columnOf(caret_)) * charW_;
    if (x < scrollX_) scrollX_ = std::max(0.0f, x - 4 * charW_);
    if (x > scrollX_ + box.w - charW_) scrollX_ = x - box.w + 4 * charW_;
    scrollY_ = std::max(0.0f, scrollY_);
}

void TextArea::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    fontSize_ = t.type("mono");
    charW_ = c.textWidth("M", Font::Mono, fontSize_);
    lineH_ = std::round(fontSize_ * 1.5f);
    c.fillRect(b, t.color("ground"));
    if (focused()) c.strokeRect(b, t.color("focus"));
    const float g = gutterWidth();
    c.fillRect({b.x, b.y, g, b.h}, t.color("panel"));
    const Rect box = textBox();
    // Clamp scrolling to the content.
    const float contentH = static_cast<float>(lineCount()) * lineH_;
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentH - box.h + lineH_));

    const int first = std::max(0, static_cast<int>(scrollY_ / lineH_));
    const int last = std::min(lineCount() - 1, static_cast<int>((scrollY_ + box.h) / lineH_) + 1);
    const auto [s0, s1] = selection();
    const float x0 = box.x - scrollX_;
    auto yOf = [&](int line) { return box.y + static_cast<float>(line) * lineH_ - scrollY_; };
    auto xOf = [&](std::size_t byte) { return x0 + static_cast<float>(columnOf(byte)) * charW_; };

    c.save();
    c.clip({b.x, box.y, b.w, box.h});
    for (int line = first; line <= last; ++line) {
        const float y = yOf(line);
        const Mark* mark = nullptr;
        for (const Mark& m : marks_)
            if (m.line == line + 1 && (!mark || (m.error && !mark->error))) mark = &m;
        if (mark) {
            const Color tint = mark->error ? t.color("clip") : t.color("meter-hot");
            c.fillRect({b.x + g, y, b.w - g, lineH_}, tint.withAlpha(0.12f));
            if (lineNumbers) c.fillRound({b.x + 4, y + lineH_ / 2 - 3, 6, 6}, 3, tint);
        }
        if (lineNumbers)
            c.text({b.x, y, g - kPad, lineH_}, std::to_string(line + 1), Font::Mono, fontSize_,
                   t.color("ink-faint"), kRight | kMiddle);
    }
    c.restore();
    c.save();
    c.clip(box);
    for (int line = first; line <= last; ++line) {
        const float y = yOf(line);
        const std::size_t ls = lineStart(line), le = lineEnd(line);
        if (focused() && s1 > s0 && s0 <= le && s1 >= ls && composing_.empty()) {
            const std::size_t a = std::max(s0, ls), z = std::min(s1, le);
            float w = xOf(z) - xOf(a);
            if (s1 > le) w += charW_ * 0.6f;  // the newline is selected
            c.fillRect({xOf(a), y, w, lineH_}, t.color("selection"));
        }
        std::string shown = text_.substr(ls, le - ls);
        const int caretLine = lineOf(caret_);
        if (!composing_.empty() && line == caretLine) shown.insert(caret_ - ls, composing_);
        c.text({x0, y, static_cast<float>(shown.size() + 2) * charW_ + 4, lineH_}, shown,
               Font::Mono, fontSize_, t.color("ink"), kLeft | kMiddle);
        for (const Mark& m : marks_) {
            if (m.line != line + 1) continue;
            // Underline the token at the mark (to the next space).
            std::size_t a = std::min(ls + static_cast<std::size_t>(std::max(0, m.column - 1)), le);
            std::size_t z = a;
            while (z < le && text_[z] != ' ' && text_[z] != '\t') ++z;
            if (z == a) z = std::min(le, nextBoundary(text_, a));
            const float ux = xOf(a), uw = std::max(charW_, xOf(z) - ux);
            const Color col = m.error ? t.color("clip") : t.color("meter-hot");
            for (float x = ux; x < ux + uw; x += 4)
                c.line({x, y + lineH_ - 2}, {std::min(x + 2, ux + uw), y + lineH_ - 3}, col, 1.2f);
        }
        if (!composing_.empty() && line == caretLine)
            c.fillRect(
                {xOf(caret_), y + lineH_ - 3, static_cast<float>(composing_.size()) * charW_, 1},
                t.color("ink"));
    }
    if (focused()) {
        const int line = lineOf(caret_);
        const float cx =
            xOf(caret_) + static_cast<float>(composing_.empty() ? 0 : composingCursor_) * charW_;
        const Rect caret{cx, yOf(line) + 2, 1.5f, lineH_ - 4};
        c.fillRect(caret, t.color("focus"));
        if (Root* r = root()) r->setTextInputArea(caret);
    }
    c.restore();
}

std::size_t TextArea::hit(Point p) const {
    const Rect box = textBox();
    const int line = std::clamp(static_cast<int>(std::floor((p.y - box.y + scrollY_) / lineH_)), 0,
                                lineCount() - 1);
    const int column =
        std::max(0, static_cast<int>(std::lround((p.x - box.x + scrollX_) / charW_)));
    return byteAt(line, column);
}

void TextArea::snapshot(bool typing) {
    const auto now = std::chrono::steady_clock::now();
    // Consecutive typing within a second is one undo step.
    if (typing && typingBurst_ && now - lastTyping_ < kBurst) {
        lastTyping_ = now;
        return;
    }
    undo_.push_back({text_, caret_, anchor_});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    typingBurst_ = typing;
    lastTyping_ = now;
}

void TextArea::undo(bool redo) {
    auto& from = redo ? redo_ : undo_;
    auto& to = redo ? undo_ : redo_;
    if (from.empty()) return;
    to.push_back({text_, caret_, anchor_});
    State s = std::move(from.back());
    from.pop_back();
    text_ = std::move(s.text);
    reindex();
    caret_ = std::min(s.caret, text_.size());
    anchor_ = std::min(s.anchor, text_.size());
    typingBurst_ = false;
    ensureCaretVisible();
    invalidate();
    if (auto callback = onChange) {
        const std::string text = text_;
        callback(text);
    }
}

void TextArea::replace(std::size_t from, std::size_t to, std::string_view with, bool typing) {
    snapshot(typing);
    text_.replace(from, to - from, with);
    reindex();
    caret_ = anchor_ = from + with.size();
    wantColumn_ = -1;
    ensureCaretVisible();
    invalidate();
    if (auto callback = onChange) {
        const std::string text = text_;
        callback(text);
    }
}

void TextArea::moveTo(std::size_t pos, bool extend, bool keepColumn) {
    caret_ = std::min(pos, text_.size());
    if (!extend) anchor_ = caret_;
    if (!keepColumn) wantColumn_ = -1;
    typingBurst_ = false;
    ensureCaretVisible();
    invalidate();
}

bool TextArea::onPointer(const PointerEvent& e) {
    switch (e.type) {
        case PointerEvent::Type::Down: {
            if (e.button != 1) return false;
            const std::size_t at = hit(e.pos);
            if (e.clicks >= 3) {
                const int line = lineOf(at);
                anchor_ = lineStart(line);
                caret_ = std::min(text_.size(), lineEnd(line) + 1);
                invalidate();
            } else if (e.clicks == 2) {
                std::size_t a = at, z = at;
                while (a > 0 && wordChar(text_[prevBoundary(text_, a)])) a = prevBoundary(text_, a);
                while (z < text_.size() && wordChar(text_[z])) z = nextBoundary(text_, z);
                anchor_ = a;
                caret_ = z;
                invalidate();
            } else {
                moveTo(at, (e.mods & kShift) != 0);
                dragging_ = true;
            }
            return true;
        }
        case PointerEvent::Type::Move:
            if (dragging_) moveTo(hit(e.pos), true);
            return dragging_;
        case PointerEvent::Type::Cancel:
        case PointerEvent::Type::Up:
            dragging_ = false;
            return true;
        case PointerEvent::Type::Wheel: {
            if (e.mods & kShift)
                scrollX_ = std::max(0.0f, scrollX_ - (e.wheelY + e.wheelX) * charW_ * 4);
            else {
                scrollY_ = std::max(0.0f, scrollY_ - e.wheelY * lineH_ * 3);
                scrollX_ = std::max(0.0f, scrollX_ - e.wheelX * charW_ * 4);
            }
            invalidate();
            return true;
        }
        default:
            return false;
    }
}

bool TextArea::onKey(const KeyEvent& e) {
    const bool shift = e.mods & kShift, ctrl = e.mods & kCtrl;
    const auto [s0, s1] = selection();
    const int line = lineOf(caret_);
    auto vertical = [&](int delta) {
        if (wantColumn_ < 0) wantColumn_ = columnOf(caret_);
        const int target = line + delta;
        if (target < 0)
            moveTo(0, shift, true);
        else if (target >= lineCount())
            moveTo(text_.size(), shift, true);
        else
            moveTo(byteAt(target, wantColumn_), shift, true);
    };
    const int page = std::max(1, static_cast<int>(textBox().h / lineH_) - 1);
    switch (e.key) {
        case SDLK_LEFT:
            if (!shift && s1 > s0)
                moveTo(s0, false);
            else
                moveTo(ctrl ? prevWord(text_, caret_) : prevBoundary(text_, caret_), shift);
            return true;
        case SDLK_RIGHT:
            if (!shift && s1 > s0)
                moveTo(s1, false);
            else
                moveTo(ctrl ? nextWord(text_, caret_) : nextBoundary(text_, caret_), shift);
            return true;
        case SDLK_UP:
            vertical(-1);
            return true;
        case SDLK_DOWN:
            vertical(1);
            return true;
        case SDLK_PAGEUP:
            vertical(-page);
            return true;
        case SDLK_PAGEDOWN:
            vertical(page);
            return true;
        case SDLK_HOME:
            if (ctrl)
                moveTo(0, shift);
            else {
                // To the first non-space, then to the line start.
                std::size_t first = lineStart(line);
                while (first < lineEnd(line) && text_[first] == ' ') ++first;
                moveTo(caret_ == first ? lineStart(line) : first, shift);
            }
            return true;
        case SDLK_END:
            moveTo(ctrl ? text_.size() : lineEnd(line), shift);
            return true;
        case SDLK_BACKSPACE:
            if (s1 > s0)
                replace(s0, s1, "", false);
            else if (caret_ > 0)
                replace(ctrl ? prevWord(text_, caret_) : prevBoundary(text_, caret_), caret_, "",
                        true);
            return true;
        case SDLK_DELETE:
            if (s1 > s0)
                replace(s0, s1, "", false);
            else if (caret_ < text_.size())
                replace(caret_, ctrl ? nextWord(text_, caret_) : nextBoundary(text_, caret_), "",
                        true);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            if (ctrl) {
                if (auto callback = onSubmit) {
                    const std::string text = text_;
                    callback(text);
                }
                return true;
            }
            // Keep the line's indentation.
            std::size_t i = lineStart(line);
            std::string indent = "\n";
            while (i < lineEnd(line) && (text_[i] == ' ' || text_[i] == '\t')) indent += text_[i++];
            replace(s0, s1, indent, false);
            return true;
        }
        case SDLK_TAB:
            if (ctrl) return false;  // focus traversal stays with the app
            replace(s0, s1, "  ", true);
            return true;
        case SDLK_ESCAPE:
            if (auto callback = onCancel) {
                callback();
                return true;
            }
            return false;
        default:
            break;
    }
    if (ctrl) {
        Root* r = root();
        switch (e.key) {
            case SDLK_A:
                anchor_ = 0;
                caret_ = text_.size();
                invalidate();
                return true;
            case SDLK_C:
                if (r && s1 > s0) r->setClipboard(text_.substr(s0, s1 - s0));
                return true;
            case SDLK_X:
                if (r && s1 > s0) {
                    r->setClipboard(text_.substr(s0, s1 - s0));
                    replace(s0, s1, "", false);
                }
                return true;
            case SDLK_V:
                if (r) {
                    std::string paste = r->clipboard();
                    std::erase(paste, '\r');
                    if (!paste.empty()) replace(s0, s1, paste, false);
                }
                return true;
            case SDLK_Z:
                undo(shift);
                return true;
            case SDLK_Y:
                undo(true);
                return true;
            default:
                break;
        }
    }
    return false;
}

bool TextArea::onText(std::string_view text) {
    composing_.clear();
    const auto [s0, s1] = selection();
    replace(s0, s1, text, true);
    return true;
}

bool TextArea::onComposition(std::string_view text, int cursor) {
    composing_ = std::string(text);
    composingCursor_ = std::clamp(cursor, 0, static_cast<int>(composing_.size()));
    invalidate();
    return true;
}

void TextArea::onFocus(bool focused) {
    if (!focused) {
        composing_.clear();
        dragging_ = false;
        typingBurst_ = false;
    }
    invalidate();
    if (auto callback = onFocusChange) callback(focused);
}

}  // namespace supple::ui
