// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <cmath>
#include <set>
#include "supple/model/audio_warp.h"
#include "supple/model/groups.h"
#include "supple/model/item_edits.h"
#include "supple/model/schemas.h"

namespace supple::model {
namespace {
Error incompatible(std::string reason, std::string code = "item.heal_incompatible") {
    return Error(
        std::move(code), std::move(reason), "/items",
        "select unchanged consecutive audio pieces and all linked peers, or consolidate instead");
}
std::map<std::string, Id> envelopes(const Session& s, Id object) {
    std::map<std::string, Id> out;
    for (const auto& e : s.envelopes)
        if (e.value->target.object == object) out[e.value->target.param] = e.id;
    return out;
}
json::Value automation(const Session& s, Id env, Id target, double shift) {
    auto out = json::Value::array();
    for (const auto& entry : s.autoItems)
        if (entry.value->envelope == env) {
            auto item = *entry.value;
            item.envelope = target;
            item.position += shift;
            out.push_back(toJson(item));
        }
    return out;
}
bool sameFields(Envelope a, Envelope b) {
    a.target.object = b.target.object;
    a.points.clear();
    b.points.clear();
    return toJson(a) == toJson(b);
}
// copyTake clips take-relative curves but leaves FX curves and automation
// items complete. Only linear/constant seam segments can be recovered without
// guessing whether an inserted nonlinear boundary knot was originally authored.
Status joinEnvelopes(const Session& s, Id first, Id next, double shift, bool relative,
                     std::map<Id, Envelope>& recovered) {
    const auto a = envelopes(s, first), b = envelopes(s, next);
    if (a.size() != b.size()) return incompatible("take/FX envelope sets differ");
    for (const auto& [param, id] : a) {
        if (!b.contains(param)) return incompatible("take/FX envelope parameters differ");
        const Id other = b.at(param);
        const auto& right = *s.envelopes.find(other);
        if (!recovered.contains(id)) recovered[id] = *s.envelopes.find(id);
        auto& left = recovered.at(id);
        if (!sameFields(left, right) ||
            automation(s, id, id, relative ? -shift : 0) != automation(s, other, id, 0))
            return incompatible("take/FX automation differs", "item.heal_envelope");
        if (!relative) {
            const auto l = toJson(left), r = toJson(right);
            const auto* lp = l.find("points");
            const auto* rp = r.find("points");
            if ((lp != nullptr) != (rp != nullptr) || (lp && *lp != *rp))
                return incompatible("stored FX envelope curves differ", "item.heal_fx");
            continue;
        }
        if (left.points.empty() && right.points.empty()) continue;
        if (left.points.empty() || right.points.empty() || right.points.front().pos != 0 ||
            (left.points.back().pos != shift && left.points.size() != 1) ||
            left.points.back().value != right.points.front().value)
            return incompatible("take envelope seam is not continuous", "item.heal_envelope");
        const auto segment =
            left.points.size() > 1 ? left.points[left.points.size() - 2] : left.points.back();
        const bool constant =
            std::all_of(left.points.begin(), left.points.end(),
                        [&](const auto& p) { return p.value == left.points.back().value; }) &&
            std::all_of(right.points.begin(), right.points.end(),
                        [&](const auto& p) { return p.value == left.points.back().value; });
        if (!constant && (segment.shape != PointShape::Linear ||
                          right.points.front().shape != PointShape::Linear))
            return incompatible("original nonlinear envelope seam cannot be recovered",
                                "item.heal_envelope");
        if (constant) {
            for (std::size_t i = 1; i < right.points.size(); ++i) {
                auto point = right.points[i];
                point.pos += shift;
                left.points.push_back(point);
            }
            continue;
        }
        auto joined = left.points;
        for (std::size_t i = 1; i < right.points.size(); ++i) {
            auto point = right.points[i];
            point.pos += shift;
            joined.push_back(point);
        }
        const auto seam = left.points.size() - 1;
        if (seam > 0 && seam + 1 < joined.size()) {
            const auto& before = joined[seam - 1];
            const auto& after = joined[seam + 1];
            const double value =
                before.value +
                (after.value - before.value) * ((shift - before.pos) / (after.pos - before.pos));
            if (value == joined[seam].value)
                joined.erase(joined.begin() + static_cast<std::ptrdiff_t>(seam));
        }
        left.points = std::move(joined);
    }
    return {};
}
Status sameTake(const Session& s, const Take& first, const Take& previous, const Take& current,
                double delta, double rightStart) {
    if (!first.source || first.source != current.source || first.source->kind != Kind::Media ||
        !s.media.contains(*first.source))
        return incompatible("corresponding takes need the same audio source");
    if (!current.warpPeers.empty())
        return incompatible("right pieces belong to a different warp bundle");
    auto expected = previous;
    rebaseAudioTake(expected, delta);
    const double speed = audioSpeed(current);
    const double rate = s.project->sampleRate;
    if (time::samplesAt(expected.startOffset, rate / speed) !=
            time::samplesAt(current.startOffset, rate / speed) ||
        expected.stretchMarkers.size() != current.stretchMarkers.size())
        return incompatible("source offsets or stretch maps are not contiguous");
    for (std::size_t i = 0; i < expected.stretchMarkers.size(); ++i) {
        const auto& a = expected.stretchMarkers[i];
        const auto& b = current.stretchMarkers[i];
        if (a.sourcePos != b.sourcePos || a.slope != b.slope ||
            time::samplesAt(a.itemPos + rightStart, rate) !=
                time::samplesAt(b.itemPos + rightStart, rate))
            return incompatible("stretch maps differ across the seam");
    }
    auto a = toJson(first), b = toJson(current);
    for (const char* key : {"start_offset", "stretch_markers", "warp_peers", "chain"}) {
        a.asObject().erase(key);
        b.asObject().erase(key);
    }
    if (a != b) return incompatible("stored take properties differ");
    if (first.chain.size() != current.chain.size())
        return incompatible("take FX chains differ", "item.heal_fx");
    for (std::size_t i = 0; i < first.chain.size(); ++i)
        if (toJson(*s.fx.find(first.chain[i])) != toJson(*s.fx.find(current.chain[i])))
            return incompatible("stored take FX states or settings differ", "item.heal_fx");
    return {};
}
}  // namespace

Result<std::vector<HealGroup>> healingGroups(const Session& s, std::span<const Id> selected) {
    SUPPLE_TRY(completeItemSelection(s, selected));
    std::map<Id, std::vector<Id>> tracks;
    for (Id id : std::set<Id>(selected.begin(), selected.end()))
        tracks[s.items.find(id)->track].push_back(id);
    std::vector<HealGroup> out;
    for (auto& [track, ids] : tracks) {
        (void)track;
        if (ids.size() < 2) return incompatible("each selected track needs at least two pieces");
        std::sort(ids.begin(), ids.end(), [&](Id a, Id b) {
            const auto x = itemSpan(s, *s.items.find(a)).startSec,
                       y = itemSpan(s, *s.items.find(b)).startSec;
            return x == y ? a < b : x < y;
        });
        const auto& first = *s.items.find(ids.front());
        if (first.takes.empty() || !first.activeTake) return incompatible("items need audio takes");
        const auto active = std::find(first.takes.begin(), first.takes.end(), *first.activeTake) -
                            first.takes.begin();
        for (Id id : first.takes)
            if (const auto* t = s.takes.find(id); !t || !t->source || !s.media.contains(*t->source))
                return incompatible("all corresponding takes must be audio");
        std::map<Id, Envelope> recovered;
        const auto firstSpan = itemSpan(s, first);
        for (std::size_t i = 1; i < ids.size(); ++i) {
            const auto& previous = *s.items.find(ids[i - 1]);
            const auto& item = *s.items.find(ids[i]);
            const auto left = itemSpan(s, previous), right = itemSpan(s, item);
            const double rate = s.project->sampleRate;
            const auto seam = time::samplesAt(left.endSec, rate),
                       start = time::samplesAt(right.startSec, rate);
            if (start > seam || right.endSec <= left.endSec)
                return incompatible("pieces have a gap, are nested or were moved");
            if (start < seam) {
                const double overlap = left.endSec - right.startSec;
                const double nominalStart =
                    i > 1 ? itemSpan(s, *s.items.find(ids[i - 2])).endSec : left.startSec;
                const auto fade = [&](const Fade& value) {
                    return value.shape == FadeShape::Linear && value.curve == 0 &&
                           time::samplesAt(value.length, rate) == seam - start;
                };
                if (overlap > .05 + 1 / rate || !fade(previous.fadeOut) || !fade(item.fadeIn) ||
                    overlap > (left.endSec - nominalStart) / 2 + 1 / rate)
                    return incompatible("overlap is not an unchanged Slice-at-hits crossfade");
            } else if (previous.fadeOut.length != 0 || item.fadeIn.length != 0) {
                return incompatible("touching pieces have authored interior fades");
            }
            if (item.lane != first.lane || item.timebase != first.timebase ||
                item.loop != first.loop || item.mute != first.mute || item.gainDb != first.gainDb ||
                item.group != first.group || item.takes.size() != first.takes.size() ||
                !item.activeTake ||
                item.takes[static_cast<std::size_t>(active)] != *item.activeTake)
                return incompatible("item lanes, groups, controls or take layouts differ");
            const double shift = s.timeMap->beatAt(left.endSec) - firstSpan.startBeat;
            for (std::size_t t = 0; t < first.takes.size(); ++t) {
                const auto& base = *s.takes.find(first.takes[t]);
                const auto& prior = *s.takes.find(previous.takes[t]);
                const auto& next = *s.takes.find(item.takes[t]);
                SUPPLE_TRY(
                    sameTake(s, base, prior, next, right.startSec - left.startSec, right.startSec));
                SUPPLE_TRY(joinEnvelopes(s, first.takes[t], item.takes[t], shift, true, recovered));
                for (std::size_t f = 0; f < base.chain.size(); ++f)
                    SUPPLE_TRY(joinEnvelopes(s, base.chain[f], next.chain[f], 0, false, recovered));
            }
        }
        HealGroup group{ids, first, {}};
        group.item.fadeOut = s.items.find(ids.back())->fadeOut;
        placeItemSeconds(s, group.item, firstSpan.startSec,
                         itemSpan(s, *s.items.find(ids.back())).endSec);
        for (auto& [id, env] : recovered) group.envelopes.emplace_back(id, std::move(env));
        out.push_back(std::move(group));
    }
    Session candidate = s;
    IdMap<Item>::Builder items(s.items);
    for (const auto& group : out) {
        items.put(group.items.front(), 0, std::make_shared<const Item>(group.item));
        for (std::size_t i = 1; i < group.items.size(); ++i) items.erase(group.items[i]);
    }
    candidate.items = items.build();
    SUPPLE_TRY(validateAudioWarp(candidate));
    SUPPLE_TRY(validateGroups(candidate));
    return out;
}
}  // namespace supple::model
