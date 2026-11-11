// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/midi_ports.h"
#include <cmath>
namespace supple::engine {
MidiPortSettings midiPortSettings(const MidiPortPreferences& preferences, std::string_view name) {
    MidiPortSettings result;
    const auto found = preferences.find(name);
    if (found == preferences.end()) return result;
    const auto& record = found->second;
    if (const auto* value = record.find("enabled")) result.enabled = value->asBool();
    if (const auto* value = record.find("offset_ms")) result.offsetMs = value->asDouble();
    if (const auto* value = record.find("reset_on_stop")) result.resetOnStop = value->asBool();
    return result;
}
Result<MidiPortPreferences> parseMidiPortPreferences(const json::Value& value, bool output) {
    const std::string path = output ? "/midi_outputs" : "/midi_inputs";
    auto fail = [&](const std::string& at) -> Result<MidiPortPreferences> {
        return Error("midi.preferences", "invalid MIDI port settings", at,
                     "use port-name objects with enabled boolean, finite offset_ms in -1000..1000, "
                     "and output reset_on_stop boolean");
    };
    if (!value.isObject()) return fail(path);
    MidiPortPreferences result;
    for (const auto& entry : value.asObject()) {
        if (entry.key.empty() || !entry.value.isObject()) return fail(path);
        for (const auto& field : entry.value.asObject()) {
            if ((field.key == "enabled" || field.key == "reset_on_stop") && !field.value.isBool())
                return fail(path + "/" + entry.key + "/" + field.key);
            if (field.key == "offset_ms" &&
                (!field.value.isNumber() || !std::isfinite(field.value.asDouble()) ||
                 std::abs(field.value.asDouble()) > 1000))
                return fail(path + "/" + entry.key + "/offset_ms");
            if (!output && field.key == "reset_on_stop")
                return fail(path + "/" + entry.key + "/reset_on_stop");
        }
        result.emplace(entry.key, entry.value);
    }
    return result;
}
MidiPortPreferences loadMidiPortPreferences(const json::Value& value, bool output,
                                            std::vector<MidiPortPreferenceWarning>& warnings) {
    MidiPortPreferences result;
    if (!value.isObject()) {
        auto parsed = parseMidiPortPreferences(value, output);
        warnings.push_back({"", parsed.error(), value, true});
        return result;
    }
    for (const auto& entry : value.asObject()) {
        auto single = json::Value::object();
        single[entry.key] = entry.value;
        auto parsed = parseMidiPortPreferences(single, output);
        if (parsed)
            result.insert(parsed->begin(), parsed->end());
        else
            warnings.push_back({entry.key, parsed.error(), entry.value});
    }
    return result;
}
json::Value midiPortPreferencesJson(const MidiPortPreferences& preferences) {
    auto result = json::Value::object();
    for (const auto& [name, value] : preferences) result[name] = value;
    return result;
}
}  // namespace supple::engine
