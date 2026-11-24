// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <algorithm>
#include <chrono>
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

namespace supple::test {

// A pause for a test thread that paces a fake device or sender. On Windows
// std::this_thread::sleep_for rounds up to the system timer tick (15.6 ms by
// default), so a loop that sleeps a block period (2.7 ms) or 100 microseconds
// runs six to fifteen times slower than on Linux while it uses no CPU
// (engine-plan-soak.tests: 932 s on Windows CI against 119 s on Linux). Here
// the wait ends on a thread-owned high-resolution waitable timer, the one the
// engine's own Windows waits use (ADR-0102): process-owned, no global timer
// change. Where that timer does not exist (before Windows 10 1803), or fails,
// and on every other platform it is std::this_thread::sleep_for.
//
// For pacing only: no test may decide pass or fail by how long this took.
inline void sleepFor(std::chrono::duration<double> duration) {
#if defined(_WIN32)
    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer() {
            if (handle) CloseHandle(handle);
        }
    };
    static thread_local Timer timer;
    if (timer.handle && duration.count() > 0) {
        LARGE_INTEGER due;  // relative, 100 ns units
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(duration.count() * 1e7));
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0)
            return;
    }
#endif
    std::this_thread::sleep_for(duration);
}

}  // namespace supple::test
