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
inline constexpr char kPreferencesSchema[] = "supple.preferences/1.8";
// Machine preferences, keyed by the backend's immutable port name. The JSON
// record is retained, so members a later version adds survive edits. One
// record per port carries everything it does: routing (ADR-0072) owns
// enabled, offset_ms and reset_on_stop (offset_ms also moves clock and MTC);
// clock output (ADR-0073) adds clock, transport, spp, mtc and
// clock_while_stopped, all outputs only.
struct MidiPortSettings {
    bool enabled = true;
    double offsetMs = 0;
    bool resetOnStop = true;
    bool clock = false;              // MIDI clock (F8), 24 per quarter note
    bool transport = false;          // Start, Continue, Stop
    bool spp = false;                // Song Position Pointer before Continue and on locates
    bool mtc = false;                // MIDI time code at the project's timecode
    bool clockWhileStopped = false;  // keep clocking at the tempo while stopped
    bool operator==(const MidiPortSettings&) const = default;
};
// Whether a record is at every default (and has no other member): it can go.
bool midiPortRecordIdle(const json::Value& record);
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
