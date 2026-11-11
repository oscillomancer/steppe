// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <map>
#include <string>
#include <vector>
#include "supple/base/result.h"
#include "supple/json/value.h"
namespace supple::engine {
// Shared current machine-preferences schema for engine and GUI writers.
inline constexpr char kPreferencesSchema[] = "supple.preferences/1.6";
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
struct MidiPortPreferenceWarning {
    std::string port;  // empty: invalid whole input/output section
    Error error;
    json::Value raw;       // retained unchanged on writes until explicitly corrected
    bool section = false;  // raw is an invalid whole section, not one record
};
// Startup only: retain valid records and report each skipped record. Command
// edits use the strict parser above and still refuse malformed settings.
MidiPortPreferences loadMidiPortPreferences(const json::Value&, bool output,
                                            std::vector<MidiPortPreferenceWarning>& warnings);
json::Value midiPortPreferencesJson(const MidiPortPreferences&);
}  // namespace supple::engine
