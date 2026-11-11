// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <cctype>
#include <set>
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/json/json.h"
namespace supple::engine {
namespace {
Result<json::Value> preferencesDocument(const std::filesystem::path& file) {
    auto doc = json::Value::object();
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        auto bytes = readFile(file);
        if (!bytes) return bytes.error();
        auto parsed = json::parse(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        if (!parsed) return parsed.error();
        doc = std::move(*parsed);
    }
    if (!doc.isObject())
        return Error("midi.preferences", "preferences must be an object", "/",
                     "repair the preferences file or choose another file");
    return doc;
}
Status writeMidiRecords(const std::filesystem::path& file, json::Value doc,
                        const MidiPortPreferences& inputs, const MidiPortPreferences& outputs) {
    if (!doc.find("schema")) doc["schema"] = kPreferencesSchema;
    doc["midi_inputs"] = midiPortPreferencesJson(inputs);
    doc["midi_outputs"] = midiPortPreferencesJson(outputs);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}
}  // namespace
Status EngineApp::writeMidiPreferences(const std::filesystem::path& file) const {
    auto doc = preferencesDocument(file);
    if (!doc) return doc.error();
    return writeMidiRecords(file, std::move(*doc), midiInputsPreferences_, midiOutputsPreferences_);
}
Status EngineApp::setMidiPreferences(MidiPortPreferences inputs, MidiPortPreferences outputs,
                                     bool save) {
    SUPPLE_TRY(parseMidiPortPreferences(midiPortPreferencesJson(inputs), false));
    SUPPLE_TRY(parseMidiPortPreferences(midiPortPreferencesJson(outputs), true));
    std::optional<json::Value> doc;
    if (save && !options_.midiPreferencesFile.empty()) {
        auto read = preferencesDocument(options_.midiPreferencesFile);
        if (!read) return read.error();
        doc = std::move(*read);
    }
    const auto oldInputs = midiInputsPreferences_, oldOutputs = midiOutputsPreferences_;
    if (engine_) {
        engine_->setMidiPreferences(inputs, outputs);
        auto loaded = engine_->load(head(), projectDir());
        if (!loaded) {
            engine_->setMidiPreferences(oldInputs, oldOutputs);
            return loaded;
        }
    }
    if (doc) {
        auto written =
            writeMidiRecords(options_.midiPreferencesFile, std::move(*doc), inputs, outputs);
        if (!written) {
            if (engine_) {
                engine_->setMidiPreferences(oldInputs, oldOutputs);
                (void)engine_->load(head(), projectDir());
            }
            return written;
        }
    }
    ++midiPreferencesRevision_;
    midiInputsPreferences_ = std::move(inputs);
    midiOutputsPreferences_ = std::move(outputs);
    for (bool output : {false, true}) {
        const auto& preferences = output ? midiOutputsPreferences_ : midiInputsPreferences_;
        std::erase_if(midiPreferencesWarnings_[output ? 1 : 0],
                      [&](const auto& warning) { return preferences.contains(warning.port); });
    }
    return {};
}
Status EngineApp::setMidiPort(bool output, const std::string& name, const json::Value& edits) {
    if (name.empty())
        return Error("midi.port", "a MIDI port name is required", "/port",
                     "use a name from midi.inputs.get or midi.outputs.get");
    const auto state = midiPortsState(output);
    std::set<std::string> names, matches;
    for (const auto& port : state.asArray()) {
        const auto& exact = port.find("name")->asString();
        names.insert(exact);
        if (name.size() == exact.size() && std::equal(name.begin(), name.end(), exact.begin(),
                                                      [](unsigned char x, unsigned char y) {
                                                          return std::tolower(x) == std::tolower(y);
                                                      }))
            matches.insert(exact);
    }
    std::string hint = "Exact port names:";
    for (const auto& exact : names) hint += " \"" + exact + "\"";
    hint += names.empty() ? " none; connect a MIDI device and restart the engine, or load its "
                            "recorded preferences"
                          : "; resolve ambiguous names or remove stale records, then retry";
    if (matches.size() != 1)
        return Error(matches.empty() ? "midi.port_unknown" : "midi.port_ambiguous",
                     matches.empty() ? "unknown MIDI port" : "MIDI port name is ambiguous", "/port",
                     hint);
    if (!edits.isObject())
        return Error("midi.preferences", "port edits must be an object", "/",
                     "use enabled, offset_ms and output reset_on_stop fields");
    const auto& canonical = *matches.begin();
    auto inputs = midiInputsPreferences_, outputs = midiOutputsPreferences_;
    auto& preferences = output ? outputs : inputs;
    auto& record = preferences[canonical];
    if (!record.isObject()) record = json::Value::object();
    for (const auto& entry : edits.asObject()) record[entry.key] = entry.value;
    return setMidiPreferences(std::move(inputs), std::move(outputs));
}
json::Value EngineApp::midiPortsState(bool output) const {
    auto ports = json::Value::array();
    const auto names = backend_ ? (output ? backend_->midiOutputPorts() : backend_->midiPorts())
                                : std::vector<std::string>{};
    const auto latency =
        backend_ && output ? backend_->midiOutputLatency() : std::vector<std::int64_t>{};
    const auto warnings = backend_
                              ? (output ? backend_->midiOutputWarnings() : backend_->midiWarnings())
                              : std::vector<MidiInputWarning>{};
    const auto& preferences = output ? midiOutputsPreferences_ : midiInputsPreferences_;
    const auto& loadWarnings = midiPreferencesWarnings_[output ? 1 : 0];
    const std::string sectionName = output ? "MIDI outputs" : "MIDI inputs";
    std::vector<std::string> all = names;
    for (const auto& warning : loadWarnings) {
        const auto& name = warning.port.empty() ? sectionName : warning.port;
        if (std::find(all.begin(), all.end(), name) == all.end()) all.push_back(name);
    }
    for (const auto& [name, settings] : preferences) {
        (void)settings;
        if (std::find(all.begin(), all.end(), name) == all.end()) all.push_back(name);
    }
    // A refused/busy port remains an addressable header with its startup
    // warning, even though the backend could not give it an open index.
    for (const auto& warning : warnings) {
        if (!warning.device.empty() && warning.device != "MIDI inputs" &&
            warning.device != "MIDI outputs" &&
            std::find(all.begin(), all.end(), warning.device) == all.end())
            all.push_back(warning.device);
    }
    for (const auto& route : view().routes) {
        const auto& device = output ? route.value->to.midiDevice : route.value->from.midiDevice;
        if (!output && device &&
            (device->empty() || *device == "*" || midiPortMask(names, *device)))
            continue;
        if (device && std::none_of(all.begin(), all.end(), [&](const auto& n) {
                return n.size() == device->size() &&
                       std::equal(n.begin(), n.end(), device->begin(),
                                  [](unsigned char x, unsigned char y) {
                                      return std::tolower(x) == std::tolower(y);
                                  });
            }))
            all.push_back(*device);
    }
    for (const auto& name : all) {
        const auto found = std::find(names.begin(), names.end(), name);
        const auto index = static_cast<std::size_t>(found - names.begin());
        const auto settings = midiPortSettings(preferences, name);
        auto port = preferences.contains(name) ? preferences.at(name) : json::Value::object();
        port["name"] = name;
        port["index"] = found == names.end() ? std::int64_t{-1} : static_cast<std::int64_t>(index);
        port["enabled"] = settings.enabled;
        port["offset_ms"] = settings.offsetMs;
        if (output) port["reset_on_stop"] = settings.resetOnStop;
        if (output)
            port["lead_ms"] = engine_
                                  ? double(engine_->midiLeadForOffset(index, settings.offsetMs)) *
                                        1000. / engine_->config().sampleRate
                                  : 0.;
        port["latency_ms"] = output && index < latency.size()
                                 ? double(latency[index]) * 1000. / backend_->config().sampleRate
                                 : 0.;
        port["dropped"] =
            engine_ && found != names.end()
                ? static_cast<std::int64_t>(engine_->midiPortDropped(output, index) +
                                            (output ? backend_->midiOutAsyncDropped(index) : 0))
                : std::int64_t{0};
        port["warnings"] = json::Value::array();
        for (const auto& warning : loadWarnings)
            if (warning.port.empty() || warning.port == name) {
                auto w = json::Value::object();
                w["device"] = warning.port.empty() ? sectionName : warning.port;
                w["os_error"] = 0;
                w["code"] = warning.error.code;
                w["path"] = warning.error.path;
                w["message"] = "Skipped malformed MIDI preference record: " + warning.error.message;
                w["hint"] = warning.error.hint;
                port["warnings"].push_back(std::move(w));
            }
        for (const auto& warning : warnings)
            if (warning.device == name || warning.device == "MIDI outputs" ||
                warning.device == "MIDI inputs") {
                auto w = json::Value::object();
                w["device"] = warning.device;
                w["os_error"] = warning.osError;
                w["message"] = warning.message;
                w["hint"] = warning.hint;
                port["warnings"].push_back(std::move(w));
            }
        if (found == names.end()) {
            auto w = json::Value::object();
            w["device"] = name;
            w["os_error"] = 0;
            w["message"] = "MIDI port is not currently available";
            w["hint"] = "connect the device and explicitly restart the engine";
            port["warnings"].push_back(std::move(w));
        }
        ports.push_back(std::move(port));
    }
    return ports;
}
namespace {
struct PortEdit {
    std::string port;
    std::optional<bool> enabled, reset;
    std::optional<double> offset;
};
}  // namespace
void registerMidiPortCommands(cmd::Registry& registry) {
    for (bool output : {false, true}) {
        const std::string prefix = output ? "midi.outputs" : "midi.inputs";
        cmd::Command<cmd::NoArgs, json::Value> get;
        get.name = prefix + ".get";
        get.kind = cmd::Kind::Query;
        get.exposure = cmd::Exposure::McpTool;
        get.summary = output ? "Hardware MIDI outputs, timing, settings, loss and startup warnings."
                             : "Hardware MIDI inputs, timing, settings, loss and startup warnings.";
        get.description =
            "ports contains name, zero-based index (-1 absent), enabled, offset_ms, latency_ms, "
            "dropped and structured warnings. Outputs also have reset_on_stop. Output lead_ms "
            "is the shared lead added to all audio, including monitoring. Settings are "
            "machine preferences keyed by exact port name; they are not project state.";
        get.tags = {"midi", "engine"};
        get.args = schema::object<cmd::NoArgs>("no_args").build();
        get.result = schema::any();
        get.examples = {
            {"No devices", json::Value::object(), json::parse(R"({"ports":[]})").value(), "empty"}};
        get.run = [output](const cmd::NoArgs&, cmd::Context& ctx) -> Result<json::Value> {
            auto result = json::Value::object();
            result["ports"] = ctx.service<EngineApp>().midiPortsState(output);
            return result;
        };
        registry.add(std::move(get));
        cmd::Command<PortEdit, json::Value> set;
        set.name = prefix + ".set";
        set.kind = cmd::Kind::Action;
        set.exposure = cmd::Exposure::McpTool;
        set.summary = "Change one machine MIDI port's enabled state and timing offset.";
        set.description =
            "port is a unique case-insensitive match to a present or recorded name; settings use "
            "its exact spelling. offset_ms is signed -1000..1000 milliseconds. Positive offsets "
            "delay. Outputs reset_on_stop defaults true. Applying settings releases "
            "disabled/changed output routes; inputs retain the existing channel input mechanism. "
            "Returns the updated ports list.";
        auto fields = schema::object<PortEdit>("midi_port_edit");
        fields.field("port", &PortEdit::port, schema::string().minLength(1));
        fields.field("enabled", &PortEdit::enabled, schema::boolean());
        fields.field("offset_ms", &PortEdit::offset, schema::number().range(-1000, 1000));
        if (output) fields.field("reset_on_stop", &PortEdit::reset, schema::boolean());
        set.args = fields.build();
        set.result = schema::any();
        set.tags = {"midi", "engine"};
        set.examples = {{"Disable a recorded port",
                         json::parse(R"({"port":"Test port","enabled":false})").value(),
                         json::parse(R"({"ports":"<any>"})").value(), "midi_ports"}};
        set.run = [output](const PortEdit& args, cmd::Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto edits = json::Value::object();
            if (args.enabled) edits["enabled"] = *args.enabled;
            if (args.offset) edits["offset_ms"] = *args.offset;
            if (args.reset) edits["reset_on_stop"] = *args.reset;
            SUPPLE_TRY(app.setMidiPort(output, args.port, edits));
            auto result = json::Value::object();
            result["ports"] = app.midiPortsState(output);
            return result;
        };
        registry.add(std::move(set));
    }
}
}  // namespace supple::engine
