// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
//
// Fault injection for the Linux collector's ptrace calls (review round 29). A test
// preloads this library into the diagnostics fixture; the supervisor's ptrace calls
// pass through it, and in one chosen mode a call fails or is held until the worker
// is in a state the collector cannot otherwise be put in on demand. No wait here
// is decided by elapsed time: every one is on process state read from /proc.
// SUPPLE_PTRACE_SHIM picks the mode, SUPPLE_PTRACE_SHIM_LOG names a file that gets
// one line per injection, so a test can tell that the fault really happened.
//
//   interrupt-fail  PTRACE_INTERRUPT on a non-leader thread fails with EIO and is not
//                   sent: the thread stays attached and running.
//   walk-freeze     after the first successful PTRACE_GETREGSET of a non-leader thread
//                   (its stop consumed), hold until the worker's leader is a zombie
//                   (its own terminal hold has expired); the collector then walks and
//                   releases a thread that died while stopped.
//   leader-exit     after PTRACE_INTERRUPT of the leader, hold until the leader is a
//                   zombie with no other thread left; the collector's wait on the main
//                   thread then observes the worker's exit.
#include <dirent.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// Request numbers from the kernel's ABI. <sys/ptrace.h> is left out: it declares
// ptrace() with a libc-specific request type, which this definition must not clash with.
constexpr int kPtraceGetRegSet = 0x4204;
constexpr int kPtraceInterrupt = 0x4207;

using PtraceFn = long (*)(int, pid_t, void*, void*);

bool fired = false;

void note(const char* what, pid_t tid) {
    const char* path = std::getenv("SUPPLE_PTRACE_SHIM_LOG");
    if (!path) return;
    std::FILE* f = std::fopen(path, "a");
    if (!f) return;
    std::fprintf(f, "%s tid=%d\n", what, static_cast<int>(tid));
    std::fclose(f);
}
pid_t tgidOf(pid_t tid) {
    char path[64];
    char line[256];
    std::snprintf(path, sizeof path, "/proc/%d/status", static_cast<int>(tid));
    std::FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    pid_t tgid = -1;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "Tgid:", 5) == 0) tgid = static_cast<pid_t>(std::atoi(line + 5));
    std::fclose(f);
    return tgid;
}
char stateOf(pid_t pid) {
    char path[64];
    char text[512];
    std::snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(pid));
    std::FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    const std::size_t n = std::fread(text, 1, sizeof text - 1, f);
    std::fclose(f);
    text[n] = 0;
    const char* close = std::strrchr(text, ')');
    return close && close[1] == ' ' ? close[2] : 0;
}
bool onlyLeaderLeft(pid_t leader) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/task", static_cast<int>(leader));
    DIR* dir = ::opendir(path);
    if (!dir) return true;
    int others = 0;
    while (const dirent* e = ::readdir(dir))
        if (e->d_name[0] != '.' && std::atoi(e->d_name) != leader) ++others;
    ::closedir(dir);
    return others == 0;
}
// whole: also wait until every other thread of the worker is gone, so the leader's
// exit is reportable when the collector resumes.
void awaitLeaderZombie(pid_t leader, bool whole) {
    for (;;) {
        const char s = stateOf(leader);
        if (s == 0 || ((s == 'Z' || s == 'X') && (!whole || onlyLeaderLeft(leader)))) return;
        ::usleep(1000);  // pacing only; the exit condition is the state
    }
}
}  // namespace

extern "C" long ptrace(int request, ...) {
    va_list ap;
    va_start(ap, request);
    const pid_t pid = va_arg(ap, pid_t);
    void* addr = va_arg(ap, void*);
    void* data = va_arg(ap, void*);
    va_end(ap);
    static PtraceFn real = nullptr;
    if (!real) real = reinterpret_cast<PtraceFn>(::dlsym(RTLD_NEXT, "ptrace"));
    const char* mode = std::getenv("SUPPLE_PTRACE_SHIM");
    if (!mode) return real(request, pid, addr, data);
    const pid_t tgid = tgidOf(pid);
    if (std::strcmp(mode, "interrupt-fail") == 0 && request == kPtraceInterrupt && tgid > 0 &&
        tgid != pid) {
        note("interrupt refused (EIO)", pid);
        errno = EIO;
        return -1;
    }
    const long r = real(request, pid, addr, data);
    const int saved = errno;
    if (!fired && std::strcmp(mode, "walk-freeze") == 0 && request == kPtraceGetRegSet && r == 0 &&
        tgid > 0 && tgid != pid) {
        fired = true;
        note("walk-freeze: registers read, waiting for the worker to die", pid);
        awaitLeaderZombie(tgid, false);
        note("walk-freeze: leader is a zombie, walk resumes", pid);
    }
    if (!fired && std::strcmp(mode, "leader-exit") == 0 && request == kPtraceInterrupt && r == 0 &&
        tgid == pid) {
        fired = true;
        note("leader-exit: leader interrupted, waiting for the worker to die", pid);
        awaitLeaderZombie(pid, true);
        note("leader-exit: leader is a zombie, collector resumes", pid);
    }
    errno = saved;
    return r;
}
