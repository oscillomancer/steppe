// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/rack/host_modules.h"

#include <algorithm>
#include <cmath>

namespace supple::rack::host {

namespace {

using Kind = sd::ParamModel::Kind;
using Unit = sd::ParamModel::Unit;
using Signal = sd::PortModel::Signal;

sd::ParamModel param(const char* key, const char* name, Kind kind, double min, double max,
                     double def, Unit unit, const char* help) {
    sd::ParamModel p;
    p.key = key;
    p.member = key;
    p.id = sd::keyId(key);
    p.kind = kind;
    p.name = name;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    p.description = help;
    return p;
}

sd::ParamModel level(const char* help) {
    sd::ParamModel p = param("level", "Level", Kind::Continuous, -60, 12, 0, Unit::Decibels, help);
    p.smoothingMs = 10;
    p.offAtMinimum = true;
    return p;
}

sd::ParamModel choice(const char* key, const char* name, std::vector<std::string> labels,
                      std::vector<std::string> keys, int def, const char* help) {
    sd::ParamModel p = param(key, name, Kind::Choice, 0, static_cast<double>(labels.size() - 1),
                             def, Unit::None, help);
    p.choices = std::move(labels);
    p.choiceKeys = std::move(keys);
    return p;
}

sd::PortModel port(const char* key, const char* name, Signal signal, bool output,
                   const char* help) {
    sd::PortModel p;
    p.key = key;
    p.member = key;
    p.name = name;
    p.signal = signal;
    p.output = output;
    p.description = help;
    return p;
}

sd::ModuleInfoModel info(std::string_view id, const char* name, const char* description,
                         std::vector<std::string> tags) {
    sd::ModuleInfoModel i;
    i.id = std::string(id);
    i.name = name;
    i.vendor = "Supple Audio";
    i.description = description;
    i.tags = std::move(tags);
    return i;
}

double gainOf(double db) { return db <= -60.0 ? 0.0 : std::pow(10.0, db / 20.0); }

// Divisions of the Transport's clock, in quarter notes.
struct Division {
    const char* label;
    const char* key;
    double beats;
};
constexpr Division kDivisions[] = {
    {"1/1", "whole", 4.0},
    {"1/2", "half", 2.0},
    {"1/4", "quarter", 1.0},
    {"1/8", "eighth", 0.5},
    {"1/16", "sixteenth", 0.25},
    {"1/32", "thirty_second", 0.125},
    {"1/64", "sixty_fourth", 0.0625},
    {"1/2 dotted", "dotted_half", 3.0},
    {"1/4 dotted", "dotted_quarter", 1.5},
    {"1/8 dotted", "dotted_eighth", 0.75},
    {"1/16 dotted", "dotted_sixteenth", 0.375},
    {"1/2 triplet", "half_triplet", 4.0 / 3.0},
    {"1/4 triplet", "quarter_triplet", 2.0 / 3.0},
    {"1/8 triplet", "eighth_triplet", 1.0 / 3.0},
    {"1/16 triplet", "sixteenth_triplet", 1.0 / 6.0},
};

}  // namespace

std::shared_ptr<const ModuleType> outType() {
    static const auto t = ModuleType::host(
        info(kOut, "Out", "The rack's audio to the DAW: a channel takes it as its input.", {"io"}),
        {level("The level the channel receives.")},
        {port("l", "L", Signal::Audio, false, "Left, or both sides while R is not connected."),
         port("r", "R", Signal::Audio, false, "Right.")});
    return t;
}

std::shared_ptr<const ModuleType> inType() {
    static const auto t =
        ModuleType::host(info(kIn, "In", "Audio from the DAW: the tracks that send to it.", {"io"}),
                         {level("The level of what the sends bring.")},
                         {port("l", "L", Signal::Audio, true, "The sends' left side."),
                          port("r", "R", Signal::Audio, true, "The sends' right side.")});
    return t;
}

std::shared_ptr<const ModuleType> notesType() {
    static const auto t = ModuleType::host(
        info(kNotes, "Notes",
             "MIDI to poly pitch, gate, velocity and pressure: the home track's notes, or "
             "another track's.",
             {"io", "midi", "poly"}),
        {param("voices", "Voices", Kind::Integer, 1, 16, 8, Unit::None,
               "How many notes sound at once: the poly outputs' channels."),
         choice("mode", "Mode", {"Rotate", "Reuse", "Reset", "Mono Last", "Mono Low", "Mono High"},
                {"rotate", "reuse", "reset", "mono_last", "mono_low", "mono_high"}, 0,
                "How notes take channels: the next free in turn, the one that last played the key, "
                "the lowest free, or one channel for the latest, lowest or highest key held."),
         param("bend_range", "Bend Range", Kind::Continuous, 0, 24, 2, Unit::Semitones,
               "How far the pitch bend moves the pitch outputs.")},
        {port("pitch", "Pitch", Signal::Pitch, true,
              "Each voice's key, 1 per octave, 0 at C4, with the bend."),
         port("gate", "Gate", Signal::Gate, true,
              "1 while each voice's key is held (or the pedal holds it); drops for a frame when a "
              "voice is retaken."),
         port("velocity", "Velocity", Signal::Unipolar, true, "Each voice's velocity, 0 to 1."),
         port("pressure", "Pressure", Signal::Unipolar, true,
              "Each voice's pressure: poly aftertouch, or channel pressure."),
         port("retrigger", "Retrigger", Signal::Gate, true, "A trigger as each note starts."),
         port("bend", "Bend", Signal::Cv, true, "The pitch bend, -1 to 1."),
         port("modwheel", "Mod Wheel", Signal::Unipolar, true, "The mod wheel (CC 1), 0 to 1.")});
    return t;
}

std::shared_ptr<const ModuleType> notesOutType() {
    static const auto t = ModuleType::host(
        info(kNotesOut, "Note Out", "Poly pitch and gates play another track's instrument.",
             {"io", "midi", "poly"}),
        {param("channel", "Channel", Kind::Integer, 1, 16, 1, Unit::None,
               "The MIDI channel of new notes, 1 to 16.")},
        {port("pitch", "Pitch", Signal::Pitch, false,
              "Each voice's pitch: 0 is C4, 1 per octave. A mono pitch serves every voice."),
         port("gate", "Gate", Signal::Gate, false,
              "Rising to 0.5 starts a note; falling below 0.25 ends it."),
         port("velocity", "Velocity", Signal::Unipolar, false,
              "Velocity at a gate's rise, 0 to 1; unconnected: MIDI velocity 96.")});
    return t;
}

int noteKey(double pitch) SUPPLE_RT {
    if (std::isnan(pitch)) pitch = 0.0;
    return static_cast<int>(std::lround(std::clamp(60.0 + 12.0 * pitch, 0.0, 127.0)));
}
int noteVelocity(double value, bool connected) SUPPLE_RT {
    if (!connected) return 96;
    if (std::isnan(value)) value = 0.0;
    return static_cast<int>(std::lround(std::clamp(127.0 * value, 1.0, 127.0)));
}
void NotesOut::prepare(Instance&) {
    voices_ = {};
    endAll_ = false;
}
void NotesOut::send(std::uint32_t frame, int channel, int key, int velocity, bool on,
                    int voice) SUPPLE_RT {
    if (!count_ || *count_ >= events_.size()) return;
    events_[(*count_)++] = {{frame, static_cast<std::uint8_t>((on ? 0x90 : 0x80) | channel),
                             static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity)},
                            voice};
}
std::array<NotesOut::State, kChannels> NotesOut::held() const SUPPLE_RT {
    std::array<State, kChannels> result{};
    for (int c = 0; c < kChannels; ++c) {
        const auto& v = voices_[static_cast<std::size_t>(c)];
        result[static_cast<std::size_t>(c)] = {v.gate.high, v.key, v.channel, v.velocity};
    }
    return result;
}
void NotesOut::process(const HostContext& ctx) SUPPLE_RT {
    const auto& pitch = ctx.instance->inputMember(0);
    const auto& gate = ctx.instance->inputMember(1);
    const auto& velocity = ctx.instance->inputMember(2);
    if (endAll_ || !ctx.transport.playing) {
        for (int c = 0; c < kChannels; ++c) {
            auto& v = voices_[static_cast<std::size_t>(c)];
            if (v.gate.high)
                send(static_cast<std::uint32_t>(ctx.offset), v.channel, v.key, 0, false, c);
            v.gate.high = false;
        }
        endAll_ = false;
    }
    if (!ctx.transport.playing) return;
    const int pitches = std::max(1, pitch.channels()),
              velocities = std::max(1, velocity.channels());
    for (int i = 0; i < ctx.frames; ++i)
        for (int c = 0; c < kChannels; ++c) {
            auto& v = voices_[static_cast<std::size_t>(c)];
            if (!v.gate.step(c < gate.channels() ? gate[c][i] : 0.0)) continue;
            const bool on = v.gate.high;
            if (on) {
                v.key = noteKey(pitch[std::min(c, pitches - 1)][i]);
                double channel = ctx.instance->paramMember(0).at(i, c);
                if (!std::isfinite(channel)) channel = 1.0;
                v.channel = static_cast<int>(std::lround(std::clamp(channel, 1.0, 16.0))) - 1;
            }
            const int vel =
                on ? noteVelocity(velocity[std::min(c, velocities - 1)][i], velocity.connected())
                   : 0;
            if (on) v.velocity = vel;
            send(static_cast<std::uint32_t>(ctx.offset + i), v.channel, v.key, vel, on, c);
        }
}

std::shared_ptr<const ModuleType> transportType() {
    static const auto t = [] {
        std::vector<std::string> labels, keys;
        for (const Division& d : kDivisions) {
            labels.emplace_back(d.label);
            keys.emplace_back(d.key);
        }
        sd::ParamModel swing = param("swing", "Swing", Kind::Continuous, 0, 75, 0, Unit::Percent,
                                     "How late every second clock comes, as a share of a step.");
        return ModuleType::host(
            info(kTransport, "Transport",
                 "The DAW's timeline as gates, clocks and phases: a patch in time with the song.",
                 {"io", "clock"}),
            {choice("division", "Division", labels, keys, 4, "How often the clock output fires."),
             swing},
            {port("playing", "Playing", Signal::Gate, true, "1 while the transport plays."),
             port("reset", "Reset", Signal::Gate, true,
                  "A trigger when playback starts or jumps: restart sequences with it."),
             port("bar", "Bar", Signal::Gate, true, "A trigger at each bar."),
             port("beat", "Beat", Signal::Gate, true, "A trigger at each beat of the meter."),
             port("clock", "Clock", Signal::Gate, true, "A trigger at each division, swung."),
             port("bar_phase", "Bar Phase", Signal::Phase, true, "0 to 1 through each bar."),
             port("beat_phase", "Beat Phase", Signal::Phase, true, "0 to 1 through each beat."),
             port("tempo", "Tempo", Signal::Cv, true, "The tempo: BPM / 300.")});
    }();
    return t;
}

std::unique_ptr<Instance> make(std::shared_ptr<const ModuleType> type,
                               const Instance::Setup& setup) {
    if (!type) return nullptr;
    std::unique_ptr<HostModule> host;
    if (type->id() == kOut)
        host = std::make_unique<Out>();
    else if (type->id() == kIn)
        host = std::make_unique<In>();
    else if (type->id() == kNotes)
        host = std::make_unique<Notes>();
    else if (type->id() == kNotesOut)
        host = std::make_unique<NotesOut>();
    else if (type->id() == kTransport)
        host = std::make_unique<Transport>();
    else
        return nullptr;
    return Instance::make(std::move(type), std::move(host), setup);
}

// --- Out and In -------------------------------------------------------------------

void Out::process(const HostContext& ctx) SUPPLE_RT {
    if (!left_ || !right_) return;
    Instance& in = *ctx.instance;
    const script::Input& l = in.inputMember(0);
    const script::Input& r = in.inputMember(1);
    const script::Param& lv = in.paramMember(0);
    const bool stereo = r.connected();
    for (int i = 0; i < ctx.frames; ++i) {
        double a = 0.0, b = 0.0;
        for (int c = 0; c < l.channels(); ++c) a += l[c][i];
        if (stereo)
            for (int c = 0; c < r.channels(); ++c) b += r[c][i];
        else
            b = a;
        const double g = gainOf(lv.at(i));
        left_[ctx.offset + i] += static_cast<float>(a * g);
        right_[ctx.offset + i] += static_cast<float>(b * g);
    }
}

void In::process(const HostContext& ctx) SUPPLE_RT {
    Instance& in = *ctx.instance;
    script::Output& l = in.outputMember(0);
    script::Output& r = in.outputMember(1);
    l.setChannels(1);
    r.setChannels(1);
    if (!left_ || !right_) return;
    const script::Param& lv = in.paramMember(0);
    for (int i = 0; i < ctx.frames; ++i) {
        const double g = gainOf(lv.at(i));
        l[0][i] = static_cast<double>(left_[ctx.offset + i]) * g;
        r[0][i] = static_cast<double>(right_[ctx.offset + i]) * g;
    }
}

// --- Notes --------------------------------------------------------------------------

void Notes::prepare(Instance& instance) {
    voice_ = {};
    stacked_ = 0;
    bend_ = modwheel_ = channelPressure_ = 0;
    sustain_ = endAll_ = false;
    rotate_ = 0;
    triggerFrames_ =
        std::max(1, static_cast<int>(std::lround(instance.setup().sampleRate * 0.001)));
}

void Notes::release(Voice& v) {
    if (sustain_ && v.held) v.sustained = true;
    v.held = false;
}

void Notes::on(int key, double velocity, int voices, int mode) {
    keyVelocity_[static_cast<std::size_t>(key)] = velocity;
    if (mode >= 3) {  // mono: the stack of keys held
        for (int i = 0; i < stacked_; ++i)
            if (stack_[static_cast<std::size_t>(i)] == key) {
                std::copy(stack_.begin() + i + 1, stack_.begin() + stacked_, stack_.begin() + i);
                --stacked_;
                break;
            }
        if (stacked_ < 128) stack_[static_cast<std::size_t>(stacked_++)] = key;
        int chosen = key;
        if (mode == 4)
            for (int i = 0; i < stacked_; ++i)
                chosen = std::min(chosen, stack_[static_cast<std::size_t>(i)]);
        if (mode == 5)
            for (int i = 0; i < stacked_; ++i)
                chosen = std::max(chosen, stack_[static_cast<std::size_t>(i)]);
        Voice& v = voice_[0];
        if (chosen != v.key || !(v.held || v.sustained)) {
            const bool legato = v.held || v.sustained;
            v.key = chosen;
            v.velocity = keyVelocity_[static_cast<std::size_t>(chosen)];
            v.trigger = triggerFrames_;
            if (!legato) v.pressure = 0;
        }
        v.held = true;
        v.sustained = false;
        v.age = ++clock_;
        return;
    }
    int chosen = -1;
    if (mode == 1)  // reuse: the voice that played this key
        for (int c = 0; c < voices; ++c)
            if (voice_[static_cast<std::size_t>(c)].key == key) chosen = c;
    if (chosen < 0) {
        const int start = mode == 0 ? rotate_ % voices : 0;
        for (int k = 0; k < voices && chosen < 0; ++k) {
            const int c = (start + k) % voices;
            const Voice& v = voice_[static_cast<std::size_t>(c)];
            if (!v.held && !v.sustained) chosen = c;
        }
    }
    if (chosen < 0) {  // all busy: the oldest
        std::uint64_t oldest = UINT64_MAX;
        for (int c = 0; c < voices; ++c)
            if (voice_[static_cast<std::size_t>(c)].age < oldest) {
                oldest = voice_[static_cast<std::size_t>(c)].age;
                chosen = c;
            }
    }
    rotate_ = (chosen + 1) % voices;
    Voice& v = voice_[static_cast<std::size_t>(chosen)];
    if (v.held || v.sustained) v.gap = 1;  // retaken: the gate drops for a frame
    v.key = key;
    v.held = true;
    v.sustained = false;
    v.velocity = velocity;
    v.pressure = 0;
    v.age = ++clock_;
    v.trigger = triggerFrames_;
}

void Notes::off(int key, int mode) {
    if (mode >= 3) {
        for (int i = 0; i < stacked_; ++i)
            if (stack_[static_cast<std::size_t>(i)] == key) {
                std::copy(stack_.begin() + i + 1, stack_.begin() + stacked_, stack_.begin() + i);
                --stacked_;
                break;
            }
        Voice& v = voice_[0];
        if (stacked_ == 0) {
            if (v.key == key) release(v);
            return;
        }
        int chosen = stack_[static_cast<std::size_t>(stacked_ - 1)];
        if (mode == 4)
            for (int i = 0; i < stacked_; ++i)
                chosen = std::min(chosen, stack_[static_cast<std::size_t>(i)]);
        if (mode == 5)
            for (int i = 0; i < stacked_; ++i)
                chosen = std::max(chosen, stack_[static_cast<std::size_t>(i)]);
        v.key = chosen;  // legato back to a key still held
        return;
    }
    for (Voice& v : voice_)
        if (v.key == key && v.held) release(v);
}

void Notes::handle(const MidiEvent& e, int voices, int mode) {
    const int type = e.status & 0xF0;
    const int key = e.data1 & 0x7F;
    if (type == 0x90 && e.data2 > 0) {
        on(key, (e.data2 & 0x7F) / 127.0, voices, mode);
    } else if (type == 0x80 || type == 0x90) {
        off(key, mode);
    } else if (type == 0xA0) {
        for (Voice& v : voice_)
            if (v.key == key && (v.held || v.sustained)) v.pressure = (e.data2 & 0x7F) / 127.0;
    } else if (type == 0xB0) {
        if (key == 1) modwheel_ = (e.data2 & 0x7F) / 127.0;
        if (key == 64) {
            sustain_ = e.data2 >= 64;
            if (!sustain_)
                for (Voice& v : voice_) v.sustained = false;
        }
        if (key == 120 || key == 123) endAll_ = true;
    } else if (type == 0xD0) {
        channelPressure_ = (e.data1 & 0x7F) / 127.0;
    } else if (type == 0xE0) {
        const int value = ((e.data2 & 0x7F) << 7) | (e.data1 & 0x7F);
        bend_ = std::clamp((value - 8192) / 8192.0, -1.0, 1.0);
    }
}

void Notes::process(const HostContext& ctx) SUPPLE_RT {
    Instance& in = *ctx.instance;
    const int voices = std::clamp(in.paramMember(0).index(), 1, kChannels);
    const int mode = std::clamp(in.paramMember(1).index(), 0, 5);
    const double range = in.paramMember(2).value();
    const int poly = mode >= 3 ? 1 : voices;
    script::Output& pitch = in.outputMember(0);
    script::Output& gate = in.outputMember(1);
    script::Output& velocity = in.outputMember(2);
    script::Output& pressure = in.outputMember(3);
    script::Output& retrigger = in.outputMember(4);
    script::Output& bend = in.outputMember(5);
    script::Output& modwheel = in.outputMember(6);
    for (script::Output* o : {&pitch, &gate, &velocity, &pressure, &retrigger})
        o->setChannels(poly);
    bend.setChannels(1);
    modwheel.setChannels(1);
    std::size_t next = 0;
    while (next < events_.size() && events_[next].frame < static_cast<std::uint32_t>(ctx.offset))
        ++next;
    for (int i = 0; i < ctx.frames; ++i) {
        if (endAll_) {
            for (Voice& v : voice_) v.held = v.sustained = false;
            stacked_ = 0;
            endAll_ = false;
        }
        while (next < events_.size() &&
               events_[next].frame <= static_cast<std::uint32_t>(ctx.offset + i))
            handle(events_[next++], voices, mode);
        for (int c = 0; c < poly; ++c) {
            Voice& v = voice_[static_cast<std::size_t>(c)];
            const double key = v.key < 0 ? 60.0 : static_cast<double>(v.key);
            pitch[c][i] = (key - 60.0) / 12.0 + bend_ * range / 12.0;
            gate[c][i] = (v.held || v.sustained) && v.gap == 0 ? 1.0 : 0.0;
            if (v.gap > 0) --v.gap;
            velocity[c][i] = v.velocity;
            pressure[c][i] = std::max(v.pressure, channelPressure_);
            retrigger[c][i] = v.trigger > 0 ? 1.0 : 0.0;
            if (v.trigger > 0) --v.trigger;
        }
        bend[0][i] = bend_;
        modwheel[0][i] = modwheel_;
    }
}

// --- Transport ------------------------------------------------------------------------

void Transport::prepare(Instance& instance) {
    expected_ = 0;
    wasPlaying_ = started_ = false;
    lastTicks_ = lastBar_ = lastBeat_ = -1;
    reset_ = bar_ = beat_ = clock_ = 0;
    triggerFrames_ =
        std::max(1, static_cast<int>(std::lround(instance.setup().sampleRate * 0.001)));
}

void Transport::process(const HostContext& ctx) SUPPLE_RT {
    Instance& in = *ctx.instance;
    const script::Transport& t = ctx.transport;
    const int division =
        std::clamp(in.paramMember(0).index(), 0, static_cast<int>(std::size(kDivisions)) - 1);
    const double step = kDivisions[division].beats;
    const double swing = std::clamp(in.paramMember(1).value(), 0.0, 0.75);
    const double rate = in.setup().sampleRate;
    const double perFrame = t.playing ? t.tempo / 60.0 / rate : 0.0;
    const double beatLen = 4.0 / std::max(1, t.timeSigDenominator);
    const double barLen = std::max(1, t.timeSigNumerator) * beatLen;
    script::Output* outs[8];
    for (int k = 0; k < 8; ++k) {
        outs[k] = &in.outputMember(k);
        outs[k]->setChannels(1);
    }
    const bool jumped =
        t.playing && wasPlaying_ && std::abs(t.beats - expected_) > 1e-9 * (1 + std::abs(t.beats));
    if (t.playing && (!wasPlaying_ || jumped)) {
        reset_ = triggerFrames_;
        lastTicks_ = lastBar_ = lastBeat_ = -1;
    }
    for (int i = 0; i < ctx.frames; ++i) {
        const double beat = t.beats + i * perFrame;
        const double inBar = beat - t.barStart;
        double barPhase = std::fmod(inBar, barLen) / barLen;
        if (barPhase < 0) barPhase += 1.0;
        double beatPhase = std::fmod(inBar, beatLen) / beatLen;
        if (beatPhase < 0) beatPhase += 1.0;
        if (t.playing) {
            // A count that rises at each event: fire when it moves, or on the
            // first frame when playback starts on the event itself.
            const double bars = std::floor(inBar / barLen), beats = std::floor(inBar / beatLen);
            const double k = std::floor(beat / step);
            const double frac = beat / step - k;
            const bool odd = std::fmod(k, 2.0) != 0.0;
            const double ticks = k + (frac >= (odd ? swing : 0.0) ? 1.0 : 0.0);
            const double eps = perFrame * 0.5;
            if (lastBar_ < 0 ? barPhase * barLen < eps : bars != lastBar_) bar_ = triggerFrames_;
            if (lastBeat_ < 0 ? beatPhase * beatLen < eps : beats != lastBeat_)
                beat_ = triggerFrames_;
            if (lastTicks_ < 0 ? (!odd && frac * step < eps) : ticks != lastTicks_)
                clock_ = triggerFrames_;
            lastBar_ = bars;
            lastBeat_ = beats;
            lastTicks_ = ticks;
        }
        (*outs[0])[0][i] = t.playing ? 1.0 : 0.0;
        (*outs[1])[0][i] = reset_ > 0 ? 1.0 : 0.0;
        (*outs[2])[0][i] = bar_ > 0 ? 1.0 : 0.0;
        (*outs[3])[0][i] = beat_ > 0 ? 1.0 : 0.0;
        (*outs[4])[0][i] = clock_ > 0 ? 1.0 : 0.0;
        (*outs[5])[0][i] = barPhase;
        (*outs[6])[0][i] = beatPhase;
        (*outs[7])[0][i] = t.tempo / 300.0;
        for (int* c : {&reset_, &bar_, &beat_, &clock_})
            if (*c > 0) --*c;
    }
    expected_ = t.beats + ctx.frames * perFrame;
    wasPlaying_ = t.playing;
}

}  // namespace supple::rack::host
