// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <vector>

#include "supple/model/session.h"

namespace supple::engine::detail {
// What `take` plays in `it` (as the engine schedules it: the source window,
// looped passes, a note held into the item's start), every note ending by the
// item's end, written into `out` (at out.ppq) `shift` beats later. Item beats:
// a take's playrate is applied. mergeMidi and item.export use it.
void heardMidi(const model::Session& s, const model::Item& it, const model::Take& take,
               const midi::Sequence& seq, double shift, midi::Sequence& out);
// item.consolidate's MIDI step, in `txn`: several MIDI items on one track
// become one item from the first start to the last end, with what each item's
// heard take plays (gaps empty, overlaps layered); one item's MIDI takes layer
// into one take. The earliest item keeps its id, settings, take name and take
// FX; the others are removed.
Status mergeMidi(const model::Session& s, model::EditTxn& txn, const std::vector<model::Id>& ids);
}  // namespace supple::engine::detail
