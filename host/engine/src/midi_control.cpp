// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/midi_control.h"
#include "supple/harmony/harmony.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace supple::engine {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// Middle C (60) is C4. A control is a key on a device, not a pitch in the song: its label uses the
// default table whatever the key or chord (ADR-0100; specs/harmony.md §3.2).
std::string noteName(int key) { return harmony::spell(key); }

}  // namespace

std::optional<model::MidiControl> controlOf(const ControlMidiEvent& e, std::string_view device) {
    model::MidiControl c;
    c.channel = (e.bytes[0] & 0x0f) + 1;
    c.device = std::string(device);
    switch (e.bytes[0] & 0xf0) {
        case 0x80:
        case 0x90:
            c.kind = model::ControlKind::Note;
            c.number = e.bytes[1] & 0x7f;
            return c;
        case 0xb0:
            c.kind = model::ControlKind::Cc;
            c.number = e.bytes[1] & 0x7f;
            return c;
        case 0xc0:
            c.kind = model::ControlKind::Program;
            c.number = e.bytes[1] & 0x7f;
            return c;
        case 0xe0:
            c.kind = model::ControlKind::PitchBend;
            c.number = 0;
            return c;
        default:
            return std::nullopt;
    }
}

bool matches(const model::MidiControl& c, const ControlMidiEvent& e, std::string_view device) {
    const auto heard = controlOf(e, device);
    if (!heard || heard->kind != c.kind || heard->channel != c.channel) return false;
    if (c.kind != model::ControlKind::PitchBend && heard->number != c.number) return false;
    return c.device.empty() || lower(device).find(lower(c.device)) != std::string::npos;
}

ControlReading read(const ControlMidiEvent& e, model::ControlMode mode) {
    ControlReading r;
    const int data1 = e.bytes[1] & 0x7f, data2 = e.bytes[2] & 0x7f;
    switch (e.bytes[0] & 0xf0) {
        case 0x80:
            break;  // a note off: released, at 0
        case 0x90:
            r.value = data2 / 127.0;
            r.press = data2 > 0;
            break;
        case 0xb0:
            r.value = data2 / 127.0;
            r.press = data2 > 0;
            switch (mode) {
                case model::ControlMode::Relative1:  // two's complement: 127 = -1
                    r.relative = true;
                    r.steps = data2 < 64 ? data2 : data2 - 128;
                    break;
                case model::ControlMode::Relative2:  // offset: 64 = 0
                    r.relative = true;
                    r.steps = data2 - 64;
                    break;
                case model::ControlMode::Relative3:  // sign and magnitude: 65 = -1
                    r.relative = true;
                    r.steps = data2 < 64 ? data2 : -(data2 - 64);
                    break;
                default:
                    break;
            }
            break;
        case 0xc0:
            r.value = 1;
            r.press = true;
            break;
        case 0xe0: {
            const int bend = data1 | (data2 << 7);
            r.value = bend / 16383.0;
            r.press = bend > 8192;
            break;
        }
        default:
            break;
    }
    return r;
}

std::string describe(const model::MidiControl& c) {
    const std::string channel = " · ch " + std::to_string(c.channel);
    switch (c.kind) {
        case model::ControlKind::Cc:
            return "CC " + std::to_string(c.number) + channel;
        case model::ControlKind::Note:
            return "Note " + noteName(c.number) + channel;
        case model::ControlKind::PitchBend:
            return "Pitch bend" + channel;
        case model::ControlKind::Program:
            return "Program " + std::to_string(c.number) + channel;
    }
    return {};
}

std::string_view modeName(model::ControlMode mode) {
    switch (mode) {
        case model::ControlMode::Absolute:
            return "absolute";
        case model::ControlMode::Relative1:
            return "relative1";
        case model::ControlMode::Relative2:
            return "relative2";
        case model::ControlMode::Relative3:
            return "relative3";
        case model::ControlMode::Toggle:
            return "toggle";
    }
    return "absolute";
}

std::string_view modeLabel(model::ControlMode mode) {
    switch (mode) {
        case model::ControlMode::Absolute:
            return "Absolute";
        case model::ControlMode::Relative1:
            return "Relative 1 (127 = -1, 1 = +1)";
        case model::ControlMode::Relative2:
            return "Relative 2 (63 = -1, 65 = +1)";
        case model::ControlMode::Relative3:
            return "Relative 3 (65 = -1, 1 = +1)";
        case model::ControlMode::Toggle:
            return "Toggle (above 0 toggles)";
    }
    return "Absolute";
}

std::optional<double> nextValue(double current, const ControlReading& r, model::ControlMode mode,
                                bool softTakeover, double step, Takeover& state) {
    if (r.relative) {
        if (r.steps == 0) return std::nullopt;
        return std::clamp(current + r.steps * step, 0.0, 1.0);
    }
    if (mode == model::ControlMode::Toggle) {
        if (!r.press) return std::nullopt;
        return current >= 0.5 ? 0.0 : 1.0;
    }
    const double target = r.value;
    const std::optional<double> knob = std::exchange(state.knob, target);
    if (!softTakeover) return target;
    constexpr double kNear = 1.0 / 127 + 1e-9;
    const bool held = state.set && std::abs(current - *state.set) < 1e-6;
    const bool near = std::abs(target - current) <= std::max(kNear, step / 2);
    const bool crossed = knob && (*knob - current) * (target - current) <= 0;
    if (!held && !near && !crossed) return std::nullopt;
    return target;
}

}  // namespace supple::engine
