// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/take_owner.h"

#include <algorithm>

namespace supple::model {

std::optional<Id> takeOwner(const Session& s, Id take) {
    for (const auto& e : s.items)
        if (std::find(e.value->takes.begin(), e.value->takes.end(), take) != e.value->takes.end())
            return e.id;
    return std::nullopt;
}

const std::vector<Id>* ownerTakes(const Session& s, Id owner) {
    if (const Item* item = s.items.find(owner)) return &item->takes;
    return nullptr;
}

TakeOwners::TakeOwners(const Session& s) {
    for (const auto& e : s.items)
        for (Id take : e.value->takes) owners_.emplace(take, e.id);  // the first in id order
}

std::optional<Id> TakeOwners::of(Id take) const {
    const auto found = owners_.find(take);
    if (found == owners_.end()) return std::nullopt;
    return found->second;
}

}  // namespace supple::model
