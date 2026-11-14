// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cstdlib>
#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <thread>
#endif
namespace supple::test {
inline int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
inline int environment(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value ? value : "");
#else
    return value ? ::setenv(name, value, 1) : ::unsetenv(name);
#endif
}
#if !defined(_WIN32)
// Runs `call` on a thread and says whether it opened the named pipe `fifo`
// and waited there for a writer. Each time it does, a writer is opened and
// closed so that the call can go on. No clock decides: the loop ends when
// the call returns.
template <class Call>
bool waitsOnFifo(const std::filesystem::path& fifo, Call call) {
    std::atomic<bool> done{false};
    std::thread worker([&] {
        call();
        done.store(true);
    });
    bool waited = false;
    while (!done.load()) {
        const int writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (writer >= 0) {
            waited = true;
            ::close(writer);
        }
        std::this_thread::yield();
    }
    worker.join();
    return waited;
}
#endif
}  // namespace supple::test
