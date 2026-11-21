// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// What a clip knows of the harmony it was written against (ADR-0098, session-model
// "Clip context"): its lead sheet text in clip bars, the project's context taken under
// a span, the chord points a notation's symbols make, and the precedence between them.
#include <optional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/session.h"
#include "supple/notation/notation.h"

namespace supple::engine::detail {

// A warning a command gives: its code, message and path.
struct ClipNote {
    std::string code, message, path;
};

// The clip's context as the lead sheet text, in clip bars: bar 1 is the clip's start,
// bars run in the project's meters and cover the clip's length (an audio clip: bar 1).
// "" when the clip has no context. Canonical: reading it back gives the same points.
std::string clipContextText(const model::Session& s, const model::Clip& clip);

// The points `text` states for a clip of `kind` and `length` (beats for MIDI, seconds for
// audio), read against the session's meters. "" is no context. Errors:
// `clip.context_syntax` and the lead sheet's own, `clip.context_range` (a point at or past
// the end of a MIDI clip), `clip.context_audio` (a chord, or a key not at the start, on an
// audio clip), `context.limit`; each names the clip bar. `path` is the argument's.
Result<std::vector<model::ContextPoint>> clipContextFromText(const model::Session& s,
                                                             const std::string& text,
                                                             model::ClipKind kind, double length,
                                                             const std::string& path);

// The project's context read from `fromBeat` for `lengthBeats`: the key and chord holding
// there become points at 0 (a "no chord" holding adds none), and the points inside the
// span follow, shifted. `keyOnly` (an audio clip) gives the key at 0 and nothing more.
struct ContextTaken {
    std::vector<model::ContextPoint> points;
    bool key = false;  // a key holds at fromBeat (whether or not this build reads it)
};
ContextTaken contextUnder(const model::Session& s, double fromBeat, double lengthBeats,
                          bool keyOnly);

// The chord points a notation's symbols make: each at its clip beat ((tick / ppq - offset) /
// rate), by tick and voice order; equal neighbours collapse; of two different symbols at one
// tick the earlier-declared voice wins; at or past `length` none is recorded. The notes are
// `clip.chords_beyond_length` and `clip.chord_conflict`.
struct SymbolChords {
    std::vector<model::ContextPoint> points;
    std::vector<ClipNote> notes;
};
SymbolChords chordsFromSymbols(const model::Session& s,
                               const std::vector<notation::ChordSymbol>& symbols, int ppq,
                               double offset, double rate, double length);

// A stated context (text, or taken from a place) and the symbols of a notation, by
// precedence: a stated context holding chords wins, and the symbols are ignored
// (`clip.chords_ignored`); otherwise the stated points and the symbols' chords. Nothing
// stated: the symbols' chords alone. Sorted by beat then type.
std::vector<model::ContextPoint> composeContext(
    std::optional<std::vector<model::ContextPoint>> stated, SymbolChords symbols,
    std::vector<ClipNote>& notes);

// The key a clip's points say at beat 0 (a readable one), for describe and the checks.
std::optional<std::string> clipKeyWords(const model::Clip& clip);

}  // namespace supple::engine::detail
