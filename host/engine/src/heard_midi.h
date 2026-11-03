// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include "supple/model/session.h"

namespace supple::engine::detail {
// What `take` plays in `it` (as the engine schedules it: the source window,
// looped passes, a note held into the item's start), every note ending by the
// item's end, written into `out` (at out.ppq) `shift` beats later. Item beats:
// a take's playrate is applied. midi.merge and item.export use it.
void heardMidi(const model::Session& s, const model::Item& it, const model::Take& take,
               const midi::Sequence& seq, double shift, midi::Sequence& out);
}  // namespace supple::engine::detail
