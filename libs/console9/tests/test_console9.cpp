// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <type_traits>
#include <vector>
#include "reference.h"
#include "supple/base/rng.h"
#include "supple/console9/console9.h"
namespace supple::console9::detail {
// Inject only the original static endpoint interpolation, not any sample DSP.
// This diagnostic stage must null against every sample of the unmodified reference.
struct StaticReferenceAccess {
    template <class Port>
    static void set(Port& stage, float B, float A, double temp) {
        const double angle = double(A) * 1.57079633, trim = double(B) * 2.0;
        const double pan = (angle * temp) + (angle * (1.0 - temp));
        double gain = (trim * temp) + (trim * (1.0 - temp));
        if (gain > 1.0) gain *= gain;
        if (gain < 1.0) gain = 1.0 - std::pow(1.0 - gain, 2);
        stage.gain_.set(gain, 0);
        stage.angle_.set(pan, 0);
        stage.coefficients();
    }
};
}  // namespace supple::console9::detail
using namespace supple::console9;
namespace {
constexpr double pi = std::numbers::pi;
template <class T>
using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
template <class T>
bool identical(T a, T b) {
    return std::bit_cast<Bits<T>>(a) == std::bit_cast<Bits<T>>(b);
}
template <class T>
struct Audio {
    std::vector<T> l, r;
    explicit Audio(size_t n) : l(n), r(n) {}
};
struct Stats {
    uint64_t samples = 0, same = 0, endpoints = 0;
    double maxFloatUlps = 0, maxAbs = 0;
    template <class T>
    void add(T a, T b, bool endpoint) {
        ++samples;
        if (identical(a, b)) {
            ++same;
            return;
        }
        if (endpoint) ++endpoints;
        const double diff = std::abs(double(a) - double(b));
        int exponent = 0;
        std::frexp(std::abs(double(b)), &exponent);
        const double ulp = std::ldexp(1., std::max(-149, exponent - 24));
        maxFloatUlps = std::max(maxFloatUlps, diff / ulp);
        maxAbs = std::max(maxAbs, diff);
    }
};
template <class T>
Audio<T> signal(double rate) {
    Audio<T> a(4096);
    supple::Rng rng(12345);
    double phase = 0;
    for (size_t i = 0; i < a.l.size(); ++i) {
        double l = 0, r = 0;
        if (i < 2048) {
            const double t = double(i) / 2047;
            const double level = std::pow(10., (-60 + 72 * t) / 20);
            phase += 2 * pi * (20 + 19980 * t) / rate;
            l = level * std::sin(phase);
            r = level * std::cos(phase * .731);
        } else if (i < 3072) {
            const double level = std::pow(10., (-60 + 72 * double(i - 2048) / 1023) / 20);
            l = rng.uniform(-1, 1) * level;
            r = rng.uniform(-1, 1) * level;
        } else if (i < 3584) {
            constexpr double threshold = 1.18e-23;
            const double tiny[]{std::nextafter(threshold, 0.),
                                threshold,
                                std::nextafter(threshold, 1.),
                                -std::nextafter(threshold, 0.),
                                -threshold,
                                -std::nextafter(threshold, 1.),
                                0.,
                                -0.,
                                1e-30,
                                -1e-30,
                                double(std::nextafter(float(threshold), 0.f)),
                                double(float(threshold)),
                                double(std::nextafter(float(threshold), 1.f)),
                                -double(std::nextafter(float(threshold), 0.f)),
                                -double(float(threshold)),
                                -double(std::nextafter(float(threshold), 1.f))};
            l = tiny[i % std::size(tiny)];
            r = tiny[(i + 3) % std::size(tiny)];
        }
        a.l[i] = T(l);
        a.r[i] = T(r);
    }
    return a;
}
template <class Port, class T>
void staticNull(bool encoder) {
    constexpr int sizes[]{1, 64, 4096};
    for (int block : sizes) {
        Stats stats, ordinary;
        uint64_t unexplained = 0;
        for (double rate : {44100., 96000.}) {
            auto input = signal<T>(rate);
            Audio<T> actual(input.l.size()), expected(input.l.size());
            for (int f = 0; f <= 20; ++f)
                for (int p = 0; p <= 4; ++p) {
                    const float B = float(f) / 20.f, A = float(p) / 4.f;
                    const auto seeds = seedsFor(99, uint64_t(f * 5 + p));
                    Port port;
                    port.setNative(B, A);
                    port.reset(seeds);
                    auto diagnostic = port;
                    auto reference = encoder ? test::channelReference() : test::bussReference();
                    reference->set(B, A, seeds);
                    for (size_t off = 0; off < input.l.size();) {
                        const auto n = uint32_t(std::min(size_t(block), input.l.size() - off));
                        T* in[]{input.l.data() + off, input.r.data() + off};
                        T* out[]{actual.l.data() + off, actual.r.data() + off};
                        T* refOut[]{expected.l.data() + off, expected.r.data() + off};
                        port.process(in, out, n);
                        reference->process(in, refOut, int32_t(n));
                        for (uint32_t i = 0; i < n; ++i) {
                            stats.add(out[0][i], refOut[0][i], i == n - 1);
                            stats.add(out[1][i], refOut[1][i], i == n - 1);
                            if (p != 4) ordinary.add(out[0][i], refOut[0][i], false);
                            ordinary.add(out[1][i], refOut[1][i], false);
                            if (block != 1) {
                                detail::StaticReferenceAccess::set(diagnostic, B, A,
                                                                   double(n - i - 1) / n);
                                const T* di[]{in[0] + i, in[1] + i};
                                T dl = 0, dr = 0;
                                T* dout[]{&dl, &dr};
                                diagnostic.process(di, dout, 1);
                                if (!identical(dl, refOut[0][i])) ++unexplained;
                                if (!identical(dr, refOut[1][i])) ++unexplained;
                            }
                        }
                        off += n;
                    }
                }
        }
        INFO("stage=" << (encoder ? "channel" : "buss") << " bits=" << sizeof(T) * 8
                      << " block=" << block);
        CHECK(stats.endpoints == 0);  // Reference interpolation weight is zero on block endpoints.
        if (block == 1) CHECK(stats.same == stats.samples);
        CHECK(ordinary.maxFloatUlps <= 2);
        CHECK(unexplained == 0);
        std::printf(
            "NULL %-7s %2zu block=%4d identical=%llu/%llu (%.9f%%) max_float_ulp=%.9g "
            "max_abs=%.9g ordinary_ulp=%.9g unexplained=%llu\n",
            encoder ? "channel" : "buss", sizeof(T) * 8, block,
            static_cast<unsigned long long>(stats.same),
            static_cast<unsigned long long>(stats.samples),
            100 * double(stats.same) / double(stats.samples), stats.maxFloatUlps, stats.maxAbs,
            ordinary.maxFloatUlps, static_cast<unsigned long long>(unexplained));
    }
}
template <class Port, class T>
Audio<T> ramped(int block, bool linear = false) {
    auto input = signal<T>(48000);
    Audio<T> output(input.l.size());
    Port stage;
    stage.setLinear(linear);
    stage.reset(seedsFor(1, 2));
    stage.set(.2, .1);
    for (size_t off = 0; off < input.l.size();) {
        if (off == 0) stage.set(4, 1, 1203);
        if (off == 997) stage.set(.03, .2, 703);
        if (off == 1700) stage.set(0, .5, 480);
        if (off == 2180) stage.setNative(.5f, .75f, 960);
        if (off == 3140) stage.setNative(1, 0, 333);
        size_t next = input.l.size();
        for (size_t event : {997u, 1700u, 2180u, 3140u})
            if (event > off) next = std::min(next, event);
        const auto n = uint32_t(std::min(size_t(block), next - off));
        if constexpr (std::is_same_v<Port, MonoChannel> || std::is_same_v<Port, MonoBuss>)
            stage.process(input.l.data() + off, output.l.data() + off, n);
        else {
            const T* in[]{input.l.data() + off, input.r.data() + off};
            T* out[]{output.l.data() + off, output.r.data() + off};
            stage.process(in, out, n);
        }
        off += n;
    }
    return output;
}
template <class Port, class T>
void splitTest(bool linear = false) {
    const auto a = ramped<Port, T>(4096, linear);
    for (int n : {1, 7, 64, 512}) {
        const auto b = ramped<Port, T>(n, linear);
        CHECK(std::equal(a.l.begin(), a.l.end(), b.l.begin(), identical<T>));
        CHECK(std::equal(a.r.begin(), a.r.end(), b.r.begin(), identical<T>));
    }
}
template <class T>
void sumNull(int block) {
    const auto input = signal<T>(48000);
    Audio<T> sum(input.l.size()), refSum(input.l.size()), encoded(input.l.size()),
        refEncoded(input.l.size());
    uint64_t unexplained = 0;
    for (int c = 0; c < 5; ++c) {
        Channel stage;
        auto reference = test::channelReference();
        const auto seeds = seedsFor(10, uint64_t(c));
        const float f = .18f + .04f * float(c), p = .1f + .2f * float(c);
        stage.setNative(f, p);
        stage.reset(seeds);
        auto diagnostic = stage;
        reference->set(f, p, seeds);
        Audio<T> scaled(input.l.size());
        for (size_t i = 0; i < input.l.size(); ++i) {
            scaled.l[i] = T(double(input.l[i]) * .1 * std::cos(double(c) + double(i) * .001));
            scaled.r[i] = T(double(input.r[i]) * .1 * std::sin(double(c) + double(i) * .003));
        }
        for (size_t off = 0; off < input.l.size();) {
            auto n = uint32_t(std::min(size_t(block), input.l.size() - off));
            T* in[]{scaled.l.data() + off, scaled.r.data() + off};
            T* out[]{encoded.l.data() + off, encoded.r.data() + off};
            T* ro[]{refEncoded.l.data() + off, refEncoded.r.data() + off};
            stage.process(in, out, n);
            reference->process(in, ro, int32_t(n));
            for (uint32_t i = 0; i < n; ++i) {
                detail::StaticReferenceAccess::set(diagnostic, f, p, double(n - i - 1) / n);
                const T* di[]{in[0] + i, in[1] + i};
                T dl = 0, dr = 0;
                T* dout[]{&dl, &dr};
                diagnostic.process(di, dout, 1);
                if (!identical(dl, ro[0][i])) ++unexplained;
                if (!identical(dr, ro[1][i])) ++unexplained;
            }
            off += n;
        }
        for (size_t i = 0; i < input.l.size(); ++i) {
            sum.l[i] += encoded.l[i];
            sum.r[i] += encoded.r[i];
            refSum.l[i] += refEncoded.l[i];
            refSum.r[i] += refEncoded.r[i];
        }
    }
    Buss buss;
    auto reference = test::bussReference();
    const auto seeds = seedsFor(10, 99);
    buss.reset(seeds);
    auto diagnostic = buss;
    reference->set(.5f, .5f, seeds);
    Stats stats;
    for (size_t off = 0; off < input.l.size();) {
        auto n = uint32_t(std::min(size_t(block), input.l.size() - off));
        T* in[]{sum.l.data() + off, sum.r.data() + off};
        T* ri[]{refSum.l.data() + off, refSum.r.data() + off};
        T* out[]{encoded.l.data() + off, encoded.r.data() + off};
        T* ro[]{refEncoded.l.data() + off, refEncoded.r.data() + off};
        buss.process(in, out, n);
        reference->process(ri, ro, int32_t(n));
        for (uint32_t i = 0; i < n; ++i) {
            stats.add(out[0][i], ro[0][i], false);
            stats.add(out[1][i], ro[1][i], false);
            detail::StaticReferenceAccess::set(diagnostic, .5f, .5f, double(n - i - 1) / n);
            const T* di[]{ri[0] + i, ri[1] + i};
            T dl = 0, dr = 0;
            T* dout[]{&dl, &dr};
            diagnostic.process(di, dout, 1);
            if (!identical(dl, ro[0][i])) ++unexplained;
            if (!identical(dr, ro[1][i])) ++unexplained;
        }
        off += n;
    }
    if (block == 1) CHECK(stats.same == stats.samples);
    CHECK(stats.maxFloatUlps <= 4);
    CHECK(unexplained == 0);
    std::printf(
        "SUM  %2zu block=%4d identical=%.9f%% max_float_ulp=%.9g max_abs=%.9g unexplained=%llu\n",
        sizeof(T) * 8, block, 100 * double(stats.same) / double(stats.samples), stats.maxFloatUlps,
        stats.maxAbs, static_cast<unsigned long long>(unexplained));
}
}  // namespace
TEST_CASE("Console9 static reference null matrix") {
    staticNull<Channel, float>(true);
    staticNull<Channel, double>(true);
    staticNull<Buss, float>(false);
    staticNull<Buss, double>(false);
}
TEST_CASE("Console9 encoded channel sums decode against reference") {
    for (int n : {1, 7, 64, 512, 4096}) {
        sumNull<float>(n);
        sumNull<double>(n);
    }
}
TEST_CASE("Console9 ramps are bit exact across every partition and retarget") {
    splitTest<Channel, float>();
    splitTest<Channel, double>();
    splitTest<Buss, float>();
    splitTest<Buss, double>();
    splitTest<MonoChannel, float>();
    splitTest<MonoChannel, double>();
    splitTest<MonoBuss, float>();
    splitTest<MonoBuss, double>();
}
TEST_CASE("Console9 mappings and deterministic seed domain") {
    CHECK(faderGain(0) == 1);
    CHECK(faderGain(100) == 4);
    CHECK(faderGain(-std::numeric_limits<double>::infinity()) == 0);
    CHECK(faderGain(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(nativeFader(0) == .5);
    CHECK(nativeFader(100) == 1);
    CHECK(std::isinf(faderDb(0)));
    CHECK(faderDb(.5) == 0);
    CHECK(nativePan(-100) == 0);
    CHECK(nativePan(0) == .5);
    CHECK(nativePan(100) == 1);
    for (double db : {-120., -60., -24., -6., 0., 6., 12.})
        CHECK(faderDb(nativeFader(db)) == doctest::Approx(db).epsilon(1e-9));
    for (uint64_t i = 0; i < 10000; ++i) {
        const auto a = seedsFor(42, i);
        CHECK(a == seedsFor(42, i));
        CHECK(a.left >= 16386);
        CHECK(a.right >= 16386);
        CHECK_FALSE(a == seedsFor(43, i));
        CHECK_FALSE(a == seedsFor(42, i + 1));
    }
}
TEST_CASE("Console9 neutral staging clip mono input reset and nonfinite policy") {
    Channel channel;
    Buss buss;
    double x = .00001, l = 0, r = 0, dl = 0, dr = 0;
    const double* in[]{&x, &x};
    double* encoded[]{&l, &r};
    channel.process(in, encoded, 1);
    const double* ei[]{&l, &r};
    double* decoded[]{&dl, &dr};
    buss.process(ei, decoded, 1);
    CHECK(dl / x == doctest::Approx(.472135956694964).epsilon(1e-8));
    CHECK(dl == dr);
    x = std::pow(10., 12. / 20);
    channel.process(in, encoded, 1);
    CHECK(l == 1);
    CHECK(r == 1);
    auto seeds = seedsFor(7, 8);
    channel.reset(seeds);
    x = 0;
    channel.process(in, encoded, 1);
    double first = l;
    channel.reset(seeds);
    channel.process(in, encoded, 1);
    CHECK(l == first);
    CHECK(l > 0);
    channel.reset(seeds);
    x = std::numeric_limits<double>::quiet_NaN();
    channel.process(in, encoded, 1);
    CHECK(l == first);
    channel.reset(seeds);
    x = std::numeric_limits<double>::infinity();
    channel.process(in, encoded, 1);
    CHECK(l == first);
    // In-place duplicated mono source must be read before either destination is written.
    double mono = .2, right = 0, expectedL = 0, expectedR = 0;
    channel.reset(seeds);
    x = mono;
    channel.process(in, encoded, 1);
    expectedL = l;
    expectedR = r;
    channel.reset(seeds);
    const double* mi[]{&mono, &mono};
    double* mo[]{&mono, &right};
    channel.process(mi, mo, 1);
    CHECK(mono == expectedL);
    CHECK(right == expectedR);
    channel.reset({0, 0});
    x = 0;
    channel.process(in, encoded, 1);
    CHECK(std::isfinite(l));
}
TEST_CASE("Console9 mono kernels have no pan and match stereo at hard left") {
    for (bool encode : {false, true}) {
        auto input = signal<float>(48000);
        Audio<float> stereo(input.l.size());
        std::vector<float> mono(input.l.size());
        const auto seeds = seedsFor(8, 9);
        for (float b : {0.f, .05f, .5f, .75f, 1.f}) {
            const float* in[]{input.l.data(), input.r.data()};
            float* out[]{stereo.l.data(), stereo.r.data()};
            if (encode) {
                MonoChannel m;
                Channel s;
                m.setNative(b);
                s.setNative(b, 0);
                m.reset(seeds);
                s.reset(seeds);
                m.process(input.l.data(), mono.data(), uint32_t(mono.size()));
                s.process(in, out, uint32_t(mono.size()));
            } else {
                MonoBuss m;
                Buss s;
                m.setNative(b);
                s.setNative(b, 0);
                m.reset(seeds);
                s.reset(seeds);
                m.process(input.l.data(), mono.data(), uint32_t(mono.size()));
                s.process(in, out, uint32_t(mono.size()));
            }
            // sin(1.57079633) rounds to unity in double; left is exactly the fader-only path.
            CHECK(mono == stereo.l);
        }
    }
}
TEST_CASE("Console9 pow power-of-two equivalence across all float exponents") {
    for (int e = -149; e <= 128; ++e) CHECK(std::pow(2, e + 62) == std::ldexp(1., e + 62));
}
TEST_CASE("Console9 caller-sized fader ramp is linear and zero-length processing is inert") {
    MonoBuss stage, neutral;
    stage.set(0);
    stage.set(1, .5, 4);
    double x = .1, unity = 0;
    neutral.process(&x, &unity, 1);
    for (int i = 1; i <= 6; ++i) {
        double y = 0;
        stage.process(&x, &y, 0);
        stage.process(&x, &y, 1);
        CHECK(y == unity * std::min(1., double(i) / 4));
    }
    Channel channel, reference;
    channel.setNative(.7f, .2f);
    reference.setNative(.7f, .2f);
    const auto seeds = seedsFor(1, 100);
    channel.reset(seeds);
    reference.reset(seeds);
    float zero = 0, l = 0, r = 0, rl = 0, rr = 0;
    const float* in[]{&zero, &zero};
    float* out[]{&l, &r};
    float* refOut[]{&rl, &rr};
    channel.process(in, out, 0);
    channel.process(in, out, 1);
    reference.process(in, refOut, 1);
    CHECK(identical(l, rl));
    CHECK(identical(r, rr));
}
namespace {
template <class Port, class T>
std::array<T, 2> tick(Port& stage, T x, T y) {
    std::array<T, 2> output{};
    if constexpr (std::is_same_v<Port, MonoChannel> || std::is_same_v<Port, MonoBuss>)
        stage.process(&x, &output[0], 1);
    else {
        const T* in[]{&x, &y};
        T* out[]{&output[0], &output[1]};
        stage.process(in, out, 1);
    }
    return output;
}
template <class Port, class T>
void linearState() {
    Port stage, control;
    stage.setNative(.71f, .3f);
    control.setNative(.71f, .3f);
    const auto seeds = seedsFor(45, 67);
    stage.reset(seeds);
    control.reset(seeds);
    CHECK(tick(stage, T(0), T(0)) == tick(control, T(0), T(0)));
    stage.setLinear(true);
    for (int i = 0; i < 1000; ++i) {
        const auto silent = tick(stage, T(0), T(0));
        CHECK(silent[0] == 0);
        CHECK(silent[1] == 0);
    }
    const auto tiny = tick(stage, T(1e-25), T(-1e-25));
    CHECK(std::abs(double(tiny[0])) < 1e-24);
    CHECK(tiny[0] > 0);  // No denormal replacement, and the actual tiny input survives.
    const auto bad =
        tick(stage, std::numeric_limits<T>::quiet_NaN(), std::numeric_limits<T>::infinity());
    CHECK(bad[0] == 0);
    CHECK(bad[1] == 0);
    stage.setLinear(false);
    const auto actual = tick(stage, T(0), T(0)), expected = tick(control, T(0), T(0));
    CHECK(identical(actual[0], expected[0]));
    CHECK(identical(actual[1], expected[1]));
}
template <class Port, class T>
void smallSignal() {
    for (float B : {.05f, .5f, 1.f})
        for (float A : {.25f, .5f, .75f}) {
            Port curves, linear;
            curves.setNative(B, A);
            linear.setNative(B, A);
            linear.setLinear(true);
            auto seeds = seedsFor(4, 5);
            curves.reset(seeds);
            linear.reset(seeds);
            for (int i = 1; i <= 31; ++i) {
                const T x = T(double(i) * (std::is_same_v<T, float> ? 1e-10 : 1e-18));
                const auto on = tick(curves, x, T(-x)), off = tick(linear, x, T(-x));
                for (size_t c = 0;
                     c < (std::is_same_v<Port, MonoChannel> || std::is_same_v<Port, MonoBuss> ? 1u
                                                                                              : 2u);
                     ++c) {
                    int exponent = 0;
                    std::frexp(std::abs(double(off[c])), &exponent);
                    const double ulp = std::ldexp(1., exponent - std::numeric_limits<T>::digits);
                    // Float has upstream +/-one-ulp dither; double has arithmetic roundoff only.
                    CHECK(std::abs(double(on[c]) - double(off[c])) <=
                          ulp * (std::is_same_v<T, float> ? 2 : 16));
                }
            }
        }
}
template <class T>
void linearNeutral() {
    Channel channel;
    Buss buss;
    channel.setLinear(true);
    buss.setLinear(true);
    const double pan = std::sin(.785398165);
    for (int e = -40; e <= 20; ++e)
        for (double sign : {-1., 1.}) {
            const T x = T(sign * std::ldexp(1., e));
            const auto encoded = tick(channel, x, x);
            const auto output = tick(buss, encoded[0], encoded[1]);
            const T expected = T(double(x) * .763932022500211 * .6180339887498949 * 2 * pan * pan);
            CHECK(identical(output[0], expected));
            CHECK(identical(output[1], expected));
        }
}
}  // namespace
TEST_CASE("Console9 linear neutral cascade is exactly the specified gain and has no clipping") {
    linearNeutral<float>();
    linearNeutral<double>();
}
TEST_CASE("Console9 linear preserves small-signal gains within float dither and double roundoff") {
    smallSignal<Channel, float>();
    smallSignal<Channel, double>();
    smallSignal<Buss, float>();
    smallSignal<Buss, double>();
    smallSignal<MonoChannel, float>();
    smallSignal<MonoChannel, double>();
    smallSignal<MonoBuss, float>();
    smallSignal<MonoBuss, double>();
}
TEST_CASE("Console9 linear makes no noise or RNG progress in every stage and precision") {
    linearState<Channel, float>();
    linearState<Channel, double>();
    linearState<Buss, float>();
    linearState<Buss, double>();
    linearState<MonoChannel, float>();
    linearState<MonoChannel, double>();
    linearState<MonoBuss, float>();
    linearState<MonoBuss, double>();
}
TEST_CASE("Console9 linear ramps are bit exact across partitions") {
    splitTest<Channel, float>(true);
    splitTest<Channel, double>(true);
    splitTest<Buss, float>(true);
    splitTest<Buss, double>(true);
    splitTest<MonoChannel, float>(true);
    splitTest<MonoChannel, double>(true);
    splitTest<MonoBuss, float>(true);
    splitTest<MonoBuss, double>(true);
}

TEST_CASE("the Channel pan law at centre is measured as sin(pi/4) per channel") {
    // Measured from the stage rather than assumed: in linear mode a Channel
    // applies gain x pan gains x trim, so the ratio of a centred pan to a hard
    // pan is the pan law itself. panGains() — what a host meters a strip's
    // pre-encode level with — must produce exactly the stage's coefficients.
    const auto run = [](double gain, double pan) {
        Channel stage;
        stage.setLinear(true);
        stage.reset(seedsFor(1, 2));
        stage.set(gain, pan);
        std::array<float, 4> l{1.0f, 1.0f, 1.0f, 1.0f}, r{1.0f, 1.0f, 1.0f, 1.0f};
        const float* in[]{l.data(), r.data()};
        std::array<float, 4> ol{}, orr{};
        float* out[]{ol.data(), orr.data()};
        stage.process(in, out, 4);
        return std::pair<double, double>{double(ol.back()), double(orr.back())};
    };
    const auto hard = run(1.0, 0.0);  // hard left: the fader times the encode trim
    const auto centre = run(1.0, 0.5);
    CHECK(hard.second == 0.0);
    CHECK(centre.first / hard.first == doctest::Approx(std::sin(pi / 4)).epsilon(1e-6));
    CHECK(centre.second == doctest::Approx(centre.first));
    double left = 0, right = 0;
    panGains(panAngle(0.5), left, right);
    CHECK(centre.first == doctest::Approx(hard.first * left).epsilon(1e-6));
    CHECK(centre.second == doctest::Approx(hard.first * right).epsilon(1e-6));
    CHECK(panAngle(0.5) == doctest::Approx(pi / 4).epsilon(1e-7));  // native .5 is centre
    panGains(panAngle(0.0), left, right);
    CHECK(left == doctest::Approx(1.0).epsilon(1e-9));  // hard left
    CHECK(right == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("Unity staging has no pan stage and an encode/decode pair is unity") {
    // ADR-0066: the host pans before the encode, the Channel has no ×0.7639
    // trim and the Buss's ×2 is ×φ, the inverse of the curves' slope.
    const auto pair = [](bool linear, double pan, float x) {
        Channel channel;
        Buss buss;
        channel.setStaging(Staging::Unity);
        buss.setStaging(Staging::Unity);
        channel.setLinear(linear);
        buss.setLinear(linear);
        channel.reset(seedsFor(3, 1));
        buss.reset(seedsFor(3, 2));
        channel.set(1.0, pan);
        std::array<float, 64> l{}, r{}, el{}, er{}, dl{}, dr{};
        l.fill(x);
        r.fill(-x);
        const float* in[]{l.data(), r.data()};
        float* enc[]{el.data(), er.data()};
        channel.process(in, enc, 64);
        const float* encIn[]{el.data(), er.data()};
        float* dec[]{dl.data(), dr.data()};
        buss.process(encIn, dec, 64);
        return std::array<float, 4>{el.back(), er.back(), dl.back(), dr.back()};
    };
    // Linear: the Channel is x · gain and the Buss passes its input, exactly.
    for (const double pan : {0.0, 0.5, 1.0}) {
        const auto out = pair(true, pan, 0.25f);
        CHECK(identical(out[0], 0.25f));
        CHECK(identical(out[1], -0.25f));
        CHECK(identical(out[2], 0.25f));
        CHECK(identical(out[3], -0.25f));
    }
    // Curves: small signals come back at their own level, whatever the pan.
    for (const double pan : {0.0, 0.5, 1.0}) {
        const auto out = pair(false, pan, 1e-3f);
        CHECK(double(out[2]) == doctest::Approx(1e-3).epsilon(1e-4));
        CHECK(double(out[3]) == doctest::Approx(-1e-3).epsilon(1e-4));
    }
    // The encode is driven by x · f(2B) · 0.618 into the curve: no trim.
    Channel channel;
    channel.setStaging(Staging::Unity);
    channel.reset(seedsFor(3, 1));
    channel.set(2.0);
    std::array<float, 4> l{.4f, .4f, .4f, .4f}, ol{}, orr{};
    const float* in[]{l.data(), l.data()};
    float* out[]{ol.data(), orr.data()};
    channel.process(in, out, 4);
    const double u = .4 * 2.0 * 0.618033988749895;
    CHECK(ol.back() == doctest::Approx(1 - std::pow(1 - u, 1.618033988749895)).epsilon(1e-6));
    CHECK(double(orr.back()) == doctest::Approx(double(ol.back())));
    // Native staging is unchanged by a round trip through Unity.
    Channel native;
    native.setStaging(Staging::Unity);
    native.setStaging(Staging::Native);
    native.setLinear(true);
    native.set(1.0, 0.5);
    native.process(in, out, 4);
    CHECK(double(ol.back()) ==
          doctest::Approx(.4 * std::sin(pi / 4) * .763932022500211).epsilon(1e-6));
}
