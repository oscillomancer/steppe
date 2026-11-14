// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "session_lock.h"

#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace supple::engine::detail {

struct SessionLock::Handle {
    fs::path file;
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    void close() {
#if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            UnlockFileEx(handle, 0, 1, 0, &overlapped);
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            fd = -1;
        }
#endif
    }
    ~Handle() { close(); }
};

std::optional<SessionLock> SessionLock::tryAcquire(const fs::path& folder) {
    auto h = std::make_unique<Handle>();
    h->file = folder / kSessionLockName;
#if defined(_WIN32)
    // Shared read/write/delete: another run may open it to try the lock.
    h->handle =
        CreateFileW(h->file.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h->handle == INVALID_HANDLE_VALUE) return std::nullopt;
    // OPEN_REPARSE_POINT opens a link itself rather than its target: refuse
    // it, as O_NOFOLLOW does, and anything else that is not a plain file.
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(h->handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(h->handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(h->handle);
        h->handle = INVALID_HANDLE_VALUE;
        return std::nullopt;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(h->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                    &overlapped)) {
        CloseHandle(h->handle);
        h->handle = INVALID_HANDLE_VALUE;
        return std::nullopt;
    }
#else
    // A link is never followed: the lock file is the folder's own.
    // Nonblocking and a regular file only: a named pipe there is not a lock.
    h->fd = ::open(h->file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (h->fd < 0) return std::nullopt;
    struct stat st{};
    if (::fstat(h->fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(h->fd);
        h->fd = -1;
        return std::nullopt;
    }
    if (::flock(h->fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(h->fd);
        h->fd = -1;
        return std::nullopt;
    }
#endif
    return SessionLock(std::move(h));
}

SessionLock::SessionLock(std::unique_ptr<Handle> handle) : handle_(std::move(handle)) {}
SessionLock::SessionLock(SessionLock&&) noexcept = default;
SessionLock& SessionLock::operator=(SessionLock&&) noexcept = default;
SessionLock::~SessionLock() = default;

void SessionLock::releaseAndRemove() {
    if (!handle_) return;
    const fs::path file = handle_->file;
    handle_.reset();
    std::error_code ec;
    fs::remove(file, ec);
}

}  // namespace supple::engine::detail
