// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// SMPTE timecode: rates, HH:MM:SS:FF labels, frame counts and seconds
// (spec: docs/specs/sync.md §2, ADR-0070).
//
// A frame count is the number of frames since 00:00:00:00. Labels and counts
// differ only for drop-frame 29.97, which skips labels :00 and :01 at the
// start of every minute except each tenth. Seconds are real time: a count
// lasts count / fps seconds, so 29.97 non-drop labels run 0.1% behind the
// clock. Counts wrap at 24 hours (kFramesPerDay). Everything here is pure and
// real-time safe except text parsing and formatting, which use std::string.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "supple/base/result.h"
#include "supple/base/rt.h"

namespace supple::sync {

enum class FrameRate : std::uint8_t {
    Fps23976,     // 24000/1001, non-drop (LTC only; MTC sends it as 24)
    Fps24,        // 24
    Fps25,        // 25
    Fps2997Drop,  // 30000/1001, drop-frame labels
    Fps2997,      // 30000/1001, non-drop labels
    Fps30,        // 30
};

// Exact rate: fps = numerator / denominator.
struct RateInfo {
    int nominal;  // frames per label second: 24, 25 or 30
    bool drop;    // drop-frame labels
    int numerator;
    int denominator;
    double fps() const { return static_cast<double>(numerator) / denominator; }
};

RateInfo rateInfo(FrameRate rate) SUPPLE_RT;
// "23.976", "24", "25", "29.97df", "29.97", "30": the GUI's and the
// commands' names.
std::string_view rateName(FrameRate rate) SUPPLE_RT;
// Accepts rateName() spellings plus "29.97ndf", "2997df", "30df" (as
// 29.97df) and similar. Error code "sync.rate".
Result<FrameRate> parseRate(std::string_view text);
// Frames in 24 hours: 2073600 at 24 and 23.976, 2160000 at 25, 2589408 at
// 29.97df, 2592000 at 29.97 and 30.
std::int64_t framesPerDay(FrameRate rate) SUPPLE_RT;

struct Timecode {
    int hours = 0;    // 0..23
    int minutes = 0;  // 0..59
    int seconds = 0;  // 0..59
    int frames = 0;   // 0..nominal-1 (drop-frame: not 0/1 at minutes not divisible by 10)
    friend bool operator==(const Timecode&, const Timecode&) = default;
};

// True when every field is in range for the rate, including drop-frame's
// skipped labels.
bool valid(const Timecode& tc, FrameRate rate) SUPPLE_RT;
// Label -> frame count. An invalid label is clamped field by field, and a
// skipped drop-frame label maps to the next valid one (00:01:00;00 ->
// 00:01:00;02), as decks do; valid() tells the two apart.
std::int64_t toFrames(const Timecode& tc, FrameRate rate) SUPPLE_RT;
// Frame count -> label, modulo 24 hours (negative counts wrap backwards).
Timecode fromFrames(std::int64_t frames, FrameRate rate) SUPPLE_RT;

// Frame count <-> real seconds. framesAt() is fractional: floor() of it is
// the frame playing at `seconds`.
double toSeconds(double frames, FrameRate rate) SUPPLE_RT;
double framesAt(double seconds, FrameRate rate) SUPPLE_RT;

// A project's timecode: timeline seconds 0 is `start` (01:00:00:00 is
// common). Timecode seconds = timeline seconds + start seconds, and they
// wrap at 24 hours only when shown as a label.
struct TimecodeMapping {
    FrameRate rate = FrameRate::Fps25;
    Timecode start{};
    double startSeconds() const SUPPLE_RT;
    // Timeline seconds -> fractional frame count (not wrapped) and back.
    double framesAt(double timelineSeconds) const SUPPLE_RT;
    double timelineSeconds(double frames) const SUPPLE_RT;
    // The label playing at a timeline position, wrapped to 24 hours.
    Timecode labelAt(double timelineSeconds) const SUPPLE_RT;
    friend bool operator==(const TimecodeMapping&, const TimecodeMapping&) = default;
};

// "HH:MM:SS:FF"; drop-frame uses ';' before the frames ("01:00:00;00").
std::string format(const Timecode& tc, FrameRate rate);
// Parses "HH:MM:SS:FF" with ':', ';', '.' or ',' as separators (any of them
// in any place), also "MM:SS:FF" and "SS:FF" (leading fields zero). Fields
// are validated for the rate. Error code "sync.timecode", with a hint.
Result<Timecode> parse(std::string_view text, FrameRate rate);

}  // namespace supple::sync
