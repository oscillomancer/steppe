// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include "native.h"
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/random.h>
#endif
extern char** environ;
namespace supple::diagnostics::detail {
namespace {
volatile sig_atomic_t supervisedPid = 0;
void forwardSignal(int signal) {
    if (supervisedPid > 0) (void)::kill(supervisedPid, signal);
}
Error failure() {
    return Error("diagnostics.bootstrap", "cannot prepare private supervision", {},
                 "continue without diagnostics; check process resource limits");
}
int noSignal() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
bool packetRead(int fd, Packet& packet, bool bootstrap = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto* data = reinterpret_cast<char*>(&packet);
    std::size_t at = 0;
    while (at < sizeof(packet)) {
        if (bootstrap) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) return false;
            pollfd item{fd, POLLIN, 0};
            const int ready = ::poll(&item, 1, static_cast<int>(left));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) return false;
        }
        const auto n = ::recv(fd, data + at, sizeof(packet) - at, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += static_cast<std::size_t>(n);
    }
    return valid(packet);
}
}  // namespace
std::uint64_t pid() { return static_cast<std::uint64_t>(::getpid()); }
Result<std::string> incidentId() {
    std::array<unsigned char, 16> random{};
    if (::getentropy(random.data(), random.size()) != 0) return failure();
    std::string id;
    for (auto x : random) {
        id += "0123456789abcdef"[x >> 4];
        id += "0123456789abcdef"[x & 15];
    }
    return id;
}
Result<Native> launch(int argc, char** argv, const Packet& boot) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return failure();
    for (auto& fd : pair) {
        if (fd < 4) {
            const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 4);
            ::close(fd);
            fd = copy;
        }
        if (fd < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            for (int value : pair)
                if (value >= 0) ::close(value);
            return failure();
        }
#if defined(__APPLE__)
        int enabled = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
            ::close(pair[0]);
            ::close(pair[1]);
            return failure();
        }
#endif
    }
    Native prepared{pair[0]};
    if (!publish(prepared, boot)) {
        ::close(pair[0]);
        ::close(pair[1]);
        return failure();
    }
    std::array<char, 32768> executable{};
#if defined(__APPLE__)
    std::uint32_t length = static_cast<std::uint32_t>(executable.size());
    if (_NSGetExecutablePath(executable.data(), &length) != 0) {
#else
    const auto length = ::readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (length <= 0 || static_cast<std::size_t>(length) >= executable.size() - 1) {
#endif
        ::close(pair[0]);
        ::close(pair[1]);
        return failure();
    }
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    char flag[] = "--diagnostic-worker", endpoint[] = "3";
    args.push_back(flag);
    args.push_back(endpoint);
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pair[0]);
        ::close(pair[1]);
        return failure();
    }
    int rc = ::posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
    if (!rc) rc = ::posix_spawn_file_actions_addclose(&actions, pair[0]);
    if (!rc) rc = ::posix_spawn_file_actions_addclose(&actions, pair[1]);
    pid_t child = 0;
    if (!rc) rc = ::posix_spawn(&child, executable.data(), &actions, nullptr, args.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pair[1]);
    if (rc) {
        ::close(pair[0]);
        return failure();
    }
    supervisedPid = child;
    struct sigaction forward{};
    forward.sa_handler = forwardSignal;
    ::sigemptyset(&forward.sa_mask);
    (void)::sigaction(SIGINT, &forward, nullptr);
    (void)::sigaction(SIGTERM, &forward, nullptr);
    Native native{pair[0], child};
    return native;  // never kill a healthy worker for collector failure
}
Result<Native> adopt(std::string_view value, Packet& boot) {
    if (value != "3") return failure();
    Native native{3};
    struct stat info{};
    if (::fstat(3, &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_uid != ::geteuid() ||
        ::fcntl(3, F_SETFD, FD_CLOEXEC) != 0)
        return failure();
    int type = 0;
    socklen_t size = sizeof(type);
    if (::getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &size) != 0) {
        if (errno != EPERM && errno != EACCES) return failure();
        native.trusted = false;
    } else if (type != SOCK_STREAM)
        return failure();
    if (!packetRead(3, boot, true) || boot.kind != Message::Boot) return failure();
    // With unavailable credential inspection, use the inherited endpoint only
    // for a bounded ready/return acknowledgement; no marker or handler trust.
#if defined(__linux__)
    ucred peer{};
    size = sizeof(peer);
    if (::getsockopt(3, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0) {
        if (errno != EPERM && errno != EACCES) return failure();
        native.trusted = false;
    } else if (peer.uid != ::geteuid() || static_cast<std::uint64_t>(peer.pid) != boot.pid)
        return failure();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(3, &uid, &gid) != 0) {
        if (errno != EPERM && errno != EACCES) return failure();
        native.trusted = false;
    } else if (uid != ::geteuid())
        return failure();
#endif
    return native;
}

bool receive(Native& native, Packet& packet) {
    return packetRead(static_cast<int>(native.channel), packet);
}
bool publish(Native& native, const Packet& packet) noexcept {
    return ::send(static_cast<int>(native.channel), &packet, sizeof(packet),
                  noSignal() | MSG_DONTWAIT) == static_cast<ssize_t>(sizeof(packet));
}
void acknowledge(Native& native) noexcept {
    const char ack = 'A';
    (void)::send(static_cast<int>(native.channel), &ack, 1, noSignal() | MSG_DONTWAIT);
}
void awaitAcknowledgement(Native& native) noexcept {
    const int fd = static_cast<int>(native.channel);
    if (fd < 0 || fd >= FD_SETSIZE) return;
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(fd, &ready);
    const timespec timeout{1, 0};
    (void)::pselect(fd + 1, &ready, nullptr, nullptr, &timeout, nullptr);
}
int wait(Native& native, bool& fatal) {
    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(static_cast<pid_t>(native.process), &status, 0);
    } while (result < 0 && errno == EINTR);
    fatal = result >= 0 && WIFSIGNALED(status);
    return result < 0 ? 1 : fatal ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
}
void close(Native& native) {
    if (native.channel >= 0) ::close(static_cast<int>(native.channel));
    native.channel = -1;
}
void closeProtocolStdio() {
    ::close(STDIN_FILENO);
    ::close(STDOUT_FILENO);
}
}  // namespace supple::diagnostics::detail
