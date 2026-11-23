// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "supple/base/result.h"
#include "supple/json/value.h"
namespace supple::engine {
// Shared current machine-preferences schema for engine and GUI writers.
inline constexpr char kPreferencesSchema[] = "supple.preferences/1.11";
// Whether `label` names a preferences schema newer than kPreferencesSchema: a
// higher minor of 1, or a major above 1. An absent or malformed label is not.
bool preferencesSchemaNewer(std::string_view label);
// Where a newer build's preferences file is kept before this build first
// writes over it: <file>.<label, '/' as '-'>.bak, so
// preferences.json.supple.preferences-1.12.bak.
std::filesystem::path newerPreferencesBackup(const std::filesystem::path& file,
                                             std::string_view label);
// Before every write over `file` (the GUI's preferences, the engine's MIDI
// records): when it holds a newer schema, copy it to newerPreferencesBackup
// once; a backup already there is kept. A file that is absent, does not read
// as a JSON object or is not newer needs none.
Status keepNewerPreferences(const std::filesystem::path& file);
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
