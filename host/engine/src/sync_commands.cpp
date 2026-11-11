// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// External sync (ADR-0071): the settings, EngineApp's main-thread share
// (resolving the settings for the engine and acting on what it hears), and
// sync.get / sync.set.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include "command_validation.h"
#include "supple/json/parse.h"
#include "supple/sync/chase.h"

namespace supple::engine {
namespace {

Error settingsError(const std::string& field, std::string message, std::string hint) {
    return Error("sync.settings", std::move(message), "/" + field, std::move(hint));
}

std::optional<SyncSource> sourceOf(std::string_view name) {
    for (SyncSource s : {SyncSource::Off, SyncSource::MidiClock, SyncSource::Mtc, SyncSource::Ltc})
        if (syncSourceName(s) == name) return s;
    return std::nullopt;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view stateName(sync::ChaseState s) {
    switch (s) {
        case sync::ChaseState::Idle:
            return "idle";
        case sync::ChaseState::Acquiring:
            return "acquiring";
        case sync::ChaseState::Relocating:
            return "relocating";
        case sync::ChaseState::Chasing:
            return "chasing";
        case sync::ChaseState::Locked:
            return "locked";
    }
    return "idle";
}

}  // namespace

Status validateSyncSettings(const SyncSettings& s) {
    if (!sourceOf(s.source))
        return settingsError("source", "source is off, midi_clock, mtc or ltc",
                             "choose one of off, midi_clock, mtc and ltc");
    if (s.ltcInput < 1 || s.ltcInput > 64)
        return settingsError("ltc_input", "ltc_input is a device input channel from 1 to 64",
                             "use the input channel the LTC cable is on, 1-based");
    if (!std::isfinite(s.offsetMs) || s.offsetMs < -10000 || s.offsetMs > 10000)
        return settingsError(
            "offset_ms", "offset_ms is from -10000 to 10000",
            "use a small offset in milliseconds; positive plays behind the master");
    if (s.onLoss != "stop" && s.onLoss != "freewheel")
        return settingsError("on_loss", "on_loss is stop or freewheel",
                             "stop stops the transport when the signal stops; freewheel keeps "
                             "going for freewheel_frames first");
    if (s.freewheelFrames < 1 || s.freewheelFrames > 300)
        return settingsError("freewheel_frames", "freewheel_frames is from 1 to 300",
                             "the timecode frames to keep playing after the signal stops");
    if (s.ltcOutputChannel < 1 || s.ltcOutputChannel > 64)
        return settingsError("ltc_output_channel",
                             "ltc_output_channel is a device output channel from 1 to 64",
                             "use a free output channel (not one a master plays to), 1-based");
    if (!std::isfinite(s.ltcLevelDb) || s.ltcLevelDb < -60 || s.ltcLevelDb > 0)
        return settingsError("ltc_level_db", "ltc_level_db is from -60 to 0",
                             "-18 dBFS is the usual line-up level");
    return {};
}

Result<SyncSettings> parseSyncSettings(const json::Value& value, SyncSettings s) {
    if (!value.isObject())
        return settingsError("", "sync settings must be an object",
                             "e.g. {\"source\": \"mtc\", \"midi_input\": \"Interface\"}");
    for (const auto& member : value.asObject()) {
        const std::string& k = member.key;
        const json::Value& v = member.value;
        auto text = [&](std::string& out) -> Status {
            if (!v.isString())
                return settingsError(k, k + " must be a string", "give it as a string");
            out = v.asString();
            return {};
        };
        auto flag = [&](bool& out) -> Status {
            if (!v.isBool()) return settingsError(k, k + " must be true or false", "use a boolean");
            out = v.asBool();
            return {};
        };
        auto integer = [&](int& out) -> Status {
            if (!v.isNumber() || !std::isfinite(v.asDouble()) ||
                v.asDouble() != std::floor(v.asDouble()) || std::abs(v.asDouble()) > 1e6)
                return settingsError(k, k + " must be a whole number", "use an integer");
            out = static_cast<int>(v.asDouble());
            return {};
        };
        auto number = [&](double& out) -> Status {
            if (!v.isNumber()) return settingsError(k, k + " must be a number", "use a number");
            out = v.asDouble();
            return {};
        };
        if (k == "source")
            SUPPLE_TRY(text(s.source));
        else if (k == "midi_input")
            SUPPLE_TRY(text(s.midiInput));
        else if (k == "ltc_input")
            SUPPLE_TRY(integer(s.ltcInput));
        else if (k == "follow_tempo")
            SUPPLE_TRY(flag(s.followTempo));
        else if (k == "offset_ms")
            SUPPLE_TRY(number(s.offsetMs));
        else if (k == "on_loss")
            SUPPLE_TRY(text(s.onLoss));
        else if (k == "freewheel_frames")
            SUPPLE_TRY(integer(s.freewheelFrames));
        else if (k == "ltc_output")
            SUPPLE_TRY(flag(s.ltcOutput));
        else if (k == "ltc_output_channel")
            SUPPLE_TRY(integer(s.ltcOutputChannel));
        else if (k == "ltc_level_db")
            SUPPLE_TRY(number(s.ltcLevelDb));
        else
            return settingsError(k, "unknown sync setting " + k,
                                 "the settings are source, midi_input, ltc_input, follow_tempo, "
                                 "offset_ms, on_loss, freewheel_frames, ltc_output, "
                                 "ltc_output_channel and ltc_level_db");
    }
    SUPPLE_TRY(validateSyncSettings(s));
    return s;
}

json::Value syncSettingsJson(const SyncSettings& s) {
    json::Value o = json::Value::object();
    o["source"] = s.source;
    o["midi_input"] = s.midiInput;
    o["ltc_input"] = s.ltcInput;
    o["follow_tempo"] = s.followTempo;
    o["offset_ms"] = s.offsetMs;
    o["on_loss"] = s.onLoss;
    o["freewheel_frames"] = s.freewheelFrames;
    o["ltc_output"] = s.ltcOutput;
    o["ltc_output_channel"] = s.ltcOutputChannel;
    o["ltc_level_db"] = s.ltcLevelDb;
    return o;
}

// --- EngineApp ---------------------------------------------------------------

Status EngineApp::setSyncSettings(const SyncSettings& settings) {
    SUPPLE_TRY(validateSyncSettings(settings));
    syncSettings_ = settings;
    applySync();
    return {};
}

SyncConfig EngineApp::syncConfigFor(std::vector<std::string>* warnings) const {
    SyncConfig c;
    const SyncSettings& s = syncSettings_;
    c.source = sourceOf(s.source).value_or(SyncSource::Off);
    c.timecode = head().project->timecode;
    c.followTempo = s.followTempo;
    c.offsetSeconds = s.offsetMs / 1000.0;
    c.freewheelSeconds =
        s.onLoss == "freewheel" ? sync::toSeconds(s.freewheelFrames, c.timecode.rate) : 0.0;
    c.ltcLevelDb = s.ltcLevelDb;
    if (!engine_) return c;
    const BackendConfig device = engine_->config();
    if (c.source == SyncSource::MidiClock || c.source == SyncSource::Mtc) {
        // The port named exactly, else the first whose name contains it.
        const std::vector<std::string> ports = engine_->midiPorts();
        for (std::size_t i = 0; i < ports.size() && c.midiPort < 0; ++i)
            if (ports[i] == s.midiInput) c.midiPort = static_cast<int>(i);
        for (std::size_t i = 0; i < ports.size() && c.midiPort < 0; ++i)
            if (!s.midiInput.empty() &&
                lower(ports[i]).find(lower(s.midiInput)) != std::string::npos)
                c.midiPort = static_cast<int>(i);
        if (c.midiPort < 0 && warnings)
            warnings->push_back(s.midiInput.empty()
                                    ? "sync: choose the MIDI input the master is on (sync.set "
                                      "{midi_input})"
                                    : "sync: no MIDI input is named \"" + s.midiInput +
                                          "\"; engine.devices lists them");
        // A disabled input feeds nothing, the chase included (ADR-0072's
        // port settings); its offset_ms is for played and recorded MIDI,
        // the chase has its own (sync.set offset_ms, ADR-0071).
        if (c.midiPort >= 0 &&
            !midiPortSettings(midiInputsPreferences_, ports[static_cast<std::size_t>(c.midiPort)])
                 .enabled) {
            if (warnings)
                warnings->push_back("sync: the MIDI input \"" +
                                    ports[static_cast<std::size_t>(c.midiPort)] +
                                    "\" is disabled, so nothing is followed; enable it with "
                                    "midi.inputs.set {enabled: true}");
            c.midiPort = -1;
        }
    }
    if (c.source == SyncSource::Ltc) {
        if (static_cast<std::uint32_t>(s.ltcInput) <= device.inputs)
            c.ltcInput = s.ltcInput - 1;
        else if (warnings)
            warnings->push_back("sync: the device has " + std::to_string(device.inputs) +
                                " inputs, so LTC input " + std::to_string(s.ltcInput) +
                                " is silent; start the engine with its inputs");
    }
    if (s.ltcOutput) {
        // The stored channel, else the device's last output (the setting is
        // kept for a device that has it).
        if (static_cast<std::uint32_t>(s.ltcOutputChannel) <= device.outputs)
            c.ltcOutput = s.ltcOutputChannel - 1;
        else if (device.outputs > 0)
            c.ltcOutput = static_cast<int>(device.outputs) - 1;
        if (warnings && c.ltcOutput < 0)
            warnings->push_back("sync: the device has no outputs, so LTC output is not written");
        else if (warnings && c.ltcOutput != s.ltcOutputChannel - 1)
            warnings->push_back("sync: the device has " + std::to_string(device.outputs) +
                                " outputs, so LTC goes to output " +
                                std::to_string(c.ltcOutput + 1) + " instead of " +
                                std::to_string(s.ltcOutputChannel) +
                                "; choose another with sync.set {ltc_output_channel}");
        // The channel is the LTC's: a master playing to it is overwritten.
        const int channel = c.ltcOutput + 1;
        if (warnings && c.ltcOutput >= 0)
            for (model::Id id : head().project->masters)
                if (const model::Track* m = head().tracks.find(id)) {
                    const auto out = model::hardwareOutputs(*m, id);
                    if (std::find(out.begin(), out.end(), channel) != out.end())
                        warnings->push_back("sync: " + (m->name.empty() ? id.str() : m->name) +
                                            " plays to output " + std::to_string(channel) +
                                            ", which LTC output replaces; move one of them");
                }
    }
    (void)resolveMidiOutputs(c, warnings);  // MIDI clock and MTC output (ADR-0073)
    return c;
}

void EngineApp::applySync() {
    syncWarnings_.clear();
    const SyncConfig c = syncConfigFor(&syncWarnings_);
    if (!engine_) return;
    if (auto set = engine_->setSync(c); !set) syncWarnings_.push_back(set.error().toString());
}

SyncReport EngineApp::syncReport() {
    SyncReport r;
    r.source = syncSettings_.source;
    r.warnings = syncWarnings_;
    if (!engine_) return r;
    const SyncConfig& c = engine_->syncConfig();
    const SyncStatus s = engine_->syncStatus();
    if (c.midiPort >= 0) {
        const auto ports = engine_->midiPorts();
        if (static_cast<std::size_t>(c.midiPort) < ports.size())
            r.input = ports[static_cast<std::size_t>(c.midiPort)];
    } else if (c.ltcInput >= 0) {
        r.input = "input " + std::to_string(c.ltcInput + 1);
    }
    r.present = s.present;
    r.running = s.running;
    r.locked = s.locked;
    r.state = std::string(stateName(static_cast<sync::ChaseState>(s.chase)));
    r.bpm = s.bpm;
    if (s.source == SyncSource::Mtc || s.source == SyncSource::Ltc) {
        if (s.present) r.timecode = sync::format(s.timecode, s.rate);
        if (s.present) r.rate = std::string(sync::rateName(s.rate));
    }
    r.chaseRate = engine_->syncRate();
    r.errorMs = s.errorSeconds * 1000.0;
    r.locateMs = syncSeekSeconds_ * 1000.0;
    r.ltcOutput = s.ltcOutput;
    return r;
}

void EngineApp::serviceSync() {
    if (!engine_) return;
    // The project's timecode or the MIDI ports may have changed: setSync does
    // nothing when the configuration is the same.
    applySync();
    const SyncStatus s = engine_->syncStatus();
    const SyncStatus was = std::exchange(syncSeen_, s);
    const SyncConfig& c = engine_->syncConfig();
    if (c.source == SyncSource::Off || s.source != c.source) {
        engine_->setSyncRate(1.0);
        return;
    }
    // A new runtime starts its counts afresh: nothing changed for the master.
    if (s.runtime != was.runtime) return;
    const TransportState t = engine_->transport();
    const auto timing = [&](auto&& action) {
        const auto began = std::chrono::steady_clock::now();
        action();
        const double took =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        syncSeekSeconds_ = std::clamp(0.5 * syncSeekSeconds_ + 0.5 * took, 0.0, 0.5);
    };
    const double ahead = syncSeekSeconds_ * std::max(s.externalRate, 0.25);
    auto locateAndPlay = [&](double seconds) {
        timing([&] {
            (void)transport("seek", std::max(0.0, seconds + ahead));
            if (!engine_->transport().playing) (void)transport("play");
        });
    };
    const model::Session session = view();
    if (c.source == SyncSource::MidiClock) {
        const auto beatSeconds = [&](double beat) {
            return session.timeMap->secondsAt(std::max(beat, 0.0)) - c.offsetSeconds;
        };
        if (s.stops != was.stops && t.playing) {
            (void)transport("pause");
        } else if (s.running && !was.running && !t.playing) {
            // The first clock after Start or Continue: play from the beat the
            // master will be at when the audio is heard, ahead by the seek's
            // measured time and the output latency at the master's tempo (the
            // project's until a second clock gives one). Without follow tempo
            // nothing would take a late start in afterwards.
            const double bpm =
                s.bpm > 0 ? s.bpm : session.timeMap->bpmAt(std::max(s.position, 0.0));
            const double lead = (syncSeekSeconds_ + s.heardLatency) * bpm / 60.0;
            timing([&] {
                (void)transport("seek", std::max(0.0, beatSeconds(s.position + lead)));
                (void)transport("play");
            });
        } else if (!s.running && s.jumps != was.jumps && !t.playing) {
            (void)transport("seek", std::max(0.0, beatSeconds(s.songPosition)));  // SPP stopped
        } else if (c.followTempo && t.playing && s.relocates != was.relocates) {
            timing([&] { (void)transport("seek", std::max(0.0, s.relocateSeconds + ahead)); });
        }
    } else {
        if (s.running && !t.playing && !(was.running && s.losses != was.losses)) {
            locateAndPlay(s.relocateSeconds);
        } else if (t.playing && s.direction < 0) {
            (void)transport("pause");  // the master runs backwards: Steppe stops
        } else if (t.playing && s.relocates != was.relocates) {
            timing([&] { (void)transport("seek", std::max(0.0, s.relocateSeconds + ahead)); });
        } else if (c.source == SyncSource::Mtc && !t.playing && !s.running &&
                   s.jumps != was.jumps) {
            // MTC's full frame: the master located while stopped. It names a
            // position, with no quarter frames to track.
            (void)transport(
                "seek", std::max(0.0, s.position - c.timecode.startSeconds() - c.offsetSeconds));
        }
    }
    if (s.losses != was.losses && engine_->transport().playing) (void)transport("pause");
    // The chase rate, while the transport plays with the master.
    engine_->setSyncRate(engine_->transport().playing ? s.chaseRate : 1.0);
}

// --- commands ----------------------------------------------------------------

namespace {
struct NoArgs {};
struct SyncInfo {
    json::Value settings;
    SyncReport status;
};

schema::Schema<SyncReport> syncReportSchema() {
    return schema::object<SyncReport>("sync_status")
        .field("source", &SyncReport::source, schema::string())
        .field("input", &SyncReport::input, schema::string())
        .field("present", &SyncReport::present, schema::boolean())
        .field("running", &SyncReport::running, schema::boolean())
        .field("locked", &SyncReport::locked, schema::boolean())
        .field("state", &SyncReport::state, schema::string())
        .field("bpm", &SyncReport::bpm, schema::number())
        .field("timecode", &SyncReport::timecode, schema::string())
        .field("rate", &SyncReport::rate, schema::string())
        .field("chase_rate", &SyncReport::chaseRate, schema::number())
        .field("error_ms", &SyncReport::errorMs, schema::number())
        .field("locate_ms", &SyncReport::locateMs, schema::number())
        .field("ltc_output", &SyncReport::ltcOutput, schema::boolean())
        .field("warnings", &SyncReport::warnings, schema::arrayOf(schema::string()))
        .build();
}

schema::Schema<SyncInfo> syncInfoSchema() {
    return schema::object<SyncInfo>("sync_info")
        .field("settings", &SyncInfo::settings, schema::any())
        .field("status", &SyncInfo::status, syncReportSchema())
        .build();
}

json::Value parsed(std::string_view text) { return json::parse(text).value(); }
}  // namespace

schema::Schema<SyncReport> syncStatusSchema() { return syncReportSchema(); }

void registerSyncCommands(cmd::Registry& registry) {
    const auto settingsDoc =
        "source: off, midi_clock, mtc or ltc. midi_input: the MIDI input port the master's clock "
        "or MTC arrives on (its name, or a part of it). ltc_input: the device input channel "
        "carrying LTC (1-based). follow_tempo (midi_clock): chase the master's tempo with the live "
        "playrate (pitch preserved); off follows Start, Continue, Stop and Song Position only. "
        "offset_ms: positive plays Steppe that much behind the master. on_loss: stop, or "
        "freewheel for freewheel_frames timecode frames, when the signal stops. ltc_output: "
        "write LTC of the transport (the project's timecode) to ltc_output_channel, replacing "
        "whatever a master plays there, at ltc_level_db.";
    [&] {
        cmd::Command<NoArgs, SyncInfo> c;
        c.name = "sync.get";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Show external sync settings and what the engine hears from the master.";
        c.description = std::string(settingsDoc) +
                        " status: present (the signal arrives), running, locked, state (idle, "
                        "acquiring, relocating, chasing, locked), the master's bpm (midi_clock) "
                        "or timecode and rate (mtc, ltc), chase_rate (the live playrate the "
                        "chase applies on top of the saved one) and error_ms.";
        c.tags = {"transport", "sync"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = syncInfoSchema();
        c.examples = {{"Off", parsed("{}"),
                       parsed(R"({"settings": {"source": "off", "midi_input": "", "ltc_input": 1,
                                  "follow_tempo": true, "offset_ms": 0.0, "on_loss": "stop",
                                  "freewheel_frames": 25, "ltc_output": false,
                                  "ltc_output_channel": 3, "ltc_level_db": -18.0},
                                  "status": "<any>"})"),
                       "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<SyncInfo> {
            auto& app = ctx.service<EngineApp>();
            return SyncInfo{syncSettingsJson(app.syncSettings()), app.syncReport()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        cmd::Command<json::Value, SyncInfo> c;
        c.name = "sync.set";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set external sync (MIDI clock, MTC, LTC) and LTC output; returns them.";
        c.description = std::string(settingsDoc) +
                        " Omitted members keep their values. Machine settings: not an undo step, "
                        "not saved in the project (the app keeps them with its preferences).";
        c.tags = {"transport", "sync"};
        json::Value spec = json::Value::object();
        spec["type"] = "object";
        spec["properties"] = parsed(R"({
            "source": {"type": "string", "enum": ["off", "midi_clock", "mtc", "ltc"]},
            "midi_input": {"type": "string"},
            "ltc_input": {"type": "integer", "minimum": 1, "maximum": 64},
            "follow_tempo": {"type": "boolean"},
            "offset_ms": {"type": "number", "minimum": -10000, "maximum": 10000},
            "on_loss": {"type": "string", "enum": ["stop", "freewheel"]},
            "freewheel_frames": {"type": "integer", "minimum": 1, "maximum": 300},
            "ltc_output": {"type": "boolean"},
            "ltc_output_channel": {"type": "integer", "minimum": 1, "maximum": 64},
            "ltc_level_db": {"type": "number", "minimum": -60, "maximum": 0}})");
        spec["additionalProperties"] = false;
        c.args = schema::Schema<json::Value>(
            "sync_settings",
            [](const json::Value& v, json::Value& out, schema::DecodeCtx& ctx,
               const std::string& p) {
                auto checked = parseSyncSettings(v);
                if (!checked) {
                    const Error& e = checked.error();
                    ctx.error(e.code, e.message, p + e.path, e.hint);
                    return false;
                }
                out = v;
                return true;
            },
            [](const json::Value& v) { return v; }, spec);
        c.result = syncInfoSchema();
        c.examples = {{"Chase MTC from a MIDI interface",
                       parsed(R"({"source": "mtc", "midi_input": "Interface"})"),
                       parsed(R"({"settings": {"source": "mtc", "midi_input": "Interface",
                                  "ltc_input": 1, "follow_tempo": true, "offset_ms": 0.0,
                                  "on_loss": "stop", "freewheel_frames": 25,
                                  "ltc_output": false, "ltc_output_channel": 3,
                                  "ltc_level_db": -18.0}, "status": "<any>"})"),
                       "empty"}};
        c.run = [](const json::Value& a, cmd::Context& ctx) -> Result<SyncInfo> {
            auto& app = ctx.service<EngineApp>();
            auto next = parseSyncSettings(a, app.syncSettings());
            if (!next) return std::move(next).error();
            SUPPLE_TRY(app.setSyncSettings(*next));
            SyncInfo info{syncSettingsJson(app.syncSettings()), app.syncReport()};
            for (const auto& w : info.status.warnings) ctx.warn("sync.warning", w, "");
            return info;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
