// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>
#include "capture_slot.h"
#include "supple/diagcap/selfcapture.h"
namespace supple::diagnostics::detail {
namespace {
namespace capture = selfcapture;
// Reads exactly `bytes` at `offset`, however the kernel splits it. False on an error or a short
// object (a pread past the end returns 0, never a fault).
bool readExactly(int fd, void* out, std::size_t bytes, std::size_t offset) {
    auto* at = static_cast<unsigned char*>(out);
    while (bytes > 0) {
        const ssize_t n = ::pread(fd, at, bytes, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += n;
        offset += static_cast<std::size_t>(n);
        bytes -= static_cast<std::size_t>(n);
    }
    return true;
}
bool sharedMemoryOfSlotSize(int fd) {
    struct stat info{};
    if (::fstat(fd, &info) != 0 || info.st_size != static_cast<off_t>(capture::kSlotBytes))
        return false;
#if defined(__linux__)
    if (!S_ISREG(info.st_mode)) return false;
    const int seals = ::fcntl(fd, F_GET_SEALS);  // -1 for anything that is not a memfd
    constexpr int kNeeded = F_SEAL_SHRINK | F_SEAL_GROW;
    return seals >= 0 && (seals & kNeeded) == kNeeded;
#else
    // A POSIX shared-memory object reports no file type on some systems; a socket, a pipe, a
    // directory or a device is never one.
    return (info.st_mode & S_IFMT) == 0 || S_ISREG(info.st_mode);
#endif
}
}  // namespace

CaptureSlot::CaptureSlot(CaptureSlot&& other) noexcept
    : resource_(std::exchange(other.resource_, kNone)) {}
CaptureSlot& CaptureSlot::operator=(CaptureSlot&& other) noexcept {
    if (this != &other) {
        release();
        resource_ = std::exchange(other.resource_, kNone);
    }
    return *this;
}
CaptureSlot::~CaptureSlot() { release(); }
void CaptureSlot::release() noexcept {
    if (resource_ != kNone) ::close(static_cast<int>(resource_));
    resource_ = kNone;
}

bool CaptureSlot::adopt(std::intptr_t resource, std::uint16_t bridge, CaptureSlot& into) {
    if (resource < 0 || resource > INT32_MAX) return false;
    const int fd = static_cast<int>(resource);
    if (!sharedMemoryOfSlotSize(fd)) return false;
    capture::SlotHeader header;
    if (!readExactly(fd, &header, sizeof header, 0) ||
        !capture::slotUsable(header, capture::kSlotBytes) || header.ordinal != bridge ||
        header.state > capture::kSlotComplete)
        return false;
    into.release();
    into.resource_ = resource;
    return true;
}

std::optional<std::vector<std::uint8_t>> CaptureSlot::capsule() const {
    if (!held()) return std::nullopt;
    const int fd = static_cast<int>(resource_);
    capture::SlotHeader header;
    if (!readExactly(fd, &header, sizeof header, 0) ||
        !capture::slotUsable(header, capture::kSlotBytes) || header.state != capture::kSlotComplete)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(header.length);
    if (!bytes.empty() && !readExactly(fd, bytes.data(), bytes.size(), sizeof header))
        return std::nullopt;
    return bytes;
}

}  // namespace supple::diagnostics::detail
