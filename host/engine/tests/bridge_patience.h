// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// For tests that compare a bridged plug-in's real-time output with another
// render (ADR-0081).
#pragma once

#include "supple/engine/realtime.h"

namespace supple::engine::test {
// A bridge deadline no healthy plug-in misses, however loaded the machine.
// A late live block is silent by design, so a run compared bit for bit must
// not have one: such tests also check bridgeLateCycles() is 0, so a late
// block fails as late rather than as differing audio.
inline EngineOptions patient(EngineOptions options) {
    options.liveBridgeDeadlineMs = 60'000;
    return options;
}
}  // namespace supple::engine::test
