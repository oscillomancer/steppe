// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Model-thread-only Linux process controls. Injectable operations let tests
// exercise opt-in settings without changing the test process's memory locks.
#pragma once

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/types.h>
#endif
#include <cstddef>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace supple::engine {

// Portable injection types; on Linux aliases preserve the existing test API.
#if defined(__linux__)
using MemoryLimit = rlimit;
using ProtectionWriteSize = ssize_t;
inline constexpr auto kUnlimitedMemory = RLIM_INFINITY;
#else
struct MemoryLimit {
    std::uint64_t rlim_cur = 0, rlim_max = 0;
};
using ProtectionWriteSize = std::ptrdiff_t;
inline constexpr auto kUnlimitedMemory = UINT64_MAX;
#endif
// Permit shallow idle without deep-state exit delays. Fixed policy; no sysfs reads.
inline constexpr std::int32_t kCpuIdleLatencyUs = 20;

struct RuntimeProtectionError {
    std::string code, message, path, hint;
    std::int64_t errnoValue = 0;
};

struct RuntimeProtectionStatus {
    bool requested = false;
    bool active = false;
    // The CPU idle-latency request only (ADR-0086): a bridged plug-in plays
    // live, so the request is held without being asked for. Linux only;
    // macOS and Windows have no per-process request of this kind.
    bool automatic = false;
    std::optional<RuntimeProtectionError> error;
};

// While the engine runs on Linux, the process is kept out of transparent huge
// pages (ADR-0084): khugepaged collapsing its heap stalls an audio thread that
// touches the range, behind a low-priority kernel thread holding the process's
// address-space lock. `advisedOnly`: regions a plug-in advised (MADV_HUGEPAGE)
// keep huge pages, on kernels from 6.18; otherwise none do. Never active on
// macOS or Windows, which have no such collapse.
struct ThpOptOutStatus {
    bool active = false;
    bool advisedOnly = false;
    std::optional<RuntimeProtectionError> error;
};

// Default operations call libc. Tests replace these before creating EngineApp.
struct RuntimeProtectionOps {
    RuntimeProtectionOps();
    std::function<int(int)> lockMemory;
    std::function<int()> unlockMemory;
    std::function<int(MemoryLimit*)> memoryLimit;
    std::function<int(const char*, int)> openLatency;
    std::function<ProtectionWriteSize(int, const void*, std::size_t)> writeLatency;
    std::function<int(int)> closeLatency;
    // prctl(PR_SET_THP_DISABLE, disable, flags, 0, 0) on Linux.
    std::function<int(unsigned long disable, unsigned long flags)> thpDisable;
};

}  // namespace supple::engine
