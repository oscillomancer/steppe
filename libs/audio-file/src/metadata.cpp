// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// File metadata for the WAV, AIFF and FLAC writers (audio_file.h FileMetadata)
// and the AIFF writer and reader. Original code from the format
// descriptions: RIFF (Microsoft/IBM multimedia spec: LIST INFO, cue, adtl),
// EBU Tech 3285 (bext v2), AIFF 1.3 (COMM, SSND, MARK, NAME, AUTH, ANNO) and
// the FLAC format (VORBIS_COMMENT, PICTURE).
#include "metadata.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "quantize.h"

#define DR_WAV_NO_STDIO
#include "dr_wav.h"

namespace supple::audio {
namespace {

using Bytes = std::vector<std::uint8_t>;

void le16(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 2; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void le32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void be16(Bytes& b, std::uint32_t v) {
    for (int i = 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void be24(Bytes& b, std::uint32_t v) {
    for (int i = 2; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void be32(Bytes& b, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void tag(Bytes& b, const char* id) { b.insert(b.end(), id, id + 4); }
void text(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
// A fixed-size ASCII field, zero-padded (bext).
void fixed(Bytes& b, const std::string& s, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
        b.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
}
// A RIFF chunk: id, little-endian size, body, pad byte to an even size.
void riffChunk(Bytes& out, const char* id, const Bytes& body) {
    tag(out, id);
    le32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() % 2) out.push_back(0);
}
void aiffChunk(Bytes& out, const char* id, const Bytes& body) {
    tag(out, id);
    be32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() % 2) out.push_back(0);
}
std::int16_t hundredths(const std::optional<double>& value) {
    if (!value || !std::isfinite(*value)) return 0x7FFF;
    return static_cast<std::int16_t>(std::clamp(std::lround(*value * 100), -32767L, 32766L));
}

Bytes bextChunk(const Bext& b) {
    Bytes body;
    fixed(body, b.description, 256);
    fixed(body, b.originator, 32);
    fixed(body, b.originatorReference, 32);
    fixed(body, b.date, 10);
    fixed(body, b.date.empty() ? std::string() : std::string("00:00:00"), 8);
    le32(body, static_cast<std::uint32_t>(b.timeReference & 0xFFFFFFFFu));
    le32(body, static_cast<std::uint32_t>(b.timeReference >> 32));
    le16(body, 2);        // version
    fixed(body, "", 64);  // UMID
    for (const auto& v :
         {b.loudness, b.loudnessRange, b.maxTruePeak, b.maxMomentary, b.maxShortTerm})
        le16(body, static_cast<std::uint16_t>(hundredths(v)));
    fixed(body, "", 180);  // reserved
    text(body, b.codingHistory);
    return body;
}

Bytes infoList(const FileTags& t) {
    Bytes body;
    tag(body, "INFO");
    for (const auto& [id, value] :
         {std::pair{"INAM", &t.title}, std::pair{"IART", &t.artist}, std::pair{"IPRD", &t.album},
          std::pair{"ICRD", &t.date}, std::pair{"IGNR", &t.genre}, std::pair{"ICMT", &t.comment},
          std::pair{"ITRK", &t.trackNumber}, std::pair{"ISFT", &t.software}}) {
        if (value->empty()) continue;
        Bytes field;
        text(field, *value);
        field.push_back(0);
        riffChunk(body, id, field);
    }
    return body.size() > 4 ? body : Bytes{};
}

void cueChunks(Bytes& out, const std::vector<CuePoint>& cues) {
    if (cues.empty()) return;
    Bytes cue;
    le32(cue, static_cast<std::uint32_t>(cues.size()));
    for (std::size_t i = 0; i < cues.size(); ++i) {
        const auto frame =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(cues[i].frame, UINT32_MAX));
        le32(cue, static_cast<std::uint32_t>(i + 1));  // id
        le32(cue, frame);                              // play order position
        tag(cue, "data");
        le32(cue, 0);  // chunk start
        le32(cue, 0);  // block start
        le32(cue, frame);
    }
    riffChunk(out, "cue ", cue);
    Bytes adtl;
    tag(adtl, "adtl");
    for (std::size_t i = 0; i < cues.size(); ++i) {
        Bytes label;
        le32(label, static_cast<std::uint32_t>(i + 1));
        text(label, cues[i].name);
        label.push_back(0);
        riffChunk(adtl, "labl", label);
        if (cues[i].length) {
            Bytes region;
            le32(region, static_cast<std::uint32_t>(i + 1));
            le32(region,
                 static_cast<std::uint32_t>(std::min<std::uint64_t>(cues[i].length, UINT32_MAX)));
            tag(region, "rgn ");
            for (int k = 0; k < 4; ++k) le16(region, 0);  // country, language, dialect, code page
            riffChunk(adtl, "ltxt", region);
        }
    }
    riffChunk(out, "LIST", adtl);
}

// An 80-bit IEEE 754 extended number (AIFF's sample rate).
void extended(Bytes& b, double value) {
    std::uint16_t exponent = 0;
    std::uint64_t mantissa = 0;
    if (value > 0) {
        int e = 0;
        const double fraction = std::frexp(value, &e);  // value = fraction * 2^e, 0.5 <= f < 1
        exponent = static_cast<std::uint16_t>(e - 1 + 16383);
        mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
    }
    be16(b, exponent);
    for (int i = 7; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(mantissa >> (8 * i)));
}

// An AIFF pstring: a count byte, the text, a pad to an even total.
void pstring(Bytes& b, const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    b.push_back(static_cast<std::uint8_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    if ((n + 1) % 2) b.push_back(0);
}

Bytes flacBlock(int type, bool last, const Bytes& body) {
    Bytes out;
    out.push_back(static_cast<std::uint8_t>((last ? 0x80 : 0) | type));
    be24(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
}  // namespace

Bytes pictureBlock(const FileTags& tags) {
    Bytes body;
    be32(body, 3);  // front cover
    be32(body, static_cast<std::uint32_t>(tags.pictureMime.size()));
    text(body, tags.pictureMime);
    be32(body, 0);                              // description
    for (int i = 0; i < 4; ++i) be32(body, 0);  // width, height, depth, colours: unknown
    be32(body, static_cast<std::uint32_t>(tags.picture.size()));
    body.insert(body.end(), tags.picture.begin(), tags.picture.end());
    return body;
}

std::vector<std::pair<std::string, std::string>> vorbisComments(const FileTags& t) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [key, value] :
         {std::pair{"TITLE", &t.title}, std::pair{"ARTIST", &t.artist},
          std::pair{"ALBUM", &t.album}, std::pair{"TRACKNUMBER", &t.trackNumber},
          std::pair{"DATE", &t.date}, std::pair{"GENRE", &t.genre},
          std::pair{"COMMENT", &t.comment}, std::pair{"ISRC", &t.isrc},
          std::pair{"ENCODER", &t.software}})
        if (!value->empty()) out.emplace_back(key, *value);
    return out;
}

namespace detail {

Result<Bytes> wavWithMetadata(Bytes riff, const FileMetadata& m) {
    if (m.empty()) return riff;
    if (riff.size() < 12 || std::memcmp(riff.data(), "RIFF", 4) != 0)
        return Error("audio.encode", "metadata needs a RIFF WAV");
    if (riff.size() % 2) riff.push_back(0);  // the data chunk's pad byte
    Bytes out(riff.begin(), riff.begin() + 12);
    if (m.bext) riffChunk(out, "bext", bextChunk(*m.bext));
    out.insert(out.end(), riff.begin() + 12, riff.end());
    if (const Bytes info = infoList(m.tags); !info.empty()) riffChunk(out, "LIST", info);
    cueChunks(out, m.cues);
    if (out.size() - 8 > UINT32_MAX)
        return Error("audio.encode", "WAV with metadata exceeds 4 GiB", {},
                     "render a shorter range or turn metadata off");
    const auto size = static_cast<std::uint32_t>(out.size() - 8);
    for (int i = 0; i < 4; ++i)
        out[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    return out;
}

Bytes flacWithTags(Bytes flac, const FileTags& t) {
    if (t.empty() || flac.size() < 42) return flac;
    Bytes blocks;
    const auto comments = vorbisComments(t);
    const bool picture = !t.picture.empty();
    {
        Bytes body;
        const std::string vendor = t.software.empty() ? std::string("audio-file") : t.software;
        le32(body, static_cast<std::uint32_t>(vendor.size()));
        text(body, vendor);
        le32(body, static_cast<std::uint32_t>(comments.size()));
        for (const auto& [key, value] : comments) {
            le32(body, static_cast<std::uint32_t>(key.size() + 1 + value.size()));
            text(body, key + "=" + value);
        }
        const Bytes block = flacBlock(4, !picture, body);
        blocks.insert(blocks.end(), block.begin(), block.end());
    }
    if (picture) {
        const Bytes block = flacBlock(6, true, pictureBlock(t));
        blocks.insert(blocks.end(), block.begin(), block.end());
    }
    flac[4] = static_cast<std::uint8_t>(flac[4] & 0x7F);  // STREAMINFO is no longer last
    flac.insert(flac.begin() + 42, blocks.begin(), blocks.end());
    return flac;
}

}  // namespace detail

Result<Bytes> encodeAiff(const AudioData& data, const AiffOptions& options) {
    const std::size_t channels = data.channelCount();
    const std::size_t frames = data.frames();
    const int bits = options.bitsPerSample;
    if (channels == 0 || channels > 64)
        return Error("audio.encode", "AIFF needs 1..64 channels", {},
                     "got " + std::to_string(channels));
    if (bits != 16 && bits != 24)
        return Error("audio.encode", "AIFF bit depth must be 16 or 24", "/bit_depth");
    for (const auto& c : data.channels)
        if (c.size() != frames) return Error("audio.encode", "channels have different lengths");
    if (!std::isfinite(data.sampleRate) || data.sampleRate < 1 || data.sampleRate > 768000)
        return Error("audio.encode", "invalid sample rate", {}, "use 1 .. 768000 Hz");
    const std::size_t bytesPer = static_cast<std::size_t>(bits / 8);
    if (frames > (UINT32_MAX - 1024) / (channels * bytesPer))
        return Error("audio.encode", "AIFF is limited to 4 GiB", {},
                     "render a shorter range or use WAV (RF64)");
    Bytes body;
    tag(body, "AIFF");
    {
        Bytes comm;
        be16(comm, static_cast<std::uint32_t>(channels));
        be32(comm, static_cast<std::uint32_t>(frames));
        be16(comm, static_cast<std::uint32_t>(bits));
        extended(comm, std::round(data.sampleRate));
        aiffChunk(body, "COMM", comm);
    }
    const auto& tags = options.metadata.tags;
    for (const auto& [id, value] : {std::pair{"NAME", &tags.title}, std::pair{"AUTH", &tags.artist},
                                    std::pair{"ANNO", &tags.comment}}) {
        if (value->empty()) continue;
        Bytes field;
        text(field, *value);
        aiffChunk(body, id, field);
    }
    const auto& cues = options.metadata.cues;
    if (!cues.empty()) {
        Bytes mark;
        const std::size_t count = std::min<std::size_t>(cues.size(), 0x7FFF);
        be16(mark, static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            be16(mark, static_cast<std::uint32_t>(i + 1));
            be32(mark,
                 static_cast<std::uint32_t>(std::min<std::uint64_t>(cues[i].frame, UINT32_MAX)));
            pstring(mark, cues[i].name);
        }
        aiffChunk(body, "MARK", mark);
    }
    Bytes ssnd;
    ssnd.reserve(8 + frames * channels * bytesPer);
    be32(ssnd, 0);  // offset
    be32(ssnd, 0);  // block size
    detail::Quantizer quantize(bits, options.dither, options.shaped, options.ditherSeed, channels);
    for (std::size_t i = 0; i < frames; ++i)
        for (std::size_t c = 0; c < channels; ++c) {
            const auto q = static_cast<std::uint32_t>(quantize(data.channels[c][i], c));
            if (bits == 16)
                be16(ssnd, q & 0xFFFFu);
            else
                be24(ssnd, q & 0xFFFFFFu);
        }
    aiffChunk(body, "SSND", ssnd);
    Bytes out;
    aiffChunk(out, "FORM", body);
    return out;
}

Result<AudioData> decodeAiff(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "FORM", 4) != 0 ||
        (std::memcmp(bytes.data() + 8, "AIFF", 4) != 0 &&
         std::memcmp(bytes.data() + 8, "AIFC", 4) != 0))
        return Error("audio.format", "not an AIFF file", {}, "give AIFF bytes");
    drwav wav;
    if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr))
        return Error("audio.decode", "could not decode the AIFF data", {},
                     "the file may be truncated or use an unsupported variant");
    const unsigned channels = wav.channels;
    if (channels == 0 || wav.totalPCMFrameCount > static_cast<drwav_uint64>(bytes.size())) {
        drwav_uninit(&wav);
        return Error("audio.decode", "could not decode the AIFF data", {},
                     "the file may be truncated or use an unsupported variant");
    }
    const auto frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
    std::vector<float> interleaved(frames * channels);
    const auto got = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
    AudioData out;
    out.sampleRate = wav.sampleRate;
    drwav_uninit(&wav);
    out.channels.assign(channels, std::vector<float>(static_cast<std::size_t>(got)));
    for (std::size_t i = 0; i < got; ++i)
        for (unsigned c = 0; c < channels; ++c) out.channels[c][i] = interleaved[i * channels + c];
    out.truncated = got < frames;
    return out;
}

}  // namespace supple::audio
