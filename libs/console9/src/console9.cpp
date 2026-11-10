// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Console 9 arithmetic: Copyright (c) 2018 Chris Johnson, MIT.
// License and original reference: third_party/airwindows-console9/LICENSE.
#include "supple/console9/console9.h"
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace supple::console9 {
namespace {
double clamp(double v, double lo, double hi) { return std::isnan(v) ? lo : std::clamp(v, lo, hi); }
double shape(double gain) {
    if (gain > 1.0) gain *= gain;
    if (gain < 1.0) gain = 1.0 - std::pow(1.0 - gain, 2);
    return gain;
}
uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
template <bool Encode>
double curve(double sample) {
    constexpr double exponent = Encode ? 1.618033988749895 : 0.6180339887498949;
    if (sample > 1.0)
        sample = 1.0;
    else if (sample > 0.0)
        sample = -std::expm1(std::log1p(-sample) * exponent);
    if (sample < -1.0)
        sample = -1.0;
    else if (sample < 0.0)
        sample = std::expm1(std::log1p(sample) * exponent);
    return sample;
}
template <class T>
double dither(double sample, uint32_t& state) {
    int expon = 0;
    if constexpr (std::is_same_v<T, float>) ::frexpf(static_cast<float>(sample), &expon);
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    if constexpr (std::is_same_v<T, float>) {
        // Keep the reference's long-double expression and final double rounding.
        sample =
            static_cast<double>(static_cast<long double>(sample) +
                                (static_cast<long double>(double(state) - uint32_t(0x7fffffff)) *
                                 5.5e-36l * static_cast<long double>(std::ldexp(1.0, expon + 62))));
    }
    // Reference double I/O advances the RNG, but its additive dither is commented out.
    return sample;
}
}  // namespace
double faderGain(double db) SUPPLE_RT { return clamp(std::pow(10., db / 20), 0., 4.); }
double nativeFader(double db) SUPPLE_RT {
    const double g = faderGain(db);
    return g >= 1 ? std::sqrt(g) / 2 : (1 - std::sqrt(1 - g)) / 2;
}
double faderDb(double native) SUPPLE_RT {
    return 20 * std::log10(shape(clamp(native, 0., 1.) * 2.0));
}
double nativePan(double percent) SUPPLE_RT { return (clamp(percent, -100., 100.) / 100 + 1) / 2; }
double panAngle(double nativePan) SUPPLE_RT { return clamp(nativePan, 0., 1.) * 1.57079633; }
void panGains(double angle, double& left, double& right) SUPPLE_RT {
    right = std::sin(angle);
    left = std::sin(1.57079633 - angle);
}
Seeds seedsFor(uint64_t projectSeed, uint64_t salt) SUPPLE_RT {
    uint64_t saltState = salt;
    uint64_t state = projectSeed ^ splitmix(saltState);
    // Fixed work, no rejection loop on the audio path.
    return {std::max(uint32_t(splitmix(state)), uint32_t(16386)),
            std::max(uint32_t(splitmix(state)), uint32_t(16386))};
}

namespace detail {
template <bool E, bool S>
void Stage<E, S>::Ramp::set(double v, uint32_t samples) {
    target = v;
    remaining = samples;
    if (!samples) value = v;
    step = samples ? (target - value) / samples : 0;
}
template <bool E, bool S>
double Stage<E, S>::Ramp::next() {
    if (remaining) {
        value += step;
        if (!--remaining) value = target;
    }
    return value;
}
template <bool E, bool S>
Stage<E, S>::Stage() SUPPLE_RT {
    setNative(.5f, .5f);
}
template <bool E, bool S>
void Stage<E, S>::reset(Seeds seeds) SUPPLE_RT {
    seeds_ = {std::max(seeds.left, uint32_t(16386)), std::max(seeds.right, uint32_t(16386))};
}
template <bool E, bool S>
void Stage<E, S>::setLinear(bool linear) SUPPLE_RT {
    linear_ = linear;
    coefficients();
}
template <bool E, bool S>
void Stage<E, S>::setStaging(Staging staging) SUPPLE_RT {
    staging_ = staging;
    coefficients();
}
template <bool E, bool S>
void Stage<E, S>::coefficients() {
    if (staging_ == Staging::Unity) {
        // Linear Unity Buss: the decode slope and ×φ cancel, so it is x · gain.
        trim_ = gain_.value * (E || linear_ ? 1.0 : 1.618033988749895);
        left_ = right_ = 1;  // exact: Unity has no pan stage
        return;
    }
    trim_ = gain_.value * (E ? 0.763932022500211 : 2.0);
    if constexpr (S) panGains(angle_.value, left_, right_);
}
template <bool E, bool S>
void Stage<E, S>::set(double gain, double pan, uint32_t rampSamples) SUPPLE_RT {
    gain_.set(clamp(gain, 0., 4.), rampSamples);
    angle_.set(panAngle(pan), rampSamples);
    coefficients();
}
template <bool E, bool S>
void Stage<E, S>::setNative(float fader, float pan, uint32_t rampSamples) SUPPLE_RT {
    // Promote B before multiplication, exactly as the original float parameter does.
    set(shape(double(std::clamp(fader, 0.f, 1.f)) * 2.0), double(pan), rampSamples);
}
template <bool E, bool S>
template <class T, bool Linear>
void Stage<E, S>::render(const T* left, const T* right, T* outLeft, T* outRight, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        if (gain_.remaining || angle_.remaining) {
            gain_.next();
            angle_.next();
            coefficients();
        }
        // Read both inputs before writing, including duplicated mono input in place.
        double samples[2]{double(left[i]), 0};
        if constexpr (S) samples[1] = double(right[i]);
        for (uint32_t c = 0; c < (S ? 2u : 1u); ++c) {
            double x = std::isfinite(samples[c]) ? samples[c] : 0;
            if constexpr (Linear) {
                if constexpr (!E)
                    if (staging_ == Staging::Native) x *= 0.6180339887498949;
            } else {
                auto& state = c == 0 ? seeds_.left : seeds_.right;
                if (std::fabs(x) < 1.18e-23) x = state * 1.18e-17;
                if constexpr (!E) x = curve<false>(x);
            }
            if constexpr (S)
                x = x * (c == 0 ? left_ : right_) * trim_;
            else
                x = x * trim_;
            if constexpr (E && !Linear) {
                x *= 0.618033988749895;
                x = curve<true>(x);
            }
            if constexpr (!Linear) x = dither<T>(x, c == 0 ? seeds_.left : seeds_.right);
            const T result = static_cast<T>(x);
            if (c == 0)
                outLeft[i] = result;
            else
                outRight[i] = result;
        }
    }
}
template <bool E, bool S>
template <class T>
void Stage<E, S>::dispatch(const T* left, const T* right, T* outLeft, T* outRight,
                           uint32_t frames) {
    if (linear_)
        render<T, true>(left, right, outLeft, outRight, frames);
    else
        render<T, false>(left, right, outLeft, outRight, frames);
}
template <bool E, bool S>
void Stage<E, S>::process(const float* const* in, float* const* out, uint32_t n) SUPPLE_RT
    requires(S)
{
    dispatch(in[0], in[1], out[0], out[1], n);
}
template <bool E, bool S>
void Stage<E, S>::process(const double* const* in, double* const* out, uint32_t n) SUPPLE_RT
    requires(S)
{
    dispatch(in[0], in[1], out[0], out[1], n);
}
template <bool E, bool S>
void Stage<E, S>::process(const float* in, float* out, uint32_t n) SUPPLE_RT
    requires(!S)
{
    dispatch(in, static_cast<const float*>(nullptr), out, static_cast<float*>(nullptr), n);
}
template <bool E, bool S>
void Stage<E, S>::process(const double* in, double* out, uint32_t n) SUPPLE_RT
    requires(!S)
{
    dispatch(in, static_cast<const double*>(nullptr), out, static_cast<double*>(nullptr), n);
}
template class Stage<true, true>;
template class Stage<false, true>;
template class Stage<true, false>;
template class Stage<false, false>;
}  // namespace detail
}  // namespace supple::console9
