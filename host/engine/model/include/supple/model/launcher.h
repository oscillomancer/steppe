// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's model rules (ADR-0097, session-model "Scenes and
// clips"): the invariants scenes and clips keep beyond their schemas, and
// the load rule for files that went through an older build (amendment G-c).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/session.h"

namespace supple::model {

// The file's and the commands' names for each quantize value.
inline constexpr std::array<std::pair<LaunchQuantize, std::string_view>, 8> kLaunchQuantizeNames{{
    {LaunchQuantize::Bar, "bar"},
    {LaunchQuantize::Beat, "beat"},
    {LaunchQuantize::Half, "1/2"},
    {LaunchQuantize::Quarter, "1/4"},
    {LaunchQuantize::Eighth, "1/8"},
    {LaunchQuantize::Sixteenth, "1/16"},
    {LaunchQuantize::ThirtySecond, "1/32"},
    {LaunchQuantize::None, "none"},
}};
std::string_view launchQuantizeName(LaunchQuantize q);

// Appends a model.invariant error for each fault in the scenes and clips:
// counters, limits, distinct scene orders, a clip's channel and scene, one
// clip per (track, scene), its length and loop, and its takes (each held by
// it alone, each source matching its kind, the active one among them).
// checkInvariants calls it; the orphan-take rule itself is checkInvariants'
// (via TakeOwners).
void checkLauncher(const Session& s, std::vector<Error>& errors);

// A file that went through an older build (G-c): drops each clip that
// refers to something gone (its track, its scene, a take, its active take),
// with its takes that nothing else holds (their take FX, the envelopes on
// them and those envelopes' automation items). Any other fault is left for
// the invariants. Returns each dropped clip and why, in id order.
// (A clip's context is made fit by fitClipContext, which the loader calls.)
std::vector<std::pair<Id, std::string>> dropStrayClips(Session& s);

// A clip's context made fit to keep (ADR-0098, session-model "Clip context"):
// each point's beat rounded to the 1/960 grid, the points sorted by beat then
// type (key first), and what breaks a rule dropped: a MIDI clip's points lie in
// [0, length), one of a type per beat, at most kMaxContextPoints of a type; an
// audio clip holds one key at 0. Then `follow` goes back to none when the clip
// is audio or has no readable key at 0. Each change is one note, with the codes
// `clip.context_dropped` and `clip.follow_reset`; callers turn them into
// warnings. Idempotent.
struct ClipContextNote {
    std::string code, message;
};
std::vector<ClipContextNote> fitClipContext(Clip& clip);

// Whether `follow` other than none is allowed on the clip as it is: a MIDI clip
// with a readable key at beat 0.
bool clipCanFollow(const Clip& clip);

// The clip in (track, scene), if any.
std::optional<Id> clipAt(const Session& s, Id track, Id scene);

// The clip kind a take's source makes: MIDI for a MIDI source, audio for
// media, nullopt for no source (which fits either).
std::optional<ClipKind> clipKindOf(const Take& take);
std::string_view clipKindName(ClipKind k);  // "midi" | "audio"

}  // namespace supple::model
