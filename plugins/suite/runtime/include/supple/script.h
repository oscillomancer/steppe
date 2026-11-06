// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The DSP script runtime: everything a script can name.
//
// A script is one C++ file that starts with `#include <supple/script.h>`.
// The script compiler accepts a checked subset of C++20 and JIT-compiles it;
// the same file also compiles as ordinary C++ against this header (a source
// export), and means the same thing. The language reference is
// skill/guides/dsp-language.md.
//
// This header deliberately includes no standard math header: every function
// here is the runtime's own, so unqualified names (`sin`, `min`, `Biquad`)
// mean the runtime's in a script whether it is read by the script compiler,
// by clangd, or by a C++ compiler in an export. Built-in objects are opaque
// storage whose methods are compiled once, in the runtime library; the JIT
// calls the very same functions, so both paths compute the same bits.
#pragma once

#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

namespace supple::script {

// --- numbers -------------------------------------------------------------------

// Math works in double: one overload each, so a call never depends on which
// overload C++ would pick (sin(1) and min(x, 0.5) with a float x would be
// ambiguous otherwise). A float argument is promoted, and the result narrows
// back when assigned to a float. min, max, clamp and abs also take ints.

inline constexpr double pi = 3.14159265358979323846;
inline constexpr double twoPi = 6.28318530717958647692;

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);  // half away from zero
double fmod(double x, double y);
double hypot(double x, double y);

inline double abs(double x) { return x < 0.0 ? -x : x; }
inline int abs(int x) { return x < 0 ? -x : x; }
inline double min(double a, double b) { return b < a ? b : a; }
inline int min(int a, int b) { return b < a ? b : a; }
inline double max(double a, double b) { return a < b ? b : a; }
inline int max(int a, int b) { return a < b ? b : a; }
inline double clamp(double x, double lo, double hi) { return x < lo ? lo : (hi < x ? hi : x); }
inline int clamp(int x, int lo, int hi) { return x < lo ? lo : (hi < x ? hi : x); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
// -1, 0 or +1.
inline double sign(double x) { return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0); }
// True unless x is NaN or infinite.
inline bool isFinite(double x) { return x - x == 0.0; }

// Decibels and gain, pitch and frequency.
double dbToGain(double db);    // 0 dB is exactly 1
double gainToDb(double gain);  // floored at -120 dB (0 gives -120)
double mtof(double note);      // MIDI note to Hz: 69 is 440 Hz
double ftom(double hz);        // Hz to MIDI note (fractional)

// A downward compressor's curve: the gain change in dB (0 or less) for a
// level in dB. Nothing below the threshold; above it, `ratio` dB in for each
// dB out (1 changes nothing, 100 limits), with a soft knee `kneeDb` wide
// (0: a hard knee) centred on the threshold.
double compressorGainDb(double levelDb, double thresholdDb, double ratio, double kneeDb);

// Float to int, defined for every input: truncated toward zero, rounded down,
// rounded up or rounded half away from zero; saturated to the int range;
// NaN gives 0. C++'s own conversion is undefined out of range, so scripts
// convert only through these.
int toInt(double x);
int floorToInt(double x);
int ceilToInt(double x);
int roundToInt(double x);
// Integer division and remainder for any divisor: 0 when b is 0, and
// INT_MIN / -1 is INT_MIN. Scripts divide by a variable only through these.
int idiv(int a, int b);
int imod(int a, int b);
// i wrapped into 0..n-1 (for n > 0; 0 otherwise), negative i included.
int wrap(int i, int n);
// Defined 32-bit operations. Shift counts outside 0..31 give 0. shr is a
// logical (zero-filling) right shift; bit gives 0 or 1. popcount counts ones.
int shl(int x, int n);
int shr(int x, int n);
int bit(int x, int n);
int popcount(int x);

// --- containers ----------------------------------------------------------------

namespace detail {
struct Layout;
// Native bounds checks have no source line; the JIT supplies its own.
void outOfRange(int line);
// Reports a Shared or SharedArray member to the plugin under construction
// (a source export's glue listens; nothing else does).
void declareShared(void* member, int bytes);
}  // namespace detail

// A fixed-size array. Indexing is checked: an index outside 0..N-1 refers to
// a spare element instead, reset to T{} on each invalid access when T is
// trivially copyable; resource-bearing elements keep their spare's state.
// The script's status reports the line. It starts zeroed, or
// from a list: Array<float, 4> taps = {0.1f, 0.2f, 0.3f, 0.4f};
template <class T, int N>
struct Array {
    static_assert(N > 0, "an Array needs at least one element");

    T& operator[](int i) {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(N)) return items_[i];
        detail::outOfRange(0);
        // Construct in place: a large aggregate needs no audio-stack temporary.
        if constexpr (std::is_trivially_copyable_v<T>) ::new (static_cast<void*>(&spare_)) T();
        return spare_;
    }
    const T& operator[](int i) const { return const_cast<Array&>(*this)[i]; }
    static constexpr int size() { return N; }
    // Every element to `value`.
    void fill(const T& value) {
        for (int i = 0; i < N; ++i) items_[i] = value;
    }
    // For range-based for: for (auto& v : voices).
    T* begin() { return items_; }
    T* end() { return items_ + N; }
    const T* begin() const { return items_; }
    const T* end() const { return items_ + N; }

    // Public so that an Array is an aggregate and takes a list; a script
    // reaches its elements only through [] (the compiler checks).
    T items_[N] = {};
    T spare_ = {};
};

// A run-time sized buffer of numbers: resize() it in prepare() (it allocates),
// then index it like an Array. New elements are zero.
template <class T>
class Buffer {
    static_assert(std::is_arithmetic_v<T>, "a Buffer holds numbers (double, float, int)");

  public:
    Buffer() = default;
    Buffer(const Buffer& other) { *this = other; }
    Buffer& operator=(const Buffer& other);
    ~Buffer();

    void resize(int size);  // prepare() only
    int size() const { return size_; }
    T& operator[](int i) {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(size_)) return items_[i];
        detail::outOfRange(0);
        spare_ = T();
        return spare_;
    }
    const T& operator[](int i) const { return const_cast<Buffer&>(*this)[i]; }
    void fill(T value) {
        for (int i = 0; i < size_; ++i) items_[i] = value;
    }
    T* begin() { return items_; }
    T* end() { return items_ + size_; }
    const T* begin() const { return items_; }
    const T* end() const { return items_ + size_; }

  private:
    friend struct detail::Layout;
    T* items_ = nullptr;
    int size_ = 0;
    T spare_ = T();
};

// --- parameters ----------------------------------------------------------------

// The physical unit of a parameter. The patch carries values in it, hosts
// show it, and agents set it.
enum class Unit {
    None,
    Decibels,
    Hertz,
    Milliseconds,
    Percent,
    Ratio,
    Semitones,
    Cents,
    Octaves,
    Degrees,
    Bpm,
    Samples,
    Meters
};

// How a parameter's value maps onto a knob's travel and a host's 0..1
// automation: Linear, Log (equal ratios per unit of travel), or Pow (exponent
// k, favouring the low end when k > 1).
enum class Curve { Linear, Log, Pow };

class Param;

// A parameter's description, made by the factories below and refined by
// chained modifiers: decibels("Gain", -24, 24, 0).smoothed(20).help("...").
struct ParamSpec {
    enum class Kind { Continuous, Integer, Choice, Toggle };
    Kind kind = Kind::Continuous;
    const char* name = "";
    double min = 0.0, max = 1.0, def = 0.0;
    Unit unit = Unit::None;
    Curve curve = Curve::Linear;
    double curveK = 1.0;
    std::initializer_list<const char*> choices = {};
    const char* group = "";
    const char* description = "";
    const char* key = "";      // the patch key; the member's name when empty
    unsigned int id = 0;       // the CLAP id; a hash of the key when 0
    double smoothingMs = 0.0;  // the framework glides it; read it with at(i)
    bool automatable = true, modulatable = false, offAtMinimum = false, output = false;
    bool polyModulatable = false;  // an instrument's, modulated per voice: forVoice(v)
    bool bypass = false, hidden = false;
    bool restart = false;  // a change restarts the plugin: prepare() runs again
    std::initializer_list<const char*> roles = {};       // "level", "time", "brightness" ...
    std::initializer_list<const char*> choiceKeys = {};  // a choice's patch keys (.keys())
    // When it applies (.activeWhen(), .activeUnless(), .activeAbove(),
    // .activeBelow()), each condition on a parameter declared above.
    struct Condition {
        enum class Kind { When, Unless, Above, Below };
        const Param* param = nullptr;
        std::initializer_list<const char*> options = {};  // a choice's labels or keys
        Kind kind = Kind::When;
        double bound = 0.0;  // Above's or Below's, in the parameter's physical unit
    };
    static constexpr int kConditions = 4;
    Condition conditions[kConditions] = {};
    int conditionCount = 0;
    // How far routes may move it (.routeRange()), physical; its own range
    // when routeMin > routeMax.
    double routeMin = 1.0, routeMax = 0.0;

    // In a group, which the automatic editor and hosts show as a section.
    ParamSpec in(const char* g) const {
        ParamSpec p = *this;
        p.group = g;
        return p;
    }
    // What it does, in one sentence: the editor's tooltip and the schema's description.
    ParamSpec help(const char* text) const {
        ParamSpec p = *this;
        p.description = text;
        return p;
    }
    // The framework glides it over `ms`, per sample; read param.at(i).
    ParamSpec smoothed(double ms) const {
        ParamSpec p = *this;
        p.smoothingMs = ms;
        return p;
    }
    // The host may modulate it, and so may routes from the plugin's global
    // sources (Source): read it with routed(i).
    ParamSpec modulated() const {
        ParamSpec p = *this;
        p.modulatable = true;
        return p;
    }
    // An instrument's: the host may modulate it per note (CLAP polyphonic
    // modulation), and routes from any source, a voice's own included; each
    // voice reads its own value with forVoice(v).
    ParamSpec polyModulated() const {
        ParamSpec p = *this;
        p.modulatable = true;
        p.polyModulatable = true;
        return p;
    }
    // How far routes may take it, physical, beyond its own range: a fine
    // tuning of +-100 ct that a pitch envelope sweeps by octaves,
    // cents("Fine", -100, 100, 0).polyModulated().routeRange(-4800, 4800).
    ParamSpec routeRange(double lowest, double highest) const {
        ParamSpec p = *this;
        p.routeMin = lowest;
        p.routeMax = highest;
        return p;
    }
    // Its minimum means off ("-inf dB" for a level).
    ParamSpec offAtMin() const {
        ParamSpec p = *this;
        p.offAtMinimum = true;
        return p;
    }
    // A meter the plugin reports with set(): read-only, never saved.
    ParamSpec meter() const {
        ParamSpec p = *this;
        p.output = true;
        p.automatable = false;
        return p;
    }
    ParamSpec notAutomatable() const {
        ParamSpec p = *this;
        p.automatable = false;
        return p;
    }
    // Changing it restarts the plugin (the host stops it and starts it
    // again), so prepare() runs again with the new value and may report
    // another latency with setLatency(): a lookahead. Not automatable.
    ParamSpec restarts() const {
        ParamSpec p = *this;
        p.restart = true;
        p.automatable = false;
        return p;
    }
    ParamSpec withKey(const char* k) const {
        ParamSpec p = *this;
        p.key = k;
        return p;
    }
    ParamSpec withId(unsigned int i) const {
        ParamSpec p = *this;
        p.id = i;
        return p;
    }
    ParamSpec role(std::initializer_list<const char*> r) const {
        ParamSpec p = *this;
        p.roles = r;
        return p;
    }
    // A choice's patch keys, one for each option, where its labels would make
    // poor ones: choice("Slope", {"12 dB/oct", "24 dB/oct"}, 0).keys({"db12", "db24"}).
    // Without it each key is its label in snake_case ("Low Pass": low_pass).
    ParamSpec keys(std::initializer_list<const char*> k) const {
        ParamSpec p = *this;
        p.choiceKeys = k;
        return p;
    }
    ParamSpec curved(Curve c, double k = 1.0) const {
        ParamSpec p = *this;
        p.curve = c;
        p.curveK = k;
        return p;
    }
    // It applies only while the toggle `on` is on, or while the choice
    // `mode` is one of `options` (labels or keys); activeUnless() the other
    // way about (activeAbove() and activeBelow(), below, for a number). Each
    // adds a condition and all must hold, with the parameter each names
    // applying itself. Hosts and agents see which parameters apply, and the
    // editor dims the rest; the script still reads every value.
    ParamSpec activeWhen(const Param& on) const { return condition({&on}); }
    ParamSpec activeWhen(const Param& mode, std::initializer_list<const char*> options) const {
        return condition({&mode, options});
    }
    ParamSpec activeUnless(const Param& on) const {
        return condition({&on, {}, Condition::Kind::Unless});
    }
    ParamSpec activeUnless(const Param& mode, std::initializer_list<const char*> options) const {
        return condition({&mode, options, Condition::Kind::Unless});
    }
    // It applies only while the number `amount` (continuous or integer) is
    // above `value`, or below it: in its physical unit, as a patch says it
    // (a percent as 30). .activeAbove(voices, 1) for what unison needs.
    ParamSpec activeAbove(const Param& amount, double value) const {
        return condition({&amount, {}, Condition::Kind::Above, value});
    }
    ParamSpec activeBelow(const Param& amount, double value) const {
        return condition({&amount, {}, Condition::Kind::Below, value});
    }

  private:
    ParamSpec condition(const Condition& c) const {
        ParamSpec s = *this;
        // One too many leaves a condition on no parameter, which the
        // plugin's definition refuses (the script compiler says so first).
        if (s.conditionCount < kConditions)
            s.conditions[s.conditionCount] = c;
        else
            s.conditions[kConditions - 1] = {};
        if (s.conditionCount < kConditions) ++s.conditionCount;
        return s;
    }
};

inline ParamSpec continuous(const char* name, double min, double max, double def,
                            Unit unit = Unit::None) {
    ParamSpec p;
    p.name = name;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    return p;
}
// A level or gain in dB, linear travel.
inline ParamSpec decibels(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Decibels);
}
// A frequency in Hz, logarithmic travel.
inline ParamSpec frequency(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Hertz).curved(Curve::Log);
}
// A time in ms: logarithmic travel when min > 0, otherwise a cubic curve.
inline ParamSpec milliseconds(const char* name, double min, double max, double def) {
    ParamSpec p = continuous(name, min, max, def, Unit::Milliseconds);
    return min > 0.0 ? p.curved(Curve::Log) : p.curved(Curve::Pow, 3.0);
}
// A percentage given in percent (0..100 by default); the script reads it as a
// fraction, 0..1.
inline ParamSpec percent(const char* name, double def, double min = 0.0, double max = 100.0) {
    return continuous(name, min, max, def, Unit::Percent);
}
// A ratio such as a compressor's 4:1, logarithmic travel.
inline ParamSpec ratio(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Ratio).curved(Curve::Log);
}
inline ParamSpec semitones(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Semitones);
}
inline ParamSpec cents(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Cents);
}
inline ParamSpec octaves(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Octaves);
}
inline ParamSpec degrees(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Degrees);
}
// A whole number.
inline ParamSpec integer(const char* name, int min, int max, int def) {
    ParamSpec p = continuous(name, min, max, def);
    p.kind = ParamSpec::Kind::Integer;
    return p;
}
// One of several options, read as an index: choice("Mode", {"Lowpass", "Highpass"}, 0).
inline ParamSpec choice(const char* name, std::initializer_list<const char*> options, int def) {
    ParamSpec p = continuous(name, 0.0, static_cast<double>(options.size()) - 1.0, def);
    p.kind = ParamSpec::Kind::Choice;
    p.choices = options;
    return p;
}
// On or off.
inline ParamSpec toggle(const char* name, bool def) {
    ParamSpec p = continuous(name, 0.0, 1.0, def ? 1.0 : 0.0);
    p.kind = ParamSpec::Kind::Toggle;
    return p;
}

namespace detail {
struct ParamBinding;
struct Instance;
class Painter;
class EditorBuilder;
class PatchBuilder;
}  // namespace detail

// A parameter: a member of the plugin, read as a number in its unit (a
// percent as 0..1, a choice as its index, a toggle as 0 or 1). Its value is
// the host's at the start of the current block (blocks are split at every
// change, so automation is sample-accurate). A smoothed() one also glides:
// at(i) is its value at frame i of the block.
class Param {
  public:
    Param(const ParamSpec& spec);  // registers it with the plugin under construction
    Param(const Param& other);
    Param& operator=(const Param& other);

    operator double() const { return value_; }
    double value() const { return value_; }
    // The value at frame i of the current block: glided for a smoothed()
    // parameter, the block's value otherwise. In a module, with the cables
    // into it of channel 0 (ADR-0062).
    double at(int i) const { return at(i, 0); }
    // A module's: the value at frame i with the cables into it of channel c
    // (a mono cable reaches every channel; past a poly cable's channels, the
    // knob alone). at(i) for a plugin's.
    double at(int i, int c) const {
        if (modChannels_ > 0) {
            const int k = modChannels_ == 1 ? 0 : c;
            if (static_cast<unsigned>(k) < static_cast<unsigned>(modChannels_) &&
                static_cast<unsigned>(i) < static_cast<unsigned>(glideFrames_))
                return mod_[k * modStride_ + i];
        }
        if (glide_ && static_cast<unsigned>(i) < static_cast<unsigned>(glideFrames_))
            return glide_[i];
        return value_;
    }
    // A choice's index, a toggle's state, a level in dB as a linear gain
    // (exactly 0 at the minimum of an offAtMin() one).
    int index() const { return roundToInt(value_); }
    bool on() const { return value_ > 0.5; }
    double gain() const;
    // In update(): whether this parameter changed since the last update.
    bool changed() const { return changed_; }
    // An instrument's polyModulated() parameter for voice v: the block's
    // value with the host's modulation of that voice's note and the routes
    // to it, from the sources' values now, kept in range. value() for any
    // other.
    double forVoice(int v) const;
    // A modulated() parameter at frame i: at(i) with the routes from global
    // sources, from their values now, kept in range. at(i) for any other.
    double routed(int i) const;
    // A meter() parameter: what the plugin reports.
    void set(double value) { reported_ = value; }
    // In editor code: the value as the host shows it ("-3.0 dB",
    // "1.20 kHz"), and the parameter's name. Each lasts until the hook
    // returns.
    const char* text() const;
    const char* name() const;
    // In editor code: where the value sits from 0 (the minimum) to 1 (the
    // maximum), as its knob shows it (a frequency's on a log scale).
    double normalized() const;
    // In pointer(): change the parameter as a user turning its knob would,
    // to a value in its units (kept in its range), to a position from 0 to
    // 1 (as normalized()), or back to its default. The host records it and
    // the audio code hears it; value() has it at once. One press's edits,
    // through its drags to its release, are one gesture, however many
    // parameters they change; a wheel's, one per burst. Edits on a Move or
    // a Leave do nothing.
    void edit(double value) const;
    void editNormalized(double position) const;
    void reset() const;

  private:
    friend struct detail::ParamBinding;
    friend struct detail::Layout;
    double value_ = 0.0;
    const double* glide_ = nullptr;
    int glideFrames_ = 0;
    // A module's cable modulation (the rack runtime's): the moved value of
    // channel k at frame i is mod_[k * modStride_ + i], for k < modChannels_
    // (0: no cables; 1: one value for every channel). Frames as glide's.
    const double* mod_ = nullptr;
    int modStride_ = 0;
    int modChannels_ = 0;
    bool changed_ = true;
    bool offAtMinimum_ = false;
    double min_ = 0.0;
    double reported_ = 0.0;
    int slot_ = -1;
};

// --- files ---------------------------------------------------------------------

// A file member as the script declares it: audioFile("Sample") or
// impulseResponse("Room"), with .help() and .withKey() as a parameter has,
// and .withDefault("factory:pad.flac"): the file a new instance starts with,
// one the plugin carries inside it (its product's PRODUCT_ASSETS). Such a
// factory file plays in the plugin built from the script, in any host; the
// script player has none, so there the member starts with no file.
struct FileSpec {
    enum class Kind { Audio, Impulse };
    Kind kind = Kind::Audio;
    const char* name = "";
    const char* description = "";
    const char* key = "";  // the patch key; the member's name when empty
    const char* def = "";  // a factory file, "factory:<name>"; none when empty

    FileSpec help(const char* text) const {
        FileSpec f = *this;
        f.description = text;
        return f;
    }
    FileSpec withKey(const char* k) const {
        FileSpec f = *this;
        f.key = k;
        return f;
    }
    FileSpec withDefault(const char* factoryFile) const {
        FileSpec f = *this;
        f.def = factoryFile;
        return f;
    }
};
// A file of audio -- WAV, AIFF or FLAC -- for an AudioFile: a sample, a loop,
// a wavetable.
inline FileSpec audioFile(const char* name) {
    FileSpec f;
    f.name = name;
    return f;
}
// An impulse response -- WAV, AIFF or FLAC -- for a Convolver: a room, a
// speaker cabinet.
inline FileSpec impulseResponse(const char* name) {
    FileSpec f;
    f.kind = FileSpec::Kind::Impulse;
    f.name = name;
    return f;
}

namespace detail {
struct FileContents;
struct FileBinding;
}  // namespace detail

// A file of audio: a member of the plugin struct, AudioFile sample =
// audioFile("Sample"). Its path is a property of the plugin, saved with it
// and set by the user or a patch ("properties": {"sample": "/path.wav"}).
// The file is loaded off the audio thread and resampled to the session's
// rate; it appears at the start of a block, and loaded() is false until
// then, or when there is none.
class AudioFile {
  public:
    AudioFile(const FileSpec& spec);  // registers it with the plugin under construction
    bool loaded() const;
    int channels() const;  // 0 when nothing is loaded
    int frames() const;
    // Sample `frame` of `channel`, 0 outside the file. A channel past the
    // file's last reads its last, so a mono file serves every channel.
    double sample(int channel, int frame) const;
    // Between samples, interpolated (cubic): for playing at another speed.
    double read(int channel, double frame) const;
    // The largest |sample| of `channel` in frames [from, to), and their root
    // mean square (frames outside the file count as silence); 0 for none.
    // Measured when the file loads, so a range of any length -- the whole
    // file, a waveform's column, the hop of an onset detector -- costs about
    // as much as a few hundred sample() calls.
    double peak(int channel, int from, int to) const;
    double rms(int channel, int from, int to) const;

  private:
    friend struct detail::FileBinding;
    detail::FileContents* contents_ = nullptr;
};

// Convolution with an impulse response: a member of the plugin struct,
// Convolver room = impulseResponse("Room"), whose path is a property as an
// AudioFile's is. Loaded and prepared off the audio thread (its first 20
// seconds); no latency. Until one is loaded, process() gives 0.
class Convolver {
  public:
    Convolver(const FileSpec& spec);  // registers it with the plugin under construction
    bool loaded() const;
    int length() const;  // the response's frames
    // The next output of channel c for input x. Each channel uses the
    // response's channel of the same number, or its last.
    double process(int channel, double x);

  private:
    friend struct detail::FileBinding;
    detail::FileContents* contents_ = nullptr;
};

// --- modulation ------------------------------------------------------------------

// A modulation source as the script declares it: source("Macro 1") for one
// value the whole plugin shares, voiceSource("LFO 1") for an instrument's
// value per voice. Bipolar (-1 to 1) unless .unipolar() (0 to 1); .help()
// and .withKey() as a parameter has.
struct SourceSpec {
    const char* name = "";
    const char* description = "";
    const char* key = "";  // the patch key; the member's name when empty
    bool perVoice = false;
    bool bipolar = true;

    SourceSpec help(const char* text) const {
        SourceSpec s = *this;
        s.description = text;
        return s;
    }
    SourceSpec withKey(const char* k) const {
        SourceSpec s = *this;
        s.key = k;
        return s;
    }
    SourceSpec unipolar() const {
        SourceSpec s = *this;
        s.bipolar = false;
        return s;
    }
};
inline SourceSpec source(const char* name) {
    SourceSpec s;
    s.name = name;
    return s;
}
inline SourceSpec voiceSource(const char* name) {
    SourceSpec s = source(name);
    s.perVoice = true;
    return s;
}

// A modulation source: a member of the plugin struct, Source lfo =
// voiceSource("LFO 1"), whose value the script computes and set()s. The
// patch's routes connect sources to parameters, each with an amount in the
// parameter's unit (a frequency's in octaves) at the source's full value:
// "lfo>cutoff": {"amount": 1} swings the cutoff an octave each way. A
// parameter that routes may move is modulated() (global sources) or, an
// instrument's, polyModulated() (any source); the script reads it with
// routed(i) or forVoice(v), after setting the sources it hears. Values are
// kept to -1..1, or 0..1 for a unipolar() source. An instrument also has
// these, the runtime's own: velocity, key (-1 at key 0, 0 at middle C, 1 at
// key 120), random (new for each note) and the note's pressure and slide
// (CLAP brightness) per voice; modwheel, breath, expression (CC 11),
// aftertouch and pitchbend for all.
class Source {
  public:
    static constexpr int kVoices = 64;  // the most voices an instrument has

    Source(const SourceSpec& spec);  // registers it with the plugin under construction
    // A per-voice source: every voice's value, or voice v's. A global
    // source has one value, which both set.
    void set(double value) {
        const double x = kept(value);
        if (!perVoice_)
            values_[0] = x;
        else
            for (int v = 0; v < kVoices; ++v) values_[v] = x;
    }
    void set(int v, double value) {
        if (!perVoice_)
            set(value);
        else if (static_cast<unsigned>(v) < static_cast<unsigned>(kVoices))
            values_[v] = kept(value);
        else
            detail::outOfRange(0);
    }
    // Voice v's value (a global source's one).
    double get(int v) const {
        if (!perVoice_) return values_[0];
        if (static_cast<unsigned>(v) < static_cast<unsigned>(kVoices)) return values_[v];
        detail::outOfRange(0);
        return 0.0;
    }

  private:
    friend struct detail::Layout;
    double kept(double x) const { return x < low_ ? low_ : (x > 1.0 ? 1.0 : (x == x ? x : 0.0)); }
    double values_[kVoices] = {};
    double low_ = -1.0;
    bool perVoice_ = false;
};

// --- the plugin ----------------------------------------------------------------

// The plugin's identity and buses, `static constexpr Info info = {...}` in the
// plugin struct. The ID never changes once the plugin is shared.
struct Info {
    const char* name = "";
    const char* id = "";  // reverse-DNS: "com.example.tilt"
    const char* vendor = "";
    const char* version = "1.0.0";
    const char* description = "";
    const char* url = "";
    // CLAP features after the main category (which the kind gives): what the
    // plugin is, first ("reverb", "distortion"), as hosts sort and label it.
    const char* features[4] = {};
    // Main bus widths offered, the first the default: {2, 1} is stereo, then
    // mono. 0 entries means stereo and mono for an effect, stereo otherwise.
    int channels[4] = {0, 0, 0, 0};
    bool sidechain = false;       // a sidechain input as wide as the main bus
    const char* outputs[8] = {};  // names of extra outputs (an instrument's "Kick", "Snare")
    int voices = 0;               // an instrument's voice pool, for hosts that ask
    int latency = 0;              // samples, reported to the host
    int tail = 0;                 // samples of output after the input stops; -1 forever
};

// Plain read-only views of one channel of audio.
class InputChannel {
  public:
    double operator[](int i) const {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(frames_)) return data_[i];
        detail::outOfRange(0);
        return 0.0;
    }
    int size() const { return frames_; }

    const double* data_ = nullptr;
    int frames_ = 0;
};

class Channel {
  public:
    double& operator[](int i) const {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(frames_)) return data_[i];
        detail::outOfRange(0);
        *spare_ = 0.0;
        return *spare_;
    }
    int size() const { return frames_; }

    double* data_ = nullptr;
    int frames_ = 0;
    double* spare_ = nullptr;
};

// The host's transport at the start of the block.
struct Transport {
    bool playing = false;
    double tempo = 120.0;  // beats per minute (120 when the host sent none)
    double beats = 0.0;    // song position in quarter notes
    double barStart = 0.0;
    int timeSigNumerator = 4, timeSigDenominator = 4;
};

// One block of audio, processed in place: io[channel][frame]. For an effect
// it holds the input on entry and must hold the output on return; for an
// instrument it arrives silent. A block may be a single frame long.
class Block {
  public:
    int channels = 0;
    int frames = 0;
    Transport transport;
    bool sidechainConnected = false;

    Channel operator[](int c) const {
        if (static_cast<unsigned>(c) < static_cast<unsigned>(channels))
            return Channel{data_[c], frames, spare_};
        detail::outOfRange(0);
        return Channel{spare_, 0, spare_};
    }
    // The sidechain (Info::sidechain): the key input, silent when unconnected.
    InputChannel sidechain(int c) const {
        if (static_cast<unsigned>(c) < static_cast<unsigned>(sidechainChannels_))
            return InputChannel{sidechain_[c], frames};
        detail::outOfRange(0);
        return InputChannel{nullptr, 0};
    }
    // An extra output (Info::outputs), silent on entry.
    Channel output(int bus, int c) const {
        if (static_cast<unsigned>(bus) < static_cast<unsigned>(outputs_) &&
            static_cast<unsigned>(c) < static_cast<unsigned>(channels))
            return Channel{aux_[bus][c], frames, spare_};
        detail::outOfRange(0);
        return Channel{spare_, 0, spare_};
    }

    double* const* data_ = nullptr;
    const double* const* sidechain_ = nullptr;
    int sidechainChannels_ = 0;
    double* const* const* aux_ = nullptr;
    int outputs_ = 0;
    double* spare_ = nullptr;
};

// What every plugin struct derives from, through Effect, Instrument or
// NoteEffect.
class Plugin {
  public:
    double sampleRate() const;
    int maxBlockSize() const;
    // The main bus width chosen by the host.
    int channels() const;
    // Whether the host renders offline (a bounce).
    bool offline() const;
    // In prepare(): the delay the plugin adds, in samples, when it depends
    // on the sample rate (an oversampler's, a lookahead); Info's .latency
    // otherwise. The host compensates it.
    void setLatency(int samples);
    // In prepare(), update() or process(): how long the output goes on after
    // the input stops, in samples (-1 forever), when it follows what the
    // plugin hears or is set to (a delay's, its time and feedback); Info's
    // .tail until called. The host is told of each change.
    void setTail(int samples);
    // Debugging: value, under name (a string literal), at the start of the
    // block or at its frame i, for a host that traces the plugin (dsp.test
    // gives each name's values over the signal). While none does it costs a
    // check; at most 16 names, of 47 characters at most.
    void trace(const char* name, double value) const;
    void trace(const char* name, double value, int i) const;

  private:
    friend struct detail::Instance;
    friend struct detail::Layout;
    friend class Instrument;
    friend class Module;
    detail::Instance* instance_ = nullptr;
};

// Audio in, audio out.
class Effect : public Plugin {};

// How an instrument's voices take notes. Poly gives each note a voice of its
// own. Mono plays the newest key held on voice 0, and returns to the last one
// still held when it is released. Legato is Mono gliding from note to note
// without starting the voice again.
enum class VoiceMode { Poly, Mono, Legato };

// Notes in, audio out. The runtime hands each note to one of the
// instrument's voices (Info's .voices, 8 when it gives none), stealing the
// oldest when every voice is busy, and holds released notes while the
// sustain pedal is down. It calls the instrument's
//
//   void startVoice(int v)     voice v starts a note: voiceKey(v), voiceVelocity(v)
//   void releaseVoice(int v)   its key is up: start the release
//   void stopVoice(int v)      (optional) silence it at once: stolen or choked
//
// process() adds every voice that voicePlaying(v) into io, which arrives
// silent, and calls voiceFinished(v) once a voice's release has died away,
// which tells the host its note is over. startVoice may take a second
// argument, startVoice(int v, bool retrigger): false in Legato mode, for a
// note that glides from the one sounding, whose envelope keeps going.
class Instrument : public Plugin {
  public:
    int voiceCount() const;
    bool voicePlaying(int v) const;     // from startVoice until voiceFinished
    int voiceKey(int v) const;          // the note's MIDI key, 0..127
    int voiceChannel(int v) const;      // its MIDI channel, 0..15
    double voiceVelocity(int v) const;  // 0..1
    // At frame i of the block: the voice's pitch in semitones (its key,
    // gliding, plus its channel's pitch bend), and its frequency in Hz.
    double voicePitch(int v, int i) const;
    double voiceHz(int v, int i) const;
    // The note's expressions, as the host sends them for it (CLAP note
    // expressions; hosts convert MPE to them): volume a linear gain 0..4,
    // 1 until sent; pan 0..1, 0.5 (the centre) until sent; vibrato,
    // expression, brightness and pressure 0..1, 0 until sent. Its tuning
    // expression is already in voicePitch() and voiceHz().
    double voiceVolume(int v) const;
    double voicePan(int v) const;
    double voiceVibrato(int v) const;
    double voiceExpression(int v) const;
    double voiceBrightness(int v) const;
    double voicePressure(int v) const;
    void voiceFinished(int v);
    // Usually in update(): how notes take voices, the glide from one note to
    // the next (Mono and Legato), and the pitch bend's reach in semitones (2
    // unless set).
    void setVoiceMode(VoiceMode mode);
    void setGlide(double ms);
    void setBendRange(double semitones);
    // Usually in update(): how many voices Poly mode plays at once, 1 to
    // voiceCount() (all of them unless set); a note past them steals one.
    // Lowering it stops the voices above it, through stopVoice().
    void setVoiceLimit(int voices);
    // In pointer(): play a note through the plugin as the editor's keyboard
    // does, on at a velocity (0..1) or off. A note still on when the press
    // that played it ends (its release, or the editor losing it) is ended
    // then, so none is left sounding.
    void playNote(int key, double velocity, bool on) const;
};

// Notes in, notes out. Not available to scripts yet, and the compiler says
// so.
class NoteEffect : public Plugin {};

// --- modules (ADR-0062) -----------------------------------------------------------
//
// A module is a unit of a modular rack: ports joined by cables, parameters
// (knobs) that cables also move. Its struct derives from Module and declares
// `static constexpr ModuleInfo info`, its parameters (Param, as a plugin's),
// its ports (Input, Output) and the hooks
//
//   void prepare()               before audio: sizes, seeds (seed())
//   void update()                when a knob moved, at a block's start
//   void process(int frames)     every block of 1..32 frames: read inputs and
//                                at(i, c), write outputs
//   void panel(Panel& p)         (optional) where its controls and jacks sit
//   void draw(Canvas& g)         (optional) what its face shows
//   void pointer(const Pointer&) (optional) the mouse on its face
//
// The rack runs every module on a 32-frame grid of the timeline, so a patch
// renders the same in real time and offline. Module guide:
// skill/guides/modules.md.

// The most channels a cable carries (its polyphony).
inline constexpr int kMaxChannels = 16;

// What a port carries, by convention: audio (+-1), pitch (octaves, 0 at
// C4), a gate (0 or 1, high at 0.5 and above; a trigger is a 1 ms gate),
// bipolar CV (-1..1), unipolar CV (0..1) or a phase (a 0..1 ramp a cycle).
enum class SignalKind { Audio, Pitch, Gate, Cv, Unipolar, Phase };

// A module's identity: `static constexpr ModuleInfo info = {...}` in its
// struct. The ID never changes once the module is shared.
struct ModuleInfo {
    const char* name = "";
    const char* id = "";  // reverse-DNS: "com.example.fold"
    const char* vendor = "";
    const char* version = "1.0.0";
    const char* description = "";
    const char* url = "";
    // What it is, first ("oscillator", "filter", "sequencer"): the browser
    // sorts by them. Lower case, digits and '-'.
    const char* tags[4] = {};
    // Its face in design units; 0 takes the automatic layout's.
    int width = 0, height = 0;
};

// A port's description, made by the factories below: audioIn("In").help("...").
struct PortSpec {
    const char* name = "";
    const char* description = "";
    const char* key = "";    // the key; the member's name when empty
    const char* group = "";  // its section in the automatic layout
    SignalKind signal = SignalKind::Audio;
    bool output = false;
    double normalValue = 0.0;  // an input's value while nothing is connected

    PortSpec help(const char* text) const {
        PortSpec p = *this;
        p.description = text;
        return p;
    }
    PortSpec in(const char* g) const {
        PortSpec p = *this;
        p.group = g;
        return p;
    }
    PortSpec withKey(const char* k) const {
        PortSpec p = *this;
        p.key = k;
        return p;
    }
    // An input's value while nothing is connected (0 unless given).
    PortSpec normal(double value) const {
        PortSpec p = *this;
        p.normalValue = value;
        return p;
    }
};

inline PortSpec portSpec(const char* name, SignalKind signal, bool output) {
    PortSpec p;
    p.name = name;
    p.signal = signal;
    p.output = output;
    return p;
}
inline PortSpec audioIn(const char* name) { return portSpec(name, SignalKind::Audio, false); }
inline PortSpec pitchIn(const char* name) { return portSpec(name, SignalKind::Pitch, false); }
inline PortSpec gateIn(const char* name) { return portSpec(name, SignalKind::Gate, false); }
inline PortSpec cvIn(const char* name) { return portSpec(name, SignalKind::Cv, false); }
inline PortSpec unipolarIn(const char* name) { return portSpec(name, SignalKind::Unipolar, false); }
inline PortSpec phaseIn(const char* name) { return portSpec(name, SignalKind::Phase, false); }
inline PortSpec audioOut(const char* name) { return portSpec(name, SignalKind::Audio, true); }
inline PortSpec pitchOut(const char* name) { return portSpec(name, SignalKind::Pitch, true); }
inline PortSpec gateOut(const char* name) { return portSpec(name, SignalKind::Gate, true); }
inline PortSpec cvOut(const char* name) { return portSpec(name, SignalKind::Cv, true); }
inline PortSpec unipolarOut(const char* name) { return portSpec(name, SignalKind::Unipolar, true); }
inline PortSpec phaseOut(const char* name) { return portSpec(name, SignalKind::Phase, true); }

namespace detail {
struct PortBinding;
}  // namespace detail

// An input: in[c][i] is channel c at frame i of the block. A mono cable
// reaches every channel; past a poly cable's channels it reads 0; with
// nothing connected, the normal value. Several cables into one input sum.
class Input {
  public:
    Input(const PortSpec& spec);  // registers it with the module under construction
    Input(const Input& other) = default;
    Input& operator=(const Input& other) = default;

    InputChannel operator[](int c) const {
        if (static_cast<unsigned>(c) >= static_cast<unsigned>(kMaxChannels) || !base_) {
            detail::outOfRange(0);
            return InputChannel{nullptr, 0};
        }
        if (stride_ != 0 && c >= channels_) return InputChannel{zero_, frames_};
        return InputChannel{base_ + c * stride_, frames_};
    }
    // 0 while nothing is connected; else the cable's channels (the widest).
    int channels() const { return channels_; }
    bool connected() const { return channels_ > 0; }

  private:
    friend struct detail::Layout;
    friend struct detail::PortBinding;
    // Channel c's frames start at base_ + c * stride_ (stride 0: one buffer
    // for every channel, a mono cable or the normal value).
    const double* base_ = nullptr;
    const double* zero_ = nullptr;
    int stride_ = 0;
    int channels_ = 0;
    int frames_ = 0;
    int slot_ = -1;
};

// An output: out[c][i] = x. It has channels() channels: the module's
// channels() unless setChannels() says, each block. It arrives zeroed.
class Output {
  public:
    Output(const PortSpec& spec);  // registers it with the module under construction
    Output(const Output& other) = default;
    Output& operator=(const Output& other) = default;

    Channel operator[](int c) const {
        if (static_cast<unsigned>(c) < static_cast<unsigned>(kMaxChannels) && base_)
            return Channel{base_ + c * stride_, frames_, spare_};
        detail::outOfRange(0);
        return Channel{spare_, 0, spare_};
    }
    int channels() const { return channels_; }
    // In process(): how many channels this output carries this block, 1..16.
    void setChannels(int n) { channels_ = n < 1 ? 1 : (n > kMaxChannels ? kMaxChannels : n); }

  private:
    friend struct detail::Layout;
    friend struct detail::PortBinding;
    double* base_ = nullptr;
    double* spare_ = nullptr;
    int stride_ = 0;
    int channels_ = 1;
    int frames_ = 0;
    int slot_ = -1;
};

namespace detail {
// Construction runs only on the main thread, with a registrar installed.
Param numberedParam(const ParamSpec& spec, int index);
Input numberedInput(const PortSpec& spec, int index);
Output numberedOutput(const PortSpec& spec, int index);
// An invalid declaration-array access uses a disconnected, zero-valued
// spare; resetting it never registers, allocates or follows a stale binding.
void resetMember(Param& p, double* spare);
void resetMember(Input& p, double* spare);
void resetMember(Output& p, double* spare);

template <class T, class Spec, int N>
class Members {
    static_assert(N > 0, "a declaration array needs a positive size");
    static_assert(N <= (std::is_same_v<T, Param> ? 256 : 32), "too many declarations");
    template <std::size_t... I>
    Members(const Spec& spec, std::index_sequence<I...>)
        : items_{make(spec, static_cast<int>(I))...}, spare_(items_[0]) {
        resetMember(spare_, &spareSample_);
    }
    template <std::size_t... I>
    Members(const Members& other, std::index_sequence<I...>)
        : items_{other.items_[I]...}, spare_(items_[0]) {
        resetMember(spare_, &spareSample_);
    }
    static T make(const Spec& spec, int i) {
        if constexpr (std::is_same_v<T, Param>)
            return numberedParam(spec, i);
        else if constexpr (std::is_same_v<T, Input>)
            return numberedInput(spec, i);
        else
            return numberedOutput(spec, i);
    }

  public:
    Members(const Spec& spec) : Members(spec, std::make_index_sequence<N>{}) {}
    Members(const Members& other) : Members(other, std::make_index_sequence<N>{}) {}
    Members& operator=(const Members& other) {
        if (this != &other) {
            for (int i = 0; i < N; ++i) items_[i] = other.items_[i];
            resetMember(spare_, &spareSample_);
        }
        return *this;
    }
    T& operator[](int i) {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(N)) return items_[i];
        outOfRange(0);
        resetMember(spare_, &spareSample_);
        return spare_;
    }
    const T& operator[](int i) const { return const_cast<Members&>(*this)[i]; }
    static constexpr int size() { return N; }
    T* begin() { return items_; }
    T* end() { return items_ + N; }
    const T* begin() const { return items_; }
    const T* end() const { return items_ + N; }

  private:
    T items_[N];
    T spare_;
    double spareSample_ = 0.0;
};
}  // namespace detail

// Module declarations replicated from one spec. Each element is an ordinary
// Param/Input/Output in the module's metadata. Index 0 is displayed as 1;
// names are "Name 1".."Name N", keys "member_1".."member_N". withKey()
// changes the key stem. Declare one bank per line, directly in a Module.
template <int N>
using Params = detail::Members<Param, ParamSpec, N>;
template <int N>
using Inputs = detail::Members<Input, PortSpec, N>;
template <int N>
using Outputs = detail::Members<Output, PortSpec, N>;

// What every module struct derives from. channels() is its polyphony: the
// widest connected input or modulating cable, at least 1.
class Module : public Plugin {
  public:
    // The host's transport at the block's start.
    Transport transport() const;
    // A seed from the project's seed and the module's id: the same in every
    // render, different for each module (Random::seed(seed())).
    int seed() const;
};

// A gate with hysteresis: rises at >= 0.5 and falls below 0.25. rose(x)
// updates its state and returns true only on a low-to-high transition.
class Trigger {
  public:
    bool rose(double x) {
        const bool before = high_;
        if (x >= 0.5)
            high_ = true;
        else if (x < 0.25)
            high_ = false;
        return high_ && !before;
    }
    bool high() const { return high_; }
    void clear() { high_ = false; }

  private:
    bool high_ = false;
};

// A 1 ms gate at the current instance's sample rate, or an explicit number
// of frames. fire() restarts it; next() returns 1 while active, then 0.
class Pulse {
  public:
    void fire();
    void fire(int frames) { remaining_ = frames > 0 ? frames : 0; }
    double next() {
        if (remaining_ <= 0) return 0.0;
        --remaining_;
        return 1.0;
    }
    void clear() { remaining_ = 0; }

  private:
    int remaining_ = 0;
};

// --- DSP -------------------------------------------------------------------------

// Every object below works at the plugin's sample rate. Their state starts
// silent, and clear() returns it there.

// A biquad filter (RBJ cookbook designs). q is the resonance (0.707 is
// Butterworth); for the shelves it sets the slope.
class Biquad {
  public:
    Biquad();
    void lowpass(double hz, double q);
    void highpass(double hz, double q);
    void bandpass(double hz, double q);  // 0 dB at the centre
    void notch(double hz, double q);
    void allpass(double hz, double q);
    void peak(double hz, double q, double gainDb);
    void lowShelf(double hz, double q, double gainDb);
    void highShelf(double hz, double q, double gainDb);
    double process(double x);
    void clear();
    // The design's gain at hz, in dB (for drawing a response).
    double responseDb(double hz) const;

  private:
    alignas(8) unsigned char storage_[56];
};

// A one-pole filter with low-pass and high-pass outputs.
class OnePole {
  public:
    OnePole();
    void setCutoff(double hz);
    double lowpass(double x);
    double highpass(double x);
    void clear();

  private:
    alignas(8) unsigned char storage_[24];
};

// A compressor's detector: the signal's level (rectified) through a one-pole
// that rises at the attack time and falls at the release time. Pair it with
// compressorGainDb:
//
//   double level = follower.process(key);
//   y = x * dbToGain(compressorGainDb(gainToDb(level), threshold, ratio, 6));
class EnvelopeFollower {
  public:
    EnvelopeFollower();
    void set(double attackMs, double releaseMs);
    double process(double x);  // the level now, linear
    double level() const;
    void clear();

  private:
    alignas(8) unsigned char storage_[24];
};

// A delay line with fractional (Hermite) reads. prepare() its longest delay
// in prepare(); push() one sample per frame, read() any delay up to it.
class DelayLine {
  public:
    DelayLine();
    DelayLine(const DelayLine& other);
    DelayLine& operator=(const DelayLine& other);
    ~DelayLine();
    void prepare(int maxDelaySamples);  // prepare() only: it allocates
    void push(double x);
    double read(double delaySamples) const;  // 0 is the sample just pushed
    void clear();

  private:
    void* line_ = nullptr;
};

// Oversampling, for anything nonlinear: a shaper's harmonics above half the
// sample rate fold back as aliases unless they are made at a higher rate and
// filtered away coming down. One per channel, per sample:
//
//   o.up(x);
//   for (int k = 0; k < o.factor(); ++k) o.set(k, tanh(drive * o.sample(k)));
//   y = o.down();
//
// prepare(factor) in prepare(): 1, 2, 4, 8 or 16 (others round down; 1, and
// until prepared, passes samples through). Its filters are minimum phase by
// default: latency() is a few samples, which setLatency() reports, and the
// top octave's phase turns a little. prepare(factor, Oversampler::Phase::Linear)
// gives linear-phase filters instead: no phase change at all, so a mix with
// the dry signal delayed by latency() is exact, at a latency of about 70
// samples.
class Oversampler {
  public:
    enum class Phase { Minimum, Linear };

    Oversampler();
    Oversampler(const Oversampler& other);
    Oversampler& operator=(const Oversampler& other);
    ~Oversampler();
    void prepare(int factor);  // prepare() only: it allocates
    void prepare(int factor, Phase phase);
    int factor() const;
    int latency() const;
    void up(double x);
    double sample(int k) const;  // 0 .. factor() - 1
    void set(int k, double value);
    double down();
    void clear();

  private:
    void* state_ = nullptr;
};

// The short-time Fourier transform, for spectral effects and analysis: one
// per channel. Each sample, push() the input; when it returns true a frame's
// spectrum is ready, bins 0 to bins() - 1, to read and change; pull() then
// gives the output, latency() samples behind the input:
//
//   if (s.push(x))
//       for (int k = 0; k < s.bins(); ++k) s.scale(k, gainAt(s.binHz(k)));
//   y = s.pull();
//
// prepare(size, overlap) in prepare(): size a power of two from 16 to 32768
// (rounded down), overlap 4 or 8 frames over each sample. Frames are Hann
// windowed both ways, so a spectrum left alone gives back the input. Until
// prepared it passes samples through. The bins are readable and writable
// from a push() that returned true until the pull() after it; otherwise
// they read 0 and writes are lost. A sine of amplitude 1 at a bin's centre
// frequency reads magnitude 1 there.
//
// An analyser that only reads bins calls prepareAnalysis(size, overlap)
// instead: the same frames and bins at about half the cost, with no audio
// back. push() alone each sample; the bins are readable from a push() that
// returned true until the next push(), and pull() gives 0.
class Stft {
  public:
    Stft();
    Stft(const Stft& other);
    Stft& operator=(const Stft& other);
    ~Stft();
    void prepare(int size, int overlap);          // prepare() only: it allocates
    void prepareAnalysis(int size, int overlap);  // the same, to read bins only
    int size() const;
    int bins() const;  // size() / 2 + 1
    int hop() const;   // samples between frames: size() / overlap
    int latency() const;
    double binHz(int k) const;  // bin k's centre frequency
    bool push(double x);
    double pull();
    double real(int k) const;
    double imag(int k) const;
    double magnitude(int k) const;
    double phase(int k) const;  // radians
    void set(int k, double real, double imag);
    void setPolar(int k, double magnitude, double phase);
    void scale(int k, double gain);
    void clear();

  private:
    void* state_ = nullptr;
};

// Pseudo-random numbers (PCG32), the same sequence everywhere for a seed.
class Random {
  public:
    Random();
    void seed(int seed);
    double next();     // 0 <= x < 1
    double bipolar();  // -1 <= x < 1
    double gaussian();
    int below(int n);  // 0 .. n-1

  private:
    alignas(8) unsigned char storage_[16];
};

// White, pink and brown noise, about -10 dBFS RMS.
class Noise {
  public:
    Noise();
    void seed(int seed);
    double white();
    double pink();
    double brown();

  private:
    alignas(8) unsigned char storage_[80];
};

// An anti-aliased (polyBLEP) oscillator.
class Oscillator {
  public:
    enum class Shape { Sine, Saw, Square, Triangle };
    Oscillator();
    void setShape(Shape shape);
    void setFrequency(double hz);
    void setPulseWidth(double width);  // square: 0.01..0.99
    void setPhase(double phase);       // 0..1
    double phase() const;
    double next();

  private:
    alignas(8) unsigned char storage_[48];
};

// An ADSR envelope: times in ms, sustain 0..1. gate(true) starts it,
// gate(false) releases it; active() is false once it has died away.
class Adsr {
  public:
    Adsr();
    void set(double attackMs, double decayMs, double sustain, double releaseMs);
    void setCurves(double attack, double decayRelease);  // 0 linear .. 1 most curved
    void gate(bool on);
    void retrigger();
    void kill();
    double next();
    double level() const;
    bool active() const;

  private:
    alignas(8) unsigned char storage_[192];
};

// --- the editor ------------------------------------------------------------------
//
// A plugin may draw its own editor with three hooks, each optional:
//
//   void editor(Editor& e)          once, as the editor opens: its size and controls
//   void draw(Canvas& g)            at each repaint: what lies under the controls
//   void pointer(const Pointer& p)  as the pointer acts where no control is:
//                                   draggable handles, clickable shapes
//
// A plugin with neither gets the automatic editor: every parameter, by group.
// Editor code runs on the host's main thread, in a copy of the plugin struct
// of its own. There each Param reads its current value (with the host's
// modulation) and each Shared member what the audio code last sent; members
// only editor code uses are the editor's own (a hover state, a scale). It
// sees nothing else of the audio code's: a member both use is refused. It
// changes a parameter only as a user would, by edit() in pointer(). Editor
// code may format text (format(), Param::text()) and loops as long as it
// likes within a generous budget.

// A value the audio code shows the editor: a level, a position, a count. The
// audio code set()s it; the editor get()s it as it was when last sent (about
// 120 times a second while the editor is open; every Shared member at the same
// moment). A member of the plugin struct, never set by editor code.
template <class T>
class Shared {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool>,
                  "a Shared holds double, float, int or bool");

  public:
    Shared() { detail::declareShared(this, static_cast<int>(sizeof(Shared))); }
    Shared(const Shared& other) : value_(other.value_) {}
    Shared& operator=(const Shared& other) = default;
    T get() const { return value_; }
    void set(T value) { value_ = value; }

    T value_ = T();
};

// An Array the audio code shows the editor: a scope's samples, a spectrum's
// bins. Indexed like an Array; sent whole, with the Shared members.
template <class T, int N>
class SharedArray : public Array<T, N> {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int> ||
                      std::is_same_v<T, bool>,
                  "a SharedArray holds double, float, int or bool");

  public:
    SharedArray() : Array<T, N>() {
        detail::declareShared(this, static_cast<int>(sizeof(SharedArray)));
    }
    SharedArray(const SharedArray& other) : Array<T, N>(other) {}
    SharedArray& operator=(const SharedArray& other) = default;
};

// Colours are 0xRRGGBB (CSS's #6fb3a8 is 0x6fb3a8), or one of the editor's
// palette by name, Theme::accent: the palette the automatic editor and the
// controls draw with, which an exported plugin may change.
struct Theme {
    enum : int {
        ground = 0x1000000,  // the background
        surface,             // raised panels
        raised,              // buttons, menus
        recess,              // wells: graph floors, meter tracks
        line,                // hairlines, panel edges
        lineLit,             // grid lines picked out
        textFaint,
        textMuted,
        text,
        accent,      // values, active toggles
        accent2,     // a second series: input meters, a second curve
        modulation,  // modulation ranges
        alert,       // clipping, errors
    };
};

enum class TextAlign { Left, Center, Right };
enum class TextBaseline { Top, Middle, Alphabetic, Bottom };
enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };

// Where draw() draws: like an HTML canvas's 2D context, in the editor's
// design units (the window scales everything). Styles and transforms hold
// until changed, or until restore() brings back what save() kept.
//
//   g.fillStyle(Theme::accent);
//   g.beginPath();
//   g.arc(100, 100, 40, 0, twoPi);
//   g.fill();
class Canvas {
  public:
    double width() const;  // the editor's size
    double height() const;

    void save();
    void restore();
    void translate(double x, double y);
    void rotate(double radians);  // clockwise, as on screen
    void scale(double x, double y);

    void fillStyle(int colour);
    void strokeStyle(int colour);
    void globalAlpha(double alpha);  // 0..1, on everything drawn after
    void lineWidth(double width);
    void lineCap(LineCap cap);
    void lineJoin(LineJoin join);
    // The fill, until the next fillStyle(): a gradient from one colour to
    // another, along a line or out from a centre.
    void linearGradient(double x0, double y0, double x1, double y1, int from, int to);
    void radialGradient(double x, double y, double innerRadius, double outerRadius, int from,
                        int to);

    void fillRect(double x, double y, double w, double h);
    void strokeRect(double x, double y, double w, double h);
    void clearRect(double x, double y, double w, double h);  // to the background

    void beginPath();
    void closePath();
    void moveTo(double x, double y);
    void lineTo(double x, double y);
    void bezierCurveTo(double c1x, double c1y, double c2x, double c2y, double x, double y);
    void quadraticCurveTo(double cx, double cy, double x, double y);
    // Angles in radians, 0 to the right, growing clockwise.
    void arc(double x, double y, double radius, double startAngle, double endAngle);
    void arcTo(double x1, double y1, double x2, double y2, double radius);
    void rect(double x, double y, double w, double h);
    void roundRect(double x, double y, double w, double h, double radius);
    void ellipse(double x, double y, double radiusX, double radiusY);
    void circle(double x, double y, double radius);
    void fill();
    void stroke();
    // Drawing after it stays inside this rectangle (and any clip before it),
    // until restore().
    void clip(double x, double y, double w, double h);

    // Text: Inter, a bold Inter, or Plex Mono for numbers, at a size; or
    // Fraunces, a soft serif, for a name or a heading (serifFont) and an
    // aside (serifItalicFont).
    void font(double size);
    void boldFont(double size);
    void monoFont(double size);
    void serifFont(double size);
    void serifItalicFont(double size);
    void textAlign(TextAlign align);
    void textBaseline(TextBaseline baseline);
    void fillText(const char* text, double x, double y);
    double measureText(const char* text) const;  // its width

    // The editor's furniture, in its palette: a raised panel with a title
    // ("" for none), a recessed well, a level meter (dBFS, -72 to +6), and a
    // graph's grid lines, at 1, 2 and 5 of each decade from lowHz to highHz
    // across, every stepDb from lowDb (bottom) to highDb (top) up.
    void panel(double x, double y, double w, double h, const char* title);
    void well(double x, double y, double w, double h);
    void meter(double levelDb, double x, double y, double w, double h);
    void frequencyGrid(double x, double y, double w, double h, double lowHz, double highHz);
    void levelGrid(double x, double y, double w, double h, double lowDb, double highDb,
                   double stepDb);

  private:
    friend struct detail::Layout;
    detail::Painter* painter_ = nullptr;
};

// For graphs: where hz falls across x .. x + w on a log scale from lowHz to
// highHz (as frequencyGrid draws it), and where db falls up from y + h (at
// lowDb) to y (at highDb).
double hzToX(double hz, double lowHz, double highHz, double x, double w);
double dbToY(double db, double lowDb, double highDb, double y, double h);

// Text, printf-style: format("%.1f dB", level). Numbers only -- %d, %i, %f,
// %e, %g, %x with their flags, width and precision, and %% -- up to four of
// them. The text lasts until the editor hook returns.
const char* format(const char* pattern, double a);
const char* format(const char* pattern, double a, double b);
const char* format(const char* pattern, double a, double b, double c);
const char* format(const char* pattern, double a, double b, double c, double d);

// Their inverses, for pointer(): the frequency at `at` across x .. x + w,
// and the level at `at` up from y + h.
double xToHz(double at, double lowHz, double highHz, double x, double w);
double yToDb(double at, double lowDb, double highDb, double y, double h);

// What the pointer did, for pointer(): pressed the main button where no
// control is, moved with it held (each move a Drag, wherever it goes),
// released it, moved with it up (a hover), turned the wheel where no
// control is, or left the editor.
enum class PointerAction { Press, Drag, Release, Move, Wheel, Leave };

// The pointer as pointer() sees it, in the editor's design units. The
// editor repaints after every call.
//
//   void pointer(const Pointer& p) {
//       if (p.action() == PointerAction::Press) held = p.within(nodeX, nodeY, 10);
//       if (p.action() == PointerAction::Drag && held)
//           cutoff.edit(xToHz(p.x(), 20, 20000, 20, 440));
//       if (p.action() == PointerAction::Release) held = false;
//   }
class Pointer {
  public:
    PointerAction action() const { return action_; }
    double x() const { return x_; }
    double y() const { return y_; }
    // Drag and Move: how far it went since the last one.
    double dx() const { return dx_; }
    double dy() const { return dy_; }
    // Wheel: notches turned, positive up (away from the user).
    double wheel() const { return wheel_; }
    // Press: the second press of a double click.
    bool doubleClick() const { return doubleClick_; }
    // Keys held: Shift (fine adjustment, by custom), Ctrl (Cmd on a Mac), Alt.
    bool shift() const { return shift_; }
    bool ctrl() const { return ctrl_; }
    bool alt() const { return alt_; }
    // Whether it is over a rectangle, or within `radius` of a point.
    bool over(double x, double y, double w, double h) const {
        return x_ >= x && x_ < x + w && y_ >= y && y_ < y + h;
    }
    bool within(double x, double y, double radius) const {
        const double a = x_ - x, b = y_ - y;
        return a * a + b * b <= radius * radius;
    }

  private:
    friend struct detail::Layout;
    PointerAction action_ = PointerAction::Move;
    double x_ = 0.0, y_ = 0.0, dx_ = 0.0, dy_ = 0.0, wheel_ = 0.0;
    bool doubleClick_ = false, shift_ = false, ctrl_ = false, alt_ = false;
};

// Metadata only: factoryPatches(Patches& p) declares read-only patches.
// Values are patch values in physical units: percent is 30, not 0.3. A
// route is as a patch writes it, its amount in the destination's unit (a
// frequency's in octaves), and through a second source when one is given:
// p.route("lfo1>pitch", 0.2, "modwheel"). A file member takes one of the
// plugin's factory files: p.file(sample, "factory:pad.flac").
class Patches {
  public:
    explicit Patches(detail::PatchBuilder& builder) : builder_(&builder) {}
    Patches& add(const char* name);
    Patches& set(const Param& param, double physical);
    Patches& set(const Param& param, int physical) {
        return set(param, static_cast<double>(physical));
    }
    Patches& set(const Param& param, const char* choiceKey);
    Patches& route(const char* route, double amount);
    Patches& route(const char* route, double amount, const char* via);
    Patches& file(const AudioFile& file, const char* factoryFile);
    Patches& file(const Convolver& file, const char* factoryFile);

  private:
    detail::PatchBuilder* builder_;
};

// What editor() builds: its size (480 x 320 unless given) and its controls,
// the framework's own, each bound to a parameter (or a file) and drawn over
// draw()'s canvas. A control's label is its parameter's name.
class Editor {
  public:
    void size(double width, double height);
    void knob(const Param& p, double x, double y, double w, double h);
    void slider(const Param& p, double x, double y, double w, double h);  // along its longer side
    void toggle(const Param& p, double x, double y, double w, double h);
    // A choice's options: buttons side by side for up to four, a menu for more.
    void choice(const Param& p, double x, double y, double w, double h);
    // A value shown, not edited: a meter().
    void readout(const Param& p, double x, double y, double w, double h);
    // Two parameters dragged together, across and up.
    void xyPad(const Param& across, const Param& up, double x, double y, double w, double h);
    // An envelope's four stages, each dragged by a handle.
    void envelope(const Param& attack, const Param& decay, const Param& sustain,
                  const Param& release, double x, double y, double w, double h);
    // A file member's file: its name and state; click to choose, drop to take.
    void file(const AudioFile& f, double x, double y, double w, double h);
    void file(const Convolver& c, double x, double y, double w, double h);
    // An instrument's keyboard, lowKey to highKey: playing it plays the plugin.
    void keyboard(int lowKey, int highKey, double x, double y, double w, double h);
    // The input and output peak meters the framework keeps.
    void meters(double x, double y, double w, double h);
    // The modulation routes, as a list to edit: a row a route (its source,
    // what it moves, by how much, through what) and a row to add one.
    // Nothing for a plugin without routes.
    void routes(double x, double y, double w, double h);

  private:
    friend struct detail::Layout;
    detail::EditorBuilder* builder_ = nullptr;
};

namespace detail {
class PanelBuilder;
}  // namespace detail

// What a module's panel() lays out on its face, in design units from its
// top-left corner: its size, and where each control and jack sits. Knobs and
// jacks are placed by their centre; a control is labelled with its
// parameter's name, a jack with its port's. Without panel() a module gets
// the automatic layout. draw() paints under them.
class Panel {
  public:
    void size(double width, double height);
    // A knob, 36 units across unless `size` says (24 small, 52 large).
    void knob(const Param& p, double x, double y);
    void knob(const Param& p, double x, double y, double size);
    void slider(const Param& p, double x, double y, double w, double h);  // along its longer side
    void toggle(const Param& p, double x, double y, double w, double h);
    // A choice's options: buttons side by side for up to four, a menu for more.
    void choice(const Param& p, double x, double y, double w, double h);
    // A value shown, not edited: a meter().
    void readout(const Param& p, double x, double y, double w, double h);
    // A jack, by its centre.
    void input(const Input& in, double x, double y);
    void output(const Output& out, double x, double y);
    // Where draw() shows something (a scope, a step grid): kept clear of
    // controls by layout checks.
    void display(double x, double y, double w, double h);

  private:
    friend struct detail::Layout;
    detail::PanelBuilder* builder_ = nullptr;
};

}  // namespace supple::script

using namespace supple::script;
