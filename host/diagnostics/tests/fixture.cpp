// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include "../src/terminal.h"
#include "supple/base/rt.h"
#include "supple/diagnostics/frontend.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
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
#if defined(_WIN32)
int handledFault() {
    __try {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 9;
}
#else
volatile sig_atomic_t handled = 0;
void priorSignal(int) { handled = 1; }
#endif
void priorTerminate() {
    std::fputs("prior terminate handler\n", stderr);
    std::_Exit(44);
}
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
    if (mode == "fault") actualFault();
    if (mode == "thread-fault") {
        std::thread worker([] {
            (void)supple::diagnostics::registerCurrentThread();
            actualFault();
        });
        worker.join();
    }
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
        std::atexit([] { std::abort(); });
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
    if (std::string_view(argv[2]) == "prior-terminate") std::set_terminate(priorTerminate);
    return supple::diagnostics::runFrontend(argc, argv, supple::diagnostics::Role::Fixture, body,
                                            {std::filesystem::path(argv[1])});
}
