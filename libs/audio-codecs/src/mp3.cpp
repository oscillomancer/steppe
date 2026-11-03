// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MP3 through LAME 3.100 (third_party/lame-3.100, LGPL-2.0-or-later, the
// approved exception of ADR-0055 §8; this library is linked only into the DAW).
#include "supple/codecs/codecs.h"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>

#include "lame.h"

namespace supple::codecs {
namespace {

// UTF-8 to NUL-terminated UTF-16 with a byte-order mark, as LAME's
// id3tag_*_utf16 functions take it. Malformed bytes become U+FFFD.
std::vector<unsigned short> utf16(const std::string& text) {
    std::vector<unsigned short> out{0xFEFF};
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0xFFFD;
        std::size_t length = 1;
        if (c < 0x80)
            code = c;
        else if ((c & 0xE0) == 0xC0)
            code = c & 0x1Fu, length = 2;
        else if ((c & 0xF0) == 0xE0)
            code = c & 0x0Fu, length = 3;
        else if ((c & 0xF8) == 0xF0)
            code = c & 0x07u, length = 4;
        if (length > 1) {
            if (i + length > text.size()) {
                code = 0xFFFD;
                length = 1;
            } else
                for (std::size_t k = 1; k < length; ++k) {
                    const auto next = static_cast<unsigned char>(text[i + k]);
                    if ((next & 0xC0) != 0x80) {
                        code = 0xFFFD;
                        length = k;
                        break;
                    }
                    code = (code << 6) | (next & 0x3Fu);
                }
        }
        i += length;
        if (code >= 0x10000 && code <= 0x10FFFF) {
            code -= 0x10000;
            out.push_back(static_cast<unsigned short>(0xD800 + (code >> 10)));
            out.push_back(static_cast<unsigned short>(0xDC00 + (code & 0x3FF)));
        } else
            out.push_back(static_cast<unsigned short>(code > 0xFFFF ? 0xFFFD : code));
    }
    out.push_back(0);
    return out;
}

struct LameDeleter {
    void operator()(lame_global_flags* gf) const { lame_close(gf); }
};

Error encodeError(std::string what) {
    return Error("audio.encode", "MP3 encoder: " + std::move(what), {},
                 "use 1 or 2 channels at an MPEG rate (8000-48000 Hz)");
}

void setText(lame_global_flags* gf, const char* id, const std::string& value) {
    if (value.empty()) return;
    const auto text = utf16(value);
    id3tag_set_textinfo_utf16(gf, id, text.data());
}
}  // namespace

std::string lameVersion() { return get_lame_version(); }

Result<std::vector<std::uint8_t>> encodeMp3(const audio::AudioData& data,
                                            const Mp3Options& options) {
    const std::size_t channels = data.channelCount();
    if (channels != 1 && channels != 2) return encodeError("1 or 2 channels");
    static constexpr std::array<int, 9> kRates{8000,  11025, 12000, 16000, 22050,
                                               24000, 32000, 44100, 48000};
    const int rate = static_cast<int>(data.sampleRate);
    if (static_cast<double>(rate) != data.sampleRate ||
        std::find(kRates.begin(), kRates.end(), rate) == kRates.end())
        return encodeError("unsupported sample rate " + std::to_string(rate));
    // LAME keeps shared static tables: lame_init fills the log table
    // (util.c, init_log_table) behind an unsynchronised flag, and every
    // lame_init_params rewrites the quantizer's power tables (quantize_pvt.c,
    // iteration_init, pow43/adj43/ipow20) that encoding reads (takehiro.c).
    // So one MP3 encode runs at a time; the lock outlives the encoder.
    static std::mutex lameLock;
    std::lock_guard lock(lameLock);
    std::unique_ptr<lame_global_flags, LameDeleter> gf(lame_init());
    if (!gf) return encodeError("could not start");
    lame_set_num_channels(gf.get(), static_cast<int>(channels));
    lame_set_in_samplerate(gf.get(), rate);
    lame_set_out_samplerate(gf.get(), rate);
    lame_set_mode(gf.get(), channels == 1 ? MONO : JOINT_STEREO);
    lame_set_quality(gf.get(), 2);  // the algorithm: near-best, still fast
    if (options.vbr) {
        lame_set_VBR(gf.get(), vbr_default);
        lame_set_VBR_quality(gf.get(), static_cast<float>(std::clamp(options.vbrQuality, 0, 9)));
    } else {
        lame_set_VBR(gf.get(), vbr_off);
        lame_set_brate(gf.get(), options.bitrateKbps);
    }
    lame_set_bWriteVbrTag(gf.get(), 1);  // LAME/Xing header: delay and padding (gapless)
    lame_set_write_id3tag_automatic(gf.get(), 0);
    const auto& tags = options.tags;
    id3tag_init(gf.get());
    id3tag_v2_only(gf.get());
    setText(gf.get(), "TIT2", tags.title);
    setText(gf.get(), "TPE1", tags.artist);
    setText(gf.get(), "TALB", tags.album);
    setText(gf.get(), "TCON", tags.genre);
    setText(gf.get(), "TSSE", tags.software);
    if (!tags.trackNumber.empty()) id3tag_set_track(gf.get(), tags.trackNumber.c_str());
    if (tags.date.size() >= 4) id3tag_set_year(gf.get(), tags.date.substr(0, 4).c_str());
    if (!tags.isrc.empty()) id3tag_set_textinfo_latin1(gf.get(), "TSRC", tags.isrc.c_str());
    if (!tags.comment.empty()) {
        const auto description = utf16("");
        const auto comment = utf16(tags.comment);
        id3tag_set_comment_utf16(gf.get(), "eng", description.data(), comment.data());
    }
    if (!tags.picture.empty() &&
        id3tag_set_albumart(gf.get(), reinterpret_cast<const char*>(tags.picture.data()),
                            tags.picture.size()) != 0)
        return Error("audio.encode", "the cover is not a PNG or JPEG LAME accepts",
                     "/metadata/cover", "use a PNG or JPEG cover");
    if (lame_init_params(gf.get()) < 0) return encodeError("invalid parameters");

    std::vector<std::uint8_t> stream;
    const std::size_t frames = data.frames();
    constexpr std::size_t kChunk = 1 << 15;
    std::vector<unsigned char> buffer(kChunk * 5 / 4 + 7200);
    const float* left = data.channels[0].data();
    const float* right = channels == 2 ? data.channels[1].data() : nullptr;
    for (std::size_t at = 0; at < frames; at += kChunk) {
        const auto n = static_cast<int>(std::min(kChunk, frames - at));
        const int written =
            lame_encode_buffer_ieee_float(gf.get(), left + at, right ? right + at : nullptr, n,
                                          buffer.data(), static_cast<int>(buffer.size()));
        if (written < 0) return encodeError("encoding failed (" + std::to_string(written) + ")");
        stream.insert(stream.end(), buffer.begin(), buffer.begin() + written);
    }
    const int flushed = lame_encode_flush(gf.get(), buffer.data(), static_cast<int>(buffer.size()));
    if (flushed < 0) return encodeError("flush failed");
    stream.insert(stream.end(), buffer.begin(), buffer.begin() + flushed);
    // The LAME/Xing frame replaces the empty frame LAME put first.
    std::vector<unsigned char> lametag(4096);
    const std::size_t tagSize = lame_get_lametag_frame(gf.get(), lametag.data(), lametag.size());
    if (tagSize > lametag.size() || tagSize > stream.size())
        return encodeError("LAME tag frame too large");
    std::copy(lametag.begin(), lametag.begin() + static_cast<std::ptrdiff_t>(tagSize),
              stream.begin());
    const std::size_t id3Size = lame_get_id3v2_tag(gf.get(), nullptr, 0);
    std::vector<std::uint8_t> out(id3Size);
    if (id3Size && lame_get_id3v2_tag(gf.get(), out.data(), out.size()) != id3Size)
        return encodeError("ID3 tag failed");
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

}  // namespace supple::codecs
