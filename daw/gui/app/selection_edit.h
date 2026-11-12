// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A strip control used on a selected track acts on the whole track selection
// (gui.md §4.2), in the TCP and the mixer alike: the same mute, solo, arm,
// monitoring or input on each (arm, monitoring and input on channels only), and
// a fader or pan drag that moves each one by the same amount. One click or one
// drag is one undo step.
#pragma once

#include <map>
#include <string>
#include <utility>

#include "app_actions.h"

namespace supple::gui {

class SelectionEdit {
  public:
    SelectionEdit(cmd::Dispatcher& dispatcher, engine::EngineApp& app, UiState& ui)
        : dispatcher_(dispatcher), app_(app), ui_(ui) {}
    // A track.set from a strip ({track, field: value}); `gesture` names a drag
    // (empty: a single edit). A drag's level and pan are relative to where each
    // track was when it began; a single edit sets the same value everywhere.
    Result<json::Value> set(json::Value args, const std::string& gesture);
    // A successful multi-selection input choice that replaced rack Outs.
    const std::string& inputReplacement() const { return inputReplacement_; }

  private:
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    std::string inputReplacement_;
    std::string gesture_;                        // the drag start_ belongs to
    std::map<std::string, model::Track> start_;  // controls at gesture start
};

}  // namespace supple::gui
