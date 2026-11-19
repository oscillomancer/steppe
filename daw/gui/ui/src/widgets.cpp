// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/ui/widgets.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>

namespace supple::ui {

Label::Label(std::string id, std::string text, Font font, std::string sizeToken,
             std::string colorToken)
    : Node(std::move(id), Role::Label),
      text_(std::move(text)),
      font_(font),
      size_(std::move(sizeToken)),
      color_(std::move(colorToken)) {
    layout.width = SizeRule::content();
    layout.height = SizeRule::content();
}

void Label::setText(std::string text) {
    if (text == text_) return;
    text_ = std::move(text);
    invalidate();
    if (layout.width.kind == SizeRule::Kind::Content) relayout();
}

Size Label::measure(const Canvas& canvas, Size) {
    const float size = canvas.theme().type(size_);
    const Insets& p = layout.padding;
    return {canvas.textWidth(text_, font_, size) + p.left + p.right,
            size * 1.4f + p.top + p.bottom};
}

void Label::paint(Canvas& canvas) {
    const Insets& p = layout.padding;
    const Rect b = bounds();
    const Rect inner{b.x + p.left, b.y + p.top, std::max(0.0f, b.w - p.left - p.right),
                     std::max(0.0f, b.h - p.top - p.bottom)};
    canvas.text(inner, text_, font_, canvas.theme().type(size_), canvas.theme().color(color_),
                align);
}

Button::Button(std::string id, std::string text, std::function<void()> click)
    : Node(std::move(id), Role::Button), onClick(std::move(click)), text_(std::move(text)) {
    focusable = true;
    interactive = true;
    layout.width = SizeRule::content();
    layout.height = SizeRule::fixed(28);
}

void Button::setText(std::string text) {
    if (text == text_) return;
    text_ = std::move(text);
    invalidate();
    relayout();
}

Size Button::measure(const Canvas& canvas, Size) {
    const Theme& t = canvas.theme();
    return {canvas.textWidth(text_, Font::UiBold, t.type("small")) + 2 * t.space("m"), 28};
}

void Button::setEmphasis(bool on) {
    if (on == emphasis_) return;
    emphasis_ = on;
    invalidate();
}

void Button::setTextColor(std::string token) {
    if (token == textColor_) return;
    textColor_ = std::move(token);
    invalidate();
}

void Button::paint(Canvas& canvas) {
    const Theme& t = canvas.theme();
    const Rect b = bounds();
    if (quiet) {
        const bool lit = enabled && (pressed_ || hovered());
        if (lit) {
            canvas.fillRound(b, t.radius("s"), t.color(pressed_ ? "selection" : "raised"));
            canvas.strokeRound(b, t.radius("s"), t.color("rule"));
        }
        if (focused()) canvas.strokeRound(b, t.radius("s"), t.color("focus"));
        canvas.text(b, text_, Font::UiBold, t.type("small"),
                    t.color(!enabled              ? "ink-faint"
                            : !textColor_.empty() ? textColor_.c_str()
                            : lit || emphasis_    ? "ink"
                                                  : "ink-quiet"),
                    kCenter | kMiddle);
        return;
    }
    const Color face = !enabled    ? t.color("panel")
                       : pressed_  ? t.color("selection")
                       : hovered() ? t.color("raised")
                                   : t.color("panel");
    canvas.fillRound(b, t.radius("s"), face);
    canvas.strokeRound(b, t.radius("s"), focused() ? t.color("focus") : t.color("rule"));
    canvas.text(b, text_, Font::UiBold, t.type("small"),
                !enabled              ? t.color("ink-faint")
                : !textColor_.empty() ? t.color(textColor_.c_str())
                                      : t.color("ink"),
                kCenter | kMiddle);
}

bool Button::onPointer(const PointerEvent& e) {
    if (e.button != 1 && e.type != PointerEvent::Type::Move && e.type != PointerEvent::Type::Cancel)
        return false;
    switch (e.type) {
        case PointerEvent::Type::Down:
            pressed_ = true;
            invalidate();
            return true;
        case PointerEvent::Type::Cancel:
            pressed_ = false;
            invalidate();
            return true;
        case PointerEvent::Type::Up: {
            const bool fire = pressed_ && bounds().contains(e.pos);
            pressed_ = false;
            invalidate();
            auto callback = onClick;
            if (fire && callback) callback();
            return true;
        }
        default:
            return false;
    }
}

// Enter activates; Space is never a button's (it is play/stop everywhere).
bool Button::onKey(const KeyEvent& e) {
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        auto callback = onClick;
        if (callback) callback();
        return true;
    }
    return false;
}

}  // namespace supple::ui
