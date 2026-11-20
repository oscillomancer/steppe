// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/take_owner.h"

#include <algorithm>

namespace supple::model {

namespace {
bool holds(const std::vector<Id>& takes, Id take) {
    return std::find(takes.begin(), takes.end(), take) != takes.end();
}
}  // namespace

std::optional<Id> takeOwner(const Session& s, Id take) {
    for (const auto& e : s.items)
        if (holds(e.value->takes, take)) return e.id;
    for (const auto& e : s.clips)
        if (holds(e.value->takes, take)) return e.id;
    return std::nullopt;
}

const std::vector<Id>* ownerTakes(const Session& s, Id owner) {
    if (const Item* item = s.items.find(owner)) return &item->takes;
    if (const Clip* clip = s.clips.find(owner)) return &clip->takes;
    return nullptr;
}

TakeOwners::TakeOwners(const Session& s) {
    // Items first, then clips, each in id order: the first claim wins.
    for (const auto& e : s.items)
        for (Id take : e.value->takes) owners_.emplace(take, e.id);
    for (const auto& e : s.clips)
        for (Id take : e.value->takes) owners_.emplace(take, e.id);
}

std::optional<Id> TakeOwners::of(Id take) const {
    const auto found = owners_.find(take);
    if (found == owners_.end()) return std::nullopt;
    return found->second;
}

}  // namespace supple::model
