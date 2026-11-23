// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <condition_variable>
#include <mutex>
#include "supple/base/thread.h"
#include "supple/engine/offline.h"

namespace supple::engine {
// One worker, one replaceable pending request, one immutable completed result.
// No plug-ins, model mutation, or callback-owned state lives on this worker.
class SourcePreparation {
  public:
    // What decodes the media for a request: MediaCache::prepareSources unless a test gives
    // its own (`cancelled` says the request was overtaken).
    using Prepare = std::function<Result<std::shared_ptr<const PreparedSources>>(
        const model::Session&, const std::filesystem::path&, double,
        const std::function<bool()>& cancelled)>;
    explicit SourcePreparation(std::function<void()> wake, Prepare prepare = {})
        : wake_(std::move(wake)),
          prepare_(std::move(prepare)),
          worker_([this](StopToken) { run(); }) {}
    ~SourcePreparation() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        changed_.notify_one();
        worker_.join();
    }
    void request(model::Session session, std::filesystem::path directory, double rate) {
        std::lock_guard lock(mutex_);
        const auto generation = ++generation_;
        request_ = Request{std::move(session), std::move(directory), rate, generation};
        result_.reset();
        changed_.notify_one();
    }
    // Before files it may be reading are removed (a first save moves the scratch media): drops
    // the pending request, cancels the running one and returns when the worker is not reading.
    // It starts nothing until resume(); the caller requests again for what it has moved.
    void pause() {
        std::unique_lock lock(mutex_);
        paused_ = true;
        ++generation_;
        request_.reset();
        result_.reset();
        idle_.wait(lock, [&] { return !busy_; });
    }
    void resume() {
        {
            std::lock_guard lock(mutex_);
            paused_ = false;
        }
        changed_.notify_one();
    }
    std::optional<Result<std::shared_ptr<const PreparedSources>>> take() {
        std::lock_guard lock(mutex_);
        auto out = std::move(result_);
        result_.reset();
        return out;
    }

  private:
    struct Request {
        model::Session session;
        std::filesystem::path directory;
        double rate;
        std::uint64_t generation;
    };
    void run() {
        MediaCache cache;
        for (;;) {
            std::optional<Request> request;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,
                              [&] { return stopping_ || (request_.has_value() && !paused_); });
                if (stopping_) return;
                request = std::move(request_);
                request_.reset();
                busy_ = true;
            }
            const auto serial = request->generation;
            const auto cancelled = [&] { return generation_.load() != serial; };
            auto result =
                prepare_ ? prepare_(request->session, request->directory, request->rate, cancelled)
                         : cache.prepareSources(request->session, request->directory, request->rate,
                                                cancelled);
            cache.trimVariants();
            {
                std::lock_guard lock(mutex_);
                busy_ = false;
                idle_.notify_all();
                if (stopping_) return;
                if (generation_.load() != serial) continue;
                result_ = std::move(result);
            }
            wake_();
        }
    }
    std::function<void()> wake_;
    Prepare prepare_;
    std::mutex mutex_;
    std::condition_variable changed_, idle_;
    bool stopping_ = false, paused_ = false, busy_ = false;
    std::atomic<std::uint64_t> generation_{0};
    std::optional<Request> request_;
    std::optional<Result<std::shared_ptr<const PreparedSources>>> result_;
    JoiningThread worker_;
};
}  // namespace supple::engine
