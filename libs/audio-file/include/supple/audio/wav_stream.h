// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A float32 WAV file written incrementally (recording, engine.md §4): the
// header's sizes are rewritten on updateHeader() and close(), so a crash
// leaves a readable file up to the last update. Not real-time: a writer
// thread owns it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "supple/base/result.h"

namespace supple::audio {

class WavStreamWriter {
  public:
    WavStreamWriter() = default;
    ~WavStreamWriter();
    WavStreamWriter(const WavStreamWriter&) = delete;
    WavStreamWriter& operator=(const WavStreamWriter&) = delete;

    Status open(const std::filesystem::path& path, double sampleRate, std::uint32_t channels);
    // Interleaved frames.
    Status append(const float* frames, std::size_t count);
    Status updateHeader();  // sizes of what is written so far, flushed to the file
    // Hands what is buffered to the OS (a crash of the process keeps it; the
    // header catches up at the next update or on repair).
    Status flush();
    Status close();
    // Closes without the final header rewrite, as a crash would leave the
    // file (tests and fixtures).
    void abandon();
    bool isOpen() const { return file_ != nullptr; }
    std::uint64_t frames() const { return frames_; }
    std::uint32_t channels() const { return channels_; }

  private:
    friend struct WavStreamWriterTestAccess;
    struct IoHooks {
        void* context = nullptr;
        std::size_t (*write)(void*, const void*, std::size_t, std::FILE*) = nullptr;
        int (*flush)(void*, std::FILE*) = nullptr;
    } io_;
    void reconcileWritten();
    std::FILE* file_ = nullptr;
    bool failed_ = false;
    std::uint32_t channels_ = 0;
    std::uint64_t frames_ = 0;
};

// A file WavStreamWriter wrote, as found on disk (crash recovery): the
// header's frame count and what the file actually holds.
struct WavStreamInfo {
    double sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t headerFrames = 0;  // what the last header rewrite recorded
    std::uint64_t fileFrames = 0;    // whole frames present after the header
};
// audio.not_stream when the file is not in the writer's layout (float32,
// 44-byte header, the data chunk last).
Result<WavStreamInfo> inspectWavStream(const std::filesystem::path& path);
// Rewrites the header's sizes to cover every whole frame in the file (a
// crash between header rewrites leaves the tail unaccounted). Returns the
// frames the file now declares.
Result<std::uint64_t> repairWavStream(const std::filesystem::path& path);

}  // namespace supple::audio
