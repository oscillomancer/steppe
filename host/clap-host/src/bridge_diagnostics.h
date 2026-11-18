// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the host tells the crash-report inventory about its bridge processes (ADR-0083, topic 6a):
// which ordinal each one has, and how it ended. The announcing itself is cold and runs only
// where a bridge is made, observed ending on the main thread, or destroyed.
#pragma once
#include <atomic>
#include <cstdint>
#include "supple/diagctx/inventory.h"
namespace supple::host::bridge {

// The ordinals bridge processes get within a run: 1, 2, 3, ..., never reused. An ordinal is a
// u16 in the report, so the 65536th bridge of a run gets none (0, which a report reads as "this
// process"): a stated limit, not a case a run reaches.
class BridgeOrdinals {
  public:
    std::uint16_t take() noexcept {
        const std::uint32_t n = next_.fetch_add(1, std::memory_order_relaxed);
        return n <= UINT16_MAX ? static_cast<std::uint16_t>(n) : std::uint16_t{0};
    }

  private:
    std::atomic<std::uint32_t> next_{1};
};
BridgeOrdinals& bridgeOrdinals() noexcept;

struct BridgeEnd {
    diagctx::BridgeHow how = diagctx::BridgeHow::Exit;
    std::int32_t status = 0;
};
// How a bridge ended, from what the host observed. `why` is NativeChild::exitStatus(): on POSIX
// 0 for an exit (the exit code is not read) and the signal number otherwise; on Windows the
// process's exit code. `hostKilled`: the host killed it (it set this before every kill), so the
// end is Host, unless the status is a fatal one that the host's own kill does not give (POSIX: a
// signal other than SIGKILL; Windows: an exception status other than the exit code 1 it kills
// with): then the bridge was already dying of its own fault when it was killed (its core or dump
// still being written), the kill only cut that short, and the end is the fault's. Otherwise
// POSIX: a signal is Signal, anything else Exit; Windows: a status with the severity bits set
// (0xC0000000, the fatal NTSTATUS class an unhandled exception ends a process with) is
// Exception, anything else Exit.
BridgeEnd classifyBridgeEnd(int why, bool hostKilled) noexcept;

}  // namespace supple::host::bridge
