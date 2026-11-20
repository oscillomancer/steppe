// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// What key and chord a MIDI note sits in (ADR-0095), for midi.edit's transpose_degrees,
// snap_to_scale and fit_to_chord: read from the context track at the project beat the
// note sounds at, or given explicitly.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/harmony/harmony.h"
#include "supple/model/session.h"

namespace supple::engine::detail {

// The project beats at which source tick `tick` of the MIDI source starts to sound:
// once per item whose heard take reads the source (pooled sharers included), and once
// per loop pass of a looping item. Notes that start before an item or after its end are
// not heard there and give no beat.
std::vector<double> soundingBeats(const model::Session& s, model::Id source, std::int64_t tick);

// "Eb major", "F# harmonic_minor" (spaces for underscores are accepted): the key
// arguments of the scale ops. Errors `key.tonic`, `scale.unknown`, `key.unsupported`,
// with `path` set to `path`.
Result<harmony::Key> parseKeyText(std::string_view text, const std::string& path);

struct NoteContext {
    std::optional<harmony::Key> key;
    std::optional<harmony::Chord> chord;
};

// The key and/or chord a note is in. `fallbackBeat` stands in when the note sounds
// nowhere (outside every item). Refuses with `midi.context_varies` when the beats it
// sounds at do not agree on a context part that is asked for, `midi.no_key` or
// `midi.no_chord` when none holds (or the point is one this build does not read).
Result<NoteContext> noteContext(const model::Session& s, model::Id source, std::int64_t tick,
                                double fallbackBeat, bool needKey, bool needChord,
                                const std::string& path);

}  // namespace supple::engine::detail
