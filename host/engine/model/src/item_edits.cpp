// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/item_edits.h"
#include <algorithm>
#include <set>
#include "supple/base/sha256.h"
#include "supple/model/audio_warp.h"
#include "supple/model/freeze.h"
#include "supple/model/groups.h"
#include "supple/model/schemas.h"
#include "supple/model/versions.h"

namespace supple::model {
Status completeItemSelection(const Session& s, std::span<const Id> selected) {
    const std::set<Id> ids(selected.begin(), selected.end());
    if (ids.empty())
        return Error("item.selection", "no items selected", "/items", "select items first");
    SUPPLE_TRY(validateAudioWarp(s));
    std::map<Id, Id> owners;
    for (const auto& entry : s.items)
        for (Id take : entry.value->takes) owners[take] = entry.id;
    for (Id id : ids) {
        const auto* item = s.items.find(id);
        if (!item)
            return Error("item.unknown", "item does not exist: " + id.str(), "/items",
                         "choose current items from track.list");
        if (auto frozen = frozenOwner(s, id)) return frozenError(s, *frozen, "/items");
        if (item->locked)
            return Error("item.locked", id.str() + " is locked", "/items",
                         "unlock it with item.set before editing");
        for (Id peer : linkedItems(s, id))
            if (!ids.contains(peer))
                return Error("item.selection", "selection omits a linked item", "/items",
                             "select all linked items or suspend their edit group");
        for (Id take : item->takes)
            if (const auto* value = s.takes.find(take))
                for (Id peer : value->warpPeers)
                    if (!owners.contains(peer) || !ids.contains(owners.at(peer)))
                        return Error("warp.peers", "selection omits a warp peer", "/items",
                                     "select every bundle member or use audio.warp.detach first");
    }
    return {};
}

Result<std::vector<ItemEditGroup>> consolidationGroups(const Session& s, std::span<const Id> ids) {
    SUPPLE_TRY(completeItemSelection(s, ids));
    std::map<std::pair<Id, bool>, std::vector<Id>> partitions;
    for (Id id : std::set<Id>(ids.begin(), ids.end())) {
        const auto& item = *s.items.find(id);
        const auto* take = item.activeTake ? s.takes.find(*item.activeTake) : nullptr;
        if (!take || !take->source ||
            (!s.media.contains(*take->source) && !s.midi.contains(*take->source)))
            return Error("item.consolidate_source", id.str() + " has no active material", "/items",
                         "activate an audio or MIDI take before consolidating");
        partitions[{item.track, take->source->kind == Kind::Media}].push_back(id);
    }
    std::vector<ItemEditGroup> out;
    bool changes = false;
    for (auto& [key, items] : partitions) {
        std::sort(items.begin(), items.end(), [&](Id a, Id b) {
            const double x = itemSpan(s, *s.items.find(a)).startSec;
            const double y = itemSpan(s, *s.items.find(b)).startSec;
            return x == y ? a < b : x < y;
        });
        const auto& first = *s.items.find(items.front());
        ItemEditGroup group{key.first, key.second, items, itemSpan(s, first).startSec,
                            itemSpan(s, first).endSec};
        for (Id id : items) {
            const auto& item = *s.items.find(id);
            if (item.lane != first.lane || item.group != first.group)
                return Error("item.consolidate_layout", "items have different lanes or groups",
                             "/items", "select one lane and one item-group token per track/kind");
            group.end = std::max(group.end, itemSpan(s, item).endSec);
        }
        if (!group.audio && items.size() == 1) {
            const auto& only = *s.items.find(items.front());
            group.layerTakes = only.takes.size() > 1 &&
                               std::all_of(only.takes.begin(), only.takes.end(), [&](Id t) {
                                   const auto* take = s.takes.find(t);
                                   return take && take->source && s.midi.contains(*take->source);
                               });
        }
        changes = changes || group.audio || items.size() > 1 || group.layerTakes;
        out.push_back(std::move(group));
    }
    if (!changes)
        return Error(
            "item.consolidate_empty", "there are no items to combine", "/items",
            "select audio, several MIDI items on a track, or a MIDI item with several takes");
    return out;
}

std::string itemEditFingerprint(const Session& s, std::span<const Id> selected) {
    std::set<Id> ids(selected.begin(), selected.end());
    for (Id id : selected)
        if (const auto* item = s.items.find(id)) {
            ids.insert(item->track);
            for (Id take : item->takes) {
                ids.insert(take);
                if (const auto* value = s.takes.find(take)) {
                    if (value->source) ids.insert(*value->source);
                    ids.insert(value->chain.begin(), value->chain.end());
                }
            }
        }
    for (const auto& entry : s.envelopes)
        if (ids.contains(entry.value->target.object) &&
            entry.value->target.object.kind != Kind::Track)
            ids.insert(entry.id);
    for (const auto& entry : s.autoItems)
        if (ids.contains(entry.value->envelope)) ids.insert(entry.id);
    std::map<Id, Id> remap;
    Counters counters;
    for (Id id : ids) remap[id] = Id{id.kind, ++counters[id.kind]};
    auto value = json::Value::array();
    auto project = toJson(*s.project);
    for (const char* key : {"sample_rate", "timebase", "seed", "media_dir"})
        value.push_back(project[key]);
    for (const auto& entry : s.tempo) value.push_back(toJson(*entry.value));
    for (const auto& entry : s.meters) value.push_back(toJson(*entry.value));
    for (const auto& entry : s.tracks)
        if (ids.contains(entry.id)) {
            value.push_back(remap.at(entry.id).str());
            value.push_back(trackTimebaseSchema().encode(entry.value->timebase));
        }
    auto append = [&](const auto& map) {
        for (const auto& entry : map)
            if (ids.contains(entry.id)) {
                using T = std::remove_cvref_t<decltype(*entry.value)>;
                auto object = toJson(*entry.value);
                if constexpr (std::is_same_v<T, MediaSource>)
                    if (!entry.value->hash.empty()) object.asObject().erase("path");
                value.push_back(remap.at(entry.id).str());
                value.push_back(remapIds(object, schemaOf<T>().jsonSchema(), remap));
            }
    };
    append(s.items);
    append(s.takes);
    append(s.fx);
    append(s.media);
    append(s.midi);
    append(s.envelopes);
    append(s.autoItems);
    return "sha256:" + sha256Hex(json::writeCompact(value));
}
}  // namespace supple::model
