// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The script runtime's implementation: the one copy of every built-in. A
// source export compiles this file; the JIT calls the same functions through
// the thunks in compiler/src/builtins.cpp. The DSP itself is the plugin framework's
// (framework/dsp); this file only gives it script-facing names and finds the
// sample rate.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "dsp/Dynamics.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"
#include "dsp/Interpolation.h"
#include "dsp/Math.h"
#include "dsp/Oscillators.h"
// core-dsp has a different Oversampler layout. Give this private template
// type its own symbol name while preserving the template's source and the
// file-resource types shared with the player glue.
#define Oversampler ScriptRuntimeOversampler
#include "dsp/Oversampler.h"
#undef Oversampler
#include "dsp/Stft.h"
#include "supple/script.h"
#include "supple/script_code.h"
#include "supple/script_files.h"
#include "supple/script_runtime.h"

namespace supple::script {

Patches& Patches::add(const char* name) {
    builder_->add(name);
    return *this;
}
Patches& Patches::set(const Param& param, double physical) {
    builder_->set(param, physical);
    return *this;
}
Patches& Patches::set(const Param& param, const char* choiceKey) {
    builder_->set(param, choiceKey);
    return *this;
}
Patches& Patches::route(const char* route, double amount) {
    builder_->route(route, amount, "");
    return *this;
}
Patches& Patches::route(const char* route, double amount, const char* via) {
    builder_->route(route, amount, via ? via : "");
    return *this;
}
Patches& Patches::file(const AudioFile& file, const char* factoryFile) {
    builder_->file(&file, factoryFile ? factoryFile : "");
    return *this;
}
Patches& Patches::file(const Convolver& file, const char* factoryFile) {
    builder_->file(&file, factoryFile ? factoryFile : "");
    return *this;
}

namespace detail {

std::string factoryChoiceKey(std::string_view label) {
    std::string key;
    bool gap = false;
    for (unsigned char c : label) {
        if (std::isalnum(c)) {
            if (gap && !key.empty()) key += '_';
            gap = false;
            key += static_cast<char>(std::tolower(c));
        } else
            gap = true;
    }
    if (!key.empty() && std::isdigit(static_cast<unsigned char>(key.front()))) key = "p" + key;
    return key;
}

bool factoryFileName(std::string_view reference) {
    constexpr std::string_view prefix = "factory:";
    if (reference.size() <= prefix.size() || reference.size() > 128 ||
        reference.substr(0, prefix.size()) != prefix)
        return false;
    return reference.substr(prefix.size()).find_first_of(std::string_view("/\\:\0", 4)) ==
           std::string_view::npos;
}

std::string choiceKey(const ParamModel& param, size_t k) {
    if (k < param.choiceKeys.size()) return param.choiceKeys[k];
    return k < param.choices.size() ? factoryChoiceKey(param.choices[k]) : std::string();
}

std::string factoryPatchProblems(const std::vector<ParamModel>& params,
                                 const std::vector<FactoryPatchModel>& patches) {
    std::set<std::string> names;
    for (const auto& patch : patches) {
        if (patch.name.empty() || patch.name.size() > 64 || !names.insert(patch.name).second)
            return "factory patch names must be unique, nonempty and at most 64 bytes";
        std::set<uint32_t> ids;
        for (const auto& [id, value] : patch.values) {
            auto p = std::find_if(params.begin(), params.end(),
                                  [id](const auto& m) { return m.id == id; });
            if (p == params.end() || p->output || !ids.insert(id).second || !std::isfinite(value) ||
                value < p->min || value > p->max ||
                (p->kind != ParamModel::Kind::Continuous && value != std::floor(value)))
                return "factory patch '" + patch.name +
                       "' sets an unknown, readonly, duplicate or invalid parameter value";
        }
    }
    return {};
}

namespace {
// Which instance each thread is running a hook of: a fixed table of
// (thread, instance) pairs, not thread_local storage, which a dynamically
// loaded module gets on a thread's first touch -- an allocation on the
// audio thread's first block. A thread holds a slot only while it is inside
// a hook (CurrentScope), so the table stays small; it is scanned up to the
// highest slot ever taken. There is room for far more threads inside hooks
// at once than any host's pool has; past it, built-ins would fall back to
// the default instance (48 kHz, no status).
constexpr int kSlots = 1024;
struct Slot {
    std::atomic<std::uintptr_t> thread{0};
    Instance* instance = nullptr;  // only its thread reads or writes it
};
Slot gSlots[kSlots];
std::atomic<int> gHighest{0};

std::uintptr_t thisThread() {
#ifdef _WIN32
    return static_cast<std::uintptr_t>(GetCurrentThreadId()) + 1;
#else
    return std::bit_cast<std::uintptr_t>(pthread_self()) | 1;  // never 0, the free mark
#endif
}

Slot* slotOf(std::uintptr_t me) {
    const int highest = gHighest.load(std::memory_order_acquire);
    for (int i = 0; i < highest; ++i)
        if (gSlots[i].thread.load(std::memory_order_relaxed) == me) return &gSlots[i];
    return nullptr;
}

thread_local ParamRegistrar* tRegistrar = nullptr;  // construction: never the audio thread
// Built-in objects used outside any instance (a test, a constant) work at
// this rate.
Instance gDefault;
}  // namespace

Instance* current() {
    const Slot* slot = slotOf(thisThread());
    return slot ? slot->instance : nullptr;
}

void setCurrent(Instance* instance) {
    const std::uintptr_t me = thisThread();
    if (Slot* slot = slotOf(me)) {
        slot->instance = instance;
        if (!instance) slot->thread.store(0, std::memory_order_release);
        return;
    }
    if (!instance) return;
    for (int i = 0; i < kSlots; ++i) {
        std::uintptr_t unused = 0;
        if (gSlots[i].thread.compare_exchange_strong(unused, me, std::memory_order_acq_rel)) {
            gSlots[i].instance = instance;
            int highest = gHighest.load(std::memory_order_relaxed);
            while (highest < i + 1 &&
                   !gHighest.compare_exchange_weak(highest, i + 1, std::memory_order_release));
            return;
        }
    }
    // More threads inside hooks at once than slots: the built-ins fall back
    // to the default instance's rate.
}

ParamRegistrar* registrar() { return tRegistrar; }
void setRegistrar(ParamRegistrar* registrar) { tRegistrar = registrar; }

void declareShared(void* member, int bytes) {
    if (auto* r = registrar()) r->declareShared(member, bytes);
}

void outOfRange(int line) {
    Instance* in = current();
    if (!in) return;
    if (in->status.outOfRange.fetch_add(1, std::memory_order_relaxed) == 0)
        in->status.outOfRangeLine.store(static_cast<std::uint32_t>(line),
                                        std::memory_order_relaxed);
}

bool reserve(std::int64_t bytes) {
    Instance* in = current();
    if (!in || in->memory < 0) return true;
    if (bytes > Instance::kMemoryLimit - in->memory) {
        in->status.memoryRefused.store(true, std::memory_order_relaxed);
        return false;
    }
    in->memory += bytes;
    return true;
}

static double rate() {
    const Instance* found = current();
    const Instance* in = found ? found : &gDefault;
    return in->sampleRate;
}

}  // namespace detail

// --- numbers -------------------------------------------------------------------

double sin(double x) { return std::sin(x); }
double cos(double x) { return std::cos(x); }
double tan(double x) { return std::tan(x); }
double asin(double x) { return std::asin(x); }
double acos(double x) { return std::acos(x); }
double atan(double x) { return std::atan(x); }
double atan2(double y, double x) { return std::atan2(y, x); }
double sinh(double x) { return std::sinh(x); }
double cosh(double x) { return std::cosh(x); }
double tanh(double x) { return std::tanh(x); }
double exp(double x) { return std::exp(x); }
double exp2(double x) { return std::exp2(x); }
double log(double x) { return std::log(x); }
double log2(double x) { return std::log2(x); }
double log10(double x) { return std::log10(x); }
double pow(double x, double y) { return std::pow(x, y); }
double sqrt(double x) { return std::sqrt(x); }
double cbrt(double x) { return std::cbrt(x); }
double floor(double x) { return std::floor(x); }
double ceil(double x) { return std::ceil(x); }
double trunc(double x) { return std::trunc(x); }
double round(double x) { return std::round(x); }
double fmod(double x, double y) { return std::fmod(x, y); }
double hypot(double x, double y) { return std::hypot(x, y); }

double dbToGain(double db) { return dsp::dbToGain(db); }
double gainToDb(double gain) { return dsp::gainToDb(gain); }
double mtof(double note) { return 440.0 * std::exp2((note - 69.0) / 12.0); }
double ftom(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
double compressorGainDb(double levelDb, double thresholdDb, double ratio, double kneeDb) {
    const dsp::GainComputer curve{thresholdDb, ratio, kneeDb};
    return curve.gainDb(levelDb);
}

namespace {
int saturate(double x) {
    if (!(x == x)) return 0;
    if (x >= 2147483647.0) return INT_MAX;
    if (x <= -2147483648.0) return INT_MIN;
    return static_cast<int>(x);
}
}  // namespace

int shl(int x, int n) {
    return static_cast<unsigned>(n) < 32 ? std::bit_cast<int>(std::bit_cast<std::uint32_t>(x) << n)
                                         : 0;
}
int shr(int x, int n) {
    return static_cast<unsigned>(n) < 32 ? std::bit_cast<int>(std::bit_cast<std::uint32_t>(x) >> n)
                                         : 0;
}
int bit(int x, int n) { return shr(x, n) & 1; }
int popcount(int x) { return std::popcount(std::bit_cast<std::uint32_t>(x)); }

void Pulse::fire() {
    const auto* in = detail::current();
    const double rate = in ? in->sampleRate : 48000.0;
    fire(max(1, ceilToInt(rate * 0.001)));
}

int toInt(double x) { return saturate(x); }
int floorToInt(double x) {
    // A normal positive value truncates to its floor without a libm call.
    // Keep libm for the other cases, including subnormals under FTZ/DAZ.
    if (x >= 1.0 && x <= 2147483647.0) return static_cast<int>(x);
    return saturate(std::floor(x));
}
int ceilToInt(double x) { return saturate(std::ceil(x)); }
int roundToInt(double x) { return saturate(std::round(x)); }
int idiv(int a, int b) {
    if (b == 0) return 0;
    if (b == -1) return static_cast<int>(0u - static_cast<unsigned>(a));
    return a / b;
}
int imod(int a, int b) {
    if (b == 0 || b == -1) return 0;
    return a % b;
}
int wrap(int i, int n) {
    if (n <= 0) return 0;
    const int r = i % n;
    return r < 0 ? r + n : r;
}

// --- containers ----------------------------------------------------------------

namespace {
std::atomic<std::size_t> gBufferAllocations{0};

template <class T>
T* allocateBuffer(int size) {
    if (size <= 0) return nullptr;
    auto* items = static_cast<T*>(std::calloc(static_cast<std::size_t>(size), sizeof(T)));
    if (items) gBufferAllocations.fetch_add(1, std::memory_order_relaxed);
    return items;
}

template <class T>
void freeBuffer(T* items) {
    if (!items) return;
    std::free(items);
    gBufferAllocations.fetch_sub(1, std::memory_order_relaxed);
}
}  // namespace

std::size_t detail::bufferAllocations() {
    return gBufferAllocations.load(std::memory_order_relaxed);
}

template <class T>
Buffer<T>& Buffer<T>::operator=(const Buffer& other) {
    if (this == &other) return *this;
    if (size_ != other.size_) {
        freeBuffer(items_);
        items_ = allocateBuffer<T>(other.size_);
        size_ = items_ ? other.size_ : 0;
    }
    if (size_ > 0) std::memcpy(items_, other.items_, static_cast<std::size_t>(size_) * sizeof(T));
    return *this;
}

template <class T>
Buffer<T>::~Buffer() {
    freeBuffer(items_);
}

template <class T>
void Buffer<T>::resize(int size) {
    // A bound, not a policy: 64 M elements is far beyond any delay or table
    // a plugin needs, and keeps a mistaken size from exhausting memory.
    constexpr int kMax = 1 << 26;
    size = size < 0 ? 0 : (size > kMax ? kMax : size);
    // Counted before the size is compared: a second prepare() resizes to what it already has.
    if (!detail::reserve(static_cast<std::int64_t>(size) * static_cast<std::int64_t>(sizeof(T))))
        return;
    if (size == size_) return;
    T* grown = allocateBuffer<T>(size);
    if (size > 0 && !grown) return;  // out of memory: keep what there was
    const int keep = size < size_ ? size : size_;
    if (keep > 0) std::memcpy(grown, items_, static_cast<std::size_t>(keep) * sizeof(T));
    freeBuffer(items_);
    items_ = grown;
    size_ = size;
}

template class Buffer<double>;
template class Buffer<float>;
template class Buffer<int>;

// --- parameters ----------------------------------------------------------------

namespace detail {
namespace {
template <class T, class Spec>
T numbered(const Spec& spec, int index) {
    const std::string suffix = std::to_string(index + 1);
    const std::string name = std::string(spec.name ? spec.name : "") + " " + suffix;
    const std::string key = spec.key && *spec.key ? std::string(spec.key) + "_" + suffix : "";
    Spec element = spec;
    element.name = name.c_str();
    element.key = key.c_str();
    return T(element);
}
}  // namespace
Param numberedParam(const ParamSpec& s, int i) { return numbered<Param>(s, i); }
Input numberedInput(const PortSpec& s, int i) { return numbered<Input>(s, i); }
Output numberedOutput(const PortSpec& s, int i) { return numbered<Output>(s, i); }
void resetMember(Param& p, double*) { ParamBinding::reset(p); }
void resetMember(Input& p, double*) {
    PortBinding::bindInput(p, nullptr, nullptr, 0, 0, 0);
    PortBinding::setSlot(p, -1);
}
void resetMember(Output& p, double* spare) {
    *spare = 0.0;
    PortBinding::bindOutput(p, nullptr, spare, 0, 1, 0);
    PortBinding::setSlot(p, -1);
}
}  // namespace detail

Param::Param(const ParamSpec& spec) {
    value_ = spec.def;
    min_ = spec.min;
    offAtMinimum_ = spec.offAtMinimum;
    if (auto* r = detail::registrar()) r->declare(*this, spec);
}

AudioFile::AudioFile(const FileSpec& spec) {
    if (auto* r = detail::registrar()) r->declareFile(this, spec);
}
Convolver::Convolver(const FileSpec& spec) {
    if (auto* r = detail::registrar()) r->declareFile(this, spec);
}
Source::Source(const SourceSpec& spec) {
    low_ = spec.bipolar ? -1.0 : 0.0;
    perVoice_ = spec.perVoice;
    if (auto* r = detail::registrar()) r->declareSource(*this, spec);
}

Param::Param(const Param& other) { *this = other; }

Input::Input(const PortSpec& spec) {
    if (auto* r = detail::registrar()) r->declarePort(this, spec);
}
Output::Output(const PortSpec& spec) {
    if (auto* r = detail::registrar()) r->declarePort(this, spec);
}

Param& Param::operator=(const Param& other) {
    value_ = other.value_;
    glide_ = other.glide_;
    glideFrames_ = other.glideFrames_;
    mod_ = other.mod_;
    modStride_ = other.modStride_;
    modChannels_ = other.modChannels_;
    changed_ = other.changed_;
    offAtMinimum_ = other.offAtMinimum_;
    min_ = other.min_;
    reported_ = other.reported_;
    slot_ = other.slot_;
    return *this;
}

double Param::gain() const {
    if (offAtMinimum_ && value_ <= min_) return 0.0;
    return value_ == 0.0 ? 1.0 : std::pow(10.0, value_ / 20.0);
}

// --- the plugin ----------------------------------------------------------------

namespace {
const detail::Instance& instanceOf(detail::Instance* in) {
    static const detail::Instance none;
    return in ? *in : none;
}
}  // namespace

double Plugin::sampleRate() const { return instanceOf(instance_).sampleRate; }
int Plugin::maxBlockSize() const { return instanceOf(instance_).maxBlockSize; }
int Plugin::channels() const { return instanceOf(instance_).channels; }
bool Plugin::offline() const { return instanceOf(instance_).offline; }
void Plugin::setLatency(int samples) {
    if (instance_) instance_->latency = samples < 0 ? 0 : samples;
}
void Plugin::setTail(int samples) {
    if (instance_) instance_->tail = samples < 0 ? -1 : samples;
}
void Plugin::trace(const char* name, double value) const { trace(name, value, 0); }
void Plugin::trace(const char* name, double value, int i) const {
    if (!instance_ || !name) return;
    if (detail::TraceLog* log = instance_->trace.load(std::memory_order_acquire))
        log->record(name, value, log->at + (i > 0 ? i : 0));
}

// --- traces --------------------------------------------------------------------

namespace detail {

TraceLog::TraceLog(int interval_, int bins_)
    : interval(std::max(1, interval_)),
      bins(std::clamp(bins_, 1, kMaxBins)),
      last(static_cast<std::size_t>(kNames * bins), std::nan("")),
      low(last),
      high(last) {}

void TraceLog::record(const char* text, double value, std::int64_t frame) {
    const std::int64_t bin = frame / interval;
    if (bin >= bins) return;
    // The name by what it says (not where its text lies: a script reloaded
    // since may hold another name there).
    const int count = names.load(std::memory_order_relaxed);
    int k = 0;
    while (k < count && std::strncmp(name[k], text, kNameBytes - 1) != 0) ++k;
    if (k == count) {
        if (count == kNames) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        int n = 0;
        for (; n < kNameBytes - 1 && text[n]; ++n) name[k][n] = text[n];
        name[k][n] = '\0';
        names.store(count + 1, std::memory_order_release);
    }
    if (!std::isfinite(value)) {
        nonFinite[k].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto cell = static_cast<std::size_t>(k * bins + bin);
    if (std::isnan(last[cell])) {
        low[cell] = value;
        high[cell] = value;
    } else {
        low[cell] = std::min(low[cell], value);
        high[cell] = std::max(high[cell], value);
    }
    last[cell] = value;
}

}  // namespace detail

// --- instruments -----------------------------------------------------------------

namespace {
// The bank that has voice v, or null (and the index reported when it is out
// of range).
detail::VoiceBank* bankFor(detail::Instance* in, int v) {
    detail::VoiceBank* bank = in ? in->voices : nullptr;
    if (!bank) return nullptr;
    if (v < 0 || v >= bank->count()) {
        detail::outOfRange(0);
        return nullptr;
    }
    return bank;
}
}  // namespace

int Instrument::voiceCount() const {
    const detail::VoiceBank* bank = instance_ ? instance_->voices : nullptr;
    return bank ? bank->count() : 0;
}
bool Instrument::voicePlaying(int v) const {
    const detail::VoiceBank* bank = bankFor(instance_, v);
    return bank && bank->playing(v);
}
int Instrument::voiceKey(int v) const {
    const detail::VoiceBank* bank = bankFor(instance_, v);
    return bank ? bank->key(v) : 0;
}
int Instrument::voiceChannel(int v) const {
    const detail::VoiceBank* bank = bankFor(instance_, v);
    return bank ? bank->channel(v) : 0;
}
double Instrument::voiceVelocity(int v) const {
    const detail::VoiceBank* bank = bankFor(instance_, v);
    return bank ? bank->velocity(v) : 0.0;
}
double Instrument::voicePitch(int v, int i) const {
    const detail::VoiceBank* bank = bankFor(instance_, v);
    return bank ? bank->pitch(v, i) : 0.0;
}
double Instrument::voiceHz(int v, int i) const { return mtof(voicePitch(v, i)); }
namespace {
double expressionOf(detail::Instance* instance, int v, int which, double neutral) {
    const detail::VoiceBank* bank = bankFor(instance, v);
    return bank ? bank->expression(v, which) : neutral;
}
}  // namespace
double Instrument::voiceVolume(int v) const { return expressionOf(instance_, v, 0, 1.0); }
double Instrument::voicePan(int v) const { return expressionOf(instance_, v, 1, 0.5); }
double Instrument::voiceVibrato(int v) const { return expressionOf(instance_, v, 3, 0.0); }
double Instrument::voiceExpression(int v) const { return expressionOf(instance_, v, 4, 0.0); }
double Instrument::voiceBrightness(int v) const { return expressionOf(instance_, v, 5, 0.0); }
double Instrument::voicePressure(int v) const { return expressionOf(instance_, v, 6, 0.0); }

// An instrument's voices, through the instance whose hook is running.
double Param::forVoice(int v) const {
    const detail::VoiceBank* bank = bankFor(detail::current(), v);
    return bank && slot_ >= 0 ? bank->modulated(slot_, v, value_) : value_;
}
double Param::routed(int i) const {
    const double base = at(i);
    const detail::Instance* in = slot_ >= 0 ? detail::current() : nullptr;
    return in && in->routes ? in->routes->routed(slot_, -1, base) : base;
}
void Instrument::voiceFinished(int v) {
    if (detail::VoiceBank* bank = bankFor(instance_, v)) bank->finished(v);
}
void Instrument::setVoiceMode(VoiceMode mode) {
    if (detail::VoiceBank* bank = instance_ ? instance_->voices : nullptr) bank->setMode(mode);
}
void Instrument::setGlide(double ms) {
    if (detail::VoiceBank* bank = instance_ ? instance_->voices : nullptr)
        bank->setGlide(std::isfinite(ms) ? std::max(0.0, ms) : 0.0);
}
void Instrument::setBendRange(double semitones) {
    if (detail::VoiceBank* bank = instance_ ? instance_->voices : nullptr)
        bank->setBendRange(std::isfinite(semitones) ? std::clamp(semitones, 0.0, 96.0) : 2.0);
}
void Instrument::setVoiceLimit(int voices) {
    if (detail::VoiceBank* bank = instance_ ? instance_->voices : nullptr)
        bank->setVoiceLimit(std::clamp(voices, 1, bank->count()));
}

// --- DSP -------------------------------------------------------------------------

namespace {

// Each object's state lives in the storage its class declares in script.h;
// these are what that storage holds.
struct BiquadState {
    dsp::Biquad coefficients;
    dsp::Biquad::State state;
};
struct OnePoleState {
    dsp::OnePole filter;
    dsp::OnePole::State state;
};
struct FollowerState {
    dsp::EnvelopeFollower follower;
    dsp::EnvelopeFollower::State state;
};

template <class T, class Storage>
T& as(Storage& storage) {
    static_assert(sizeof(T) <= sizeof(storage),
                  "a built-in object's storage in script.h is too small");
    static_assert(alignof(T) <= 8, "a built-in object needs more alignment than script.h gives it");
    return *std::launder(reinterpret_cast<T*>(&storage));
}
template <class T, class Storage>
const T& as(const Storage& storage) {
    return *std::launder(reinterpret_cast<const T*>(&storage));
}

}  // namespace

Biquad::Biquad() { new (storage_) BiquadState(); }
void Biquad::lowpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::lowpass(hz, q, detail::rate());
}
void Biquad::highpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::highpass(hz, q, detail::rate());
}
void Biquad::bandpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::bandpass(hz, q, detail::rate());
}
void Biquad::notch(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::notch(hz, q, detail::rate());
}
void Biquad::allpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::allpass(hz, q, detail::rate());
}
void Biquad::peak(double hz, double q, double gainDb) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::peak(hz, q, gainDb, detail::rate());
}
void Biquad::lowShelf(double hz, double q, double gainDb) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::lowShelf(hz, q, gainDb, detail::rate());
}
void Biquad::highShelf(double hz, double q, double gainDb) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::highShelf(hz, q, gainDb, detail::rate());
}
double Biquad::process(double x) {
    auto& b = as<BiquadState>(storage_);
    return b.coefficients.tick(b.state, x);
}
void Biquad::clear() { as<BiquadState>(storage_).state = {}; }
double Biquad::responseDb(double hz) const {
    return 20.0 * std::log10(std::abs(
                      as<BiquadState>(storage_).coefficients.response(hz, detail::rate())));
}

OnePole::OnePole() { new (storage_) OnePoleState(); }
void OnePole::setCutoff(double hz) {
    as<OnePoleState>(storage_).filter.setCutoff(hz, detail::rate());
}
double OnePole::lowpass(double x) {
    auto& f = as<OnePoleState>(storage_);
    return f.filter.lowpass(f.state, x);
}
double OnePole::highpass(double x) {
    auto& f = as<OnePoleState>(storage_);
    return f.filter.highpass(f.state, x);
}
void OnePole::clear() { as<OnePoleState>(storage_).state = {}; }

EnvelopeFollower::EnvelopeFollower() { new (storage_) FollowerState(); }
void EnvelopeFollower::set(double attackMs, double releaseMs) {
    as<FollowerState>(storage_).follower.setTimes(attackMs, releaseMs, detail::rate());
}
double EnvelopeFollower::process(double x) {
    auto& f = as<FollowerState>(storage_);
    return f.follower.tick(f.state, x);
}
double EnvelopeFollower::level() const { return as<FollowerState>(storage_).state.envelope; }
void EnvelopeFollower::clear() { as<FollowerState>(storage_).state = {}; }

// A resource: the line's buffer is allocated, so the object holds it by
// pointer. Constructed with the plugin (main thread); prepare() sizes it.
namespace {
dsp::FractionalDelay& line(void* p) { return *static_cast<dsp::FractionalDelay*>(p); }
}  // namespace
DelayLine::DelayLine() : line_(new dsp::FractionalDelay()) {}
DelayLine::DelayLine(const DelayLine& other) : line_(new dsp::FractionalDelay(line(other.line_))) {}
DelayLine& DelayLine::operator=(const DelayLine& other) {
    // Restoring a plugin's state copies lines of equal capacity, which
    // std::vector does in place; only a differently sized line allocates.
    if (this != &other) line(line_) = line(other.line_);
    return *this;
}
DelayLine::~DelayLine() { delete &line(line_); }
void DelayLine::prepare(int maxDelaySamples) {
    if (maxDelaySamples < 0) maxDelaySamples = 0;
    // The framework's ring: a power of two, four samples longer than the delay.
    std::int64_t ring = 4;
    while (ring < std::int64_t{maxDelaySamples} + 4) ring <<= 1;
    if (!detail::reserve(ring * static_cast<std::int64_t>(sizeof(double)))) return;
    line(line_).prepare(maxDelaySamples);
}
void DelayLine::push(double x) { line(line_).push(x); }
double DelayLine::read(double delaySamples) const {
    // The framework's line converts the delay to an index: NaN must not reach it.
    if (!(delaySamples == delaySamples)) delaySamples = 0.0;
    return line(line_).read(delaySamples);
}
void DelayLine::clear() { line(line_).clear(); }

// A resource, as a DelayLine is: its filters' buffers are allocated in
// prepare(). Unprepared, it passes samples through.
namespace {
struct OversamplerState {
    dsp::ScriptRuntimeOversampler over;
    bool prepared = false;
    double one = 0.0;      // the sample, unprepared
    double* hi = nullptr;  // up()'s factor() samples, until down()
};
OversamplerState& over(void* p) { return *static_cast<OversamplerState*>(p); }
}  // namespace
Oversampler::Oversampler() : state_(new OversamplerState()) {}
Oversampler::Oversampler(const Oversampler& other)
    : state_(new OversamplerState(over(other.state_))) {
    over(state_).hi = nullptr;
}
Oversampler& Oversampler::operator=(const Oversampler& other) {
    // Filters of the same factor copy in place, as std::vector does.
    if (this != &other) {
        over(state_) = over(other.state_);
        over(state_).hi = nullptr;
    }
    return *this;
}
Oversampler::~Oversampler() { delete &over(state_); }
void Oversampler::prepare(int factor) { prepare(factor, Phase::Minimum); }
void Oversampler::prepare(int factor, Phase phase) {
    // Its filters, at any factor, hold less than this.
    if (!detail::reserve(std::int64_t{16} << 10)) return;
    OversamplerState& o = over(state_);
    // 1 passes samples through: a restart to 1x needs no code of its own.
    o.over.prepare(1, 1, std::clamp(factor, 1, 16),
                   phase == Phase::Linear ? dsp::ScriptRuntimeOversampler::Phase::Linear
                                          : dsp::ScriptRuntimeOversampler::Phase::Minimum);
    o.prepared = true;
    o.hi = nullptr;
}
int Oversampler::factor() const {
    const OversamplerState& o = over(state_);
    return o.prepared ? o.over.factor() : 1;
}
int Oversampler::latency() const {
    const OversamplerState& o = over(state_);
    return o.prepared ? o.over.latency() : 0;
}
void Oversampler::up(double x) {
    OversamplerState& o = over(state_);
    if (!(x == x)) x = 0.0;
    if (!o.prepared) {
        o.one = x;
        o.hi = &o.one;
        return;
    }
    o.hi = o.over.up(0, &x, 1);
}
double Oversampler::sample(int k) const {
    const OversamplerState& o = over(state_);
    if (!o.hi || k < 0 || k >= factor()) {
        detail::outOfRange(0);
        return 0.0;
    }
    return o.hi[k];
}
void Oversampler::set(int k, double value) {
    OversamplerState& o = over(state_);
    if (!o.hi || k < 0 || k >= factor()) {
        detail::outOfRange(0);
        return;
    }
    o.hi[k] = value;
}
double Oversampler::down() {
    OversamplerState& o = over(state_);
    if (!o.prepared) {
        const double y = o.hi ? o.one : 0.0;
        o.hi = nullptr;
        return y;
    }
    double y = 0.0;
    if (o.hi) o.over.down(0, &y, 1);
    o.hi = nullptr;
    return y;
}
void Oversampler::clear() {
    OversamplerState& o = over(state_);
    if (o.prepared) o.over.reset();
    o.one = 0.0;
    o.hi = nullptr;
}

// A resource, like DelayLine: one lane of the framework's STFT.
namespace {
struct StftState {
    dsp::Stft stft;
    bool prepared = false;
    bool ready = false;  // a spectrum to read and change, until pull()
    double one = 0.0;    // the sample, unprepared
};
StftState& stft(void* p) { return *static_cast<StftState*>(p); }
const StftState& stft(const void* p) { return *static_cast<const StftState*>(p); }
// Bin k of the ready spectrum, or null.
std::complex<double>* binOf(void* p, int k) {
    StftState& s = stft(p);
    if (!s.ready || k < 0 || k >= s.stft.bins()) return nullptr;
    return s.stft.spectrum(0) + k;
}
// What a transform of `size` holds, counted: its rings, window, frame and FFT
// come to less than 64 bytes a sample and a kilobyte.
bool reserveStft(int size) { return detail::reserve(std::int64_t{64} * size + 1024); }
}  // namespace
Stft::Stft() : state_(new StftState()) {}
Stft::Stft(const Stft& other) : state_(new StftState(stft(other.state_))) {}
Stft& Stft::operator=(const Stft& other) {
    // Transforms of one size copy in place, as std::vector does.
    if (this != &other) stft(state_) = stft(other.state_);
    return *this;
}
Stft::~Stft() { delete &stft(state_); }
void Stft::prepare(int size, int overlap) {
    size = std::clamp(size, 16, 32768);
    if (!reserveStft(size)) return;
    StftState& s = stft(state_);
    s.stft.prepare(1, size, overlap);
    s.prepared = true;
    s.ready = false;
}
void Stft::prepareAnalysis(int size, int overlap) {
    size = std::clamp(size, 16, 32768);
    if (!reserveStft(size)) return;
    StftState& s = stft(state_);
    s.stft.prepare(1, size, overlap, dsp::Stft::Mode::Analyse);
    s.prepared = true;
    s.ready = false;
}
int Stft::size() const { return stft(state_).prepared ? stft(state_).stft.size() : 0; }
int Stft::bins() const { return stft(state_).prepared ? stft(state_).stft.bins() : 0; }
int Stft::hop() const { return stft(state_).prepared ? stft(state_).stft.hop() : 0; }
int Stft::latency() const { return stft(state_).prepared ? stft(state_).stft.latency() : 0; }
double Stft::binHz(int k) const {
    const StftState& s = stft(state_);
    return s.prepared ? s.stft.binHz(k, detail::rate()) : 0.0;
}
bool Stft::push(double x) {
    StftState& s = stft(state_);
    if (!(x == x)) x = 0.0;
    if (!s.prepared) {
        s.one = x;
        return false;
    }
    s.ready = s.stft.push(0, x);
    return s.ready;
}
double Stft::pull() {
    StftState& s = stft(state_);
    // Analysing, the bins stay readable until the next push().
    if (s.prepared && s.stft.mode() == dsp::Stft::Mode::Analyse) return s.stft.pull(0);
    s.ready = false;
    if (!s.prepared) {
        const double y = s.one;
        s.one = 0.0;
        return y;
    }
    return s.stft.pull(0);
}
// Bins as scripts see them: a sine of amplitude 1 at a bin's centre reads 1
// there (the transform's own value is size / 4 under a Hann window). Both
// factors are powers of two, so the scaling is exact.
namespace {
double toScript(const void* p) { return 4.0 / stft(p).stft.size(); }
double fromScript(const void* p) { return stft(p).stft.size() / 4.0; }
}  // namespace
double Stft::real(int k) const {
    const auto* b = binOf(state_, k);
    return b ? b->real() * toScript(state_) : 0.0;
}
double Stft::imag(int k) const {
    const auto* b = binOf(state_, k);
    return b ? b->imag() * toScript(state_) : 0.0;
}
double Stft::magnitude(int k) const {
    const auto* b = binOf(state_, k);
    return b ? std::abs(*b) * toScript(state_) : 0.0;
}
double Stft::phase(int k) const {
    const auto* b = binOf(state_, k);
    return b ? std::arg(*b) : 0.0;
}
void Stft::set(int k, double real, double imag) {
    if (auto* b = binOf(state_, k)) *b = std::complex<double>{real, imag} * fromScript(state_);
}
void Stft::setPolar(int k, double magnitude, double phase) {
    if (auto* b = binOf(state_, k))
        *b = std::complex<double>{magnitude * std::cos(phase), magnitude * std::sin(phase)} *
             fromScript(state_);
}
void Stft::scale(int k, double gain) {
    if (auto* b = binOf(state_, k)) *b *= gain;
}
void Stft::clear() {
    StftState& s = stft(state_);
    if (s.prepared) s.stft.reset();
    s.ready = false;
    s.one = 0.0;
}

// --- files ----------------------------------------------------------------------

bool AudioFile::loaded() const { return contents_ && contents_->audio.frames() > 0; }
int AudioFile::channels() const { return contents_ ? contents_->audio.channels() : 0; }
int AudioFile::frames() const {
    return contents_ ? static_cast<int>(contents_->audio.frames()) : 0;
}
double AudioFile::sample(int channel, int frame) const {
    if (!contents_ || channel < 0) return 0.0;
    const auto& all = contents_->audio.channel;
    if (all.empty()) return 0.0;
    const std::vector<float>& c = all[std::min(static_cast<size_t>(channel), all.size() - 1)];
    return static_cast<unsigned>(frame) < c.size()
               ? static_cast<double>(c[static_cast<size_t>(frame)])
               : 0.0;
}
double AudioFile::read(int channel, double frame) const {
    if (!(frame == frame) || frame < -2.0 || frame > 2147483000.0) return 0.0;
    const double whole = std::floor(frame);
    const int i = static_cast<int>(whole);
    return dsp::hermite(sample(channel, i - 1), sample(channel, i), sample(channel, i + 1),
                        sample(channel, i + 2), frame - whole);
}

double AudioFile::peak(int channel, int from, int to) const {
    return contents_ ? contents_->peak(channel, from, to) : 0.0;
}
double AudioFile::rms(int channel, int from, int to) const {
    return contents_ ? contents_->rms(channel, from, to) : 0.0;
}

void detail::FileContents::measure() {
    levels.assign(audio.channel.size(), Levels{});
    for (size_t c = 0; c < audio.channel.size(); ++c) {
        const std::vector<float>& x = audio.channel[c];
        Levels& l = levels[c];
        const size_t blocks = (x.size() + kBlock - 1) / kBlock;
        l.energy.assign(blocks + 1, 0.0);
        std::vector<float> top(blocks, 0.0f);
        for (size_t b = 0; b < blocks; ++b) {
            double sum = 0.0;
            float most = 0.0f;
            for (size_t i = b * kBlock; i < std::min(x.size(), (b + 1) * kBlock); ++i) {
                sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
                most = std::max(most, std::abs(x[i]));
            }
            l.energy[b + 1] = l.energy[b] + sum;
            top[b] = most;
        }
        l.peak.push_back(std::move(top));
        while (l.peak.back().size() > kBlock) {
            const std::vector<float>& below = l.peak.back();
            std::vector<float> above((below.size() + kBlock - 1) / kBlock, 0.0f);
            for (size_t i = 0; i < below.size(); ++i)
                above[i / kBlock] = std::max(above[i / kBlock], below[i]);
            l.peak.push_back(std::move(above));
        }
    }
}

namespace {

// [from, to) within a channel of `frames`, or false when that holds nothing.
bool clampRange(int from, int to, size_t frames, size_t& lo, size_t& hi) {
    lo = static_cast<size_t>(std::max(from, 0));
    hi = std::min(static_cast<size_t>(std::max(to, 0)), frames);
    return lo < hi;
}

}  // namespace

double detail::FileContents::peak(int channel, int from, int to) const {
    if (channel < 0 || levels.empty()) return 0.0;
    const size_t c = std::min(static_cast<size_t>(channel), levels.size() - 1);
    const std::vector<float>& x = audio.channel[c];
    size_t lo = 0, hi = 0;
    if (!clampRange(from, to, x.size(), lo, hi)) return 0.0;
    // The ends sample by sample to block edges, then each level's ends to
    // its own, and the top level's middle whole.
    float most = 0.0f;
    while (lo < hi && lo % kBlock) most = std::max(most, std::abs(x[lo++]));
    while (hi > lo && hi % kBlock) most = std::max(most, std::abs(x[--hi]));
    lo /= kBlock;
    hi /= kBlock;
    for (const std::vector<float>& p : levels[c].peak) {
        if (lo >= hi) break;
        if (&p == &levels[c].peak.back()) {
            for (size_t i = lo; i < hi; ++i) most = std::max(most, p[i]);
            break;
        }
        while (lo < hi && lo % kBlock) most = std::max(most, p[lo++]);
        while (hi > lo && hi % kBlock) most = std::max(most, p[--hi]);
        lo /= kBlock;
        hi /= kBlock;
    }
    return static_cast<double>(most);
}

double detail::FileContents::rms(int channel, int from, int to) const {
    if (channel < 0 || levels.empty() || to <= from) return 0.0;
    const size_t c = std::min(static_cast<size_t>(channel), levels.size() - 1);
    const std::vector<float>& x = audio.channel[c];
    size_t lo = 0, hi = 0;
    double sum = 0.0;
    if (clampRange(from, to, x.size(), lo, hi)) {
        auto square = [&](size_t i) {
            return static_cast<double>(x[i]) * static_cast<double>(x[i]);
        };
        while (lo < hi && lo % kBlock) sum += square(lo++);
        while (hi > lo && hi % kBlock) sum += square(--hi);
        sum += levels[c].energy[hi / kBlock] - levels[c].energy[lo / kBlock];
    }
    // Frames outside the file are silence, as sample() reads them.
    return std::sqrt(std::max(sum, 0.0) / (static_cast<double>(to) - from));
}

bool Convolver::loaded() const { return contents_ && contents_->convolution; }
int Convolver::length() const { return contents_ ? contents_->length : 0; }
double Convolver::process(int channel, double x) {
    if (!contents_ || !contents_->convolution || channel < 0 ||
        channel >= contents_->convolution->channels())
        return 0.0;
    if (!(x == x)) x = 0.0;
    contents_->convolution->process(channel, &x, 1);
    return x;
}

Random::Random() { new (storage_) dsp::Random(); }
void Random::seed(int seed) {
    as<dsp::Random>(storage_).reseed(static_cast<std::uint64_t>(static_cast<unsigned>(seed)));
}
double Random::next() { return as<dsp::Random>(storage_).uniform(); }
double Random::bipolar() { return as<dsp::Random>(storage_).bipolar(); }
double Random::gaussian() { return as<dsp::Random>(storage_).gaussian(); }
int Random::below(int n) {
    return n <= 0
               ? 0
               : static_cast<int>(as<dsp::Random>(storage_).below(static_cast<std::uint32_t>(n)));
}

Noise::Noise() { new (storage_) dsp::Noise(); }
void Noise::seed(int seed) {
    as<dsp::Noise>(storage_).reseed(static_cast<std::uint64_t>(static_cast<unsigned>(seed)));
}
double Noise::white() { return as<dsp::Noise>(storage_).white(); }
double Noise::pink() { return as<dsp::Noise>(storage_).pink(); }
double Noise::brown() { return as<dsp::Noise>(storage_).brown(); }

Oscillator::Oscillator() { new (storage_) dsp::Oscillator(); }
void Oscillator::setShape(Shape shape) {
    as<dsp::Oscillator>(storage_).setShape(
        static_cast<dsp::Oscillator::Shape>(static_cast<int>(shape)));
}
void Oscillator::setFrequency(double hz) {
    as<dsp::Oscillator>(storage_).setFrequency(hz, detail::rate());
}
void Oscillator::setPulseWidth(double width) { as<dsp::Oscillator>(storage_).setPulseWidth(width); }
void Oscillator::setPhase(double phase) { as<dsp::Oscillator>(storage_).setPhase(phase); }
double Oscillator::phase() const { return as<dsp::Oscillator>(storage_).phase(); }
double Oscillator::next() { return as<dsp::Oscillator>(storage_).tick(); }

// Constructors depend on nothing but the type: a new object is the same
// whatever is running (the code generator may copy a constructed one).
Adsr::Adsr() { new (storage_) dsp::Adsr(); }
void Adsr::set(double attackMs, double decayMs, double sustain, double releaseMs) {
    as<dsp::Adsr>(storage_).setTimes(attackMs, decayMs, sustain, releaseMs, detail::rate());
}
void Adsr::setCurves(double attack, double decayRelease) {
    as<dsp::Adsr>(storage_).setCurves(attack, decayRelease);
}
void Adsr::gate(bool on) { as<dsp::Adsr>(storage_).gate(on); }
void Adsr::retrigger() { as<dsp::Adsr>(storage_).retrigger(); }
void Adsr::kill() { as<dsp::Adsr>(storage_).kill(); }
double Adsr::next() { return as<dsp::Adsr>(storage_).tick(); }
double Adsr::level() const { return as<dsp::Adsr>(storage_).level(); }
bool Adsr::active() const { return as<dsp::Adsr>(storage_).active(); }

// --- the editor ------------------------------------------------------------------

namespace {
detail::EditorServices* services() {
    detail::Instance* in = detail::current();
    return in ? in->editor : nullptr;
}

// One printf conversion of `value`, checked: flags, width and precision
// within reason, and only the number conversions, so nothing a script
// writes reaches snprintf unexamined.
int formatOne(char* out, int room, const char* spec, int length, double value) {
    const char conversion = spec[length - 1];
    const bool integer = conversion == 'd' || conversion == 'i';
    const bool hex = conversion == 'x' || conversion == 'X';
    char safe[32];
    int n = 0;
    safe[n++] = '%';
    int i = 1;
    for (; i < length && std::strchr("-+ #0", spec[i]); ++i)
        if (n < 8 && !(spec[i] == '#' && integer) && !((spec[i] == '+' || spec[i] == ' ') && hex))
            safe[n++] = spec[i];
    int width = 0, precision = -1;
    while (i < length && spec[i] >= '0' && spec[i] <= '9')
        width = std::min(64, width * 10 + (spec[i++] - '0'));
    if (i < length && spec[i] == '.') {
        precision = 0;
        ++i;
        while (i < length && spec[i] >= '0' && spec[i] <= '9')
            precision = std::min(32, precision * 10 + (spec[i++] - '0'));
    }
    if (width > 0) n += std::snprintf(safe + n, sizeof(safe) - static_cast<size_t>(n), "%d", width);
    if (precision >= 0)
        n += std::snprintf(safe + n, sizeof(safe) - static_cast<size_t>(n), ".%d", precision);
    if (integer || hex) {
        safe[n++] = 'l';
        safe[n++] = 'l';
        safe[n++] = conversion;
        safe[n] = '\0';
        long long whole = 0;
        if (value >= 9.2e18)
            whole = LLONG_MAX;
        else if (value <= -9.2e18)
            whole = LLONG_MIN;
        else if (value == value)
            whole = static_cast<long long>(value);
        if (hex)
            return std::snprintf(out, static_cast<size_t>(room), safe,
                                 static_cast<unsigned long long>(whole));
        return std::snprintf(out, static_cast<size_t>(room), safe, whole);
    }
    safe[n++] = conversion;
    safe[n] = '\0';
    return std::snprintf(out, static_cast<size_t>(room), safe, value);
}

const char* formatValues(const char* pattern, const double* values, int count) {
    detail::EditorServices* ui = services();
    constexpr int kRoom = 256;
    char* out = ui ? ui->text(kRoom) : nullptr;
    if (!out) return "";
    int used = 0, next = 0;
    for (const char* p = pattern ? pattern : ""; *p && used < kRoom - 1;) {
        if (*p != '%') {
            out[used++] = *p++;
            continue;
        }
        if (p[1] == '%') {
            out[used++] = '%';
            p += 2;
            continue;
        }
        // The conversion's extent: flags, width, precision, length, letter.
        int length = 1;
        while (p[length] && std::strchr("-+ #0123456789.lh", p[length])) ++length;
        if (!p[length] || !std::strchr("dixXfFeEgG", p[length])) {
            out[used++] = *p++;  // not one of ours: as written
            continue;
        }
        ++length;
        if (next < count) {
            const int wrote = formatOne(out + used, kRoom - used, p, length, values[next++]);
            if (wrote > 0) used = std::min(kRoom - 1, used + wrote);
        }
        p += length;
    }
    out[used] = '\0';
    return out;
}
}  // namespace

const char* format(const char* pattern, double a) { return formatValues(pattern, &a, 1); }
const char* format(const char* pattern, double a, double b) {
    const double v[] = {a, b};
    return formatValues(pattern, v, 2);
}
const char* format(const char* pattern, double a, double b, double c) {
    const double v[] = {a, b, c};
    return formatValues(pattern, v, 3);
}
const char* format(const char* pattern, double a, double b, double c, double d) {
    const double v[] = {a, b, c, d};
    return formatValues(pattern, v, 4);
}

const char* Param::text() const {
    detail::EditorServices* ui = services();
    return ui ? ui->paramText(slot_, value_) : "";
}
const char* Param::name() const {
    detail::EditorServices* ui = services();
    return ui ? ui->paramName(slot_) : "";
}
double Param::normalized() const {
    detail::EditorServices* ui = services();
    return ui ? ui->paramNormalized(slot_, value_) : 0.0;
}
void Param::edit(double value) const {
    if (detail::EditorServices* ui = services())
        ui->editParam(slot_, detail::EditorServices::Edit::Value, value);
}
void Param::editNormalized(double position) const {
    if (detail::EditorServices* ui = services())
        ui->editParam(slot_, detail::EditorServices::Edit::Position, position);
}
void Instrument::playNote(int key, double velocity, bool on) const {
    if (detail::EditorServices* ui = services()) ui->playNote(key, velocity, on);
}
void Param::reset() const {
    if (detail::EditorServices* ui = services())
        ui->editParam(slot_, detail::EditorServices::Edit::Default, 0.0);
}

double hzToX(double hz, double lowHz, double highHz, double x, double w) {
    if (!(lowHz > 0.0) || !(highHz > lowHz) || !(hz > 0.0)) return x;
    return x + w * std::log(hz / lowHz) / std::log(highHz / lowHz);
}
double dbToY(double db, double lowDb, double highDb, double y, double h) {
    if (!(highDb > lowDb)) return y + h;
    return y + h * (highDb - db) / (highDb - lowDb);
}
double xToHz(double at, double lowHz, double highHz, double x, double w) {
    if (!(lowHz > 0.0) || !(highHz > lowHz) || !(w > 0.0)) return lowHz;
    return lowHz * std::pow(highHz / lowHz, (at - x) / w);
}
double yToDb(double at, double lowDb, double highDb, double y, double h) {
    if (!(highDb > lowDb) || !(h > 0.0)) return lowDb;
    return highDb - (highDb - lowDb) * (at - y) / h;
}

// Every method draws through the glue's painter; a Canvas without one (none
// is made outside draw()) draws nothing.
double Canvas::width() const { return painter_ ? painter_->width() : 0.0; }
double Canvas::height() const { return painter_ ? painter_->height() : 0.0; }
void Canvas::save() {
    if (painter_) painter_->save();
}
void Canvas::restore() {
    if (painter_) painter_->restore();
}
void Canvas::translate(double x, double y) {
    if (painter_) painter_->translate(x, y);
}
void Canvas::rotate(double radians) {
    if (painter_) painter_->rotate(radians);
}
void Canvas::scale(double x, double y) {
    if (painter_) painter_->scale(x, y);
}
void Canvas::fillStyle(int colour) {
    if (painter_) painter_->fillStyle(colour);
}
void Canvas::strokeStyle(int colour) {
    if (painter_) painter_->strokeStyle(colour);
}
void Canvas::globalAlpha(double alpha) {
    if (painter_) painter_->globalAlpha(alpha);
}
void Canvas::lineWidth(double width) {
    if (painter_) painter_->lineWidth(width);
}
void Canvas::lineCap(LineCap cap) {
    if (painter_) painter_->lineCap(cap);
}
void Canvas::lineJoin(LineJoin join) {
    if (painter_) painter_->lineJoin(join);
}
void Canvas::linearGradient(double x0, double y0, double x1, double y1, int from, int to) {
    if (painter_) painter_->linearGradient(x0, y0, x1, y1, from, to);
}
void Canvas::radialGradient(double x, double y, double innerRadius, double outerRadius, int from,
                            int to) {
    if (painter_) painter_->radialGradient(x, y, innerRadius, outerRadius, from, to);
}
void Canvas::fillRect(double x, double y, double w, double h) {
    if (painter_) painter_->fillRect(x, y, w, h);
}
void Canvas::strokeRect(double x, double y, double w, double h) {
    if (painter_) painter_->strokeRect(x, y, w, h);
}
void Canvas::clearRect(double x, double y, double w, double h) {
    if (painter_) painter_->clearRect(x, y, w, h);
}
void Canvas::beginPath() {
    if (painter_) painter_->beginPath();
}
void Canvas::closePath() {
    if (painter_) painter_->closePath();
}
void Canvas::moveTo(double x, double y) {
    if (painter_) painter_->moveTo(x, y);
}
void Canvas::lineTo(double x, double y) {
    if (painter_) painter_->lineTo(x, y);
}
void Canvas::bezierCurveTo(double c1x, double c1y, double c2x, double c2y, double x, double y) {
    if (painter_) painter_->bezierCurveTo(c1x, c1y, c2x, c2y, x, y);
}
void Canvas::quadraticCurveTo(double cx, double cy, double x, double y) {
    if (painter_) painter_->quadraticCurveTo(cx, cy, x, y);
}
void Canvas::arc(double x, double y, double radius, double startAngle, double endAngle) {
    if (painter_) painter_->arc(x, y, radius, startAngle, endAngle);
}
void Canvas::arcTo(double x1, double y1, double x2, double y2, double radius) {
    if (painter_) painter_->arcTo(x1, y1, x2, y2, radius);
}
void Canvas::rect(double x, double y, double w, double h) {
    if (painter_) painter_->rect(x, y, w, h);
}
void Canvas::roundRect(double x, double y, double w, double h, double radius) {
    if (painter_) painter_->roundRect(x, y, w, h, radius);
}
void Canvas::ellipse(double x, double y, double radiusX, double radiusY) {
    if (painter_) painter_->ellipse(x, y, radiusX, radiusY);
}
void Canvas::circle(double x, double y, double radius) {
    if (painter_) painter_->circle(x, y, radius);
}
void Canvas::fill() {
    if (painter_) painter_->fill();
}
void Canvas::stroke() {
    if (painter_) painter_->stroke();
}
void Canvas::clip(double x, double y, double w, double h) {
    if (painter_) painter_->clip(x, y, w, h);
}
void Canvas::font(double size) {
    if (painter_) painter_->font(detail::Painter::Face::Regular, size);
}
void Canvas::boldFont(double size) {
    if (painter_) painter_->font(detail::Painter::Face::Bold, size);
}
void Canvas::monoFont(double size) {
    if (painter_) painter_->font(detail::Painter::Face::Mono, size);
}
void Canvas::serifFont(double size) {
    if (painter_) painter_->font(detail::Painter::Face::Serif, size);
}
void Canvas::serifItalicFont(double size) {
    if (painter_) painter_->font(detail::Painter::Face::SerifItalic, size);
}
void Canvas::textAlign(TextAlign align) {
    if (painter_) painter_->textAlign(align);
}
void Canvas::textBaseline(TextBaseline baseline) {
    if (painter_) painter_->textBaseline(baseline);
}
void Canvas::fillText(const char* text, double x, double y) {
    if (painter_ && text) painter_->fillText(text, x, y);
}
double Canvas::measureText(const char* text) const {
    return painter_ && text ? painter_->measureText(text) : 0.0;
}
void Canvas::panel(double x, double y, double w, double h, const char* title) {
    if (painter_) painter_->panel(x, y, w, h, title ? title : "");
}
void Canvas::well(double x, double y, double w, double h) {
    if (painter_) painter_->well(x, y, w, h);
}
void Canvas::meter(double levelDb, double x, double y, double w, double h) {
    if (painter_) painter_->meter(levelDb, x, y, w, h);
}
void Canvas::frequencyGrid(double x, double y, double w, double h, double lowHz, double highHz) {
    if (painter_) painter_->frequencyGrid(x, y, w, h, lowHz, highHz);
}
void Canvas::levelGrid(double x, double y, double w, double h, double lowDb, double highDb,
                       double stepDb) {
    if (painter_) painter_->levelGrid(x, y, w, h, lowDb, highDb, stepDb);
}

namespace {
int slotOf(const Param& p) { return detail::ParamBinding::slot(p); }
}  // namespace

void Editor::size(double width, double height) {
    if (builder_) builder_->size(width, height);
}
void Editor::knob(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::EditorBuilder::Control::Knob, slotOf(p), x, y, w, h);
}
void Editor::slider(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::EditorBuilder::Control::Slider, slotOf(p), x, y, w, h);
}
void Editor::toggle(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::EditorBuilder::Control::Toggle, slotOf(p), x, y, w, h);
}
void Editor::choice(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::EditorBuilder::Control::Choice, slotOf(p), x, y, w, h);
}
void Editor::readout(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::EditorBuilder::Control::Readout, slotOf(p), x, y, w, h);
}
void Editor::xyPad(const Param& across, const Param& up, double x, double y, double w, double h) {
    if (builder_) builder_->xyPad(slotOf(across), slotOf(up), x, y, w, h);
}
void Editor::envelope(const Param& attack, const Param& decay, const Param& sustain,
                      const Param& release, double x, double y, double w, double h) {
    if (builder_)
        builder_->envelope(slotOf(attack), slotOf(decay), slotOf(sustain), slotOf(release), x, y, w,
                           h);
}
void Editor::file(const AudioFile& f, double x, double y, double w, double h) {
    if (builder_) builder_->file(&f, x, y, w, h);
}
void Editor::file(const Convolver& c, double x, double y, double w, double h) {
    if (builder_) builder_->file(&c, x, y, w, h);
}
void Editor::keyboard(int lowKey, int highKey, double x, double y, double w, double h) {
    if (builder_) builder_->keyboard(lowKey, highKey, x, y, w, h);
}
void Editor::meters(double x, double y, double w, double h) {
    if (builder_) builder_->meters(x, y, w, h);
}

void Editor::routes(double x, double y, double w, double h) {
    if (builder_) builder_->routes(x, y, w, h);
}

// --- modules ------------------------------------------------------------------------

Transport Module::transport() const { return instanceOf(instance_).transport; }
int Module::seed() const { return instanceOf(instance_).seed; }

void Panel::size(double width, double height) {
    if (builder_) builder_->size(width, height);
}
void Panel::knob(const Param& p, double x, double y) { knob(p, x, y, 36.0); }
void Panel::knob(const Param& p, double x, double y, double size) {
    if (builder_)
        builder_->control(detail::PanelBuilder::Control::Knob, slotOf(p), x, y, size, size);
}
void Panel::slider(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::PanelBuilder::Control::Slider, slotOf(p), x, y, w, h);
}
void Panel::toggle(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::PanelBuilder::Control::Toggle, slotOf(p), x, y, w, h);
}
void Panel::choice(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::PanelBuilder::Control::Choice, slotOf(p), x, y, w, h);
}
void Panel::readout(const Param& p, double x, double y, double w, double h) {
    if (builder_) builder_->control(detail::PanelBuilder::Control::Readout, slotOf(p), x, y, w, h);
}
void Panel::input(const Input& in, double x, double y) {
    if (builder_) builder_->jack(false, detail::PortBinding::slot(in), x, y);
}
void Panel::output(const Output& out, double x, double y) {
    if (builder_) builder_->jack(true, detail::PortBinding::slot(out), x, y);
}
void Panel::display(double x, double y, double w, double h) {
    if (builder_) builder_->display(x, y, w, h);
}

}  // namespace supple::script
