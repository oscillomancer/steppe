// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The host tests make bridged calls off the device callback (process(), the
// lifecycle commands, state and parameter requests). Such a call fails at its
// deadline, and the failed instance is silent (ADR-0013): on a loaded runner
// a stall of a healthy bridge past the product's 2 s (7 s for a lifecycle
// command) fails a test that has nothing to do with deadlines, and shows as a
// wrong sample or a missing answer rather than as the stall. Every test in
// this binary gets a deadline no healthy plug-in misses, as the engine tests
// do (host/engine/tests/bridge_patience.cpp); a test of the deadline itself
// sets a short one on the instance (Instance::setCallDeadlineMs) just before
// the call that must hang, or puts the process-wide one back and restores it.
#include <doctest/doctest.h>

#include <supple/host/clap_host.h>

namespace {
// The engine tests' value: far past any stall a healthy bridge has had.
constexpr int kPatientBridgeMs = 60'000;
const bool kPatient = [] {
    supple::host::setBridgeDeadlineMs(kPatientBridgeMs);
    return true;
}();
}  // namespace

TEST_CASE("host tests wait for bridged calls past any stall of a loaded machine") {
    CHECK(kPatient);
    CHECK(supple::host::bridgeDeadlineMs() == kPatientBridgeMs);
}
