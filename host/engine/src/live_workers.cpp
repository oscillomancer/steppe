// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "live_workers.h"

#include <algorithm>
#include <string>

#include "supple/base/thread.h"
#include "supple/diagctx/table.h"

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
                (void)diagctx::registerThread(diagctx::ThreadRole::AudioLive,
                                              static_cast<std::uint16_t>(i + 1));
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

void LiveWorkers::help(TaskGraph& g, Fn fn, void* context) SUPPLE_RT {
    while (g.remaining.load(std::memory_order_acquire) != 0) {
        std::uint32_t task = 0;
        if (!g.ready.pop(task)) {
            rt::relax();
            continue;
        }
        fn(context, task);
        for (std::uint32_t next : g.succ[task])
            if (g.pending[next].fetch_sub(1, std::memory_order_acq_rel) == 1) g.ready.push(next);
        g.remaining.fetch_sub(1, std::memory_order_acq_rel);
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
    // The preceding close/join excludes readers of the old job. A late claim
    // either sees closed and reads no fields, or acquires this publication.
    open_.store(true, std::memory_order_seq_cst);
    for (unsigned i = 0; i < selected; ++i) {
        slots_[i].sequence.fetch_add(1, std::memory_order_release);
        rt::wake(slots_[i].sequence);
    }
    event(Event::Published);
    help(g, fn, context);
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
            help(*job_, fn_, context_);
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
