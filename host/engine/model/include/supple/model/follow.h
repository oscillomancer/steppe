// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A take's context and follow (ADR-0098 "Amendment: following is playback",
// session-model "Take context and follow"): what a note's place is, and what a note
// sounds as. The engine's compile, `take.bake`, `midi.get {sounding}`, `item.export`,
// placement's answer and the GUI's ghosts all take both from here, so they agree by
// construction. This is the model's view of `harmony::follow`, which stays free of the
// model.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "supple/harmony/harmony.h"
#include "supple/model/context.h"
#include "supple/model/launcher.h"
#include "supple/model/objects.h"
#include "supple/model/session.h"

namespace supple::model {

// How near below a point a beat still reads the point: float noise at a boundary (a note
// at a point's beat computed through a sum), a millionth of a beat, as the engine's tables.
inline constexpr double kPlaceSlack = 1e-6;

// --- reading a take's context -----------------------------------------------------------

// The point of the type that holds at context beat `contextBeat`: the last at or before it,
// and before the first point of the type, that first point (a take's analogue of a clip's
// key at 0, so a left trim or a pickup never leaves it without a key). `points` are sorted
// by beat then type, as a take keeps them. Null when the take has none of the type. An
// unreadable point holds and says nothing (read it with parseKey / parseChordPoint).
const ContextPoint* contextKeyPointAt(std::span<const ContextPoint> points, double contextBeat);
const ContextPoint* contextChordPointAt(std::span<const ContextPoint> points, double contextBeat);

// The key and chord that hold at a take's context beat, or at a song beat of the project.
// `chords` false leaves the chord out (a key follow reads none). A part that is absent or
// that this build cannot read is absent from the place.
harmony::Place takePlaceAt(std::span<const ContextPoint> points, double contextBeat, bool chords);
harmony::Place projectPlaceAt(const ContextView& project, double songBeat, bool chords);

// --- the fields and their rules -----------------------------------------------------------

// Whether the take may hold the four members at all: its source is MIDI (a take with no
// source or an audio source holds none).
bool takeIsMidi(const Take& take);
// Whether `follow` other than none is allowed on the take as it is: a MIDI take whose first key
// point is one this build reads (that key holds before the context begins).
bool takeCanFollow(const Take& take);

// A take's home: an item holds its context, a clip's takes hold none of the four (the clip's
// own context and follow are the clip's).
enum class TakeHome { Item, Clip };

// A take's context made fit to keep (the same rules `checkTakeContext` states): each point's
// beat rounded to the 1/960 grid, the points sorted by beat then type (key first), what breaks a
// rule dropped (a beat before 0 or not finite, at or past a context_period, a second point of a
// type at a beat, past kMaxContextPoints of a type), a context_period that is not finite and
// positive dropped, a context_phase that is not finite read as 0; then `follow` goes back to
// none when the take cannot follow. A take that is not MIDI, or that a clip holds, loses all
// four. Each change is one note: `take.context_dropped` or `take.follow_reset`. Idempotent.
// The loader and every command that edits a take's context call it.
std::vector<ClipContextNote> fitTakeContext(Take& take, TakeHome home);

// Appends a model.invariant error for each fault in the takes' context members (the rules
// above, at commit); checkInvariants calls it.
void checkTakeContext(const Session& s, std::vector<Error>& errors);

// A take laid from a clip (placement, `launch.commit`, `take.extract` from a clip) gets the
// clip's context in the take's own beats (session-model "Converting between a clip and a take"):
// each point's beat times the take's playrate `r`, `context_period` the clip's length times
// `r`, `context_phase` = `c0 * r - start_offset` where `c0` is the clip beat at the item's start
// (0 for placement; for `launch.commit`, where the laid stretch begins in the clip's period),
// and `follow` as given. A note heard `d` beats after the item's start then reads context beat
// `(c0 + d) * r` modulo the period: its clip beat, as the launcher reads it. A clip with no
// context leaves the take as it is. The take is made fit afterwards (`fitTakeContext`), so
// follow other than none needs a key the take reads.
void takeContextFromClip(Take& take, const Clip& clip, double clipBeatAtStart, ClipFollow follow);

// --- a note's place -----------------------------------------------------------------------

// The context beat of the **unwrapped source beat** `u0`, the note's start in the source with
// no modulo by the source or the loop length: `start_offset` plus the song-time distance from
// the item's start times the playrate. The context is anchored to the source: it travels with
// the notes, so a move, a trim, a split, a loop change, a slip and a playrate change leave a
// note's context beat as it was. It is `context_phase + u0`, modulo `context_period` when
// there is one (a beat within kPlaceSlack below the period reads as 0).
double contextBeatOf(const Take& take, double unwrappedSourceBeat);

// A note start's place: its context beat, and its song beat, where it sounds or would have
// sounded: `itemStartBeat + (u0 - start_offset) / playrate`. The one function that turns a
// note into a place; `soundingNotes` and every other reader call it.
struct NotePlace {
    double contextBeat = 0.0;
    double songBeat = 0.0;
};
NotePlace notePlace(const Take& take, double itemStartBeat, double unwrappedSourceBeat);

// The place a note that does not exist yet would have if it started at `songBeat` in an item's
// take (the roll's pencil and drag previews): the inverse of `notePlace`, so a preview never
// re-derives the context beat. `sourceBeat` is the unwrapped source beat the song beat falls at
// (`start_offset + (songBeat - item start) * playrate`); both sides' places are read, with
// chords, whatever the take's follow (a key follow ignores the chord parts). For a looped item
// the song beat maps to the pass it falls in, as the engine plays it. A song beat before the
// item's start is a place too (a note chased or a pickup). Null for an item without a MIDI take.
struct PlaceReading {
    double songBeat = 0.0;
    double sourceBeat = 0.0;
    double contextBeat = 0.0;
    harmony::Place take;     // the take's key and chord at contextBeat
    harmony::Place project;  // the project's at songBeat
    ClipFollow follow = ClipFollow::None;
};
PlaceReading placeAt(const Session& s, const Item& item, const Take& take, double songBeat);
// The active take of `item`; or the named `take` of it. Null when the item or take is missing, or
// the take is not MIDI.
std::optional<PlaceReading> placeAt(const Session& s, Id item, double songBeat);
std::optional<PlaceReading> placeAt(const Session& s, Id item, Id take, double songBeat);
// What a note written as `key` on `channel` sounds as at that place: as `soundingKey`, from the
// places already read. A take that does not follow, and channel 10, give the note as written.
harmony::Followed soundsAt(const PlaceReading& place, int channel, int key);

// Stores the take's `start_offset` inside one source period, [0, sourceBeats), the amount
// removed (a whole number of periods) added to `context_phase`, so no note's context beat
// moves. A command that stores a looped item's slip wrapped (a split's right part) calls this;
// the engine reads an offset modulo the period when the item loops, so the sound is the same.
// Does nothing for a source with no length.
void wrapStartOffset(Take& take, double sourceBeats);

// --- what a note sounds as ----------------------------------------------------------------

// One heard note occurrence of an item's active take: as the engine's compile plays it
// (the item's passes, a note whose start lies before the item's edge and is chased, and the
// preceding cycle's crossing notes), with its place and what it sounds as.
struct SoundingNote {
    std::uint32_t note;    // index of the note in the source
    std::int32_t pass;     // the item's pass it sounds in (-1: the preceding cycle)
    double songBeat;       // where its start sounds or would have sounded
    double contextBeat;    // where its start falls in the take's context
    int channel, written;  // as stored (channel 0..15) and the pitch written
    harmony::Followed sounds;
};

// The occurrences of `item`'s active take whose heard start (their song beat, or the item's
// start for a chased note) lies in [fromBeat, toBeat) and before the item's end, by pass then
// by note index. Empty for an item whose active take is not MIDI. A take that does not follow
// sounds each note as written (`sounds.by` is Copy). Muting is the caller's: a muted item
// still has places (the GUI shows its ghosts). The take's `pitch` shift, which the caller
// applies after, is not in `sounds`.
std::vector<SoundingNote> soundingNotes(const Session& s, Id item, double fromBeat, double toBeat);

// What a note written as `key` on `channel` sounds as at a place: the per-note step of the
// helper above, for the engine's live tables for launched clips and for tests. Channel 10
// (stored 9) and a `none` follow give the note as written. `contextBeat` is already wrapped
// into the period; `context` is sorted by beat then type.
harmony::Followed soundingKey(std::span<const ContextPoint> context, ClipFollow follow,
                              const ContextView& project, double contextBeat, double songBeat,
                              int channel, int key);

// The take's context over context beats [u0, u1), repeats unrolled, re-based to start at 0, with
// the key and the chord in force at u0 carried in at 0 (a point at u0 is the carried one). An
// unreadable point is data and is carried as it is. The result is sorted by beat then type, its
// beats on the 1/960 grid. At most kMaxContextPoints of a type: past that it stops and sets
// `*truncated` (a tiny period over a long span). `scene.capture` and `clip.add {item}` build a
// clip's context from it.
std::vector<ContextPoint> contextOver(const Take& take, double u0, double u1,
                                      bool* truncated = nullptr);

}  // namespace supple::model
