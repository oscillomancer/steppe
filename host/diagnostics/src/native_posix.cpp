// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
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
#include <ctime>
#include <utility>
#include "inventory.h"
#include "supple/diagcap/capsule.h"
#include "supple/diagcap/native.h"
#include "supple/diagcap/nonblocking.h"
#if defined(__linux__)
#include <sys/ptrace.h>
#endif
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
// The supervisor's end of the channel, for its fatal handlers (ADR-0091); -1 when none.
volatile sig_atomic_t noticeChannel = -1;
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
// The supervisor's fatal handler (ADR-0091): one byte, "collector failed", to the worker, then
// the death the signal would have caused. Everything here is async-signal-safe. SA_RESETHAND
// has put the default action back, so the raise is the signal's own end.
void collectorFailed(int signal, siginfo_t*, void*) {
    const int fd = noticeChannel;
    const char notice = 'F';
    if (fd >= 0) (void)::send(fd, &notice, 1, noSignal() | MSG_DONTWAIT);
    sigset_t one;
    (void)sigemptyset(&one);
    (void)sigaddset(&one, signal);
    (void)::pthread_sigmask(SIG_UNBLOCK, &one, nullptr);
    (void)::raise(signal);
    ::_exit(128 + signal);  // only if the re-raise did not end the process
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
    (void)sigemptyset(&forward.sa_mask);
    (void)::sigaction(SIGINT, &forward, nullptr);
    (void)::sigaction(SIGTERM, &forward, nullptr);
    Native native{pair[0], child};
    announceCollectorFailure(native);
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

// Observes the worker's exit: waitpid on the worker, with `flags` (WNOHANG or 0).
// Once a trace has left a thread of the worker attached, the worker's leader is not
// reported until that thread is reaped, and only its tracer can reap it. Everything
// the supervisor can wait for is then drained, as the worker is its only child: a
// dead thread is discarded and a stopped one is detached, handing back a signal-
// delivery-stop signal (never swallowing a real one) as the collector's release does.
namespace {
pid_t observe(Native& native, int flags) {
    pid_t result;
#if defined(__linux__)
    if (native.traced) {
        const auto worker = static_cast<pid_t>(native.process);
        for (;;) {
            int status = 0;
            result = ::waitpid(-1, &status, __WALL | flags);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) return result;
            if (WIFSTOPPED(status)) {
                const int signal = (status >> 16) == 0 ? WSTOPSIG(status) : 0;
                (void)::ptrace(PTRACE_DETACH, result, nullptr,
                               reinterpret_cast<void*>(std::intptr_t{signal}));
                continue;
            }
            if (result == worker) {
                native.status = status;
                return result;
            }
        }
    }
#endif
    do {
        result = ::waitpid(static_cast<pid_t>(native.process), &native.status, flags);
    } while (result < 0 && errno == EINTR);
    return result;
}
}  // namespace
namespace {
// What a read of the channel may have brought beside its bytes (topic 6c): descriptors sent as
// SCM_RIGHTS. The first of the packet's first read is the packet's resource; every other one is
// refused here (closed and counted), as is control data of any other kind. Everything is closed
// that the caller does not take, on every path.
struct Received {
    int descriptor = -1;
    ~Received() {
        if (descriptor >= 0) ::close(descriptor);
    }
    int take() { return std::exchange(descriptor, -1); }
};
void harvest(Native& native, msghdr& message, std::size_t at, Received& held) {
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            ++native.refusedResources;
            continue;
        }
        const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; ++i) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fd < 0) continue;
#if defined(__APPLE__)
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
                ::close(fd);
                ++native.refusedResources;
                continue;
            }
#endif
            // One descriptor, with the packet's first byte: anything else is damage.
            if (at == 0 && held.descriptor < 0) {
                held.descriptor = fd;
            } else {
                ::close(fd);
                ++native.refusedResources;
            }
        }
    }
    if (message.msg_flags & MSG_CTRUNC) ++native.refusedResources;  // the kernel closed the rest
}
}  // namespace
bool receive(Native& native, Packet& packet, std::intptr_t* resource) {
    if (resource) *resource = -1;
    auto* bytes = reinterpret_cast<char*>(&packet);
    std::size_t at = 0;
    const int fd = static_cast<int>(native.channel);
    Received held;
    for (;;) {
        if (!native.exited) {
            const pid_t result = observe(native, WNOHANG);
            native.statusKnown = result > 0;
            native.exited = native.statusKnown || (result < 0 && errno == ECHILD);
        }
        // Drain complete packets already sent before exit, even if a forked
        // helper retains the endpoint. Never block on a partial packet/EOF.
        // The first read of a packet takes one byte: a descriptor is delivered with the byte it
        // is attached to, and a read that spans several sends cannot tell where in them it was.
        // With this, a descriptor that arrives on that read was attached to the packet's first
        // byte, and one that comes later was not.
        iovec part{bytes + at, at == 0 ? std::size_t{1} : sizeof(packet) - at};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)];
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        const auto n = ::recvmsg(fd, &message,
#if defined(MSG_CMSG_CLOEXEC)
                                 MSG_CMSG_CLOEXEC |
#endif
                                     MSG_DONTWAIT);
        if (n > 0) {
            harvest(native, message, at, held);
            at += static_cast<std::size_t>(n);
            if (at == sizeof(packet)) {
                if (!valid(packet)) return false;
                if (held.descriptor >= 0) {
                    if (resource && mayCarryResource(packet))
                        *resource = held.take();
                    else
                        ++native.refusedResources;  // closed with `held`
                }
                return true;
            }
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (native.exited) return false;
        pollfd item{fd, POLLIN, 0};
        // A bounded idle park observes process exit without installing a
        // SIGCHLD hook or relying on EOF from every inherited descriptor.
        if (::poll(&item, 1, 100) < 0 && errno != EINTR) return false;
    }
}
bool publish(Native& native, const Packet& packet) noexcept {
    return publishWithResource(native, packet, -1);
}
bool publishWithResource(Native& native, const Packet& packet, std::intptr_t resource) noexcept {
    const int fd = static_cast<int>(native.channel);
    const auto* bytes = reinterpret_cast<const char*>(&packet);
    // The announcing thread must never wait on a full channel, so the descriptor is made
    // non-blocking for the call (macOS decides from the socket's own state, not the flag).
    const NonBlocking guard(fd);
    iovec whole{const_cast<char*>(bytes), sizeof(packet)};
    msghdr message{};
    message.msg_iov = &whole;
    message.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    if (resource >= 0) {
        std::memset(control, 0, sizeof control);
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        const int descriptor = static_cast<int>(resource);
        std::memcpy(CMSG_DATA(header), &descriptor, sizeof(int));
    }
    ssize_t sent = ::sendmsg(fd, &message, noSignal() | MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(sizeof(packet))) return true;
    if (sent <= 0) return false;  // nothing went out: the stream is as it was
    // A stream socket may take part of a message when its buffer is nearly full (macOS does;
    // Linux's unix sockets take a message this small whole or not at all): the rest goes out now,
    // within a bound (finishPartSend). The descriptor went with the first bytes.
    return finishPartSend(native, packet, static_cast<std::size_t>(sent));
}
bool finishPartSend(Native& native, const Packet& packet, std::size_t sent) noexcept {
    const int fd = static_cast<int>(native.channel);
    const auto* bytes = reinterpret_cast<const char*>(&packet);
    const NonBlocking guard(fd);
    // The `sent` bytes already out cannot be taken back, and the supervisor reads fixed-size
    // packets, so the rest goes out now: the wait is bounded (the supervisor is draining, or a
    // terminal hold is on and nothing else is being sent) and uses only async-signal-safe calls. If
    // it cannot be finished the stream is torn, the supervisor's next packet fails its checks and
    // it stops reading, as it does for any damaged packet. The bound is time: kPartSendPolls ms
    // from here by the monotonic clock, or the deadline closeInventory published
    // (partSendCutoffNs), whichever is earlier: that is what the close waits out (a count of 1 ms
    // polls would take longer under load or timer slack, and the terminal packet could then land
    // inside this one). Only without a clock does it count polls.
    std::size_t at = sent;
    timespec begin{};
    const bool clocked = ::clock_gettime(CLOCK_MONOTONIC, &begin) == 0;
    for (int attempts = 0; at < sizeof(packet); ++attempts) {
        if (clocked) {
            timespec now{};
            if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
            const long long elapsed =
                (now.tv_sec - begin.tv_sec) * 1'000'000'000LL + (now.tv_nsec - begin.tv_nsec);
            if (elapsed >= static_cast<long long>(kPartSendPolls) * 1'000'000LL) break;
            const std::uint64_t cutoff = partSendCutoffNs();
            if (cutoff != 0 && static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
                                       static_cast<std::uint64_t>(now.tv_nsec) >=
                                   cutoff)
                break;
        } else if (attempts >= kPartSendPolls) {
            break;
        }
        pollfd item{fd, POLLOUT, 0};
        const int ready = ::poll(&item, 1, 1);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        const ssize_t more = ::send(fd, bytes + at, sizeof(packet) - at, noSignal() | MSG_DONTWAIT);
        if (more > 0)
            at += static_cast<std::size_t>(more);
        else if (more < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
    }
    if (at == sizeof(packet)) return true;
    native.torn = true;  // the part sent stays in the stream: nothing more may follow it
    return false;
}
void acknowledge(Native& native) noexcept {
    const char ack = 'A';
    (void)::send(static_cast<int>(native.channel), &ack, 1, noSignal() | MSG_DONTWAIT);
}
void awaitAcknowledgement(Native& native) noexcept {
    awaitAcknowledgementFor(native, 1'000'000'000ULL);
}
void awaitAcknowledgementFor(Native& native, std::uint64_t nanoseconds) noexcept {
    const int fd = static_cast<int>(native.channel);
    if (fd < 0 || fd >= FD_SETSIZE) return;
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(fd, &ready);
    // The wait is `nanoseconds` in total, a second at most in the terminal hold. A ptrace
    // interrupt from the collector (the worker may be the very thread it stops) or any
    // signal ends pselect early with EINTR; the remainder is waited out rather than
    // letting the worker die mid-collection. clock_gettime and pselect are
    // async-signal-safe.
    timespec start{};
    if (::clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        // No clock: wait the stated time once, not a fixed second (macOS passes the
        // remainder of its hold here).
        const timespec timeout{static_cast<time_t>(nanoseconds / 1'000'000'000ULL),
                               static_cast<long>(nanoseconds % 1'000'000'000ULL)};
        (void)::pselect(fd + 1, &ready, nullptr, nullptr, &timeout, nullptr);
        return;
    }
    const long long total = static_cast<long long>(nanoseconds);
    for (;;) {
        timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
        const long long left =
            total - ((now.tv_sec - start.tv_sec) * 1'000'000'000LL + (now.tv_nsec - start.tv_nsec));
        if (left <= 0) return;
        const timespec timeout{static_cast<time_t>(left / 1'000'000'000LL),
                               static_cast<long>(left % 1'000'000'000LL)};
        fd_set again = ready;
        if (::pselect(fd + 1, &again, nullptr, nullptr, &timeout, nullptr) >= 0 || errno != EINTR)
            return;
    }
}
int wait(Native& native, bool& fatal, std::optional<std::uint32_t> expected) {
    if (!native.exited) {
        native.statusKnown = observe(native, 0) > 0;
        native.exited = true;
    }
    fatal = native.statusKnown && WIFSIGNALED(native.status);
    return !native.statusKnown ? static_cast<int>(expected.value_or(1))
           : fatal             ? 128 + WTERMSIG(native.status)
                               : WEXITSTATUS(native.status);
}
void close(Native& native) {
    // The supervisor is done with the worker: a late fatal signal has no one to tell, and the
    // descriptor number may be someone else's by then.
    if (noticeChannel == native.channel) noticeChannel = -1;
    if (native.channel >= 0) ::close(static_cast<int>(native.channel));
    native.channel = -1;
}
void announceCollectorFailure(const Native& native) noexcept {
    // The handlers run on an alternate stack, so the notice goes out even when the supervisor's
    // own stack is what ran out. A stack the process already enabled stays.
    static char alternate[64 * 1024] __attribute__((aligned(16)));
    stack_t current{};
    if (::sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE)) {
        stack_t replacement{};
        replacement.ss_sp = alternate;
        replacement.ss_size = sizeof(alternate);
        (void)::sigaltstack(&replacement, nullptr);
    }
    noticeChannel = static_cast<int>(native.channel);
    // The worker's own set. A signal someone else already handles (a sanitizer) stays theirs.
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        struct sigaction prior{}, action{};
        if (::sigaction(signal, nullptr, &prior) != 0 || prior.sa_handler != SIG_DFL) continue;
        action.sa_sigaction = collectorFailed;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        (void)sigemptyset(&action.sa_mask);
        (void)::sigaction(signal, &action, nullptr);
    }
}
SupervisorEnd awaitSupervisorEnd(const Native& native) noexcept {
    const int fd = static_cast<int>(native.channel);
    if (fd < 0) return SupervisorEnd::Unknown;
    for (;;) {
        pollfd item{fd, POLLIN, 0};
        if (::poll(&item, 1, -1) < 0) {
            if (errno == EINTR) continue;
            return SupervisorEnd::Unknown;
        }
        if (item.revents & POLLNVAL) return SupervisorEnd::Unknown;
        // A peek, with a per-call flag: the byte stays where the terminal handler's wait can
        // see it, and the descriptor's blocking mode (which a capsule transfer changes on
        // this same open file description) decides nothing.
        char byte = 0;
        const ssize_t n = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) return byte == 'F' ? SupervisorEnd::Collector : SupervisorEnd::Unknown;
        if (n == 0) return SupervisorEnd::Launcher;
        if (errno == EINTR) continue;
        // A supervisor killed with packets of ours unread leaves a reset, not a clean end.
        if (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE)
            return SupervisorEnd::Launcher;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Readable but nothing to read, again and again, would be a spin; hangup or
            // error with no way to tell which is not a reason to stop the program.
            if (item.revents & (POLLHUP | POLLERR)) return SupervisorEnd::Unknown;
            continue;
        }
        return SupervisorEnd::Unknown;
    }
}
void requestStop() noexcept { (void)::kill(::getpid(), SIGTERM); }
void hardExit() noexcept { ::_exit(128 + SIGTERM); }
void closeProtocolStdio() {
    ::close(STDIN_FILENO);
    ::close(STDOUT_FILENO);
}
}  // namespace supple::diagnostics::detail
