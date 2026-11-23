// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// For tests that compare a bridged plug-in's real-time output with another
// render (ADR-0081).
#pragma once

#include "supple/engine/realtime.h"
#include "supple/host/clap_host.h"

namespace supple::engine::test {
// The engine tests' deadline for bridged calls off the callback (set for
// the whole binary by bridge_patience.cpp): a stall of the machine never
// fails a healthy plug-in, so a render compared bit for bit is decided by
// its audio, not by load.
inline constexpr int kPatientBridgeMs = 60'000;
// For a test of the deadline itself: the product's default meanwhile.
class BridgeDeadline {
  public:
    explicit BridgeDeadline(int ms) : previous_(host::bridgeDeadlineMs()) {
        host::setBridgeDeadlineMs(ms);
    }
    ~BridgeDeadline() { host::setBridgeDeadlineMs(previous_); }
    BridgeDeadline(const BridgeDeadline&) = delete;
    BridgeDeadline& operator=(const BridgeDeadline&) = delete;

  private:
    int previous_;
};

// For a test of the transport wait's refusal itself (engine.transport): a
// short bound meanwhile, where the binary is patient (bridge_patience.cpp).
class TransportWait {
  public:
    explicit TransportWait(double seconds) : previous_(transportWaitSeconds()) {
        setTransportWaitSeconds(seconds);
    }
    ~TransportWait() { setTransportWaitSeconds(previous_); }
    TransportWait(const TransportWait&) = delete;
    TransportWait& operator=(const TransportWait&) = delete;

  private:
    double previous_;
};

// A bridge deadline no healthy plug-in misses, however loaded the machine.
// A late live block is silent by design, so a run compared bit for bit must
// not have one: such tests also check bridgeLateCycles() is 0, so a late
// block fails as late rather than as differing audio.
inline EngineOptions patient(EngineOptions options) {
    options.liveBridgeDeadlineMs = 60'000;
    return options;
}
}  // namespace supple::engine::test
