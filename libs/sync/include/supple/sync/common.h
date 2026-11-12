// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Shared pieces of the sync library (spec: docs/specs/sync.md §1):
// timestamped MIDI events in a caller-owned bounded buffer, the mapping from
// device frames to transport position that the engine implements, and the
// observations the receivers hand to the chase tracker.
//
// Time is always a device frame: the audio stream's sample count, as a
// double so that sub-sample positions survive. Nothing here reads a clock.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "supple/base/rt.h"

namespace supple::sync {

// One outgoing MIDI message. `frame` is the exact device frame (fractional)
// at which the message belongs; a sample-accurate scheduler sends it at
// ceil(frame), after any per-port offset. Messages in a buffer are in
// non-decreasing frame order, and messages at the same frame keep their
// order (FC before F2 before FB before F8).
struct MidiEvent {
    double frame = 0.0;
    std::uint8_t size = 0;
    std::array<std::uint8_t, 10> bytes{};  // the longest is MTC's full-frame SysEx
};

// A bounded event list over storage the caller owns (no allocation). When
// full, further pushes fail and overflowed() turns true; size the storage for
// the worst block (see the generators' maxEventsPerBlock()).
class EventBuffer {
  public:
    explicit EventBuffer(std::span<MidiEvent> storage) : storage_(storage) {}
    bool push(double frame, std::span<const std::uint8_t> bytes) SUPPLE_RT;
    bool push(double frame, std::uint8_t status) SUPPLE_RT;
    std::span<const MidiEvent> events() const SUPPLE_RT { return storage_.first(count_); }
    std::size_t size() const SUPPLE_RT { return count_; }
    std::size_t capacity() const SUPPLE_RT { return storage_.size(); }
    bool full() const SUPPLE_RT { return count_ == storage_.size(); }
    bool overflowed() const SUPPLE_RT { return overflowed_; }
    void clear() SUPPLE_RT {
        count_ = 0;
        overflowed_ = false;
    }

  private:
    std::span<MidiEvent> storage_;
    std::size_t count_ = 0;
    bool overflowed_ = false;
};

// Monotonic map between device frames and transport position over one
// segment of continuous motion: beats (quarter notes) for MIDI clock,
// timeline seconds for MTC and LTC. The engine implements it from its tempo
// map and playrate clock (ADR-0045), so tempo changes and playrate changes
// inside a block are exact. positionAt() must be non-decreasing in `frame`.
class PositionMap {
  public:
    virtual ~PositionMap() = default;
    virtual double positionAt(double frame) const SUPPLE_RT = 0;
    // The frame in [lo, hi] at which positionAt() reaches `position`;
    // positionAt(lo) <= position <= positionAt(hi) is guaranteed by the
    // caller. The default bisects positionAt() to the last bit (about 60
    // evaluations for a block); override it with the exact inverse when
    // there is one.
    virtual double frameAt(double position, double lo, double hi) const SUPPLE_RT;
};

// A constant speed: position = origin + (frame - originFrame) * perFrame.
// Tests and simple callers; the inverse is exact.
class LinearMap final : public PositionMap {
  public:
    LinearMap(double originFrame, double origin, double perFrame)
        : originFrame_(originFrame), origin_(origin), perFrame_(perFrame) {}
    double positionAt(double frame) const SUPPLE_RT override {
        return origin_ + (frame - originFrame_) * perFrame_;
    }
    double frameAt(double position, double lo, double hi) const SUPPLE_RT override;

  private:
    double originFrame_;
    double origin_;
    double perFrame_;
};

// An external position seen at a device frame, from a receiver: beats for
// MIDI clock, timecode seconds (frame count / fps, from 00:00:00:00) for MTC
// and LTC. The chase tracker consumes these.
struct Observation {
    double frame = 0.0;
    double position = 0.0;
};

}  // namespace supple::sync
