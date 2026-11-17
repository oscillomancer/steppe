// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/audio/wav_stream.h"

#include "file_platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace supple::audio {

namespace {
void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
constexpr std::int64_t kHeaderBytes = 80;
constexpr std::int64_t kLegacyHeaderBytes = 44;
constexpr std::uint64_t kMaxLegacyData = UINT32_MAX - (kLegacyHeaderBytes - 8);

// Promotion can tear within either FourCC, including reordered byte writes.
// Only old/new bytes at their exact positions qualify; all fixed layout and
// format checks still apply before any recovery write.
bool promotionTag(const std::uint8_t* tag, const char* before, const char* after) {
    for (int i = 0; i < 4; ++i)
        if (tag[i] != static_cast<std::uint8_t>(before[i]) &&
            tag[i] != static_cast<std::uint8_t>(after[i]))
            return false;
    return true;
}

bool writeAt(std::FILE* file, std::int64_t offset, const void* bytes, std::size_t size) {
    return detail::files::seek(file, offset, SEEK_SET) == 0 &&
           std::fwrite(bytes, 1, size, file) == size;
}
bool writeSizes(std::FILE* file, std::int64_t header, std::uint64_t data, std::uint64_t frames,
                bool rf64) {
    std::uint8_t bytes[28]{};
    if (rf64) {
        put64(bytes, data + static_cast<std::uint64_t>(header - 8));
        put64(bytes + 8, data);
        put64(bytes + 16, frames);
        // Make ds64 valid before publishing the RF64 marker. A crash in
        // between leaves the recognizable RIFF+ds64 transitional layout.
        if (!writeAt(file, 20, bytes, sizeof bytes) || !writeAt(file, 12, "ds64", 4)) return false;
        put32(bytes, UINT32_MAX);
        return writeAt(file, 4, bytes, 4) && writeAt(file, header - 4, bytes, 4) &&
               writeAt(file, 0, "RF64", 4);
    }
    put32(bytes, static_cast<std::uint32_t>(data + static_cast<std::uint64_t>(header - 8)));
    if (!writeAt(file, 4, bytes, 4)) return false;
    put32(bytes, static_cast<std::uint32_t>(data));
    return writeAt(file, header - 4, bytes, 4);
}
}  // namespace

WavStreamWriter::~WavStreamWriter() { (void)close(); }

Status WavStreamWriter::open(const std::filesystem::path& path, double sampleRate,
                             std::uint32_t channels) {
    if (file_) return Error("audio.stream", "the writer is already open");
    if (channels == 0 || channels > 64 || !std::isfinite(sampleRate) || sampleRate < 1 ||
        sampleRate > 768000)
        return Error("audio.stream", "bad channel count or rate");
    file_ = detail::files::open(path, true, true);
    if (!file_)
        return Error("file.write",
                     "cannot create " + detail::files::text(path) + " (does it exist?)");
    channels_ = channels;
    frames_ = 0;
    failed_ = synced_ = rf64_ = false;
    std::uint8_t h[kHeaderBytes]{};
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, kHeaderBytes - 8);
    std::memcpy(h + 8, "WAVEJUNK", 8);
    put32(h + 16, 28);
    std::memcpy(h + 48, "fmt ", 4);
    put32(h + 52, 16);
    put16(h + 56, 3);
    put16(h + 58, static_cast<std::uint16_t>(channels));
    put32(h + 60, rate);
    put32(h + 64, rate * channels * 4);
    put16(h + 68, static_cast<std::uint16_t>(channels * 4));
    put16(h + 70, 32);
    std::memcpy(h + 72, "data", 4);
    if (std::fwrite(h, 1, sizeof h, file_) != sizeof h) return Error("file.write", "header");
    return okStatus();
}

Status WavStreamWriter::append(const float* frames, std::size_t count) {
    if (!file_) return Error("audio.stream", "the writer is closed");
    if (failed_) return Error("file.write", "the recording target has failed");
    const std::uint64_t frameBytes = std::uint64_t(channels_) * 4;
    const auto maxFrames = (std::uint64_t(INT64_MAX) - kHeaderBytes) / frameBytes;
    if (frames_ > maxFrames || count > maxFrames - frames_ ||
        count > std::numeric_limits<std::size_t>::max() / frameBytes)
        return Error("audio.stream_full", "recording size exceeds 64-bit file offsets");
    synced_ = false;
    const auto bytes = count * sizeof(float) * channels_;
    const auto written = io_.write ? io_.write(io_.context, frames, bytes, file_)
                                   : std::fwrite(frames, 1, bytes, file_);
    if (written != bytes) {
        (void)std::fflush(file_);
        reconcileWritten();
        return Error("file.write", "short write");
    }
    frames_ += count;
    if (!rf64_ && frames_ * frameBytes + kHeaderBytes - 8 > riffLimit_) return updateHeader();
    return okStatus();
}

Status WavStreamWriter::updateHeader() {
    if (!file_) return okStatus();
    synced_ = false;
    auto status = flush();
    std::clearerr(file_);
    const auto data = frames_ * channels_ * 4;
    rf64_ = rf64_ || data + kHeaderBytes - 8 > riffLimit_;
    const auto end = detail::files::tell(file_);
    if (end < 0 || !writeSizes(file_, kHeaderBytes, data, frames_, rf64_) ||
        detail::files::seek(file_, end, SEEK_SET) != 0) {
        failed_ = true;
        return Error("file.write", "cannot update recording header");
    }
    auto header = flush();
    return status ? header : status;
}

void WavStreamWriter::reconcileWritten() {
    failed_ = true;
    const auto bytes = detail::files::size(file_);
    if (bytes >= kHeaderBytes)
        frames_ = static_cast<std::uint64_t>(bytes - kHeaderBytes) / (channels_ * 4);
    else if (bytes >= 0)
        frames_ = 0;
}

Status WavStreamWriter::flush() {
    if (file_ && (io_.flush ? io_.flush(io_.context, file_) : std::fflush(file_)) != 0) {
        reconcileWritten();
        return Error("file.write", "flush");
    }
    return okStatus();
}

void WavStreamWriter::abandon() {
    if (!file_) return;
    std::fclose(file_);
    file_ = nullptr;
}

Status WavStreamWriter::sync() {
    if (!file_) return okStatus();
    SUPPLE_TRY(updateHeader());
    const bool synced = io_.sync ? io_.sync(io_.context, file_) : detail::files::sync(file_);
    if (!synced) {
        failed_ = true;
        return Error("file.sync", "cannot sync recording data and header");
    }
    synced_ = true;
    return okStatus();
}
Status WavStreamWriter::close() {
    if (!file_) return okStatus();
    auto st = synced_ ? okStatus() : sync();
    const int closed = std::fclose(file_);
    file_ = nullptr;
    if (st && closed != 0) return Error("file.write", "close");
    return st;
}

namespace {
struct StreamHeader {
    WavStreamInfo info;
    std::int64_t bytes = 0;
    bool rf64 = false, sizesMatch = false;
};
Result<StreamHeader> inspectStream(const std::filesystem::path& path) {
    auto invalid = [&] {
        return Error("audio.not_stream",
                     detail::files::text(path) + " is not a recording's WAV/RF64 file");
    };
    std::FILE* file = detail::files::open(path, false);
    if (!file)
        return Error("file.open", "cannot read " + detail::files::text(path) +
                                      " (or it is a link or not a regular file)");
    std::array<std::uint8_t, kHeaderBytes> h{};
    const auto read = std::fread(h.data(), 1, h.size(), file);
    std::int64_t end = -1;
    if (detail::files::seek(file, 0, SEEK_END) == 0) end = detail::files::tell(file);
    std::fclose(file);
    if (read < kLegacyHeaderBytes || end < kLegacyHeaderBytes ||
        std::memcmp(h.data() + 8, "WAVE", 4) != 0)
        return invalid();
    const bool riff = std::memcmp(h.data(), "RIFF", 4) == 0;
    const bool rf64 = std::memcmp(h.data(), "RF64", 4) == 0;
    const bool legacy = std::memcmp(h.data() + 12, "fmt ", 4) == 0;
    const bool ds64 = std::memcmp(h.data() + 12, "ds64", 4) == 0;
    const bool junk = std::memcmp(h.data() + 12, "JUNK", 4) == 0;
    const bool container = promotionTag(h.data(), "RIFF", "RF64");
    const bool reserved = promotionTag(h.data() + 12, "JUNK", "ds64");
    if (!container || (legacy && !riff) ||
        (!legacy && (!reserved || get32(h.data() + 16) != 28 || read < kHeaderBytes)))
        return invalid();
    StreamHeader out;
    out.bytes = legacy ? kLegacyHeaderBytes : kHeaderBytes;
    if (end < out.bytes) return invalid();
    const auto fmt = legacy ? 12 : 48;
    const auto* f = h.data() + fmt;
    if (std::memcmp(f, "fmt ", 4) != 0 || get32(f + 4) != 16 || get16(f + 8) != 3 ||
        get16(f + 22) != 32 || std::memcmp(f + 24, "data", 4) != 0)
        return invalid();
    auto& info = out.info;
    info.channels = get16(f + 10);
    info.sampleRate = get32(f + 12);
    if (info.channels == 0 || info.channels > 64 || info.sampleRate < 1 ||
        info.sampleRate > 768000 || get16(f + 20) != info.channels * 4 ||
        get32(f + 16) != static_cast<std::uint32_t>(info.sampleRate) * info.channels * 4)
        return invalid();
    const auto frameBytes = std::uint64_t(info.channels) * 4;
    const auto physical = static_cast<std::uint64_t>(end - out.bytes);
    info.fileFrames = (legacy ? std::min(physical, kMaxLegacyData) : physical) / frameBytes;
    const auto wholeBytes = info.fileFrames * frameBytes;
    info.headerFrames = (ds64 ? get64(h.data() + 28) : get32(f + 28)) / frameBytes;
    const bool torn = (!riff && !rf64) || (!legacy && !junk && !ds64);
    out.rf64 = rf64 || ds64 || torn || (!legacy && wholeBytes + kHeaderBytes - 8 > UINT32_MAX);
    if (rf64 && ds64)
        out.sizesMatch = get32(h.data() + 4) == UINT32_MAX && get32(f + 28) == UINT32_MAX &&
                         get64(h.data() + 20) == wholeBytes + kHeaderBytes - 8 &&
                         get64(h.data() + 28) == wholeBytes &&
                         get64(h.data() + 36) == info.fileFrames && get32(h.data() + 44) == 0;
    else if (riff && (legacy || junk) && !out.rf64)
        out.sizesMatch =
            get32(h.data() + 4) == wholeBytes + static_cast<std::uint64_t>(out.bytes - 8) &&
            get32(f + 28) == wholeBytes;
    return out;
}
}  // namespace

Result<WavStreamInfo> inspectWavStream(const std::filesystem::path& path) {
    auto parsed = inspectStream(path);
    if (!parsed) return std::move(parsed).error();
    return parsed->info;
}
Result<std::uint64_t> repairWavStream(const std::filesystem::path& path) {
    auto parsed = inspectStream(path);
    if (!parsed) return std::move(parsed).error();
    if (parsed->sizesMatch) return parsed->info.fileFrames;
    auto* file = detail::files::open(path, true);
    if (!file)
        return Error("file.write", "cannot open " + detail::files::text(path) +
                                       " (or it is a link or not a regular file)");
    const auto& info = parsed->info;
    const bool written = writeSizes(file, parsed->bytes, info.fileFrames * info.channels * 4,
                                    info.fileFrames, parsed->rf64);
    const bool synced = written && detail::files::sync(file);
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
        return Error("file.write", "cannot rewrite the header of " + detail::files::text(path));
    if (!synced)
        return Error("file.sync", "cannot sync repaired header of " + detail::files::text(path));
    return info.fileFrames;
}
}  // namespace supple::audio
