// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The launcher header's Capture toggle (gui.md §4.15; ADR-0097, step L4). It
// behaves as recording does: armed while lit, the engine keeps the launches
// while the transport runs (launch.capture), and when the transport stops or
// relocates, or the toggle goes off, the performance is written onto the
// timeline in one undo step (launch.commit), as a recording is committed at
// stop. A refusal goes to onError (the status line).
//
// A refused commit (a locked item, a clip loop with no item equivalent, a
// track or clip deleted meanwhile) keeps the performance: the engine holds it,
// the toggle goes dark, and the next click on it commits it first, once the
// user has fixed what the refusal names, and then arms. When that commit is
// refused again, the toggle stays dark and says so; one more click drops the
// performance and arms. Opening another project drops it too.
#pragma once

#include <cstdint>
#include <functional>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"

namespace supple::gui {

class CaptureControl {
  public:
    CaptureControl(engine::EngineApp& app, cmd::Dispatcher& dispatcher);
    bool armed() const { return armed_; }
    // A performance whose commit was refused waits for another try.
    bool kept() const { return kept_ != Kept::None; }
    // On commits a kept performance first, then arms (launch.capture {on:
    // true}: from the position heard now, or where playing starts); off ends
    // the capture and commits what it holds.
    Status set(bool on);
    // Once a pass: a capture the engine ended (a stop, a seek, a loop change)
    // is committed and the toggle, still lit, arms the next one; a refused
    // commit keeps the performance and the toggle goes dark. Without a running
    // engine it goes dark.
    void service();
    // A commit's or a re-arm's refusal, from service().
    std::function<void(const Error&)> onError;

  private:
    // Ends the capture and commits it when it holds something to write: a
    // track that left the arrangement (no launch at all writes nothing).
    Status commit();
    // A refused commit: the performance is kept and the toggle goes dark.
    Error keep(Error refusal);
    Result<json::Value> run(const std::string& command, json::Value args);
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    bool armed_ = false;
    bool capturing_ = false;  // the engine's, at the last look
    // Refused once, or refused again after a click (the next click drops it).
    enum class Kept { None, Refused, Warned } kept_ = Kept::None;
    std::uint64_t keptGeneration_ = 0;  // the project it belongs to
};

}  // namespace supple::gui
