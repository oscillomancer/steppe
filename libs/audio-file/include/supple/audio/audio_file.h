// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Audio file I/O (spec: docs/specs/core-analysis.md §9). Main/I-O threads only.
//
// Reading: WAV (PCM 8/16/24/32, float 32/64, RF64/BW64), FLAC and MP3, detected
// by content (not extension), decoded to planar float at the file's own rate.
// Writing: RIFF WAV or RF64 beyond 4 GiB as 16/24-bit PCM with seeded TPDF
// dither, or 32-bit float; FLAC at 16 or 24 bits.
// Writes are atomic (temporary file + rename, see writeFileAtomic). PCM
// encoding maps NaN to 0 and clamps infinities to full scale; float32 stores
// samples unchanged. A WAV whose data is shorter than its header declares
// decodes to the audio present with AudioData::truncated set.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "supple/base/result.h"

namespace supple::audio {

struct AudioData {
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;  // planar; all the same length
    // Set by readAudio when the file holds less audio than its header declares
    // (e.g. an interrupted recording): the audio is what was there.
    bool truncated = false;

    std::size_t channelCount() const { return channels.size(); }
    std::size_t frames() const { return channels.empty() ? 0 : channels.front().size(); }
    double seconds() const {
        return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0;
    }
    // Pointers for APIs that take `const float* const*`.
    std::vector<const float*> pointers() const;
};

enum class Container { Wav, Flac, Mp3, Unknown };
Container detect(std::span<const std::uint8_t> head);  // from the first bytes

// What a file's header says, without decoding it: the media model's
// MediaSource needs exactly this (rate, channels, frames). The container is
// detected by content, like readAudio. `truncated` mirrors AudioData: the data
// chunk holds less audio than it declares. Error codes: "audio.open",
// "audio.read", "audio.format".
struct FileInfo {
    Container container = Container::Unknown;
    double sampleRate = 0;
    int channels = 0;
    std::int64_t frames = 0;
    bool truncated = false;
    // WAV only (0/false otherwise): the stored sample size and whether the
    // samples are IEEE float rather than integer PCM.
    int bitsPerSample = 0;
    bool floatSamples = false;
};
Result<FileInfo> inspect(const std::filesystem::path& path);

Result<AudioData> resample(const AudioData& data, double targetRate);

Result<AudioData> readAudio(const std::filesystem::path& path);
Result<AudioData> readAudio(std::span<const std::uint8_t> bytes);

// Tests: how many whole-file decodes readAudio() has performed (the path
// overload goes through the byte overload, which counts once). The media model
// must read a file's header — inspect() — never decode it to learn metadata.
std::uint64_t decodeCountForTests();
void resetDecodeCountForTests();

// What a written file says about itself besides audio (render metadata,
// session-model §1.10). Empty members are not written. WAV: a LIST INFO
// chunk (title, artist, album, date, genre, comment, track, software), an
// optional BWF bext chunk (version 2) and cue points with labels (regions
// as labelled text with a length); AIFF: NAME, AUTH, ANNO and MARK chunks;
// FLAC: a Vorbis comment block and a PICTURE block. Nothing reads the clock.
struct FileTags {
    std::string title, artist, album, date, genre, comment, isrc, trackNumber, software;
    std::vector<std::uint8_t> picture;  // PNG or JPEG
    std::string pictureMime;            // "image/png" or "image/jpeg"
    bool empty() const {
        return title.empty() && artist.empty() && album.empty() && date.empty() && genre.empty() &&
               comment.empty() && isrc.empty() && trackNumber.empty() && software.empty() &&
               picture.empty();
    }
};
// The tags as Vorbis comment fields (TITLE, ARTIST, …, ENCODER), for FLAC
// and Ogg Vorbis, and the picture as a FLAC PICTURE block body (front
// cover), which Ogg Vorbis carries base64-encoded as METADATA_BLOCK_PICTURE.
std::vector<std::pair<std::string, std::string>> vorbisComments(const FileTags& tags);
std::vector<std::uint8_t> pictureBlock(const FileTags& tags);
struct CuePoint {
    std::uint64_t frame = 0;   // from the start of the file
    std::uint64_t length = 0;  // frames; 0 for a marker, else a region
    std::string name;
};
// EBU Tech 3285 v2. Loudness members are in the spec's units (LUFS, LU, dB)
// and written as hundredths; absent ones are written as 0x7FFF ("not set").
struct Bext {
    std::string description, originator, originatorReference, date;  // date YYYY-MM-DD
    std::uint64_t timeReference = 0;                                 // samples since midnight
    std::optional<double> loudness, loudnessRange, maxTruePeak, maxMomentary, maxShortTerm;
    std::string codingHistory;
};
struct FileMetadata {
    FileTags tags;
    std::optional<Bext> bext;    // WAV only
    std::vector<CuePoint> cues;  // WAV and AIFF
    bool empty() const { return tags.empty() && !bext && cues.empty(); }
};

enum class SampleFormat { Pcm16, Pcm24, Float32 };
struct WavOptions {
    SampleFormat format = SampleFormat::Float32;
    bool dither = true;            // TPDF dither for PCM formats
    std::uint64_t ditherSeed = 0;  // deterministic
};
// What the render writers add to WavOptions/FlacOptions: noise-shaped TPDF
// dither (implies dither) and metadata. RF64 files (beyond 4 GiB) carry no
// metadata.
struct EncodeExtras {
    bool shaped = false;
    FileMetadata metadata;
};

// Builds a canonical RIFF or RF64 header for the given payload size. RF64
// includes ds64 with the 64-bit sizes and sample count; no audio allocation.
Result<std::vector<std::uint8_t>> wavHeader(std::uint64_t frames, std::uint32_t channels,
                                            std::uint32_t sampleRate, SampleFormat format);

Result<std::vector<std::uint8_t>> encodeWav(const AudioData& data, const WavOptions& options = {},
                                            const EncodeExtras& extras = {});
Status writeWav(const std::filesystem::path& path, const AudioData& data,
                const WavOptions& options = {}, const EncodeExtras& extras = {});

// FLAC (an original encoder: fixed predictors, Rice coding, stereo
// decorrelation, MD5 in STREAMINFO). 16- or 24-bit, quantized and dithered
// exactly as encodeWav does for the same options, so the two decode alike.
struct FlacOptions {
    int bitsPerSample = 24;  // 16 or 24
    bool dither = true;
    std::uint64_t ditherSeed = 0;
    int blockSize = 4096;
};
// extras.metadata.tags become a Vorbis comment block and a PICTURE block.
Result<std::vector<std::uint8_t>> encodeFlac(const AudioData& data, const FlacOptions& options = {},
                                             const EncodeExtras& extras = {});
Status writeFlac(const std::filesystem::path& path, const AudioData& data,
                 const FlacOptions& options = {});

// AIFF (an original writer): big-endian 16- or 24-bit PCM, quantized and
// dithered exactly as encodeWav does for the same options; markers from
// metadata.cues (a region marks its start), title/artist/comment as NAME,
// AUTH and ANNO chunks.
struct AiffOptions {
    int bitsPerSample = 24;  // 16 or 24
    bool dither = true;
    bool shaped = false;
    std::uint64_t ditherSeed = 0;
    FileMetadata metadata;
};
Result<std::vector<std::uint8_t>> encodeAiff(const AudioData& data,
                                             const AiffOptions& options = {});
// Decodes AIFF bytes (dr_wav). Not a media import format: detect() and
// readAudio() do not take AIFF.
Result<AudioData> decodeAiff(std::span<const std::uint8_t> bytes);

}  // namespace supple::audio
