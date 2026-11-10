// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI learn on a strip's, a send's or a VCA's own controls (gui.md §4.5,
// ADR-0058): a right-click on a track's fader, pan or button (the TCP, the
// mixer), on a VCA's fader, mute or solo (the mixer) or on a control of the
// routing matrix offers "MIDI learn…", which opens the learn dialog. The
// binding is the project's (track/route/vca `learn`), undoable.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app_actions.h"
#include "supple/model/ids.h"
#include "supple/model/session.h"
#include "supple/ui/root.h"

namespace supple::gui {

// The control to bind: tracks, sends or VCAs (their groups' ids; bound alike:
// the matrix binds every selected send), and the control as their `learn`
// names it.
struct StripControl {
    std::vector<model::Id> objects;
    std::string param;  // volume, pan, mute, solo, … ; tap:pre_fader on a send
};

// The control's name for people: "Volume", "Pre-FX tap", …
std::string stripControlLabel(std::string_view param);

// Its menu entry: "MIDI learn: Pan…", naming what is bound now.
std::string stripLearnLabel(const model::Session& s, const StripControl& control);

// The control's right-click menu at `anchor`: "MIDI learn…" (naming what is
// bound now). OK in the dialog binds every object in one undo step; Remove
// unbinds those bound.
void openStripLearnMenu(ui::Root& root, AppActions& actions, StripControl control,
                        const ui::Rect& anchor);

// The same dialog without the menu (the menu's entry, and tests).
void openStripLearn(ui::Root& root, AppActions& actions, StripControl control);

}  // namespace supple::gui
