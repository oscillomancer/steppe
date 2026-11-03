// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Single-line text editing: caret and selection on UTF-8 boundaries, word
// motion, clipboard, IME composition shown inline, horizontal scrolling.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "supple/ui/node.h"

namespace supple::ui {

class TextField : public Node {
  public:
    explicit TextField(std::string id, std::string text = {}, std::string placeholder = {});

    std::function<void(const std::string&)> onChange;  // every edit
    std::function<void(const std::string&)> onSubmit;  // Enter
    std::function<void()> onCancel;                    // Escape
    std::function<void(const std::string&)> onBlur;    // focus moved away (a click elsewhere)
    // Keys the field does not use (Up/Down for a list under a search field).
    std::function<bool(const KeyEvent&)> onOtherKey;
    bool secret = false;  // show dots (API keys); copy and cut are disabled

    const std::string& text() const { return text_; }
    const std::string& placeholder() const { return placeholder_; }
    void setPlaceholder(std::string placeholder) {
        if (placeholder == placeholder_) return;
        placeholder_ = std::move(placeholder);
        invalidate();
    }
    void setText(std::string text);  // does not call onChange
    void selectAll();
    std::pair<std::size_t, std::size_t> selection() const {
        return {std::min(anchor_, caret_), std::max(anchor_, caret_)};
    }
    std::size_t caret() const { return caret_; }
    const std::string& composition() const { return composing_; }

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    bool onText(std::string_view text) override;
    bool onComposition(std::string_view text, int cursor) override;
    void onFocus(bool focused) override;
    std::string valueText() const override { return text_; }

  private:
    void insert(std::string_view s);
    void erase(std::size_t from, std::size_t to);
    void moveTo(std::size_t pos, bool extend);
    std::size_t hit(float x) const;
    static std::string dots(const std::string& s);
    std::size_t dotOffset(std::size_t byte) const;
    void changed();
    std::string text_, placeholder_, composing_;
    int composingCursor_ = 0;
    std::size_t caret_ = 0, anchor_ = 0;  // byte offsets on UTF-8 boundaries
    float scroll_ = 0;                    // design units scrolled left
    bool dragging_ = false;
    std::vector<std::pair<std::size_t, float>> carets_;  // from the last paint
};

}  // namespace supple::ui
