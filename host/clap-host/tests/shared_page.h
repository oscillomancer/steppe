// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A page of memory that a test and a bridged fixture plug-in share, so the
// test can order the plug-in's audio thread with atomics alone (no system
// call on the plug-in's side). POSIX maps a file (the name is its path);
// Windows maps a page-file section in the session's Local namespace (the
// name is the section's). The bridge is the test's child, in its session.
// Defined in shared_page.cpp, so <windows.h> stays out of the test files.
#pragma once

#include <cstddef>
#include <string>

namespace supple::test {

class SharedPage {
  public:
    static constexpr std::size_t kBytes = 4096;
    // A name unique to this process, from `stem`: a path in the temporary
    // folder (POSIX) or Local\<stem>-<pid> (Windows).
    static std::string nameFor(const std::string& stem);
    // Makes the page, zeroed (the test); empty on failure.
    static SharedPage create(const std::string& name);
    // Maps a page another process made (the plug-in); empty on failure.
    static SharedPage open(const std::string& name);

    SharedPage() = default;
    SharedPage(SharedPage&& other) noexcept;
    SharedPage& operator=(SharedPage&& other) noexcept;
    SharedPage(const SharedPage&) = delete;
    SharedPage& operator=(const SharedPage&) = delete;
    // Unmaps; the maker also removes the file (POSIX).
    ~SharedPage();

    void* data() const { return data_; }
    explicit operator bool() const { return data_ != nullptr; }

  private:
    void reset();
    void* data_ = nullptr;
    void* section_ = nullptr;  // Windows: the section's handle
    std::string file_;         // POSIX: the file the maker removes
};

}  // namespace supple::test
