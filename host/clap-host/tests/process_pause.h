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
#include <csignal>
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
inline bool pauseProcess(int pid) { return ::kill(pid, SIGSTOP) == 0; }
inline bool resumeProcess(int pid) { return ::kill(pid, SIGCONT) == 0; }
inline bool killProcess(int pid) { return ::kill(pid, SIGKILL) == 0; }
#endif
}  // namespace supple::test
