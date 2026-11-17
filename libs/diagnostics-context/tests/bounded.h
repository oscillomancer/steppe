// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// A wait that always ends. The thread tests of this library hand work back and forth through
// flags; a flag that never comes (a thread that died, a hook that did not run on some
// platform) must fail the case and name the wait, not hold the whole test run until the
// runner's timeout with nothing in its log.
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

namespace supple::diagctx::testing {

// Yields until `done()` or `limit` has passed. False, with a failed check naming `what`, on
// the limit. Safe on any thread and inside noexcept hooks (a doctest check does not throw).
template <class Done>
bool waitFor(Done&& done, const char* what, std::chrono::seconds limit = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            FAIL_CHECK("gave up after " << limit.count() << " s waiting for " << what);
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace supple::diagctx::testing
