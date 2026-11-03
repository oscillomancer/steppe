// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/audio/audio_file.h"

#include "supple/base/files.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>
#include "file_platform.h"
#include "metadata.h"
#include "quantize.h"

#include "supple/base/rng.h"

#define DR_WAV_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_MP3_NO_STDIO
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

namespace supple::audio {

namespace fs = std::filesystem;

std::vector<const float*> AudioData::pointers() const {
    std::vector<const float*> p;
    for (const auto& c : channels) p.push_back(c.data());
    return p;
}

Container detect(std::span<const std::uint8_t> h) {
    auto is = [&](const char* tag, std::size_t at = 0) {
        const std::size_t n = std::strlen(tag);
        return h.size() >= at + n && std::memcmp(h.data() + at, tag, n) == 0;
    };
    if ((is("RIFF") || is("RF64") || is("BW64")) && is("WAVE", 8)) return Container::Wav;
    if (is("fLaC")) return Container::Flac;
    if (is("ID3")) return Container::Mp3;
    if (h.size() >= 2 && h[0] == 0xff && (h[1] & 0xe0) == 0xe0) return Container::Mp3;
    return Container::Unknown;
}

namespace {

AudioData deinterleave(const float* interleaved, std::size_t frames, unsigned channels,
                       double rate) {
    AudioData out;
    out.sampleRate = rate;
    out.channels.assign(channels, std::vector<float>(frames));
    for (std::size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < channels; ++c) out.channels[c][i] = interleaved[i * channels + c];
    return out;
}

Error decodeError(const char* what) {
    return Error("audio.decode", std::string("could not decode the ") + what + " data", {},
                 "the file may be truncated or use an unsupported variant");
}

// Tests: every whole-file decode (audio_file.h).
std::atomic<std::uint64_t> g_decodes{0};

}  // namespace

std::uint64_t decodeCountForTests() { return g_decodes.load(std::memory_order_relaxed); }
void resetDecodeCountForTests() { g_decodes.store(0, std::memory_order_relaxed); }

Result<AudioData> readAudio(std::span<const std::uint8_t> bytes) {
    g_decodes.fetch_add(1, std::memory_order_relaxed);
    switch (detect(bytes)) {
        case Container::Wav: {
            drwav wav;
            if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr))
                return decodeError("WAV");
            const unsigned channels = wav.channels;
            const double rate = wav.sampleRate;
            // dr_wav clamps a data chunk that runs past the end of the file
            // (interrupted recordings are common); keep what is there but say so.
            // RIFF/RIFX store the declared size just before the data.
            bool truncated = false;
            if ((wav.container == drwav_container_riff || wav.container == drwav_container_rifx) &&
                wav.dataChunkDataPos >= 4 && wav.dataChunkDataPos <= bytes.size()) {
                const std::size_t at = static_cast<std::size_t>(wav.dataChunkDataPos) - 4;
                const bool be = wav.container == drwav_container_rifx;
                std::uint32_t declared = 0;
                for (int b = 0; b < 4; ++b)
                    declared |= std::uint32_t(bytes[at + static_cast<std::size_t>(be ? 3 - b : b)])
                                << (8 * b);
                truncated = declared != 0xFFFFFFFFu && wav.dataChunkDataPos + declared >
                                                           static_cast<drwav_uint64>(bytes.size());
            }
            // No encoding stores more than two frames per byte (4-bit ADPCM,
            // mono): anything larger is a lying header; this also bounds the allocation.
            if (channels == 0 ||
                wav.totalPCMFrameCount > 2 * static_cast<drwav_uint64>(bytes.size())) {
                drwav_uninit(&wav);
                return decodeError("WAV");
            }
            const std::size_t frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
            std::vector<float> interleaved(frames * channels);
            const auto got = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
            drwav_uninit(&wav);
            auto out =
                deinterleave(interleaved.data(), static_cast<std::size_t>(got), channels, rate);
            out.truncated = truncated || got < frames;
            return out;
        }
        case Container::Flac: {
            unsigned int channels = 0, rate = 0;
            drflac_uint64 frames = 0;
            float* data = drflac_open_memory_and_read_pcm_frames_f32(
                bytes.data(), bytes.size(), &channels, &rate, &frames, nullptr);
            if (!data) return decodeError("FLAC");
            auto out = deinterleave(data, static_cast<std::size_t>(frames), channels, rate);
            drflac_free(data, nullptr);
            return out;
        }
        case Container::Mp3: {
            drmp3_config config{};
            drmp3_uint64 frames = 0;
            float* data = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(),
                                                                    &config, &frames, nullptr);
            if (!data) return decodeError("MP3");
            auto out = deinterleave(data, static_cast<std::size_t>(frames), config.channels,
                                    config.sampleRate);
            drmp3_free(data, nullptr);
            return out;
        }
        case Container::Unknown:
            break;
    }
    return Error("audio.format", "unrecognised audio format", {}, "supported: WAV, FLAC, MP3");
}

Result<AudioData> readAudio(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Error("audio.open", "cannot open " + detail::files::text(path), {},
                     "check the path");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    auto r = readAudio(bytes);
    if (!r) {
        Error e = std::move(r).error();
        e.message = detail::files::text(path.filename()) + ": " + e.message;
        return e;
    }
    return r;
}

Result<std::vector<std::uint8_t>> wavHeader(std::uint64_t frames, std::uint32_t channels,
                                            std::uint32_t sampleRate, SampleFormat sampleFormat) {
    if (channels == 0 || channels > 64 || sampleRate == 0 || sampleRate > 768000)
        return Error("audio.encode", "invalid WAV channel count or sample rate");
    const std::uint32_t bytesPer = sampleFormat == SampleFormat::Pcm16   ? 2
                                   : sampleFormat == SampleFormat::Pcm24 ? 3
                                                                         : 4;
    const std::uint64_t frameBytes = std::uint64_t(channels) * bytesPer;
    if (frames > (UINT64_MAX - 80) / frameBytes)
        return Error("audio.encode", "WAV size overflows 64 bits");
    const std::uint64_t dataBytes = frames * frameBytes;
    const bool rf64 = dataBytes > UINT32_MAX - 72;
    std::vector<std::uint8_t> h(rf64 ? 80 : 44);
    auto tag = [&](std::size_t at, const char* s) { std::memcpy(h.data() + at, s, 4); };
    auto put16 = [&](std::size_t at, std::uint16_t x) {
        for (int i = 0; i < 2; ++i) h[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
    };
    auto put32 = [&](std::size_t at, std::uint32_t x) {
        for (int i = 0; i < 4; ++i) h[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
    };
    auto put64 = [&](std::size_t at, std::uint64_t x) {
        for (int i = 0; i < 8; ++i) h[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
    };
    tag(0, rf64 ? "RF64" : "RIFF");
    put32(4, rf64 ? UINT32_MAX : static_cast<std::uint32_t>(dataBytes + 36));
    tag(8, "WAVE");
    std::size_t fmt = 12;
    if (rf64) {
        tag(12, "ds64");
        put32(16, 28);
        put64(20, dataBytes + 72);
        put64(28, dataBytes);
        put64(36, frames);
        put32(44, 0);
        fmt = 48;
    }
    tag(fmt, "fmt ");
    put32(fmt + 4, 16);
    put16(fmt + 8, sampleFormat == SampleFormat::Float32 ? 3 : 1);
    put16(fmt + 10, static_cast<std::uint16_t>(channels));
    put32(fmt + 12, sampleRate);
    put32(fmt + 16, sampleRate * static_cast<std::uint32_t>(frameBytes));
    put16(fmt + 20, static_cast<std::uint16_t>(frameBytes));
    put16(fmt + 22, static_cast<std::uint16_t>(8 * bytesPer));
    tag(fmt + 24, "data");
    put32(fmt + 28, rf64 ? UINT32_MAX : static_cast<std::uint32_t>(dataBytes));
    return h;
}

Result<std::vector<std::uint8_t>> encodeWav(const AudioData& data, const WavOptions& options,
                                            const EncodeExtras& extras) {
    const std::size_t channels = data.channelCount();
    const std::size_t frames = data.frames();
    if (channels == 0 || channels > 64)
        return Error("audio.encode", "WAV needs 1..64 channels", {},
                     "got " + std::to_string(channels));
    for (const auto& c : data.channels)
        if (c.size() != frames) return Error("audio.encode", "channels have different lengths");
    if (!std::isfinite(data.sampleRate) || data.sampleRate < 1 || data.sampleRate > 768000)
        return Error("audio.encode", "invalid sample rate", {}, "use 1 .. 768000 Hz");

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.channels = static_cast<drwav_uint32>(channels);
    format.sampleRate = static_cast<drwav_uint32>(std::lround(data.sampleRate));
    std::vector<std::uint8_t> payload;
    switch (options.format) {
        case SampleFormat::Float32: {
            format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
            format.bitsPerSample = 32;
            payload.resize(frames * channels * 4);
            auto* out = reinterpret_cast<float*>(payload.data());
            for (std::size_t i = 0; i < frames; ++i)
                for (std::size_t c = 0; c < channels; ++c)
                    out[i * channels + c] = data.channels[c][i];
            break;
        }
        case SampleFormat::Pcm16:
        case SampleFormat::Pcm24: {
            const bool is16 = options.format == SampleFormat::Pcm16;
            format.format = DR_WAVE_FORMAT_PCM;
            format.bitsPerSample = is16 ? 16 : 24;
            const std::size_t bytesPer = is16 ? 2 : 3;
            payload.resize(frames * channels * bytesPer);
            detail::Quantizer quantize(is16 ? 16 : 24, options.dither, extras.shaped,
                                       options.ditherSeed, channels);
            std::size_t w = 0;
            for (std::size_t i = 0; i < frames; ++i)
                for (std::size_t c = 0; c < channels; ++c) {
                    const auto q = quantize(data.channels[c][i], c);
                    for (std::size_t b = 0; b < bytesPer; ++b)
                        payload[w++] = static_cast<std::uint8_t>(q >> (8 * b));
                }
            break;
        }
    }
    drwav wav;
    void* mem = nullptr;
    size_t size = 0;
    if (!drwav_init_memory_write(&wav, &mem, &size, &format, nullptr))
        return Error("audio.encode", "WAV writer failed");
    const auto written = drwav_write_raw(&wav, payload.size(), payload.data());
    drwav_uninit(&wav);
    if (written != payload.size()) {
        drwav_free(mem, nullptr);
        return Error("audio.encode", "WAV writer wrote fewer bytes than expected");
    }
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(mem),
                                  static_cast<std::uint8_t*>(mem) + size);
    drwav_free(mem, nullptr);
    return detail::wavWithMetadata(std::move(out), extras.metadata);
}

Status writeWav(const fs::path& path, const AudioData& data, const WavOptions& options,
                const EncodeExtras& extras) {
    const std::size_t channels = data.channelCount();
    const std::uint64_t bytesPer = options.format == SampleFormat::Pcm16   ? 2
                                   : options.format == SampleFormat::Pcm24 ? 3
                                                                           : 4;
    if (channels && data.frames() > (UINT32_MAX - 72) / (channels * bytesPer)) {
        if (!std::isfinite(data.sampleRate) || data.sampleRate < 1 || data.sampleRate > 768000)
            return Error("audio.encode", "invalid sample rate", {}, "use 1 .. 768000 Hz");
        for (const auto& channel : data.channels)
            if (channel.size() != data.frames())
                return Error("audio.encode", "channels have different lengths");
        auto header =
            wavHeader(data.frames(), static_cast<std::uint32_t>(channels),
                      static_cast<std::uint32_t>(std::lround(data.sampleRate)), options.format);
        if (!header) return std::move(header).error();
        std::error_code ec;
        fs::create_directories(path.has_parent_path() ? path.parent_path() : fs::path("."), ec);
        if (ec) return Error("audio.write", "cannot create render directory");
        fs::path temporary;
        std::FILE* file = detail::files::temporary(path, temporary);
        if (!file)
            return Error("audio.write", "cannot create temporary WAV file", {},
                         "check that the directory is writable and has free space");
        bool good = std::fwrite(header->data(), 1, header->size(), file) == header->size();
        detail::Quantizer quantize(options.format == SampleFormat::Pcm16 ? 16 : 24, options.dither,
                                   extras.shaped, options.ditherSeed, channels);
        std::vector<std::uint8_t> block(4096 * channels * bytesPer);
        for (std::size_t start = 0; good && start < data.frames(); start += 4096) {
            const std::size_t count = std::min<std::size_t>(4096, data.frames() - start);
            std::size_t w = 0;
            for (std::size_t i = 0; i < count; ++i)
                for (std::size_t c = 0; c < channels; ++c) {
                    const float sample = data.channels[c][start + i];
                    if (options.format == SampleFormat::Float32) {
                        std::memcpy(block.data() + w, &sample, 4);
                        w += 4;
                    } else {
                        const auto q = quantize(sample, c);
                        for (std::size_t b = 0; b < bytesPer; ++b)
                            block[w++] = static_cast<std::uint8_t>(q >> (8 * b));
                    }
                }
            good = std::fwrite(block.data(), 1, w, file) == w;
        }
        good = detail::files::sync(file) && good;
        good = std::fclose(file) == 0 && good;
        bool busy = false;
        if (good) good = detail::files::publish(temporary, path, &busy);
        if (!good) {
            fs::remove(temporary, ec);
            if (busy)
                return Error("audio.write",
                             "cannot replace " + detail::files::text(path) +
                                 ": another program has the file open",
                             detail::files::text(path),
                             "close the program using the file (a viewer, sync client, backup or "
                             "antivirus scan), or wait a moment, then write again; the previous "
                             "file is unchanged");
            return Error("audio.write", "could not write or publish RF64 WAV", {},
                         "check free disk space and permissions");
        }
        return okStatus();
    }
    auto bytes = encodeWav(data, options, extras);
    if (!bytes) return std::move(bytes).error();
    auto st = writeFileAtomic(path, *bytes);
    if (!st) {
        Error e = std::move(st).error();
        e.code = "audio.write";
        return e;
    }
    return okStatus();
}

}  // namespace supple::audio
