// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "supple/audio/audio_file.h"
#include "supple/base/thread.h"
#include "supple/engine/realtime.h"

namespace supple::engine {
// The file audition's decoder (ADR-0094): one worker, one replaceable
// request, one finished voice. It reads the file's start (readAudioHead,
// its first two channels) and resamples it to the device's rate, off the
// model and audio threads; a newer request or cancel() stops a read between
// chunks and discards its result.
class AuditionLoader {
  public:
    struct Request {
        std::filesystem::path path;
        std::int64_t maxFrames = 0;  // at the file's rate
        double rate = 48000;         // the device's
    };
    struct Done {
        std::uint64_t generation = 0;
        Result<std::shared_ptr<const AuditionVoice>> voice;
    };
    // `stopCheck` (tests): called on the worker with the read's generation
    // each time the read asks whether to stop.
    explicit AuditionLoader(std::function<void()> wake,
                            std::function<void(std::uint64_t)> stopCheck = {})
        : wake_(std::move(wake)),
          stopCheck_(std::move(stopCheck)),
          worker_([this](StopToken) { run(); }) {}
    ~AuditionLoader() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        changed_.notify_one();
        worker_.join();
    }
    AuditionLoader(const AuditionLoader&) = delete;
    AuditionLoader& operator=(const AuditionLoader&) = delete;
    // Returns the request's generation, which its Done carries.
    std::uint64_t request(Request r) {
        std::lock_guard lock(mutex_);
        const auto generation = ++generation_;
        request_ = std::make_pair(std::move(r), generation);
        done_.reset();
        changed_.notify_one();
        return generation;
    }
    void cancel() {
        std::lock_guard lock(mutex_);
        ++generation_;
        request_.reset();
        done_.reset();
    }
    // Reads that a newer request or cancel() overtook: their result dropped.
    std::uint64_t discarded() const { return discarded_.load(std::memory_order_acquire); }
    std::optional<Done> take() {
        std::lock_guard lock(mutex_);
        auto out = std::move(done_);
        done_.reset();
        return out;
    }

  private:
    static Result<std::shared_ptr<const AuditionVoice>> load(const Request& r,
                                                             const std::function<bool()>& stop) {
        auto data = audio::readAudioHead(
            r.path, {.maxFrames = r.maxFrames, .maxChannels = 2, .stop = stop});
        if (!data) return std::move(data).error();
        if (data->sampleRate != r.rate) {
            if (stop()) return Error("audio.cancelled", "the read was cancelled");
            auto resampled = audio::resample(*data, r.rate);
            if (!resampled) return std::move(resampled).error();
            data = std::move(resampled);
        }
        auto voice = std::make_shared<AuditionVoice>();
        if (!data->channels.empty()) voice->left = std::move(data->channels[0]);
        if (data->channels.size() > 1) voice->right = std::move(data->channels[1]);
        return std::shared_ptr<const AuditionVoice>(std::move(voice));
    }
    void run() {
        for (;;) {
            std::optional<std::pair<Request, std::uint64_t>> request;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || request_.has_value(); });
                if (stopping_) return;
                request = std::move(request_);
                request_.reset();
            }
            const auto serial = request->second;
            auto voice = load(request->first, [&] {
                if (stopCheck_) stopCheck_(serial);
                return generation_.load() != serial;
            });
            {
                std::lock_guard lock(mutex_);
                if (stopping_) return;
                if (generation_.load() != serial) {
                    discarded_.fetch_add(1, std::memory_order_release);
                    continue;
                }
                done_ = Done{serial, std::move(voice)};
            }
            wake_();
        }
    }
    std::function<void()> wake_;
    std::function<void(std::uint64_t)> stopCheck_;
    std::atomic<std::uint64_t> discarded_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> generation_{0};
    std::optional<std::pair<Request, std::uint64_t>> request_;
    std::optional<Done> done_;
    JoiningThread worker_;
};
}  // namespace supple::engine
