// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI outputs' settings (ADR-0072): the per-port record (shared with Phase
// 2's hardware MIDI routing), EngineApp's share (resolving the records
// against the device's outputs for the engine's clock and MTC output, the
// report), and midi.outputs.get / midi.outputs.set.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "command_validation.h"
#include "supple/json/parse.h"

namespace supple::engine {
namespace {

Error outputError(const std::string& field, std::string message, std::string hint) {
    return Error("midi.outputs", std::move(message), "/" + field, std::move(hint));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A port's name without its direction, for telling whether an input and an
// output are the same device: "system:midi_capture_1" and
// "system:midi_playback_1", "Interface MIDI In" and "Interface MIDI Out".
std::string deviceKey(std::string_view name) {
    std::string s = lower(name);
    for (std::string_view word : {"capture", "playback", "midiin", "midiout"})
        for (std::size_t at; (at = s.find(word)) != std::string::npos;) s.erase(at, word.size());
    std::string key, token;
    auto flush = [&] {
        if (!token.empty() && token != "in" && token != "out" && token != "input" &&
            token != "output" && token != "source" && token != "destination") {
            if (!key.empty()) key += ' ';
            key += token;
        }
        token.clear();
    };
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            token += c;
        else
            flush();
    }
    flush();
    return key;
}

bool sameDevice(std::string_view input, std::string_view output) {
    const std::string a = deviceKey(input);
    return !a.empty() && a == deviceKey(output);
}

bool byPort(const MidiOutputSettings& a, const MidiOutputSettings& b) { return a.port < b.port; }

}  // namespace

Status validateMidiOutputSettings(const MidiOutputSettings& s) {
    if (s.port.empty() || s.port.size() > 256)
        return outputError("port", "port is the MIDI output's name, 1 to 256 characters",
                           "use a name from engine.inputs.midi_outputs");
    if (!std::isfinite(s.offsetMs) || s.offsetMs < -1000 || s.offsetMs > 1000)
        return outputError("offset_ms", "offset_ms is from -1000 to 1000",
                           "positive sends the port's MIDI later, negative earlier");
    return {};
}

Result<MidiOutputSettings> parseMidiOutputSettings(const json::Value& value, MidiOutputSettings s) {
    if (!value.isObject())
        return outputError("", "a MIDI output's settings must be an object",
                           "e.g. {\"port\": \"Interface\", \"clock\": true, \"transport\": true, "
                           "\"spp\": true}");
    for (const auto& member : value.asObject()) {
        const std::string& k = member.key;
        const json::Value& v = member.value;
        auto flag = [&](bool& out) -> Status {
            if (!v.isBool()) return outputError(k, k + " must be true or false", "use a boolean");
            out = v.asBool();
            return {};
        };
        if (k == "port") {
            if (!v.isString()) return outputError(k, "port must be a string", "give its name");
            s.port = v.asString();
        } else if (k == "enabled") {
            SUPPLE_TRY(flag(s.enabled));
        } else if (k == "offset_ms") {
            if (!v.isNumber()) return outputError(k, "offset_ms must be a number", "use a number");
            s.offsetMs = v.asDouble();
        } else if (k == "reset_on_stop") {
            SUPPLE_TRY(flag(s.resetOnStop));
        } else if (k == "clock") {
            SUPPLE_TRY(flag(s.clock));
        } else if (k == "transport") {
            SUPPLE_TRY(flag(s.transport));
        } else if (k == "spp") {
            SUPPLE_TRY(flag(s.spp));
        } else if (k == "mtc") {
            SUPPLE_TRY(flag(s.mtc));
        } else if (k == "clock_while_stopped") {
            SUPPLE_TRY(flag(s.clockWhileStopped));
        } else {
            return outputError(k, "unknown MIDI output setting " + k,
                               "the settings are port, enabled, offset_ms, reset_on_stop, clock, "
                               "transport, spp, mtc and clock_while_stopped");
        }
    }
    SUPPLE_TRY(validateMidiOutputSettings(s));
    return s;
}

json::Value midiOutputSettingsJson(const MidiOutputSettings& s, bool withPort) {
    json::Value o = json::Value::object();
    if (withPort) o["port"] = s.port;
    o["enabled"] = s.enabled;
    o["offset_ms"] = s.offsetMs;
    o["reset_on_stop"] = s.resetOnStop;
    o["clock"] = s.clock;
    o["transport"] = s.transport;
    o["spp"] = s.spp;
    o["mtc"] = s.mtc;
    o["clock_while_stopped"] = s.clockWhileStopped;
    return o;
}

Result<std::vector<MidiOutputSettings>> parseMidiOutputList(const json::Value& value) {
    if (!value.isObject())
        return outputError("", "midi_outputs maps each output's name to its settings",
                           "e.g. {\"Interface\": {\"clock\": true}}");
    std::vector<MidiOutputSettings> list;
    for (const auto& member : value.asObject()) {
        if (member.value.isObject() && member.value.find("port"))
            return outputError(member.key + "/port", "the port is the record's key",
                               "remove port from the record");
        MidiOutputSettings base;
        base.port = member.key;
        auto s = parseMidiOutputSettings(member.value, base);
        if (!s) {
            Error e = std::move(s).error();
            e.path = "/" + member.key + e.path;
            return e;
        }
        if (!s->idle()) list.push_back(std::move(*s));
    }
    std::sort(list.begin(), list.end(), byPort);
    return list;
}

json::Value midiOutputListJson(const std::vector<MidiOutputSettings>& list) {
    json::Value o = json::Value::object();
    for (const MidiOutputSettings& s : list) o[s.port] = midiOutputSettingsJson(s);
    return o;
}

// --- EngineApp ---------------------------------------------------------------

Status EngineApp::setMidiOutputSettings(const MidiOutputSettings& settings) {
    SUPPLE_TRY(validateMidiOutputSettings(settings));
    auto list = midiOutputSettings_;
    std::erase_if(list, [&](const MidiOutputSettings& s) { return s.port == settings.port; });
    if (!settings.idle()) list.push_back(settings);
    std::sort(list.begin(), list.end(), byPort);
    midiOutputSettings_ = std::move(list);
    applySync();
    return {};
}

Status EngineApp::setMidiOutputSettings(std::vector<MidiOutputSettings> list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        SUPPLE_TRY(validateMidiOutputSettings(list[i]));
        for (std::size_t k = 0; k < i; ++k)
            if (list[k].port == list[i].port)
                return outputError("port", "port " + list[i].port + " is listed twice",
                                   "keep one record per MIDI output");
    }
    std::erase_if(list, [](const MidiOutputSettings& s) { return s.idle(); });
    std::sort(list.begin(), list.end(), byPort);
    midiOutputSettings_ = std::move(list);
    applySync();
    return {};
}

void EngineApp::resolveMidiOutputs(SyncConfig& c, std::vector<std::string>* warnings,
                                   std::vector<std::vector<std::string>>* perPort) const {
    if (!engine_) return;
    const std::vector<std::string> ports = engine_->midiOutputPorts();
    const std::vector<std::int64_t> latency = engine_->midiOutputLatency();
    const double rate = engine_->config().sampleRate;
    if (perPort) perPort->assign(ports.size(), {});
    // The sync master's device: what it sends is not echoed back to it.
    std::string master;
    if ((c.source == SyncSource::MidiClock || c.source == SyncSource::Mtc) && c.midiPort >= 0) {
        const auto inputs = engine_->midiPorts();
        if (static_cast<std::size_t>(c.midiPort) < inputs.size())
            master = inputs[static_cast<std::size_t>(c.midiPort)];
    }
    for (const MidiOutputSettings& s : midiOutputSettings_) {
        const auto found = std::find(ports.begin(), ports.end(), s.port);
        if (found == ports.end()) continue;  // not connected: the report says so
        const auto i = static_cast<std::size_t>(found - ports.begin());
        if (i >= kMaxSyncOutputs) continue;
        auto warn = [&](const std::string& text) {
            if (warnings) warnings->push_back(text);
            if (perPort) (*perPort)[i].push_back(text);
        };
        SyncOutput& out = c.outputs[i];
        if (!s.enabled) continue;  // a disabled port sends nothing
        out.clock = s.clock;
        out.transport = s.transport;
        out.spp = s.spp;
        out.mtc = s.mtc;
        out.whileStopped = s.clockWhileStopped;
        out.delay =
            (i < latency.size() ? latency[i] : 0) + std::llround(s.offsetMs / 1000.0 * rate);
        if (!master.empty() && sameDevice(master, ports[i])) {
            const bool clockMaster = c.source == SyncSource::MidiClock;
            const bool echoes = clockMaster ? (out.clock || out.transport || out.spp) : out.mtc;
            if (clockMaster) out.clock = out.transport = out.spp = out.whileStopped = false;
            if (!clockMaster) out.mtc = false;
            if (echoes)
                warn("midi.outputs: " + ports[i] + " is the sync master's device (" + master +
                     "), so its " + (clockMaster ? "clock" : "MTC") +
                     " is not sent back to it; send it to another output");
        }
    }
}

std::vector<MidiOutputReport> EngineApp::midiOutputReport() {
    std::vector<MidiOutputReport> reports;
    const std::vector<std::string> ports =
        engine_ ? engine_->midiOutputPorts() : std::vector<std::string>{};
    const std::vector<std::int64_t> latency =
        engine_ ? engine_->midiOutputLatency() : std::vector<std::int64_t>{};
    const double rate = engine_ ? engine_->config().sampleRate : 48000.0;
    SyncConfig c = engine_ ? engine_->syncConfig() : SyncConfig{};
    SyncConfig fresh = c;
    fresh.outputs = {};
    std::vector<std::vector<std::string>> perPort;
    resolveMidiOutputs(fresh, nullptr, &perPort);
    auto record = [&](const std::string& name) {
        for (const MidiOutputSettings& s : midiOutputSettings_)
            if (s.port == name) return s;
        MidiOutputSettings s;
        s.port = name;
        return s;
    };
    auto fill = [&](const MidiOutputSettings& s, int index) {
        MidiOutputReport m;
        m.name = s.port;
        m.index = index;
        m.enabled = s.enabled;
        m.offsetMs = s.offsetMs;
        m.resetOnStop = s.resetOnStop;
        m.clock = s.clock;
        m.transport = s.transport;
        m.spp = s.spp;
        m.mtc = s.mtc;
        m.clockWhileStopped = s.clockWhileStopped;
        if (index < 0) return m;
        const auto i = static_cast<std::size_t>(index);
        const std::int64_t frames = i < latency.size() ? latency[i] : 0;
        m.latencyMs = static_cast<double>(frames) * 1000.0 / rate;
        if (i < perPort.size()) m.warnings = perPort[i];
        if (i >= kMaxSyncOutputs) return m;
        const SyncOutput& out = c.outputs[i];
        auto kind = [&](bool wanted, bool sent, const char* name) {
            if (wanted && sent) m.sends.emplace_back(name);
            if (wanted && !sent && s.enabled) m.suppressed.emplace_back(name);
        };
        kind(s.clock, out.clock, "clock");
        kind(s.transport, out.transport, "transport");
        kind(s.spp, out.spp, "spp");
        kind(s.mtc, out.mtc, "mtc");
        kind(s.clock && s.clockWhileStopped, out.clock && out.whileStopped, "clock_while_stopped");
        m.delayMs = static_cast<double>(static_cast<std::int64_t>(engine_->outputLatency()) +
                                        (out.sends() ? out.delay : frames)) *
                    1000.0 / rate;
        m.sent = static_cast<std::int64_t>(engine_->syncOutputSent(i));
        m.dropped = static_cast<std::int64_t>(engine_->syncOutputDropped(i));
        return m;
    };
    for (std::size_t i = 0; i < ports.size(); ++i)
        reports.push_back(fill(record(ports[i]), static_cast<int>(i)));
    for (const MidiOutputSettings& s : midiOutputSettings_)
        if (std::find(ports.begin(), ports.end(), s.port) == ports.end())
            reports.push_back(fill(s, -1));
    return reports;
}

// --- commands ----------------------------------------------------------------

namespace {
struct NoArgs {};
struct OutputsInfo {
    std::vector<MidiOutputReport> ports;
};

schema::Schema<MidiOutputReport> reportSchema() {
    return schema::object<MidiOutputReport>("midi_output")
        .field("name", &MidiOutputReport::name, schema::string())
        .field("index", &MidiOutputReport::index, schema::integerOf<int>())
        .field("enabled", &MidiOutputReport::enabled, schema::boolean())
        .field("offset_ms", &MidiOutputReport::offsetMs, schema::number())
        .field("reset_on_stop", &MidiOutputReport::resetOnStop, schema::boolean())
        .field("latency_ms", &MidiOutputReport::latencyMs, schema::number())
        .field("dropped", &MidiOutputReport::dropped, schema::integer())
        .field("warnings", &MidiOutputReport::warnings, schema::arrayOf(schema::string()))
        .field("clock", &MidiOutputReport::clock, schema::boolean())
        .field("transport", &MidiOutputReport::transport, schema::boolean())
        .field("spp", &MidiOutputReport::spp, schema::boolean())
        .field("mtc", &MidiOutputReport::mtc, schema::boolean())
        .field("clock_while_stopped", &MidiOutputReport::clockWhileStopped, schema::boolean())
        .field("sends", &MidiOutputReport::sends, schema::arrayOf(schema::string()))
        .field("suppressed", &MidiOutputReport::suppressed, schema::arrayOf(schema::string()))
        .field("delay_ms", &MidiOutputReport::delayMs, schema::number())
        .field("sent", &MidiOutputReport::sent, schema::integer())
        .build();
}

schema::Schema<OutputsInfo> infoSchema() {
    return schema::object<OutputsInfo>("midi_outputs_info")
        .field("ports", &OutputsInfo::ports, schema::arrayOf(reportSchema()))
        .build();
}

json::Value parsed(std::string_view text) { return json::parse(text).value(); }
}  // namespace

schema::Schema<MidiOutputReport> midiOutputStatusSchema() { return reportSchema(); }

void registerMidiOutputCommands(cmd::Registry& registry) {
    const auto settingsDoc =
        "Per MIDI output (port: its name in engine.inputs.midi_outputs): enabled (false: the port "
        "sends nothing); offset_ms (-1000..1000) sends the port's MIDI later (negative: earlier); "
        "reset_on_stop (hardware MIDI routing); clock sends MIDI clock (24 per quarter note) while "
        "the transport plays; transport sends Start, Continue and Stop; spp sends Song Position "
        "before Continue and on locates (a start between 16ths continues from the next one); mtc "
        "sends MIDI time code at the project's timecode; clock_while_stopped keeps clocking at "
        "the tempo while stopped. Clock and MTC leave with the audio they belong to: after the "
        "plan's latency, the output's audio-minus-MIDI latency and the offset. Nothing is sent "
        "during a count-in.";
    const auto reportDoc =
        " ports: each output the running device has (index into engine.inputs.midi_outputs), "
        "then saved records for outputs it lacks (index -1), with the settings, latency_ms "
        "(audio minus MIDI), warnings, what it sends now, what it does not send back to the sync "
        "master (suppressed), delay_ms (plan latency + latency + offset), and the clock and MTC "
        "messages sent and dropped since the engine started.";
    [&] {
        cmd::Command<NoArgs, OutputsInfo> c;
        c.name = "midi.outputs.get";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Show each MIDI output's settings and what it sends (clock, SPP, MTC).";
        c.description = std::string(settingsDoc) + reportDoc;
        c.tags = {"transport", "sync", "midi"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = infoSchema();
        c.examples = {{"No device running", parsed("{}"), parsed(R"({"ports": []})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<OutputsInfo> {
            return OutputsInfo{ctx.service<EngineApp>().midiOutputReport()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        cmd::Command<json::Value, OutputsInfo> c;
        c.name = "midi.outputs.set";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set a MIDI output's settings: clock, transport, SPP, MTC, offset.";
        c.description = std::string(settingsDoc) +
                        " Omitted members keep their values; a record back at every default is "
                        "removed. Machine settings: not an undo step, not saved in the project "
                        "(the app keeps them with its preferences)." +
                        reportDoc;
        c.tags = {"transport", "sync", "midi"};
        json::Value spec = json::Value::object();
        spec["type"] = "object";
        spec["properties"] = parsed(R"({
            "port": {"type": "string", "minLength": 1, "maxLength": 256},
            "enabled": {"type": "boolean"},
            "offset_ms": {"type": "number", "minimum": -1000, "maximum": 1000},
            "reset_on_stop": {"type": "boolean"},
            "clock": {"type": "boolean"},
            "transport": {"type": "boolean"},
            "spp": {"type": "boolean"},
            "mtc": {"type": "boolean"},
            "clock_while_stopped": {"type": "boolean"}})");
        spec["required"] = parsed(R"(["port"])");
        spec["additionalProperties"] = false;
        c.args = schema::Schema<json::Value>(
            "midi_output_settings",
            [](const json::Value& v, json::Value& out, schema::DecodeCtx& ctx,
               const std::string& p) {
                if (v.isObject() && !v.find("port")) {
                    ctx.error("midi.outputs", "port is required", p + "/port",
                              "name the MIDI output, e.g. {\"port\": \"Interface\", \"clock\": "
                              "true}");
                    return false;
                }
                auto checked = parseMidiOutputSettings(v);
                if (!checked) {
                    const Error& e = checked.error();
                    ctx.error(e.code, e.message, p + e.path, e.hint);
                    return false;
                }
                out = v;
                return true;
            },
            [](const json::Value& v) { return v; }, spec);
        c.result = infoSchema();
        c.examples = {
            {"Clock, transport and Song Position to an interface, stored while it is away",
             parsed(R"({"port": "Interface", "clock": true, "transport": true, "spp": true})"),
             parsed(R"({"ports": [{"name": "Interface", "index": -1, "enabled": true,
                        "offset_ms": 0.0, "reset_on_stop": true, "latency_ms": 0.0,
                        "dropped": 0, "warnings": [], "clock": true, "transport": true,
                        "spp": true, "mtc": false, "clock_while_stopped": false, "sends": [],
                        "suppressed": [], "delay_ms": 0.0, "sent": 0}]})"),
             "empty"}};
        c.run = [](const json::Value& a, cmd::Context& ctx) -> Result<OutputsInfo> {
            auto& app = ctx.service<EngineApp>();
            MidiOutputSettings base;
            if (const json::Value* port = a.find("port"); port && port->isString())
                for (const MidiOutputSettings& s : app.midiOutputSettings())
                    if (s.port == port->asString()) base = s;
            auto next = parseMidiOutputSettings(a, base);
            if (!next) return std::move(next).error();
            SUPPLE_TRY(app.setMidiOutputSettings(*next));
            OutputsInfo info{app.midiOutputReport()};
            for (const MidiOutputReport& r : info.ports) {
                if (r.name != next->port) continue;
                for (const auto& w : r.warnings) ctx.warn("midi.outputs.warning", w, "");
                // Only a running device has outputs to miss.
                if (app.backend() && r.index < 0 && !next->idle())
                    ctx.warn("midi.outputs.warning",
                             "midi.outputs: no MIDI output is named \"" + next->port +
                                 "\" now; it applies when one is (engine.inputs lists them)",
                             "");
            }
            return info;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
