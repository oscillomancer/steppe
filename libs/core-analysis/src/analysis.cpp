// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/analysis/analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <sstream>

#include "supple/dsp/dsp.h"

namespace supple::analysis {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kSilence = std::pow(10.0, -70.0 / 20.0);  // RMS gate for statistics

constexpr std::array<double, kBandCount + 1> kBandEdges = {20,   60,   250,  500,
                                                           2000, 4000, 6000, 20000};
constexpr std::string_view kBandNames[kBandCount] = {"sub",      "low",      "low_mid", "mid",
                                                     "high_mid", "presence", "air"};

std::size_t pow2Nearest(double x) {
    const double l = std::round(std::log2(std::max(16.0, x)));
    return static_cast<std::size_t>(1) << static_cast<int>(l);
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const auto i = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] + (v[i + 1] - v[i]) * frac : v[i];
}

Stats stats(const std::vector<double>& v) {
    Stats s;
    if (v.empty()) return {kNaN, kNaN, kNaN, kNaN};
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    s.median = percentile(v, 0.5);
    s.p10 = percentile(v, 0.1);
    s.p90 = percentile(v, 0.9);
    return s;
}

std::vector<double> downmix(const Signal& s) {
    std::vector<double> m(static_cast<std::size_t>(s.frames), 0.0);
    const std::size_t n = s.channels.size();
    for (std::int64_t i = 0; i < s.frames; ++i) {
        double v = 0.0;
        if (s.layout == Layout::Mono || n == 1) {
            v = static_cast<double>(s.channels[0][i]);
        } else if (s.layout == Layout::Stereo || n == 2) {
            v = 0.5 *
                (static_cast<double>(s.channels[0][i]) + static_cast<double>(s.channels[1][i]));
        } else {
            // BS.1770 weights: L R C (1.0), surrounds 1.41, LFE excluded; normalized.
            constexpr double w50[5] = {1, 1, 1, 1.41, 1.41};
            constexpr double w51[6] = {1, 1, 1, 0, 1.41, 1.41};
            const double* w = s.layout == Layout::Surround50 ? w50 : w51;
            double sum = 0.0;
            for (std::size_t c = 0; c < std::min<std::size_t>(n, 6); ++c) {
                v += w[c] * static_cast<double>(s.channels[c][i]);
                sum += w[c];
            }
            v /= sum;
        }
        m[static_cast<std::size_t>(i)] = v;
    }
    return m;
}

double rms(const double* x, std::size_t n) {
    double e = 0.0;
    for (std::size_t i = 0; i < n; ++i) e += x[i] * x[i];
    return n ? std::sqrt(e / static_cast<double>(n)) : 0.0;
}

double db(double power) { return 10.0 * std::log10(std::max(power, 1e-12)); }

// Short-time spectra: Hann window, hop N/4, zero-padded final frame.
struct Stft {
    std::size_t n = 0, hop = 0;
    double fs = 48000.0;
    double norm = 1.0;                       // |X|² → one-sided power normalized to signal power
    std::vector<std::vector<double>> power;  // [frame][k], k = 0..N/2
    std::vector<bool> silent;

    Stft(const std::vector<double>& x, double sampleRate, std::size_t size)
        : n(size), hop(size / 4), fs(sampleRate) {
        std::vector<double> w(n);
        double w2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            w[i] = dsp::hann(i, n);
            w2 += w[i] * w[i];
        }
        norm = 2.0 / (static_cast<double>(n) * w2);
        dsp::RealFft fft;
        fft.prepare(n);
        std::vector<double> frame(n);
        std::vector<std::complex<double>> spec(n / 2 + 1);
        for (std::size_t start = 0; start < std::max<std::size_t>(x.size(), 1); start += hop) {
            const std::size_t avail = start < x.size() ? std::min(n, x.size() - start) : 0;
            for (std::size_t i = 0; i < n; ++i) frame[i] = i < avail ? x[start + i] * w[i] : 0.0;
            fft.forward(frame, spec);
            std::vector<double> p(n / 2 + 1);
            for (std::size_t k = 0; k <= n / 2; ++k)
                p[k] = std::norm(spec[k]) * norm * (k == 0 || k == n / 2 ? 0.5 : 1.0);
            power.push_back(std::move(p));
            silent.push_back(avail == 0 || rms(x.data() + start, avail) < kSilence);
            if (start + n >= x.size()) break;
        }
    }
    double freq(std::size_t k) const {
        return static_cast<double>(k) * fs / static_cast<double>(n);
    }
    // Power in [lo, hi) Hz of a spectrum, bins as intervals (fractional overlap).
    double bandPower(const std::vector<double>& p, double lo, double hi) const {
        const double df = fs / static_cast<double>(n);
        double sum = 0.0;
        for (std::size_t k = 1; k < p.size(); ++k) {
            const double a = (static_cast<double>(k) - 0.5) * df,
                         b = (static_cast<double>(k) + 0.5) * df;
            const double overlap = std::min(b, hi) - std::max(a, lo);
            if (overlap > 0.0) sum += p[k] * overlap / df;
        }
        return sum;
    }
};

std::vector<double> decimate(const std::vector<double>& v, std::size_t maxPoints) {
    if (v.size() <= maxPoints) return v;
    std::vector<double> out;
    for (std::size_t i = 0; i < maxPoints; ++i) out.push_back(v[i * v.size() / maxPoints]);
    return out;
}

// --- loudness -------------------------------------------------------------------------------

LoudnessReport loudnessOf(const Signal& s) {
    LoudnessMeter meter;
    meter.prepare(s.sampleRate, s.layout, LoudnessMeter::Mode::Exact,
                  static_cast<double>(s.frames) / s.sampleRate + 1.0);
    const int hop = static_cast<int>(std::llround(s.sampleRate * 0.1));
    std::vector<const float*> ptr(s.channels.begin(), s.channels.end());
    LoudnessReport r;
    for (std::int64_t pos = 0; pos < s.frames; pos += hop) {
        const int n = static_cast<int>(std::min<std::int64_t>(hop, s.frames - pos));
        std::vector<const float*> at(ptr.size());
        for (std::size_t c = 0; c < ptr.size(); ++c) at[c] = ptr[c] + pos;
        meter.process(at, n);
        r.momentary.push_back(meter.momentary());
        r.shortTerm.push_back(meter.shortTerm());
    }
    r.integrated = meter.integrated();
    r.lra = meter.loudnessRange();
    r.maxMomentary = meter.maxMomentary();
    r.maxShortTerm = meter.maxShortTerm();
    // Drain the 32-tap true-peak interpolator after capturing loudness, so
    // the final samples' peaks are included without extending the program.
    const std::array<float, 32> silence{};
    std::fill(ptr.begin(), ptr.end(), silence.data());
    meter.process(ptr, static_cast<int>(silence.size()));
    r.truePeak = meter.truePeak();
    r.samplePeak = meter.samplePeak();
    r.plr = r.truePeak - r.integrated;
    return r;
}

// --- spectral ---------------------------------------------------------------------------------

SpectralReport spectralOf(const Stft& st) {
    SpectralReport r;
    std::vector<double> centroid, rolloff, flatness, flux;
    std::vector<double> ltas(st.n / 2 + 1, 0.0), prevNorm;
    std::size_t active = 0;
    for (std::size_t t = 0; t < st.power.size(); ++t) {
        const auto& p = st.power[t];
        std::vector<double> mag(p.size());
        double norm2 = 0.0;
        for (std::size_t k = 1; k < p.size(); ++k) {
            mag[k] = std::sqrt(p[k]);
            norm2 += p[k];
        }
        const double inv = norm2 > 0.0 ? 1.0 / std::sqrt(norm2) : 0.0;
        std::vector<double> unit(p.size());
        for (std::size_t k = 1; k < p.size(); ++k) unit[k] = mag[k] * inv;
        double fl = 0.0;
        if (!prevNorm.empty()) {
            for (std::size_t k = 1; k < p.size(); ++k) {
                const double d = std::max(0.0, unit[k] - prevNorm[k]);
                fl += d * d;
            }
            fl = std::sqrt(fl);
        }
        prevNorm = unit;
        if (st.silent[t]) continue;
        ++active;
        double sm = 0.0, sfm = 0.0, total = 0.0;
        for (std::size_t k = 1; k < p.size(); ++k) {
            sm += mag[k];
            sfm += st.freq(k) * mag[k];
            total += p[k];
            ltas[k] += p[k];
        }
        if (sm > 0.0) centroid.push_back(sfm / sm);
        double cum = 0.0;
        for (std::size_t k = 1; k < p.size(); ++k) {
            cum += p[k];
            if (cum >= 0.85 * total) {
                rolloff.push_back(st.freq(k));
                break;
            }
        }
        double logSum = 0.0, sum = 0.0;
        std::size_t count = 0;
        for (std::size_t k = 1; k < p.size(); ++k) {
            const double f = st.freq(k);
            if (f < 30.0 || f > 16000.0) continue;
            const double v = std::max(p[k], 1e-20);
            logSum += std::log(v);
            sum += v;
            ++count;
        }
        if (count)
            flatness.push_back(std::exp(logSum / static_cast<double>(count)) /
                               (sum / static_cast<double>(count)));
        if (t > 0) flux.push_back(fl);
    }
    r.centroidHz = stats(centroid);
    r.rolloffHz = stats(rolloff);
    r.flatness = stats(flatness);
    r.flux = stats(flux);
    r.centroidTrack = decimate(centroid, 200);
    if (active)
        for (double& v : ltas) v /= static_cast<double>(active);
    const double total = st.bandPower(ltas, 20.0, std::min(20000.0, st.fs / 2));
    for (int b = 0; b < kBandCount; ++b) {
        Band& band = r.bands[static_cast<std::size_t>(b)];
        band.lowHz = kBandEdges[static_cast<std::size_t>(b)];
        band.highHz = std::min(kBandEdges[static_cast<std::size_t>(b) + 1], st.fs / 2);
        const double p =
            band.highHz > band.lowHz ? st.bandPower(ltas, band.lowHz, band.highHz) : 0.0;
        band.levelDb = total > 0.0 ? db(p / total) : -120.0;
        band.sharePct = total > 0.0 ? 100.0 * p / total : 0.0;
    }
    // 1/3-octave LTAS (full-scale sine = 0 dB: its mean power is 1/2) and tilt.
    std::vector<double> xs, ys;
    for (int i = 0; i < kThirdOctaves; ++i) {
        const double fc = thirdOctaveCentre(i);
        const double lo = fc * std::pow(2.0, -1.0 / 6.0),
                     hi = std::min(fc * std::pow(2.0, 1.0 / 6.0), st.fs / 2);
        const double p = hi > lo ? st.bandPower(ltas, lo, hi) : 0.0;
        r.ltasDb[static_cast<std::size_t>(i)] = db(p / 0.5);
        if (fc >= 100.0 && fc <= 10000.0 && p > 0.0) {
            xs.push_back(std::log2(fc));
            ys.push_back(r.ltasDb[static_cast<std::size_t>(i)]);
        }
    }
    if (xs.size() >= 2) {
        const double mx =
            std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());
        const double my =
            std::accumulate(ys.begin(), ys.end(), 0.0) / static_cast<double>(ys.size());
        double sxy = 0.0, sxx = 0.0;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            sxy += (xs[i] - mx) * (ys[i] - my);
            sxx += (xs[i] - mx) * (xs[i] - mx);
        }
        r.tiltDbPerOct = sxx > 0.0 ? sxy / sxx : 0.0;
    }
    return r;
}

// --- harmonic ---------------------------------------------------------------------------------

std::string noteName(double hz) {
    static constexpr const char* names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                            "F#", "G",  "G#", "A",  "A#", "B"};
    const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    const auto n = static_cast<int>(std::lround(midi));
    const auto cents = static_cast<int>(std::lround((midi - n) * 100.0));
    std::string s = std::string(names[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
    if (cents != 0) s += (cents > 0 ? "+" : "") + std::to_string(cents) + "c";
    return s;
}

HarmonicReport harmonicOf(const std::vector<double>& x, double fs, std::pair<double, double> range,
                          int maxFrames) {
    HarmonicReport r;
    const auto w = static_cast<std::size_t>(std::lround(4096.0 * fs / 48000.0));
    const auto hop = static_cast<std::size_t>(std::lround(0.01 * fs));
    const auto tauMin = static_cast<std::size_t>(std::max(2.0, std::floor(fs / range.second)));
    auto tauMax = static_cast<std::size_t>(std::ceil(fs / range.first));
    tauMax = std::min(tauMax, w / 2);
    const std::size_t integ = w - tauMax;
    // FFT cross-correlation of [0, integ) with [0, w).
    const std::size_t nfft = static_cast<std::size_t>(1)
                             << static_cast<int>(std::ceil(std::log2(static_cast<double>(2 * w))));
    dsp::RealFft fft;
    fft.prepare(nfft);
    std::vector<double> a(nfft), b(nfft);
    std::vector<std::complex<double>> A(nfft / 2 + 1), Bs(nfft / 2 + 1), prod(nfft / 2 + 1);
    std::vector<double> full(nfft);
    std::vector<double> d(tauMax + 2), dn(tauMax + 2);
    std::vector<double> voicedF0, hnr, periods;
    std::vector<std::size_t> voicedStarts;
    std::size_t nonSilent = 0;
    // With a frame budget, step over whole hops so frame positions stay on the
    // 10 ms grid (the track keeps its hop; skipped frames read as unvoiced).
    std::size_t stride = hop;
    if (maxFrames > 0 && x.size() > w) {
        const std::size_t frames = (x.size() - w) / hop + 1;
        stride = hop * std::max<std::size_t>(1, (frames + static_cast<std::size_t>(maxFrames) - 1) /
                                                    static_cast<std::size_t>(maxFrames));
    }
    for (std::size_t start = 0; start + w <= x.size() || (start == 0 && !x.empty());
         start += stride) {
        if (start + w > x.size()) break;
        const double* f = x.data() + start;
        r.f0Track.push_back(0.0);
        const double level = rms(f, w);
        if (level < kSilence) continue;
        ++nonSilent;
        // Without variation there is no period to find: a constant (DC)
        // frame's roundoff would otherwise read as a confident trough.
        double mean = 0.0;
        for (std::size_t i = 0; i < w; ++i) mean += f[i];
        mean /= static_cast<double>(w);
        double ac = 0.0;
        for (std::size_t i = 0; i < w; ++i) ac += (f[i] - mean) * (f[i] - mean);
        if (std::sqrt(ac / static_cast<double>(w)) < std::max(kSilence, 1e-4 * level)) continue;
        std::fill(a.begin(), a.end(), 0.0);
        std::fill(b.begin(), b.end(), 0.0);
        for (std::size_t i = 0; i < integ; ++i) a[i] = f[i];
        for (std::size_t i = 0; i < w; ++i) b[i] = f[i];
        fft.forward(a, A);
        fft.forward(b, Bs);
        // r(τ) = Σ a[j]·b[j+τ] = IFFT(conj(A)·B).
        for (std::size_t k = 0; k <= nfft / 2; ++k) prod[k] = std::conj(A[k]) * Bs[k];
        fft.inverse(prod, full);
        std::vector<double> corr(full.begin(),
                                 full.begin() + static_cast<std::ptrdiff_t>(tauMax + 1));
        // Energies of the sliding window [τ, τ + integ).
        std::vector<double> e(tauMax + 1);
        double e0 = 0.0;
        for (std::size_t i = 0; i < integ; ++i) e0 += f[i] * f[i];
        double run = e0;
        for (std::size_t t = 0; t <= tauMax; ++t) {
            e[t] = run;
            if (t + integ < w) run += f[t + integ] * f[t + integ] - f[t] * f[t];
        }
        d[0] = 0.0;
        dn[0] = 1.0;
        double cum = 0.0;
        for (std::size_t t = 1; t <= tauMax; ++t) {
            d[t] = std::max(0.0, e0 + e[t] - 2.0 * corr[t]);
            cum += d[t];
            dn[t] = cum > 0.0 ? d[t] * static_cast<double>(t) / cum : 1.0;
        }
        std::size_t tau = 0;
        for (std::size_t t = tauMin; t <= tauMax; ++t)
            if (dn[t] < 0.10) {
                while (t + 1 <= tauMax && dn[t + 1] < dn[t]) ++t;
                tau = t;
                break;
            }
        if (tau == 0) {
            tau = tauMin;
            for (std::size_t t = tauMin; t <= tauMax; ++t)
                if (dn[t] < dn[tau]) tau = t;
        }
        double refined = static_cast<double>(tau);
        if (tau > 1 && tau < tauMax) {
            const double y0 = dn[tau - 1], y1 = dn[tau], y2 = dn[tau + 1];
            const double den = y0 - 2.0 * y1 + y2;
            if (std::fabs(den) > 1e-12) refined += 0.5 * (y0 - y2) / den;
        }
        const double confidence = 1.0 - dn[tau];
        if (confidence >= 0.85) {
            const double f0 = fs / refined;
            r.f0Track.back() = f0;
            voicedF0.push_back(f0);
            periods.push_back(refined);
            voicedStarts.push_back(start);
            const double rr =
                std::clamp(corr[tau] / std::sqrt(std::max(1e-30, e0 * e[tau])), 1e-6, 1.0 - 1e-6);
            hnr.push_back(10.0 * std::log10(rr / (1.0 - rr)));
        }
    }
    r.voicedPct =
        nonSilent ? 100.0 * static_cast<double>(voicedF0.size()) / static_cast<double>(nonSilent)
                  : 0.0;
    r.pitched = nonSilent > 0 && r.voicedPct >= 50.0;
    if (!r.pitched) {
        r.f0Track.clear();
        return r;
    }
    r.f0Hz = percentile(voicedF0, 0.5);
    r.note = noteName(r.f0Hz);
    double sq = 0.0;
    for (double f : voicedF0) {
        const double c = 1200.0 * std::log2(f / r.f0Hz);
        sq += c * c;
    }
    r.stabilityCents = std::sqrt(sq / static_cast<double>(voicedF0.size()));
    r.hnrDb = percentile(hnr, 0.5);
    // Harmonics on up to 64 evenly spaced voiced frames.
    std::vector<std::vector<double>> amps(16);
    std::vector<double> inharm;
    const std::size_t step = std::max<std::size_t>(1, voicedStarts.size() / 64);
    for (std::size_t v = 0; v < voicedStarts.size(); v += step) {
        const double f0 = voicedF0[v];
        const auto len = std::max<std::size_t>(2048, static_cast<std::size_t>(4.0 * periods[v]));
        const std::size_t start = voicedStarts[v];
        if (start + len > x.size()) continue;
        std::size_t n = 1;
        while (n < len * 2) n <<= 1;
        dsp::RealFft hf;
        hf.prepare(n);
        std::vector<double> frame(n, 0.0);
        for (std::size_t i = 0; i < len; ++i) frame[i] = x[start + i] * dsp::hann(i, len);
        std::vector<std::complex<double>> spec(n / 2 + 1);
        hf.forward(frame, spec);
        const double df = fs / static_cast<double>(n);
        std::array<double, 16> amp{}, freq{};
        for (int k = 1; k <= 16; ++k) {
            const double target = k * f0;
            if (target * 1.03 >= fs / 2) {
                amp[static_cast<std::size_t>(k - 1)] = 0.0;
                continue;
            }
            const auto lo = static_cast<std::size_t>(std::max(1.0, std::floor(target * 0.97 / df)));
            const auto hi =
                std::min(n / 2 - 1, static_cast<std::size_t>(std::ceil(target * 1.03 / df)));
            std::size_t best = lo;
            for (std::size_t i = lo; i <= hi; ++i)
                if (std::abs(spec[i]) > std::abs(spec[best])) best = i;
            const double m0 = std::abs(spec[best - 1]), m1 = std::abs(spec[best]),
                         m2 = std::abs(spec[best + 1]);
            double shift = 0.0;
            const double l0 = std::log(std::max(m0, 1e-30)), l1 = std::log(std::max(m1, 1e-30)),
                         l2 = std::log(std::max(m2, 1e-30));
            const double den = l0 - 2.0 * l1 + l2;
            if (std::fabs(den) > 1e-12) shift = 0.5 * (l0 - l2) / den;
            freq[static_cast<std::size_t>(k - 1)] = (static_cast<double>(best) + shift) * df;
            amp[static_cast<std::size_t>(k - 1)] = std::exp(l1 - 0.25 * (l0 - l2) * shift);
        }
        if (amp[0] <= 0.0) continue;
        double wsum = 0.0, isum = 0.0;
        for (int k = 1; k <= 16; ++k) {
            const double rel = amp[static_cast<std::size_t>(k - 1)] / amp[0];
            amps[static_cast<std::size_t>(k - 1)].push_back(20.0 * std::log10(std::max(rel, 1e-6)));
            if (k >= 2 && rel >= 1e-3) {
                wsum += rel;
                isum += rel * std::fabs(freq[static_cast<std::size_t>(k - 1)] - k * f0) / (k * f0);
            }
        }
        if (wsum > 0.0) inharm.push_back(isum / wsum);
    }
    for (const auto& a2 : amps) r.harmonicsDb.push_back(a2.empty() ? -120.0 : percentile(a2, 0.5));
    r.inharmonicity = inharm.empty() ? 0.0 : percentile(inharm, 0.5);
    return r;
}

// --- envelope ---------------------------------------------------------------------------------

EnvelopeReport envelopeOf(const std::vector<double>& x, double fs, std::optional<double> noteOff) {
    EnvelopeReport r;
    const auto win = static_cast<std::size_t>(std::lround(0.005 * fs));
    const auto hop = static_cast<std::size_t>(std::max(1L, std::lround(0.001 * fs)));
    std::vector<double> env;
    for (std::size_t s = 0; s < x.size(); s += hop)
        env.push_back(rms(x.data() + s, std::min(win, x.size() - s)));
    if (env.empty()) return r;
    const double hopMs = 1000.0 * static_cast<double>(hop) / fs;
    const auto maxIt = std::max_element(env.begin(), env.end());
    const double peak = *maxIt;
    const auto peakIdx = static_cast<std::size_t>(maxIt - env.begin());
    std::size_t i10 = 0, i90 = 0;
    while (i10 < env.size() && env[i10] < 0.1 * peak) ++i10;
    i90 = i10;
    while (i90 < env.size() && env[i90] < 0.9 * peak) ++i90;
    r.attackMs = static_cast<double>(i90 - i10) * hopMs;
    for (std::size_t i = peakIdx; i < env.size(); ++i)
        if (env[i] <= 0.1 * peak) {
            r.decay20Ms = static_cast<double>(i - peakIdx) * hopMs;
            break;
        }
    if (noteOff && *noteOff < static_cast<double>(x.size()) / fs) {
        const auto off = static_cast<std::size_t>(std::max(0.0, *noteOff * 1000.0 / hopMs));
        if (off < env.size() && env[off] > 0.0) {
            const double l0 = env[off];
            for (std::size_t i = off; i < env.size(); ++i)
                if (env[i] <= l0 * 1e-3) {
                    r.releaseMs = static_cast<double>(i - off) * hopMs;
                    break;
                }
            if (!r.releaseMs) {
                std::vector<double> ts, ls;
                for (std::size_t i = off; i < env.size(); ++i) {
                    const double rel = 20.0 * std::log10(std::max(env[i], 1e-12) / l0);
                    if (rel <= -10.0 && rel >= -30.0) {
                        ts.push_back(static_cast<double>(i - off) * hopMs);
                        ls.push_back(rel);
                    }
                }
                if (ts.size() >= 2) {
                    const double mt =
                        std::accumulate(ts.begin(), ts.end(), 0.0) / static_cast<double>(ts.size());
                    const double ml =
                        std::accumulate(ls.begin(), ls.end(), 0.0) / static_cast<double>(ls.size());
                    double sxy = 0.0, sxx = 0.0;
                    for (std::size_t i = 0; i < ts.size(); ++i) {
                        sxy += (ts[i] - mt) * (ls[i] - ml);
                        sxx += (ts[i] - mt) * (ts[i] - mt);
                    }
                    const double slope = sxx > 0.0 ? sxy / sxx : 0.0;
                    if (slope < 0.0) {
                        r.releaseMs = mt + (-60.0 - ml) / slope;
                        r.releaseExtrapolated = true;
                    }
                }
            }
            if (off > i90) {
                std::vector<double> sus(env.begin() + static_cast<std::ptrdiff_t>(i90),
                                        env.begin() + static_cast<std::ptrdiff_t>(off));
                const double m = percentile(sus, 0.5);
                r.sustainDb = 20.0 * std::log10(std::max(m, 1e-12) / peak);
            }
        }
    }
    double samplePeak = 0.0;
    for (double v : x) samplePeak = std::max(samplePeak, std::fabs(v));
    const double total = rms(x.data(), x.size());
    r.crestDb = total > 0.0 ? 20.0 * std::log10(samplePeak / total) : 0.0;
    // Onsets: positive flux of log(1 + 100·M), N ≈ 21 ms, hop N/4.
    const std::size_t n = pow2Nearest(0.0213 * fs);
    Stft st(x, fs, n);
    const double magNorm = 4.0 / static_cast<double>(n);  // full-scale sine peak → 1
    std::vector<double> odf, energy;
    std::vector<double> prev(n / 2 + 1, 0.0);  // the first frame rises from silence
    for (const auto& p : st.power) {
        energy.push_back(std::accumulate(p.begin(), p.end(), 0.0));
        std::vector<double> l(p.size());
        for (std::size_t k = 1; k < p.size(); ++k)
            l[k] = std::log1p(100.0 * std::sqrt(p[k] / st.norm) * magNorm);
        double fl = 0.0;
        for (std::size_t k = 1; k < p.size(); ++k) fl += std::max(0.0, l[k] - prev[k]);
        odf.push_back(fl);
        prev = std::move(l);
    }
    const double gmax = odf.empty() ? 0.0 : *std::max_element(odf.begin(), odf.end());
    const double frameS = static_cast<double>(st.hop) / fs;
    double last = -1.0;
    for (std::size_t t = 0; t < odf.size() && gmax > 0.0; ++t) {
        bool localMax = true;
        for (std::size_t j = t >= 3 ? t - 3 : 0; j <= std::min(odf.size() - 1, t + 3); ++j)
            if (odf[j] > odf[t]) localMax = false;
        if (!localMax) continue;
        std::vector<double> around;
        for (std::size_t j = t >= 8 ? t - 8 : 0; j <= std::min(odf.size() - 1, t + 8); ++j)
            around.push_back(odf[j]);
        if (odf[t] <= percentile(around, 0.5) + 0.1 * gmax) continue;
        // A real onset also raises the energy (≥ 6 dB over the frames before);
        // this rejects decays and the beating of detuned partials.
        const double before = (t >= 2 ? energy[t - 2] : 0.0) + (t >= 3 ? energy[t - 3] : 0.0);
        const double after = energy[t] + (t + 1 < energy.size() ? energy[t + 1] : energy[t]);
        if (after < 4.0 * before + 1e-12) continue;
        const double time = static_cast<double>(t) * frameS;
        if (last >= 0.0 && time - last < 0.030) continue;
        r.onsetTimes.push_back(time);
        last = time;
    }
    const double seconds = static_cast<double>(x.size()) / fs;
    r.onsetsPerSecond = seconds > 0.0 ? static_cast<double>(r.onsetTimes.size()) / seconds : 0.0;
    return r;
}

// --- stereo -----------------------------------------------------------------------------------

double pearson(const float* l, const float* r, std::size_t n) {
    double ml = 0.0, mr = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ml += static_cast<double>(l[i]);
        mr += static_cast<double>(r[i]);
    }
    ml /= static_cast<double>(n);
    mr /= static_cast<double>(n);
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = static_cast<double>(l[i]) - ml, b = static_cast<double>(r[i]) - mr;
        sxy += a * b;
        sxx += a * a;
        syy += b * b;
    }
    return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : (sxx == syy ? 1.0 : 0.0);
}

StereoReport stereoOf(const Signal& s, bool monoLoss, const LoudnessReport* loudness) {
    StereoReport r;
    const float* l = s.channels[0];
    const float* rr = s.channels[1];
    const auto n = static_cast<std::size_t>(s.frames);
    r.correlation = pearson(l, rr, n);
    const auto win = static_cast<std::size_t>(std::lround(0.1 * s.sampleRate));
    std::vector<double> corr;
    for (std::size_t p = 0; p + win <= n; p += win) {
        double e = 0.0;
        for (std::size_t i = p; i < p + win; ++i) {
            const double m = 0.5 * (static_cast<double>(l[i]) + static_cast<double>(rr[i]));
            e += m * m;
        }
        if (std::sqrt(e / static_cast<double>(win)) < kSilence) continue;
        corr.push_back(pearson(l + p, rr + p, win));
    }
    r.correlationMean = corr.empty() ? r.correlation
                                     : std::accumulate(corr.begin(), corr.end(), 0.0) /
                                           static_cast<double>(corr.size());
    r.correlationP5 = corr.empty() ? r.correlation : percentile(corr, 0.05);
    double em = 0.0, es = 0.0, eL = 0.0, eR = 0.0;
    std::vector<double> mid(n), side(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto a = static_cast<double>(l[i]), b = static_cast<double>(rr[i]);
        mid[i] = 0.5 * (a + b);
        side[i] = 0.5 * (a - b);
        em += mid[i] * mid[i];
        es += side[i] * side[i];
        eL += a * a;
        eR += b * b;
    }
    r.width = em + es > 0.0 ? es / (em + es) : 0.0;
    r.balanceDb = eL + eR > 0.0 ? 10.0 * std::log10(eR / eL) : 0.0;
    const std::size_t nfft = pow2Nearest(0.0427 * s.sampleRate);
    Stft sm(mid, s.sampleRate, nfft), ss(side, s.sampleRate, nfft);
    std::vector<double> pm(nfft / 2 + 1, 0.0), ps(nfft / 2 + 1, 0.0);
    for (std::size_t t = 0; t < sm.power.size(); ++t)
        for (std::size_t k = 0; k < pm.size(); ++k) {
            pm[k] += sm.power[t][k];
            ps[k] += ss.power[t][k];
        }
    for (int b = 0; b < kBandCount; ++b) {
        const double lo = kBandEdges[static_cast<std::size_t>(b)];
        const double hi = std::min(kBandEdges[static_cast<std::size_t>(b) + 1], s.sampleRate / 2);
        const double a = hi > lo ? sm.bandPower(pm, lo, hi) : 0.0,
                     c = hi > lo ? sm.bandPower(ps, lo, hi) : 0.0;
        r.bandWidth[static_cast<std::size_t>(b)] = a + c > 0.0 ? c / (a + c) : 0.0;
    }
    // Mono loss: loudness of the stereo signal minus the mono downmix in both channels.
    if (!monoLoss) return r;
    const double stereoIntegrated = loudness ? loudness->integrated : loudnessOf(s).integrated;
    std::vector<float> m32(n);
    for (std::size_t i = 0; i < n; ++i) m32[i] = static_cast<float>(mid[i]);
    const float* both[2] = {m32.data(), m32.data()};
    Signal mono{std::span<const float* const>(both, 2), s.frames, s.sampleRate, Layout::Stereo};
    const LoudnessReport monoL = loudnessOf(mono);
    // Unavailable (NaN, left out of reports) when the stereo loudness is not
    // measurable (under one 400 ms block); a downmix that cancels below the
    // absolute gate is measured against that gate, -70 LUFS.
    r.monoLossLu = !std::isfinite(stereoIntegrated)  ? std::numeric_limits<double>::quiet_NaN()
                   : std::isfinite(monoL.integrated) ? stereoIntegrated - monoL.integrated
                                                     : stereoIntegrated + 70.0;
    return r;
}

// --- JSON with rounding (§8) -----------------------------------------------------------------

// Rounds to `decimals` places by scaling with a power of ten and dividing, so
// the result is the double nearest the decimal value (prints as "-9.2").
double roundTo(double v, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(v * scale) / scale;
}
json::Value num(double v, int decimals) {
    if (!std::isfinite(v)) return json::Value(nullptr);
    return json::Value(roundTo(v, decimals) + 0.0);  // + 0.0: no "-0.0"
}
json::Value tenth(double v) { return num(v, 1); }
json::Value milli(double v) { return num(v, 3); }
// Three significant digits, never finer than `minDecimals` places.
json::Value sig3(double v, int maxDecimals) {
    if (!std::isfinite(v)) return json::Value(nullptr);
    if (v == 0.0) return json::Value(0.0);
    const int decimals =
        std::min(maxDecimals, 2 - static_cast<int>(std::floor(std::log10(std::fabs(v)))));
    if (decimals >= 0) return json::Value(roundTo(v, decimals) + 0.0);
    const double scale = std::pow(10.0, -decimals);
    return json::Value(std::round(v / scale) * scale + 0.0);
}
json::Value hz(double v) { return sig3(v, 0); }  // minimum resolution 1 Hz
json::Value ms(double v) { return sig3(v, 6); }

json::Value statsJson(const Stats& s, bool full, json::Value (*fmt)(double)) {
    if (!full) return fmt(s.median);
    json::Value o = json::Value::object();
    o["mean"] = fmt(s.mean);
    o["median"] = fmt(s.median);
    o["p10"] = fmt(s.p10);
    o["p90"] = fmt(s.p90);
    return o;
}

std::string_view layoutName(Layout l) {
    switch (l) {
        case Layout::Mono:
            return "mono";
        case Layout::Stereo:
            return "stereo";
        case Layout::Surround50:
            return "5.0";
        case Layout::Surround51:
            return "5.1";
    }
    return "stereo";
}

json::Value series(const std::vector<double>& v, json::Value (*fmt)(double)) {
    json::Value a = json::Value::array();
    for (double x : decimate(v, 200)) a.push_back(fmt(x));
    return a;
}

}  // namespace

std::string_view bandName(int band) { return kBandNames[band]; }
double thirdOctaveCentre(int band) { return 1000.0 * std::pow(2.0, (band - 17) / 3.0); }

Result<Report> analyze(const Signal& s, const Options& o) {
    if (s.channels.empty() || s.frames <= 0)
        return Error("analysis.empty", "the signal has no audio", {},
                     "render at least a few milliseconds");
    if (!(s.sampleRate >= 22050.0 && s.sampleRate <= 192000.0))
        return Error("analysis.sample_rate", "sample rate must be 22.05..192 kHz", {},
                     "resample first (audio::resample)");
    const std::size_t expected = s.layout == Layout::Mono         ? 1
                                 : s.layout == Layout::Stereo     ? 2
                                 : s.layout == Layout::Surround50 ? 5
                                 : s.layout == Layout::Surround51 ? 6
                                                                  : 0;
    if (s.channels.size() != expected)
        return Error("analysis.layout", "channel count does not match layout", "/channels",
                     "supply channels in the declared layout order");
    for (const float* channel : s.channels) {
        if (!channel) return Error("analysis.samples", "channel data is null", "/channels");
        for (std::int64_t i = 0; i < s.frames; ++i)
            if (!std::isfinite(channel[i]))
                return Error("analysis.samples", "audio contains nonfinite samples", "/channels",
                             "supply finite audio samples");
    }
    if (o.noteOffSeconds && (!std::isfinite(*o.noteOffSeconds) || *o.noteOffSeconds < 0))
        return Error("analysis.note_off", "note-off must be finite and nonnegative",
                     "/noteOffSeconds", "give the note-off time in seconds");
    if (o.f0RangeHz) {
        const auto [lo, hi] = *o.f0RangeHz;
        const double minHz = s.sampleRate / (std::lround(4096.0 * s.sampleRate / 48000.0) / 2.0);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo < minHz || hi <= lo ||
            hi > s.sampleRate / 2.0)
            return Error("analysis.f0_range", "invalid pitch frequency range", "/f0RangeHz",
                         "use finite ascending frequencies within the YIN window and Nyquist");
    }
    Report r;
    r.seconds = static_cast<double>(s.frames) / s.sampleRate;
    r.sampleRate = s.sampleRate;
    r.channels = static_cast<int>(s.channels.size());
    r.layout = s.layout;
    const std::vector<double> mono = downmix(s);
    if (o.loudness) r.loudness = loudnessOf(s);
    if (o.spectral)
        r.spectral = spectralOf(Stft(mono, s.sampleRate, pow2Nearest(0.0427 * s.sampleRate)));
    if (o.harmonic)
        r.harmonic = harmonicOf(mono, s.sampleRate, o.f0RangeHz.value_or(std::pair{30.0, 4000.0}),
                                o.pitchFrames);
    if (o.envelope) r.envelope = envelopeOf(mono, s.sampleRate, o.noteOffSeconds);
    if (o.stereo && s.layout == Layout::Stereo)
        r.stereo = stereoOf(s, o.monoLoss, r.loudness ? &*r.loudness : nullptr);
    return r;
}

json::Value toJson(const Report& r, Detail detail) {
    const bool standard = detail != Detail::Summary, full = detail == Detail::Full;
    json::Value out = json::Value::object();
    out["schema"] = "supple.analysis-report/1.0";
    json::Value src = json::Value::object();
    src["seconds"] = milli(r.seconds);
    src["sample_rate"] = r.sampleRate;
    src["channels"] = r.channels;
    src["layout"] = layoutName(r.layout);
    out["source"] = std::move(src);
    if (r.loudness) {
        const auto& l = *r.loudness;
        json::Value v = json::Value::object();
        v["integrated_lufs"] = tenth(l.integrated);
        v["lra_lu"] = tenth(l.lra);
        v["max_momentary_lufs"] = tenth(l.maxMomentary);
        v["max_short_term_lufs"] = tenth(l.maxShortTerm);
        v["true_peak_dbtp"] = tenth(l.truePeak);
        v["sample_peak_dbfs"] = tenth(l.samplePeak);
        v["plr_lu"] = tenth(l.plr);
        if (full) {
            v["momentary_lufs"] = series(l.momentary, tenth);
            v["short_term_lufs"] = series(l.shortTerm, tenth);
        }
        out["loudness"] = std::move(v);
    }
    if (r.spectral) {
        const auto& sp = *r.spectral;
        json::Value v = json::Value::object();
        v["centroid_hz"] = statsJson(sp.centroidHz, standard, hz);
        v["rolloff_hz"] = statsJson(sp.rolloffHz, standard, hz);
        v["flatness"] = statsJson(sp.flatness, standard, milli);
        v["flux"] = statsJson(sp.flux, standard, milli);
        v["tilt_db_per_oct"] = tenth(sp.tiltDbPerOct);
        json::Value bands = json::Value::object();
        for (int b = 0; b < kBandCount; ++b) {
            const Band& band = sp.bands[static_cast<std::size_t>(b)];
            if (!standard) {
                bands[bandName(b)] = tenth(band.levelDb);
            } else {
                json::Value bv = json::Value::object();
                bv["level_db"] = tenth(band.levelDb);
                bv["share_pct"] = tenth(band.sharePct);
                if (r.stereo) bv["width"] = milli(r.stereo->bandWidth[static_cast<std::size_t>(b)]);
                bands[bandName(b)] = std::move(bv);
            }
        }
        v["bands"] = std::move(bands);
        if (standard) {
            json::Value ltas = json::Value::array();
            for (double x : sp.ltasDb) ltas.push_back(tenth(std::max(x, -120.0)));
            v["ltas_db"] = std::move(ltas);
        }
        if (full) v["centroid_track_hz"] = series(sp.centroidTrack, hz);
        out["spectral"] = std::move(v);
    }
    if (r.harmonic) {
        const auto& h = *r.harmonic;
        json::Value v = json::Value::object();
        v["pitched"] = h.pitched;
        v["voiced_pct"] = tenth(h.voicedPct);
        if (h.pitched) {
            v["f0_hz"] = hz(h.f0Hz);
            v["note"] = h.note;
            v["stability_cents"] = tenth(h.stabilityCents);
            json::Value harm = json::Value::array();
            const std::size_t count =
                full ? h.harmonicsDb.size() : std::min<std::size_t>(8, h.harmonicsDb.size());
            for (std::size_t k = 0; k < count; ++k) harm.push_back(tenth(h.harmonicsDb[k]));
            v["harmonics_db"] = std::move(harm);
            v["inharmonicity"] = milli(h.inharmonicity);
            v["hnr_db"] = tenth(h.hnrDb);
            if (full) v["f0_track_hz"] = series(h.f0Track, hz);
        }
        out["harmonic"] = std::move(v);
    }
    if (r.envelope) {
        const auto& e = *r.envelope;
        json::Value v = json::Value::object();
        v["attack_ms"] = ms(e.attackMs);
        v["decay20_ms"] = e.decay20Ms ? ms(*e.decay20Ms) : json::Value(nullptr);
        if (e.releaseMs) {
            v["release_ms"] = ms(*e.releaseMs);
            if (e.releaseExtrapolated) v["release_extrapolated"] = true;
        }
        if (e.sustainDb) v["sustain_db"] = tenth(*e.sustainDb);
        v["crest_db"] = tenth(e.crestDb);
        v["onsets_per_s"] = tenth(e.onsetsPerSecond);
        if (full) {
            json::Value on = json::Value::array();
            for (double t : e.onsetTimes) on.push_back(milli(t));
            v["onset_times_s"] = std::move(on);
        }
        out["envelope"] = std::move(v);
    }
    if (r.stereo) {
        const auto& s = *r.stereo;
        json::Value v = json::Value::object();
        v["correlation"] = milli(s.correlation);
        v["width"] = milli(s.width);
        v["balance_db"] = tenth(s.balanceDb);
        v["mono_loss_lu"] = tenth(s.monoLossLu);
        if (standard) {
            v["correlation_mean"] = milli(s.correlationMean);
            v["correlation_p5"] = milli(s.correlationP5);
        }
        out["stereo"] = std::move(v);
    }
    return out;
}

std::map<std::string, double> flatten(const Report& r) {
    std::map<std::string, double> m;
    auto put = [&](const std::string& k, double v) {
        if (std::isfinite(v)) m[k] = v;
    };
    auto putStats = [&](const std::string& k, const Stats& s) {
        put(k + ".mean", s.mean);
        put(k + ".median", s.median);
        put(k + ".p10", s.p10);
        put(k + ".p90", s.p90);
    };
    if (r.loudness) {
        const auto& l = *r.loudness;
        put("loudness.integrated_lufs", l.integrated);
        put("loudness.lra_lu", l.lra);
        put("loudness.max_momentary_lufs", l.maxMomentary);
        put("loudness.max_short_term_lufs", l.maxShortTerm);
        put("loudness.true_peak_dbtp", l.truePeak);
        put("loudness.sample_peak_dbfs", l.samplePeak);
        put("loudness.plr_lu", l.plr);
    }
    if (r.spectral) {
        const auto& s = *r.spectral;
        putStats("spectral.centroid_hz", s.centroidHz);
        putStats("spectral.rolloff_hz", s.rolloffHz);
        putStats("spectral.flatness", s.flatness);
        putStats("spectral.flux", s.flux);
        put("spectral.tilt_db_per_oct", s.tiltDbPerOct);
        for (int b = 0; b < kBandCount; ++b) {
            const std::string k = "spectral.bands." + std::string(bandName(b));
            put(k + ".level_db", s.bands[static_cast<std::size_t>(b)].levelDb);
            put(k + ".share_pct", s.bands[static_cast<std::size_t>(b)].sharePct);
        }
    }
    if (r.harmonic) {
        const auto& h = *r.harmonic;
        put("harmonic.voiced_pct", h.voicedPct);
        if (h.pitched) {
            put("harmonic.f0_hz", h.f0Hz);
            put("harmonic.stability_cents", h.stabilityCents);
            put("harmonic.inharmonicity", h.inharmonicity);
            put("harmonic.hnr_db", h.hnrDb);
            for (std::size_t k = 1; k < h.harmonicsDb.size(); ++k)
                put("harmonic.harmonics_db." + std::to_string(k + 1), h.harmonicsDb[k]);
        }
    }
    if (r.envelope) {
        const auto& e = *r.envelope;
        put("envelope.attack_ms", e.attackMs);
        if (e.decay20Ms) put("envelope.decay20_ms", *e.decay20Ms);
        if (e.releaseMs) put("envelope.release_ms", *e.releaseMs);
        if (e.sustainDb) put("envelope.sustain_db", *e.sustainDb);
        put("envelope.crest_db", e.crestDb);
        put("envelope.onsets_per_s", e.onsetsPerSecond);
    }
    if (r.stereo) {
        const auto& s = *r.stereo;
        put("stereo.correlation", s.correlation);
        put("stereo.correlation_mean", s.correlationMean);
        put("stereo.correlation_p5", s.correlationP5);
        put("stereo.width", s.width);
        put("stereo.balance_db", s.balanceDb);
        put("stereo.mono_loss_lu", s.monoLossLu);
        for (int b = 0; b < kBandCount; ++b)
            put("stereo.bands." + std::string(bandName(b)) + ".width",
                s.bandWidth[static_cast<std::size_t>(b)]);
    }
    return m;
}

std::string summarize(const Report& r) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    if (r.loudness && std::isfinite(r.loudness->integrated)) {
        o << r.loudness->integrated << " LUFS";
        if (std::isfinite(r.loudness->lra)) o << " (LRA " << r.loudness->lra << " LU)";
        o << ", true peak " << r.loudness->truePeak << " dBTP. ";
    } else if (r.loudness) {
        if (r.seconds < 0.4 && std::isfinite(r.loudness->samplePeak))
            o << "Too short to measure loudness. ";
        else
            o << "Silent. ";
    }
    if (r.harmonic) {
        if (r.harmonic->pitched) {
            o << "Pitched " << r.harmonic->note << " (";
            o.precision(0);
            o << r.harmonic->f0Hz << " Hz)";
            o.precision(1);
            if (r.harmonic->harmonicsDb.size() >= 3)
                o << ", 2nd/3rd harmonics " << r.harmonic->harmonicsDb[1] << "/"
                  << r.harmonic->harmonicsDb[2] << " dB";
            if (r.harmonic->inharmonicity > 0.01) o << ", inharmonic";
            o << ". ";
        } else {
            o << "Unpitched. ";
        }
    }
    if (r.spectral && std::isfinite(r.spectral->centroidHz.median)) {
        int loudest = 0;
        for (int b = 1; b < kBandCount; ++b)
            if (r.spectral->bands[static_cast<std::size_t>(b)].levelDb >
                r.spectral->bands[static_cast<std::size_t>(loudest)].levelDb)
                loudest = b;
        o.precision(0);
        o << "Centroid " << r.spectral->centroidHz.median << " Hz";
        o.precision(1);
        o << ", tilt " << r.spectral->tiltDbPerOct << " dB/oct, most energy in "
          << bandName(loudest) << ". ";
    }
    if (r.envelope) {
        o.precision(0);
        o << "Attack " << r.envelope->attackMs << " ms";
        if (r.envelope->decay20Ms) o << ", -20 dB after " << *r.envelope->decay20Ms << " ms";
        o << ". ";
    }
    if (r.stereo) {
        o.precision(2);
        o << "Width " << r.stereo->width << ", correlation " << r.stereo->correlation << ".";
    }
    std::string s = o.str();
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

}  // namespace supple::analysis
