// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <vector>

#include "supple/dsp/dsp.h"

using namespace supple::dsp;
TEST_CASE("BS.1770 coefficients at 48 kHz") {
    auto s = kShelf(48000);
    CHECK(s.b0 == doctest::Approx(1.53512485958697).epsilon(1e-12));
    CHECK(s.b1 == doctest::Approx(-2.69169618940638).epsilon(1e-12));
    CHECK(s.b2 == doctest::Approx(1.19839281085285).epsilon(1e-12));
    CHECK(s.a1 == doctest::Approx(-1.69065929318241).epsilon(1e-12));
    CHECK(s.a2 == doctest::Approx(0.73248077421585).epsilon(1e-12));
    auto h = kHighpass(48000);
    CHECK(
        h.b0 ==
        doctest::Approx(
            1.0 / (1 + std::tan(std::numbers::pi * 38.13547087602444 / 48000) / 0.5003270373238773 +
                   std::pow(std::tan(std::numbers::pi * 38.13547087602444 / 48000), 2))));
}
TEST_CASE("DSP processing can run on RT thread") {
    DenormalGuard denormals;
    BiquadFilter f;
    f.set(lowpass(48000, 1000, 0.707));
    OnePoleSmoother smoother;
    smoother.prepare(48000, 0.01);
    PolyphaseUpsampler up;
    up.prepare(4);
    double output[4];
    for (int i = 0; i < 1000; ++i) {
        const double v = f.process(smoother.process(i == 0 ? 1 : 0));
        up.process(static_cast<float>(v), output);
    }
    CHECK(std::isfinite(output[0]));
}
TEST_CASE("FFT bin and resampler sine") {
    RealFft fft;
    fft.prepare(1024);
    std::vector<double> input(1024);
    for (int i = 0; i < 1024; ++i) input[i] = std::sin(2 * std::numbers::pi * 32 * i / 1024);
    std::vector<std::complex<double>> output(513);
    CHECK(fft.forward(input, output));
    std::vector<std::complex<double>> rtOutput(513);
    CHECK(fft.forwardRt(input, rtOutput));
    for (std::size_t k = 0; k < output.size(); ++k) CHECK(std::abs(output[k] - rtOutput[k]) < 1e-8);
    CHECK(std::abs(output[32]) == doctest::Approx(512).epsilon(1e-10));
    std::vector<float> src(48000);
    for (int i = 0; i < 48000; ++i)
        src[i] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * i / 48000));
    auto dst = resample(src, 48000, 44100);
    CHECK(dst.size() == 44100);
    double error = 0;
    for (int i = 100; i < 44000; ++i)
        error = std::max(error, std::abs(static_cast<double>(dst[static_cast<std::size_t>(i)]) -
                                         std::sin(2 * std::numbers::pi * 1000 * i / 44100)));
    CHECK(error < 0.002);
}
TEST_CASE("resampler passband and stopband quality") {
    auto measure = [](double hz) {
        std::vector<float> src(48000);
        for (int i = 0; i < 48000; ++i)
            src[static_cast<std::size_t>(i)] =
                static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / 48000));
        auto dst = resample(src, 48000, 24000);
        double power = 0;
        for (std::size_t i = 100; i < dst.size() - 100; ++i)
            power += static_cast<double>(dst[i]) * static_cast<double>(dst[i]);
        return 10 * std::log10(power / (dst.size() - 200) * 2);
    };
    double ripple = 0;
    for (double hz : {100.0, 1000.0, 2000.0, 4000.0, 6000.0, 8000.0})
        ripple = std::max(ripple, std::abs(measure(hz)));
    const double stop = measure(18000);
    std::cout << "resampler peak ripple 100 Hz-8 kHz=" << ripple << " dB, 18 kHz=" << stop
              << " dB\n";
    CHECK(ripple < 0.02);
    CHECK(stop < -60);
    auto highRatio = [](double hz) {
        std::vector<float> src(192000);
        for (int i = 0; i < 192000; ++i)
            src[static_cast<std::size_t>(i)] =
                static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / 192000));
        const auto dst = resample(src, 192000, 24000);
        double power = 0;
        for (std::size_t i = 100; i < dst.size() - 100; ++i)
            power += static_cast<double>(dst[i]) * static_cast<double>(dst[i]);
        return 10 * std::log10(power / (dst.size() - 200) * 2);
    };
    const double highPass = highRatio(8000), highStop = highRatio(40000);
    std::cout << "resampler 192k->24k 8k=" << highPass << " dB, 40k=" << highStop << " dB\n";
    CHECK(std::abs(highPass) < 0.02);
    CHECK(highStop < -70);
}
TEST_CASE("bilinear prewarp preserves analog cutoff") {
    const double w = 2 * std::numbers::pi * 1000;
    const auto c = bilinear({0, 0, w * w}, {1, w * std::sqrt(2.0), w * w}, 48000, 1000);
    const auto z = std::polar(1.0, -2 * std::numbers::pi * 1000 / 48000);
    const auto h = (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
    CHECK(20 * std::log10(std::abs(h)) == doctest::Approx(-3.01029995664).epsilon(1e-9));
}

TEST_CASE("resampler rejects nonfinite and unrepresentable rate ratios") {
    const std::array<float, 4> input{1, 0, 0, 0};
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        CHECK(resample(input, invalid, 48000).empty());
        CHECK(resample(input, 48000, invalid).empty());
    }
    CHECK(resample(input, 1e-300, 1e300).empty());
    CHECK(resample(input, 1e300, 1e-300).empty());
    CHECK(resample(input, 48000, 1e-20).empty());
}

TEST_CASE("polyphase upsampler ignores short output spans without advancing history") {
    PolyphaseUpsampler tested, reference;
    tested.prepare(4);
    reference.prepare(4);
    std::array<double, 3> shortOutput{123, 123, 123};
    tested.process(1, shortOutput);
    CHECK(shortOutput == std::array<double, 3>{123, 123, 123});
    std::array<double, 4> actual{}, expected{};
    tested.process(0.5f, actual);
    reference.process(0.5f, expected);
    CHECK(actual == expected);
}

#if defined(__aarch64__)
namespace {
std::uint64_t fpcr() {
    std::uint64_t value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}
void setFpcr(std::uint64_t value) { asm volatile("msr fpcr, %0" : : "r"(value)); }
}  // namespace
TEST_CASE("arm64 denormal guard sets FZ and restores the entire FPCR across nesting") {
    const auto saved = fpcr();
    constexpr std::uint64_t fz = std::uint64_t{1} << 24;
    const auto original = saved & ~fz;
    setFpcr(original);
    {
        DenormalGuard outer;
        CHECK(fpcr() == (original | fz));
        {
            DenormalGuard inner;
            setFpcr(original);  // a plug-in leaks its mode: inner restores outer
        }
        CHECK(fpcr() == (original | fz));
    }
    const auto restored = fpcr();
    setFpcr(saved);
    CHECK(restored == original);
}
#endif
