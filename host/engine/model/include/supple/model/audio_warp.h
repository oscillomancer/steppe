// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include "supple/model/session.h"

namespace supple::model {
// Marker coordinates are original source seconds and item-local seconds.
// Outside the authored map, extrapolate at the take's scalar speed.
// Source seconds an unwarped take plays per item second: its playrate, times
// the pitch ratio when pitch is not preserved (what playback does).
double audioSpeed(const Take& take);
Status validateWarpMap(const Take& take, std::string path = {});
bool activeWarp(const Take& take);
// Borrowed immutable take; validate once, then O(log markers) lookups.
class AudioWarpMap {
  public:
    explicit AudioWarpMap(const Take& take) : take_(take), active_(activeWarp(take)) {}
    double sourceAt(double itemSeconds) const;
    double itemAt(double sourceSeconds) const;

  private:
    const Take& take_;
    bool active_;
};
double audioSourceAt(const Take& take, double itemSeconds);
double audioItemAt(const Take& take, double sourceSeconds);
// Rebase a take at a split/trim while retaining its complete source mapping.
void rebaseAudioTake(Take& take, double itemSeconds);
void slipAudioTake(Take& take, double sourceDelta);
void rateAudioTake(Take& take, double rate);
Status validateAudioWarp(const Session& session, bool bundles = true);
}  // namespace supple::model
