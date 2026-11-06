// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The rack window (docs/specs/rack.md §8, gui.md §4.10): a separate,
// resizable utility window, as the routing matrix's. Racks are tabs along
// its top, with "+" for a new one on the selected channel; the space below
// shows the open rack (RackSpace); a module browser on its left lists the
// module types to add, host first, then stock, library and project. Every
// edit is a command (rack.*, module.*, cable.*); a drag is one undo step.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app_actions.h"
#include "floating_panel.h"
#include "rack_space.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class Button;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

class RackTabs;
class RackBrowser;
class PluginBrowser;

class RackWindow {
  public:
    RackWindow(FloatingPanel& panel, AppActions& actions, cmd::Dispatcher& dispatcher,
               engine::EngineApp& app, UiState& ui);
    ~RackWindow();
    // Opens on `rack` (else the last one shown, else the first).
    void open(std::optional<model::Id> rack = std::nullopt);
    void close() { panel_.close(); }
    bool isOpen() const { return space_ != nullptr; }
    void sync();  // reload when the session changed while open
    // Telemetry: reads what the visible modules show; true when it changed.
    bool tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;
    // A new rack on `track` (a channel; none: the selected channel, else a
    // new one), shown.
    Status addRack(std::optional<model::Id> track = std::nullopt);
    std::optional<model::Id> rack() const { return rack_; }
    // Opens a plug-in's editor window (the app's PluginWindows).
    std::function<Status(model::Id fx)> openEditor;
    // Space pressed and released in the space: the transport, as elsewhere.
    std::function<void()> spaceTap;
    // The plug-in browser's favourites (empty: none saved; tests).
    std::filesystem::path favouritesFile;

    // The UI sidecar's part: each rack's view, the rack shown, the browser.
    json::Value saveLayout() const;
    void loadLayout(const json::Value& layout);

    // Tests.
    RackSpace* space() const { return space_; }
    RackBrowser* browser() const { return browser_; }
    std::string status() const;
    // The browser's tag chips as shown, where one is, and the types it lists.
    std::vector<std::string> tagChips() const;
    std::optional<ui::Point> tagChipAt(const std::string& label) const;
    std::vector<std::string> browserShows() const;
    Status addModule(const std::string& type, std::optional<ui::Point> at = std::nullopt);

  private:
    Status reload();
    void show(std::optional<model::Id> rack);
    void refreshTabs();
    void watch(bool on);
    Result<json::Value> exec(const std::string& command, json::Value args,
                             const std::string& gesture = {});
    void report(const Error& e);
    // The space's hooks.
    void move(const std::vector<RackSpace::Move>& moves);
    void param(model::Id module, int index, double value, ui::Gesture g);
    void connect(const model::CableEnd& from, const model::CableEnd& to,
                 std::optional<model::Id> replace);
    void moduleMenu(model::Id module, ui::Point at);
    void spaceMenu(ui::Point at, ui::Point space);
    void paramMenu(model::Id module, int index, ui::Point at);
    void tabMenu(model::Id rack, ui::Point at);
    void rename(model::Id module);

  public:
    // A type's source in a code view: a library module's saves and compiles
    // (module.write); a stock one offers an editable copy (module.fork).
    void showSource(const std::string& type);

  private:
    void renameRack(model::Id rack);
    void deleteSelected();
    ui::Root& root() const { return panel_.root(); }

    FloatingPanel& panel_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    std::shared_ptr<const rack::Catalog> catalog_;
    std::uint64_t version_ = ~std::uint64_t{0};
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::optional<model::Id> rack_;
    std::map<model::Id, RackSpace::View> views_;
    bool browserVisible_ = true;
    int gesture_ = 0;
    std::optional<std::chrono::steady_clock::time_point> nextTelemetry_;
    std::vector<model::Id> watched_;
    RackSpace* space_ = nullptr;
    RackTabs* tabs_ = nullptr;
    RackBrowser* browser_ = nullptr;
    ui::Node* browserColumn_ = nullptr;
    ui::Label* status_ = nullptr;
    std::unique_ptr<PluginBrowser> plugins_;  // picks a plugin module's plug-in
    ui::Button* zoom_ = nullptr;
    ui::Button* browserToggle_ = nullptr;
    std::string error_;
};

}  // namespace supple::gui
