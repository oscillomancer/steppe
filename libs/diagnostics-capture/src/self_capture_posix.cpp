// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <cstdlib>
#include <exception>
#include "self_capture_internal.h"
#include "supple/diagctx/table.h"
#if defined(__APPLE__)
// <ucontext.h> is refused without _XOPEN_SOURCE; the struct definitions come from here.
#include <sys/ucontext.h>
#if defined(__aarch64__)
#include <mach/arm/thread_status.h>
#endif
#else
#include <ucontext.h>
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(realtime_sanitizer)
#define SUPPLE_SELF_CAPTURE_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SUPPLE_SELF_CAPTURE_SANITIZER 1
#endif
// The bridge's fault handlers on POSIX (topic 6c). The same shape as the worker's terminal
// handler (host/diagnostics/src/terminal.cpp), with nobody to hand the failure to: the handler
// writes the capsule into the slot and then lets the process die as it would have. What it
// may use is the async-signal-safe set: system calls, atomics, static storage. No allocation,
// no lock, no stdio, no C++ runtime beyond the terminate hook's own call.
namespace supple::diagnostics::selfcapture {
namespace {
[[maybe_unused]] std::terminate_handler priorTerminate = nullptr;

// The signal's default action, delivered again: the process ends with the signal as its
// status, which is how the host classifies the end (signal, not an exit code).
[[noreturn]] void reraise(int signal) {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    (void)sigemptyset(&action.sa_mask);
    (void)::sigaction(signal, &action, nullptr);
    sigset_t one;
    (void)sigemptyset(&one);
    (void)sigaddset(&one, signal);
    (void)::sigprocmask(SIG_UNBLOCK, &one, nullptr);
    (void)::raise(signal);
    ::_exit(128 + signal);  // only if the native fatal re-raise could not complete
}

void registersOf(const void* context, Fault& fault) noexcept {
    const auto* state = static_cast<const ucontext_t*>(context);
    if (!state) return;
#if defined(__APPLE__) && defined(__aarch64__)
    if (state->uc_mcontext) {
        fault.pc = arm_thread_state64_get_pc(state->uc_mcontext->__ss);
        fault.fp = arm_thread_state64_get_fp(state->uc_mcontext->__ss);
        fault.sp = arm_thread_state64_get_sp(state->uc_mcontext->__ss);
    }
#elif defined(__APPLE__) && defined(__x86_64__)
    if (state->uc_mcontext) {
        fault.pc = state->uc_mcontext->__ss.__rip;
        fault.fp = state->uc_mcontext->__ss.__rbp;
        fault.sp = state->uc_mcontext->__ss.__rsp;
    }
#elif defined(__x86_64__)
    fault.pc = static_cast<std::uint64_t>(state->uc_mcontext.gregs[REG_RIP]);
    fault.fp = static_cast<std::uint64_t>(state->uc_mcontext.gregs[REG_RBP]);
    fault.sp = static_cast<std::uint64_t>(state->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
    fault.pc = state->uc_mcontext.pc;
    fault.fp = state->uc_mcontext.regs[29];
    fault.sp = state->uc_mcontext.sp;
#else
    (void)fault;
#endif
}

[[maybe_unused]] void fatalSignal(int signal, siginfo_t*, void* context) {
    const std::uint64_t start = monotonic();
    Fault fault;
    fault.tid = diagctx::currentThreadId();
    if (!beginCapture(fault.tid)) {
        // Another thread is writing the capsule: the process ends once it has, or at the bound.
        awaitCapture(fault.tid, start);
        reraise(signal);
    }
    registersOf(context, fault);
    (void)captureAndPublish(fault, start);
    endCapture();
    reraise(signal);
}

// std::terminate: no registers, so the walk starts from this frame, as the worker's does.
[[maybe_unused, noreturn]] void terminateHook() noexcept {
    const std::uint64_t start = monotonic();
    Fault fault;
    fault.tid = diagctx::currentThreadId();
    if (!beginCapture(fault.tid)) {
        awaitCapture(fault.tid, start);
        std::abort();
    }
    const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    fault.pc = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    fault.sp = frame;
    fault.fp = frame != 0 ? *reinterpret_cast<const std::uintptr_t*>(frame) : 0;
#if defined(__aarch64__)
    fault.pc &= 0x0000ffffffffffffULL;
#endif
    (void)captureAndPublish(fault, start);
    endCapture();
    // The program's own terminate handler runs as it would have; if it returns, abort.
    if (priorTerminate) priorTerminate();
    std::abort();
}
}  // namespace

bool installHandlers() noexcept {
#if defined(SUPPLE_SELF_CAPTURE_SANITIZER)
    // Sanitizer handlers and RT checking keep ownership, as for the worker.
    return false;
#else
    // Registered threads get their alternate stack through the table's hook, and so does this
    // one (the bridge's main thread registers after the install).
    diagctx::setThreadHook([]() noexcept { (void)detail::registerThreadAlternateStack(); });
    (void)detail::registerThreadAlternateStack();
    bool installed = false;
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        struct sigaction prior{}, action{};
        // Only where nobody else has a handler: the previous action is then the default one,
        // which the handler restores and raises, so the exit status stays the fatal one.
        if (::sigaction(signal, nullptr, &prior) != 0 || prior.sa_handler != SIG_DFL) continue;
        action.sa_sigaction = fatalSignal;
        // No SA_RESETHAND: the handler puts the default action back itself when it re-raises, and
        // until then a second thread's fault must reach it (and wait for the capture) instead of
        // finding the default action and ending the process under the first thread's write. A
        // fault inside the handler on the same thread meets the signal blocked, which the kernel
        // answers by ending the process: no loop.
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        (void)sigemptyset(&action.sa_mask);
        installed |= ::sigaction(signal, &action, nullptr) == 0;
    }
    if (installed) priorTerminate = std::set_terminate(terminateHook);
    return installed;
#endif
}
}  // namespace supple::diagnostics::selfcapture
