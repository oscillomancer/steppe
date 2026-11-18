// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Wrapped text and a vertical scroll container (transcripts, long lists of
// heterogeneous rows, forms).
#pragma once

#include <string>
#include <vector>

#include "supple/ui/list.h"
#include "supple/ui/node.h"

namespace supple::ui {

// A paragraph wrapped to its width; its content height follows the width.
class TextBlock : public Node {
  public:
    TextBlock(std::string id, std::string text, Font font = Font::Ui,
              std::string sizeToken = "small", std::string colorToken = "ink");
    void setText(std::string text);
    void append(std::string_view more);  // streaming text
    const std::string& text() const { return text_; }
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    std::string valueText() const override { return text_; }

  private:
    std::string text_;
    Font font_;
    std::string size_, color_;
};

// A paragraph wrapped to its width whose words differ in weight or ink: a sentence with a name in
// bold, a first sentence in full ink and the rest quiet. Words break at spaces, across runs.
struct TextRun {
    std::string text;
    Font font = Font::Ui;
    std::string colorToken = "ink";
};
class RichText : public Node {
  public:
    RichText(std::string id, std::vector<TextRun> runs, std::string sizeToken = "small");
    void setRuns(std::vector<TextRun> runs);
    const std::vector<TextRun>& runs() const { return runs_; }
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    std::string valueText() const override;  // the runs joined: what a test reads

  private:
    std::vector<TextRun> runs_;
    std::string size_;
};

// One child (`content()`, a column by default) taller than the view, scrolled
// vertically with the wheel and a scroll bar.
class ScrollView : public Node {
  public:
    explicit ScrollView(std::string id);
    Node& content() { return *content_; }
    void scrollTo(float y);
    void scrollToEnd();
    // Keep the end in view as content grows (a transcript), until the user
    // scrolls up.
    bool stickToEnd = false;
    float scroll() const { return scroll_; }
    float contentHeight() const { return contentHeight_; }

    void arrange(const Canvas& canvas, const Rect& bounds) override;
    bool onPointer(const PointerEvent& e) override;

  private:
    Node* content_ = nullptr;
    ScrollBar* bar_ = nullptr;
    float scroll_ = 0, contentHeight_ = 0;
    bool atEnd_ = true;
};

}  // namespace supple::ui
