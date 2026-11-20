// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The launcher header's Capture toggle (gui.md §4.15; ADR-0097, step L4). It
// behaves as recording does: armed while lit, the engine keeps the launches
// while the transport runs (launch.capture), and when the transport stops or
// relocates, or the toggle goes off, the performance is written onto the
// timeline in one undo step (launch.commit), as a recording is committed at
// stop. A refusal goes to onError (the status line).
#pragma once

#include <functional>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"

namespace supple::gui {

class CaptureControl {
  public:
    CaptureControl(engine::EngineApp& app, cmd::Dispatcher& dispatcher);
    bool armed() const { return armed_; }
    // On arms it (launch.capture {on: true}: from the position heard now, or
    // where playing starts); off ends the capture and commits what it holds.
    Status set(bool on);
    // Once a pass: a capture the engine ended (a stop, a seek, a loop change)
    // is committed and the toggle, still lit, arms the next one. Without a
    // running engine it goes dark.
    void service();
    // A commit's or a re-arm's refusal, from service().
    std::function<void(const Error&)> onError;

  private:
    // Ends the capture and commits it when it holds something to write: a
    // track that left the arrangement (no launch at all writes nothing).
    Status commit();
    Result<json::Value> run(const std::string& command, json::Value args);
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    bool armed_ = false;
    bool capturing_ = false;  // the engine's, at the last look
};

}  // namespace supple::gui
