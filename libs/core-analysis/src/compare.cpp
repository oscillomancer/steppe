// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <set>
#include <sstream>

#include "supple/analysis/analysis.h"
#include "supple/base/closest.h"
#include "supple/dsp/dsp.h"

namespace supple::analysis {

namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

std::string unitOf(std::string_view key) {
    if (key.find("_per_oct") != std::string_view::npos) return "dB/oct";
    if (key.find("_lufs") != std::string_view::npos) return "LUFS";
    if (key.find("_lu") != std::string_view::npos) return "LU";
    if (key.find("_dbtp") != std::string_view::npos) return "dBTP";
    if (key.find("_dbfs") != std::string_view::npos) return "dBFS";
    if (key.find("_db") != std::string_view::npos || startsWith(key, "harmonic.harmonics_db"))
        return "dB";
    if (key.find("_hz") != std::string_view::npos) return "Hz";
    if (key.find("_ms") != std::string_view::npos) return "ms";
    if (key.find("_pct") != std::string_view::npos) return "%";
    if (key.find("_cents") != std::string_view::npos) return "ct";
    if (key.find("_per_s") != std::string_view::npos) return "/s";
    return "";
}

// How large a change from a to b is relative to its notability threshold
// (spec §6); ≥ 1 is notable, 0 for descriptors without a threshold.
double score(std::string_view key, double a, double b) {
    const double d = std::fabs(b - a);
    auto rel = [&](double frac) {
        return std::fabs(a) > 0.0 ? d / std::fabs(a) / frac : (d > 0.0 ? 1.0 : 0.0);
    };
    if (startsWith(key, "loudness.true_peak") || startsWith(key, "loudness.sample_peak"))
        return d / 0.5;
    if (startsWith(key, "loudness.")) return d / 1.0;
    if (startsWith(key, "spectral.centroid_hz.") || startsWith(key, "spectral.rolloff_hz."))
        return rel(0.10);
    if (startsWith(key, "spectral.bands.") && endsWith(key, ".level_db")) return d / 1.5;
    if (key == "spectral.tilt_db_per_oct") return d / 1.0;
    if (startsWith(key, "spectral.flatness.")) return d / 0.05;
    if (startsWith(key, "stereo.correlation")) return d / 0.1;
    if (key == "stereo.width" || (startsWith(key, "stereo.bands.") && endsWith(key, ".width")))
        return d / 0.05;
    if (key == "envelope.attack_ms" || key == "envelope.decay20_ms" || key == "envelope.release_ms")
        return rel(0.20);
    if (key == "harmonic.f0_hz")
        return a > 0.0 && b > 0.0 ? std::fabs(1200.0 * std::log2(b / a)) / 10.0 : 0.0;
    if (startsWith(key, "harmonic.harmonics_db.")) {
        // Harmonics 2..8 (the summary's set); higher ones are too weak to lead.
        const int k = std::atoi(std::string(key.substr(key.rfind('.') + 1)).c_str());
        return k <= 8 ? d / 3.0 : 0.0;
    }
    return 0.0;
}

// Every key a report can carry (profiles are validated against it).
const std::set<std::string>& vocabulary() {
    static const std::set<std::string> keys = [] {
        std::set<std::string> k;
        for (auto s : {"loudness.integrated_lufs",
                       "loudness.lra_lu",
                       "loudness.max_momentary_lufs",
                       "loudness.max_short_term_lufs",
                       "loudness.true_peak_dbtp",
                       "loudness.sample_peak_dbfs",
                       "loudness.plr_lu",
                       "spectral.tilt_db_per_oct",
                       "harmonic.voiced_pct",
                       "harmonic.f0_hz",
                       "harmonic.stability_cents",
                       "harmonic.inharmonicity",
                       "harmonic.hnr_db",
                       "envelope.attack_ms",
                       "envelope.decay20_ms",
                       "envelope.release_ms",
                       "envelope.sustain_db",
                       "envelope.crest_db",
                       "envelope.onsets_per_s",
                       "stereo.correlation",
                       "stereo.correlation_mean",
                       "stereo.correlation_p5",
                       "stereo.width",
                       "stereo.balance_db",
                       "stereo.mono_loss_lu"})
            k.insert(s);
        for (auto d : {"centroid_hz", "rolloff_hz", "flatness", "flux"})
            for (auto s : {"mean", "median", "p10", "p90"})
                k.insert("spectral." + std::string(d) + "." + s);
        for (int b = 0; b < kBandCount; ++b) {
            k.insert("spectral.bands." + std::string(bandName(b)) + ".level_db");
            k.insert("spectral.bands." + std::string(bandName(b)) + ".share_pct");
            k.insert("stereo.bands." + std::string(bandName(b)) + ".width");
        }
        for (int h = 2; h <= 16; ++h) k.insert("harmonic.harmonics_db." + std::to_string(h));
        return k;
    }();
    return keys;
}

double ltasRms(const std::array<double, kThirdOctaves>& a,
               const std::array<double, kThirdOctaves>& b,
               std::array<double, kThirdOctaves>* deltas) {
    double sum = 0.0;
    int n = 0, shared = 0;
    std::array<double, kThirdOctaves> d{};
    std::array<bool, kThirdOctaves> use{};
    for (int i = 0; i < kThirdOctaves; ++i) {
        const auto u = static_cast<std::size_t>(i);
        d[u] = std::max(b[u], -100.0) - std::max(a[u], -100.0);
        use[u] = a[u] > -100.0 || b[u] > -100.0;
        if (use[u]) ++n;
        if (a[u] > -100.0 && b[u] > -100.0) {
            sum += d[u];
            ++shared;
        }
    }
    if (deltas) *deltas = d;
    if (n == 0) return 0.0;
    const double mean = shared ? sum / shared : 0.0;
    double sq = 0.0;
    for (int i = 0; i < kThirdOctaves; ++i)
        if (use[static_cast<std::size_t>(i)])
            sq += (d[static_cast<std::size_t>(i)] - mean) * (d[static_cast<std::size_t>(i)] - mean);
    return std::sqrt(sq / n);
}

json::Value round(double v, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::isfinite(v) ? json::Value(std::round(v * scale) / scale + 0.0)
                            : json::Value(nullptr);
}

// Zwicker critical-band edges (Hz): 24 Bark bands.
constexpr double kBark[25] = {20,   100,  200,  300,  400,  510,   630,  770,  920,
                              1080, 1270, 1480, 1720, 2000, 2320,  2700, 3150, 3700,
                              4400, 5300, 6400, 7700, 9500, 12000, 15500};

std::vector<std::vector<double>> barkFrames(const Signal& s, std::int64_t frames, std::size_t n) {
    // Mono downmix, zero-padded to `frames`.
    std::vector<double> x(static_cast<std::size_t>(frames), 0.0);
    constexpr double w50[5] = {1, 1, 1, 1.41, 1.41};
    constexpr double w51[6] = {1, 1, 1, 0, 1.41, 1.41};
    for (std::int64_t i = 0; i < s.frames; ++i) {
        double v = 0.0;
        if (s.layout == Layout::Mono) {
            v = static_cast<double>(s.channels[0][i]);
        } else if (s.layout == Layout::Stereo) {
            v = 0.5 *
                (static_cast<double>(s.channels[0][i]) + static_cast<double>(s.channels[1][i]));
        } else {
            const double* weights = s.layout == Layout::Surround50 ? w50 : w51;
            double sum = 0.0;
            for (std::size_t c = 0; c < s.channels.size(); ++c) {
                v += weights[c] * static_cast<double>(s.channels[c][i]);
                sum += weights[c];
            }
            v /= sum;
        }
        x[static_cast<std::size_t>(i)] = v;
    }
    std::vector<double> w(n);
    double w2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = dsp::hann(i, n);
        w2 += w[i] * w[i];
    }
    const double norm = 2.0 / (static_cast<double>(n) * w2);
    dsp::RealFft fft;
    fft.prepare(n);
    std::vector<double> frame(n);
    std::vector<std::complex<double>> spec(n / 2 + 1);
    std::vector<std::vector<double>> out;
    const double df = s.sampleRate / static_cast<double>(n);
    for (std::size_t start = 0; start < x.size(); start += n / 4) {
        for (std::size_t i = 0; i < n; ++i)
            frame[i] = start + i < x.size() ? x[start + i] * w[i] : 0.0;
        fft.forward(frame, spec);
        std::vector<double> bands(24, 0.0);
        for (std::size_t k = 1; k <= n / 2; ++k) {
            const double f = static_cast<double>(k) * df;
            for (int b = 0; b < 24; ++b)
                if (f >= kBark[b] && f < kBark[b + 1]) {
                    bands[static_cast<std::size_t>(b)] += std::norm(spec[k]) * norm;
                    break;
                }
        }
        out.push_back(std::move(bands));
        if (start + n >= x.size()) break;
    }
    return out;
}

}  // namespace

Comparison compare(const Report& reference, const Report& test) {
    Comparison c;
    const auto a = flatten(reference), b = flatten(test);
    for (const auto& [key, va] : a) {
        auto it = b.find(key);
        if (it == b.end()) continue;
        c.deltas.push_back(
            {key, va, it->second, it->second - va, unitOf(key), score(key, va, it->second) >= 1.0});
    }
    // Notable first, largest relative to its threshold first.
    std::stable_sort(
        c.deltas.begin(), c.deltas.end(), [](const DescriptorDelta& x, const DescriptorDelta& y) {
            if (x.notable != y.notable) return x.notable;
            return score(x.key, x.reference, x.test) > score(y.key, y.reference, y.test);
        });
    if (reference.spectral && test.spectral)
        c.ltasRmsDb = ltasRms(reference.spectral->ltasDb, test.spectral->ltasDb, &c.ltasDeltaDb);
    return c;
}

json::Value toJson(const Comparison& c) {
    json::Value out = json::Value::object();
    out["schema"] = "supple.analysis-comparison/1.0";
    json::Value deltas = json::Value::array();
    for (const auto& d : c.deltas) {
        if (!d.notable) continue;
        json::Value v = json::Value::object();
        v["key"] = d.key;
        v["reference"] = round(d.reference, 3);
        v["test"] = round(d.test, 3);
        v["delta"] = round(d.delta, 3);
        if (!d.unit.empty()) v["unit"] = d.unit;
        deltas.push_back(std::move(v));
    }
    out["notable"] = std::move(deltas);
    out["ltas_rms_db"] = round(c.ltasRmsDb, 1);
    json::Value ltas = json::Value::array();
    for (double d : c.ltasDeltaDb) ltas.push_back(round(d, 1));
    out["ltas_delta_db"] = std::move(ltas);
    out["summary"] = summarize(c);
    return out;
}

std::string summarize(const Comparison& c) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    int shown = 0;
    for (const auto& d : c.deltas) {
        if (!d.notable || shown == 5) continue;
        if (shown) o << "; ";
        o << d.key << " " << (d.delta >= 0 ? "+" : "") << d.delta;
        if (!d.unit.empty()) o << " " << d.unit;
        ++shown;
    }
    if (!shown) o << "No notable descriptor changes";
    o << ". Timbre difference " << c.ltasRmsDb << " dB (LTAS RMS, level removed).";
    return o.str();
}

Result<ProfileResult> evaluate(const Report& report, const json::Value& profile) {
    if (!profile.isObject()) return Error("analysis.profile", "a profile is an object", "");
    if (const auto* s = profile.find("schema");
        !s || !s->isString() || s->asString() != "supple.analysis-profile/1.0")
        return Error("analysis.profile.schema",
                     "profile schema must be supple.analysis-profile/1.0", "/schema");
    const auto values = flatten(report);
    ProfileResult r;
    double mean = 0.0, weights = 0.0, scale = 0.0;
    auto accumulate = [&](double distance, double weight) {
        if (weight == 0) return;
        if (weight > scale) {
            weights *= scale / weight;
            scale = weight;
        }
        const double scaled = weight / scale;
        const double next = weights + scaled;
        mean = mean * (weights / next) + distance * (scaled / next);
        weights = next;
    };
    bool pass = true;
    if (const auto* targets = profile.find("targets")) {
        if (!targets->isObject())
            return Error("analysis.profile", "targets must be an object", "/targets");
        std::vector<std::string_view> vocab(vocabulary().begin(), vocabulary().end());
        for (const auto& m : targets->asObject()) {
            const std::string path = "/targets/" + m.key;
            if (!vocabulary().count(m.key)) {
                const std::string best = closestMatch(m.key, vocab);
                return Error("analysis.profile.key", "unknown report key \"" + m.key + "\"", path,
                             best.empty()
                                 ? "keys are dotted report paths, e.g. spectral.centroid_hz.median"
                                 : "did you mean \"" + best + "\"?");
            }
            const json::Value& t = m.value;
            if (!t.isObject()) return Error("analysis.profile", "a target is an object", path);
            const auto* weightValue = t.find("weight");
            if (weightValue &&
                (!weightValue->isNumber() || !std::isfinite(weightValue->asDouble()) ||
                 weightValue->asDouble() <= 0.0))
                return Error("analysis.profile.weight", "weight must be a positive number",
                             path + "/weight");
            const double weight = weightValue ? weightValue->asDouble() : 1.0;
            const auto* target = t.find("target");
            const auto* tol = t.find("tolerance");
            const auto* minValue = t.find("min");
            const auto* maxValue = t.find("max");
            if (target) {
                if (!target->isNumber() || !std::isfinite(target->asDouble()) || !tol ||
                    !tol->isNumber() || !std::isfinite(tol->asDouble()) || tol->asDouble() <= 0.0)
                    return Error("analysis.profile",
                                 "target needs a number and a positive tolerance", path,
                                 R"(e.g. {"target": 1200, "tolerance": 150})");
            } else if (minValue || maxValue) {
                if ((minValue && (!minValue->isNumber() || !std::isfinite(minValue->asDouble()))) ||
                    (maxValue && (!maxValue->isNumber() || !std::isfinite(maxValue->asDouble()))) ||
                    (minValue && maxValue && minValue->asDouble() > maxValue->asDouble()))
                    return Error("analysis.profile.range", "min and max must be ordered numbers",
                                 path);
            } else {
                return Error("analysis.profile", "a target has target+tolerance or min/max", path);
            }
            TargetResult tr;
            tr.key = m.key;
            tr.weight = weight;
            auto it = values.find(m.key);
            if (it == values.end()) {
                // Not applicable to this report (e.g. f0 of unpitched audio): fails hard.
                tr.pass = false;
                tr.distance = 10.0;
            } else {
                tr.value = it->second;
                if (target) {
                    tr.distance = std::fabs(tr.value - target->asDouble()) / tol->asDouble();
                    tr.pass = tr.distance <= 1.0;
                } else if (minValue || maxValue) {
                    const double lo = minValue ? minValue->asDouble() : -1e300;
                    const double hi = maxValue ? maxValue->asDouble() : 1e300;
                    const double span = hi - lo < 1e299 && hi > lo ? hi - lo : 1.0;
                    tr.distance = tr.value < lo   ? (lo - tr.value) / span
                                  : tr.value > hi ? (tr.value - hi) / span
                                                  : 0.0;
                    tr.pass = tr.distance == 0.0;
                }
            }
            pass = pass && tr.pass;
            accumulate(tr.distance, weight);
            r.targets.push_back(tr);
        }
    }
    if (const auto* ltas = profile.find("ltas")) {
        if (!ltas->isArray() || ltas->size() != static_cast<std::size_t>(kThirdOctaves))
            return Error("analysis.profile", "ltas holds 31 relative band levels (dB)", "/ltas");
        if (!report.spectral)
            return Error("analysis.profile", "the report has no spectral section", "/ltas");
        std::array<double, kThirdOctaves> want{};
        for (int i = 0; i < kThirdOctaves; ++i) {
            const json::Value& v = ltas->at(static_cast<std::size_t>(i));
            if (!v.isNumber() || !std::isfinite(v.asDouble()))
                return Error("analysis.profile.ltas", "LTAS levels must be numbers",
                             "/ltas/" + std::to_string(i));
            want[static_cast<std::size_t>(i)] = v.asDouble();
        }
        TargetResult tr;
        tr.key = "ltas";
        tr.value = ltasRms(want, report.spectral->ltasDb, nullptr);
        tr.distance = tr.value / 3.0;  // 3 dB RMS = one tolerance
        tr.pass = tr.distance <= 1.0;
        pass = pass && tr.pass;
        accumulate(tr.distance, 1.0);
        r.targets.push_back(tr);
    }
    r.loss = mean;
    r.pass = pass;
    return r;
}

json::Value toJson(const ProfileResult& r) {
    json::Value out = json::Value::object();
    json::Value targets = json::Value::array();
    for (const auto& t : r.targets) {
        json::Value v = json::Value::object();
        v["key"] = t.key;
        v["value"] = round(t.value, 3);
        v["pass"] = t.pass;
        v["distance"] = round(t.distance, 3);
        targets.push_back(std::move(v));
    }
    out["targets"] = std::move(targets);
    out["loss"] = round(r.loss, 3);
    out["pass"] = r.pass;
    return out;
}

Result<MaskingReport> masking(const Signal& masker, const Signal& maskee) {
    if (masker.channels.empty() || maskee.channels.empty() || masker.frames <= 0 ||
        maskee.frames <= 0)
        return Error("analysis.empty", "both signals need audio", {});
    if (masker.sampleRate != maskee.sampleRate)
        return Error("analysis.sample_rate", "masker and maskee must share a sample rate", {},
                     "resample one of them");
    auto validLayout = [](const Signal& s) {
        const std::size_t expected = s.layout == Layout::Mono         ? 1
                                     : s.layout == Layout::Stereo     ? 2
                                     : s.layout == Layout::Surround50 ? 5
                                     : s.layout == Layout::Surround51 ? 6
                                                                      : 0;
        return expected != 0 && s.channels.size() == expected;
    };
    if (!validLayout(masker) || !validLayout(maskee))
        return Error("analysis.layout", "channel count does not match layout", "/channels");
    if (!(masker.sampleRate >= 22050.0 && masker.sampleRate <= 192000.0))
        return Error("analysis.sample_rate", "sample rate must be 22.05..192 kHz", {});
    for (const Signal* signal : {&masker, &maskee}) {
        for (const float* channel : signal->channels) {
            if (!channel) return Error("analysis.samples", "channel data is null", "/channels");
            for (std::int64_t i = 0; i < signal->frames; ++i)
                if (!std::isfinite(channel[i]))
                    return Error("analysis.samples", "audio contains nonfinite samples",
                                 "/channels", "supply finite audio samples");
        }
    }
    const std::int64_t frames = std::max(masker.frames, maskee.frames);
    const double fs = masker.sampleRate;
    const auto n = static_cast<std::size_t>(1)
                   << static_cast<int>(std::round(std::log2(0.0427 * fs)));
    const auto a = barkFrames(masker, frames, n), b = barkFrames(maskee, frames, n);
    MaskingReport r;
    std::vector<double> activeCount(24, 0.0), maskedCount(24, 0.0), excess(24, 0.0),
        maskedEnergy(24, 0.0);
    double totalEnergy = 0.0, totalMasked = 0.0;
    auto spread = [](double dz) {
        const double t = dz + 0.474;
        return 15.81 + 7.5 * t - 17.5 * std::sqrt(1.0 + t * t);
    };
    for (std::size_t t = 0; t < std::min(a.size(), b.size()); ++t) {
        for (int j = 0; j < 24; ++j) {
            const double pe = b[t][static_cast<std::size_t>(j)];
            const double le = 10.0 * std::log10(std::max(pe / 0.5, 1e-30));
            if (le <= -70.0) continue;
            double thr = 0.0;  // power sum of spread masker contributions
            for (int i = 0; i < 24; ++i) {
                const double pm = a[t][static_cast<std::size_t>(i)];
                if (pm <= 0.0) continue;
                const double lm = 10.0 * std::log10(pm / 0.5);
                thr += std::pow(10.0, (lm + spread(j - i)) / 10.0);
            }
            const double thrDb = thr > 0.0 ? 10.0 * std::log10(thr) - 10.0 : -300.0;
            activeCount[static_cast<std::size_t>(j)] += 1.0;
            totalEnergy += pe;
            if (le < thrDb) {
                maskedCount[static_cast<std::size_t>(j)] += 1.0;
                excess[static_cast<std::size_t>(j)] += thrDb - le;
                maskedEnergy[static_cast<std::size_t>(j)] += pe;
                totalMasked += pe;
            }
        }
    }
    for (int j = 0; j < 24; ++j) {
        MaskingBand band;
        band.lowHz = kBark[j];
        band.highHz = kBark[j + 1];
        const auto u = static_cast<std::size_t>(j);
        band.maskedPct = activeCount[u] > 0.0 ? 100.0 * maskedCount[u] / activeCount[u] : 0.0;
        band.meanExcessDb = maskedCount[u] > 0.0 ? excess[u] / maskedCount[u] : 0.0;
        r.bands.push_back(band);
    }
    r.maskedEnergyPct = totalEnergy > 0.0 ? 100.0 * totalMasked / totalEnergy : 0.0;
    std::vector<int> order(24);
    for (int j = 0; j < 24; ++j) order[static_cast<std::size_t>(j)] = j;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        return maskedEnergy[static_cast<std::size_t>(x)] >
               maskedEnergy[static_cast<std::size_t>(y)];
    });
    for (int k = 0; k < 3; ++k)
        if (maskedEnergy[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])] > 0.0)
            r.worst.push_back(order[static_cast<std::size_t>(k)]);
    return r;
}

json::Value toJson(const MaskingReport& m) {
    json::Value out = json::Value::object();
    out["schema"] = "supple.analysis-masking/1.0";
    out["note"] = "Estimate for corrective mixing decisions, not a psychoacoustic model of record.";
    out["masked_energy_pct"] = round(m.maskedEnergyPct, 1);
    json::Value worst = json::Value::array();
    for (int w : m.worst) {
        json::Value v = json::Value::object();
        const MaskingBand& b = m.bands[static_cast<std::size_t>(w)];
        v["low_hz"] = b.lowHz;
        v["high_hz"] = b.highHz;
        v["masked_pct"] = round(b.maskedPct, 1);
        v["mean_excess_db"] = round(b.meanExcessDb, 1);
        worst.push_back(std::move(v));
    }
    out["worst"] = std::move(worst);
    json::Value bands = json::Value::array();
    for (const auto& b : m.bands) {
        json::Value v = json::Value::object();
        v["low_hz"] = b.lowHz;
        v["high_hz"] = b.highHz;
        v["masked_pct"] = round(b.maskedPct, 1);
        v["mean_excess_db"] = round(b.meanExcessDb, 1);
        bands.push_back(std::move(v));
    }
    out["bands"] = std::move(bands);
    return out;
}

}  // namespace supple::analysis
