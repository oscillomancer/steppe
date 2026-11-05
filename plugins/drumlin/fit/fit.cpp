// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// drumlin-fit: fits a Drumlin recipe to recordings of a drum machine.
//
// It renders with Drumlin's own engine (the script, compiled here as C++, as
// the stock plugin compiles it), so what it fits is exactly what the plugin
// plays. A spec file names the recipe's free parameters with their ranges,
// the fixed ones, and the recordings to match, each with the clock its
// machine played it at; CMA-ES searches the free parameters on every core,
// scoring each candidate by how far its sound lies from the recordings in
// level over time and frequency (and, for tonal sounds, in waveform at the
// hit), and by the narrow tones it rings with that they do not. See
// plugins/drumlin/fit/README.md.
//
//   drumlin-fit fit <spec> [--evals N] [--seed S] [--sigma X] [--threads T]
//   drumlin-fit score <spec>           the spec's recipe as it stands, scored
//   drumlin-fit render <spec> <dir>    its renders beside the recordings, as WAVs
//   drumlin-fit emit <spec> <model>    the recipe as the script's code
//   drumlin-fit floor <spec>           each recording scored against the next
//
// The recordings never leave the machine they are on: nothing here copies
// them anywhere but into memory, and the recipe is all that is kept.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#include <pocketfft_hdronly.h>

#include "supple/script_native.h"

namespace supple::script::drumlin_fit {
#include "../script/drumlin.cpp"
}

namespace {

namespace S = supple::script;
namespace D = S::detail;
namespace E = S::drumlin_fit;

constexpr double kRate = 44100.0;  // the recordings' rate: renders are made at it

// --- files -----------------------------------------------------------------------------------

std::vector<double> readWav(const std::string& path) {
    unsigned channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    float* data =
        drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &frames, nullptr);
    if (!data) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(1);
    }
    if (rate != 44100)
        std::fprintf(stderr, "warning: %s is %u Hz, not 44100\n", path.c_str(), rate);
    std::vector<double> x(frames);
    for (drwav_uint64 i = 0; i < frames; ++i) x[i] = data[i * channels];
    drwav_free(data, nullptr);
    return x;
}

void writeWav(const std::string& path, const std::vector<double>& x) {
    drwav_data_format f{};
    f.container = drwav_container_riff;
    f.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    f.channels = 1;
    f.sampleRate = 44100;
    f.bitsPerSample = 32;
    drwav w;
    if (!drwav_init_file_write(&w, path.c_str(), &f, nullptr)) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::vector<float> y(x.begin(), x.end());
    drwav_write_pcm_frames(&w, y.size(), y.data());
    drwav_uninit(&w);
}

// --- a recipe's fields by name --------------------------------------------------------------

double* numberField(E::Recipe& r, const std::string& name) {
    static const std::map<std::string, double E::Recipe::*> top = {
        {"clock", &E::Recipe::clock},
        {"length", &E::Recipe::length},
        {"start", &E::Recipe::start},
        {"drive", &E::Recipe::drive},
        {"strike", &E::Recipe::strike},
        {"bend", &E::Recipe::bend},
        {"bendMs", &E::Recipe::bendMs},
        {"burstMs", &E::Recipe::burstMs},
        {"burstGain", &E::Recipe::burstGain},
        {"burstJitter", &E::Recipe::burstJitter},
        {"level", &E::Recipe::level},
        {"vcfOpen", &E::Recipe::vcfOpen},
        {"vcfShut", &E::Recipe::vcfShut},
        {"vcfMs", &E::Recipe::vcfMs},
        {"hpf", &E::Recipe::hpf},
        {"hold", &E::Recipe::hold},
        {"decay", &E::Recipe::decay},
        {"accent", &E::Recipe::accent},
        {"gate", &E::Recipe::gate},
        {"gateMs", &E::Recipe::gateMs}};
    static const std::map<std::string, double E::Mode::*> mode = {
        {"hz", &E::Mode::hz},     {"gain", &E::Mode::gain},   {"fast", &E::Mode::fast},
        {"slow", &E::Mode::slow}, {"split", &E::Mode::split}, {"phase", &E::Mode::phase}};
    static const std::map<std::string, double E::Band::*> band = {
        {"hz", &E::Band::hz},       {"q", &E::Band::q},           {"gain", &E::Band::gain},
        {"delay", &E::Band::delay}, {"attack", &E::Band::attack}, {"fast", &E::Band::fast},
        {"slow", &E::Band::slow},   {"split", &E::Band::split}};
    if (auto it = top.find(name); it != top.end()) return &(r.*(it->second));
    const auto dot = name.find('.');
    if (dot == std::string::npos) return nullptr;
    const std::string head = name.substr(0, dot), tail = name.substr(dot + 1);
    if (head.rfind("mode", 0) == 0) {
        const int j = std::atoi(head.c_str() + 4);
        auto it = mode.find(tail);
        if (j < 0 || j >= E::kModes || it == mode.end()) return nullptr;
        return &(r.mode[j].*(it->second));
    }
    if (head.rfind("band", 0) == 0) {
        const int b = std::atoi(head.c_str() + 4);
        auto it = band.find(tail);
        if (b < 0 || b >= E::kBands || it == band.end()) return nullptr;
        return &(r.band[b].*(it->second));
    }
    return nullptr;
}

int* intField(E::Recipe& r, const std::string& name) {
    if (name == "modes") return &r.modes;
    if (name == "bands") return &r.bands;
    if (name == "bursts") return &r.bursts;
    if (name == "loop") return &r.loop;
    if (name == "bits") return &r.bits;
    if (name.rfind("band", 0) == 0 && name.size() > 6 &&
        name.substr(name.find('.') + 1) == "shape") {
        const int b = std::atoi(name.c_str() + 4);
        if (b >= 0 && b < E::kBands) return &r.band[b].shape;
    }
    return nullptr;
}

bool setField(E::Recipe& r, const std::string& name, double v) {
    if (double* f = numberField(r, name)) {
        *f = v;
        return true;
    }
    if (int* f = intField(r, name)) {
        *f = static_cast<int>(std::lround(v));
        return true;
    }
    return false;
}

// --- the spec ----------------------------------------------------------------------------------

struct Free {
    std::string name;
    double init = 0.0, lo = 0.0, hi = 1.0;
    bool log = false;
    // [0, 1] (CMA-ES's box) to the value, and back.
    double value(double u) const {
        u = std::clamp(u, 0.0, 1.0);
        return log ? lo * std::pow(hi / lo, u) : lo + (hi - lo) * u;
    }
    double unit(double v) const {
        v = std::clamp(v, lo, hi);
        return log ? std::log(v / lo) / std::log(hi / lo) : (v - lo) / (hi - lo);
    }
};

struct Target {
    std::string path;
    double clock = 20000.0;  // the DAC clock the machine played it at
    double weight = 1.0;
    double decayMs = -1.0;  // the VCA's decay for it (-1: the recipe's)
    int hit = 0;            // the hit's number (where a loop starts)
    bool accent = false;    // an accented hit (the VCA's accent)
    double upto = 0.0;      // seconds after the hit the score stops at (0: the file's end)
    std::vector<double> x;  // the recording
};

struct Spec {
    std::string path;
    E::Recipe base;
    std::vector<Free> free;
    std::vector<Target> targets;
    bool autoDrive = true;
    double peak = 0.95;  // the recording's peak into the DAC, with autoDrive
    bool peakLoaded = false;
    std::set<std::string> loaded;  // fields a `load` set
    // The score's parts: the hit's map, the body's map, the level, the
    // waveform, the body's fine spectrum, the hit's crest factor, and the
    // narrow peaks the render rings with that the recording does not.
    double wHit = 0.5, wBody = 1.0, wLevel = 0.5, wWave = 0.0, wFine = 0.0, wCrest = 0.0;
    double wRing = 0.0;
    double waveMs = 20.0;   // how much of the hit the waveform term compares
    double before = 0.060;  // seconds compared before the hit
    double longest = 1.5;   // ... and after it, at most
};

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Lines: `set <field> <value>`, `free <field> <init> <lo> <hi> [log]`,
// `load <file>` (a fit's .best: its values), `target <wav> clock=<Hz>
// [weight=<w>] [decay=<ms>] [hit=<n>] [accent=1] [upto=<ms>]`, `weights hit body level wave
// [fine [crest [ring]]]`, `drive auto|fixed`, `wave <ms>` (the waveform term's length),
// `window <before s> <longest s>`.
// `#` starts a comment. Paths are relative to the spec's folder unless absolute.
Spec readSpec(const std::string& path) {
    Spec spec;
    spec.path = path;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(1);
    }
    const std::string dir =
        path.find('/') == std::string::npos ? "." : path.substr(0, path.rfind('/'));
    const auto resolve = [&](const std::string& p) {
        return p.empty() || p[0] == '/' ? p : dir + "/" + p;
    };
    std::string line;
    int number = 0;
    const auto fail = [&](const char* what) {
        std::fprintf(stderr, "%s:%d: %s\n", path.c_str(), number, what);
        std::exit(1);
    };
    while (std::getline(in, line)) {
        ++number;
        if (auto hash = line.find('#'); hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        std::istringstream words(line);
        std::string verb;
        words >> verb;
        if (verb == "set") {
            std::string name;
            double v = 0.0;
            words >> name >> v;
            if (!setField(spec.base, name, v)) fail("no such field");
        } else if (verb == "free") {
            Free f;
            std::string mode;
            words >> f.name >> f.init >> f.lo >> f.hi >> mode;
            f.log = mode == "log";
            // `peak` is the spec's own: the recording's peak into the DAC
            // (above 1, the recording clipped), from which its drive follows.
            if (f.name != "peak" && !numberField(spec.base, f.name)) fail("no such number field");
            if (!(f.hi > f.lo) || (f.log && f.lo <= 0.0)) fail("bad range");
            spec.free.push_back(f);
        } else if (verb == "load") {
            std::string file;
            words >> file;
            std::ifstream best(resolve(file));
            if (!best) fail("cannot read the loaded file");
            std::string name;
            double v = 0.0;
            while (best >> name >> v) {
                if (name == "peak") {
                    spec.peak = v;
                    spec.peakLoaded = true;
                } else if (setField(spec.base, name, v)) {
                    spec.loaded.insert(name);
                }
            }
        } else if (verb == "target") {
            Target t;
            std::string rest, word;
            std::getline(words, rest);
            rest = trim(rest);
            // The path may hold spaces: it runs up to the first " key=".
            auto cut = rest.find(" clock=");
            if (cut == std::string::npos) fail("a target needs clock=");
            t.path = resolve(trim(rest.substr(0, cut)));
            std::istringstream keys(rest.substr(cut));
            while (keys >> word) {
                const auto eq = word.find('=');
                const std::string k = word.substr(0, eq);
                const double v = std::atof(word.c_str() + eq + 1);
                if (k == "clock")
                    t.clock = v;
                else if (k == "weight")
                    t.weight = v;
                else if (k == "decay")
                    t.decayMs = v;
                else if (k == "hit")
                    t.hit = static_cast<int>(v);
                else if (k == "accent")
                    t.accent = v != 0.0;
                else if (k == "upto")
                    t.upto = v * 0.001;
                else
                    fail("unknown target key");
            }
            spec.targets.push_back(t);
        } else if (verb == "weights") {
            words >> spec.wHit >> spec.wBody >> spec.wLevel >> spec.wWave;
            if (!(words >> spec.wFine)) spec.wFine = 0.0;
            if (!(words >> spec.wCrest)) spec.wCrest = 0.0;
            if (!(words >> spec.wRing)) spec.wRing = 0.0;
        } else if (verb == "drive") {
            std::string how;
            words >> how;
            spec.autoDrive = how == "auto";
            if (spec.autoDrive && (words >> spec.peak)) {
            }
        } else if (verb == "wave") {
            words >> spec.waveMs;
        } else if (verb == "window") {
            words >> spec.before >> spec.longest;
        } else {
            fail("unknown line");
        }
    }
    // Free fields start at their initial values, unless a loaded file set them.
    for (auto& f : spec.free) {
        if (f.name == "peak") {
            f.init = std::clamp(spec.peakLoaded ? spec.peak : f.init, f.lo, f.hi);
            continue;
        }
        double* field = numberField(spec.base, f.name);
        if (!spec.loaded.count(f.name)) *field = f.init;
        f.init = std::clamp(*field, f.lo, f.hi);
    }
    for (auto& t : spec.targets) t.x = readWav(t.path);
    return spec;
}

// --- rendering ---------------------------------------------------------------------------------

// What one thread needs to render: its own script instance (the runtime's
// objects read the rate from it) and buffers.
struct Context {
    D::Instance instance;
    S::Buffer<double> loops;
    S::Buffer<double> wide;  // the oversampled voice
    E::Voice voice;
    S::Oversampler over;
    Context() {
        instance.sampleRate = kRate;
        instance.maxBlockSize = 4096;
        D::CurrentScope scope(&instance);
        loops.resize(E::kLoops * E::kLoop);
        over.prepare(E::kFactor);
    }
};

// The recording's loudest sample before the DAC, to set its drive. A
// loop's is its table's, as makeLoop() makes it.
double recordingPeak(Context& c, E::Recipe r) {
    r.drive = 1.0;
    double peak = 1e-9;
    if (r.loop >= 0) {
        E::loopRecording(r, c.loops);
        for (int k = r.loop * E::kLoop; k < (r.loop + 1) * E::kLoop; ++k)
            peak = std::max(peak, std::abs(c.loops[k]));
        return peak;
    }
    E::Voice& s = c.voice;
    E::strike(s, r, 0, 0, 0.0, 0, 1.0);
    for (int k = 0; k < s.end; ++k) {
        peak = std::max(peak, std::abs(E::record(s, r)));
        s.at += 1;
    }
    return peak;
}

// Recipe r played at the target's clock: n samples at 44.1 kHz from the
// note's start.
std::vector<double> render(Context& c, const E::Recipe& r, double clock, double decayMs, int n,
                           int hit = 0, bool accent = false) {
    D::CurrentScope scope(&c.instance);
    if (r.loop >= 0) E::makeLoop(r, c.loops);
    const int wideN = n * E::kFactor;
    if (c.wide.size() < wideN) c.wide.resize(wideN);
    for (int i = 0; i < wideN; ++i) c.wide[i] = 0.0;
    E::Voice& s = c.voice;
    E::strike(s, r, 0, 0, 0.0, hit, 1.0);
    E::open(s, r, clock / r.clock, kRate * E::kFactor, decayMs >= 0.0 ? decayMs : r.decay,
            S::dbToGain(r.level + (accent ? r.accent : 0.0)));
    for (int at = 0; at < wideN && s.sounding; at += 256)
        E::play(s, r, c.loops, kRate * E::kFactor, c.wide, at, std::min(256, wideN - at));
    c.over.clear();
    std::vector<double> y(n);
    for (int i = 0; i < n; ++i) {
        c.over.up(0.0);
        for (int k = 0; k < E::kFactor; ++k) c.over.set(k, c.wide[i * E::kFactor + k]);
        y[i] = c.over.down();
    }
    return y;
}

// --- the score -------------------------------------------------------------------------------

// Where the hit is: the first sample past a quarter of the peak.
int hitAt(const std::vector<double>& x) {
    double peak = 0.0;
    for (double v : x) peak = std::max(peak, std::abs(v));
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) > 0.25 * peak) return static_cast<int>(i);
    return 0;
}

struct Bands {
    int size = 0, hop = 0, count = 0;
    std::vector<int> lo, hi;  // bins per band
    std::vector<double> window;
};

Bands makeBands(int size, int hop, double lowHz, double highHz, int count) {
    Bands b;
    b.size = size;
    b.hop = hop;
    b.window.resize(size);
    for (int i = 0; i < size; ++i)
        b.window[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / size);
    const double binHz = kRate / size;
    int last = -1;
    for (int k = 0; k < count; ++k) {
        const double a = lowHz * std::pow(highHz / lowHz, double(k) / count);
        const double z = lowHz * std::pow(highHz / lowHz, double(k + 1) / count);
        int l = std::max(last + 1, static_cast<int>(std::floor(a / binHz)));
        int h = std::max(l + 1, static_cast<int>(std::floor(z / binHz)));
        h = std::min(h, size / 2);
        if (l >= h) continue;
        b.lo.push_back(l);
        b.hi.push_back(h);
        last = h - 1;
    }
    b.count = static_cast<int>(b.lo.size());
    return b;
}

// Band energies (dB) of x in frames centred from `from` for `frames` hops.
std::vector<double> energyMap(const std::vector<double>& x, int from, int frames, const Bands& b) {
    std::vector<double> out(static_cast<size_t>(frames) * b.count);
    std::vector<double> buf(b.size);
    std::vector<std::complex<double>> spec(b.size / 2 + 1);
    const pocketfft::shape_t shape{static_cast<size_t>(b.size)};
    const pocketfft::stride_t sIn{sizeof(double)}, sOut{sizeof(std::complex<double>)};
    for (int f = 0; f < frames; ++f) {
        const int start = from + f * b.hop - b.size / 2;
        for (int i = 0; i < b.size; ++i) {
            const int j = start + i;
            buf[i] = (j >= 0 && j < static_cast<int>(x.size()) ? x[j] : 0.0) * b.window[i];
        }
        pocketfft::r2c(shape, sIn, sOut, 0, true, buf.data(), spec.data(), 1.0);
        for (int k = 0; k < b.count; ++k) {
            double e = 0.0;
            for (int j = b.lo[k]; j < b.hi[k]; ++j) e += std::norm(spec[j]);
            out[static_cast<size_t>(f) * b.count + k] = 10.0 * std::log10(e + 1e-20);
        }
    }
    return out;
}

// Level (dB) of x every 2 ms from `from`, each over 8 ms around it: an
// envelope, not the waveform of a low drum's cycles.
constexpr int kLevelHop = 88;
std::vector<double> levels(const std::vector<double>& x, int from, int frames) {
    std::vector<double> out(frames);
    const int w = 352;
    for (int f = 0; f < frames; ++f) {
        double e = 0.0;
        for (int i = 0; i < w; ++i) {
            const int j = from + f * kLevelHop + i - w / 2;
            if (j >= 0 && j < static_cast<int>(x.size())) e += x[j] * x[j];
        }
        out[f] = 10.0 * std::log10(e / w + 1e-20);
    }
    return out;
}

// The body's fine spectrum (dB per 21.5 Hz bin, 150 Hz to 20 kHz) over
// [from, to): what the band maps blur, a tone standing out of noise or the
// narrow peaks of a cymbal's. The ring's takes n points (finer bins).
constexpr int kFine = 2048;
std::vector<double> fineSpectrum(const std::vector<double>& x, int from, int to, int n = kFine) {
    const int lo = static_cast<int>(150.0 * n / kRate), hi = static_cast<int>(20000.0 * n / kRate);
    std::vector<double> power(hi - lo, 0.0), buf(n);
    std::vector<std::complex<double>> spec(n / 2 + 1);
    const pocketfft::shape_t shape{static_cast<size_t>(n)};
    const pocketfft::stride_t sIn{sizeof(double)}, sOut{sizeof(std::complex<double>)};
    int frames = 0;
    for (int start = from; start + n <= to || frames == 0; start += n / 4) {
        for (int i = 0; i < n; ++i) {
            const int j = start + i;
            const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / n);
            buf[i] = (j >= 0 && j < static_cast<int>(x.size()) ? x[j] : 0.0) * w;
        }
        pocketfft::r2c(shape, sIn, sOut, 0, true, buf.data(), spec.data(), 1.0);
        for (int k = lo; k < hi; ++k) power[k - lo] += std::norm(spec[k]);
        ++frames;
    }
    for (double& p : power) p = 10.0 * std::log10(p / frames + 1e-20);
    return power;
}

// How far each bin of a fine spectrum (dB) stands above the mean of its
// third of an octave (dB): a pure tone stands 15 to 30 dB out, broad noise
// and a real drum's partials, which noise and damping widen, far less.
std::vector<double> prominence(const std::vector<double>& fine, int points) {
    const int lo = static_cast<int>(150.0 * points / kRate), n = static_cast<int>(fine.size());
    std::vector<double> sum(n + 1, 0.0), out(n);
    for (int i = 0; i < n; ++i) sum[i + 1] = sum[i] + fine[i];
    for (int i = 0; i < n; ++i) {
        const int k = lo + i;  // the bin
        const int a = std::max(0, static_cast<int>(k / 1.12) - lo);
        const int b = std::min(n, static_cast<int>(k * 1.12) - lo + 1);
        out[i] = fine[i] - (sum[b] - sum[a]) / std::max(1, b - a);
    }
    return out;
}

// The ring: the render's narrow peaks (within 50 dB of the recording's
// loudest bin) standing out further than the recording does at the same
// bin, by more than kRingSlack dB (what a fixed noise seed's own random
// peaks reach), as the RMS over the worst handful: one pure tone 25 dB out
// where the recording has noise scores about 5 dB, which the fine
// spectrum's mean over all its bins (a fraction of a dB) cannot see. It is
// taken in each of kRingSpans after the hit, the worst counting: a quiet
// tone that outlasts the noise around it shows only once the noise is gone.
// Each span's spectrum is as fine as the span allows (up to 8192 points,
// 5.4 Hz bins): a tone stands further out of noise the finer the bins, as
// the ear hears it, where the noise's own random peaks do not.
constexpr double kRingSlack = 10.0;
constexpr double kRingSpans[] = {0.005, 0.06, 0.25, 1.5};  // seconds after the hit
double ringOf(const std::vector<double>& refProm, double refTop, const std::vector<double>& fine,
              int points) {
    const auto prom = prominence(fine, points);
    double sum = 0.0;
    for (size_t i = 0; i < fine.size(); ++i) {
        if (fine[i] < refTop - 50.0) continue;
        const double over = prom[i] - std::max(refProm[i], 0.0) - kRingSlack;
        if (over > 0.0) sum += over * over;
    }
    return std::sqrt(sum / 8.0);
}

// Crest factors (peak over RMS, dB) and levels (dB) of x in 5 ms windows
// from `from`: how much of the hit is clipped flat, which the spectra miss.
constexpr int kCrestWindow = 220;
void crests(const std::vector<double>& x, int from, int count, std::vector<double>& crest,
            std::vector<double>& level) {
    crest.assign(count, 0.0);
    level.assign(count, -200.0);
    for (int w = 0; w < count; ++w) {
        double peak = 0.0, e = 0.0;
        for (int i = 0; i < kCrestWindow; ++i) {
            const int j = from + w * kCrestWindow + i;
            const double v = j >= 0 && j < static_cast<int>(x.size()) ? x[j] : 0.0;
            peak = std::max(peak, std::abs(v));
            e += v * v;
        }
        const double rms = std::sqrt(e / kCrestWindow);
        crest[w] = 20.0 * std::log10((peak + 1e-12) / (rms + 1e-12));
        level[w] = 20.0 * std::log10(rms + 1e-12);
    }
}

struct Score {
    double total = 0.0, hit = 0.0, body = 0.0, level = 0.0, wave = 0.0, fine = 0.0, crest = 0.0;
    double ring = 0.0;
};

// What a recording looks like to the score, measured once.
struct Reference {
    int at = 0;  // its hit
    int length = 0;
    Bands hitBands, bodyBands;
    int hitFrom = 0, hitFrames = 0, bodyFrom = 0, bodyFrames = 0, levelFrom = 0, levelFrames = 0;
    int fineFrom = 0, fineTo = 0, crestCount = 0;
    std::vector<double> hitMap, bodyMap, level, fine, crest, crestLevel;
    struct Span {
        int from = 0, to = 0, points = kFine;
        double top = 0.0;
        std::vector<double> prom;
    };
    std::vector<Span> spans;  // the ring's
    double top = 0.0, hitTop = 0.0, levelTop = 0.0, fineTop = 0.0;
    std::vector<double> bodyWeight, levelWeight;
};

Reference measure(const Spec& spec, const Target& t) {
    Reference r;
    r.at = hitAt(t.x);
    const int before = static_cast<int>(spec.before * kRate);
    int after =
        std::min(static_cast<int>(t.x.size()) - r.at, static_cast<int>(spec.longest * kRate));
    if (t.upto > 0.0) after = std::min(after, static_cast<int>(t.upto * kRate));
    r.hitBands = makeBands(256, 32, 150.0, 20000.0, 28);
    r.bodyBands = makeBands(2048, 256, 25.0, 20000.0, 44);
    r.hitFrom = r.at - before;
    r.hitFrames = (before + static_cast<int>(0.06 * kRate)) / 32;
    r.bodyFrom = r.at - before;
    r.bodyFrames = (before + after) / 256;
    r.levelFrom = r.at - before;
    r.levelFrames = (before + after) / kLevelHop;
    r.length = r.at + after + 2048;
    r.hitMap = energyMap(t.x, r.hitFrom, r.hitFrames, r.hitBands);
    r.bodyMap = energyMap(t.x, r.bodyFrom, r.bodyFrames, r.bodyBands);
    r.level = levels(t.x, r.levelFrom, r.levelFrames);
    r.fineFrom = r.at + static_cast<int>(0.005 * kRate);
    r.fineTo = r.at + after;
    if (spec.wFine > 0.0) {
        r.fine = fineSpectrum(t.x, r.fineFrom, r.fineTo);
        r.fineTop = *std::max_element(r.fine.begin(), r.fine.end());
    }
    if (spec.wRing > 0.0)
        for (int k = 0; k + 1 < 4; ++k) {
            Reference::Span span;
            span.from = r.at + static_cast<int>(kRingSpans[k] * kRate);
            span.to = std::min(r.at + after, r.at + static_cast<int>(kRingSpans[k + 1] * kRate));
            if (span.to - span.from < kFine / 4) continue;  // a short sound: one frame
            span.points = 1024;
            while (span.points < 8192 && span.points * 2 <= span.to - span.from) span.points *= 2;
            const auto fine = fineSpectrum(t.x, span.from, span.to, span.points);
            span.top = *std::max_element(fine.begin(), fine.end());
            span.prom = prominence(fine, span.points);
            r.spans.push_back(span);
        }
    // The crest factor through the hit's first 60 ms.
    r.crestCount = std::min(after, static_cast<int>(0.06 * kRate)) / kCrestWindow;
    crests(t.x, r.at, r.crestCount, r.crest, r.crestLevel);
    r.top = *std::max_element(r.bodyMap.begin(), r.bodyMap.end());
    r.hitTop = *std::max_element(r.hitMap.begin(), r.hitMap.end());
    r.levelTop = *std::max_element(r.level.begin(), r.level.end());
    // Later frames count less: the hit is what the ear follows most closely.
    const auto weigh = [&](int frames, int hop) {
        std::vector<double> w(frames);
        for (int f = 0; f < frames; ++f) {
            const double sec = (f * hop - before) / kRate;
            w[f] = sec < 0.0 ? 0.5 : 1.0 / (1.0 + sec / 0.08);
        }
        return w;
    };
    r.bodyWeight = weigh(r.bodyFrames, 256);
    r.levelWeight = weigh(r.levelFrames, kLevelHop);
    return r;
}

// Mean squared dB difference, each side floored `floorDb` below the
// reference's top so that what neither can hear does not count.
double mapDistance(const std::vector<double>& a, const std::vector<double>& b, int bands,
                   double top, double floorDb, const std::vector<double>* weight) {
    double sum = 0.0, norm = 0.0;
    const double floor = top - floorDb;
    const int frames = static_cast<int>(a.size()) / bands;
    for (int f = 0; f < frames; ++f) {
        const double w = weight ? (*weight)[f] : 1.0;
        for (int k = 0; k < bands; ++k) {
            const size_t i = static_cast<size_t>(f) * bands + k;
            const double d = std::max(a[i], floor) - std::max(b[i], floor);
            sum += w * d * d;
        }
        norm += w * bands;
    }
    return sum / std::max(norm, 1e-9);
}

Score score(const Spec& spec, const Target& t, const Reference& ref, const std::vector<double>& y) {
    Score s;
    // The render lined up with the recording at the hit.
    const int shift = hitAt(y) - ref.at;
    const auto mapOf = [&](int from, int frames, const Bands& b) {
        return energyMap(y, from + shift, frames, b);
    };
    const auto hitMap = mapOf(ref.hitFrom, ref.hitFrames, ref.hitBands);
    const auto bodyMap = mapOf(ref.bodyFrom, ref.bodyFrames, ref.bodyBands);
    const auto level = levels(y, ref.levelFrom + shift, ref.levelFrames);
    s.hit =
        std::sqrt(mapDistance(ref.hitMap, hitMap, ref.hitBands.count, ref.hitTop, 60.0, nullptr));
    s.body = std::sqrt(
        mapDistance(ref.bodyMap, bodyMap, ref.bodyBands.count, ref.top, 70.0, &ref.bodyWeight));
    s.level = std::sqrt(mapDistance(ref.level, level, 1, ref.levelTop, 70.0, &ref.levelWeight));
    if (spec.wWave > 0.0) {
        // The waveform from just before the hit through its first waveMs.
        double ab = 0.0, aa = 0.0, bb = 0.0;
        const int last = static_cast<int>(spec.waveMs * 0.001 * kRate);
        for (int i = -44; i < last; ++i) {
            const int j = ref.at + i, k = ref.at + shift + i;
            const double a = j >= 0 && j < static_cast<int>(t.x.size()) ? t.x[j] : 0.0;
            const double b = k >= 0 && k < static_cast<int>(y.size()) ? y[k] : 0.0;
            ab += a * b;
            aa += a * a;
            bb += b * b;
        }
        s.wave = 10.0 * (1.0 - ab / std::sqrt(aa * bb + 1e-30));
    }
    if (spec.wFine > 0.0) {
        const auto fine = fineSpectrum(y, ref.fineFrom + shift, ref.fineTo + shift);
        s.fine = std::sqrt(
            mapDistance(ref.fine, fine, static_cast<int>(fine.size()), ref.fineTop, 60.0, nullptr));
    }
    for (const auto& span : ref.spans) {
        const auto fine = fineSpectrum(y, span.from + shift, span.to + shift, span.points);
        s.ring = std::max(s.ring, ringOf(span.prom, span.top, fine, span.points));
    }
    if (spec.wCrest > 0.0) {
        // Windows within 30 dB of the hit's loudest, where a clipped top shows.
        std::vector<double> crest, level;
        crests(y, ref.at + shift, ref.crestCount, crest, level);
        const double top = *std::max_element(ref.crestLevel.begin(), ref.crestLevel.end());
        double sum = 0.0;
        int n = 0;
        for (int w = 0; w < ref.crestCount; ++w)
            if (ref.crestLevel[w] > top - 30.0) {
                const double d = ref.crest[w] - crest[w];
                sum += d * d;
                ++n;
            }
        s.crest = n > 0 ? std::sqrt(sum / n) : 0.0;
    }
    const double wsum = spec.wHit + spec.wBody + spec.wLevel + spec.wFine + spec.wCrest;
    s.total = (spec.wHit * s.hit + spec.wBody * s.body + spec.wLevel * s.level +
               spec.wFine * s.fine + spec.wCrest * s.crest) /
                  wsum +
              spec.wWave * s.wave + spec.wRing * s.ring;
    return s;
}

// --- evaluation ---------------------------------------------------------------------------------

struct Problem {
    const Spec* spec = nullptr;
    std::vector<Reference> refs;
};

E::Recipe recipeAt(const Spec& spec, const std::vector<double>& u, Context& c) {
    E::Recipe r = spec.base;
    double peak = spec.peak;
    for (size_t i = 0; i < spec.free.size(); ++i) {
        if (spec.free[i].name == "peak")
            peak = spec.free[i].value(u[i]);
        else
            *numberField(r, spec.free[i].name) = spec.free[i].value(u[i]);
    }
    if (spec.autoDrive) {
        D::CurrentScope scope(&c.instance);
        r.drive = peak / recordingPeak(c, r);
    }
    return r;
}

Score evaluate(const Problem& p, const E::Recipe& r, Context& c,
               std::vector<Score>* each = nullptr) {
    Score total;
    double wsum = 0.0;
    for (size_t i = 0; i < p.spec->targets.size(); ++i) {
        const Target& t = p.spec->targets[i];
        const Reference& ref = p.refs[i];
        const auto y = render(c, r, t.clock, t.decayMs, ref.length + 4096, t.hit, t.accent);
        const Score s = score(*p.spec, t, ref, y);
        if (each) each->push_back(s);
        total.total += t.weight * s.total;
        total.hit += t.weight * s.hit;
        total.body += t.weight * s.body;
        total.level += t.weight * s.level;
        total.wave += t.weight * s.wave;
        total.fine += t.weight * s.fine;
        total.crest += t.weight * s.crest;
        total.ring += t.weight * s.ring;
        wsum += t.weight;
    }
    total.total /= wsum;
    total.hit /= wsum;
    total.body /= wsum;
    total.level /= wsum;
    total.wave /= wsum;
    total.fine /= wsum;
    total.crest /= wsum;
    total.ring /= wsum;
    return total;
}

// --- CMA-ES --------------------------------------------------------------------------------------

// Eigen decomposition of a symmetric matrix (Jacobi rotations): a = v diag(d) v'.
void eigen(std::vector<double> a, int n, std::vector<double>& d, std::vector<double>& v) {
    v.assign(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) v[i * n + i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) off += a[i * n + j] * a[i * n + j];
        if (off < 1e-22) break;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                if (std::abs(apq) < 1e-30) continue;
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                const double t =
                    (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double cs = 1.0 / std::sqrt(t * t + 1.0), sn = t * cs;
                for (int k = 0; k < n; ++k) {
                    const double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = cs * akp - sn * akq;
                    a[k * n + q] = sn * akp + cs * akq;
                }
                for (int k = 0; k < n; ++k) {
                    const double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = cs * apk - sn * aqk;
                    a[q * n + k] = sn * apk + cs * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = v[k * n + p], vkq = v[k * n + q];
                    v[k * n + p] = cs * vkp - sn * vkq;
                    v[k * n + q] = sn * vkp + cs * vkq;
                }
            }
    }
    d.resize(n);
    for (int i = 0; i < n; ++i) d[i] = a[i * n + i];
}

double reflect(double u) {
    u = std::fmod(std::abs(u), 2.0);
    return u > 1.0 ? 2.0 - u : u;
}

struct Fitted {
    std::vector<double> u;
    double score = 1e300;
};

Fitted cmaes(const Problem& p, std::vector<double> mean, double sigma, int evals, unsigned seed,
             int threads) {
    const int n = static_cast<int>(mean.size());
    const int lambda = std::max(16, 4 + static_cast<int>(3.0 * std::log(n))) * 2;
    const int mu = lambda / 2;
    std::vector<double> w(mu);
    double wsum = 0.0, w2 = 0.0;
    for (int i = 0; i < mu; ++i) {
        w[i] = std::log(mu + 0.5) - std::log(i + 1.0);
        wsum += w[i];
    }
    for (double& x : w) {
        x /= wsum;
        w2 += x * x;
    }
    const double mueff = 1.0 / w2;
    const double cc = (4.0 + mueff / n) / (n + 4.0 + 2.0 * mueff / n);
    const double cs = (mueff + 2.0) / (n + mueff + 5.0);
    const double c1 = 2.0 / ((n + 1.3) * (n + 1.3) + mueff);
    const double cmu =
        std::min(1.0 - c1, 2.0 * (mueff - 2.0 + 1.0 / mueff) / ((n + 2.0) * (n + 2.0) + mueff));
    const double damps = 1.0 + 2.0 * std::max(0.0, std::sqrt((mueff - 1.0) / (n + 1.0)) - 1.0) + cs;
    const double chiN = std::sqrt(n) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));
    std::vector<double> pc(n, 0.0), ps(n, 0.0), C(static_cast<size_t>(n) * n, 0.0), B, Dg(n, 1.0);
    for (int i = 0; i < n; ++i) C[i * n + i] = 1.0;
    eigen(C, n, Dg, B);
    for (double& x : Dg) x = std::sqrt(std::max(x, 1e-30));
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss;
    std::vector<Context> contexts(threads);
    Fitted best;
    int done = 0, generation = 0, eigenAt = 0;
    std::vector<std::vector<double>> z(lambda, std::vector<double>(n)),
        y(lambda, std::vector<double>(n)), x(lambda, std::vector<double>(n));
    std::vector<double> f(lambda);
    while (done < evals) {
        for (int k = 0; k < lambda; ++k) {
            for (int i = 0; i < n; ++i) z[k][i] = gauss(rng);
            for (int i = 0; i < n; ++i) {
                double s = 0.0;
                for (int j = 0; j < n; ++j) s += B[i * n + j] * Dg[j] * z[k][j];
                y[k][i] = s;
                x[k][i] = reflect(mean[i] + sigma * s);
            }
        }
        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (int k; (k = next++) < lambda;) {
                    const E::Recipe r = recipeAt(*p.spec, x[k], contexts[t]);
                    f[k] = evaluate(p, r, contexts[t]).total;
                    if (!std::isfinite(f[k])) f[k] = 1e9;
                }
            });
        for (auto& th : pool) th.join();
        done += lambda;
        ++generation;
        std::vector<int> order(lambda);
        for (int k = 0; k < lambda; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return f[a] < f[b]; });
        if (f[order[0]] < best.score) {
            best.score = f[order[0]];
            best.u = x[order[0]];
        }
        // The new mean from the best half, through the reflected points.
        std::vector<double> old = mean, yw(n, 0.0);
        for (int i = 0; i < n; ++i) {
            double m = 0.0;
            for (int k = 0; k < mu; ++k) m += w[k] * x[order[k]][i];
            mean[i] = m;
            yw[i] = (mean[i] - old[i]) / sigma;
        }
        // C^(-1/2) yw
        std::vector<double> tmp(n), cinv(n);
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int i = 0; i < n; ++i) s += B[i * n + j] * yw[i];
            tmp[j] = s / Dg[j];
        }
        for (int i = 0; i < n; ++i) {
            double s = 0.0;
            for (int j = 0; j < n; ++j) s += B[i * n + j] * tmp[j];
            cinv[i] = s;
        }
        double psn = 0.0;
        for (int i = 0; i < n; ++i) {
            ps[i] = (1.0 - cs) * ps[i] + std::sqrt(cs * (2.0 - cs) * mueff) * cinv[i];
            psn += ps[i] * ps[i];
        }
        psn = std::sqrt(psn);
        const bool hsig = psn / std::sqrt(1.0 - std::pow(1.0 - cs, 2.0 * generation)) / chiN <
                          1.4 + 2.0 / (n + 1.0);
        for (int i = 0; i < n; ++i)
            pc[i] = (1.0 - cc) * pc[i] + (hsig ? std::sqrt(cc * (2.0 - cc) * mueff) * yw[i] : 0.0);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j <= i; ++j) {
                double rmu = 0.0;
                for (int k = 0; k < mu; ++k) {
                    const double yi = (x[order[k]][i] - old[i]) / sigma,
                                 yj = (x[order[k]][j] - old[j]) / sigma;
                    rmu += w[k] * yi * yj;
                }
                const double v =
                    (1.0 - c1 - cmu) * C[i * n + j] +
                    c1 * (pc[i] * pc[j] + (hsig ? 0.0 : cc * (2.0 - cc) * C[i * n + j])) +
                    cmu * rmu;
                C[i * n + j] = v;
                C[j * n + i] = v;
            }
        sigma *= std::exp((cs / damps) * (psn / chiN - 1.0));
        sigma = std::min(sigma, 0.5);
        if (done - eigenAt > lambda / (c1 + cmu) / n / 10.0) {
            eigenAt = done;
            eigen(C, n, Dg, B);
            for (double& v : Dg) v = std::sqrt(std::max(v, 1e-20));
        }
        if (generation % 10 == 0 || done >= evals)
            std::fprintf(stderr, "  %6d evals  best %.4f  gen best %.4f  sigma %.4f\n", done,
                         best.score, f[order[0]], sigma);
        if (sigma < 1e-5) break;
    }
    return best;
}

// --- output ----------------------------------------------------------------------------------

void writeBest(const std::string& path, const Spec& spec, const E::Recipe& r, double peak) {
    std::ofstream out(path);
    out << "peak " << peak << "\n";
    E::Recipe copy = r;
    const char* tops[] = {"clock", "length",  "start",   "drive",     "strike",
                          "bend",  "bendMs",  "burstMs", "burstGain", "burstJitter",
                          "level", "vcfOpen", "vcfShut", "vcfMs",     "hpf",
                          "hold",  "decay",   "accent",  "gate",      "gateMs"};
    for (const char* name : tops) out << name << " " << *numberField(copy, name) << "\n";
    out << "modes " << r.modes << "\nbands " << r.bands << "\nbursts " << r.bursts << "\nloop "
        << r.loop << "\nbits " << r.bits << "\n";
    const char* modeFields[] = {"hz", "gain", "fast", "slow", "split", "phase"};
    for (int j = 0; j < r.modes; ++j)
        for (const char* f : modeFields) {
            const std::string name = "mode" + std::to_string(j) + "." + f;
            out << name << " " << *numberField(copy, name) << "\n";
        }
    const char* bandFields[] = {"hz", "q", "gain", "delay", "attack", "fast", "slow", "split"};
    for (int b = 0; b < r.bands; ++b) {
        out << "band" << b << ".shape " << r.band[b].shape << "\n";
        for (const char* f : bandFields) {
            const std::string name = "band" + std::to_string(b) + "." + f;
            out << name << " " << *numberField(copy, name) << "\n";
        }
    }
    (void)spec;
}

// The recipe as the script's code, for the block between its markers.
void emit(const E::Recipe& r, int m) {
    const auto num = [](double v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.6g", v);
        std::string s = buf;
        if (s.find_first_of(".e") == std::string::npos) s += ".0";
        return s;
    };
    std::printf("    // model %d\n", m);
    std::printf("    recording(book[%d], %s, %s, %s, %s, %s, %s, %s);\n", m, num(r.clock).c_str(),
                num(r.length).c_str(), num(r.start).c_str(), num(r.drive).c_str(),
                num(r.strike).c_str(), num(r.bend).c_str(), num(r.bendMs).c_str());
    std::printf("    machine(book[%d], %s, %s, %s, %s, %s, %s, %s, %s);\n", m, num(r.level).c_str(),
                num(r.vcfOpen).c_str(), num(r.vcfShut).c_str(), num(r.vcfMs).c_str(),
                num(r.hpf).c_str(), num(r.hold).c_str(), num(r.decay).c_str(),
                num(r.accent).c_str());
    if (r.bursts > 0)
        std::printf("    claps(book[%d], %d, %s, %s, %s);\n", m, r.bursts, num(r.burstMs).c_str(),
                    num(r.burstGain).c_str(), num(r.burstJitter).c_str());
    if (r.gate > 0.0)
        std::printf("    cut(book[%d], %s, %s);\n", m, num(r.gate).c_str(), num(r.gateMs).c_str());
    if (r.loop >= 0) std::printf("    book[%d].loop = %d;\n", m, r.loop);
    if (r.bits > 0) std::printf("    book[%d].bits = %d;\n", m, r.bits);
    for (int j = 0; j < r.modes; ++j) {
        const E::Mode& o = r.mode[j];
        std::printf("    addMode(book[%d], %s, %s, %s, %s, %s, %s);\n", m, num(o.hz).c_str(),
                    num(o.gain).c_str(), num(o.fast).c_str(), num(o.slow).c_str(),
                    num(o.split).c_str(), num(o.phase).c_str());
    }
    for (int b = 0; b < r.bands; ++b) {
        const E::Band& o = r.band[b];
        std::printf("    addBand(book[%d], %d, %s, %s, %s, %s, %s, %s, %s, %s);\n", m, o.shape,
                    num(o.hz).c_str(), num(o.q).c_str(), num(o.gain).c_str(), num(o.delay).c_str(),
                    num(o.attack).c_str(), num(o.fast).c_str(), num(o.slow).c_str(),
                    num(o.split).c_str());
    }
}

void report(const Problem& p, const E::Recipe& r, Context& c) {
    std::vector<Score> each;
    const Score s = evaluate(p, r, c, &each);
    std::printf(
        "score %.4f  (hit %.2f dB, body %.2f dB, level %.2f dB, wave %.3f, fine %.2f dB, "
        "crest %.2f dB, ring %.2f dB)\n",
        s.total, s.hit, s.body, s.level, s.wave, s.fine, s.crest, s.ring);
    for (size_t i = 0; i < each.size(); ++i) {
        const auto& t = p.spec->targets[i];
        const auto slash = t.path.rfind('/');
        std::printf(
            "  %-40s %.4f  (hit %.2f, body %.2f, level %.2f, wave %.3f, fine %.2f, "
            "crest %.2f, ring %.2f)\n",
            t.path.substr(slash + 1).c_str(), each[i].total, each[i].hit, each[i].body,
            each[i].level, each[i].wave, each[i].fine, each[i].crest, each[i].ring);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: drumlin-fit fit|score|render|emit <spec> [...]\n"
                     "  fit <spec> [--evals N] [--seed S] [--sigma X] [--threads T]\n"
                     "  score <spec>\n  render <spec> <dir>\n  emit <spec> <model>\n");
        return 2;
    }
    const std::string verb = argv[1];
    Spec spec = readSpec(argv[2]);
    Problem p;
    p.spec = &spec;
    for (const auto& t : spec.targets) p.refs.push_back(measure(spec, t));
    Context c;
    std::vector<double> init(spec.free.size());
    for (size_t i = 0; i < spec.free.size(); ++i) init[i] = spec.free[i].unit(spec.free[i].init);
    if (verb == "score") {
        report(p, recipeAt(spec, init, c), c);
        return 0;
    }
    if (verb == "render") {
        const std::string dir = argc > 3 ? argv[3] : ".";
        const E::Recipe r = recipeAt(spec, init, c);
        for (size_t i = 0; i < spec.targets.size(); ++i) {
            const auto& t = spec.targets[i];
            const auto y =
                render(c, r, t.clock, t.decayMs, p.refs[i].length + 4096, t.hit, t.accent);
            writeWav(dir + "/render" + std::to_string(i) + ".wav", y);
        }
        report(p, r, c);
        return 0;
    }
    if (verb == "floor") {
        // Each recording scored as if it were a render of the next one: how
        // far two real takes lie apart, the least a fit can expect.
        for (size_t i = 0; i + 1 < spec.targets.size(); ++i) {
            const Score s = score(spec, spec.targets[i + 1], p.refs[i + 1], spec.targets[i].x);
            std::printf(
                "%zu vs %zu: %.4f  (hit %.2f, body %.2f, level %.2f, wave %.3f, fine %.2f, crest "
                "%.2f)\n",
                i, i + 1, s.total, s.hit, s.body, s.level, s.wave, s.fine, s.crest);
        }
        return 0;
    }
    if (verb == "emit") {
        emit(recipeAt(spec, init, c), argc > 3 ? std::atoi(argv[3]) : 0);
        return 0;
    }
    if (verb == "fit") {
        int evals = 20000, threads = std::max(1u, std::thread::hardware_concurrency());
        int restarts = 1;
        unsigned seed = 1;
        double sigma = 0.15;
        for (int i = 3; i + 1 < argc; i += 2) {
            const std::string k = argv[i];
            if (k == "--evals")
                evals = std::atoi(argv[i + 1]);
            else if (k == "--seed")
                seed = static_cast<unsigned>(std::atoi(argv[i + 1]));
            else if (k == "--sigma")
                sigma = std::atof(argv[i + 1]);
            else if (k == "--threads")
                threads = std::atoi(argv[i + 1]);
            else if (k == "--restarts")
                restarts = std::max(1, std::atoi(argv[i + 1]));
        }
        std::fprintf(stderr, "fitting %zu parameters to %zu recordings, %d threads\n",
                     spec.free.size(), spec.targets.size(), threads);
        report(p, recipeAt(spec, init, c), c);
        // Several searches from the start, each with its own seed, then the
        // best of them refined with a narrower search.
        Fitted f;
        for (int k = 0; k < restarts; ++k) {
            const Fitted g = cmaes(p, init, sigma, evals, seed + k, threads);
            std::fprintf(stderr, "search %d: %.4f\n", k + 1, g.score);
            if (g.score < f.score) f = g;
        }
        if (restarts > 1) {
            const Fitted g = cmaes(p, f.u, sigma * 0.25, evals / 2, seed + 1000, threads);
            std::fprintf(stderr, "refined: %.4f\n", g.score);
            if (g.score < f.score) f = g;
        }
        const E::Recipe best = recipeAt(spec, f.u, c);
        double peak = spec.peak;
        for (size_t i = 0; i < spec.free.size(); ++i)
            if (spec.free[i].name == "peak") peak = spec.free[i].value(f.u[i]);
        writeBest(spec.path + ".best", spec, best, peak);
        report(p, best, c);
        return 0;
    }
    std::fprintf(stderr, "unknown verb %s\n", verb.c_str());
    return 2;
}
