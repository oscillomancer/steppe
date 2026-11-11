// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Project > Timecode… (gui.md §4.8): the project's timecode frame rate and
// start, the timecode at project time 0 (ADR-0071). Each change is one
// project.set {timecode} edit, undoable like any project setting. MTC and
// LTC sync and LTC output use it.
#pragma once

#include <functional>
#include <string>

#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::gui {

class TimecodeDialog {
  public:
    TimecodeDialog(ui::Root& root, cmd::Dispatcher& dispatcher, engine::EngineApp& app);
    void open();
    bool isOpen() const;
    // Open dialog only: shows the project's current settings (an undo, an
    // agent's edit); also called by the GUI tick.
    void refresh();
    // As the dialog's controls do (tests): one project.set each.
    Status setRate(const std::string& rate);
    Status setStart(const std::string& start);
    std::function<void(const std::string&)> onError;

  private:
    void build();
    void show(const std::string& error);
    ui::Root& root_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    std::string shown_;  // the settings the controls show ("rate start")
};

}  // namespace supple::gui
