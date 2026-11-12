// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sync/chase.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace supple::sync {

double ChaseTracker::predict(double frame) const SUPPLE_RT {
    if (phase_ < 2) return position_;
    return position_ + speed_ * (frame / config_.sampleRate - time_);
}

void ChaseTracker::observe(const Observation& observation) SUPPLE_RT {
    const double t = observation.frame / config_.sampleRate;
    const double z = observation.position;
    if (phase_ == 0 || (reseed_ && phase_ == 1)) {
        phase_ = 1;
        reseed_ = false;
        seedTime_ = time_ = t;
        seedPosition_ = position_ = z;
        return;
    }
    if (phase_ == 1) {
        time_ = t;
        position_ = z;
        if (t - seedTime_ >= config_.seedSeconds) {
            speed_ = (z - seedPosition_) / (t - seedTime_);
            phase_ = 2;
            trackingSince_ = t;
        }
        return;
    }
    const double dt = std::max(t - time_, 0.0);
    const double predicted = position_ + speed_ * dt;
    const double residual = z - predicted;
    if (reseed_ || std::abs(residual) > config_.jumpSeconds * std::max(std::abs(speed_), 1e-3)) {
        // A locate in the source: keep the speed, take the new position.
        reseed_ = false;
        position_ = z;
        time_ = t;
        ++jumps_;
        return;
    }
    // Continuous loop x' = v + 2*zeta*w*r, v' = w*w*r (zeta = 0.707),
    // integrated over dt with the gains bounded so a sparse stream stays
    // stable.
    const double w = 2.0 * std::numbers::pi * config_.estimatorHz;
    const double k1 = std::min(std::numbers::sqrt2 * w * dt, 1.0);
    const double k2 = dt > 0.0 ? std::min(w * w * dt, 1.0 / dt) : 0.0;
    position_ = predicted + k1 * residual;
    speed_ += k2 * residual;
    time_ = t;
}

ChaseOutput ChaseTracker::update(double frame, double localPosition,
                                 double nominalSpeed) SUPPLE_RT {
    ChaseOutput out;
    const double now = frame / config_.sampleRate;
    if (phase_ == 0) {
        updated_ = false;
        return out;
    }
    const double nominal = std::max(nominalSpeed, 1e-9);
    out.errorSeconds = (predict(frame) - localPosition) / nominal;
    if (phase_ == 1) {
        out.state = ChaseState::Acquiring;
        return out;
    }
    out.externalRate = speed_ / nominal;
    const double dt = updated_ ? std::max(now - lastUpdate_, 0.0) : 0.0;
    lastUpdate_ = now;
    updated_ = true;
    const double error = out.errorSeconds;
    const double magnitude = std::abs(error);

    // Relocation, with hysteresis: fire once the error has stayed above the
    // upper threshold for the delay; re-arm below the lower threshold, or
    // after the retry time if the relocate did not land.
    if (magnitude > config_.relocateAboveSeconds) {
        if (!above_) aboveSince_ = now;
        above_ = true;
    } else {
        above_ = false;
    }
    if (!armed_ && (magnitude < config_.relocateBelowSeconds ||
                    (above_ && now - relocatedAt_ >= config_.retrySeconds)))
        armed_ = true;
    if (armed_ && above_ && now - aboveSince_ >= config_.relocateDelaySeconds) {
        out.relocate = true;
        armed_ = false;
        relocatedAt_ = now;
    }

    double rate = out.externalRate;
    if (above_) {
        integral_ = 0.0;
    } else {
        const double limit = config_.window * std::max(std::abs(out.externalRate), 1e-3);
        double correction = error / config_.phaseSeconds + integral_;
        if (std::abs(correction) < limit && magnitude < config_.integralBandSeconds) {
            // Integrate only near lock and inside the window (anti-windup).
            integral_ += error / (config_.phaseSeconds * config_.integralSeconds) * dt;
            integral_ = std::clamp(integral_, -limit, limit);
        } else {
            correction = std::clamp(correction, -limit, limit);
        }
        rate += correction;
    }
    out.rate = std::clamp(rate, config_.minRate, config_.maxRate);

    if (magnitude <= config_.lockSeconds) {
        if (!within_) withinSince_ = now;
        within_ = true;
    } else {
        within_ = false;
    }
    const bool settled = time_ - trackingSince_ >= config_.settleSeconds;
    if (magnitude > config_.unlockSeconds || above_) locked_ = false;
    if (within_ && settled && now - withinSince_ >= config_.lockHoldSeconds) locked_ = true;

    if (above_)
        out.state = ChaseState::Relocating;
    else if (!settled)
        out.state = ChaseState::Acquiring;
    else
        out.state = locked_ ? ChaseState::Locked : ChaseState::Chasing;
    return out;
}

}  // namespace supple::sync
