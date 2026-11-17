// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "terminal.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include "supple/diagctx/table.h"
#include "supple/diagnostics/frontend.h"
#if defined(__APPLE__)
#include "capsule.h"
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
static_assert(std::atomic<bool>::is_always_lock_free);
// One terminal producer wins. No pointer/model lookup, TLS, allocation or
// formatting. Raw PC/frame/SP and the thread id stay solely in the private native channel.
void handoff(Packet packet) noexcept {
    packet.kind = Message::Terminal;
    if (!detail::publish(endpoint, packet)) return;
#if defined(__APPLE__)
    // No external collector on macOS: the process reads its own threads, sends them,
    // and waits out the rest of the one-second hold (capture_macos.cpp).
    detail::captureAndHold(endpoint, packet);
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
    // would record a different cause and skip that handler.
    if (exception && exception->ExceptionRecord &&
        exception->ExceptionRecord->ExceptionCode == kCppExceptionCode)
        return priorFilter ? priorFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
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
    const LONG chained = priorFilter ? priorFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
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
    ULONG bytes = 64 * 1024;
    return SetThreadStackGuarantee(&bytes) != FALSE;
#else
    thread_local SignalStack stack;
    return stack.ready;
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
    if (installed) priorTerminate = std::set_terminate(terminate);
    active.store(installed, std::memory_order_release);
    return installed;
#endif
}
#if defined(_WIN32)
bool lastChanceFilterInstalled() {
    const auto current = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(current);
    return current == lastChance;
}
#endif
}  // namespace detail
}  // namespace supple::diagnostics
