// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/freeze.h"
#include <algorithm>
#include <set>
#include "supple/base/sha256.h"
#include "supple/model/schemas.h"
#include "supple/model/versions.h"

namespace supple::model {
namespace {
std::set<Id> dependencies(const Session& s, Id track) {
    std::set<Id> ids{track};
    const auto* t = s.tracks.find(track);
    if (!t) return ids;
    ids.insert(t->chain.begin(), t->chain.end());
    ids.insert(t->inputChain.begin(), t->inputChain.end());
    for (const auto& item : s.items) {
        if (item.value->track != track) continue;
        ids.insert(item.id);
        for (Id takeId : item.value->takes) {
            ids.insert(takeId);
            if (const auto* take = s.takes.find(takeId)) {
                if (take->source) ids.insert(*take->source);
                ids.insert(take->chain.begin(), take->chain.end());
            }
        }
    }
    for (const auto& env : s.envelopes)
        if ((ids.count(env.value->target.object) && env.value->target.object != track) ||
            (env.value->target.object == kMaster && env.value->target.param == "playrate"))
            ids.insert(env.id);
    for (const auto& item : s.autoItems)
        if (ids.count(item.value->envelope)) ids.insert(item.id);
    return ids;
}
template <class T>
void append(json::Value& values, const IdMap<T>& objects, const std::set<Id>& ids,
            const std::map<Id, Id>& remap) {
    for (const auto& e : objects)
        if (ids.count(e.id)) {
            auto value = toJson(*e.value);
            if constexpr (std::is_same_v<T, MediaSource>) {
                if (!e.value->hash.empty()) value.asObject().erase("path");
            }
            values.push_back(remapIds(value, schemaOf<T>().jsonSchema(), remap));
        }
}
json::Value timing(const Session& s) {
    json::Value value = json::Value::object();
    value["sample_rate"] = s.project->sampleRate;
    value["timebase"] = timebaseSchema().encode(s.project->timebase);
    value["playrate"] = toJson(*s.project)["playrate"];
    value["tempo"] = json::Value::array();
    for (const auto& e : s.tempo) value["tempo"].push_back(toJson(*e.value));
    value["meters"] = json::Value::array();
    for (const auto& e : s.meters) value["meters"].push_back(toJson(*e.value));
    return value;
}
}  // namespace

std::string freezeDependencyHash(const Session& s, Id track) {
    auto ids = dependencies(s, track);
    std::map<Id, Id> remap;
    Counters counters;
    for (Id id : ids) remap[id] = Id{id.kind, ++counters[id.kind]};
    json::Value value = timing(s);
    value["seed"] = toJson(*s.project)["seed"];
    const auto* t = s.tracks.find(track);
    if (t) {
        Track sound = *t;
        sound.name.clear();
        sound.color.clear();
        sound.notes.clear();
        sound.parent.reset();
        sound.children.clear();
        sound.parentSend = {};
        sound.volumeDb = sound.pan = 0;
        sound.mute = false;
        sound.solo = Solo::Off;
        sound.locked = sound.hiddenTcp = sound.hiddenMixer = false;
        sound.folderCompact = 0;
        sound.learn.clear();  // controllers move the fader, never the print
        sound.freeze.reset();
        // Inactive record configuration never reaches the offline signal.
        sound.record = {};
        value["track"] = remapIds(toJson(sound), schemaOf<Track>().jsonSchema(), remap);
    }
    auto objects = json::Value::array();
    append(objects, s.items, ids, remap);
    append(objects, s.takes, ids, remap);
    append(objects, s.fx, ids, remap);
    append(objects, s.media, ids, remap);
    append(objects, s.midi, ids, remap);
    append(objects, s.envelopes, ids, remap);
    append(objects, s.autoItems, ids, remap);
    value["objects"] = std::move(objects);
    return "sha256:" + sha256Hex(json::writeCompact(value));
}

std::vector<Id> freezeFxIds(const Session& s, Id track) {
    std::vector<Id> out;
    for (Id id : dependencies(s, track))
        if (id.kind == Kind::Fx) out.push_back(id);
    return out;
}

std::optional<Id> frozenOwner(const Session& s, Id object) {
    for (const auto& e : s.tracks)
        if (e.value->freeze && dependencies(s, e.id).count(object)) return e.id;
    return std::nullopt;
}

Error frozenError(const Session& s, Id track, std::string path) {
    return Error("track.frozen", trackLabel(s, track) + " (" + track.str() + ") is frozen",
                 std::move(path), "unfreeze with track.unfreeze {track: \"" + track.str() + "\"}");
}

Status validateFreezeInput(const Session& s, Id track) {
    const auto* t = s.tracks.find(track);
    if (!t) return Error("track.unknown", "track does not exist", "/track", "use track.list");
    const auto ids = dependencies(s, track);
    auto unsupported = [&](std::string message) -> Status {
        return Error("freeze.unsupported", std::move(message), "/track",
                     "use an unarmed channel with no external inputs and unity master playrate");
    };
    if (t->type != StripType::Channel) return unsupported("only channel strips can freeze");
    if (t->record.armed)
        return unsupported("disarm the track before freezing (monitoring requires the arm)");
    if (s.project->playrate.rate != 1 || activePlayrateEnvelope(s))
        return unsupported("freeze requires unity master playrate with no active rate envelope");
    for (const auto& e : s.routes) {
        const auto& r = *e.value;
        if (r.to.track != track && !(r.to.fx && ids.count(*r.to.fx))) continue;
        // An unarmed hardware record route is configuration, not a signal source.
        if (t->record.input == e.id && !r.from.track) continue;
        return unsupported("external audio, MIDI or key input " + e.id.str() + " feeds this track");
    }
    return {};
}

Status validateFrozenState(const Session& s) {
    for (const auto& e : s.tracks) {
        if (!e.value->freeze) continue;
        SUPPLE_TRY(validateFreezeInput(s, e.id));
        const auto& f = *e.value->freeze;
        const auto* media = s.media.find(f.source);
        if (!media || f.contentFrames <= 0 || f.tailFrames < 0 || f.latencyFrames < 0 ||
            f.contentFrames > (1LL << 28) || f.tailFrames > (1LL << 28) - f.contentFrames ||
            f.latencyFrames > (1LL << 28) || media->frames != f.contentFrames + f.tailFrames ||
            media->channels != std::max(2, e.value->channels) ||
            media->sampleRate != s.project->sampleRate || media->format != "wav" ||
            media->hash.empty())
            return Error("freeze.media", "invalid frozen media descriptor",
                         "/tracks/" + e.id.str() + "/freeze",
                         "restore the frozen media or unfreeze");
    }
    return {};
}

Status validateFrozenTransition(const Session& before, const Session& after) {
    std::string frozen;
    for (const auto& e : before.tracks)
        if (const auto* next = after.tracks.find(e.id); e.value->freeze && next && next->freeze) {
            if (!frozen.empty()) frozen += ", ";
            frozen += trackLabel(before, e.id) + " (" + e.id.str() + ")";
        }
    if (!frozen.empty() && timing(before) != timing(after))
        return Error("track.frozen", "timing is locked by frozen tracks: " + frozen, "/project",
                     "use track.unfreeze on " + frozen + " before changing timing");
    for (const auto& e : before.tracks) {
        const auto* next = after.tracks.find(e.id);
        if (!e.value->freeze || !next || !next->freeze) continue;
        if (freezeDependencyHash(before, e.id) != freezeDependencyHash(after, e.id))
            return frozenError(before, e.id, "/tracks/" + e.id.str());
        if (auto allowed = validateFreezeInput(after, e.id); !allowed)
            return frozenError(before, e.id, "/tracks/" + e.id.str());
    }
    return validateFrozenState(after);
}
}  // namespace supple::model
