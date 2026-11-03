// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The render processing chain (engine.md §3 Chain, ADR-0055 §6): seamless
// loop fold, mono fold, normalization, true-peak limit, trim; then, per
// output, resample, dither and encode; then the delivery checks. Offline
// only (model/worker threads); deterministic: nothing reads the clock.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/model/render_settings.h"

namespace supple::engine {
namespace rs = model::render;

// One output's encoding, fully resolved.
struct RenderEncoding {
    rs::Format format = rs::Format::Wav;
    int bitDepth = 32;  // 16, 24 or 32 (float, WAV); 0 for MP3 and Vorbis
    rs::Dither dither = rs::Dither::Tpdf;
    int sampleRate = 48000;
    rs::Mp3Mode mp3Mode = rs::Mp3Mode::Cbr;
    int mp3Bitrate = 320, mp3VbrQuality = 2;
    double vorbisQuality = 6.0;
    std::uint64_t ditherSeed = 0;
    std::string extension() const;
};

// What a file carries besides audio (session-model §1.10 metadata).
struct RenderTags {
    std::string title, artist, album, date, genre, comment, isrc;
    int trackNumber = 0;
    std::vector<std::uint8_t> cover;  // PNG or JPEG bytes
    std::string coverMime;
    bool bwf = false;
    std::uint64_t timeReference = 0;  // samples since midnight, at the output rate
    std::string originator, description;
    std::optional<double> loudness, lra, truePeak, maxMomentary, maxShortTerm;
    struct Marker {
        std::uint64_t frame = 0, length = 0;  // at the output rate; length 0: a marker
        std::string name;
    };
    std::vector<Marker> markers;
};

// Step 2: sums the samples past `rangeFrames` onto the start, wrapping, and
// cuts the audio to exactly `rangeFrames`.
void foldSeamlessLoop(audio::AudioData& data, std::size_t rangeFrames);
// Step 4: (L+R)/2.
void foldMono(audio::AudioData& data);
// The level normalization measures (LUFS or dB), -inf for silence.
Result<double> measureLevel(const audio::AudioData& data, rs::NormalizeMode mode);
// The linear gain from a measured level to the target; 1 when the level is
// not finite, at most 1 with only_reduce.
Result<double> normalizationGain(double measured, double target, bool onlyReduce);
// Multiplies every sample; refuses gains that would leave the float range.
Status applyGain(audio::AudioData& data, double gain);
// Step 6: look-ahead gain envelope with oversampled detection, then a final trim.
Status limitTruePeak(audio::AudioData& data, double ceilingDb);
// Step 7: removes frames below the threshold at either end (at least one
// frame stays). Returns the frames removed from the start.
std::size_t trimSilence(audio::AudioData& data, bool start, bool end, double thresholdDb);
// Normalization then limiting, as before ADR-0055 (mode by name).
Status normalizeRenderAudio(audio::AudioData& data, std::string_view mode, double target,
                            std::optional<double> limitTruePeakDb = {});

// Step 8 for one output: `data` already at the output's rate.
Result<std::vector<std::uint8_t>> encodeRender(const audio::AudioData& data,
                                               const RenderEncoding& encoding,
                                               const RenderTags& tags);
// Decodes an encoded output (for the checks).
Result<audio::AudioData> decodeRender(std::span<const std::uint8_t> bytes, rs::Format format);

// The delivery checks of one output (engine.md §3 Checks). `measured`
// receives the decoded output's loudness for the report.
struct RenderCheckSpec {
    std::optional<rs::NormalizeMode> loudnessMode;  // a LUFS mode, when normalizing to one
    double target = -14;
    bool onlyReduce = false;
    std::optional<double> ceiling;
};
struct RenderMeasurement {
    double integrated = 0, lra = 0, maxShortTerm = 0, maxMomentary = 0, truePeak = 0,
           samplePeak = 0, seconds = 0;
};
Result<RenderMeasurement> measureRender(const audio::AudioData& data);
json::Value renderChecks(const RenderMeasurement& measured, const RenderCheckSpec& spec);
// The plain-text loudness report (no timestamps).
std::string loudnessReport(const std::string& file, const RenderEncoding& encoding,
                           const RenderMeasurement& measured, const json::Value& checks);

}  // namespace supple::engine
