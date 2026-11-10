// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/groups.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace supple::model {
namespace {
template <class T>
bool has(const std::vector<T>& values, const T& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}
std::set<Id> controllers(const Session& s, Id target) {
    std::set<Id> out;
    std::vector<Id> pending{target};
    while (!pending.empty()) {
        const Id at = pending.back();
        pending.pop_back();
        for (const auto& e : s.groups) {
            if (!e.value->vca) continue;
            const auto& members =
                at.kind == Kind::Track ? e.value->members : e.value->vca->followers;
            if (has(members, at) && out.insert(e.id).second) pending.push_back(e.id);
        }
    }
    return out;
}
}  // namespace
std::vector<Id> trackGroups(const Session& s, Id track) {
    std::vector<Id> out;
    for (const auto& e : s.groups)
        if (has(e.value->members, track)) out.push_back(e.id);
    return out;
}
std::vector<Id> linkedTracks(const Session& s, Id track, std::string_view attribute) {
    if (attribute == "width" || attribute == "pan_left" || attribute == "pan_right" ||
        attribute == "pan_mode" || attribute == "pan_law")
        attribute = "pan";
    std::set<Id> out{track};
    std::vector<Id> pending{track};
    while (!pending.empty()) {
        const Id at = pending.back();
        pending.pop_back();
        for (const auto& e : s.groups)
            if (e.value->active && has(e.value->links, std::string(attribute)) &&
                has(e.value->members, at))
                for (Id member : e.value->members)
                    if (out.insert(member).second) pending.push_back(member);
    }
    return {out.begin(), out.end()};
}
std::vector<Id> linkedItems(const Session& s, Id item) {
    const Item* original = s.items.find(item);
    if (!original) return {};
    const auto tracks = linkedTracks(s, original->track, "edit");
    const auto span = itemSpan(s, *original);
    const auto start = time::samplesAt(span.startSec, s.project->sampleRate);
    const auto end = time::samplesAt(span.endSec, s.project->sampleRate);
    std::vector<Id> out;
    // Lanes currently carry stable IDs, not a separate ordered lane model.
    // Main matches main; numbered lane tokens match the same ordinal token.
    for (const auto& e : s.items) {
        if (e.id != item && e.value->track == original->track) continue;
        if (!has(tracks, e.value->track) || e.value->lane != original->lane) continue;
        const auto other = itemSpan(s, *e.value);
        if (time::samplesAt(other.startSec, s.project->sampleRate) == start &&
            time::samplesAt(other.endSec, s.project->sampleRate) == end)
            out.push_back(e.id);
    }
    return out;
}
std::vector<Id> vcaControllers(const Session& s, Id target) {
    auto out = controllers(s, target);
    out.erase(target);
    return {out.begin(), out.end()};
}
Status validateGroups(const Session& s) {
    auto fail = [](std::string code, std::string message, std::string path, std::string hint) {
        return Status(Error(std::move(code), std::move(message), std::move(path), std::move(hint)));
    };
    for (const auto& e : s.groups) {
        const auto& g = *e.value;
        const std::string path = "/groups/" + e.id.str();
        if (!validTrackColor(g.color))
            return fail("group.color", "unknown group color", path + "/color",
                        "use a named track color or #rrggbb");
        std::set<Id> seen;
        for (Id id : g.members)
            if (!s.tracks.contains(id) || !seen.insert(id).second)
                return fail("group.member", "missing or repeated track " + id.str(),
                            path + "/members", "use each existing track once");
        std::set<std::string> links;
        for (const auto& link : g.links)
            if (!has(std::vector<std::string>{"volume", "pan", "mute", "solo", "armed", "monitor",
                                              "selection", "edit"},
                     link) ||
                !links.insert(link).second)
                return fail(
                    "group.attribute", "unknown or repeated link " + link, path + "/links",
                    "use volume, pan, mute, solo, armed, monitor, selection or edit once each");
        if (!g.vca) continue;
        if (!std::isfinite(g.vca->volumeDb) || g.vca->volumeDb < kSilenceDb || g.vca->volumeDb > 12)
            return fail("vca.level", "VCA volume is out of range", path + "/vca/volume_db",
                        "use -150 through +12 dB");
        seen.clear();
        for (Id id : g.vca->followers)
            if (!s.groups.contains(id) || !s.groups.find(id)->vca || !seen.insert(id).second)
                return fail("vca.member", "missing or repeated VCA " + id.str(),
                            path + "/vca/followers", "use each existing VCA group once");
        const auto parents = controllers(s, e.id);
        if (parents.contains(e.id))
            return fail("vca.cycle", "VCA cycle through " + e.id.str(), path + "/vca/followers",
                        "remove the follower link that reaches this VCA again");
        if (parents.size() >= kMaxVcaControllers)
            return fail("vca.limit", "too many nested VCA controls", path + "/vca/followers",
                        "use at most 32 controls including this VCA");
    }
    for (const auto& e : s.tracks)
        if (controllers(s, e.id).size() > kMaxVcaControllers)
            return fail("vca.limit", "more than 32 VCAs control " + e.id.str(),
                        "/tracks/" + e.id.str(), "remove a VCA membership or nesting link");
    for (const auto& e : s.envelopes) {
        const auto& env = *e.value;
        if (env.target.object.kind != Kind::Group) continue;
        const auto* g = s.groups.find(env.target.object);
        const auto path = "/envelopes/" + e.id.str();
        if (!g || !g->vca || (env.target.param != "volume" && env.target.param != "mute"))
            return fail("env.target", "group automation requires a VCA volume or mute target",
                        path + "/target", "add a VCA and target volume or mute");
        auto valid = [&](const std::vector<EnvPoint>& points, double baseline,
                         double amplitude) -> Status {
            for (const auto& pt : points) {
                const double value = baseline + amplitude * (pt.value - baseline);
                const double lo = env.target.param == "volume" ? kSilenceDb : 0;
                const double hi = env.target.param == "volume" ? 12 : 1;
                if (!std::isfinite(value) || value < lo || value > hi)
                    return fail("env.value", "VCA automation value is out of range",
                                path + "/points", "use -150..12 dB for volume or 0..1 for mute");
            }
            return {};
        };
        SUPPLE_TRY(valid(env.points, 0, 1));
        for (const auto& ai : s.autoItems)
            if (ai.value->envelope == e.id) {
                SUPPLE_TRY(valid(ai.value->points, 0, 1));
                SUPPLE_TRY(valid(ai.value->points, ai.value->baseline, ai.value->amplitude));
            }
    }
    return {};
}
double groupParameterAt(const Session& s, Id object, std::string_view param, double seconds,
                        double fallback) {
    for (const auto& e : s.envelopes)
        if (e.value->active && e.value->target.object == object && e.value->target.param == param) {
            const double at =
                e.value->timebase == Timebase::Time ? seconds : s.timeMap->beatAt(seconds);
            fallback = pointsValueAt(effectivePoints(s, e.id), at, fallback);
        }
    return fallback;
}
EffectiveLevel effectiveTrackLevel(const Session& s, Id track, double seconds) {
    EffectiveLevel out;
    const auto* t = s.tracks.find(track);
    if (!t) return out;
    out.requestedDb = groupParameterAt(s, track, "volume", seconds, t->volumeDb);
    bool silence = out.requestedDb <= kSilenceDb;
    out.mute = groupParameterAt(s, track, "mute", seconds, t->mute ? 1 : 0) >= .5;
    const auto control = vcaControllers(s, track);
    for (Id id : control) {
        const auto& vca = *s.groups.find(id)->vca;
        const double db = groupParameterAt(s, id, "volume", seconds, vca.volumeDb);
        silence |= db <= kSilenceDb;
        out.requestedDb += db;
        out.mute |= groupParameterAt(s, id, "mute", seconds, vca.mute ? 1 : 0) >= .5;
    }
    if (silence) out.requestedDb = kSilenceDb;
    // Masters retain their existing plain stage when not under VCA control.
    const double ceiling =
        t->type == StripType::Master && control.empty() ? out.requestedDb : kFaderCeilingDb;
    out.appliedDb = std::clamp(out.requestedDb, kSilenceDb, std::max(kSilenceDb, ceiling));
    std::set<Id> soloed;
    for (const auto& e : s.tracks) {
        bool solo = e.value->solo == Solo::On || e.value->solo == Solo::InPlace;
        for (Id id : vcaControllers(s, e.id)) solo |= s.groups.find(id)->vca->solo;
        if (solo) soloed.insert(e.id);
    }
    std::set<Id> audible = soloed;
    for (bool forward : {false, true}) {
        std::vector<Id> pending(soloed.begin(), soloed.end());
        std::set<Id> visited;
        while (!pending.empty()) {
            const Id at = pending.back();
            pending.pop_back();
            if (!visited.insert(at).second) continue;
            auto edge = [&](Id from, Id to) {
                if ((forward ? from : to) == at) {
                    Id next = forward ? to : from;
                    audible.insert(next);
                    pending.push_back(next);
                }
            };
            for (const auto& e : s.tracks)
                if (e.value->parent && e.value->parentSend.enabled) edge(e.id, *e.value->parent);
            for (const auto& e : s.routes)
                if (e.value->from.track && e.value->to.track)
                    edge(*e.value->from.track, *e.value->to.track);
        }
    }
    out.soloSilenced = !soloed.empty() && !audible.contains(track) && t->solo != Solo::Defeat &&
                       t->type != StripType::Master;
    return out;
}
}  // namespace supple::model
