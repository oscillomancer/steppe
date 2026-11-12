// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// SMPTE linear timecode (LTC) audio: an encoder and a decoder over libltc
// (LGPL-3.0-or-later, ADR-0070; DAW only). Spec: docs/specs/sync.md §5.
//
// create() allocates everything (libltc's buffers and queue); the per-block
// calls never allocate. An LTC frame names the frame that starts with it;
// its 80 bits span one frame, sync word last.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "supple/base/result.h"
#include "supple/base/rt.h"
#include "supple/sync/common.h"
#include "supple/sync/timecode.h"

namespace supple::sync {

struct LtcEncoderConfig {
    double sampleRate = 48000.0;
    TimecodeMapping mapping{};
    // Peak level of the square wave, dBFS (-18: the usual line-up level).
    double levelDb = -18.0;
};

class LtcEncoder {
  public:
    // Errors "sync.ltc.config" (rates, level) or "sync.ltc.alloc".
    static Result<LtcEncoder> create(const LtcEncoderConfig& config);
    LtcEncoder(LtcEncoder&&) noexcept;
    LtcEncoder& operator=(LtcEncoder&&) noexcept;
    ~LtcEncoder();

    // Transport starts (or jumps) at `timelineSeconds`: LTC begins at the
    // next frame boundary, silence before it.
    void start(double timelineSeconds) SUPPLE_RT;
    void locate(double timelineSeconds) SUPPLE_RT { start(timelineSeconds); }
    // Silence from the next render().
    void stop() SUPPLE_RT;
    // Writes out.size() samples for device frames [from, from + size), with
    // `seconds` mapping device frames to timeline seconds over that range.
    // Each LTC byte (a tenth of a frame) is stretched so that it ends where
    // the map puts it, so playrate and tempo changes are followed to the
    // sample, at speeds from 1/8 to 8 (buffers are sized for them at create
    // time); beyond the block the map's mean speed is extrapolated. A jump in
    // the map of more than a frame that start() was not told about, or a
    // first render already past the boundary start() named, starts at the
    // next boundary.
    void render(double from, std::span<float> out, const PositionMap& seconds) SUPPLE_RT;

    bool running() const SUPPLE_RT;
    // The frame count being encoded, or the next one while waiting.
    std::int64_t frame() const SUPPLE_RT;
    const LtcEncoderConfig& config() const SUPPLE_RT;

  private:
    struct Impl;
    explicit LtcEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct LtcDecoderConfig {
    double sampleRate = 48000.0;
    // The project's rate, when set: LTC cannot tell 24 from 23.976, nor 30
    // from 29.97 non-drop, except by measuring frame lengths (two seconds).
    // It holds at any speed, except that a source whose frames are within 3%
    // of another standard rate's length (25 against an expected 30) keeps its
    // own.
    std::optional<FrameRate> expected{};
    // Decoded frames held per feed() call: one second at 30 fps by default,
    // so feed() blocks up to a second long.
    int queueFrames = 32;
    // No frame for this long: signal lost.
    double timeoutSeconds = 0.2;
    // Consecutive frames (each one on from the last, either way) before
    // locked().
    int lockFrames = 4;
};

struct LtcFrame {
    Timecode timecode{};
    std::int64_t count = 0;  // frame count since 00:00:00:00
    double start = 0.0;      // device frame of its first bit (its last when reverse)
    double end = 0.0;        // device frame of its last sample
    bool reverse = false;
    bool dropFlag = false;
    double levelDb = 0.0;  // input level over the frame, dBFS
    Observation observation{};
};

class LtcDecoder {
  public:
    // Errors "sync.ltc.config" or "sync.ltc.alloc".
    static Result<LtcDecoder> create(const LtcDecoderConfig& config);
    LtcDecoder(LtcDecoder&&) noexcept;
    LtcDecoder& operator=(LtcDecoder&&) noexcept;
    ~LtcDecoder();

    // Feeds input samples for device frames [frame, frame + size) (contiguous
    // calls), returning the frames whose end fell in them, oldest first.
    // The span stays valid until the next feed().
    std::span<const LtcFrame> feed(double frame, std::span<const float> samples) SUPPLE_RT;
    // Checks the dropout timeout at `frame` (call once per block).
    void update(double frame) SUPPLE_RT;
    void reset() SUPPLE_RT;

    bool present() const SUPPLE_RT;
    bool locked() const SUPPLE_RT;
    // +1 forward, -1 backward, 0 unknown.
    int direction() const SUPPLE_RT;
    FrameRate rate() const SUPPLE_RT;
    // True once the rate rests on two seconds of measured frame lengths (or
    // on the drop-frame flag, or on the expected rate).
    bool rateMeasured() const SUPPLE_RT;
    double measuredFps() const SUPPLE_RT;  // 0 before two frames in a row
    Timecode timecode() const SUPPLE_RT;   // of the latest frame
    // The latest frame's end: forwards the next frame's start (count + 1),
    // backwards its own start (count), in timecode seconds.
    Observation lastObservation() const SUPPLE_RT;
    // Counts discontinuities in the frame count.
    std::uint32_t jumps() const SUPPLE_RT;

  private:
    struct Impl;
    explicit LtcDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace supple::sync
