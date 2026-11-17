// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include "../src/terminal.h"
#include "supple/base/rt.h"
#include "supple/diagctx/table.h"
#include "supple/diagnostics/frontend.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <fstream>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#if defined(__linux__)
#include <link.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#endif
#endif
namespace {
std::atomic<std::uintptr_t> badAddress{0};
void actualFault() SUPPLE_RT { *reinterpret_cast<volatile int*>(badAddress.load()) = 1; }
std::atomic<unsigned> depth{0};
#if defined(__clang__)
[[clang::optnone]]
#elif defined(__GNUC__)
[[gnu::optimize("O0")]]
#endif
[[gnu::noinline]] unsigned exhaustStack() {
    volatile char data[8192];
    data[0] = 1;
    const unsigned next = depth.fetch_add(1);
    return next < 1000000 ? exhaustStack() + static_cast<unsigned>(data[0]) : 0;
}
#if defined(__linux__)
// Trace fixtures. Each marked function sits on its own page so a return address
// maps to exactly one of them, whatever the optimization level.
std::atomic<unsigned> parked{0};
std::atomic<bool> neverReleased{false};
[[gnu::noinline, gnu::aligned(4096)]] void parkInner() {
    while (!neverReleased.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
[[gnu::noinline, gnu::aligned(4096)]] void parkOuter() {
    parked.fetch_add(1);
    parkInner();
    asm volatile("" ::: "memory");
}
[[gnu::noinline, gnu::aligned(4096)]] void faultMiddle() {
    actualFault();
    asm volatile("" ::: "memory");
}
[[gnu::noinline, gnu::aligned(4096)]] void faultOuter() {
    faultMiddle();
    asm volatile("" ::: "memory");
}
// The executable's load bias and GNU build ID, found by reading its own program
// headers, independently of the collector's parser.
int imageInfo(dl_phdr_info* info, std::size_t, void* out) {
    auto* result = static_cast<std::pair<std::uintptr_t, std::string>*>(out);
    result->first = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto* at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        std::size_t pos = 0;
        while (pos + 12 <= ph.p_filesz) {
            std::uint32_t namesz, descsz, kind;
            std::memcpy(&namesz, at + pos, 4);
            std::memcpy(&descsz, at + pos + 4, 4);
            std::memcpy(&kind, at + pos + 8, 4);
            const std::size_t desc = pos + 12 + ((namesz + 3) & ~3U);
            if (kind == 3) {
                for (std::uint32_t b = 0; b < descsz; ++b) {
                    result->second += "0123456789abcdef"[at[desc + b] >> 4];
                    result->second += "0123456789abcdef"[at[desc + b] & 15];
                }
                return 1;
            }
            pos = desc + ((descsz + 3) & ~3U);
        }
    }
    return 1;  // the first object is the executable
}
void writeExpected(const char* path) {
    std::pair<std::uintptr_t, std::string> image;
    dl_iterate_phdr(imageInfo, &image);
    auto offset = [&](auto function) {
        return reinterpret_cast<std::uintptr_t>(function) - image.first;
    };
    std::ofstream out(path);
    out << "{\"build_id\":\"" << image.second << "\",\"parkOuter\":" << offset(&parkOuter)
        << ",\"faultMiddle\":" << offset(&faultMiddle) << ",\"faultOuter\":" << offset(&faultOuter)
        << ",\"pid\":" << ::getpid() << "}\n";
}
#endif
#if defined(_WIN32)
// Trace fixtures, as on Linux but without page alignment: the lifecycle test
// attributes a frame to the nearest marked function start below it.
std::atomic<unsigned> parked{0};
std::atomic<bool> neverReleased{false};
std::atomic<unsigned> sink{0};  // work after each call keeps it from being a tail call
[[gnu::noinline]] void parkInner() {
    while (!neverReleased.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sink.fetch_add(1);
}
[[gnu::noinline]] void parkOuter() {
    parked.fetch_add(1);
    parkInner();
    sink.fetch_add(1);
}
[[gnu::noinline]] void faultMiddle() {
    actualFault();
    sink.fetch_add(1);
}
[[gnu::noinline]] void faultOuter() {
    faultMiddle();
    sink.fetch_add(1);
}
// The executable's CodeView identity and load bias, read from its own headers with the
// SDK's structures, independently of the collector's parser. The id is the PDB GUID in
// symbol-server order followed by the age, 40 lowercase hex characters; empty when the
// image has no CodeView record.
void writeExpected(const char* path) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::uintptr_t bias = base - static_cast<std::uintptr_t>(nt->OptionalHeader.ImageBase);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    std::string id;
    const auto* entries =
        reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (DWORD i = 0;
         directory.VirtualAddress && i < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        const auto* record =
            reinterpret_cast<const unsigned char*>(base + entries[i].AddressOfRawData);
        if (std::memcmp(record, "RSDS", 4) != 0) continue;
        GUID guid;
        DWORD age;
        std::memcpy(&guid, record + 4, sizeof(guid));
        std::memcpy(&age, record + 20, sizeof(age));
        char text[64];
        std::snprintf(text, sizeof(text), "%08lx%04x%04x", static_cast<unsigned long>(guid.Data1),
                      static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3));
        id = text;
        for (unsigned char byte : guid.Data4) {
            std::snprintf(text, sizeof(text), "%02x", static_cast<unsigned>(byte));
            id += text;
        }
        std::snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(age));
        id += text;
        break;
    }
    auto offset = [&](auto function) { return reinterpret_cast<std::uintptr_t>(function) - bias; };
    std::ofstream out(path);
    out << "{\"build_id\":\"" << id << "\",\"parkOuter\":" << offset(&parkOuter)
        << ",\"faultMiddle\":" << offset(&faultMiddle) << ",\"faultOuter\":" << offset(&faultOuter)
        << ",\"pid\":" << GetCurrentProcessId() << "}\n";
}
// An earlier handler that deals with the exception, so it never reaches the
// last-chance filter. A vectored handler, not __try: clang-cl rejects the
// language extension under -Werror.
std::atomic<bool> expectFault{false};
LONG CALLBACK priorHandler(EXCEPTION_POINTERS* info) {
    if (expectFault.load() && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
int handledFault() {
    void* const handle = AddVectoredExceptionHandler(1, priorHandler);
    if (!handle) return 9;
    expectFault = true;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);  // continuable
    expectFault = false;
    RemoveVectoredExceptionHandler(handle);
    return 0;
}
#else
volatile sig_atomic_t handled = 0;
void priorSignal(int) { handled = 1; }
#endif
void priorTerminate() {
    std::fputs("prior terminate handler\n", stderr);
    std::_Exit(44);
}
#if defined(_WIN32)
// A top-level filter that was already in place when the worker installed its own, as the
// MSVC runtime's always is. The last-chance filter must chain to it after its handoff.
bool filterResumes = false;
LONG WINAPI priorFilter(EXCEPTION_POINTERS*) {
    std::fputs("prior filter ran\n", stderr);
    return filterResumes ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#endif
int body(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[2];
    if (mode == "normal") return 7;
    if (mode == "negative-return") return -1;
    if (mode == "exit") std::_Exit(31);
#if !defined(_WIN32)
    if (mode == "fork-channel") {
        const auto helper = ::fork();
        if (helper < 0) return 2;
        if (helper == 0) {
            // Keep the diagnostic channel, but no protocol output pipe writer.
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
            char release;
            while (::read(STDIN_FILENO, &release, 1) < 0 && errno == EINTR) {
            }
            ::_exit(0);
        }
        std::cout << helper << std::endl;
        std::_Exit(31);
    }
#endif
    if (mode == "abort") std::abort();
    if (mode == "terminate") std::terminate();
    if (mode == "prior-terminate") {
        std::terminate();
    }
    // An exception nobody catches reaches the terminate handler on every platform, and so
    // the program's own handler (on Windows it first meets the top-level filter).
    if (mode == "prior-terminate-throw") throw std::runtime_error("uncaught");
#if defined(_WIN32)
    // Evidence for a failed capture: whether the last-chance filter is in place.
    if (mode == "fault" || mode == "prior-filter" || mode == "prior-filter-resume")
        std::fprintf(stderr, "last-chance filter installed: %s\n",
                     supple::diagnostics::detail::lastChanceFilterInstalled() ? "yes" : "no");
    // A continuable exception whose chained filter asks to resume: the fault must stay
    // fatal, so the code after it must never run.
    if (mode == "prior-filter-resume") {
        RaiseException(0xE0000001, 0, 0, nullptr);
        std::fputs("resumed after the fault\n", stderr);
        return 0;
    }
#endif
    if (mode == "fault" || mode == "prior-filter") actualFault();
    if (mode == "thread-fault") {
        std::thread worker([] {
            (void)supple::diagnostics::registerCurrentThread();
            actualFault();
        });
        worker.join();
    }
#if defined(__linux__) || defined(_WIN32)
    if (mode == "trace" || mode == "trace-nondumpable") {
        if (argc != 4) return 4;
        writeExpected(argv[3]);
        const bool nondumpable = mode == "trace-nondumpable";  // Linux only
        // Owned threads register their roles, as the engine's do (topic 5): the report
        // must label each by what it registered as, and the faulting one as the fault.
        using supple::diagctx::ThreadRole;
        std::thread first([] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioLive, 2);
            parkOuter();
        });
        std::thread second([] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioBackground, 1);
            parkOuter();
        });
        std::thread faulting([nondumpable] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioRender);
            (void)supple::diagnostics::registerCurrentThread();
            while (parked.load() != 3) supple::rt::relax();  // two threads plus main
#if defined(__linux__)
            if (nondumpable) (void)::prctl(PR_SET_DUMPABLE, 0);
#else
            (void)nondumpable;
#endif
            faultOuter();
        });
        parkOuter();  // the main thread parks too; the fault ends the process
        faulting.join();
        first.join();
        second.join();
    }
#endif
#if defined(__linux__)
    // A thread in a killable kernel wait cannot reach a ptrace stop, so the collector
    // gives up on it at its deadline and the supervisor must still see the worker
    // exit and write the report (review round 27, F1). The wait is a vfork until its
    // child execs or exits, as posix_spawn does; the child exits when the worker dies.
    if (mode == "trace-killable") {
        int gate[2];
        if (::pipe(gate) != 0) return 2;
        static std::atomic<long> blockedTid{0};
        std::thread blocked([gate] {
            blockedTid.store(::syscall(SYS_gettid));
            const pid_t child = ::vfork();
            if (child == 0) {
                ::syscall(SYS_close, gate[1]);
                char c;
                ::syscall(SYS_read, gate[0], &c, 1);  // end of file once the worker is gone
                ::syscall(SYS_exit_group, 0);
            }
        });
        while (blockedTid.load() == 0) supple::rt::relax();
        // State, not time: wait until the thread sits in the vfork wait ('D').
        const std::string stat = "/proc/self/task/" + std::to_string(blockedTid.load()) + "/stat";
        for (;;) {
            std::ifstream in(stat);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            const auto close = text.rfind(')');
            if (close != std::string::npos && close + 2 < text.size() && text[close + 2] == 'D')
                break;
            supple::rt::relax();
        }
        std::thread faulting([] {
            (void)supple::diagnostics::registerCurrentThread();
            faultOuter();
        });
        faulting.join();
        blocked.join();
    }
#endif
    if (mode == "stack") return static_cast<int>(exhaustStack());
    if (mode == "concurrent") {
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        auto fault = [&] {
            (void)supple::diagnostics::registerCurrentThread();
            ++ready;
            while (!go.load()) supple::rt::relax();
            actualFault();
        };
        std::thread a(fault), b(fault);
        while (ready.load() != 2) supple::rt::relax();
        go = true;
        a.join();
        b.join();
    }
    if (mode == "handled") {
#if defined(_WIN32)
        return handledFault();
#else
        ::raise(SIGSEGV);
        return handled == 1 ? 0 : 9;
#endif
    }
    if (mode == "killed") {
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), 9);
#else
        ::kill(::getpid(), SIGKILL);
#endif
    }
#if defined(_WIN32)
    if (mode == "failfast") RaiseFailFastException(nullptr, nullptr, 0);
#endif
    if (mode == "teardown") {
        std::atexit([] {
#if !defined(_WIN32)
            // Evidence for a teardown abort that left no capsule (native CI on macOS).
            // abort() rewrites the signal mask itself, so the mask is not worth printing;
            // what can make the kernel force a SIGILL instead of running the handler is
            // an alternate stack that is still enabled but no longer mapped (review
            // round 29). Print whether SIGABRT has our handler, and the alternate stack
            // as sigaltstack(2) reports it now: its flags, size, and whether its first
            // page is still mapped (msync fails with ENOMEM on an unmapped page).
            struct sigaction current{};
            (void)::sigaction(SIGABRT, nullptr, &current);
            const bool ours =
                (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction != nullptr;
            std::fprintf(stderr, "teardown: SIGABRT handler %s\n", ours ? "installed" : "absent");
            stack_t alt{};
            if (::sigaltstack(nullptr, &alt) != 0) {
                std::fprintf(stderr, "teardown: sigaltstack unreadable (errno %d)\n", errno);
            } else if ((alt.ss_flags & SS_DISABLE) != 0) {
                std::fprintf(stderr, "teardown: alternate stack disabled (flags %d)\n",
                             alt.ss_flags);
            } else {
                const bool mapped = ::msync(alt.ss_sp, 1, MS_ASYNC) == 0 || errno != ENOMEM;
                std::fprintf(stderr, "teardown: alternate stack enabled (flags %d, size %zu), %s\n",
                             alt.ss_flags, static_cast<std::size_t>(alt.ss_size),
                             mapped ? "mapped" : "NOT MAPPED");
            }
            std::fflush(stderr);
#endif
            std::abort();
        });
        return 0;
    }
    if (mode == "echo") {
        if (argc != 4 || std::string(argv[3]) != "argument with spaces \" and slash\\") return 4;
        std::cout << std::cin.rdbuf();
        return 0;
    }
    if (mode == "collector-loss" || mode == "fault-after-loss") {
#if defined(_WIN32)
        std::cout << GetCurrentProcessId() << std::endl;
#else
        std::cout << ::getpid() << std::endl;
#endif
        std::string line;
        std::getline(std::cin, line);
        if (mode == "fault-after-loss") actualFault();
        std::cout << line << std::endl;
        return 0;
    }
    return 2;
}
}  // namespace
int main(int argc, char** argv) {
#if defined(_WIN32)
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    rlimit limit{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &limit);
#endif
    if (argc < 3) return 2;
#if !defined(_WIN32)
    if (std::string_view(argv[2]) == "terminal-no-ack" && argc == 4) {
        supple::diagnostics::detail::Native native{std::atoi(argv[3])};
        supple::diagnostics::detail::Packet boot;
        boot.role = 3;
        std::fill(boot.incident.begin(), boot.incident.begin() + 32, '0');
        (void)supple::diagnostics::detail::installTerminal(native, boot);
        std::cout << "ready" << std::endl;
        std::string line;
        std::getline(std::cin, line);
        actualFault();
        return 9;
    }
#endif
#if !defined(_WIN32)
    if (std::string_view(argv[2]) == "handled") std::signal(SIGSEGV, priorSignal);
#endif
    if (std::string_view(argv[2]) == "prior-terminate" ||
        std::string_view(argv[2]) == "prior-terminate-throw")
        std::set_terminate(priorTerminate);
#if defined(_WIN32)
    if (std::string_view(argv[2]) == "prior-filter" ||
        std::string_view(argv[2]) == "prior-filter-resume") {
        filterResumes = std::string_view(argv[2]) == "prior-filter-resume";
        SetUnhandledExceptionFilter(priorFilter);
    }
#endif
    return supple::diagnostics::runFrontend(argc, argv, supple::diagnostics::Role::Fixture, body,
                                            {std::filesystem::path(argv[1])});
}
