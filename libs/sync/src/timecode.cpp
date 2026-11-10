// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sync/timecode.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace supple::sync {
namespace {

// Drop-frame 29.97: 1798 frames per ordinary minute, 1800 per tenth minute,
// 17982 per ten minutes.
constexpr std::int64_t kDropTen = 17982;
constexpr std::int64_t kDropMinute = 1798;

std::int64_t wrap(std::int64_t frames, std::int64_t day) SUPPLE_RT {
    const std::int64_t r = frames % day;
    return r < 0 ? r + day : r;
}

struct NamedRate {
    std::string_view name;
    FrameRate rate;
};

// The first spelling of each rate is its rateName().
constexpr std::array<NamedRate, 14> kRateNames{{
    {"23.976", FrameRate::Fps23976},
    {"24", FrameRate::Fps24},
    {"25", FrameRate::Fps25},
    {"29.97df", FrameRate::Fps2997Drop},
    {"29.97", FrameRate::Fps2997},
    {"30", FrameRate::Fps30},
    {"23.98", FrameRate::Fps23976},
    {"29.97drop", FrameRate::Fps2997Drop},
    {"2997df", FrameRate::Fps2997Drop},
    {"30df", FrameRate::Fps2997Drop},
    {"29.97ndf", FrameRate::Fps2997},
    {"2997", FrameRate::Fps2997},
    {"29.97nd", FrameRate::Fps2997},
    {"2997ndf", FrameRate::Fps2997},
}};

void appendTwo(std::string& out, int value) {
    value = std::clamp(value, 0, 99);
    out.push_back(static_cast<char>('0' + value / 10));
    out.push_back(static_cast<char>('0' + value % 10));
}

}  // namespace

RateInfo rateInfo(FrameRate rate) SUPPLE_RT {
    switch (rate) {
        case FrameRate::Fps23976:
            return {24, false, 24000, 1001};
        case FrameRate::Fps24:
            return {24, false, 24, 1};
        case FrameRate::Fps25:
            return {25, false, 25, 1};
        case FrameRate::Fps2997Drop:
            return {30, true, 30000, 1001};
        case FrameRate::Fps2997:
            return {30, false, 30000, 1001};
        case FrameRate::Fps30:
            return {30, false, 30, 1};
    }
    return {25, false, 25, 1};
}

std::string_view rateName(FrameRate rate) SUPPLE_RT {
    for (const NamedRate& n : kRateNames)
        if (n.rate == rate) return n.name;
    return "25";
}

Result<FrameRate> parseRate(std::string_view text) {
    std::string lower;
    for (char c : text) {
        if (c == ' ' || c == '_' || c == '-') continue;
        lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (lower.size() > 3 && lower.substr(lower.size() - 3) == "fps") lower.resize(lower.size() - 3);
    for (const NamedRate& n : kRateNames)
        if (lower == n.name) return n.rate;
    return Error("sync.rate", "Unknown timecode frame rate \"" + std::string(text) + "\".", {},
                 "Use one of 23.976, 24, 25, 29.97df, 29.97 or 30.");
}

std::int64_t framesPerDay(FrameRate rate) SUPPLE_RT {
    const RateInfo info = rateInfo(rate);
    if (info.drop) return 24 * 6 * kDropTen;
    return std::int64_t{24} * 3600 * info.nominal;
}

bool valid(const Timecode& tc, FrameRate rate) SUPPLE_RT {
    const RateInfo info = rateInfo(rate);
    if (tc.hours < 0 || tc.hours > 23 || tc.minutes < 0 || tc.minutes > 59 || tc.seconds < 0 ||
        tc.seconds > 59 || tc.frames < 0 || tc.frames >= info.nominal)
        return false;
    if (info.drop && tc.seconds == 0 && tc.frames < 2 && tc.minutes % 10 != 0) return false;
    return true;
}

std::int64_t toFrames(const Timecode& tc, FrameRate rate) SUPPLE_RT {
    const RateInfo info = rateInfo(rate);
    const std::int64_t h = std::clamp(tc.hours, 0, 23);
    const std::int64_t m = std::clamp(tc.minutes, 0, 59);
    const std::int64_t s = std::clamp(tc.seconds, 0, 59);
    std::int64_t f = std::clamp(tc.frames, 0, info.nominal - 1);
    if (info.drop && s == 0 && f < 2 && m % 10 != 0) f = 2;
    std::int64_t frames = ((h * 60 + m) * 60 + s) * info.nominal + f;
    if (info.drop) {
        const std::int64_t minutes = h * 60 + m;
        frames -= 2 * (minutes - minutes / 10);
    }
    return frames;
}

Timecode fromFrames(std::int64_t frames, FrameRate rate) SUPPLE_RT {
    const RateInfo info = rateInfo(rate);
    std::int64_t n = wrap(frames, framesPerDay(rate));
    if (info.drop) {
        // Add back the labels skipped before this frame: 18 per whole ten
        // minutes, 2 per whole ordinary minute after the tenth.
        const std::int64_t tens = n / kDropTen;
        const std::int64_t rest = n % kDropTen;
        n += 18 * tens + (rest < 2 ? 0 : 2 * ((rest - 2) / kDropMinute));
    }
    const std::int64_t perSecond = info.nominal;
    Timecode tc;
    tc.frames = static_cast<int>(n % perSecond);
    n /= perSecond;
    tc.seconds = static_cast<int>(n % 60);
    n /= 60;
    tc.minutes = static_cast<int>(n % 60);
    tc.hours = static_cast<int>(n / 60);
    return tc;
}

double toSeconds(double frames, FrameRate rate) SUPPLE_RT {
    const RateInfo info = rateInfo(rate);
    return frames * info.denominator / info.numerator;
}

double framesAt(double seconds, FrameRate rate) SUPPLE_RT {
    const RateInfo info = rateInfo(rate);
    return seconds * info.numerator / info.denominator;
}

double TimecodeMapping::startSeconds() const SUPPLE_RT {
    return toSeconds(static_cast<double>(toFrames(start, rate)), rate);
}

double TimecodeMapping::framesAt(double timelineSeconds) const SUPPLE_RT {
    return static_cast<double>(toFrames(start, rate)) + sync::framesAt(timelineSeconds, rate);
}

double TimecodeMapping::timelineSeconds(double frames) const SUPPLE_RT {
    return toSeconds(frames - static_cast<double>(toFrames(start, rate)), rate);
}

Timecode TimecodeMapping::labelAt(double timelineSeconds) const SUPPLE_RT {
    // A small tolerance keeps exact frame starts computed through seconds
    // (1001/30000 is not exact in binary) on their own frame.
    const double f = framesAt(timelineSeconds);
    return fromFrames(static_cast<std::int64_t>(std::floor(f + 1e-6)), rate);
}

std::string format(const Timecode& tc, FrameRate rate) {
    std::string out;
    out.reserve(11);
    appendTwo(out, tc.hours);
    out.push_back(':');
    appendTwo(out, tc.minutes);
    out.push_back(':');
    appendTwo(out, tc.seconds);
    out.push_back(rateInfo(rate).drop ? ';' : ':');
    appendTwo(out, tc.frames);
    return out;
}

Result<Timecode> parse(std::string_view text, FrameRate rate) {
    auto failure = [&](std::string message) {
        const RateInfo info = rateInfo(rate);
        std::string hint = "Write HH:MM:SS:FF with frames 00.." + std::to_string(info.nominal - 1) +
                           " at " + std::string(rateName(rate)) + " fps";
        if (info.drop) hint += "; drop-frame skips frames 00 and 01 at minutes not divisible by 10";
        return Error("sync.timecode", std::move(message), {}, hint + ".");
    };
    std::array<int, 4> fields{};
    int count = 0;
    int digits = 0;
    int value = 0;
    std::size_t i = 0;
    while (i < text.size() && text[i] == ' ') ++i;
    std::size_t end = text.size();
    while (end > i && text[end - 1] == ' ') --end;
    if (i == end) return failure("The timecode is empty.");
    for (; i <= end; ++i) {
        const char c = i < end ? text[i] : ':';
        if (c >= '0' && c <= '9') {
            if (++digits > 2)
                return failure("Timecode field has more than two digits: \"" + std::string(text) +
                               "\".");
            value = value * 10 + (c - '0');
        } else if (c == ':' || c == ';' || c == '.' || c == ',') {
            if (digits == 0)
                return failure("Timecode has an empty field: \"" + std::string(text) + "\".");
            if (count == 4)
                return failure("Timecode has more than four fields: \"" + std::string(text) +
                               "\".");
            fields[static_cast<std::size_t>(count++)] = value;
            digits = 0;
            value = 0;
        } else {
            return failure("Unexpected character '" + std::string(1, c) + "' in timecode \"" +
                           std::string(text) + "\".");
        }
    }
    if (count < 2)
        return failure("Timecode needs at least seconds and frames: \"" + std::string(text) +
                       "\".");
    Timecode tc;
    int* targets[4] = {&tc.hours, &tc.minutes, &tc.seconds, &tc.frames};
    for (int k = 0; k < count; ++k) *targets[4 - count + k] = fields[static_cast<std::size_t>(k)];
    if (!valid(tc, rate)) return failure("Timecode \"" + std::string(text) + "\" is out of range.");
    return tc;
}

}  // namespace supple::sync
