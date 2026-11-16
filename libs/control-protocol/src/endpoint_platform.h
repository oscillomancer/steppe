// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <supple/base/result.h>
#include <cstring>
#include <filesystem>
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

namespace supple::rpc::net {
struct Identity {
    std::uint64_t device = 0, inode = 0;
    bool socket = false;
    bool operator==(const Identity&) const = default;
};
inline bool identity(const std::filesystem::path& path, Identity& result) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool found = GetFileInformationByHandle(file, &info) &&
                       GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag));
    CloseHandle(file);
    if (found)
        result = {info.dwVolumeSerialNumber,
                  (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow,
                  tag.ReparseTag == 0x80000023UL};  // IO_REPARSE_TAG_AF_UNIX (Windows 10 1803)
    return found;
#else
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) return false;
    result = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
              S_ISSOCK(info.st_mode)};
    return true;
#endif
}
inline int remove(const std::filesystem::path& path) {
#if defined(_WIN32)
    if (DeleteFileW(path.c_str())) return 0;
    errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
    return -1;
#else
    return ::unlink(path.c_str());
#endif
}
inline int privatePermissions(const std::filesystem::path& path, bool directory) {
#if defined(_WIN32)
    // The user's profile directories supply the inherited Windows ACL.
    if (!directory) return 0;
    const auto handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EACCES;
        return -1;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    const bool valid =
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) &&
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(handle);
    if (!valid) errno = EACCES;
    return valid ? 0 : -1;
#else
    if (!directory) return ::chmod(path.c_str(), 0600);
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat info{};
    int result = ::fstat(fd, &info);
    if (result == 0 && (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())) {
        errno = EACCES;
        result = -1;
    }
    if (result == 0) result = ::fchmod(fd, 0700);
    const int error = errno;
    ::close(fd);
    errno = error;
    return result;
#endif
}
class EndpointLock {
  public:
    explicit EndpointLock(const std::filesystem::path& path) {
#if defined(_WIN32)
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED overlapped{};
        valid_ = file_ != INVALID_HANDLE_VALUE &&
                 LockFileEx(file_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped);
        if (!valid_) errno = EACCES;
#else
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        valid_ = fd_ >= 0 && ::flock(fd_, LOCK_EX) == 0;
#endif
    }
    ~EndpointLock() {
#if defined(_WIN32)
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (fd_ >= 0) {
            if (valid_) ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
#endif
    }
    EndpointLock(const EndpointLock&) = delete;
    EndpointLock& operator=(const EndpointLock&) = delete;
    bool valid() const { return valid_; }

  private:
    bool valid_ = false;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};
}  // namespace supple::rpc::net
