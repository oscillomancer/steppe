// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>
#include <cstring>
#include "supple/diagcap/capsule.h"
// The alternate signal stack a registered thread gets, so that a fault handler can run when the
// fault is a stack overflow. Shared by the worker's terminal handler and the bridge's
// self-capture (topic 6c), which both register their threads through the execution table's
// thread hook.
namespace supple::diagnostics::detail {
namespace {
struct SignalStack {
    void* memory = nullptr;
    std::size_t bytes = 0;
    stack_t previous{};
    bool ready = false;
    SignalStack() {
        if (::sigaltstack(nullptr, &previous) != 0) return;
        if (!(previous.ss_flags & SS_DISABLE)) {
            ready = true;
            return;
        }
        const long page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0) return;
        constexpr std::size_t stackBytes = 64 * 1024;
        bytes = stackBytes + 2 * static_cast<std::size_t>(page);
        memory = ::mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            memory = nullptr;
            return;
        }
        auto* stack = static_cast<char*>(memory) + page;
        if (::mprotect(stack, stackBytes, PROT_READ | PROT_WRITE) != 0) return;
        std::memset(stack, 0, stackBytes);  // commit pages while cold
        stack_t replacement{};
        replacement.ss_sp = stack;
        replacement.ss_size = stackBytes;
        ready = ::sigaltstack(&replacement, nullptr) == 0;
    }
    ~SignalStack() {
        if (!memory) return;
        // If the old alternate stack could not be restored, ours is still the enabled one:
        // unmapping it would make the kernel push the next signal frame onto unmapped
        // memory, which it answers with a forced SIGILL and no handler. A leaked 64 KiB at
        // thread exit is the safe choice.
        if (ready && ::sigaltstack(&previous, nullptr) != 0) return;
        (void)::munmap(memory, bytes);
    }
};
}  // namespace

bool registerThreadAlternateStack() noexcept {
    thread_local SignalStack stack;
    return stack.ready;
}
}  // namespace supple::diagnostics::detail
