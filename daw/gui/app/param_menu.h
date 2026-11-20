// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The parameter menu (gui.md §4.5; ADR-0058 and ADR-0066 notes): a
// right-click on a strip's fader, pan or button, in the track panel or the
// mixer, or on a VCA's fader, mute or solo opens one menu, the same in both
// places:
// - MIDI learn… for the control, naming what is bound now (a pan has one
//   entry per bar of its mode, and one for its gain compensation);
// - Show or Hide lane where the parameter can be automated, as the lanes
//   decide it (gui.md §4.3): through the envelope's `visible` and the
//   modulation routes' `lane` (a new lane starts flat at the control's value);
// - Reset where the control has a default: a fader's 0 dB, a pan's centre
//   (and full width, or full left and right in dual pan).
// - Modulate: Volume… beside each lane (ADR-0096, M-k), opening the sources
//   `modulate` makes; greyed where the parameter takes no modulation (a
//   switch, a dual pan's bars, a VCA, a frozen track).
// A channel's or buss's pan then adds its mode, its law and gain
// compensation; a master's pan is a balance and has none of those.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "app_actions.h"
#include "supple/model/ids.h"
#include "supple/model/session.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"

namespace supple::gui {

// `object`: the track (tr_*) or the VCA's group (gr_*); `param`: the
// control as its `learn` names it (volume, pan, mute, solo, …).
void openParamMenu(ui::Root& root, AppActions& actions, const model::Id& object,
                   const std::string& param, const ui::Rect& anchor);

// The value a new envelope on a strip's or VCA's parameter starts flat at:
// the control's value now.
double envelopeStartValue(const model::Session& s, const model::Id& object, std::string_view param);

// Whether a target's lane is on screen (gui.md §4.3): its envelope is
// visible, or one of its modulation routes has `lane`.
bool laneShown(const model::Session& s, const model::EnvTarget& target);
// Shows or hides a target's lane in one undo step through whatever holds it.
// Hiding clears the envelope's `visible` and every route's `lane`. Showing
// sets them on the envelope and routes there are; with neither it adds an
// envelope flat at `startValue` (`envArgs`: env.add's target fields, such
// as {"track": "tr_1"} or {"fx": "fx_1"}).
// Modulate…: a menu at `anchor` of the sources `modulate` makes for
// `target` (its {"track" | "fx" | "route", "param"}): LFO, Random, Macro,
// then "Follower: <name>" for each channel and buss but `ownTrack` (a
// follower cannot hear the track it modulates). One undo step each.
void openModulateMenu(ui::Root& root, cmd::Dispatcher& dispatcher, const model::Session& s,
                      json::Value target, std::optional<model::Id> ownTrack, const ui::Rect& anchor,
                      std::function<void(const std::string&)> onError);
// The row that opens it: "Modulate: <name>…", greyed when not `modulatable`.
// `head` gives the session when the row runs.
ui::MenuItem modulateItem(ui::Root& root, cmd::Dispatcher& dispatcher,
                          std::function<model::Session()> head, const std::string& name,
                          json::Value target, std::optional<model::Id> ownTrack, bool modulatable,
                          const ui::Rect& anchor, std::function<void(const std::string&)> onError);

Status setLaneShown(cmd::Dispatcher& dispatcher, const model::Session& s,
                    const model::EnvTarget& target, bool shown, json::Value envArgs,
                    double startValue);

}  // namespace supple::gui
