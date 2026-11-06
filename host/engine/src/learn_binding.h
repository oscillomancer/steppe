// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What MIDI learn on plug-in parameters (learn.cpp, ADR-0027) and on strips',
// sends' and VCAs' controls (control_learn.cpp, ADR-0058) share: a binding's
// command arguments, and moving a target with the messages heard for it.
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "supple/engine/app.h"
#include "supple/engine/midi_control.h"
#include "supple/model/schemas.h"

namespace supple::engine {

// A *.learn.set command's midi, mode and soft_takeover.
struct LearnBindingArgs {
    model::MidiControl midi;
    std::optional<model::ControlMode> mode;
    std::optional<bool> softTakeover;

    // The binding they describe: a pitch bend has no number; absolute and
    // no soft takeover unless given.
    model::ControlBinding binding() const {
        model::ControlBinding b;
        b.control = midi;
        if (b.control.kind == model::ControlKind::PitchBend) b.control.number = 0;
        b.mode = mode.value_or(model::ControlMode::Absolute);
        b.softTakeover = softTakeover.value_or(false);
        return b;
    }
};

// LearnBindingArgs' members on a command's argument schema (T extends it).
template <class T>
schema::ObjectBuilder<T>& learnBindingArgFields(schema::ObjectBuilder<T>& b) {
    model::MidiControl T::* midi = &T::midi;
    std::optional<model::ControlMode> T::* mode = &T::mode;
    std::optional<bool> T::* softTakeover = &T::softTakeover;
    b.field("midi", midi, model::midiControlSchema());
    b.field("mode", mode, model::controlModeSchema());
    b.field("soft_takeover", softTakeover, schema::boolean());
    return b;
}

// A *.learn.set command's result.
struct LearnSetResult {
    std::string param, control;
};

// Whether any of the messages is the control's.
inline bool heardControl(const std::vector<EngineApp::ControlEvent>& events,
                         const model::MidiControl& control) {
    return std::any_of(events.begin(), events.end(),
                       [&](const auto& e) { return matches(control, e.midi, e.device); });
}

// Moves a target with every message heard for `binding`, in order: relative
// steps add up, the last absolute position wins. `move` is the target's
// moveParam or moveControl, given the function from its current value and
// step to its next; `value` gives one message's next value (none: it leaves
// it). Soft takeover's memory follows what was set.
template <class Move, class Value>
void driveLearned(const std::vector<EngineApp::ControlEvent>& events,
                  const model::ControlBinding& binding, Takeover& takeover, Move&& move,
                  Value&& value) {
    const std::optional<double> set = move([&](double current, double step) {
        std::optional<double> next;
        for (const EngineApp::ControlEvent& e : events) {
            if (!matches(binding.control, e.midi, e.device)) continue;
            if (auto v = value(current, read(e.midi, binding.mode), step)) {
                current = *v;
                takeover.set = current;
                next = current;
            }
        }
        return next;
    });
    if (set) takeover.set = *set;
}

}  // namespace supple::engine
