// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <filesystem>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// afunix.h depends on Winsock's ADDRESS_FAMILY definition.
#include <afunix.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace supple::rpc::net {
inline std::string pathName(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
    return path.string();
#endif
}
#if defined(_WIN32)
using Socket = SOCKET;
using PollFd = WSAPOLLFD;
inline constexpr Socket invalid = INVALID_SOCKET;
inline int failed() {
    switch (WSAGetLastError()) {
        case WSAEINTR:
            errno = EINTR;
            break;
        case WSAEWOULDBLOCK:
            errno = EAGAIN;
            break;
        case WSAECONNREFUSED:
            errno = ECONNREFUSED;
            break;
        case WSAEADDRINUSE:
            errno = EADDRINUSE;
            break;
        case WSAEACCES:
            errno = EACCES;
            break;
        case WSAEINVAL:
            errno = EINVAL;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
}
inline bool initialized() {
    struct Winsock {
        bool active;
        Winsock() {
            WSADATA data{};
            active = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }
        ~Winsock() {
            if (active) WSACleanup();
        }
    };
    static Winsock winsock;
    if (!winsock.active) errno = EIO;
    return winsock.active;
}
inline void close(Socket fd) { closesocket(fd); }
inline int nonblocking(Socket fd) {
    u_long one = 1;
    return ioctlsocket(fd, FIONBIO, &one) == 0 ? 0 : failed();
}
inline Socket open() {
    if (!initialized()) return invalid;
    auto fd = WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (fd == invalid) failed();
    return fd;
}
inline Socket accept(Socket fd) {
    auto peer = ::accept(fd, nullptr, nullptr);
    if (peer == invalid) {
        failed();
        return invalid;
    }
    SetHandleInformation(reinterpret_cast<HANDLE>(peer), HANDLE_FLAG_INHERIT, 0);
    if (nonblocking(peer) != 0) {
        close(peer);
        return invalid;
    }
    return peer;
}
inline int poll(PollFd* fds, std::size_t count, int timeout) {
    const int result = WSAPoll(fds, static_cast<ULONG>(count), timeout);
    return result == SOCKET_ERROR ? failed() : result;
}
inline int send(Socket fd, const char* data, std::size_t size) {
    const int result = ::send(fd, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
    return result == SOCKET_ERROR ? failed() : result;
}
inline int recv(Socket fd, char* data, std::size_t size) {
    const int result = ::recv(fd, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
    return result == SOCKET_ERROR ? failed() : result;
}
inline int peek(Socket fd) {
    char byte;
    const int result = ::recv(fd, &byte, 1, MSG_PEEK);
    return result == SOCKET_ERROR ? failed() : result;
}
// On a nonblocking socket a connect still under way fails with EINPROGRESS:
// poll for POLLOUT, then connectError() says how it ended.
inline int connect(Socket fd, const sockaddr_un& address) {
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return 0;
    switch (WSAGetLastError()) {
        case WSAEWOULDBLOCK:
        case WSAEALREADY:
            errno = EINPROGRESS;
            return -1;
        case WSAEISCONN:
            errno = EISCONN;
            return -1;
        default:
            return failed();
    }
}
inline int connectError(Socket fd) {
    int error = 0;
    int size = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0)
        return failed();
    if (error == 0) return 0;
    WSASetLastError(error);
    return failed();
}
inline int bind(Socket fd, const sockaddr_un& address) {
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 ? 0
                                                                                         : failed();
}
inline int listen(Socket fd) { return ::listen(fd, 16) == 0 ? 0 : failed(); }
#else
using Socket = int;
using PollFd = pollfd;
inline constexpr Socket invalid = -1;
inline void close(Socket fd) { ::close(fd); }
inline int nonblocking(Socket fd) { return ::fcntl(fd, F_SETFL, O_NONBLOCK); }
inline Socket open() {
#if defined(__linux__)
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return invalid;
    const int one = 1;
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return invalid;
    }
    return fd;
#endif
}
inline Socket accept(Socket fd) {
#if defined(__linux__)
    return ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const auto peer = ::accept(fd, nullptr, nullptr);
    if (peer < 0) return invalid;
    const int one = 1;
    if (::fcntl(peer, F_SETFD, FD_CLOEXEC) != 0 || nonblocking(peer) != 0 ||
        ::setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
        const int error = errno;
        close(peer);
        errno = error;
        return invalid;
    }
    return peer;
#endif
}
inline int poll(PollFd* fds, std::size_t count, int timeout) { return ::poll(fds, count, timeout); }
inline auto send(Socket fd, const char* data, std::size_t size) {
#if defined(__linux__)
    return ::send(fd, data, size, MSG_NOSIGNAL);
#else
    return ::send(fd, data, size, 0);
#endif
}
inline auto recv(Socket fd, char* data, std::size_t size) { return ::recv(fd, data, size, 0); }
inline auto peek(Socket fd) {
    char byte;
    return ::recv(fd, &byte, 1, MSG_PEEK);
}
// On a nonblocking socket a connect still under way fails with EINPROGRESS:
// poll for POLLOUT, then connectError() says how it ended. Linux fails with
// EAGAIN instead while the listener's backlog is full: try again.
inline int connect(Socket fd, const sockaddr_un& address) {
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
inline int connectError(Socket fd) {
    int error = 0;
    socklen_t size = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) return -1;
    if (error == 0) return 0;
    errno = error;
    return -1;
}
inline int bind(Socket fd, const sockaddr_un& address) {
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
inline int listen(Socket fd) { return ::listen(fd, 16); }
#endif
}  // namespace supple::rpc::net
