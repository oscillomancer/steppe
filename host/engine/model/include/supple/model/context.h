// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The context track's index and queries (ADR-0095, session-model "Context track"):
// what key, chord and section hold at a beat. The points are objects in
// Session::context; the view below is derived from them and rebuilt only when
// that collection's storage changes.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "supple/harmony/harmony.h"
#include "supple/model/idmap.h"
#include "supple/model/objects.h"

namespace supple::model {

struct Session;

// The points of each type by beat. `source` is the collection the index was
// built from: a Session whose `context` shares its storage reuses the view.
struct ContextView {
    struct Entry {
        double beat = 0.0;
        Id id;
    };
    IdMap<ContextPoint> source;
    std::vector<Entry> keys, chords;  // sorted by beat; one per beat per type
};

// Sorts `points` into a view. Two points of one type at one beat (a bad file
// the loader repairs): the lowest id.
ContextView buildContextView(const IdMap<ContextPoint>& points);

// The point of the type that holds at `beat`: the last at or before it.
std::optional<Id> keyPointAt(const Session& s, double beat);
std::optional<Id> chordPointAt(const Session& s, double beat);

// What the point says, when the strings parse: a key whose tonic and scale are
// known, a chord the grammar takes (an empty symbol is "no chord"). An unknown
// scale or symbol answers nothing; the point is still there to read.
std::optional<harmony::Key> parseKey(const ContextPoint& point);
std::optional<harmony::Chord> parseChordPoint(const ContextPoint& point);

// "Eb harmonic minor": the key point's words for people and agents, as written (with a
// trailing "?") when this build does not read its scale or tonic.
std::string keyWords(const ContextPoint& point);

std::optional<harmony::Key> keyAt(const Session& s, double beat);
std::optional<harmony::Chord> chordAt(const Session& s, double beat);

// A clip's own context (ADR-0098): the point of the type that holds at clip beat
// `beat` (the last at or before it), null before the first. Points are sorted by
// beat then type, as the clip keeps them. keyAt and chordAt read what it says,
// as the session's do: an unreadable point holds and answers nothing.
const ContextPoint* keyPointAt(const Clip& clip, double beat);
const ContextPoint* chordPointAt(const Clip& clip, double beat);
std::optional<harmony::Key> keyAt(const Clip& clip, double beat);
std::optional<harmony::Chord> chordAt(const Clip& clip, double beat);

// The section at `beat`: of the regions with `section` true that cover it
// (start <= beat < end, in beats through the time map), the one that starts
// latest; ties go to the shorter, then the lower id. None for a gap.
std::optional<Id> sectionAt(const Session& s, double beat);

}  // namespace supple::model
