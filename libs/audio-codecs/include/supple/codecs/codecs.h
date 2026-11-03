// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Lossy render encoders (engine.md §3 Chain, ADR-0055): MP3 (LAME 3.100) and
// Ogg Vorbis (libvorbis 1.3.7), and an Ogg Vorbis decoder (vorbisfile) for
// the delivery checks. Main/worker threads only. Deterministic: the same
// audio and options give the same bytes (no clock, no randomness; the Ogg
// stream serial number is an option).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"

namespace supple::codecs {

// MP3: 1 or 2 channels at an MPEG rate (8000-48000). A LAME/Xing ("Info"
// for CBR) header carries the encoder delay and padding for gapless
// playback; tags go into an ID3v2.3 tag at the start (title, artist, album,
// track, year, genre, comment, TSRC for the ISRC, TSSE for the software, APIC
// for the cover).
struct Mp3Options {
    bool vbr = false;
    int bitrateKbps = 320;  // CBR
    int vbrQuality = 2;     // VBR, 0 (best) to 9
    audio::FileTags tags;
};
Result<std::vector<std::uint8_t>> encodeMp3(const audio::AudioData& data,
                                            const Mp3Options& options = {});

// Ogg Vorbis: 1 or 2 channels, 8000-192000 Hz, quality -1 to 10. Tags are
// Vorbis comments; the cover is METADATA_BLOCK_PICTURE.
struct VorbisOptions {
    double quality = 6.0;
    std::uint32_t serial = 1;  // the Ogg logical stream's serial number
    audio::FileTags tags;
};
Result<std::vector<std::uint8_t>> encodeVorbis(const audio::AudioData& data,
                                               const VorbisOptions& options = {});
Result<audio::AudioData> decodeVorbis(std::span<const std::uint8_t> bytes);

// The LAME version string ("3.100"), for reports.
std::string lameVersion();

}  // namespace supple::codecs
