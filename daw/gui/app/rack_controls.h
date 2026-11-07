// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A rack's knobs outside its own space (gui.md §4.12): the Macro modules'
// cabled knobs a channel's Rack device shows (named after what they drive),
// the menu any rack knob offers (Default, Automate, MIDI learn), and the
// telemetry subscriptions several views share.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "learn_dialog.h"
#include "rack_engine.h"
#include "supple/engine/app.h"
#include "supple/rack/module_type.h"
#include "supple/ui/popup.h"

namespace supple::gui::racks {

// The Macro module (modules/stock/macro.cpp): eight knobs, each sent out of
// its own output (macro_N out of out_N).
inline constexpr std::string_view kMacroModule = "com.suppleaudio.rack.macro";

// One Macro knob whose output is cabled.
struct MacroKnob {
    int param = -1;   // its index among the Macro type's parameters
    std::string key;  // "macro_3"
    // What it drives, for people: the first cable's far end ("Filter
    // cutoff"), then "+N" for the other cables from the same output.
    std::string name;
};
struct MacroGroup {
    model::Id module;
    std::string title;  // the Macro module's
    std::vector<MacroKnob> knobs;
};
// The Macro modules of `rack` (in module order) with their cabled knobs;
// Macro modules with none are left out. `pluginParamName` names a plug-in
// parameter a cable reaches (fx, key), else the key is used.
std::vector<MacroGroup> macroGroups(
    const model::Session& s, const rack::Catalog& catalog, model::Id rack,
    const std::function<std::string(model::Id fx, const std::string& key)>& pluginParamName = {});

// The racks whose Outs feed `track` (its record input), in route order.
std::vector<model::Id> racksFeeding(const model::Session& s, model::Id track);

// A knob's right-click rows: Default, Automate (or Show/Hide automation
// lane), MIDI learn… (and Remove MIDI learn) for a module's parameter, then
// a choice's values. `current`: its value now, physical.
struct KnobMenuHooks {
    std::function<void(const std::string& command, json::Value args)> exec;
    std::function<void(double physical)> set;  // a one-shot change of the knob
    std::function<void()> learn;               // opens the learn dialog
};
std::vector<ui::MenuItem> knobMenu(const model::Session& s, model::Id module,
                                   const sd::ParamModel& param, double current,
                                   KnobMenuHooks hooks);
// The learn dialog's request for a module's parameter (module.learn.set).
LearnRequest knobLearn(const model::Session& s, model::Id module, const std::string& moduleTitle,
                       const sd::ParamModel& param,
                       std::function<void(const std::string& command, json::Value args)> exec);

// Telemetry subscriptions (rack::Instance::watch) that several views share:
// each view says which modules it reads now, and a module is watched while
// any view reads it. Main thread.
class Watchers {
  public:
    explicit Watchers(engine::EngineApp& app);
    ~Watchers();
    Watchers(const Watchers&) = delete;
    Watchers& operator=(const Watchers&) = delete;
    // What this view reads from now on (an instance made since is watched too).
    void set(std::vector<model::Id> modules);
    const std::vector<model::Id>& modules() const { return mine_; }

  private:
    engine::EngineApp& app_;
    std::vector<model::Id> mine_;
};

}  // namespace supple::gui::racks
