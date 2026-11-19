// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/text_field.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cctype>

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
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           (static_cast<unsigned char>(c) & 0x80);
}

std::size_t nextWord(const std::string& s, std::size_t i) {
    while (i < s.size() && !wordChar(s[i])) i = nextBoundary(s, i);
    while (i < s.size() && wordChar(s[i])) i = nextBoundary(s, i);
    return i;
}

std::size_t prevWord(const std::string& s, std::size_t i) {
    while (i > 0 && !wordChar(s[prevBoundary(s, i)])) i = prevBoundary(s, i);
    while (i > 0 && wordChar(s[prevBoundary(s, i)])) i = prevBoundary(s, i);
    return i;
}

constexpr float kPad = 8.0f;

}  // namespace

TextField::TextField(std::string id, std::string text, std::string placeholder)
    : Node(std::move(id), Role::TextField),
      text_(std::move(text)),
      placeholder_(std::move(placeholder)) {
    focusable = true;
    interactive = true;
    caret_ = anchor_ = text_.size();
    layout.height = SizeRule::fixed(28);
}

void TextField::setText(std::string text) {
    text_ = std::move(text);
    caret_ = anchor_ = text_.size();
    composing_.clear();
    invalidate();
}

void TextField::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
    invalidate();
}

Size TextField::measure(const Canvas& canvas, Size) {
    const float w = canvas.textWidth(text_.empty() ? placeholder_ : text_, Font::Ui,
                                     canvas.theme().type("body"));
    return {w + 2 * kPad, 28};
}

void TextField::paint(Canvas& c) {
    const Theme& t = c.theme();
    const Rect b = bounds();
    const float size = t.type("body");
    // Disabled, it lies flat on the panel and its text is faint (as a
    // disabled dropdown's is).
    c.fillRound(b, t.radius("s"), enabled ? t.color("raised") : t.color("panel"));
    c.strokeRound(b, t.radius("s"), focused() ? t.color("focus") : t.color("rule"));
    const Rect inner{b.x + kPad, b.y, std::max(0.0f, b.w - 2 * kPad), b.h};

    // What is shown: the text with any composition inserted at the caret
    // (secret fields: one dot per character).
    const std::string shown = secret ? dots(text_)
                              : composing_.empty()
                                  ? text_
                                  : text_.substr(0, caret_) + composing_ + text_.substr(caret_);
    carets_ = c.carets(shown, Font::Ui, size, 0);
    auto xOf = [&](std::size_t byte) {
        if (secret) byte = dotOffset(byte);
        for (const auto& [at, x] : carets_)
            if (at >= byte) return x;
        return carets_.back().second;
    };
    // Keep the caret visible.
    const float caretX =
        xOf(caret_ + (composing_.empty() ? 0 : static_cast<std::size_t>(composingCursor_)));
    if (caretX - scroll_ > inner.w) scroll_ = caretX - inner.w + 1;
    if (caretX - scroll_ < 0) scroll_ = caretX;
    scroll_ =
        std::max(0.0f, std::min(scroll_, std::max(0.0f, carets_.back().second - inner.w + 1)));

    c.save();
    c.clip(inner);
    const float x0 = inner.x - scroll_;
    const auto [s0, s1] = selection();
    if (focused() && s1 > s0 && composing_.empty())
        c.fillRect({x0 + xOf(s0), b.y + 5, xOf(s1) - xOf(s0), b.h - 10}, t.color("selection"));
    if (shown.empty() && !placeholder_.empty())
        c.text({inner.x, b.y, inner.w, b.h}, placeholder_, Font::Ui, size, t.color("ink-faint"));
    else
        c.text({x0, b.y, carets_.back().second + 2, b.h}, shown, Font::Ui, size,
               t.color(enabled ? "ink" : "ink-faint"));
    if (!composing_.empty())  // underline the composition
        c.fillRect(
            {x0 + xOf(caret_), b.bottom() - 7, xOf(caret_ + composing_.size()) - xOf(caret_), 1},
            t.color("ink"));
    if (focused()) {
        const Rect caret{x0 + caretX, b.y + 6, 1.5f, b.h - 12};
        c.fillRect(caret, t.color("focus"));
        if (Root* r = root()) r->setTextInputArea(caret);
    }
    c.restore();
}

std::string TextField::dots(const std::string& s) {
    std::string out;
    for (char ch : s)
        if (!continuation(ch)) out += "\xE2\x80\xA2";  // U+2022, three bytes
    return out;
}

std::size_t TextField::dotOffset(std::size_t byte) const {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < byte && i < text_.size(); ++i) chars += !continuation(text_[i]);
    return chars * 3;
}

std::size_t TextField::hit(float x) const {
    const float local = x - (bounds().x + kPad) + scroll_;
    std::size_t best = 0;
    float bestD = 1e9f;
    for (const auto& [at, cx] : carets_) {
        const float d = std::abs(cx - local);
        if (d < bestD) {
            bestD = d;
            best = at;
        }
    }
    if (secret) {  // dot offset → byte offset in the text
        std::size_t chars = best / 3, i = 0;
        while (i < text_.size() && chars > 0) {
            i = nextBoundary(text_, i);
            --chars;
        }
        return i;
    }
    return std::min(best, text_.size());
}

void TextField::changed() {
    invalidate();
    if (auto callback = onChange) {
        const std::string text = text_;
        callback(text);
    }
}

void TextField::insert(std::string_view s) {
    const auto [s0, s1] = selection();
    text_.replace(s0, s1 - s0, s);
    caret_ = anchor_ = s0 + s.size();
    changed();
}

void TextField::erase(std::size_t from, std::size_t to) {
    if (to <= from) return;
    text_.erase(from, to - from);
    caret_ = anchor_ = from;
    changed();
}

void TextField::moveTo(std::size_t pos, bool extend) {
    caret_ = std::min(pos, text_.size());
    if (!extend) anchor_ = caret_;
    invalidate();
}

bool TextField::onPointer(const PointerEvent& e) {
    switch (e.type) {
        case PointerEvent::Type::Down:
            if (e.button != 1) return false;
            if (e.clicks >= 2) {
                selectAll();
            } else {
                moveTo(hit(e.pos.x), (e.mods & kShift) != 0);
                dragging_ = true;
            }
            return true;
        case PointerEvent::Type::Move:
            if (dragging_) moveTo(hit(e.pos.x), true);
            return dragging_;
        case PointerEvent::Type::Cancel:
        case PointerEvent::Type::Up:
            dragging_ = false;
            return true;
        default:
            return false;
    }
}

bool TextField::onKey(const KeyEvent& e) {
    const bool shift = e.mods & kShift, word = e.mods & kCtrl;
    const auto [s0, s1] = selection();
    switch (e.key) {
        case SDLK_LEFT:
            if (!shift && s1 > s0)
                moveTo(s0, false);
            else
                moveTo(word ? prevWord(text_, caret_) : prevBoundary(text_, caret_), shift);
            return true;
        case SDLK_RIGHT:
            if (!shift && s1 > s0)
                moveTo(s1, false);
            else
                moveTo(word ? nextWord(text_, caret_) : nextBoundary(text_, caret_), shift);
            return true;
        case SDLK_HOME:
            moveTo(0, shift);
            return true;
        case SDLK_END:
            moveTo(text_.size(), shift);
            return true;
        case SDLK_BACKSPACE:
            if (s1 > s0)
                erase(s0, s1);
            else
                erase(word ? prevWord(text_, caret_) : prevBoundary(text_, caret_), caret_);
            return true;
        case SDLK_DELETE:
            if (s1 > s0)
                erase(s0, s1);
            else
                erase(caret_, word ? nextWord(text_, caret_) : nextBoundary(text_, caret_));
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (auto callback = onSubmit) {
                const std::string text = text_;
                callback(text);
            }
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
    if (e.mods & kCtrl) {
        Root* r = root();
        switch (e.key) {
            case SDLK_A:
                selectAll();
                return true;
            case SDLK_C:
                if (r && s1 > s0 && !secret) r->setClipboard(text_.substr(s0, s1 - s0));
                return true;
            case SDLK_X:
                if (r && s1 > s0 && !secret) {
                    r->setClipboard(text_.substr(s0, s1 - s0));
                    erase(s0, s1);
                }
                return true;
            case SDLK_V:
                if (r) {
                    std::string paste = r->clipboard();
                    std::replace(paste.begin(), paste.end(), '\n', ' ');  // single line
                    std::erase(paste, '\r');
                    if (!paste.empty()) insert(paste);
                }
                return true;
            default:
                break;
        }
    }
    auto callback = onOtherKey;
    return callback ? callback(e) : false;
}

bool TextField::onText(std::string_view text) {
    composing_.clear();
    insert(text);
    return true;
}

bool TextField::onComposition(std::string_view text, int cursor) {
    composing_ = std::string(text);
    composingCursor_ = std::clamp(cursor, 0, static_cast<int>(composing_.size()));
    invalidate();
    return true;
}

void TextField::onFocus(bool focused) {
    if (!focused) {
        composing_.clear();
        dragging_ = false;
    }
    invalidate();
    if (!focused)
        if (auto callback = onBlur) callback(text_);
}

}  // namespace supple::ui
