// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Actions bound to a MIDI controller (gui.md §6, ADR-0027): the user's
// MIDI learn for the actions list, kept beside the keymap in
// `<config>/<slug>/controls.json` (supple.controls/1.0). A control drives
// one action; an action may have several controls.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/objects.h"

namespace supple::gui {

// Soft takeover applies to an action that sets a value from a knob's
// position (Action::takeover).
struct ActionControl : model::ControlBinding {
    std::string action;
    bool operator==(const ActionControl&) const = default;
};

class ControlMap {
  public:
    static std::filesystem::path defaultFile(std::string_view slug);
    // Absent: none. A file of another schema is an error (the caller keeps
    // an empty map and leaves the file alone).
    static Result<ControlMap> load(const std::filesystem::path& file);
    Status save(const std::filesystem::path& file) const;

    // Replaces whatever that control drove; returns the action it had, if any.
    std::string bind(ActionControl binding);
    void unbindAction(std::string_view action);  // every control of the action
    const std::vector<ActionControl>& bindings() const { return bindings_; }
    std::vector<ActionControl> bindingsFor(std::string_view action) const;
    bool empty() const { return bindings_.empty(); }

  private:
    std::vector<ActionControl> bindings_;
};

}  // namespace supple::gui
