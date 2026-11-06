// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Measurements for the sound modules' tests: frequency, level, spectra.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace supple::rack::test {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kC4 = 261.6255653005986;

// The octave from C4 of a frequency, as a pitch input carries it.
inline double octaveOf(double hz) { return std::log2(hz / kC4); }

// Frames [from, to) of a recording (to < 0: the end).
inline std::vector<double> slice(const std::vector<double>& y, std::size_t from,
                                 std::ptrdiff_t to = -1) {
    const std::size_t end = to < 0 ? y.size() : std::min(y.size(), static_cast<std::size_t>(to));
    return std::vector<double>(y.begin() + static_cast<std::ptrdiff_t>(from),
                               y.begin() + static_cast<std::ptrdiff_t>(end));
}

// The frequency from rising zero crossings (interpolated); 0 with fewer than two.
inline double frequency(const std::vector<double>& y, double sampleRate) {
    double first = -1, last = -1;
    int count = 0;
    for (std::size_t n = 1; n < y.size(); ++n)
        if (y[n - 1] < 0.0 && y[n] >= 0.0) {
            const double t = static_cast<double>(n - 1) + -y[n - 1] / (y[n] - y[n - 1]);
            if (first < 0) first = t;
            last = t;
            ++count;
        }
    if (count < 2) return 0.0;
    return (count - 1) / (last - first) * sampleRate;
}

inline double peak(const std::vector<double>& y) {
    double p = 0;
    for (double x : y) p = std::max(p, std::abs(x));
    return p;
}

inline double rms(const std::vector<double>& y) {
    if (y.empty()) return 0.0;
    double s = 0;
    for (double x : y) s += x * x;
    return std::sqrt(s / static_cast<double>(y.size()));
}

inline double mean(const std::vector<double>& y) {
    if (y.empty()) return 0.0;
    double s = 0;
    for (double x : y) s += x;
    return s / static_cast<double>(y.size());
}

// The amplitude of the component at hz (a sine of amplitude 1 reads 1), by a
// Hann-windowed correlation over the whole recording.
inline double amplitudeAt(const std::vector<double>& y, double hz, double sampleRate) {
    const std::size_t n = y.size();
    if (n < 2) return 0.0;
    double re = 0, im = 0, w = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / (n - 1));
        const double ph = 2 * kPi * hz * static_cast<double>(i) / sampleRate;
        re += y[i] * win * std::cos(ph);
        im += y[i] * win * std::sin(ph);
        w += win;
    }
    return 2.0 * std::hypot(re, im) / w;
}

// The power of DFT bin k of y (whole length, no window), for exactly periodic
// signals.
inline double binPower(const std::vector<double>& y, std::size_t k) {
    double re = 0, im = 0;
    const double n = static_cast<double>(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double ph = 2 * kPi * static_cast<double>(k) * static_cast<double>(i) / n;
        re += y[i] * std::cos(ph);
        im -= y[i] * std::sin(ph);
    }
    return re * re + im * im;
}

// For a signal exactly periodic in y's length with `cycles` cycles: the
// power off the harmonics over the power on them (aliases, noise), in dB.
inline double inharmonicDb(const std::vector<double>& y, std::size_t cycles) {
    double on = 0, off = 0;
    for (std::size_t k = 1; k < y.size() / 2; ++k) {
        const double p = binPower(y, k);
        (k % cycles == 0 ? on : off) += p;
    }
    return 10.0 * std::log10(std::max(off, 1e-30) / std::max(on, 1e-30));
}

// The magnitude response of an output to a unit sine at hz, after settling.
inline double gainAt(const std::vector<double>& y, double hz, double sampleRate,
                     std::size_t settle) {
    return amplitudeAt(slice(y, settle), hz, sampleRate);
}

}  // namespace supple::rack::test
