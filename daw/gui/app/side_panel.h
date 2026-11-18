// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel (docs/specs/gui.md §4.14): a dock at the window's right edge
// with one tab per tool (the inspector, the history). It is shown only when
// asked, on the tab asked for; its left edge drags to set its width. Which
// tab is in view, whether the panel is open and how wide it is are UI state.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "supple/json/value.h"
#include "supple/ui/node.h"
#include "supple/ui/root.h"

namespace supple::gui {

class SidePanel {
  public:
    static constexpr float kDefaultWidth = 300, kMinWidth = 240, kMaxWidth = 560;

    explicit SidePanel(ui::Root& root);
    // A tab, in the order added. Its content fills the panel under the tab
    // bar. `shown` is told when the tab comes into view and when it leaves
    // (a tab that reloads on show does it there).
    void addTab(std::string id, std::string title, std::unique_ptr<ui::Node> content,
                std::function<void(bool)> shown = {});
    std::unique_ptr<ui::Node> build();  // after the tabs are added; hidden until asked

    void show(const std::string& tab);    // open, on that tab
    void toggle(const std::string& tab);  // close when that tab is in view, else show it
    void hide();
    bool visible() const;
    // The tab in view, or the one last in view while the panel is closed.
    const std::string& tab() const { return tab_; }
    bool showing(const std::string& tab) const { return visible() && tab_ == tab; }
    float width() const { return width_; }
    void setWidth(float width);  // clamped to kMinWidth..kMaxWidth

    // UI state: {"visible": bool, "tab": id, "width": units}. Unknown tabs and
    // fields of the wrong type are ignored.
    json::Value save() const;
    void load(const json::Value& state);

  private:
    struct Tab {
        std::string id, title;
        std::unique_ptr<ui::Node> pending;  // until build()
        ui::Node* content = nullptr;
        ui::Node* button = nullptr;
        std::function<void(bool)> shown;
    };
    void apply(bool visible, const std::string& tab);
    ui::Root& root_;
    std::vector<Tab> tabs_;
    ui::Node* panel_ = nullptr;
    std::string tab_;
    float width_ = kDefaultWidth;
};

}  // namespace supple::gui
