// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The parameter menu (gui.md §4.5; ADR-0058 and ADR-0066 notes): a
// right-click on a strip's fader, pan or button, in the track panel or the
// mixer, or on a VCA's fader, mute or solo opens one menu, the same in both
// places:
// - MIDI learn… for the control, naming what is bound now (a pan has one
//   entry per bar of its mode, and one for its gain compensation);
// - Show or Hide lane where the parameter can be automated (a new lane starts
//   flat at the control's value);
// - Reset where the control has a default: a fader's 0 dB, a pan's centre
//   (and full width, or full left and right in dual pan).
// A channel's or buss's pan then adds its mode, its law and gain
// compensation; a master's pan is a balance and has none of those. Modulate
// joins the menu with modulation.
#pragma once

#include <string>
#include <string_view>

#include "app_actions.h"
#include "supple/model/ids.h"
#include "supple/model/session.h"
#include "supple/ui/root.h"

namespace supple::gui {

// `object`: the track (tr_*) or the VCA's group (gr_*); `param`: the
// control as its `learn` names it (volume, pan, mute, solo, …).
void openParamMenu(ui::Root& root, AppActions& actions, const model::Id& object,
                   const std::string& param, const ui::Rect& anchor);

// The value a new envelope on a strip's or VCA's parameter starts flat at:
// the control's value now.
double envelopeStartValue(const model::Session& s, const model::Id& object, std::string_view param);

}  // namespace supple::gui
