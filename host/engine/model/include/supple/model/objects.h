// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Session object types (session-model §1). Plain value structs: immutable once
// shared through an IdMap; edited only by copying in an EditTxn. Their JSON
// form (schemas.h) is the project file's and the command bus's.
//
// Units: positions and lengths are in the domain their timebase implies
// (seconds for `time`, beats = quarter notes for `beats`); levels in dB
// (kSilenceDb = -inf), pan in percent, pitch in semitones.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/json/json.h"
#include "supple/midi/midi.h"
#include "supple/model/ids.h"

namespace supple::model {

inline constexpr double kSilenceDb = -150.0;  // levels at or below this are silence (-inf)
// The highest effective strip level: Console 9's native fader top, 20·log10(4).
inline constexpr double kFaderCeilingDb = 12.041199826559248;

enum class Timebase : std::uint8_t { Time, Beats, BeatsPosition };
enum class ItemTimebase : std::uint8_t { Track, Time, Beats, BeatsPosition };
enum class TrackTimebase : std::uint8_t { Project, Time, Beats, BeatsPosition };

inline constexpr double kMinPlayrate = 0.25, kMaxPlayrate = 4.0;
struct Playrate {
    double rate = 1.0;
    bool preservePitch = false;
    bool operator==(const Playrate&) const = default;
};

struct Project {
    std::string name, title, author, notes;
    double sampleRate = 48000.0;
    int ppq = 960;
    Timebase timebase = Timebase::Beats;
    std::string mediaDir = "media";
    bool console9 = true;
    Playrate playrate;
    std::vector<Id> masters{kMaster};
    std::uint64_t seed = 0;
    std::uint64_t historyBudgetBytes = 1ULL << 30;
    json::Value render = json::Value::object();  // render.* defaults and matrix
    bool operator==(const Project&) const = default;
};

struct TempoPointObj {
    double beat = 0.0;
    double bpm = 120.0;
    bool ramp = false;  // ramp linearly (in beats) to the next point
};

struct MeterObj {
    int bar = 1;
    int numerator = 4, denominator = 4;
};

enum class Solo : std::uint8_t { Off, On, InPlace, Defeat };
enum class Monitor : std::uint8_t { Off, On, Auto };
enum class RecordMode : std::uint8_t { Input, Output, MidiOverdub, MidiReplace };

struct ParentSend {
    bool enabled = true;
};

struct RecordSettings {
    bool armed = false;
    std::optional<Id> input;  // a route from hw_input/midi_input
    Monitor monitor = Monitor::Off;
    RecordMode mode = RecordMode::Input;
};

enum class StripType : std::uint8_t { Channel, Buss, Master };
std::string_view stripTypeName(StripType);

// Track colours for organizing (gui.md §7): a small named set the GUI draws in
// each theme's tones, or "#rrggbb"; "" = none.
inline constexpr std::array<std::string_view, 8> kTrackColors = {
    "red", "orange", "yellow", "green", "teal", "blue", "violet", "grey"};
bool validTrackColor(std::string_view color);

// A strip's channels at most (ADR-0020, ADR-0021): the main pair and up to
// 126 auxiliary channels (a multi-out instrument's outputs), as REAPER.
inline constexpr int kMaxTrackChannels = 128;
// Device output channels a master can play to (ADR-0056).
inline constexpr int kMaxDeviceOutputs = 64;

// Whole-chain pre-fader render, with original objects retained (ADR-0054).
struct TrackFreeze {
    Id source;
    std::int64_t contentFrames = 0, tailFrames = 0, latencyFrames = 0;
    std::string dependencyHash;
};

// MIDI learn (ADR-0027): a controller message that drives a parameter. The
// device is an input port's name, matched like a MIDI route's (a
// case-insensitive substring; empty: any device).
enum class ControlKind : std::uint8_t { Cc, Note, PitchBend, Program };
// How a CC's value moves the parameter: to the knob's position (absolute),
// by signed steps in one of three encoder conventions (relative 1: 127 = -1,
// 1 = +1; relative 2: 63 = -1, 65 = +1; relative 3: 65 = -1, 1 = +1), or
// flipping it on each press (toggle: a value above 0).
enum class ControlMode : std::uint8_t { Absolute, Relative1, Relative2, Relative3, Toggle };

struct MidiControl {
    ControlKind kind = ControlKind::Cc;
    int channel = 1;  // 1..16
    int number = 0;   // the CC, note or program number (0..127); 0 for pitch bend
    std::string device;
    bool operator==(const MidiControl&) const = default;
};

// MIDI learn on a track's or a send's own controls (ADR-0058): the fader,
// pan and buttons. `param` names the control: volume, pan, mute, solo,
// phase_invert, mono, armed, monitor or lane_mute on a track; volume, pan,
// mute, phase_invert, mono or tap:post_fader|pre_fader|pre_fx on a send.
struct ControlLearn {
    std::string param;
    MidiControl control;
    ControlMode mode = ControlMode::Absolute;
    bool softTakeover = false;  // absolute (a level or pan): follow once the knob reaches it
    bool operator==(const ControlLearn&) const = default;
};

struct Track {
    StripType type = StripType::Channel;
    std::string name, color, notes;
    std::optional<Id> parent;  // empty exactly for masters
    std::vector<Id> children;
    // 1-2: the strip's main pair; 3..kMaxTrackChannels add auxiliary channels
    // (ADR-0020) that routes and plug-in pins address, outside the fader, pan
    // and parent send.
    int channels = 2;
    double volumeDb = 0.0;
    double pan = 0.0;  // % -100..100
    bool mute = false;
    // Polarity and mono switches on the main pair, after the chain and before
    // the fader (ADR-0022).
    bool phaseInvert = false, mono = false;
    Solo solo = Solo::Off;
    ParentSend parentSend;
    // Masters only (ADR-0056): the device output channels the main pair plays
    // to, 1-based: two (left, right), one (mono, (L+R)/2) or none. Unset: the
    // main master plays to 1/2, other masters to none (hardwareOutputs()).
    std::optional<std::vector<int>> hwOutput;
    // Masters only (ADR-0057): silences the master's own items (its prints),
    // nothing else.
    bool laneMute = false;
    RecordSettings record;
    TrackTimebase timebase = TrackTimebase::Project;
    std::vector<Id> chain, inputChain;
    // UI intent (kept in the model, ignored by merges' conflict checks).
    bool locked = false, hiddenTcp = false, hiddenMixer = false;
    std::optional<TrackFreeze> freeze;
    int folderCompact = 0;            // 0 normal, 1 small, 2 collapsed
    std::vector<ControlLearn> learn;  // MIDI learn, one per control (ADR-0058)
};

// A master's device output channels (1-based), with the default resolved.
inline std::vector<int> hardwareOutputs(const Track& track, Id id) {
    if (track.type != StripType::Master) return {};
    if (track.hwOutput) return *track.hwOutput;
    return id == kMaster ? std::vector<int>{1, 2} : std::vector<int>{};
}

// Track edit links and an optional audio-free VCA control (ADR-0052).
inline constexpr std::size_t kMaxVcaControllers = 32;
struct Vca {
    double volumeDb = 0.0;
    bool mute = false, solo = false;
    std::vector<Id> followers;
};
struct TrackGroup {
    std::string name, color;
    bool active = true;
    std::vector<Id> members;
    std::vector<std::string> links{"volume", "mute", "solo"};
    std::optional<Vca> vca;
};

enum class FadeShape : std::uint8_t { Linear, Slow, Fast, SlowFast, FastSlow, Cos, Exp };
struct Fade {
    double length = 0.0;  // seconds
    FadeShape shape = FadeShape::Linear;
    double curve = 0.0;  // % -100..100
};

struct Item {
    Id track = kMaster;
    std::optional<Id> lane;
    double position = 0.0;
    double length = 0.0;
    ItemTimebase timebase = ItemTimebase::Track;
    bool loop = false, mute = false, locked = false;
    double gainDb = 0.0;
    Fade fadeIn, fadeOut;
    std::vector<Id> takes;
    std::optional<Id> activeTake;
    double snapOffset = 0.0;
    std::optional<Id> group;
    std::string color, notes;
};

enum class ChannelMode : std::uint8_t { Normal, ReverseStereo, MonoMix, Left, Right };

struct StretchMarker {
    double sourcePos = 0.0, itemPos = 0.0, slope = 0.0;
    bool operator==(const StretchMarker&) const = default;
};
inline constexpr std::size_t kMaxAudioMarkers = 65536;
struct Transients {
    std::string algorithm = "flux-v1", sourceHash;
    double sensitivity = 0.5;
    std::vector<double> positions;
    bool operator==(const Transients&) const = default;
};
struct TakeMarker {
    double pos = 0.0;
    std::string name, color;
};

struct Take {
    std::string name;
    std::optional<Id> source;  // md_* or mi_*; empty = an empty take
    double startOffset = 0.0;  // seconds into audio, beats into MIDI
    double playrate = 1.0;
    bool preservePitch = true;
    double pitch = 0.0;  // semitones
    std::string pitchMode = "signalsmith";
    double gainDb = 0.0, pan = 0.0;
    bool phaseInvert = false;
    ChannelMode channelMode = ChannelMode::Normal;
    std::vector<StretchMarker> stretchMarkers;
    std::optional<Transients> transients;
    std::vector<Id> warpPeers;
    std::vector<TakeMarker> takeMarkers;
    std::vector<Id> chain;
};

struct MediaSource {
    std::string path;    // relative to the project's media_dir when inside it
    std::string hash;    // "sha256:<hex>" of the file bytes; empty in older projects
    std::string format;  // "wav", "flac", "mp3"
    double sampleRate = 0.0;
    int channels = 0;
    std::int64_t frames = 0;
};

struct MidiSource {
    midi::Sequence sequence;
    bool pooled = false;
};

enum class Isolation : std::uint8_t { Inherit, InProcess, SharedBridge, Dedicated };
enum class PdcMode : std::uint8_t { Compensate, Ignore };

struct PluginRef {
    std::string format = "clap";  // clap | vst3 | lv2 | container
    std::string id, name, vendor, version;
};

// Which of its track's channels (1-based, ADR-0020) a plug-in reads and
// writes: its main input pair, its main output pair, and its sidechain (key)
// input — a detector fed from, say, channels 3/4. Empty: 1/2 in and out, the
// key from key routes only. `out` past its first two entries names a track
// channel for each of the plug-in's auxiliary output channels in order (a
// multi-out instrument's "Kick" on 3/4, ADR-0021); 0 leaves one unconnected.
struct PinMap {
    std::vector<int> in, out, key;
    bool empty() const { return in.empty() && out.empty() && key.empty(); }
};

struct ParamLearn {
    std::string param;  // a semantic key, or a plain parameter's id
    MidiControl control;
    ControlMode mode = ControlMode::Absolute;
    bool softTakeover = false;  // absolute: follow only once the knob reaches the value
    // Enable only while the effect's track is selected, while its window or
    // editor has the focus, or while one of them shows (a window app; a
    // headless engine has neither, so such a binding waits).
    bool whenSelected = false, whenFocused = false, whenVisible = false;
    bool operator==(const ParamLearn&) const = default;
};

struct FxInstance {
    PluginRef plugin;
    PinMap pins;
    std::string label;
    bool bypass = false, offline = false;
    double wet = 100.0;  // %
    Isolation isolation = Isolation::Inherit;
    int oversampling = 1;  // 1, 2, 4, 8
    bool parallel = false;
    PdcMode pdc = PdcMode::Compensate;
    bool autoBypassOnSilence = false;
    // State: the canonical patch (semantic plugins) or a blob reference plus
    // the last known CLAP values (display/diff only; the blob is authoritative).
    std::optional<json::Value> patch;
    std::optional<std::string> state;  // "sha256:<hex>" into Session::blobs
    std::map<std::uint32_t, double> params;
    std::vector<ParamLearn> learn;  // MIDI learn, one per parameter (ADR-0027)
};

enum class Tap : std::uint8_t { PostFader, PreFader, PreFx };

struct RouteEnd {
    std::optional<Id> track;
    std::optional<Id> fx;  // to only: a plug-in's sidechain input (key), ADR-0018
    std::optional<std::vector<int>> hwChannels;  // hw_input (from) / hw_output (to), 1-based
    std::optional<std::string> midiDevice;       // midi_input (from only)
};

struct ChannelMap {
    std::vector<int> from, to;  // 1-based; empty = identity on the first N
};

struct MidiRouting {
    int fromChannel = 0;  // 0 = all
    int toChannel = 0;    // 0 = original
};

// A send follows its source strip's pan unless explicitly positioned.
struct RoutePan {
    std::optional<double> percent;
    RoutePan() = default;
    RoutePan(double value) : percent(value) {}
    double resolve(double stripPan) const { return percent.value_or(stripPan); }
};

struct Route {
    RouteEnd from, to;
    Tap tap = Tap::PostFader;
    double volumeDb = 0.0;
    RoutePan pan;
    bool mute = false, phaseInvert = false, mono = false;
    ChannelMap channels;
    std::optional<MidiRouting> midi;  // empty = audio only
    std::vector<ControlLearn> learn;  // MIDI learn, one per control (ADR-0058)
};

enum class EnvMode : std::uint8_t { Trim, Read, Touch, Latch, LatchPreview, Write };
enum class PointShape : std::uint8_t { Linear, Square, Slow, FastStart, FastEnd, Bezier };

struct EnvPoint {
    double pos = 0.0, value = 0.0;
    PointShape shape = PointShape::Linear;
    double tension = 0.0;
};

struct EnvTarget {
    Id object = kMaster;  // tr_*, sd_*, fx_*, tk_* or a VCA gr_*
    std::string param;    // volume|pan|mute, pitch for takes, playrate on tr_0, or plugin key/id
};

struct Envelope {
    EnvTarget target;
    EnvMode mode = EnvMode::Read;
    bool active = true, armed = false, visible = true;
    Timebase timebase = Timebase::Beats;
    std::vector<EnvPoint> points;  // sorted by pos
};

// An automation item (gap log #28): a block of envelope on its lane that
// replaces the envelope under its span. Its points are in beats from its
// start over `sourceLength` beats, repeated while `loop` and the item is
// longer; values play as baseline + amplitude x (value - baseline). Items with
// the same `pool` (> 0) share their points and source length: an edit to one
// is an edit to all.
struct AutomationItem {
    Id envelope = kMaster;  // en_*: its lane
    double position = 0.0;  // beats
    double length = 4.0;    // beats
    double sourceLength = 4.0;
    bool loop = true;
    int pool = 0;  // 0: its own points
    std::vector<EnvPoint> points;
    double baseline = 0.0, amplitude = 1.0;
    std::string name;
};

struct Marker {
    double pos = 0.0;
    Timebase timebase = Timebase::Beats;
    std::string name, color;
};

struct Region {
    double start = 0.0, end = 0.0;
    Timebase timebase = Timebase::Beats;
    std::string name, color;
};

}  // namespace supple::model
