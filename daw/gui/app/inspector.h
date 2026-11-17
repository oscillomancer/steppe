// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The inspector (docs/specs/gui.md §4.14): the side panel's tab that says
// what is selected. It reads the selection and the session each pass and
// executes nothing by itself.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app_actions.h"
#include "supple/engine/app.h"
#include "supple/model/session.h"
#include "supple/ui/canvas.h"
#include "supple/ui/node.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class TextBlock;
}  // namespace supple::ui

namespace supple::gui {

// What the inspector's header says about a selection: a title, a line under
// it, and the colour the thing is drawn in elsewhere (an item's or a track's;
// none for several things or nothing).
struct InspectorSubject {
    enum class Kind { Nothing, Item, Items, Track, Tracks } kind = Kind::Nothing;
    std::string title, detail;
    std::optional<ui::Color> color;
};
// Tracks win when they were selected last (UiState::tracksFocused), else
// items; ids the session no longer holds are left out.
InspectorSubject inspectorSubject(const model::Session& session, const UiState& selection,
                                  const ui::Theme& theme);

class Inspector {
  public:
    Inspector(ui::Root& root, engine::EngineApp& app, const UiState& selection);
    std::unique_ptr<ui::Node> build();
    void setVisible(bool visible);  // the side panel tells it; it reads nothing while hidden
    void sync();                    // every loop pass

  private:
    ui::Root& root_;
    engine::EngineApp& app_;
    const UiState& selection_;
    ui::Node* panel_ = nullptr;
    ui::Node* swatch_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::TextBlock* detail_ = nullptr;
    bool visible_ = false;
    // What the header was last built from.
    std::uint64_t version_ = ~std::uint64_t{0}, generation_ = ~std::uint64_t{0};
    std::vector<std::string> tracks_, items_;
    bool tracksFocused_ = false;
};

}  // namespace supple::gui
