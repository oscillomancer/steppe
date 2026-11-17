// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The command log and history dock (docs/specs/gui.md §4.10): the current
// branch's versions (undo, redo, jump to a version), the branches (switch,
// create, merge into main, delete), and the command log — every command with
// its origin, arguments and outcome, agent commands visibly attributed.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app_actions.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class List;
class TextBlock;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

class HistoryPanel {
  public:
    HistoryPanel(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                 engine::EngineApp& app);
    // Set before build() when the side panel holds this panel as a tab
    // (gui.md §4.14): `toggle` then asks the host, which shows and hides the
    // panel with setVisible, and the panel leaves out its own title row and
    // width.
    std::function<void()> host;
    std::unique_ptr<ui::Node> build();  // hidden until toggled
    void toggle();
    void setVisible(bool visible);
    bool visible() const;
    void sync();  // every loop pass

    // Tests.
    struct Version {
        std::int64_t version = 0;
        std::string label, origin;
    };
    struct Entry {
        std::int64_t seq = 0, timeMs = 0;
        std::string origin, command, args, outcome;
        bool ok = true;
    };
    const std::vector<Version>& versions() const { return versions_; }
    const std::vector<std::string>& branches() const { return branches_; }
    const std::vector<Entry>& entries() const { return entries_; }
    Status jumpTo(std::size_t versionRow);  // 0 is the head
    Status switchBranch(const std::string& name);
    Status createBranch(const std::string& name);

  private:
    void reloadHistory();
    void pollLog();
    void showEntry(std::size_t row);
    Result<json::Value> exec(const std::string& command, json::Value args);
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    ui::Node* panel_ = nullptr;
    ui::Label* branchLabel_ = nullptr;
    ui::List* versionList_ = nullptr;
    ui::List* branchList_ = nullptr;
    ui::List* logList_ = nullptr;
    ui::TextBlock* detail_ = nullptr;
    ui::TextField* newBranch_ = nullptr;
    std::vector<Version> versions_;
    std::vector<std::string> branches_;
    std::string current_;
    std::vector<Entry> entries_;
    std::uint64_t version_ = ~0ull;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::int64_t lastSeq_ = 0;
    bool canUndo_ = false, canRedo_ = false;
};

}  // namespace supple::gui
