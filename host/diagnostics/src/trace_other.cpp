// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "trace.h"
#if defined(__APPLE__)
#include "capsule.h"
#endif
// Platforms without an external collector. macOS reads nothing of the worker itself:
// the worker captures its own threads inside its terminal hold (capture_macos.cpp) and
// this takes the capsule it sends, within the same 600 ms budget as the other
// platforms. When no capsule arrives, or it does not validate, the report still records
// the fault thread, unavailable with a stated reason, rather than an empty "successful"
// trace.
namespace supple::diagnostics::detail {
TraceSnapshot collectProcessTrace(Native& native, const Packet& terminal) {
    Missing why = Missing::Unsupported;
#if defined(__APPLE__)
    why = Missing::NotCollected;
    const std::uint64_t deadline = monotonicNanoseconds() + 600'000'000ULL;
    if (const auto bytes = receiveCapsule(static_cast<int>(native.channel), terminal, deadline))
        if (auto snapshot = parseCapsule(bytes->data(), bytes->size())) return std::move(*snapshot);
#else
    (void)native;
    (void)terminal;
#endif
    TraceSnapshot snapshot;
    ThreadTrace fault;
    fault.role = Role::Fault;
    fault.missing = why;
    snapshot.threads.push_back(fault);
    return snapshot;
}
}  // namespace supple::diagnostics::detail
