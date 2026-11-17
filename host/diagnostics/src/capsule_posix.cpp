// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <cerrno>
#include <ctime>
#include "capsule.h"
// Everything the worker runs here is reachable from its terminal handler: fixed storage,
// system calls and arithmetic only.
namespace supple::diagnostics::detail {
namespace {
constexpr std::uint64_t kMaxStep = std::uint64_t{1} << 30;  // as the supervisor-side walker

int noSignal() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
// Puts a descriptor in non-blocking mode for the duration of a send or receive and puts
// the flags back. MSG_DONTWAIT alone is not enough everywhere: where the kernel looks at
// the socket's own non-blocking state to decide whether to wait for buffer space, a
// full socket would block the caller past its deadline, and the caller is a crash
// handler. fcntl is async-signal-safe. Failure to set the flag is not fatal: the
// per-call flag still applies where it works.
class NonBlocking {
  public:
    explicit NonBlocking(int fd) noexcept : fd_(fd) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags >= 0 && !(flags & O_NONBLOCK) && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0)
            restore_ = flags;
    }
    ~NonBlocking() {
        if (restore_ >= 0) (void)::fcntl(fd_, F_SETFL, restore_);
    }
    NonBlocking(const NonBlocking&) = delete;
    NonBlocking& operator=(const NonBlocking&) = delete;

  private:
    int fd_;
    int restore_ = -1;  // the flags to put back; -1 when nothing was changed
};
// Waits until `fd` is ready for `events`, at most until `deadline`, which must be a real
// time on the monotonic clock. False at the deadline or on a hard error.
bool waitFor(int fd, short events, std::uint64_t deadline) noexcept {
    for (;;) {
        const std::uint64_t now = monotonicNanoseconds();
        if (deadline == 0 || now >= deadline) return false;
        const std::uint64_t left = (deadline - now + 999999) / 1000000;
        pollfd item{fd, events, 0};
        const int result = ::poll(&item, 1, left > 100 ? 100 : static_cast<int>(left));
        if (result > 0) return true;
        if (result < 0 && errno != EINTR) return false;
    }
}
bool sendAll(int fd, const void* data, std::size_t bytes, std::uint64_t deadline) noexcept {
    const auto* at = static_cast<const char*>(data);
    while (bytes > 0) {
        const ssize_t sent = ::send(fd, at, bytes, noSignal() | MSG_DONTWAIT);
        if (sent > 0) {
            at += sent;
            bytes -= static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (!waitFor(fd, POLLOUT, deadline)) return false;
    }
    return true;
}
bool receiveAll(int fd, void* data, std::size_t bytes, std::uint64_t deadline) {
    auto* at = static_cast<char*>(data);
    while (bytes > 0) {
        const ssize_t got = ::recv(fd, at, bytes, MSG_DONTWAIT);
        if (got > 0) {
            at += got;
            bytes -= static_cast<std::size_t>(got);
            continue;
        }
        if (got == 0) return false;  // the worker closed the channel
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (!waitFor(fd, POLLIN, deadline)) return false;
    }
    return true;
}
std::size_t usedBytes(const CapsuleData& data) noexcept {
    std::size_t total = sizeof(CapsuleHeader) + data.header.moduleCount * sizeof(CapsuleModule);
    for (std::uint32_t i = 0; i < data.header.threadCount; ++i)
        total += sizeof(CapsuleThread) + data.threads[i].head.frameCount * sizeof(std::uint64_t);
    return total + data.header.commandCount * sizeof(CapsuleCommand);
}
}  // namespace

std::uint64_t monotonicNanoseconds() noexcept {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

void walkStack(const StackReader& reader, const CapsuleRegs& regs, bool arm64,
               CapsuleThreadData& thread, std::uint64_t deadline) noexcept {
    std::uint32_t count = 0;
    thread.frames[count++] = regs.pc;
    thread.head.stop = 0;  // Stop::End
    std::uint64_t fp = regs.fp, previous = 0;
    bool first = true;
    for (;;) {
        if (deadline != 0 && (count & 15) == 0 && monotonicNanoseconds() >= deadline) {
            thread.head.stop = 6;  // Stop::Deadline
            break;
        }
        if (fp == 0) {
            thread.head.stop = 0;
            break;
        }
        if (count >= kCapsuleFrames) {
            thread.head.stop = 1;  // Stop::Depth
            break;
        }
        if ((fp & 7) != 0 || (first && regs.sp != 0 && fp < regs.sp)) {
            thread.head.stop = 4;  // Stop::BadFramePointer
            break;
        }
        if (!first && fp <= previous) {
            thread.head.stop = 5;  // Stop::Cycle
            break;
        }
        if (!first && fp - previous > kMaxStep) {
            thread.head.stop = 4;
            break;
        }
        std::uint64_t record[2] = {0, 0};
        if (!reader.read || !reader.read(reader.context, fp, record, sizeof(record))) {
            thread.head.stop = 2;  // Stop::Unreadable
            break;
        }
        std::uint64_t ret = record[1];
        if (arm64) ret &= 0x0000ffffffffffffULL;  // strip pointer-auth bits
        if (ret == 0) {
            thread.head.stop = 0;
            break;
        }
        thread.frames[count++] = ret;
        previous = fp;
        fp = record[0];
        first = false;
    }
    thread.head.frameCount = count;
}

bool sendCapsule(int fd, const Packet& base, const CapsuleData* data,
                 std::uint64_t deadline) noexcept {
    const NonBlocking nonBlocking(fd);
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    Packet packet = base;
    packet.kind = Message::Trace;
    packet.code = data ? static_cast<std::uint32_t>(usedBytes(*data)) : 0;
    if (!sendAll(fd, &packet, sizeof(packet), deadline)) return false;
    if (!data) return true;
    if (!sendAll(fd, &data->header, sizeof(CapsuleHeader), deadline)) return false;
    if (data->header.moduleCount > 0 &&
        !sendAll(fd, data->modules, data->header.moduleCount * sizeof(CapsuleModule), deadline))
        return false;
    for (std::uint32_t i = 0; i < data->header.threadCount; ++i) {
        const auto& thread = data->threads[i];
        if (!sendAll(fd, &thread.head, sizeof(CapsuleThread), deadline)) return false;
        if (thread.head.frameCount > 0 &&
            !sendAll(fd, thread.frames, thread.head.frameCount * sizeof(std::uint64_t), deadline))
            return false;
    }
    if (data->header.commandCount > 0 &&
        !sendAll(fd, data->commands, data->header.commandCount * sizeof(CapsuleCommand), deadline))
        return false;
    return true;
}

std::optional<std::vector<std::uint8_t>> receiveCapsule(int fd, const Packet& terminal,
                                                        std::uint64_t deadline) {
    const NonBlocking nonBlocking(fd);
    Packet packet;
    if (!receiveAll(fd, &packet, sizeof(packet), deadline)) return std::nullopt;
    if (packet.magic != terminal.magic || packet.version != terminal.version ||
        packet.kind != Message::Trace || packet.incident != terminal.incident ||
        packet.role != terminal.role || packet.code == 0 || packet.code > kCapsuleMaxBytes)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(packet.code);
    if (!receiveAll(fd, bytes.data(), bytes.size(), deadline)) return std::nullopt;
    return bytes;
}
}  // namespace supple::diagnostics::detail
