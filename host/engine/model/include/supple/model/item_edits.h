// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <span>
#include "supple/model/session.h"

namespace supple::model {
struct ItemEditGroup {
    Id track;
    bool audio = false;
    std::vector<Id> items;
    double start = 0, end = 0;
    // MIDI: one item whose takes (all MIDI, two or more) layer into one take.
    bool layerTakes = false;
};
// Read-only command/menu planning. Explicit selection must include affected peers.
Status completeItemSelection(const Session&, std::span<const Id>);
Result<std::vector<ItemEditGroup>> consolidationGroups(const Session&, std::span<const Id>);
// Selected objects, owned material/automation and source timing; no mixer controls.
std::string itemEditFingerprint(const Session&, std::span<const Id>);
struct HealGroup {
    std::vector<Id> items;  // first ID survives; other items and their owned takes go
    Item item;
    std::vector<std::pair<Id, Envelope>> envelopes;
};
// Restore continuous source spans; no render, source I/O or plugin instance access.
Result<std::vector<HealGroup>> healingGroups(const Session&, std::span<const Id>);
}  // namespace supple::model
