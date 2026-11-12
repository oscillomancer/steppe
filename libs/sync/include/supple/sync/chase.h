// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The chase tracker, shared by MIDI clock, MTC and LTC input: it turns a
// stream of observations of an external position into the master playrate
// that makes the local transport follow it, a relocate decision when it is
// too far off, and a lock state (spec: docs/specs/sync.md §6, ADR-0070 §7).
//
// Units: positions are whatever the observations use (beats for MIDI clock,
// timecode seconds for MTC and LTC); the local position passed to update()
// must be in the same units. `nominalSpeed` says how many of those units the
// local transport covers per second at playrate 1 (bpm / 60 for beats, 1 for
// seconds); errors and thresholds are in seconds of external time.
//
// Estimator: a second-order tracking loop (critically damped, bandwidth
// estimatorHz) on (frame, position) observations gives the external
// position and speed at any frame; a residual beyond jumpSeconds re-seeds it
// (a locate in the source). Control: rate = speed / nominalSpeed +
// e / phaseSeconds + (1 / (phaseSeconds * integralSeconds)) * integral(e),
// e the position error in seconds, the correction limited to +-window of the
// external speed and the rate to [minRate, maxRate]. The defaults put the
// loop's crossover near 0.5 rad/s, so the engine's playrate path (a ramp of
// at most 300 ms and a 150 ms low-pass, ADR-0046, plus render-ahead: about
// 0.5 s of delay in all) costs about 15 degrees of phase margin.
// Deterministic: the outputs depend only on the sequence of calls.
#pragma once

#include <cstdint>

#include "supple/base/rt.h"
#include "supple/sync/common.h"

namespace supple::sync {

struct ChaseConfig {
    double sampleRate = 48000.0;
    // Estimator bandwidth (Hz): 0.5 keeps the rate within 0.1% under 1 ms
    // of observation jitter.
    double estimatorHz = 0.5;
    // Proportional time constant (s): an error is corrected at 1/phaseSeconds
    // of it per second.
    double phaseSeconds = 2.0;
    // Integral time (s) of the PI loop. The integral runs only while the
    // error is within integralBandSeconds: the speed estimate already feeds
    // forward, so the integral only trims a residual bias, and winding it up
    // over a large error would overshoot.
    double integralSeconds = 8.0;
    double integralBandSeconds = 0.02;
    // The correction's limit, as a fraction of the external speed.
    double window = 0.1;
    double minRate = 0.25;
    double maxRate = 4.0;
    // Relocate when the error stays above relocateAboveSeconds for
    // relocateDelaySeconds; re-arm once it falls below relocateBelowSeconds,
    // or after retrySeconds if a relocate did not take.
    double relocateAboveSeconds = 0.25;
    double relocateBelowSeconds = 0.05;
    double relocateDelaySeconds = 0.1;
    double retrySeconds = 1.0;
    // Locked after the error stays within lockSeconds for lockHoldSeconds;
    // unlocked above unlockSeconds.
    double lockSeconds = 0.002;
    double unlockSeconds = 0.010;
    double lockHoldSeconds = 0.5;
    // An observation this far (s) from the prediction is a jump.
    double jumpSeconds = 0.5;
    // Observations spanning this long (s) before the speed is first
    // estimated, and then before the estimate counts as settled.
    double seedSeconds = 0.1;
    double settleSeconds = 1.0;
};

enum class ChaseState : std::uint8_t {
    Idle,        // no observations
    Acquiring,   // speed not yet settled
    Relocating,  // too far off to chase: waiting for the relocate to land
    Chasing,     // correcting the error
    Locked,      // within lockSeconds for lockHoldSeconds
};

struct ChaseOutput {
    ChaseState state = ChaseState::Idle;
    // The playrate to apply (1 while idle).
    double rate = 1.0;
    // External minus local position, in seconds: positive when the local
    // transport is behind.
    double errorSeconds = 0.0;
    // The external speed as a playrate (speed / nominalSpeed).
    double externalRate = 1.0;
    // Seek now: to predict(frame + the engine's seek latency).
    bool relocate = false;
};

class ChaseTracker {
  public:
    explicit ChaseTracker(ChaseConfig config = {}) : config_(config) {}
    const ChaseConfig& config() const SUPPLE_RT { return config_; }

    void observe(const Observation& observation) SUPPLE_RT;
    // Once per block, after its observations: `frame` is the block start,
    // `localPosition` the local transport there.
    ChaseOutput update(double frame, double localPosition, double nominalSpeed) SUPPLE_RT;
    // The estimated external position at `frame` (the last observation's,
    // before the speed is known).
    double predict(double frame) const SUPPLE_RT;
    // The source announced a jump (Start, Song Position, full frame): the
    // next observation re-seeds the estimate.
    void jumped() SUPPLE_RT { reseed_ = true; }
    void reset() SUPPLE_RT { *this = ChaseTracker(config_); }

    // Re-seeds so far, by jumped() or by the residual.
    std::uint32_t jumps() const SUPPLE_RT { return jumps_; }

  private:
    ChaseConfig config_;
    // Estimator.
    int phase_ = 0;  // 0 none, 1 seeding the speed, 2 tracking
    bool reseed_ = false;
    double seedTime_ = 0.0;
    double seedPosition_ = 0.0;
    double time_ = 0.0;  // seconds of the estimate
    double position_ = 0.0;
    double speed_ = 0.0;
    double trackingSince_ = 0.0;
    std::uint32_t jumps_ = 0;
    // Control.
    bool updated_ = false;
    double lastUpdate_ = 0.0;
    double integral_ = 0.0;
    bool armed_ = true;
    bool above_ = false;
    double aboveSince_ = 0.0;
    double relocatedAt_ = 0.0;
    bool locked_ = false;
    bool within_ = false;
    double withinSince_ = 0.0;
};

}  // namespace supple::sync
