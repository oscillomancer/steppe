// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/dsp/dsp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#include <pocketfft_hdronly.h>

namespace supple::dsp {
namespace {
constexpr double pi = std::numbers::pi;
double sinc(double x) { return std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x); }
double bessel0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k) {
        term *= x * x / (4.0 * k * k);
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return sum;
}
}  // namespace

Coefficients bilinear(std::array<double, 3> n, std::array<double, 3> d, double fs,
                      double prewarpHz) {
    const double k =
        prewarpHz > 0 ? 2.0 * pi * prewarpHz / std::tan(pi * prewarpHz / fs) : 2.0 * fs;
    const double a0 = d[0] * k * k + d[1] * k + d[2];
    return {(n[0] * k * k + n[1] * k + n[2]) / a0, (2.0 * (n[2] - n[0] * k * k)) / a0,
            (n[0] * k * k - n[1] * k + n[2]) / a0, (2.0 * (d[2] - d[0] * k * k)) / a0,
            (d[0] * k * k - d[1] * k + d[2]) / a0};
}
Coefficients lowpass(double fs, double hz, double q) {
    const double w = 2 * pi * hz / fs, a = std::sin(w) / (2 * q), c = std::cos(w);
    const double d = 1 + a;
    return {(1 - c) / (2 * d), (1 - c) / d, (1 - c) / (2 * d), -2 * c / d, (1 - a) / d};
}
Coefficients highpass(double fs, double hz, double q) {
    const double w = 2 * pi * hz / fs, a = std::sin(w) / (2 * q), c = std::cos(w);
    const double d = 1 + a;
    return {(1 + c) / (2 * d), -(1 + c) / d, (1 + c) / (2 * d), -2 * c / d, (1 - a) / d};
}
Coefficients highShelf(double fs, double hz, double gainDb, double q) {
    const double a = std::pow(10.0, gainDb / 40.0), w = 2 * pi * hz / fs;
    const double c = std::cos(w);
    const double alpha = std::sin(w) * 0.5 * std::sqrt((a + 1 / a) * (1 / q - 1) + 2);
    const double root = 2 * std::sqrt(a) * alpha;
    const double d = (a + 1) - (a - 1) * c + root;
    return {a * ((a + 1) + (a - 1) * c + root) / d, -2 * a * ((a - 1) + (a + 1) * c) / d,
            a * ((a + 1) + (a - 1) * c - root) / d, 2 * ((a - 1) - (a + 1) * c) / d,
            ((a + 1) - (a - 1) * c - root) / d};
}
Coefficients kShelf(double fs) {
    constexpr double f = 1681.974450955533, q = 0.7071752369554196;
    constexpr double gain = 3.999843853973347;
    const double vh = std::pow(10.0, gain / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double k = std::tan(pi * f / fs), k2 = k * k, d = 1 + k / q + k2;
    return {(vh + vb * k / q + k2) / d, 2 * (k2 - vh) / d, (vh - vb * k / q + k2) / d,
            2 * (k2 - 1) / d, (1 - k / q + k2) / d};
}
Coefficients kHighpass(double fs) {
    constexpr double f = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(pi * f / fs), d = 1 + k / q + k * k;
    return {1 / d, -2 / d, 1 / d, 2 * (k * k - 1) / d, (1 - k / q + k * k) / d};
}

double hann(std::size_t i, std::size_t n) {
    return n <= 1 ? 1 : 0.5 - 0.5 * std::cos(2 * pi * i / (n - 1));
}
double blackmanHarris(std::size_t i, std::size_t n) {
    if (n <= 1) return 1;
    const double p = 2 * pi * i / (n - 1);
    return 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2 * p) - 0.01168 * std::cos(3 * p);
}
double kaiser(std::size_t i, std::size_t n, double beta) {
    if (n <= 1) return 1;
    const double t = 2.0 * i / (n - 1) - 1;
    return bessel0(beta * std::sqrt(std::max(0.0, 1 - t * t))) / bessel0(beta);
}
void PolyphaseUpsampler::prepare(int factor) {
    factor_ = std::max(1, factor);
    coefficients_.resize(static_cast<std::size_t>(factor_) * 32);
    for (int phase = 0; phase < factor_; ++phase) {
        double sum = 0;
        for (int tap = 0; tap < 32; ++tap) {
            const double x = tap - 15.5 + static_cast<double>(phase) / factor_;
            const double h = 0.94 * sinc(0.94 * x) * kaiser(tap, 32, 8.6);
            coefficients_[static_cast<std::size_t>(phase) * 32 + tap] = h;
            sum += h;
        }
        for (int tap = 0; tap < 32; ++tap)
            coefficients_[static_cast<std::size_t>(phase) * 32 + tap] /= sum;
    }
    reset();
}
void PolyphaseUpsampler::reset() SUPPLE_RT {
    delay_.fill(0);
    head_ = 0;
}
void PolyphaseUpsampler::process(float input, std::span<double> output) SUPPLE_RT {
    if (coefficients_.empty() || output.size() < static_cast<std::size_t>(factor_)) return;
    delay_[static_cast<std::size_t>(head_)] = static_cast<double>(input);
    for (int p = 0; p < factor_; ++p) {
        double y = 0;
        for (int t = 0; t < 32; ++t) {
            const int index = (head_ - t + 32) % 32;
            y += coefficients_[static_cast<std::size_t>(p) * 32 + t] *
                 delay_[static_cast<std::size_t>(index)];
        }
        output[static_cast<std::size_t>(p)] = y;
    }
    head_ = (head_ + 1) % 32;
}
std::vector<float> resample(std::span<const float> input, double sourceRate, double targetRate) {
    if (!std::isfinite(sourceRate) || !std::isfinite(targetRate) || sourceRate <= 0 ||
        targetRate <= 0 || input.empty())
        return {};
    const double rateRatio = targetRate / sourceRate;
    const double outputCount = input.size() * rateRatio;
    if (!std::isfinite(rateRatio) || rateRatio <= 0 || !std::isfinite(outputCount) ||
        outputCount >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
        outputCount >= static_cast<double>(std::vector<float>().max_size()))
        return {};
    const auto count = static_cast<std::size_t>(std::llround(outputCount));
    if (count == 0) return {};
    constexpr int phases = 2048;
    const double ratio = std::min(1.0, rateRatio);
    const int taps = static_cast<int>(std::min(4096.0, 2 * std::ceil(32 / ratio)));
    const int center = taps / 2 - 1;
    const double cutoff = 0.90 * ratio;
    std::vector<double> window(static_cast<std::size_t>(taps));
    for (int t = 0; t < taps; ++t) window[static_cast<std::size_t>(t)] = kaiser(t, taps, 10.5);
    std::vector<double> table(static_cast<std::size_t>(taps * phases));
    for (int p = 0; p < phases; ++p) {
        double sum = 0;
        for (int t = 0; t < taps; ++t) {
            const double x = t - center - static_cast<double>(p) / phases;
            const double h = cutoff * sinc(cutoff * x) * window[static_cast<std::size_t>(t)];
            table[static_cast<std::size_t>(p * taps + t)] = h;
            sum += h;
        }
        for (int t = 0; t < taps; ++t) table[static_cast<std::size_t>(p * taps + t)] /= sum;
    }
    std::vector<float> output(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double position = i / rateRatio;
        const auto base = static_cast<std::int64_t>(std::floor(position));
        const int phase = std::min(phases - 1, static_cast<int>((position - base) * phases));
        double y = 0;
        for (int t = 0; t < taps; ++t) {
            const auto index = base + t - center;
            if (index >= 0 && static_cast<std::size_t>(index) < input.size())
                y += table[static_cast<std::size_t>(phase * taps + t)] *
                     static_cast<double>(input[static_cast<std::size_t>(index)]);
        }
        output[i] = static_cast<float>(y);
    }
    return output;
}
void RealFft::prepare(std::size_t size) {
    size_ = size;
    scratch_.resize(size);
    bitReverse_.clear();
    twiddles_.clear();
    if (!size || (size & (size - 1)) != 0) return;
    bitReverse_.resize(size);
    twiddles_.resize(size / 2);
    unsigned bits = 0;
    for (std::size_t n = size; n > 1; n >>= 1) ++bits;
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t reversed = 0;
        for (unsigned bit = 0; bit < bits; ++bit) reversed = (reversed << 1) | ((i >> bit) & 1);
        bitReverse_[i] = reversed;
    }
    for (std::size_t k = 0; k < size / 2; ++k) twiddles_[k] = std::polar(1.0, -2 * pi * k / size);
}
bool RealFft::forward(std::span<const double> input, std::span<std::complex<double>> output) {
    if (input.size() != size_ || output.size() < size_ / 2 + 1 || !size_) return false;
    try {
        pocketfft::r2c<double>({size_}, {sizeof(double)}, {sizeof(std::complex<double>)}, 0, true,
                               input.data(), output.data(), 1.0, 1);
        return true;
    } catch (...) {
        return false;
    }
}
bool RealFft::inverse(std::span<const std::complex<double>> input, std::span<double> output) {
    if (input.size() < size_ / 2 + 1 || output.size() != size_ || !size_) return false;
    try {
        pocketfft::c2r<double>({size_}, {sizeof(std::complex<double>)}, {sizeof(double)}, 0, false,
                               input.data(), output.data(), 1.0 / static_cast<double>(size_), 1);
        return true;
    } catch (...) {
        return false;
    }
}

bool RealFft::forwardRt(std::span<const double> input,
                        std::span<std::complex<double>> output) SUPPLE_RT {
    if (bitReverse_.size() != size_ || input.size() != size_ || output.size() < size_ / 2 + 1 ||
        !size_)
        return false;
    for (std::size_t i = 0; i < size_; ++i) scratch_[bitReverse_[i]] = {input[i], 0};
    for (std::size_t width = 2; width <= size_; width *= 2) {
        const std::size_t half = width / 2;
        for (std::size_t base = 0; base < size_; base += width)
            for (std::size_t j = 0; j < half; ++j) {
                const auto t = scratch_[base + j + half] * twiddles_[j * size_ / width];
                const auto u = scratch_[base + j];
                scratch_[base + j] = u + t;
                scratch_[base + j + half] = u - t;
            }
    }
    for (std::size_t k = 0; k <= size_ / 2; ++k) output[k] = scratch_[k];
    return true;
}
void OnePoleSmoother::prepare(double sampleRate, double timeSeconds) {
    coefficient_ = timeSeconds <= 0 ? 1 : -std::expm1(-1.0 / (sampleRate * timeSeconds));
}
DenormalGuard::DenormalGuard() SUPPLE_RT {
#if defined(__SSE2__) || defined(_M_X64)
    previous_ = _mm_getcsr();
    _mm_setcsr(static_cast<unsigned>(previous_) | 0x8040u);
#elif defined(__aarch64__)
    asm volatile("mrs %0, fpcr" : "=r"(previous_));
    const std::uint64_t flush = previous_ | (std::uint64_t{1} << 24);
    asm volatile("msr fpcr, %0" : : "r"(flush));
#endif
}
DenormalGuard::~DenormalGuard() SUPPLE_RT {
#if defined(__SSE2__) || defined(_M_X64)
    _mm_setcsr(static_cast<unsigned>(previous_));
#elif defined(__aarch64__)
    asm volatile("msr fpcr, %0" : : "r"(previous_));
#endif
}
}  // namespace supple::dsp
