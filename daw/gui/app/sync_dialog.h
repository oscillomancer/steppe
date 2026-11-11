// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Sync settings… (the tempo button's right-click menu; gui.md §4.13,
// ADR-0071): the external sync source and its input (a MIDI port, or an
// audio input channel for LTC), follow tempo, the offset, what a signal
// loss does, LTC output, and the project's timecode. Each change is one
// sync.set and is saved with the preferences; an agent's sync.set is saved
// too (watch()).
#pragma once

#include <functional>
#include <string>

#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::gui {

class SyncDialog {
  public:
    using Save = std::function<Status(const engine::SyncSettings&)>;
    // `save` persists the settings (empty: not saved, as in tests).
    SyncDialog(ui::Root& root, cmd::Dispatcher& dispatcher, engine::EngineApp& app, Save save);
    void open();
    bool isOpen() const;
    // Open dialog: the status lines; any time: saves settings an agent's
    // sync.set changed. The GUI tick calls it.
    void refresh();
    // As the dialog's controls do (tests): sync.set with these members.
    Status set(const json::Value& members);
    // Opens Project > Timecode… (the dialog's timecode row).
    std::function<void()> onTimecode;
    std::function<void(const std::string&)> onError;

  private:
    void build();
    void watch();
    ui::Root& root_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    Save save_;
    engine::SyncSettings saved_;
    std::string shown_;  // the settings the controls show (JSON)
};

}  // namespace supple::gui
