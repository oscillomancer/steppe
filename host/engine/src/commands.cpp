// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "command_validation.h"
#include "freeze_commands.h"
#include "supple/model/numeric.h"
// The engine's command catalogue (session-model §7) — the core set. Every
// command is a thin, typed wrapper: validate references with helpful errors,
// edit through the open EditTxn (edits) or read the view (queries), return
// IDs and the few fields an agent needs next.
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <set>

#include "analysis_commands.h"
#include "media_files.h"
#include "render_commands.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/engine/racks.h"
#include "supple/midi/midi.h"
#include "supple/model/groups.h"
#include "supple/model/render_settings.h"
#include "supple/model/schemas.h"
#include "supple/notation/notation.h"
#include "supple/params/params.h"

namespace supple::engine {

void registerExtraCommands(cmd::Registry& registry);
void registerGroupCommands(cmd::Registry& registry);
void registerMidiCommands(cmd::Registry& registry);
void registerChainCommands(cmd::Registry& registry);
void registerTakeCommands(cmd::Registry& registry);
void registerAudioCommands(cmd::Registry& registry);
void registerConsolidateCommands(cmd::Registry& registry);
void registerExportCommands(cmd::Registry& registry);
void registerMixCommands(cmd::Registry& registry);
void registerHealCommands(cmd::Registry& registry);
void registerSyncCommands(cmd::Registry& registry);
schema::Schema<SyncReport> syncStatusSchema();
void registerLearnCommands(cmd::Registry& registry);
void registerControlLearnCommands(cmd::Registry& registry);
void registerDspCommands(cmd::Registry& registry);
void registerRackCommands(cmd::Registry& registry);
void registerRackClipCommands(cmd::Registry& registry);
void registerRackLevelCommands(cmd::Registry& registry);
void registerModuleCommands(cmd::Registry& registry);
void registerRackProbe(cmd::Registry& registry);
void removeRacksOn(model::EditTxn& t, const model::Session& s, model::Id track);

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using cmd::NoArgs;
using model::Id;
namespace fs = std::filesystem;

namespace {

json::Value obj(std::string_view text) { return json::parse(text).value(); }
EngineApp& A(Context& ctx) { return ctx.service<EngineApp>(); }

// Whether `track`'s signal reaches `rack` (through parent sends, routes and
// other racks): it may not then take that rack's Out as input (rack.cycle).
bool trackFeedsRack(const model::Session& s, Id track, Id rack) {
    std::set<Id> seen{track};
    std::vector<Id> todo{track};
    auto rackOf = [&](Id module) -> std::optional<Id> {
        const model::RackModule* m = s.modules.find(module);
        return m ? std::optional<Id>(m->rack) : std::nullopt;
    };
    while (!todo.empty()) {
        const Id at = todo.back();
        todo.pop_back();
        if (at == rack) return true;
        std::vector<Id> next;
        if (const model::Track* t = s.tracks.find(at); t && t->parent && t->parentSend.enabled)
            next.push_back(*t->parent);
        for (const auto& e : s.routes) {
            const model::Route& r = *e.value;
            const std::optional<Id> from = r.from.module ? rackOf(*r.from.module) : r.from.track;
            if (from != at) continue;
            const std::optional<Id> to = r.to.module ? rackOf(*r.to.module)
                                         : r.to.fx   ? model::trackOfFx(s, *r.to.fx)
                                                     : r.to.track;
            if (to) next.push_back(*to);
        }
        for (Id n : next)
            if (seen.insert(n).second) todo.push_back(n);
    }
    return false;
}

auto idArg(const char* kindPrefix, const char* what) {
    return schema::string()
        .pattern(std::string("^") + kindPrefix + "_[0-9]+$", std::string(what) + " id")
        .doc(std::string(what) + " id, e.g. \"" + kindPrefix + "_1\".");
}

// Existence check against the current view, with the candidates in the hint.
template <class T>
Result<Id> existing(const model::Session& s, const std::string& text, const char* path) {
    auto id = Id::parse(text);
    const auto& map = model::Traits<T>::map(s);
    if (id && id->kind == model::Traits<T>::kind && map.contains(*id)) return *id;
    std::string some;
    int n = 0;
    for (const auto& e : map) {
        if (n++ == 8) {
            some += ", …";
            break;
        }
        some += (some.empty() ? "" : ", ") + e.id.str();
    }
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown", "no " + text,
                 path, some.empty() ? "there are none yet" : "existing: " + some);
}

double beatsOf(const model::Session& s, std::optional<double> beats, std::optional<int> bar) {
    if (bar) return s.timeMap->beatOfBar(*bar);
    return beats.value_or(0.0);
}

struct Empty {};
schema::Schema<Empty> emptySchema() { return schema::object<Empty>("empty").build(); }

// --- project / session -----------------------------------------------------------------

struct ProjectTimecode {
    std::string rate, start;  // as project.timecode saves them, defaults included
};
struct ProjectInfo {
    std::string name, path, branch;
    double sampleRate = 0, tempo = 0;
    std::string meter;
    ProjectTimecode timecode;
    std::int64_t version = 0;
    std::int64_t generation = 0;
    std::int64_t recoverable = 0;    // recordings project.recover can bring back
    bool autosaveAvailable = false;  // a newer autosave exists (not loaded)
    std::optional<std::vector<schema::Warning>> warnings;
};

schema::Schema<schema::Warning> warningSchema(std::string_view title) {
    return schema::object<schema::Warning>(std::string(title))
        .field("code", &schema::Warning::code, schema::string())
        .field("message", &schema::Warning::message, schema::string())
        .field("path", &schema::Warning::path, schema::string())
        .build();
}

schema::Schema<ProjectInfo> projectInfoSchema() {
    return schema::object<ProjectInfo>("project_info")
        .field("name", &ProjectInfo::name, schema::string())
        .field("path", &ProjectInfo::path, schema::string())
        .field("branch", &ProjectInfo::branch, schema::string())
        .field("sample_rate", &ProjectInfo::sampleRate, schema::number())
        .field("tempo", &ProjectInfo::tempo, schema::number())
        .field("meter", &ProjectInfo::meter, schema::string())
        .field("timecode", &ProjectInfo::timecode,
               schema::object<ProjectTimecode>("project_timecode")
                   .field("rate", &ProjectTimecode::rate, schema::string())
                   .field("start", &ProjectTimecode::start, schema::string())
                   .build()
                   .doc("The timecode frame rate and the label at project time 0"))
        .field("version", &ProjectInfo::version, schema::integer())
        .field("generation", &ProjectInfo::generation, schema::integer())
        .field("recoverable", &ProjectInfo::recoverable, schema::integer())
        .field("autosave_available", &ProjectInfo::autosaveAvailable, schema::boolean())
        .field("warnings", &ProjectInfo::warnings, schema::arrayOf(warningSchema("load_warning")))
        .build();
}

ProjectInfo projectInfo(EngineApp& app, const model::Session& s) {
    const auto m = s.timeMap->meterAt(1);
    const fs::path project = app.projectPath();
    return ProjectInfo{s.project->name,
                       project.string(),
                       app.versions().branch(),
                       s.project->sampleRate,
                       s.timeMap->bpmAt(0),
                       std::to_string(m.numerator) + "/" + std::to_string(m.denominator),
                       {std::string(sync::rateName(s.project->timecode.rate)),
                        sync::format(s.project->timecode.start, s.project->timecode.rate)},
                       static_cast<std::int64_t>(s.version),
                       static_cast<std::int64_t>(app.projectGeneration()),
                       static_cast<std::int64_t>(app.recoverable().size()),
                       !app.newerAutosave(project).empty(),
                       std::nullopt};
}

struct RecoverableList {
    std::vector<RecoverableInfo> recordings;
    std::optional<std::vector<RecordingWarning>> warnings;
};
struct RecoverArgs {
    std::optional<std::vector<std::string>> files;
    std::optional<std::string> track;
};
struct RecoverResult {
    std::vector<RecordedInfo> recovered;
    std::optional<std::vector<RecordingWarning>> warnings;
};
struct TimecodeArgs {
    std::optional<sync::FrameRate> rate;
    std::optional<std::string> start;
};
struct ProjectSetArgs {
    std::optional<std::string> name, title, author, notes;
    std::optional<bool> console9;
    std::optional<TimecodeArgs> timecode;
    std::optional<model::PanLaw> panLaw;
    std::optional<json::Value> render;
    std::optional<model::Timebase> timebase;
    std::optional<std::uint64_t> historyBudgetBytes;
};
struct PathArgs {
    std::optional<std::string> path;
};
struct PathResult {
    std::string path;
};
struct SaveResult {
    std::string path;
    std::optional<std::vector<schema::Warning>> warnings;  // plug-in states not read
};
struct TextResult {
    std::string text;
};

// --- engine / transport -------------------------------------------------------------------

enum class BackendArg { Jack, Dummy, PortAudio };
struct EngineDevicesArgs {
    std::optional<BackendArg> backend;
};
struct EngineDevicesResult {
    std::vector<AudioDevice> devices;
};
struct EngineStartArgs {
    std::optional<BackendArg> backend;
    std::optional<std::string> hostApi, inputDevice, outputDevice;
    std::optional<double> rate;
    std::optional<std::int64_t> inputs, outputs;
    std::optional<std::int64_t> block;
    std::optional<bool> anticipative, forceGraph;
};
struct SeekArgs {
    std::optional<double> seconds, beat;
    std::optional<std::int64_t> bar;
};
struct RangeArgs {
    std::optional<bool> enabled, clear;
    std::optional<double> startSeconds, startBeat, endSeconds, endBeat;
    std::optional<std::int64_t> startBar, endBar;
};

schema::Schema<MeterDrops> meterDropsSchema() {
    return schema::object<MeterDrops>("meter_drops")
        .field("renderer", &MeterDrops::renderer, schema::integer())
        .field("callback", &MeterDrops::callback, schema::integer())
        .build();
}

schema::Schema<RuntimeProtectionStatus> runtimeProtectionSchema() {
    auto error = schema::object<RuntimeProtectionError>("runtime_protection_error")
                     .field("code", &RuntimeProtectionError::code, schema::string())
                     .field("message", &RuntimeProtectionError::message, schema::string())
                     .field("path", &RuntimeProtectionError::path, schema::string())
                     .field("hint", &RuntimeProtectionError::hint, schema::string())
                     .field("errno", &RuntimeProtectionError::errnoValue, schema::integer())
                     .build();
    return schema::object<RuntimeProtectionStatus>("runtime_protection_status")
        .field("requested", &RuntimeProtectionStatus::requested, schema::boolean())
        .field("active", &RuntimeProtectionStatus::active, schema::boolean())
        .field("error", &RuntimeProtectionStatus::error, error)
        .build();
}

schema::Schema<ThreadClass> threadClassSchema() {
    return schema::object<ThreadClass>("thread_class")
        .field("policy", &ThreadClass::policy,
               schema::enumeration<ThreadPolicy::Kind>({{ThreadPolicy::Kind::None, "none"},
                                                        {ThreadPolicy::Kind::Normal, "normal"},
                                                        {ThreadPolicy::Kind::Fifo, "fifo"},
                                                        {ThreadPolicy::Kind::RoundRobin, "rr"},
                                                        {ThreadPolicy::Kind::High, "high"}}))
        .field("priority", &ThreadClass::priority, schema::integer().range(0, 99))
        .build();
}

schema::Schema<EngineScheduling> engineSchedulingSchema() {
    return schema::object<EngineScheduling>("engine_scheduling")
        .field("callback", &EngineScheduling::callback, threadClassSchema())
        .field("live", &EngineScheduling::live, threadClassSchema())
        .field("renderer", &EngineScheduling::renderer, threadClassSchema())
        .field("background", &EngineScheduling::background, threadClassSchema())
        .build();
}

schema::Schema<EngineWorkgroup> engineWorkgroupSchema() {
    return schema::object<EngineWorkgroup>("engine_workgroup")
        .field("state", &EngineWorkgroup::state, schema::string())
        .field("joined", &EngineWorkgroup::joined, schema::integer())
        .field("failed", &EngineWorkgroup::failed, schema::integer())
        .field("os_error", &EngineWorkgroup::osError, schema::integer())
        .field("message", &EngineWorkgroup::message, schema::string())
        .field("hint", &EngineWorkgroup::hint, schema::string())
        .build();
}

schema::Schema<MidiInputWarning> midiInputWarningSchema() {
    return schema::object<MidiInputWarning>("midi_input_warning")
        .field("device", &MidiInputWarning::device, schema::string())
        .field("os_error", &MidiInputWarning::osError, schema::integer())
        .field("message", &MidiInputWarning::message, schema::string())
        .field("hint", &MidiInputWarning::hint, schema::string())
        .build();
}

schema::Schema<EngineStatus> engineStatusSchema() {
    return schema::object<EngineStatus>("engine_status")
        .field("running", &EngineStatus::running, schema::boolean())
        .field("preparing_audio", &EngineStatus::preparingAudio, schema::boolean())
        .field("prepared_version", &EngineStatus::preparedVersion,
               schema::integerOf<std::uint64_t>())
        .field("preparation_version", &EngineStatus::preparationVersion,
               schema::integerOf<std::uint64_t>())
        .field("preparation_error", &EngineStatus::preparationError, schema::string())
        .field("backend", &EngineStatus::backend, schema::string())
        .field("sample_rate", &EngineStatus::sampleRate, schema::number())
        .field("block", &EngineStatus::block, schema::integer())
        .field("period", &EngineStatus::period, schema::integer())
        .field("plan_latency", &EngineStatus::planLatency, schema::integer())
        .field("device_latency", &EngineStatus::deviceLatency, schema::integer())
        .field("xruns", &EngineStatus::xruns, schema::integer())
        .field("midi_dropped", &EngineStatus::midiDropped, schema::integer())
        .field("midi_out_dropped", &EngineStatus::midiOutDropped, schema::integer())
        .field("midi_output_warnings", &EngineStatus::midiOutputWarnings,
               schema::arrayOf(midiInputWarningSchema()))
        .field("midi_output_latency", &EngineStatus::midiOutputLatency,
               schema::arrayOf(schema::integer()))
        .field("midi_warnings", &EngineStatus::midiWarnings,
               schema::arrayOf(midiInputWarningSchema()))
        .field("midi_devices_changed", &EngineStatus::midiDevicesChanged, schema::boolean())
        .field("midi_devices_message", &EngineStatus::midiDevicesMessage, schema::string())
        .field("cycles", &EngineStatus::cycles, schema::integer())
        .field("process_errors", &EngineStatus::processErrors, schema::integer())
        .field("restart_needed", &EngineStatus::restartNeeded, schema::boolean())
        .field("anticipative", &EngineStatus::anticipative, schema::boolean())
        .field("anticipation_misses", &EngineStatus::anticipationMisses, schema::integer())
        .field("live_helpers", &EngineStatus::liveHelpers, schema::integer())
        .field("scheduling", &EngineStatus::scheduling, engineSchedulingSchema())
        .field("workgroup", &EngineStatus::workgroup, engineWorkgroupSchema())
        .field("warnings", &EngineStatus::warnings, schema::arrayOf(schema::string()))
        .field("memory_lock", &EngineStatus::memoryLock, runtimeProtectionSchema())
        .field("cpu_idle_latency", &EngineStatus::cpuIdleLatency, runtimeProtectionSchema())
        .field("meter_drops", &EngineStatus::meterDrops, meterDropsSchema())
        .field("failed_fx", &EngineStatus::failedFx, schema::arrayOf(schema::string()))
        .field("dropped", &EngineStatus::dropped, schema::integer())
        .field("voice_dropped", &EngineStatus::voiceDropped, schema::integer())
        .field("failed_errors", &EngineStatus::failedErrors, schema::any())
        .field("sync", &EngineStatus::sync, syncStatusSchema())
        .build();
}

schema::Schema<RecordingWarning> recordingWarningSchema() {
    return schema::object<RecordingWarning>("recording_warning")
        .field("code", &RecordingWarning::code, schema::string())
        .field("message", &RecordingWarning::message, schema::string())
        .field("path", &RecordingWarning::path, schema::string())
        .field("hint", &RecordingWarning::hint, schema::string())
        .field("track", &RecordingWarning::track, schema::string())
        .field("file", &RecordingWarning::file, schema::string())
        .build();
}

schema::Schema<RecordedInfo> recordedSchema() {
    return schema::object<RecordedInfo>("recorded_take")
        .field("track", &RecordedInfo::track, schema::string())
        .field("item", &RecordedInfo::item, schema::string())
        .field("take", &RecordedInfo::take, schema::string())
        .field("file", &RecordedInfo::file, schema::string())
        .field("seconds", &RecordedInfo::seconds, schema::number())
        .field("lost_frames", &RecordedInfo::lostFrames, schema::integer())
        .field("takes", &RecordedInfo::takes, schema::arrayOf(schema::string()))
        .field("warnings", &RecordedInfo::warnings, schema::arrayOf(recordingWarningSchema()))
        .build();
}

schema::Schema<TransportRange> rangeSchema(const char* name) {
    return schema::object<TransportRange>(name)
        .field("enabled", &TransportRange::enabled, schema::boolean())
        .field("start_seconds", &TransportRange::start, schema::number())
        .field("end_seconds", &TransportRange::end, schema::number())
        .build();
}

schema::Schema<TransportInfo> transportSchema() {
    auto recorded = recordedSchema();
    return schema::object<TransportInfo>("transport")
        .field("playing", &TransportInfo::playing, schema::boolean())
        .field("recording", &TransportInfo::recording, schema::boolean())
        .field("counting_in", &TransportInfo::countingIn, schema::boolean())
        .field("seconds", &TransportInfo::seconds, schema::number())
        .field("beat", &TransportInfo::beat, schema::number())
        .field("sample", &TransportInfo::sample, schema::integer())
        .field("loop", &TransportInfo::loop, rangeSchema("loop"))
        .field("punch", &TransportInfo::punch, rangeSchema("punch"))
        .field("recorded", &TransportInfo::recorded, schema::arrayOf(recorded))
        .build();
}

struct PlayrateArgs {
    std::optional<double> rate;
    std::optional<bool> preservePitch;
};
struct PlayrateInfo {
    double requestedRate = 1, effectiveRate = 1;
    bool preservePitch = false;
    std::optional<std::string> envelope;
};
schema::Schema<PlayrateInfo> playrateSchema() {
    return schema::object<PlayrateInfo>("transport_playrate")
        .field("requested_rate", &PlayrateInfo::requestedRate, schema::number())
        .field("effective_rate", &PlayrateInfo::effectiveRate, schema::number())
        .field("preserve_pitch", &PlayrateInfo::preservePitch, schema::boolean())
        .field("envelope", &PlayrateInfo::envelope, schema::string())
        .build();
}
PlayrateInfo playrateInfo(const model::Session& session, const Engine* engine = nullptr) {
    PlayrateInfo out;
    out.requestedRate = session.project->playrate.rate;
    out.preservePitch = session.project->playrate.preservePitch;
    if (const auto envelope = model::activePlayrateEnvelope(session))
        out.envelope = envelope->str();
    // Consumption telemetry can lag requested settings at the render horizon
    // and through a compensated source transition (ADR-0045).
    out.effectiveRate = engine ? engine->effectivePlayrate() : 1.;
    return out;
}

// --- automation envelopes ---------------------------------------------------------------

struct EnvPointArg {
    std::optional<double> beat, seconds;
    std::optional<std::int64_t> bar;
    json::Value value;
    std::optional<model::PointShape> shape;
    std::optional<double> tension;
};
struct EnvAddArgs {
    std::optional<std::string> track, fx, route, take, group, module;
    std::string param;
    std::optional<std::vector<EnvPointArg>> points;
};
struct EnvAddResult {
    std::string env;
    std::int64_t points = 0;
};
struct EnvPointsSetArgs {
    std::string env;
    std::vector<EnvPointArg> points;
    std::optional<double> fromBeat, toBeat;
};
struct EnvCount {
    std::int64_t points = 0;
};
struct EnvRef {
    std::string env;
};
struct EnvSetArgs {
    std::string env;
    std::optional<bool> active, visible;
    std::optional<model::EnvMode> mode;
};
struct EnvListArgs {
    std::optional<std::string> object;
};
struct EnvEntry {
    std::string env, target, param, mode;
    std::int64_t points = 0;
    bool active = true, visible = true;
};
struct EnvList {
    std::vector<EnvEntry> envelopes;
};

schema::Schema<model::PointShape> pointShapeSchema() {
    return schema::enumeration<model::PointShape>({{model::PointShape::Linear, "linear"},
                                                   {model::PointShape::Square, "square"},
                                                   {model::PointShape::Slow, "slow"},
                                                   {model::PointShape::FastStart, "fast_start"},
                                                   {model::PointShape::FastEnd, "fast_end"},
                                                   {model::PointShape::Bezier, "bezier"}});
}
schema::Schema<model::EnvMode> envModeSchema() {
    return schema::enumeration<model::EnvMode>({{model::EnvMode::Trim, "trim"},
                                                {model::EnvMode::Read, "read"},
                                                {model::EnvMode::Touch, "touch"},
                                                {model::EnvMode::Latch, "latch"},
                                                {model::EnvMode::LatchPreview, "latch_preview"},
                                                {model::EnvMode::Write, "write"}});
}
schema::Schema<EnvPointArg> envPointSchema() {
    return schema::object<EnvPointArg>("env_point_arg")
        .field("beat", &EnvPointArg::beat, schema::number().min(0))
        .field("bar", &EnvPointArg::bar,
               schema::integer().range(1, std::numeric_limits<int>::max()))
        .field("seconds", &EnvPointArg::seconds, schema::number().min(0))
        .field("value", &EnvPointArg::value, schema::any())
        .field("shape", &EnvPointArg::shape, pointShapeSchema())
        .field("tension", &EnvPointArg::tension, schema::number().range(-1, 1))
        .build();
}

// A mixer parameter's unit, or none when `param` is not one.
std::optional<units::Unit> mixerUnit(model::Kind kind, const std::string& param) {
    if (param == "volume") return units::Unit::dB;
    if (param == "pan" || (kind == model::Kind::Track &&
                           (param == "width" || param == "pan_left" || param == "pan_right")))
        return units::Unit::percent;
    if (kind == model::Kind::Track && param == "pan_boost") return units::Unit::none;
    if (param == "mute") return units::Unit::none;
    return std::nullopt;
}

// How an envelope's values are read: a mixer unit, a plugin's semantic
// parameter (physical units, text with units accepted), or a plain CLAP value.
struct EnvDomain {
    bool playrate = false;
    std::optional<units::Unit> mixer;
    std::unique_ptr<params::DescribedParam> semantic;
    std::optional<std::pair<double, double>> clapRange;
    std::optional<script::detail::ParamModel> knob;  // a rack module's (physical units)
};

Result<EnvDomain> envDomain(EngineApp& app, const model::Session& s, const model::EnvTarget& t,
                            const char* path) {
    EnvDomain d;
    if (t.object.kind == model::Kind::Track && (t.param == "width" || t.param == "pan_left" ||
                                                t.param == "pan_right" || t.param == "pan_boost")) {
        const auto* track = s.tracks.find(t.object);
        if (track && track->type == model::StripType::Master)
            return Error("track.strip_type", "a master's pan is a balance", path,
                         "automate pan modes and gain compensation on channels or busses");
    }
    if (t.param == "playrate" && t.object.kind != model::Kind::Fx) {
        if (t.object != model::kMaster)
            return Error("env.target", "master playrate belongs to tr_0", path,
                         "use env.add {track: tr_0, param: playrate}");
        d.playrate = true;
        return d;
    }
    if (t.object.kind == model::Kind::Module) {
        const model::RackModule* m = s.modules.find(t.object);
        if (!m)
            return Error("rack.unknown_module", "no module " + t.object.str(), path, "rack.get");
        if (m->fx)
            return Error("env.param", "a plugin module's knobs are its plug-in's", path,
                         "env.add {fx: \"" + m->fx->str() + "\", param}");
        const auto* type = app.modules()->find(m->type);
        if (!type)
            return Error("rack.unknown_type", "the type of " + t.object.str() + " is not installed",
                         path, "module.list shows what is");
        std::string keys;
        for (const auto& p : type->type->params()) {
            if (p.output || p.hidden) continue;
            if (p.key == t.param) {
                d.knob = p;
                return d;
            }
            keys += (keys.empty() ? "" : ", ") + p.key;
        }
        return Error("env.param", type->type->id() + " has no knob \"" + t.param + "\"", path,
                     keys.empty() ? "it has no knobs" : "knobs: " + keys);
    }
    if (t.object.kind != model::Kind::Fx) {
        d.mixer = mixerUnit(t.object.kind, t.param);
        if (!d.mixer)
            return Error(t.object.kind == model::Kind::Track ? "track.strip_type" : "env.param",
                         "no automatable parameter \"" + t.param + "\" on " + t.object.str(), path,
                         t.object.kind == model::Kind::Track
                             ? "automate volume, pan, mute, width, pan_left, pan_right or pan_boost"
                             : "volume, pan, mute");
        return d;
    }
    const model::FxInstance& fx = *s.fx.find(t.object);
    auto schema = app.fxSchema(fx);
    if (!schema) return std::move(schema).error();
    if (const json::Value* ps = schema->isObject() ? schema->find("params") : nullptr) {
        if (const json::Value* entry = ps->find(t.param)) {
            auto spec = params::fromDescription(t.param, *entry);
            if (!spec) return std::move(spec).error();
            if ((*spec)->spec.flags & params::kReadonly)
                return Error("env.param", t.param + " is read-only", path,
                             "choose a writable parameter from fx.describe instead of automating a "
                             "read-only output");
            d.semantic = std::move(*spec);
            return d;
        }
    }
    auto list = app.fxParams(fx);
    if (!list) return std::move(list).error();
    for (const host::ParamInfo& p : *list)
        if (std::to_string(p.id) == t.param || p.name == t.param) {
            d.clapRange = std::pair{p.min, p.max};
            return d;
        }
    std::string keys;
    if (const json::Value* ps = schema->isObject() ? schema->find("params") : nullptr)
        for (const auto& m : ps->asObject()) keys += (keys.empty() ? "" : ", ") + m.key;
    return Error("env.param", fx.plugin.id + " has no parameter \"" + t.param + "\"", path,
                 keys.empty() ? "use a CLAP id or parameter name (fx.describe)" : "keys: " + keys);
}

Result<double> envValue(const EnvDomain& d, const json::Value& v, const std::string& path) {
    if (d.playrate) {
        if (!v.isNumber() || !std::isfinite(v.asDouble()) || v.asDouble() < model::kMinPlayrate ||
            v.asDouble() > model::kMaxPlayrate)
            return Error("playrate.range", "master playrate needs a ratio in 0.25..4.0", path,
                         "provide a finite numeric playback ratio from 0.25 to 4.0");
        return v.asDouble();
    }
    auto located = [&](Result<double> r) -> Result<double> {
        if (r) return r;
        Error e = std::move(r).error();
        if (e.path.empty()) e.path = path;
        return e;
    };
    if (d.semantic) return located(params::fromJson(d.semantic->spec, v, true));
    if (d.knob) {
        using K = script::detail::ParamModel::Kind;
        const script::detail::ParamModel& p = *d.knob;
        if (p.kind == K::Choice && v.isString()) {
            for (std::size_t k = 0; k < p.choices.size(); ++k)
                if (script::detail::choiceKey(p, k) == v.asString() || p.choices[k] == v.asString())
                    return static_cast<double>(k);
        } else if (p.kind == K::Toggle && v.isBool()) {
            return v.asBool() ? 1.0 : 0.0;
        } else if (v.isNumber() && std::isfinite(v.asDouble())) {
            const double x = std::clamp(v.asDouble(), p.min, p.max);
            return p.kind == K::Continuous ? x : std::round(x);
        }
        return Error("env.value",
                     p.kind == K::Choice   ? "one of the knob's choices (its key) or an index"
                     : p.kind == K::Toggle ? "true, false, 0 or 1"
                                           : "a number in the knob's unit",
                     path, "module.describe lists the knob's range, unit and choices");
    }
    if (d.mixer) {
        if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
        if (v.isNumber()) return v.asDouble();
        if (v.isString() && *d.mixer != units::Unit::none)
            return located(units::parseQuantity(v.asString(), *d.mixer));
        return Error(
            "env.value", "a number (or text with a unit)", path,
            "provide a number in the parameter's physical unit, or unit text such as \"-6 dB\"");
    }
    if (!v.isNumber())
        return Error(
            "env.value", "a CLAP value (number)", path,
            "provide a numeric CLAP value within the parameter range shown by fx.describe");
    return std::clamp(v.asDouble(), d.clapRange->first, d.clapRange->second);
}

// `itemRelative`: a take's envelope, positions in beats from its item's start.
Result<std::vector<model::EnvPoint>> envPoints(const model::Session& s, const EnvDomain& d,
                                               const std::vector<EnvPointArg>& in,
                                               const std::string& path, bool itemRelative = false,
                                               model::Timebase timebase = model::Timebase::Beats) {
    std::vector<model::EnvPoint> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const EnvPointArg& a = in[i];
        const std::string at = path + "/" + std::to_string(i);
        if (itemRelative && !a.beat)
            return Error("env.position", "a take's envelope takes beats from the item's start", at,
                         "e.g. {\"beat\": 0, \"value\": -6} at the item's start");
        const int given = a.beat.has_value() + a.bar.has_value() + a.seconds.has_value();
        if (given != 1)
            return Error("env.position", "give one of beat, bar or seconds", at,
                         "provide exactly one of beat, bar or seconds for each point");
        model::EnvPoint pt;
        pt.pos = a.seconds
                     ? s.timeMap->beatAt(*a.seconds)
                     : beatsOf(s, a.beat,
                               a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
        SUPPLE_TRY(model::checkedBeat(s, pt.pos,
                                      at + (a.seconds ? "/seconds"
                                            : a.bar   ? "/bar"
                                                      : "/beat")));
        if (timebase == model::Timebase::Time)
            pt.pos = a.seconds.value_or(s.timeMap->secondsAt(pt.pos));
        auto v = envValue(d, a.value, at + "/value");
        if (!v) return std::move(v).error();
        pt.value = *v;
        pt.shape = a.shape.value_or(model::PointShape::Linear);
        pt.tension = a.tension.value_or(0.0);
        out.push_back(pt);
    }
    std::stable_sort(
        out.begin(), out.end(),
        [](const model::EnvPoint& x, const model::EnvPoint& y) { return x.pos < y.pos; });
    return out;
}

// --- tempo ------------------------------------------------------------------------------

struct TempoPointArg {
    double beat = 0, bpm = 120;
    bool ramp = false;
};
struct TempoSetArgs {
    std::vector<TempoPointArg> points;
};
struct TimesigSetArgs {
    int bar = 1;
    std::string meter;
};
struct TimesigRemoveArgs {
    int bar = 2;
};
struct CountResult {
    std::int64_t count = 0;
};

// --- tracks -----------------------------------------------------------------------------

struct TrackAddArgs {
    model::StripType type = model::StripType::Channel;
    std::optional<std::string> name, parent;
    std::optional<std::int64_t> index;
    std::optional<std::int64_t> channels;
};
struct TrackRef {
    std::string track;
};
struct TrackSetArgs {
    std::string track;
    bool bypassGroups = false;
    std::optional<std::string> name, color, notes;
    std::optional<double> volumeDb, pan;
    std::optional<model::PanMode> panMode;
    std::optional<double> width, panLeft, panRight;
    std::optional<json::Value> panLaw;  // {db, boost}, or null: the project's
    std::optional<model::StripType> type;
    std::optional<bool> mute, phaseInvert, mono;
    std::optional<std::string> solo;
    std::optional<bool> armed;
    std::optional<json::Value> recordInput;  // channels, or null: no hardware audio
    std::optional<json::Value> recordMidi;
    std::optional<json::Value> recordRack;  // an Out module (ADR-0062), or null: none
    std::optional<std::int64_t> midiChannel;
    std::optional<model::RecordMode> recordMode;
    std::optional<model::Monitor> monitor;
    std::optional<bool> parentSend;
    std::optional<model::TrackTimebase> timebase;
    std::optional<std::int64_t> channels;
    std::optional<std::vector<std::int64_t>> hwOutput;
    std::optional<bool> laneMute;
};
struct TrackEntry {
    bool frozen = false;
    std::optional<model::TrackFreeze> freeze;
    std::string id, name, parent;
    model::StripType type = model::StripType::Channel;
    std::int64_t depth = 0;
    double volumeDb = 0;
    bool mute = false;
    std::string solo;
    std::vector<std::string> fx, items;
    std::vector<Id> groups, vcas;
    std::optional<std::vector<std::int64_t>> hwOutput;  // masters
    std::optional<bool> armed, laneMute;                // masters (ADR-0057)
    double requestedEffective = 0, effective = 0, at = 0;
    bool effectiveMute = false, soloSilenced = false;
};
struct TrackListArgs {
    std::optional<double> at;
};
struct TrackList {
    std::vector<TrackEntry> tracks;
};

// --- items / MIDI ------------------------------------------------------------------------

struct ItemAddArgs {
    std::string track;
    std::optional<double> position;  // beats
    std::optional<std::int64_t> bar;
    std::optional<double> length;  // beats
    std::optional<std::string> notation, audioFile, midiFile;
    std::optional<std::string> name;
};
struct ItemAddResult {
    std::string item, take;
    std::optional<std::string> source;
    double position = 0, length = 0;
    std::optional<std::vector<std::string>> warnings;
};
struct ItemRef {
    bool bypassGroups = false;
    std::string item;
};
struct ItemMoveArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> position;
    std::optional<std::int64_t> bar;
    std::optional<std::string> track;
};
struct MidiWriteArgs {
    std::string item;
    std::string notation;
};
struct MidiGetArgs {
    std::string item;
};

// --- fx ------------------------------------------------------------------------------------

struct FxAddArgs {
    std::optional<std::string> track, take;
    std::string plugin;
    std::optional<std::int64_t> index;
    std::optional<json::Value> patch;
    std::optional<std::string> chain;  // "main" (default) or "input"
};
struct FxAddResult {
    std::string fx, name;
    bool semantic = false;
    std::vector<std::string> chain;
};
struct FxRef {
    std::string fx;
};
struct FxSetArgs {
    std::string fx;
    std::optional<bool> bypass, offline;
    std::optional<double> wet;
    std::optional<std::string> label;
    std::optional<model::Isolation> isolation;
    std::optional<model::PinMap> pins;
};
struct FxPatchArgs {
    std::string fx;
    json::Value patch;
};
struct FxParamSetArgs {
    std::string fx;
    json::Value values;
};
struct MergeResult {
    json::Value changed;
    std::string description;
    std::optional<std::vector<std::string>> inactive;
};
struct FxDescribeArgs {
    std::string fx;
    bool full = false;
    std::optional<std::string> filter;
};
struct FxDescription {
    std::string plugin, name, description;
    std::optional<json::Value> params, patch;
    bool statusAvailable = false;
    std::optional<json::Value> problems;
    std::optional<std::int64_t> paramsTotal;
    std::optional<std::int64_t> sidechainChannels;  // a key input (ADR-0018)
    std::optional<std::int64_t> latencySamples;     // the instance's reported latency
};
constexpr std::size_t kMaxListedParams = 512;

// --- render / history ------------------------------------------------------------------------

struct HistoryResult {
    std::int64_t version = 0;
    std::string label;
};
struct HistoryEntry {
    std::int64_t version = 0;
    std::string label, origin;
    std::uint64_t squashedSteps = 0;
};
struct HistoryList {
    std::vector<HistoryEntry> versions;
    bool canUndo = false, canRedo = false, truncated = false;
    std::uint64_t retainedBytes = 0, budgetBytes = 0;
};

// The model's name for a container ("wav"/"flac"/"mp3"): the detected one, not
// the file's extension.
const char* formatName(audio::Container container) {
    switch (container) {
        case audio::Container::Wav:
            return "wav";
        case audio::Container::Flac:
            return "flac";
        case audio::Container::Mp3:
            return "mp3";
        case audio::Container::Unknown:
            return "";
    }
    return "";
}

// A media source added before hashes existed: there is no hash to match, so the
// path (resolved the way the loader resolves it) and the header it was added
// from decide whether the file is still the one it came from. Reusing it lets a
// pre-hash project converge on one source per file instead of duplicating it.
std::optional<model::Id> legacyMediaSourceFor(const model::Session& s, const fs::path& file,
                                              const audio::FileInfo& info, const fs::path& media) {
    const auto same = [](const fs::path& a, const fs::path& b) {
        std::error_code ea, eb;
        const fs::path ca = fs::weakly_canonical(a, ea);
        const fs::path cb = fs::weakly_canonical(b, eb);
        return (ea ? a.lexically_normal() : ca) == (eb ? b.lexically_normal() : cb);
    };
    for (const auto& e : s.media) {
        const model::MediaSource& md = *e.value;
        if (!md.hash.empty()) continue;  // a hashed source is matched by its bytes
        if (md.sampleRate != info.sampleRate || md.channels != info.channels ||
            md.frames != info.frames)
            continue;  // not the recording the source was added from
        std::filesystem::path path = md.path;
        if (path.is_relative()) {
            const fs::path inMedia = media / path;
            path = fs::exists(inMedia) ? inMedia : media.parent_path() / path;
        }
        if (same(path, file)) return e.id;
    }
    return std::nullopt;
}

// Compiles notation into a MIDI source; the length in beats is the item's.
std::function<midi::Meter(int)> meterContext(const model::Session& s, double atBeat) {
    const auto map = s.timeMap;
    const int first = map->barBeat(std::max(0.0, atBeat)).bar;
    return [map, first](int bar) {
        const auto absolute = std::clamp<std::int64_t>(std::int64_t(first) + bar - 1, 1, INT_MAX);
        const auto meter = map->meterAt(static_cast<int>(absolute));
        return midi::Meter{meter.numerator, meter.denominator};
    };
}

Result<std::pair<model::MidiSource, double>> compileNotation(const model::Session& s,
                                                             const std::string& text,
                                                             double atBeat) {
    notation::CompileContext cc;
    cc.ppq = s.project->ppq;
    cc.meterAt = meterContext(s, atBeat);
    auto compiled = notation::compile(text, cc);
    if (!compiled) {
        Error e = std::move(compiled).error();
        if (e.path.empty() || e.path.front() != '/') e.path = "/notation";
        return e;
    }
    model::MidiSource src;
    src.sequence = std::move(compiled->sequence);
    const double beats = static_cast<double>(src.sequence.length) / src.sequence.ppq;
    return std::pair{std::move(src), beats};
}

}  // namespace

// Exposed to rack.probe (rack_probe.cpp).
Result<std::pair<model::MidiSource, double>> notationSource(const model::Session& s,
                                                            const std::string& text,
                                                            double atBeat) {
    return compileNotation(s, text, atBeat);
}

void registerEngineCommands(cmd::Registry& registry) {
    // Each command is built in its own immediately-invoked lambda, so an
    // unoptimized build reuses one frame instead of stacking every command's
    // locals (Windows' 1 MB main stack overflowed).
    registerFreezeCommands(registry);
    registerExtraCommands(registry);
    registerGroupCommands(registry);
    registerMidiCommands(registry);
    registerChainCommands(registry);
    registerTakeCommands(registry);
    registerAudioCommands(registry);
    registerConsolidateCommands(registry);
    registerExportCommands(registry);
    registerMixCommands(registry);
    registerHealCommands(registry);
    registerSyncCommands(registry);
    registerLearnCommands(registry);
    registerControlLearnCommands(registry);
    registerDspCommands(registry);
    registerRackCommands(registry);
    registerRackClipCommands(registry);
    registerRackLevelCommands(registry);
    registerModuleCommands(registry);
    registerRackProbe(registry);
    lua::registerScriptCommands(registry);
    // ---------------------------------------------------------------- project.get
    [&] {
        Command<NoArgs, ProjectInfo> c;
        c.name = "project.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Show the project: name, rate, tempo, meter, file, version and branch.";
        c.tags = {"project"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.description =
            "recoverable: recordings a crash left out of the project (project.recoverable). "
            "autosave_available: a newer autosave exists next to the file; open it as a project "
            "if you want it (nothing is loaded silently).";
        c.result = projectInfoSchema();
        c.examples = {
            {"A new project", obj("{}"),
             obj(R"({"name": "", "path": "", "branch": "main", "sample_rate": 48000.0, "tempo": 120.0, "meter": "4/4", "timecode": {"rate": "30", "start": "00:00:00:00"}, "version": 0, "recoverable": 0, "generation": "<any>", "autosave_available": false})"),
             "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            return projectInfo(app, app.view());
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.set
    [&] {
        Command<ProjectSetArgs, Empty> c;
        c.name = "project.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set project name, notes, Console 9, pan law, timebase or timecode.";
        c.description =
            "timebase: what items follow when the tempo changes, for items and tracks that "
            "inherit it — beats (default), time or beats_position; they keep their place now. "
            "timecode: the frame rate (23.976, 24, 25, 29.97df, 29.97 or 30; default 30) and "
            "start, the timecode at project time 0 (default 00:00:00:00), which MTC and LTC "
            "sync and output use; either member alone keeps the other.";
        c.tags = {"project"};
        // Share the model's ID metadata for merge replay, while retaining the
        // state-aware checks and repair hints in run().
        auto renderSchema = model::schemaOf<model::Project>().jsonSchema()["properties"]["render"];
        renderSchema.asObject().erase("default");  // omission leaves existing settings unchanged
        // The typed render settings (session-model §1.10); members before
        // schema 2.3 (format, bit_depth, secondary, tail_ms) are migrated.
        const schema::Schema<json::Value> renderArgs{
            "render settings",
            [](const json::Value& v, json::Value& out, schema::DecodeCtx& ctx,
               const std::string& p) {
                out = v;
                if (auto migrated = model::render::migrateLegacy(out, p); !migrated) {
                    ctx.error(migrated.error().code, migrated.error().message,
                              migrated.error().path, migrated.error().hint);
                    return false;
                }
                model::render::Settings settings;
                if (!model::render::settingsSchema().decodeInto(out, settings, ctx, p))
                    return false;
                out = model::render::toJson(settings);
                return true;
            },
            [](const json::Value& v) { return v; }, std::move(renderSchema)};
        c.args = schema::object<ProjectSetArgs>("project_set_args")
                     .field("name", &ProjectSetArgs::name, schema::string())
                     .field("title", &ProjectSetArgs::title, schema::string())
                     .field("author", &ProjectSetArgs::author, schema::string())
                     .field("notes", &ProjectSetArgs::notes, schema::string())
                     .field("console9", &ProjectSetArgs::console9, schema::boolean())
                     .field("pan_law", &ProjectSetArgs::panLaw,
                            model::panLawSchema().doc(
                                "Channels' and busses' pan law unless a strip sets its own: db "
                                "is the centre's cut (0, -2.5, -3, -4.5 or -6; default 0); "
                                "boost keeps the centre at 0 dB and raises the sides instead"))
                     .field("history_budget_bytes", &ProjectSetArgs::historyBudgetBytes,
                            schema::integerOf<std::uint64_t>().min(1).doc(
                                "Estimated retained history byte budget; default 1 GiB. Leases and "
                                "branch boundaries may exceed it."))
                     .field("render", &ProjectSetArgs::render, renderArgs)
                     .field("timebase", &ProjectSetArgs::timebase, model::timebaseSchema())
                     .field("timecode", &ProjectSetArgs::timecode,
                            schema::object<TimecodeArgs>("timecode_args")
                                .field("rate", &TimecodeArgs::rate, model::frameRateSchema())
                                .field("start", &TimecodeArgs::start,
                                       schema::string().doc(
                                           "HH:MM:SS:FF, \";\" before the frames in drop-frame"))
                                .build())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Name it", obj(R"({"name": "Demo"})"), obj("{}"), "empty"},
            {"EBU timecode starting at one hour",
             obj(R"({"timecode": {"rate": "25", "start": "01:00:00:00"}})"), obj("{}"), "empty"},
            {"Pan with the -3 dB law, boosting the sides",
             obj(R"({"pan_law": {"db": -3, "boost": true}})"), obj("{}"), "empty"}};
        c.run = [](const ProjectSetArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = A(ctx).txn();
            model::Project p = t.project();
            if (a.name) p.name = *a.name;
            if (a.title) p.title = *a.title;
            if (a.author) p.author = *a.author;
            if (a.notes) p.notes = *a.notes;
            if (a.console9) p.console9 = *a.console9;
            if (a.panLaw) p.panLaw = *a.panLaw;
            if (a.timecode) {
                sync::TimecodeMapping tc = p.timecode;
                if (a.timecode->rate) tc.rate = *a.timecode->rate;
                if (a.timecode->start) {
                    auto label = sync::parse(*a.timecode->start, tc.rate);
                    if (!label)
                        return Error("timecode.start", label.error().message, "/timecode/start",
                                     label.error().hint);
                    tc.start = *label;
                } else if (!sync::valid(tc.start, tc.rate)) {
                    return Error("timecode.start",
                                 "the start " + sync::format(tc.start, p.timecode.rate) +
                                     " is not a timecode at " +
                                     std::string(sync::rateName(tc.rate)) + " fps",
                                 "/timecode/start",
                                 "set start with the rate, e.g. {\"rate\": \"" +
                                     std::string(sync::rateName(tc.rate)) +
                                     "\", \"start\": \"00:00:00:00\"}");
                }
                p.timecode = tc;
            }
            if (a.historyBudgetBytes) p.historyBudgetBytes = *a.historyBudgetBytes;
            if (a.render) {
                if (!a.render->isObject())
                    return Error(
                        "render.settings", "render settings must be an object", "/render",
                        "provide render as an object, for example {\"normalize\":\"none\"}");
                if (const auto* matrix = a.render->find("matrix")) {
                    if (!matrix->isObject())
                        return Error("render.matrix", "matrix must be an object", "/render/matrix",
                                     "provide matrix as an object mapping region ids or project to "
                                     "arrays of track ids");
                    for (const auto& [key, row] : matrix->asObject()) {
                        if (key != "project") {
                            auto id = model::Id::parse(key);
                            if (!id || !A(ctx).view().regions.contains(*id))
                                return Error("render.matrix", "unknown matrix region " + key,
                                             "/render/matrix", "use region.add first");
                        }
                        if (!row.isArray())
                            return Error("render.matrix", "matrix row must be an array",
                                         "/render/matrix",
                                         "replace this matrix row with an array of track/master "
                                         "ids from track.list");
                        for (const auto& cell : row.asArray()) {
                            if (!cell.isString())
                                return Error("render.matrix",
                                             "matrix cell must be a track or master id",
                                             "/render/matrix",
                                             "replace this cell with an existing track or master "
                                             "id from track.list");
                            {
                                auto id = model::Id::parse(cell.asString());
                                if (!id || !A(ctx).view().tracks.contains(*id))
                                    return Error("render.matrix",
                                                 "unknown matrix track " + cell.asString(),
                                                 "/render/matrix", "use track.list");
                            }
                        }
                    }
                }
                if (const auto* stems = a.render->find("stems"))
                    if (const auto* tracks = stems->find("tracks"))
                        for (const auto& cell : tracks->asArray()) {
                            auto id = model::Id::parse(cell.asString());
                            if (!id || !A(ctx).view().tracks.contains(*id))
                                return Error("track.unknown",
                                             "unknown stem track " + cell.asString(),
                                             "/render/stems/tracks", "use track.list");
                        }
                if (const auto* masters = a.render->find("masters"))  // ADR-0056
                    for (const auto& cell : masters->asArray()) {
                        auto id = model::Id::parse(cell.asString());
                        const model::Session view = A(ctx).view();
                        const model::Track* strip = id ? view.tracks.find(*id) : nullptr;
                        if (!strip || strip->type != model::StripType::Master)
                            return Error("render.masters", "not a master: " + cell.asString(),
                                         "/render/masters",
                                         "list master ids from track.list (type master)");
                    }
                p.render = *a.render;
            }
            if (a.timebase && *a.timebase != p.timebase) {
                // Items that inherit the project's timebase keep their place.
                const model::Session s = A(ctx).view();
                for (const auto& e : s.items) {
                    const model::Track* tr = s.tracks.find(e.value->track);
                    if (e.value->timebase != model::ItemTimebase::Track ||
                        (tr && tr->timebase != model::TrackTimebase::Project))
                        continue;
                    t.modify<model::Item>(e.id, [&](model::Item& it) {
                        model::retimeItem(s, it, p.timebase, *a.timebase);
                    });
                }
                p.timebase = *a.timebase;
            }
            t.setProject(std::move(p));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.save / project.open
    [&] {
        Command<PathArgs, SaveResult> c;
        c.name = "project.save";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save the project (canonical JSON) to its file or a new path.";
        c.description =
            "path: where to save (becomes the project's file); omit to save in place. warnings: "
            "running plug-ins whose current state could not be read; the file keeps the state "
            "each had before (fx.restart, then save again, or replace the plug-in).";
        c.tags = {"project"};
        c.args = schema::object<PathArgs>("path_args")
                     .field("path", &PathArgs::path, schema::string())
                     .build();
        c.result = schema::object<SaveResult>("save_result")
                       .field("path", &SaveResult::path, schema::string())
                       .field("warnings", &SaveResult::warnings,
                              schema::arrayOf(warningSchema("save_warning")))
                       .build();
        c.examples = {{"Save next to the work directory",
                       obj(R"({"path": "example-project.json"})"), obj(R"({"path": "<any>"})"),
                       "empty"}};
        c.run = [](const PathArgs& a, Context& ctx) -> Result<SaveResult> {
            EngineApp& app = A(ctx);
            fs::path p = a.path ? fs::path(*a.path) : app.projectPath();
            if (p.empty())
                return Error("project.no_path", "the project has no file yet", "/path",
                             "give a path");
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(app.save(p, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            SaveResult result{app.projectPath().string(), {}};
            if (!warnings.empty()) result.warnings = std::move(warnings);
            return result;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<PathArgs, ProjectInfo> c;
        c.name = "project.open";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Open a project file (replaces the current project and its history).";
        c.tags = {"project"};
        c.args = schema::object<PathArgs>("path_args")
                     .field("path", &PathArgs::path, schema::string())
                     .build();
        c.description =
            "recoverable > 0: recordings a crash left out of the project; project.recoverable "
            "lists them and project.recover puts them back. autosave_available: a newer autosave "
            "exists next to the file (it is not loaded silently). warnings also report project "
            "module copies that could not be read or compiled, and name types using a library "
            "version instead or left unavailable.";
        c.result = projectInfoSchema();
        c.errors = {{"file.open", "the file cannot be read", "check the path"},
                    {"schema.invalid", "the file has problems", "details list each with its path"}};
        c.examples = {
            {"Open", obj(R"({"path": "example-project.json"})"),
             obj(R"({"name": "<any>", "path": "<any>", "branch": "main", "sample_rate": "<any>", "tempo": "<any>", "meter": "<any>", "timecode": "<any>", "version": "<any>", "recoverable": 0, "generation": "<any>", "autosave_available": false})"),
             "saved"}};
        c.run = [](const PathArgs& a, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            if (!a.path)
                return Error("project.no_path", "give the project file to open", "/path",
                             "provide path naming an existing project JSON file");
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(app.open(*a.path, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            auto info = projectInfo(app, app.head());
            if (!warnings.empty()) info.warnings = std::move(warnings);
            return info;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.new
    [&] {
        struct NewArgs {
            std::optional<std::string> name;
            std::optional<double> sampleRate;
        };
        Command<NewArgs, ProjectInfo> c;
        c.name = "project.new";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Start a new, empty project (replaces the current one and its history).";
        c.description =
            "The new project has one master and no file until project.save {path}. Unsaved "
            "changes to the current project are lost: save first.";
        c.tags = {"project"};
        c.args = schema::object<NewArgs>("project_new_args")
                     .field("name", &NewArgs::name, schema::string())
                     .field("sample_rate", &NewArgs::sampleRate,
                            schema::number().min(8000).max(384000).doc("Hz; default 48000."))
                     .build();
        c.result = projectInfoSchema();
        c.examples = {
            {"New", obj(R"({"name": "Sketch"})"),
             obj(R"({"name": "Sketch", "path": "", "branch": "main", "sample_rate": 48000.0, "tempo": "<any>", "meter": "<any>", "timecode": "<any>", "version": 0, "recoverable": 0, "generation": "<any>", "autosave_available": false})"),
             "song"}};
        c.run = [](const NewArgs& a, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            model::Project p;
            if (a.name) p.name = *a.name;
            if (a.sampleRate) p.sampleRate = *a.sampleRate;
            SUPPLE_TRY(app.newProject(std::move(p)));
            return projectInfo(app, app.head());
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- project.recoverable /
    // project.recover
    [&] {
        Command<NoArgs, RecoverableList> c;
        c.name = "project.recoverable";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List recordings a crash left out of the project.";
        c.description =
            "Takes recorded but never committed (the program stopped mid-recording) or never "
            "saved, found in the media folder's rec/. start_seconds is where each was "
            "recorded; seconds includes audio written after the file's last header update.";
        c.tags = {"project", "recording"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result =
            schema::object<RecoverableList>("recoverable_list")
                .field("warnings", &RecoverableList::warnings,
                       schema::arrayOf(recordingWarningSchema()))
                .field("recordings", &RecoverableList::recordings,
                       schema::arrayOf(
                           schema::object<RecoverableInfo>("recoverable_recording")
                               .field("file", &RecoverableInfo::file, schema::string())
                               .field("track", &RecoverableInfo::track, schema::string())
                               .field("start_seconds", &RecoverableInfo::startSeconds,
                                      schema::number())
                               .field("seconds", &RecoverableInfo::seconds, schema::number())
                               .field("channels", &RecoverableInfo::channels, schema::integer())
                               .build()))
                .build();
        c.examples = {{"After a crash while recording", obj("{}"),
                       obj(R"({"recordings": [{"file": "rec/Vocal-1.wav", "track": "tr_1",
                              "start_seconds": 1.0, "seconds": 1.0, "channels": 2}]})"),
                       "recording_interrupted"},
                      {"Nothing to recover", obj("{}"), obj(R"({"recordings": []})"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<RecoverableList> {
            std::vector<RecordingWarning> warnings;
            RecoverableList out;
            out.recordings = A(ctx).recoverable(&warnings);
            if (!warnings.empty()) out.warnings = std::move(warnings);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<RecoverArgs, RecoverResult> c;
        c.name = "project.recover";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Put recordings a crash left out back into the project (one undo step).";
        c.description =
            "files: as project.recoverable lists them (omit for all). Each becomes an item on "
            "the track it was recorded on (or track) at the position it was recorded.";
        c.tags = {"project", "recording"};
        c.args = schema::object<RecoverArgs>("recover_args")
                     .field("files", &RecoverArgs::files, schema::arrayOf(schema::string()))
                     .field("track", &RecoverArgs::track, idArg("tr", "Track"))
                     .build();
        c.result =
            schema::object<RecoverResult>("recover_result")
                .field("recovered", &RecoverResult::recovered, schema::arrayOf(recordedSchema()))
                .field("warnings", &RecoverResult::warnings,
                       schema::arrayOf(recordingWarningSchema()))
                .build();
        c.errors = {{"recover.nothing", "there is nothing to recover", "project.recoverable"},
                    {"recover.unknown_file", "a file is not a recording to recover",
                     "project.recoverable lists them"},
                    {"recover.duplicate_file", "a file is listed twice", "list each once"},
                    {"recover.no_track", "the track it was recorded on is gone", "give track"}};
        c.examples = {{"Recover everything", obj("{}"),
                       obj(R"({"recovered": [{"track": "tr_1", "item": "it_1", "take": "tk_1",
                              "file": "<any>", "seconds": 1.0, "lost_frames": 0,
                              "takes": ["tk_1"]}]})"),
                       "recording_interrupted"}};
        c.run = [](const RecoverArgs& a, Context& ctx) -> Result<RecoverResult> {
            EngineApp& app = A(ctx);
            std::optional<Id> track;
            if (a.track) {
                auto id = existing<model::Track>(app.view(), *a.track, "/track");
                if (!id) return std::move(id).error();
                track = *id;
            }
            std::vector<RecordingWarning> warnings;
            auto r = app.recover(a.files.value_or(std::vector<std::string>{}), track, &warnings);
            if (!r) return std::move(r).error();
            RecoverResult out;
            out.recovered = std::move(*r);
            if (!warnings.empty()) out.warnings = std::move(warnings);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- session.describe
    [&] {
        Command<NoArgs, TextResult> c;
        c.name = "session.describe";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Describe the whole session in compact text (tracks, items, fx, routes).";
        c.description =
            "The quickest way to see where things are; use the IDs it shows with other commands.";
        c.tags = {"session"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<TextResult>("text")
                       .field("text", &TextResult::text, schema::string())
                       .build();
        c.examples = {{"Describe", obj("{}"), obj(R"({"text": "<any>"})"), "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TextResult> {
            return TextResult{describeSession(A(ctx).view())};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- tempo.set / timesig.set
    [&] {
        Command<TempoSetArgs, CountResult> c;
        c.name = "tempo.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace the tempo map: points {beat, bpm, ramp?}; the first must be at beat 0.";
        c.description =
            "ramp: true changes the tempo linearly (in beats) to the next point; otherwise it "
            "steps.";
        c.tags = {"tempo"};
        auto point = schema::object<TempoPointArg>("tempo_point")
                         .field("beat", &TempoPointArg::beat, schema::number().min(0))
                         .field("bpm", &TempoPointArg::bpm,
                                schema::number().range(1, 1000).unit(units::Unit::bpm))
                         .field("ramp", &TempoPointArg::ramp, schema::boolean())
                         .defaultValue(false)
                         .build();
        c.args = schema::object<TempoSetArgs>("tempo_set_args")
                     .field("points", &TempoSetArgs::points, schema::arrayOf(point))
                     .build();
        c.result = schema::object<CountResult>("count")
                       .field("count", &CountResult::count, schema::integer())
                       .build();
        c.examples = {
            {"Slow down after 8 bars",
             obj(R"({"points": [{"beat": 0, "bpm": 120}, {"beat": 32, "bpm": 100, "ramp": false}]})"),
             obj(R"({"count": 2})"), "empty"}};
        c.run = [](const TempoSetArgs& a, Context& ctx) -> Result<CountResult> {
            model::EditTxn& t = A(ctx).txn();
            if (a.points.empty() || a.points.front().beat != 0.0)
                return Error("tempo.first", "the first tempo point must be at beat 0",
                             "/points/0/beat", "set the first point beat to 0");
            for (std::size_t i = 1; i < a.points.size(); ++i)
                if (!(a.points[i].beat > a.points[i - 1].beat))
                    return Error(
                        "tempo.order", "tempo points must increase in beat",
                        "/points/" + std::to_string(i) + "/beat",
                        "sort points by increasing beat and remove duplicate beat positions");
            std::vector<Id> old;
            const auto current = A(ctx).view();
            for (const auto& e : current.tempo) old.push_back(e.id);
            for (Id id : old) t.remove<model::TempoPointObj>(id);
            for (const auto& p : a.points) t.create(model::TempoPointObj{p.beat, p.bpm, p.ramp});
            return CountResult{static_cast<std::int64_t>(a.points.size())};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TimesigSetArgs, Empty> c;
        c.name = "timesig.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set the meter from a bar on, e.g. {\"bar\": 9, \"meter\": \"3/4\"}.";
        c.tags = {"tempo"};
        c.args = schema::object<TimesigSetArgs>("timesig_set_args")
                     .field("bar", &TimesigSetArgs::bar, schema::integerOf<int>().min(1))
                     .field("meter", &TimesigSetArgs::meter,
                            schema::string().pattern("^[0-9]{1,2}/[0-9]{1,2}$", "N/D"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Waltz from bar 9", obj(R"({"bar": 9, "meter": "3/4"})"), obj("{}"), "empty"}};
        c.run = [](const TimesigSetArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = A(ctx).txn();
            const auto slash = a.meter.find('/');
            const int n = std::stoi(a.meter.substr(0, slash)),
                      d = std::stoi(a.meter.substr(slash + 1));
            if (n < 1 || n > 64 || d < 1 || d > 64 || (d & (d - 1)))
                return Error("timesig.invalid", "invalid meter " + a.meter, "/meter",
                             "N 1..64, D a power of 2");
            std::optional<Id> at;
            const auto current = A(ctx).view();
            for (const auto& e : current.meters)
                if (e.value->bar == a.bar) at = e.id;
            if (at)
                t.put(*at, model::MeterObj{a.bar, n, d});
            else
                t.create(model::MeterObj{a.bar, n, d});
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- timesig.remove
    [&] {
        Command<TimesigRemoveArgs, Empty> c;
        c.name = "timesig.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove the meter change at a bar: the meter before it continues.";
        c.description = "Bar 1's meter cannot be removed (change it with timesig.set).";
        c.tags = {"tempo"};
        c.args = schema::object<TimesigRemoveArgs>("timesig_remove_args")
                     .field("bar", &TimesigRemoveArgs::bar, schema::integerOf<int>().min(2))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Back to 4/4 at bar 9", obj(R"({"bar": 9})"), obj("{}"), "meter_change"}};
        c.run = [](const TimesigRemoveArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = A(ctx).txn();
            std::optional<Id> at;
            const auto current = A(ctx).view();
            for (const auto& e : current.meters)
                if (e.value->bar == a.bar) at = e.id;
            if (!at)
                return Error("timesig.unknown", "no meter change at bar " + std::to_string(a.bar),
                             "/bar", "timesig changes are listed by session.describe");
            t.remove<model::MeterObj>(*at);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.add
    [&] {
        Command<TrackAddArgs, TrackRef> c;
        c.name = "track.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a channel, buss or master strip and return its id.";
        c.description =
            "parent: a buss/master id (default tr_0); masters have no parent; index: position "
            "among its siblings; name: default the first free ch_N, buss_N or mstr_N. A new "
            "channel gets the user's default monitoring (on unless set otherwise; monitoring "
            "needs the track armed).";
        c.tags = {"track"};
        c.args = schema::object<TrackAddArgs>("track_add_args")
                     .field("type", &TrackAddArgs::type, model::stripTypeSchema())
                     .defaultValue(model::StripType::Channel)
                     .field("name", &TrackAddArgs::name, schema::string())
                     .field("parent", &TrackAddArgs::parent, idArg("tr", "Parent track"))
                     .field("index", &TrackAddArgs::index, schema::integer().min(0))
                     .field("channels", &TrackAddArgs::channels,
                            schema::integer().range(1, model::kMaxTrackChannels))
                     .build();
        c.result = schema::object<TrackRef>("track_ref")
                       .field("track", &TrackRef::track, schema::string())
                       .build();
        c.examples = {
            {"Add a bass track", obj(R"({"name": "Bass"})"), obj(R"({"track": "tr_1"})"), "empty"}};
        c.run = [](const TrackAddArgs& a, Context& ctx) -> Result<TrackRef> {
            model::EditTxn& t = A(ctx).txn();
            Id parent = model::kMaster;
            if (a.parent) {
                auto p = Id::parse(*a.parent);
                if (!p || !t.exists<model::Track>(*p))
                    return Error("tr.unknown", "no track " + *a.parent, "/parent",
                                 "see track.list");
                parent = *p;
            }
            if (a.type == model::StripType::Master && a.parent)
                return Error("track.strip_type", "a master has no parent", "/parent",
                             "omit parent when adding a master");
            if (a.type != model::StripType::Master &&
                t.get<model::Track>(parent)->type == model::StripType::Channel)
                return Error("track.strip_type", "a channel cannot have children", "/parent",
                             "add a buss and place the channels under it");
            if (a.type != model::StripType::Channel && a.channels.value_or(2) < 2)
                return Error("track.channels", "busses and masters require at least two channels",
                             "/channels", "use 2..128 channels");
            model::Track tr;
            tr.type = a.type;
            tr.name = a.name.value_or("");
            if (tr.name.empty()) tr.name = model::nextTrackName(t.preview(), a.type);
            if (a.type == model::StripType::Channel) tr.record.monitor = A(ctx).defaultMonitor();
            if (a.type != model::StripType::Master) tr.parent = parent;
            if (a.channels) tr.channels = static_cast<int>(*a.channels);
            const Id id = t.create(tr);
            if (tr.type == model::StripType::Master) {
                auto p = t.project();
                p.masters.push_back(id);
                t.setProject(std::move(p));
            } else
                t.modify<model::Track>(parent, [&](model::Track& p) {
                    const auto at = a.index
                                        ? std::min<std::size_t>(static_cast<std::size_t>(*a.index),
                                                                p.children.size())
                                        : p.children.size();
                    p.children.insert(p.children.begin() + static_cast<std::ptrdiff_t>(at), id);
                });
            return TrackRef{id.str()};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.remove
    [&] {
        Command<TrackRef, Empty> c;
        c.name = "track.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a track with its items, fx, routes and envelopes (not a folder).";
        c.tags = {"track"};
        c.args = schema::object<TrackRef>("track_ref")
                     .field("track", &TrackRef::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"track": "tr_1"})"), obj("{}"), "song"}};
        c.run = [](const TrackRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            model::EditTxn& t = app.txn();
            const model::Session s = app.view();
            auto idr = existing<model::Track>(s, a.track, "/track");
            if (!idr) return std::move(idr).error();
            const Id id = *idr;
            if (id == model::kMaster)
                return Error("track.strip_type", "tr_0 cannot be removed", "/track",
                             "keep the main master; remove another empty strip instead");
            const model::Track& tr = *s.tracks.find(id);
            if (!tr.children.empty())
                return Error(
                    "tr.has_children",
                    a.track + " is a folder with " + std::to_string(tr.children.size()) + " tracks",
                    "/track", "move or remove its children first");
            std::set<Id> gone(tr.chain.begin(), tr.chain.end());
            gone.insert(tr.inputChain.begin(), tr.inputChain.end());
            gone.insert(id);
            for (const auto& e : s.items)
                if (e.value->track == id) {
                    for (Id tk : e.value->takes) {
                        const model::Take* take = s.takes.find(tk);
                        if (take)
                            for (Id f : take->chain) {
                                t.remove<model::FxInstance>(f);
                                gone.insert(f);
                            }
                        t.remove<model::Take>(tk);
                        gone.insert(tk);  // its envelopes go too
                    }
                    t.remove<model::Item>(e.id);
                }
            for (Id f : tr.chain) t.remove<model::FxInstance>(f);
            for (Id f : tr.inputChain) t.remove<model::FxInstance>(f);
            for (const auto& e : s.routes)
                if ((e.value->from.track && *e.value->from.track == id) ||
                    (e.value->to.track && *e.value->to.track == id) ||
                    (e.value->to.fx && std::count(tr.chain.begin(), tr.chain.end(),
                                                  *e.value->to.fx))) {  // keys into its plug-ins
                    gone.insert(e.id);
                    t.remove<model::Route>(e.id);
                }
            for (const auto& e : s.envelopes)
                if (gone.count(e.value->target.object)) t.remove<model::Envelope>(e.id);
            // Its racks (ADR-0062), and Notes modules elsewhere stop playing it.
            removeRacksOn(t, s, id);
            for (const auto& e : s.modules)
                if (e.value->source == id)
                    t.modify<model::RackModule>(e.id,
                                                [](model::RackModule& m) { m.source.reset(); });
            if (!tr.parent) {
                auto p = t.project();
                std::erase(p.masters, id);
                t.setProject(std::move(p));
            } else
                t.modify<model::Track>(*tr.parent, [&](model::Track& p) {
                    p.children.erase(std::remove(p.children.begin(), p.children.end(), id),
                                     p.children.end());
                });
            for (const auto& g : s.groups)
                if (std::find(g.value->members.begin(), g.value->members.end(), id) !=
                    g.value->members.end())
                    t.modify<model::TrackGroup>(
                        g.id, [&](auto& group) { std::erase(group.members, id); });
            t.remove<model::Track>(id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.set
    [&] {
        Command<TrackSetArgs, Empty> c;
        c.name = "track.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Set track fields: name, type, volume, pan, mute, solo, polarity, mono, arming.";
        c.description =
            "Only the given fields change. solo: off | on | in_place | defeat. armed + "
            "record_input (device input channels, 1-based) prepare a track for "
            "transport.record; record_midi records MIDI instead: hardware audio "
            "and MIDI replace each other. A rack's Out (record_rack) keeps a MIDI input beside "
            "it, which plays its Notes; recording then takes MIDI. null clears each one "
            "(record_input: null takes off hardware audio, record_rack: null a rack's Out), "
            "leaving the others. A device name is a "
            "substring of "
            "the MIDI port (\"*\" = every device), with midi_channel 1-16 (0 = all). monitor: "
            "off | on | auto (auto: while armed) — a monitored track hears its input live (an "
            "instrument plays the keyboard), also while stopped. record_mode: input (new "
            "items), midi_overdub (merge into the MIDI item under the start, as a new take), "
            "midi_replace. color: red | orange | yellow | green | teal | blue | violet | grey | "
            "\"#rrggbb\" (\"\" clears it), shown as a stripe on the track. A master takes "
            "armed alone: transport.record then records its output (after its fader, before "
            "its own items) as an item on the master; lane_mute silences a master's items.";
        c.tags = {"track", "mixer"};
        c.args =
            schema::object<TrackSetArgs>("track_set_args")
                .field("bypass_groups", &TrackSetArgs::bypassGroups, schema::boolean())
                .defaultValue(false)
                .field("track", &TrackSetArgs::track, idArg("tr", "Track"))
                .field("type", &TrackSetArgs::type, model::stripTypeSchema())
                .field("name", &TrackSetArgs::name, schema::string())
                .field("color", &TrackSetArgs::color, schema::string())
                .field("notes", &TrackSetArgs::notes, schema::string())
                .field("volume_db", &TrackSetArgs::volumeDb,
                       schema::number().range(model::kSilenceDb, 12).unit(units::Unit::dB))
                .field("pan", &TrackSetArgs::pan,
                       schema::number().range(-100, 100).unit(units::Unit::percent))
                .field("pan_mode", &TrackSetArgs::panMode,
                       model::panModeSchema().doc(
                           "Channels and busses: pan (balance a stereo image, pan a mono one), "
                           "dual (pan_left and pan_right place each input on its own) or width "
                           "(width narrows, widens or swaps the image, pan places it)"))
                .field("width", &TrackSetArgs::width,
                       schema::number()
                           .range(-100, 100)
                           .unit(units::Unit::percent)
                           .doc("Width mode: 100 as recorded, 0 mono, -100 sides swapped"))
                .field("pan_left", &TrackSetArgs::panLeft,
                       schema::number()
                           .range(-100, 100)
                           .unit(units::Unit::percent)
                           .doc("Dual mode: where the left input goes (default -100)"))
                .field("pan_right", &TrackSetArgs::panRight,
                       schema::number()
                           .range(-100, 100)
                           .unit(units::Unit::percent)
                           .doc("Dual mode: where the right input goes (default 100)"))
                .field("pan_law", &TrackSetArgs::panLaw,
                       clearable("clearable_pan_law",
                                 model::panLawSchema().doc(
                                     "This strip's pan law: db is the centre's cut (0, -2.5, "
                                     "-3, -4.5 or -6); boost raises the sides instead. null "
                                     "follows project.pan_law")))
                .field("mute", &TrackSetArgs::mute, schema::boolean())
                .field("phase_invert", &TrackSetArgs::phaseInvert,
                       schema::boolean().doc("Flip the polarity of the main pair (after the FX "
                                             "chain, before the fader)"))
                .field("mono", &TrackSetArgs::mono,
                       schema::boolean().doc("Sum the main pair to mono (after the FX chain, "
                                             "before the fader and pan): a mono check on the "
                                             "master"))
                .field("solo", &TrackSetArgs::solo,
                       schema::string().pattern("^(off|on|in_place|defeat)$",
                                                "off, on, in_place or defeat"))
                .field("armed", &TrackSetArgs::armed, schema::boolean())
                .field("record_input", &TrackSetArgs::recordInput,
                       clearable("clearable_channels",
                                 schema::arrayOf(schema::integer().range(1, 64))))
                .field("record_midi", &TrackSetArgs::recordMidi, clearableString(schema::string()))
                .field("record_rack", &TrackSetArgs::recordRack,
                       clearableString(
                           idArg("mo", "Out module")
                               .doc("A rack's Out module as the channel's input: the rack plays "
                                    "on the channel, before its inserts (rack.list lists them); "
                                    "null takes a rack's Out off the input")))
                .field("midi_channel", &TrackSetArgs::midiChannel, schema::integer().range(0, 16))
                .field("record_mode", &TrackSetArgs::recordMode,
                       schema::enumeration<model::RecordMode>(
                           {{model::RecordMode::Input, "input"},
                            {model::RecordMode::MidiOverdub, "midi_overdub"},
                            {model::RecordMode::MidiReplace, "midi_replace"}}))
                .field("monitor", &TrackSetArgs::monitor,
                       schema::enumeration<model::Monitor>({{model::Monitor::Off, "off"},
                                                            {model::Monitor::On, "on"},
                                                            {model::Monitor::Auto, "auto"}}))
                .field("parent_send", &TrackSetArgs::parentSend,
                       schema::boolean().doc("The implicit send to the parent buss/master "
                                             "(off: only explicit sends leave the strip)"))
                .field("timebase", &TrackSetArgs::timebase,
                       model::trackTimebaseSchema().doc(
                           "What this channel's items follow when the tempo changes (those "
                           "that inherit it): project, beats, time or beats_position; they keep "
                           "their place now"))
                .field(
                    "channels", &TrackSetArgs::channels,
                    schema::integer()
                        .range(1, model::kMaxTrackChannels)
                        .doc("1-2: the strip's main pair; up to 128 adds auxiliary channels 3-128 "
                             "for routes and plug-in pins (a detector on 3/4, a multi-out "
                             "instrument's outputs), outside the fader, pan and parent send"))
                .field("hw_output", &TrackSetArgs::hwOutput,
                       schema::arrayOf(schema::integer().range(1, model::kMaxDeviceOutputs))
                           .doc("Masters: the device outputs the main pair plays to: [l, r], "
                                "[mono] or [] for none (engine.inputs lists them). Default: "
                                "tr_0 [1, 2], other masters []"))
                .field("lane_mute", &TrackSetArgs::laneMute,
                       schema::boolean().doc("Masters: silence the master's own items (its "
                                             "prints) and nothing else"))
                .build();
        c.result = emptySchema();
        c.examples = {
            {"Turn the bass down 4 dB and a little left",
             obj(R"({"track": "tr_1", "volume_db": "-4 dB", "pan": -15})"), obj("{}"), "song"},
            {"Arm for recording from inputs 1 and 2",
             obj(R"({"track": "tr_1", "armed": true, "record_input": [1, 2]})"), obj("{}"), "song"},
            {"Feed the parent only through explicit sends",
             obj(R"({"track": "tr_1", "parent_send": false})"), obj("{}"), "song"},
            {"Play and record a MIDI keyboard (any device, channel 1)",
             obj(R"({"track": "tr_1", "armed": true, "record_midi": "*", "midi_channel": 1, "monitor": "auto"})"),
             obj("{}"), "song"},
            {"Narrow a stereo strip to half width, a little right",
             obj(R"({"track": "tr_1", "pan_mode": "width", "width": 50, "pan": 20})"), obj("{}"),
             "song"},
            {"Give a strip the -3 dB pan law", obj(R"({"track": "tr_1", "pan_law": {"db": -3}})"),
             obj("{}"), "song"},
            {"Check the mix in mono", obj(R"({"track": "tr_0", "mono": true})"), obj("{}"), "song"},
            {"Four channels: a key signal can arrive on 3/4",
             obj(R"({"track": "tr_1", "channels": 4})"), obj("{}"), "song"},
            {"Play the main mix to outputs 3 and 4",
             obj(R"({"track": "tr_0", "hw_output": [3, 4]})"), obj("{}"), "song"},
            {"Arm the main master to print the mix", obj(R"({"track": "tr_0", "armed": true})"),
             obj("{}"), "song"},
            {"Hear the live mix without the print", obj(R"({"track": "tr_0", "lane_mute": true})"),
             obj("{}"), "song"}};
        c.run = [](const TrackSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Track>(s, a.track, "/track");
            if (!id) return std::move(id).error();
            if (a.color && !model::validTrackColor(*a.color))
                return Error("track.color", "unknown colour \"" + *a.color + "\"", "/color",
                             "red, orange, yellow, green, teal, blue, violet, grey or \"#rrggbb\"; "
                             "\"\" clears it");
            const model::StripType newType = a.type.value_or(s.tracks.find(*id)->type);
            if ((a.recordInput || a.recordMidi || a.recordRack || a.midiChannel || a.recordMode ||
                 a.monitor) &&
                newType != model::StripType::Channel)
                return Error("track.strip_type", "only channels have record inputs or monitor",
                             "/track",
                             newType == model::StripType::Master
                                 ? "arm the master alone (armed: true): it records its output"
                                 : "record or monitor on a child channel instead");
            if (a.armed && *a.armed && newType == model::StripType::Buss)
                return Error("track.strip_type", "only channels and masters can be armed", "/armed",
                             "arm a child channel, or the master to print the mix");
            if (a.laneMute && newType != model::StripType::Master)
                return Error("track.strip_type", "only masters have a lane mute", "/lane_mute",
                             "mute the items themselves (item.set {mute}) or the strip");
            if ((a.panMode || a.width || a.panLeft || a.panRight || a.panLaw) &&
                newType == model::StripType::Master)
                return Error("track.strip_type",
                             "a master's pan is a balance, with no pan mode or law",
                             a.panLaw    ? "/pan_law"
                             : a.panMode ? "/pan_mode"
                                         : "/width",
                             "use pan on the master; set pan modes and laws on channels and "
                             "busses");
            std::optional<model::PanLaw> panLaw;
            if (a.panLaw && !a.panLaw->isNull()) {
                schema::DecodeCtx decode;
                panLaw.emplace();
                if (!model::panLawSchema().decodeInto(*a.panLaw, *panLaw, decode, "/pan_law"))
                    return Error("pan_law.db", "invalid pan law", "/pan_law",
                                 "give {db: 0|-2.5|-3|-4.5|-6, boost: true|false} or null");
            }
            // null clears that input; a value sets it.
            const bool clearAudio = a.recordInput && a.recordInput->isNull();
            const bool clearRack = a.recordRack && a.recordRack->isNull();
            std::optional<std::vector<std::int64_t>> recordInput;
            if (a.recordInput && !clearAudio) {
                recordInput.emplace();
                for (const json::Value& ch : a.recordInput->asArray())
                    recordInput->push_back(ch.asInt());
            }
            const std::optional<std::string> recordRack =
                a.recordRack && !clearRack ? std::optional(a.recordRack->asString()) : std::nullopt;
            if (recordRack && recordInput)
                return Error("track.record_input", "choose one audio input: hardware or a rack Out",
                             "/record_rack",
                             "give either record_rack or record_input; MIDI is independent");
            if (recordInput && a.recordMidi && !a.recordMidi->isNull())
                return Error("track.record_input", "record hardware audio or MIDI, not both",
                             "/record_midi",
                             "provide either record_input for audio or record_midi for MIDI; only "
                             "a rack's Out (record_rack) keeps a MIDI input beside it");
            if (a.hwOutput && a.type.value_or(s.tracks.find(*id)->type) != model::StripType::Master)
                return Error("track.strip_type", "only masters play to hardware outputs",
                             "/hw_output",
                             "send this strip to a master and set that master's hw_output");
            if (a.hwOutput && (a.hwOutput->size() > 2 ||
                               (a.hwOutput->size() == 2 && (*a.hwOutput)[0] == (*a.hwOutput)[1])))
                return Error("track.hw_output",
                             "hw_output is two different channels, one channel or none",
                             "/hw_output", "e.g. [3, 4] for a pair, [5] for mono, [] for none");
            if (a.parentSend && !s.tracks.find(*id)->parent)
                return Error("track.parent_send", "a master has no parent", "/parent_send",
                             "masters feed hardware outputs and renders");
            if (a.type && *a.type != s.tracks.find(*id)->type) {
                if (*id == model::kMaster)
                    return Error("track.strip_type", "tr_0 cannot be retyped", "/type",
                                 "keep the main master; add another strip instead");
                model::EditTxn& txn = app.txn();
                const auto& old = *s.tracks.find(*id);
                if (*a.type == model::StripType::Master) {
                    txn.modify<model::Track>(*old.parent,
                                             [&](auto& p) { std::erase(p.children, *id); });
                    auto p = txn.project();
                    p.masters.push_back(*id);
                    txn.setProject(std::move(p));
                    txn.modify<model::Track>(*id, [](auto& tr) { tr.parent.reset(); });
                } else if (old.type == model::StripType::Master) {
                    auto p = txn.project();
                    std::erase(p.masters, *id);
                    txn.setProject(std::move(p));
                    txn.modify<model::Track>(model::kMaster,
                                             [&](auto& tr) { tr.children.push_back(*id); });
                    txn.modify<model::Track>(*id, [](auto& tr) {
                        tr.parent = model::kMaster;
                        tr.hwOutput.reset();  // only masters play to hardware
                    });
                }
                txn.modify<model::Track>(*id, [&](auto& tr) {
                    tr.type = *a.type;
                    // A new channel's default monitoring goes with the channel.
                    if (*a.type != model::StripType::Channel)
                        tr.record.monitor = model::Monitor::Off;
                    // Only masters have a lane mute; busses are never armed (ADR-0057).
                    if (*a.type != model::StripType::Master) tr.laneMute = false;
                    if (*a.type == model::StripType::Buss) tr.record.armed = false;
                });
            }
            // The record input is a route from the device inputs (hw_input).
            std::optional<Id> input = s.tracks.find(*id)->record.input;
            std::optional<Id> midiInput = s.tracks.find(*id)->record.midiInput;
            if (input)
                if (const auto* old = s.routes.find(*input); old && old->from.midiDevice) {
                    if (!midiInput) midiInput = input;
                    input.reset();
                }
            // A route and the automation of its level and pan.
            auto removeRoute = [&](Id route) {
                for (const auto& env : s.envelopes)
                    if (env.value->target.object == route) {
                        for (const auto& item : s.autoItems)
                            if (item.value->envelope == env.id)
                                app.txn().remove<model::AutomationItem>(item.id);
                        app.txn().remove<model::Envelope>(env.id);
                    }
                app.txn().remove<model::Route>(route);
            };
            const model::Route* audioRoute = input ? s.routes.find(*input) : nullptr;
            bool hardwareAudio = audioRoute && audioRoute->from.hwChannels;
            // No hardware audio input (record_input: null), or no rack Out
            // (record_rack: null): each takes off only an input of its kind.
            if ((clearAudio && hardwareAudio) ||
                (clearRack && audioRoute && audioRoute->from.module)) {
                removeRoute(*input);
                input.reset();
                hardwareAudio = false;
            }
            const bool clearMidi = a.recordMidi && a.recordMidi->isNull();
            if ((a.recordMidi && !clearMidi) || a.midiChannel)
                if (!a.recordMidi && !(midiInput && s.routes.find(*midiInput)))
                    return Error("track.record_midi", "give record_midi (the device) first",
                                 "/midi_channel",
                                 "set record_midi to a device from engine.inputs first");
            if (recordInput) {
                if (recordInput->empty())
                    return Error("track.record_input", "give one or more input channels",
                                 "/record_input", "e.g. [1, 2] for a stereo pair");
                // Hardware audio and MIDI are exclusive (ADR-0065): the MIDI
                // route becomes the audio one, as before independent inputs.
                if (midiInput && !clearMidi) {
                    if (!input)
                        input = midiInput;
                    else
                        removeRoute(*midiInput);
                    midiInput.reset();
                }
                std::vector<int> channels(recordInput->begin(), recordInput->end());
                if (input && s.routes.find(*input)) {
                    app.txn().modify<model::Route>(*input, [&](model::Route& r) {
                        r.from.hwChannels = channels;
                        r.from.midiDevice.reset();  // audio replaces MIDI
                        r.from.module.reset();      // and a rack's Out
                        r.midi.reset();
                    });
                } else {
                    model::Route r;
                    r.from.hwChannels = channels;
                    r.to.track = *id;
                    input = app.txn().create(r);
                }
            }
            if (clearMidi) {
                if (midiInput) removeRoute(*midiInput);
                midiInput.reset();
            } else if (a.recordMidi || a.midiChannel) {
                // MIDI replaces hardware audio; only a rack's Out stays beside it.
                if (hardwareAudio) {
                    if (!midiInput)
                        midiInput = input;
                    else
                        removeRoute(*input);
                    input.reset();
                }
                const model::Route* old = midiInput ? s.routes.find(*midiInput) : nullptr;
                auto setMidi = [&](model::Route& r) {
                    r.from = model::RouteEnd{};
                    r.from.midiDevice =
                        a.recordMidi ? a.recordMidi->asString() : *old->from.midiDevice;
                    model::MidiRouting m = r.midi.value_or(model::MidiRouting{});
                    if (a.midiChannel) m.fromChannel = static_cast<int>(*a.midiChannel);
                    r.midi = m;
                };
                if (old)
                    app.txn().modify<model::Route>(*midiInput, setMidi);
                else {
                    model::Route r;
                    r.to.track = *id;
                    setMidi(r);
                    midiInput = app.txn().create(r);
                }
            }
            if (recordRack) {  // a rack's Out module (ADR-0062)
                auto out = Id::parse(*recordRack);
                const model::RackModule* m = out ? s.modules.find(*out) : nullptr;
                if (!m || m->type != model::kOutModule)
                    return Error("rack.port", *recordRack + " is not an Out module", "/record_rack",
                                 "rack.list lists every rack's Out modules");
                if (trackFeedsRack(s, *id, m->rack))
                    return Error(
                        "rack.cycle",
                        a.track + " sends into " + m->rack.str() + ", so it cannot hear that rack",
                        "/record_rack",
                        "remove the send into the rack's In module, or pick another Out");
                if (input && s.routes.find(*input)) {
                    app.txn().modify<model::Route>(*input, [&](model::Route& r) {
                        r.from = model::RouteEnd{};
                        r.from.module = *out;
                        r.midi.reset();
                    });
                } else {
                    model::Route r;
                    r.from.module = *out;
                    r.to.track = *id;
                    input = app.txn().create(r);
                }
            }
            app.txn().modify<model::Track>(*id, [&](model::Track& t) {
                if (a.armed) t.record.armed = *a.armed;
                if (a.recordMode) t.record.mode = *a.recordMode;
                if (a.recordInput || a.recordMidi || a.recordRack || a.midiChannel)
                    t.record.input =
                        input ? input : midiInput;  // retain the MIDI-only legacy alias
                if (a.recordInput || a.recordMidi || a.recordRack || a.midiChannel)
                    t.record.midiInput = midiInput;
                if (a.monitor) t.record.monitor = *a.monitor;
                if (a.name) t.name = *a.name;
                if (a.color) t.color = *a.color;
                if (a.notes) t.notes = *a.notes;
                if (a.volumeDb) t.volumeDb = *a.volumeDb;
                if (a.pan) t.pan = *a.pan;
                if (a.panMode) t.panMode = *a.panMode;
                if (a.width) t.width = *a.width;
                if (a.panLeft) t.panLeft = *a.panLeft;
                if (a.panRight) t.panRight = *a.panRight;
                if (a.panLaw) t.panLaw = panLaw;
                if (a.mute) t.mute = *a.mute;
                if (a.phaseInvert) t.phaseInvert = *a.phaseInvert;
                if (a.mono) t.mono = *a.mono;
                if (a.parentSend) t.parentSend.enabled = *a.parentSend;
                if (a.timebase) t.timebase = *a.timebase;
                if (a.channels) t.channels = static_cast<int>(*a.channels);
                if (a.hwOutput)
                    t.hwOutput = std::vector<int>(a.hwOutput->begin(), a.hwOutput->end());
                if (a.laneMute) t.laneMute = *a.laneMute;
                if (a.solo)
                    t.solo = *a.solo == "on"         ? model::Solo::On
                             : *a.solo == "in_place" ? model::Solo::InPlace
                             : *a.solo == "defeat"   ? model::Solo::Defeat
                                                     : model::Solo::Off;
            });
            if (a.timebase && *a.timebase != s.tracks.find(*id)->timebase) {
                // Its items that inherit the track's timebase keep their place.
                const model::Timebase to =
                    *a.timebase == model::TrackTimebase::Time    ? model::Timebase::Time
                    : *a.timebase == model::TrackTimebase::Beats ? model::Timebase::Beats
                    : *a.timebase == model::TrackTimebase::BeatsPosition
                        ? model::Timebase::BeatsPosition
                        : s.project->timebase;
                for (const auto& e : s.items) {
                    if (e.value->track != *id || e.value->timebase != model::ItemTimebase::Track)
                        continue;
                    app.txn().modify<model::Item>(e.id, [&](model::Item& it) {
                        model::retimeItem(s, it, model::itemTimebase(s, it), to);
                    });
                }
            }
            // Expand each linked attribute once from the pre-edit snapshot. Selection
            // editing uses bypassed explicit targets, so overlaps never multiply a drag.
            if (!a.bypassGroups) {
                const auto& source = *s.tracks.find(*id);
                auto spread = [&](std::string_view attribute, auto change) {
                    for (Id member : model::linkedTracks(s, *id, attribute)) {
                        if (member == *id) continue;
                        const auto& old = *s.tracks.find(member);
                        // Arm spreads to channels and masters (ADR-0057), monitoring
                        // to channels.
                        if ((attribute == "armed" && old.type == model::StripType::Buss) ||
                            (attribute == "monitor" && old.type != model::StripType::Channel))
                            continue;
                        if (attribute == "pan" && !a.pan && old.type == model::StripType::Master)
                            continue;
                        app.txn().modify<model::Track>(member, [&](auto& tr) { change(tr, old); });
                        const auto tr = *app.view().tracks.find(member);
                        if (attribute == "volume")
                            app.recordAutomation(member, "volume", tr.volumeDb);
                        if (attribute == "pan") {
                            if (a.pan) app.recordAutomation(member, "pan", tr.pan);
                            if (a.width && old.type != model::StripType::Master)
                                app.recordAutomation(member, "width", tr.width);
                            if (a.panLeft && old.type != model::StripType::Master)
                                app.recordAutomation(member, "pan_left", tr.panLeft);
                            if (a.panRight && old.type != model::StripType::Master)
                                app.recordAutomation(member, "pan_right", tr.panRight);
                            if (a.panLaw && model::panLawOf(*s.project, old).boost !=
                                                model::panLawOf(*s.project, tr).boost)
                                app.recordAutomation(member, "pan_boost",
                                                     model::panLawOf(*s.project, tr).boost ? 1 : 0);
                        }
                        if (attribute == "mute")
                            app.recordAutomation(member, "mute", tr.mute ? 1 : 0);
                    }
                };
                if (a.volumeDb)
                    spread("volume", [&](auto& tr, const auto& old) {
                        if (*a.volumeDb <= model::kSilenceDb)
                            tr.volumeDb = model::kSilenceDb;
                        else if (source.volumeDb > model::kSilenceDb &&
                                 old.volumeDb > model::kSilenceDb)
                            tr.volumeDb = std::clamp(old.volumeDb + *a.volumeDb - source.volumeDb,
                                                     model::kSilenceDb, 12.0);
                    });
                if (a.pan || a.width || a.panLeft || a.panRight || a.panMode || a.panLaw)
                    spread("pan", [&](auto& tr, const auto& old) {
                        if (a.pan)
                            tr.pan = std::clamp(old.pan + *a.pan - source.pan, -100.0, 100.0);
                        if (old.type == model::StripType::Master) return;
                        if (a.width)
                            tr.width =
                                std::clamp(old.width + *a.width - source.width, -100.0, 100.0);
                        if (a.panLeft)
                            tr.panLeft = std::clamp(old.panLeft + *a.panLeft - source.panLeft,
                                                    -100.0, 100.0);
                        if (a.panRight)
                            tr.panRight = std::clamp(old.panRight + *a.panRight - source.panRight,
                                                     -100.0, 100.0);
                        if (a.panMode) tr.panMode = *a.panMode;
                        if (a.panLaw) tr.panLaw = panLaw;
                    });
                if (a.mute) spread("mute", [&](auto& tr, const auto&) { tr.mute = *a.mute; });
                if (a.solo) {
                    const auto value = app.view().tracks.find(*id)->solo;
                    spread("solo", [&](auto& tr, const auto&) { tr.solo = value; });
                }
                if (a.armed)
                    spread("armed", [&](auto& tr, const auto&) { tr.record.armed = *a.armed; });
                if (a.monitor)
                    spread("monitor",
                           [&](auto& tr, const auto&) { tr.record.monitor = *a.monitor; });
            }
            // Touch/latch/write envelopes record the change while the transport plays.
            if (a.volumeDb) app.recordAutomation(*id, "volume", *a.volumeDb);
            if (a.pan) app.recordAutomation(*id, "pan", *a.pan);
            if (a.width) app.recordAutomation(*id, "width", *a.width);
            if (a.panLeft) app.recordAutomation(*id, "pan_left", *a.panLeft);
            if (a.panRight) app.recordAutomation(*id, "pan_right", *a.panRight);
            if (a.panLaw) {
                const bool before = model::panLawOf(*s.project, *s.tracks.find(*id)).boost;
                const bool after = model::panLawOf(*s.project, *app.view().tracks.find(*id)).boost;
                if (before != after) app.recordAutomation(*id, "pan_boost", after ? 1 : 0);
            }
            if (a.mute) app.recordAutomation(*id, "mute", *a.mute ? 1.0 : 0.0);
            if (a.parentSend && *a.parentSend && !s.tracks.find(*id)->parentSend.enabled)
                if (auto valid = model::checkInvariants(app.view()); !valid)
                    return Error(
                        "route.cycle", "the parent send would feed back: " + valid.error().message,
                        "/parent_send", "remove the send from the parent to this strip first");
            if (a.type && *a.type != s.tracks.find(*id)->type) {
                SUPPLE_TRY(model::validateStripRules(app.view()));
                auto valid = model::checkInvariants(app.view());
                if (!valid) {
                    auto error = std::move(valid).error();
                    error.code = "track.strip_type";
                    error.hint =
                        "keep a type and parent that preserve the routing graph; use a buss to "
                        "group";
                    return error;
                }
            }
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- track.list
    [&] {
        Command<TrackListArgs, TrackList> c;
        c.name = "track.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List tracks in tree order with their level, mute/solo, fx and items.";
        c.tags = {"track"};
        c.args = schema::object<TrackListArgs>("track_list_args")
                     .field("at", &TrackListArgs::at, schema::number().min(0))
                     .build();
        auto entry = schema::object<TrackEntry>("track_entry")
                         .field("frozen", &TrackEntry::frozen, schema::boolean())
                         .field("freeze", &TrackEntry::freeze, model::trackFreezeSchema())
                         .field("id", &TrackEntry::id, schema::string())
                         .field("name", &TrackEntry::name, schema::string())
                         .field("type", &TrackEntry::type, model::stripTypeSchema())
                         .field("parent", &TrackEntry::parent, schema::string())
                         .field("depth", &TrackEntry::depth, schema::integer())
                         .field("volume_db", &TrackEntry::volumeDb, schema::number())
                         .field("mute", &TrackEntry::mute, schema::boolean())
                         .field("solo", &TrackEntry::solo, schema::string())
                         .field("fx", &TrackEntry::fx, schema::arrayOf(schema::string()))
                         .field("items", &TrackEntry::items, schema::arrayOf(schema::string()))
                         .field("groups", &TrackEntry::groups, schema::arrayOf(model::idSchema()))
                         .field("vcas", &TrackEntry::vcas, schema::arrayOf(model::idSchema()))
                         .field("hw_output", &TrackEntry::hwOutput,
                                schema::arrayOf(schema::integer())
                                    .doc("Masters: the device outputs they play to"))
                         .field("armed", &TrackEntry::armed,
                                schema::boolean().doc("Masters: recording takes the output"))
                         .field("lane_mute", &TrackEntry::laneMute,
                                schema::boolean().doc("Masters: their own items are silent"))
                         .field("requested_effective_volume_db", &TrackEntry::requestedEffective,
                                schema::number())
                         .field("effective_volume_db", &TrackEntry::effective, schema::number())
                         .field("at", &TrackEntry::at, schema::number())
                         .field("effective_mute", &TrackEntry::effectiveMute, schema::boolean())
                         .field("solo_silenced", &TrackEntry::soloSilenced, schema::boolean())
                         .build();
        c.result = schema::object<TrackList>("track_list")
                       .field("tracks", &TrackList::tracks, schema::arrayOf(entry))
                       .build();
        c.examples = {{"List", obj("{}"), obj(R"({"tracks": "<any>"})"), "song"}};
        c.run = [](const TrackListArgs& args, Context& ctx) -> Result<TrackList> {
            const model::Session s = A(ctx).view();
            TrackList out;
            static constexpr const char* kSolo[] = {"off", "on", "in_place", "defeat"};
            std::function<void(Id, int)> walk = [&](Id id, int depth) {
                const model::Track& t = *s.tracks.find(id);
                TrackEntry e;
                e.frozen = t.freeze.has_value();
                e.freeze = t.freeze;
                e.id = id.str();
                e.name = t.name;
                e.type = t.type;
                e.parent = t.parent ? t.parent->str() : "";
                e.depth = depth;
                e.volumeDb = t.volumeDb;
                e.at = args.at.value_or(A(ctx).engine() ? A(ctx).engine()->transport().seconds : 0);
                e.groups = model::trackGroups(s, id);
                e.vcas = model::vcaControllers(s, id);
                if (t.type == model::StripType::Master) {
                    const auto channels = model::hardwareOutputs(t, id);
                    e.hwOutput = std::vector<std::int64_t>(channels.begin(), channels.end());
                    e.armed = t.record.armed;
                    e.laneMute = t.laneMute;
                }
                const auto level = model::effectiveTrackLevel(s, id, e.at);
                e.requestedEffective = level.requestedDb;
                e.effective = level.appliedDb;
                e.effectiveMute = level.mute;
                e.soloSilenced = level.soloSilenced;
                e.mute = t.mute;
                e.solo = kSolo[static_cast<int>(t.solo)];
                for (Id f : t.chain) e.fx.push_back(f.str());
                for (const auto& it : s.items)
                    if (it.value->track == id) e.items.push_back(it.id.str());
                out.tracks.push_back(std::move(e));
                for (Id c2 : t.children) walk(c2, depth + 1);
            };
            for (Id master : s.project->masters) walk(master, 0);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- item.add
    [&] {
        Command<ItemAddArgs, ItemAddResult> c;
        c.name = "item.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add an item on a track: MIDI in notation, a MIDI file or an audio file.";
        c.description =
            "position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "
            "\"voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h\" — every bar must add up; the item "
            "is as long as the written bars unless length (beats) is given. audio_file: a path at "
            "the project's sample rate; with the app's copy-imports setting on, the project "
            "keeps its own copy in its media folder. midi_file: a Standard MIDI File (type 0 or 1, "
            "every track merged, channels kept); its notes keep their beats and follow the "
            "project's tempo (the file's tempo and meter are not applied; a warning says when they "
            "differ), and the item is as long as the file unless length is given. A master holds "
            "audio items only (prints, played after its fader); busses hold none.";
        c.tags = {"item", "midi"};
        c.args =
            schema::object<ItemAddArgs>("item_add_args")
                .field("track", &ItemAddArgs::track, idArg("tr", "Track"))
                .field("position", &ItemAddArgs::position, schema::number().min(0).doc("Beats."))
                .field("bar", &ItemAddArgs::bar,
                       schema::integer()
                           .range(1, std::numeric_limits<int>::max())
                           .doc("Start at this bar (1-based)."))
                .field("length", &ItemAddArgs::length,
                       schema::number().exclusiveMin(0).doc("Beats."))
                .field("notation", &ItemAddArgs::notation, schema::string())
                .field("audio_file", &ItemAddArgs::audioFile, schema::string())
                .field("midi_file", &ItemAddArgs::midiFile, schema::string())
                .field("name", &ItemAddArgs::name, schema::string())
                .build();
        c.result =
            schema::object<ItemAddResult>("item_add")
                .field("item", &ItemAddResult::item, schema::string())
                .field("take", &ItemAddResult::take, schema::string())
                .field("source", &ItemAddResult::source, schema::string())
                .field("position", &ItemAddResult::position, schema::number())
                .field("length", &ItemAddResult::length, schema::number())
                .field("warnings", &ItemAddResult::warnings, schema::arrayOf(schema::string()))
                .build();
        c.errors = {
            {"notation.*", "the notation does not compile", "line:col and a hint in the error"},
            {"tr.unknown", "no such track", "see track.list"}};
        c.examples = {
            {"A bass line at bar 1",
             obj(R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q r:q C2:q r:q"})"),
             obj(R"({"item": "it_1", "take": "tk_1", "source": "mi_1", "position": 0.0, "length": 4.0})"),
             "song"}};
        c.run = [](const ItemAddArgs& a, Context& ctx) -> Result<ItemAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto track = existing<model::Track>(s, a.track, "/track");
            if (!track) return std::move(track).error();
            SUPPLE_TRY(checkItemTrack(s, *track, {}, "/track"));
            if ((a.notation || a.midiFile) &&
                s.tracks.find(*track)->type == model::StripType::Master)
                return Error("track.strip_type", "a master holds audio items only",
                             a.notation ? "/notation" : "/midi_file",
                             "give audio_file for a master, or put the MIDI on a channel "
                             "with an instrument");
            if ((a.notation ? 1 : 0) + (a.audioFile ? 1 : 0) + (a.midiFile ? 1 : 0) > 1)
                return Error("item.source", "give one of notation, audio_file or midi_file", {},
                             "provide exactly one source for the item");
            const double pos = beatsOf(
                s, a.position, a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
            SUPPLE_TRY(model::checkedBeat(s, pos, a.bar ? "/bar" : "/position"));
            if (a.length) SUPPLE_TRY(model::checkedBeat(s, pos + *a.length, "/length"));
            model::EditTxn& t = app.txn();
            model::Take take;
            take.name = a.name.value_or("");
            double length = a.length.value_or(4.0);
            ItemAddResult r;
            if (a.notation) {
                auto compiled = compileNotation(s, *a.notation, pos);
                if (!compiled) return std::move(compiled).error();
                if (!a.length) length = compiled->second;
                const Id mi = t.create(std::move(compiled->first));
                take.source = mi;
                r.source = mi.str();
            } else if (a.midiFile) {
                auto bytes = readFile(app.resolve(*a.midiFile), midi::kMaxSmfBytes);
                if (!bytes) return std::move(bytes).error().at("/midi_file");
                midi::SmfInfo info;
                auto seq = midi::readSmf(*bytes, &info);
                if (!seq) return std::move(seq).error().at("/midi_file");
                auto warn = [&r](std::string w) {
                    if (!r.warnings) r.warnings.emplace();
                    r.warnings->push_back(std::move(w));
                };
                // The file's own end, else where its last event ends.
                std::int64_t end = seq->length;
                for (const auto& n : seq->notes) end = std::max(end, n.start + n.length);
                for (const auto& cc : seq->controls) end = std::max(end, cc.tick);
                seq->length = end;
                seq->canonicalize();
                if (!a.length && end > 0) length = static_cast<double>(end) / seq->ppq;
                const double bpm = s.timeMap->bpmAt(pos);
                for (const auto& [tick, tempo] : info.tempos)
                    if (std::abs(tempo - bpm) > 1e-6) {
                        warn("the file's tempo (" + json::formatDouble(tempo) +
                             " BPM) is not applied: its notes follow the "
                             "project's tempo");
                        break;
                    }
                if (info.skippedEvents > 0)
                    warn(std::to_string(info.skippedEvents) +
                         " unsupported events in the file were skipped");
                model::MidiSource src;
                src.sequence = std::move(*seq);
                const Id mi = t.create(std::move(src));
                take.source = mi;
                r.source = mi.str();
            } else if (a.audioFile) {
                // The header, not a decode: the media model needs only the rate,
                // the channel count and the length, and adding one file to many
                // items must not decode it many times. The hash is the source's
                // identity, so a second add of the same bytes reuses the md_*.
                auto info = audio::inspect(*a.audioFile);
                if (!info) return std::move(info).error().at("/audio_file");
                auto hash = sha256File(*a.audioFile);
                if (!hash) return std::move(hash).error().at("/audio_file");
                if (!a.length && info->sampleRate > 0) {
                    const double secs = static_cast<double>(info->frames) / info->sampleRate;
                    length = s.timeMap->beatAt(s.timeMap->secondsAt(pos) + secs) - pos;
                }
                // The same bytes are one media source: a second add of the same
                // file reuses the source it already has. A project saved before
                // hashes existed has a source with none, so the file's path and
                // header decide, and the source's hash is filled in with the
                // bytes on disk now — an old project converges rather than
                // growing a second source for the same file.
                std::optional<model::Id> known = model::mediaSourceWithHash(s, *hash);
                if (!known) {
                    if (const auto legacy =
                            legacyMediaSourceFor(s, *a.audioFile, *info, app.mediaDir()))
                        known = *legacy;
                    if (known)
                        t.modify<model::MediaSource>(
                            *known, [&](model::MediaSource& md) { md.hash = *hash; });
                }
                if (known) {
                    take.source = *known;
                    r.source = known->str();
                } else {
                    model::MediaSource md;
                    md.path = fs::absolute(*a.audioFile).string();
                    if (app.copyImportedMedia() && !app.projectDir().empty()) {
                        // The project's own copy, so it never depends on the
                        // original staying where it is (a replay finds the
                        // same bytes under the same name and reuses them).
                        auto name = detail::copyIntoFolder(*a.audioFile, app.mediaDir());
                        if (!name) return std::move(name).error().at("/audio_file");
                        md.path = *name;
                    }
                    md.hash = *hash;
                    md.format = formatName(info->container);
                    md.sampleRate = info->sampleRate;
                    md.channels = info->channels;
                    md.frames = info->frames;
                    const Id md_ = t.create(std::move(md));
                    take.source = md_;
                    r.source = md_.str();
                }
            }
            const Id tk = t.create(std::move(take));
            model::Item item;
            item.track = *track;
            model::placeItem(s, item, pos, length);  // in the track's timebase
            item.takes = {tk};
            item.activeTake = tk;
            const Id it = t.create(std::move(item));
            r.item = it.str();
            r.take = tk.str();
            r.position = pos;
            r.length = length;
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- item.remove / item.move
    [&] {
        Command<ItemRef, Empty> c;
        c.name = "item.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove an item with its takes (unpooled sources go too).";
        c.tags = {"item"};
        c.args = schema::object<ItemRef>("item_ref")
                     .field("bypass_groups", &ItemRef::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemRef::item, idArg("it", "Item"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"item": "it_1"})"), obj("{}"), "song_with_item"}};
        c.run = [](const ItemRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            model::EditTxn& t = app.txn();
            for (Id tk : s.items.find(*id)->takes) SUPPLE_TRY(removeTakeObjects(t, s, tk));
            t.remove<model::Item>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemMoveArgs, Empty> c;
        c.name = "item.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move an item in time (position in beats or bar) and/or to another track.";
        c.tags = {"item"};
        c.args = schema::object<ItemMoveArgs>("item_move_args")
                     .field("bypass_groups", &ItemMoveArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemMoveArgs::item, idArg("it", "Item"))
                     .field("position", &ItemMoveArgs::position, schema::number().min(0))
                     .field("bar", &ItemMoveArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("track", &ItemMoveArgs::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Two bars later", obj(R"({"item": "it_1", "bar": 3})"), obj("{}"), "song_with_item"}};
        c.run = [](const ItemMoveArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            std::optional<Id> track;
            if (a.track) {
                auto tr = existing<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                SUPPLE_TRY(checkItemTrack(s, *tr, s.items.find(*id)->takes, "/track"));
                track = *tr;
            }
            app.txn().modify<model::Item>(*id, [&](model::Item& it) {
                const double start =
                    a.position || a.bar
                        ? beatsOf(
                              s, a.position,
                              a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt)
                        : model::itemSpan(s, it).startBeat;
                model::moveItem(s, it, track.value_or(it.track), start);
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- midi.write / midi.get
    [&] {
        Command<MidiWriteArgs, Empty> c;
        c.name = "midi.write";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace an item's MIDI with notation (the item keeps its position and length).";
        c.tags = {"midi"};
        c.args = schema::object<MidiWriteArgs>("midi_write_args")
                     .field("item", &MidiWriteArgs::item, idArg("it", "Item"))
                     .field("notation", &MidiWriteArgs::notation, schema::string())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Rewrite the bass line",
                       obj(R"({"item": "it_1", "notation": "voice bass; 1: C2:e C2:e r:q G1:h"})"),
                       obj("{}"), "song_with_item"}};
        c.run = [](const MidiWriteArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            const model::Item& it = *s.items.find(*id);
            const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "use item.add with notation");
            auto compiled = compileNotation(s, a.notation, model::itemSpan(s, it).startBeat);
            if (!compiled) return std::move(compiled).error();
            const model::MidiSource* old = s.midi.find(*take->source);
            compiled->first.pooled = old && old->pooled;
            model::EditTxn& txn = app.txn();
            txn.put<model::MidiSource>(*take->source, std::move(compiled->first));
            if (!old || !old->pooled)
                txn.modify<model::Take>(*it.activeTake,
                                        [](model::Take& tk) { tk.startOffset = 0; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<MidiGetArgs, TextResult> c;
        c.name = "midi.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read an item's MIDI as notation.";
        c.tags = {"midi"};
        c.args = schema::object<MidiGetArgs>("midi_get_args")
                     .field("item", &MidiGetArgs::item, idArg("it", "Item"))
                     .build();
        c.result = schema::object<TextResult>("text")
                       .field("text", &TextResult::text, schema::string())
                       .build();
        c.examples = {{"Read the bass line", obj(R"({"item": "it_1"})"),
                       obj(R"({"text": "<any>"})"), "song_with_item"}};
        c.run = [](const MidiGetArgs& a, Context& ctx) -> Result<TextResult> {
            const model::Session s = A(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& it = *s.items.find(*id);
            const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "choose an item with a MIDI take from track.list or create one with "
                             "item.add notation");
            notation::DecompileOptions options;
            options.meterAt = meterContext(s, model::itemSpan(s, it).startBeat);
            auto text = notation::decompile(s.midi.find(*take->source)->sequence, options);
            if (!text) return std::move(text).error();
            return TextResult{std::move(*text)};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.add
    [&] {
        Command<FxAddArgs, FxAddResult> c;
        c.name = "fx.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Insert a plugin into a track's (or a take's) FX chain and return its id.";
        c.description =
            "plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic "
            "plugins. chain: main (default; the track's effects) or input (the recorded-input "
            "chain). take instead of track: the take's own chain (take FX) — the item plays "
            "through it before the track's input, cut to the item; an instrument there plays a "
            "MIDI take on its own. An instrument with outputs past its main pair (a multi-out "
            "sampler's Kick, Snare...) gets that many more channels on the track, and its "
            "pins.out sends each output there; route.matrix lists them and route.add "
            "{channels: {from: [3, 4]}} sends one to a buss.";
        c.tags = {"fx"};
        c.args = schema::object<FxAddArgs>("fx_add_args")
                     .field("track", &FxAddArgs::track, idArg("tr", "Track"))
                     .field("take", &FxAddArgs::take, idArg("tk", "Take"))
                     .field("plugin", &FxAddArgs::plugin, schema::string().minLength(1))
                     .field("index", &FxAddArgs::index, schema::integer().min(0))
                     .field("patch", &FxAddArgs::patch, schema::any())
                     .field("chain", &FxAddArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .build();
        c.result = schema::object<FxAddResult>("fx_add")
                       .field("fx", &FxAddResult::fx, schema::string())
                       .field("name", &FxAddResult::name, schema::string())
                       .field("semantic", &FxAddResult::semantic, schema::boolean())
                       .field("chain", &FxAddResult::chain, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"plugin.unknown", "no such plugin", "closest ids in the hint; see plugin.list"}};
        c.examples = {
            {"An instrument on the bass track",
             obj(R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})"),
             obj(R"({"fx": "fx_1", "name": "Test Sine", "semantic": false, "chain": ["fx_1"]})"),
             "song"},
            {"An effect in the track's input chain",
             obj(R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain", "chain": "input"})"),
             obj(R"({"fx": "fx_1", "name": "<any>", "semantic": "<any>", "chain": ["fx_1"]})"),
             "song"},
            {"Take FX: a gain on the take of an item",
             obj(R"({"take": "tk_1", "plugin": "com.suppleaudio.test.gain"})"),
             obj(R"({"fx": "fx_1", "name": "<any>", "semantic": "<any>", "chain": ["fx_1"]})"),
             "song_with_item"}};
        c.run = [](const FxAddArgs& a, Context& ctx) -> Result<FxAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            if (a.track.has_value() == a.take.has_value())
                return Error("fx.target", "give track or take", "/track",
                             "track: a track's chain; take: a take's own chain (take FX)");
            std::optional<Id> track, take;
            if (a.track) {
                auto t = existing<model::Track>(s, *a.track, "/track");
                if (!t) return std::move(t).error();
                track = *t;
            } else {
                auto t = existing<model::Take>(s, *a.take, "/take");
                if (!t) return std::move(t).error();
                take = *t;
                if (a.chain && *a.chain == "input")
                    return Error("fx.chain", "a take has one chain", "/chain", "leave chain out");
            }
            auto info = app.findPlugin(a.plugin);
            if (!info) return std::move(info).error().at("/plugin");
            model::FxInstance fx;
            fx.plugin = {info->format, info->id, info->name, info->vendor, info->version};
            if (a.patch) {
                auto merged = app.mergedPatch(fx, *a.patch, false);
                if (!merged) {
                    Error e = std::move(merged).error();
                    e.path = "/patch" + e.path;
                    return e;
                }
                fx.patch = std::move(*merged);
            }
            const bool input = a.chain && *a.chain == "input";
            // A multi-out instrument in a track's chain (ADR-0021): its
            // auxiliary outputs land on new channels after the track's own, as
            // far as the track can grow; out names them after the main pair.
            // An effect's extra outputs stay unconnected until pinned.
            int grownChannels = 0;
            if (track && !input && info->instrument) {
                if (auto ports = app.fxAuxOutputs(fx); ports && !ports->empty()) {
                    int channels = std::max(s.tracks.find(*track)->channels, 2);
                    std::vector<int> out{1, 2};
                    bool connected = false;
                    for (const host::AudioPortInfo& port : *ports)
                        for (std::uint32_t ch = 0; ch < port.channels; ++ch) {
                            if (channels < model::kMaxTrackChannels) {
                                out.push_back(++channels);
                                connected = true;
                            } else {
                                out.push_back(0);
                            }
                        }
                    if (connected) {
                        fx.pins.out = std::move(out);
                        grownChannels = channels;
                    }
                }
            }
            model::EditTxn& t = app.txn();
            const Id id = t.create(std::move(fx));
            if (take) {  // take FX keep the caller's order
                std::vector<Id> chain = s.takes.find(*take)->chain;
                const std::size_t at =
                    a.index
                        ? std::min<std::size_t>(static_cast<std::size_t>(*a.index), chain.size())
                        : chain.size();
                chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), id);
                t.modify<model::Take>(*take, [&](model::Take& tk) { tk.chain = chain; });
                FxAddResult r{id.str(), info->name, info->semantic, {}};
                for (Id f : chain) r.chain.push_back(f.str());
                return r;
            }
            const model::Track* strip = s.tracks.find(*track);
            std::vector<Id> chain = input ? strip->inputChain : strip->chain;
            std::size_t at =
                a.index ? std::min<std::size_t>(static_cast<std::size_t>(*a.index), chain.size())
                        : chain.size();
            // An instrument goes to the front of the track's chain unless the
            // caller said where; the input chain keeps the caller's order.
            if (!input && info->instrument && !a.index) at = 0;
            chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), id);
            t.modify<model::Track>(*track, [&](model::Track& modified) {
                (input ? modified.inputChain : modified.chain) = chain;
                if (grownChannels) modified.channels = grownChannels;
            });
            FxAddResult r{id.str(), info->name, info->semantic, {}};
            for (Id f : chain) r.chain.push_back(f.str());
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.remove / fx.set
    [&] {
        Command<FxRef, Empty> c;
        c.name = "fx.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a plugin instance (and its envelopes).";
        c.tags = {"fx"};
        c.args = schema::object<FxRef>("fx_ref")
                     .field("fx", &FxRef::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"fx": "fx_1"})"), obj("{}"), "song_with_fx"}};
        c.run = [](const FxRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            model::EditTxn& t = app.txn();
            auto drop = [&](std::vector<Id>& chain) {
                chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
            };
            for (const auto& e : s.tracks)
                if (std::count(e.value->chain.begin(), e.value->chain.end(), *id) ||
                    std::count(e.value->inputChain.begin(), e.value->inputChain.end(), *id))
                    t.modify<model::Track>(e.id, [&](model::Track& tr) {
                        drop(tr.chain);
                        drop(tr.inputChain);
                    });
            for (const auto& e : s.takes)
                if (std::count(e.value->chain.begin(), e.value->chain.end(), *id))
                    t.modify<model::Take>(e.id, [&](model::Take& tk) { drop(tk.chain); });
            std::set<Id> gone{*id};
            for (const auto& e : s.routes)  // key routes into it (ADR-0018)
                if (e.value->to.fx && *e.value->to.fx == *id) {
                    gone.insert(e.id);
                    t.remove<model::Route>(e.id);
                }
            for (const auto& e : s.envelopes)
                if (gone.count(e.value->target.object)) t.remove<model::Envelope>(e.id);
            t.remove<model::FxInstance>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxSetArgs, Empty> c;
        c.name = "fx.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set instance options: bypass, offline, wet (%), label, isolation, pins.";
        c.description =
            "isolation: in_process, dedicated (the plugin runs in a process of its own: a "
            "crash silences only it), shared_bridge (one process for all such plugins), "
            "inherit (the project default: in_process). pins: which of the track's channels "
            "(1-based; track.set channels adds 3-128) the plugin reads ({in: [l, r]}), writes "
            "({out: [l, r]}) and keys from ({key: [3, 4]}: a detector on 3/4, for a plugin "
            "with a sidechain input); {} restores 1/2 in and out. out past its first two "
            "entries: a track channel for each of the plugin's auxiliary output channels in "
            "order (0: unconnected); fx.add sets them for a multi-out instrument.";
        c.tags = {"fx"};
        c.args = schema::object<FxSetArgs>("fx_set_args")
                     .field("fx", &FxSetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("bypass", &FxSetArgs::bypass, schema::boolean())
                     .field("offline", &FxSetArgs::offline, schema::boolean())
                     .field("wet", &FxSetArgs::wet,
                            schema::number().range(0, 100).unit(units::Unit::percent))
                     .field("label", &FxSetArgs::label, schema::string())
                     .field("isolation", &FxSetArgs::isolation,
                            schema::enumeration<model::Isolation>(
                                {{model::Isolation::Inherit, "inherit"},
                                 {model::Isolation::InProcess, "in_process"},
                                 {model::Isolation::SharedBridge, "shared_bridge"},
                                 {model::Isolation::Dedicated, "dedicated"}}))
                     .field("pins", &FxSetArgs::pins, model::pinMapSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Half wet", obj(R"({"fx": "fx_1", "wet": "50%"})"), obj("{}"), "song_with_fx"},
            {"Key the plugin from channels 3/4", obj(R"({"fx": "fx_1", "pins": {"key": [3, 4]}})"),
             obj("{}"), "song_with_keymix_4ch"}};
        c.run = [](const FxSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::FxInstance>(app.view(), a.fx, "/fx");
            if (!id) return std::move(id).error();
            if (a.pins && !a.pins->empty()) {
                const model::Session s = app.view();
                const auto track = model::trackOfFx(s, *id);
                if (!track)
                    return Error("fx.pins", "pins are for a track's FX chain", "/pins",
                                 "take FX and input FX are stereo");
                const int channels = s.tracks.find(*track)->channels;
                for (auto [list, name] :
                     {std::pair{&a.pins->in, "in"}, {&a.pins->out, "out"}, {&a.pins->key, "key"}}) {
                    // out may go on with the auxiliary outputs' channels (ADR-0021).
                    const bool out = list == &a.pins->out;
                    if (out ? list->size() > static_cast<std::size_t>(model::kMaxTrackChannels)
                            : list->size() > 2)
                        return Error("fx.pins", std::string(name) + " names one or two channels",
                                     std::string("/pins/") + name, "e.g. [3, 4]");
                    for (std::size_t k = 0; k < list->size(); ++k) {
                        if ((*list)[k] == 0 && !(out && k >= 2))
                            return Error("fx.pins", "channels count from 1",
                                         std::string("/pins/") + name + "/" + std::to_string(k),
                                         "0 leaves only an auxiliary output unconnected");
                        if ((*list)[k] > std::max(channels, 2))
                            return Error("fx.pins",
                                         "the track has " + std::to_string(std::max(channels, 2)) +
                                             " channels",
                                         std::string("/pins/") + name + "/" + std::to_string(k),
                                         "track.set channels adds channels 3-128");
                    }
                }
            }
            app.txn().modify<model::FxInstance>(*id, [&](model::FxInstance& f) {
                if (a.bypass) f.bypass = *a.bypass;
                if (a.offline) f.offline = *a.offline;
                if (a.wet) f.wet = *a.wet;
                if (a.label) f.label = *a.label;
                if (a.isolation) f.isolation = *a.isolation;
                if (a.pins) f.pins = *a.pins;
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxRef, Empty> c;
        c.name = "fx.restart";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Re-create a plugin instance from its saved patch/state (after a crash).";
        c.description =
            "engine.status lists failed_fx: isolated plugins whose process crashed or hung. "
            "The new instance starts from the model's patch or state.";
        c.tags = {"fx"};
        c.args = schema::object<FxRef>("fx_ref")
                     .field("fx", &FxRef::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Restart", obj(R"({"fx": "fx_1"})"), obj("{}"), "song_with_fx"}};
        c.run = [](const FxRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::FxInstance>(app.view(), a.fx, "/fx");
            if (!id) return std::move(id).error();
            app.restartFx(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.patch.merge /
    // fx.param.set
    auto mergeRun = [](EngineApp& app, const std::string& fxText, const json::Value& patch,
                       const char* patchPath) -> Result<MergeResult> {
        const model::Session s = app.view();
        auto id = existing<model::FxInstance>(s, fxText, "/fx");
        if (!id) return std::move(id).error();
        const model::FxInstance& fx = *s.fx.find(*id);
        auto merged = app.mergedPatch(fx, patch, false);
        if (!merged) {
            Error e = std::move(merged).error();
            auto fix = [&](Error& x) {
                if (std::string_view(patchPath) == "/values" && x.path.rfind("/params", 0) == 0)
                    x.path = "/values" + x.path.substr(7);
                else if (!x.path.empty() || x.details.empty())
                    x.path = std::string(patchPath) + x.path;
            };
            fix(e);
            for (auto& d : e.details) fix(d);
            return e;
        }
        MergeResult r;
        auto before = fx.patch ? *fx.patch : json::Value::object();
        auto changed = json::createMergePatch(before, *merged);
        r.changed = changed ? std::move(*changed) : json::Value::object();
        model::FxInstance next = fx;
        next.patch = std::move(*merged);
        auto d = app.describeFx(next, true);
        r.description = d ? *d : "";
        // The scratch instance holds the merged patch now: its schema says
        // which of the values set do not apply with it.
        if (const json::Value* set = patch.find("params"); set && set->isObject())
            if (auto schema = app.fxSchema(next); schema && schema->find("params")) {
                std::vector<std::string> inactive;
                for (const auto& m : set->asObject()) {
                    const json::Value* e = schema->find("params")->find(m.key);
                    const json::Value* active = e ? e->find("active") : nullptr;
                    if (active && active->isBool() && !active->asBool()) inactive.push_back(m.key);
                }
                if (!inactive.empty()) r.inactive = std::move(inactive);
            }
        app.txn().put<model::FxInstance>(*id, std::move(next));
        return r;
    };
    auto mergeResultSchema = [] {
        return schema::object<MergeResult>("merge_result")
            .field("changed", &MergeResult::changed, schema::any())
            .field("description", &MergeResult::description, schema::string())
            .field("inactive", &MergeResult::inactive,
                   schema::arrayOf(schema::string())
                       .doc("Params set that do not apply with the new values: kept, and heard "
                            "once what fx.describe full's active_when names allows it"))
            .build();
    };
    [&] {
        Command<FxPatchArgs, MergeResult> c;
        c.name = "fx.patch.merge";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Merge a partial patch into a plugin instance; returns the effective change.";
        c.description =
            "Semantic plugins only. Values in physical units: {\"params\": {\"gain\": \"-6 dB\"}}.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchArgs>("fx_patch_args")
                     .field("fx", &FxPatchArgs::fx, idArg("fx", "Plugin instance"))
                     .field("patch", &FxPatchArgs::patch, schema::any())
                     .build();
        c.result = mergeResultSchema();
        c.examples = {{"Turn the gain down",
                       obj(R"({"fx": "fx_2", "patch": {"params": {"gain": "-6 dB"}}})"),
                       obj(R"({"changed": "<any>", "description": "<any>"})"), "song_with_fx"}};
        c.run = [mergeRun](const FxPatchArgs& a, Context& ctx) {
            return mergeRun(A(ctx), a.fx, a.patch, "/patch");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxParamSetArgs, MergeResult> c;
        c.name = "fx.param.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set semantic parameters in units, or plain plugin parameters by name/id.";
        c.description =
            "Plain CLAP/VST3 values use the declared range or plugin text; read-only parameters "
            "refuse edits.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxParamSetArgs>("fx_param_set_args")
                     .field("fx", &FxParamSetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("values", &FxParamSetArgs::values, schema::any())
                     .build();
        c.result = mergeResultSchema();
        c.examples = {{"Set the gain", obj(R"({"fx": "fx_2", "values": {"gain": -3}})"),
                       obj(R"({"changed": "<any>", "description": "<any>"})"), "song_with_fx"}};
        c.run = [mergeRun](const FxParamSetArgs& a, Context& ctx) -> Result<MergeResult> {
            auto id = existing<model::FxInstance>(A(ctx).view(), a.fx, "/fx");
            if (!id) return id.error();
            auto schema = A(ctx).fxSchema(*A(ctx).view().fx.find(*id));
            if (!schema) return schema.error();
            // The window's edits are the "last touched parameter" (ADR-0027).
            const bool touches = ctx.origin().type == cmd::Origin::Type::Ui;
            if (schema->isNull()) {
                auto changed = A(ctx).setPlainParams(*id, a.values);
                if (!changed) return changed.error();
                if (touches && changed->isObject())
                    if (auto list = A(ctx).fxParams(*A(ctx).view().fx.find(*id)))
                        for (const auto& m : changed->asObject())
                            for (const host::ParamInfo& p : *list)
                                if (std::to_string(p.id) == m.key ||
                                    std::ranges::equal(p.name, m.key, [](char x, char y) {
                                        return std::tolower(static_cast<unsigned char>(x)) ==
                                               std::tolower(static_cast<unsigned char>(y));
                                    })) {
                                    A(ctx).touchParam(*id, std::to_string(p.id));
                                    break;
                                }
                return MergeResult{std::move(*changed), "Plugin parameters updated", std::nullopt};
            }
            json::Value patch = json::Value::object();
            patch["params"] = a.values;
            auto merged = mergeRun(A(ctx), a.fx, patch, "/values");
            if (!merged || !a.values.isObject()) return merged;
            if (touches)
                for (const auto& m : a.values.asObject()) A(ctx).touchParam(*id, m.key);
            // Touch/latch/write envelopes of these parameters record the change
            // while the transport plays (values in the parameter's unit).
            const auto fx = model::Id::parse(a.fx);
            if (!fx) return merged;
            for (const auto& m : a.values.asObject()) {
                if (!A(ctx).writesAutomation(*fx, m.key)) continue;
                const model::EnvTarget target{*fx, m.key};
                auto domain = envDomain(A(ctx), A(ctx).view(), target, "/values");
                if (!domain) continue;
                if (auto v = envValue(*domain, m.value, "/values/" + m.key); v)
                    A(ctx).recordAutomation(*fx, m.key, *v);
            }
            return merged;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- fx.describe
    [&] {
        Command<FxDescribeArgs, FxDescription> c;
        c.name = "fx.describe";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Describe an instance and its live problems; full adds parameters and patch.";
        c.tags = {"fx"};
        c.args = schema::object<FxDescribeArgs>("fx_describe_args")
                     .field("fx", &FxDescribeArgs::fx, idArg("fx", "Plugin instance"))
                     .field("full", &FxDescribeArgs::full, schema::boolean())
                     .defaultValue(false)
                     .field("filter", &FxDescribeArgs::filter,
                            schema::string().doc(
                                "Plain parameters only: list those whose name contains this "
                                "text (case-insensitive)."))
                     .build();
        c.result =
            schema::object<FxDescription>("fx_description")
                .field("plugin", &FxDescription::plugin, schema::string())
                .field("name", &FxDescription::name, schema::string())
                .field("description", &FxDescription::description, schema::string())
                .field("params", &FxDescription::params, schema::any())
                .field("patch", &FxDescription::patch, schema::any())
                .field("status_available", &FxDescription::statusAvailable,
                       schema::boolean().doc("A live instance supplied its current problems"))
                .field("problems", &FxDescription::problems,
                       schema::any().doc("Current plugin problems: code, message, scope, line when "
                                         "known, hint, silent; scope host when unreadable"))
                .field("sidechain_channels", &FxDescription::sidechainChannels,
                       schema::integer().doc("The plug-in's key input: route.add {fx} feeds it"))
                .field("latency_samples", &FxDescription::latencySamples,
                       schema::integer().doc("The instance's reported latency, in samples"))
                .field("params_total", &FxDescription::paramsTotal,
                       schema::integer().doc("Plain parameters matching; at most " +
                                             std::to_string(kMaxListedParams) + " are listed."))
                .build();
        c.examples = {
            {"Describe", obj(R"({"fx": "fx_2"})"),
             obj(R"({"plugin": "com.suppleaudio.test.gain", "name": "<any>", "description": "<any>", "latency_samples": "<any>", "status_available": false})"),
             "song_with_fx"}};
        c.run = [](const FxDescribeArgs& a, Context& ctx) -> Result<FxDescription> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const model::FxInstance& fx = *s.fx.find(*id);
            FxDescription r;
            r.plugin = fx.plugin.id;
            r.name = fx.plugin.name;
            if (auto owner = model::frozenOwner(s, *id)) {
                r.description = "Frozen on " + owner->str() +
                                "; stored state retained. Use track.unfreeze to edit.";
                if (fx.patch) r.patch = *fx.patch;
                r.params = model::toJson(fx);
                return r;
            }
            // A scratch instance cannot know what went wrong in the one playing.
            host::Instance* live = app.instanceOf(*id);
            if (live) {
                // A failed query (a crashed bridge) is reported, not fatal: the
                // description is what an agent needs to decide what to do next.
                auto status = live->pluginStatus();
                if (!status)
                    r.problems = host::statusFailure(status.error());
                else if (!status->isNull()) {
                    r.statusAvailable = true;
                    r.problems = *status->find("problems");
                }
            }
            auto d = app.describeFx(fx, !a.full);
            // A bad saved revision may prevent creating a scratch instance.
            // Still inspect the older version that is actually loaded.
            if (!d && live)
                d = live->info().semantic ? live->describe(!a.full)
                                          : Result<std::string>(live->info().name);
            if (!d) return std::move(d).error();
            r.description = *d;
            if (auto key = app.fxSidechainChannels(fx); key && *key > 0) r.sidechainChannels = *key;
            if (auto latency = app.fxLatencySamples(*id, fx)) r.latencySamples = *latency;
            if (a.full) {
                auto schema = app.fxSchema(fx);
                if (!schema && live) schema = live->semanticSchema();
                if (schema && schema->isObject() && schema->find("params"))
                    r.params = *schema->find("params");
                else if (auto list = app.fxParams(fx)) {
                    // Not semantic (a plain CLAP or a VST3): its parameters as the
                    // plugin declares them, for fx.param.set by id or name.
                    auto lower = [](std::string t) {
                        for (char& ch : t)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        return t;
                    };
                    const std::string filter = lower(a.filter.value_or(""));
                    json::Value ps = json::Value::array();
                    std::int64_t total = 0;
                    for (const host::ParamInfo& p : *list) {
                        if (p.flags & CLAP_PARAM_IS_HIDDEN) continue;
                        if (!filter.empty() && lower(p.name).find(filter) == std::string::npos)
                            continue;
                        if (static_cast<std::size_t>(total++) >= kMaxListedParams) continue;
                        json::Value e = json::Value::object();
                        e["id"] = static_cast<double>(p.id);
                        e["name"] = p.name;
                        e["min"] = p.min;
                        e["max"] = p.max;
                        e["default"] = p.def;
                        if (auto saved = fx.params.find(p.id); saved != fx.params.end())
                            e["value"] = saved->second;
                        if (p.flags & (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM))
                            e["stepped"] = true;
                        if (p.flags & CLAP_PARAM_IS_READONLY) e["read_only"] = true;
                        ps.push_back(std::move(e));
                    }
                    r.params = std::move(ps);
                    r.paramsTotal = total;
                }
                if (fx.patch) r.patch = *fx.patch;
            }
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    registerRenderCommands(registry);
    registerEngineAnalysisCommands(registry);
    // ---------------------------------------------------------------- history
    auto historyResult = [] {
        return schema::object<HistoryResult>("history_result")
            .field("version", &HistoryResult::version, schema::integer())
            .field("label", &HistoryResult::label, schema::string())
            .build();
    };
    for (bool undo : {true, false}) {
        Command<NoArgs, HistoryResult> c;
        c.name = undo ? "history.undo" : "history.redo";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            undo ? "Undo the last change on the current branch." : "Redo the last undone change.";
        c.tags = {"history"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = historyResult();
        c.examples = {{undo ? "Undo" : "Redo", obj("{}"),
                       obj(R"({"version": "<any>", "label": "<any>"})"),
                       undo ? "song" : "song_undone"}};
        c.run = [undo](const NoArgs&, Context& ctx) -> Result<HistoryResult> {
            EngineApp& app = A(ctx);
            SUPPLE_TRY(undo ? app.undo() : app.redo());
            const model::Version* v =
                app.versions().version(app.versions().headVersion(app.versions().branch()));
            return HistoryResult{static_cast<std::int64_t>(v->id), v->txn.label};
        };
        addChecked(registry, std::move(c));
    }
    [&] {
        Command<NoArgs, HistoryList> c;
        c.name = "history.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the current branch's recent versions (newest first).";
        c.tags = {"history"};
        c.args = schema::object<NoArgs>("no_args").build();
        auto entry = schema::object<HistoryEntry>("history_entry")
                         .field("version", &HistoryEntry::version, schema::integer())
                         .field("label", &HistoryEntry::label, schema::string())
                         .field("origin", &HistoryEntry::origin, schema::string())
                         .field("squashed_steps", &HistoryEntry::squashedSteps,
                                schema::integerOf<std::uint64_t>())
                         .build();
        c.result = schema::object<HistoryList>("history_list")
                       .field("versions", &HistoryList::versions, schema::arrayOf(entry))
                       .field("can_undo", &HistoryList::canUndo, schema::boolean())
                       .field("can_redo", &HistoryList::canRedo, schema::boolean())
                       .field("truncated", &HistoryList::truncated, schema::boolean())
                       .field("retained_bytes", &HistoryList::retainedBytes,
                              schema::integerOf<std::uint64_t>())
                       .field("budget_bytes", &HistoryList::budgetBytes,
                              schema::integerOf<std::uint64_t>())
                       .build();
        c.examples = {
            {"List", obj("{}"),
             obj(R"({"versions": "<any>", "can_undo": true, "can_redo": false, "truncated": false, "retained_bytes": "<any>", "budget_bytes": 1073741824})"),
             "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<HistoryList> {
            EngineApp& app = A(ctx);
            HistoryList out;
            for (const model::Version* v : app.versions().history(20))
                out.versions.push_back({static_cast<std::int64_t>(v->id), v->txn.label,
                                        v->txn.origin, v->squashedSteps});
            out.canUndo = app.versions().canUndo();
            out.canRedo = app.versions().canRedo();
            out.truncated = app.versions().historyTruncated();
            out.retainedBytes = app.versions().retainedBytes();
            out.budgetBytes = app.head().project->historyBudgetBytes;
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- branches
    struct BranchCreateArgs {
        std::string name;
        std::optional<std::string> from;
        std::optional<std::int64_t> version;
        bool switchTo = true;
    };
    struct BranchName {
        std::string name;
    };
    struct BranchList {
        std::vector<std::string> branches;
        std::string current;
    };
    struct BranchDiffArgs {
        std::string a, b;
    };
    struct DiffEntry {
        std::string op, id;
        std::vector<std::string> fields;
    };
    struct BranchDiff {
        std::vector<DiffEntry> changes;
    };
    struct BranchMergeArgs {
        std::string from;
        std::optional<std::string> into;
    };
    struct BranchMergeResult {
        bool fastForward = false, upToDate = false;
        std::int64_t version = 0;
        json::Value remapped;
    };
    auto branchName = [] {
        return schema::string().pattern("^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$",
                                        "a branch name like agent/claude/darker");
    };
    [&] {
        Command<BranchCreateArgs, BranchName> c;
        c.name = "branch.create";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start a branch from a branch's head or an earlier version, and switch to it.";
        c.description =
            "Agents: work on agent/<you>/<topic> branches, then branch.diff and branch.merge into "
            "main. "
            "Merging replays your edits onto main even if main moved; conflicting edits are "
            "reported.";
        c.tags = {"branch"};
        c.args = schema::object<BranchCreateArgs>("branch_create_args")
                     .field("name", &BranchCreateArgs::name, branchName())
                     .field("from", &BranchCreateArgs::from, branchName())
                     .field("version", &BranchCreateArgs::version,
                            schema::integer().min(1).doc(
                                "Start at this version of the current branch's history "
                                "(history.list; a render result's version) instead of a head"))
                     .field("switch", &BranchCreateArgs::switchTo, schema::boolean())
                     .defaultValue(true)
                     .build();
        c.result = schema::object<BranchName>("branch_name")
                       .field("name", &BranchName::name, schema::string())
                       .build();
        c.errors = {{"branch.exists", "a branch with that name exists",
                     "pick another name or switch to it"}};
        c.examples = {{"An agent branch", obj(R"({"name": "agent/claude/darker"})"),
                       obj(R"({"name": "agent/claude/darker"})"), "song"}};
        c.run = [](const BranchCreateArgs& a, Context& ctx) -> Result<BranchName> {
            EngineApp& app = A(ctx);
            if (a.version) {
                if (a.from)
                    return Error("branch.args", "give from or version, not both", "/version",
                                 "version starts the branch at a version of the current branch");
                SUPPLE_TRY(
                    app.versions().createBranchAt(a.name, static_cast<std::uint64_t>(*a.version)));
            } else
                SUPPLE_TRY(
                    app.versions().createBranch(a.name, a.from.value_or(app.versions().branch())));
            if (a.switchTo) SUPPLE_TRY(app.switchBranch(a.name));
            return BranchName{a.name};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchName, BranchName> c;
        c.name = "branch.switch";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Make another branch current (edits, undo and playback follow it).";
        c.tags = {"branch"};
        c.args = schema::object<BranchName>("branch_name")
                     .field("name", &BranchName::name, branchName())
                     .build();
        c.result = schema::object<BranchName>("branch_name")
                       .field("name", &BranchName::name, schema::string())
                       .build();
        c.examples = {
            {"Back to main", obj(R"({"name": "main"})"), obj(R"({"name": "main"})"), "song"}};
        c.run = [](const BranchName& a, Context& ctx) -> Result<BranchName> {
            SUPPLE_TRY(A(ctx).switchBranch(a.name));
            return BranchName{a.name};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, BranchList> c;
        c.name = "branch.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List branches and the current one.";
        c.tags = {"branch"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<BranchList>("branch_list")
                       .field("branches", &BranchList::branches, schema::arrayOf(schema::string()))
                       .field("current", &BranchList::current, schema::string())
                       .build();
        c.examples = {
            {"List", obj("{}"), obj(R"({"branches": ["main"], "current": "main"})"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<BranchList> {
            EngineApp& app = A(ctx);
            return BranchList{app.versions().branches(), app.versions().branch()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchDiffArgs, BranchDiff> c;
        c.name = "branch.diff";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "What differs between two branch heads: objects added, removed, changed (fields).";
        c.tags = {"branch"};
        c.args = schema::object<BranchDiffArgs>("branch_diff_args")
                     .field("a", &BranchDiffArgs::a, branchName())
                     .field("b", &BranchDiffArgs::b, branchName())
                     .build();
        auto entry = schema::object<DiffEntry>("diff_entry")
                         .field("op", &DiffEntry::op, schema::string())
                         .field("id", &DiffEntry::id, schema::string())
                         .field("fields", &DiffEntry::fields, schema::arrayOf(schema::string()))
                         .build();
        c.result = schema::object<BranchDiff>("branch_diff")
                       .field("changes", &BranchDiff::changes, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Main against itself", obj(R"({"a": "main", "b": "main"})"),
                       obj(R"({"changes": []})"), "song"}};
        c.run = [](const BranchDiffArgs& a, Context& ctx) -> Result<BranchDiff> {
            EngineApp& app = A(ctx);
            auto ha = app.versions().head(a.a), hb = app.versions().head(a.b);
            if (!ha) return Error("branch.unknown", "no branch " + a.a, "/a", "see branch.list");
            if (!hb) return Error("branch.unknown", "no branch " + a.b, "/b", "see branch.list");
            BranchDiff out;
            static constexpr const char* kOps[] = {"add", "remove", "update"};
            for (auto& d : model::diff(*ha, *hb))
                out.changes.push_back(
                    {kOps[static_cast<int>(d.op)], d.id.str(), std::move(d.fields)});
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchMergeArgs, BranchMergeResult> c;
        c.name = "branch.merge";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Merge a branch into another (default: main) by replaying its edits.";
        c.description =
            "Fast-forwards when the target has not moved. Otherwise replays the branch's edits; "
            "IDs it "
            "created are renumbered where needed (see remapped). If both sides changed the same "
            "field "
            "of an object, nothing is merged and the error lists each conflict.";
        c.tags = {"branch"};
        c.args = schema::object<BranchMergeArgs>("branch_merge_args")
                     .field("from", &BranchMergeArgs::from, branchName())
                     .field("into", &BranchMergeArgs::into, branchName())
                     .build();
        c.result = schema::object<BranchMergeResult>("branch_merge")
                       .field("fast_forward", &BranchMergeResult::fastForward, schema::boolean())
                       .field("up_to_date", &BranchMergeResult::upToDate, schema::boolean())
                       .field("version", &BranchMergeResult::version, schema::integer())
                       .field("remapped", &BranchMergeResult::remapped, schema::any())
                       .build();
        c.errors = {{"branch.conflict", "both sides changed the same thing",
                     "details: one per conflict with the object"}};
        c.examples = {
            {"Merge an agent branch", obj(R"({"from": "agent/claude/darker"})"),
             obj(R"({"fast_forward": true, "up_to_date": false, "version": "<any>", "remapped": {}})"),
             "branch_ahead"}};
        c.run = [](const BranchMergeArgs& a, Context& ctx) -> Result<BranchMergeResult> {
            EngineApp& app = A(ctx);
            const std::string into = a.into.value_or("main");
            auto out = app.mergeBranch(ctx.service<const cmd::Registry>(), a.from, into);
            if (!out.conflicts.empty()) {
                Error e("branch.conflict", std::to_string(out.conflicts.size()) +
                                               " conflict(s) merging " + a.from + " into " + into +
                                               "; nothing was merged");
                e.hint = "change one side (or undo the edit on the branch) and merge again";
                for (const auto& cf : out.conflicts)
                    e.details.emplace_back(cf.kind == model::Conflict::Kind::ConcurrentEdit
                                               ? "merge.concurrent_edit"
                                               : "merge.invalid",
                                           cf.message, "/steps/" + std::to_string(cf.step),
                                           cf.object ? "object " + cf.object->str() : "");
                return e;
            }
            BranchMergeResult r;
            r.fastForward = out.fastForward;
            r.upToDate = out.upToDate;
            r.version = static_cast<std::int64_t>(out.head);
            r.remapped = json::Value::object();
            for (const auto& [from, to] : out.remap) r.remapped[from.str()] = to.str();
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BranchName, Empty> c;
        c.name = "branch.delete";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a branch (not main, not the current one).";
        c.tags = {"branch"};
        c.args = schema::object<BranchName>("branch_name")
                     .field("name", &BranchName::name, branchName())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Delete", obj(R"({"name": "agent/claude/darker"})"), obj("{}"), "branch_ahead"}};
        c.run = [](const BranchName& a, Context& ctx) -> Result<Empty> {
            SUPPLE_TRY(A(ctx).versions().deleteBranch(a.name));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- env.*
    [&] {
        Command<EnvAddArgs, EnvAddResult> c;
        c.name = "env.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Automate a parameter: a track's or route's mixer control, or a plugin's.";
        c.description =
            "Target one of track (param volume|pan|mute; width|pan_left|pan_right|pan_boost on "
            "channels/busses; playrate only on tr_0), route "
            "(volume|pan|mute), group (VCA volume|mute), or "
            "fx (a semantic key such as \"gain\" or \"filter1.cutoff\", or for other plugins a "
            "CLAP id or parameter name), or module (a rack module's knob by key, values in its "
            "unit, a choice by its key; module.describe). points: [{beat|bar|seconds, value, "
            "shape?, tension?}]; "
            "values in the parameter's unit (dB, %, Hz… — text like \"800 Hz\" works for "
            "plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values "
            "move along the parameter's curve (a log cutoff sweeps evenly in pitch). take (param "
            "volume|pan|mute): a take envelope, its points in beats from the item's start (it "
            "moves with the item); it shapes the take after its take FX (a MIDI take needs an "
            "instrument in its take FX). Master playrate points are finite numeric ratios "
            "0.25..4.0; an active nonempty master curve replaces the saved scalar during "
            "playback and rendering. Audio sources convert before take FX/live mixing; MIDI "
            "and automation follow the same timeline clock. Width and dual pans use -100..100 "
            "percent; pan_boost uses 0..1 (on at 0.5), overriding the effective law's "
            "boost while the envelope is active and nonempty.";
        c.tags = {"automation"};
        c.args = schema::object<EnvAddArgs>("env_add_args")
                     .field("track", &EnvAddArgs::track, idArg("tr", "Track"))
                     .field("module", &EnvAddArgs::module, idArg("mo", "Rack module"))
                     .field("take", &EnvAddArgs::take, idArg("tk", "Take"))
                     .field("group", &EnvAddArgs::group, idArg("gr", "VCA group"))
                     .field("fx", &EnvAddArgs::fx, idArg("fx", "Plugin instance"))
                     .field("route", &EnvAddArgs::route, idArg("sd", "Route"))
                     .field("param", &EnvAddArgs::param, schema::string())
                     .field("points", &EnvAddArgs::points, schema::arrayOf(envPointSchema()))
                     .build();
        c.result = schema::object<EnvAddResult>("env_add")
                       .field("env", &EnvAddResult::env, schema::string())
                       .field("points", &EnvAddResult::points, schema::integer())
                       .build();
        c.examples = {
            {"Fade a track out over bar 2",
             obj(R"({"track": "tr_1", "param": "volume", "points": [{"bar": 2, "value": 0}, {"bar": 3, "value": "-60 dB"}]})"),
             obj(R"({"env": "en_1", "points": 2})"), "song"},
            {"Sweep a plugin parameter",
             obj(R"({"fx": "fx_2", "param": "gain", "points": [{"beat": 0, "value": "-12 dB", "shape": "slow"}, {"beat": 4, "value": 0}]})"),
             obj(R"({"env": "en_1", "points": 2})"), "song_with_fx"},
            {"Duck a take halfway through its item",
             obj(R"({"take": "tk_1", "param": "volume", "points": [{"beat": 0, "value": 0}, {"beat": 2, "value": "-12 dB"}]})"),
             obj(R"({"env": "en_1", "points": 2})"), "song_with_audio"},
            {"A rack module's knob: a VCA opening over a bar",
             obj(R"({"module": "mo_3", "param": "level", "points": [{"beat": 0, "value": -40}, {"beat": 4, "value": 0}]})"),
             obj(R"({"env": "en_1", "points": 2})"), "song_with_rack"},
            {"Master rate envelope",
             obj(R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1},{"beat":4,"value":2}]})"),
             obj(R"({"env":"en_1","points":2})"), "empty"}};
        c.run = [](const EnvAddArgs& a, Context& ctx) -> Result<EnvAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            if (a.track.has_value() + a.fx.has_value() + a.route.has_value() + a.take.has_value() +
                    a.group.has_value() + a.module.has_value() !=
                1)
                return Error("env.target", "give one of track, fx, route, take, group or module",
                             "", "e.g. {\"fx\": \"fx_1\", \"param\": \"filter1.cutoff\"}");
            model::Envelope env;
            if (a.take) {
                auto id = existing<model::Take>(s, *a.take, "/take");
                if (!id) return std::move(id).error();
                const model::Take& take = *s.takes.find(*id);
                if (take.source && take.source->kind == model::Kind::Midi) {
                    bool instrument = false;
                    for (Id f : take.chain)
                        if (auto info = app.findPlugin(s.fx.find(f)->plugin.id))
                            instrument = instrument || info->instrument;
                    if (!instrument)
                        return Error("env.take_midi",
                                     "a MIDI take's envelope needs an instrument in its take FX",
                                     "/take",
                                     "fx.add {take, plugin: an instrument} first, or automate "
                                     "the track");
                }
                env.target.object = *id;
            } else if (a.group) {
                auto id = existing<model::TrackGroup>(s, *a.group, "/group");
                if (!id) return id.error();
                if (!s.groups.find(*id)->vca || (a.param != "volume" && a.param != "mute"))
                    return Error("env.target", "VCA target must be volume or mute", "/group",
                                 "add a VCA with vca.add and choose volume or mute");
                env.target.object = *id;
            } else if (a.track) {
                auto id = existing<model::Track>(s, *a.track, "/track");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else if (a.fx) {
                auto id = existing<model::FxInstance>(s, *a.fx, "/fx");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else if (a.module) {
                auto id = existing<model::RackModule>(s, *a.module, "/module");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else {
                auto id = existing<model::Route>(s, *a.route, "/route");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            }
            env.target.param = a.param;
            for (const auto& e : s.envelopes)
                if (e.value->target.object == env.target.object && e.value->target.param == a.param)
                    return Error("env.exists", "an envelope already automates this parameter",
                                 "/param", "env.points.set " + e.id.str());
            auto domain = envDomain(app, s, env.target, "/param");
            if (!domain) return std::move(domain).error();
            if (a.points) {
                auto pts = envPoints(s, *domain, *a.points, "/points", a.take.has_value());
                if (!pts) return std::move(pts).error();
                env.points = std::move(*pts);
            }
            const Id id = app.txn().create(env);
            return EnvAddResult{id.str(), static_cast<std::int64_t>(env.points.size())};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvPointsSetArgs, EnvCount> c;
        c.name = "env.points.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace an envelope's points (all of them, or those in a beat range).";
        c.description = "from_beat/to_beat: replace only points with from_beat <= beat < to_beat.";
        c.tags = {"automation"};
        c.args = schema::object<EnvPointsSetArgs>("env_points_set_args")
                     .field("env", &EnvPointsSetArgs::env, idArg("en", "Envelope"))
                     .field("points", &EnvPointsSetArgs::points, schema::arrayOf(envPointSchema()))
                     .field("from_beat", &EnvPointsSetArgs::fromBeat, schema::number().min(0))
                     .field("to_beat", &EnvPointsSetArgs::toBeat, schema::number().min(0))
                     .build();
        c.result = schema::object<EnvCount>("env_count")
                       .field("points", &EnvCount::points, schema::integer())
                       .build();
        c.examples = {
            {"Three points",
             obj(R"({"env": "en_1", "points": [{"beat": 0, "value": -6}, {"beat": 2, "value": 0, "shape": "square"}, {"beat": 3, "value": "-3 dB"}]})"),
             obj(R"({"points": 3})"), "song_with_env"}};
        c.run = [](const EnvPointsSetArgs& a, Context& ctx) -> Result<EnvCount> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Envelope>(s, a.env, "/env");
            if (!id) return std::move(id).error();
            const model::Envelope& env = *s.envelopes.find(*id);
            auto domain = envDomain(app, s, env.target, "/env");
            if (!domain) return std::move(domain).error();
            auto pts = envPoints(s, *domain, a.points, "/points",
                                 env.target.object.kind == model::Kind::Take, env.timebase);
            if (!pts) return std::move(pts).error();
            std::vector<model::EnvPoint> next;
            if (a.fromBeat || a.toBeat) {
                const auto position = [&](double beat) {
                    return env.timebase == model::Timebase::Time ? s.timeMap->secondsAt(beat)
                                                                 : beat;
                };
                const double lo = a.fromBeat ? position(*a.fromBeat) : 0.0;
                const double hi =
                    a.toBeat ? position(*a.toBeat) : std::numeric_limits<double>::infinity();
                for (const auto& pt : env.points)
                    if (pt.pos < lo || pt.pos >= hi) next.push_back(pt);
            }
            next.insert(next.end(), pts->begin(), pts->end());
            std::stable_sort(next.begin(), next.end(),
                             [](const auto& x, const auto& y) { return x.pos < y.pos; });
            const auto count = static_cast<std::int64_t>(next.size());
            app.txn().modify<model::Envelope>(
                *id, [&](model::Envelope& e) { e.points = std::move(next); });
            return EnvCount{count};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvSetArgs, Empty> c;
        c.name = "env.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set an envelope's options: active (heard), mode (read, touch, latch, write).";
        c.description =
            "visible shows or hides the envelope's lane in the arrange view (a hidden envelope "
            "still plays while active).";
        c.tags = {"automation"};
        c.args = schema::object<EnvSetArgs>("env_set_args")
                     .field("env", &EnvSetArgs::env, idArg("en", "Envelope"))
                     .field("active", &EnvSetArgs::active, schema::boolean())
                     .field("visible", &EnvSetArgs::visible, schema::boolean())
                     .field("mode", &EnvSetArgs::mode, envModeSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Mute the automation", obj(R"({"env": "en_1", "active": false})"), obj("{}"),
                       "song_with_env"}};
        c.run = [](const EnvSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::Envelope>(app.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            app.txn().modify<model::Envelope>(*id, [&](model::Envelope& e) {
                if (a.active) e.active = *a.active;
                if (a.visible) e.visible = *a.visible;
                if (a.mode) e.mode = *a.mode;
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvRef, Empty> c;
        c.name = "env.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove an envelope (the parameter returns to its set value).";
        c.tags = {"automation"};
        c.args = schema::object<EnvRef>("env_ref")
                     .field("env", &EnvRef::env, idArg("en", "Envelope"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"env": "en_1"})"), obj("{}"), "song_with_env"}};
        c.run = [](const EnvRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::Envelope>(app.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            app.txn().remove<model::Envelope>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EnvListArgs, EnvList> c;
        c.name = "env.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List envelopes (optionally of one track, route or plugin).";
        c.tags = {"automation"};
        c.args = schema::object<EnvListArgs>("env_list_args")
                     .field("object", &EnvListArgs::object, schema::string())
                     .build();
        auto entry = schema::object<EnvEntry>("env_entry")
                         .field("env", &EnvEntry::env, schema::string())
                         .field("target", &EnvEntry::target, schema::string())
                         .field("param", &EnvEntry::param, schema::string())
                         .field("points", &EnvEntry::points, schema::integer())
                         .field("active", &EnvEntry::active, schema::boolean())
                         .field("visible", &EnvEntry::visible, schema::boolean())
                         .field("mode", &EnvEntry::mode, schema::string())
                         .build();
        c.result = schema::object<EnvList>("env_list")
                       .field("envelopes", &EnvList::envelopes, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"All", obj("{}"),
             obj(R"({"envelopes": [{"env": "en_1", "target": "fx_2", "param": "gain", "points": 2, "active": true, "visible": true, "mode": "read"}]})"),
             "song_with_env"}};
        c.run = [](const EnvListArgs& a, Context& ctx) -> Result<EnvList> {
            const model::Session s = A(ctx).view();
            static constexpr const char* kModes[] = {"trim",  "read",          "touch",
                                                     "latch", "latch_preview", "write"};
            EnvList out;
            for (const auto& e : s.envelopes) {
                const model::Envelope& env = *e.value;
                if (a.object && env.target.object.str() != *a.object) continue;
                out.envelopes.push_back({e.id.str(), env.target.object.str(), env.target.param,
                                         kModes[static_cast<int>(env.mode)],
                                         static_cast<std::int64_t>(env.points.size()), env.active,
                                         env.visible});
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- autoitem.* (gap log #28)
    [&] {
        struct ItemPointArg {
            double beat = 0;  // from the item's start
            json::Value value;
            std::optional<model::PointShape> shape;
            std::optional<double> tension;
        };
        struct AutoItemAddArgs {
            std::string env;
            std::optional<double> position;
            std::optional<std::int64_t> bar;
            double length = 4;
            std::optional<std::vector<ItemPointArg>> points;
            std::optional<double> sourceLength;
            std::optional<bool> loop;
            std::optional<std::string> name;
        };
        struct AutoItemSetArgs {
            std::string item;
            std::optional<double> position, length, sourceLength, baseline, amplitude;
            std::optional<bool> loop;
            std::optional<std::vector<ItemPointArg>> points;
            std::optional<std::string> name;
        };
        struct AutoItemRef {
            std::string item;
        };
        struct AutoItemDupArgs {
            std::string item;
            std::optional<double> position;
            bool pooled = true;
        };
        auto pointArgSchema = [] {
            return schema::object<ItemPointArg>("item_point")
                .field("beat", &ItemPointArg::beat, schema::number().min(0))
                .field("value", &ItemPointArg::value, schema::any())
                .field("shape", &ItemPointArg::shape,
                       schema::enumeration<model::PointShape>(
                           {{model::PointShape::Linear, "linear"},
                            {model::PointShape::Square, "square"},
                            {model::PointShape::Slow, "slow"},
                            {model::PointShape::FastStart, "fast_start"},
                            {model::PointShape::FastEnd, "fast_end"},
                            {model::PointShape::Bezier, "bezier"}}))
                .field("tension", &ItemPointArg::tension, schema::number().range(-1, 1))
                .build();
        };
        // An item's points in its envelope's units, sorted.
        auto itemPoints = [](EngineApp& app, const model::Session& s, const model::Envelope& env,
                             const std::vector<ItemPointArg>& in,
                             const std::string& path) -> Result<std::vector<model::EnvPoint>> {
            auto domain = envDomain(app, s, env.target, path.c_str());
            if (!domain) return std::move(domain).error();
            std::vector<model::EnvPoint> out;
            for (std::size_t i = 0; i < in.size(); ++i) {
                auto v = envValue(*domain, in[i].value, path + "/" + std::to_string(i) + "/value");
                if (!v) return std::move(v).error();
                out.push_back({in[i].beat, *v, in[i].shape.value_or(model::PointShape::Linear),
                               in[i].tension.value_or(0.0)});
            }
            std::stable_sort(out.begin(), out.end(),
                             [](const auto& a, const auto& b) { return a.pos < b.pos; });
            return out;
        };
        // An edit to a pooled item's points or source length reaches its pool.
        auto propagate = [](model::EditTxn& t, const model::Session& s, Id from,
                            const model::AutomationItem& edited) {
            if (edited.pool <= 0) return;
            for (const auto& e : s.autoItems)
                if (e.id != from && e.value->pool == edited.pool)
                    t.modify<model::AutomationItem>(e.id, [&](model::AutomationItem& x) {
                        x.points = edited.points;
                        x.sourceLength = edited.sourceLength;
                    });
        };
        auto entryOf = [](Id id, const model::AutomationItem& ai) {
            json::Value e = json::Value::object();
            e["id"] = id.str();
            e["env"] = ai.envelope.str();
            e["position"] = ai.position;
            e["length"] = ai.length;
            e["source_length"] = ai.sourceLength;
            e["loop"] = ai.loop;
            e["pool"] = static_cast<std::int64_t>(ai.pool);
            e["baseline"] = ai.baseline;
            e["amplitude"] = ai.amplitude;
            e["name"] = ai.name;
            json::Value pts = json::Value::array();
            for (const auto& p : ai.points) {
                json::Value pt = json::Value::object();
                pt["beat"] = p.pos;
                pt["value"] = p.value;
                pts.push_back(std::move(pt));
            }
            e["points"] = std::move(pts);
            return e;
        };
        {
            Command<AutoItemAddArgs, json::Value> c;
            c.name = "autoitem.add";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Put an automation item on an envelope: a block that replaces it there.";
            c.description =
                "position (beat) or bar, length in beats. points (beats from the item's start, "
                "values in the envelope's units) over source_length beats (default: length), "
                "repeated while loop (default true) and the item is longer; without points it "
                "captures what the envelope plays over that range. Duplicate it pooled "
                "(autoitem.duplicate) to edit several at once. Track, route and plugin "
                "envelopes.";
            c.tags = {"automation"};
            c.args =
                schema::object<AutoItemAddArgs>("autoitem_add_args")
                    .field("env", &AutoItemAddArgs::env, idArg("en", "Envelope"))
                    .field("position", &AutoItemAddArgs::position, schema::number().min(0))
                    .field("bar", &AutoItemAddArgs::bar,
                           schema::integer().range(1, std::numeric_limits<int>::max()))
                    .field("length", &AutoItemAddArgs::length, schema::number().exclusiveMin(0))
                    .defaultValue(4.0)
                    .field("points", &AutoItemAddArgs::points, schema::arrayOf(pointArgSchema()))
                    .field("source_length", &AutoItemAddArgs::sourceLength,
                           schema::number().exclusiveMin(0))
                    .field("loop", &AutoItemAddArgs::loop, schema::boolean())
                    .field("name", &AutoItemAddArgs::name, schema::string())
                    .build();
            c.result = schema::any();
            c.examples = {
                {"A one-bar swell, looped over two bars",
                 obj(R"({"env": "en_1", "bar": 1, "length": 8, "source_length": 4, "points": [{"beat": 0, "value": -12}, {"beat": 4, "value": 0}]})"),
                 obj(R"({"id": "ai_1", "env": "en_1", "position": 0.0, "length": 8.0,
                                   "source_length": 4.0, "loop": true, "pool": 0,
                                   "baseline": 0.0, "amplitude": 1.0, "name": "",
                                   "points": [{"beat": 0.0, "value": -12.0},
                                              {"beat": 4.0, "value": 0.0}]})"),
                 "song_with_env"}};
            c.run = [itemPoints, entryOf](const AutoItemAddArgs& a,
                                          Context& ctx) -> Result<json::Value> {
                EngineApp& app = A(ctx);
                const model::Session s = app.view();
                auto en = existing<model::Envelope>(s, a.env, "/env");
                if (!en) return std::move(en).error();
                const model::Envelope& env = *s.envelopes.find(*en);
                if (env.target.object.kind == model::Kind::Take)
                    return Error("autoitem.take",
                                 "automation items sit on track, route and "
                                 "plugin envelopes",
                                 "/env", "edit the take envelope's points");
                if (a.position && a.bar)
                    return Error("autoitem.position", "give position or bar", "/position",
                                 "provide either position in beats or bar, not both");
                model::AutomationItem ai;
                ai.envelope = *en;
                ai.position = a.bar ? s.timeMap->beatOfBar(static_cast<int>(*a.bar))
                                    : a.position.value_or(0.0);
                ai.length = a.length;
                ai.sourceLength = a.sourceLength.value_or(a.length);
                ai.loop = a.loop.value_or(true);
                ai.name = a.name.value_or("");
                if (a.points) {
                    auto pts = itemPoints(app, s, env, *a.points, "/points");
                    if (!pts) return std::move(pts).error();
                    ai.points = std::move(*pts);
                } else {  // capture what plays there now
                    const auto played = model::effectivePoints(s, *en);
                    const double end = ai.position + ai.sourceLength;
                    ai.points.push_back({0.0, model::pointsValueAt(played, ai.position, 0.0)});
                    for (const auto& p : played)
                        if (p.pos > ai.position && p.pos < end)
                            ai.points.push_back({p.pos - ai.position, p.value, p.shape, p.tension});
                    ai.points.push_back({ai.sourceLength, model::pointsValueAt(played, end, 0.0)});
                }
                const Id id = app.txn().create(ai);
                return entryOf(id, ai);
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<AutoItemSetArgs, json::Value> c;
            c.name = "autoitem.set";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Move, resize, loop, scale or rewrite an automation item.";
            c.description =
                "points and source_length reach every item of its pool; position, length, loop, "
                "baseline, amplitude (values play as baseline + amplitude x (value - baseline)) "
                "and name are the item's own.";
            c.tags = {"automation"};
            c.args =
                schema::object<AutoItemSetArgs>("autoitem_set_args")
                    .field("item", &AutoItemSetArgs::item, idArg("ai", "Automation item"))
                    .field("position", &AutoItemSetArgs::position, schema::number().min(0))
                    .field("length", &AutoItemSetArgs::length, schema::number().exclusiveMin(0))
                    .field("source_length", &AutoItemSetArgs::sourceLength,
                           schema::number().exclusiveMin(0))
                    .field("loop", &AutoItemSetArgs::loop, schema::boolean())
                    .field("baseline", &AutoItemSetArgs::baseline, schema::number())
                    .field("amplitude", &AutoItemSetArgs::amplitude, schema::number().range(-2, 2))
                    .field("points", &AutoItemSetArgs::points, schema::arrayOf(pointArgSchema()))
                    .field("name", &AutoItemSetArgs::name, schema::string())
                    .build();
            c.result = schema::any();
            c.examples = {{"Two beats later", obj(R"({"item": "ai_1", "position": 2})"),
                           obj(R"("<any>")"), "song_with_autoitem"}};
            c.run = [itemPoints, propagate, entryOf](const AutoItemSetArgs& a,
                                                     Context& ctx) -> Result<json::Value> {
                EngineApp& app = A(ctx);
                const model::Session s = app.view();
                auto id = existing<model::AutomationItem>(s, a.item, "/item");
                if (!id) return std::move(id).error();
                model::AutomationItem ai = *s.autoItems.find(*id);
                if (a.position) ai.position = *a.position;
                if (a.length) ai.length = *a.length;
                if (a.sourceLength) ai.sourceLength = *a.sourceLength;
                if (a.loop) ai.loop = *a.loop;
                if (a.baseline) ai.baseline = *a.baseline;
                if (a.amplitude) ai.amplitude = *a.amplitude;
                if (a.name) ai.name = *a.name;
                if (a.points) {
                    auto pts =
                        itemPoints(app, s, *s.envelopes.find(ai.envelope), *a.points, "/points");
                    if (!pts) return std::move(pts).error();
                    ai.points = std::move(*pts);
                }
                auto& t = app.txn();
                t.put<model::AutomationItem>(*id, ai);
                if (a.points || a.sourceLength) propagate(t, s, *id, ai);
                return entryOf(*id, ai);
            };
            addChecked(registry, std::move(c));
        }
        {
            Command<AutoItemDupArgs, json::Value> c;
            c.name = "autoitem.duplicate";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Copy an automation item (pooled by default: edits to either reach both).";
            c.description = "position: where the copy starts (default: right after the item).";
            c.tags = {"automation"};
            c.args = schema::object<AutoItemDupArgs>("autoitem_duplicate_args")
                         .field("item", &AutoItemDupArgs::item, idArg("ai", "Automation item"))
                         .field("position", &AutoItemDupArgs::position, schema::number().min(0))
                         .field("pooled", &AutoItemDupArgs::pooled, schema::boolean())
                         .defaultValue(true)
                         .build();
            c.result = schema::any();
            c.examples = {{"A pooled copy right after it", obj(R"({"item": "ai_1"})"),
                           obj(R"("<any>")"), "song_with_autoitem"}};
            c.run = [entryOf](const AutoItemDupArgs& a, Context& ctx) -> Result<json::Value> {
                EngineApp& app = A(ctx);
                const model::Session s = app.view();
                auto id = existing<model::AutomationItem>(s, a.item, "/item");
                if (!id) return std::move(id).error();
                model::AutomationItem copy = *s.autoItems.find(*id);
                auto& t = app.txn();
                if (a.pooled && copy.pool == 0) {  // a new pool for the two
                    int top = 0;
                    for (const auto& e : s.autoItems) top = std::max(top, e.value->pool);
                    if (top == std::numeric_limits<int>::max())
                        return Error("autoitem.pool", "pool id space is exhausted", "/pooled",
                                     "duplicate unpooled or remove the highest pool");
                    copy.pool = top + 1;
                    t.modify<model::AutomationItem>(
                        *id, [&](model::AutomationItem& x) { x.pool = copy.pool; });
                } else if (!a.pooled) {
                    copy.pool = 0;
                }
                copy.position = a.position.value_or(copy.position + copy.length);
                const Id fresh = t.create(copy);
                return entryOf(fresh, copy);
            };
            addChecked(registry, std::move(c));
        }
        for (const bool unpool : {false, true}) {
            Command<AutoItemRef, Empty> c;
            c.name = unpool ? "autoitem.unpool" : "autoitem.remove";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = unpool ? "Take an automation item out of its pool (its points stay)."
                               : "Remove an automation item (the envelope under it plays again).";
            c.tags = {"automation"};
            c.args = schema::object<AutoItemRef>(unpool ? "autoitem_unpool" : "autoitem_remove")
                         .field("item", &AutoItemRef::item, idArg("ai", "Automation item"))
                         .build();
            c.result = emptySchema();
            c.examples = {{unpool ? "Unpool" : "Remove", obj(R"({"item": "ai_1"})"), obj("{}"),
                           "song_with_autoitem"}};
            c.run = [unpool](const AutoItemRef& a, Context& ctx) -> Result<Empty> {
                EngineApp& app = A(ctx);
                auto id = existing<model::AutomationItem>(app.view(), a.item, "/item");
                if (!id) return std::move(id).error();
                if (unpool)
                    app.txn().modify<model::AutomationItem>(
                        *id, [](model::AutomationItem& x) { x.pool = 0; });
                else
                    app.txn().remove<model::AutomationItem>(*id);
                return Empty{};
            };
            addChecked(registry, std::move(c));
        }
        {
            struct ListArgs {
                std::optional<std::string> env;
            };
            Command<ListArgs, json::Value> c;
            c.name = "autoitem.list";
            c.kind = Kind::Query;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "List automation items (all, or one envelope's), with their points.";
            c.tags = {"automation"};
            c.args = schema::object<ListArgs>("autoitem_list_args")
                         .field("env", &ListArgs::env, idArg("en", "Envelope"))
                         .build();
            c.result = schema::any();
            c.examples = {{"None yet", obj("{}"), obj(R"({"items": []})"), "empty"}};
            c.run = [entryOf](const ListArgs& a, Context& ctx) -> Result<json::Value> {
                const model::Session s = A(ctx).view();
                json::Value items = json::Value::array();
                for (const auto& e : s.autoItems)
                    if (!a.env || e.value->envelope.str() == *a.env)
                        items.push_back(entryOf(e.id, *e.value));
                json::Value out = json::Value::object();
                out["items"] = std::move(items);
                return out;
            };
            addChecked(registry, std::move(c));
        }
    }();
    // ---------------------------------------------------------------- engine.*
    [&] {
        Command<EngineDevicesArgs, EngineDevicesResult> c;
        c.name = "engine.devices";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List native audio device names, host APIs, rates and channel capacities.";
        c.description =
            "Enumerates PortAudio devices on macOS/Windows without opening a stream. "
            "backend defaults to portaudio; dummy returns an empty list without initializing "
            "hardware. Use exact host_api and name values with engine.start. Linux uses JACK "
            "ports and returns backend.unsupported; engine.inputs lists running JACK inputs.";
        c.tags = {"engine"};
        c.args = schema::object<EngineDevicesArgs>("engine_devices_args")
                     .field("backend", &EngineDevicesArgs::backend,
                            schema::enumeration<BackendArg>({{BackendArg::Jack, "jack"},
                                                             {BackendArg::Dummy, "dummy"},
                                                             {BackendArg::PortAudio, "portaudio"}}))
                     .build();
        const auto device =
            schema::object<AudioDevice>("audio_device")
                .field("host_api", &AudioDevice::hostApi, schema::string())
                .field("name", &AudioDevice::name, schema::string())
                .field("inputs", &AudioDevice::inputs, schema::integerOf<std::uint32_t>())
                .field("outputs", &AudioDevice::outputs, schema::integerOf<std::uint32_t>())
                .field("default_rate", &AudioDevice::defaultRate, schema::number())
                .field("default_input", &AudioDevice::defaultInput, schema::boolean())
                .field("default_output", &AudioDevice::defaultOutput, schema::boolean())
                .build();
        c.result = schema::object<EngineDevicesResult>("engine_devices_result")
                       .field("devices", &EngineDevicesResult::devices, schema::arrayOf(device))
                       .build();
        c.errors = {{"backend.unsupported", "native device enumeration is unavailable",
                     "use engine.inputs with JACK"}};
        c.examples = {
            {"Without hardware", obj(R"({"backend":"dummy"})"), obj(R"({"devices":[]})"), "empty"}};
        c.run = [](const EngineDevicesArgs& args, Context&) -> Result<EngineDevicesResult> {
            if (args.backend == BackendArg::Dummy) return EngineDevicesResult{};
            if (args.backend == BackendArg::Jack)
                return Error("backend.unsupported", "JACK exposes graph ports", "/backend",
                             "use engine.inputs while JACK is running");
            auto devices = PortAudioBackend::devices();
            if (!devices) return std::move(devices).error();
            return EngineDevicesResult{std::move(*devices)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<EngineStartArgs, EngineStatus> c;
        c.name = "engine.start";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start playback through JACK, native PortAudio, or a device-free dummy.";
        c.description =
            "backend: jack (Linux default), portaudio (macOS/Windows native device), or dummy "
            "(default "
            "elsewhere, no device). For portaudio, host_api, input_device and output_device "
            "select exact names from engine.devices. inputs defaults to 0 (playback only); "
            "outputs (the device channels masters can play to, track.set hw_output) defaults to "
            "one per physical playback port with JACK (2 to 64) and 2 otherwise. rate defaults to "
            "the project rate; a rate given is asked of "
            "the device (CoreAudio switches the device's rate when it can, WASAPI converts) or, "
            "with JACK on PipeWire, suggested to the graph (node.rate: applied only while the "
            "devices are idle and the rate is allowed). force_graph (JACK on PipeWire, default "
            "false) instead forces block as the graph's quantum, and rate if given, while the "
            "engine runs. PortAudio opens up to "
            "16 native MIDI inputs and outputs; engine.inputs lists their names. Dummy is paced "
            "like a device. "
            "block: the device block in frames "
            "(default 256); the device's "
            "period may differ and is reported. anticipative (default true): render ahead of "
            "the playhead on worker threads (edits are heard up to 200 ms later); false renders "
            "everything in the audio callback. The engine runs at the device's rate (media "
            "is resampled; renders stay at the project's rate). Background and live thread "
            "capacities "
            "follows the app's Audio processing threads preference (Automatic by default); "
            "live helpers join each callback with no added latency. memory_lock reports the "
            "default-off host-process memory policy: requested, active, and a structured OS "
            "error on failure. cpu_idle_latency reports the default-off 20 microsecond PM QoS "
            "request, released on engine stop. A protection failure does not prevent audio "
            "startup. MIDI output refusals are in midi_output_warnings; midi_output_latency "
            "reports signed audio-minus-MIDI frames per output. MIDI inputs that refuse to open "
            "are skipped: midi_warnings reports each "
            "device, numeric os_error, message and corrective hint; audio continues.";
        c.tags = {"engine"};
        c.args = schema::object<EngineStartArgs>("engine_start_args")
                     .field("backend", &EngineStartArgs::backend,
                            schema::enumeration<BackendArg>({{BackendArg::Jack, "jack"},
                                                             {BackendArg::Dummy, "dummy"},
                                                             {BackendArg::PortAudio, "portaudio"}}))
                     .field("host_api", &EngineStartArgs::hostApi, schema::string())
                     .field("input_device", &EngineStartArgs::inputDevice, schema::string())
                     .field("output_device", &EngineStartArgs::outputDevice, schema::string())
                     .field("rate", &EngineStartArgs::rate, schema::number().range(1, 768000))
                     .field("inputs", &EngineStartArgs::inputs, schema::integer().range(0, 64))
                     .field("outputs", &EngineStartArgs::outputs, schema::integer().range(2, 64))
                     .field("block", &EngineStartArgs::block, schema::integer().range(16, 4096))
                     .field("anticipative", &EngineStartArgs::anticipative, schema::boolean())
                     .field("force_graph", &EngineStartArgs::forceGraph, schema::boolean())
                     .build();
        c.result = engineStatusSchema();
        c.errors = {
            {"engine.running", "the engine is already running", "engine.stop first"},
            {"backend.device", "device or host API was not found",
             "choose names from engine.devices"},
            {"backend.config", "device format is invalid", "check rate and channel capacities"},
            {"backend.unavailable", "JACK is not installed", "install pipewire-jack"},
            {"backend.open", "no JACK server answered", "is PipeWire running?"},
            {"backend.unsupported", "backend is unavailable on this platform", "select dummy"}};
        c.examples = {
            {"Without a device", obj(R"({"backend": "dummy", "block": 128})"),
             obj(R"({"running": true, "backend": "dummy", "sample_rate": 48000.0, "block": 128,
                     "period": 128, "plan_latency": 0, "device_latency": 0, "xruns": 0, "midi_dropped": 0, "midi_warnings": [], "midi_out_dropped": 0, "midi_output_warnings": [], "midi_output_latency": [],
                     "midi_devices_changed": false, "midi_devices_message": "",
                     "preparing_audio": false, "preparation_error": "",
                     "prepared_version": "<any>", "preparation_version": 0,
                     "cycles": "<any>", "process_errors": 0, "restart_needed": false,
                     "anticipative": true, "anticipation_misses": 0, "live_helpers": "<any>",
                     "scheduling": {"callback": {"policy": "normal", "priority": 0},
                                    "live": "<any>",
                                    "renderer": {"policy": "normal", "priority": 0},
                                    "background": "<any>"},
                     "workgroup": {"state": "none", "joined": 0, "failed": 0, "os_error": 0,
                                   "message": "", "hint": ""},
                     "warnings": [],
                     "memory_lock": {"requested": false, "active": false},
                     "cpu_idle_latency": {"requested": false, "active": false},
                     "meter_drops": {"renderer": 0, "callback": 0}, "failed_fx": [],
                     "dropped": 0, "voice_dropped": 0, "failed_errors": [], "sync": "<any>"})"),
             "empty"}};
        c.run = [](const EngineStartArgs& a, Context& ctx) -> Result<EngineStatus> {
            EngineApp& app = A(ctx);
#if defined(__linux__)
            constexpr auto defaultBackend = BackendArg::Jack;
#else
            constexpr auto defaultBackend = BackendArg::Dummy;
#endif
            const auto backend = a.backend.value_or(defaultBackend);
            EngineDeviceOptions device;
            device.native.hostApi = a.hostApi.value_or("");
            device.native.inputDevice = a.inputDevice.value_or("");
            device.native.outputDevice = a.outputDevice.value_or("");
            device.rate = a.rate.value_or(0);
            device.inputs = static_cast<std::uint32_t>(a.inputs.value_or(0));
            device.outputs = static_cast<std::uint32_t>(a.outputs.value_or(0));
            device.forceGraph = a.forceGraph.value_or(false);
            SUPPLE_TRY(app.startEngine(backend == BackendArg::Dummy  ? "dummy"
                                       : backend == BackendArg::Jack ? "jack"
                                                                     : "portaudio",
                                       static_cast<std::uint32_t>(a.block.value_or(256)),
                                       a.anticipative.value_or(true), device));
            return app.engineStatus();
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, Empty> c;
        c.name = "engine.stop";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Stop real-time playback and release the audio device.";
        c.tags = {"engine"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = emptySchema();
        c.examples = {{"Stop", obj("{}"), obj("{}"), "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<Empty> {
            A(ctx).stopEngine();
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, json::Value> c;
        c.name = "engine.inputs";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The device's audio inputs, MIDI devices and the outputs masters play to.";
        c.description =
            "audio: one entry per input channel (channel, the 1-based number track.set "
            "record_input takes, and the device port it is connected to). midi: the MIDI devices "
            "(names track.set record_midi matches; JACK ports or native CoreMIDI/WinMM inputs, "
            "fixed until restart). midi_outputs: destination names for the hardware output API "
            "(routing and clock generation are separate). outputs: one entry per output channel "
            "(channel, the number "
            "track.set hw_output takes for a master, and the port it is connected to). While the "
            "engine is stopped: running false and "
            "empty lists (engine.start opens the device).";
        c.tags = {"engine", "record"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::any();
        c.examples = {
            {"Stopped", obj("{}"),
             obj(R"({"running": false, "audio": [], "midi": [], "outputs": [], "midi_outputs": []})"),
             "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<json::Value> {
            EngineApp& app = A(ctx);
            const Backend* backend = app.backend();
            json::Value out = json::Value::object();
            out["running"] = backend != nullptr;
            out["audio"] = json::Value::array();
            out["midi"] = json::Value::array();
            out["midi_outputs"] = json::Value::array();
            out["outputs"] = json::Value::array();
            if (!backend) return out;
            json::Value audio = json::Value::array();
            const auto names = backend->inputNames();
            const std::uint32_t count = backend->config().inputs;
            for (std::uint32_t ch = 0; ch < count; ++ch) {
                json::Value e = json::Value::object();
                e["channel"] = static_cast<std::int64_t>(ch + 1);
                e["port"] = ch < names.size() ? names[ch] : std::string();
                audio.push_back(std::move(e));
            }
            json::Value outputs = json::Value::array();
            const auto outNames = backend->outputNames();
            for (std::uint32_t ch = 0; ch < backend->config().outputs; ++ch) {
                json::Value e = json::Value::object();
                e["channel"] = static_cast<std::int64_t>(ch + 1);
                e["port"] = ch < outNames.size() ? outNames[ch] : std::string();
                outputs.push_back(std::move(e));
            }
            json::Value midi = json::Value::array();
            for (const std::string& name : backend->midiPorts()) midi.push_back(name);
            out["backend"] = backend->name();
            out["audio"] = std::move(audio);
            out["outputs"] = std::move(outputs);
            out["midi"] = std::move(midi);
            for (const auto& name : backend->midiOutputPorts()) out["midi_outputs"].push_back(name);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, EngineStatus> c;
        c.name = "engine.status";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Real-time engine state: backend, rate, block, device period, latency, xruns.";
        c.description =
            "preparing_audio: source preparation is pending; the previous ready plan stays "
            "audible. "
            "prepared_version is the last compiled version, preparation_version the pending target "
            "(zero when idle); preparation_error explains a failed preparation. "
            "plan_latency: the session's plugin delay in frames; device_latency: frames from "
            "the engine's outputs to the device. restart_needed: the device changed rate or went "
            "away; engine.stop and engine.start again. midi_devices_changed is advisory: "
            "MIDI topology changed but audio keeps running. midi_devices_message asks for an "
            "explicit engine restart to pick up the new set; existing port names stay fixed. "
            "midi_out_dropped counts hardware output rejections, queue/device loss and pending "
            "events discarded at shutdown. midi_output_warnings lists outputs skipped at start; "
            "midi_output_latency is signed audio-minus-MIDI latency per output, in frames. "
            "sync: external sync (sync.get has the same status with the settings). "
            "midi_dropped: MIDI input messages/packets rejected or lost to bounded queues "
            "(JACK and native; zero for the dummy backend). midi_warnings lists inputs skipped at start, with "
            "device, numeric os_error, message and corrective hint. meter_drops: entries a full "
            "ring "
            "dropped (the renderer's, the audio callback's) — a strip's peak/RMS missed a block, "
            "never audio. dropped/voice_dropped count lost plug-in output/voice records. "
            "memory_lock reports requested/active and an optional OS error with errno and a "
            "corrective hint, including while stopped. Bridges are excluded. cpu_idle_latency "
            "reports the owned 20 microsecond request with the same status/error fields; it "
            "affects system idle while held and leaves CPU frequency policy unchanged. "
            "live_helpers is the enabled live helper capacity. scheduling: each thread class's "
            "policy (none, normal, fifo, rr, high) and priority as applied at start: the device "
            "callback, live helpers, renderer and background helpers (real-time below a "
            "real-time callback when permitted). workgroup: whether the live helpers joined the "
            "device's audio workgroup (macOS CoreAudio; state none, ready, joined, partial or "
            "failed, with the helper counts and the last refusal's OS error, message and hint). "
            "warnings includes scheduling fallbacks and "
            "plan warnings with corrective actions. failed_errors adds structured code, "
            "message, path and hint to each failure. "
            "failed_fx: isolated plugins whose "
            "process crashed or hung (silent now; fx.restart re-creates them).";
        c.tags = {"engine"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = engineStatusSchema();
        c.examples = {{"Not running", obj("{}"),
                       obj(R"({"running": false, "backend": "", "sample_rate": 0.0, "block": 0,
                     "period": 0, "plan_latency": 0, "device_latency": 0, "xruns": 0, "midi_dropped": 0, "midi_warnings": [], "midi_out_dropped": 0, "midi_output_warnings": [], "midi_output_latency": [],
                     "midi_devices_changed": false, "midi_devices_message": "",
                     "preparing_audio": false, "preparation_error": "",
                     "prepared_version": 0, "preparation_version": 0,
                     "cycles": 0, "process_errors": 0, "restart_needed": false,
                     "anticipative": false, "anticipation_misses": 0, "live_helpers": 0,
                     "scheduling": {"callback": {"policy": "none", "priority": 0},
                                    "live": {"policy": "none", "priority": 0},
                                    "renderer": {"policy": "none", "priority": 0},
                                    "background": {"policy": "none", "priority": 0}},
                     "workgroup": {"state": "none", "joined": 0, "failed": 0, "os_error": 0,
                                   "message": "", "hint": ""},
                     "warnings": [],
                     "memory_lock": {"requested": false, "active": false},
                     "cpu_idle_latency": {"requested": false, "active": false},
                     "meter_drops": {"renderer": 0, "callback": 0}, "failed_fx": [],
                     "dropped": 0, "voice_dropped": 0, "failed_errors": [], "sync": "<any>"})"),
                       "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<EngineStatus> {
            return A(ctx).engineStatus();
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- transport.playrate.*
    [&] {
        Command<NoArgs, PlayrateInfo> c;
        c.name = "transport.playrate.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Get requested/effective master playrate, pitch mode and active envelope.";
        c.description =
            "requested_rate is the saved project ratio; envelope names the active nonempty "
            "tr_0/playrate envelope, including automation items. No engine is required. "
            "effective_rate follows consumed output, including compensated source transitions "
            "and graph/backend output latency; it is 1.0 without a running engine. GUI clients "
            "can use EngineApp::playrateState() without executing/logging a query each sync.";
        c.tags = {"transport", "automation"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = playrateSchema();
        c.examples = {{"Factory settings", obj("{}"),
                       obj(R"({"requested_rate":1.0,"effective_rate":1.0,"preserve_pitch":false})"),
                       "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<PlayrateInfo> {
            return playrateInfo(A(ctx).view(), A(ctx).engine());
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<PlayrateArgs, PlayrateInfo> c;
        c.name = "transport.playrate.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set the project's master playrate ratio and preserve-pitch flag.";
        c.description =
            "rate: finite 0.25..4.0 (default 1); preserve_pitch defaults false in a new project. "
            "Omitted fields keep their values. Updates during a drag use Origin.gesture like "
            "strip faders, and write/touch/latch capture tr_0/playrate coarsely: unlike strip "
            "levels (ADR-0061), the curve keeps playing while it is written (no hold or touch "
            "return). Recording requires unity. "
            "Settings are saved/undoable. File sources convert before take FX and live input "
            "mixing; instruments retain their pitch. Scalar edits ramp and converter changes "
            "crossfade for 10 ms after priming. Edits include the render horizon and compensated "
            "source lookahead; newer requests coalesce while a transition finishes. Long-latency "
            "take chains can exceed the usual 200 ms response. effective_rate follows audible "
            "output, independently of requested_rate.";
        c.tags = {"transport", "automation"};
        c.args = schema::object<PlayrateArgs>("transport_playrate_set_args")
                     .field("rate", &PlayrateArgs::rate,
                            schema::number().range(model::kMinPlayrate, model::kMaxPlayrate))
                     .field("preserve_pitch", &PlayrateArgs::preservePitch, schema::boolean())
                     .build();
        c.result = playrateSchema();
        c.errors = {{"record.playrate", "recording requires unity master playrate",
                     "stop recording before changing rate or enabling a non-unity rate envelope"}};
        c.examples = {{"Store double speed with pitch preservation",
                       obj(R"({"rate":2,"preserve_pitch":true})"),
                       obj(R"({"requested_rate":2.0,"effective_rate":1.0,"preserve_pitch":true})"),
                       "empty"}};
        c.run = [](const PlayrateArgs& a, Context& ctx) -> Result<PlayrateInfo> {
            EngineApp& app = A(ctx);
            // Refuse before recording an automation move into transient writer
            // state; a failed transaction must not leave a latch hold behind.
            if (app.engine() && app.engine()->recording() && a.rate && *a.rate != 1)
                return Error("record.playrate", "recording requires unity master playrate", "/rate",
                             "stop recording before changing master playrate");
            auto project = app.view().project;
            if (a.rate || a.preservePitch) {
                auto next = *project;
                if (a.rate) next.playrate.rate = *a.rate;
                if (a.preservePitch) next.playrate.preservePitch = *a.preservePitch;
                if (next.playrate != project->playrate) app.txn().setProject(std::move(next));
                if (a.rate) app.recordAutomation(model::kMaster, "playrate", *a.rate);
            }
            return playrateInfo(app.view(), app.engine());
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, Empty> c;
        c.name = "transport.playrate.apply-to-tempo";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Scale every tempo-point BPM by master playrate, then reset the rate to 1.";
        c.description =
            "One version: multiply every BPM by the saved scalar rate, preserving beat "
            "positions, ramp flags and meter changes, and reset rate to 1. Require stopped "
            "transport, no recording and no active nonempty master playrate envelope. "
            "Reject resulting BPM outside 1..1000 before mutation. Existing item timebases "
            "still apply; time-locked audio is not made sound-equivalent by this tempo edit.";
        c.tags = {"transport", "tempo"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = emptySchema();
        c.errors = {{"playrate.playing", "the transport is playing",
                     "transport.stop before applying playrate to tempo"},
                    {"record.playrate", "a recording is active", "transport.stop first"},
                    {"playrate.envelope", "an active nonempty master playrate envelope exists",
                     "disable it with env.set {active: false} before applying the scalar rate"},
                    {"tempo.range", "a scaled BPM is outside 1..1000",
                     "reduce the master rate or adjust the tempo map first"}};
        c.examples = {{"Apply the factory unity rate", obj("{}"), obj("{}"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            if (const auto* engine = app.engine()) {
                if (engine->recording())
                    return Error("record.playrate", "stop recording before applying playrate",
                                 "/project/playrate",
                                 "call transport.stop, then apply the playrate to tempo");
                if (engine->transport().playing || engine->transport().countingIn)
                    return Error("playrate.playing", "stop playback before applying playrate",
                                 "/transport/playing",
                                 "call transport.stop, then apply the playrate to tempo");
            }
            const auto s = app.view();
            if (const auto env = model::activePlayrateEnvelope(s))
                return Error("playrate.envelope", "an active master playrate envelope exists",
                             "/envelopes/" + env->str(),
                             "disable the envelope with env.set {active: false} first");
            const double rate = s.project->playrate.rate;
            for (const auto& e : s.tempo) {
                const double bpm = e.value->bpm * rate;
                if (!std::isfinite(bpm) || bpm < 1 || bpm > 1000)
                    return Error("tempo.range", "applying playrate puts BPM outside 1..1000",
                                 "/tempo/points/" + e.id.str() + "/bpm",
                                 "reduce master playrate or adjust this tempo point first");
            }
            if (rate != 1) {
                for (const auto& e : s.tempo)
                    app.txn().modify<model::TempoPointObj>(
                        e.id, [rate](auto& point) { point.bpm *= rate; });
                auto project = *s.project;
                project.playrate.rate = 1;
                app.txn().setProject(std::move(project));
            }
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();

    // ---------------------------------------------------------------- transport.*
    for (const auto& [action, summary, playing] :
         {std::tuple{"play", "Start playback from the current position.", true},
          std::tuple{"pause", "Pause playback where it is (ends a recording).", false},
          std::tuple{"stop", "Stop playback and return to where it started (ends a recording).",
                     false},
          std::tuple{"record", "Play and record every armed track's input or master's output.",
                     true}}) {
        Command<NoArgs, TransportInfo> c;
        c.name = std::string("transport.") + action;
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = summary;
        c.tags = {"transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = transportSchema();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"},
                    {"record.nothing_armed", "no armed track has an input",
                     "track.set {armed: true, record_input: [1, 2]}, or arm a master"},
                    {"record.busy", "the audio callback did not return for 2 s (still recording)",
                     "stop again, or engine.stop (the files stay recoverable)"}};
        const bool rec = std::string(action) == "record";
        if (rec) {
            c.description =
                "Recording requires saved master rate 1 and no active master rate "
                "envelope that leaves 1 anywhere, including automation items. "
                "Non-unity recording and an armed track with unsupported output record mode "
                "are refused before opening files. An armed master records its output (after "
                "its fader, before its own items) as an item on the master; while it records, "
                "its items are silent in the punch range, or everywhere without punch. "
                "Armed channels in midi_overdub or midi_replace also record every incoming "
                "Note Out route, alongside an optional MIDI device input; no instrument is "
                "required. Rack Out audio inputs record stereo after input FX, before items "
                "and track inserts.";
            c.errors.push_back({"record.rack_input",
                                "the loaded plan has no armed rack input for the target",
                                "select an Out with track.set {record_rack, armed: true} and load "
                                "it before recording"});
            c.errors.push_back(
                {"record.midi_route", "a Note Out recording target is not armed in the loaded plan",
                 "arm the destination in a MIDI record mode and load it before recording"});
            c.errors.push_back({"record.playrate", "master playrate can leave unity",
                                "set rate to 1 and disable non-unity master playrate automation"});
            c.errors.push_back({"record.output_unsupported",
                                "output recording is not supported yet",
                                "use track.set {record_mode: input} on the reported track"});
        }
        c.examples = {{"While the engine runs", obj("{}"),
                       obj(std::string(R"({"playing": )") + (playing ? "true" : "false") +
                           R"(, "recording": )" + (rec ? "true" : "false") +
                           R"(, "seconds": "<any>", "beat": "<any>", "sample": "<any>",)"
                           R"( "loop": "<any>", "punch": "<any>", "counting_in": false})"),
                       rec ? "engine_armed" : "engine_running"}};
        c.run = [act = std::string(action)](const NoArgs&, Context& ctx) -> Result<TransportInfo> {
            return A(ctx).transport(act);
        };
        addChecked(registry, std::move(c));
    }
    [&] {
        Command<SeekArgs, TransportInfo> c;
        c.name = "transport.seek";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move the play position (one of seconds, beat or bar).";
        c.description =
            "With master playrate enabled, prepare file-source converters off-thread "
            "before publishing the audible seek. Existing playback and live monitoring "
            "continue until ready; preparation errors leave the old seek epoch intact.";
        c.tags = {"transport"};
        c.args = schema::object<SeekArgs>("seek_args")
                     .field("seconds", &SeekArgs::seconds, schema::number().min(0))
                     .field("beat", &SeekArgs::beat, schema::number().min(0))
                     .field("bar", &SeekArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .build();
        c.result = transportSchema();
        c.errors = {
            {"engine.stopped", "the engine is not running", "engine.start first"},
            {"transport.position", "not exactly one position", "give seconds, beat or bar"}};
        c.examples = {
            {"To bar 2 (120 BPM, 4/4)", obj(R"({"bar": 2})"),
             obj(R"({"playing": false, "recording": false, "seconds": 2.0, "beat": 4.0, "sample": 96000,
                    "loop": "<any>", "punch": "<any>", "counting_in": false})"),
             "engine_running"}};
        c.run = [](const SeekArgs& a, Context& ctx) -> Result<TransportInfo> {
            EngineApp& app = A(ctx);
            if (a.seconds.has_value() + a.beat.has_value() + a.bar.has_value() != 1)
                return Error("transport.position", "give exactly one position", "",
                             "seconds, beat or bar");
            const model::Session s = app.view();
            const double seconds =
                a.seconds
                    ? *a.seconds
                    : s.timeMap->secondsAt(beatsOf(
                          s, a.beat,
                          a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt));
            SUPPLE_TRY(model::checkedSeconds(
                s, seconds,
                a.seconds ? "/seconds"
                : a.bar   ? "/bar"
                          : "/beat",
                app.engine() ? app.engine()->config().sampleRate : s.project->sampleRate));
            return app.transport("seek", seconds);
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, TransportInfo> c;
        c.name = "transport.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The play position and whether it is playing.";
        c.tags = {"transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = transportSchema();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"}};
        c.examples = {
            {"Stopped at the start", obj("{}"),
             obj(R"({"playing": false, "recording": false, "seconds": 0.0, "beat": 0.0, "sample": 0,
                    "loop": {"enabled": false, "start_seconds": 0.0, "end_seconds": 0.0},
                    "punch": {"enabled": false, "start_seconds": 0.0, "end_seconds": 0.0},
                    "counting_in": false})"),
             "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TransportInfo> {
            return A(ctx).transport("get");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        struct MetronomeArgs {
            std::optional<bool> enabled;
            std::optional<double> volumeDb;
            std::optional<int> countInBars;
            std::optional<bool> countInPlayback;
        };
        Command<MetronomeArgs, MetronomeSettings> c;
        c.name = "transport.metronome";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Turn the metronome on or off and set its level; returns the settings.";
        c.description =
            "A click on every beat of the meter while the transport plays (the bar's first "
            "accented), in the live output only: renders and recordings never contain it. "
            "count_in_bars: that many bars of clicks (at the meter and tempo where playback "
            "starts) before transport.record begins — the position holds and nothing is "
            "recorded until the count ends; count_in_playback counts in before transport.play "
            "too. The count clicks even with the metronome off. "
            "Transport state: not an undo step, not saved in the project. No arguments: the "
            "current settings.";
        c.tags = {"transport", "recording"};
        c.args = schema::object<MetronomeArgs>("metronome_args")
                     .field("enabled", &MetronomeArgs::enabled, schema::boolean())
                     .field("volume_db", &MetronomeArgs::volumeDb,
                            schema::number().range(-60, 6).unit(units::Unit::dB))
                     .field("count_in_bars", &MetronomeArgs::countInBars,
                            schema::integerOf<int>().range(0, 8))
                     .field("count_in_playback", &MetronomeArgs::countInPlayback, schema::boolean())
                     .build();
        c.result =
            schema::object<MetronomeSettings>("metronome")
                .field("enabled", &MetronomeSettings::enabled, schema::boolean())
                .field("volume_db", &MetronomeSettings::volumeDb, schema::number())
                .field("count_in_bars", &MetronomeSettings::countInBars, schema::integerOf<int>())
                .field("count_in_playback", &MetronomeSettings::countInPlayback, schema::boolean())
                .build();
        c.examples = {{"On", obj(R"({"enabled": true})"),
                       obj(R"({"enabled": true, "volume_db": -6.0, "count_in_bars": 0,
                               "count_in_playback": false})"),
                       "empty"},
                      {"Two bars of count-in before recording", obj(R"({"count_in_bars": 2})"),
                       obj(R"({"enabled": false, "volume_db": -6.0, "count_in_bars": 2,
                               "count_in_playback": false})"),
                       "empty"}};
        c.run = [](const MetronomeArgs& a, Context& ctx) -> Result<MetronomeSettings> {
            EngineApp& app = A(ctx);
            MetronomeSettings m = app.metronome();
            if (a.enabled) m.enabled = *a.enabled;
            if (a.volumeDb) m.volumeDb = *a.volumeDb;
            if (a.countInBars) m.countInBars = *a.countInBars;
            if (a.countInPlayback) m.countInPlayback = *a.countInPlayback;
            app.setMetronome(m);
            return m;
        };
        addChecked(registry, std::move(c));
    }();
    for (const bool loop : {true, false}) {
        Command<RangeArgs, TransportRange> c;
        c.name = loop ? "transport.loop" : "transport.punch";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = loop ? "Set or toggle the loop range: playback reaching its end repeats it."
                         : "Set or toggle the punch range: recordings keep only that range.";
        c.description =
            loop ? "start and end as seconds, beat or bar (start_bar 5, end_bar 9 = bars 5-8); "
                   "a range turns the loop on unless enabled says otherwise; enabled alone "
                   "toggles the current range; clear removes the range and turns the loop off. "
                   "Recording while looping makes one take per pass on one item. Transport "
                   "state: not an undo step, not saved in the project."
                 : "start and end as seconds, beat or bar; a range turns punch on unless enabled "
                   "says otherwise; clear removes the range and turns punch off. The recording "
                   "keeps running; the committed items cover only the punch range (the file "
                   "keeps everything). Not an undo step.";
        c.tags = {"transport", "recording"};
        c.args = schema::object<RangeArgs>("range_args")
                     .field("enabled", &RangeArgs::enabled, schema::boolean())
                     .field("clear", &RangeArgs::clear, schema::boolean())
                     .field("start_seconds", &RangeArgs::startSeconds, schema::number().min(0))
                     .field("start_beat", &RangeArgs::startBeat, schema::number().min(0))
                     .field("start_bar", &RangeArgs::startBar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("end_seconds", &RangeArgs::endSeconds, schema::number().min(0))
                     .field("end_beat", &RangeArgs::endBeat, schema::number().min(0))
                     .field("end_bar", &RangeArgs::endBar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .build();
        c.result = rangeSchema(loop ? "loop" : "punch");
        c.errors = {{"transport.range",
                     "not one start and one end, end not after start, or clear with a range",
                     "e.g. start_bar 5, end_bar 9"}};
        c.examples = {
            {"Bars 2-3 (120 BPM, 4/4)", obj(R"({"start_bar": 2, "end_bar": 4})"),
             obj(R"({"enabled": true, "start_seconds": 2.0, "end_seconds": 6.0})"), "empty"},
            {"Remove the range", obj(R"({"clear": true})"),
             obj(R"({"enabled": false, "start_seconds": 0, "end_seconds": 0})"), "empty"}};
        c.run = [loop](const RangeArgs& a, Context& ctx) -> Result<TransportRange> {
            EngineApp& app = A(ctx);
            TransportRange r = loop ? app.loop() : app.punch();
            const int starts =
                a.startSeconds.has_value() + a.startBeat.has_value() + a.startBar.has_value();
            const int ends =
                a.endSeconds.has_value() + a.endBeat.has_value() + a.endBar.has_value();
            if (starts > 1 || ends > 1 || starts != ends)
                return Error("transport.range", "give one start and one end (or neither)", "",
                             "provide exactly one start_seconds/start_beat/start_bar and one "
                             "end_seconds/end_beat/end_bar, or omit all six");
            if (a.clear.value_or(false)) {
                if (starts == 1 || a.enabled.value_or(false))
                    return Error("transport.range", "clear removes the range", "/clear",
                                 "send clear alone, or a start and end without clear");
                r = {};
            } else if (starts == 1) {
                const model::Session s = app.view();
                auto at = [&](std::optional<double> sec, std::optional<double> beat,
                              std::optional<std::int64_t> bar) {
                    return sec ? *sec
                               : s.timeMap->secondsAt(
                                     beatsOf(s, beat,
                                             bar ? std::optional<int>(static_cast<int>(*bar))
                                                 : std::nullopt));
                };
                const double start = at(a.startSeconds, a.startBeat, a.startBar);
                const double end = at(a.endSeconds, a.endBeat, a.endBar);
                SUPPLE_TRY(model::checkedSeconds(
                    s, start,
                    a.startSeconds ? "/start_seconds"
                    : a.startBar   ? "/start_bar"
                                   : "/start_beat",
                    app.engine() ? app.engine()->config().sampleRate : s.project->sampleRate));
                SUPPLE_TRY(model::checkedSeconds(
                    s, end,
                    a.endSeconds ? "/end_seconds"
                    : a.endBar   ? "/end_bar"
                                 : "/end_beat",
                    app.engine() ? app.engine()->config().sampleRate : s.project->sampleRate));
                if (!(end > start))
                    return Error("transport.range", "the end must come after the start",
                                 a.endSeconds ? "/end_seconds"
                                 : a.endBar   ? "/end_bar"
                                              : "/end_beat",
                                 "set the supplied end_seconds/end_beat/end_bar later than the "
                                 "supplied start_seconds/start_beat/start_bar");
                r.start = start;
                r.end = end;
                r.enabled = a.enabled.value_or(true);
            } else if (a.enabled) {
                if (*a.enabled && !(r.end > r.start))
                    return Error("transport.range", "there is no range to turn on", "",
                                 "provide one start_seconds/start_beat/start_bar and one "
                                 "end_seconds/end_beat/end_bar with end later than start");
                r.enabled = *a.enabled;
            }
            if (loop)
                app.setLoop(r);
            else
                app.setPunch(r);
            return r;
        };
        addChecked(registry, std::move(c));
    }
}

// --- describe ----------------------------------------------------------------------------------

std::string describeSession(const model::Session& s) {
    std::string out;
    const auto meter = s.timeMap->meterAt(1);
    const double endBeat = model::lastItemBeat(s);
    char head[256];
    std::snprintf(head, sizeof head, "Project \"%s\" · %g kHz · %g BPM %d/%d · %.1f s\n",
                  s.project->name.c_str(), s.project->sampleRate / 1000.0, s.timeMap->bpmAt(0),
                  meter.numerator, meter.denominator, s.timeMap->secondsAt(endBeat));
    out += head;
    auto barOf = [&](double beat) {
        const auto bb = s.timeMap->barBeat(beat);
        char b[48];
        if (bb.beat == 1 && bb.fraction == 0)
            std::snprintf(b, sizeof b, "bar %d", bb.bar);
        else
            std::snprintf(b, sizeof b, "bar %d.%d", bb.bar, bb.beat);
        return std::string(b);
    };
    std::function<void(Id, int)> walk = [&](Id id, int depth) {
        const model::Track& t = *s.tracks.find(id);
        {
            std::string line(static_cast<std::size_t>(2 * depth), ' ');
            line += id.str() + " " + (t.name.empty() ? "(unnamed)" : t.name);
            line += " [" + std::string(model::stripTypeName(t.type)) + "]";
            if (t.freeze) line += " [Frozen " + t.freeze->source.str() + "]";
            if (t.volumeDb != 0) {
                char v[32];
                std::snprintf(v, sizeof v, " · vol %+.1f dB", t.volumeDb);
                line += v;
            }
            if (t.pan != 0) line += " · pan " + std::to_string(static_cast<int>(t.pan));
            if (t.mute) line += " · muted";
            if (t.solo == model::Solo::On || t.solo == model::Solo::InPlace) line += " · solo";
            if (!t.chain.empty()) {
                line += " · fx:";
                for (Id f : t.chain) {
                    const model::FxInstance& fx = *s.fx.find(f);
                    line += " " + f.str() + " " +
                            (fx.plugin.name.empty() ? fx.plugin.id : fx.plugin.name);
                    if (fx.bypass) line += " (bypassed)";
                }
            }
            out += line + "\n";
            for (const auto& e : s.items) {
                if (e.value->track != id) continue;
                const model::Item& it = *e.value;
                std::string il(static_cast<std::size_t>(2 * depth + 2), ' ');
                const double start = model::itemSpan(s, it).startBeat;
                il += e.id.str() + " " + barOf(start) + "–" +
                      barOf(start + model::itemLengthBeats(s, it));
                if (model::itemTimebase(s, it) != model::Timebase::Beats)
                    il += model::itemTimebase(s, it) == model::Timebase::Time ? " [time]"
                                                                              : " [beats position]";
                const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
                if (take && take->source) {
                    if (take->source->kind == model::Kind::Midi) {
                        const model::MidiSource* m = s.midi.find(*take->source);
                        il += " (MIDI " + take->source->str() + ", " +
                              std::to_string(m ? m->sequence.notes.size() : 0) + " notes)";
                    } else {
                        const model::MediaSource* m = s.media.find(*take->source);
                        il += " (audio " + take->source->str() + " " +
                              (m ? fs::path(m->path).filename().string() : "") + ")";
                    }
                }
                if (take) {
                    if (take->transients)
                        il += " · " + std::to_string(take->transients->positions.size()) + " hits";
                    if (!take->stretchMarkers.empty())
                        il += " · " + std::to_string(take->stretchMarkers.size()) + " warp anchors";
                }
                if (it.mute) il += " muted";
                out += il + "\n";
            }
        }
        for (Id c : t.children) walk(c, depth + 1);
    };
    for (Id master : s.project->masters) walk(master, 0);
    if (!s.routes.empty()) {
        out += "routes:";
        for (const auto& e : s.routes) {
            const model::Route& r = *e.value;
            out += " " + e.id.str() + " " + (r.from.track ? r.from.track->str() : "in") + "→" +
                   (r.to.track ? r.to.track->str() : "out");
        }
        out += "\n";
    }
    if (!s.markers.empty()) {
        out += "markers:";
        for (const auto& e : s.markers)
            out +=
                " " + e.id.str() + " \"" + e.value->name + "\" " +
                barOf(e.value->timebase == model::Timebase::Time ? s.timeMap->beatAt(e.value->pos)
                                                                 : e.value->pos);
        out += "\n";
    }
    if (!s.regions.empty()) {
        out += "regions:";
        for (const auto& e : s.regions) {
            const auto beat = [&](double pos) {
                return e.value->timebase == model::Timebase::Time ? s.timeMap->beatAt(pos) : pos;
            };
            out += " " + e.id.str() + " \"" + e.value->name + "\" " + barOf(beat(e.value->start)) +
                   "–" + barOf(beat(e.value->end));
        }
        out += "\n";
    }
    return out;
}

}  // namespace supple::engine
