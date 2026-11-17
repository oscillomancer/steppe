// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The macOS trace capsule (ADR-0083, topic 4, step d).
//
// macOS has no external collector: reading another process needs task_for_pid and an
// entitlement, so the failing process reads its own threads inside its terminal hold
// and sends the result to the supervisor. This file is the wire format and the parts
// of that capture that touch no Mach API, so every platform can test them:
//   - fixed storage only: the capsule is static, nothing allocates, locks or formats;
//   - a frame-pointer walker over an injected reader;
//   - the framing (a Message::Trace packet carrying the byte count, then the bytes)
//     with a deadline on both the sending and the receiving side.
// Frames are raw addresses here, in the worker, and only there: the supervisor turns
// them into module-relative offsets (parseCapsule) and drops the addresses. The modules
// it knows are the images dyld lists; the system libraries of the shared cache appear
// as frames without identity.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "native.h"
namespace supple::diagnostics::detail {
constexpr std::uint32_t kCapsuleMagic = 0x32435443;  // "CTC2": v2 adds thread roles
constexpr std::size_t kCapsuleThreads = 128, kCapsuleFrames = 128, kCapsuleModules = 256;
constexpr std::uint32_t kCapsuleModulesTruncated = 1, kCapsuleThreadsDropped = 2;

// Every field is zero by default so the worker's static capsule lives in zero-filled
// memory and costs the binary nothing; the writer sets `magic` and the roles itself.
struct CapsuleHeader {
    std::uint32_t magic, moduleCount, threadCount, flags;
    std::uint32_t dropped, reserved;  // threads not recorded beyond the cap
};
// An image the frames fall in: [start, end) is its executable text at run time and
// `bias` is the load address minus its link address (the slide), so an address minus
// the bias is the link address a resolver finds in the binary.
struct CapsuleModule {
    std::uint64_t bias, start, end;
    std::uint8_t uuid[16];
    std::uint32_t hasUuid, reserved;
};
// `role` is a Role, `missing` a Missing (None once walked) and `stop` a Stop; the
// parser validates all three. `registered` is the diagctx::ThreadRole the process
// registered the thread as (0: none) and `worker` its number for audio-live-N and
// audio-bg-N; the worker reads both from its own execution table. Frame 0 is the exact
// pc, the rest return addresses.
struct CapsuleThread {
    std::uint8_t role, missing, stop, registered;
    std::uint16_t worker, reserved;
    std::uint32_t frameCount;
};
struct CapsuleThreadData {
    CapsuleThread head;
    std::uint64_t frames[kCapsuleFrames];
};
struct CapsuleData {
    CapsuleHeader header;
    CapsuleModule modules[kCapsuleModules];
    CapsuleThreadData threads[kCapsuleThreads];
};
constexpr std::size_t kCapsuleMaxBytes =
    sizeof(CapsuleHeader) + kCapsuleModules * sizeof(CapsuleModule) +
    kCapsuleThreads * (sizeof(CapsuleThread) + kCapsuleFrames * sizeof(std::uint64_t));

#if !defined(_WIN32)
// How long the supervisor reads a capsule for, from the moment it starts collecting. The
// worker must have sent it well before this ends (capture_macos.cpp checks the margin).
constexpr std::uint64_t kCollectionBudget = 600'000'000ULL;
// Monotonic nanoseconds; async-signal-safe.
std::uint64_t monotonicNanoseconds() noexcept;

// Registers to start a walk from.
struct CapsuleRegs {
    std::uint64_t pc = 0, sp = 0, fp = 0;
};
// Reads `bytes` at an address of the target, all or nothing, never faulting.
struct StackReader {
    bool (*read)(void* context, std::uint64_t address, void* out, std::size_t bytes) = nullptr;
    void* context = nullptr;
};
// Fills `thread.frames`, `frameCount` and `stop` by following the frame-pointer chain,
// with the same rules as the supervisor-side walker; stops at `deadline` (nanoseconds
// on the monotonic clock, 0 for none). Allocates nothing. Whether an address is
// executable is for the supervisor to say, against the modules the capsule carries.
void walkStack(const StackReader& reader, const CapsuleRegs& regs, bool arm64,
               CapsuleThreadData& thread, std::uint64_t deadline) noexcept;
// Sends a Message::Trace packet (`base` carries the incident and role) and then the
// used part of `data`, or only an empty marker packet when `data` is null. Every wait
// ends at `deadline`; false if it could not all be sent. Async-signal-safe.
bool sendCapsule(int fd, const Packet& base, const CapsuleData* data,
                 std::uint64_t deadline) noexcept;
// Supervisor side: reads one Message::Trace packet and its bytes within `deadline`.
// Nothing is returned for a missing, empty, oversized or truncated capsule.
std::optional<std::vector<std::uint8_t>> receiveCapsule(int fd, const Packet& terminal,
                                                        std::uint64_t deadline);
// The terminal hold's wait for the supervisor's acknowledgement, for `nanoseconds` in
// all, resuming after interruptions. Async-signal-safe.
void awaitAcknowledgementFor(Native& native, std::uint64_t nanoseconds) noexcept;
#endif
#if defined(__APPLE__)
// Worker side, from the terminal handler after the terminal packet went out: reads this
// process's own threads and images with Mach calls on its own task (no task_for_pid, no
// entitlement), sends the capsule, then waits for the acknowledgement. The whole of it,
// wait included, ends one second after entry. `terminal` carries the faulting thread's
// registers when the handler had them; without a pc (terminate, abort) the calling
// thread is walked from the handler's own frame. Allocates nothing on the heap, takes
// no lock and formats nothing.
void captureAndHold(Native& endpoint, const Packet& terminal) noexcept;
// Records the calling thread as the main thread; called once, on it, at install.
void rememberMainThread() noexcept;
#endif
}  // namespace supple::diagnostics::detail
