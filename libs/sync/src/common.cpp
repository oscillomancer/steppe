// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sync/common.h"

#include <algorithm>

namespace supple::sync {

bool EventBuffer::push(double frame, std::span<const std::uint8_t> bytes) SUPPLE_RT {
    if (count_ == storage_.size() || bytes.size() > MidiEvent{}.bytes.size()) {
        overflowed_ = true;
        return false;
    }
    MidiEvent& e = storage_[count_++];
    e.frame = frame;
    e.size = static_cast<std::uint8_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), e.bytes.begin());
    return true;
}

bool EventBuffer::push(double frame, std::uint8_t status) SUPPLE_RT {
    const std::uint8_t bytes[1] = {status};
    return push(frame, bytes);
}

double PositionMap::frameAt(double position, double lo, double hi) const SUPPLE_RT {
    // Bisection to the last bit: positionAt() is monotonic, so the bracket
    // holds, and the answer does not depend on how wide it started. 1100
    // halvings cover any pair of doubles; a frame-sized bracket needs ~60.
    for (int i = 0; i < 1100; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (!(mid > lo && mid < hi)) break;
        if (positionAt(mid) < position)
            lo = mid;
        else
            hi = mid;
    }
    return hi;
}

double LinearMap::frameAt(double position, double lo, double hi) const SUPPLE_RT {
    if (perFrame_ <= 0.0) return lo;
    return std::clamp(originFrame_ + (position - origin_) / perFrame_, lo, hi);
}

}  // namespace supple::sync
