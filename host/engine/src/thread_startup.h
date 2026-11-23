// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <optional>

#include "supple/base/thread.h"
#include "supple/engine/realtime.h"
namespace supple::engine::detail {
// Non-RT startup barrier. Every helper applies device policy on itself; all
// roll back if any refuses. Main publishes live jobs only after finish(),
// which returns once every thread has taken or rolled back its policy.
// Without a device policy, `self` is a policy each helper sets on itself
// instead: macOS takes a thread's QoS only from the thread (ADR-0026).
class ThreadStartup {
  public:
    // A test seam for `self`: sets `policy` on the calling thread, which is
    // thread `index` of the barrier, in place of setThreadPolicy. Null in
    // production.
    using Apply = Status (*)(void* context, unsigned index, ThreadPolicy policy);
    ThreadStartup(unsigned count, Backend* backend, bool live,
                  std::optional<ThreadPolicy> self = std::nullopt, Apply apply = nullptr,
                  void* context = nullptr)
        : results_(backend || self ? count : 0),
          backend_(backend),
          live_(live),
          self_(backend ? std::nullopt : self),
          apply_(apply),
          context_(context) {}
    bool enter(unsigned index) {
        if (!backend_ && !self_) return false;
        results_[index] =
            backend_ ? backend_->prepareCurrentThread(live_) : applySelf(index, *self_);
        ready_.fetch_add(1, std::memory_order_release);
        ready_.notify_one();
        gate_.wait(0, std::memory_order_acquire);
        const bool taken = gate_.load(std::memory_order_acquire) == 1;
        if (!taken) {
            if (backend_)
                backend_->releaseCurrentThread();
            else
                (void)applySelf(index, {ThreadPolicy::Kind::Normal, 0});
        }
        settled_.fetch_add(1, std::memory_order_release);
        settled_.notify_one();
        return taken;
    }
    Status finish() {
        if (!backend_ && !self_) return okStatus();
        unsigned seen;
        while ((seen = ready_.load(std::memory_order_acquire)) < results_.size()) ready_.wait(seen);
        Status result;
        for (const auto& status : results_)
            if (!status) {
                result = status;
                break;
            }
        gate_.store(result ? 1 : 2, std::memory_order_release);
        gate_.notify_all();
        // A refusal is rolled back by each thread on itself: wait for them, so
        // what is read back afterwards is what they hold.
        while ((seen = settled_.load(std::memory_order_acquire)) < results_.size())
            settled_.wait(seen);
        return result;
    }
    void cancel() {
        gate_.store(2, std::memory_order_release);
        gate_.notify_all();
    }
    struct Cleanup {
        Backend* backend;
        ~Cleanup() {
            if (backend) backend->releaseCurrentThread();
        }
    };

  private:
    Status applySelf(unsigned index, ThreadPolicy policy) {
        return apply_ ? apply_(context_, index, policy)
                      : setThreadPolicy(currentThreadHandle(), policy);
    }
    std::vector<Status> results_;
    Backend* backend_;
    bool live_;
    std::optional<ThreadPolicy> self_;
    Apply apply_;
    void* context_;
    std::atomic<unsigned> ready_{0}, gate_{0}, settled_{0};
};
}  // namespace supple::engine::detail
