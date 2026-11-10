// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI Time Code: a generator of quarter-frame (F1) and full-frame SysEx
// messages, and a receiver that assembles them back into timecode (spec:
// docs/specs/sync.md §4, ADR-0070 §5-6).
//
// Quarter frames: eight messages per two frames, piece p = 0..7 carrying
// frames low/high, seconds low/high, minutes low/high, hours low, and hours
// high with the rate code (0 = 24, 1 = 25, 2 = 29.97 drop, 3 = 30). A
// sequence starts on an even frame F, which it names, and piece p goes out
// at F + p/4 frames: the receiver, which knows F only at piece 7 (F + 1.75),
// counts each piece as F + p/4. Backwards, pieces run 7..0 with the same
// meaning. The full frame F0 7F 7F 01 01 hh mm ss ff F7 (rate in hh's bits
// 5-6) announces a locate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "supple/base/rt.h"
#include "supple/sync/common.h"
#include "supple/sync/dll.h"
#include "supple/sync/timecode.h"

namespace supple::sync {

// 0 = 24 (and 23.976), 1 = 25, 2 = 29.97 drop, 3 = 30 (and 29.97 non-drop).
int mtcRateCode(FrameRate rate) SUPPLE_RT;

class MtcGenerator {
  public:
    // Timeline seconds map to timecode through `mapping` (rate and start).
    explicit MtcGenerator(TimecodeMapping mapping) : mapping_(mapping) {}
    const TimecodeMapping& mapping() const SUPPLE_RT { return mapping_; }

    // Transport starts at `frame` (`seconds` maps device frames to timeline
    // seconds): a full frame naming the frame playing there, then quarter
    // frames from the next even frame boundary. Already running: locate().
    void start(double frame, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT;
    // A jump (locate, loop wrap): a full frame; quarter frames resume from
    // the next even frame boundary when running.
    void locate(double frame, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT;
    // Quarter frames stop; no message.
    void stop() SUPPLE_RT { running_ = false; }
    // Quarter frames inside [from, to), each at its exact frame.
    void run(double from, double to, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT;

    bool running() const SUPPLE_RT { return running_; }
    // Quarter frames since 00:00:00:00 of the next message (frames * 4 + piece).
    std::int64_t nextQuarter() const SUPPLE_RT { return nextQuarter_; }

    // Events a block can produce at `maxSpeed` (timeline seconds per second,
    // the highest playrate), plus a full frame.
    static std::size_t maxEventsPerBlock(double frames, double sampleRate, FrameRate rate,
                                         double maxSpeed) SUPPLE_RT;

  private:
    void fullFrame(double frame, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT;
    TimecodeMapping mapping_;
    bool running_ = false;
    std::int64_t nextQuarter_ = 0;
};

struct MtcReceiverConfig {
    double sampleRate = 48000.0;
    // The project's rate, when the user set one. MTC codes 23.976 as 24 and
    // 29.97 non-drop as 30; with no expected rate the measured quarter-frame
    // rate (a least-squares fit over two seconds of lock) decides, once, until
    // the signal is lost.
    std::optional<FrameRate> expected{};
    // Quarter-frame timing loop bandwidth (Hz), for the measured rate and the
    // observations' filtered times.
    double bandwidthHz = 1.0;
    // No quarter frame for this long: signal lost (not present, not locked,
    // position unknown until the next complete sequence).
    double timeoutSeconds = 0.15;
    // Consistent complete sequences before locked() (2: four frames).
    int lockSequences = 2;
};

class MtcReceiver {
  public:
    explicit MtcReceiver(MtcReceiverConfig config = {}) : config_(config) {}

    // Feeds one complete message at `frame`: F1 dd, or a full-frame SysEx
    // F0 7F <device> 01 01 hh mm ss ff F7 (any device id). Returns true when
    // the message produced a new observation (a quarter frame once the
    // position is known).
    bool feed(double frame, std::span<const std::uint8_t> message) SUPPLE_RT;
    // Checks the dropout timeout at `frame` (call once per block).
    void update(double frame) SUPPLE_RT;
    void reset() SUPPLE_RT;

    bool present() const SUPPLE_RT { return present_; }
    bool locked() const SUPPLE_RT { return locked_; }
    // True once a complete sequence or a full frame gave a position.
    bool known() const SUPPLE_RT { return known_; }
    // +1 forward, -1 backward, 0 unknown or stopped.
    int direction() const SUPPLE_RT { return direction_; }
    // The rate in use: the expected one, or the coded one refined by the
    // measured quarter-frame rate (rateMeasured()).
    FrameRate rate() const SUPPLE_RT { return rate_; }
    bool rateMeasured() const SUPPLE_RT { return rateMeasured_; }
    // Frames per second measured from the quarter frames of the current
    // run, a least-squares fit (0 before two).
    double measuredFps() const SUPPLE_RT;
    // The label of the frame playing at the latest observation.
    Timecode timecode() const SUPPLE_RT;
    // The latest quarter frame: filtered frame, timecode seconds
    // (frames since 00:00:00:00 / fps).
    Observation lastObservation() const SUPPLE_RT { return last_; }
    // Counts full frames and discontinuities in the quarter-frame count.
    std::uint32_t jumps() const SUPPLE_RT { return jumps_; }

  private:
    void quarterFrame(double frame, std::uint8_t data, bool& observed) SUPPLE_RT;
    void lose() SUPPLE_RT;
    void restartRun() SUPPLE_RT;
    double secondsOf(std::int64_t quarter) const SUPPLE_RT;
    MtcReceiverConfig config_;
    PeriodDll dll_;
    LineFit fit_;
    std::int64_t runSteps_ = 0;
    double runStart_ = 0.0;
    FrameRate rate_ = FrameRate::Fps25;
    bool rateMeasured_ = false;
    int refinedCode_ = -1;  // the code rateMeasured_ refined, to refinedRate_
    FrameRate refinedRate_ = FrameRate::Fps25;
    bool present_ = false;
    bool locked_ = false;
    bool known_ = false;
    int direction_ = 0;
    int lastPiece_ = -1;
    unsigned mask_ = 0;
    std::uint8_t nibbles_[8]{};
    std::int64_t quarter_ = 0;  // quarter frames since 00:00:00:00 at the latest piece
    int consistent_ = 0;
    double lastFrame_ = 0.0;
    double lockedSince_ = 0.0;
    Observation last_{};
    std::uint32_t jumps_ = 0;
};

}  // namespace supple::sync
