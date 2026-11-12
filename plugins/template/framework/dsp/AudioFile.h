// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Audio files: WAV (and RF64, W64), AIFF and FLAC in, through David Reid's
// dr_wav and dr_flac (MIT-0). Whole files decoded into memory, resampled to
// the session's rate, and long ones streamed from disk instead. A path may
// be a factory reference ("factory:pad.flac", core/Files.h): the product's
// own file, read from the binary.
//
// All of it reads files and allocates: the main thread or a worker, never
// the audio thread -- except DiskStream::read() and restart(), which are
// made for it. A file property marked .audioFile() (core/Property.h) gets
// all this from the framework, and its processor the decoded audio.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Ring.h"

namespace supple::dsp {

// Decoded audio, one vector per channel.
struct AudioData {
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channel;

    int channels() const { return static_cast<int>(channel.size()); }
    size_t frames() const { return channel.empty() ? 0 : channel[0].size(); }
    double seconds() const { return sampleRate > 0.0 ? static_cast<double>(frames()) / sampleRate : 0.0; }
    // Each channel's first sample, for Convolution::prepare and the like.
    std::vector<const float*> data() const {
        std::vector<const float*> p;
        for (const auto& c : channel) p.push_back(c.data());
        return p;
    }
};

// The extensions the decoder reads, for a file dialog or a drop.
inline constexpr const char* kAudioFileExtensions[] = {"wav", "wave", "aif", "aiff", "aifc", "flac", "rf64", "w64"};

// Decodes a whole file, or its first `maxSeconds` when that is above zero:
// the rest of a longer one is never read. False, with a reason in `error`,
// when it cannot: missing, unreadable, or a format it does not know.
bool loadAudioFile(const std::string& path, AudioData& out, std::string* error = nullptr, double maxSeconds = 0.0);

// 32-bit float WAV, for tests and exports. False, with a reason in `error`,
// when the file cannot be made or the disk does not take all of it (full,
// over quota): a buffered write's failure shows only when the header is
// finished and the file closed, and those are checked too.
bool saveWavFile(const std::string& path, const AudioData& audio, std::string* error = nullptr);

// The same audio at another rate: windowed sinc, flat to 0.45 of the lower
// rate and 100 dB down past its Nyquist. Returns a copy when the rates match.
AudioData resample(const AudioData& in, double sampleRate);

// A decoder read a little at a time. Non-finite float samples become zero,
// in both whole-file loading and streaming.
class AudioReader {
  public:
    AudioReader();
    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool isOpen() const;
    int channels() const;
    double sampleRate() const;
    uint64_t frames() const;
    // Up to `frames` interleaved frames; how many it read (0 at the end).
    size_t read(float* interleaved, size_t frames);
    bool seek(uint64_t frame);

  private:
    struct Decoder;
    std::unique_ptr<Decoder> decoder_;
};

// A file too long to hold in memory, played from disk: its first seconds are
// read at open() so playback can start at once, and a worker reads ahead of
// the audio thread from then on.
//
//   main:    stream_.open(path);  id_ = worker_.repeat([this] { stream_.service(); }, 5);
//   audio:   stream_.restart();   ...   stream_.read(out, channels, frames);
//   close:   worker_.cancel(id_); stream_.close();
//
// The audio thread restarts at the top and reads forward; if the disk falls
// behind, read() returns short and underruns() counts it.
class DiskStream {
  public:
    static constexpr size_t kChunkFrames = 4096;
    static constexpr int kChunks = 16;  // read-ahead: 64k frames

    DiskStream() = default;
    ~DiskStream() = default;
    DiskStream(const DiskStream&) = delete;
    DiskStream& operator=(const DiskStream&) = delete;

    // Main thread, while neither read() nor service() runs. Allocates.
    // Refuses unknown/empty lengths; loadAudioFile can decode to EOF.
    bool open(const std::string& path, double headSeconds = 1.0, std::string* error = nullptr);
    void close();
    bool isOpen() const { return open_; }
    int channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }
    uint64_t frames() const { return frames_; }

    // --- audio thread ------------------------------------------------------
    // Back to the first frame.
    void restart();
    // Up to `frames` frames into out[0..channels), planar; the file's
    // channels beyond `channels` are dropped and missing ones repeat its
    // last. Returns how many it wrote: fewer at the end of the file, or when
    // the disk is behind.
    int read(double* const* out, int channels, int frames);
    uint64_t position() const { return position_; }
    uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // --- the worker ----------------------------------------------------------
    void service();

  private:
    struct Chunk {
        std::vector<float> data;  // interleaved
        uint32_t generation = 0;
        uint64_t start = 0;
        size_t frames = 0;
    };

    AudioReader reader_;
    bool open_ = false;
    int channels_ = 0;
    double sampleRate_ = 0.0;
    uint64_t frames_ = 0, headFrames_ = 0;
    std::vector<float> head_;  // interleaved
    std::vector<Chunk> chunks_;
    Ring<int, 5> filled_;  // worker -> audio
    Ring<int, 5> free_;    // audio -> worker
    std::atomic<uint32_t> wanted_{0};
    std::atomic<uint32_t> underruns_{0};
    // The audio thread's.
    uint32_t generation_ = 0;
    uint64_t position_ = 0;
    int current_ = -1;
    size_t offset_ = 0;
    // The worker's.
    uint32_t serviceGeneration_ = 0;
    uint64_t next_ = 0;
    std::vector<int> spare_;
};

}  // namespace supple::dsp
