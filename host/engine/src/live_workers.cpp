// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "live_workers.h"

#include "supple/host/clap_host.h"

#include <algorithm>
#include <bit>
#include <string>

#include "supple/base/thread.h"

#include "supple/engine/realtime.h"

namespace supple::engine::detail {

LiveWorkers::LiveWorkers(unsigned helpers) : LiveWorkers(helpers, Hooks{}) {}
LiveWorkers::LiveWorkers(unsigned helpers, Hooks hooks, Backend* backend)
    : slots_(std::make_unique<Slot[]>(helpers)),
      startup_(helpers, backend, true),
      nativeBackend_(backend),
      hooks_(hooks) {
    threads_.reserve(helpers);
    try {
        for (unsigned i = 0; i < helpers; ++i)
            threads_.emplace_back([this, i] {
                nameThread("audio-live-" + std::to_string(i + 1));
                rt::prefaultStack();  // ADR-0025
                const bool elevated = startup_.enter(i);
                ThreadStartup::Cleanup cleanup{elevated ? nativeBackend_ : nullptr};
                loop(i);
            });
    } catch (...) {
        startup_.cancel();
        shutDown();  // even a partially constructed pool must reap its threads
        throw;
    }
    startupStatus_ = startup_.finish();
}

LiveWorkers::~LiveWorkers() { shutDown(); }

void LiveWorkers::shutDown() {
    // The backend must be stopped before destroying the pool.
    quit_.store(true, std::memory_order_release);
    for (unsigned i = 0; i < helpers(); ++i) {
        slots_[i].sequence.fetch_add(1, std::memory_order_release);
        rt::wake(slots_[i].sequence);
    }
    for (auto& thread : threads_) thread.join();
}

unsigned LiveWorkers::maxHelpers() {
    const unsigned cpus = allowedCpuCount();
    return cpus > 1 ? cpus - 1 : 0;
}

Status LiveWorkers::configure(Backend& backend) {
    SUPPLE_TRY(startupStatus_);
    if (nativeBackend_) return okStatus();
    for (auto& thread : threads_) SUPPLE_TRY(backend.prepareLiveThread(thread.native_handle()));
    return okStatus();
}

ThreadPolicy LiveWorkers::policy() {
    return threads_.empty()                   ? ThreadPolicy{}
           : nativeBackend_ && startupStatus_ ? ThreadPolicy{ThreadPolicy::Kind::High, 0}
                                              : threadPolicy(threads_.front().native_handle());
}

void LiveWorkers::help(TaskGraph& g, Fn fn, void* context, int self) SUPPLE_RT {
    const bool parks = self >= 0 && self < 64 && parkAfterNs_ > 0;
    std::int64_t idleSince = 0;
    while (g.remaining.load(std::memory_order_acquire) != 0) {
        std::uint32_t task = 0;
        if (!g.ready.pop(task)) {
            if (parks) {
                const std::int64_t now = host::steadyNanoseconds();
                if (idleSince == 0) {
                    idleSince = now;
                } else if (now - idleSince >= parkAfterNs_) {
                    park(g, self);
                    idleSince = 0;
                    continue;
                }
            }
            rt::relax();
            continue;
        }
        idleSince = 0;
        fn(context, task);
        int pushed = 0;
        for (std::uint32_t next : g.succ[task])
            if (g.pending[next].fetch_sub(1, std::memory_order_acq_rel) == 1) {
                g.ready.push(next);
                ++pushed;
            }
        const bool last = g.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1;
        if (parkAfterNs_ > 0 && (pushed > 0 || last)) unpark(pushed, last);
    }
}

// A parked helper's bit is set, then (after a full fence) it looks again for
// a task or the job's end before it waits; a pusher pushes, then (after a
// full fence) reads the bits. Either the pusher sees the bit and bumps the
// helper's word, or the helper's second look sees the push: no wake is lost.
void LiveWorkers::park(TaskGraph& g, int self) SUPPLE_RT {
    const std::uint64_t bit = std::uint64_t{1} << self;
    std::atomic<std::uint32_t>& word = slots_[static_cast<std::size_t>(self)].park;
    parked_.fetch_or(bit, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::uint32_t seen = word.load(std::memory_order_acquire);
    if (g.remaining.load(std::memory_order_acquire) != 0 && g.ready.empty()) {
        // Experimental (item D): a helper's wait within a job. Helpers already
        // block between jobs (loop); this needs an ADR-0024 amendment if kept.
        event(Event::Park);
        rt::ScopedSanctionedSyscall wait;
        rt::wait(word, seen);
    }
    parked_.fetch_and(~bit, std::memory_order_seq_cst);
}

void LiveWorkers::unpark(int count, bool all) SUPPLE_RT {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::uint64_t waiting = parked_.load(std::memory_order_seq_cst);
    while (waiting != 0 && (all || count-- > 0)) {
        const int i = std::countr_zero(waiting);
        waiting &= waiting - 1;
        std::atomic<std::uint32_t>& word = slots_[static_cast<std::size_t>(i)].park;
        word.fetch_add(1, std::memory_order_release);
        rt::wake(word);
    }
}

void LiveWorkers::run(TaskGraph& g, Fn fn, void* context) SUPPLE_RT {
    const auto n = g.size();
    if (n == 0) return;
    const unsigned selected = std::min(helpers(), g.parallelism - 1);
    if (selected == 0) {
        for (std::uint32_t i = 0; i < n; ++i) fn(context, i);
        return;
    }
    for (std::uint32_t i = 0; i < n; ++i) g.pending[i].store(g.preds[i], std::memory_order_relaxed);
    g.remaining.store(n, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i)
        if (g.preds[i] == 0) g.ready.push(i);
    job_ = &g;
    fn_ = fn;
    context_ = context;
    deadline_ = host::currentLiveBridgeDeadline();
    // The preceding close/join excludes readers of the old job. A late claim
    // either sees closed and reads no fields, or acquires this publication.
    open_.store(true, std::memory_order_seq_cst);
    for (unsigned i = 0; i < selected; ++i) {
        slots_[i].sequence.fetch_add(1, std::memory_order_release);
        rt::wake(slots_[i].sequence);
    }
    event(Event::Published);
    help(g, fn, context, -1);
    // This store/load and the helper's increment/open check form a Dekker
    // handshake: either we see its claim, or it sees closed. Acquire/release
    // alone would let both sides miss each other and retire a job still read.
    open_.store(false, std::memory_order_seq_cst);
    while (active_.load(std::memory_order_seq_cst) != 0) {
        event(Event::JoinWait);
        rt::relax();
    }
}

void LiveWorkers::loop(unsigned index) {
    auto& sequence = slots_[index].sequence;
    std::uint32_t seen = 0;
    for (;;) {
        event(Event::Idle);
        rt::wait(sequence, seen);
        seen = sequence.load(std::memory_order_acquire);
        if (quit_.load(std::memory_order_acquire)) return;
        event(Event::Join);
        active_.fetch_add(1, std::memory_order_seq_cst);
        event(Event::Claimed);
        if (open_.load(std::memory_order_seq_cst)) {
            event(Event::Entered);
            const host::LiveBridgeDeadline deadline(deadline_);
            help(*job_, fn_, context_, static_cast<int>(index));
            event(Event::Leave);
        } else {
            // The graph and context may already be gone. An old notification
            // is only a chance to claim an open job, not ownership of a job.
            event(Event::Missed);
        }
        // No job/graph/context access after acknowledgement; the callback can
        // already be publishing a new job or reclaiming the old plan.
        active_.fetch_sub(1, std::memory_order_release);
    }
}

}  // namespace supple::engine::detail
