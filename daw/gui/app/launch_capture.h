// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The launcher header's Capture toggle (gui.md §4.15; ADR-0097, step L4). It
// behaves as recording does: armed while lit, the engine keeps the launches
// while the transport runs (launch.capture), and when the transport stops or
// relocates, or the toggle goes off, the performance is written onto the
// timeline in one undo step (launch.commit), as a recording is committed at
// stop. Turned off while playing, the capture ends on the next boundary of
// the project's launch quantize (launch.capture {on: false, quantize}), as a
// scene launch made then would land, and is committed once the engine has
// heard it.
// A refusal goes to onError (the status line).
//
// A refused commit (a locked item, a clip loop with no item equivalent, a
// track or clip deleted meanwhile) keeps the performance: the engine holds it,
// the toggle goes dark, and the next click on it commits it first, once the
// user has fixed what the refusal names, and then arms. When that commit is
// refused again, the toggle stays dark and says so; one more click drops the
// performance and arms. Opening another project drops it too.
//
// A take the engine ended (an engine stop, Restart audio, a device restart;
// the app keeps it, ADR-0097) is committed by the next pass as a stop's is,
// and nothing arms by itself after it. What is uncommitted is the app's
// answer (EngineApp::captureUncommitted, launchCapture), never this class's
// copy; a take whose commit the user undid is declined: a click arms over it,
// and it is kept (redo restores it) until the new capture hears a frame.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

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
    // A take is kept uncommitted, and not declined (its commit undone): a
    // refused one waiting for another try, or one another caller's capture
    // left. What the header's cell shows dark with its dot in outline.
    bool kept();
    // That take in the pane's words, for the cell's hint (empty: none): its
    // bars and launches, why it is kept, and what a click does.
    std::string keptLine();
    // On commits a kept performance first, then arms (launch.capture {on:
    // true}: from the position heard now, or where playing starts; before a
    // pending end, it cancels it); off ends the capture and commits what it
    // holds, also one another caller armed: while playing at the next
    // boundary of the project's launch quantize (ending() until then, still
    // lit), stopped at once. Off again while ending (the cell's click, lit)
    // ends it at once, also an end another caller asked for.
    Status set(bool on);
    // A quantized end is pending: the toggle's own (the capture runs to its
    // boundary, then service() commits it) or another caller's
    // (launch.capture {on: false, quantize}, which that caller commits).
    bool ending() const { return ending_ || app_.launchCaptureEnding(); }
    // Once a pass: a capture the engine ended (a stop, a seek, a loop change)
    // is committed and the toggle, still lit, arms the next one; a refused
    // commit keeps the performance and the toggle goes dark. An ending
    // capture that has reached its boundary (or a relocation) is committed,
    // and nothing is armed. A take an engine stop or a device restart ended
    // (the app's kept take) is committed, said through onNotice, and the
    // toggle goes dark: nothing arms by itself after it.
    void service();
    // Before the project is replaced or the app quits (FileActions'
    // unsaved-work question): ends a running or ending capture now and
    // commits whatever take is uncommitted, arming nothing, as Stop
    // finalizes a recording. A refusal keeps the take (kept() stays true)
    // and is returned. The one place where a question could take the place
    // of the commit.
    Status finalize();
    // A kept take for the question: "The launcher take (bars 3–18,
    // 2 launches) could not be committed: …"; empty when none.
    std::string lostLine();
    // Changes when the kept take does (another take, committed, dropped), so
    // a decision about one take is not applied to another.
    std::string takeKey();
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
    // The take the engine ended while this pane armed or was ending it:
    // committed, a refusal kept and sent to onError.
    void settle();
    // "bars 3–18, 2 launches", on the current time map.
    std::string takeWords(const engine::EngineApp::CaptureView& v);
    // A refused commit: the performance is kept and the toggle goes dark.
    Error keep(Error refusal);
    Result<json::Value> run(const std::string& command, json::Value args);
    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    bool armed_ = false;
    bool capturing_ = false;  // the engine's, at the last look
    bool ending_ = false;     // turned off while playing: committed at its boundary
    std::string describeKept();
    struct KeptKey {
        std::uint64_t project = 0, version = 0;
        bool engine = false, capturing = false, lit = false, warned = false;
        std::string refusal;
        bool operator==(const KeptKey&) const = default;
    };
    std::optional<KeptKey> keptKey_;
    std::string keptLine_;
    // A click's commit of the kept take was refused: the next click drops it.
    bool warned_ = false;
    // The kept take's last refusal, in the pane's words (empty: none).
    std::string refusal_;
    // Armed over a declined take: said once when the new capture replaces it.
    bool replacing_ = false;
};

}  // namespace supple::gui
