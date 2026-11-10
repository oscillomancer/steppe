// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/rng.h>
#include <supple/sync/ltc.h>

#include <array>
#include <cmath>
#include <vector>

using namespace supple;
using namespace supple::sync;

namespace {

Timecode tc(int h, int m, int s, int f) { return Timecode{h, m, s, f}; }

constexpr std::array<FrameRate, 6> kRates{FrameRate::Fps23976, FrameRate::Fps24,
                                          FrameRate::Fps25,    FrameRate::Fps2997Drop,
                                          FrameRate::Fps2997,  FrameRate::Fps30};

struct Signal {
    double sampleRate = 48000.0;
    double noise = 0.0;     // white noise peak, linear
    double gainFrom = 1.0;  // linear gain ramp over the signal
    double gainTo = 1.0;
    std::size_t block = 512;
};

struct RoundTrip {
    std::vector<LtcFrame> frames;
    double worstObservation = 0.0;  // seconds, against the map
    double worstStart = 0.0;        // samples, frame starts against the map
    FrameRate rate = FrameRate::Fps25;
    bool rateMeasured = false;
    bool locked = false;
    std::uint32_t jumps = 0;
    int direction = 0;
};

// Encodes `seconds` of LTC through `map` (device frames to timeline
// seconds), shaped by `signal`, and decodes it.
RoundTrip roundTrip(FrameRate rate, const PositionMap& map, double seconds, const Signal& signal,
                    LtcDecoderConfig decoderConfig = {}) {
    LtcEncoderConfig ec;
    ec.sampleRate = signal.sampleRate;
    ec.mapping = TimecodeMapping{rate, tc(1, 0, 0, 0)};
    auto encoder = LtcEncoder::create(ec);
    REQUIRE(encoder);
    decoderConfig.sampleRate = signal.sampleRate;
    auto decoder = LtcDecoder::create(decoderConfig);
    REQUIRE(decoder);
    encoder->start(map.positionAt(0.0));
    Rng rng(11);
    RoundTrip r;
    const auto total = static_cast<std::size_t>(seconds * signal.sampleRate);
    std::vector<float> buffer(signal.block);
    for (std::size_t at = 0; at < total; at += signal.block) {
        const double from = static_cast<double>(at);
        encoder->render(from, buffer, map);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            const double x = static_cast<double>(at + i) / static_cast<double>(total);
            const double gain = signal.gainFrom + (signal.gainTo - signal.gainFrom) * x;
            buffer[i] = static_cast<float>(static_cast<double>(buffer[i]) * gain +
                                           signal.noise * static_cast<double>(rng.bipolar()));
        }
        for (const LtcFrame& f : decoder->feed(from, buffer)) {
            r.frames.push_back(f);
            const double sent =
                toSeconds(ec.mapping.framesAt(map.positionAt(f.observation.frame)), rate);
            // Before the rate is decided, 23.976 and 29.97 read as 24 and 30.
            if (decoder->locked() && decoder->rateMeasured()) {
                r.worstObservation =
                    std::max(r.worstObservation, std::abs(f.observation.position - sent));
                const double startTruth =
                    map.frameAt(ec.mapping.timelineSeconds(static_cast<double>(f.count)), 0.0,
                                from + static_cast<double>(signal.block));
                r.worstStart = std::max(r.worstStart, std::abs(f.start - startTruth));
            }
        }
    }
    r.rate = decoder->rate();
    r.rateMeasured = decoder->rateMeasured();
    r.locked = decoder->locked();
    r.jumps = decoder->jumps();
    r.direction = decoder->direction();
    return r;
}

// Counts frames that are not one on from the previous.
int breaks(const std::vector<LtcFrame>& frames) {
    int n = 0;
    for (std::size_t i = 1; i < frames.size(); ++i)
        if (frames[i].count != frames[i - 1].count + 1) {
            ++n;
            MESSAGE("break at ", i, ": ", frames[i - 1].count, " -> ", frames[i].count, " start ",
                    frames[i].start);
        }
    return n;
}

}  // namespace

TEST_CASE("ltc: encode and decode at every sample rate and frame rate") {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (FrameRate rate : kRates) {
            CAPTURE(sr);
            CAPTURE(rateName(rate));
            Signal s;
            s.sampleRate = sr;
            const LinearMap map(0.0, 0.0, 1.0 / sr);
            const RoundTrip r = roundTrip(rate, map, 3.0, s);
            const double fps = rateInfo(rate).fps();
            MESSAGE(sr, " Hz ", rateName(rate), ": ", r.frames.size(),
                    " frames, worst observation ", r.worstObservation * 1000.0, " ms, worst start ",
                    r.worstStart, " samples");
            REQUIRE(r.frames.size() >= static_cast<std::size_t>(3.0 * fps) - 3);
            CHECK(breaks(r.frames) == 0);
            CHECK(r.frames.front().count == toFrames(tc(1, 0, 0, 0), rate));
            CHECK(r.frames.front().timecode == tc(1, 0, 0, 0));
            for (const LtcFrame& f : r.frames) CHECK(f.timecode == fromFrames(f.count, rate));
            CHECK(r.frames.back().dropFlag == rateInfo(rate).drop);
            CHECK(r.locked);
            CHECK(r.rateMeasured);
            CHECK(r.rate == rate);
            CHECK(r.jumps == 0);
            CHECK(r.direction == 1);
            CHECK(r.worstObservation < 0.0005);
            CHECK(r.worstStart < 0.0005 * sr);
        }
    }
}

TEST_CASE("ltc: noise, level changes and a slow start") {
    const double sr = 48000.0;
    const LinearMap map(0.0, 0.0, 1.0 / sr);
    for (FrameRate rate : {FrameRate::Fps25, FrameRate::Fps2997Drop, FrameRate::Fps24}) {
        CAPTURE(rateName(rate));
        Signal s;
        s.noise = 0.005;   // white noise peaking at -46 dBFS
        s.gainFrom = 0.2;  // LTC from -32 dBFS (14 dB above the noise) rising 20 dB
        s.gainTo = 2.0;
        LtcDecoderConfig config;
        config.expected = rate;
        const RoundTrip r = roundTrip(rate, map, 4.0, s, config);
        REQUIRE(r.frames.size() >= static_cast<std::size_t>(4.0 * rateInfo(rate).fps()) - 4);
        CHECK(breaks(r.frames) == 0);
        CHECK(r.locked);
        CHECK(r.worstObservation < 0.0005);
    }
}

TEST_CASE("ltc: the encoder follows the playrate and a tempo-mapped clock") {
    const double sr = 48000.0;
    for (double speed : {0.5, 0.8, 1.25, 2.0}) {
        CAPTURE(speed);
        const LinearMap map(0.0, 10.0, speed / sr);
        Signal s;
        LtcDecoderConfig config;
        config.expected = FrameRate::Fps25;
        const RoundTrip r = roundTrip(FrameRate::Fps25, map, 3.0, s, config);
        REQUIRE(r.frames.size() >= static_cast<std::size_t>(3.0 * 25.0 * speed) - 3);
        CHECK(breaks(r.frames) == 0);
        // libltc adapts to a bit rate far from its guess within a frame.
        const std::int64_t first = toFrames(tc(1, 0, 10, 0), FrameRate::Fps25);
        CHECK(r.frames.front().count >= first);
        CHECK(r.frames.front().count <= first + 1);
        // Frame starts land where the map puts them, to a few samples.
        CHECK(r.worstStart < 3.0);
        CHECK(r.worstObservation < 0.0005 * speed);
    }
}

TEST_CASE("ltc: block size does not change the signal") {
    const double sr = 44100.0;
    LtcEncoderConfig ec;
    ec.sampleRate = sr;
    ec.mapping = TimecodeMapping{FrameRate::Fps2997Drop, tc(0, 9, 59, 0)};
    const LinearMap map(0.0, 0.123, 1.0 / sr);
    std::vector<std::vector<float>> outputs;
    for (std::size_t block :
         {std::size_t{64}, std::size_t{333}, std::size_t{4096}, std::size_t{44100}}) {
        auto e = LtcEncoder::create(ec);
        REQUIRE(e);
        e->start(map.positionAt(0.0));
        std::vector<float> all(static_cast<std::size_t>(sr) * 2);
        for (std::size_t at = 0; at < all.size(); at += block) {
            const std::size_t n = std::min(block, all.size() - at);
            e->render(static_cast<double>(at), std::span(all).subspan(at, n), map);
        }
        outputs.push_back(std::move(all));
    }
    for (std::size_t i = 1; i < outputs.size(); ++i) CHECK(outputs[i] == outputs[0]);
    // The level is the configured peak.
    float peak = 0.0f;
    for (float v : outputs[0]) peak = std::max(peak, std::abs(v));
    CHECK(20.0 * std::log10(static_cast<double>(peak)) == doctest::Approx(-18.0).epsilon(0.01));
}

TEST_CASE("ltc: locate, unannounced jumps, stop and dropout") {
    const double sr = 48000.0;
    LtcEncoderConfig ec;
    ec.sampleRate = sr;
    ec.mapping = TimecodeMapping{FrameRate::Fps25, tc(0, 0, 0, 0)};
    auto e = LtcEncoder::create(ec);
    REQUIRE(e);
    LtcDecoderConfig dc;
    dc.sampleRate = sr;
    auto d = LtcDecoder::create(dc);
    REQUIRE(d);
    std::vector<float> block(480);
    std::vector<LtcFrame> seen;
    auto play = [&](const PositionMap& map, double from, double to) {
        for (double f = from; f < to; f += 480.0) {
            e->render(f, block, map);
            for (const LtcFrame& x : d->feed(f, block)) seen.push_back(x);
        }
    };
    const LinearMap a(0.0, 0.0, 1.0 / sr);
    e->start(0.0);
    play(a, 0.0, sr);
    CHECK(d->locked());
    CHECK(d->jumps() == 0);
    // Announced locate to 100 s.
    const LinearMap b(sr, 100.0, 1.0 / sr);
    e->locate(100.0);
    play(b, sr, 2.0 * sr);
    CHECK(d->jumps() == 1);
    CHECK(d->locked());
    CHECK(d->timecode().seconds >= 40);  // 00:01:40:xx
    // A jump the encoder was not told about restarts at a frame boundary.
    const LinearMap c(2.0 * sr, 200.0, 1.0 / sr);
    play(c, 2.0 * sr, 3.0 * sr);
    CHECK(d->jumps() == 2);
    CHECK(d->locked());
    CHECK(seen.back().count ==
          static_cast<std::int64_t>(std::floor(c.positionAt(seen.back().end) * 25.0)));
    // Stop: silence, and the decoder loses the signal after its timeout.
    e->stop();
    play(c, 3.0 * sr, 3.5 * sr);
    CHECK_FALSE(d->present());
    CHECK_FALSE(d->locked());
    CHECK(d->direction() == 0);
}

TEST_CASE("ltc: a first render past the named boundary starts at the next one") {
    const double sr = 48000.0;
    LtcEncoderConfig ec;
    ec.sampleRate = sr;
    ec.mapping = TimecodeMapping{FrameRate::Fps25, tc(0, 0, 0, 0)};
    auto e = LtcEncoder::create(ec);
    REQUIRE(e);
    e->start(1.0);                              // frame 25 at 1.0 s
    const LinearMap late(0.0, 1.01, 1.0 / sr);  // but the transport is at 1.01 s
    std::vector<float> out(static_cast<std::size_t>(sr));
    e->render(0.0, out, late);
    // Silence up to frame 26's boundary at 1.04 s (sample 1440), then LTC.
    for (std::size_t i = 0; i < 1440; ++i) CHECK(out[i] == 0.0f);
    CHECK(out[1440] != 0.0f);
    LtcDecoderConfig dc;
    dc.sampleRate = sr;
    dc.expected = FrameRate::Fps25;
    auto d = LtcDecoder::create(dc);
    REQUIRE(d);
    const auto frames = d->feed(0.0, out);
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.front().count == 26);
    // libltc places the very first frame's start to within a bit (24 samples).
    CHECK(std::abs(frames.front().start - 1440.0) <= 24.0);
}

TEST_CASE("ltc: positions a hair past a boundary terminate") {
    LtcEncoderConfig ec;
    ec.mapping = TimecodeMapping{FrameRate::Fps2997Drop, tc(0, 0, 0, 0)};
    auto e = LtcEncoder::create(ec);
    REQUIRE(e);
    std::vector<float> out(64);
    for (double past : {0.0, 1e-12, 1e-9, 3e-8, 1e-7, 1e-5}) {
        const double at = 100.0 * 1001.0 / 30000.0 + past;
        e->start(at - 1e-3);
        e->render(0.0, out, LinearMap(0.0, at, 1.0 / 48000.0));
        CHECK(e->frame() >= 100);
        CHECK(e->frame() <= 101);
    }
}

TEST_CASE("ltc: reverse playback decodes backwards") {
    // Encode forwards, then feed the decoder the samples reversed.
    const double sr = 48000.0;
    LtcEncoderConfig ec;
    ec.sampleRate = sr;
    ec.mapping = TimecodeMapping{FrameRate::Fps25, tc(0, 0, 0, 0)};
    auto e = LtcEncoder::create(ec);
    REQUIRE(e);
    const LinearMap map(0.0, 10.0, 1.0 / sr);
    e->start(10.0);
    std::vector<float> forward(static_cast<std::size_t>(sr) * 2);
    e->render(0.0, forward, map);
    std::vector<float> backward(forward.rbegin(), forward.rend());
    LtcDecoderConfig dc;
    dc.sampleRate = sr;
    dc.expected = FrameRate::Fps25;
    auto d = LtcDecoder::create(dc);
    REQUIRE(d);
    std::vector<LtcFrame> seen;
    for (std::size_t at = 0; at < backward.size(); at += 512) {
        const std::size_t n = std::min<std::size_t>(512, backward.size() - at);
        for (const LtcFrame& x :
             d->feed(static_cast<double>(at), std::span(backward).subspan(at, n)))
            seen.push_back(x);
    }
    REQUIRE(seen.size() > 40);
    CHECK(d->direction() == -1);
    CHECK(d->locked());
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].reverse);
        CHECK(seen[i].count == seen[i - 1].count - 1);
    }
}

TEST_CASE("ltc: configuration errors") {
    LtcEncoderConfig ec;
    ec.levelDb = 3.0;
    auto e = LtcEncoder::create(ec);
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "sync.ltc.config");
    CHECK_FALSE(e.error().hint.empty());
    ec.levelDb = -18.0;
    ec.sampleRate = 100.0;
    CHECK_FALSE(LtcEncoder::create(ec));
    LtcDecoderConfig dc;
    dc.queueFrames = 0;
    auto d = LtcDecoder::create(dc);
    REQUIRE_FALSE(d);
    CHECK(d.error().code == "sync.ltc.config");
}

namespace {
struct RtHarness {
    LtcEncoder& encoder;
    LtcDecoder& decoder;
    std::span<float> block;
    int frames = 0;
    void run(double from, const PositionMap& map) SUPPLE_RT {
        if (!encoder.running()) encoder.start(map.positionAt(from));
        encoder.render(from, block, map);
        frames += static_cast<int>(decoder.feed(from, block).size());
        decoder.update(from + static_cast<double>(block.size()));
    }
};
}  // namespace

TEST_CASE("ltc: per-block calls are real-time safe") {
    LtcEncoderConfig ec;
    ec.mapping = TimecodeMapping{FrameRate::Fps30, tc(1, 0, 0, 0)};
    auto e = LtcEncoder::create(ec);
    LtcDecoderConfig dc;
    auto d = LtcDecoder::create(dc);
    REQUIRE(e);
    REQUIRE(d);
    std::vector<float> block(256);
    RtHarness h{*e, *d, block};
    const LinearMap map(0.0, 0.0, 1.1 / 48000.0);
    for (double f = 0.0; f < 48000.0; f += 256.0) h.run(f, map);
    CHECK(h.frames > 25);
    CHECK(d->locked());
}
