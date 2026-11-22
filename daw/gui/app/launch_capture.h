// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The launcher header's Capture toggle (gui.md §4.15; ADR-0097, step L4). It
// behaves as recording does: armed while lit, the engine keeps the launches
// while the transport runs (launch.capture), and when the transport stops or
// relocates, or the toggle goes off, the performance is written onto the
// timeline in one undo step (launch.commit), as a recording is committed at
// stop. Turned off while playing, the capture ends on the next boundary of
// the project's launch quantize (launch.capture {on: false, quantize}), as a
// launch made then would land, and is committed once the engine has heard it.
// A refusal goes to onError (the status line).
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
    // What the header's cell shows: lit while the toggle is armed or the
    // engine captures, whoever armed it (an agent's launch.capture too).
    bool lit() const { return armed_ || app_.launchCapturing(); }
    // A performance whose commit was refused waits for another try.
    bool kept() const { return kept_ != Kept::None; }
    // On commits a kept performance first, then arms (launch.capture {on:
    // true}: from the position heard now, or where playing starts; before a
    // pending end, it cancels it); off ends the capture and commits what it
    // holds, also one another caller armed: while playing at the next
    // boundary of the project's launch quantize (ending() until then, still
    // lit), stopped at once.
    Status set(bool on);
    // Turned off while playing: the capture runs to its boundary, then
    // service() commits it.
    bool ending() const { return ending_; }
    // Once a pass: a capture the engine ended (a stop, a seek, a loop change)
    // is committed and the toggle, still lit, arms the next one; a refused
    // commit keeps the performance and the toggle goes dark. An ending
    // capture that has reached its boundary (or a relocation) is committed,
    // and nothing is armed. Without a running engine it goes dark.
    void service();
    // A commit's or a re-arm's refusal, from service().
    std::function<void(const Error&)> onError;
    // What a commit did with its clips' keys (views::followNotice: the clips
    // copied under another key), for the status line; not called when there
    // is nothing to say.
    std::function<void(const std::string&)> onNotice;

  private:
    // Ends the capture and commits it when it holds something to write: a
    // track that left the arrangement (no launch at all writes nothing).
    // Committed while stopped, the tracks return to the arrangement
    // (launch.arrangement, at the play start).
    Status commit();
    // A refused commit: the performance is kept and the toggle goes dark.
    Error keep(Error refusal);
    Result<json::Value> run(const std::string& command, json::Value args);
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    bool armed_ = false;
    bool capturing_ = false;  // the engine's, at the last look
    bool ending_ = false;     // turned off while playing: committed at its boundary
    // Refused once, or refused again after a click (the next click drops it).
    enum class Kept { None, Refused, Warned } kept_ = Kept::None;
    std::uint64_t keptGeneration_ = 0;  // the project it belongs to
};

}  // namespace supple::gui
