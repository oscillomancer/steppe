// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <utility>
#include "capture_slot.h"
#include "supple/diagcap/selfcapture.h"
namespace supple::diagnostics::detail {
namespace {
namespace capture = selfcapture;
HANDLE handleOf(std::intptr_t value) { return reinterpret_cast<HANDLE>(value); }
// A read-only view of the whole section, or null. Size 0 maps the section's whole extent, so what
// is mapped is what the section is: an event, a file or a section of another size is told by
// the region it makes (a mapped region of exactly the slot's size), never trusted by name.
const unsigned char* mapSlot(HANDLE section) {
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    if (!view) return nullptr;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(view, &info, sizeof(info)) != sizeof(info) || info.Type != MEM_MAPPED ||
        info.AllocationBase != view || info.RegionSize != capture::kSlotBytes) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    return static_cast<const unsigned char*>(view);
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
    if (resource_ != kNone) CloseHandle(handleOf(resource_));
    resource_ = kNone;
}

bool CaptureSlot::adopt(std::intptr_t resource, std::uint16_t bridge, CaptureSlot& into) {
    if (resource <= 0 || resource == reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE))
        return false;
    // The handle came from the worker's table of handles: by the time it was duplicated it may
    // have been closed and the value reused. The mapping and the header (this bridge's
    // ordinal) are what decide whether it is the slot.
    const unsigned char* view = mapSlot(handleOf(resource));
    if (!view) return false;
    capture::SlotHeader header;
    std::memcpy(&header, view, sizeof header);
    UnmapViewOfFile(view);
    if (!capture::slotUsable(header, capture::kSlotBytes) || header.ordinal != bridge ||
        header.state > capture::kSlotComplete)
        return false;
    into.release();
    into.resource_ = resource;
    return true;
}

std::optional<std::vector<std::uint8_t>> CaptureSlot::capsule() const {
    if (!held()) return std::nullopt;
    const unsigned char* view = mapSlot(handleOf(resource_));
    if (!view) return std::nullopt;
    std::optional<std::vector<std::uint8_t>> result;
    capture::SlotHeader header;
    std::memcpy(&header, view, sizeof header);
    if (capture::slotUsable(header, capture::kSlotBytes) && header.state == capture::kSlotComplete)
        result.emplace(view + sizeof header, view + sizeof header + header.length);
    UnmapViewOfFile(view);
    return result;
}

}  // namespace supple::diagnostics::detail
