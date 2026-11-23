// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Waveform peak pyramids (docs/specs/gui.md §3.1): min/max/RMS per channel
// at 64…16384 samples per bucket, built off the UI thread, cached on disk
// under the user cache directory keyed by the media's content hash (never in
// the project). The arrange view samples the coarsest level with at least
// one bucket per pixel column.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "supple/base/stop.h"

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"

namespace supple::peaks {

inline constexpr std::array<std::uint32_t, 5> kSamplesPerBucket{64, 256, 1024, 4096, 16384};

struct Bucket {
    std::int16_t min = 0, max = 0, rms = 0;  // Q15 (±1.0 = ±32767), clamped
};

struct Level {
    std::uint32_t samplesPerBucket = 0;
    std::size_t count = 0;        // buckets per channel
    std::vector<Bucket> buckets;  // interleaved: bucket i of channel c at i * channels + c
};

struct Span {
    float min = 0, max = 0, rms = 0;
};

class Pyramid {
  public:
    static Pyramid build(const audio::AudioData& audio);
    static Result<Pyramid> deserialize(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> serialize() const;

    int channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }
    std::int64_t frames() const { return frames_; }
    const std::vector<Level>& levels() const { return levels_; }
    // The coarsest level whose buckets are no wider than `samplesPerColumn`
    // (the finest when even that is wider: zoomed in past 64 samples/pixel).
    const Level& pick(double samplesPerColumn) const;
    // Min/max/RMS over source frames [from, to) of `channel`, from `level`.
    Span range(const Level& level, int channel, double from, double to) const;

  private:
    int channels_ = 0;
    double sampleRate_ = 0;
    std::int64_t frames_ = 0;
    std::vector<Level> levels_;
};

// $XDG_CACHE_HOME/<slug>/peaks, else ~/.cache/<slug>/peaks.
std::filesystem::path defaultCacheDir(std::string_view slug);

// Loads or builds pyramids on one worker thread. get() never blocks: it
// returns the pyramid when ready, otherwise queues the work (once) and
// returns null; `post` delivers the ready notification to the owner's thread.
class PeakService {
  public:
    using Post = std::function<void(std::function<void()>)>;
    PeakService(std::filesystem::path cacheDir, Post post);
    ~PeakService();
    PeakService(const PeakService&) = delete;
    PeakService& operator=(const PeakService&) = delete;

    // `hash` is the media's "sha256:<hex>" (model::MediaSource::hash); when
    // empty, path, size and mtime key both memory and disk caches. A file's
    // size and mtime are read again at most every two seconds, so a repaint
    // does not touch the file system.
    std::shared_ptr<const Pyramid> get(const std::string& hash, const std::filesystem::path& media);
    // Forget the remembered sizes and mtimes (the session changed, so files
    // may have been replaced): the next lookups read them again.
    void revalidate();
    // Called on the owner's thread when a pyramid becomes ready (or fails),
    // with the key: the hash, or "path:<media>:<size>:<mtime>".
    std::function<void(const std::string& key)> onReady;
    // A failure is remembered for that media revision; file changes permit retry.
    std::optional<Error> failure(const std::string& hash, const std::filesystem::path& media) const;

    struct Stats {
        int built = 0, loaded = 0, failed = 0;
    };
    Stats stats() const;

    // Lets go of every media file under `dir` and opens none there until the
    // returned function is called (a file that is open cannot be removed on
    // Windows: the engine's first save calls this before it removes the
    // scratch media it has moved). A read in progress there stops between two
    // chunks, without counting as a failure, and is asked for again when the
    // hold ends (onReady); get() queues nothing there meanwhile. Returns once
    // the worker has let go. Any thread; holds may nest. The returned function
    // does nothing once the service is gone, a check that holds only if it is
    // called on the thread that destroys the service (the app's saves and its
    // teardown share the GUI thread).
    [[nodiscard]] std::function<void()> holdUnder(const std::filesystem::path& dir);
    // Whether a hold is in place (tests wait for one).
    bool holding() const;
    // Tests: called on the worker between two chunks of a media read.
    std::function<void(const std::filesystem::path& media)> readHook;

  private:
    void run(supple::StopToken stop);
    std::string key(const std::string& hash, const std::filesystem::path& media) const;
    std::filesystem::path cacheDir_;
    Post post_;
    mutable std::mutex mutex_;
    // Hashless media: the key last read for a path, and when.
    mutable std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>
        pathKeys_;
    std::condition_variable_any wake_;
    struct Job {
        std::string key, hash;
        std::filesystem::path media;
    };
    std::deque<Job> queue_;
    std::map<std::string, std::shared_ptr<const Pyramid>> ready_;
    std::map<std::string, Error> failed_;
    std::map<std::string, bool> pending_;
    Stats stats_;
    // holdUnder: the folders held, the media being read now (empty: none) and
    // the keys a hold stopped or turned away, to announce when it ends.
    std::vector<std::filesystem::path> held_;
    std::filesystem::path reading_;
    std::condition_variable_any idle_;
    std::vector<std::string> stopped_;
    bool heldLocked(const std::filesystem::path& media) const;
    // Notifications posted to the owner check this first: the service may be
    // gone by the time the owner's loop runs them.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    supple::JoiningThread worker_;  // last: joined before the rest is destroyed
};

}  // namespace supple::peaks
