// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "supple/base/rt.h"

namespace supple::dsp {

struct Coefficients {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
// Analog numerator/denominator coefficients, descending powers of s. Bilinear
// transform with frequency prewarping (Hz), normalized to a0=1.
Coefficients bilinear(std::array<double, 3> numerator, std::array<double, 3> denominator,
                      double sampleRate, double prewarpHz);
Coefficients lowpass(double sampleRate, double hz, double q);
Coefficients highpass(double sampleRate, double hz, double q);
Coefficients highShelf(double sampleRate, double hz, double gainDb, double q);
// BS.1770 analog prototype. At 48 kHz agrees with the published coefficients.
Coefficients kShelf(double sampleRate);
Coefficients kHighpass(double sampleRate);

class BiquadFilter {
  public:
    void set(Coefficients c) {
        c_ = c;
        reset();
    }
    void reset() SUPPLE_RT { s1_ = s2_ = 0; }
    double process(double x) SUPPLE_RT {
        const double y = c_.b0 * x + s1_;
        s1_ = c_.b1 * x - c_.a1 * y + s2_;
        s2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }
    Coefficients coefficients() const { return c_; }

  private:
    Coefficients c_;
    double s1_ = 0, s2_ = 0;
};

double hann(std::size_t i, std::size_t n);
double blackmanHarris(std::size_t i, std::size_t n);
double kaiser(std::size_t i, std::size_t n, double beta);

// Streaming linear-phase interpolator: 32 input taps per phase, Kaiser beta 8.6,
// sinc cutoff 0.94 of input Nyquist. Latency is 16 input samples. Zero padded at start.
class PolyphaseUpsampler {
  public:
    void prepare(int factor);
    void reset() SUPPLE_RT;
    int factor() const { return factor_; }
    void process(float input, std::span<double> output) SUPPLE_RT;

  private:
    int factor_ = 1;
    std::vector<double> coefficients_;
    std::array<double, 32> delay_{};
    int head_ = 0;
};

// Offline arbitrary-ratio, windowed-sinc polyphase resampling. At least 64 taps,
// scaled for downsampling ratio,
// Kaiser beta 10.5, sinc cutoff 0.90 of the lower Nyquist.
std::vector<float> resample(std::span<const float> input, double sourceRate, double targetRate);

class RealFft {
  public:
    void prepare(std::size_t size);
    std::size_t size() const { return size_; }
    // PocketFFT path for arbitrary sizes (offline; pocketfft allocates scratch).
    bool forward(std::span<const double> input, std::span<std::complex<double>> output);
    // Inverse of forward (offline, pocketfft): size/2 + 1 bins → size real
    // samples, scaled by 1/size so inverse(forward(x)) == x.
    bool inverse(std::span<const std::complex<double>> input, std::span<double> output);
    // Allocation-free radix-2 path for audio-thread use after prepare.
    bool forwardRt(std::span<const double> input, std::span<std::complex<double>> output) SUPPLE_RT;

  private:
    std::size_t size_ = 0;
    std::vector<std::complex<double>> scratch_, twiddles_;
    std::vector<std::size_t> bitReverse_;
};

class OnePoleSmoother {
  public:
    void prepare(double sampleRate, double timeSeconds);
    void reset(double value = 0) SUPPLE_RT { value_ = value; }
    double process(double target) SUPPLE_RT { return value_ += coefficient_ * (target - value_); }
    double value() const { return value_; }

  private:
    double coefficient_ = 1, value_ = 0;
};

class DenormalGuard {
  public:
    DenormalGuard() SUPPLE_RT;
    ~DenormalGuard() SUPPLE_RT;

  private:
    [[maybe_unused]] std::uint64_t previous_ = 0;
};

}  // namespace supple::dsp
