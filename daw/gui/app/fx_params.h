// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The semantic parameter panel (docs/specs/gui.md §4.5): a tab in the FX
// window, generated from the plugin's semantic schema through `fx.describe`
// (the same names, units, ranges and groups an agent reads). Edits go through
// `fx.param.set`; a slider drag is one undo step (Origin::gesture). A
// right-click on a parameter's row opens its parameter menu: MIDI learn…,
// Modulate… (greyed unless fx.describe marks it modulatable, ADR-0096) and
// Show or Hide lane. A plug-in without semantic parameters lists its plain
// CLAP parameters: read-only, except a modulatable one, which gets a slider
// over its declared range and the menu without a lane.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "learn_dialog.h"
#include "modulation_status.h"
#include "supple/engine/app.h"
#include "supple/ui/controls.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
}

namespace supple::gui {

class ParamPanel {
  public:
    ParamPanel(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& root);

    // Builds the rows for `fx` under `parent` (a column). Returns how many
    // parameters are shown; an error is returned to display in the panel.
    Result<std::size_t> build(ui::Node& parent, model::Id fx);
    // The modulatable sliders' live values (telemetry, ADR-0096 M2): call
    // each pass; redraws only when they move.
    void tickLive();
    // Errors from edits (shown by the window).
    std::function<void(const std::string&)> onError;
    // Opens the MIDI learn dialog (each row's "MIDI" button; unset: no button).
    std::function<void(LearnRequest)> openLearn;
    // "Show modulation source" and "Remove modulation" rows for a
    // parameter (FxWindow::Hooks).
    std::function<std::vector<ui::MenuItem>(const model::EnvTarget&)> sourceItems;

  private:
    // A row's MIDI learn button: the bound control, or "MIDI".
    void learnButton(ui::Node& row, const std::string& id, const std::string& param,
                     const std::string& title);
    void learn(const std::string& param, const std::string& title);
    std::map<std::string, LearnChoice> learned_;  // by parameter, for the shown effect
    void setValue(const std::string& key, json::Value value, bool gesture);
    // Shows the parameter's automation lane, creating the envelope (flat at
    // the current value) when it has none (P5.14).
    Status automate(const std::string& key, double physical);
    // A modulatable slider's routes (ADR-0096), and a depth drag on its bar
    // (modulation.set; a drag is one undo step).
    std::optional<ui::ModulationView> modulationView(const std::string& key);
    void setDepth(const std::string& key, double depth, ui::Gesture g);
    // A row's right-click menu at `at` (window coordinates). `lane`: offer
    // Show or Hide lane (a plain parameter has no lane).
    void openMenu(const std::string& key, const std::string& name, bool learnable, bool modulatable,
                  double physical, ui::Point at, bool lane = true);

    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    model::Id fx_;
    std::string gesture_, depthGesture_;
    std::vector<std::string> modulatable_;  // the shown modulatable parameters
    LiveModulation liveModulation_;         // the live offsets, inert routes left out
    std::map<model::Id, double> live_;      // the telemetry they show
    std::uint64_t gestureSerial_ = 0;
};

}  // namespace supple::gui
