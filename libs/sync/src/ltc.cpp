// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// LTC over libltc 1.3.2 (third_party/libltc-1.3.2, LGPL-3.0-or-later). Only
// the allocation-free calls run per block: ltc_encoder_encode_byte,
// ltc_encoder_get/set_frame, ltc_frame_set_parity, ltc_encoder_buffer_flush,
// ltc_decoder_write_float and ltc_decoder_read. libltc's label helpers
// (ltc_frame_to_time) format a time zone with sprintf, so labels are packed
// and unpacked here instead.
#include "supple/sync/ltc.h"

#include <ltc.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace supple::sync {
namespace {

LTC_TV_STANDARD standardOf(FrameRate rate) SUPPLE_RT {
    switch (rate) {
        case FrameRate::Fps25:
            return LTC_TV_625_50;
        case FrameRate::Fps23976:
        case FrameRate::Fps24:
            return LTC_TV_FILM_24;
        default:
            return LTC_TV_525_60;
    }
}

Error configError(std::string message, std::string hint) {
    return Error("sync.ltc.config", std::move(message), {}, std::move(hint));
}

// Speed factors libltc may stretch a byte by: its buffer holds one byte at
// the largest (sized at create time).
constexpr double kMaxStretch = 8.0;

}  // namespace

// --- Encoder ----------------------------------------------------------------

struct LtcEncoder::Impl {
    LtcEncoderConfig config;
    ::LTCEncoder* encoder = nullptr;
    LTC_TV_STANDARD standard = LTC_TV_625_50;
    double byteSamples = 0.0;  // samples per byte at speed 1
    double gain = 0.0;
    bool running = false;
    bool waiting = false;  // for the first frame boundary
    std::int64_t frame = 0;
    int byte = 0;
    ltcsnd_sample_t* pending = nullptr;
    int pendingSize = 0;
    int pendingAt = 0;

    ~Impl() {
        if (encoder) ltc_encoder_free(encoder);
    }

    void setFrame() SUPPLE_RT {
        const Timecode tc = fromFrames(frame, config.mapping.rate);
        LTCFrame f;
        ltc_encoder_get_frame(encoder, &f);
        f.frame_units = static_cast<unsigned>(tc.frames % 10);
        f.frame_tens = static_cast<unsigned>(tc.frames / 10);
        f.secs_units = static_cast<unsigned>(tc.seconds % 10);
        f.secs_tens = static_cast<unsigned>(tc.seconds / 10);
        f.mins_units = static_cast<unsigned>(tc.minutes % 10);
        f.mins_tens = static_cast<unsigned>(tc.minutes / 10);
        f.hours_units = static_cast<unsigned>(tc.hours % 10);
        f.hours_tens = static_cast<unsigned>(tc.hours / 10);
        // libltc flags every 29.97 as drop-frame; only 29.97df is.
        f.dfbit = rateInfo(config.mapping.rate).drop ? 1u : 0u;
        ltc_frame_set_parity(&f, standard);
        ltc_encoder_set_frame(encoder, &f);
    }
};

LtcEncoder::LtcEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LtcEncoder::LtcEncoder(LtcEncoder&&) noexcept = default;
LtcEncoder& LtcEncoder::operator=(LtcEncoder&&) noexcept = default;
LtcEncoder::~LtcEncoder() = default;

Result<LtcEncoder> LtcEncoder::create(const LtcEncoderConfig& config) {
    if (!(config.sampleRate >= 8000.0 && config.sampleRate <= 768000.0))
        return configError(
            "LTC sample rate " + std::to_string(config.sampleRate) + " is out of range.",
            "Use a sample rate from 8000 to 768000 Hz.");
    if (!(config.levelDb <= 0.0 && config.levelDb >= -60.0))
        return configError("LTC level " + std::to_string(config.levelDb) + " dBFS is out of range.",
                           "Use a level from -60 to 0 dBFS.");
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->standard = standardOf(config.mapping.rate);
    const double fps = rateInfo(config.mapping.rate).fps();
    impl->encoder = ltc_encoder_create(config.sampleRate, fps, impl->standard, 0);
    if (!impl->encoder ||
        // Room for one byte at the largest stretch, with margin: a whole frame
        // at kMaxStretch.
        ltc_encoder_set_buffersize(impl->encoder, config.sampleRate, fps / kMaxStretch) != 0 ||
        ltc_encoder_set_volume(impl->encoder, 0.0) != 0)
        return Error("sync.ltc.alloc", "libltc could not create an LTC encoder.", {},
                     "Check available memory.");
    impl->byteSamples = config.sampleRate / (fps * 10.0);
    impl->gain = std::pow(10.0, config.levelDb / 20.0);
    return LtcEncoder(std::move(impl));
}

const LtcEncoderConfig& LtcEncoder::config() const SUPPLE_RT { return impl_->config; }
bool LtcEncoder::running() const SUPPLE_RT { return impl_->running; }
std::int64_t LtcEncoder::frame() const SUPPLE_RT { return impl_->frame; }

void LtcEncoder::start(double timelineSeconds) SUPPLE_RT {
    Impl& m = *impl_;
    m.running = true;
    m.waiting = true;
    m.byte = 0;
    m.pendingSize = m.pendingAt = 0;
    m.frame =
        static_cast<std::int64_t>(std::ceil(m.config.mapping.framesAt(timelineSeconds) - 1e-6));
}

void LtcEncoder::stop() SUPPLE_RT {
    impl_->running = false;
    impl_->pendingSize = impl_->pendingAt = 0;
}

void LtcEncoder::render(double from, std::span<float> out, const PositionMap& seconds) SUPPLE_RT {
    Impl& m = *impl_;
    const auto n = out.size();
    const double to = from + static_cast<double>(n);
    const TimecodeMapping& map = m.config.mapping;
    const double firstPos = seconds.positionAt(from);
    const double lastPos = seconds.positionAt(to);
    const double frameSeconds = toSeconds(1.0, map.rate);
    // Device frame of timeline position `p`, extrapolated past the block.
    auto deviceFrame = [&](double p, double lo) SUPPLE_RT {
        if (p <= lastPos) return seconds.frameAt(p, lo, to);
        double slope = n > 0 ? (lastPos - firstPos) / static_cast<double>(n) : 0.0;
        if (!(slope > 0.0)) slope = 1.0 / m.config.sampleRate;
        return to + (p - lastPos) / slope;
    };
    std::size_t at = 0;
    while (at < n) {
        if (m.pendingAt < m.pendingSize) {
            const auto count = std::min<std::size_t>(
                n - at, static_cast<std::size_t>(m.pendingSize - m.pendingAt));
            for (std::size_t i = 0; i < count; ++i) {
                const int v = m.pending[m.pendingAt + static_cast<int>(i)];
                out[at + i] = static_cast<float>((v - 128) / 127.0 * m.gain);
            }
            at += count;
            m.pendingAt += static_cast<int>(count);
            continue;
        }
        const double s = from + static_cast<double>(at);
        if (!m.running) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(at), out.end(), 0.0f);
            return;
        }
        if (!m.waiting) {
            // A jump the caller did not announce: restart at the next boundary.
            const double expected =
                map.timelineSeconds(static_cast<double>(m.frame) + m.byte / 10.0);
            const double here = at == 0 ? firstPos : seconds.positionAt(s);
            if (std::abs(here - expected) > frameSeconds) {
                m.waiting = true;
                m.byte = 0;
                m.frame = static_cast<std::int64_t>(std::ceil(map.framesAt(here) - 1e-6));
            }
        }
        if (m.waiting) {
            const double p = map.timelineSeconds(static_cast<double>(m.frame));
            if (p > lastPos) {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(at), out.end(), 0.0f);
                return;
            }
            const double here = at == 0 ? firstPos : seconds.positionAt(s);
            if (p < here - 1e-6 * frameSeconds) {
                // Already past that boundary: wait for the next one (always
                // a later frame, so this ends).
                m.frame = std::max(m.frame + 1,
                                   static_cast<std::int64_t>(std::ceil(map.framesAt(here) - 1e-6)));
                continue;
            }
            const double begin = p <= here ? s : std::round(seconds.frameAt(p, s, to));
            if (begin > s) {
                const auto gap = std::min<std::size_t>(n - at, static_cast<std::size_t>(begin - s));
                std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(at), gap, 0.0f);
                at += gap;
                continue;
            }
            m.waiting = false;
            m.byte = 0;
            ltc_encoder_reset(m.encoder);
            m.setFrame();
        }
        // Encode the next byte so that it ends where the map says.
        const double end =
            deviceFrame(map.timelineSeconds(static_cast<double>(m.frame) + (m.byte + 1) / 10.0), s);
        const double stretch =
            std::clamp((end - s) / m.byteSamples, 1.0 / kMaxStretch, kMaxStretch);
        ltc_encoder_buffer_flush(m.encoder);
        ltc_encoder_encode_byte(m.encoder, m.byte, stretch);
        m.pendingSize = ltc_encoder_get_bufferptr(m.encoder, &m.pending, 1);
        m.pendingAt = 0;
        if (++m.byte == 10) {
            m.byte = 0;
            ++m.frame;
            m.setFrame();
        }
        if (m.pendingSize <= 0) {
            // Cannot happen with the buffer sized at create time; never spin.
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(at), out.end(), 0.0f);
            return;
        }
    }
}

// --- Decoder ----------------------------------------------------------------

struct LtcDecoder::Impl {
    LtcDecoderConfig config;
    ::LTCDecoder* decoder = nullptr;
    LtcFrame* frames = nullptr;  // queueFrames entries
    std::unique_ptr<LtcFrame[]> storage;
    int count = 0;
    bool present = false;
    bool locked = false;
    bool havePrevious = false;
    int direction = 0;
    int consistent = 0;
    FrameRate rate = FrameRate::Fps25;
    bool rateMeasured = false;
    double lastSeen = 0.0;
    std::int64_t previous = 0;
    // A run of forward frames, one on from the next: its first frame's count
    // and start sample measure the frame length.
    std::int64_t runCount = 0;
    double runStart = 0.0;
    double measured = 0.0;  // samples per frame, 0 before a run of two
    LtcFrame latest{};
    std::uint32_t jumps = 0;

    ~Impl() {
        if (decoder) ltc_decoder_free(decoder);
    }

    void lose() SUPPLE_RT {
        present = false;
        locked = false;
        havePrevious = false;
        direction = 0;
        consistent = 0;
        measured = 0.0;
        if (!config.expected) rateMeasured = false;
        rate = config.expected ? *config.expected : FrameRate::Fps25;
    }

    // The nearest rate to `fps` whose labels reach `maxLabel`; 23.976 and
    // 29.97 non-drop only when `fine` (two seconds measured).
    static FrameRate nearest(double fps, int maxLabel, bool fine) SUPPLE_RT {
        constexpr FrameRate kCandidates[] = {FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps30,
                                             FrameRate::Fps23976, FrameRate::Fps2997};
        FrameRate best = FrameRate::Fps30;
        double distance = 1e300;
        for (FrameRate r : kCandidates) {
            if (rateInfo(r).nominal <= maxLabel) continue;
            if (!fine && (r == FrameRate::Fps23976 || r == FrameRate::Fps2997)) continue;
            const double d = std::abs(fps - rateInfo(r).fps());
            if (d < distance) {
                distance = d;
                best = r;
            }
        }
        return best;
    }

    FrameRate resolve(bool drop, int label, double frameLength) const SUPPLE_RT {
        if (drop) return FrameRate::Fps2997Drop;
        // Until decided: the integer rate nearest the run's mean frame length,
        // or this frame's own length.
        const double length = measured > 0.0 ? measured : frameLength;
        const double fps = config.sampleRate / std::max(length, 1.0);
        const FrameRate integer = nearest(fps, label, false);
        // The expected rate holds (it settles 23.976/24 and 29.97/30, and a
        // varispeed source's frames match no standard length) unless the
        // frames are within 3% of another family's: that source keeps its own.
        if (config.expected && !rateInfo(*config.expected).drop) {
            const auto& e = rateInfo(*config.expected);
            const auto& i = rateInfo(integer);
            if (i.nominal == e.nominal || std::abs(fps - i.fps()) > 0.03 * i.fps() ||
                i.nominal <= label)
                return *config.expected;
        }
        if (rateMeasured && rate != FrameRate::Fps2997Drop && rateInfo(rate).nominal > label &&
            rateInfo(rate).nominal == rateInfo(integer).nominal)
            return rate;
        return integer;
    }

    void accept(const LTCFrameExt& ext) SUPPLE_RT {
        const LTCFrame& f = ext.ltc;
        Timecode tc;
        tc.frames = static_cast<int>(f.frame_units + 10 * f.frame_tens);
        tc.seconds = static_cast<int>(f.secs_units + 10 * f.secs_tens);
        tc.minutes = static_cast<int>(f.mins_units + 10 * f.mins_tens);
        tc.hours = static_cast<int>(f.hours_units + 10 * f.hours_tens);
        const bool drop = f.dfbit != 0;
        const bool reverse = ext.reverse != 0;
        present = true;
        lastSeen = static_cast<double>(ext.off_end);

        const FrameRate next =
            resolve(drop, tc.frames, static_cast<double>(ext.off_end - ext.off_start + 1));
        const std::int64_t c = toFrames(tc, next);
        const int step = reverse ? -1 : 1;
        const std::int64_t day = framesPerDay(next);
        // Same labels per second (23.976 and 24, 29.97 and 30) count alike.
        const bool sameCount = rateInfo(next).nominal == rateInfo(rate).nominal &&
                               rateInfo(next).drop == rateInfo(rate).drop;
        const bool follows =
            havePrevious && sameCount && ((c - previous - step) % day + day) % day == 0;
        if (follows) {
            ++consistent;
            if (!reverse && c > runCount) {
                // The mean length since the run began: its error shrinks with it.
                measured = (static_cast<double>(ext.off_start) - runStart) /
                           static_cast<double>(c - runCount);
            }
        } else {
            // A provisional rate refined is not a jump in the source.
            if (havePrevious && sameCount) ++jumps;
            consistent = 1;
            locked = false;
            measured = 0.0;
        }
        if (!follows || reverse) {
            runCount = c;
            runStart = static_cast<double>(ext.off_start);
        }
        if (consistent >= config.lockFrames) locked = true;
        direction = step;
        rate = next;
        if (follows && !rateMeasured && !drop && measured > 0.0 &&
            c - runCount >= static_cast<std::int64_t>(2.0 * rateInfo(next).fps())) {
            // Two seconds of frames decide 23.976/24 and 29.97/30, once.
            rateMeasured = true;
            rate = nearest(config.sampleRate / measured, tc.frames, true);
        }
        if (drop) rateMeasured = true;
        previous = c;
        havePrevious = true;

        LtcFrame& out = frames[count++];
        out.timecode = tc;
        out.count = c;
        out.start = static_cast<double>(ext.off_start);
        out.end = static_cast<double>(ext.off_end);
        out.reverse = reverse;
        out.dropFlag = drop;
        out.levelDb = ext.volume;
        out.observation.frame = static_cast<double>(ext.off_end) + 1.0;
        out.observation.position = toSeconds(static_cast<double>(reverse ? c : c + 1), rate);
        latest = out;
    }
};

LtcDecoder::LtcDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LtcDecoder::LtcDecoder(LtcDecoder&&) noexcept = default;
LtcDecoder& LtcDecoder::operator=(LtcDecoder&&) noexcept = default;
LtcDecoder::~LtcDecoder() = default;

Result<LtcDecoder> LtcDecoder::create(const LtcDecoderConfig& config) {
    if (!(config.sampleRate >= 8000.0 && config.sampleRate <= 768000.0))
        return configError(
            "LTC sample rate " + std::to_string(config.sampleRate) + " is out of range.",
            "Use a sample rate from 8000 to 768000 Hz.");
    if (config.queueFrames < 1 || config.queueFrames > 1024)
        return configError(
            "LTC queue of " + std::to_string(config.queueFrames) + " frames is out of range.",
            "Use a queue of 1 to 1024 frames.");
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    if (config.expected) {
        impl->rate = *config.expected;
        impl->rateMeasured = true;
    }
    // libltc tracks the bit period from its first transitions; 25 fps is the
    // middle of the range it adapts over.
    const int samplesPerFrame = static_cast<int>(config.sampleRate / 25.0);
    // One more than the frames returned per call: libltc's ring keeps one free.
    impl->decoder = ltc_decoder_create(samplesPerFrame, config.queueFrames + 1);
    impl->storage = std::make_unique<LtcFrame[]>(static_cast<std::size_t>(config.queueFrames));
    impl->frames = impl->storage.get();
    if (!impl->decoder)
        return Error("sync.ltc.alloc", "libltc could not create an LTC decoder.", {},
                     "Check available memory.");
    return LtcDecoder(std::move(impl));
}

std::span<const LtcFrame> LtcDecoder::feed(double frame, std::span<const float> samples) SUPPLE_RT {
    Impl& m = *impl_;
    m.count = 0;
    if (!samples.empty()) {
        // libltc reads the samples only; its signature is not const.
        ltc_decoder_write_float(m.decoder, const_cast<float*>(samples.data()), samples.size(),
                                static_cast<ltc_off_t>(std::llround(frame)));
    }
    LTCFrameExt ext;
    while (m.count < m.config.queueFrames && ltc_decoder_read(m.decoder, &ext) == 1) m.accept(ext);
    update(frame + static_cast<double>(samples.size()));
    return {m.frames, static_cast<std::size_t>(m.count)};
}

void LtcDecoder::update(double frame) SUPPLE_RT {
    Impl& m = *impl_;
    if (m.present && frame - m.lastSeen > m.config.timeoutSeconds * m.config.sampleRate) m.lose();
}

void LtcDecoder::reset() SUPPLE_RT {
    Impl& m = *impl_;
    ltc_decoder_queue_flush(m.decoder);
    m.lose();
    m.jumps = 0;
    m.latest = {};
}

bool LtcDecoder::present() const SUPPLE_RT { return impl_->present; }
bool LtcDecoder::locked() const SUPPLE_RT { return impl_->locked; }
int LtcDecoder::direction() const SUPPLE_RT { return impl_->direction; }
FrameRate LtcDecoder::rate() const SUPPLE_RT { return impl_->rate; }
bool LtcDecoder::rateMeasured() const SUPPLE_RT { return impl_->rateMeasured; }
double LtcDecoder::measuredFps() const SUPPLE_RT {
    return impl_->measured > 0.0 ? impl_->config.sampleRate / impl_->measured : 0.0;
}
Timecode LtcDecoder::timecode() const SUPPLE_RT { return impl_->latest.timecode; }
Observation LtcDecoder::lastObservation() const SUPPLE_RT { return impl_->latest.observation; }
std::uint32_t LtcDecoder::jumps() const SUPPLE_RT { return impl_->jumps; }

}  // namespace supple::sync
