// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The FX chain window (docs/specs/gui.md §4.5): a floating panel for one track's
// chain — per-effect name, bypass and offline (distinct, fx.set), wet/dry,
// latency, editor, reorder and remove — plus the input chain, the plugin
// browser and a semantic parameter tab. Every change is a command on the bus.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "floating_panel.h"
#include "fx_params.h"
#include "plugin_browser.h"
#include "rack_controls.h"
#include "rack_device.h"
#include "rack_plugin_params.h"
#include "supple/engine/app.h"
#include "supple/model/ids.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
class Label;
class Button;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

struct UiState;

class FxWindow {
  public:
    struct Hooks {
        // Open (or close, when open) a plugin's own editor (PluginWindows in the app);
        // errors are shown.
        std::function<Status(model::Id)> openEditor;
        // A failure with no window open to show it (the browser's add).
        std::function<void(const std::string&)> onError = {};
        // The MIDI learn dialog, for the parameter panel's "MIDI" buttons.
        std::function<void(LearnRequest)> openLearn = {};
        // The rack window on a rack (a channel's Rack device's Open).
        std::function<void(model::Id rack)> openRack = {};
    };

    FxWindow(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& mainRoot,
             FloatingPanel& panel, Hooks hooks, std::filesystem::path favouritesFile);
    ~FxWindow();

    Status open(model::Id track);
    // A take's own chain (take FX, gap log #22): the item plays through it
    // before its track's input.
    Status openTake(model::Id take);
    std::optional<model::Id> take() const { return take_; }
    // The `ui.fx.chain` action: a track id in `prompt`, else the selected track.
    Status openFor(const UiState& ui, const json::Value& prompt);
    // The plugin browser alone, adding to that track's chain, or with no
    // track given to every selected track's (ui.fx.add).
    Status openToAdd(const UiState& ui, const json::Value& prompt);
    void close();
    bool isOpen() const { return page_ != nullptr; }

    // Frame pass: refresh after changes made elsewhere (agents, undo).
    void tick(bool event = true);
    // When the Rack devices' knobs next read their Macros' telemetry.
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;
    // Tests: the chain's Rack devices (racks that feed the track).
    std::vector<RackDevice*> rackDevices() const { return devices_; }
    // Before dispatching floating input: project commands may have run earlier
    // in the same SDL event batch, before the next frame's regular tick.
    void syncProject() {
        if (app_.projectGeneration() != projectGeneration_) tick(false);
    }
    void refresh();
    void setStatus(std::string message);

    // MIDI learn's "enable only when" (ADR-0027): the window shows `fx` (its
    // track's or take's chain), and has the keyboard focus.
    bool shows(model::Id fx) const;
    bool hasFocus() const;

    // State the tests read back.
    model::Id track() const { return track_; }
    model::Id selected() const { return selected_; }
    // The badge a row shows: how the effect runs (in-process / shared bridge /
    // isolated), offline, and failed. Pure, so the composition is testable
    // without an engine that can crash a bridge.
    static std::string badgeText(const model::FxInstance& fx, bool failed);
    // The pins button's text (ADR-0020): "pins 1/2", "3/4 → 1/2", "key 3/4".
    static std::string pinsText(const model::PinMap& pins);
    // The pins menu of a track-chain effect: input pair, output pair, key.
    void openPinsMenu(model::Id fx, const ui::Rect& anchor);
    // A note-output plug-in's place on the track's MIDI bus (ADR-0068).
    void openMidiOutputMenu(model::Id fx, const ui::Rect& anchor);
    // Engine-reported failures (engine.status failed_fx), by effect id.
    const std::set<std::string>& failed() const { return failed_; }
    void select(model::Id fx);  // alone: any multi-selection ends
    // The selected effect, `delta` places along the track's chain (fx.move;
    // ui.fx.move_up/down, Alt+Up/Down).
    void moveSelected(int delta);
    // Every selected effect, top to bottom (Ctrl/Shift+click select several;
    // their bypass, offline and remove act on all of them).
    std::vector<model::Id> selection() const;
    // A left press on an effect's row (a double click opens its editor).
    void press(model::Id fx, unsigned mods, int clicks = 1);
    void showParamsTab();
    void showChainTab();
    std::vector<model::Id> chain() const;
    std::vector<model::Id> inputChain() const;
    PluginBrowser& browser() {
        return mainBrowser_->isOpen() || !browser_ ? *mainBrowser_ : *browser_;
    }

  private:
    enum class Tab { Chain, Params };
    bool hasSelected() const { return selected_.n != 0; }
    std::unique_ptr<ui::Node> buildPage();
    void buildChain(ui::Node& body);
    void buildParams(ui::Node& body);
    void buildRow(ui::Node& body, model::Id fx, std::size_t index, bool input);
    Status dispatch(const char* command, json::Value args);
    std::string nameOf(model::Id fx) const;
    bool picked(model::Id fx) const;
    std::vector<model::Id> rowOrder() const;
    // The effects a row's bypass/offline/remove act on: the selection when
    // the row is part of a multi-selection, else the row's alone.
    std::vector<model::Id> targets(model::Id fx) const;
    Status setEach(model::Id fx, const char* field, bool value);
    std::string latencyOf(model::Id fx);
    bool outputsNotes(model::Id fx);                     // cached by plug-in and live instance
    void dragRow(model::Id fx, int phase, ui::Point p);  // phase: Row::Phase
    void openBrowser(bool input = false);
    void addFx(const std::string& pluginId);
    void addFxTo(const std::vector<model::Id>& tracks, const std::string& pluginId);
    void promptSaveChain();
    void hideSavePrompt();
    void saveChain();
    void loadChainMenu(bool replace);
    void choosePresetChain(bool input);
    // The header's track switch: the arrows step `delta` strips along the
    // mixer's order (wrapping), the title's menu lists every strip.
    void showTrack(model::Id track);
    void stepTrack(int delta);
    void trackMenu();
    // A plugin's presets (fx.preset.*): the row's "presets" menu, and the name
    // field its "Save preset…" shows.
    void presetMenu(model::Id fx);
    void promptSavePreset(model::Id fx);
    void hidePresetPrompt();
    void savePreset();
    void checkFailures();  // engine.status failed_fx, at most twice a second

    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    ui::Root& root() const { return panel_.root(); }
    FloatingPanel& panel_;
    std::filesystem::path favouritesFile_;
    Hooks hooks_;
    std::unique_ptr<PluginBrowser> browser_;
    std::unique_ptr<PluginBrowser> mainBrowser_;
    std::unique_ptr<ParamPanel> params_;

    model::Id track_, selected_;
    model::Id anchor_;               // Shift+click extends from here
    std::set<std::string> picked_;   // a multi-selection (with selected_), else empty
    std::optional<model::Id> take_;  // set: the window shows that take's chain
    Tab tab_ = Tab::Chain;
    std::string gesture_;  // the active slider drag
    std::uint64_t gestureSerial_ = 0;
    std::string pendingChain_ = "main";  // where Add plugin… inserts
    struct Latency {
        const model::FxInstance* model = nullptr;
        host::Instance* live = nullptr;
        double rate = 0;
        std::optional<std::uint32_t> samples;
        std::string text;
    };
    std::map<std::string, Latency> latency_;
    struct NotesOut {
        std::string plugin;
        host::Instance* live = nullptr;
        bool value = false;
    };
    std::map<std::string, NotesOut> notesOut_;
    std::set<std::string> failed_;  // engine.status failed_fx
    std::string presetChain_ = "main";
    bool chainMenuOpen_ = false;  // the Load chain… menu is showing
    bool trackMenuOpen_ = false;  // the title's track menu is showing
    ui::Button* presetChainButton_ = nullptr;
    ui::Node* savePrompt_ = nullptr;
    ui::TextField* saveName_ = nullptr;
    bool presetMenuOpen_ = false;
    model::Id presetFx_;         // the effect the preset name field saves
    std::string presetReplace_;  // a name that exists: saving it again replaces it
    ui::Node* presetPrompt_ = nullptr;
    ui::TextField* presetName_ = nullptr;
    ui::Node* page_ = nullptr;
    ui::Node* body_ = nullptr;
    ui::Node* tabsRow_ = nullptr;
    ui::Button* title_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::Label* touched_ = nullptr;          // the last touched parameter
    std::vector<RackDevice*> devices_;      // in body_
    racks::Watchers watchers_;              // their Macro modules
    racks::PluginParamCache pluginParams_;  // the names of plug-in parameters they drive
    std::optional<std::chrono::steady_clock::time_point> nextFollow_;
    std::uint64_t touchedSerial_ = 0;
    std::uint64_t shownVersion_ = ~std::uint64_t{0};
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
};

}  // namespace supple::gui
