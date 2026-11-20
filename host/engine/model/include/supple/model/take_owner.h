// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Who holds a take (ADR-0097, "The take owner"). Every take belongs to
// exactly one owner, an item or a clip. Code that asks which owner holds a
// take, or what an owner holds, goes through here rather than scanning
// `items` or `clips`.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "supple/model/session.h"

namespace supple::model {

// The owner of `take` (an it_* or cl_* id), or nullopt when nothing holds
// it. A broken session that lists the take under several owners gives the
// first: items before clips, each in id order.
std::optional<Id> takeOwner(const Session& s, Id take);

// The takes `owner` holds, in its order; nullptr when `owner` is not an
// existing owner.
const std::vector<Id>* ownerTakes(const Session& s, Id owner);

// Every take's owner, found in one pass, for code that asks about many takes.
// It answers as takeOwner does.
class TakeOwners {
  public:
    explicit TakeOwners(const Session& s);
    std::optional<Id> of(Id take) const;
    std::size_t size() const { return owners_.size(); }

  private:
    std::map<Id, Id> owners_;
};

}  // namespace supple::model
