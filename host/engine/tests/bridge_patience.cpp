// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The engine tests compare bridged renders bit for bit. A bridged call made
// off the device callback (an offline render, a renderer, the main thread)
// fails at its deadline, and the failed node is silent (ADR-0013): on a
// loaded machine a 2 s stall of a healthy bridge, as memory pressure can
// cause, made such a render differ (review round 26, item 2). Every test in
// this binary gets a deadline no healthy plug-in misses; a test of the
// deadline itself sets the default back (BridgeDeadline in bridge_patience.h).
// Likewise the engine waits for a device to act on a transport request (1 s,
// then engine.transport): on a starved runner a healthy dummy device took
// longer (Windows CI 45186989443), so the tests wait kPatientTransportSeconds,
// and a test of the refusal sets a short bound (TransportWait). The waits
// for the renderer to be ahead or to adopt a plan (awaitRendered, awaitPlan:
// 10 s) are patient in the same way: a renderer thread a starved runner does
// not schedule for that long is not a failure of the test (setAwaitSeconds).
#include <doctest/doctest.h>

#include "bridge_patience.h"

namespace {
const bool kPatient = [] {
    supple::host::setBridgeDeadlineMs(supple::engine::test::kPatientBridgeMs);
    supple::engine::setTransportWaitSeconds(supple::engine::kPatientTransportSeconds);
    supple::engine::setAwaitSeconds(supple::engine::kPatientTransportSeconds);
    return true;
}();
}  // namespace

TEST_CASE("engine tests wait for bridged calls past any stall of a loaded machine") {
    CHECK(kPatient);
    CHECK(supple::host::bridgeDeadlineMs() == supple::engine::test::kPatientBridgeMs);
    CHECK(supple::engine::transportWaitSeconds() == supple::engine::kPatientTransportSeconds);
    CHECK(supple::engine::awaitSeconds() == supple::engine::kPatientTransportSeconds);
}

TEST_CASE("the await bound is set, restored and left to a call that passes its own") {
    using namespace supple::engine;
    const double patient = awaitSeconds();
    setAwaitSeconds(0.25);
    CHECK(awaitSeconds() == 0.25);
    setAwaitSeconds(0);  // the default, or the environment's
    CHECK((awaitSeconds() == kAwaitSeconds || awaitSeconds() == kPatientTransportSeconds));
    setAwaitSeconds(patient);
    CHECK(awaitSeconds() == patient);
}
