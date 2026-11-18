// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// A bridge process describes its own death (ADR-0083, topic 6c).
//
// A bridge is a child of the worker and has no supervisor: nothing can read it from outside
// before it is gone. So its fault handler writes a capsule into a **capture slot**, a region of
// memory that exists before the fault and outlives the process (the host creates it and shares
// it with the supervisor; step 6c.3). Everything the handler runs is async-signal-safe on POSIX
// and allocation-free and lock-free on Windows: static storage, system calls, arithmetic and
// atomic loads of the execution table. Nothing here ever takes a lock, allocates, formats,
// opens a file or talks to anyone while a process is failing.
//
// This header is the contract of the slot and of the CTC5 capsule that goes in it, portable so
// that the parser and the tests of every platform can use it, and the bridge's entry points.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "supple/diagcap/capsule.h"
#include "supple/diagctx/table.h"
namespace supple::diagnostics::selfcapture {
using detail::CapsuleHeader;
using detail::CapsuleThread;

// ---- The slot ----------------------------------------------------------------------------
// One per bridge process: kSlotBytes of zero-filled shared memory. A 64-byte header, then the
// capsule. `state` is one atomic word: empty, writing, complete. The handler sets writing,
// writes, then publishes complete with a release store, so a reader that sees complete sees
// the bytes, and one that sees writing knows the bridge died mid-write and uses nothing. The
// slot is written once per bridge.
constexpr std::uint32_t kSlotMagic = 0x544c5343;  // "CSLT"
constexpr std::uint32_t kSlotVersion = 1;
constexpr std::uint32_t kSlotEmpty = 0, kSlotWriting = 1, kSlotComplete = 2;
constexpr std::size_t kSlotBytes = 256 * 1024;
struct SlotHeader {
    std::uint32_t magic, version;
    std::uint32_t state;     // kSlotEmpty, kSlotWriting, kSlotComplete: accessed atomically
    std::uint32_t length;    // bytes of capsule after the header; meaningful once complete
    std::uint32_t capacity;  // the slot's size in bytes, header included
    std::uint32_t ordinal;   // the bridge's number, as the host counts them
    std::uint8_t reserved[40];
};
static_assert(sizeof(SlotHeader) == 64 && std::is_trivially_copyable_v<SlotHeader>);

// ---- The CTC5 capsule --------------------------------------------------------------------
// The capsule of topic 4 (`CapsuleHeader`, modules, threads with their frames, commands) in
// the same compact order, with three differences: the module record carries a build id of up
// to 32 bytes (an ELF note, a Mach-O UUID or a CodeView record are not all 16), the header's
// `reserved` word holds the fault thread's id (low 32 bits: the parser leaves that thread out
// when it lists the table's other threads), and the copy of the process's execution table
// follows the commands when `flags` has kCapsuleHasTable. `magic` is kBridgeCapsuleMagic.
constexpr std::uint32_t kBridgeCapsuleMagic = 0x35435443;  // "CTC5"
constexpr std::uint32_t kCapsuleHasTable = 4;  // joins kCapsuleModulesTruncated (1), ...
constexpr std::size_t kModuleIdBytes = 32;
struct BridgeModule {
    std::uint64_t bias, start, end;
    std::uint8_t id[kModuleIdBytes];
    std::uint32_t idLength, reserved;  // idLength 0: the image has no identity
};
static_assert(sizeof(BridgeModule) == 64 && std::is_trivially_copyable_v<BridgeModule>);
constexpr std::size_t kMaxModules = detail::kCapsuleModules;  // 256
constexpr std::size_t kBridgeCapsuleMaxBytes =
    sizeof(CapsuleHeader) + kMaxModules * sizeof(BridgeModule) +
    detail::kCapsuleThreads *
        (sizeof(CapsuleThread) + detail::kCapsuleFrames * sizeof(std::uint64_t)) +
    diagctx::kMaxCommandEntries * sizeof(detail::CapsuleCommand) + diagctx::kTableBytes;
static_assert(sizeof(SlotHeader) + kBridgeCapsuleMaxBytes <= kSlotBytes,
              "the largest capsule must fit the slot");

// Writes an empty slot's header into `memory` (zero-filled, `bytes` long). The host calls it
// on the region it creates; a reader checks every field with `slotUsable`.
bool initializeSlot(void* memory, std::size_t bytes, std::uint32_t ordinal) noexcept;
// A slot's header as a reader must find it before trusting `state`: magic, version, the
// capacity the reader knows, and a length that fits.
bool slotUsable(const SlotHeader& header, std::size_t bytes) noexcept;

// ---- The bridge's entry points -----------------------------------------------------------
// Cold; once, on the main thread, before any plug-in is loaded and before the first command is
// read. Installs the fault handlers (POSIX: SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT on an
// alternate stack per registered thread, only where the signal still has its default action;
// Windows: the last-chance filter, SIGABRT, SIGSEGV, SIGILL, SIGFPE and the terminate hook),
// makes registered threads get their alternate stack, and records the bridge's own executable.
// A handler chains to the action it replaced, so the process's end stays the fatal one. False
// where nothing was installed (a sanitizer owns the signals); the bridge then works as before,
// with no capsule.
bool install() noexcept;
// Cold. Gives the handler the shared slot to write: it must be an initialized, empty slot
// (`initializeSlot`) of `bytes` bytes that stays mapped for the rest of the process. Until
// then the handler writes a private slot of the same shape that nobody reads (the host hands the
// bridge its shared slot right after it starts: `AttachCaptureSlot`). False, and
// nothing changed, for anything else, and after the first success.
bool attachSlot(void* memory, std::size_t bytes) noexcept;
// Cold. Records the identity of the image that holds `address` (text range, load bias and build
// id) for the capsule's module list: the bridge calls it when a plug-in is created, and for its
// own executable. At most kMaxModules distinct images; a duplicate, an address in no image and
// an image that cannot be read are not recorded. Returns whether the image is in the list.
bool recordImage(const void* address);
// How many images the list holds (tests and the report of what was recorded).
std::uint32_t recordedImages() noexcept;
// What the handlers run, for tests and for the platforms' handlers: builds the capsule in
// static storage and publishes it into the slot. `registers` are the faulting thread's; `now`
// is the monotonic clock reading the handler took at its start. Returns whether the slot is
// complete. Async-signal-safe.
struct Fault {
    std::uint64_t pc = 0, sp = 0, fp = 0, tid = 0;
    // Windows: the CONTEXT of the exception, which the unwinder needs whole; otherwise null.
    const void* native = nullptr;
};
// Monotonic nanoseconds, async-signal-safe: what `start` and the handler's bound are on.
std::uint64_t monotonic() noexcept;
bool captureAndPublish(const Fault& fault, std::uint64_t start) noexcept;
// The handler's bound: nanoseconds from its first instruction to a published slot.
constexpr std::uint64_t kHandlerBudget = 200'000'000ULL;
}  // namespace supple::diagnostics::selfcapture
