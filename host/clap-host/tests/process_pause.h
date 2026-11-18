// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A loaded machine's stall of a bridge, for tests: the whole process stops
// and then goes on (SIGSTOP and SIGCONT; NtSuspendProcess and
// NtResumeProcess on Windows), or it dies. The load-dependent failures of
// lead-round26 were reproduced this way.
#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <chrono>
#include <csignal>
#include <thread>
#if defined(__APPLE__)
#include <sys/proc.h>
#include <sys/sysctl.h>
#else
#include <filesystem>
#include <fstream>
#include <string>
#endif
#endif

namespace supple::test {
#if defined(_WIN32)
inline bool ntProcessCall(int pid, const char* name) {
    using Call = LONG(NTAPI*)(HANDLE);
    const auto call = reinterpret_cast<Call>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)));
    HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, static_cast<DWORD>(pid));
    if (!call || !process) {
        if (process) CloseHandle(process);
        return false;
    }
    const bool ok = call(process) >= 0;
    CloseHandle(process);
    return ok;
}
inline bool pauseProcess(int pid) { return ntProcessCall(pid, "NtSuspendProcess"); }
inline bool resumeProcess(int pid) { return ntProcessCall(pid, "NtResumeProcess"); }
inline bool killProcess(int pid) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const bool ok = TerminateProcess(process, 9) != 0;
    CloseHandle(process);
    return ok;
}
#else
// SIGSTOP is asynchronous: kill() returns before the process's threads have
// stopped, and a thread woken meanwhile could still answer a request. So
// wait until the whole process is stopped (NtSuspendProcess already does).
#if defined(__APPLE__)
inline bool processStopped(int pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    return sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size > 0 &&
           info.kp_proc.p_stat == SSTOP;
}
#else
inline bool processStopped(int pid) {
    std::error_code ec;
    bool any = false;
    for (const auto& task :
         std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec)) {
        std::ifstream stat(task.path() / "stat");
        std::string line;
        std::getline(stat, line);
        const auto close = line.rfind(')');  // the state follows the command name
        if (close == std::string::npos || close + 2 >= line.size()) return false;
        const char state = line[close + 2];
        if (state != 'T' && state != 't') return false;
        any = true;
    }
    return any && !ec;
}
#endif
// True once the process has stopped; false if it could not be signalled or
// had not stopped within a minute (a guard against a lost stop, not a
// measure: a loaded machine stops it far sooner).
inline bool pauseProcess(int pid) {
    if (::kill(pid, SIGSTOP) != 0) return false;
    const auto guard = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!processStopped(pid)) {
        if (std::chrono::steady_clock::now() > guard) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
inline bool resumeProcess(int pid) { return ::kill(pid, SIGCONT) == 0; }
inline bool killProcess(int pid) { return ::kill(pid, SIGKILL) == 0; }
#endif
}  // namespace supple::test
