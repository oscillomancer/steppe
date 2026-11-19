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
// One capture is made per process, by the first thread that fails; a thread that fails while it is
// being made must not end the process under it (the slot would be left `writing`), so it waits for
// the capture to end, for no longer than the handler's own bound, before it lets the process die.
// Async-signal-safe: atomics, the clock and a short sleep.
//   beginCapture: true for the first caller, which captures and then calls endCapture, whatever
//                 came of it. False for every other.
//   awaitCapture: for a caller that lost: returns when the capture has ended or `start` (the
//                 caller's own handler-start clock reading) plus the handler budget has passed;
//                 at once when `tid` is the claiming thread itself (a fault inside its own
//                 handler cannot wait for itself).
bool beginCapture(std::uint64_t tid) noexcept;
void endCapture() noexcept;
void awaitCapture(std::uint64_t tid, std::uint64_t start) noexcept;
// Tests only: forgets the claim and the end, so the protocol can be run again in one process.
void resetCaptureForTests() noexcept;
// Tests only. The claiming thread's capture holds still after it marks the slot `writing`, until
// `releaseCaptureForTests` (so a second thread can fail in the middle of it, with nothing decided
// by how long that takes); `captureHolding` says it is holding now, and `captureWaiters` how many
// threads have begun to wait for a capture since the last reset. `setHandlerBudgetForTests`
// replaces the handler's bound (reset restores it): a test that must not be decided by the clock
// sets it far past anything the test lasts, or tiny to see the bound itself.
void holdCaptureForTests() noexcept;
void releaseCaptureForTests() noexcept;
bool captureHolding() noexcept;
std::uint32_t captureWaiters() noexcept;
void setHandlerBudgetForTests(std::uint64_t nanoseconds) noexcept;
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
