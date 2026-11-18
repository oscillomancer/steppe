// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// A bridge's capture slot, as the supervisor holds it (ADR-0083, topic 6c).
//
// The slot is the shared memory a bridge process writes its own crash capsule into
// (`diagcap/selfcapture.h`); the worker announces it with a Slot event and the supervisor keeps
// the descriptor (POSIX) or handle (Windows) with the bridge's entry. The supervisor never maps it
// where a mapping could fault: POSIX reads with `pread`, Windows maps a read-only view of a section
// of fixed extent and copies. The resource is untrusted until `adopt` has checked it, and what the
// slot holds is untrusted input after that (the capsule parser's business, step 6c.4).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace supple::diagnostics::detail {

class CaptureSlot {
  public:
    CaptureSlot() = default;
    CaptureSlot(const CaptureSlot&) = delete;
    CaptureSlot& operator=(const CaptureSlot&) = delete;
    CaptureSlot(CaptureSlot&& other) noexcept;
    CaptureSlot& operator=(CaptureSlot&& other) noexcept;
    ~CaptureSlot();

    // Checks `resource` and takes it into `into`. True: `into` owns it and the caller must not
    // close it. False: nothing changed and the caller still owns it (and closes it). A resource
    // is a slot when it is shared memory of exactly `selfcapture::kSlotBytes` (POSIX: a regular
    // object, on Linux a memfd sealed against shrinking and growing; Windows: a section whose
    // whole extent that is) whose header is a slot's: its magic, version and capacity, the
    // ordinal `bridge`, and a state that is one of the three. Reading it never waits.
    static bool adopt(std::intptr_t resource, std::uint16_t bridge, CaptureSlot& into);

    bool held() const noexcept { return resource_ != kNone; }
    // The capsule a bridge published, copied out: nullopt unless the slot is complete (empty, or
    // writing: the bridge died mid-write, or a header that has stopped being a slot's) and its
    // length fits. At most the slot's size is ever read, once, with no mapping that can fault on
    // POSIX. Safe to call after the bridge is gone, which is its only use.
    std::optional<std::vector<std::uint8_t>> capsule() const;
    // Closes the resource now (it is closed at destruction anyway).
    void release() noexcept;

  private:
    static constexpr std::intptr_t kNone = -1;
    std::intptr_t resource_ = kNone;
};

// Tests only: reads slots through a read-only mapping (what macOS does always, because its
// shared-memory objects cannot be read with pread) or with pread (what Linux does). POSIX only;
// Windows always maps.
#if !defined(_WIN32)
void readSlotsByMappingForTests(bool mapped) noexcept;
#endif

}  // namespace supple::diagnostics::detail
