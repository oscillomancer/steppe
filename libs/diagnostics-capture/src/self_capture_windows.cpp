// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include "self_capture_internal.h"
#include "supple/diagcap/trace.h"
#include "supple/diagctx/table.h"
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(realtime_sanitizer)
#define SUPPLE_SELF_CAPTURE_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SUPPLE_SELF_CAPTURE_SANITIZER 1
#endif
// The bridge's fault handlers on Windows (topic 6c): the last-chance exception filter, SIGABRT
// and std::terminate. A handler here runs in ordinary thread context, not a signal, but it keeps
// the same rule: static storage, system calls and arithmetic only, so a heap or a lock the
// failing thread was holding cannot stop it. The walk reads the stack through the OS unwinder
// after checking that the stack pointer lies on the thread's own stack.
namespace supple::diagnostics::selfcapture {
namespace {
std::atomic<unsigned> claimed{0};
LPTOP_LEVEL_EXCEPTION_FILTER priorFilter = nullptr;
[[maybe_unused]] std::terminate_handler priorTerminate = nullptr;
std::atomic<bool> terminateHooked{false};
// The code the MSVC runtime gives an escaping C++ exception: the runtime's own filter (the
// prior one) turns it into std::terminate, whose hook records it (as the worker's does).
constexpr DWORD kCppExceptionCode = 0xE06D7363;
// STATUS_FAIL_FAST_EXCEPTION: what abort() ends the bridge with (fatal by its severity bits).
constexpr UINT kFailFast = 0xC0000409u;

bool readOwn(std::uint64_t address, void* out, std::size_t bytes) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), out,
                             bytes, &got) != FALSE &&
           got == bytes;
}

[[noreturn]] void terminateHook() noexcept;
void threadHook() noexcept {
    // Room on the stack for the handler after an overflow, and our terminate hook on this thread
    // (the runtime keeps the handler per thread).
    ULONG bytes = 64 * 1024;
    (void)SetThreadStackGuarantee(&bytes);
    if (terminateHooked.load(std::memory_order_acquire)) (void)std::set_terminate(terminateHook);
}

Fault faultFrom(const CONTEXT* context) noexcept {
    Fault fault;
    fault.tid = GetCurrentThreadId();
    fault.native = context;
#if defined(_M_X64) || defined(__x86_64__)
    fault.pc = context->Rip;
    fault.fp = context->Rbp;
    fault.sp = context->Rsp;
#elif defined(_M_ARM64)
    fault.pc = context->Pc;
    fault.fp = context->Fp;
    fault.sp = context->Sp;
#endif
    return fault;
}

LONG WINAPI lastChance(EXCEPTION_POINTERS* exception) {
    if (exception && exception->ExceptionRecord &&
        exception->ExceptionRecord->ExceptionCode == kCppExceptionCode && priorFilter)
        return priorFilter(exception);  // the runtime makes it std::terminate; the hook records
    if (!claimed.exchange(1, std::memory_order_relaxed) && exception && exception->ContextRecord) {
        const std::uint64_t start = monotonic();
        (void)captureAndPublish(faultFrom(exception->ContextRecord), start);
    }
    // The fault stays fatal and the status stays the exception's: the filter we replaced runs
    // as it would have, and a request to resume the faulting code is refused.
    const LONG chained = priorFilter ? priorFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
    return chained == EXCEPTION_CONTINUE_EXECUTION ? EXCEPTION_CONTINUE_SEARCH : chained;
}

void abortSignal(int) {
    if (!claimed.exchange(1, std::memory_order_relaxed)) {
        const std::uint64_t start = monotonic();
        CONTEXT context{};
        RtlCaptureContext(&context);
        (void)captureAndPublish(faultFrom(&context), start);
    }
    // Ends at once, as the bridge's abort handler did before capture (CI 44242073297).
    TerminateProcess(GetCurrentProcess(), kFailFast);
}

[[noreturn]] void terminateHook() noexcept {
    if (!claimed.exchange(1, std::memory_order_relaxed)) {
        const std::uint64_t start = monotonic();
        CONTEXT context{};
        RtlCaptureContext(&context);
        (void)captureAndPublish(faultFrom(&context), start);
    }
    if (priorTerminate) priorTerminate();
    std::abort();
}
}  // namespace

bool identifyImage(const void* address, BridgeModule& out) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module) ||
        !module)
        return false;
    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(module));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = static_cast<const IMAGE_NT_HEADERS*>(
        static_cast<const void*>(reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0) return false;
    out = BridgeModule{};
    out.start = base;
    out.end = base + nt->OptionalHeader.SizeOfImage;
    const detail::ReadMemory reader = [](std::uint64_t a, void* buffer, std::size_t n) {
        return readOwn(a, buffer, n);
    };
    std::uint64_t bias = 0;
    const auto id = detail::peDebugId(reader, base, &bias);
    out.bias = bias;
    if (id && id->size() % 2 == 0 && id->size() / 2 <= kModuleIdBytes) {
        const auto nibble = [](char c) -> std::uint8_t {
            return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        for (std::size_t i = 0; i < id->size(); i += 2)
            out.id[i / 2] = static_cast<std::uint8_t>(nibble((*id)[i]) << 4 | nibble((*id)[i + 1]));
        out.idLength = static_cast<std::uint32_t>(id->size() / 2);
    }
    return true;
}

void walkFault(const Fault& fault, detail::CapsuleThreadData& thread,
               std::uint64_t deadline) noexcept {
    std::uint32_t count = 0;
    thread.frames[count++] = fault.pc;
    thread.head.stop = 0;  // Stop::End
    (void)deadline;
#if defined(_M_X64) || defined(__x86_64__)
    if (!fault.native) {
        thread.head.frameCount = count;
        return;
    }
    CONTEXT context = *static_cast<const CONTEXT*>(fault.native);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);  // the thread's own stack: the one a fault is on
    DWORD64 previous = 0;
    for (;;) {
        if (deadline != 0 && (count & 15) == 0 && monotonic() >= deadline) {
            thread.head.stop = 6;  // Stop::Deadline
            break;
        }
        if (count >= detail::kCapsuleFrames) {
            thread.head.stop = 1;  // Stop::Depth
            break;
        }
        // The unwinder reads the frame above the stack pointer: it must be on the stack.
        if ((context.Rsp & 7) != 0 || context.Rsp < low || context.Rsp + 16 > high ||
            (previous != 0 && context.Rsp <= previous)) {
            thread.head.stop = 4;  // Stop::BadFramePointer
            break;
        }
        previous = context.Rsp;
        DWORD64 imageBase = 0;
        if (PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr)) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context,
                             &handlerData, &establisher, nullptr);
        } else {  // a leaf function: the return address is at the stack pointer
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
        if (context.Rip == 0) {
            thread.head.stop = 0;
            break;
        }
        thread.frames[count++] = context.Rip;
    }
#endif
    thread.head.frameCount = count;
}

bool installHandlers() noexcept {
#if defined(SUPPLE_SELF_CAPTURE_SANITIZER)
    return false;
#else
    // Always ours: the runtime's startup has set its own filter, which becomes the prior one
    // and is chained. SetUnhandledExceptionFilter cannot fail.
    diagctx::setThreadHook(&threadHook);
    priorFilter = SetUnhandledExceptionFilter(lastChance);
    // Replaces the bridge's earlier SIGABRT handler (which only ended the process with the same
    // status): this one captures first and then ends it the same way.
    (void)std::signal(SIGABRT, abortSignal);
    priorTerminate = std::set_terminate(terminateHook);
    terminateHooked.store(true, std::memory_order_release);
    ULONG bytes = 64 * 1024;
    (void)SetThreadStackGuarantee(&bytes);
    return true;
#endif
}
}  // namespace supple::diagnostics::selfcapture
