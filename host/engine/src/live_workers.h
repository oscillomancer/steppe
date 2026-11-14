// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Callback-only fork-join executor (ADR-0024). The background Workers pool
// parks within jobs and must never be called from the device callback.
#pragma once

#include "supple/base/result.h"
#include "thread_startup.h"
#include "workers.h"

namespace supple::engine {
class Backend;
namespace detail {

class LiveWorkers {
  public:
    using Fn = Workers::Fn;
    // Test seams: Join precedes the claim; Claimed precedes the open check;
    // Entered/Leave surround job access. Published/JoinWait run on the caller.
    enum class Event { Join, Claimed, Entered, Leave, Missed, Idle, Published, JoinWait };
    struct Hooks {
        void (*event)(void*, Event) = nullptr;
        void* context = nullptr;
    };
    explicit LiveWorkers(unsigned helpers);
    LiveWorkers(unsigned helpers, Hooks hooks, Backend* backend = nullptr);
    Status startupStatus() const { return startupStatus_; }
    ~LiveWorkers();
    LiveWorkers(const LiveWorkers&) = delete;
    LiveWorkers& operator=(const LiveWorkers&) = delete;

    unsigned helpers() const { return static_cast<unsigned>(threads_.size()); }
    // Main thread, before publication: CPU capacity and backend RT scheduling.
    static unsigned maxHelpers();
    Status configure(Backend& backend);
    ThreadPolicy policy();  // main thread: the first helper's (None without helpers)
    // One callback at a time. No helper can access this job after return.
    void run(TaskGraph& graph, Fn fn, void* context) SUPPLE_RT;

  private:
    void shutDown();
    void loop(unsigned index);
    void help(TaskGraph& graph, Fn fn, void* context) SUPPLE_RT;
    void event(Event e) {
        if (hooks_.event) hooks_.event(hooks_.context, e);
    }
    struct alignas(64) Slot {
        std::atomic<std::uint32_t> sequence{0};
    };
    std::unique_ptr<Slot[]> slots_;
    ThreadStartup startup_;
    Status startupStatus_;
    Backend* nativeBackend_ = nullptr;
    std::vector<std::thread> threads_;
    Hooks hooks_;
    std::atomic<unsigned> active_{0};
    std::atomic<bool> open_{false};
    std::atomic<bool> quit_{false};
    TaskGraph* job_ = nullptr;
    Fn fn_ = nullptr;
    void* context_ = nullptr;
    // The callback's bridge deadline for this job (ADR-0081), which helpers
    // take for their bridged calls; 0 for none.
    std::int64_t deadline_ = 0;
};

}  // namespace detail
}  // namespace supple::engine
