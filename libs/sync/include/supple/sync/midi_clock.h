// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI beat clock: a generator of Clock (F8), Start (FA), Continue (FB),
// Stop (FC) and Song Position Pointer (F2), and a receiver that turns them
// back into running state, position and tempo (spec: docs/specs/sync.md §3,
// ADR-0070 §3-4).
//
// Clock semantics, both directions: 24 clocks per quarter note. After Start
// the first clock is beat 0; after Continue the first clock is the Song
// Position Pointer's position (16th notes); each further clock advances
// 1/24 beat. A position between 16ths therefore starts at the next 16th:
// SPP names it, Continue goes out at once, and the first clock falls at the
// 16th's exact frame, so the receiver waits for it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "supple/base/rt.h"
#include "supple/sync/common.h"
#include "supple/sync/dll.h"

namespace supple::sync {

inline constexpr int kClocksPerBeat = 24;
inline constexpr int kClocksPerSixteenth = 6;
// The largest Song Position Pointer: 16383 sixteenths (1024 bars of 4/4).
inline constexpr int kMaxSongPosition = 16383;

class ClockGenerator {
  public:
    // Transport starts at `frame`, at map.positionAt(frame) beats. At or
    // before beat 0: Start, and the first clock at beat 0's frame. After it:
    // SPP of the next 16th (or this one, when on it) and Continue, and the
    // first clock at that 16th's frame. Already running: as locate().
    void start(double frame, const PositionMap& beats, EventBuffer& out) SUPPLE_RT;
    // A jump to map.positionAt(frame) (locate, loop wrap). Running: Stop, then
    // as start(). Stopped: SPP only, so receivers show the new position.
    void locate(double frame, const PositionMap& beats, EventBuffer& out) SUPPLE_RT;
    // Stop at `frame` (no-op when stopped).
    void stop(double frame, EventBuffer& out) SUPPLE_RT;
    // Clocks inside [from, to): one at every 1/24-beat crossing of `beats`,
    // at its exact frame. Contiguous calls emit each clock once. Nothing
    // while stopped. When `out` fills, the rest are emitted at the start of
    // the next call.
    void run(double from, double to, const PositionMap& beats, EventBuffer& out) SUPPLE_RT;

    bool running() const SUPPLE_RT { return running_; }
    // The index (24 per beat) of the next clock to send.
    std::int64_t nextClock() const SUPPLE_RT { return nextClock_; }

    // Events a block can produce: its clocks at the highest speed (beats per
    // frame: bpm * playrate / 60 / sample rate) plus Stop, SPP, Continue.
    static std::size_t maxEventsPerBlock(double frames, double maxBeatsPerFrame) SUPPLE_RT;

  private:
    void begin(double frame, const PositionMap& beats, EventBuffer& out) SUPPLE_RT;
    bool running_ = false;
    std::int64_t nextClock_ = 0;
};

struct ClockReceiverConfig {
    double sampleRate = 48000.0;
    // Tempo loop bandwidth (Hz): lower is steadier, higher follows tempo
    // changes faster. 0.3 Hz: tempo within 0.06% under +-1 ms of jitter.
    // A step that puts a clock half a period off restarts the loop, which
    // then re-acquires in a few beats (spec §3.2).
    double bandwidthHz = 0.3;
    // No clock for this long (or for 4 clock periods, if longer) means the
    // signal is lost: not present, not locked. Running state stays as the
    // last Start, Continue or Stop left it; the engine decides what a loss
    // while running means.
    double timeoutSeconds = 0.25;
    // Consecutive clocks within 25% of a period of the prediction before
    // locked() turns true (24: one beat).
    int lockClocks = 24;
    // Clocks lost while running (a gap of more than 1.5 periods, up to this
    // long) are counted from the last filtered period, so the position
    // survives a dropout; longer gaps keep the count as it was.
    double recoverSeconds = 2.0;
};

class ClockReceiver {
  public:
    explicit ClockReceiver(ClockReceiverConfig config = {}) : config_(config) {}

    // Feeds one complete message received at `frame`: F8, FA, FB, FC or
    // F2 lsb msb; anything else is ignored. Returns true when it produced a
    // new observation (a clock while running).
    bool feed(double frame, std::span<const std::uint8_t> message) SUPPLE_RT;
    // Checks the dropout timeout at `frame` (call once per block).
    void update(double frame) SUPPLE_RT;
    void reset() SUPPLE_RT;

    // After Start or Continue, until Stop.
    bool running() const SUPPLE_RT { return running_; }
    // Clocks arriving, steadily, for lockClocks clocks.
    bool locked() const SUPPLE_RT { return locked_; }
    // Any clock within the timeout.
    bool present() const SUPPLE_RT { return present_; }
    // Filtered tempo, once two clocks have arrived (0 before).
    double bpm() const SUPPLE_RT;
    // Position in beats: of the latest clock while running, extrapolated
    // from it by the filtered period (at most one clock ahead); when stopped,
    // the song position a Continue would resume from.
    double positionAt(double frame) const SUPPLE_RT;
    // The latest clock as an observation: its filtered frame and its beat.
    Observation lastObservation() const SUPPLE_RT { return last_; }
    // Counts Start, Continue and Song Position changes: a change tells the
    // chase tracker the position jumped.
    std::uint32_t jumps() const SUPPLE_RT { return jumps_; }

  private:
    void lose() SUPPLE_RT;
    ClockReceiverConfig config_;
    PeriodDll dll_;
    bool running_ = false;
    bool locked_ = false;
    bool present_ = false;
    int steady_ = 0;
    double lastClockFrame_ = 0.0;
    double heldPeriod_ = 0.0;     // the filtered period when the loop last ran
    std::int64_t nextClock_ = 0;  // index of the next clock while running
    std::int64_t current_ = -1;   // index of the latest clock, -1 before one
    Observation last_{};
    std::uint32_t jumps_ = 0;
};

}  // namespace supple::sync
