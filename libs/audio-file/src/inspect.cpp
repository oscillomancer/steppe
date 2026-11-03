// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Container metadata without decoding (audio_file.h inspect): a host that only
// needs a file's rate, channel count and frame count must not pay for a full
// decode (item.add, the media model). WAV/RF64 walk their chunks, FLAC reads
// STREAMINFO, MP3 takes the Xing/Info frame count when present and otherwise
// walks the frames — music data is never touched.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

#include "supple/audio/audio_file.h"

namespace supple::audio {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kHeadBytes = 1 << 16;  // any real header fits in this

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
}
std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8));
}

std::vector<std::uint8_t> readHead(std::ifstream& in, std::uint64_t fileBytes, std::size_t want) {
    std::vector<std::uint8_t> head(
        static_cast<std::size_t>(std::min<std::uint64_t>(fileBytes, want)));
    in.clear();
    in.seekg(0);
    if (!head.empty())
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(std::max<std::streamoff>(in.gcount(), 0)));
    return head;
}

// --- WAV / RF64 ---------------------------------------------------------------------

struct Riff {
    std::uint32_t formatTag = 0;
    std::uint16_t channels = 0, bitsPerSample = 0;
    std::uint32_t sampleRate = 0, blockAlign = 0;
    std::uint64_t dataOffset = 0, dataBytes = 0, rf64Frames = 0;
    bool rf64 = false, haveFmt = false, haveData = false;
    std::uint64_t need = 0;  // bytes of header the walk still needs (0: done)
};

Riff parseRiff(std::span<const std::uint8_t> head) {
    Riff out;
    if (head.size() < 12) return out;
    out.rf64 = std::memcmp(head.data(), "RF64", 4) == 0 || std::memcmp(head.data(), "BW64", 4) == 0;
    if (std::memcmp(head.data() + 8, "WAVE", 4) != 0) {
        out.need = std::numeric_limits<std::uint64_t>::max();  // not a WAVE file
        return out;
    }
    std::uint64_t at = 12;
    while (at + 8 <= head.size()) {
        const std::uint8_t* p = head.data() + at;
        const std::uint32_t size = le32(p + 4);
        if (std::memcmp(p, "ds64", 4) == 0 && size >= 28 && at + 8 + 28 <= head.size()) {
            out.dataBytes = le32(p + 8 + 8) | (std::uint64_t(le32(p + 8 + 12)) << 32);
            out.rf64Frames = le32(p + 8 + 16) | (std::uint64_t(le32(p + 8 + 20)) << 32);
        } else if (std::memcmp(p, "fmt ", 4) == 0 && size >= 16 && at + 8 + 16 <= head.size()) {
            out.formatTag = le16(p + 8);
            out.channels = le16(p + 10);
            out.sampleRate = le32(p + 12);
            out.blockAlign = le16(p + 20);
            out.bitsPerSample = le16(p + 22);
            if (out.formatTag == 0xfffe && size >= 40 &&
                at + 8 + 40 <= head.size())  // WAVE_FORMAT_EXTENSIBLE
                out.formatTag = le16(p + 8 + 24);
            out.haveFmt = true;
        } else if (std::memcmp(p, "data", 4) == 0) {
            out.dataOffset = at + 8;
            if (!out.rf64) out.dataBytes = size;
            out.haveData = true;
            return out;
        }
        at += 8ull + size + (size & 1u);  // chunks are word aligned
    }
    out.need = at;  // the walk ran out of buffer: the caller reads more
    return out;
}

Result<FileInfo> inspectWav(std::ifstream& in, std::span<const std::uint8_t> head,
                            std::uint64_t fileBytes) {
    Riff riff = parseRiff(head);
    if (riff.need == std::numeric_limits<std::uint64_t>::max())
        return Error("audio.format", "not a RIFF/WAVE file");
    if (riff.need != 0) {
        // A chunk before `data` is larger than the window: read up to 4 MiB.
        const auto more = readHead(in, fileBytes, 4u << 20);
        riff = parseRiff(more);
        if (riff.need != 0) return Error("audio.format", "the WAV header is unexpectedly large");
    }
    if (!riff.haveFmt) return Error("audio.format", "the WAV file has no fmt chunk");
    if (riff.formatTag == 1 || riff.formatTag == 3) {
        const auto expected = std::uint32_t(riff.channels) * ((riff.bitsPerSample + 7) / 8);
        if (riff.blockAlign != expected)
            return Error("audio.format", "WAV block alignment does not match its sample format");
    }
    const std::uint32_t align = riff.blockAlign != 0
                                    ? riff.blockAlign
                                    : std::uint32_t(riff.channels) * ((riff.bitsPerSample + 7) / 8);
    if (riff.channels == 0 || riff.sampleRate == 0 || align == 0)
        return Error("audio.format", "the WAV header is incomplete");
    FileInfo info;
    info.container = Container::Wav;
    info.sampleRate = riff.sampleRate;
    info.channels = riff.channels;
    info.bitsPerSample = riff.bitsPerSample;
    info.floatSamples = riff.formatTag == 3;
    std::uint64_t bytes = riff.haveData ? riff.dataBytes : 0;
    if (riff.haveData) {
        const std::uint64_t present = fileBytes > riff.dataOffset ? fileBytes - riff.dataOffset : 0;
        info.truncated = bytes > present;
        bytes = std::min(bytes, present);
    }
    info.frames = static_cast<std::int64_t>(bytes / align);
    if (riff.rf64 && riff.rf64Frames != 0 &&
        riff.rf64Frames < static_cast<std::uint64_t>(info.frames))
        info.frames = static_cast<std::int64_t>(riff.rf64Frames);
    return info;
}

// --- FLAC ---------------------------------------------------------------------------

Result<FileInfo> inspectFlac(std::span<const std::uint8_t> head) {
    if (head.size() < 4 + 4 + 34) return Error("audio.format", "the FLAC header is short");
    if (std::memcmp(head.data(), "fLaC", 4) != 0) return Error("audio.format", "not a FLAC file");
    std::size_t at = 4;
    while (at + 4 <= head.size()) {
        const bool last = (head[at] & 0x80) != 0;
        const std::uint32_t type = head[at] & 0x7f;
        const std::uint32_t size = (std::uint32_t(head[at + 1]) << 16) |
                                   (std::uint32_t(head[at + 2]) << 8) | std::uint32_t(head[at + 3]);
        at += 4;
        if (type == 0) {  // STREAMINFO: 20-bit rate, 3-bit channels, 36-bit frames
            if (size != 34 || at + size > head.size())
                return Error("audio.format", "the STREAMINFO must contain exactly 34 bytes");
            const std::uint8_t* p = head.data() + at;
            FileInfo info;
            info.container = Container::Flac;
            info.sampleRate = double((std::uint32_t(p[10]) << 12) | (std::uint32_t(p[11]) << 4) |
                                     (std::uint32_t(p[12]) >> 4));
            if (info.sampleRate == 0)
                return Error("audio.format", "the FLAC sample rate must be positive");
            info.channels = static_cast<int>((p[12] >> 1) & 0x07) + 1;
            info.frames = static_cast<std::int64_t>(
                (std::uint64_t(p[13] & 0x0f) << 32) | (std::uint64_t(p[14]) << 24) |
                (std::uint64_t(p[15]) << 16) | (std::uint64_t(p[16]) << 8) | std::uint64_t(p[17]));
            return info;
        }
        if (last) break;
        at += size;
    }
    return Error("audio.format", "the FLAC file has no STREAMINFO");
}

// --- MP3 ----------------------------------------------------------------------------

struct Mp3Frame {
    int version = 1;  // 1: MPEG-1, 2: MPEG-2, 25: MPEG-2.5
    int bitrate = 0;  // kbit/s
    int sampleRate = 0;
    int channels = 0;
    int samples = 0;           // PCM samples per frame
    std::uint32_t length = 0;  // bytes
};

std::optional<Mp3Frame> mp3Frame(const std::uint8_t* p) {
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0) return std::nullopt;
    const int versionBits = (p[1] >> 3) & 0x03;          // 0: 2.5, 2: 2, 3: MPEG-1
    if (versionBits == 1) return std::nullopt;           // reserved
    if (((p[1] >> 1) & 0x03) != 1) return std::nullopt;  // Layer III only
    const int bitrateIndex = (p[2] >> 4) & 0x0f;
    const int rateIndex = (p[2] >> 2) & 0x03;
    if (bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return std::nullopt;
    static constexpr int kBitrateV1[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                           112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr int kBitrateV2[16] = {0,  8,  16, 24,  32,  40,  48,  56,
                                           64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr int kRate[4][4] = {
        {11025, 12000, 8000, 0}, {0, 0, 0, 0}, {22050, 24000, 16000, 0}, {44100, 48000, 32000, 0}};
    Mp3Frame frame;
    frame.version = versionBits == 3 ? 1 : (versionBits == 2 ? 2 : 25);
    frame.bitrate = (versionBits == 3 ? kBitrateV1 : kBitrateV2)[bitrateIndex];
    frame.sampleRate = kRate[versionBits][rateIndex];
    frame.channels = ((p[3] >> 6) & 0x03) == 3 ? 1 : 2;
    frame.samples = versionBits == 3 ? 1152 : 576;
    const int padding = (p[2] >> 1) & 0x01;
    frame.length =
        static_cast<std::uint32_t>(frame.samples / 8 * frame.bitrate * 1000 / frame.sampleRate) +
        static_cast<std::uint32_t>(padding);
    if (frame.bitrate == 0 || frame.sampleRate == 0 || frame.length == 0) return std::nullopt;
    return frame;
}

// An ID3v1 tag is the last 128 bytes starting with "TAG".
bool hasId3v1(std::ifstream& in, std::uint64_t fileBytes) {
    if (fileBytes < 128) return false;
    char tag[3] = {0, 0, 0};
    in.clear();
    in.seekg(static_cast<std::streamoff>(fileBytes - 128));
    in.read(tag, 3);
    return in.gcount() == 3 && std::memcmp(tag, "TAG", 3) == 0;
}

Result<FileInfo> inspectMp3(std::ifstream& in, std::span<const std::uint8_t> head,
                            std::uint64_t fileBytes) {
    // An ID3v2 tag first (10-byte header, syncsafe size, optional footer).
    std::uint64_t at = 0;
    if (head.size() >= 10 && std::memcmp(head.data(), "ID3", 3) == 0) {
        const std::uint32_t size =
            (std::uint32_t(head[6] & 0x7f) << 21) | (std::uint32_t(head[7] & 0x7f) << 14) |
            (std::uint32_t(head[8] & 0x7f) << 7) | std::uint32_t(head[9] & 0x7f);
        at = 10 + size + (((head[5] & 0x10) != 0) ? 10 : 0);
    }
    // The first valid frame: scan a bounded window, then walk frame to frame.
    const std::uint64_t scanEnd = std::min<std::uint64_t>(fileBytes, at + (1u << 20));
    std::optional<Mp3Frame> first;
    std::vector<std::uint8_t> window(4096);
    while (at + 4 <= scanEnd && !first) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(window.size(), scanEnd - at));
        in.read(reinterpret_cast<char*>(window.data()), static_cast<std::streamsize>(want));
        const auto got = static_cast<std::size_t>(std::max<std::streamoff>(in.gcount(), 0));
        if (got < 4) break;
        for (std::size_t i = 0; i + 4 <= got; ++i) {
            if (auto frame = mp3Frame(window.data() + i)) {
                first = frame;
                at += i;
                break;
            }
        }
        if (!first) at += got - 3;  // keep 3 bytes: a sync may straddle the read
    }
    if (!first) return Error("audio.format", "no MP3 frame found");
    FileInfo info;
    info.container = Container::Mp3;
    info.sampleRate = first->sampleRate;
    info.channels = first->channels;
    // Xing/Info in the first frame: the frame count without walking. The tag
    // follows the frame header's side info, but encoders differ by a few bytes
    // (this fixture has it four late), so find it rather than compute its
    // offset. The LAME extension that follows carries the encoder delay and the
    // end padding, and a decoder subtracts both (dr_mp3 does) — the count must
    // agree with what playback yields.
    std::uint64_t frames = 0, delay = 0, padding = 0;
    {
        std::vector<std::uint8_t> frame(
            static_cast<std::size_t>(std::min<std::uint64_t>(first->length, 4096)));
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
        const auto got = static_cast<std::size_t>(std::max<std::streamoff>(in.gcount(), 0));
        std::size_t tagAt = frame.size();
        for (std::size_t i = 0; i + 4 <= std::min<std::size_t>(got, 256); ++i)
            if (std::memcmp(frame.data() + i, "Xing", 4) == 0 ||
                std::memcmp(frame.data() + i, "Info", 4) == 0) {
                tagAt = i;
                break;
            }
        if (tagAt + 12 <= got) {
            const std::uint8_t* tag = frame.data() + tagAt;
            const std::uint32_t flags = be32(tag + 4);
            if ((flags & 0x01u) != 0) frames = be32(tag + 8);
            std::size_t lame = tagAt + 8;
            if ((flags & 0x01u) != 0) lame += 4;
            if ((flags & 0x02u) != 0) lame += 4;
            if ((flags & 0x04u) != 0) lame += 100;
            if ((flags & 0x08u) != 0) lame += 4;
            if (lame + 24 <= got) {  // the delay/padding field is at LAME+21
                const std::uint8_t* p = frame.data() + lame;
                delay = (std::uint32_t(p[21]) << 4) | (std::uint32_t(p[22]) >> 4);
                padding = (std::uint32_t(p[22] & 0x0f) << 8) | std::uint32_t(p[23]);
            }
        }
    }
    if (frames != 0) {
        std::uint64_t total = frames * static_cast<std::uint64_t>(first->samples);
        if (total > delay) total -= delay;
        if (total > padding) total -= padding;
        info.frames = static_cast<std::int64_t>(total);
        return info;
    }
    // No header: walk the frames (exact, no decoding).
    const std::uint64_t end = hasId3v1(in, fileBytes) ? fileBytes - 128 : fileBytes;
    std::uint64_t total = 0;
    std::uint64_t pos = at;
    std::uint8_t header[4];
    while (pos + 4 <= end) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(header), 4);
        if (in.gcount() != 4) break;
        const auto frame = mp3Frame(header);
        if (!frame || pos + frame->length > end) break;
        total += static_cast<std::uint64_t>(frame->samples);
        pos += frame->length;
    }
    info.frames = static_cast<std::int64_t>(total);
    return info;
}

}  // namespace

Result<FileInfo> inspect(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Error("audio.open", "cannot open " + path.string(), path.string(), "check the path");
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) return Error("audio.read", "cannot read " + path.string());
    const auto fileBytes = static_cast<std::uint64_t>(end);
    const auto head = readHead(in, fileBytes, kHeadBytes);
    if (head.empty()) return Error("audio.read", "cannot read " + path.string());
    switch (detect(head)) {
        case Container::Wav:
            return inspectWav(in, head, fileBytes);
        case Container::Flac:
            return inspectFlac(head);
        case Container::Mp3:
            return inspectMp3(in, head, fileBytes);
        case Container::Unknown:
            break;
    }
    return Error("audio.format", "unknown audio container in " + path.string());
}

}  // namespace supple::audio
