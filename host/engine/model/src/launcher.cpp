// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/launcher.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "supple/json/write.h"
#include "supple/model/context.h"
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
        for (auto [name, fade] :
             {std::pair{"fade_in", c.fadeIn}, std::pair{"fade_out", c.fadeOut}}) {
            if (!(std::isfinite(fade.length) && fade.length >= 0 && std::isfinite(fade.curve)))
                fail(at("clips", e.id, name), "a fade's length is finite and 0 or more");
            else if (fade.length > 0 && c.kind == ClipKind::Midi)
                fail(at("clips", e.id, name), "a MIDI clip has no fades, as MIDI items play none");
        }
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
        // Clip context (ADR-0098): the points a clip keeps, sorted and unique, and a
        // follow that needs a MIDI clip with a readable key at 0.
        {
            const std::string where = at("clips", e.id, "context");
            std::size_t keys = 0, chords = 0;
            for (std::size_t i = 0; i < c.context.size(); ++i) {
                const ContextPoint& p = c.context[i];
                const std::string pt = where + "/" + std::to_string(i);
                (p.type == ContextType::Key ? keys : chords)++;
                if (!std::isfinite(p.beat) || p.beat < 0)
                    fail(pt + "/beat", "a clip's context point lies at beat 0 or later");
                else if (c.kind == ClipKind::Midi && std::isfinite(c.length) && p.beat >= c.length)
                    fail(pt + "/beat", "the point lies at or past the clip's length");
                if (i > 0) {
                    const ContextPoint& q = c.context[i - 1];
                    if (p.beat < q.beat || (p.beat == q.beat && p.type < q.type))
                        fail(pt, "the points are not sorted by beat, then type (key first)");
                    else if (p.beat == q.beat && p.type == q.type)
                        fail(pt, "two points of one type at one beat");
                }
            }
            if (keys > kMaxContextPoints || chords > kMaxContextPoints)
                fail(where, "more than " + std::to_string(kMaxContextPoints) +
                                " context points of one type");
            if (c.kind == ClipKind::Audio &&
                (chords > 0 || keys > 1 || (keys == 1 && c.context.front().beat != 0)))
                fail(where, "an audio clip holds at most one key, at 0");
            if (c.follow != ClipFollow::None && !clipCanFollow(c))
                fail(at("clips", e.id, "follow"),
                     "follow needs a MIDI clip with a readable key at beat 0");
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

bool clipCanFollow(const Clip& clip) {
    return clip.kind == ClipKind::Midi && keyAt(clip, 0).has_value();
}

std::vector<ClipContextNote> fitClipContext(Clip& clip) {
    std::vector<ClipContextNote> notes;
    const auto words = [](const ContextPoint& p) {
        return p.type == ContextType::Key ? "key " + keyWords(p)
                                          : "chord " + (p.symbol.empty() ? "none" : p.symbol);
    };
    const auto drop = [&](const ContextPoint& p, const std::string& why) {
        notes.push_back({"clip.context_dropped", "dropped " + words(p) + " at beat " +
                                                     json::formatDouble(p.beat) + ": " + why});
    };
    std::vector<ContextPoint> points = clip.context;
    for (ContextPoint& p : points)
        if (std::isfinite(p.beat)) p.beat = std::round(p.beat * 960.0) / 960.0;
    std::stable_sort(points.begin(), points.end(),
                     [](const ContextPoint& a, const ContextPoint& b) {
                         if (a.beat != b.beat) return a.beat < b.beat;
                         return a.type < b.type;
                     });
    std::vector<ContextPoint> kept;
    std::size_t keys = 0, chords = 0;
    for (const ContextPoint& p : points) {
        if (!std::isfinite(p.beat) || p.beat < 0) {
            drop(p, "before the clip's start");
        } else if (clip.kind == ClipKind::Midi && std::isfinite(clip.length) &&
                   p.beat >= clip.length) {
            drop(p, "at or past the clip's length");
        } else if (clip.kind == ClipKind::Audio &&
                   (p.type != ContextType::Key || p.beat != 0 || keys > 0)) {
            drop(p, "an audio clip holds one key, at 0");
        } else if (!kept.empty() && kept.back().beat == p.beat && kept.back().type == p.type) {
            drop(p, "another point of its type is at that beat");
        } else if ((p.type == ContextType::Key ? keys : chords) >= kMaxContextPoints) {
            drop(p, "a clip holds at most " + std::to_string(kMaxContextPoints) + " of a type");
        } else {
            (p.type == ContextType::Key ? keys : chords)++;
            kept.push_back(p);
        }
    }
    clip.context = std::move(kept);
    if (clip.follow != ClipFollow::None && !clipCanFollow(clip)) {
        clip.follow = ClipFollow::None;
        notes.push_back(
            {"clip.follow_reset",
             std::string("follow is reset to none: ") +
                 (clip.kind == ClipKind::Audio ? "an audio clip cannot follow"
                                               : "no readable key holds at the clip's start")});
    }
    return notes;
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
