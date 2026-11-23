// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/midi_ports.h"
#include <cmath>
#include <optional>
#include <utility>
#include "supple/base/files.h"
#include "supple/json/json.h"
namespace supple::engine {
namespace {
// supple.preferences/<major>.<minor> as numbers, or nothing.
std::optional<std::pair<int, int>> preferencesVersion(std::string_view label) {
    constexpr std::string_view prefix = "supple.preferences/";
    if (!label.starts_with(prefix)) return std::nullopt;
    label.remove_prefix(prefix.size());
    const auto dot = label.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == label.size()) return std::nullopt;
    auto number = [](std::string_view digits) -> std::optional<int> {
        if (digits.size() > 6) return std::nullopt;
        int n = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            n = n * 10 + (c - '0');
        }
        return n;
    };
    const auto major = number(label.substr(0, dot)), minor = number(label.substr(dot + 1));
    if (!major || !minor) return std::nullopt;
    return std::pair{*major, *minor};
}
}  // namespace
bool preferencesSchemaNewer(std::string_view label) {
    const auto mine = preferencesVersion(kPreferencesSchema), theirs = preferencesVersion(label);
    return mine && theirs && *theirs > *mine;
}
std::filesystem::path newerPreferencesBackup(const std::filesystem::path& file,
                                             std::string_view label, int n) {
    std::string tag(label);
    for (char& c : tag)
        if (c == '/' || c == '\\') c = '-';
    auto out = file;
    out += "." + tag + (n > 1 ? "." + std::to_string(n) : std::string()) + ".bak";
    return out;
}
Status keepNewerPreferences(const std::filesystem::path& file, std::string_view ownBytes) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec)) return {};
    auto bytes = readFile(file);
    if (!bytes) return {};  // the write that follows reports what is wrong with the file
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (!ownBytes.empty() && text == ownBytes) return {};  // this writer's own last write
    auto doc = json::parse(text);
    if (!doc || !doc->isObject()) return {};
    const json::Value* schema = doc->find("schema");
    if (!schema || !schema->isString() || !preferencesSchemaNewer(schema->asString())) return {};
    // Never overwrite a backup: each different file the newer version left gets its own name.
    constexpr int kMostBackups = 1000;
    for (int n = 1; n <= kMostBackups; ++n) {
        const auto backup = newerPreferencesBackup(file, schema->asString(), n);
        if (std::filesystem::exists(backup, ec)) {
            if (auto kept = readFile(backup); kept && *kept == *bytes) return {};  // kept already
            continue;
        }
        if (auto written = writeFileAtomic(backup, std::span<const std::uint8_t>(*bytes)); !written)
            return Error("preferences.backup",
                         "cannot keep the newer version's preferences as " + backup.string() +
                             ": " + written.error().message,
                         backup.string(),
                         "free space or allow writing in " + backup.parent_path().string() +
                             ", then change the setting again");
        return {};
    }
    return Error("preferences.backup",
                 "the newer version's preferences already have " + std::to_string(kMostBackups) +
                     " backups beside " + file.string(),
                 file.string(), "move the old .bak files elsewhere, then change the setting again");
}
MidiPortSettings midiPortSettings(const MidiPortPreferences& preferences, std::string_view name) {
    MidiPortSettings result;
    const auto found = preferences.find(name);
    if (found == preferences.end()) return result;
    const auto& record = found->second;
    if (const auto* value = record.find("enabled")) result.enabled = value->asBool();
    if (const auto* value = record.find("offset_ms")) result.offsetMs = value->asDouble();
    if (const auto* value = record.find("reset_on_stop")) result.resetOnStop = value->asBool();
    auto flag = [&](const char* key, bool& out) {
        if (const auto* value = record.find(key)) out = value->asBool();
    };
    flag("clock", result.clock);
    flag("transport", result.transport);
    flag("spp", result.spp);
    flag("mtc", result.mtc);
    flag("clock_while_stopped", result.clockWhileStopped);
    return result;
}
namespace {
bool outputFlag(std::string_view key) {
    return key == "reset_on_stop" || key == "clock" || key == "transport" || key == "spp" ||
           key == "mtc" || key == "clock_while_stopped";
}
}  // namespace
bool midiPortRecordIdle(const json::Value& record) {
    if (!record.isObject()) return false;
    for (const auto& field : record.asObject()) {
        const auto& v = field.value;
        const bool idle = field.key == "enabled" || field.key == "reset_on_stop"
                              ? v.isBool() && v.asBool()
                          : field.key == "offset_ms" ? v.isNumber() && v.asDouble() == 0
                          : outputFlag(field.key)    ? v.isBool() && !v.asBool()
                                                     : false;
        if (!idle) return false;
    }
    return true;
}
Result<MidiPortPreferences> parseMidiPortPreferences(const json::Value& value, bool output) {
    const std::string path = output ? "/midi_outputs" : "/midi_inputs";
    auto fail = [&](const std::string& at) -> Result<MidiPortPreferences> {
        return Error("midi.preferences", "invalid MIDI port settings", at,
                     "use port-name objects with enabled boolean, finite offset_ms in -1000..1000, "
                     "and output booleans reset_on_stop, clock, transport, spp, mtc and "
                     "clock_while_stopped");
    };
    if (!value.isObject()) return fail(path);
    MidiPortPreferences result;
    for (const auto& entry : value.asObject()) {
        if (entry.key.empty() || !entry.value.isObject()) return fail(path);
        for (const auto& field : entry.value.asObject()) {
            if ((field.key == "enabled" || outputFlag(field.key)) && !field.value.isBool())
                return fail(path + "/" + entry.key + "/" + field.key);
            if (field.key == "offset_ms" &&
                (!field.value.isNumber() || !std::isfinite(field.value.asDouble()) ||
                 std::abs(field.value.asDouble()) > 1000))
                return fail(path + "/" + entry.key + "/offset_ms");
            if (!output && outputFlag(field.key))
                return fail(path + "/" + entry.key + "/" + field.key);
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
