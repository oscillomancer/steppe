// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A rack's knobs outside its own space (gui.md §4.12): the Macro modules'
// cabled knobs a channel's Rack device shows (named after what they drive),
// the menu any rack knob offers (MIDI learn…, Show lane, Reset), and the
// telemetry subscriptions several views share.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "learn_dialog.h"
#include "rack_engine.h"
#include "supple/base/result.h"
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
// parameter a cable reaches (fx, key), as the plug-in writes it; else the
// cable's key is used.
std::vector<MacroGroup> macroGroups(
    const model::Session& s, const rack::Catalog& catalog, model::Id rack,
    const std::function<std::string(model::Id fx, const std::string& key)>& pluginParamName = {});

// The racks whose Outs feed `track` (its record input), in route order.
std::vector<model::Id> racksFeeding(const model::Session& s, model::Id track);

// The racks a strip's mod slots list (ADR-0096, M3): those homed on `track`
// (Rack::track) that hold a Mod Out module, in rack order. A rack is a
// modulator by holding a Mod Out, whatever its name ("Modulators" is only
// the default `modulate` gives a new one).
std::vector<model::Id> modRacks(const model::Session& s, model::Id track);

// Whether a Mod Out input carries 0..1 (ADR-0096, "Range"): it has a cable
// and every cable into it comes from a unipolar, gate or phase output. An
// unpatched input, or one fed by any other output, carries -1..1.
bool unipolarSource(const model::Session& s, const rack::Catalog& catalog,
                    const model::ModSource& source);

// A knob's right-click rows, in the strips' parameter menu's order and
// wording (gui.md §4.2): MIDI learn… (and Remove MIDI learn), Show or Hide
// lane, Reset to the default, then a choice's values. `current`: its value
// now, physical.
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

// The commands a rack knob outside the rack window issues: a drag, Reset
// or a choice (module.set), MIDI learn (module.learn.set, .remove) and its
// lane (env.add, env.set).
inline constexpr std::array<std::string_view, 5> kKnobCommands{
    "module.set", "module.learn.set", "module.learn.remove", "env.add", "env.set"};
// The status line's words for `e`, one of kKnobCommands' refusals (gui.md
// §4.5, "Rack devices"): what was being done, then why, from the command,
// the code and the argument it is refused at (a modulation cycle by
// views::modulationRefusalWords), never the engine's message, its hint, a
// command's name or an id. A code with no words here says what was being
// done and the code: "The knob could not be set (schema.type)." `s` is the
// session the command was refused on.
std::string knobRefusalWords(const model::Session& s, std::string_view command, const Error& e);

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
