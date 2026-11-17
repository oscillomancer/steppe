// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "trace.h"
// Platforms without an external collector yet (Windows: topic 4 step c; macOS captures
// in process, step d). The report still records the fault thread, unavailable with a
// stated reason, rather than an empty "successful" trace.
namespace supple::diagnostics::detail {
TraceSnapshot collectProcessTrace(Native&, const Packet&) {
    TraceSnapshot snapshot;
    ThreadTrace fault;
    fault.role = Role::Fault;
    fault.missing = Missing::Unsupported;
    snapshot.threads.push_back(fault);
    return snapshot;
}
}  // namespace supple::diagnostics::detail
