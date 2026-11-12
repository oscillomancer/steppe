// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/sync/timecode.h>

#include <array>
#include <cmath>
#include <cstdint>

using namespace supple;
using namespace supple::sync;

namespace {

constexpr std::array<FrameRate, 6> kRates{FrameRate::Fps23976, FrameRate::Fps24,
                                          FrameRate::Fps25,    FrameRate::Fps2997Drop,
                                          FrameRate::Fps2997,  FrameRate::Fps30};

Timecode tc(int h, int m, int s, int f) { return Timecode{h, m, s, f}; }

// Conversions on the audio thread: RealtimeSanitizer checks this under rtsan.
std::int64_t roundTrip(std::int64_t frames, FrameRate rate) SUPPLE_RT {
    const Timecode label = fromFrames(frames, rate);
    const double seconds = toSeconds(static_cast<double>(frames), rate);
    const auto back = static_cast<std::int64_t>(std::llround(framesAt(seconds, rate)));
    return valid(label, rate) && back == frames ? toFrames(label, rate) : -1;
}

}  // namespace

TEST_CASE("timecode: rates and the frames in a day") {
    CHECK(rateInfo(FrameRate::Fps2997Drop).fps() == doctest::Approx(29.97002997));
    CHECK(rateInfo(FrameRate::Fps23976).fps() == doctest::Approx(23.976023976));
    CHECK(framesPerDay(FrameRate::Fps24) == 2073600);
    CHECK(framesPerDay(FrameRate::Fps23976) == 2073600);
    CHECK(framesPerDay(FrameRate::Fps25) == 2160000);
    CHECK(framesPerDay(FrameRate::Fps2997Drop) == 2589408);
    CHECK(framesPerDay(FrameRate::Fps2997) == 2592000);
    CHECK(framesPerDay(FrameRate::Fps30) == 2592000);
    for (FrameRate r : kRates) {
        auto parsed = parseRate(rateName(r));
        REQUIRE(parsed);
        CHECK(*parsed == r);
    }
    CHECK(*parseRate("29.97 DF") == FrameRate::Fps2997Drop);
    CHECK(*parseRate("30df") == FrameRate::Fps2997Drop);
    CHECK(*parseRate("29.97ndf") == FrameRate::Fps2997);
    CHECK(*parseRate("25fps") == FrameRate::Fps25);
    auto bad = parseRate("50");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "sync.rate");
    CHECK_FALSE(bad.error().hint.empty());
}

TEST_CASE("timecode: every frame of a day round-trips at every rate") {
    for (FrameRate r : kRates) {
        CAPTURE(rateName(r));
        const std::int64_t day = framesPerDay(r);
        std::int64_t failures = 0;
        Timecode previous = fromFrames(-1, r);
        for (std::int64_t f = 0; f < day; ++f) {
            if (roundTrip(f, r) != f) ++failures;
            // Labels advance one frame at a time, skipping only drop labels.
            const Timecode label = fromFrames(f, r);
            if (f > 0 && label.frames != previous.frames + 1 && label.frames != 0 &&
                !(rateInfo(r).drop && label.frames == 2 && label.seconds == 0))
                ++failures;
            previous = label;
        }
        CHECK(failures == 0);
        CHECK(fromFrames(day, r) == tc(0, 0, 0, 0));
        CHECK(fromFrames(day - 1, r) == tc(23, 59, 59, rateInfo(r).nominal - 1));
    }
}

TEST_CASE("timecode: drop-frame minute boundaries") {
    const auto r = FrameRate::Fps2997Drop;
    CHECK(toFrames(tc(0, 0, 59, 29), r) == 1799);
    CHECK(fromFrames(1800, r) == tc(0, 1, 0, 2));
    CHECK(fromFrames(1799, r) == tc(0, 0, 59, 29));
    CHECK(toFrames(tc(0, 9, 59, 29), r) == 17981);
    CHECK(fromFrames(17982, r) == tc(0, 10, 0, 0));
    CHECK(fromFrames(17983, r) == tc(0, 10, 0, 1));
    CHECK(toFrames(tc(0, 11, 0, 2), r) == 17982 + 1800);
    CHECK(toFrames(tc(1, 0, 0, 0), r) == 107892);
    CHECK(toFrames(tc(23, 59, 59, 29), r) == 2589407);
    // Skipped labels are invalid and map to the next valid label.
    CHECK_FALSE(valid(tc(0, 1, 0, 0), r));
    CHECK_FALSE(valid(tc(0, 1, 0, 1), r));
    CHECK(valid(tc(0, 1, 0, 2), r));
    CHECK(valid(tc(0, 10, 0, 0), r));
    CHECK(valid(tc(0, 1, 0, 0), FrameRate::Fps2997));
    CHECK(toFrames(tc(0, 1, 0, 0), r) == toFrames(tc(0, 1, 0, 2), r));
    CHECK_FALSE(valid(tc(0, 0, 0, 25), FrameRate::Fps25));
    CHECK_FALSE(valid(tc(24, 0, 0, 0), FrameRate::Fps25));
}

TEST_CASE("timecode: seconds are real time") {
    // One hour of 29.97 labels: non-drop runs 3.6 s behind the clock, drop
    // frame keeps up to 3.6 ms.
    CHECK(toSeconds(static_cast<double>(toFrames(tc(1, 0, 0, 0), FrameRate::Fps2997)),
                    FrameRate::Fps2997) == doctest::Approx(3603.6));
    CHECK(toSeconds(static_cast<double>(toFrames(tc(1, 0, 0, 0), FrameRate::Fps2997Drop)),
                    FrameRate::Fps2997Drop) == doctest::Approx(3599.9964));
    CHECK(toSeconds(90000.0, FrameRate::Fps25) == 3600.0);
    CHECK(fromFrames(-1, FrameRate::Fps25) == tc(23, 59, 59, 24));
}

TEST_CASE("timecode: a start offset") {
    TimecodeMapping m{FrameRate::Fps25, tc(1, 0, 0, 0)};
    CHECK(m.startSeconds() == 3600.0);
    CHECK(m.framesAt(0.0) == 90000.0);
    CHECK(m.labelAt(1.0) == tc(1, 0, 1, 0));
    CHECK(m.labelAt(-0.04) == tc(0, 59, 59, 24));
    CHECK(m.timelineSeconds(90025.0) == 1.0);
    // Exact frame starts at 29.97, computed through seconds, stay on their
    // frame.
    TimecodeMapping d{FrameRate::Fps2997Drop, tc(0, 0, 0, 0)};
    for (int k = 0; k < 40000; k += 7) {
        const double seconds = k * 1001.0 / 30000.0;
        CHECK(toFrames(d.labelAt(seconds), d.rate) == k);
    }
}

TEST_CASE("timecode: text") {
    CHECK(format(tc(1, 2, 3, 4), FrameRate::Fps25) == "01:02:03:04");
    CHECK(format(tc(1, 0, 0, 0), FrameRate::Fps2997Drop) == "01:00:00;00");
    auto p = parse("01:02:03:04", FrameRate::Fps25);
    REQUIRE(p);
    CHECK(*p == tc(1, 2, 3, 4));
    CHECK(*parse(" 10:00:00;00 ", FrameRate::Fps2997Drop) == tc(10, 0, 0, 0));
    CHECK(*parse("1.2.3.4", FrameRate::Fps24) == tc(1, 2, 3, 4));
    CHECK(*parse("05:06", FrameRate::Fps24) == tc(0, 0, 5, 6));
    CHECK(*parse("4:05:06", FrameRate::Fps24) == tc(0, 4, 5, 6));
    for (const char* text :
         {"", "1", "01:02:03:04:05", "01::03:04", "01:02:03:x4", "001:00:00:00"}) {
        CAPTURE(text);
        auto e = parse(text, FrameRate::Fps25);
        REQUIRE_FALSE(e);
        CHECK(e.error().code == "sync.timecode");
        CHECK_FALSE(e.error().hint.empty());
    }
    auto range = parse("00:00:00:25", FrameRate::Fps25);
    REQUIRE_FALSE(range);
    CHECK(range.error().hint.find("00..24") != std::string::npos);
    auto dropped = parse("00:01:00;00", FrameRate::Fps2997Drop);
    REQUIRE_FALSE(dropped);
    CHECK(dropped.error().hint.find("drop-frame") != std::string::npos);
    for (FrameRate r : kRates) {
        const Timecode label = fromFrames(1234567, r);
        CHECK(*parse(format(label, r), r) == label);
    }
}
