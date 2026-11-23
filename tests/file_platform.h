// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>

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
// A temporary directory for a test fixture, unique to this process and this
// call: `prefix`, the process id and a counter. A name from the clock alone
// repeats when two test processes ask within one tick (ctest runs them in
// parallel), and then they share, and delete, each other's files. Anything
// already there was left by an earlier process with the same id: it is removed.
inline std::filesystem::path uniqueTempDir(const std::string& prefix) {
    static std::atomic<unsigned> next{0};
    auto path = std::filesystem::temp_directory_path() /
                (prefix + std::to_string(processId()) + "-" + std::to_string(next.fetch_add(1)));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    return path;
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
