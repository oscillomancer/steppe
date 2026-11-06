// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The MIDI learn dialog (gui.md §4.5, §6; ADR-0027): "Move the MIDI
// controller you wish to assign". It shows the control it heard last, its
// CC mode and options; OK binds it, Remove unbinds what was bound. Opened
// from the actions list (Ctrl+L), from a parameter's "MIDI" button in the
// FX window, and from a right-click on a strip's or a send's control. While it is open the devices'
// messages come to it, and neither actions nor learned parameters move.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "app_actions.h"
#include "supple/model/objects.h"
#include "supple/ui/root.h"

namespace supple::gui {

// The binding chosen, and for a plug-in parameter its conditions.
struct LearnChoice : model::ControlBinding {
    bool whenSelected = false, whenFocused = false, whenVisible = false;
    LearnChoice() = default;
    explicit LearnChoice(const model::ControlBinding& binding) : ControlBinding(binding) {}
};

// A *.learn.set command's midi, mode and soft_takeover for the choice, and
// with `conditions` its when_selected, when_focused and when_visible.
json::Value learnArgs(const LearnChoice& choice, bool conditions = false);

struct LearnRequest {
    std::string title;                   // what is bound: an action's or a parameter's name
    std::optional<LearnChoice> current;  // its binding now: Remove is offered
    bool param = false;                  // a plug-in parameter: soft takeover, enable only when …
    bool takeover = false;  // a strip's or send's level or pan, or a knob's action: soft takeover
    // A CC's mode when nothing is bound yet (a button's: toggle).
    model::ControlMode mode = model::ControlMode::Absolute;
    // An action that runs once per press: a knob heard here gets a warning
    // that its position is not passed on, naming `alternative` (the "(MIDI
    // CC)" action that follows one) when there is one.
    bool pressOnly = false;
    std::string alternative;
    std::function<void(const LearnChoice&)> onOk;
    std::function<void()> onRemove;
};

void openLearnDialog(ui::Root& root, AppActions& actions, engine::EngineApp& app,
                     LearnRequest request);

}  // namespace supple::gui
