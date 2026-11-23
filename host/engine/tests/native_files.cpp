// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "native_files.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace supple::test {

std::optional<std::uint64_t> fileIndex(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool found = GetFileInformationByHandle(file, &info) != 0;
    CloseHandle(file);
    if (!found) return std::nullopt;
    return (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) return std::nullopt;
    return static_cast<std::uint64_t>(info.st_ino);
#endif
}

}  // namespace supple::test
