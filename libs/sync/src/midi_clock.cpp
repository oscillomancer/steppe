// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sync/midi_clock.h"

#include <algorithm>
#include <cmath>

namespace supple::sync {
namespace {

constexpr std::uint8_t kClock = 0xF8;
constexpr std::uint8_t kStart = 0xFA;
constexpr std::uint8_t kContinue = 0xFB;
constexpr std::uint8_t kStop = 0xFC;
constexpr std::uint8_t kSongPosition = 0xF2;
// Positions this close to a 16th (or to beat 0) count as on it, so a
// position computed through a tempo map does not skip a 16th by rounding.
constexpr double kOnGrid = 1e-9;  // beats

double clockBeat(std::int64_t clock) SUPPLE_RT {
    return static_cast<double>(clock) / kClocksPerBeat;
}

}  // namespace

void ClockGenerator::begin(double frame, const PositionMap& beats, EventBuffer& out) SUPPLE_RT {
    const double beat = beats.positionAt(frame);
    running_ = true;
    if (beat <= kOnGrid) {
        out.push(frame, kStart);
        nextClock_ = 0;
        return;
    }
    const auto sixteenth = static_cast<std::int64_t>(std::ceil(beat * 4.0 - kOnGrid * 4.0));
    // Past the pointer's range the receiver cannot be told the position; it
    // gets the last one and the clocks still count from the true 16th.
    const auto pointer = static_cast<int>(std::min<std::int64_t>(sixteenth, kMaxSongPosition));
    const std::uint8_t spp[3] = {kSongPosition, static_cast<std::uint8_t>(pointer & 0x7F),
                                 static_cast<std::uint8_t>((pointer >> 7) & 0x7F)};
    out.push(frame, spp);
    out.push(frame, kContinue);
    nextClock_ = sixteenth * kClocksPerSixteenth;
}

void ClockGenerator::start(double frame, const PositionMap& beats, EventBuffer& out) SUPPLE_RT {
    if (running_) {
        locate(frame, beats, out);
        return;
    }
    begin(frame, beats, out);
}

void ClockGenerator::locate(double frame, const PositionMap& beats, EventBuffer& out) SUPPLE_RT {
    if (running_) {
        out.push(frame, kStop);
        begin(frame, beats, out);
        return;
    }
    const double beat = std::max(beats.positionAt(frame), 0.0);
    const auto sixteenth = static_cast<std::int64_t>(std::ceil(beat * 4.0 - kOnGrid * 4.0));
    const auto pointer = static_cast<int>(std::min<std::int64_t>(sixteenth, kMaxSongPosition));
    const std::uint8_t spp[3] = {kSongPosition, static_cast<std::uint8_t>(pointer & 0x7F),
                                 static_cast<std::uint8_t>((pointer >> 7) & 0x7F)};
    out.push(frame, spp);
}

void ClockGenerator::stop(double frame, EventBuffer& out) SUPPLE_RT {
    if (!running_) return;
    running_ = false;
    out.push(frame, kStop);
}

void ClockGenerator::run(double from, double to, const PositionMap& beats,
                         EventBuffer& out) SUPPLE_RT {
    if (!running_ || !(to > from)) return;
    const double first = beats.positionAt(from);
    const double last = beats.positionAt(to);
    while (clockBeat(nextClock_) < last) {
        const double beat = clockBeat(nextClock_);
        double frame = beat <= first ? from : beats.frameAt(beat, from, to);
        frame = std::clamp(frame, from, to);
        if (!out.push(frame, kClock)) return;
        ++nextClock_;
    }
}

std::size_t ClockGenerator::maxEventsPerBlock(double frames, double maxBeatsPerFrame) SUPPLE_RT {
    const double clocks =
        std::ceil(std::max(frames, 0.0) * std::max(maxBeatsPerFrame, 0.0) * kClocksPerBeat);
    return static_cast<std::size_t>(clocks) + 1 + 4;
}

// --- Receiver ---------------------------------------------------------------

void ClockReceiver::reset() SUPPLE_RT { *this = ClockReceiver(config_); }

void ClockReceiver::lose() SUPPLE_RT {
    // Running state stays as the last Start/Continue/Stop left it: a master
    // may start and send its first clock after a count-in.
    present_ = false;
    locked_ = false;
    steady_ = 0;
    if (dll_.running()) heldPeriod_ = dll_.period();
    dll_.reset();
}

double ClockReceiver::bpm() const SUPPLE_RT {
    if (!dll_.running()) return 0.0;
    return 60.0 * config_.sampleRate / (kClocksPerBeat * dll_.period());
}

double ClockReceiver::positionAt(double frame) const SUPPLE_RT {
    if (!running_ || current_ < 0 || nextClock_ != current_ + 1) return clockBeat(nextClock_);
    double ahead = 0.0;
    if (dll_.running()) ahead = std::clamp((frame - last_.frame) / dll_.period(), 0.0, 1.0);
    return (static_cast<double>(current_) + ahead) / kClocksPerBeat;
}

void ClockReceiver::update(double frame) SUPPLE_RT {
    if (!present_) return;
    double timeout = config_.timeoutSeconds * config_.sampleRate;
    if (dll_.running()) timeout = std::max(timeout, 4.0 * dll_.period());
    if (frame - lastClockFrame_ > timeout) lose();
}

bool ClockReceiver::feed(double frame, std::span<const std::uint8_t> message) SUPPLE_RT {
    if (message.empty()) return false;
    switch (message[0]) {
        case kClock: {
            update(frame);
            const double period = dll_.running() ? dll_.period() : heldPeriod_;
            const double gap = frame - lastClockFrame_;
            if (running_ && current_ >= 0 && period > 0.0 && gap > 1.5 * period &&
                gap <= config_.recoverSeconds * config_.sampleRate) {
                // Lost clocks: count them, so the position carries on.
                nextClock_ += static_cast<std::int64_t>(std::llround(gap / period)) - 1;
            }
            present_ = true;
            const double previous = dll_.period();
            const bool seeded = dll_.count() >= 2;
            const double error = dll_.feed(frame, config_.bandwidthHz, config_.sampleRate);
            lastClockFrame_ = frame;
            if (seeded && std::abs(error) > 0.5 * previous) {
                // A tempo jump or a lost clock: seed the loop again from here.
                dll_.restart(frame);
                steady_ = 0;
                locked_ = false;
            } else if (seeded && std::abs(error) <= 0.25 * previous) {
                if (++steady_ >= config_.lockClocks) locked_ = true;
            } else if (seeded) {
                steady_ = 0;
            }
            if (!running_) return false;
            current_ = nextClock_++;
            last_.frame = dll_.count() >= 3 ? dll_.time() : frame;
            last_.position = clockBeat(current_);
            return true;
        }
        case kStart:
            running_ = true;
            nextClock_ = 0;
            current_ = -1;
            ++jumps_;
            return false;
        case kContinue:
            running_ = true;
            current_ = -1;
            ++jumps_;
            return false;
        case kStop:
            running_ = false;
            current_ = -1;
            return false;
        case kSongPosition: {
            if (message.size() < 3) return false;
            const int pointer = (message[1] & 0x7F) | ((message[2] & 0x7F) << 7);
            const std::int64_t clock = std::int64_t{pointer} * kClocksPerSixteenth;
            if (clock != nextClock_ || running_) ++jumps_;
            nextClock_ = clock;
            current_ = -1;
            return false;
        }
        default:
            return false;
    }
}

}  // namespace supple::sync
