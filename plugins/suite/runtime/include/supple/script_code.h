// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A plugin made from a script, as the glue that runs it sees it, however it
// was built: by the script compiler (the player's JIT, scriptc::Program) or
// by a C++ compiler (a source export, supple/script_native.h). Its
// description -- identity, buses, parameters, files and shared state, as the
// script declares them -- and what an instance needs to be made, copied,
// destroyed and run, and its editor drawn.
// Scripts never see it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace supple::script::detail {

enum class PluginKind : std::uint8_t { None, Effect, Instrument, NoteEffect, Module };

// A parameter as the script declares it (script.h's ParamSpec, evaluated).
// min, max and def are physical: a percent in percent, a choice an index.
struct ParamModel {
    enum class Kind { Continuous, Integer, Choice, Toggle };
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
    enum class Curve { Linear, Log, Pow };

    std::string member;    // the member's name
    std::string key;       // the patch key (the member's name in snake_case unless given)
    std::uint32_t id = 0;  // the CLAP id (keyId(key) unless given)
    Kind kind = Kind::Continuous;
    std::string name;
    double min = 0.0, max = 1.0, def = 0.0;
    Unit unit = Unit::None;
    Curve curve = Curve::Linear;
    double curveK = 1.0;
    std::vector<std::string> choices;
    std::vector<std::string> choiceKeys;  // given by .keys(); empty: from the labels
    std::string group, description;
    double smoothingMs = 0.0;
    bool automatable = true, modulatable = false, offAtMinimum = false, output = false,
         bypass = false, hidden = false;
    bool polyModulatable = false;  // modulated per voice (an instrument's)
    bool restarts = false;         // a change restarts the plugin (.restarts())
    std::vector<std::string> roles;
    // When it applies (.activeWhen(), .activeUnless(), .activeAbove(),
    // .activeBelow()): while every condition holds and the parameter each
    // names applies itself; always, without any.
    struct Condition {
        int param = -1;  // a parameter declared before this one (declaration order)
        // A toggle's or a choice's: its values that satisfy it (a toggle's 0
        // or 1, choice indices). Empty for a number's.
        std::vector<int> values;
        // A number's (continuous or integer): above < value < below, in its
        // physical unit (a percent in percent).
        double above = -std::numeric_limits<double>::infinity();
        double below = std::numeric_limits<double>::infinity();
        bool operator==(const Condition&) const = default;
    };
    std::vector<Condition> activeWhen;
    // How far routes may move it (.routeRange()), physical; its own range
    // when routeMin > routeMax.
    double routeMin = 1.0, routeMax = 0.0;
    int offset = 0;  // of the Param member in the plugin struct
};

// A Source member as the script declares it: a modulation source the
// patch's routes name.
struct SourceModel {
    std::string member;  // the member's name
    std::string key;     // the routes' name for it (the member's name in snake_case unless given)
    std::string name, description;
    bool perVoice = false;  // voiceSource(): a value per voice
    bool bipolar = true;    // -1..1, or 0..1
    int offset = 0;         // of the member in the plugin struct
    bool operator==(const SourceModel&) const = default;
};

// The sources every instrument has, the runtime's own, after the script's.
struct BuiltinSource {
    const char* key;
    const char* name;
    bool perVoice, bipolar;
    const char* description;
};
inline constexpr BuiltinSource kInstrumentSources[] = {
    {"velocity", "Velocity", true, false, "The note's velocity."},
    {"key", "Key", true, true,
     "The note's key: -1 at key 0, 0 at middle C (60), 1 at key 120; an amount of 5 oct moves a "
     "cutoff an octave per octave."},
    {"random", "Random", true, true, "A random value for each note, new at each note."},
    {"pressure", "Pressure", true, false, "The note's pressure (poly aftertouch, MPE pressure)."},
    {"slide", "Slide", true, false, "The note's slide (CLAP brightness, MPE timbre)."},
    {"modwheel", "Mod Wheel", false, false, "The mod wheel (CC 1)."},
    {"breath", "Breath", false, false, "The breath controller (CC 2)."},
    {"expression", "Expression", false, false, "The expression pedal (CC 11)."},
    {"aftertouch", "Aftertouch", false, false, "Channel pressure."},
    {"pitchbend", "Pitch Bend", false, true, "The pitch bend, -1 to 1."},
};

// A file member (AudioFile, Convolver) as the script declares it: a file
// property of the plugin.
struct FileModel {
    enum class Kind { Audio, Impulse };  // audioFile(), impulseResponse()

    std::string member;    // the member's name
    std::string key;       // the patch key (the member's name in snake_case unless given)
    std::uint32_t id = 0;  // the property's id: keyId(key)
    Kind kind = Kind::Audio;
    std::string name, description;
    std::string def;  // its default: a factory file, or "" for none
    int offset = 0;   // of the member in the plugin struct
};

// "factory:<name>": one of the plugin's own files, inside its binary (the
// framework's core/Files.h), by a name without separators.
bool factoryFileName(std::string_view reference);

// A Shared or SharedArray member: bytes the audio code sends the editor.
struct SharedModel {
    std::string member;  // the member's name (empty when compiled as C++)
    int offset = 0;      // in the plugin struct
    int bytes = 0;
};

struct InfoModel {
    std::string name, id, vendor, version = "1.0.0", description, url;
    std::vector<std::string> features;  // after the main category, what the plugin is first
    std::vector<int> channels;  // main bus widths, the default first; empty: the kind's default
    bool sidechain = false;
    std::vector<std::string> outputs;
    int voices = 0, latency = 0, tail = 0;
};

// Parameter values in patch/physical units, keyed by the permanent CLAP id.
// Omitted parameters take their defaults when the patch is applied.
struct FactoryPatchModel {
    std::string name;
    std::vector<std::pair<std::uint32_t, double>> values;
    // Its routes, as Patches::route() gives them.
    struct Route {
        std::string route, via;  // "lfo1>pitch"; "" for none
        double amount = 0.0;
        bool operator==(const Route&) const = default;
    };
    std::vector<Route> routes;
    // Factory files for file members, by the property's id.
    std::vector<std::pair<std::uint32_t, std::string>> files;
    bool operator==(const FactoryPatchModel&) const = default;
};

std::string factoryPatchProblems(const std::vector<ParamModel>& params,
                                 const std::vector<FactoryPatchModel>& patches);
std::string factoryChoiceKey(std::string_view label);
// Option k's patch key: the one .keys() gave, or its label in snake_case.
std::string choiceKey(const ParamModel& param, size_t k);

// A parameter's CLAP id from its key (FNV-1a), stable however the members
// are ordered; never CLAP's invalid id, never 0.
constexpr std::uint32_t keyId(std::string_view key) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : key) {
        h ^= c;
        h *= 16777619u;
    }
    if (h == 0xFFFFFFFFu) h = 0xFFFFFFFEu;
    if (h == 0) h = 1;
    return h;
}

// The four letters a plugin's state is tagged with, made from its ID: as
// permanent as the ID, and the same wherever the plugin is built (the
// player, a source export).
inline std::string stateTag(std::string_view id) {
    std::uint32_t h = 2166136261u;
    for (char c : id) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    static constexpr char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string tag;
    for (int k = 0; k < 4; ++k) {
        tag += kLetters[h % 52];
        h = h / 52 + static_cast<std::uint32_t>(k) * 7919u;
    }
    return tag;
}

// Immutable once made; any number of instances share it, on any thread.
class Code {
  public:
    virtual ~Code() = default;

    virtual const InfoModel& info() const = 0;
    virtual PluginKind kind() const = 0;
    // In declaration order.
    virtual const std::vector<ParamModel>& params() const = 0;
    virtual const std::vector<FileModel>& files() const = 0;
    // In declaration order.
    virtual const std::vector<SourceModel>& sources() const = 0;
    virtual const std::vector<FactoryPatchModel>& factoryPatches() const = 0;
    // In declaration order.
    virtual const std::vector<SharedModel>& shared() const = 0;

    // An instance: the plugin struct, constructed (each Param at its
    // default). Main thread: it allocates.
    virtual void* create() const = 0;
    virtual void destroy(void* self) const = 0;
    // The whole struct, as its copy assignment does it: runtime objects by
    // their own copy (a DelayLine of the same capacity copies in place,
    // without allocating).
    virtual void copy(void* dst, const void* src) const = 0;

    // The hooks: (ctx: the instance's runtime record, self: the struct
    // [, block: a script::Block]). Null when the script has none. The glue
    // sets the current instance (CurrentScope) around each call.
    using Hook = void (*)(void* ctx, void* self);
    using ProcessHook = void (*)(void* ctx, void* self, void* block);
    virtual Hook prepare() const = 0;
    virtual Hook update() const = 0;
    virtual ProcessHook process() const = 0;

    // An instrument's voice hooks (script.h's Instrument), with the voice's
    // index (and for startVoice, whether to retrigger) as 64-bit integers.
    // startVoice is the one-argument form, startVoiceRetrigger the
    // two-argument one; a script has at most one of them.
    using VoiceHook = void (*)(void* ctx, void* self, std::int64_t v);
    using StartHook = void (*)(void* ctx, void* self, std::int64_t v, std::int64_t retrigger);
    virtual VoiceHook startVoice() const { return nullptr; }
    virtual StartHook startVoiceRetrigger() const { return nullptr; }
    virtual VoiceHook releaseVoice() const { return nullptr; }
    virtual VoiceHook stopVoice() const { return nullptr; }

    // The editor's hooks, on the main thread in the editor's own instance:
    // editor(Editor&), draw(Canvas&) and pointer(const Pointer&), the object
    // a script::Editor, script::Canvas or script::Pointer. Null when the
    // script has none; with neither editor() nor draw(), the plugin has the
    // automatic editor (and pointer() is never called).
    using UiHook = void (*)(void* ctx, void* self, void* object);
    virtual UiHook editor() const { return nullptr; }
    virtual UiHook draw() const { return nullptr; }
    virtual UiHook pointer() const { return nullptr; }
};

}  // namespace supple::script::detail
