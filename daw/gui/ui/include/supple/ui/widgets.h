// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Basic widgets. Each reads theme tokens only and exposes its value for
// tests (valueText).
#pragma once

#include <functional>
#include <string>

#include "supple/ui/node.h"

namespace supple::ui {

class Label : public Node {
  public:
    Label(std::string id, std::string text, Font font = Font::Ui, std::string sizeToken = "body",
          std::string colorToken = "ink");
    void setText(std::string text);
    const std::string& text() const { return text_; }
    int align = kLeft | kMiddle;
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    std::string valueText() const override { return text_; }

  private:
    std::string text_;
    Font font_;
    std::string size_, color_;
};

class Button : public Node {
  public:
    Button(std::string id, std::string text, std::function<void()> onClick = {});
    std::function<void()> onClick;
    void setText(std::string text);
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override { return text_; }
    // A quiet button has no box at rest: its text alone, in quiet ink; the
    // box appears under the pointer. For the secondary buttons that repeat
    // on every track row and mixer strip (gui.md §7).
    bool quiet = false;
    // A quiet button whose text is in full ink at rest, to say that what it
    // opens is not empty (a track's "fx" with plug-ins in its chain).
    void setEmphasis(bool on);
    bool emphasis() const { return emphasis_; }
    // The text in one theme colour whatever the state, while enabled ("" = the usual): for the
    // one feature that has a colour (a chain holding a disabled plug-in, `meter-hot`).
    void setTextColor(std::string token);
    const std::string& textColor() const { return textColor_; }

  protected:
    bool pressed() const { return pressed_; }

  private:
    std::string text_;
    bool pressed_ = false, emphasis_ = false;
    std::string textColor_;
};

}  // namespace supple::ui
