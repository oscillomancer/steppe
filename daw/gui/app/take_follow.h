// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A timeline item's take following the song (ADR-0098 amended, "following is
// playback"; gui.md §4.7 and §4.14): what the inspector's Follow and Bake,
// the Item menu's Follow entries and Bake into notes, and the roll's take
// row send. take.set takes a take's context as a lead sheet in bars of the
// meter at its item, so the GUI prints the points as that text and the
// engine reads, checks and keeps them.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/json/json.h"
#include "supple/model/objects.h"
#include "supple/model/session.h"

namespace supple::gui {

// The lead sheet take.set {context} reads for a take's `points` (context
// beats): bars of the meter at `item`'s start, bar 1 at context beat 0,
// through the take's period when it has one.
std::string takeLeadSheet(const model::Session& s, const model::Item& item, const model::Take& take,
                          const std::vector<model::ContextPoint>& points);

// The project's key and chords under `item`, as points of its heard take's
// context: the key and the chord that hold at the item's start, carried to
// it, then each point inside the item, at the context beat of its song beat
// (model::placeAt). Empty when the item's heard take is not MIDI.
std::vector<model::ContextPoint> songContextUnder(const model::Session& s, model::Id item);

// Why the heard take of `item` cannot be set to follow, in a few words for
// a menu or a dropdown ("needs a key in its context", "no key in the song
// here"), or nothing when it can. A take with no context of its own takes
// the song's under the item (below), so it needs a key there.
std::optional<std::string> followRefusal(const model::Session& s, model::Id item);

// The take.set arguments that make `item`'s heard take follow `follow`. A
// take with no context, set to Key or Chords, gets the song's context under
// the item in the same call (songContextUnder): nothing changes sound until
// the context track changes, and from then on the part moves with it.
Result<json::Value> takeFollowArgs(const model::Session& s, model::Id item,
                                   model::ClipFollow follow);

// The items of `items` whose heard take follows: those take.bake can bake.
std::vector<model::Id> bakeable(const model::Session& s, const std::vector<model::Id>& items);

// take.bake on the heard take of each item of `items` that follows, as one
// undo step; the status line's words (views::bakeNotice). Nothing to bake
// is an error that says so.
Result<std::string> bakeItems(cmd::Dispatcher& dispatcher, const cmd::Origin& origin,
                              const model::Session& before, const std::vector<model::Id>& items);

}  // namespace supple::gui
