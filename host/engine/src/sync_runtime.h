// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The audio callback's share of external sync (ADR-0071): receivers, the
// chase tracker and the LTC encoder. Built on the main thread for a
// configuration (it allocates the LTC buffers), then owned by one callback.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "supple/base/result.h"
#include "supple/base/rt.h"
#include "supple/engine/realtime.h"
#include "supple/engine/sync.h"
#include "supple/sync/chase.h"
#include "supple/sync/ltc.h"
#include "supple/sync/midi_clock.h"
#include "supple/sync/mtc.h"
#include "supple/time/timeline.h"

namespace supple::engine::detail {

// One audio cycle as the runtime sees it.
struct SyncCycle {
    const float* const* in = nullptr;
    std::uint32_t inputs = 0;
    float* const* out = nullptr;
    std::uint32_t outputs = 0;
    std::uint32_t frames = 0;
    std::span<const SyncMidiEvent> midi;
    std::int64_t device = 0;  // device frame of the cycle's first frame
    bool playing = false;     // the transport played (part of) this cycle
    // Timeline seconds the callback processed at the cycle's start and end.
    double timelineStart = 0.0, timelineEnd = 0.0;
    // The timeline wrapped at the loop's end to its start inside the cycle.
    bool wrapped = false;
    double loopStart = 0.0, loopEnd = 0.0;
    std::int64_t planLatency = 0;            // frames from processing to the device outputs
    std::int64_t outputLatency = 0;          // frames from processing to the listener
    std::int64_t inputLatency = 0;           // frames from the device input to the callback
    const time::TimeMap* timeMap = nullptr;  // the plan's (beats for MIDI clock)
};

class SyncRuntime {
  public:
    static Result<std::unique_ptr<SyncRuntime>> create(const SyncConfig& config, double sampleRate,
                                                       std::uint32_t id);
    const SyncConfig& config() const SUPPLE_RT { return config_; }
    // Main thread: follow tempo, offset and freewheel time, which the
    // callback reads at its next cycle (config.sameRuntime() holds).
    void adjust(const SyncConfig& config);
    // Returns true when something the main thread acts on changed (a start or
    // stop, a jump, a relocate request, a loss, a new chase rate).
    bool process(const SyncCycle& cycle) SUPPLE_RT;
    const SyncStatus& status() const SUPPLE_RT { return status_; }

  private:
    SyncRuntime(const SyncConfig& config, double sampleRate);
    void feedMidi(const SyncCycle& c) SUPPLE_RT;
    void feedLtc(const SyncCycle& c) SUPPLE_RT;
    void writeLtc(const SyncCycle& c) SUPPLE_RT;
    // The timeline the cycle processed, as spans (two at a loop wrap).
    void remember(const SyncCycle& c) SUPPLE_RT;
    // The cycle's speed in timeline seconds per frame, a wrap included.
    double slope(const SyncCycle& c) const SUPPLE_RT;
    // Local timeline seconds <-> the tracker's units (beats or timecode seconds).
    double toUnits(double seconds, const time::TimeMap* map) const SUPPLE_RT;
    double toSeconds(double units, const time::TimeMap* map) const SUPPLE_RT;

    SyncConfig config_;
    double sampleRate_;
    std::atomic<bool> follow_;
    std::atomic<double> offset_, freewheel_;
    sync::ClockReceiver clock_;
    sync::MtcReceiver mtc_;
    std::optional<sync::LtcDecoder> ltcIn_;
    std::optional<sync::LtcEncoder> ltcOut_;
    sync::ChaseTracker tracker_;
    SyncStatus status_;
    std::uint32_t receiverJumps_ = 0;
    std::int64_t lastObservation_ = INT64_MIN;
    bool everObserved_ = false;
    bool lossFinal_ = false;
    std::uint64_t clocksSinceStart_ = 0;
    double ltcEnd_ = 0.0;  // the timeline seconds LTC output reached
    // What LTC output labels: the timeline processed at each device frame,
    // read back the plan's latency later, when that audio leaves (ADR-0071
    // §9). A ring, sized on the main thread; older spans read as stopped.
    struct Span {
        std::int64_t device = 0;  // first frame
        std::int64_t frames = 0;
        double start = 0.0;  // timeline seconds at `device`
        double slope = 0.0;  // timeline seconds per frame
        bool playing = false;
    };
    std::vector<Span> spans_;
    std::size_t spanHead_ = 0, spanCount_ = 0;
    double notifiedRate_ = 1.0;
    std::int64_t notifiedAt_ = 0;
};

}  // namespace supple::engine::detail
