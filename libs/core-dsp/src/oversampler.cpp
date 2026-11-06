// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/dsp/oversampler.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "supple/dsp/dsp.h"
namespace supple::dsp {
void HalfbandOversampler::prepare(int factor, uint32_t) {
    factor_ = factor == 2 || factor == 4 || factor == 8 ? factor : 1;
    count_ = factor_ == 8 ? 3 : factor_ == 4 ? 2 : factor_ == 2 ? 1 : 0;
    latency_ = 0;
    for (int s = 0; s < count_; ++s) {
        const int center = s == 0 ? 72 : s == 1 ? 16 : 12;
        stages_[s].center = static_cast<size_t>(center);
        latency_ += static_cast<uint32_t>(center) >> s;
        auto& h = stages_[s].h;
        h.fill(0);
        h[center] = 0.5;
        double sum = 0;
        for (int i = 1; i < 2 * center + 1; i += 2) {
            const double x = i - center;
            h[i] = std::sin(std::numbers::pi * x * .5) / (std::numbers::pi * x) *
                   kaiser(i, 2 * center + 1, 11);
            sum += h[i];
        }
        for (int i = 1; i < 2 * center + 1; i += 2) h[i] *= .5 / sum;
    }
    reset();
}
void HalfbandOversampler::reset() SUPPLE_RT {
    for (auto& s : stages_) {
        s.up.fill(0);
        s.down.fill(0);
        s.upHead = s.downHead = 0;
    }
}
void HalfbandOversampler::Stage::interpolate(double x, double* out) SUPPLE_RT {
    up[upHead] = x;
    out[0] = up[(upHead + 128 - center / 2) & 127];
    double y = 0;
    for (size_t i = 0; i < center; ++i) y += 2 * h[2 * i + 1] * up[(upHead + 128 - i) & 127];
    out[1] = y;
    upHead = (upHead + 1) & 127;
}
double HalfbandOversampler::Stage::decimate(const double* in) SUPPLE_RT {
    down[downHead] = in[0];
    double y = .5 * down[(downHead + 256 - center) & 255];
    for (size_t i = 1; i < 2 * center + 1; i += 2) y += h[i] * down[(downHead + 256 - i) & 255];
    downHead = (downHead + 1) & 255;
    down[downHead] = in[1];
    downHead = (downHead + 1) & 255;
    return y;
}
void HalfbandOversampler::upsample(double input, std::span<double> output) SUPPLE_RT {
    std::array<double, 8> a{}, b{};
    a[0] = input;
    int n = 1;
    for (int s = 0; s < count_; ++s) {
        for (int i = 0; i < n; ++i) stages_[s].interpolate(a[i], &b[2 * i]);
        n *= 2;
        a = b;
    }
    std::copy_n(a.begin(), std::min(output.size(), static_cast<size_t>(n)), output.begin());
}
double HalfbandOversampler::downsample(std::span<const double> input) SUPPLE_RT {
    if (input.size() < static_cast<size_t>(factor_)) return 0;
    std::array<double, 8> a{}, b{};
    std::copy_n(input.begin(), factor_, a.begin());
    int n = factor_;
    for (int s = count_ - 1; s >= 0; --s) {
        n /= 2;
        for (int i = 0; i < n; ++i) b[i] = stages_[s].decimate(&a[2 * i]);
        a = b;
    }
    return a[0];
}
}  // namespace supple::dsp
