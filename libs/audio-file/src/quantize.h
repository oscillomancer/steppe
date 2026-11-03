// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The one PCM quantizer of the WAV, AIFF and FLAC writers, so they decode
// alike: scale, seeded TPDF dither (one stream, frame by frame, channel by
// channel), round, clamp. Noise shaping adds fixed error feedback per channel
// before the dither: the classic three-tap filter (1.623, -0.982, 0.109)
// that moves the requantization noise towards the top of the band, where the
// ear is least sensitive. Deterministic: the same seed gives the same bits.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "supple/base/rng.h"

namespace supple::audio::detail {

class Quantizer {
  public:
    Quantizer(int bits, bool dither, bool shaped, std::uint64_t seed, std::size_t channels)
        : scale_(bits == 16 ? 32768.0 : 8388608.0),
          max_(bits == 16 ? 32767.0 : 8388607.0),
          dither_(dither || shaped),
          shaped_(shaped),
          rng_(seed),
          errors_(shaped ? channels : 0) {}

    std::int32_t operator()(float sample, std::size_t channel) {
        double x = static_cast<double>(sample) * scale_;
        double shapedInput = x;
        if (shaped_) {
            if (std::isnan(x)) x = 0.0;
            auto& e = errors_[channel];
            shapedInput = x - (1.623 * e[0] - 0.982 * e[1] + 0.109 * e[2]);
            x = shapedInput;
        }
        if (dither_) x += rng_.uniform() - rng_.uniform();   // TPDF, +-1 LSB
        if (!std::isfinite(x)) x = std::isnan(x) ? 0.0 : x;  // NaN -> 0; inf clamps below
        const double q = std::clamp(std::floor(x + 0.5), -scale_, max_);
        if (shaped_) {
            auto& e = errors_[channel];
            // Clipping makes the error large; bounding it keeps the loop stable.
            const double error = std::clamp(q - shapedInput, -4.0, 4.0);
            e = {error, e[0], e[1]};
        }
        return static_cast<std::int32_t>(q);
    }

  private:
    double scale_, max_;
    bool dither_, shaped_;
    Rng rng_;
    std::vector<std::array<double, 3>> errors_;
};

}  // namespace supple::audio::detail
