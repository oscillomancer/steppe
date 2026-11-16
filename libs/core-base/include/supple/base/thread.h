// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Names and scheduling for the threads the host owns (engine.md §2.2,
// ADR-0026, ADR-0034). Setup only: never call from an audio path.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>

#include "supple/base/result.h"

namespace supple {

// Names the calling thread for `ps -L`, `top -H`, debuggers and profilers.
// Linux keeps at most 15 bytes; a longer name is cut there.
void nameThread(std::string_view name);

// A thread's scheduling class. `None`: there is no such thread, or its
// scheduling could not be read.
struct ThreadPolicy {
    enum class Kind : std::uint8_t { None, Normal, Fifo, RoundRobin, High };
    Kind kind = Kind::None;
    int priority = 0;  // 1..99 for Fifo and RoundRobin; 0 otherwise
    bool realtime() const { return kind == Kind::Fifo || kind == Kind::RoundRobin; }
    bool operator==(const ThreadPolicy&) const = default;
};
// "none", "normal", "fifo", "rr" or "high".
std::string_view policyName(ThreadPolicy::Kind kind);
// What `thread` runs with: SCHED_OTHER, BATCH and IDLE are Normal.
ThreadPolicy threadPolicy(std::thread::native_handle_type thread);
// Gives `thread` the policy (Normal: SCHED_OTHER, its nice value unchanged)
// and verifies it. Fifo and RoundRobin are refused (`thread.rttime`) while
// RLIMIT_RTTIME is finite: the kernel kills a real-time thread that runs that
// long without blocking. Permission failures are `thread.policy`.
// High: Windows thread priority, macOS self-applied QoS; not real-time.
Status setThreadPolicy(std::thread::native_handle_type thread, ThreadPolicy policy);

// Setup only: CPUs available to this process, with hardware-count fallback.
unsigned availableCpus();
// Like availableCpus, but Linux reports 0 if affinity could not be read.
unsigned allowedCpuCount();
// Linux process RT limit; other platforms leave the requested value unchanged.
int realtimePriorityLimit(int requested);

// Page faults taken: minor ones the kernel served from memory (a first
// touch, a page it moved or merged), major ones that waited for storage.
// `major` is -1 where the platform does not tell them apart (Windows).
struct PageFaults {
    std::int64_t minor = 0, major = 0;
};
// The calling thread's native handle, without a system call: callable on an
// audio path, to name the thread for the calls below.
std::thread::native_handle_type currentThreadHandle();
// Linux: the kernel's id of a live thread (its tid); 0 elsewhere.
std::int64_t kernelThreadId(std::thread::native_handle_type thread);
// Linux: the faults the thread with that kernel id took since it started;
// nullopt once it has gone, and on macOS and Windows, which keep no count
// per thread.
std::optional<PageFaults> threadPageFaults(std::int64_t kernelId);
// The whole process's since it started.
PageFaults processPageFaults();
}  // namespace supple
