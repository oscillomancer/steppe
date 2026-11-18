// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The side panel's Browser tab (docs/specs/gui.md §4.14): a row of kinds
// over one page per kind. Plug-ins is the plugin browser built as a panel's
// content; Chains lists the saved FX chain presets. What is chosen goes on
// the selected tracks, or on the track it is dragged onto, through commands
// that exist (`fx.add`, `fx.chain.load`).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app_actions.h"
#include "plugin_drag.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class List;
class TextBlock;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

class FxWindow;

class SideBrowser {
  public:
    // `ui` (the selection) and the others outlive the browser.
    SideBrowser(ui::Root& root, engine::EngineApp& app, cmd::Dispatcher& dispatcher,
                const UiState& ui, FxWindow& fx, PluginDrag& drag);
    std::unique_ptr<ui::Node> build();  // once
    // The tab came into view or left it: in view, the kind shown reads its
    // list again (the scan cache and favourites, the chains folder).
    void setVisible(bool visible);
    // The kinds, in the row's order: "plugins", "chains". Unknown: ignored.
    void show(const std::string& kind);
    const std::string& kind() const { return kind_; }
    void focusSearch();  // the shown kind's search field; typing searches

    // The chains as listed (after the search), for tests: a row's name and
    // what is at its right ("3 effects").
    struct ChainRow {
        std::string name, detail;
    };
    std::size_t chainCount() const { return shown_.size(); }
    ChainRow chainRow(std::size_t row) const;

  private:
    struct Chain {
        std::string name, path;
        int effects = 0;
    };
    std::unique_ptr<ui::Node> buildChains();
    void reload();        // the kind in view
    void filterChains();  // by the search
    void addChain(std::size_t row);
    Status loadChain(const Chain& chain, const std::vector<model::Id>& tracks);
    void setChainStatus(std::string text);

    ui::Root& root_;
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    const UiState& ui_;
    FxWindow& fx_;
    PluginDrag& drag_;
    std::string kind_ = "plugins";
    bool visible_ = false;
    ui::Node* kinds_ = nullptr;
    ui::Node* pluginsPage_ = nullptr;
    ui::Node* chainsPage_ = nullptr;
    ui::TextField* chainSearch_ = nullptr;
    ui::List* chainList_ = nullptr;
    ui::Label* chainStatus_ = nullptr;
    ui::TextBlock* chainEmpty_ = nullptr;
    std::vector<Chain> chains_;       // as fx.chain.list gave them
    std::vector<std::size_t> shown_;  // indices into chains_, after the search
    std::optional<Chain> dragged_;    // the chain in a drag
};

}  // namespace supple::gui
