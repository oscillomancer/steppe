// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Non-RT file operations. Windows handles use wide paths and delete sharing;
// CRT streams own the handles after adoption and retain buffered writer I/O.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include "supple/base/files.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace supple::audio::detail::files {
inline std::string text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
inline std::FILE* open(const std::filesystem::path& path, bool write, bool create = false) {
#if defined(_WIN32)
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(handle);
        return nullptr;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                   _O_BINARY | (write ? _O_RDWR : _O_RDONLY));
    if (fd < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    std::FILE* stream = _fdopen(fd, write ? "r+b" : "rb");
    if (!stream) _close(fd);
#else
    // A recording's file is regular (crash recovery reads and repairs these):
    // anything else is refused unopened, and one put in its place after that
    // check (a named pipe, a device) is opened without waiting, then refused.
    struct stat info{};
    if (!create && ::lstat(path.c_str(), &info) == 0 && !S_ISREG(info.st_mode)) return nullptr;
    const int fd = ::open(path.c_str(),
                          (write ? O_RDWR : O_RDONLY) | (create ? O_CREAT | O_EXCL : 0) |
                              O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
                          0644);
    if (fd < 0) return nullptr;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK) != 0) {
        ::close(fd);
        return nullptr;
    }
    std::FILE* stream = ::fdopen(fd, write ? "r+b" : "rb");
    if (!stream) ::close(fd);
#endif
    return stream;
}
inline std::int64_t tell(std::FILE* stream) {
#if defined(_WIN32)
    return _ftelli64(stream);
#else
    return ::ftello(stream);
#endif
}
inline int seek(std::FILE* stream, std::int64_t offset, int origin) {
#if defined(_WIN32)
    return _fseeki64(stream, offset, origin);
#else
    return ::fseeko(stream, static_cast<off_t>(offset), origin);
#endif
}
inline bool sync(std::FILE* stream) {
    if (std::fflush(stream) != 0) return false;
#if defined(_WIN32)
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)))) != 0;
#else
    return ::fsync(::fileno(stream)) == 0;
#endif
}
inline std::FILE* temporary(const std::filesystem::path& target, std::filesystem::path& temp) {
#if defined(_WIN32)
    static std::atomic<std::uint64_t> counter{0};
    temp = target;
    temp += "." + std::to_string(GetCurrentProcessId()) + "." +
            std::to_string(counter.fetch_add(1)) + ".wav-tmp";
    return open(temp, true, true);
#else
    std::string name = target.string() + ".tmp-XXXXXX";
    const int fd = ::mkstemp(name.data());
    if (fd < 0) return nullptr;
    temp = name;
    std::FILE* stream = ::fdopen(fd, "wb");
    if (!stream) {
        ::close(fd);
        ::unlink(temp.c_str());
    }
    return stream;
#endif
}
// `busy` (optional) is set when the replace stayed blocked by another program
// holding the target open; Windows retries for about a second first.
inline bool publish(const std::filesystem::path& temp, const std::filesystem::path& target,
                    bool* busy = nullptr) {
#if defined(_WIN32)
    unsigned long error = 0;
    const bool replaced = supple::retryWhileFileBusy(
        target,
        [&] {
            return MoveFileExW(temp.c_str(), target.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        },
        error);
    if (busy) *busy = !replaced && supple::isFileBusyError(target, error);
    return replaced;
#else
    if (busy) *busy = false;
    if (::rename(temp.c_str(), target.c_str()) != 0) return false;
    const auto dir = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
#endif
}
}  // namespace supple::audio::detail::files
