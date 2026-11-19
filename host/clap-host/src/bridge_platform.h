// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Native resources used only by the plugin bridge. Linux stays fd-based.
#pragma once
#include <supple/base/waitable.h>
#include <chrono>
#include <cstdint>
#include <memory>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace supple::host::bridge {
using Resource =
#if defined(_WIN32)
    std::intptr_t;
#else
    int;
#endif
inline constexpr Resource noResource = -1;
void closeResource(Resource resource);
Resource createMemory(std::size_t size);
void* mapMemory(Resource resource, std::size_t size);
void unmapMemory(void* map, std::size_t size);
// Maps a capture slot (ADR-0083 topic 6c) that another process sent: only a shared-memory object
// of exactly `size` bytes is one. POSIX: a regular object of that size (Linux: a memfd sealed
// against shrinking and growing, so the mapping cannot become a fault); Windows: a file-mapping
// section whose whole extent is `size`. The mapping is read-write and shared; the caller closes
// `resource` either way. Null for anything else (a pipe, an event, a file of another size, an
// unsealed memfd).
void* mapCaptureSlot(Resource resource, std::size_t size);
bool validNotification(Resource resource);
bool signalNotification(Resource resource);
inline Resource notificationTarget(const WakeEvent& event) {
#if defined(_WIN32)
    return reinterpret_cast<Resource>(event.waitable().nativeHandle());
#else
    return event.writeFd();
#endif
}
#if defined(_WIN32)
// One overlapped read owns a bounded buffer and a manual-reset readiness event.
// Calls are serialized by the control-plane owner, never made on audio.
class NativeControl {
  public:
    explicit NativeControl(HANDLE pipe, bool acceptsResources = false);
    bool acceptsResources() const { return acceptsResources_; }
    void rejectResource() { rejectedResource_ = true; }
    bool rejectedResource() const { return rejectedResource_; }
    ~NativeControl();
    NativeControl(const NativeControl&) = delete;
    NativeControl& operator=(const NativeControl&) = delete;
    Waitable waitable();
    bool read(void* data, std::size_t size, std::chrono::steady_clock::time_point deadline);
    bool write(const void* data, std::size_t size, std::chrono::steady_clock::time_point deadline);

  private:
    void arm();
    HANDLE pipe_;
    bool acceptsResources_;
    bool rejectedResource_ = false;
    OVERLAPPED input_{};
    bool pending_ = false, ended_ = false;
    char buffer_[65536];
    std::size_t pos_ = 0, end_ = 0;
};
using Control = NativeControl*;
inline constexpr Control noControl = nullptr;
#else
using Control = int;
inline constexpr Control noControl = -1;
#endif
bool controlPair(Resource (&pair)[2]);
void closeControl(Control control);
Waitable controlWaitable(Control control);
}  // namespace supple::host::bridge
