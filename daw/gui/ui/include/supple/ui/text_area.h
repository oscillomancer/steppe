// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Multi-line plain-text editing in the monospace face: caret and selection on
// UTF-8 boundaries across lines, line numbers, scrolling, local undo, IME
// composition inline, clipboard, and marks (located diagnostics: a gutter dot,
// a tinted line, the token underlined). No wrapping: long lines scroll.
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "supple/ui/node.h"

namespace supple::ui {

class TextArea : public Node {
  public:
    struct Mark {
        int line = 1, column = 1;  // 1-based; column in bytes (the compilers' convention)
        std::string message;
        bool error = true;  // false: a warning
    };

    explicit TextArea(std::string id, std::string text = {});

    std::function<void(const std::string&)> onChange;  // every edit
    std::function<void(const std::string&)> onSubmit;  // Ctrl+Enter
    std::function<void()> onCancel;                    // Escape
    std::function<void(bool focused)> onFocusChange;
    // The gutter with the line numbers and the marks' dots. Off for prose
    // (a notes field); set before the first layout.
    bool lineNumbers = true;

    const std::string& text() const { return text_; }
    // Replaces the text (no onChange); the caret stays at the same line and
    // column where possible, and local undo restarts.
    void setText(std::string text);
    void setMarks(std::vector<Mark> marks);
    const std::vector<Mark>& marks() const { return marks_; }
    std::pair<std::size_t, std::size_t> selection() const {
        return {std::min(anchor_, caret_), std::max(anchor_, caret_)};
    }
    std::size_t caret() const { return caret_; }
    void setCaret(std::size_t byte, bool extend = false) { moveTo(byte, extend); }
    std::pair<int, int> caretLineColumn() const;  // 1-based, column in bytes
    int lineCount() const { return static_cast<int>(lines_.size()); }

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    Cursor cursorAt(Point) const override { return Cursor::Text; }
    bool onKey(const KeyEvent& e) override;
    bool onText(std::string_view text) override;
    bool onComposition(std::string_view text, int cursor) override;
    void onFocus(bool focused) override;
    std::string valueText() const override { return text_; }

  private:
    void reindex();
    int lineOf(std::size_t byte) const;
    std::size_t lineStart(int line) const { return lines_[static_cast<std::size_t>(line)]; }
    std::size_t lineEnd(int line) const;
    int columnOf(std::size_t byte) const;  // code points from the line start
    std::size_t byteAt(int line, int column) const;
    std::size_t hit(Point p) const;
    void replace(std::size_t from, std::size_t to, std::string_view with, bool typing);
    void moveTo(std::size_t pos, bool extend, bool keepColumn = false);
    void snapshot(bool typing);
    void undo(bool redo);
    void ensureCaretVisible();
    float gutterWidth() const;
    Rect textBox() const;

    std::string text_, composing_;
    std::vector<std::size_t> lines_{0};  // byte offset of each line start
    std::vector<Mark> marks_;
    int composingCursor_ = 0;
    std::size_t caret_ = 0, anchor_ = 0;
    int wantColumn_ = -1;  // kept across Up/Down
    float scrollX_ = 0, scrollY_ = 0;
    float charW_ = 7.2f, lineH_ = 18.0f, fontSize_ = 12.0f;
    bool dragging_ = false;
    struct State {
        std::string text;
        std::size_t caret, anchor;
    };
    std::vector<State> undo_, redo_;
    std::chrono::steady_clock::time_point lastTyping_{};
    bool typingBurst_ = false;
};

}  // namespace supple::ui
