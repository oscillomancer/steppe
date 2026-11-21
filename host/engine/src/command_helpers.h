// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Types and helpers the core command files share (commands.cpp and the
// per-namespace *_commands.cpp files split from it).
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "analysis_commands.h"
#include "command_validation.h"
#include "freeze_commands.h"
#include "media_files.h"
#include "render_commands.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/engine/racks.h"
#include "supple/midi/midi.h"
#include "supple/model/audio_warp.h"
#include "supple/model/env_shape.h"
#include "supple/model/groups.h"
#include "supple/model/numeric.h"
#include "supple/model/render_settings.h"
#include "supple/model/schemas.h"
#include "supple/notation/notation.h"
#include "supple/params/params.h"
#include "supple/time/timeline.h"

namespace supple::engine {

std::vector<schema::Warning> removeRacksOn(model::EditTxn& t, const model::Session& s,
                                           model::Id track);
schema::Schema<SyncReport> syncStatusSchema();

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using cmd::NoArgs;
using model::Id;
namespace fs = std::filesystem;

inline json::Value j(std::string_view text) { return json::parse(text).value(); }

inline EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }

inline auto idArg(const char* kindPrefix, const char* what) {
    return schema::string()
        .pattern(std::string("^") + kindPrefix + "_[0-9]+$", std::string(what) + " id")
        .doc(std::string(what) + " id, e.g. \"" + kindPrefix + "_1\".");
}

// Existence check against the current view, with the candidates in the hint.
template <class T>
Result<Id> existing(const model::Session& s, const std::string& text, std::string path) {
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
                 std::move(path), some.empty() ? "there are none yet" : "existing: " + some);
}

inline double beatsOf(const model::Session& s, std::optional<double> beats,
                      std::optional<int> bar) {
    if (bar) return s.timeMap->beatOfBar(*bar);
    return beats.value_or(0.0);
}

struct Empty {};

inline schema::Schema<Empty> emptySchema() { return schema::object<Empty>("empty").build(); }

struct TextResult {
    std::string text;
};

inline schema::Schema<RecordingWarning> recordingWarningSchema() {
    return schema::object<RecordingWarning>("recording_warning")
        .field("code", &RecordingWarning::code, schema::string())
        .field("message", &RecordingWarning::message, schema::string())
        .field("path", &RecordingWarning::path, schema::string())
        .field("hint", &RecordingWarning::hint, schema::string())
        .field("track", &RecordingWarning::track, schema::string())
        .field("file", &RecordingWarning::file, schema::string())
        .build();
}

inline schema::Schema<RecordedInfo> recordedSchema() {
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

// A mixer parameter's unit, or none when `param` is not one.
inline std::optional<units::Unit> mixerUnit(model::Kind kind, const std::string& param) {
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

inline Result<EnvDomain> envDomain(EngineApp& app, const model::Session& s,
                                   const model::EnvTarget& t, const char* path) {
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
    if (t.object.kind == model::Kind::Route && t.param == "pan")
        if (const auto* route = s.routes.find(t.object); route && model::rackSendHome(s, *route))
            return Error("route.rack_send", "a rack send has level and mute only", path,
                         "automate volume or mute; pan in the rack or on the buss");
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

inline Result<double> envValue(const EnvDomain& d, const json::Value& v, const std::string& path) {
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

// Compiles notation into a MIDI source; the length in beats is the item's.
inline std::function<midi::Meter(int)> meterContext(const model::Session& s, double atBeat) {
    const auto map = s.timeMap;
    const int first = map->barBeat(std::max(0.0, atBeat)).bar;
    return [map, first](int bar) {
        const auto absolute = std::clamp<std::int64_t>(std::int64_t(first) + bar - 1, 1, INT_MAX);
        const auto meter = map->meterAt(static_cast<int>(absolute));
        return midi::Meter{meter.numerator, meter.denominator};
    };
}

// What compiling notation gives: the source, its length in beats, and the chord symbols
// the text voiced (ADR-0098), for the commands that record them as a clip's context.
struct NotationCompiled {
    model::MidiSource source;
    double beats = 0;
    std::vector<notation::ChordSymbol> chords;
};

inline Result<NotationCompiled> compileNotationFull(const model::Session& s,
                                                    const std::string& text, double atBeat) {
    notation::CompileContext cc;
    cc.ppq = s.project->ppq;
    cc.meterAt = meterContext(s, atBeat);
    auto compiled = notation::compile(text, cc);
    if (!compiled) {
        Error e = std::move(compiled).error();
        if (e.path.empty() || e.path.front() != '/') e.path = "/notation";
        return e;
    }
    NotationCompiled out;
    out.source.sequence = std::move(compiled->sequence);
    out.beats = static_cast<double>(out.source.sequence.length) / out.source.sequence.ppq;
    out.chords = std::move(compiled->chords);
    return out;
}

inline Result<std::pair<model::MidiSource, double>> compileNotation(const model::Session& s,
                                                                    const std::string& text,
                                                                    double atBeat) {
    auto full = compileNotationFull(s, text, atBeat);
    if (!full) return std::move(full).error();
    return std::pair{std::move(full->source), full->beats};
}

// A model entry the session itself refers to (an item's takes, a take's chain):
// an id the model hands back must resolve, and a dangling one is a structured
// error rather than a crash.
template <class T>
Result<const T*> dangling(const model::Session& s, Id id, std::string path) {
    const T* found = model::Traits<T>::map(s).find(id);
    if (found) return found;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown",
                 id.str() + " is missing from the model", std::move(path),
                 "the session refers to it; reload the project or fix the file");
}

struct IdResult {
    std::string id;
};

inline auto idResultSchema() {
    return schema::object<IdResult>("created_id")
        .field("id", &IdResult::id, schema::string())
        .build();
}

// The audio and MIDI signal graph between tracks and racks: parent sends,
// routes (all but `ignored`) and MIDI followers. Modulation routes are not
// edges here; the model's invariants check those.
inline std::map<Id, std::vector<Id>> signalEdges(const model::Session& s,
                                                 std::optional<Id> ignored = {}) {
    std::map<Id, std::vector<Id>> edges;
    for (const auto& e : s.tracks)
        if (e.value->parent && e.value->parentSend.enabled) edges[e.id].push_back(*e.value->parent);
    // Racks are nodes too (ADR-0062): a send reaches an In module's rack, an
    // Out module's rack feeds its channel.
    auto rackOf = [&](Id module) -> std::optional<Id> {
        const model::RackModule* m = s.modules.find(module);
        return m ? std::optional<Id>(m->rack) : std::nullopt;
    };
    for (const auto& e : s.routes) {
        if (e.id == ignored) continue;
        const auto source =
            e.value->from.module ? rackOf(*e.value->from.module) : e.value->from.track;
        const auto target = e.value->to.module ? rackOf(*e.value->to.module)
                            : e.value->to.fx   ? model::trackOfFx(s, *e.value->to.fx)
                                               : e.value->to.track;
        if (source && target) edges[*source].push_back(*target);
        if (source && e.value->midi && e.value->to.track)
            for (Id module : model::midiFollowers(s, *e.value->to.track, source))
                edges[*source].push_back(s.modules.find(module)->rack);
    }
    return edges;
}

inline std::optional<std::vector<Id>> routeCycle(const model::Session& s, Id from, Id to,
                                                 std::optional<Id> ignored = {}) {
    if (from == to) return std::vector<Id>{from, to};
    std::map<Id, std::vector<Id>> edges = signalEdges(s, ignored);
    std::set<Id> seen;
    std::vector<Id> path{to};
    std::function<bool(Id)> walk = [&](Id cur) {
        if (cur == from) return true;
        if (!seen.insert(cur).second) return false;
        for (Id next : edges[cur]) {
            path.push_back(next);
            if (walk(next)) return true;
            path.pop_back();
        }
        return false;
    };
    if (!walk(to)) return {};
    path.push_back(to);
    return path;
}

// A cycle's path by name, a rack marked as one: "Keys -> Synth (rack) ->
// Synth". With ids, each name is followed by its id instead, "Keys (tr_2) ->
// Synth (rk_1)", for a message an agent acts on.
inline std::string cycleText(const model::Session& s, const std::vector<Id>& path, bool ids) {
    std::string out;
    for (Id at : path) {
        const bool rack = at.kind == model::Kind::Rack;
        out += out.empty() ? "" : " -> ";
        out += rack ? model::rackLabel(s, at) : model::trackLabel(s, at);
        if (ids || rack) out += " (" + (ids ? at.str() : std::string("rack")) + ")";
    }
    return out;
}

}  // namespace supple::engine
