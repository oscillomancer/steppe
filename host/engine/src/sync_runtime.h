// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The audio callback's share of external sync (ADR-0071): receivers, the
// chase tracker, the LTC encoder, and MIDI clock and MTC output (ADR-0073).
// Built on the main thread for a configuration (it allocates the LTC
// buffers and the timeline history), then owned by one callback.
#pragma once

#include <array>
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
    bool loopActive = false;  // the transport loops (output predicted ahead wraps too)
    double loopStart = 0.0, loopEnd = 0.0;
    std::int64_t planLatency = 0;            // frames from processing to the device outputs
    std::int64_t outputLatency = 0;          // frames from processing to the listener
    std::int64_t inputLatency = 0;           // frames from the device input to the callback
    const time::TimeMap* timeMap = nullptr;  // the plan's (beats for MIDI clock)
    // MIDI clock and MTC output: the device, and each port's counts.
    Backend* midiOut = nullptr;
    std::atomic<std::uint64_t>* outSent = nullptr;     // kMaxSyncOutputs each
    std::atomic<std::uint64_t>* outDropped = nullptr;  // likewise
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
    // The runtime is leaving (replaced, or the engine stops): Stop at frame 0
    // of the current callback on every port whose transport runs, so no
    // slave is left playing; it sends nothing after (ADR-0073 §9).
    void farewell(Backend* out, std::atomic<std::uint64_t>* sent,
                  std::atomic<std::uint64_t>* dropped) SUPPLE_RT;
    // A port's transport is running (Stop would go out at a farewell).
    bool transportRunning() const SUPPLE_RT;
    const SyncStatus& status() const SUPPLE_RT { return status_; }

  private:
    SyncRuntime(const SyncConfig& config, double sampleRate);
    void feedMidi(const SyncCycle& c) SUPPLE_RT;
    void feedLtc(const SyncCycle& c) SUPPLE_RT;
    void writeLtc(const SyncCycle& c) SUPPLE_RT;
    void writeMidi(const SyncCycle& c) SUPPLE_RT;
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
    // The loop of the latest cycle, for output predicted beyond the newest span.
    bool loopActive_ = false;
    double loopStart_ = 0.0, loopEnd_ = 0.0;
    // One piece of the timeline, by processed device frame: from the history,
    // or predicted ahead of it at the newest span's speed. `known`: false
    // before the oldest span kept (nothing to tell a receiver).
    struct Piece {
        std::int64_t device = 0;
        std::int64_t frames = 0;
        double start = 0.0;
        double slope = 0.0;
        bool playing = false;
        bool known = true;
    };
    // Calls visit(Piece) for [from, to), in order.
    template <typename Visit>
    void pieces(std::int64_t from, std::int64_t to, Visit&& visit) const SUPPLE_RT;

    // MIDI clock and MTC output, per Backend::midiOutputPorts() index.
    struct Port {
        explicit Port(const sync::TimecodeMapping& m) : mtc(m) {}
        sync::ClockGenerator clock;
        sync::MtcGenerator mtc;
        bool known = false;         // `expected` holds a position
        bool playing = false;       // the latest piece played
        double expected = 0.0;      // timeline seconds the next piece continues from
        double lastClock = -1e300;  // output frame of the latest F8 sent
        // The slave was last sent Start or Continue, not yet Stop: what was
        // written, not the generator's state, so a refused Stop is owed.
        bool slaveRunning = false;
    };
    void emit(std::size_t port, const SyncOutput& out, const Piece& piece, std::int64_t delay,
              const time::TimeMap* map, sync::EventBuffer& events) SUPPLE_RT;
    std::vector<Port> ports_;
    std::array<std::atomic<std::int64_t>, kMaxSyncOutputs> delays_{};
    std::vector<sync::MidiEvent> eventStorage_;
    bool leaving_ = false;  // after farewell(): no more MIDI output
    double notifiedRate_ = 1.0;
    std::int64_t notifiedAt_ = 0;
};

}  // namespace supple::engine::detail
