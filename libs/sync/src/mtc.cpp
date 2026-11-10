// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sync/mtc.h"

#include <algorithm>
#include <cmath>

namespace supple::sync {
namespace {

constexpr std::uint8_t kQuarterFrame = 0xF1;
// Frame boundaries computed through seconds may land a hair early.
constexpr double kOnFrame = 1e-6;  // frames

std::int64_t floorDiv(std::int64_t a, std::int64_t b) SUPPLE_RT {
    const std::int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

std::int64_t floorMod(std::int64_t a, std::int64_t b) SUPPLE_RT { return a - floorDiv(a, b) * b; }

// The four-bit piece `piece` of the label of frame count `frame`.
std::uint8_t pieceNibble(const Timecode& tc, int piece, int code) SUPPLE_RT {
    switch (piece) {
        case 0:
            return static_cast<std::uint8_t>(tc.frames & 0x0F);
        case 1:
            return static_cast<std::uint8_t>((tc.frames >> 4) & 0x01);
        case 2:
            return static_cast<std::uint8_t>(tc.seconds & 0x0F);
        case 3:
            return static_cast<std::uint8_t>((tc.seconds >> 4) & 0x03);
        case 4:
            return static_cast<std::uint8_t>(tc.minutes & 0x0F);
        case 5:
            return static_cast<std::uint8_t>((tc.minutes >> 4) & 0x03);
        case 6:
            return static_cast<std::uint8_t>(tc.hours & 0x0F);
        default:
            return static_cast<std::uint8_t>(((tc.hours >> 4) & 0x01) | (code << 1));
    }
}

FrameRate codedRate(int code) SUPPLE_RT {
    switch (code & 3) {
        case 0:
            return FrameRate::Fps24;
        case 1:
            return FrameRate::Fps25;
        case 2:
            return FrameRate::Fps2997Drop;
        default:
            return FrameRate::Fps30;
    }
}

}  // namespace

int mtcRateCode(FrameRate rate) SUPPLE_RT {
    switch (rate) {
        case FrameRate::Fps23976:
        case FrameRate::Fps24:
            return 0;
        case FrameRate::Fps25:
            return 1;
        case FrameRate::Fps2997Drop:
            return 2;
        case FrameRate::Fps2997:
        case FrameRate::Fps30:
            return 3;
    }
    return 1;
}

// --- Generator --------------------------------------------------------------

void MtcGenerator::fullFrame(double frame, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT {
    const Timecode tc = mapping_.labelAt(seconds.positionAt(frame));
    const std::uint8_t sysex[10] = {
        0xF0,
        0x7F,
        0x7F,
        0x01,
        0x01,
        static_cast<std::uint8_t>((mtcRateCode(mapping_.rate) << 5) | (tc.hours & 0x1F)),
        static_cast<std::uint8_t>(tc.minutes),
        static_cast<std::uint8_t>(tc.seconds),
        static_cast<std::uint8_t>(tc.frames),
        0xF7};
    out.push(frame, sysex);
}

void MtcGenerator::start(double frame, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT {
    running_ = true;
    locate(frame, seconds, out);
}

void MtcGenerator::locate(double frame, const PositionMap& seconds, EventBuffer& out) SUPPLE_RT {
    fullFrame(frame, seconds, out);
    // The next even frame boundary at or after the position.
    auto first = static_cast<std::int64_t>(
        std::ceil(mapping_.framesAt(seconds.positionAt(frame)) - kOnFrame));
    if (floorMod(first, 2) != 0) ++first;
    nextQuarter_ = first * 4;
}

void MtcGenerator::run(double from, double to, const PositionMap& seconds,
                       EventBuffer& out) SUPPLE_RT {
    if (!running_ || !(to > from)) return;
    const double first = seconds.positionAt(from);
    const double last = seconds.positionAt(to);
    const int code = mtcRateCode(mapping_.rate);
    for (;;) {
        const double at = mapping_.timelineSeconds(static_cast<double>(nextQuarter_) / 4.0);
        if (!(at < last)) break;
        double frame = at <= first ? from : seconds.frameAt(at, from, to);
        frame = std::clamp(frame, from, to);
        const auto piece = static_cast<int>(floorMod(nextQuarter_, 8));
        const Timecode tc = fromFrames(floorDiv(nextQuarter_, 8) * 2, mapping_.rate);
        const std::uint8_t message[2] = {
            kQuarterFrame, static_cast<std::uint8_t>((piece << 4) | pieceNibble(tc, piece, code))};
        if (!out.push(frame, message)) return;
        ++nextQuarter_;
    }
}

std::size_t MtcGenerator::maxEventsPerBlock(double frames, double sampleRate, FrameRate rate,
                                            double maxSpeed) SUPPLE_RT {
    if (!(sampleRate > 0.0)) return 2;
    const double quarters =
        std::max(frames, 0.0) / sampleRate * std::max(maxSpeed, 0.0) * rateInfo(rate).fps() * 4.0;
    return static_cast<std::size_t>(std::ceil(quarters)) + 2;
}

// --- Receiver ---------------------------------------------------------------

void MtcReceiver::reset() SUPPLE_RT { *this = MtcReceiver(config_); }

void MtcReceiver::restartRun() SUPPLE_RT {
    fit_.reset();
    runSteps_ = 0;
}

void MtcReceiver::lose() SUPPLE_RT {
    present_ = false;
    locked_ = false;
    known_ = false;
    rateMeasured_ = false;
    refinedCode_ = -1;
    direction_ = 0;
    lastPiece_ = -1;
    mask_ = 0;
    consistent_ = 0;
    dll_.reset();
    restartRun();
}

double MtcReceiver::measuredFps() const SUPPLE_RT {
    const double perQuarter = fit_.slope();
    return perQuarter > 0.0 ? config_.sampleRate / (4.0 * perQuarter) : 0.0;
}

double MtcReceiver::secondsOf(std::int64_t quarter) const SUPPLE_RT {
    return toSeconds(static_cast<double>(quarter) / 4.0, rate_);
}

Timecode MtcReceiver::timecode() const SUPPLE_RT {
    return fromFrames(floorDiv(quarter_, 4), rate_);
}

void MtcReceiver::update(double frame) SUPPLE_RT {
    if (present_ && frame - lastFrame_ > config_.timeoutSeconds * config_.sampleRate) lose();
}

void MtcReceiver::quarterFrame(double frame, std::uint8_t data, bool& observed) SUPPLE_RT {
    update(frame);
    present_ = true;
    lastFrame_ = frame;
    const int piece = (data >> 4) & 7;
    const auto nibble = static_cast<std::uint8_t>(data & 0x0F);
    int step = 0;
    if (lastPiece_ >= 0) {
        if (piece == ((lastPiece_ + 1) & 7))
            step = 1;
        else if (piece == ((lastPiece_ + 7) & 7))
            step = -1;
    }
    if (lastPiece_ >= 0 && step == 0) {
        // A lost or repeated piece: the count is no longer known.
        known_ = false;
        locked_ = false;
        consistent_ = 0;
        mask_ = 0;
        direction_ = 0;
        dll_.restart(frame);
        restartRun();
    } else if (step != 0 && direction_ != 0 && step != direction_) {
        // Reversal: the sequence being assembled is mixed; the count goes on.
        mask_ = 0;
        consistent_ = 0;
        locked_ = false;
        dll_.restart(frame);
        restartRun();
    } else {
        dll_.feed(frame, config_.bandwidthHz, config_.sampleRate);
    }
    if (step != 0) direction_ = step;
    lastPiece_ = piece;

    // A sequence begins at piece 0 forwards, at piece 7 backwards.
    if ((piece == 0 && direction_ >= 0) || (piece == 7 && direction_ <= 0)) mask_ = 0;
    nibbles_[piece] = nibble;
    mask_ |= 1u << piece;
    if (known_ && step != 0) quarter_ = floorMod(quarter_ + step, 4 * framesPerDay(rate_));

    const bool complete =
        mask_ == 0xFF && ((direction_ > 0 && piece == 7) || (direction_ < 0 && piece == 0));
    if (complete) {
        const int code = (nibbles_[7] >> 1) & 3;
        FrameRate rate = codedRate(code);
        if (config_.expected && mtcRateCode(*config_.expected) == code)
            rate = *config_.expected;
        else if (code == refinedCode_)
            rate = refinedRate_;
        Timecode tc;
        tc.frames = nibbles_[0] | ((nibbles_[1] & 1) << 4);
        tc.seconds = nibbles_[2] | ((nibbles_[3] & 3) << 4);
        tc.minutes = nibbles_[4] | ((nibbles_[5] & 3) << 4);
        tc.hours = nibbles_[6] | ((nibbles_[7] & 1) << 4);
        // A refined rate (24 to 23.976, 30 to 29.97) keeps the labels and so
        // the count; only the seconds change.
        rate_ = rate;
        const std::int64_t q = floorMod(toFrames(tc, rate) * 4 + piece, 4 * framesPerDay(rate));
        if (known_ && q == quarter_) {
            ++consistent_;
        } else {
            if (known_) ++jumps_;
            restartRun();
            quarter_ = q;
            known_ = true;
            consistent_ = 1;
            locked_ = false;
        }
        if (!locked_ && consistent_ >= config_.lockSequences) {
            locked_ = true;
            lockedSince_ = frame;
        }
        // Two seconds of lock decide 23.976/24 and 29.97/30, once.
        if (locked_ && !rateMeasured_ && !config_.expected &&
            frame - runStart_ >= 2.0 * config_.sampleRate &&
            frame - lockedSince_ >= 2.0 * config_.sampleRate) {
            rateMeasured_ = true;
            const double fps = measuredFps();
            if (code == 0 || code == 3) {
                refinedCode_ = code;
                refinedRate_ = code == 0 ? (fps < 23.988 ? FrameRate::Fps23976 : FrameRate::Fps24)
                                         : (fps < 29.985 ? FrameRate::Fps2997 : FrameRate::Fps30);
            }
        }
    }
    if (known_ && step != 0) {
        if (fit_.count() == 0) runStart_ = frame;
        fit_.add(static_cast<double>(runSteps_++), frame - runStart_);
    }
    if (known_) {
        last_.frame = dll_.count() >= 3 ? dll_.time() : frame;
        last_.position = secondsOf(quarter_);
        observed = true;
    }
}

bool MtcReceiver::feed(double frame, std::span<const std::uint8_t> message) SUPPLE_RT {
    if (message.size() >= 2 && message[0] == kQuarterFrame) {
        bool observed = false;
        quarterFrame(frame, message[1], observed);
        return observed;
    }
    if (message.size() >= 10 && message[0] == 0xF0 && message[1] == 0x7F && message[3] == 0x01 &&
        message[4] == 0x01 && message[9] == 0xF7) {
        const int code = (message[5] >> 5) & 3;
        FrameRate rate = codedRate(code);
        if (config_.expected && mtcRateCode(*config_.expected) == code) rate = *config_.expected;
        rate_ = rate;
        Timecode tc;
        tc.hours = message[5] & 0x1F;
        tc.minutes = message[6] & 0x3F;
        tc.seconds = message[7] & 0x3F;
        tc.frames = message[8] & 0x1F;
        quarter_ = toFrames(tc, rate) * 4;
        known_ = true;
        locked_ = false;
        consistent_ = 0;
        direction_ = 0;
        lastPiece_ = -1;
        mask_ = 0;
        dll_.reset();
        restartRun();
        ++jumps_;
        last_.frame = frame;
        last_.position = secondsOf(quarter_);
    }
    return false;
}

}  // namespace supple::sync
