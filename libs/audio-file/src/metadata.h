// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Metadata helpers shared by the writers (metadata.cpp).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "supple/audio/audio_file.h"

namespace supple::audio::detail {
// Adds bext (before fmt), LIST INFO and cue/adtl (after data) to a RIFF WAV.
Result<std::vector<std::uint8_t>> wavWithMetadata(std::vector<std::uint8_t> riff,
                                                  const FileMetadata& metadata);
// Adds VORBIS_COMMENT and PICTURE blocks after STREAMINFO.
std::vector<std::uint8_t> flacWithTags(std::vector<std::uint8_t> flac, const FileTags& tags);
}  // namespace supple::audio::detail
