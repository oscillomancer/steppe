// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "shared_page.h"

#include <filesystem>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace supple::test {

#if defined(_WIN32)
namespace {
std::wstring wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
}  // namespace

std::string SharedPage::nameFor(const std::string& stem) {
    return "Local\\" + stem + "-" + std::to_string(_getpid());
}

SharedPage SharedPage::create(const std::string& name) {
    SharedPage page;
    // A page-file section starts zeroed.
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(kBytes), wide(name).c_str());
    if (!section) return page;
    page.section_ = section;
    page.data_ = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, kBytes);
    if (!page.data_) page.reset();
    return page;
}

SharedPage SharedPage::open(const std::string& name) {
    SharedPage page;
    HANDLE section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide(name).c_str());
    if (!section) return page;
    page.section_ = section;
    page.data_ = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, kBytes);
    if (!page.data_) page.reset();
    return page;
}

void SharedPage::reset() {
    if (data_) UnmapViewOfFile(data_);
    if (section_) CloseHandle(static_cast<HANDLE>(section_));
    data_ = nullptr;
    section_ = nullptr;
}
#else
std::string SharedPage::nameFor(const std::string& stem) {
    return (std::filesystem::temp_directory_path() /
            (stem + "-" + std::to_string(static_cast<int>(::getpid()))))
        .string();
}

namespace {
void* mapFile(const std::string& name, int flags) {
    const int fd = ::open(name.c_str(), flags, 0600);
    if (fd < 0) return nullptr;
    if ((flags & O_CREAT) && ::ftruncate(fd, static_cast<off_t>(SharedPage::kBytes)) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* words = ::mmap(nullptr, SharedPage::kBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    return words == MAP_FAILED ? nullptr : words;
}
}  // namespace

SharedPage SharedPage::create(const std::string& name) {
    SharedPage page;
    page.data_ = mapFile(name, O_CREAT | O_TRUNC | O_RDWR);  // a new file reads as zeros
    if (page.data_) page.file_ = name;
    return page;
}

SharedPage SharedPage::open(const std::string& name) {
    SharedPage page;
    page.data_ = mapFile(name, O_RDWR);
    return page;
}

void SharedPage::reset() {
    if (data_) ::munmap(data_, kBytes);
    if (!file_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(file_, ignored);
    }
    data_ = nullptr;
    file_.clear();
}
#endif

SharedPage::SharedPage(SharedPage&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      section_(std::exchange(other.section_, nullptr)),
      file_(std::exchange(other.file_, {})) {}

SharedPage& SharedPage::operator=(SharedPage&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        section_ = std::exchange(other.section_, nullptr);
        file_ = std::exchange(other.file_, {});
    }
    return *this;
}

SharedPage::~SharedPage() { reset(); }

}  // namespace supple::test
