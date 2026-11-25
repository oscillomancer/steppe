// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "supple/base/sleep.h"

using namespace supple;
using namespace std::chrono_literals;

namespace {
struct RestoreSwitch {
    ~RestoreSwitch() { setHighResolutionSleepEnabledForTests(true); }
};
}  // namespace

TEST_CASE("sleepFor and sleepUntil never return before the deadline, on either path") {
    RestoreSwitch restore;
    for (bool enabled : {true, false}) {
        INFO("high resolution enabled " << enabled);
        setHighResolutionSleepEnabledForTests(enabled);
        const auto start = std::chrono::steady_clock::now();
        sleepFor(5ms);
        // The guarantee is not before; how much later it ends is the scheduler's, and nothing
        // here looks at an upper bound.
        CHECK(std::chrono::steady_clock::now() >= start + 5ms);
        if (!enabled) CHECK_FALSE(lastSleepUsedHighResolutionTimer());
    }
}

TEST_CASE("sleepUntil returns at once for a time that has passed") {
    sleepUntil(std::chrono::steady_clock::now() - 1s);
    CHECK_FALSE(lastSleepUsedHighResolutionTimer());
    RestoreSwitch restore;
    for (bool enabled : {true, false}) {
        INFO("high resolution enabled " << enabled);
        setHighResolutionSleepEnabledForTests(enabled);
        const auto start = std::chrono::steady_clock::now();
        sleepUntil(start + 3ms);
        CHECK(std::chrono::steady_clock::now() >= start + 3ms);
    }
}

TEST_CASE("a zero, negative or NaN sleep does not wait") {
    sleepFor(0us);
    sleepFor(-1s);
    sleepFor(std::chrono::duration<double>(std::nan("")));
    CHECK_FALSE(lastSleepUsedHighResolutionTimer());
}

#if defined(_WIN32)
TEST_CASE(
    "on Windows a sleep ends on the thread's high-resolution timer, or the ordinary one when it is "
    "refused") {
    RestoreSwitch restore;
    // Another thread, so its timer is created and closed inside the test.
    std::atomic<int> used{-1};
    std::thread([&] {
        sleepFor(1ms);
        used = lastSleepUsedHighResolutionTimer() ? 1 : 0;
    }).join();
    // Windows 10 1803 and newer have the timer; an older image would say 0 and keep the
    // ordinary path, which the next block covers. Not asserted either way.
    MESSAGE("high-resolution timer used: " << used.load());
    setHighResolutionSleepEnabledForTests(false);
    std::thread([&] {
        sleepFor(1ms);
        used = lastSleepUsedHighResolutionTimer() ? 1 : 0;
    }).join();
    CHECK(used.load() == 0);  // the refusal path: ordinary sleep_for
}
#else
TEST_CASE("off Windows a sleep never reports the high-resolution timer") {
    sleepFor(1ms);
    CHECK_FALSE(lastSleepUsedHighResolutionTimer());
}
#endif
