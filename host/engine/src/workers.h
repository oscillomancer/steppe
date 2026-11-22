// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: the engine's worker pool (ADR-0012/0023, engine.md §2.2). It runs a
// task graph — one render block of a plan, one task per node — on its
// threads and the submitting thread together: a task starts once the tasks
// it depends on have finished; ready tasks wait in a bounded lock-free queue.
// Results do not depend on the thread count because every task reads only
// finished inputs and writes only its own state.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "supple/base/result.h"
#include "supple/base/rt.h"
#include "supple/base/thread.h"
#include "thread_startup.h"

namespace supple::engine::detail {

// A bounded multi-producer multi-consumer queue of task indices (Vyukov's
// design: one sequence number per cell). Capacity is a power of two.
class ReadyQueue {
  public:
    void init(std::size_t minCapacity);
    bool push(std::uint32_t task) SUPPLE_RT;
    bool pop(std::uint32_t& task) SUPPLE_RT;
    // Nothing pushed that is not popped (a push still publishing counts).
    bool empty() const SUPPLE_RT {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

  private:
    struct Cell {
        std::atomic<std::size_t> seq{0};
        std::uint32_t task = 0;
    };
    std::unique_ptr<Cell[]> cells_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

// The dependency structure of a plan's nodes and the per-run counters.
// Built on main; used by one background or live run at a time.
struct TaskGraph {
    std::vector<std::uint32_t> preds;              // how many tasks each waits for
    std::vector<std::vector<std::uint32_t>> succ;  // the tasks waiting for each
    std::unique_ptr<std::atomic<std::uint32_t>[]> pending;
    ReadyQueue ready;
    std::atomic<std::uint32_t> remaining{0};
    // Size of a disjoint dependency-chain cover: a conservative upper bound
    // on simultaneous tasks, computed once when the graph is built.
    unsigned parallelism = 0;

    // `deps[i]` lists the tasks task i reads from; every one must be < i.
    void build(const std::vector<std::vector<std::uint32_t>>& deps);
    std::uint32_t size() const { return static_cast<std::uint32_t>(preds.size()); }
};

class Workers {
  public:
    using Fn = void (*)(void* ctx, std::uint32_t task);

    // `helpers` threads besides the caller (0: the caller runs everything).
    // Internal deterministic scheduling seam; null in production. Hooks may
    // block in tests, including immediately before an atomic wait.
    enum class Event { Join, Leave, Wait, JoinWait };
    struct Hooks {
        void (*event)(void*, Event) = nullptr;
        void* context = nullptr;
    };
    explicit Workers(unsigned helpers);
    // `self`: without a device `backend`, a policy each helper sets on itself as
    // it starts, all or none (ThreadStartup); startupStatus() says which.
    Workers(unsigned helpers, Hooks hooks, Backend* backend = nullptr,
            std::optional<ThreadPolicy> self = std::nullopt);
    Status startupStatus() const { return startupStatus_; }
    ~Workers();
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    unsigned helpers() const { return static_cast<unsigned>(threads_.size()); }
    // The default helper count: every CPU the calling thread may use but two,
    // less the caller.
    static unsigned defaultHelpers();
    // Main thread: every helper's scheduling (ADR-0026), stopping at the first
    // failure; the first helper's (None without helpers).
    Status setPolicy(ThreadPolicy policy);
    ThreadPolicy policy();

    // Runs every task of `g` (fn(ctx, task) on any of the threads) and
    // returns when all selected helpers have left. One run at a time.
    // Background/offline only: scheduling may block, never call from RT.
    void run(TaskGraph& g, Fn fn, void* ctx);

  private:
    void loop(unsigned index);
    void help(TaskGraph& g, Fn fn, void* ctx);
    void event(Event e) {
        if (hooks_.event) hooks_.event(hooks_.context, e);
    }

    struct alignas(64) Slot {
        // Native futex-sized values give each idle helper its own wait address
        // on Linux, avoiding shared wait-table wakeups for larger atomics.
        // A selected slot must acknowledge before it can advance again, so a
        // waiter cannot miss a full sequence wrap.
        std::atomic<std::uint32_t> sequence{0};
    };

    ThreadStartup startup_;
    Status startupStatus_;
    Backend* nativeBackend_ = nullptr;
    std::vector<std::thread> threads_;
    std::unique_ptr<Slot[]> slots_;
    // Enrollment is complete BEFORE any helper can read the job. run() cannot
    // return (or publish another graph) until every selected slot acknowledges.
    std::atomic<unsigned> active_{0};
    std::atomic<std::uint64_t> workSequence_{0};
    std::atomic<bool> quit_{false};
    Hooks hooks_;
    TaskGraph* job_ = nullptr;
    Fn fn_ = nullptr;
    void* ctx_ = nullptr;
};

}  // namespace supple::engine::detail
