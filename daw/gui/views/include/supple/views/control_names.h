// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What a track's controls are called (gui.md §2, "Names"): the status line
// says the name of the one the pointer rests on. A row of the track panel
// and a mixer strip carry the same controls, so the words are in one place.
#pragma once

#include "supple/ui/node.h"

namespace supple::views::names {

inline const ui::Hint kMute{"Mute", {}};
inline const ui::Hint kLaneMute{"Mute the master's own items", {}};
inline const ui::Hint kSolo{"Solo", {}};
inline const ui::Hint kPolarity{"Invert polarity", {}};
inline const ui::Hint kMono{"Mono", {}};
inline const ui::Hint kVolume{"Volume", {}};
inline const ui::Hint kPan{"Pan", {}};
inline const ui::Hint kInput{"Record input", {}};
inline const ui::Hint kHardwareOutput{"Hardware output", {}};
inline const ui::Hint kEnvelopes{"Automation lanes", {}};
// These run an action the keymap can bind, aimed at their own track.
inline const ui::Hint kChain{"Plug-in chain", "ui.fx.chain"};
inline const ui::Hint kAddFx{"Add a plug-in", "ui.fx.add"};
inline const ui::Hint kRouting{"Output routing", "ui.routing"};

}  // namespace supple::views::names
