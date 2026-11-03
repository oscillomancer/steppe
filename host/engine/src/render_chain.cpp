// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "render_chain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <sstream>

#include "supple/analysis/analysis.h"
#include "supple/dsp/dsp.h"

namespace supple::engine {
namespace {

Result<analysis::Report> loudnessOf(const audio::AudioData& data) {
    auto pointers = data.pointers();
    const auto layout = pointers.size() == 1 ? analysis::Layout::Mono : analysis::Layout::Stereo;
    analysis::Options options;
    options.spectral = false;
    options.harmonic = false;
    options.envelope = false;
    options.stereo = false;
    return analysis::analyze({std::span<const float* const>(pointers.data(), pointers.size()),
                              static_cast<std::int64_t>(data.frames()), data.sampleRate, layout},
                             options);
}

Status finite(const audio::AudioData& data) {
    for (const auto& channel : data.channels)
        for (float sample : channel)
            if (!std::isfinite(sample))
                return Error(
                    "render.normalize", "source audio contains non-finite samples", "/normalize",
                    "bypass or replace the FX producing non-finite audio before normalizing");
    return okStatus();
}

std::string dbText(double value) {
    if (!std::isfinite(value)) return "-inf";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", value);
    return buf;
}

std::optional<rs::NormalizeMode> modeNamed(std::string_view mode) {
    static constexpr std::array<std::pair<std::string_view, rs::NormalizeMode>, 7> kModes{{
        {"none", rs::NormalizeMode::None},
        {"lufs_i", rs::NormalizeMode::LufsI},
        {"lufs_s", rs::NormalizeMode::LufsS},
        {"lufs_m", rs::NormalizeMode::LufsM},
        {"peak", rs::NormalizeMode::Peak},
        {"true_peak", rs::NormalizeMode::TruePeak},
        {"rms", rs::NormalizeMode::Rms},
    }};
    for (const auto& [name, value] : kModes)
        if (name == mode) return value;
    return std::nullopt;
}
}  // namespace

std::string RenderEncoding::extension() const { return std::string(rs::extension(format)); }

void foldSeamlessLoop(audio::AudioData& data, std::size_t rangeFrames) {
    if (rangeFrames == 0) return;
    for (auto& channel : data.channels) {
        for (std::size_t i = rangeFrames; i < channel.size(); ++i)
            channel[i % rangeFrames] += channel[i];
        channel.resize(rangeFrames, 0.0f);
    }
}

void foldMono(audio::AudioData& data) {
    if (data.channelCount() != 2) return;
    for (std::size_t n = 0; n < data.frames(); ++n)
        data.channels[0][n] = static_cast<float>(0.5 * (static_cast<double>(data.channels[0][n]) +
                                                        static_cast<double>(data.channels[1][n])));
    data.channels.resize(1);
}

Result<double> measureLevel(const audio::AudioData& data, rs::NormalizeMode mode) {
    if (mode == rs::NormalizeMode::Rms) {
        double sum = 0;
        for (const auto& channel : data.channels)
            for (float sample : channel)
                sum += static_cast<double>(sample) * static_cast<double>(sample);
        const double count = static_cast<double>(data.frames() * data.channelCount());
        return sum > 0 && count > 0 ? 10.0 * std::log10(sum / count)
                                    : -std::numeric_limits<double>::infinity();
    }
    auto report = loudnessOf(data);
    if (!report) return std::move(report).error();
    switch (mode) {
        case rs::NormalizeMode::LufsI:
            return report->loudness->integrated;
        case rs::NormalizeMode::LufsS:
            return report->loudness->maxShortTerm;
        case rs::NormalizeMode::LufsM:
            return report->loudness->maxMomentary;
        case rs::NormalizeMode::Peak:
            return report->loudness->samplePeak;
        case rs::NormalizeMode::TruePeak:
            return report->loudness->truePeak;
        default:
            return -std::numeric_limits<double>::infinity();
    }
}

Result<double> normalizationGain(double measured, double target, bool onlyReduce) {
    if (!std::isfinite(measured)) return 1.0;
    double gain = std::pow(10.0, (target - measured) / 20.0);
    if (!std::isfinite(gain))
        return Error("render.normalize", "normalization gain is not finite", "/normalize/target",
                     "reduce the target so its gain is finite");
    if (onlyReduce) gain = std::min(gain, 1.0);
    return gain;
}

Status applyGain(audio::AudioData& data, double gain) {
    if (gain == 1.0) return okStatus();
    // Check the complete result before mutating any channel. A finite
    // double gain can still overflow the float buffers used by encoders.
    for (const auto& channel : data.channels)
        for (float sample : channel) {
            const double scaled = static_cast<double>(sample) * gain;
            if (!std::isfinite(scaled) ||
                std::abs(scaled) > static_cast<double>(std::numeric_limits<float>::max()))
                return Error("render.normalize", "normalization would produce non-finite samples",
                             "/normalize/target",
                             "reduce the target to keep samples finite; bypass any FX "
                             "producing non-finite audio");
        }
    for (auto& channel : data.channels)
        for (float& sample : channel)
            sample = static_cast<float>(static_cast<double>(sample) * gain);
    return okStatus();
}

Status limitTruePeak(audio::AudioData& data, double ceilingDb) {
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    if (!(ceiling > 0 && ceiling <= 1))
        return Error("render.normalize", "true peak limit must be at or below 0 dBTP",
                     "/limit_true_peak_db",
                     "provide a finite limit_true_peak_db at or below 0 whose linear ceiling "
                     "remains greater than zero");
    SUPPLE_TRY(finite(data));
    const std::size_t frames = data.frames();
    std::vector<double> peaks(frames, 0.0);
    const int factor = data.sampleRate <= 48000 ? 4 : data.sampleRate <= 96000 ? 2 : 1;
    for (const auto& channel : data.channels) {
        dsp::PolyphaseUpsampler upsampler;
        if (factor > 1) upsampler.prepare(factor);
        std::array<double, 4> phases{};
        for (std::size_t i = 0; i < frames + (factor > 1 ? 16 : 0); ++i) {
            const float sample = i < frames ? channel[i] : 0.0f;
            if (i < frames) peaks[i] = std::max(peaks[i], std::abs(static_cast<double>(sample)));
            if (factor > 1) {
                upsampler.process(sample, std::span<double>(phases.data(), factor));
                if (i >= 16 && i - 16 < frames)
                    for (int phase = 0; phase < factor; ++phase)
                        peaks[i - 16] = std::max(peaks[i - 16],
                                                 std::abs(phases[static_cast<std::size_t>(phase)]));
            }
        }
    }
    std::vector<double> gain(frames, 1.0);
    for (std::size_t i = 0; i < frames; ++i)
        if (peaks[i] > ceiling) gain[i] = ceiling / peaks[i];
    const double attackStep = 1.0 / std::max(1.0, data.sampleRate * 0.005);
    for (std::size_t i = frames; i-- > 1;)
        gain[i - 1] = std::min(gain[i - 1], gain[i] + attackStep);
    const double releaseStep = 1.0 / std::max(1.0, data.sampleRate * 0.05);
    double envelope = 1.0;
    for (std::size_t i = 0; i < frames; ++i) {
        envelope = std::min(gain[i], envelope + releaseStep);
        for (auto& channel : data.channels)
            channel[i] = static_cast<float>(static_cast<double>(channel[i]) * envelope);
    }
    // The final check catches intersample peaks introduced by gain changes.
    auto post = loudnessOf(data);
    if (!post) return std::move(post).error();
    if (post->loudness->truePeak > ceilingDb) {
        const double finalTrim = std::pow(10.0, (ceilingDb - post->loudness->truePeak) / 20.0);
        for (auto& channel : data.channels)
            for (float& sample : channel)
                sample = static_cast<float>(static_cast<double>(sample) * finalTrim);
    }
    return okStatus();
}

std::size_t trimSilence(audio::AudioData& data, bool start, bool end, double thresholdDb) {
    const std::size_t frames = data.frames();
    if (frames == 0 || (!start && !end)) return 0;
    const double threshold = std::pow(10.0, thresholdDb / 20.0);
    auto loud = [&](std::size_t i) {
        for (const auto& channel : data.channels)
            if (std::abs(static_cast<double>(channel[i])) >= threshold) return true;
        return false;
    };
    std::size_t first = 0, last = frames;  // [first, last)
    if (start)
        while (first + 1 < frames && !loud(first)) ++first;
    if (end)
        while (last > first + 1 && !loud(last - 1)) --last;
    if (first == 0 && last == frames) return 0;
    for (auto& channel : data.channels) {
        channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(last), channel.end());
        channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(first));
    }
    return first;
}

Status normalizeRenderAudio(audio::AudioData& data, std::string_view mode, double target,
                            std::optional<double> limitTruePeakDb) {
    const auto parsed = modeNamed(mode);
    if (!parsed)
        return Error("render.normalize", "unsupported normalization mode", "/normalize",
                     "use none, lufs_i, lufs_s, lufs_m, peak, true_peak or rms as normalize.mode");
    if (*parsed != rs::NormalizeMode::None) {
        SUPPLE_TRY(finite(data));
        auto measured = measureLevel(data, *parsed);
        if (!measured) return std::move(measured).error();
        auto gain = normalizationGain(*measured, target, false);
        if (!gain) return std::move(gain).error();
        SUPPLE_TRY(applyGain(data, *gain));
    }
    if (limitTruePeakDb) SUPPLE_TRY(limitTruePeak(data, *limitTruePeakDb));
    return okStatus();
}

Result<std::vector<std::uint8_t>> encodeRender(const audio::AudioData& data,
                                               const RenderEncoding& e, const RenderTags&) {
    if (e.dither == rs::Dither::Shaped)
        return Error("render.format", "noise-shaped dither is not available yet", "/outputs",
                     "use dither tpdf or none");
    switch (e.format) {
        case rs::Format::Wav: {
            audio::WavOptions wav;
            wav.format = e.bitDepth == 16   ? audio::SampleFormat::Pcm16
                         : e.bitDepth == 24 ? audio::SampleFormat::Pcm24
                                            : audio::SampleFormat::Float32;
            wav.dither = e.dither != rs::Dither::None;
            wav.ditherSeed = e.ditherSeed;
            return audio::encodeWav(data, wav);
        }
        case rs::Format::Flac: {
            audio::FlacOptions flac;
            flac.bitsPerSample = e.bitDepth;
            flac.dither = e.dither != rs::Dither::None;
            flac.ditherSeed = e.ditherSeed;
            return audio::encodeFlac(data, flac);
        }
        default:
            return Error("render.format",
                         std::string(rs::formatName(e.format)) + " output is not available yet",
                         "/outputs", "use wav or flac");
    }
}

Result<audio::AudioData> decodeRender(std::span<const std::uint8_t> bytes, rs::Format) {
    return audio::readAudio(bytes);
}

Result<RenderMeasurement> measureRender(const audio::AudioData& data) {
    auto report = loudnessOf(data);
    if (!report) return std::move(report).error();
    RenderMeasurement m;
    m.integrated = report->loudness->integrated;
    m.lra = report->loudness->lra;
    m.maxShortTerm = report->loudness->maxShortTerm;
    m.maxMomentary = report->loudness->maxMomentary;
    m.truePeak = report->loudness->truePeak;
    m.samplePeak = report->loudness->samplePeak;
    m.seconds = data.seconds();
    return m;
}

json::Value renderChecks(const RenderMeasurement& m, const RenderCheckSpec& spec) {
    json::Value checks = json::Value::object();
    auto rounded = [](double v) {
        return std::isfinite(v) ? json::Value(std::round(v * 100) / 100) : json::Value();
    };
    if (spec.loudnessMode) {
        const double measured = *spec.loudnessMode == rs::NormalizeMode::LufsI   ? m.integrated
                                : *spec.loudnessMode == rs::NormalizeMode::LufsS ? m.maxShortTerm
                                                                                 : m.maxMomentary;
        json::Value loudness = json::Value::object();
        loudness["mode"] = *spec.loudnessMode == rs::NormalizeMode::LufsI   ? "lufs_i"
                           : *spec.loudnessMode == rs::NormalizeMode::LufsS ? "lufs_s"
                                                                            : "lufs_m";
        loudness["target"] = spec.target;
        loudness["measured"] = rounded(measured);
        loudness["ok"] =
            std::isfinite(measured) && (spec.onlyReduce ? measured <= spec.target + 1.0
                                                        : std::abs(measured - spec.target) <= 1.0);
        checks["loudness"] = std::move(loudness);
    }
    if (spec.ceiling) {
        json::Value peak = json::Value::object();
        peak["ceiling"] = *spec.ceiling;
        peak["measured"] = rounded(m.truePeak);
        peak["ok"] = !std::isfinite(m.truePeak) || m.truePeak <= *spec.ceiling + 0.1;
        checks["true_peak"] = std::move(peak);
    }
    return checks;
}

std::string loudnessReport(const std::string& file, const RenderEncoding& e,
                           const RenderMeasurement& m, const json::Value& checks) {
    std::ostringstream out;
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.3f", m.seconds);
    out << "File: " << file << "\n";
    out << "Format: " << rs::formatName(e.format);
    if (e.bitDepth) out << " " << e.bitDepth << (e.bitDepth == 32 ? "-bit float" : "-bit");
    out << ", " << e.sampleRate << " Hz\n";
    out << "Duration: " << seconds << " s\n";
    out << "Integrated loudness: " << dbText(m.integrated) << " LUFS\n";
    out << "Loudness range: " << dbText(m.lra) << " LU\n";
    out << "Max short-term loudness: " << dbText(m.maxShortTerm) << " LUFS\n";
    out << "Max momentary loudness: " << dbText(m.maxMomentary) << " LUFS\n";
    out << "True peak: " << dbText(m.truePeak) << " dBTP\n";
    out << "Sample peak: " << dbText(m.samplePeak) << " dBFS\n";
    if (const auto* loudness = checks.find("loudness")) {
        const auto* measured = loudness->find("measured");
        out << "Check loudness: " << (loudness->find("ok")->asBool() ? "pass" : "FAIL")
            << " (target " << dbText(loudness->find("target")->asDouble()) << " LUFS, measured "
            << (measured && measured->isNumber() ? dbText(measured->asDouble()) : "-inf") << ")\n";
    }
    if (const auto* peak = checks.find("true_peak")) {
        const auto* measured = peak->find("measured");
        out << "Check true peak: " << (peak->find("ok")->asBool() ? "pass" : "FAIL") << " (ceiling "
            << dbText(peak->find("ceiling")->asDouble()) << " dBTP, measured "
            << (measured && measured->isNumber() ? dbText(measured->asDouble()) : "-inf") << ")\n";
    }
    return out.str();
}

}  // namespace supple::engine
