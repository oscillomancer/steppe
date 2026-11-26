// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Real-time annotations.
//
// SUPPLE_RT marks a function as real-time: RealtimeSanitizer (the `rtsan`
// preset, -fsanitize=realtime) aborts if anything it calls allocates, locks,
// blocks or performs a system call. Mark every audio-thread entry point.
// Reviewed syscall exceptions: background bridge IPC and private futex wake.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define SUPPLE_RT [[clang::nonblocking]]
#endif
#endif
#ifndef SUPPLE_RT
#define SUPPLE_RT
#endif

// SUPPLE_BLOCKING marks a function that waits (sleeps, blocks on a lock or event) so that
// RealtimeSanitizer reports a call to it from a SUPPLE_RT function directly, whichever
// system call it makes on the platform. Compilers without the attribute ignore it.
#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::blocking)
#define SUPPLE_BLOCKING [[clang::blocking]]
#endif
#endif
#ifndef SUPPLE_BLOCKING
#define SUPPLE_BLOCKING
#endif

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#include <sanitizer/rtsan_interface.h>
#define SUPPLE_RTSAN_ENABLED 1
#endif
#endif

namespace supple::rt {

// RAII: disables RealtimeSanitizer checks in scope. Use only in tests (e.g. to
// build fixtures inside an RT-annotated harness) -- never in product code.
struct ScopedRtsanDisabler {
#ifdef SUPPLE_RTSAN_ENABLED
    ScopedRtsanDisabler() { __rtsan_disable(); }
    ~ScopedRtsanDisabler() { __rtsan_enable(); }
#endif
};

// RAII: the sanctioned system calls of an audio thread -- the futex wake and
// bounded wait of a background bridged plugin (ADR-0013), and the private
// futex wake inside rt::wake (ADRs 0024, 0028). No callback waits are sanctioned.
struct ScopedSanctionedSyscall {
    ScopedSanctionedSyscall() {
#ifdef SUPPLE_RTSAN_ENABLED
        __rtsan_disable();
#endif
    }
    ~ScopedSanctionedSyscall() {
#ifdef SUPPLE_RTSAN_ENABLED
        __rtsan_enable();
#endif
    }
    ScopedSanctionedSyscall(const ScopedSanctionedSyscall&) = delete;
    ScopedSanctionedSyscall& operator=(const ScopedSanctionedSyscall&) = delete;
};

// Process-local address notification. Publish with release before wake;
// wait acquires that publication. One waiter per word, lifetime through join.
void wake(std::atomic<std::uint32_t>& word) SUPPLE_RT;
// Idle helpers only: blocks until the word differs, including spurious wakes.
void wait(std::atomic<std::uint32_t>& word, std::uint32_t seen);
// Non-RT only: rechecks after spurious wakes/EINTR, returns false at deadline.
bool waitUntil(std::atomic<std::uint32_t>& word, std::uint32_t seen,
               std::chrono::steady_clock::time_point deadline);
// Owned audio threads, once before their first job (ADR-0025): commits the
// 128 KiB of stack below the caller, so DSP does not first-touch it while
// timed. Does nothing when less than 512 KiB of stack remains. Not real-time.
void prefaultStack();
inline void relax() SUPPLE_RT {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

}  // namespace supple::rt
