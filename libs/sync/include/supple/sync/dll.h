// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A second-order delay-locked loop over the arrival times of periodic events
// (MIDI clocks, MTC quarter frames, LTC frames), used by the receivers
// (spec: docs/specs/sync.md §1.3).
//
// The loop predicts the next event time t1 from a period estimate. For each
// event at time t, with error e = t - t1:
//     t0 = t1;  t1 += b*e + period;  period += c*e
// with w = 2*pi*B*T (B the loop bandwidth in Hz, T the period in seconds),
// b = sqrt(2)*w and c = w*w: a critically damped (zeta = 0.707) loop whose
// noise bandwidth is B. t0 is the filtered time of the latest event and
// `period` the filtered period; both follow a tempo ramp without lag at
// steady state (a type-2 loop). Two events seed it: the first interval is
// the initial period. To acquire quickly the bandwidth starts wide and
// narrows as events arrive: B' = max(B, 1 / (n*T)) after n events, the
// bandwidth of a least-squares fit over them, so a one-interval seed is
// refined like a growing window until the configured loop takes over.
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

#include "supple/base/rt.h"

namespace supple::sync {

class PeriodDll {
  public:
    void reset() SUPPLE_RT { count_ = 0; }
    // Restarts from this event: the next event seeds the period again.
    void restart(double t) SUPPLE_RT {
        count_ = 1;
        t0_ = t;
    }
    // Feeds an event at `t` (frames). Returns the error against the
    // prediction once the period is seeded (from the third event), else 0.
    double feed(double t, double bandwidthHz, double sampleRate) SUPPLE_RT {
        if (count_ == 0) {
            restart(t);
            return 0.0;
        }
        if (count_ == 1) {
            period_ = std::max(t - t0_, 1e-9);
            t0_ = t;
            t1_ = t + period_;
            count_ = 2;
            return 0.0;
        }
        const double e = t - t1_;
        const double seconds = period_ / sampleRate;
        const double bandwidth = std::max(bandwidthHz, 1.0 / (count_ * seconds));
        const double w = std::min(2.0 * std::numbers::pi * bandwidth * seconds, 0.5);
        t0_ = t1_;
        t1_ += std::numbers::sqrt2 * w * e + period_;
        period_ = std::max(period_ + w * w * e, 1e-9);
        if (count_ < 1 << 30) ++count_;
        return e;
    }
    // Events seen since the last reset (saturating).
    int count() const SUPPLE_RT { return count_; }
    bool running() const SUPPLE_RT { return count_ >= 2; }
    double period() const SUPPLE_RT { return period_; }  // frames
    double time() const SUPPLE_RT { return t0_; }        // filtered time of the latest event
    double next() const SUPPLE_RT { return t1_; }        // predicted time of the next event

  private:
    int count_ = 0;
    double t0_ = 0.0;
    double t1_ = 0.0;
    double period_ = 1.0;
};

// An incremental least-squares line y = a + b*x, for rates measured over a
// run of events (x the event count, y its frame): the slope's error falls
// with the run's length to the power 1.5, far faster than a loop's.
class LineFit {
  public:
    void reset() SUPPLE_RT { *this = LineFit(); }
    void add(double x, double y) SUPPLE_RT {
        ++n_;
        sx_ += x;
        sy_ += y;
        sxx_ += x * x;
        sxy_ += x * y;
    }
    int count() const SUPPLE_RT { return n_; }
    // 0 until two distinct x.
    double slope() const SUPPLE_RT {
        const double d = n_ * sxx_ - sx_ * sx_;
        return n_ >= 2 && d > 0.0 ? (n_ * sxy_ - sx_ * sy_) / d : 0.0;
    }

  private:
    int n_ = 0;
    double sx_ = 0.0, sy_ = 0.0, sxx_ = 0.0, sxy_ = 0.0;
};

}  // namespace supple::sync
