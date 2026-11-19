// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Work the supervisor does off its receive loop (ADR-0083, round 6a).
//
// The supervisor reads what its worker sends in one loop. A bridge report is a file write and a
// retention pass over every report in the folder: it must not hold the loop up, or the packets
// behind it (a bridge's later events, the worker's own terminal packet) wait for the disk.
// `BackgroundWriter` runs such work, in order, on one thread of its own. `post` only queues and
// never waits for earlier work; `finish` runs what is queued and joins, and is what the
// supervisor calls before it writes the worker's own report, so that two retention passes never
// overlap and no report is left unwritten when the supervisor ends. Work posted after `finish`
// runs at once on the caller's thread, as does work posted
// when no thread can be started.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace supple::diagnostics::detail {

class BackgroundWriter {
  public:
    BackgroundWriter() = default;
    BackgroundWriter(const BackgroundWriter&) = delete;
    BackgroundWriter& operator=(const BackgroundWriter&) = delete;
    ~BackgroundWriter() { finish(); }

    // Queues `work` (which must not throw) and returns. The thread starts with the first piece.
    // When the system gives no thread, `work` runs at once on the caller's: a report is written
    // late, never lost, and the supervisor stays alive.
    void post(std::function<void()> work) {
        {
            std::lock_guard lock(mutex_);
            if (!finished_ && startThread()) {
                queue_.push_back(std::move(work));
                wake_.notify_one();
                return;
            }
        }
        work();
    }
    // Tests only: the next attempt to start the thread fails, as it does when the process is out
    // of threads.
    void failNextStartForTests() {
        std::lock_guard lock(mutex_);
        failStart_ = true;
    }
    // Runs what is queued, then joins. Safe to call again.
    void finish() {
        std::thread thread;
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
            wake_.notify_one();
            thread = std::move(thread_);
        }
        if (thread.joinable()) thread.join();
    }

  private:
    // With the mutex held. True when the thread runs (already, or now).
    bool startThread() {
        if (thread_.joinable()) return true;
        try {
            if (std::exchange(failStart_, false))
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            thread_ = std::thread([this] { run(); });
            return true;
        } catch (const std::system_error&) {
            return false;
        }
    }
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return finished_ || !queue_.empty(); });
            if (queue_.empty()) return;  // finished, and nothing left
            auto work = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            work();
            lock.lock();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
    bool finished_ = false, failStart_ = false;
};

}  // namespace supple::diagnostics::detail
