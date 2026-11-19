// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "bridge_diagnostics.h"
#if !defined(_WIN32)
#include <signal.h>
#endif
namespace supple::host::bridge {

BridgeOrdinals& bridgeOrdinals() noexcept {
    static BridgeOrdinals ordinals;
    return ordinals;
}

BridgeEnd classifyBridgeEnd(int why, bool hostKilled) noexcept {
    BridgeEnd end;
    end.status = why;
#if defined(_WIN32)
    // The host ends a bridge with TerminateProcess / TerminateJobObject and exit code 1; a fatal
    // NTSTATUS (severity bits set) is what an unhandled exception ends a process with.
    constexpr int kHostKillStatus = 1;
    const bool fatal = (static_cast<std::uint32_t>(why) & 0xC0000000U) == 0xC0000000U;
    const auto own = diagctx::BridgeHow::Exception;
#else
    // The host ends a bridge with SIGKILL; any signal is a fatal one.
    constexpr int kHostKillStatus = SIGKILL;
    const bool fatal = why > 0;
    const auto own = diagctx::BridgeHow::Signal;
#endif
    // A kill by the host is the host's. Unless the status is another fatal one: a bridge that is
    // dying of its own fault (its core or dump still being written) and is then killed by the host
    // keeps the status of the fault, the kill only cuts the dump short, so the fault came first.
    if (hostKilled) {
        end.how = fatal && why != kHostKillStatus ? own : diagctx::BridgeHow::Host;
        return end;
    }
    end.how = fatal ? own : diagctx::BridgeHow::Exit;
    return end;
}

}  // namespace supple::host::bridge
