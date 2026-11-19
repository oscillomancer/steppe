// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "terminal.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include "inventory.h"
#include "supple/diagctx/table.h"
#include "supple/diagnostics/frontend.h"
#if !defined(_WIN32)
#include "supple/diagcap/capsule.h"
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>

#include <csignal>
#else
#include <signal.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
// <ucontext.h> is refused without _XOPEN_SOURCE; the struct definitions come from here.
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/arm/thread_status.h>
#endif
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(realtime_sanitizer)
#define SUPPLE_DIAGNOSTICS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SUPPLE_DIAGNOSTICS_SANITIZER 1
#endif
namespace supple::diagnostics {
namespace {
using namespace detail;
static_assert(std::atomic<unsigned>::is_always_lock_free);
Native endpoint;
Packet initial;
std::atomic<unsigned> claimed{0};
[[maybe_unused]] std::terminate_handler priorTerminate = nullptr;
std::atomic<bool> active{false};
// Set once the worker's main thread has put the terminate hook in and kept what it replaced:
// threads registered after that get the hook too (Windows keeps it per thread).
[[maybe_unused]] std::atomic<bool> terminateHooked{false};
static_assert(std::atomic<bool>::is_always_lock_free);
// One terminal producer wins. No pointer/model lookup, TLS, allocation or
// formatting. Raw PC/frame/SP and the thread id stay solely in the private native channel.
void handoff(Packet packet) noexcept {
    packet.kind = Message::Terminal;
    // No inventory event goes out from here on: on macOS the channel carries the capsule
    // right after this packet, and another thread's packet inside it would corrupt it. The
    // count of events that were not sent travels with this packet.
    // A stream an announcement left torn cannot carry this packet (the supervisor would read it
    // as the end of the torn one and stop): nothing is sent, and the process goes on dying as it
    // would with no supervisor listening. A send still in progress is waited out first.
    if (!detail::closeInventory()) return;
    packet.dropped = detail::inventoryDropped();
#if defined(__APPLE__)
    // The hold's clock starts before the packet goes out. The supervisor's own budget
    // starts when it receives that packet, so it can never start before this one, however
    // long this thread is descheduled after the write (review round 31, F3).
    const std::uint64_t start = detail::monotonicNanoseconds();
#endif
    if (!detail::publish(endpoint, packet)) return;
#if defined(__APPLE__)
    // No external collector on macOS: the process reads its own threads, sends them,
    // and waits out the rest of the one-second hold (capture_macos.cpp).
    detail::captureAndHold(endpoint, packet, start);
#else
    detail::awaitAcknowledgement(endpoint);
#endif
}
// The calling thread's id, for the private channel only. Async-signal-safe.
[[maybe_unused]] std::uint64_t currentThread() noexcept {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}
#if defined(_WIN32)
// The top-level filter that was in place when ours went in. An EXE linked with the MSVC
// runtime always has one (the runtime's own, which turns an uncaught C++ exception into
// std::terminate), so this is never null in the product. Written once at install, before
// other threads exist; read only after the handoff.
LPTOP_LEVEL_EXCEPTION_FILTER priorFilter = nullptr;
// The code the MSVC runtime gives an escaping C++ exception.
constexpr DWORD kCppExceptionCode = 0xE06D7363;
[[maybe_unused]] LONG WINAPI lastChance(EXCEPTION_POINTERS* exception) {
    // An exception nobody caught is not a fault: the runtime's filter, which is the prior
    // one here, turns it into std::terminate, and our terminate hook records it (cause 3)
    // and chains the program's terminate handler, as on Linux and macOS. Claiming it here
    // would record a different cause and skip that handler. The runtime's filter never
    // returns for a real C++ exception. If it does return (the record carries the code but
    // is not a C++ throw, as when a foreign runtime or a program raises the number itself,
    // or the prior filter is not the runtime's, or there is none), nobody is handling it:
    // it falls through to the fault path below as any other exception does (review round
    // 35, F2), and the filter's answer is not asked for a second time.
    bool chainedAlready = false;
    LONG early = EXCEPTION_CONTINUE_SEARCH;
    if (exception && exception->ExceptionRecord &&
        exception->ExceptionRecord->ExceptionCode == kCppExceptionCode && priorFilter) {
        early = priorFilter(exception);
        chainedAlready = true;
    }
    if (claimed.exchange(1, std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    Packet packet = initial;
    packet.cause = 2;
    packet.tid = currentThread();
    if (exception && exception->ExceptionRecord) {
        packet.code = exception->ExceptionRecord->ExceptionCode;
        packet.pc = reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    }
    if (exception && exception->ContextRecord) {
#if defined(_M_X64) || defined(__x86_64__)
        packet.frame = exception->ContextRecord->Rbp;
        packet.sp = exception->ContextRecord->Rsp;
#elif defined(_M_ARM64)
        packet.frame = exception->ContextRecord->Fp;
        packet.sp = exception->ContextRecord->Sp;
#endif
    }
    handoff(packet);
    // Chain to the filter we replaced (ADR-0083 clarification, 2026-11-16): the runtime's
    // and any other that would have run anyway, so independently configured crash
    // reporting is preserved. The fault stays fatal: the terminal packet has gone out and
    // the supervisor will report a crash, so a chained filter that asks to resume the
    // faulting code (EXCEPTION_CONTINUE_EXECUTION) is answered with CONTINUE_SEARCH, which
    // ends the process through the system's own reporting.
    const LONG chained = chainedAlready ? early
                         : priorFilter  ? priorFilter(exception)
                                        : EXCEPTION_CONTINUE_SEARCH;
    return chained == EXCEPTION_CONTINUE_EXECUTION ? EXCEPTION_CONTINUE_SEARCH : chained;
}
[[maybe_unused]] void abortSignal(int signal) {
    if (!claimed.exchange(1, std::memory_order_relaxed)) {
        Packet packet = initial;
        packet.cause = 4;
        packet.code = static_cast<unsigned>(signal);
        packet.tid = currentThread();
        handoff(packet);
    }
    // CRT abort remains fatal and retains the user's OS reporting policy.
}
#else
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
[[maybe_unused]] void fatalSignal(int signal, siginfo_t*, void* context) {
    if (claimed.exchange(1, std::memory_order_relaxed)) reraise(signal);
    Packet packet = initial;
    packet.cause = 1;
    packet.code = static_cast<unsigned>(signal);
    packet.tid = currentThread();
    const auto* state = static_cast<const ucontext_t*>(context);
    if (state) {
#if defined(__APPLE__) && defined(__aarch64__)
        if (state->uc_mcontext) {
            packet.pc = arm_thread_state64_get_pc(state->uc_mcontext->__ss);
            packet.frame = arm_thread_state64_get_fp(state->uc_mcontext->__ss);
            packet.sp = arm_thread_state64_get_sp(state->uc_mcontext->__ss);
        }
#elif defined(__APPLE__) && defined(__x86_64__)
        if (state->uc_mcontext) {
            packet.pc = state->uc_mcontext->__ss.__rip;
            packet.frame = state->uc_mcontext->__ss.__rbp;
            packet.sp = state->uc_mcontext->__ss.__rsp;
        }
#elif defined(__x86_64__)
        packet.pc = static_cast<std::uint64_t>(state->uc_mcontext.gregs[REG_RIP]);
        packet.frame = static_cast<std::uint64_t>(state->uc_mcontext.gregs[REG_RBP]);
        packet.sp = static_cast<std::uint64_t>(state->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
        packet.pc = state->uc_mcontext.pc;
        packet.frame = state->uc_mcontext.regs[29];
        packet.sp = state->uc_mcontext.sp;
#endif
    }
    handoff(packet);
    reraise(signal);
}
#endif
[[maybe_unused, noreturn]] void terminate() noexcept {
    if (claimed.exchange(1, std::memory_order_relaxed)) std::abort();
    Packet packet = initial;
    packet.cause = 3;
    packet.code = 0;  // The previous handler chooses the actual exit path/status.
    packet.tid = currentThread();
    handoff(packet);
    // Ordinary terminate context only: the prior handler would have run anyway.
    // This is the sole sanctioned foreign call, never made by signal/SEH hooks.
    if (priorTerminate) priorTerminate();
    std::abort();  // Null/returning handlers must still terminate the process.
}
}  // namespace
bool registerCurrentThread() noexcept {
    if (!active.load(std::memory_order_acquire)) return false;
#if defined(_WIN32)
    // The MSVC runtime keeps the terminate handler per thread, so each thread that is
    // registered gets ours too (review round 35, F1); without it an escaping exception
    // or a broken noexcept on that thread would find none and end in abort (cause 4).
    // The program's own handler the hook chains to is still the one the worker's main
    // thread had, as on Linux and macOS, where the handler is process-wide. A thread
    // that is never registered keeps whatever the runtime gives it.
    if (terminateHooked.load(std::memory_order_acquire)) (void)std::set_terminate(terminate);
    ULONG bytes = 64 * 1024;
    return SetThreadStackGuarantee(&bytes) != FALSE;
#else
    return detail::registerThreadAlternateStack();
#endif
}
namespace detail {
bool installTerminal(Native native, const Packet& boot) {
#if defined(SUPPLE_DIAGNOSTICS_SANITIZER)
    (void)native;
    (void)boot;
    // Sanitizer handlers and RT checking retain ownership. Supervisor exit
    // summaries still work; no product diagnostic suppression is introduced.
    return false;
#else
    if (!native.trusted) return false;
    endpoint = native;
    initial = boot;
    active.store(true, std::memory_order_release);
    // Owned threads register in the execution table next to their name; each new one
    // gets the alternate stack through this hook, so the engine never links this library.
    diagctx::setThreadHook([]() noexcept { (void)registerCurrentThread(); });
    (void)diagctx::registerThread(diagctx::ThreadRole::Model);  // installed by the main thread
    (void)registerCurrentThread();
#if defined(__APPLE__)
    rememberMainThread();  // installed by the main thread, before frontend code
#endif
    bool installed = false;
#if defined(_WIN32)
    // Always ours, whatever was there before: the MSVC runtime's startup has set its own
    // filter long before this runs, so declining to replace an existing one meant never
    // installing in a real executable. SetUnhandledExceptionFilter cannot fail, so the
    // filter is the process's top-level one from here on; it is not probed (reading it
    // means briefly clearing it, which a faulting thread could hit). abort has its own
    // hook below. lastChanceFilterInstalled() is evidence for the test fixture only.
    priorFilter = SetUnhandledExceptionFilter(lastChance);
    installed = true;
    if (installed) {
        const auto old = std::signal(SIGABRT, abortSignal);
        if (old != SIG_DFL) std::signal(SIGABRT, old);
    }
#else
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        struct sigaction prior{}, action{};
        if (::sigaction(signal, nullptr, &prior) != 0 || prior.sa_handler != SIG_DFL) continue;
        action.sa_sigaction = fatalSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        (void)sigemptyset(&action.sa_mask);
        installed |= ::sigaction(signal, &action, nullptr) == 0;
    }
#endif
    if (installed) {
        priorTerminate = std::set_terminate(terminate);
        terminateHooked.store(true, std::memory_order_release);
    }
    active.store(installed, std::memory_order_release);
    return installed;
#endif
}
bool terminalBegun() noexcept { return claimed.load(std::memory_order_relaxed) != 0; }
#if defined(_WIN32)
bool lastChanceFilterInstalled() {
    const auto current = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(current);
    return current == lastChance;
}
#endif
}  // namespace detail
}  // namespace supple::diagnostics
