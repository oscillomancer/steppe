// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/model/session.h"

namespace supple::model {
// Main-thread dependency closure, independent of revisions and mixer controls.
std::string freezeDependencyHash(const Session&, Id track);
std::vector<Id> freezeFxIds(const Session&, Id track);
// Includes input/take FX, pooled sources, envelopes and automation items.
std::optional<Id> frozenOwner(const Session&, Id object);
// The frozen channel whose racks hold `object` (a module, cable, plugin
// module's plug-in, or an envelope or automation item on them; ADR-0075).
// The freeze locks them (their edits fail with track.frozen) but does not
// own them: the rack keeps running for its rack sends.
std::optional<Id> frozenRackHome(const Session&, Id object);
Error frozenError(const Session&, Id track, std::string path = {});
Status validateFreezeInput(const Session&, Id track);
Status validateFrozenState(const Session&);
Status validateFrozenTransition(const Session& before, const Session& after);
}  // namespace supple::model
