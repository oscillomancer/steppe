// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/audio/wav_stream.h"

#include "file_platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace supple::audio {

namespace {

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

constexpr long kHeaderBytes = 44;
// The RIFF container caps sizes at 32 bits; longer recordings need RF64.
constexpr std::uint64_t kMaxData = 0xFFFFFFFFull - (kHeaderBytes - 8);

}  // namespace

WavStreamWriter::~WavStreamWriter() { (void)close(); }

Status WavStreamWriter::open(const std::filesystem::path& path, double sampleRate,
                             std::uint32_t channels) {
    if (file_) return Error("audio.stream", "the writer is already open");
    if (channels == 0 || channels > 64 || !std::isfinite(sampleRate) || sampleRate < 1 ||
        sampleRate > 768000)
        return Error("audio.stream", "bad channel count or rate");
    file_ = detail::files::open(path, true, true);  // never an existing file (another take's)
    if (!file_)
        return Error("file.write",
                     "cannot create " + detail::files::text(path) + " (does it exist?)");
    channels_ = channels;
    frames_ = 0;
    std::uint8_t h[kHeaderBytes] = {};
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, kHeaderBytes - 8);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 3);  // IEEE float
    put16(h + 22, static_cast<std::uint16_t>(channels));
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 4);
    put16(h + 32, static_cast<std::uint16_t>(channels * 4));
    put16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, 0);
    if (std::fwrite(h, 1, sizeof h, file_) != sizeof h) return Error("file.write", "header");
    return okStatus();
}

Status WavStreamWriter::append(const float* frames, std::size_t count) {
    if (!file_) return Error("audio.stream", "the writer is closed");
    if (count > kMaxData / (channels_ * 4) - frames_)
        return Error("audio.stream_full", "a recording over 4 GiB needs RF64 (not yet)");
    if (count && std::fwrite(frames, sizeof(float) * channels_, count, file_) != count)
        return Error("file.write", "short write");
    frames_ += count;
    return okStatus();
}

Status WavStreamWriter::updateHeader() {
    if (!file_) return okStatus();
    const auto data = static_cast<std::uint32_t>(frames_ * channels_ * 4);
    std::uint8_t b[4];
    const auto end = detail::files::tell(file_);
    if (end < 0) return Error("file.write", "position");
    put32(b, data + kHeaderBytes - 8);
    if (detail::files::seek(file_, 4, SEEK_SET) != 0 || std::fwrite(b, 1, 4, file_) != 4)
        return Error("file.write", "header");
    put32(b, data);
    if (detail::files::seek(file_, 40, SEEK_SET) != 0 || std::fwrite(b, 1, 4, file_) != 4)
        return Error("file.write", "header");
    if (detail::files::seek(file_, end, SEEK_SET) != 0) return Error("file.write", "seek");
    return flush();
}

Status WavStreamWriter::flush() {
    if (file_ && std::fflush(file_) != 0) return Error("file.write", "flush");
    return okStatus();
}

void WavStreamWriter::abandon() {
    if (!file_) return;
    std::fclose(file_);
    file_ = nullptr;
}

Status WavStreamWriter::close() {
    if (!file_) return okStatus();
    auto st = updateHeader();
    const int closed = std::fclose(file_);
    file_ = nullptr;
    if (st && closed != 0) return Error("file.write", "close");
    return st;
}

namespace {
Result<WavStreamInfo> inspectStream(const std::filesystem::path& path, bool& sizesMatch) {
    std::FILE* f = detail::files::open(path, false);
    if (!f)
        return Error("file.open", "cannot read " + detail::files::text(path) +
                                      " (or it is a link or not a regular file)");
    std::uint8_t h[kHeaderBytes];
    bool read = std::fread(h, 1, sizeof h, f) == sizeof h;
    std::int64_t end = -1;
    if (read && detail::files::seek(f, 0, SEEK_END) == 0) end = detail::files::tell(f);
    std::fclose(f);
    read = read && end >= kHeaderBytes;
    const auto size = static_cast<std::uint64_t>(end);
    if (!read || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVEfmt ", 8) != 0 ||
        get32(h + 16) != 16 || get16(h + 20) != 3 || get16(h + 34) != 32 ||
        std::memcmp(h + 36, "data", 4) != 0)
        return Error("audio.not_stream",
                     detail::files::text(path) + " is not a recording's WAV file");
    WavStreamInfo info;
    info.channels = get16(h + 22);
    info.sampleRate = get32(h + 24);
    if (info.channels == 0)
        return Error("audio.not_stream", detail::files::text(path) + " has no channels");
    const std::uint64_t frameBytes = std::uint64_t(info.channels) * 4;
    info.headerFrames = get32(h + 40) / frameBytes;
    const std::uint64_t data = std::min<std::uint64_t>(size - kHeaderBytes, kMaxData);
    info.fileFrames = data / frameBytes;
    const auto wholeBytes = info.fileFrames * frameBytes;
    sizesMatch = get32(h + 4) == wholeBytes + kHeaderBytes - 8 && get32(h + 40) == wholeBytes;
    return info;
}
}  // namespace

Result<WavStreamInfo> inspectWavStream(const std::filesystem::path& path) {
    bool sizesMatch = false;
    return inspectStream(path, sizesMatch);
}

Result<std::uint64_t> repairWavStream(const std::filesystem::path& path) {
    bool sizesMatch = false;
    auto info = inspectStream(path, sizesMatch);
    if (!info) return std::move(info).error();
    if (sizesMatch) return info->fileFrames;
    std::FILE* f = detail::files::open(path, true);
    if (!f)
        return Error("file.write", "cannot open " + detail::files::text(path) +
                                       " (or it is a link or not a regular file)");
    const auto data = static_cast<std::uint32_t>(info->fileFrames * info->channels * 4);
    std::uint8_t b[4];
    bool ok = true;
    put32(b, data + kHeaderBytes - 8);
    ok = ok && detail::files::seek(f, 4, SEEK_SET) == 0 && std::fwrite(b, 1, 4, f) == 4;
    put32(b, data);
    ok = ok && detail::files::seek(f, 40, SEEK_SET) == 0 && std::fwrite(b, 1, 4, f) == 4;
    ok = std::fclose(f) == 0 && ok;
    if (!ok)
        return Error("file.write", "cannot rewrite the header of " + detail::files::text(path));
    return info->fileFrames;
}

}  // namespace supple::audio
