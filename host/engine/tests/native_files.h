// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the operating system says about a file, read natively for tests that
// set up a case the product must notice (a file replaced under the same name).
// Defined in native_files.cpp, so <windows.h> stays out of the test files.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace supple::test {
// The file's index on its volume: the inode on POSIX, the file index on
// Windows. Empty when the file cannot be read.
std::optional<std::uint64_t> fileIndex(const std::filesystem::path& path);

#if defined(_WIN32)
// Holds `path` open with no sharing, as another program can on Windows, so
// that no one else can open it until this is destroyed. POSIX has no such
// lock; a test there takes the file's permissions away instead.
class ExclusiveHold {
  public:
    explicit ExclusiveHold(const std::filesystem::path& path);
    ~ExclusiveHold();
    ExclusiveHold(const ExclusiveHold&) = delete;
    ExclusiveHold& operator=(const ExclusiveHold&) = delete;
    bool held() const { return handle_ != nullptr; }

  private:
    void* handle_ = nullptr;
};
#endif
}  // namespace supple::test
