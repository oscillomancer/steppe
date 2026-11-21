// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <atomic>
#include <span>
#include <vector>

#include "supple/base/spsc.h"

namespace supple::host {

// Only the audio consumer touches the SPSC read cursor. The main-thread mirror
// lets an inactive save flush unconsumed edits without taking over that cursor.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

class PendingEdits {
  public:
    // `kind`: a value (Value), or, from a VST3 editor (round 81 E2), a value
    // it performed (Edited) or the begin or end of its gesture, which carry no
    // value for the processor.
    enum Kind : std::uint8_t { Value, Edited, Begin, End };
    struct Change {
        std::uint32_t id;
        double value;
        std::uint64_t generation, sequence;
        Kind kind = Value;
    };
    PendingEdits() { pending_.reserve(1025); }
    bool push(std::uint32_t id, double value, Kind kind = Value) {
        prune();
        Change change{id, value, generation_.load(), ++sequence_, kind};
        if (!queue_.push(change)) return false;
        pending_.push_back(change);
        return true;
    }
    bool pop(Change& change) SUPPLE_RT {
        while (queue_.pop(change)) {
            consumed_.store(change.sequence, std::memory_order_release);
            if (change.generation == generation_.load(std::memory_order_acquire)) return true;
        }
        return false;
    }
    std::span<const Change> pending() {
        prune();
        return pending_;
    }
    void barrier() {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        pending_.clear();
    }

  private:
    void prune() {
        const auto consumed = consumed_.load(std::memory_order_acquire);
        std::erase_if(pending_, [&](const Change& c) { return c.sequence <= consumed; });
    }
    SpscRing<Change, 1024> queue_;
    std::atomic<std::uint64_t> generation_{0}, consumed_{0};
    std::uint64_t sequence_ = 0;
    std::vector<Change> pending_;  // main thread only
};

}  // namespace supple::host
