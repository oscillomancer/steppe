// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>
#include "child_status.h"
#include "supple/base/waitable.h"
#include "supple/host/child_process.h"
extern char** environ;
namespace supple::host {
namespace {
Error failure(const char* code, const char* message, int rc = errno) {
    return Error(code, std::string(message) + ": " + std::strerror(rc), "/path",
                 "check the child executable and its permissions");
}
}  // namespace
struct ChildProcess::Impl {
    pid_t pid = 0;
    int fd = -1;
};
ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
void ChildProcess::killTree() {
    if (impl_->pid > 0) ::kill(-impl_->pid, SIGKILL);
}
ChildProcess::~ChildProcess() {
    if (impl_->fd >= 0) ::close(impl_->fd);
    if (impl_->pid > 0) {
        // Retain the unreaped leader until the tree is killed: no PID reuse.
        killTree();
        while (::waitpid(impl_->pid, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
}
Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const std::filesystem::path& executable,
                                                          std::span<const std::string> arguments) {
    return spawn(executable, arguments, LaunchOptions{});
}
int ChildProcess::pid() const { return impl_->pid; }
std::intptr_t ChildProcess::nativeProcess() const { return 0; }
Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const std::filesystem::path& executable,
                                                          std::span<const std::string> arguments,
                                                          LaunchOptions options) {
    std::unique_ptr<ChildProcess> child(new ChildProcess);
    int fds[2];
#if defined(__APPLE__)
    if (::pipe(fds) != 0) return failure("child.spawn", "cannot create pipe");
    if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        const int rc = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        return failure("child.spawn", "cannot protect pipe", rc);
    }
#else
    if (::pipe2(fds, O_CLOEXEC) != 0) return failure("child.spawn", "cannot create pipe");
#endif
    // Keep pipe endpoints above stdio and the reserved control fd, even when
    // the parent closed stdio. Mapping control must not overwrite capture.
    const int firstPipeFd = options.control < 0 ? 3 : 4;
    for (auto& fd : fds) {
        if (fd >= firstPipeFd) continue;
        const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, firstPipeFd);
        if (copy < 0) {
            const int rc = errno;
            ::close(fds[0]);
            ::close(fds[1]);
            return failure("child.spawn", "cannot duplicate pipe", rc);
        }
        ::close(fd);
        fd = copy;
    }
    child->impl_->fd = fds[0];
    int rc = ::fcntl(fds[0], F_GETFL);
    if (rc < 0 || ::fcntl(fds[0], F_SETFL, rc | O_NONBLOCK) < 0) {
        ::close(fds[1]);
        return failure("child.spawn", "cannot make pipe nonblocking");
    }
    posix_spawn_file_actions_t actions;
    rc = ::posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        ::close(fds[1]);
        return failure("child.spawn", "cannot initialize actions", rc);
    }
    posix_spawnattr_t attrs;
    rc = ::posix_spawnattr_init(&attrs);
    if (rc == 0) {
        // Preserve control first: it may occupy stdout if parent stdio was closed.
        if (options.control >= 0)
            rc = ::posix_spawn_file_actions_adddup2(&actions, static_cast<int>(options.control), 3);
        if (rc == 0)
            rc = options.captureOutput
                     ? ::posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO)
                     : ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                                          O_WRONLY, 0);
        if (rc == 0)
            rc = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY,
                                                    0);
        if (rc == 0 && !options.inheritStderr)
            rc = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY,
                                                    0);
        // Explicit dup2 also preserves fd 2 under Apple's CLOEXEC_DEFAULT.
        // A parent with closed stderr leaves it closed rather than failing spawn.
        if (rc == 0 && options.inheritStderr && ::fcntl(STDERR_FILENO, F_GETFD) >= 0)
            rc = ::posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO);
#if defined(__APPLE__)
        if (rc == 0 && (options.control < 0 || fds[0] != 3))
            rc = ::posix_spawn_file_actions_addclose(&actions, fds[0]);
        if (rc == 0 && (options.control < 0 || fds[1] != 3))
            rc = ::posix_spawn_file_actions_addclose(&actions, fds[1]);
        constexpr short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
        if (rc == 0)
            rc = ::posix_spawn_file_actions_addclosefrom_np(&actions, options.control < 0 ? 3 : 4);
        constexpr short flags = POSIX_SPAWN_SETPGROUP;
#endif
        if (rc == 0) rc = ::posix_spawnattr_setpgroup(&attrs, 0);
        if (rc == 0) rc = ::posix_spawnattr_setflags(&attrs, flags);
        const auto exe = executable.string();
        std::vector<char*> argv{const_cast<char*>(exe.c_str())};
        for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        if (rc == 0)
            rc = ::posix_spawn(&child->impl_->pid, exe.c_str(), &actions, &attrs, argv.data(),
                               environ);
        ::posix_spawnattr_destroy(&attrs);
    }
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        child->impl_->pid = 0;
        return failure("child.spawn", "cannot start child", rc);
    }
    return child;
}
Result<ChildProcess::Output> ChildProcess::collect(std::chrono::steady_clock::time_point deadline,
                                                   std::size_t maxBytes) {
    Output output;
    std::array<char, 8192> bytes;
    bool eof = false;
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            return failure("child.timeout", "child exceeded its deadline", ETIMEDOUT);
        if (!eof) {
            const auto n = ::read(impl_->fd, bytes.data(), bytes.size());
            if (n > 0) {
                if (static_cast<std::size_t>(n) > maxBytes - output.text.size())
                    return failure("child.output_limit", "child output exceeds its byte limit",
                                   EFBIG);
                output.text.append(bytes.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0)
                eof = true;
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                return failure("child.io", "cannot read child output");
        }
        siginfo_t exit{};
        if (::waitid(P_PID, static_cast<id_t>(impl_->pid), &exit, WEXITED | WNOHANG | WNOWAIT) <
                0 &&
            errno != EINTR)
            return failure("child.wait", "cannot observe child exit");
        if (childEnded(exit)) {
            output.exitCode = static_cast<std::uint32_t>(exit.si_status);
            output.crashed = exit.si_code == CLD_KILLED || exit.si_code == CLD_DUMPED;
            if (output.crashed || eof) return output;
        }
        WaitItem item{eof ? Waitable{} : Waitable(impl_->fd)};
        if (waitFor(eof ? std::span<WaitItem>{} : std::span<WaitItem>(&item, 1),
                    static_cast<int>(std::min<std::int64_t>(remaining, 10))) < 0 &&
            errno != EINTR)
            return failure("child.io", "cannot wait for child output");
    }
}
}  // namespace supple::host
