// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "transients.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include "supple/dsp/dsp.h"
#include "supple/model/objects.h"

namespace supple::engine {
Result<std::vector<double>> detectTransients(const audio::AudioData& audio, double sensitivity,
                                             const std::function<bool()>& cancelled) {
    if (!std::isfinite(sensitivity) || sensitivity < 0 || sensitivity > 1 || audio.sampleRate <= 0)
        return Error("audio.sensitivity", "invalid sensitivity or sample rate", "/sensitivity",
                     "use sensitivity 0..1");
    const auto n = static_cast<std::size_t>(1) << static_cast<int>(
                       std::round(std::log2(std::clamp(audio.sampleRate * .0213, 64., 8192.))));
    const auto hop = n / 4;
    const auto count = (audio.frames() + hop - 1) / hop;
    std::vector<double> flux(count), energy(count), envelope(audio.frames());
    for (const auto& ch : audio.channels)
        for (std::size_t i = 0; i < ch.size(); ++i)
            envelope[i] = std::max(envelope[i], std::abs(static_cast<double>(ch[i])));
    dsp::RealFft fft;
    fft.prepare(n);
    std::vector<double> frame(n), window(n), previous(n / 2 + 1), power(n / 2 + 1);
    std::vector<std::complex<double>> spectrum(n / 2 + 1);
    for (std::size_t i = 0; i < n; ++i) window[i] = dsp::hann(i, n);
    for (std::size_t f = 0; f < count; ++f) {
        if (cancelled && cancelled())
            return Error("job.cancelled", "hit detection cancelled", {}, "start detection again");
        std::fill(power.begin(), power.end(), 0.);
        for (const auto& ch : audio.channels) {
            for (std::size_t i = 0; i < n; ++i) {
                const auto at =
                    static_cast<std::int64_t>(f * hop + i) - static_cast<std::int64_t>(n / 2);
                frame[i] = at >= 0 && static_cast<std::size_t>(at) < ch.size()
                               ? static_cast<double>(ch[static_cast<std::size_t>(at)]) * window[i]
                               : 0;
            }
            fft.forward(frame, spectrum);
            for (std::size_t k = 1; k < power.size(); ++k) power[k] += std::norm(spectrum[k]);
        }
        for (std::size_t k = 1; k < power.size(); ++k) {
            const double magnitude = std::sqrt(power[k]) * 4 / static_cast<double>(n);
            const double log = std::log1p(100 * magnitude);
            // Bass bins contribute more, without excluding ringing-note attacks.
            const double weight =
                static_cast<double>(k) * audio.sampleRate / static_cast<double>(n) < 250 ? 2 : 1;
            flux[f] += weight * std::max(0., log - previous[k]);
            energy[f] += power[k];
            previous[k] = log;
        }
    }
    const double maximum = flux.empty() ? 0 : *std::max_element(flux.begin(), flux.end());
    std::vector<double> hits;
    const auto refractory = static_cast<std::size_t>(std::ceil(audio.sampleRate * .015));
    std::size_t last = 0;
    for (std::size_t f = 0; f < count; ++f) {
        if (maximum <= 1e-9 || energy[f] < 1e-10 || (f && flux[f] <= flux[f - 1]) ||
            (f + 1 < count && flux[f] < flux[f + 1]))
            continue;
        std::vector<double> around(
            flux.begin() + static_cast<std::ptrdiff_t>(f > 8 ? f - 8 : 0),
            flux.begin() + static_cast<std::ptrdiff_t>(std::min(count, f + 9)));
        std::sort(around.begin(), around.end());
        if (flux[f] <= around[around.size() / 2] + maximum * (.30 - .28 * sensitivity)) continue;
        const auto from = f * hop > n / 2 ? f * hop - n / 2 : 0;
        const auto to = std::min(audio.frames(), f * hop + n / 2);
        if (from == to) continue;
        const auto peak = static_cast<std::size_t>(
            std::max_element(envelope.begin() + static_cast<std::ptrdiff_t>(from),
                             envelope.begin() + static_cast<std::ptrdiff_t>(to)) -
            envelope.begin());
        const double threshold = envelope[peak] * .12;
        auto attack = peak;
        // Backtrack over a short smoothed envelope, rather than choosing a
        // zero crossing in the middle of a bass attack.
        const auto smooth =
            std::max<std::size_t>(1, static_cast<std::size_t>(audio.sampleRate * .001));
        for (auto i = peak; i > from; --i) {
            double local = 0;
            for (auto j = i; j < std::min(to, i + smooth); ++j)
                local = std::max(local, envelope[j]);
            attack = i;
            if (local < threshold) break;
        }
        if (!hits.empty() && attack < last + refractory) continue;
        // A short FFT window can produce periodic flux peaks on a sustained
        // bass note. Require an energy attack as well as a spectral change.
        const auto width = static_cast<std::size_t>(audio.sampleRate * .025);
        const auto before = attack > width ? attack - width : 0;
        const auto after = std::min(audio.frames(), attack + width);
        double preceding = 0, following = 0;
        for (auto i = before; i < attack; ++i) preceding += envelope[i] * envelope[i];
        for (auto i = attack; i < after; ++i) following += envelope[i] * envelope[i];
        preceding /= static_cast<double>(std::max<std::size_t>(1, attack - before));
        following /= static_cast<double>(std::max<std::size_t>(1, after - attack));
        if (following <= preceding * (2. - .8 * sensitivity)) continue;
        hits.push_back(static_cast<double>(attack) / audio.sampleRate);
        last = attack;
        if (hits.size() > model::kMaxAudioMarkers)
            return Error("audio.hit_budget", "too many detected hits", {},
                         "lower sensitivity or split the source");
    }
    return hits;
}
}  // namespace supple::engine
