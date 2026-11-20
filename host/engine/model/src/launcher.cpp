// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/launcher.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "supple/model/take_owner.h"

namespace supple::model {
namespace {

std::string at(const char* coll, Id id, const char* field = nullptr) {
    std::string p = std::string("/") + coll + "/" + id.str();
    if (field) p += std::string("/") + field;
    return p;
}

}  // namespace

std::string_view launchQuantizeName(LaunchQuantize q) {
    for (const auto& [value, name] : kLaunchQuantizeNames)
        if (value == q) return name;
    return "bar";
}

void checkLauncher(const Session& s, std::vector<Error>& errors) {
    auto fail = [&](std::string path, std::string message) {
        if (errors.size() < 50)
            errors.emplace_back("model.invariant", std::move(message), std::move(path));
    };
    auto counter = [&](const auto& map, Kind k) {
        for (const auto& e : map)
            if (e.id.n >= s.next[k])
                fail("/project/next_ids/" + std::string(prefix(k)),
                     "counter " + std::to_string(s.next[k]) + " is not past " + e.id.str());
    };
    counter(s.scenes, Kind::Scene);
    counter(s.clips, Kind::Clip);
    if (s.scenes.size() > kMaxScenes)
        fail("/scenes", "more than " + std::to_string(kMaxScenes) + " scenes");
    if (s.clips.size() > kMaxClips)
        fail("/clips", "more than " + std::to_string(kMaxClips) + " clips");

    std::map<int, Id> orders;
    for (const auto& e : s.scenes) {
        const int order = e.value->order;
        if (order < 0)
            fail(at("scenes", e.id, "order"), "order " + std::to_string(order) + " is negative");
        else if (auto [it, fresh] = orders.emplace(order, e.id); !fresh)
            fail(at("scenes", e.id, "order"),
                 "order " + std::to_string(order) + " is also " + it->second.str() + "'s");
    }

    const TakeOwners owners(s);
    std::set<Id> claimed;
    std::map<std::pair<Id, Id>, Id> cells;
    for (const auto& e : s.clips) {
        const Clip& c = *e.value;
        if (const Track* tr = s.tracks.find(c.track); !tr)
            fail(at("clips", e.id, "track"), "track " + c.track.str() + " does not exist");
        else if (tr->type != StripType::Channel)
            fail(at("clips", e.id, "track"), c.track.str() + " is not a channel");
        if (!s.scenes.contains(c.scene))
            fail(at("clips", e.id, "scene"), "scene " + c.scene.str() + " does not exist");
        else if (auto [it, fresh] = cells.emplace(std::pair{c.track, c.scene}, e.id); !fresh)
            fail(at("clips", e.id),
                 it->second.str() + " is already in " + c.track.str() + "'s " + c.scene.str());
        if (!(std::isfinite(c.length) && c.length > 0))
            fail(at("clips", e.id, "length"), "length must be finite and above 0");
        if (c.loop.on && !(std::isfinite(c.loop.start) && c.loop.start >= 0 &&
                           std::isfinite(c.loop.length) && c.loop.length > 0))
            fail(at("clips", e.id, "loop"),
                 "a loop that is on starts at 0 or later and has a length");
        for (Id t : c.takes) {
            const Take* take = s.takes.find(t);
            if (!take) {
                fail(at("clips", e.id, "takes"), "take " + t.str() + " does not exist");
                continue;
            }
            const auto owner = owners.of(t);
            if (!claimed.insert(t).second || owner != e.id)
                fail(at("clips", e.id, "takes"),
                     t.str() + " also belongs to " + (owner ? *owner : e.id).str());
            if (const auto kind = clipKindOf(*take); kind && *kind != c.kind)
                fail(at("clips", e.id, "takes"),
                     t.str() + " holds " + std::string(clipKindName(*kind)) + "; the clip is " +
                         std::string(clipKindName(c.kind)));
        }
        if (c.activeTake &&
            std::find(c.takes.begin(), c.takes.end(), *c.activeTake) == c.takes.end())
            fail(at("clips", e.id, "active_take"),
                 c.activeTake->str() + " is not one of the clip's takes");
        if (!c.takes.empty() && !c.activeTake)
            fail(at("clips", e.id, "active_take"), "a clip with takes needs an active take");
    }
}

std::vector<std::pair<Id, std::string>> dropStrayClips(Session& s) {
    std::vector<std::pair<Id, std::string>> dropped;
    std::set<Id> takes;  // the dropped clips' takes that still exist
    for (const auto& e : s.clips) {
        const Clip& c = *e.value;
        std::string why;
        if (!s.tracks.contains(c.track))
            why = "track " + c.track.str() + " does not exist";
        else if (!s.scenes.contains(c.scene))
            why = "scene " + c.scene.str() + " does not exist";
        else if (c.activeTake && !s.takes.contains(*c.activeTake))
            why = "active take " + c.activeTake->str() + " does not exist";
        else
            for (Id t : c.takes)
                if (!s.takes.contains(t)) {
                    why = "take " + t.str() + " does not exist";
                    break;
                }
        if (why.empty()) continue;
        dropped.emplace_back(e.id, std::move(why));
        for (Id t : c.takes)
            if (s.takes.contains(t)) takes.insert(t);
    }
    if (dropped.empty()) return dropped;
    IdMap<Clip>::Builder clips(s.clips);
    for (const auto& [id, why] : dropped) clips.erase(id);
    s.clips = clips.build();
    // Their takes that nothing else holds go with them, with their FX and the
    // envelopes on them, so no orphan is left to fail the open.
    const TakeOwners owners(s);
    std::set<Id> gone;
    for (Id t : takes)
        if (!owners.of(t)) gone.insert(t);
    if (gone.empty()) return dropped;
    IdMap<Take>::Builder tb(s.takes);
    IdMap<FxInstance>::Builder fb(s.fx);
    std::set<Id> targets = gone;
    for (Id t : gone) {
        for (Id f : s.takes.find(t)->chain) {
            fb.erase(f);
            targets.insert(f);
        }
        tb.erase(t);
    }
    IdMap<Envelope>::Builder eb(s.envelopes);
    std::set<Id> envelopes;
    for (const auto& e : s.envelopes)
        if (targets.contains(e.value->target.object)) {
            eb.erase(e.id);
            envelopes.insert(e.id);
        }
    IdMap<AutomationItem>::Builder ab(s.autoItems);
    for (const auto& e : s.autoItems)
        if (envelopes.contains(e.value->envelope)) ab.erase(e.id);
    s.takes = tb.build();
    s.fx = fb.build();
    s.envelopes = eb.build();
    s.autoItems = ab.build();
    return dropped;
}

std::optional<Id> clipAt(const Session& s, Id track, Id scene) {
    for (const auto& e : s.clips)
        if (e.value->track == track && e.value->scene == scene) return e.id;
    return std::nullopt;
}

std::optional<ClipKind> clipKindOf(const Take& take) {
    if (!take.source) return std::nullopt;
    return take.source->kind == Kind::Midi ? ClipKind::Midi : ClipKind::Audio;
}

std::string_view clipKindName(ClipKind k) { return k == ClipKind::Midi ? "midi" : "audio"; }

}  // namespace supple::model
