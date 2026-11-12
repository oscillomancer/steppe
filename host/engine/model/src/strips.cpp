// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/schemas.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace supple::model {

bool validTrackColor(std::string_view color) {
    if (color.empty()) return true;
    if (std::find(kTrackColors.begin(), kTrackColors.end(), color) != kTrackColors.end())
        return true;
    return color.size() == 7 && color[0] == '#' &&
           std::all_of(color.begin() + 1, color.end(),
                       [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string_view stripTypeName(StripType type) {
    switch (type) {
        case StripType::Channel:
            return "channel";
        case StripType::Buss:
            return "buss";
        case StripType::Master:
            return "master";
    }
    return "unknown";
}

schema::Schema<StripType> stripTypeSchema() {
    return schema::enumeration<StripType>({{StripType::Channel, "channel"},
                                           {StripType::Buss, "buss"},
                                           {StripType::Master, "master"}});
}

schema::Schema<PanLaw> panLawSchema() {
    const auto number = schema::number().unit(schema::Unit::dB);
    auto spec = number.jsonSchema();
    spec["enum"] = json::Value::array();
    for (const double db : kPanLawsDb) spec["enum"].asArray().push_back(db);
    schema::Schema<double> db{
        "pan_law_db",
        [number](const json::Value& v, double& out, schema::DecodeCtx& ctx,
                 const std::string& path) {
            if (!number.decodeInto(v, out, ctx, path)) return false;
            if (std::find(kPanLawsDb.begin(), kPanLawsDb.end(), out) == kPanLawsDb.end()) {
                ctx.error("pan_law.db", "a pan law is 0, -2.5, -3, -4.5 or -6 dB", path,
                          "set db to one of 0, -2.5, -3, -4.5 or -6");
                return false;
            }
            return true;
        },
        [number](double v) { return number.encode(v); }, spec};
    return schema::object<PanLaw>("pan_law")
        .field("db", &PanLaw::db, db)
        .defaultValue(0.0)
        .field("boost", &PanLaw::boost, schema::boolean())
        .defaultValue(false)
        .omitIfDefault()
        .build();
}

schema::Schema<PanMode> panModeSchema() {
    return schema::enumeration<PanMode>(
        {{PanMode::Pan, "pan"}, {PanMode::Dual, "dual"}, {PanMode::Width, "width"}});
}

schema::Schema<RoutePan> routePanSchema() {
    auto numeric = schema::number().range(-100, 100).unit(schema::Unit::percent);
    json::Value exported = json::Value::object();
    exported["oneOf"] = json::Value::array();
    exported["oneOf"].push_back(numeric.jsonSchema());
    json::Value follow = json::Value::object();
    follow["const"] = "follow";
    exported["oneOf"].push_back(std::move(follow));
    return {"send pan",
            [numeric](const json::Value& v, RoutePan& out, schema::DecodeCtx& ctx,
                      const std::string& path) {
                if (v.isString() && v.asString() == "follow") {
                    out.percent.reset();
                    return true;
                }
                double pan = 0;
                if (!numeric.decodeInto(v, pan, ctx, path)) return false;
                out.percent = pan;
                return true;
            },
            [](const RoutePan& pan) {
                return pan.percent ? json::Value(*pan.percent) : json::Value("follow");
            },
            std::move(exported)};
}

Status validateStripRules(const Session& s) {
    auto trackError = [](std::string message, std::string path, std::string hint) -> Status {
        return Error("track.strip_type", std::move(message), std::move(path), std::move(hint));
    };
    auto routeError = [](Id id, std::string message, std::string field,
                         std::string hint) -> Status {
        return Error("route.strip_type", std::move(message), "/routes/" + id.str() + "/" + field,
                     std::move(hint));
    };
    const auto& masters = s.project->masters;
    if (masters.empty() || masters.front() != kMaster)
        return trackError("the main master must be first", "/project/masters",
                          "keep tr_0 first in the ordered master list");
    std::set<Id> roots;
    for (Id id : masters) {
        const Track* t = s.tracks.find(id);
        if (!roots.insert(id).second || !t || t->type != StripType::Master || t->parent)
            return trackError("masters must list distinct master roots", "/project/masters",
                              "list each parentless master once, with tr_0 first");
    }
    for (const auto& e : s.tracks) {
        const Track& t = *e.value;
        const std::string path = "/tracks/" + e.id.str();
        if (t.type != StripType::Channel && t.type != StripType::Buss &&
            t.type != StripType::Master)
            return trackError("unknown strip type", path + "/type", "use channel, buss or master");
        if (t.type == StripType::Master) {
            if (t.parent || !roots.contains(e.id))
                return trackError("a master must be a listed root", path + "/parent",
                                  "give a master no parent and list it in project.masters");
            // A master's pan is a balance (ADR-0019 §5); modes and laws are
            // a channel's or buss's (ADR-0066).
            const Track plain;
            if (t.panMode != plain.panMode || t.width != plain.width ||
                t.panLeft != plain.panLeft || t.panRight != plain.panRight || t.panLaw)
                return trackError("a master's pan is a balance, with no pan mode or law",
                                  path + (t.panLaw ? "/pan_law" : "/pan_mode"),
                                  "use pan on the master; set pan modes and laws on channels "
                                  "and busses");
        } else {
            const Track* parent = t.parent ? s.tracks.find(*t.parent) : nullptr;
            if (!parent || parent->type == StripType::Channel)
                return trackError("a channel or buss needs a buss/master parent", path + "/parent",
                                  "add a buss and place the channels under it, or use tr_0");
        }
        // The main pair, plus auxiliary channels (ADR-0020).
        if (t.channels < 1 || t.channels > kMaxTrackChannels ||
            (t.type != StripType::Channel && t.channels < 2))
            return trackError("a strip has 1-128 channels; busses and masters at least 2",
                              path + "/channels",
                              "use 2 for a stereo strip, more for auxiliary channels 3-128");
        if (t.type == StripType::Channel && !t.children.empty())
            return trackError("a channel cannot have children", path + "/children",
                              "add a buss and place the channels under it");
        // A master may be armed: it records its output (ADR-0057). Inputs,
        // monitoring, record modes and input FX stay on channels.
        if (t.type != StripType::Channel &&
            ((t.record.armed && t.type != StripType::Master) || t.record.input ||
             t.record.midiInput || t.record.monitor != Monitor::Off ||
             t.record.mode != RecordMode::Input || !t.inputChain.empty()))
            return trackError(t.type == StripType::Master
                                  ? "a master records its output: no input, monitoring or input FX"
                                  : "only channels and masters can be armed",
                              path,
                              t.type == StripType::Master
                                  ? "arm the master alone; record or monitor inputs on a channel"
                                  : "record or monitor on a child channel instead");
        if (t.laneMute && t.type != StripType::Master)
            return trackError("only masters have a lane mute", path + "/lane_mute",
                              "mute the items themselves, or the strip");
        for (const auto& learn : t.learn)
            if (t.type == StripType::Master &&
                (learn.param == "width" || learn.param == "pan_left" ||
                 learn.param == "pan_right" || learn.param == "pan_boost"))
                return trackError("a master has no " + learn.param, path + "/learn",
                                  "learn these pan controls on a channel or buss");
        if (t.hwOutput) {  // ADR-0056
            const auto& out = *t.hwOutput;
            if (t.type != StripType::Master)
                return trackError("only masters play to hardware outputs", path + "/hw_output",
                                  "send this strip to a master and set that master's hw_output");
            if (out.size() > 2 || (out.size() == 2 && out[0] == out[1]))
                return Error(
                    "track.hw_output", "hw_output is two different channels, one channel or none",
                    path + "/hw_output", "e.g. [3, 4] for a pair, [5] for mono, [] for none");
        }
        // A MIDI-only input may retain its legacy alias in record.input;
        // two distinct routes would be two inputs (ADR-0074).
        if (t.record.input && t.record.midiInput && t.record.input != t.record.midiInput)
            return Error("track.record_input", "a track has one input: audio, MIDI or rack",
                         path + "/record/input",
                         "choose one input with track.set {record_input, record_midi or "
                         "record_rack}; route MIDI to a rack's Notes from another channel");
        if (t.record.midiInput) {
            const auto* route = s.routes.find(*t.record.midiInput);
            if (!route || !route->from.midiDevice || route->to.track != e.id)
                return trackError("a MIDI input must enter this channel from a MIDI device",
                                  path + "/record/midi_input",
                                  "choose a MIDI input route into this channel");
        }
        if (t.record.input) {
            const Route* r = s.routes.find(*t.record.input);
            if (!r || (!r->from.hwChannels && !r->from.midiDevice && !r->from.module) ||
                r->to.track != e.id)
                return trackError(
                    "a record input must enter this channel from hardware, MIDI or a rack",
                    path + "/record/input",
                    "choose a hardware, MIDI or rack Out input route into this channel");
        }
    }
    for (const auto& e : s.items) {
        const Track* t = s.tracks.find(e.value->track);
        if (t && t->type == StripType::Buss)
            return trackError("only channels and masters hold items",
                              "/items/" + e.id.str() + "/track",
                              "put the items on a child channel and route it into the buss");
        // A master's items are prints of a mix (ADR-0057): audio only.
        if (t && t->type == StripType::Master)
            for (Id take : e.value->takes)
                if (const Take* tk = s.takes.find(take);
                    tk && tk->source && tk->source->kind != Kind::Media)
                    return trackError("a master's items are audio",
                                      "/items/" + e.id.str() + "/takes",
                                      "put MIDI on a channel with an instrument");
    }
    bool hasPlayrate = false;
    for (const auto& e : s.envelopes) {
        const auto& target = e.value->target;
        if (target.param == "playrate" && target.object.kind != Kind::Fx) {
            if (target.object != kMaster || hasPlayrate)
                return Error("env.target", "master playrate has one envelope on tr_0",
                             "/envelopes/" + e.id.str() + "/target",
                             "use one envelope with track tr_0 and param playrate");
            hasPlayrate = true;
            continue;
        }
        if (target.object.kind == Kind::Track) {
            const bool position = target.param == "width" || target.param == "pan_left" ||
                                  target.param == "pan_right";
            const bool boost = target.param == "pan_boost";
            if (!position && !boost && target.param != "volume" && target.param != "pan" &&
                target.param != "mute")
                return trackError("unknown strip envelope parameter",
                                  "/envelopes/" + e.id.str() + "/target/param",
                                  "use volume, pan, mute, width, pan_left, pan_right or pan_boost");
            const Track* t = s.tracks.find(target.object);
            if ((position || boost) && t && t->type == StripType::Master)
                return trackError("a master's pan is a balance",
                                  "/envelopes/" + e.id.str() + "/target/param",
                                  "automate pan modes and gain compensation on channels or busses");
        }
    }
    for (const auto& e : s.routes) {
        const Route& r = *e.value;
        if (r.midi || r.from.midiDevice) continue;
        if (r.to.fx) {
            if (!r.from.track)
                return routeError(
                    e.id, "a key is fed from a strip tap", "from",
                    "route hardware into a channel, then send that channel to the key");
            continue;
        }
        if (r.to.hwChannels)  // ADR-0056: a master's hw_output, not a route
            return Error("route.hw_output", "hardware outputs are set on a master, not routed",
                         "/routes/" + e.id.str() + "/to",
                         "route the strip into a master and set that master's hw_output "
                         "(track.set {track, hw_output: [1, 2]})");
        if ((r.to.track || r.to.module) && r.from.track) {
            const Track* from = s.tracks.find(*r.from.track);
            if (from && from->type == StripType::Master)
                return routeError(e.id, "masters are terminal", "from",
                                  "masters feed hardware outputs and renders; use a buss to group");
        }
        if (r.to.track) {
            const Track* to = s.tracks.find(*r.to.track);
            if (!to) continue;  // reference errors belong to the invariant checker
            if (to->type == StripType::Channel && !r.from.hwChannels && !r.from.module)
                return routeError(e.id, "a channel accepts only hardware and rack inputs", "to",
                                  "add a buss and route the channels into it; use fx for a key");
            // A buss or master takes a rack's Out as a rack send (ADR-0075).
            if (to->type != StripType::Channel && !r.from.track && !r.from.module)
                return routeError(
                    e.id, "a buss/master accepts audio only from strips", "from",
                    "route hardware into a channel, then route that channel into the buss/master");
        }
    }
    return okStatus();
}

}  // namespace supple::model
