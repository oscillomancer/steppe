// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "workers.h"

#include <algorithm>
#include <bit>
#include <string>

#include "supple/base/thread.h"
#include "supple/diagctx/table.h"

namespace supple::engine::detail {

// --- ReadyQueue ------------------------------------------------------------------------

void ReadyQueue::init(std::size_t minCapacity) {
    const std::size_t cap = std::bit_ceil(std::max<std::size_t>(minCapacity, 2));
    cells_ = std::make_unique<Cell[]>(cap);
    for (std::size_t i = 0; i < cap; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    mask_ = cap - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

bool ReadyQueue::push(std::uint32_t task) SUPPLE_RT {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& c = cells_[pos & mask_];
        const std::size_t seq = c.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
        if (diff == 0) {
            if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.task = task;
                c.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;  // full
        } else {
            pos = tail_.load(std::memory_order_relaxed);
        }
    }
}

bool ReadyQueue::pop(std::uint32_t& task) SUPPLE_RT {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& c = cells_[pos & mask_];
        const std::size_t seq = c.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
        if (diff == 0) {
            if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                task = c.task;
                c.seq.store(pos + mask_ + 1, std::memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;  // empty
        } else {
            pos = head_.load(std::memory_order_relaxed);
        }
    }
}

// --- TaskGraph -------------------------------------------------------------------------

void TaskGraph::build(const std::vector<std::vector<std::uint32_t>>& deps) {
    const std::size_t n = deps.size();
    preds.assign(n, 0);
    succ.assign(n, {});
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<std::uint32_t> d = deps[i];
        std::sort(d.begin(), d.end());
        d.erase(std::unique(d.begin(), d.end()), d.end());
        preds[i] = static_cast<std::uint32_t>(d.size());
        for (std::uint32_t from : d) succ[from].push_back(static_cast<std::uint32_t>(i));
    }
    // Greedily extend disjoint chains along real dependency edges. At most
    // one member of each chain can execute at a time. This upper bound cannot
    // hide parallel work at unequal depths (unlike a largest-level heuristic).
    std::vector<bool> covered(n, false);
    parallelism = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!covered[i]) ++parallelism;
        for (auto next : succ[i])
            if (!covered[next]) {
                covered[next] = true;
                break;
            }
    }
    pending = std::make_unique<std::atomic<std::uint32_t>[]>(std::max<std::size_t>(n, 1));
    ready.init(n);
}

// --- Workers ---------------------------------------------------------------------------

Workers::Workers(unsigned helpers) : Workers(helpers, Hooks{}) {}

Workers::Workers(unsigned helpers, Hooks hooks, Backend* backend)
    : startup_(helpers, backend, false),
      nativeBackend_(backend),
      slots_(std::make_unique<Slot[]>(helpers)),
      hooks_(hooks) {
    threads_.reserve(helpers);
    try {
        for (unsigned i = 0; i < helpers; ++i)
            threads_.emplace_back([this, i] {
                nameThread("audio-bg-" + std::to_string(i + 1));
                (void)diagctx::registerThread(diagctx::ThreadRole::AudioBackground,
                                              static_cast<std::uint16_t>(i + 1));
                rt::prefaultStack();  // ADR-0025
                const bool elevated = startup_.enter(i);
                ThreadStartup::Cleanup cleanup{elevated ? nativeBackend_ : nullptr};
                loop(i);
            });
    } catch (...) {
        startup_.cancel();
        quit_.store(true);
        for (unsigned i = 0; i < threads_.size(); ++i) {
            slots_[i].sequence.fetch_add(1);
            slots_[i].sequence.notify_one();
        }
        for (auto& thread : threads_) thread.join();
        throw;
    }
    startupStatus_ = startup_.finish();
}

Workers::~Workers() {
    quit_.store(true);
    for (unsigned i = 0; i < helpers(); ++i) {
        slots_[i].sequence.fetch_add(1);
        slots_[i].sequence.notify_one();
    }
    for (auto& t : threads_) t.join();
}

Status Workers::setPolicy(ThreadPolicy policy) {
    for (auto& thread : threads_) SUPPLE_TRY(setThreadPolicy(thread.native_handle(), policy));
    return okStatus();
}

ThreadPolicy Workers::policy() {
    return threads_.empty()                   ? ThreadPolicy{}
           : nativeBackend_ && startupStatus_ ? ThreadPolicy{ThreadPolicy::Kind::High, 0}
                                              : threadPolicy(threads_.front().native_handle());
}

unsigned Workers::defaultHelpers() {
    // The CPUs the calling thread may run on (taskset, cpusets), not every
    // online core: a larger pool would only contend and fail the real-time
    // CPU cap (ADR-0026).
    const unsigned cpus = availableCpus();
    return cpus > 3 ? cpus - 3 : 0;  // cpus - 2 workers, the caller being one
}

void Workers::help(TaskGraph& g, Fn fn, void* ctx) {
    for (;;) {
        // Observe the event BEFORE checking either predicate. Publication
        // between a failed pop and wait then changes the value, preventing a
        // lost wake, including when an MPMC producer has reserved but not yet
        // published its cell. That producer signals after publishing.
        const auto seen = workSequence_.load(std::memory_order_acquire);
        if (g.remaining.load(std::memory_order_acquire) == 0) return;
        std::uint32_t t = 0;
        if (!g.ready.pop(t)) {
            event(Event::Wait);
            workSequence_.wait(seen, std::memory_order_acquire);
            continue;
        }
        fn(ctx, t);
        bool published = false;
        for (std::uint32_t s : g.succ[t])
            if (g.pending[s].fetch_sub(1, std::memory_order_acq_rel) == 1) {
                g.ready.push(s);  // capacity >= graph size; each task is queued once
                published = true;
            }
        const bool finished = g.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
        if (published || finished) {
            workSequence_.fetch_add(1, std::memory_order_release);
            workSequence_.notify_all();
        }
    }
}

void Workers::run(TaskGraph& g, Fn fn, void* ctx) {
    const std::uint32_t n = g.size();
    if (n == 0) return;
    const unsigned selected = std::min(helpers(), g.parallelism - 1);
    if (selected == 0) {
        // Indices are topological. A serial graph needs neither wakeups nor
        // dependency/queue atomics, even if the pool has many sleeping helpers.
        for (std::uint32_t i = 0; i < n; ++i) fn(ctx, i);
        return;
    }
    for (std::uint32_t i = 0; i < n; ++i) g.pending[i].store(g.preds[i], std::memory_order_relaxed);
    g.remaining.store(n, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i)
        if (g.preds[i] == 0) g.ready.push(i);
    job_ = &g;
    fn_ = fn;
    ctx_ = ctx;
    active_.store(selected, std::memory_order_relaxed);
    for (unsigned i = 0; i < selected; ++i) {
        slots_[i].sequence.fetch_add(1, std::memory_order_release);
        slots_[i].sequence.notify_one();
    }
    help(g, fn, ctx);
    // Includes helpers preempted before joining or after finishing DSP.
    // No graph or job pointer can be reclaimed while one is still enrolled.
    for (unsigned active = active_.load(std::memory_order_acquire); active != 0;
         active = active_.load(std::memory_order_acquire)) {
        event(Event::JoinWait);
        active_.wait(active, std::memory_order_acquire);
    }
}

void Workers::loop(unsigned index) {
    auto& sequence = slots_[index].sequence;
    std::uint32_t seen = 0;
    for (;;) {
        sequence.wait(seen, std::memory_order_acquire);
        seen = sequence.load(std::memory_order_acquire);
        if (quit_.load()) return;
        event(Event::Join);
        help(*job_, fn_, ctx_);
        event(Event::Leave);
        // Nothing after this release touches the graph or published job.
        if (active_.fetch_sub(1, std::memory_order_acq_rel) == 1) active_.notify_one();
    }
}

}  // namespace supple::engine::detail
