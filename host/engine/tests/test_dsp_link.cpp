// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Link both DSP implementations: core-dsp's and the template framework's,
// which the script runtime compiles. Classes of one qualified name in both
// break the one-definition rule: Windows rejects some duplicate symbols,
// while ELF keeps one copy of each inline member and silently runs it on the
// other class's objects (core-dsp's FFT destructor freed a script Stft's
// memory). core-dsp's names differ from the framework's; each case uses
// both sides' classes, so a clash that returns calls the wrong code here.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <complex>
#include <vector>

#include "supple/dsp/dsp.h"
#include "supple/dsp/oversampler.h"
#include "supple/script.h"

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST_CASE("engine and script oversamplers coexist and reset their own state") {
    supple::dsp::HalfbandOversampler engine;
    engine.prepare(2);
    std::array<double, 2> hi{};
    engine.upsample(1, hi);
    (void)engine.downsample(hi);
    engine.reset();
    engine.upsample(0, hi);
    CHECK(engine.downsample(hi) == 0);

    supple::script::Oversampler script;
    script.prepare(2);
    script.up(1);
    (void)script.down();
    script.clear();
    CHECK(script.factor() == 2);
    script.up(0);
    CHECK(script.down() == 0);
    script.up(0.5);
    CHECK(script.down() != 0);
}

TEST_CASE("engine FFT and script STFT coexist, each built, copied and destroyed whole") {
    // core-dsp's FFT: a sine on bin 8.
    {
        supple::dsp::RealFft fft;
        fft.prepare(256);
        std::vector<double> x(256);
        for (std::size_t n = 0; n < x.size(); ++n) x[n] = std::sin(2 * kPi * 8.0 * n / 256.0);
        std::vector<std::complex<double>> bins(129);
        REQUIRE(fft.forwardRt(x, bins));
        CHECK(std::abs(bins[8]) == doctest::Approx(128.0));
        CHECK(std::abs(bins[9]) < 1e-9);
    }
    // The script runtime's STFT over the framework's FFT: what goes in comes
    // back, a latency later, from the original and from a copy.
    supple::script::Stft stft;
    stft.prepare(1024, 4);
    const int late = stft.latency();
    std::vector<double> in, out;
    for (int n = 0; n < 8192; ++n) {
        in.push_back(std::sin(2 * kPi * 440.0 * n / 48000.0));
        (void)stft.push(in.back());
        out.push_back(stft.pull());
    }
    for (int n = 4096; n < 8192; n += 97) CHECK(out[n] == doctest::Approx(in[n - late]));
    {
        supple::script::Stft copy(stft);
        supple::script::Stft assigned;
        assigned = copy;
        CHECK(assigned.size() == 1024);
    }
    CHECK(stft.bins() == 513);
}

TEST_CASE("engine and script filters coexist and filter by their own state") {
    // core-dsp's smoother and biquad.
    supple::dsp::OnePoleSmoother smoother;
    smoother.prepare(48000.0, 0.01);
    for (int n = 0; n < 480; ++n) (void)smoother.process(1.0);
    CHECK(smoother.value() == doctest::Approx(1.0 - std::exp(-1.0)).epsilon(0.01));
    supple::dsp::BiquadFilter low;
    low.set(supple::dsp::lowpass(48000.0, 1000.0, 0.7071));
    double y = 0.0;
    for (int n = 0; n < 4800; ++n) y = low.process(1.0);
    CHECK(y == doctest::Approx(1.0).epsilon(1e-6));

    // The script runtime's, over the framework's: a one-pole and a biquad
    // pass DC through their lowpasses.
    supple::script::OnePole pole;
    pole.setCutoff(1000.0);
    double p = 0.0;
    for (int n = 0; n < 4800; ++n) p = pole.lowpass(1.0);
    CHECK(p == doctest::Approx(1.0).epsilon(1e-6));
    supple::script::Biquad biquad;
    biquad.lowpass(1000.0, 0.7071);
    double b = 0.0;
    for (int n = 0; n < 4800; ++n) b = biquad.process(1.0);
    CHECK(b == doctest::Approx(1.0).epsilon(1e-6));
}
