// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Ogg Vorbis through libvorbis 1.3.7 and libogg 1.3.6 (BSD-3-Clause).
#include "supple/codecs/codecs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "supple/base/files.h"
#include "vorbis/codec.h"
#include "vorbis/vorbisenc.h"
#include "vorbis/vorbisfile.h"

namespace supple::codecs {
namespace {

Error encodeError(std::string what) {
    return Error("audio.encode", "Ogg Vorbis encoder: " + std::move(what), {},
                 "use 1 or 2 channels at 8000-192000 Hz and a quality from -1 to 10");
}

// Every libvorbis/libogg object of one encode, released together.
struct Encoder {
    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    ogg_stream_state stream{};
    bool dspReady = false, streamReady = false;
    Encoder() {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }
    ~Encoder() {
        if (streamReady) ogg_stream_clear(&stream);
        if (dspReady) {
            vorbis_block_clear(&block);
            vorbis_dsp_clear(&dsp);
        }
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
};

void append(std::vector<std::uint8_t>& out, const ogg_page& page) {
    out.insert(out.end(), page.header, page.header + page.header_len);
    out.insert(out.end(), page.body, page.body + page.body_len);
}

// vorbisfile reads from memory through these callbacks.
struct Memory {
    std::span<const std::uint8_t> bytes;
    std::size_t at = 0;
};
std::size_t readMemory(void* into, std::size_t size, std::size_t count, void* source) {
    auto& m = *static_cast<Memory*>(source);
    const std::size_t n = std::min(size * count, m.bytes.size() - m.at);
    if (n) std::memcpy(into, m.bytes.data() + m.at, n);
    m.at += n;
    return size ? n / size : 0;
}
int seekMemory(void* source, ogg_int64_t offset, int whence) {
    auto& m = *static_cast<Memory*>(source);
    const auto base = whence == SEEK_SET   ? ogg_int64_t{0}
                      : whence == SEEK_CUR ? static_cast<ogg_int64_t>(m.at)
                                           : static_cast<ogg_int64_t>(m.bytes.size());
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(m.bytes.size())) return -1;
    m.at = static_cast<std::size_t>(target);
    return 0;
}
long tellMemory(void* source) { return static_cast<long>(static_cast<Memory*>(source)->at); }
}  // namespace

Result<std::vector<std::uint8_t>> encodeVorbis(const audio::AudioData& data,
                                               const VorbisOptions& options) {
    const std::size_t channels = data.channelCount();
    if (channels != 1 && channels != 2) return encodeError("1 or 2 channels");
    if (!(data.sampleRate >= 8000 && data.sampleRate <= 192000))
        return encodeError("unsupported sample rate");
    if (!(options.quality >= -1 && options.quality <= 10)) return encodeError("quality");
    Encoder e;
    if (vorbis_encode_init_vbr(&e.info, static_cast<long>(channels),
                               static_cast<long>(std::lround(data.sampleRate)),
                               static_cast<float>(options.quality / 10.0)) != 0)
        return encodeError("this rate and quality are not supported");
    for (const auto& [key, value] : audio::vorbisComments(options.tags))
        vorbis_comment_add_tag(&e.comment, key.c_str(), value.c_str());
    if (!options.tags.picture.empty()) {
        const std::string picture = base64Encode(audio::pictureBlock(options.tags));
        vorbis_comment_add_tag(&e.comment, "METADATA_BLOCK_PICTURE", picture.c_str());
    }
    if (vorbis_analysis_init(&e.dsp, &e.info) != 0) return encodeError("could not start");
    vorbis_block_init(&e.dsp, &e.block);
    e.dspReady = true;
    ogg_stream_init(&e.stream, static_cast<int>(options.serial));
    e.streamReady = true;
    std::vector<std::uint8_t> out;
    ogg_packet identification, comments, codebooks;
    vorbis_analysis_headerout(&e.dsp, &e.comment, &identification, &comments, &codebooks);
    ogg_stream_packetin(&e.stream, &identification);
    ogg_stream_packetin(&e.stream, &comments);
    ogg_stream_packetin(&e.stream, &codebooks);
    ogg_page page;
    while (ogg_stream_flush(&e.stream, &page) != 0) append(out, page);  // audio starts a page
    auto drain = [&] {
        ogg_packet packet;
        while (vorbis_analysis_blockout(&e.dsp, &e.block) == 1) {
            vorbis_analysis(&e.block, nullptr);
            vorbis_bitrate_addblock(&e.block);
            while (vorbis_bitrate_flushpacket(&e.dsp, &packet) == 1) {
                ogg_stream_packetin(&e.stream, &packet);
                while (ogg_stream_pageout(&e.stream, &page) != 0) append(out, page);
            }
        }
    };
    const std::size_t frames = data.frames();
    constexpr std::size_t kChunk = 4096;
    for (std::size_t at = 0; at < frames; at += kChunk) {
        const std::size_t n = std::min(kChunk, frames - at);
        float** buffer = vorbis_analysis_buffer(&e.dsp, static_cast<int>(n));
        for (std::size_t c = 0; c < channels; ++c)
            std::copy_n(data.channels[c].data() + at, n, buffer[c]);
        vorbis_analysis_wrote(&e.dsp, static_cast<int>(n));
        drain();
    }
    vorbis_analysis_wrote(&e.dsp, 0);  // end of stream
    drain();
    while (ogg_stream_flush(&e.stream, &page) != 0) append(out, page);
    return out;
}

Result<audio::AudioData> decodeVorbis(std::span<const std::uint8_t> bytes) {
    Memory memory{bytes, 0};
    ov_callbacks callbacks{readMemory, seekMemory, nullptr, tellMemory};
    OggVorbis_File file;
    if (ov_open_callbacks(&memory, &file, nullptr, 0, callbacks) != 0)
        return Error("audio.decode", "could not decode the Ogg Vorbis data", {},
                     "the file may be truncated or not Ogg Vorbis");
    const vorbis_info* info = ov_info(&file, -1);
    audio::AudioData out;
    out.sampleRate = static_cast<double>(info->rate);
    const auto channels = static_cast<std::size_t>(info->channels);
    out.channels.assign(channels, {});
    int section = 0;
    for (;;) {
        float** pcm = nullptr;
        const long n = ov_read_float(&file, &pcm, 4096, &section);
        if (n == 0) break;
        if (n < 0) {
            ov_clear(&file);
            return Error("audio.decode", "Ogg Vorbis data is damaged", {}, "render the file again");
        }
        for (std::size_t c = 0; c < channels; ++c)
            out.channels[c].insert(out.channels[c].end(), pcm[c], pcm[c] + n);
    }
    ov_clear(&file);
    return out;
}

}  // namespace supple::codecs
