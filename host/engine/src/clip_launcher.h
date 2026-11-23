// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the clip launcher's commands share with the item and take commands
// (ADR-0097): reading a source from a file, removing a clip with its takes,
// and the conversion rule between an item and a clip.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "clip_launcher.h"
#include "supple/engine/app.h"
#include "supple/model/session.h"

namespace supple::engine {

// --- sources from files (item.add, clip.add; item_commands.cpp) -----------------

// An audio file as a media source: the source that already holds the same
// bytes, else a new one (copied into the media folder when the app copies
// imports). `seconds` is the file's length. Errors carry no path; the
// caller adds its field's.
struct AudioImport {
    model::Id source;
    double seconds = 0;
};
Result<AudioImport> importAudioFile(EngineApp& app, model::EditTxn& t, const model::Session& s,
                                    const std::string& path);

// A Standard MIDI File as a MIDI source, its notes in beats; `beats` is the
// file's length (its own end, else its last event's). `atBeat` is where the
// tempo comparison for the warnings is made.
struct MidiFileImport {
    model::MidiSource source;
    double beats = 0;
    std::vector<std::string> warnings;
};
Result<MidiFileImport> readMidiFile(EngineApp& app, const model::Session& s,
                                    const std::string& path, double atBeat);

// --- take owners: an item or a clip (clip_launcher_commands.cpp) -----------------

// The `item` argument of a content command, which takes an item or a clip
// (ADR-0097 "The take owner"); merges remap both kinds.
schema::Schema<std::string> ownerIdArg(const char* what = "Item or clip");
// The `item` argument of a timeline command: a clip's id is accepted only so
// that refuseClip can say what to use instead.
schema::Schema<std::string> timelineItemArg();
// An item or a clip by id, else it.unknown or cl.unknown with candidates.
Result<model::Id> existingOwner(const model::Session& s, const std::string& text,
                                const std::string& path);
// item.kind when `text` names a clip: timeline commands place material in
// time, and a clip has no place in time. `instead` is the hint.
Status refuseClip(const std::string& text, const std::string& path, const std::string& instead);
// item.kind for the first clip among `ids` (at path/k).
Status refuseClipIds(const std::vector<model::Id>& ids, const std::string& path,
                     const std::string& instead);
// An owner's active take, if any.
std::optional<model::Id> ownerActiveTake(const model::Session& s, model::Id owner);
// Edits an owner's takes and active take.
void modifyOwnerTakes(
    model::EditTxn& t, model::Id owner,
    const std::function<void(std::vector<model::Id>&, std::optional<model::Id>&)>& f);
// Where an owner's content starts, in beats: an item's start, 0 for a clip.
double ownerStartBeat(const model::Session& s, model::Id owner);
// The kind of the clip holding `take`, or nullopt when an item holds it.
std::optional<model::ClipKind> clipKindOfTake(const model::Session& s, model::Id take);
// The kind of the clip whose take's FX `fx` is, or nullopt for any other FX.
// Its envelopes are in clip time, as the take's own (ADR-0097, amended
// 2026-11-20, C2).
std::optional<model::ClipKind> clipKindOfFx(const model::Session& s, model::Id fx);

// --- clips (clip_launcher_commands.cpp) -------------------------------------------

// Removes a clip with its takes (their FX, envelopes and unpooled MIDI).
Status removeClipObjects(model::EditTxn& t, const model::Session& s, model::Id clip);
// Removes every clip `which` accepts, and returns them in id order.
Result<std::vector<model::Id>> removeClipsWhere(
    model::EditTxn& t, const model::Session& s,
    const std::function<bool(const model::Clip&)>& which);
// The clip.removed warning naming `clips`, or none when there are none.
void warnClipsRemoved(cmd::Context& ctx, const std::vector<model::Id>& clips,
                      const std::string& path);

// The tracks in display order: each master in project order, each track
// followed depth-first by its children.
std::vector<model::Id> tracksInDisplayOrder(const model::Session& s);

// One bar in beats at the first meter.
double firstBarBeats(const model::Session& s);

// An envelope on a take's FX moving between an item, where it is in timeline
// position, and a clip, where it is in clip time from the clip's start
// (ADR-0097, amended 2026-11-20, C2): rewrites its points and timebase.
// Points before 0 go, with the curve's value at 0 kept as a point there.
using FxEnvelopeMap = std::function<void(model::Envelope&)>;
// Into a clip of `kind` from an item starting at `startBeat` (`startSec`):
// beats from its start on a MIDI clip, seconds on an audio clip.
FxEnvelopeMap fxEnvelopesToClip(const model::Session& s, model::ClipKind kind, double startBeat,
                                double startSec);
// Out of a clip of `kind` onto an item at `pos` (beats), the clip's content
// `phase` in (beats on a MIDI clip, seconds on an audio clip) at its start.
// With a `period` (the clip's, in its unit), the curve repeats each period
// over the item's `length` (in the clip's unit), as the clip plays it: each
// pass's points up to the period, its value there held to the boundary, and
// the next pass from its first value at the boundary; a span ending on a
// boundary takes the next pass's start, which its FX hears within its latency.
FxEnvelopeMap fxEnvelopesFromClip(const model::Session& s, model::ClipKind kind, double pos,
                                  double phase = 0, double period = 0, double length = 0);
// A clip take's own envelopes (volume, pan, mute) onto a looped item at `pos`
// that spans more than one of the clip's periods: repeated pass after pass, as
// fxEnvelopesFromClip repeats its FX's, in item-relative beats (a take
// envelope's unit on an item), its timebase kept. On an item the take's
// envelopes run once from its start, where the clip ran them from the start of
// each pass.
FxEnvelopeMap takeEnvelopesFromClip(const model::Session& s, model::ClipKind kind, double pos,
                                    double phase, double period, double length);
// Applies `map` to the envelopes on `take`'s FX.
void convertTakeFxEnvelopes(model::EditTxn& t, const model::Session& s, model::Id take,
                            const FxEnvelopeMap& map);

// Copies `takes` into new takes for another owner, as ADR-0097 "Converting
// between an item and a clip" says: FX, envelopes and their automation items
// come along; a MIDI source is copied unless it is pooled, or shared and
// made pooled when `pooled`. `envelopePos` maps each take envelope point's
// position from the old owner's unit to the new one's (null: unchanged); a
// take envelope with automation items cannot be mapped (clip.unsupported).
// `fxEnvelope` maps the envelopes on its FX the same way (null: unchanged),
// which cannot carry automation items either. `takeEnvelope`, when given,
// maps each take envelope whole instead of `envelopePos` point by point
// (takeEnvelopesFromClip). `path` is where errors point.
Result<std::vector<model::Id>> copyTakesFor(model::EditTxn& t, const model::Session& s,
                                            const std::vector<model::Id>& takes, bool pooled,
                                            const std::function<double(double)>& envelopePos,
                                            const std::string& path,
                                            const FxEnvelopeMap& fxEnvelope = {},
                                            const FxEnvelopeMap& takeEnvelope = {});

// The item a clip's `take` makes at `pos` on `track` (ADR-0097 "Converting
// between an item and a clip"): its length, loop, gain, mute and colour,
// with no takes yet. `envelopePos` is set to the take envelope conversion
// (null for a MIDI clip, whose beats stay). A loop window an item cannot
// express is clip.unsupported.
struct ClipItem {
    model::Item item;
    std::function<double(double)> envelopePos;
    FxEnvelopeMap fxEnvelope;  // its take FX's envelopes onto the timeline (C2)
};
Result<ClipItem> itemFromClip(const model::Session& s, const model::Clip& clip,
                              const model::Take& take, model::Id track, double pos,
                              const std::string& path);
// Rewrites the points of the envelopes on `take` itself through `pos`.
void convertTakeEnvelopes(model::EditTxn& t, const model::Session& s, model::Id take,
                          const std::function<double(double)>& pos);

// The length of one pass of a take's content in played time: beats for
// MIDI, seconds for audio (the source's length over the take's playrate);
// 0 when it has none.
double contentLength(const model::Session& s, const model::Take& take);

// Whether launch.commit lays a stretch of `clip` (its active take `take`) as
// one looped item: its period is its content's pass, from the start, with no
// loop window inside it and no fade. Otherwise it lays one item per period, and the
// launched player on a playrate plan reads each period as that item does.
bool clipLaysAsOneItem(const model::Session& s, const model::Clip& clip, const model::Take& take);

// Refuses a take whose audio is warped: the clip player does not warp
// (clip.unsupported).
Status checkClipTakeUnwarped(const model::Session& s, model::Id take, const std::string& path);

// The clip an item makes (clip.add {item}, scene.capture): its kind, length,
// loop, takes (copied; a MIDI source with them unless `pooled`), gain, mute,
// colour and name (an item's name is its active take's). The track, scene and
// mode are the caller's, and so is the name when one is given. A warped take is
// clip.unsupported and takes of both kinds clip.mixed; errors point at `path`.
//
// `truncated`, when given, is set if the active take's context was longer than a clip holds
// (more points than the limit, or too many repeats to walk): the clip's context is then a
// shortened one, and the caller says so (`clip.context_dropped`).
Result<model::Clip> clipFromItem(model::EditTxn& t, const model::Session& s, model::Id item,
                                 bool pooled, const std::string& path, bool* truncated = nullptr);

// An item made from `clip` (its takes already copied and set on `item`) is
// named as the clip is: an item's name is its active take's, so the clip's
// name, when it has one, becomes that take's. A clip without a name leaves the
// copied take's own.
void nameItemFromClip(model::EditTxn& t, const model::Clip& clip, const model::Item& item);

// A clip laid on the timeline as an item over [startBeat, endBeat) on the
// clip's own track (scene.insert; ADR-0097 "Scene <-> section: the rules"):
// a `once` clip lasts its length or the span if shorter; a `loop` clip the
// span, as one looping item when it repeats within the span, which only a
// loop over the whole content with a length of whole repeats allows
// (scene.insert.period otherwise). An audio clip with a fade that repeats is
// laid one item per pass instead, each faded as its pass (S3 step C). Takes
// are copied as for item.add {clip}. The items laid, in order.
Result<std::vector<model::Id>> layClipOnTimeline(model::EditTxn& t, const model::Session& s,
                                                 const model::Clip& clip, double startBeat,
                                                 double endBeat, bool pooled,
                                                 const std::string& path);

// session.describe's scene section (session-model "Scenes and clips"), or
// nothing when there are no scenes.
std::string describeScenes(const model::Session& s);

// The clip.memory check: the clips' resident audio in `after` must all fit
// beside the arrangement's within the preparation budget (ADR-0097 "Memory").
Status checkClipMemory(EngineApp& app, const model::Session& after, const std::string& path);

}  // namespace supple::engine
