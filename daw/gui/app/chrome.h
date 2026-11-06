// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Menus and the toolbar, built from actions (gui.md §6) and customizable in
// $XDG_CONFIG_HOME/<slug>/menus.json (supple.menus/1.0):
//   {"schema": "supple.menus/1.0",
//    "menus": [{"title": "File", "items": ["project.save", "-", "ui.quit"]}],
//    "toolbar": ["transport.toggle", "history.undo"]}
// Also edited in the app (gap log #37): the actions list adds an action to the
// toolbar (Ctrl+T) or a menu (Ctrl+M), a right-click on a toolbar button
// moves or removes it; each edit is saved to that file.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app_actions.h"
#include "supple/json/value.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct ChromeLayout {
    struct Menu {
        std::string title;
        std::vector<std::string> items;  // action ids; "-" is a separator
    };
    std::vector<Menu> menus;
    std::vector<std::string> toolbar;
    // The default actions this layout has seen (saved as "known"): a default
    // added since appears in its default place when the layout loads; one
    // the user removed is not brought back.
    std::set<std::string, std::less<>> known;
    // Every action id the defaults hold (menus and toolbar).
    static std::set<std::string, std::less<>> defaultIds();
    static ChromeLayout defaults();
    static Result<ChromeLayout> load(const std::filesystem::path& file);  // defaults when absent
    static std::filesystem::path defaultFile(std::string_view slug);
    Status save(const std::filesystem::path& file) const;
    json::Value toJson() const;
};

// A menu whose entries are built when it opens (the Item menu:
// ItemGestures::itemBarMenu); the layout's action ids for that title follow.
using MenuSource = std::function<std::vector<ui::MenuItem>()>;
using MenuSources = std::map<std::string, MenuSource, std::less<>>;

// The menu bar and toolbar as one row node.
std::unique_ptr<ui::Node> makeChrome(ui::Root& root, AppActions& actions, ChromeLayout layout,
                                     MenuSources sources = {});

// The chrome, editable from the GUI: edits rebuild the bar in place and are
// saved to `file` (empty: kept in memory only).
class Chrome {
  public:
    Chrome(ui::Root& root, AppActions& actions, ChromeLayout layout, std::filesystem::path file);
    // The bar to put in the window (the chrome keeps a pointer to refill it).
    std::unique_ptr<ui::Node> build();
    const ChromeLayout& layout() const { return layout_; }
    bool inToolbar(const std::string& id) const;
    bool inMenu(const std::string& title, const std::string& id) const;
    // Adds the action to the toolbar's end, or removes it if it is there.
    void toggleToolbar(const std::string& id);
    // Moves a toolbar action `delta` places (clamped).
    void moveTool(const std::string& id, int delta);
    // Adds the action to the menu's end, or removes it if the menu has it.
    void toggleMenu(const std::string& title, const std::string& id);
    void reset();  // the default menus and toolbar
    // Builds the titled menu's leading entries when it opens (then refills the bar).
    void setMenuSource(std::string title, MenuSource source);
    // A toolbar button's right-click menu (also the tests' way in).
    void openToolMenu(const std::string& id, const ui::Rect& anchor);
    std::function<void(const std::string&)> onError;

  private:
    void changed();
    void fill();
    ui::Root& root_;
    AppActions& actions_;
    ChromeLayout layout_;
    MenuSources sources_;
    std::filesystem::path file_;
    ui::Node* bar_ = nullptr;
};

}  // namespace supple::gui
