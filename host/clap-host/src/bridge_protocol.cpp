// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "bridge_protocol.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <limits>

namespace supple::host::bridge {

namespace {

#if !defined(_WIN32)
bool ready(int socket, short events, Deadline deadline) {
    for (;;) {
        int timeout = -1;
        if (deadline != Deadline::max()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= Deadline::duration::zero()) return false;
            const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            timeout = static_cast<int>(std::min<std::int64_t>(ms, INT32_MAX));
        }
        pollfd pfd{socket, events, 0};
        const int result = ::poll(&pfd, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        return result > 0 && (pfd.revents & events);
    }
}
bool writeAll(int s, const void* data, std::size_t n, int fd, Deadline deadline) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bool fdPending = fd >= 0;
    while (n > 0) {
        if (!ready(s, POLLOUT, deadline)) return false;
        iovec iov{const_cast<std::uint8_t*>(p), n};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        if (fdPending) {
            std::memset(control, 0, sizeof control);
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
        }
        const ssize_t w = ::sendmsg(s, &msg,
#if defined(__linux__)
                                    MSG_NOSIGNAL | MSG_DONTWAIT
#else
                                    MSG_DONTWAIT
#endif
        );
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (w == 0) return false;
        fdPending = false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

bool readAll(int s, void* data, std::size_t n, int* fd, Deadline deadline, int* descriptorCount) {
    auto* p = static_cast<std::uint8_t*>(data);
    while (n > 0) {
        if (!ready(s, POLLIN, deadline)) return false;
        iovec iov{p, n};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)];
        if (fd) {
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
        }
        const ssize_t r = ::recvmsg(s, &msg,
#if defined(__linux__)
                                    MSG_CMSG_CLOEXEC | MSG_DONTWAIT
#else
                                    MSG_DONTWAIT
#endif
        );
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (r == 0) return false;  // the peer is gone
        if (fd) {
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (std::size_t i = 0; i < count; ++i) {
                    if (descriptorCount) ++*descriptorCount;
                    int received = -1;
                    std::memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
#if defined(__APPLE__)
                    if (::fcntl(received, F_SETFD, FD_CLOEXEC) != 0) {
                        ::close(received);
                        return false;
                    }
#endif
                    if (*fd < 0)
                        *fd = received;
                    else
                        ::close(received);
                }
            }
            if (msg.msg_flags & MSG_CTRUNC) return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

#endif
}  // namespace

bool sendFrame(Control socket, std::uint32_t head, std::uint32_t instance,
               const std::vector<std::uint8_t>& payload, Resource fd, Deadline deadline) {
    if (payload.size() > kMaxMessage) return false;
    std::uint32_t prefix[3] = {static_cast<std::uint32_t>(payload.size()), head, instance};
#if defined(_WIN32)
    const auto attachment = static_cast<std::uint64_t>(fd);
    return socket && socket->write(prefix, sizeof(prefix), deadline) &&
           socket->write(&attachment, sizeof(attachment), deadline) &&
           (payload.empty() || socket->write(payload.data(), payload.size(), deadline));
#else
    return writeAll(socket, prefix, sizeof prefix, fd, deadline) &&
           (payload.empty() || writeAll(socket, payload.data(), payload.size(), -1, deadline));
#endif
}

bool recvFrame(Control socket, std::uint32_t& head, std::uint32_t& instance,
               std::vector<std::uint8_t>& payload, Resource* fd, int timeoutMs,
               int* descriptorCount) {
    return recvFrameUntil(socket, head, instance, payload, fd, deadlineAfter(timeoutMs),
                          descriptorCount);
}

bool recvFrameUntil(Control socket, std::uint32_t& head, std::uint32_t& instance,
                    std::vector<std::uint8_t>& payload, Resource* fd, Deadline deadline,
                    int* descriptorCount) {
    if (fd) *fd = -1;
    if (descriptorCount) *descriptorCount = 0;
    auto fail = [&] {
        if (fd && *fd >= 0) {
            closeResource(*fd);
            *fd = -1;
        }
        return false;
    };
    std::uint32_t prefix[3] = {0, 0, 0};
#if defined(_WIN32)
    std::uint64_t attachment = static_cast<std::uint64_t>(noResource);
    if (!socket || !socket->read(prefix, sizeof(prefix), deadline) ||
        !socket->read(&attachment, sizeof(attachment), deadline))
        return fail();
    const auto resource = static_cast<Resource>(attachment);
    if (resource != noResource) {
        // The bridge cannot duplicate into the parent. A reply containing a
        // numeric handle is a protocol violation, never authority to close a
        // parent handle chosen by untrusted plugin code.
        if (!socket->acceptsResources()) {
            socket->rejectResource();
            return false;
        }
        if (descriptorCount) *descriptorCount = 1;
        if (fd)
            *fd = resource;
        else
            closeResource(resource);
    }
#else
    if (!readAll(socket, prefix, sizeof prefix, fd, deadline, descriptorCount)) return fail();
#endif
    if (prefix[0] > kMaxMessage) return fail();
    head = prefix[1];
    instance = prefix[2];
    payload.resize(prefix[0]);
#if defined(_WIN32)
    if (prefix[0] && !socket->read(payload.data(), payload.size(), deadline)) return fail();
#else
    if (prefix[0] &&
        !readAll(socket, payload.data(), payload.size(), fd, deadline, descriptorCount))
        return fail();
#endif
    return true;
}

void writeOutputEvent(Writer& w, const OutputEvent& e) {
    w.u8(static_cast<std::uint8_t>(e.kind));
    w.u32(e.time);
    w.u32(e.paramId);
    w.f64(e.value);
    w.i32(e.noteId);
    w.u32(e.flags);
    w.i16(e.port);
    w.i16(e.channel);
    w.i16(e.key);
}

OutputEvent readOutputEvent(Reader& r) {
    OutputEvent e;
    e.kind = static_cast<OutputEvent::Kind>(r.u8());
    e.time = r.u32();
    e.paramId = r.u32();
    e.value = r.f64();
    e.noteId = r.i32();
    e.flags = r.u32();
    e.port = r.i16();
    e.channel = r.i16();
    e.key = r.i16();
    return e;
}

bool validRequest(Op op, Reader r) {
    switch (op) {
        case Op::Create:
            for (int i = 0; i < 7; ++i) r.skipString();
            break;
        case Op::SubscribeNotifications:
            r.skip(4);
            break;
        case Op::AckNotifications:
            break;
        case Op::Activate:
            r.skip(20);
#if defined(_WIN32)
            r.skip(16);  // duplicated request and done events
#endif
            break;
        case Op::ParamValue:
            r.skip(4);
            break;
        case Op::ParamText:
        case Op::SetParam:
        case Op::NormalizedParam:
        case Op::PlainParam:
            r.skip(12);
            break;
        case Op::ParamFromText:
            r.skip(4);
            r.skipString();
            break;
        case Op::GetPatch:
        case Op::Describe:
            r.skip(1);
            break;
        case Op::SetPatch:
            r.skipString();
            r.skip(4);
            break;
        case Op::LoadState:
            r.skipString();
            break;
        case Op::EditorOpen: {
            const auto kind = r.u8();
            if (kind > static_cast<std::uint8_t>(NativeWindow::Kind::Floating)) return false;
            const auto handle = r.u64();
            if (kind == static_cast<std::uint8_t>(NativeWindow::Kind::Floating) &&
                handle > std::numeric_limits<std::uint32_t>::max())
                return false;
            r.skip(8);
            (void)readFrame(r);
            break;
        }
        case Op::EditorResize:
        case Op::EditorScale:
            r.skip(8);
            break;
        case Op::Deactivate:
        case Op::Params:
        case Op::Latency:
        case Op::SemanticSchema:
        case Op::PluginStatus:
        case Op::FactoryPatches:
        case Op::SaveState:
        case Op::Service:
        case Op::Destroy:
        case Op::EditorClose:
        case Op::EditorRequests:
        case Op::TakeIdleOutputs:
            break;
        default:
            return false;
    }
    return r.done();
}

bool validReply(Op op, Reader r) {
    switch (op) {
        case Op::Create: {
            if (!r.u32()) return false;
            r.skip(16);
            const auto ports = r.u32();  // auxiliary outputs: name, channels
            if (ports > kMaxAuxPorts) return false;
            for (std::uint32_t i = 0; i < ports && r.ok(); ++i) {
                r.skipString();
                r.skip(4);
            }
            break;
        }
        case Op::Activate: {  // the settled widths: in, out, key, auxiliary outputs
            r.skip(12);
            const auto ports = r.u32();
            if (ports > kMaxAuxPorts) return false;
            for (std::uint32_t i = 0; i < ports && r.ok(); ++i) {
                r.skipString();
                r.skip(4);
            }
            break;
        }
        case Op::Params: {
            const auto count = r.u32();
            for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
                r.skip(4);
                r.skipString();
                r.skipString();
                r.skip(28);
            }
            break;
        }
        case Op::ParamValue:
        case Op::ParamFromText:
        case Op::NormalizedParam:
        case Op::PlainParam:
            r.skip(8);
            break;
        case Op::ParamText:
        case Op::SemanticSchema:
        case Op::PluginStatus:
        case Op::FactoryPatches:
        case Op::GetPatch:
        case Op::Describe:
        case Op::SaveState:
            r.skipString();
            break;
        case Op::Latency:
            r.skip(9);
            break;
        case Op::Service: {
            r.skip(10);                    // work/flags and paramsRescanFlags
            if (r.u8() > 1) return false;  // consumed request_process
            const auto clears = r.u32();
            if (clears > kMaxParamClears) return false;
            r.skip(static_cast<std::size_t>(clears) * 8);
            r.skip(5);  // droppedEvents, needsAudioFlush
            const auto lines = r.u32();
            for (std::uint32_t i = 0; i < lines && r.ok(); ++i) r.skipString();
            r.skip(10);  // latency, tail, reportsTail, restartPending
            if (r.u8()) (void)r.error();
            break;
        }
        case Op::EditorOpen:
        case Op::EditorResize:
        case Op::EditorScale:
            r.skip(9);
            break;
        case Op::EditorRequests:
            r.skip(12);
            (void)readFrame(r);
            break;
        case Op::EditorClose:
            (void)readFrame(r);
            break;
        case Op::TakeIdleOutputs: {
            (void)r.u64();
            const auto count = r.u32();
            if (count > Instance::IdleOutputs::kCapacity) return false;
            for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
                const auto e = readOutputEvent(r);
                if (e.kind > OutputEvent::Kind::NoteEnd) return false;
            }
            break;
        }
        case Op::SubscribeNotifications:
            if (r.u32() != kVersion) return false;
            break;
        case Op::AckNotifications:
        case Op::Deactivate:
        case Op::SetParam:
        case Op::SetPatch:
        case Op::LoadState:
        case Op::Destroy:
            break;
        default:
            return false;
    }
    return r.done();
}

}  // namespace supple::host::bridge
