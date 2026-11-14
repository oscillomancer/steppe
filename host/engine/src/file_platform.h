// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>

#include "supple/base/files.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace supple::engine::detail::files {
inline int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}
// Publish a complete temporary file without replacing an existing target.
// The caller removes any remaining temp name afterwards (Windows consumes it).
// On Windows a temporary file that another program briefly holds open is
// retried for about a second; a lasting hold reports EBUSY.
inline int publishNew(const std::filesystem::path& temp, const std::filesystem::path& target) {
#if defined(_WIN32)
    unsigned long error = 0;
    if (supple::retryWhileFileBusy(
            target,
            [&] { return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0; },
            error))
        return 0;
    switch (error) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            errno = EBUSY;
            break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            errno = EEXIST;
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_DISK_FULL:
            errno = ENOSPC;
            break;
        case ERROR_NOT_SAME_DEVICE:
            errno = EXDEV;
            break;
        case ERROR_ACCESS_DENIED:
            errno = EACCES;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
#else
    return ::link(temp.c_str(), target.c_str());
#endif
}
// A new file to append to (a recording's MIDI journal, ADR-0080): never
// over an existing file and never through a link. Null on failure (errno).
inline std::FILE* createAppend(const std::filesystem::path& path) {
#if defined(_WIN32)
    const HANDLE handle =
        CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int fd =
        _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_WRONLY | _O_APPEND);
    if (fd < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    std::FILE* stream = _fdopen(fd, "ab");
    if (!stream) _close(fd);
    return stream;
#else
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return nullptr;
    std::FILE* stream = ::fdopen(fd, "ab");
    if (!stream) ::close(fd);
    return stream;
#endif
}
// What was written reaches the disk (fflush, then fsync / FlushFileBuffers).
inline bool syncFile(std::FILE* stream) {
    if (std::fflush(stream) != 0) return false;
#if defined(_WIN32)
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)))) != 0;
#else
    return ::fsync(::fileno(stream)) == 0;
#endif
}
// Nonserialized cache identity, follows symlinks. No file reads on audio threads.
inline std::optional<std::string> identity(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool found = GetFileInformationByHandle(file, &info) != 0;
    CloseHandle(file);
    if (!found) return std::nullopt;
    const auto combine = [](DWORD high, DWORD low) { return (std::uint64_t{high} << 32) | low; };
    return "file:" + std::to_string(info.dwVolumeSerialNumber) + ":" +
           std::to_string(combine(info.nFileIndexHigh, info.nFileIndexLow)) + ":" +
           std::to_string(combine(info.nFileSizeHigh, info.nFileSizeLow)) + ":" +
           std::to_string(
               combine(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime));
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) return std::nullopt;
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
#else
    const auto modified = info.st_mtim;
#endif
    return "file:" + std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
           std::to_string(info.st_size) + ":" + std::to_string(modified.tv_sec) + ":" +
           std::to_string(modified.tv_nsec);
#endif
}
}  // namespace supple::engine::detail::files
