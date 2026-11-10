// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The DSP library (framework/dsp), measured: each block against what it
// claims -- a response, a delay, a level, an alias floor -- and against a
// slow, obvious version of itself where there is one. None of it depends on
// the product.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "core/Fft.h"
#include "core/Realtime.h"
#include "dsp/AudioFile.h"
#include "dsp/Convolution.h"
#include "dsp/Dynamics.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"
#include "dsp/Interpolation.h"
#include "dsp/Loudness.h"
#include "dsp/Oscillators.h"
#include "dsp/Oversampler.h"
#include "dsp/Shapers.h"
#include "dsp/Stft.h"
#include "dsp/Voices.h"
#include "dsp/Worker.h"
#include "tools/wav.h"
#include "tools/Timing.h"

using namespace supple;
using namespace supple::dsp;

namespace {

constexpr double kRate = 48000.0;

std::vector<double> sine(double hz, int frames, double amplitude = 1.0, double rate = kRate, double phase = 0.0) {
    std::vector<double> x(static_cast<size_t>(frames));
    for (int n = 0; n < frames; ++n) x[static_cast<size_t>(n)] = amplitude * std::sin(2.0 * kPi * hz * n / rate + phase);
    return x;
}

// The amplitude of the component at hz, over x[from..]: a projection, exact
// for a whole number of cycles.
double amplitudeAt(const std::vector<double>& x, double hz, size_t from = 0, double rate = kRate) {
    std::complex<double> sum = 0.0;
    for (size_t n = from; n < x.size(); ++n) sum += x[n] * std::polar(1.0, -2.0 * kPi * hz * static_cast<double>(n) / rate);
    return 2.0 * std::abs(sum) / static_cast<double>(x.size() - from);
}

std::vector<double> noise(int frames, uint64_t seed = 3) {
    Random r(seed);
    std::vector<double> x(static_cast<size_t>(frames));
    for (double& v : x) v = r.bipolar();
    return x;
}

// Of a periodic signal whose fundamental sits on bin `bin` of an n-point
// FFT: the energy off the harmonics' bins over the energy on them.
double aliasRatio(const std::vector<double>& x, size_t bin) {
    const size_t n = x.size();
    Fft fft(n);
    std::vector<std::complex<double>> s(x.begin(), x.end());
    fft.forward(s.data());
    double on = 0.0, off = 0.0;
    for (size_t k = 1; k <= n / 2; ++k) ((k % bin == 0) ? on : off) += std::norm(s[k]);
    return off / on;
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-300)); }

std::filesystem::path scratchFolder(const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                    ("supple-dsp-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / name;
    std::filesystem::create_directories(p);
    return p;
}

}  // namespace

// --- filters ----------------------------------------------------------------------

namespace {
template <class F>
void runPrepared(F&& run) SUPPLE_REALTIME {
    run();
}
}

TEST(prepared_primitives_run_inside_a_realtime_context) {
    Fft fft(64);
    std::complex<double> bins[64] = {};
    Stft stft, looking;
    stft.prepare(1, 64);
    looking.prepare(1, 64, 4, Stft::Mode::Analyse);
    Convolution convolution;
    const double impulse[65] = {1.0};
    const double* ir[] = {impulse};
    convolution.prepare(ir, 1, 65, 1, 16);
    FractionalDelay delay;
    delay.prepare(32);
    AllpassDelay allpass;
    allpass.prepare(32);
    Loudness loudness;
    loudness.prepare(kRate, 1);
    TruePeak peak;
    TruePeak::State peakState;
    Adsr envelope;
    envelope.setTimes(1, 10, 0.5, 10, kRate);
    Biquad filter = Biquad::lowpass(1000, 0.707, kRate);
    Biquad::State filterState;
    Adaa<TanhShape>::State shaper;
    Handoff<int> handoff;
    handoff.publish(std::make_unique<int>(7));
    double checksum = 0.0;
    for (auto phase : {Oversampler::Phase::Linear, Oversampler::Phase::Minimum}) {
        Oversampler over;
        over.prepare(1, 64, 4, phase);
        runPrepared([&] {
            envelope.gate(true);
            handoff.update();
            for (int block = 0; block < 16; ++block) {
                double audio[64];
                for (double& x : audio) {
                    x = filter.tick(filterState, envelope.tick());
                    x = delay.process(x, 4.5);
                    x = allpass.process(x, 4.5);
                    loudness.tick(&x);
                    checksum += peak.tick(peakState, x);
                }
                double* high = over.up(0, audio, 64);
                for (int n = 0; n < 256; ++n) high[n] = Adaa<TanhShape>::tick(shaper, high[n]);
                over.down(0, audio, 64);
                convolution.process(0, audio, 64);
                stft.process(0, audio, 64, [](std::complex<double>*, int) {});
                looking.process(0, audio, 64, [](std::complex<double>*, int) {});
                bins[0] = audio[0];
                fft.forward(bins);
                fft.inverse(bins);
                checksum += bins[0].real();
            }
            over.reset();
            convolution.reset();
            stft.reset();
            looking.reset();
            delay.clear();
            allpass.clear();
            loudness.reset();
        });
    }
    CHECK(std::isfinite(checksum) && checksum > 0.0);
    CHECK(handoff.get() && *handoff.get() == 7);
}

TEST(biquads_do_what_their_responses_say) {
    const Biquad designs[] = {
        Biquad::lowpass(1000.0, 0.7071, kRate),       Biquad::highpass(1000.0, 0.7071, kRate),
        Biquad::bandpass(1000.0, 2.0, kRate),         Biquad::notch(1000.0, 2.0, kRate),
        Biquad::allpass(1000.0, 0.7071, kRate),       Biquad::peak(1000.0, 1.0, 6.0, kRate),
        Biquad::lowShelf(1000.0, 0.7071, 6.0, kRate), Biquad::highShelf(1000.0, 0.7071, -6.0, kRate),
        Biquad::lowpass1(1000.0, kRate),              Biquad::highpass1(1000.0, kRate),
    };
    for (const Biquad& f : designs) {
        for (double hz : {100.0, 1000.0, 5000.0}) {
            std::vector<double> x = sine(hz, 48000);
            Biquad::State s;
            f.process(s, x.data(), static_cast<int>(x.size()));
            CHECK_NEAR(amplitudeAt(x, hz, 24000), std::abs(f.response(hz, kRate)), 1e-4);
        }
    }
    // The designs' own landmarks.
    CHECK_NEAR(std::abs(Biquad::lowpass(1000.0, std::sqrt(0.5), kRate).response(1000.0, kRate)), std::sqrt(0.5), 1e-9);
    CHECK_NEAR(std::abs(Biquad::peak(1000.0, 1.0, 6.0, kRate).response(1000.0, kRate)), dbToGain(6.0), 1e-9);
    CHECK_NEAR(std::abs(Biquad::lowShelf(1000.0, 0.7071, 6.0, kRate).response(0.0, kRate)), dbToGain(6.0), 1e-9);
    CHECK_NEAR(std::abs(Biquad::highShelf(1000.0, 0.7071, 6.0, kRate).response(0.5 * kRate, kRate)), dbToGain(6.0), 1e-9);
    CHECK_NEAR(std::abs(Biquad::notch(1000.0, 2.0, kRate).response(1000.0, kRate)), 0.0, 1e-9);
    for (double hz : {20.0, 1000.0, 20000.0})
        CHECK_NEAR(std::abs(Biquad::allpass(1000.0, 0.7071, kRate).response(hz, kRate)), 1.0, 1e-12);
}

TEST(butterworth_is_the_analog_prototype_through_the_bilinear_transform) {
    const double fc = 2000.0;
    for (int order = 1; order <= 16; ++order) {
        const Cascade lp = Butterworth::lowpass(order, fc, kRate), hp = Butterworth::highpass(order, fc, kRate);
        CHECK(lp.sections == (order + 1) / 2);
        for (double hz : {50.0, 1000.0, 2000.0, 4000.0, 15000.0}) {
            const double w = std::tan(kPi * hz / kRate) / std::tan(kPi * fc / kRate);
            CHECK_NEAR(std::abs(lp.response(hz, kRate)), 1.0 / std::sqrt(1.0 + std::pow(w, 2 * order)), 1e-9);
            CHECK_NEAR(std::abs(hp.response(hz, kRate)), 1.0 / std::sqrt(1.0 + std::pow(1.0 / w, 2 * order)), 1e-9);
        }
    }
    // And in time, a high order included.
    const Cascade lp = Butterworth::lowpass(8, fc, kRate);
    std::vector<double> x = sine(3000.0, 48000);
    Cascade::State s;
    lp.process(s, x.data(), static_cast<int>(x.size()));
    CHECK_NEAR(amplitudeAt(x, 3000.0, 24000), std::abs(lp.response(3000.0, kRate)), 1e-6);
}

TEST(linkwitz_riley_halves_sum_to_an_allpass) {
    for (int order : {4, 8}) {
        const Cascade lp = LinkwitzRiley::lowpass(order, 800.0, kRate), hp = LinkwitzRiley::highpass(order, 800.0, kRate);
        CHECK_NEAR(std::abs(lp.response(800.0, kRate)), 0.5, 1e-9);  // -6 dB at the split
        for (double hz : {20.0, 200.0, 800.0, 3000.0, 18000.0})
            CHECK_NEAR(std::abs(lp.response(hz, kRate) + hp.response(hz, kRate)), 1.0, 1e-9);
    }
    // The crossover's bands, summed in time, are the input through that all-pass.
    Crossover x;
    x.setCoefficients(800.0, kRate, 4);
    Crossover::State s;
    std::vector<double> in = sine(800.0, 48000), sum(in.size());
    for (size_t n = 0; n < in.size(); ++n) {
        const Crossover::Bands b = x.tick(s, in[n]);
        sum[n] = b.low + b.high;
    }
    CHECK_NEAR(amplitudeAt(sum, 800.0, 24000), 1.0, 1e-6);
}

TEST(one_pole_outputs_follow_their_responses) {
    OnePole f;
    f.setCutoff(500.0, kRate);
    for (double hz : {50.0, 500.0, 5000.0}) {
        std::vector<double> x = sine(hz, 48000), lo(x.size()), hi(x.size()), all(x.size());
        OnePole::State s;
        for (size_t n = 0; n < x.size(); ++n) {
            const OnePole::Outputs o = f.tick(s, x[n]);
            lo[n] = o.low;
            hi[n] = o.high;
            all[n] = o.all;
        }
        CHECK_NEAR(amplitudeAt(lo, hz, 24000), std::abs(f.lowpassResponse(hz, kRate)), 1e-6);
        CHECK_NEAR(amplitudeAt(hi, hz, 24000), std::abs(f.highpassResponse(hz, kRate)), 1e-6);
        CHECK_NEAR(amplitudeAt(all, hz, 24000), 1.0, 1e-6);
    }
    CHECK_NEAR(std::abs(f.lowpassResponse(500.0, kRate)), std::sqrt(0.5), 1e-12);
}

TEST(dc_blocker_removes_dc_and_keeps_the_rest) {
    DcBlocker b;
    b.setCutoff(10.0, kRate);
    DcBlocker::State s;
    double y = 0.0;
    for (int n = 0; n < 48000; ++n) y = b.tick(s, 1.0);
    CHECK(std::fabs(y) < 1e-6);
    CHECK_NEAR(std::abs(b.response(1000.0, kRate)), 1.0, 1e-3);
    CHECK_NEAR(std::abs(b.response(0.0, kRate)), 0.0, 1e-12);
}

// --- dynamics ----------------------------------------------------------------------

TEST(gain_computer_has_the_knee_and_slope_it_declares) {
    GainComputer g{-20.0, 4.0, 6.0};
    CHECK_NEAR(g.gainDb(-40.0), 0.0, 1e-12);
    CHECK_NEAR(g.gainDb(-23.0), 0.0, 1e-12);       // the knee's lower edge
    CHECK_NEAR(g.gainDb(-20.0), -0.5625, 1e-12);   // its middle: slope * (knee / 2)^2 / (2 knee)
    CHECK_NEAR(g.gainDb(-17.0), -2.25, 1e-12);     // its upper edge meets the line
    CHECK_NEAR(g.gainDb(-10.0), -7.5, 1e-12);      // 10 dB over at 4:1
    GainComputer limiter{-1.0, std::numeric_limits<double>::infinity(), 0.0};
    CHECK_NEAR(limiter.gainDb(5.0), -6.0, 1e-12);
    // Continuous and never rising, across the knee.
    double last = 0.0;
    for (double level = -40.0; level < 0.0; level += 0.01) {
        const double gain = g.gainDb(level);
        CHECK(gain <= last + 1e-12);
        CHECK(std::fabs(gain - last) < 0.01);
        last = gain;
    }
}

TEST(followers_reach_one_over_e_in_their_time) {
    EnvelopeFollower f;
    f.setTimes(10.0, 100.0, kRate);
    EnvelopeFollower::State s;
    double e = 0.0;
    for (int n = 0; n < 480; ++n) e = f.tick(s, 1.0);
    CHECK_NEAR(e, 1.0 - std::exp(-1.0), 1e-9);
    for (int n = 0; n < 4800; ++n) e = f.tick(s, 0.0);
    CHECK_NEAR(e, (1.0 - std::exp(-1.0)) * std::exp(-1.0), 1e-9);

    RmsFollower r;
    r.setTime(50.0, kRate);
    RmsFollower::State rs;
    double rms = 0.0;
    for (double v : sine(1000.0, 48000)) rms = r.tick(rs, v);
    CHECK_NEAR(rms, std::sqrt(0.5), 0.01);
}

TEST(sliding_max_is_the_window_maximum) {
    SlidingMax m;
    m.prepare(100);
    const std::vector<double> x = noise(5000);
    for (int window : {1, 7, 100}) {
        m.setWindow(window);
        m.clear();
        for (size_t n = 0; n < x.size(); ++n) {
            const double got = m.push(x[n]);
            double want = -1e300;
            for (size_t k = n + 1 > static_cast<size_t>(window) ? n + 1 - static_cast<size_t>(window) : 0; k <= n; ++k)
                want = std::max(want, x[k]);
            CHECK(got == want);
        }
    }
}

TEST(lookahead_gain_is_never_above_what_the_delayed_audio_needs) {
    Lookahead l;
    l.prepare(256);
    l.setLength(64);
    Random r(11);
    std::vector<double> needed(20000, 1.0);
    for (size_t n = 0; n < needed.size(); ++n)
        if (r.uniform() < 0.01) needed[n] = 0.2 + 0.7 * r.uniform();
    double previous = 1.0;
    for (size_t n = 0; n < needed.size(); ++n) {
        const double g = l.tick(needed[n]);
        if (n >= 64) CHECK(g <= needed[n - 64] + 1e-12);
        CHECK(std::fabs(g - previous) <= 1.0 / 64.0 + 1e-12);  // no faster than a ramp over the lookahead
        previous = g;
    }
}

TEST(true_peak_finds_the_peak_between_samples) {
    TruePeak meter;
    TruePeak::State s;
    // A quarter of the sample rate, 45 degrees off: every sample is 0.707.
    double samplePeak = 0.0, truePeak = 0.0;
    const std::vector<double> x = sine(kRate / 4.0, 9600, 1.0, kRate, kPi / 4.0);
    for (size_t n = 0; n < x.size(); ++n) {
        const double p = meter.tick(s, x[n]);
        if (n > 100) {
            samplePeak = std::max(samplePeak, std::fabs(x[n]));
            truePeak = std::max(truePeak, p);
        }
    }
    CHECK_NEAR(samplePeak, std::sqrt(0.5), 1e-9);
    CHECK_MSG(std::fabs(db(truePeak)) < 0.3, "true peak %.3f dB", db(truePeak));
    // An ordinary tone reads its own peak.
    TruePeak::State t;
    double p = 0.0;
    for (double v : sine(997.0, 9600, 0.5)) p = std::max(p, meter.tick(t, v));
    CHECK_NEAR(db(p), db(0.5), 0.1);
}

TEST(true_peak_keeps_sample_peaks_and_measures_the_high_band_at_every_phase) {
    TruePeak meter;
    double worst = 0.0;
    for (int f = 1; f <= 91; ++f) {
        const double frequency = 0.005 * f;
        for (int phase = 0; phase < 32; ++phase) {
            TruePeak::State state;
            double peak = 0.0;
            for (int n = 0; n < 1080; ++n) {
                const double x = std::sin(2.0 * kPi * (frequency * n + phase / 32.0));
                const double p = meter.tick(state, x);
                CHECK(p >= std::fabs(x));
                if (n >= 80) peak = std::max(peak, p);
            }
            worst = std::min(worst, db(peak));
            CHECK_MSG(db(peak) >= -0.5, "f/fs %.3f phase %d: %.3f dB", frequency, phase, db(peak));
        }
    }
    std::printf("      worst true peak under-read %.3f dB through 0.455 fs\n", worst);
}

namespace {
struct TestClock {
    using duration = std::chrono::duration<double>;
    using time_point = std::chrono::time_point<TestClock>;
    static inline double seconds = 0.0;
    static time_point now() { return time_point(duration(seconds)); }
};
}

TEST(benchmark_timing_excludes_every_trials_preparation) {
    int trials = 0;
    const double elapsed = tools::fastestPrepared<TestClock>(5, [&] {
        TestClock::seconds += 100.0;
        ++trials;
    }, [&] {
        TestClock::seconds += 2.0 + trials;
    });
    CHECK(trials == 5);
    CHECK_NEAR(elapsed, 3.0, 1e-12);
}

// --- oversampling ---------------------------------------------------------------------

TEST(linear_phase_oversampling_is_a_pure_delay_in_the_passband) {
    for (int factor : {2, 4, 8, 16}) {
        Oversampler o;
        o.prepare(1, 100, factor);
        CHECK(o.factor() == factor);
        std::vector<double> in(9600), out(in.size());
        for (size_t n = 0; n < in.size(); ++n)
            in[n] = 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kRate) +
                    0.3 * std::sin(2.0 * kPi * 15000.0 * static_cast<double>(n) / kRate);
        for (size_t at = 0; at < in.size(); at += 100) {
            o.up(0, in.data() + at, 100);
            o.down(0, out.data() + at, 100);
        }
        double worst = 0.0;
        for (size_t n = 3000; n < in.size(); ++n) worst = std::max(worst, std::fabs(out[n] - in[n - static_cast<size_t>(o.latency())]));
        CHECK_MSG(worst < 1e-4, "%dx: latency %d, error %.3g", factor, o.latency(), worst);
    }
}

TEST(oversampling_keeps_images_and_aliases_out) {
    for (Oversampler::Phase phase : {Oversampler::Phase::Linear, Oversampler::Phase::Minimum}) {
        const char* kind = phase == Oversampler::Phase::Linear ? "linear" : "minimum";
        // Up: a 10 kHz tone at 2x has no image at 38 kHz.
        Oversampler o;
        o.prepare(1, 4800, 2, phase);
        const std::vector<double> tone = sine(10000.0, 4800);
        const double* hi = o.up(0, tone.data(), 4800);
        std::vector<double> high(hi, hi + 9600);
        const double signal = amplitudeAt(high, 10000.0, 4800, 2.0 * kRate);
        const double image = amplitudeAt(high, 38000.0, 4800, 2.0 * kRate);
        CHECK_MSG(std::fabs(db(signal)) < 0.01, "%s: tone %.4f dB", kind, db(signal));
        CHECK_MSG(db(image) < -95.0, "%s: image %.1f dB", kind, db(image));
        // Down: 30 kHz made at 2x does not fold to 18 kHz.
        Oversampler d;
        d.prepare(1, 4800, 2, phase);
        std::vector<double> base(4800), out(4800);
        double* work = d.up(0, base.data(), 4800);
        const std::vector<double> above = sine(30000.0, 9600, 1.0, 2.0 * kRate);
        std::copy(above.begin(), above.end(), work);
        d.down(0, out.data(), 4800);
        const double alias = amplitudeAt(out, 18000.0, 2400);
        CHECK_MSG(db(alias) < -95.0, "%s: alias %.1f dB", kind, db(alias));
    }
    // Minimum phase is short.
    Oversampler m;
    m.prepare(1, 64, 4, Oversampler::Phase::Minimum);
    CHECK(m.latency() >= 0 && m.latency() < 8);
}

// --- spectral ------------------------------------------------------------------------

TEST(fft_inverse_undoes_forward) {
    Fft fft(256);
    std::vector<std::complex<double>> x(256), y;
    Random r(5);
    for (auto& v : x) v = {r.bipolar(), r.bipolar()};
    y = x;
    fft.forward(y.data());
    fft.inverse(y.data());
    for (size_t k = 0; k < x.size(); ++k) CHECK(std::abs(x[k] - y[k]) < 1e-12);
}

TEST(stft_that_changes_nothing_is_a_delay) {
    for (int overlap : {4, 8}) {
        Stft stft;
        stft.prepare(1, 512, overlap);
        const std::vector<double> in = noise(6000);
        std::vector<double> out = in;
        for (size_t at = 0; at < out.size(); at += 37)
            stft.process(0, out.data() + at, static_cast<int>(std::min<size_t>(37, out.size() - at)), [](std::complex<double>*, int) {});
        double worst = 0.0;
        for (size_t n = static_cast<size_t>(stft.latency()); n < in.size(); ++n)
            worst = std::max(worst, std::fabs(out[n] - in[n - static_cast<size_t>(stft.latency())]));
        CHECK_MSG(worst < 1e-12, "overlap %d: error %.3g", overlap, worst);
    }
}

TEST(stft_bins_filter_what_they_say) {
    Stft stft;
    stft.prepare(1, 1024);
    std::vector<double> x(48000);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / kRate) + std::sin(2.0 * kPi * 9000.0 * static_cast<double>(n) / kRate);
    constexpr int cut = static_cast<int>(5000.0 / (kRate / 1024.0));
    stft.process(0, x.data(), static_cast<int>(x.size()), [](std::complex<double>* bins, int count) {
        for (int k = cut; k < count; ++k) bins[k] = 0.0;
    });
    CHECK_NEAR(amplitudeAt(x, 1000.0, 24000), 1.0, 1e-3);
    CHECK(db(amplitudeAt(x, 9000.0, 24000)) < -60.0);
}

TEST(stft_a_sample_at_a_time_is_the_callback_form) {
    Stft a, b;
    a.prepare(1, 256);
    b.prepare(1, 256);
    std::vector<double> x = noise(3000), y = x;
    const auto change = [](std::complex<double>* bins, int count) {
        for (int k = count / 3; k < count; ++k) bins[k] *= 0.25;
    };
    a.process(0, x.data(), static_cast<int>(x.size()), change);
    for (double& v : y) {
        if (b.push(0, v)) change(b.spectrum(0), b.bins());
        v = b.pull(0);
    }
    CHECK(x == y);
}

TEST(stft_analysing_sees_the_same_frames_and_leaves_the_audio) {
    for (int overlap : {4, 8}) {
        Stft full, look;
        full.prepare(1, 256, overlap);
        look.prepare(1, 256, overlap, Stft::Mode::Analyse);
        CHECK(look.mode() == Stft::Mode::Analyse);
        const std::vector<double> x = noise(3000);
        int frames = 0;
        bool same = true;
        for (double v : x) {
            const bool ready = full.push(0, v);
            // push() alone: an analyser needs no pull().
            CHECK(look.push(0, v) == ready);
            if (ready) {
                ++frames;
                for (int k = 0; k < full.bins(); ++k) same = same && full.spectrum(0)[k] == look.spectrum(0)[k];
            }
            full.pull(0);
        }
        CHECK(frames == static_cast<int>(x.size()) / look.hop());
        CHECK_MSG(same, "overlap %d", overlap);
        std::vector<double> y = x;
        look.reset();
        look.process(0, y.data(), static_cast<int>(y.size()), [](std::complex<double>*, int) {});
        CHECK(y == x);
        CHECK(look.pull(0) == 0.0);
    }
}

// --- sources ------------------------------------------------------------------------

TEST(random_is_the_same_everywhere) {
    Random a(42), b(42), c(43), u(44);
    bool differs = false;
    double sum = 0.0;
    for (int n = 0; n < 100000; ++n) {
        const uint32_t x = a.next();
        CHECK(x == b.next());
        differs = differs || x != c.next();
        sum += u.uniform();
    }
    CHECK(differs);
    CHECK_NEAR(sum / 100000.0, 0.5, 0.01);
    // Reseeding starts the sequence again.
    a.reseed(42);
    Random d(42);
    for (int n = 0; n < 10; ++n) CHECK(a.next() == d.next());
}

TEST(polyblep_shapes_alias_far_less_than_naive_ones) {
    // 1875 Hz at 48 kHz: the fundamental on bin 640 of 16384, periodic.
    const size_t n = 16384, bin = 640;
    const double hz = kRate * bin / n;
    const std::pair<Oscillator::Shape, double (*)(double)> shapes[] = {
        {Oscillator::Shape::Saw, [](double t) { return 2.0 * t - 1.0; }},
        {Oscillator::Shape::Square, [](double t) { return t < 0.5 ? 1.0 : -1.0; }},
        {Oscillator::Shape::Triangle, [](double t) { return 1.0 - 4.0 * std::fabs(t - 0.5); }},
    };
    for (const auto& [shape, naive] : shapes) {
        Oscillator o;
        o.setShape(shape);
        o.setFrequency(hz, kRate);
        std::vector<double> blep(n), plain(n);
        for (size_t k = 0; k < n; ++k) {
            plain[k] = naive(o.phase());
            blep[k] = o.tick();
        }
        const double gain = db(aliasRatio(plain, bin)) - db(aliasRatio(blep, bin));
        CHECK_MSG(gain > 10.0, "shape %d: %.1f dB less aliasing", static_cast<int>(shape), gain);
    }
    // The saw's fundamental is the naive one's: 2 / pi.
    Oscillator saw;
    saw.setFrequency(hz, kRate);
    std::vector<double> x(n);
    for (double& v : x) v = saw.tick();
    CHECK_NEAR(amplitudeAt(x, hz), 2.0 / kPi, 0.01);
}

TEST(wavetable_plays_any_pitch_without_aliasing) {
    std::vector<double> cycle(2048);
    for (size_t k = 0; k < cycle.size(); ++k) cycle[k] = 2.0 * static_cast<double>(k) / 2048.0 - 1.0;  // a naive saw
    Wavetable table;
    table.build(cycle.data(), static_cast<int>(cycle.size()), kRate);
    for (size_t bin : {16u, 160u, 1600u}) {
        const size_t n = 16384;
        const double hz = kRate * static_cast<double>(bin) / n;
        Phasor p;
        p.setFrequency(hz, kRate);
        std::vector<double> x(n);
        for (double& v : x) v = table.tick(p, hz);
        CHECK_MSG(db(aliasRatio(x, bin)) < -70.0, "%.1f Hz: %.1f dB", hz, db(aliasRatio(x, bin)));
        CHECK_NEAR(amplitudeAt(x, hz), 2.0 / kPi, 2e-3);
    }
    // Additive: one harmonic is a sine.
    const double amplitudes[] = {1.0};
    Wavetable s;
    s.buildAdditive(amplitudes, 1, kRate);
    CHECK_NEAR(s.sample(0.25, 100.0), 1.0, 1e-9);
    CHECK_NEAR(s.sample(0.0, 100.0), 0.0, 1e-9);
}

TEST(lfo_shapes_phases_and_sync) {
    Lfo l;
    l.setRate(1.0, 1000.0);
    double v = 0.0;
    for (int n = 0; n <= 250; ++n) v = l.tick();
    CHECK_NEAR(v, 1.0, 1e-9);  // a quarter of the way: the sine's top
    l.setShape(Lfo::Shape::Triangle);
    l.reset(0.25);
    CHECK_NEAR(l.tick(), 1.0, 1e-9);
    l.syncTo(3.0, 2.0);
    CHECK_NEAR(l.phase(), 0.5, 1e-12);
    // Sample and hold: one value a cycle, the same sequence from the same seed.
    Lfo a, b;
    a.setShape(Lfo::Shape::SampleAndHold);
    b.setShape(Lfo::Shape::SampleAndHold);
    a.setRate(10.0, 1000.0);
    b.setRate(10.0, 1000.0);
    int changes = 0;
    double last = a.tick();
    CHECK(last == b.tick());
    for (int n = 1; n < 1000; ++n) {
        const double x = a.tick();
        CHECK(x == b.tick());
        if (x != last) ++changes;
        last = x;
    }
    CHECK(changes == 9 || changes == 10);
}

TEST(random_lfo_changes_at_the_start_of_the_next_cycle) {
    Random random(7);
    double previous = 0.0, current = random.bipolar();
    Lfo held, smooth;
    held.setShape(Lfo::Shape::SampleAndHold);
    smooth.setShape(Lfo::Shape::SmoothRandom);
    // An exactly representable increment puts the boundary on sample 8.
    held.setRate(1.0, 8.0);
    smooth.setRate(1.0, 8.0);
    for (int n = 0; n < 32; ++n) {
        if (n > 0 && n % 8 == 0) {
            previous = current;
            current = random.bipolar();
        }
        const double phase = (n % 8) / 8.0;
        const double ease = 0.5 - 0.5 * std::cos(kPi * phase);
        CHECK(held.tick() == current);
        CHECK_NEAR(smooth.tick(), previous + (current - previous) * ease, 1e-12);
    }
}

TEST(noise_colours_have_their_slopes) {
    Noise n(9);
    const size_t size = 1 << 16;
    std::vector<double> white(size), pink(size);
    for (size_t k = 0; k < size; ++k) {
        white[k] = n.white();
        pink[k] = n.pink();
    }
    auto band = [&](const std::vector<double>& x, double lo, double hi) {
        Fft fft(size);
        std::vector<std::complex<double>> s(x.begin(), x.end());
        fft.forward(s.data());
        double e = 0.0;
        for (size_t k = static_cast<size_t>(lo * size / kRate); k < static_cast<size_t>(hi * size / kRate); ++k) e += std::norm(s[k]);
        return e;
    };
    // White: equal energy per hertz; pink: per octave.
    CHECK_NEAR(10.0 * std::log10(band(white, 4000.0, 8000.0) / band(white, 500.0, 1000.0)), 9.03, 1.0);
    CHECK_NEAR(10.0 * std::log10(band(pink, 4000.0, 8000.0) / band(pink, 500.0, 1000.0)), 0.0, 1.0);
}

// --- envelope ------------------------------------------------------------------------

TEST(adsr_segments_take_their_times) {
    Adsr env;
    env.setTimes(10.0, 20.0, 0.5, 40.0, kRate);
    env.gate(true);
    int n = 0;
    while (env.stage() == Adsr::Stage::Attack) env.tick(), ++n;
    CHECK(n >= 480 && n <= 481);
    n = 0;
    while (env.stage() == Adsr::Stage::Decay) env.tick(), ++n;
    CHECK(n >= 480 && n <= 481);  // half the range, half the time
    for (int k = 0; k < 100; ++k) CHECK(env.tick() == 0.5);
    env.gate(false);
    n = 0;
    while (env.active()) env.tick(), ++n;
    CHECK(n >= 960 && n <= 961);
    CHECK(env.tick() == 0.0);

    // Curved: the same times, a different shape.
    Adsr curved;
    curved.setTimes(10.0, 20.0, 0.0, 40.0, kRate);
    curved.setCurves(0.8, 0.8);
    curved.gate(true);
    n = 0;
    double atQuarter = 0.0;
    while (curved.stage() == Adsr::Stage::Attack) {
        const double v = curved.tick();
        if (++n == 120) atQuarter = v;
    }
    CHECK(n >= 479 && n <= 481);
    CHECK(atQuarter > 0.25);  // bowed up
    // A gate while releasing starts the attack from where it is: no jump.
    Adsr again;
    again.setTimes(5.0, 5.0, 1.0, 50.0, kRate);
    again.gate(true);
    for (int k = 0; k < 1000; ++k) again.tick();
    again.gate(false);
    for (int k = 0; k < 600; ++k) again.tick();
    const double before = again.level();
    again.gate(true);
    CHECK(std::fabs(again.tick() - before) < 0.01);
}

// --- voices ---------------------------------------------------------------------------

namespace {

struct VoiceLog {
    std::vector<int> started, released, stopped;
    std::vector<bool> retriggered;
    std::vector<NoteEvent> ended;
    void voiceStart(int v, bool retrigger) {
        started.push_back(v);
        retriggered.push_back(retrigger);
    }
    void voiceRelease(int v) { released.push_back(v); }
    void voiceStop(int v) { stopped.push_back(v); }
    void voiceEnded(const NoteEvent& n) { ended.push_back(n); }
    int endedKey(int key) const {
        int count = 0;
        for (const NoteEvent& n : ended) count += n.key == key;
        return count;
    }
};

NoteEvent on(int key, int channel = 0, int32_t id = -1) {
    NoteEvent e;
    e.type = NoteEvent::Type::On;
    e.key = static_cast<int16_t>(key);
    e.channel = static_cast<int16_t>(channel);
    e.noteId = id;
    e.velocity = 0.8;
    return e;
}
NoteEvent off(int key, int channel = 0, int32_t id = -1) {
    NoteEvent e = on(key, channel, id);
    e.type = NoteEvent::Type::Off;
    return e;
}
MidiMessage cc(int controller, int value, int channel = 0) {
    MidiMessage m;
    m.data[0] = static_cast<uint8_t>(0xB0 | channel);
    m.data[1] = static_cast<uint8_t>(controller);
    m.data[2] = static_cast<uint8_t>(value);
    return m;
}
MidiMessage bend(double amount, int channel) {
    MidiMessage m;
    const int v = static_cast<int>(std::lround(8192.0 + std::clamp(amount, -1.0, 8191.0 / 8192.0) * 8192.0));
    m.data[0] = static_cast<uint8_t>(0xE0 | channel);
    m.data[1] = static_cast<uint8_t>(v & 0x7F);
    m.data[2] = static_cast<uint8_t>(v >> 7);
    return m;
}

}  // namespace

TEST(voices_steal_the_oldest_and_end_each_note_once) {
    Voices<2> v;
    v.prepare(kRate);
    VoiceLog log;
    v.note(on(60), log);
    v.note(on(62), log);
    v.note(on(64), log);  // steals 60's voice
    CHECK(log.stopped.size() == 1 && log.stopped[0] == 0);
    CHECK(log.endedKey(60) == 1);
    CHECK(v[0].note.key == 64 && v[1].note.key == 62);
    // A released voice is stolen before a held one.
    v.note(off(62), log);
    CHECK(log.released.size() == 1 && log.released[0] == 1);
    v.note(on(65), log);
    CHECK(v[1].note.key == 65 && log.endedKey(62) == 1);
    // Finishing reports the note; nothing is reported twice.
    v.note(off(64), log);
    v.finished(0, log);
    v.finished(0, log);
    CHECK(log.endedKey(64) == 1);
    v.reset(log);
    CHECK(log.endedKey(65) == 1);
    CHECK(log.ended.size() == 4);
}

TEST(voice_controllers_are_omni_and_mpe_sustain_comes_from_the_master) {
    Voices<4> v;
    VoiceLog log;
    v.note(on(60, 0), log);
    v.note(on(64, 1), log);
    v.midi(cc(64, 127, 3), log);
    v.note(off(60, 0), log);
    v.note(off(64, 1), log);
    CHECK(v[0].pedal && v[1].pedal);
    v.midi(cc(64, 0, 4), log);
    CHECK(log.released.size() == 2);
    v.midi(cc(120, 0, 5), log);
    CHECK(log.ended.size() == 2);
    v.note(on(60, 0), log);
    v.note(on(64, 1), log);
    v.midi(cc(123, 0, 5), log);
    CHECK(!v[0].held && !v[1].held);
    v.reset(log);
    v.setMpe(Voices<4>::Mpe::Lower);
    v.midi(cc(64, 127, 1), log);
    CHECK(!v.sustainPedal());
    v.midi(cc(64, 127, 0), log);
    CHECK(v.sustainPedal());
}

TEST(changing_voice_mode_releases_tails_and_forgets_held_keys) {
    using Mode = Voices<4>::Mode;
    for (Mode from : {Mode::Poly, Mode::Mono, Mode::Legato}) {
        for (Mode to : {Mode::Poly, Mode::Mono, Mode::Legato}) {
            if (from == to) continue;
            Voices<4> v;
            VoiceLog log;
            v.setMode(from, log);
            v.note(on(60), log);
            v.note(on(64), log);
            v.midi(cc(64, 127), log);
            v.setMode(to, log);
            CHECK(log.stopped.empty());
            for (int k = 0; k < 4; ++k) {
                if (!v[k].active) continue;
                CHECK(!v[k].held && !v[k].pedal);
                CHECK(log.endedKey(v[k].note.key) == 0);
                v.finished(k, log);
            }
            CHECK(log.endedKey(60) == 1 && log.endedKey(64) == 1);
            v.midi(cc(64, 0), log);
            v.note(off(60), log);
            v.note(off(64), log);
            v.note(on(67), log);
            v.note(off(67), log);
            v.reset(log);
            CHECK(log.ended.size() == 3);
        }
    }
}

TEST(sustain_pedal_holds_released_notes_until_it_lifts) {
    Voices<4> v;
    v.prepare(kRate);
    VoiceLog log;
    v.midi(cc(64, 127), log);
    v.note(on(60), log);
    v.note(off(60), log);
    CHECK(log.released.empty() && v[0].held && v[0].pedal);
    v.midi(cc(64, 0), log);
    CHECK(log.released.size() == 1 && !v[0].held);
}

TEST(mono_plays_the_newest_key_and_returns_to_the_last) {
    for (auto mode : {Voices<4>::Mode::Mono, Voices<4>::Mode::Legato}) {
        Voices<4> v;
        v.prepare(kRate);
        VoiceLog log;
        v.setMode(mode, log);
        CHECK(v.voiceCount() == 1);
        v.note(on(60), log);
        v.note(on(64), log);
        CHECK(v[0].note.key == 64);
        CHECK(log.retriggered.back() == (mode == Voices<4>::Mode::Mono));
        CHECK(log.ended.empty());  // 60 is still held under it
        v.note(off(64), log);
        CHECK(v[0].note.key == 60 && log.endedKey(64) == 1);
        v.note(off(60), log);
        CHECK(log.released.size() == 1);
        v.finished(0, log);
        CHECK(log.endedKey(60) == 1 && log.ended.size() == 2);
        // A held key released under the sounding one ends at once.
        v.note(on(50), log);
        v.note(on(52), log);
        v.note(off(50), log);
        CHECK(log.endedKey(50) == 1 && v[0].note.key == 52);
        v.reset(log);
        CHECK(log.endedKey(52) == 1);
    }
}

TEST(glide_slides_from_the_last_note_in_its_time) {
    Voices<2> v;
    v.prepare(kRate);
    v.setGlide(10.0);
    VoiceLog log;
    v.note(on(60), log);
    CHECK(v.pitch(0) == 60.0);  // nothing to slide from
    v.note(on(72), log);
    CHECK_NEAR(v.pitch(1), 60.0, 1e-12);
    for (int n = 0; n < 240; ++n) v.tick();
    CHECK_NEAR(v.pitch(1), 66.0, 1e-9);
    for (int n = 0; n < 240; ++n) v.tick();
    CHECK(v.pitch(1) == 72.0);
}

TEST(glide_a_block_at_a_time_follows_the_ticks) {
    Voices<2> ticked, blocked;
    for (Voices<2>* v : {&ticked, &blocked}) {
        v->prepare(kRate);
        v->setGlide(10.0);  // 480 samples
        VoiceLog log;
        v->note(on(60), log);
        v->note(on(72), log);
    }
    // To the bit, in uneven blocks: a host's block size must not change a note.
    for (int frames : {1, 7, 64, 13, 100, 2, 200, 300}) {
        for (int i = 0; i < frames; ++i) {
            CHECK(blocked.pitchAt(1, i) == ticked.pitch(1));
            ticked.tick();
        }
        CHECK(blocked.pitchAt(1, frames) == ticked.pitch(1));
        blocked.advance(frames);
    }
    CHECK(blocked.pitch(1) == 72.0 && blocked.pitchAt(1, 50) == 72.0);
}

TEST(voices_play_the_count_chosen_at_run_time) {
    Voices<4> v;
    VoiceLog log;
    v.setCount(2, log);
    CHECK(v.count() == 2 && v.voiceCount() == 2);
    v.note(on(60), log);
    v.note(on(62), log);
    v.note(on(64), log);  // two voices: 60's is stolen
    CHECK(log.stopped.size() == 1 && log.stopped[0] == 0);
    CHECK(!v[2].active && !v[3].active);
    v.setCount(1, log);  // the voice above the new count stops, and its note ends
    CHECK(!v[1].active && log.endedKey(62) == 1);
    v.setCount(9, log);
    CHECK(v.count() == 4);
}

TEST(bend_moves_its_channel_and_mpe_members_alone) {
    Voices<4> v;
    v.prepare(kRate);
    v.setBendRange(2.0);
    VoiceLog log;
    v.note(on(60, 0), log);
    v.note(on(64, 1), log);
    v.midi(bend(1.0 * 8191.0 / 8192.0, 1), log);
    CHECK(v.pitch(0) == 60.0);
    CHECK_NEAR(v.pitch(1), 66.0, 1e-3);
    // MPE: a member's bend is its note's, 48 semitones; the master's moves all.
    Voices<4> m;
    m.prepare(kRate);
    m.setMpe(Voices<4>::Mpe::Lower);
    m.note(on(60, 1), log);
    m.note(on(60, 2), log);
    m.midi(bend(0.5, 1), log);
    m.midi(bend(-0.5, 0), log);  // master: -1 semitone
    CHECK_NEAR(m.pitch(0), 60.0 + 24.0 - 1.0, 1e-9);
    CHECK_NEAR(m.pitch(1), 60.0 - 1.0, 1e-9);
    MidiMessage pressure;
    pressure.data[0] = 0xD2;
    pressure.data[1] = 127;
    m.midi(pressure, log);
    CHECK(m[1].pressure == 1.0 && m[0].pressure == 0.0);
}

TEST(every_note_ends_exactly_once_whatever_happens) {
    for (auto mode : {Voices<3>::Mode::Poly, Voices<3>::Mode::Mono, Voices<3>::Mode::Legato}) {
        Voices<3> v;
        v.prepare(kRate);
        VoiceLog log;
        v.setMode(mode, log);
        Random r(static_cast<uint64_t>(mode) + 1);
        std::vector<int32_t> started;
        int32_t id = 0;
        for (int step = 0; step < 4000; ++step) {
            const double u = r.uniform();
            const int key = 60 + static_cast<int>(r.below(6));
            if (u < 0.35) {
                v.note(on(key, 0, id), log);
                started.push_back(id++);
            } else if (u < 0.65) {
                v.note(off(key), log);
            } else if (u < 0.7) {
                NoteEvent c = off(key);
                c.type = NoteEvent::Type::Choke;
                v.note(c, log);
            } else if (u < 0.75) {
                v.midi(cc(64, r.uniform() < 0.5 ? 127 : 0), log);
            } else if (u < 0.77) {
                v.midi(cc(r.uniform() < 0.5 ? 120 : 123, 0), log);
            } else {
                // A release finishing.
                const int k = static_cast<int>(r.below(3));
                if (v[k].active && !v[k].held) v.finished(k, log);
            }
        }
        v.reset(log);
        std::vector<int> count(static_cast<size_t>(id), 0);
        bool known = true;
        for (const NoteEvent& e : log.ended) {
            if (e.noteId < 0 || e.noteId >= id) known = false;
            else ++count[static_cast<size_t>(e.noteId)];
        }
        CHECK(known);
        int wrong = 0;
        for (int c : count) wrong += c != 1;
        CHECK_MSG(wrong == 0, "mode %d: %d notes not ended exactly once", static_cast<int>(mode), wrong);
        for (int k = 0; k < 3; ++k) CHECK(!v[k].active);
    }
}

// --- interpolation ----------------------------------------------------------------------

TEST(fractional_delays_delay_by_the_fraction) {
    for (Interpolation mode : {Interpolation::Linear, Interpolation::Hermite, Interpolation::Lagrange}) {
        FractionalDelay d;
        d.prepare(64);
        // Whole delays are exact.
        const std::vector<double> x = noise(200);
        for (size_t n = 0; n < x.size(); ++n) {
            const double y = d.process(x[n], 7.0, mode);
            if (n >= 7) CHECK(y == x[n - 7]);
        }
        // A half sample, on a sine.
        d.clear();
        double worst = 0.0;
        const double hz = 1000.0;
        for (int n = 0; n < 2000; ++n) {
            const double y = d.process(std::sin(2.0 * kPi * hz * n / kRate), 10.5, mode);
            if (n > 20) worst = std::max(worst, std::fabs(y - std::sin(2.0 * kPi * hz * (n - 10.5) / kRate)));
        }
        CHECK_MSG(worst < (mode == Interpolation::Linear ? 3e-3 : 1e-4), "mode %d: %.3g", static_cast<int>(mode), worst);
    }
    AllpassDelay a;
    a.prepare(64);
    std::vector<double> y(48000);
    for (size_t n = 0; n < y.size(); ++n) y[n] = a.process(std::sin(2.0 * kPi * 500.0 * static_cast<double>(n) / kRate), 10.3);
    CHECK_NEAR(amplitudeAt(y, 500.0, 24000), 1.0, 1e-6);
    // Its phase: the delay, at low frequency.
    std::complex<double> s = 0.0;
    for (size_t n = 24000; n < y.size(); ++n) s += y[n] * std::polar(1.0, -2.0 * kPi * 500.0 * static_cast<double>(n) / kRate);
    const double phase = std::arg(s * std::complex<double>(0.0, 1.0));  // relative to sin
    CHECK_NEAR(-phase / (2.0 * kPi * 500.0 / kRate), 10.3, 0.02);
}

// --- shapers ------------------------------------------------------------------------------

TEST(adaa_antiderivatives_are_right_and_cut_aliasing) {
    auto checkShape = [](auto shape) {
        using S = decltype(shape);
        for (double x = -3.0; x <= 3.0; x += 0.05)
            CHECK_NEAR((S::F(x + 1e-6) - S::F(x - 1e-6)) / 2e-6, S::f(x), 1e-6);
    };
    checkShape(HardClipShape{});
    checkShape(TanhShape{});
    checkShape(CubicShape{});

    const size_t n = 16384, bin = 1600;  // 4687.5 Hz
    const double hz = kRate * bin / n;
    std::vector<double> naive(n), adaa(n), soft(n), softAdaa(n);
    Adaa<HardClipShape>::State hs;
    Adaa<TanhShape>::State ts;
    // Run a cycle first so the state is periodic too.
    for (size_t k = 0; k < 2 * n; ++k) {
        const double x = 4.0 * std::sin(2.0 * kPi * hz * static_cast<double>(k) / kRate);
        const double a = Adaa<HardClipShape>::tick(hs, x), b = Adaa<TanhShape>::tick(ts, x);
        if (k < n) continue;
        naive[k - n] = HardClipShape::f(x);
        adaa[k - n] = a;
        soft[k - n] = TanhShape::f(x);
        softAdaa[k - n] = b;
    }
    CHECK_MSG(db(aliasRatio(adaa, bin)) < db(aliasRatio(naive, bin)) - 6.0, "hard clip: %.1f vs %.1f dB",
              db(aliasRatio(adaa, bin)), db(aliasRatio(naive, bin)));
    CHECK_MSG(db(aliasRatio(softAdaa, bin)) < db(aliasRatio(soft, bin)) - 6.0, "tanh: %.1f vs %.1f dB",
              db(aliasRatio(softAdaa, bin)), db(aliasRatio(soft, bin)));
}

// --- loudness -------------------------------------------------------------------------------

TEST(k_weighting_is_bs1770s_table_at_48k) {
    const KWeighting k = KWeighting::design(48000.0);
    CHECK_NEAR(k.shelf.b0, 1.53512485958697, 1e-6);
    CHECK_NEAR(k.shelf.b1, -2.69169618940638, 1e-6);
    CHECK_NEAR(k.shelf.b2, 1.19839281085285, 1e-6);
    CHECK_NEAR(k.shelf.a1, -1.69065929318241, 1e-6);
    CHECK_NEAR(k.shelf.a2, 0.73248077421585, 1e-6);
    CHECK_NEAR(k.highpass.a1, -1.99004745483398, 1e-6);
    CHECK_NEAR(k.highpass.a2, 0.99007225036621, 1e-6);
}

TEST(loudness_reads_ebu_test_signals) {
    auto play = [](Loudness& l, double dbfs, double seconds) {
        const double a = dbToGain(dbfs);
        const int frames = static_cast<int>(seconds * kRate);
        static int phase = 0;
        for (int n = 0; n < frames; ++n, ++phase) {
            const double v = a * std::sin(2.0 * kPi * 1000.0 * phase / kRate);
            const double frame[2] = {v, v};
            l.tick(frame);
        }
    };
    Loudness l;
    l.prepare(kRate, 2);
    CHECK(l.momentary() == Loudness::kFloor);
    // EBU Tech 3341 case 1: a stereo 1 kHz sine at -23 dBFS reads -23 LUFS.
    play(l, -23.0, 20.0);
    CHECK_NEAR(l.momentary(), -23.0, 0.1);
    CHECK_NEAR(l.shortTerm(), -23.0, 0.1);
    CHECK_NEAR(l.integrated(), -23.0, 0.1);
    // Case 3: -36, -23, -36 dBFS for 10, 60 and 10 seconds: the gate keeps
    // the quiet parts out of the integrated reading.
    l.reset();
    play(l, -36.0, 10.0);
    play(l, -23.0, 60.0);
    play(l, -36.0, 10.0);
    CHECK_NEAR(l.integrated(), -23.0, 0.1);
    CHECK_NEAR(l.shortTerm(), -36.0, 0.1);
}

TEST(loudness_gates_quiet_content_below_loud_and_overflow_groups) {
    // Steady blocks at +37 and +137 LUFS both used to read as silence once
    // their relative gate exceeded the old histogram ceiling of +10.
    for (double loud : {100.0, 1e7}) {
        Loudness meter;
        meter.prepare(kRate, 1);
        for (int n = 0; n < 5 * static_cast<int>(kRate); ++n) {
            double x = loud * std::sin(2.0 * kPi * 1000.0 * n / kRate);
            meter.tick(&x);
        }
        const double onlyLoud = meter.integrated();
        CHECK_NEAR(onlyLoud, 20.0 * std::log10(loud) - 3.0036, 0.02);
        for (int n = 0; n < 5 * static_cast<int>(kRate); ++n) {
            double x = 0.001 * std::sin(2.0 * kPi * 1000.0 * n / kRate);
            meter.tick(&x);
        }
        // Only the three 400 ms transition windows alter the integrated
        // mean; five seconds of the quiet tone must be gated away.
        CHECK_NEAR(meter.integrated(), onlyLoud, 0.16);
        meter.reset();
        CHECK(meter.integrated() == Loudness::kFloor);
    }
}

TEST(correlation_tells_mono_from_inverted_from_unrelated) {
    Correlation c;
    c.prepare(kRate);
    const std::vector<double> a = noise(48000, 1), b = noise(48000, 2);
    for (double v : a) c.tick(v, v);
    CHECK_NEAR(c.value(), 1.0, 1e-9);
    c.reset();
    for (double v : a) c.tick(v, -v);
    CHECK_NEAR(c.value(), -1.0, 1e-9);
    c.reset();
    for (size_t n = 0; n < a.size(); ++n) c.tick(a[n], b[n]);
    CHECK(std::fabs(c.value()) < 0.15);
}

// --- convolution -------------------------------------------------------------------------------

TEST(convolution_is_the_direct_sum_in_any_blocks) {
    const std::vector<double> h0 = noise(3001, 7);
    std::vector<double> h1 = noise(2000, 8);
    h1.resize(h0.size(), 0.0);  // every channel's response is as long as the first
    const double* ir[2] = {h0.data(), h1.data()};
    const std::vector<double> x = noise(12000, 9);
    for (bool zeroLatency : {true, false}) {
        Convolution conv;
        conv.prepare(ir, 2, h0.size(), 2, 128, zeroLatency);
        std::vector<double> y0 = x, y1 = x;
        Random r(4);
        for (size_t at = 0; at < x.size();) {
            const int n = std::min(static_cast<int>(1 + r.below(300)), static_cast<int>(x.size() - at));
            conv.process(0, y0.data() + at, n);
            conv.process(1, y1.data() + at, n);
            at += static_cast<size_t>(n);
        }
        const size_t latency = static_cast<size_t>(conv.latency());
        CHECK(latency == (zeroLatency ? 0u : 128u));
        double worst = 0.0;
        for (size_t t = latency; t < x.size(); t += 7) {
            double want0 = 0.0, want1 = 0.0;
            for (size_t k = 0; k < h0.size() && k + latency <= t; ++k) {
                want0 += h0[k] * x[t - latency - k];
                want1 += h1[k] * x[t - latency - k];
            }
            worst = std::max({worst, std::fabs(y0[t] - want0), std::fabs(y1[t] - want1)});
        }
        CHECK_MSG(worst < 1e-9, "zero latency %d: error %.3g", zeroLatency ? 1 : 0, worst);
    }
    // A response shorter than a partition, and a mono one for two channels.
    const double tiny[3] = {0.5, 0.25, 0.125};
    const double* mono[1] = {tiny};
    Convolution c;
    c.prepare(mono, 1, 3, 2, 64);
    double impulse[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    c.process(1, impulse, 8);
    CHECK(impulse[0] == 0.5 && impulse[1] == 0.25 && impulse[2] == 0.125 && impulse[3] == 0.0);
}

// --- worker and hand-off -------------------------------------------------------------------------

TEST(worker_runs_jobs_in_order_and_repeats_until_cancelled) {
    Worker w;
    std::vector<int> order;
    for (int k = 0; k < 50; ++k) w.post([&order, k] { order.push_back(k); });
    w.wait();
    CHECK(order.size() == 50);
    for (int k = 0; k < 50; ++k) CHECK(order[static_cast<size_t>(k)] == k);
    std::atomic<int> ticks{0};
    const int id = w.repeat([&ticks] { ++ticks; }, 1.0);
    for (int k = 0; k < 2000 && ticks.load() < 5; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(ticks.load() >= 5);
    w.cancel(id);
    const int after = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ticks.load() == after);
}

TEST(handoff_gives_the_newest_and_frees_off_the_audio_thread) {
    struct Payload {
        int value;
        std::atomic<int>* freedOnAudio;
        std::thread::id audio;
        ~Payload() {
            if (std::this_thread::get_id() == audio) ++*freedOnAudio;
        }
    };
    std::atomic<int> freedOnAudio{0};
    const std::thread::id audio = std::this_thread::get_id();  // this thread plays the audio thread
    {
        Handoff<Payload> h;
        Worker w;
        CHECK(h.get() == nullptr);
        for (int k = 1; k <= 200; ++k) {
            w.post([&h, k, &freedOnAudio, audio] { h.publish(std::make_unique<Payload>(Payload{k, &freedOnAudio, audio})); });
            if (k % 3 == 0) w.wait();
            h.update();
            if (const Payload* p = h.get()) CHECK(p->value <= k);
        }
        w.wait();
        h.update();
        CHECK(h.get() && h.get()->value == 200);
        w.post([&h] { h.collect(); });
        w.wait();
        CHECK(freedOnAudio.load() == 0);
        w.stop();
    }
}

// --- audio files ----------------------------------------------------------------------------

namespace {
// A stereo 16-bit ramp, 100 frames at 48 kHz, as the reference encoder
// (flac 1.5, -8) wrote it: left (i * 300 - 15000) / 32768, right its negative.
const unsigned char kRampFlac[] = {
    0x66, 0x4c, 0x61, 0x43, 0x00, 0x00, 0x00, 0x22, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x1f, 0x0b, 0xb8,
    0x02, 0xf0, 0x00, 0x00, 0x00, 0x64, 0x3d, 0x31, 0xe0, 0x3c, 0xbb, 0x34, 0x52, 0x06, 0x05, 0x6f, 0xb9, 0xfa, 0x16, 0xaf,
    0xf1, 0xc5, 0x84, 0x00, 0x00, 0x28, 0x20, 0x00, 0x00, 0x00, 0x72, 0x65, 0x66, 0x65, 0x72, 0x65, 0x6e, 0x63, 0x65, 0x20,
    0x6c, 0x69, 0x62, 0x46, 0x4c, 0x41, 0x43, 0x20, 0x31, 0x2e, 0x35, 0x2e, 0x30, 0x20, 0x32, 0x30, 0x32, 0x35, 0x30, 0x32,
    0x31, 0x31, 0x00, 0x00, 0x00, 0x00, 0xff, 0xf8, 0x6a, 0xa8, 0x00, 0x63, 0x41, 0x00, 0x00, 0x00, 0x15, 0x38, 0xad, 0x63,
    0x4a, 0x00, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xe0, 0x0e, 0x34,
};
}  // namespace

TEST(float_file_non_finite_samples_become_zero_in_loads_and_streams) {
    const auto path = scratchFolder("nonfinite-file") / "float.wav";
    AudioData input;
    input.sampleRate = kRate;
    input.channel = {{0.25f, std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -0.5f}};
    CHECK(saveWavFile(path.string(), input));
    AudioData output;
    CHECK(loadAudioFile(path.string(), output));
    CHECK(output.channel[0] == std::vector<float>({0.25f, 0.0f, 0.0f, 0.0f, -0.5f}));
    DiskStream stream;
    CHECK(stream.open(path.string()));
    double samples[5] = {};
    double* channels[] = {samples};
    CHECK(stream.read(channels, 1, 5) == 5);
    for (size_t n = 0; n < 5; ++n) CHECK(samples[n] == output.channel[0][n]);
}

TEST(a_long_file_loads_only_as_far_as_asked) {
    const auto path = scratchFolder("max-seconds") / "long.wav";
    AudioData input;
    input.sampleRate = 1000.0;
    input.channel.assign(2, std::vector<float>(3000));
    for (size_t n = 0; n < 3000; ++n) input.channel[0][n] = input.channel[1][n] = static_cast<float>(n) / 3000.0f;
    CHECK(saveWavFile(path.string(), input));
    AudioData whole, start;
    CHECK(loadAudioFile(path.string(), whole) && whole.frames() == 3000);
    CHECK(loadAudioFile(path.string(), start, nullptr, 1.25) && start.frames() == 1250 && start.channels() == 2);
    CHECK(std::equal(start.channel[1].begin(), start.channel[1].end(), whole.channel[1].begin()));
    // A limit past the end, or none, is the whole file.
    CHECK(loadAudioFile(path.string(), start, nullptr, 60.0) && start.frames() == 3000);
    CHECK(loadAudioFile(path.string(), start, nullptr, -1.0) && start.frames() == 3000);
    // So is one that does not say how long it is, cut where asked.
    const auto flac = scratchFolder("max-seconds") / "unknown.flac";
    std::vector<unsigned char> bytes(std::begin(kRampFlac), std::end(kRampFlac));
    bytes[21] &= 0xf0;
    std::fill(bytes.begin() + 22, bytes.begin() + 26, 0);
    std::ofstream(flac, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    AudioData ramp;
    CHECK(loadAudioFile(flac.string(), ramp) && ramp.frames() == 100);
    CHECK(loadAudioFile(flac.string(), ramp, nullptr, 40.0 / ramp.sampleRate) && ramp.frames() == 40);
}

TEST(unknown_length_flac_loads_whole_but_is_refused_for_streaming) {
    const auto path = scratchFolder("unknown-length") / "unknown.flac";
    std::vector<unsigned char> bytes(std::begin(kRampFlac), std::end(kRampFlac));
    bytes[21] &= 0xf0;
    std::fill(bytes.begin() + 22, bytes.begin() + 26, 0);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    AudioData audio;
    std::string error;
    CHECK(loadAudioFile(path.string(), audio, &error));
    CHECK(audio.frames() == 100);
    DiskStream stream;
    CHECK(!stream.open(path.string(), 1.0, &error));
    CHECK(!stream.isOpen());
    CHECK(error.find("length") != std::string::npos);
    CHECK(stream.frames() == 0 && stream.channels() == 0);
}

TEST(wav_aiff_and_flac_decode_and_wav_round_trips) {
    const std::filesystem::path dir = scratchFolder("files");
    AudioData a;
    a.sampleRate = 44100.0;
    a.channel = {std::vector<float>(1000), std::vector<float>(1000)};
    for (size_t n = 0; n < 1000; ++n) {
        a.channel[0][n] = static_cast<float>(std::sin(0.01 * static_cast<double>(n)));
        a.channel[1][n] = -0.25f;
    }
    const std::string wav = (dir / "round trip \xC3\xA9.wav").string();
    std::string error;
    CHECK(saveWavFile(wav, a, &error));
    AudioData b;
    CHECK(loadAudioFile(wav, b, &error));
    CHECK(b.sampleRate == 44100.0 && b.channels() == 2 && b.frames() == 1000);
    CHECK(b.channel == a.channel);

    auto ramp = [](const AudioData& d) {
        bool ok = d.channels() == 2 && d.frames() == 100 && d.sampleRate == 48000.0;
        for (size_t i = 0; ok && i < 100; ++i) {
            const float left = static_cast<float>(static_cast<int>(i) * 300 - 15000) / 32768.0f;
            ok = d.channel[0][i] == left && d.channel[1][i] == -left;
        }
        return ok;
    };
    const std::string flac = (dir / "ramp.flac").string();
    std::ofstream(flac, std::ios::binary).write(reinterpret_cast<const char*>(kRampFlac), sizeof(kRampFlac));
    AudioData f;
    CHECK_MSG(loadAudioFile(flac, f, &error), "%s", error.c_str());
    CHECK(ramp(f));

    // AIFF, written here: FORM, COMM (an 80-bit rate), SSND, big-endian.
    std::vector<unsigned char> aiff;
    auto u32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) aiff.push_back(static_cast<unsigned char>(v >> s)); };
    auto u16 = [&](uint16_t v) { aiff.push_back(static_cast<unsigned char>(v >> 8)); aiff.push_back(static_cast<unsigned char>(v)); };
    auto tag = [&](const char* t) { aiff.insert(aiff.end(), t, t + 4); };
    tag("FORM");
    u32(4 + 8 + 18 + 8 + 8 + 400);
    tag("AIFF");
    tag("COMM");
    u32(18);
    u16(2);
    u32(100);
    u16(16);
    const unsigned char rate48k[10] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
    aiff.insert(aiff.end(), rate48k, rate48k + 10);
    tag("SSND");
    u32(8 + 400);
    u32(0);
    u32(0);
    for (int i = 0; i < 100; ++i) {
        u16(static_cast<uint16_t>(static_cast<int16_t>(i * 300 - 15000)));
        u16(static_cast<uint16_t>(static_cast<int16_t>(-(i * 300 - 15000))));
    }
    const std::string aif = (dir / "ramp.aiff").string();
    std::ofstream(aif, std::ios::binary).write(reinterpret_cast<const char*>(aiff.data()), static_cast<std::streamsize>(aiff.size()));
    AudioData r;
    CHECK_MSG(loadAudioFile(aif, r, &error), "%s", error.c_str());
    CHECK(ramp(r));

    // What it cannot read, it says.
    AudioData none;
    CHECK(!loadAudioFile((dir / "nothing.wav").string(), none, &error) && !error.empty());
    std::ofstream((dir / "text.wav").string()) << "not audio";
    CHECK(!loadAudioFile((dir / "text.wav").string(), none, &error));
    std::filesystem::remove_all(dir.parent_path());
}

TEST(wav_writer_rejects_invalid_audio_before_opening_the_file) {
    const std::filesystem::path dir = scratchFolder("invalid-wav");
    const std::string path = (dir / "keep.wav").string();
    std::ofstream(path) << "keep";
    AudioData a;
    a.channel = {{0.25f, 0.5f}, {0.25f, 0.5f}};
    std::string error;
    for (double rate : {0.0, -1.0, 0.1, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN(), 4294967296.0}) {
        a.sampleRate = rate;
        CHECK(!saveWavFile(path, a, &error));
        CHECK(!error.empty());
    }
    a.sampleRate = 48000.0;
    a.channel[1].resize(1);
    CHECK(!saveWavFile(path, a, &error));
    a.channel[1].resize(3);
    CHECK(!saveWavFile(path, a, &error));
    std::string contents;
    std::ifstream(path) >> contents;
    CHECK(contents == "keep");
    std::filesystem::remove_all(dir.parent_path());
}

#ifdef __linux__
TEST(tool_wav_writer_reports_a_full_disk) {
    wavio::Audio audio;
    audio.channels = 1;
    audio.ch = {{0.25}};
    std::string error;
    CHECK(!wavio::write("/dev/full", audio, 32, error));
    CHECK(!error.empty());
}
#endif

TEST(resampling_keeps_the_band_and_drops_what_cannot_fit) {
    AudioData in;
    in.sampleRate = 44100.0;
    in.channel.assign(1, std::vector<float>(44100));
    for (size_t n = 0; n < 44100; ++n) in.channel[0][n] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / 44100.0));
    const AudioData up = resample(in, 48000.0);
    CHECK(up.frames() == 48000 && up.sampleRate == 48000.0);
    std::vector<double> y(up.channel[0].begin(), up.channel[0].end());
    CHECK_NEAR(amplitudeAt(std::vector<double>(y.begin() + 1000, y.end() - 1000), 1000.0), 0.5, 1e-4);
    // Down to 32 kHz, a 20 kHz tone has nowhere to go.
    AudioData high;
    high.sampleRate = 48000.0;
    high.channel.assign(1, std::vector<float>(48000));
    for (size_t n = 0; n < 48000; ++n)
        high.channel[0][n] = static_cast<float>(std::sin(2.0 * kPi * 20000.0 * static_cast<double>(n) / 48000.0) +
                                                std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / 48000.0));
    const AudioData down = resample(high, 32000.0);
    std::vector<double> z(down.channel[0].begin() + 2000, down.channel[0].end() - 2000);
    CHECK_NEAR(amplitudeAt(z, 1000.0, 0, 32000.0), 1.0, 1e-3);
    CHECK(db(amplitudeAt(z, 12000.0, 0, 32000.0)) < -80.0);  // 20 kHz would fold to 12
}

TEST(resampling_each_channel_is_the_same_alone_or_together) {
    // The channels share each output sample's tap weights; none may lean on
    // another's samples, at a rate up or down, nor change a bit doing so.
    AudioData in;
    in.sampleRate = 48000.0;
    in.channel.assign(3, std::vector<float>(2000));
    for (size_t n = 0; n < 2000; ++n) {
        const double x = static_cast<double>(n);
        in.channel[0][n] = static_cast<float>(std::sin(0.031 * x));
        in.channel[1][n] = static_cast<float>(0.5 * std::sin(0.4 * x) - 0.2 * std::cos(0.07 * x));
        in.channel[2][n] = n % 97 == 0 ? 1.0f : 0.0f;
    }
    for (const double rate : {44100.0, 96000.0, 1234.5678}) {
        const AudioData together = resample(in, rate);
        CHECK(together.channel.size() == 3);
        for (size_t c = 0; c < 3; ++c) {
            AudioData one;
            one.sampleRate = in.sampleRate;
            one.channel = {in.channel[c]};
            const AudioData alone = resample(one, rate);
            CHECK(alone.channel[0] == together.channel[c]);
        }
    }
}

TEST(disk_stream_plays_the_file_from_disk_and_restarts) {
    const std::filesystem::path dir = scratchFolder("stream");
    AudioData a;
    a.sampleRate = 48000.0;
    a.channel = {std::vector<float>(100000), std::vector<float>(100000)};
    for (size_t n = 0; n < 100000; ++n) {
        a.channel[0][n] = static_cast<float>(n) / 100000.0f;
        a.channel[1][n] = -a.channel[0][n];
    }
    const std::string path = (dir / "long.wav").string();
    CHECK(saveWavFile(path, a));
    DiskStream s;
    CHECK(s.open(path, 0.25));
    CHECK(s.channels() == 2 && s.frames() == 100000);
    std::vector<double> left(512), right(512);
    double* out[2] = {left.data(), right.data()};
    auto playAll = [&](size_t upTo) {
        size_t at = 0;
        bool exact = true;
        while (at < upTo) {
            s.service();  // the worker, between blocks
            const int n = s.read(out, 2, 512);
            if (n == 0) break;
            for (int i = 0; i < n; ++i)
                exact = exact && left[static_cast<size_t>(i)] == a.channel[0][at + static_cast<size_t>(i)] &&
                        right[static_cast<size_t>(i)] == a.channel[1][at + static_cast<size_t>(i)];
            at += static_cast<size_t>(n);
        }
        return exact && at >= std::min<size_t>(upTo, 100000);
    };
    CHECK(playAll(100000));
    CHECK(s.read(out, 2, 512) == 0);  // the end
    // Part way, then from the top: the head plays while the disk catches up.
    s.restart();
    CHECK(playAll(30000));
    s.restart();
    CHECK(playAll(100000));
    CHECK(s.underruns() == 0);
    // On a real worker thread.
    Worker w;
    s.restart();
    const int id = w.repeat([&s] { s.service(); }, 1.0);
    size_t at = 0;
    bool exact = true;
    while (at < 100000) {
        const int n = s.read(out, 2, 256);
        for (int i = 0; i < n; ++i) exact = exact && left[static_cast<size_t>(i)] == a.channel[0][at + static_cast<size_t>(i)];
        at += static_cast<size_t>(n);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    w.cancel(id);
    CHECK(exact);
    s.close();
    std::filesystem::remove_all(dir.parent_path());
}

SUPPLE_TEST_MAIN
