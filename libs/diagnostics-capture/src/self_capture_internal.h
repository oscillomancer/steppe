// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// What the bridge's self-capture needs from each platform (topic 6c). The portable part
// (self_capture.cpp) owns the module list, the slot and the capsule; a platform supplies how
// to identify an image while cold, how to walk the fault thread, and how its handlers call in.
#include <cstdint>
#include "supple/diagcap/selfcapture.h"
namespace supple::diagnostics::selfcapture {
// Cold: fills `out` for the image holding `address` (start, end, bias, id). False when there
// is none or it cannot be read. May allocate.
bool identifyImage(const void* address, BridgeModule& out);
// Handler: fills `thread.frames`, `frameCount` and `stop` for the fault thread, stopping at
// `deadline` (monotonic nanoseconds). Allocation-free, lock-free, cannot fault.
void walkFault(const Fault& fault, detail::CapsuleThreadData& thread,
               std::uint64_t deadline) noexcept;
// The platform's handlers (self_capture_posix.cpp, self_capture_windows.cpp).
bool installHandlers() noexcept;
#if defined(__APPLE__)
// macOS walks every thread of the own task as the worker does (capture_macos.cpp).
bool captureAndPublishMac(const Fault& fault, std::uint64_t start) noexcept;
// The cold list as the portable part holds it, copied out for the macOS handler to add to.
std::uint32_t copyRecorded(BridgeModule* out, std::uint32_t capacity,
                           std::uint32_t& flags) noexcept;
#endif
// Handler building block, shared by the platforms: publishes the capsule described by the
// arguments into the slot (static copies only). `threads` are in the capsule's order.
bool publishCapsule(const detail::CapsuleHeader& header, const BridgeModule* modules,
                    std::uint32_t moduleCount, const detail::CapsuleThreadData* threads,
                    std::uint32_t threadCount, std::uint64_t start) noexcept;
// The execution table's counters into the header, and `thread`'s role and entry from the
// table (`sp` and the thread's recorded stack decide the entry).
void summarizeTable(detail::CapsuleHeader& header) noexcept;
void attributeFault(detail::CapsuleThreadData& thread, std::uint64_t tid,
                    std::uint64_t sp) noexcept;
}  // namespace supple::diagnostics::selfcapture
