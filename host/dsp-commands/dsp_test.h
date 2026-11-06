// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// dsp.test, the DSP script harness (ADR-0032), shared by the engine and the
// sandbox: standard signals through a fresh instance of an effect, or notes
// into an instrument (a script's, through the script player, or any other),
// each measured, with what looks wrong said in words. It renders with the sandbox's offline
// renderer (supple/sandbox/render.h), the same in both.
//
// Host is the Context's service and has findPlugin(id), plugins(rescan) and
// scriptDir(); TestTraits<Host> gives the render settings, the host identity
// and the folder for saved outputs.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "artifact_name.h"
#include "dsp_shared.h"
#include "supple/audio/audio_file.h"
#include "supple/clap/semantic_params.h"
#include "supple/dsp/dsp.h"
#include "supple/host/clap_host.h"
#include "supple/params/params.h"
#include "supple/sandbox/render.h"

namespace supple::dsp_commands {

template <class Host>
struct TestTraits;  // static settings(h), identity(h), outputDir(h)

namespace test {
namespace fs = std::filesystem;

enum class Signal { Sine, Sweep, Impulse, Noise, Silence, Notes, Note, File };
inline constexpr Signal kEffectSignals[] = {Signal::Sine, Signal::Sweep, Signal::Impulse,
                                            Signal::Noise, Signal::Silence};
inline constexpr Signal kInstrumentSignals[] = {Signal::Notes, Signal::Note, Signal::Silence};
inline bool forInstruments(Signal s) { return s == Signal::Notes || s == Signal::Note; }

inline const char* signalName(Signal s) {
    switch (s) {
        case Signal::Sine:
            return "sine";
        case Signal::Sweep:
            return "sweep";
        case Signal::Impulse:
            return "impulse";
        case Signal::Noise:
            return "noise";
        case Signal::Notes:
            return "notes";
        case Signal::Note:
            return "note";
        case Signal::File:
            return "file";
        default:
            return "silence";
    }
}

struct AutomationEvent {
    std::string param;
    double atS = 0.0;
    json::Value value;
};
struct SidechainArgs {
    Signal signal = Signal::Sine;
    double levelDb = -6.0, frequencyHz = 100.0;
    std::optional<double> startS, lengthS;
};
struct Note {
    int key = 60;
    double velocity = 0.8, startS = 0.0, lengthS = 0.0;
};
struct TestArgs {
    std::optional<std::string> plugin, script, audioFile;
    std::optional<json::Value> patch;
    std::optional<std::vector<Signal>> signals;
    std::optional<std::vector<AutomationEvent>> automation;
    std::optional<SidechainArgs> sidechain;
    std::optional<std::vector<Note>> notes;
    double levelDb = -12.0;
    double frequencyHz = 1000.0;
    double seconds = 1.0;
    std::optional<double> sampleRate;
    bool save = false;
    int tracePoints = 100;
};
struct ResponsePoint {
    double hz = 0.0, db = 0.0;
};
struct AutomationResult {
    double maxStep = 0.0, baselineMaxStep = 0.0, excessStep = 0.0;
};
struct SidechainResult {
    double rmsDbfs = -200.0, referenceRmsDbfs = -200.0;
    json::Value gainChangeDb;  // null when either measurement is below -120 dBFS
};
// One name a script traced: per interval, the last value given and the least
// and greatest, null where it gave none.
struct TraceResult {
    std::string name;
    json::Value last, low, high;
    std::int64_t nonFinite = 0;
};
struct SignalResult {
    std::string signal;
    double peakDbfs = -200.0, rmsDbfs = -200.0, dc = 0.0;
    std::int64_t clipped = 0, denormals = 0;
    std::optional<double> gainDb, thdPercent;
    std::optional<std::vector<ResponsePoint>> response;
    std::optional<double> pitchHz, releaseMs;
    std::optional<std::string> path;
    std::optional<AutomationResult> automation;
    std::optional<SidechainResult> sidechain;
    std::optional<json::Value> meters;  // read-only parameters by key, physical
    std::optional<double> traceIntervalS;
    std::optional<std::vector<TraceResult>> traces;
};
struct TestResult {
    std::string plugin;
    double sampleRate = 0.0;
    std::int64_t latency = 0;
    std::vector<SignalResult> signals;
    double cpuPercent = 0.0;
    std::vector<std::string> problems;
    std::vector<std::string> log;
};

// Plugin validation paths are relative to its patch, not the command arguments.
inline Error patchError(Error error) {
    error.path = "/patch" + error.path;
    for (auto& detail : error.details) detail = patchError(std::move(detail));
    return error;
}

inline double toDb(double amplitude) {
    return amplitude > 1e-10 ? 20.0 * std::log10(amplitude) : -200.0;
}
// What a reader needs: dB to a hundredth, the rest to a few digits. Divided
// by a power of ten, so the result is the double nearest the decimal.
inline double rounded(double v, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(v * scale) / scale;
}

// Stereo, the same on both channels, at the session's rate.
inline audio::AudioData stimulus(Signal s, double rate, double seconds, double levelDb, double hz) {
    const auto frames = static_cast<std::size_t>(std::llround(rate * seconds));
    audio::AudioData a;
    a.sampleRate = rate;
    std::vector<float> x(frames, 0.0f);
    const double amp = std::pow(10.0, levelDb / 20.0);
    const double twoPi = 2.0 * std::numbers::pi;
    switch (s) {
        case Signal::Sine:
            for (std::size_t i = 0; i < frames; ++i)
                x[i] =
                    static_cast<float>(amp * std::sin(twoPi * hz * static_cast<double>(i) / rate));
            break;
        case Signal::Sweep: {
            // Exponential, 20 Hz to 20 kHz (or 0.45 of the rate).
            const double f0 = 20.0, f1 = std::min(20000.0, 0.45 * rate);
            const double k = std::log(f1 / f0);
            for (std::size_t i = 0; i < frames; ++i) {
                const double t = static_cast<double>(i) / rate;
                const double phase = twoPi * f0 * seconds / k * (std::exp(t / seconds * k) - 1.0);
                x[i] = static_cast<float>(amp * std::sin(phase));
            }
            break;
        }
        case Signal::Impulse:
            if (frames > 0) x[0] = static_cast<float>(amp);
            break;
        case Signal::Noise: {
            // Pink (Paul Kellet's filter on seeded white noise), without DC,
            // peaking at the level like every other signal.
            std::mt19937 rng(0x5eed1234u);
            std::uniform_real_distribution<double> white(-1.0, 1.0);
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0;
            std::vector<double> y(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                const double w = white(rng);
                b0 = 0.99886 * b0 + w * 0.0555179;
                b1 = 0.99332 * b1 + w * 0.0750759;
                b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856;
                b4 = 0.55000 * b4 + w * 0.5329522;
                b5 = -0.7616 * b5 - w * 0.0168980;
                y[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
                b6 = w * 0.115926;
                sum += y[i];
            }
            const double mean = frames ? sum / static_cast<double>(frames) : 0.0;
            double peak = 0.0;
            for (double& v : y) peak = std::max(peak, std::abs(v -= mean));
            const double scale = peak > 0.0 ? amp / peak : 0.0;
            for (std::size_t i = 0; i < frames; ++i) x[i] = static_cast<float>(y[i] * scale);
            break;
        }
        case Signal::Silence:
        case Signal::Notes:  // an instrument's: notes, no audio
        case Signal::Note:
        case Signal::File:  // the file's own (fileSignal)
            break;
    }
    a.channels.assign(2, x);
    return a;
}

// The longest file a file signal plays, and the longest file it reads: a
// longer one would be decoded whole only to be cut.
inline constexpr double kFileSeconds = 60.0;
inline constexpr double kFileReadSeconds = 600.0;

// What a file signal plays: the file at the render's rate, stereo (a mono
// file on both sides, of more channels the first two), its first 60 s.
inline Result<audio::AudioData> fileSignal(const std::string& path, double rate,
                                           std::vector<std::string>& log) {
    if (!fs::path(path).is_absolute())
        return Error("dsp.file", "audio_file is not an absolute path", "/audio_file",
                     "give the file's full path");
    auto info = audio::inspect(path);
    if (!info) return std::move(info).error().at("/audio_file");
    if (info->sampleRate > 0 &&
        static_cast<double>(info->frames) / info->sampleRate > kFileReadSeconds)
        return Error("dsp.file",
                     "audio_file is " +
                         std::to_string(std::lround(info->frames / info->sampleRate)) +
                         " s long; at most its first 60 s would play",
                     "/audio_file", "give a file of 10 minutes or less: a section of it");
    auto audio = audio::readAudio(path);
    if (!audio) return std::move(audio).error().at("/audio_file");
    if (audio->channels.empty() || audio->frames() == 0)
        return Error("dsp.file", "audio_file holds no audio", "/audio_file",
                     "give a WAV, FLAC or MP3 with at least one frame");
    if (audio->truncated) log.push_back("audio_file is truncated; playing what is there");
    if (audio->channels.size() > 2) {
        log.push_back("audio_file has " + std::to_string(audio->channels.size()) +
                      " channels; the first two played");
        audio->channels.resize(2);
    }
    if (audio->channels.size() == 1) audio->channels.push_back(audio->channels.front());
    // Cut before resampling, so only what plays is resampled.
    auto cut = [&](double at) {
        const auto most = static_cast<std::size_t>(std::llround(kFileSeconds * at));
        if (audio->frames() <= most) return false;
        for (auto& c : audio->channels) c.resize(most);
        return true;
    };
    if (cut(audio->sampleRate))
        log.push_back("audio_file is longer than 60 s; its first 60 s played");
    if (audio->sampleRate != rate) {
        log.push_back("audio_file is at " + std::to_string(std::lround(audio->sampleRate)) +
                      " Hz; resampled to " + std::to_string(std::lround(rate)) + " Hz");
        auto resampled = audio::resample(*audio, rate);
        if (!resampled) return std::move(resampled).error().at("/audio_file");
        audio = std::move(resampled);
        cut(rate);  // the resampler's rounding
    }
    audio->sampleRate = rate;
    return std::move(*audio);
}

// An instrument's stimulus: its notes (key, on, at a fraction of `seconds`),
// at velocity 0.8. notes: a note, then a chord, released 0.2 s before the
// end; note: A4 (key 69), released likewise; silence: none.
inline std::vector<sandbox::TimedEvent> noteEvents(Signal s, double rate, double seconds) {
    std::vector<sandbox::TimedEvent> out;
    auto add = [&](double at, int key, bool on) {
        sandbox::TimedEvent e;
        e.sample = static_cast<std::int64_t>(std::llround(at * seconds * rate));
        e.event.kind = on ? host::Event::Kind::NoteOn : host::Event::Kind::NoteOff;
        e.event.key = static_cast<std::int16_t>(key);
        e.event.velocity = 0.8;
        out.push_back(e);
    };
    if (s == Signal::Notes) {
        add(0.0, 60, true);
        add(0.2, 60, false);
        for (int key : {60, 64, 67}) add(0.3, key, true);
        for (int key : {60, 64, 67}) add(0.8, key, false);
    } else if (s == Signal::Note) {
        add(0.0, 69, true);
        add(0.8, 69, false);
    }
    return out;
}
// When the last note is released, as a fraction of `seconds`.
inline constexpr double kReleaseAt = 0.8;

// All validation and conversions happen on main, before the renderer starts.
inline Result<std::pair<std::int64_t, std::int64_t>> timeSpan(double start, double length,
                                                              double seconds, double rate,
                                                              const std::string& path,
                                                              const std::string& code) {
    if (!(start >= 0.0 && start < seconds))
        return Error(code, "start lies outside the musical render", path + "/start_s",
                     "use a start in [0, seconds)");
    if (!(length > 0.0 && start + length <= seconds))
        return Error(code, "length lies outside the musical render", path + "/length_s",
                     "use a positive length ending at or before seconds");
    const auto begin = std::llround(start * rate), end = std::llround((start + length) * rate);
    if (end <= begin)
        return Error(code, "span rounds to zero samples", path + "/length_s",
                     "use a length of at least one sample at the render rate");
    return std::pair<std::int64_t, std::int64_t>{begin, end};
}

inline Result<std::vector<sandbox::TimedEvent>> automationEvents(host::Instance& instance,
                                                                 const TestArgs& a, double rate) {
    std::vector<sandbox::TimedEvent> out;
    if (!a.automation) return out;
    if (!instance.info().semantic)
        return Error("dsp.automation", "automation needs physical parameter definitions",
                     "/automation", "choose a plugin with the semantic-params extension");
    auto schema = instance.semanticSchema();
    if (!schema) return std::move(schema).error().at("/automation");
    const auto* entries = schema->find("params");
    const auto live = instance.params();
    for (std::size_t i = 0; i < a.automation->size(); ++i) {
        const auto& point = (*a.automation)[i];
        const std::string path = "/automation/" + std::to_string(i);
        const auto* entry = entries && entries->isObject() ? entries->find(point.param) : nullptr;
        if (!entry)
            return Error("dsp.automation", "unknown parameter " + point.param, path + "/param",
                         "choose a parameter key from the plugin's semantic schema");
        auto described = params::fromDescription(point.param, *entry);
        if (!described) return std::move(described).error().at(path + "/param");
        const auto& spec = (*described)->spec;
        const auto hostParam =
            std::find_if(live.begin(), live.end(), [&](const auto& p) { return p.id == spec.id; });
        const auto* active = entry->find("active");
        if ((active && active->isBool() && !active->asBool()) || (spec.flags & params::kReadonly) ||
            !(spec.flags & params::kAutomatable) || hostParam == live.end() ||
            (hostParam->flags & CLAP_PARAM_IS_READONLY) ||
            !(hostParam->flags & CLAP_PARAM_IS_AUTOMATABLE))
            return Error("dsp.automation", "parameter is not available for automation",
                         path + "/param", "choose an active, writable, automatable parameter");
        auto physical = params::fromJson(spec, point.value);
        if (!physical) return std::move(physical).error().at(path + "/value");
        if (!(point.atS >= 0.0 && point.atS < a.seconds) ||
            std::llround(point.atS * rate) >= std::llround(a.seconds * rate))
            return Error("dsp.automation", "event lies outside the musical render", path + "/at_s",
                         "place the event at a sample before seconds");
        sandbox::TimedEvent e;
        e.sample = std::llround(point.atS * rate);
        e.event.kind = host::Event::Kind::Param;
        e.event.paramId = spec.id;
        e.event.value = params::toClap(spec, *physical);
        if (instance.paramsNormalized()) {
            auto normalized = instance.normalizedParam(spec.id, e.event.value);
            if (!normalized) return std::move(normalized).error().at(path + "/value");
            e.event.value = *normalized;
        }
        out.push_back(e);
    }
    return out;
}

inline Result<std::vector<sandbox::TimedEvent>> customNoteEvents(const TestArgs& a, double rate) {
    std::vector<sandbox::TimedEvent> out;
    if (!a.notes) return out;
    for (std::size_t i = 0; i < a.notes->size(); ++i) {
        const auto& note = (*a.notes)[i];
        auto span = timeSpan(note.startS, note.lengthS, a.seconds, rate,
                             "/notes/" + std::to_string(i), "dsp.notes");
        if (!span) return std::move(span).error();
        sandbox::TimedEvent e;
        e.sample = span->first;
        e.event.kind = host::Event::Kind::NoteOn;
        e.event.key = static_cast<std::int16_t>(note.key);
        e.event.noteId = static_cast<std::int32_t>(i);
        e.event.velocity = note.velocity;
        out.push_back(e);
        e.sample = span->second;
        e.event.kind = host::Event::Kind::NoteOff;
        out.push_back(e);
    }
    return out;
}

inline double maxStep(const audio::AudioData& audio) {
    double step = 0.0;
    for (const auto& ch : audio.channels)
        for (std::size_t i = 1; i < ch.size(); ++i)
            step = std::max(step,
                            std::abs(static_cast<double>(ch[i]) - static_cast<double>(ch[i - 1])));
    return step;
}

inline double windowRmsDb(const audio::AudioData& audio, std::size_t begin, std::size_t end) {
    double squares = 0.0;
    for (const auto& ch : audio.channels)
        for (std::size_t i = begin; i < std::min(end, ch.size()); ++i)
            squares += static_cast<double>(ch[i]) * static_cast<double>(ch[i]);
    const auto count = (end - begin) * audio.channels.size();
    return toDb(count ? std::sqrt(squares / static_cast<double>(count)) : 0.0);
}

// The fundamental of x over 4096 samples from `begin` (the difference
// function's first dip, as YIN finds it), between 40 Hz and 2 kHz, and how
// periodic the sound is there (1: exactly); nothing when it is not a tone.
inline std::optional<std::pair<double, double>> pitchOf(const std::vector<float>& x,
                                                        std::size_t begin, double rate) {
    constexpr std::size_t kWindow = 4096;
    const auto minLag = static_cast<std::size_t>(rate / 2000.0);
    const auto maxLag = static_cast<std::size_t>(rate / 40.0);
    if (begin + kWindow + maxLag + 2 > x.size()) return std::nullopt;
    std::vector<double> d(maxLag + 2, 0.0);
    double running = 0.0;
    d[0] = 1.0;
    for (std::size_t tau = 1; tau <= maxLag + 1; ++tau) {
        double sum = 0.0;
        for (std::size_t i = begin; i < begin + kWindow; ++i) {
            const double diff = static_cast<double>(x[i]) - static_cast<double>(x[i + tau]);
            sum += diff * diff;
        }
        running += sum;
        d[tau] = running > 0.0 ? sum * static_cast<double>(tau) / running : 1.0;
    }
    for (std::size_t tau = std::max<std::size_t>(minLag, 2); tau <= maxLag; ++tau) {
        if (d[tau] > 0.15) continue;
        while (tau + 1 <= maxLag && d[tau + 1] < d[tau]) ++tau;  // to the dip's bottom
        const double a = d[tau - 1], b = d[tau], c = d[tau + 1];
        const double denom = a - 2.0 * b + c;
        const double shift = denom > 0.0 ? 0.5 * (a - c) / denom : 0.0;
        return std::make_pair(rate / (static_cast<double>(tau) + shift), 1.0 - b);
    }
    return std::nullopt;
}

// The amplitude of a tone at hz in x[begin, end): a Hann-windowed DFT bin.
inline double toneAmplitude(const std::vector<float>& x, std::size_t begin, std::size_t end,
                            double hz, double rate) {
    if (end <= begin + 1) return 0.0;
    const double n = static_cast<double>(end - begin);
    std::complex<double> acc = 0.0;
    double weights = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        const double t = static_cast<double>(i - begin);
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * t / (n - 1.0));
        acc += w * static_cast<double>(x[i]) *
               std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
        weights += w;
    }
    return 2.0 * std::abs(acc) / weights;
}

// Levels and counts over every channel; gain, THD and response by signal.
inline SignalResult measure(Signal s, const audio::AudioData& out, std::size_t inputFrames,
                            const TestArgs& a, std::optional<std::int64_t> releasedAt = {}) {
    SignalResult r;
    r.signal = signalName(s);
    double peak = 0.0, squares = 0.0;
    std::size_t count = 0;
    for (const auto& ch : out.channels) {
        double sum = 0.0;
        for (const float v : ch) {
            const double d = static_cast<double>(v);
            peak = std::max(peak, std::abs(d));
            squares += d * d;
            sum += d;
            if (std::abs(d) > 1.0) ++r.clipped;
            if (std::fpclassify(v) == FP_SUBNORMAL) ++r.denormals;
        }
        count += ch.size();
        if (!ch.empty()) r.dc = std::max(r.dc, std::abs(sum / static_cast<double>(ch.size())));
    }
    r.peakDbfs = rounded(toDb(peak), 2);
    r.rmsDbfs = rounded(toDb(count ? std::sqrt(squares / static_cast<double>(count)) : 0.0), 2);
    r.dc = rounded(r.dc, 6);
    if (out.channels.empty()) return r;
    const std::vector<float>& x = out.channels[0];
    const double rate = out.sampleRate;
    const double amp = std::pow(10.0, a.levelDb / 20.0);
    if (s == Signal::Note) {
        // Past the attack, before any envelope has had long to fall.
        if (auto pitch = pitchOf(x, static_cast<std::size_t>(0.03 * rate), rate))
            r.pitchHz = rounded(pitch->first, 2);
    }
    if (forInstruments(s)) {
        // From the last release to the last sample above -80 dBFS.
        const auto released = static_cast<std::size_t>(
            releasedAt.value_or(std::llround(kReleaseAt * a.seconds * rate)));
        std::size_t last = 0;
        for (const auto& ch : out.channels)
            for (std::size_t i = ch.size(); i-- > 0;)
                if (std::abs(ch[i]) > 1e-4f) {
                    last = std::max(last, i + 1);
                    break;
                }
        r.releaseMs = rounded(
            last > released ? 1000.0 * static_cast<double>(last - released) / rate : 0.0, 1);
    }
    if (s == Signal::Sine) {
        // The steady part: past the first 100 ms, within the input.
        const std::size_t end = std::min(inputFrames, x.size());
        const std::size_t begin = std::min(static_cast<std::size_t>(0.1 * rate), end / 4);
        const double fundamental = toneAmplitude(x, begin, end, a.frequencyHz, rate);
        r.gainDb = toDb(fundamental) - toDb(amp);
        if (fundamental > 1e-7 && !a.automation &&
            (!a.sidechain || a.sidechain->signal == Signal::Silence)) {
            double harmonics = 0.0;
            for (int k = 2; k <= 10 && k * a.frequencyHz < 0.45 * rate; ++k) {
                const double h = toneAmplitude(x, begin, end, k * a.frequencyHz, rate);
                harmonics += h * h;
            }
            r.thdPercent = rounded(100.0 * std::sqrt(harmonics) / fundamental, 4);
        }
        r.gainDb = rounded(*r.gainDb, 2);
    } else if (s == Signal::Impulse && !a.automation &&
               (!a.sidechain || a.sidechain->signal == Signal::Silence)) {
        std::size_t n = 1;
        while (n < x.size() && n < (std::size_t(1) << 19)) n <<= 1;
        std::vector<double> in(n, 0.0);
        for (std::size_t i = 0; i < std::min(n, x.size()); ++i) in[i] = static_cast<double>(x[i]);
        std::vector<std::complex<double>> spectrum(n / 2 + 1);
        dsp::RealFft fft;
        fft.prepare(n);
        if (fft.forward(in, spectrum)) {
            r.response.emplace();
            for (double hz = 31.25; hz < 0.45 * rate && hz <= 16000.0; hz *= 2.0) {
                const auto bin =
                    static_cast<std::size_t>(std::llround(hz * static_cast<double>(n) / rate));
                if (bin < spectrum.size())
                    r.response->push_back({hz, rounded(toDb(std::abs(spectrum[bin]) / amp), 2)});
            }
        }
    }
    return r;
}

// A trace's numbers to six significant digits: what a reader needs, and short.
inline json::Value sixDigits(const json::Value& values) {
    json::Value out = json::Value::array();
    if (!values.isArray()) return out;
    for (const auto& v : values.asArray()) {
        if (!v.isNumber()) {
            out.push_back(json::Value());
            continue;
        }
        char text[32];
        std::snprintf(text, sizeof text, "%.6g", v.asDouble());
        out.push_back(std::strtod(text, nullptr));
    }
    return out;
}

// What the plugin traced in a render, as the result gives it; problems for
// what it could not keep.
inline std::vector<TraceResult> traceResults(const json::Value& doc,
                                             std::vector<std::string>& problems,
                                             const std::string& signal) {
    std::vector<TraceResult> out;
    if (const auto* traces = doc.find("traces"); traces && traces->isArray())
        for (const auto& t : traces->asArray()) {
            TraceResult r;
            if (const auto* n = t.find("name"); n && n->isString()) r.name = n->asString();
            if (const auto* v = t.find("last")) r.last = sixDigits(*v);
            if (const auto* v = t.find("low")) r.low = sixDigits(*v);
            if (const auto* v = t.find("high")) r.high = sixDigits(*v);
            if (const auto* v = t.find("non_finite"); v && v->isNumber())
                r.nonFinite = static_cast<std::int64_t>(v->asDouble());
            if (r.nonFinite > 0)
                problems.push_back("trace \"" + r.name + "\" was given NaN or infinity " +
                                   std::to_string(r.nonFinite) + " times with the " + signal +
                                   " (look for a division by zero or a log of 0 there)");
            out.push_back(std::move(r));
        }
    if (const auto* d = doc.find("dropped"); d && d->isNumber() && d->asDouble() > 0)
        problems.push_back("trace() was given more than 16 names with the " + signal +
                           "; the values under the rest were dropped");
    return out;
}

inline std::string fixed(double v, int digits = 1) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", digits, v);
    return text;
}

}  // namespace test

template <class Host>
void registerDspTest(cmd::Registry& registry, const std::string& exampleEffect) {
    using namespace test;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    Command<TestArgs, TestResult> c;
    c.name = "dsp.test";
    c.kind = Kind::Action;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Play test signals or notes through a plugin and measure it: levels, THD, pitch.";
    c.description =
        "Give plugin (an id: a script's, or any effect's or instrument's) or script (a name in "
        "the library). A fresh instance, set by patch (a merge patch) when given, plays each "
        "signal. An effect: sine (frequency_hz; gain_db and thd_percent), sweep (20 Hz to 20 "
        "kHz), impulse (response_db at octave centres), pink noise and silence, each peaking at "
        "level_db; and file, audio_file played as it is (resampled to the render's rate, mono "
        "on both channels, at most 60 s), the only signal when signals is not given. An "
        "instrument, at velocity 0.8: notes (a note, then a chord), note (A4; "
        "pitch_hz, the pitch heard), and silence (no notes); release_ms is how long the sound "
        "lasts after the last release. Each reports peak and RMS in dBFS, DC, samples beyond 0 "
        "dBFS and denormals; problems says in words what looks wrong (clipping, DC, sound from "
        "silence, no output, a note out of tune, sound that never dies away, non-finite "
        "samples the framework silenced, a script stopped by its loop budget or indexing "
        "outside an array). cpu_percent is measured, so it varies between runs. save: true "
        "keeps each output as a WAV (path) for analyze and compare. "
        "automation is a list of {param, at_s, value}: semantic patch keys and physical "
        "values, as discrete sample-accurate changes applied to every signal (no implicit "
        "ramps). automation.max_step is the largest adjacent-sample change in full-scale "
        "amplitude; baseline_max_step uses the same render without automation; excess_step "
        "is their positive difference. A step above both four times baseline and 0.05 "
        "suggests clicks or zipper noise: a screening heuristic, not proof of an artifact "
        "or of inaudibility. sidechain supplies a timed key; its middle half is measured "
        "against a connected silent key with identical automation: gain_change_db is "
        "negative for ducking, null if either output is below -120 dBFS. Slow attack may "
        "not settle in that window. Each pass has a fresh instance and initial patch. "
        "THD and impulse response are omitted during automation or a non-silent key; "
        "gain_db then describes the changing render, not static gain. notes supplies an "
        "instrument's sequence ({key, velocity, start_s, length_s}), replacing the notes "
        "signal; defaults become notes and silence. Do not combine it with the A4 note "
        "signal or select only silence. release_ms starts at its last note-off. "
        "Times round to samples at the render rate; spans must fit within seconds. "
        "sample_rate renders at that rate instead of the session's (sandbox) or the "
        "project's (DAW); latency is in samples at the rate used, which sample_rate in the "
        "result reports. meters gives a semantic plugin's read-only parameters (its meters "
        "and readouts) by patch key in physical units, as they stood when the signal ended "
        "(the tail after it does not move them). traces gives what a script's trace(name, "
        "value) calls recorded over the signal, in trace_points intervals of "
        "trace_interval_s: for each name, the last value in each interval and the lowest and "
        "highest, null where none was given.";
    c.tags = {"dsp", "analysis"};
    auto automationEvent = schema::object<AutomationEvent>("dsp_automation_event")
                               .field("param", &AutomationEvent::param,
                                      schema::string().minLength(1).doc("semantic patch key"))
                               .field("at_s", &AutomationEvent::atS, schema::number().min(0))
                               .field("value", &AutomationEvent::value,
                                      schema::any().doc("physical value, enum key or boolean"))
                               .build();
    auto sidechainArgs = schema::object<SidechainArgs>("dsp_sidechain_args")
                             .field("signal", &SidechainArgs::signal,
                                    schema::enumeration<Signal>({{Signal::Sine, "sine"},
                                                                 {Signal::Sweep, "sweep"},
                                                                 {Signal::Impulse, "impulse"},
                                                                 {Signal::Noise, "noise"},
                                                                 {Signal::Silence, "silence"}}))
                             .defaultValue(Signal::Sine)
                             .field("level_db", &SidechainArgs::levelDb,
                                    schema::number().range(-60, 0).unit(units::Unit::dB))
                             .defaultValue(-6.0)
                             .field("frequency_hz", &SidechainArgs::frequencyHz,
                                    schema::number().range(10, 20000).unit(units::Unit::Hz))
                             .defaultValue(100.0)
                             .field("start_s", &SidechainArgs::startS,
                                    schema::number().min(0).doc("default: 0.25 * seconds"))
                             .field("length_s", &SidechainArgs::lengthS,
                                    schema::number().exclusiveMin(0).doc("default: 0.5 * seconds"))
                             .build();
    auto note = schema::object<Note>("dsp_test_note")
                    .field("key", &Note::key, schema::integerOf<int>().range(0, 127))
                    .field("velocity", &Note::velocity, schema::number().exclusiveMin(0).max(1))
                    .defaultValue(0.8)
                    .field("start_s", &Note::startS, schema::number().min(0))
                    .field("length_s", &Note::lengthS, schema::number().exclusiveMin(0))
                    .build();
    c.args = schema::object<TestArgs>("dsp_test_args")
                 .field("plugin", &TestArgs::plugin, schema::string().minLength(1))
                 .field("script", &TestArgs::script, schema::string().minLength(1))
                 .field("patch", &TestArgs::patch, schema::any())
                 .field("signals", &TestArgs::signals,
                        schema::arrayOf(schema::enumeration<Signal>({{Signal::Sine, "sine"},
                                                                     {Signal::Sweep, "sweep"},
                                                                     {Signal::Impulse, "impulse"},
                                                                     {Signal::Noise, "noise"},
                                                                     {Signal::Silence, "silence"},
                                                                     {Signal::Notes, "notes"},
                                                                     {Signal::Note, "note"},
                                                                     {Signal::File, "file"}}))
                            .doc("default: effects five, or file with audio_file; instruments "
                                 "three, or notes and silence with custom notes"))
                 .field("audio_file", &TestArgs::audioFile,
                        schema::string().minLength(1).doc(
                            "the absolute path of a WAV, FLAC or MP3 (10 minutes at most) the file "
                            "signal plays through an effect"))
                 .field("automation", &TestArgs::automation,
                        schema::arrayOf(automationEvent)
                            .minItems(1)
                            .maxItems(1024)
                            .doc("discrete physical-value changes on every signal"))
                 .field("sidechain", &TestArgs::sidechain,
                        sidechainArgs.doc("timed generated key input"))
                 .field("notes", &TestArgs::notes,
                        schema::arrayOf(note).minItems(1).maxItems(1024).doc(
                            "instrument sequence replacing the notes stimulus"))
                 .field("level_db", &TestArgs::levelDb,
                        schema::number().range(-60, 0).unit(units::Unit::dB))
                 .defaultValue(-12.0)
                 .field("frequency_hz", &TestArgs::frequencyHz,
                        schema::number().range(10, 20000).unit(units::Unit::Hz))
                 .defaultValue(1000.0)
                 .field("seconds", &TestArgs::seconds, schema::number().range(0.1, 10))
                 .defaultValue(1.0)
                 .field("sample_rate", &TestArgs::sampleRate,
                        schema::number()
                            .range(8000, 384000)
                            .unit(units::Unit::Hz)
                            .doc("default: the session's or project's rate"))
                 .field("save", &TestArgs::save, schema::boolean())
                 .defaultValue(false)
                 .field("trace_points", &TestArgs::tracePoints,
                        schema::integerOf<int>().range(1, 4096).doc(
                            "intervals the signal's traces are kept in"))
                 .defaultValue(100)
                 .build();
    auto point = schema::object<ResponsePoint>("dsp_response_point")
                     .field("hz", &ResponsePoint::hz, schema::number())
                     .field("db", &ResponsePoint::db, schema::number())
                     .build();
    auto automationResult =
        schema::object<AutomationResult>("dsp_automation_result")
            .field("max_step", &AutomationResult::maxStep, schema::number())
            .field("baseline_max_step", &AutomationResult::baselineMaxStep, schema::number())
            .field("excess_step", &AutomationResult::excessStep, schema::number())
            .build();
    schema::Schema<json::Value> nullableNumber(
        "number or null",
        [](const json::Value& v, json::Value& out, schema::DecodeCtx& ctx,
           const std::string& path) {
            if (!v.isNull()) {
                double value = 0.0;
                if (!schema::number().decodeInto(v, value, ctx, path)) return false;
            }
            out = v;
            return true;
        },
        [](const json::Value& v) { return v; }, detail::j(R"({"type":["number","null"]})"));
    auto sidechainResult =
        schema::object<SidechainResult>("dsp_sidechain_result")
            .field("rms_dbfs", &SidechainResult::rmsDbfs, schema::number())
            .field("reference_rms_dbfs", &SidechainResult::referenceRmsDbfs, schema::number())
            .field("gain_change_db", &SidechainResult::gainChangeDb, nullableNumber)
            .build();
    auto traceResult =
        schema::object<TraceResult>("dsp_trace_result")
            .field("name", &TraceResult::name, schema::string())
            .field("last", &TraceResult::last,
                   schema::any().doc("each interval's last value, or null"))
            .field("low", &TraceResult::low, schema::any().doc("each interval's lowest"))
            .field("high", &TraceResult::high, schema::any().doc("each interval's highest"))
            .field("non_finite", &TraceResult::nonFinite,
                   schema::integer().min(0).doc("NaN or infinity given, not kept"))
            .build();
    auto signal = schema::object<SignalResult>("dsp_signal_result")
                      .field("signal", &SignalResult::signal, schema::string())
                      .field("peak_dbfs", &SignalResult::peakDbfs, schema::number())
                      .field("rms_dbfs", &SignalResult::rmsDbfs, schema::number())
                      .field("dc", &SignalResult::dc, schema::number())
                      .field("clipped", &SignalResult::clipped, schema::integer().min(0))
                      .field("denormals", &SignalResult::denormals, schema::integer().min(0))
                      .field("gain_db", &SignalResult::gainDb,
                             schema::number().doc("sine: the fundamental out against in"))
                      .field("thd_percent", &SignalResult::thdPercent, schema::number())
                      .field("response_db", &SignalResult::response, schema::arrayOf(point))
                      .field("pitch_hz", &SignalResult::pitchHz,
                             schema::number().doc("note: the pitch heard (A4 is 440 Hz)"))
                      .field("release_ms", &SignalResult::releaseMs,
                             schema::number().doc("an instrument's: sound after the last release"))
                      .field("path", &SignalResult::path, schema::string())
                      .field("automation", &SignalResult::automation, automationResult)
                      .field("sidechain", &SignalResult::sidechain, sidechainResult)
                      .field("meters", &SignalResult::meters,
                             schema::any().doc("read-only parameters (meters) by patch key, "
                                               "physical, as of the signal's end"))
                      .field("trace_interval_s", &SignalResult::traceIntervalS,
                             schema::number().doc("each trace interval's length"))
                      .field("traces", &SignalResult::traces,
                             schema::arrayOf(traceResult).doc("what trace() recorded, by name"))
                      .build();
    c.result = schema::object<TestResult>("dsp_test_result")
                   .field("plugin", &TestResult::plugin, schema::string())
                   .field("sample_rate", &TestResult::sampleRate,
                          schema::number().unit(units::Unit::Hz).doc("the rate rendered at"))
                   .field("latency", &TestResult::latency, schema::integer().doc("samples"))
                   .field("signals", &TestResult::signals, schema::arrayOf(signal))
                   .field("cpu_percent", &TestResult::cpuPercent,
                          schema::number().doc("of real time, measured"))
                   .field("problems", &TestResult::problems, schema::arrayOf(schema::string()))
                   .field("log", &TestResult::log, schema::arrayOf(schema::string()))
                   .build();
    c.errors = {{"dsp.args", "neither or both of plugin and script", "give exactly one"},
                {"dsp.id_taken", "another script in the library has the script's id",
                 "give each script its own Info.id"},
                {"dsp.not_listed", "the script compiles but no plugin has its id",
                 "put the script player on the plugin path"},
                {"dsp.not_effect", "the plugin takes no audio and no notes",
                 "test an effect or an instrument"},
                {"dsp.signal", "a signal the plugin cannot play",
                 "an effect plays audio signals, an instrument notes"},
                {"dsp.automation", "unavailable parameter or automation time outside the render",
                 "choose a writable semantic parameter and a time before seconds"},
                {"dsp.sidechain", "no key input or invalid key timing",
                 "choose a plugin with a key and a nonempty span within seconds"},
                {"dsp.notes", "invalid custom sequence or signal selection",
                 "use an instrument, notes or notes and silence, and spans within seconds"},
                {"dsp.frequency", "a sine at or above half the sample rate",
                 "lower frequency_hz or raise sample_rate"},
                {"dsp.file",
                 "the file signal without audio_file, or a file that is not absolute, is over 10 "
                 "minutes or holds no audio",
                 "give audio_file a WAV, FLAC or MP3, or leave file out of signals"}};
    if (!exampleEffect.empty()) {
        json::Value args = json::Value::object();
        args["plugin"] = exampleEffect;
        args["signals"] = json::Value::array();
        args["signals"].push_back("sine");
        c.examples = {{"Measure an effect with a sine", args,
                       detail::j(R"({"plugin":"<any>","sample_rate":"<any>","latency":"<any>",
                          "signals":"<any>","cpu_percent":"<any>","problems":"<any>","log":"<any>"})"),
                       "empty"}};
        auto expected = detail::j(
            R"({"plugin":"<any>","sample_rate":"<any>","latency":"<any>","signals":"<any>",
                                     "cpu_percent":"<any>","problems":"<any>","log":"<any>"})");
        c.examples.push_back({"Automate an effect in dB",
                              detail::j(R"({"plugin":"com.suppleaudio.test.gain","signals":["sine"],
                              "automation":[{"param":"gain","at_s":0.253,"value":-12},
                                            {"param":"gain","at_s":0.753,"value":0}]})"),
                              expected, "empty"});
        c.examples.push_back(
            {"Measure a timed sidechain",
             detail::j(R"({"plugin":"com.suppleaudio.test.keymix","signals":["sine"],
                                            "sidechain":{"level_db":-6}})"),
             expected, "empty"});
        c.examples.push_back({"Measure an effect at 96 kHz",
                              detail::j(R"({"plugin":"com.suppleaudio.test.gain","signals":["sine"],
                              "sample_rate":96000})"),
                              expected, "empty"});
        c.examples.push_back({"Play a custom note sequence",
                              detail::j(R"({"plugin":"com.suppleaudio.test.sine",
                              "notes":[{"key":60,"start_s":0.013,"length_s":0.2},
                                       {"key":67,"velocity":0.6,"start_s":0.303,"length_s":0.4}]})"),
                              expected, "empty"});
    }
    c.run = [](const TestArgs& a, Context& ctx) -> Result<TestResult> {
        Host& s = ctx.service<Host>();
        using Traits = TestTraits<Host>;
        if (a.plugin.has_value() == a.script.has_value())
            return Error("dsp.args", "give exactly one of plugin and script", "",
                         "plugin is an id from plugin.list; script is a name from dsp.list");
        std::string id;
        if (a.plugin) {
            id = *a.plugin;
        } else {
            auto text = detail::readScript(s, *a.script, "/script");
            if (!text) return std::move(text).error();
            auto compiled = detail::compile(*text);
            if (!compiled.plugin)
                return Error("dsp.compile", "the script does not compile", "/script",
                             "dsp.check {name: \"" + *a.script + "\"} gives its diagnostics");
            id = compiled.plugin->id;
            SUPPLE_TRY(detail::uniqueId(s, id, *a.script, "/script"));
        }
        auto info = s.findPlugin(id);
        if (!info && a.script) {
            (void)s.plugins(true);  // a script written since the last scan
            info = s.findPlugin(id);
            if (!info)
                return Error("dsp.not_listed", "no plugin has the id " + id, "/script",
                             "the script player must be on the sandbox's plugin path "
                             "(--plugin-path)");
        }
        if (!info) return std::move(info).error().at("/plugin");
        auto created = host::Instance::create(*info, Traits::identity(s));
        if (!created) return std::move(created).error();
        host::Instance& instance = **created;
        const bool instrument = instance.audioInputChannels() == 0;
        if (instrument && !info->instrument)
            return Error("dsp.not_effect", info->name + " takes no audio and no notes",
                         a.plugin ? "/plugin" : "/script",
                         "dsp.test measures effects and instruments");
        if (a.patch) {
            if (!instance.info().semantic)
                return Error("dsp.not_semantic", info->name + " takes no patch", "/patch",
                             "set it by plugin.load and param.set instead");
            auto applied = instance.setPatch(json::writeCompact(*a.patch), CLAP_SUPPLE_SET_MERGE);
            if (!applied) return patchError(std::move(applied).error());
        }

        // The meters a semantic plugin reports, by CLAP id: key and units.
        std::vector<std::unique_ptr<params::DescribedParam>> meters;
        if (instance.info().semantic && !instance.paramsNormalized()) {
            auto schema = instance.semanticSchema();
            if (!schema) return std::move(schema).error();
            if (const auto* entries = schema->find("params"); entries && entries->isObject())
                for (const auto& m : entries->asObject()) {
                    auto described = params::fromDescription(m.key, m.value);
                    if (described && ((*described)->spec.flags & params::kReadonly))
                        meters.push_back(std::move(*described));
                }
        }
        auto meterValues = [&](const sandbox::RenderResult& r) -> std::optional<json::Value> {
            if (meters.empty()) return std::nullopt;
            json::Value values = json::Value::object();
            for (const auto& reported : r.meters)
                for (const auto& m : meters)
                    if (reported.stage == 0 && m->spec.id == reported.paramId) {
                        const double physical = params::fromClap(m->spec, reported.value);
                        values[m->key] = m->spec.type == params::Type::Float
                                             ? json::Value(rounded(physical, 4))
                                             : params::toJson(m->spec, physical);
                    }
            return values;
        };

        TestResult out;
        out.plugin = info->id;
        const std::vector<Signal> signals =
            a.signals && !a.signals->empty() ? *a.signals
            : a.notes                        ? std::vector<Signal>{Signal::Notes, Signal::Silence}
            : a.audioFile && !instrument     ? std::vector<Signal>{Signal::File}
            : instrument
                ? std::vector<Signal>(std::begin(kInstrumentSignals), std::end(kInstrumentSignals))
                : std::vector<Signal>(std::begin(kEffectSignals), std::end(kEffectSignals));
        if (a.notes && (!instrument ||
                        std::find(signals.begin(), signals.end(), Signal::Note) != signals.end() ||
                        std::find(signals.begin(), signals.end(), Signal::Notes) == signals.end()))
            return Error("dsp.notes", "custom notes require an instrument's notes signal", "/notes",
                         "select notes, or notes and silence; omit the A4 note signal");
        for (size_t k = 0; k < signals.size(); ++k)
            if (signals[k] != Signal::Silence && forInstruments(signals[k]) != instrument)
                return Error("dsp.signal",
                             info->name +
                                 (instrument ? " is an instrument: it plays notes, not "
                                             : " is an effect: it takes audio, not ") +
                                 signalName(signals[k]),
                             "/signals/" + std::to_string(k),
                             instrument ? "use notes, note or silence"
                                        : "use sine, sweep, impulse, noise, silence or file");
        const bool playsFile =
            std::find(signals.begin(), signals.end(), Signal::File) != signals.end();
        if (playsFile && !a.audioFile)
            return Error("dsp.file", "the file signal plays audio_file, which is not given",
                         "/audio_file", "give audio_file a WAV, FLAC or MP3 to play");
        sandbox::RenderSettings settings = Traits::settings(s);
        if (a.sampleRate) settings.sampleRate = *a.sampleRate;
        std::optional<audio::AudioData> file;
        if (playsFile) {
            auto loaded = fileSignal(*a.audioFile, settings.sampleRate, out.log);
            if (!loaded) return std::move(loaded).error();
            file = std::move(*loaded);
        }
        const double nyquist = 0.5 * settings.sampleRate;
        if (a.frequencyHz >= nyquist &&
            std::find(signals.begin(), signals.end(), Signal::Sine) != signals.end())
            return Error(
                "dsp.frequency",
                "a " + fixed(a.frequencyHz) + " Hz sine cannot be sampled at " +
                    fixed(settings.sampleRate, 0) + " Hz",
                "/frequency_hz",
                "use a frequency below " + fixed(nyquist, 0) + " Hz, or a higher sample_rate");
        if (a.sidechain && a.sidechain->signal == Signal::Sine &&
            a.sidechain->frequencyHz >= nyquist)
            return Error(
                "dsp.frequency",
                "a " + fixed(a.sidechain->frequencyHz) + " Hz key cannot be sampled at " +
                    fixed(settings.sampleRate, 0) + " Hz",
                "/sidechain/frequency_hz",
                "use a frequency below " + fixed(nyquist, 0) + " Hz, or a higher sample_rate");
        auto automated = automationEvents(instance, a, settings.sampleRate);
        if (!automated) return std::move(automated).error();
        auto sequence = customNoteEvents(a, settings.sampleRate);
        if (!sequence) return std::move(sequence).error();
        std::optional<std::int64_t> releasedAt;
        for (const auto& e : *sequence)
            if (e.event.kind == host::Event::Kind::NoteOff)
                releasedAt = std::max(releasedAt.value_or(0), e.sample);
        audio::AudioData key, silentKey;
        std::pair<std::int64_t, std::int64_t> keySpan{};
        if (a.sidechain) {
            if (!instance.sidechainChannels())
                return Error("dsp.sidechain", "the plugin has no sidechain input", "/sidechain",
                             "choose a plugin with a key input or omit sidechain");
            const auto& sc = *a.sidechain;
            auto span =
                timeSpan(sc.startS.value_or(a.seconds * 0.25), sc.lengthS.value_or(a.seconds * 0.5),
                         a.seconds, settings.sampleRate, "/sidechain", "dsp.sidechain");
            if (!span) return std::move(span).error();
            keySpan = *span;
            key = stimulus(Signal::Silence, settings.sampleRate, a.seconds, 0.0, 100.0);
            silentKey = key;
            const auto active =
                stimulus(sc.signal, settings.sampleRate,
                         static_cast<double>(span->second - span->first) / settings.sampleRate,
                         sc.levelDb, sc.frequencyHz);
            for (std::size_t ch = 0; ch < key.channels.size(); ++ch)
                std::copy(active.channels[ch].begin(), active.channels[ch].end(),
                          key.channels[ch].begin() + span->first);
        }
        std::set<std::string> logged;
        double rendered = 0.0, spent = 0.0;
        // The traces' intervals: each signal's frames in trace_points of them.
        const auto traceBins = static_cast<std::uint32_t>(std::clamp(a.tracePoints, 1, 4096));
        std::uint32_t traceInterval = 1;
        // A pass on a fresh instance; with `trace`, what its trace() calls
        // recorded over `traceFrames` frames, read once it is deactivated.
        auto renderPass = [&](const sandbox::RenderRequest& request,
                              std::optional<json::Value>* trace =
                                  nullptr) -> Result<sandbox::RenderResult> {
            auto fresh = host::Instance::create(*info, Traits::identity(s));
            if (!fresh) return std::move(fresh).error();
            if (a.patch) {
                auto applied =
                    (*fresh)->setPatch(json::writeCompact(*a.patch), CLAP_SUPPLE_SET_MERGE);
                if (!applied) return patchError(std::move(applied).error());
            }
            const bool tracing = trace && (*fresh)->traces();
            if (tracing) SUPPLE_TRY((*fresh)->startTrace(traceInterval, traceBins));
            host::Instance* chain[] = {fresh->get()};
            const auto start = std::chrono::steady_clock::now();
            auto result = sandbox::render(chain, settings, request);
            spent +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (result) rendered += result->audio.seconds();
            if (result && tracing) {
                (*fresh)->deactivate();
                auto read = (*fresh)->readTrace();
                if (!read) return std::move(read).error();
                *trace = std::move(*read);
            }
            return result;
        };
        auto diagnose = [&](Signal sig, const sandbox::RenderResult& r, const SignalResult& m,
                            const std::string& label) {
            for (const auto& line : r.log) {
                const std::string labeled = label.empty() ? line : m.signal + label + ": " + line;
                if (logged.insert(labeled).second) out.log.push_back(labeled);
            }
            const std::string with = " with the " + m.signal + label;
            if (m.clipped > 0)
                out.problems.push_back(std::to_string(m.clipped) + " samples beyond 0 dBFS" + with +
                                       " (peak " + fixed(m.peakDbfs) + " dBFS)");
            if (m.dc > 0.01) out.problems.push_back("a DC offset of " + fixed(m.dc, 3) + with);
            if (sig == Signal::Silence && m.peakDbfs > -100.0)
                out.problems.push_back(
                    (instrument ? "sound with no notes: peak " : "sound from silence: peak ") +
                    fixed(m.peakDbfs) +
                    (instrument ? " dBFS (a voice that plays without a note, or a source that "
                                  "runs free)"
                                : " dBFS (feedback that sustains itself, or a source that runs "
                                  "free)") +
                    label);
            if (sig != Signal::Silence && m.peakDbfs < -120.0)
                out.problems.push_back(
                    instrument ? "no sound" + with +
                                     " (does startVoice start the voice, and process() add "
                                     "each voice that voicePlaying(v) into io?)"
                               : "no output" + with);
            if (instrument && sig != Signal::Silence && r.tailCapped)
                out.problems.push_back(
                    "still sounding " + fixed(settings.tailLimitSeconds) +
                    " s after every note was released" + with +
                    " (does releaseVoice start each voice's release, and process() call "
                    "voiceFinished(v) once it has died away?)");
            if (sig == Signal::Note && m.pitchHz) {
                const double cents = 1200.0 * std::log2(*m.pitchHz / 440.0);
                if (std::abs(cents) > 50.0)
                    out.problems.push_back("A4 (key 69) sounded at " + fixed(*m.pitchHz) + " Hz, " +
                                           fixed(cents, 0) +
                                           " cents from 440 Hz (voiceHz(v, i) is the note's "
                                           "frequency)" +
                                           label);
            }
        };
        for (const Signal sig : signals) {
            const audio::AudioData generated =
                instrument || sig == Signal::File
                    ? audio::AudioData{}
                    : stimulus(sig, settings.sampleRate, a.seconds, a.levelDb, a.frequencyHz);
            const audio::AudioData& input = sig == Signal::File ? *file : generated;
            const std::size_t inputFrames =
                instrument ? static_cast<std::size_t>(std::llround(a.seconds * settings.sampleRate))
                           : input.frames();
            traceInterval = static_cast<std::uint32_t>(std::max<std::size_t>(
                1, (std::max<std::size_t>(inputFrames, 1) + traceBins - 1) / traceBins));
            sandbox::RenderRequest request;
            if (instrument) {
                request.lengthSamples = static_cast<std::int64_t>(inputFrames);
            } else {
                request.input = &input;
            }
            const auto notes = a.notes && sig == Signal::Notes
                                   ? *sequence
                                   : noteEvents(sig, settings.sampleRate, a.seconds);
            request.events = *automated;
            request.events.insert(request.events.end(), notes.begin(), notes.end());
            // Stable ties: parameters, releases, starts. The renderer keeps this order.
            auto rank = [](host::Event::Kind kind) {
                return kind == host::Event::Kind::Param     ? 0
                       : kind == host::Event::Kind::NoteOff ? 1
                                                            : 2;
            };
            std::stable_sort(
                request.events.begin(), request.events.end(), [&](const auto& x, const auto& y) {
                    return x.sample != y.sample ? x.sample < y.sample
                                                : rank(x.event.kind) < rank(y.event.kind);
                });
            if (a.sidechain) request.sidechain = &key;
            std::optional<json::Value> traced;
            auto r = renderPass(request, &traced);
            if (!r) return std::move(r).error();
            out.latency = r->latency;
            SignalResult m = measure(sig, r->audio, inputFrames, a, releasedAt);
            m.meters = meterValues(*r);
            if (traced) {
                auto kept = traceResults(*traced, out.problems, m.signal);
                if (!kept.empty()) {
                    m.traceIntervalS =
                        rounded(static_cast<double>(traceInterval) / settings.sampleRate, 6);
                    m.traces = std::move(kept);
                }
            }
            diagnose(sig, *r, m, "");
            if (a.automation) {
                auto baselineRequest = request;
                std::erase_if(baselineRequest.events, [](const auto& e) {
                    return e.event.kind == host::Event::Kind::Param;
                });
                auto baseline = renderPass(baselineRequest);
                if (!baseline) return std::move(baseline).error();
                diagnose(sig, *baseline, measure(sig, baseline->audio, inputFrames, a, releasedAt),
                         " (without automation)");
                AutomationResult metric;
                metric.maxStep = maxStep(r->audio);
                metric.baselineMaxStep = maxStep(baseline->audio);
                metric.excessStep = std::max(0.0, metric.maxStep - metric.baselineMaxStep);
                m.automation = metric;
                if (metric.maxStep > 4.0 * metric.baselineMaxStep && metric.maxStep > 0.05)
                    out.problems.push_back(
                        "possible clicks or zipper noise with the " + m.signal +
                        ": largest sample step increased from " + fixed(metric.baselineMaxStep, 6) +
                        " to " + fixed(metric.maxStep, 6) +
                        " (check parameter smoothing; intended changes can also increase it)");
            }
            if (a.sidechain) {
                auto referenceRequest = request;
                referenceRequest.sidechain = &silentKey;
                auto reference = renderPass(referenceRequest);
                if (!reference) return std::move(reference).error();
                diagnose(sig, *reference,
                         measure(sig, reference->audio, inputFrames, a, releasedAt),
                         " (silent key)");
                const auto length = keySpan.second - keySpan.first;
                const auto begin = static_cast<std::size_t>(keySpan.first + length / 4);
                const auto end = static_cast<std::size_t>(keySpan.first + (3 * length + 3) / 4);
                SidechainResult metric;
                const double activeDb = windowRmsDb(r->audio, begin, end);
                const double referenceDb = windowRmsDb(reference->audio, begin, end);
                metric.rmsDbfs = rounded(activeDb, 2);
                metric.referenceRmsDbfs = rounded(referenceDb, 2);
                if (activeDb >= -120.0 && referenceDb >= -120.0)
                    metric.gainChangeDb = rounded(activeDb - referenceDb, 2);
                m.sidechain = metric;
            }
            if (a.save) {
                const fs::path saved = Traits::outputDir(s) / ("dsp-test-" + artifactId(info->id) +
                                                               "-" + m.signal + ".wav");
                SUPPLE_TRY(audio::writeWav(saved, r->audio));
                m.path = saved.string();
            }
            out.signals.push_back(std::move(m));
        }
        out.sampleRate = settings.sampleRate;
        out.cpuPercent = rendered > 0.0 ? rounded(100.0 * spent / rendered, 2) : 0.0;
        for (const std::string& line : out.log) {
            if (line.find("non-finite") != std::string::npos)
                out.problems.push_back(
                    "it produced NaN or infinity, which the framework silenced: " + line +
                    " (look for a division by zero, log of 0 or growing feedback)");
            else if (line.find("loop budget") != std::string::npos)
                out.problems.push_back("a loop never ended: " + line);
            else if (line.find("outside an array") != std::string::npos)
                out.problems.push_back("an index went out of range: " + line);
        }
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::dsp_commands
