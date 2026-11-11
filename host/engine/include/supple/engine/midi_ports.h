// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <map>
#include <string>
#include "supple/base/result.h"
#include "supple/json/value.h"
namespace supple::engine {
// Machine preferences, keyed by the backend's immutable port name. The JSON
// record is retained so subsequent clock/sync fields survive common edits.
struct MidiPortSettings {
    bool enabled = true;
    double offsetMs = 0;
    bool resetOnStop = true;
};
using MidiPortPreferences = std::map<std::string, json::Value, std::less<>>;
MidiPortSettings midiPortSettings(const MidiPortPreferences&, std::string_view name);
Result<MidiPortPreferences> parseMidiPortPreferences(const json::Value&, bool output);
json::Value midiPortPreferencesJson(const MidiPortPreferences&);
}  // namespace supple::engine
