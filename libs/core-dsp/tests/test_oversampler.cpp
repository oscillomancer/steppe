// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>
#include "supple/dsp/oversampler.h"
using namespace supple::dsp;
namespace {
std::vector<double> impulse(int factor) {
    HalfbandOversampler os;
    os.prepare(factor, 4096);
    std::vector<double> out(512);
    std::array<double, 8> hi{};
    for (size_t n = 0; n < out.size(); ++n) {
        os.upsample(n == 0 ? 1 : 0, hi);
        out[n] = os.downsample(hi);
    }
    return out;
}
}  // namespace
TEST_CASE("oversampler exact integer latency and passband") {
    for (int f : {1, 2, 4, 8}) {
        HalfbandOversampler os;
        os.prepare(f);
        auto h = impulse(f);
        auto peak = static_cast<uint32_t>(std::max_element(h.begin(), h.end()) - h.begin());
        CHECK(peak == os.latency());
        double maximum = 0;
        for (int k = 0; k <= 90; ++k) {
            std::complex<double> sum{};
            for (size_t n = 0; n < h.size(); ++n)
                sum += h[n] * std::polar(1.0, -2 * std::numbers::pi * (k * .005) * n);
            maximum = std::max(maximum, std::abs(20 * std::log10(std::abs(sum))));
        }
        CHECK(maximum < .01);
        bool symmetric = true;
        for (size_t n = 1; n <= os.latency(); ++n)
            symmetric = (std::abs(h[os.latency() - n] - h[os.latency() + n]) < 1e-12) && symmetric;
        CHECK(symmetric);
    }
}
TEST_CASE("oversampler rejects interpolation images and downsampling aliases") {
    for (int f : {2, 4, 8})
        for (double hz : {.125, .45})
            for (int imageRate = 1; imageRate <= f / 2; imageRate *= 2) {
                HalfbandOversampler up, down;
                up.prepare(f);
                down.prepare(f);
                std::array<double, 8> hi{};
                std::complex<double> base{}, image{}, alias{};
                constexpr int count = 2000;  // Coherent for both frequencies.
                const double imageHz = imageRate - hz;
                for (int n = 0; n < count + 256; ++n) {
                    up.upsample(std::sin(2 * std::numbers::pi * hz * n), hi);
                    if (n >= 256)
                        for (int p = 0; p < f; ++p) {
                            const int t = n * f + p;
                            base += hi[p] * std::polar(1.0, -2 * std::numbers::pi * hz / f * t);
                            image +=
                                hi[p] * std::polar(1.0, -2 * std::numbers::pi * imageHz / f * t);
                        }
                    for (int p = 0; p < f; ++p)
                        hi[p] = std::sin(2 * std::numbers::pi * imageHz / f * (n * f + p));
                    const double y = down.downsample(hi);
                    if (n >= 256) alias += y * std::polar(1.0, -2 * std::numbers::pi * hz * n);
                }
                CAPTURE(f);
                CAPTURE(hz);
                CAPTURE(imageRate);
                CHECK(20 * std::log10(std::abs(image / base)) < -100);
                CHECK(20 * std::log10(std::abs(alias) * 2 / count) < -100);
            }
}
TEST_CASE("oversampler state is independent of block boundaries and reset") {
    for (int f : {1, 2, 4, 8}) {
        const auto reference = impulse(f);
        for (int block : {1, 7, 64, 4096}) {
            HalfbandOversampler os;
            os.prepare(f, 4096);
            std::array<double, 8> hi{};
            bool same = true;
            for (int start = 0; start < 512; start += block)
                for (int n = start; n < std::min(512, start + block); ++n) {
                    os.upsample(n == 0 ? 1 : 0, hi);
                    same = (os.downsample(hi) == reference[n]) && same;
                }
            CHECK(same);
            os.reset();
            os.upsample(1, hi);
            CHECK(os.downsample(hi) == reference[0]);
        }
    }
}
