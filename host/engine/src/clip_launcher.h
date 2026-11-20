// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the clip launcher's commands share with the item and take commands
// (ADR-0097): reading a source from a file, removing a clip with its takes,
// and the conversion rule between an item and a clip.
#pragma once

#include <functional>
#include <string>
#include <vector>

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

// Copies `takes` into new takes for another owner, as ADR-0097 "Converting
// between an item and a clip" says: FX, envelopes and their automation items
// come along; a MIDI source is copied unless it is pooled, or shared and
// made pooled when `pooled`. `envelopePos` maps each take envelope point's
// position from the old owner's unit to the new one's (null: unchanged); a
// take envelope with automation items cannot be mapped (clip.unsupported).
// `path` is where errors point.
Result<std::vector<model::Id>> copyTakesFor(model::EditTxn& t, const model::Session& s,
                                            const std::vector<model::Id>& takes, bool pooled,
                                            const std::function<double(double)>& envelopePos,
                                            const std::string& path);

// The length of one pass of a take's content in played time: beats for
// MIDI, seconds for audio (the source's length over the take's playrate);
// 0 when it has none.
double contentLength(const model::Session& s, const model::Take& take);

// Refuses a take whose audio is warped: the clip player does not warp
// (clip.unsupported).
Status checkClipTakeUnwarped(const model::Session& s, model::Id take, const std::string& path);

// session.describe's scene section (session-model "Scenes and clips"), or
// nothing when there are no scenes.
std::string describeScenes(const model::Session& s);

// The clip.memory check: the clips' resident audio in `after` must all fit
// beside the arrangement's within the preparation budget (ADR-0097 "Memory").
Status checkClipMemory(EngineApp& app, const model::Session& after, const std::string& path);

}  // namespace supple::engine
