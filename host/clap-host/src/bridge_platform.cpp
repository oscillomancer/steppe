// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "bridge_platform.h"
#include <supple/product.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>
#include <string>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#endif

namespace supple::host::bridge {
namespace {
#if !defined(__linux__)
std::string uniqueName() {
    std::random_device random;
    // Darwin shm names have a 31-byte limit. Keep a product-derived prefix
    // and 132 random bits in 22 filename-safe characters (at most 30 bytes
    // including shm_open's leading slash).
    std::string name(std::string_view(product::kSlug).substr(0, 6));
    name += '-';
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 22; ++i) name += alphabet[random() & 63];
    return name;
}
#endif
#if defined(_WIN32)
bool complete(HANDLE pipe, OVERLAPPED& op, std::chrono::steady_clock::time_point deadline,
              DWORD& count) {
    DWORD timeout = INFINITE;
    if (deadline != std::chrono::steady_clock::time_point::max()) {
        const auto now = std::chrono::steady_clock::now();
        timeout = now >= deadline
                      ? 0
                      : static_cast<DWORD>(std::min<std::int64_t>(
                            std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count(),
                            INFINITE - 1));
    }
    if (WaitForSingleObject(op.hEvent, timeout) == WAIT_OBJECT_0)
        return GetOverlappedResult(pipe, &op, &count, FALSE) != 0;
    // Drain cancellation before the OVERLAPPED and its buffer can be destroyed.
    CancelIoEx(pipe, &op);
    GetOverlappedResult(pipe, &op, &count, TRUE);
    return false;
}
#endif
}  // namespace
void closeResource(Resource resource) {
    if (resource == noResource) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(resource));
#else
    ::close(resource);
#endif
}
Resource createMemory(std::size_t size) {
#if defined(_WIN32)
    const auto bytes = static_cast<std::uint64_t>(size);
    const auto map =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                           static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    return map ? reinterpret_cast<Resource>(map) : noResource;
#else
#if defined(__linux__)
    const int fd = ::memfd_create("supple-bridge", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
    const auto name = "/" + uniqueName();
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
        // The descriptor sent with SCM_RIGHTS retains the object after unlink.
        ::shm_unlink(name.c_str());
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
            ::close(fd);
            return noResource;
        }
    }
#endif
    if (fd < 0) return noResource;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return noResource;
    }
#if defined(__linux__)
    if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        ::close(fd);
        return noResource;
    }
#endif
    return fd;
#endif
}
void* mapMemory(Resource resource, std::size_t size) {
#if defined(_WIN32)
    return MapViewOfFile(reinterpret_cast<HANDLE>(resource), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                         size);
#else
    struct stat info{};
    if (::fstat(resource, &info) != 0 || info.st_size < static_cast<off_t>(size)) return nullptr;
    void* result = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, resource, 0);
    return result == MAP_FAILED ? nullptr : result;
#endif
}
void* mapCaptureSlot(Resource resource, std::size_t size) {
#if defined(_WIN32)
    const auto section = reinterpret_cast<HANDLE>(resource);
    // Size 0 maps the whole section: an event, a pipe or a file is no section and does not map.
    void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!view) return nullptr;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(view, &info, sizeof(info)) != sizeof(info) || info.Type != MEM_MAPPED ||
        info.AllocationBase != view || info.RegionSize != size) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    return view;
#else
    struct stat info{};
    if (::fstat(resource, &info) != 0 || info.st_size != static_cast<off_t>(size)) return nullptr;
#if defined(__linux__)
    if (!S_ISREG(info.st_mode)) return nullptr;
    const int seals = ::fcntl(resource, F_GET_SEALS);  // -1 for anything that is not a memfd
    constexpr int kNeeded = F_SEAL_SHRINK | F_SEAL_GROW;
    if (seals < 0 || (seals & kNeeded) != kNeeded) return nullptr;
#else
    // A POSIX shared-memory object reports no file type on some systems; a socket, a pipe, a
    // directory or a device is never one.
    if ((info.st_mode & S_IFMT) != 0 && !S_ISREG(info.st_mode)) return nullptr;
#endif
    void* result = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, resource, 0);
    return result == MAP_FAILED ? nullptr : result;
#endif
}
void unmapMemory(void* map, std::size_t size) {
    if (!map) return;
#if defined(_WIN32)
    (void)size;
    UnmapViewOfFile(map);
#else
    ::munmap(map, size);
#endif
}
bool validNotification(Resource resource) {
#if defined(_WIN32)
    const auto event = reinterpret_cast<HANDLE>(resource);
    // Subscription precedes the parent watcher: no consumer can steal these
    // probes. Only a manual-reset event stays ready for both waits.
    if (!SetEvent(event)) return false;
    const bool valid = WaitForSingleObject(event, 0) == WAIT_OBJECT_0 &&
                       WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    ResetEvent(event);
    return valid;
#else
    const int flags = ::fcntl(resource, F_GETFL);
    if (flags < 0 || !(flags & O_NONBLOCK)) return false;
#if defined(__linux__)
    char target[64];
    const auto path = "/proc/self/fd/" + std::to_string(resource);
    const auto length = ::readlink(path.c_str(), target, sizeof(target));
    return length >= 0 &&
           std::string_view(target, static_cast<std::size_t>(length)) == "anon_inode:[eventfd]" &&
           (flags & O_ACCMODE) == O_RDWR;
#else
    struct stat info{};
    return ::fstat(resource, &info) == 0 && S_ISFIFO(info.st_mode) &&
           (flags & O_ACCMODE) == O_WRONLY;
#endif
#endif
}
bool signalNotification(Resource resource) {
#if defined(_WIN32)
    return SetEvent(reinterpret_cast<HANDLE>(resource)) != 0;
#else
    const std::uint64_t one = 1;
    decltype(::write(0, nullptr, 0)) written;
    do written = ::write(resource, &one, sizeof(one));
    while (written < 0 && errno == EINTR);
    return written == sizeof(one) || (written < 0 && errno == EAGAIN);
#endif
}
bool controlPair(Resource (&pair)[2]) {
#if defined(_WIN32)
    const auto name = "\\\\.\\pipe\\" + uniqueName();
    const std::wstring wide(name.begin(), name.end());
    const auto server = CreateNamedPipeW(
        wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
        65536, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) return false;
    const auto client = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return false;
    }
    OVERLAPPED connected{};
    connected.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool ok = connected.hEvent && (ConnectNamedPipe(server, &connected) ||
                                         GetLastError() == ERROR_PIPE_CONNECTED);
    if (connected.hEvent) CloseHandle(connected.hEvent);
    if (!ok) {
        CloseHandle(client);
        CloseHandle(server);
        return false;
    }
    pair[0] = reinterpret_cast<Resource>(server);
    pair[1] = reinterpret_cast<Resource>(client);
    return true;
#else
#if defined(__linux__)
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0;
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
    for (int fd : pair) {
        const int one = 1;
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
            ::close(pair[0]);
            ::close(pair[1]);
            return false;
        }
    }
    return true;
#endif
#endif
}
void closeControl(Control control) {
#if defined(_WIN32)
    delete control;
#else
    closeResource(control);
#endif
}
Waitable controlWaitable(Control control) {
#if defined(_WIN32)
    return control ? control->waitable() : Waitable{};
#else
    return Waitable(control);
#endif
}
#if defined(_WIN32)
NativeControl::NativeControl(HANDLE pipe, bool acceptsResources)
    : pipe_(pipe), acceptsResources_(acceptsResources) {
    input_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
NativeControl::~NativeControl() {
    if (pending_) {
        DWORD count = 0;
        CancelIoEx(pipe_, &input_);
        GetOverlappedResult(pipe_, &input_, &count, TRUE);
    }
    CloseHandle(pipe_);
    if (input_.hEvent) CloseHandle(input_.hEvent);
}
void NativeControl::arm() {
    if (pending_ || pos_ < end_ || ended_) return;
    if (!input_.hEvent) {
        ended_ = true;
        return;
    }
    ResetEvent(input_.hEvent);
    DWORD count = 0;
    if (ReadFile(pipe_, buffer_, sizeof(buffer_), &count, &input_)) {
        pos_ = 0;
        end_ = count;
        ended_ = count == 0;
        SetEvent(input_.hEvent);
    } else if (GetLastError() == ERROR_IO_PENDING)
        pending_ = true;
    else {
        ended_ = true;
        SetEvent(input_.hEvent);
    }
}
Waitable NativeControl::waitable() {
    arm();
    return Waitable::handle(input_.hEvent);
}
bool NativeControl::read(void* data, std::size_t size,
                         std::chrono::steady_clock::time_point deadline) {
    auto* dst = static_cast<char*>(data);
    while (size) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        arm();
        if (pending_) {
            DWORD count = 0;
            const bool ok = complete(pipe_, input_, deadline, count);
            pending_ = false;
            pos_ = 0;
            end_ = count;
            if (!ok || !count) ended_ = true;
        }
        if (ended_) return false;
        const auto count = std::min(size, end_ - pos_);
        std::memcpy(dst, buffer_ + pos_, count);
        dst += count;
        size -= count;
        pos_ += count;
    }
    return true;
}
bool NativeControl::write(const void* data, std::size_t size,
                          std::chrono::steady_clock::time_point deadline) {
    const auto* src = static_cast<const char*>(data);
    OVERLAPPED op{};
    op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!op.hEvent) return false;
    bool ok = true;
    while (size && ok) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ok = false;
            break;
        }
        ResetEvent(op.hEvent);
        DWORD count = 0;
        ok = WriteFile(pipe_, src, static_cast<DWORD>(std::min<std::size_t>(size, UINT32_MAX)),
                       &count, &op) != 0;
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = complete(pipe_, op, deadline, count);
        if (!ok || !count) {
            ok = false;
            break;
        }
        src += count;
        size -= count;
    }
    CloseHandle(op.hEvent);
    return ok;
}
#endif
}  // namespace supple::host::bridge
