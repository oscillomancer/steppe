// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The top edge of a resizable dock (the mixer, the MIDI editor): drag to set
// the panel's height, double-click to restore its default. It lights up under
// the pointer and while dragged.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "supple/ui/node.h"

namespace supple::gui {

class PanelGrip : public ui::Node {
  public:
    PanelGrip(std::string id, float thickness) : Node(std::move(id)) {
        interactive = true;
        layout.height = ui::SizeRule::fixed(thickness);
    }
    std::function<float()> height;      // the panel's height now
    std::function<void(float)> resize;  // a new height
    std::function<void()> reset;        // the default
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const bool live = hovered() || dragging_;
        c.fillRect(bounds(), live ? t.color("selection") : t.color("panel"));
        c.fillRect({bounds().x, bounds().y, bounds().w, 1}, t.color("rule"));
        if (live)
            c.fillRect({bounds().x + bounds().w / 2 - 16, bounds().y + 1, 32, 2}, t.color("focus"));
    }
    void onHover(bool) override { invalidate(); }
    ui::Cursor cursorAt(ui::Point) const override { return ui::Cursor::ResizeVertical; }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            if (e.clicks >= 2) {
                if (reset) reset();
                return true;
            }
            dragging_ = true;
            pressY_ = e.pos.y;
            start_ = height ? height() : 0;
            invalidate();
            return true;
        }
        if (e.type == ui::PointerEvent::Type::Move && dragging_) {
            if (resize) resize(start_ + (pressY_ - e.pos.y));  // up makes it taller
            return true;
        }
        if ((e.type == ui::PointerEvent::Type::Up || e.type == ui::PointerEvent::Type::Cancel) &&
            dragging_) {
            dragging_ = false;
            invalidate();
            return true;
        }
        return false;
    }

  private:
    bool dragging_ = false;
    float pressY_ = 0, start_ = 0;
};

}  // namespace supple::gui
