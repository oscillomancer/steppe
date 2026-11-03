// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// FLAC encoding (the format as the Xiph FLAC format specification, RFC 9639,
// describes it): an original encoder. Fixed-blocksize frames of
// FlacOptions::blockSize samples; per frame the cheapest of independent,
// left/side, right/side and mid/side stereo; per subframe the cheapest of
// constant, verbatim and the fixed predictors of order 0-4, residuals Rice
// coded in the best partition order (4- or 5-bit parameters). STREAMINFO
// carries the MD5 of the samples and the frame size range. Samples are
// quantized exactly as encodeWav quantizes them (the same TPDF dither stream),
// so a FLAC and a WAV of one render decode to the same values.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "metadata.h"
#include "quantize.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/md5.h"
#include "supple/base/rng.h"

namespace supple::audio {
namespace {

class BitWriter {
  public:
    std::vector<std::uint8_t> bytes;
    void bits(std::uint64_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bit((value >> i) & 1u);
    }
    void bit(std::uint64_t b) {
        acc_ = static_cast<std::uint8_t>((acc_ << 1) | (b & 1u));
        if (++filled_ == 8) {
            bytes.push_back(acc_);
            acc_ = 0;
            filled_ = 0;
        }
    }
    void unary(std::uint64_t zeros) {  // `zeros` zero bits, then a one
        for (std::uint64_t i = 0; i < zeros; ++i) bit(0);
        bit(1);
    }
    void align() {
        while (filled_ != 0) bit(0);
    }
    std::size_t size() const { return bytes.size(); }

  private:
    std::uint8_t acc_ = 0;
    int filled_ = 0;
};

std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = static_cast<std::uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ (p[i] << 8));
        for (int b = 0; b < 8; ++b)
            crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

std::uint64_t zigzag(std::int64_t r) {
    return r >= 0 ? static_cast<std::uint64_t>(r) << 1 : (static_cast<std::uint64_t>(-r) << 1) - 1;
}

// The fixed predictor's residual of `order` at sample n (n >= order).
std::int64_t residual(const std::int64_t* x, std::size_t n, int order) {
    switch (order) {
        case 0:
            return x[n];
        case 1:
            return x[n] - x[n - 1];
        case 2:
            return x[n] - 2 * x[n - 1] + x[n - 2];
        case 3:
            return x[n] - 3 * x[n - 1] + 3 * x[n - 2] - x[n - 3];
        default:
            return x[n] - 4 * x[n - 1] + 6 * x[n - 2] - 4 * x[n - 3] + x[n - 4];
    }
}

constexpr int kMaxPartitionOrder = 8;

// The best Rice coding of residuals u[order..n): partition order, a parameter
// per partition, the method (0: 4-bit parameters up to 14, 1: 5-bit up to 30)
// and the bits it takes (the 2+4 bit header included).
struct RicePlan {
    int partitionOrder = 0;
    int method = 0;
    std::vector<int> params;
    std::uint64_t bits = std::numeric_limits<std::uint64_t>::max();
};

RicePlan planRice(const std::vector<std::uint64_t>& u, std::size_t n, int order) {
    RicePlan best;
    for (int p = 0; p <= kMaxPartitionOrder; ++p) {
        const std::size_t parts = std::size_t{1} << p;
        if (n % parts != 0) break;
        const std::size_t span = n / parts;
        if (span <= static_cast<std::size_t>(order)) break;
        RicePlan plan;
        plan.partitionOrder = p;
        std::uint64_t total = 6;
        int maxParam = 0;
        for (std::size_t part = 0; part < parts; ++part) {
            const std::size_t from = part == 0 ? static_cast<std::size_t>(order) : part * span;
            const std::size_t to = (part + 1) * span;
            std::uint64_t sum = 0;
            for (std::size_t i = from; i < to; ++i) sum += u[i];
            const std::size_t count = to - from;
            // The optimum is near log2(mean); try that and its neighbours.
            const double mean = count ? static_cast<double>(sum) / static_cast<double>(count) : 0;
            const int guess = mean > 1 ? static_cast<int>(std::log2(mean)) : 0;
            std::uint64_t bestBits = std::numeric_limits<std::uint64_t>::max();
            int bestK = 0;
            for (int k = std::max(0, guess - 1); k <= std::min(30, guess + 1); ++k) {
                std::uint64_t b = count * static_cast<std::uint64_t>(k + 1);
                for (std::size_t i = from; i < to; ++i) b += u[i] >> k;
                if (b < bestBits) {
                    bestBits = b;
                    bestK = k;
                }
            }
            plan.params.push_back(bestK);
            maxParam = std::max(maxParam, bestK);
            total += bestBits;
        }
        plan.method = maxParam > 14 ? 1 : 0;
        total += parts * static_cast<std::uint64_t>(plan.method == 1 ? 5 : 4);
        plan.bits = total;
        if (plan.bits < best.bits) best = std::move(plan);
    }
    return best;
}

struct SubframePlan {
    enum class Kind { Constant, Verbatim, Fixed } kind = Kind::Verbatim;
    int order = 0;
    RicePlan rice;
    std::vector<std::uint64_t> u;  // zigzagged residuals (fixed)
    std::uint64_t bits = 0;
};

SubframePlan planSubframe(const std::vector<std::int64_t>& x, std::size_t n, int bps) {
    SubframePlan best;
    best.kind = SubframePlan::Kind::Verbatim;
    best.bits = 8 + n * static_cast<std::uint64_t>(bps);
    if (std::all_of(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n),
                    [&](std::int64_t v) { return v == x[0]; })) {
        best.kind = SubframePlan::Kind::Constant;
        best.bits = 8 + static_cast<std::uint64_t>(bps);
        return best;
    }
    std::vector<std::uint64_t> u(n);
    for (int order = 0; order <= 4; ++order) {
        if (static_cast<std::size_t>(order) >= n) break;
        bool fits = true;
        for (std::size_t i = static_cast<std::size_t>(order); i < n; ++i) {
            const std::int64_t r = residual(x.data(), i, order);
            if (r > std::numeric_limits<std::int32_t>::max() ||
                r < std::numeric_limits<std::int32_t>::min() + 1) {
                fits = false;  // a decoder keeps residuals in 32 bits
                break;
            }
            u[i] = zigzag(r);
        }
        if (!fits) continue;
        RicePlan rice = planRice(u, n, order);
        const std::uint64_t bits =
            8 + static_cast<std::uint64_t>(order) * static_cast<std::uint64_t>(bps) + rice.bits;
        if (bits < best.bits) {
            best.kind = SubframePlan::Kind::Fixed;
            best.order = order;
            best.rice = std::move(rice);
            best.u = u;
            best.bits = bits;
        }
    }
    return best;
}

void writeSubframe(BitWriter& w, const SubframePlan& plan, const std::vector<std::int64_t>& x,
                   std::size_t n, int bps) {
    auto sample = [&](std::int64_t v) {
        w.bits(static_cast<std::uint64_t>(v) & ((std::uint64_t{1} << bps) - 1), bps);
    };
    w.bit(0);  // padding
    switch (plan.kind) {
        case SubframePlan::Kind::Constant:
            w.bits(0, 6);
            w.bit(0);  // no wasted bits
            sample(x[0]);
            return;
        case SubframePlan::Kind::Verbatim:
            w.bits(1, 6);
            w.bit(0);
            for (std::size_t i = 0; i < n; ++i) sample(x[i]);
            return;
        case SubframePlan::Kind::Fixed:
            break;
    }
    w.bits(0b001000u | static_cast<unsigned>(plan.order), 6);
    w.bit(0);
    for (int i = 0; i < plan.order; ++i) sample(x[static_cast<std::size_t>(i)]);
    const RicePlan& rice = plan.rice;
    w.bits(static_cast<std::uint64_t>(rice.method), 2);
    w.bits(static_cast<std::uint64_t>(rice.partitionOrder), 4);
    const std::size_t parts = std::size_t{1} << rice.partitionOrder;
    const std::size_t span = n / parts;
    for (std::size_t part = 0; part < parts; ++part) {
        const int k = rice.params[part];
        w.bits(static_cast<std::uint64_t>(k), rice.method == 1 ? 5 : 4);
        const std::size_t from = part == 0 ? static_cast<std::size_t>(plan.order) : part * span;
        for (std::size_t i = from; i < (part + 1) * span; ++i) {
            w.unary(plan.u[i] >> k);
            if (k > 0) w.bits(plan.u[i] & ((std::uint64_t{1} << k) - 1), k);
        }
    }
}

// FLAC's "UTF-8" coding of the frame number.
void writeFrameNumber(BitWriter& w, std::uint64_t v) {
    if (v < 0x80) {
        w.bits(v, 8);
        return;
    }
    int bytes = 2;
    while (bytes < 7 && v >= (std::uint64_t{1} << (5 * bytes + 1))) ++bytes;
    const std::uint64_t lead = (0xFFu << (8 - bytes)) & 0xFFu;
    w.bits(lead | (v >> (6 * (bytes - 1))), 8);
    for (int i = bytes - 2; i >= 0; --i) w.bits(0x80u | ((v >> (6 * i)) & 0x3Fu), 8);
}

int blockSizeCode(std::size_t n, int& extraBits) {
    extraBits = 0;
    switch (n) {
        case 192:
            return 1;
        case 576:
            return 2;
        case 1152:
            return 3;
        case 2304:
            return 4;
        case 4608:
            return 5;
        default:
            break;
    }
    for (int code = 8; code <= 15; ++code)
        if (n == (std::size_t{256} << (code - 8))) return code;
    extraBits = n <= 256 ? 8 : 16;
    return n <= 256 ? 6 : 7;
}

int sampleSizeCode(int bps) {
    switch (bps) {
        case 8:
            return 1;
        case 12:
            return 2;
        case 16:
            return 4;
        case 20:
            return 5;
        case 24:
            return 6;
        case 32:
            return 7;
        default:
            return 0;  // from STREAMINFO
    }
}

}  // namespace

Result<std::vector<std::uint8_t>> encodeFlac(const AudioData& data, const FlacOptions& options,
                                             const EncodeExtras& extras) {
    const std::size_t channels = data.channelCount();
    const std::size_t frames = data.frames();
    const int bps = options.bitsPerSample;
    if (channels < 1 || channels > 8)
        return Error("audio.encode", "FLAC holds 1 to 8 channels", {}, "mix down first");
    if (bps != 16 && bps != 24)
        return Error("audio.encode", "FLAC bit depth must be 16 or 24", "/bit_depth");
    if (!std::isfinite(data.sampleRate) || data.sampleRate < 1 || data.sampleRate > 655350)
        return Error("audio.encode", "invalid sample rate for FLAC", {}, "use 1 .. 655350 Hz");
    for (const auto& channel : data.channels)
        if (channel.size() != frames)
            return Error("audio.encode", "channels have different lengths");
    const auto blockSize = static_cast<std::size_t>(std::clamp(options.blockSize, 16, 65535));

    // Quantize exactly as encodeWav does (one dither stream, frame by frame).
    std::vector<std::vector<std::int64_t>> pcm(channels, std::vector<std::int64_t>(frames));
    detail::Quantizer quantize(bps, options.dither, extras.shaped, options.ditherSeed, channels);
    Md5 md5;
    const std::size_t bytesPer = static_cast<std::size_t>(bps / 8);
    std::vector<std::uint8_t> md5Frame(channels * bytesPer);
    for (std::size_t i = 0; i < frames; ++i) {
        std::size_t w = 0;
        for (std::size_t c = 0; c < channels; ++c) {
            const auto q = static_cast<std::int64_t>(quantize(data.channels[c][i], c));
            pcm[c][i] = q;
            for (std::size_t b = 0; b < bytesPer; ++b)
                md5Frame[w++] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(q) >> (8 * b));
        }
        md5.update(md5Frame);
    }

    BitWriter out;
    out.bits(0x664C6143u, 32);  // "fLaC"
    // STREAMINFO (last metadata block, 34 bytes); frame sizes patched below.
    out.bit(1);
    out.bits(0, 7);
    out.bits(34, 24);
    const std::size_t info = out.size();
    out.bits(blockSize, 16);
    out.bits(blockSize, 16);
    out.bits(0, 24);  // min frame size
    out.bits(0, 24);  // max frame size
    out.bits(static_cast<std::uint64_t>(std::lround(data.sampleRate)), 20);
    out.bits(channels - 1, 3);
    out.bits(static_cast<std::uint64_t>(bps - 1), 5);
    out.bits(frames, 36);
    for (std::uint8_t byte : md5.finish()) out.bits(byte, 8);

    std::size_t minFrame = std::numeric_limits<std::size_t>::max(), maxFrame = 0;
    std::vector<std::int64_t> side(blockSize), mid(blockSize);
    std::vector<std::vector<std::int64_t>> block(channels, std::vector<std::int64_t>(blockSize));
    std::uint64_t frameNumber = 0;
    for (std::size_t start = 0; start < frames; start += blockSize, ++frameNumber) {
        const std::size_t n = std::min(blockSize, frames - start);
        for (std::size_t c = 0; c < channels; ++c)
            std::copy_n(pcm[c].begin() + static_cast<std::ptrdiff_t>(start), n, block[c].begin());
        // Channel assignment: 0..7 independent; stereo may decorrelate.
        int assignment = static_cast<int>(channels) - 1;
        std::vector<SubframePlan> plans;
        std::vector<const std::vector<std::int64_t>*> signals;
        std::vector<int> depths;
        if (channels == 2) {
            for (std::size_t i = 0; i < n; ++i) {
                side[i] = block[0][i] - block[1][i];
                mid[i] = (block[0][i] + block[1][i]) >> 1;
            }
            const SubframePlan l = planSubframe(block[0], n, bps);
            const SubframePlan r = planSubframe(block[1], n, bps);
            const SubframePlan s = planSubframe(side, n, bps + 1);
            const SubframePlan m = planSubframe(mid, n, bps);
            const std::uint64_t costs[4] = {l.bits + r.bits, l.bits + s.bits, s.bits + r.bits,
                                            m.bits + s.bits};
            const int pick =
                static_cast<int>(std::min_element(std::begin(costs), std::end(costs)) - costs);
            switch (pick) {
                case 0:
                    assignment = 1;
                    plans = {l, r};
                    signals = {&block[0], &block[1]};
                    depths = {bps, bps};
                    break;
                case 1:
                    assignment = 8;  // left/side
                    plans = {l, s};
                    signals = {&block[0], &side};
                    depths = {bps, bps + 1};
                    break;
                case 2:
                    assignment = 9;  // side/right
                    plans = {s, r};
                    signals = {&side, &block[1]};
                    depths = {bps + 1, bps};
                    break;
                default:
                    assignment = 10;  // mid/side
                    plans = {m, s};
                    signals = {&mid, &side};
                    depths = {bps, bps + 1};
                    break;
            }
        } else {
            for (std::size_t c = 0; c < channels; ++c) {
                plans.push_back(planSubframe(block[c], n, bps));
                signals.push_back(&block[c]);
                depths.push_back(bps);
            }
        }
        BitWriter f;
        f.bits(0x3FFE, 14);
        f.bit(0);  // reserved
        f.bit(0);  // fixed block size
        int extra = 0;
        const int sizeCode = blockSizeCode(n, extra);
        f.bits(static_cast<std::uint64_t>(sizeCode), 4);
        f.bits(0, 4);  // sample rate: from STREAMINFO
        f.bits(static_cast<std::uint64_t>(assignment), 4);
        f.bits(static_cast<std::uint64_t>(sampleSizeCode(bps)), 3);
        f.bit(0);
        writeFrameNumber(f, frameNumber);
        if (extra) f.bits(n - 1, extra);
        f.bits(crc8(f.bytes.data(), f.size()), 8);
        for (std::size_t c = 0; c < plans.size(); ++c)
            writeSubframe(f, plans[c], *signals[c], n, depths[c]);
        f.align();
        const std::uint16_t crc = crc16(f.bytes.data(), f.size());
        f.bits(crc, 16);
        minFrame = std::min(minFrame, f.size());
        maxFrame = std::max(maxFrame, f.size());
        out.bytes.insert(out.bytes.end(), f.bytes.begin(), f.bytes.end());
    }
    if (frames == 0) minFrame = 0;
    for (int b = 0; b < 3; ++b) {
        out.bytes[info + 4 + static_cast<std::size_t>(b)] =
            static_cast<std::uint8_t>(minFrame >> (8 * (2 - b)));
        out.bytes[info + 7 + static_cast<std::size_t>(b)] =
            static_cast<std::uint8_t>(maxFrame >> (8 * (2 - b)));
    }
    return detail::flacWithTags(std::move(out.bytes), extras.metadata.tags);
}

Status writeFlac(const std::filesystem::path& path, const AudioData& data,
                 const FlacOptions& options) {
    auto bytes = encodeFlac(data, options);
    if (!bytes) return std::move(bytes).error();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return writeFileAtomic(path, *bytes);
}

}  // namespace supple::audio
