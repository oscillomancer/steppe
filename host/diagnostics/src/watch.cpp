// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "watch.h"
#include <atomic>
#include <thread>
#include "supple/diagnostics/frontend.h"
#include "terminal.h"
#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#endif
namespace supple::diagnostics::detail {
namespace {
std::atomic<unsigned> decision{0};
static_assert(std::atomic<unsigned>::is_always_lock_free);
std::atomic<std::int64_t> boundMilliseconds{
    std::chrono::duration_cast<std::chrono::milliseconds>(kStopBound).count()};
std::atomic<SupervisorHook> hook{nullptr};
// The watcher thread. Off the audio path, parked in the channel wait until the supervisor is
// gone. Its decision is stored before anything is done about it, so a test can read it.
void watch(Native native) noexcept {
    (void)registerCurrentThread();  // a fault on this thread is still reported
    const SupervisorEnd end = awaitSupervisorEnd(native);
    // A terminal handler that has begun owns the process's last moments: a crashing worker
    // does not also start a stop.
    const SupervisorEnd result = terminalBegun() ? SupervisorEnd::Unknown : end;
    decision.store(static_cast<unsigned>(result), std::memory_order_release);
    if (result != SupervisorEnd::Launcher) return;
    // The launcher's stop: what a forwarded SIGTERM does. A program that has not installed
    // its handler yet gets the default action.
    requestStop();
    // The bound, on the monotonic clock, from the request. The program ends itself within
    // it, which takes this thread with it, or it is ended hard: not abort, which would run
    // the crash handlers and publish a report to nobody.
    std::this_thread::sleep_until(std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(boundMilliseconds.load()));
    hardExit();
}
}  // namespace
void startSupervisorWatch(const Native& native) noexcept {
    if (!native.trusted) return;
#if !defined(_WIN32)
    // The thread starts with every signal blocked except the synchronous faults, so a
    // process-directed SIGTERM (its own stop request) is never taken on this thread.
    sigset_t blocked, previous;
    (void)sigfillset(&blocked);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) (void)sigdelset(&blocked, signal);
    (void)::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
#endif
    try {
        std::thread(watch, native).detach();
    } catch (...) {
        // No thread: the worker runs unwatched, as it did before the watch existed.
    }
#if !defined(_WIN32)
    (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
}
SupervisorEnd supervisorEnd() noexcept {
    return static_cast<SupervisorEnd>(decision.load(std::memory_order_acquire));
}
const char* supervisorEndName(SupervisorEnd end) noexcept {
    switch (end) {
        case SupervisorEnd::Pending:
            return "pending";
        case SupervisorEnd::Collector:
            return "collector";
        case SupervisorEnd::Launcher:
            return "launcher";
        case SupervisorEnd::Unknown:
            return "unknown";
    }
    return "pending";
}
void setStopBound(std::chrono::milliseconds bound) noexcept {
    boundMilliseconds.store(bound.count(), std::memory_order_relaxed);
}
void setSupervisorHook(SupervisorHook value) noexcept {
    hook.store(value, std::memory_order_release);
}
SupervisorHook supervisorHook() noexcept { return hook.load(std::memory_order_acquire); }
}  // namespace supple::diagnostics::detail
