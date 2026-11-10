// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A channel's or buss's pan menu (gui.md §4.5, ADR-0066): a right-click on its
// pan in the TCP or the mixer chooses the pan mode (pan, dual pan, pan and
// width), the pan law (the project's, or 0 to -6 dB) and gain compensation,
// above the pan's MIDI learn. A master's pan is a balance: MIDI learn only.
#pragma once

#include "app_actions.h"
#include "supple/model/ids.h"
#include "supple/ui/controls.h"
#include "supple/ui/root.h"

namespace supple::gui {

void openPanMenu(ui::Root& root, AppActions& actions, const model::Id& track,
                 const ui::Rect& anchor);

// How a strip's pan control splits for its mode, and the values it shows.
ui::PanControl::Split panSplit(const model::Track& track);
void showPan(ui::PanControl& control, const model::Track& track);
// The track.set field a pan control's bar sets in the strip's mode.
const char* panField(const model::Track& track, int part);

}  // namespace supple::gui
