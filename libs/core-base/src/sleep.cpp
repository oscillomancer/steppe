// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/sleep.h"

#include <algorithm>
#include <atomic>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace supple {
namespace {

std::atomic<bool> gHighResolutionEnabled{true};
thread_local bool tLastHighResolution = false;

#if defined(_WIN32)
// One per thread that sleeps; the destructor closes the handle when the thread exits.
struct ThreadTimer {
    HANDLE handle = nullptr;
    bool tried = false;
    ~ThreadTimer() {
        if (handle) CloseHandle(handle);
    }
    HANDLE get() {
        if (!tried) {
            tried = true;  // a refusal is not asked again by this thread
            handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
        }
        return handle;
    }
};
thread_local ThreadTimer tTimer;
#endif

// One wait of about `seconds`, which may end a little before or after it; true if it ended on
// the high-resolution timer. The callers loop until steady_clock has reached their deadline.
bool waitOnce(double seconds) {
#if defined(_WIN32)
    // Under a million seconds, so the 100 ns count below fits; longer is not a poll.
    if (seconds < 1e6 && gHighResolutionEnabled.load(std::memory_order_relaxed)) {
        if (HANDLE timer = tTimer.get()) {
            LARGE_INTEGER due;  // relative, in 100 ns units
            due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(seconds * 1e7));
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
                return true;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return false;
}

}  // namespace

void sleepUntil(std::chrono::steady_clock::time_point deadline) {
    tLastHighResolution = false;
    // Not before the deadline on either path: a wait can end early by steady_clock's measure
    // (a timer's due time is not steady_clock's), so look again and wait the rest.
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        tLastHighResolution = waitOnce(
            std::chrono::duration_cast<std::chrono::duration<double>>(deadline - now).count());
    }
}

void sleepFor(std::chrono::duration<double> duration) {
    if (!(duration.count() > 0)) {  // zero, negative or NaN: nothing to wait for
        tLastHighResolution = false;
        return;
    }
    // Clamped to what a time_point holds (about 3 years is far more than a poll needs).
    const auto wait = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(std::min(duration.count(), 1e8)));
    sleepUntil(std::chrono::steady_clock::now() + wait);
}

bool lastSleepUsedHighResolutionTimer() { return tLastHighResolution; }

void setHighResolutionSleepEnabledForTests(bool enabled) {
    gHighResolutionEnabled.store(enabled, std::memory_order_relaxed);
}

}  // namespace supple
