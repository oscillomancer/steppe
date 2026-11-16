// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include "supple/diagnostics/frontend.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
namespace {
int body(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[2];
    if (mode == "normal") return 7;
    if (mode == "exit") std::_Exit(31);
    if (mode == "abort") std::abort();
    if (mode == "terminate") std::terminate();
    if (mode == "teardown") {
        std::atexit([] { std::abort(); });
        return 0;
    }
    if (mode == "echo") {
        if (argc != 4 || std::string(argv[3]) != "argument with spaces \" and slash\\") return 4;
        std::cout << std::cin.rdbuf();
        return 0;
    }
    if (mode == "collector-loss") {
#if defined(_WIN32)
        std::cout << GetCurrentProcessId() << std::endl;
#else
        std::cout << ::getpid() << std::endl;
#endif
        std::string line;
        std::getline(std::cin, line);
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
    return supple::diagnostics::runFrontend(argc, argv, supple::diagnostics::Role::Fixture, body,
                                            {std::filesystem::path(argv[1])});
}
