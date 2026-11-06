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
#include "rack_plugin_params.h"
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
    // `amount`: a parameter cable's (else the command's default).
    void connect(const model::CableEnd& from, const model::CableEnd& to,
                 std::optional<model::Id> replace, std::optional<double> amount = std::nullopt);
    // A cable dropped on a plugin module: its plug-in's modulatable
    // parameters in a searchable list; choosing one makes the cable.
    void pickParam(const model::CableEnd& from, model::Id module, std::optional<model::Id> replace,
                   ui::Point at);
    // A cable's right-click: what it reaches, its amount, remove.
    void cableMenu(model::Id cable, ui::Point at);
    // A plug-in's parameters (asked once per plug-in and version).
    const racks::PluginParams& paramsOf(model::Id fx);
    // A search list of what can be added, at the pointer (`text` typed so
    // far); the choice lands at `space`, and takes `from`'s cable if given.
    void quickAdd(ui::Point at, ui::Point space, const std::string& text,
                  std::optional<model::CableEnd> from, bool fromOutput);
    // Adds `type` at `space` (its top centred there) and plugs `from` into its
    // first port of the same signal (else its first): one undo step.
    Status addWired(const std::string& type, ui::Point space, std::optional<model::CableEnd> from,
                    bool fromOutput);
    // The selection's copy: its modules and the cables among them.
    void copySelected();
    void duplicateSelected();
    void moduleMenu(model::Id module, ui::Point at);
    void spaceMenu(ui::Point at, ui::Point space);
    void paramMenu(model::Id module, int index, ui::Point at);
    void tabMenu(model::Id rack, ui::Point at);
    // MIDI learn for a knob: the learn dialog listens, then module.learn.set.
    void learn(model::Id module, int index);
    // Rack presets (rack.preset.*): listed in the browser after the types.
    void readPresets();
    void presetsSaved();

  public:
    // Pastes the copied modules with `space` their top-left: a duplicate in
    // the rack they came from while they are there (plug-in state and all),
    // else rack.patch from the copy.
    Status paste(ui::Point space);
    // A right-click on a browser row (a type, or "preset:<name>").
    void browserMenu(const std::string& type, ui::Point at);
    // Loads a saved preset into the rack shown (at a space point, else
    // beside its modules), selected and in view.
    Status loadPreset(const std::string& name, std::optional<ui::Point> at = std::nullopt);
    // Asks a name and saves the rack, or `modules` of it, as a preset.
    void savePreset(std::optional<std::vector<model::Id>> modules);

  private:
    void rename(model::Id module);

  public:
    // A type's source in a code view: a library module's saves and compiles
    // (module.write); a stock one offers an editable copy (module.fork).
    void showSource(const std::string& type);

  private:
    void renameRack(model::Id rack);
    void deleteSelected();
    // rack.arrange: the selection when it holds several modules, else the
    // whole rack; then shows what moved.
    void tidyUp();
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
    json::Value presets_ = json::Value::object();  // rack.preset.list's
    bool presetsRead_ = false;
    // Copied modules (Ctrl+C or Ctrl+X): as they were, and the cables among them.
    struct Clip {
        model::Id rack;
        std::vector<std::pair<model::Id, model::RackModule>> modules;
        std::vector<model::Cable> cables;
    };
    std::optional<Clip> clip_;
    // paramsOf's answers by fx, with the plug-in they were asked of.
    std::map<model::Id, std::pair<std::string, racks::PluginParams>> pluginParams_;
};

}  // namespace supple::gui
