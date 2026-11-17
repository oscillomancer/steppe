// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <map>
#include "trace.h"
// Linux collector. The supervisor is the worker's parent, so Yama's default
// restricted policy (ptrace_scope 1) lets it trace the worker as an ancestor:
// nothing is raised, no sysctl changes, no PR_SET_PTRACER_ANY. Stricter policies
// (2, 3), a non-dumpable worker or a seccomp filter refuse the attach; every
// thread then reports its reason and the report is partial. Threads are stopped
// one at a time, only while their registers and stack are read, and always
// detached before the worker is released.
namespace supple::diagnostics::detail {
namespace {
// The worker waits one second for its acknowledgement; collection keeps well inside.
constexpr std::chrono::milliseconds kBudget{600};
constexpr std::size_t kMaxMapsBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxTasks = 4096;

struct Seized {
    int status = 0;
    bool stopped = false;
};
class Collector {
  public:
    explicit Collector(Native& native)
        : native_(native),
          pid_(static_cast<pid_t>(native.process)),
          deadline_(std::chrono::steady_clock::now() + kBudget) {}
    ~Collector() {
        settle();
        // A thread still attached cannot block the worker's exit: the supervisor's
        // wait drains it (native_posix.cpp, observe).
        native_.traced = !seized_.empty();
    }
    bool expired() const { return std::chrono::steady_clock::now() >= deadline_; }
    std::optional<std::string> maps() const {
        const std::string path = "/proc/" + std::to_string(pid_) + "/maps";
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return std::nullopt;
        std::string text;
        char buffer[16384];
        for (;;) {
            const auto n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ::close(fd);
                return n == 0 ? std::optional<std::string>(std::move(text)) : std::nullopt;
            }
            text.append(buffer, static_cast<std::size_t>(n));
            if (text.size() > kMaxMapsBytes) {
                ::close(fd);
                return std::nullopt;
            }
        }
    }
    bool read(std::uint64_t address, void* out, std::size_t bytes) const {
        iovec local{out, bytes};
        iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), bytes};
        const auto n = ::process_vm_readv(pid_, &local, 1, &remote, 1, 0);
        return n == static_cast<ssize_t>(bytes);
    }
    std::vector<std::uint64_t> threads() const {
        std::vector<std::uint64_t> tids;
        const std::string path = "/proc/" + std::to_string(pid_) + "/task";
        DIR* dir = ::opendir(path.c_str());
        if (!dir) return tids;
        while (const dirent* entry = ::readdir(dir)) {
            std::uint64_t tid = 0;
            const char* name = entry->d_name;
            if (*name < '0' || *name > '9') continue;
            for (; *name >= '0' && *name <= '9'; ++name)
                tid = tid * 10 + static_cast<std::uint64_t>(*name - '0');
            if (*name == 0 && tids.size() < kMaxTasks) tids.push_back(tid);
        }
        ::closedir(dir);
        return tids;
    }
    Missing stop(std::uint64_t id, Regs& regs) {
        const pid_t tid = static_cast<pid_t>(id);
        if (::ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) != 0)
            return errno == ESRCH ? Missing::ThreadExited : Missing::PtraceDenied;
        seized_[tid] = {};
        if (::ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0) {
            // Still attached, though not stopped, and detaching needs a stopped
            // tracee: it stays in the set for settle() and the supervisor's wait.
            return errno == ESRCH ? Missing::ThreadExited : Missing::PtraceDenied;
        }
        for (;;) {
            int status = 0;
            const pid_t r = ::waitpid(tid, &status, __WALL | WNOHANG);
            if (r == tid) {
                if (!WIFSTOPPED(status)) {
                    seized_.erase(tid);
                    exited(tid, status);
                    return Missing::ThreadExited;
                }
                seized_[tid] = {status, true};
                break;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) {
                seized_.erase(tid);
                return Missing::ThreadExited;
            }
            if (expired()) return Missing::Deadline;  // detached by settle() once stopped
            const timespec pause{0, 100'000};
            ::nanosleep(&pause, nullptr);
        }
        user_regs_struct raw{};
        iovec io{&raw, sizeof(raw)};
        if (::ptrace(PTRACE_GETREGSET, tid,
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(NT_PRSTATUS)), &io) != 0 ||
            io.iov_len < sizeof(raw)) {
            release(id);
            return Missing::RegistersUnreadable;
        }
#if defined(__x86_64__)
        regs = {raw.rip, raw.rsp, raw.rbp};
#else
        regs = {raw.pc, raw.sp, raw.regs[29]};
#endif
        return Missing::None;
    }
    void release(std::uint64_t id) {
        const pid_t tid = static_cast<pid_t>(id);
        auto found = seized_.find(tid);
        if (found == seized_.end()) return;
        // Hand a pending signal-delivery-stop signal back; never swallow a real one.
        const int status = found->second.status;
        const int signal = found->second.stopped && (status >> 16) == 0 ? WSTOPSIG(status) : 0;
        if (::ptrace(PTRACE_DETACH, tid, nullptr, reinterpret_cast<void*>(std::intptr_t{signal})) ==
            0)
            seized_.erase(found);
        else
            found->second = {};  // died while stopped: a zombie to reap, see settle()
    }
    // A thread whose stop was still pending at the deadline, or that died while
    // attached: detach it if it has stopped since, and reap it if it has died.
    // Otherwise it stays attached, and the supervisor's wait drains it when the
    // worker dies, which it does right after the acknowledgement.
    void settle() {
        const auto pending = seized_;
        for (const auto& item : pending) {
            if (!item.second.stopped) {
                int status = 0;
                const pid_t r = ::waitpid(item.first, &status, __WALL | WNOHANG);
                if (r == item.first && !WIFSTOPPED(status)) {
                    seized_.erase(item.first);
                    exited(item.first, status);
                    continue;
                }
                if (r != item.first) continue;
                seized_[item.first] = {status, true};
            }
            release(static_cast<std::uint64_t>(item.first));
        }
    }

  private:
    // The worker's main thread is its process id. If its exit is observed here, the
    // supervisor's own wait would find nothing: hand the real status back.
    void exited(pid_t tid, int status) {
        if (tid != pid_) return;
        native_.status = status;
        native_.statusKnown = true;
        native_.exited = true;
    }

    Native& native_;
    pid_t pid_;
    std::chrono::steady_clock::time_point deadline_;
    std::map<pid_t, Seized> seized_;
};
}  // namespace
TraceSnapshot collectProcessTrace(Native& native, const Packet& terminal) {
#if defined(__x86_64__) || defined(__aarch64__)
    Collector collector(native);
    TraceOps ops;
    ops.expired = [&] { return collector.expired(); };
    ops.maps = [&] { return collector.maps(); };
    ops.read = [&](std::uint64_t a, void* out, std::size_t n) { return collector.read(a, out, n); };
    ops.threads = [&] { return collector.threads(); };
    ops.stop = [&](std::uint64_t tid, Regs& regs) { return collector.stop(tid, regs); };
    ops.release = [&](std::uint64_t tid) { collector.release(tid); };
    return collectTrace(ops, terminal.tid, Regs{terminal.pc, terminal.sp, terminal.frame},
                        static_cast<std::uint64_t>(native.process), hostArch(), native.table);
#else
    (void)native;
    TraceSnapshot snapshot;
    ThreadTrace fault;
    fault.role = Role::Fault;
    fault.missing = Missing::Unsupported;
    (void)terminal;
    snapshot.threads.push_back(fault);
    return snapshot;
#endif
}
}  // namespace supple::diagnostics::detail
