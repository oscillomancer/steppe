// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "supple/base/rt.h"

namespace supple::dsp {
// One channel, factors 1/2/4/8, symmetric half-band sincs (Kaiser beta 11).
// Stages use 145/33/25 taps at 2/4/8 times the base sample rate fs. Even taps
// except the centre are zero. The first stage protects 0..0.45*fs with a
// stopband from 0.55*fs; later stopbands start at 1.55*fs and 3.55*fs.
// Rejection is >=100 dB and round-trip ripple <0.01 dB in the protected band.
// Integer up/down delays are 0/72/80/83 base samples: stage centres 72/16/12
// divided by 1/2/4. Frequencies above 0.45*fs may lie in a transition band.
// Fixed-capacity storage; no fractional phase or host-block-dependent state.
class HalfbandOversampler {
  public:
    void prepare(int factor, uint32_t maxFrames = 0);
    void reset() SUPPLE_RT;
    int factor() const { return factor_; }
    uint32_t latency() const { return latency_; }
    void upsample(double input, std::span<double> output) SUPPLE_RT;
    double downsample(std::span<const double> input) SUPPLE_RT;

  private:
    struct Stage {
        std::array<double, 145> h{};
        std::array<double, 256> down{};
        std::array<double, 128> up{};
        size_t center = 72;
        size_t upHead = 0, downHead = 0;
        void interpolate(double x, double* out) SUPPLE_RT;
        double decimate(const double* in) SUPPLE_RT;
    };
    std::array<Stage, 3> stages_{};
    int factor_ = 1, count_ = 0;
    uint32_t latency_ = 0;
};
}  // namespace supple::dsp
