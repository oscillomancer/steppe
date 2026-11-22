// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "clip_launcher.h"
#include "command_validation.h"
#include "heard_midi.h"
#include "midi_context.h"
#include "midi_ticks.h"
// Note-level MIDI editing (the piano roll's commands, and an agent's when
// notation is the wrong tool): midi.notes reads an item's notes and CCs in
// beats from the item's start, midi.edit applies ordered operations to them.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <tuple>

#include "supple/engine/app.h"
#include "supple/harmony/harmony.h"
#include "supple/json/write.h"
#include "supple/model/follow.h"
#include "supple/model/schemas.h"
#include "supple/model/source_mapping.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

json::Value j(std::string_view s) { return json::parse(s).value(); }

struct ItemArgs {
    std::string item;
};
struct NotesArgs {
    std::string item;
    bool sounding = false;  // also each note's sounding pitch (a following take's)
};

enum class OpKind : std::uint8_t {
    Add,
    Remove,
    Move,
    Resize,
    Velocity,
    Quantize,
    CcSet,
    CcClear,
    TransposeDegrees,
    SnapToScale,
    FitToChord
};

struct CcPoint {
    double beat = 0;
    std::int64_t value = 0;
};

struct MidiOp {
    OpKind op = OpKind::Add;
    std::optional<std::vector<std::int64_t>> notes;  // indices in midi.notes order
    // add
    std::optional<std::int64_t> pitch, velocity, channel;
    std::optional<double> start, length;
    // move
    std::optional<double> beats;
    std::optional<std::int64_t> semitones;
    // resize: the end by length_delta, or the start by start_delta (the end stays)
    std::optional<double> lengthDelta, startDelta;
    // velocity: set (velocity) or delta
    std::optional<std::int64_t> velocityDelta;
    // quantize
    std::optional<double> grid, strength;
    std::optional<bool> ends;
    // cc.set / cc.clear
    std::optional<std::int64_t> number;
    std::optional<std::vector<CcPoint>> points;
    std::optional<double> from, to;
    // transpose_degrees / snap_to_scale / fit_to_chord (ADR-0095)
    std::optional<std::int64_t> degrees;
    std::optional<harmony::SnapMode> mode;
    std::optional<std::string> key, chord;  // explicit; else read from the context track
};

struct EditArgs {
    std::string item;
    std::vector<MidiOp> ops;
};

// Whether `owner` is an item whose active take follows the project's key or chords.
bool followingTake(const model::Session& s, model::Id owner) {
    const model::Item* item = s.items.find(owner);
    const model::Take* take = item && item->activeTake ? s.takes.find(*item->activeTake) : nullptr;
    return take && take->follow != model::ClipFollow::None;
}

struct Frame {  // item beats <-> source ticks
    double ppq = 960, offset = 0, rate = 1, itemPosition = 0;
    // What a looped item's roll adds to an item beat of this frame: the whole passes the
    // take's start offset skips, in item beats, so a note's beat in the first heard pass
    // (where it plays and the roll draws it) is itemPosition + beatOf + shift. 0 when the item
    // does not loop, and for a clip.
    double shift = 0;
    bool clip = false;  // a launcher clip's take: no place in time (ADR-0095 C-e)
    double beatOf(std::int64_t tick) const {
        return (static_cast<double>(tick) / ppq - offset) / rate;
    }
    Result<std::int64_t> convert(double beats, bool absolute, const std::string& path) const {
        const double ticks = (beats * rate + (absolute ? offset : 0)) * ppq;
        if (!std::isfinite(ticks) || std::abs(ticks) > static_cast<double>(std::int64_t{1} << 56))
            return Error("midi.range", "position exceeds the finite MIDI tick range", path,
                         "reduce the beat position or delta, take playrate or start_offset");
        return std::llround(ticks);
    }
    Result<std::int64_t> tickOf(double beat, const std::string& path) const {
        return convert(beat, true, path);
    }
    Result<std::int64_t> ticksOf(double beats, const std::string& path) const {
        return convert(beats, false, path);
    }
};

struct Target {
    Id item, take, source;
    const model::MidiSource* midi = nullptr;
    Frame frame;
};

// An item's or a clip's active MIDI take (ADR-0097 "The take owner"). A
// clip's beats count from its own start.
Result<Target> midiTarget(const model::Session& s, const std::string& text) {
    auto id = Id::parse(text);
    const bool clip = id && id->kind == model::Kind::Clip && s.clips.contains(*id);
    const model::Item* it = id && id->kind == model::Kind::Item ? s.items.find(*id) : nullptr;
    if (!it && !clip)
        return Error(id && id->kind == model::Kind::Clip ? "cl.unknown" : "it.unknown",
                     "no " + text, "/item",
                     "use an item id from track.list or item.add, or a clip id from "
                     "session.describe");
    const auto active = ownerActiveTake(s, *id);
    const model::Take* take = active ? s.takes.find(*active) : nullptr;
    if (!take || !take->source || take->source->kind != model::Kind::Midi)
        return Error("midi.not_midi", text + " has no MIDI take", "/item",
                     "use item.add with notation to create a MIDI item");
    const model::MidiSource* midi = s.midi.find(*take->source);
    if (!midi)
        return Error("mi.unknown", take->source->str() + " is missing from the model", "/item",
                     "load a project containing the item's MIDI source, or recreate the item with "
                     "item.add notation");
    Target t{*id, *active, *take->source, midi, {}};
    t.frame.ppq = midi->sequence.ppq;
    t.frame.offset = take->startOffset;
    t.frame.rate = take->playrate > 0 ? take->playrate : 1.0;
    t.frame.itemPosition = ownerStartBeat(s, *id);
    t.frame.clip = clip;
    const double cycle = midi->sequence.ppq > 0
                             ? static_cast<double>(midi->sequence.length) / midi->sequence.ppq
                             : 0.0;
    if (it && it->loop && cycle > 0)
        t.frame.shift =
            (take->startOffset -
             model::SourceBeatMapping{take->startOffset, 1.0, cycle}.sourceOffset(true)) /
            t.frame.rate;
    return t;
}

// Beat values in query results: rounded to 1e-9 beats, so floating noise
// (0.30000000000000004) does not show while every tick stays exact: a tick
// is at least 3e-7 beats (ppq <= 32767, playrate <= 100), so rounding moves a
// value by under 0.002 of a tick and a value read back lands on the same tick.
double roundBeats(double v) { return std::round(v * 1e9) / 1e9; }

json::Value noteJson(const midi::Note& n, const Frame& f) {
    json::Value v = json::Value::object();
    v["pitch"] = static_cast<std::int64_t>(n.pitch);
    v["start"] = roundBeats(f.beatOf(n.start));
    v["length"] = roundBeats(static_cast<double>(n.length) / f.ppq / f.rate);
    v["velocity"] = static_cast<std::int64_t>(n.velocity);
    if (n.channel) v["channel"] = static_cast<std::int64_t>(n.channel) + 1;
    return v;
}

Result<json::Value> describe(const model::Session& s, const Target& t, bool sounding = false) {
    const midi::Sequence& seq = t.midi->sequence;
    json::Value out = json::Value::object();
    out["item"] = t.item.str();
    out["ppq"] = static_cast<std::int64_t>(seq.ppq);
    if (const model::Clip* clip = s.clips.find(t.item)) {
        out["length"] = roundBeats(clip->length);  // a clip has no position
    } else {
        out["length"] = roundBeats(model::itemLengthBeats(s, *s.items.find(t.item)));
        out["position"] = roundBeats(t.frame.itemPosition);
    }
    if (t.midi->pooled) out["pooled"] = true;
    json::Value notes = json::Value::array();
    for (const midi::Note& n : seq.notes) notes.push_back(noteJson(n, t.frame));
    if (sounding) {
        // Each note's pitch as it sounds (its first whole pass; as written when the take does
        // not follow or the note is never heard): `sounds`, beside the written `pitch`.
        const model::Item* item = s.items.find(t.item);
        const auto heard = item ? model::soundingNotes(s, t.item, -1e18, 1e18)
                                : std::vector<model::SoundingNote>{};
        std::vector<std::int64_t> sounded;
        for (const auto& h : heard) sounded.push_back(h.channel == 9 ? h.written : h.sounds.pitch);
        const model::LeadPitches lead = model::leadPitches(
            heard, sounded, item ? model::itemSpan(s, *item).startBeat : 0.0, seq.notes.size());
        for (std::size_t i = 0; i < seq.notes.size() && i < lead.keep.size(); ++i)
            notes.asArray()[i]["sounds"] =
                lead.keep[i] ? *lead.keep[i] : static_cast<std::int64_t>(seq.notes[i].pitch);
        if (!lead.alike) out["differs_by_pass"] = true;
    }
    out["notes"] = std::move(notes);
    json::Value ccs = json::Value::array();
    for (const midi::Control& c : seq.controls) {
        if (c.kind != midi::ControlKind::CC) continue;
        json::Value v = json::Value::object();
        v["number"] = static_cast<std::int64_t>(c.number);
        v["beat"] = roundBeats(t.frame.beatOf(c.tick));
        v["value"] = static_cast<std::int64_t>(c.value);
        if (c.channel) v["channel"] = static_cast<std::int64_t>(c.channel) + 1;
        ccs.push_back(std::move(v));
    }
    out["cc"] = std::move(ccs);
    return out;
}

// One working note: the original index (or -1 for an added note), alive, touched.
struct Working {
    midi::Note note;
    bool alive = true, touched = false;
};

Result<std::int64_t> noteEnd(const midi::Note& note, const std::string& path) {
    auto end = detail::addMidiTicks(note.start, note.length, path);
    if (!end) return end.error();
    // Match the model/file boundary, before canonicalization or publication.
    if (note.start < 0 || note.length < 0 || *end > std::numeric_limits<std::int64_t>::max() / 4)
        return Error("midi.range", "resulting note exceeds the model's MIDI tick range", path,
                     "shorten the note length or reduce its start position or delta");
    return *end;
}

Status applyOp(const MidiOp& op, std::size_t at, std::vector<Working>& notes, std::size_t original,
               midi::Sequence& seq, const Frame& f, const model::Session& session, const Id& source,
               const Id& owner) {
    const std::string here = "/ops/" + std::to_string(at);
    constexpr std::int64_t tickLimit = std::int64_t{1} << 56;
    auto validBeat = [&](double beat, bool absolute) {
        const double ticks = (beat * f.rate + (absolute ? f.offset : 0)) * f.ppq;
        return std::isfinite(ticks) && std::abs(ticks) <= static_cast<double>(tickLimit);
    };
    const std::pair<const char*, std::optional<double>> numbers[] = {
        {"start", op.start},
        {"length", op.length},
        {"beats", op.beats},
        {"start_delta", op.startDelta},
        {"length_delta", op.lengthDelta},
        {"grid", op.grid},
        {"from", op.from},
        {"to", op.to},
        {"strength", op.strength}};
    for (const auto& [name, value] : numbers)
        if (value && !validBeat(*value, std::string_view(name) == "start" ||
                                            std::string_view(name) == "from" ||
                                            std::string_view(name) == "to"))
            return Error("midi.range", "number exceeds the finite MIDI tick range",
                         here + "/" + name, "use a smaller finite number");
    if (op.grid && *op.grid < 1e-9)
        return Error("midi.range", "grid is too small", here + "/grid", "use at least 1e-9 beats");
    if ((op.semitones && (*op.semitones < -127 || *op.semitones > 127)) ||
        (op.velocityDelta && (*op.velocityDelta < -127 || *op.velocityDelta > 127)))
        return Error("midi.range", "pitch/velocity delta exceeds -127..127", here,
                     "use a delta within the MIDI range");
    for (const auto& n : notes)
        if (n.note.start < 0 || n.note.start > tickLimit || n.note.length < 0 ||
            n.note.length > tickLimit)
            return Error("midi.range", "source notes exceed the MIDI tick range", "/item",
                         "shorten the MIDI source");
    if (op.points)
        for (const auto& point : *op.points)
            if (!validBeat(point.beat, true))
                return Error("midi.range", "CC beat exceeds the finite MIDI tick range",
                             here + "/points",
                             "reduce the beat position, delta or grid so resulting source ticks "
                             "fit the finite MIDI range");
    auto selection = [&](bool allWhenAbsent) -> Result<std::vector<std::size_t>> {
        std::vector<std::size_t> out;
        if (!op.notes) {
            if (!allWhenAbsent)
                return Error("midi.no_notes", "say which notes", here + "/notes",
                             "indices from midi.notes");
            for (std::size_t i = 0; i < original; ++i)
                if (notes[i].alive) out.push_back(i);
            return out;
        }
        std::set<std::size_t> selected;
        for (std::int64_t i : *op.notes) {
            if (i < 0 || static_cast<std::size_t>(i) >= original)
                return Error("midi.note_index", "no note " + std::to_string(i), here + "/notes",
                             "indices are positions in midi.notes' list, 0 to " +
                                 std::to_string(original == 0 ? 0 : original - 1));
            const auto index = static_cast<std::size_t>(i);
            if (notes[index].alive && selected.insert(index).second) out.push_back(index);
        }
        return out;
    };
    const std::int64_t minLength = std::max<std::int64_t>(1, seq.ppq / 64);
    switch (op.op) {
        case OpKind::Add: {
            if (!op.pitch || !op.start || !op.length)
                return Error("midi.add_args", "add needs pitch, start and length", here,
                             R"({"op":"add","pitch":60,"start":0,"length":1})");
            midi::Note n;
            n.pitch = static_cast<std::uint8_t>(*op.pitch);
            auto start = f.tickOf(*op.start, here + "/start");
            if (!start) return start.error();
            auto length = f.ticksOf(*op.length, here + "/length");
            if (!length) return length.error();
            n.start = *start;
            n.length = std::max(minLength, *length);
            n.velocity = static_cast<std::uint8_t>(op.velocity.value_or(96));
            n.channel = static_cast<std::uint8_t>(op.channel.value_or(1) - 1);
            if (n.start < 0)
                return Error("midi.before_source", "the note starts before the item's MIDI",
                             here + "/start",
                             "start at " + std::to_string(roundBeats(f.beatOf(0))) + " or later");
            SUPPLE_TRY(noteEnd(n, here + "/length"));
            notes.push_back({n, true, true});
            return {};
        }
        case OpKind::Remove: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) notes[i].alive = false;
            return {};
        }
        case OpKind::Move: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            auto dt = f.ticksOf(op.beats.value_or(0), here + "/beats");
            if (!dt) return dt.error();
            const std::int64_t dp = op.semitones.value_or(0);
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                auto start = detail::addMidiTicks(n.start, *dt, here + "/beats");
                if (!start) return start.error();
                if (*start < 0)
                    return Error("midi.before_source", "a note would start before the item's MIDI",
                                 here + "/beats",
                                 "use a smaller negative beats delta or move the note later");
                if (n.pitch + dp < 0 || n.pitch + dp > 127)
                    return Error("midi.pitch_range", "a note would leave the MIDI range 0-127",
                                 here + "/semitones",
                                 "reduce semitones so every selected pitch remains 0..127");
                n.start = *start;
                SUPPLE_TRY(noteEnd(n, here + "/beats"));
                n.pitch = static_cast<std::uint8_t>(n.pitch + dp);
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::Resize: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                if (op.startDelta) {
                    auto end = noteEnd(n, here + "/start_delta");
                    if (!end) return end.error();
                    auto delta = f.ticksOf(*op.startDelta, here + "/start_delta");
                    if (!delta) return delta.error();
                    auto start = detail::addMidiTicks(n.start, *delta, here + "/start_delta");
                    if (!start) return start.error();
                    auto latest = detail::addMidiTicks(*end, -minLength, here + "/start_delta");
                    if (!latest) return latest.error();
                    n.start =
                        std::clamp<std::int64_t>(*start, 0, std::max<std::int64_t>(0, *latest));
                    auto length = detail::addMidiTicks(*end, -n.start, here + "/start_delta");
                    if (!length) return length.error();
                    n.length = *length;
                }
                if (op.lengthDelta) {
                    auto delta = f.ticksOf(*op.lengthDelta, here + "/length_delta");
                    if (!delta) return delta.error();
                    auto length = detail::addMidiTicks(n.length, *delta, here + "/length_delta");
                    if (!length) return length.error();
                    n.length = std::max(minLength, *length);
                }
                if (op.length) {
                    auto length = f.ticksOf(*op.length, here + "/length");
                    if (!length) return length.error();
                    n.length = std::max(minLength, *length);
                }
                SUPPLE_TRY(noteEnd(n, here + "/length"));
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::Velocity: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                std::int64_t v = op.velocity ? *op.velocity : n.velocity;
                v += op.velocityDelta.value_or(0);
                n.velocity = static_cast<std::uint8_t>(std::clamp<std::int64_t>(v, 1, 127));
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::Quantize: {
            if (!op.grid || *op.grid <= 0)
                return Error("midi.grid", "quantize needs a grid in beats", here + "/grid",
                             "0.25 for sixteenths, 0.5 for eighths, 1/3 for triplet quarters");
            auto sel = selection(true);
            if (!sel) return std::move(sel).error();
            const double strength = std::clamp(op.strength.value_or(1.0), 0.0, 1.0);
            // The project's grid: snap project beats, not item beats, at the beat the note
            // plays at (a looped item's roll draws it there: the frame's shift).
            auto snap = [&](double itemBeat) {
                const double project = f.itemPosition + itemBeat + f.shift;
                const double target = std::round(project / *op.grid) * *op.grid;
                return itemBeat + (target - project) * strength;
            };
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                auto oldEnd = noteEnd(n, here + "/grid");
                if (!oldEnd) return oldEnd.error();
                const double start = f.beatOf(n.start);
                const double end = f.beatOf(*oldEnd);
                auto snappedStart = f.tickOf(snap(start), here + "/grid");
                if (!snappedStart) return snappedStart.error();
                // Validate the end even when quantizing only the start, as before.
                auto snappedEnd = f.tickOf(snap(end), here + "/grid");
                if (!snappedEnd) return snappedEnd.error();
                const auto newStart = std::max<std::int64_t>(0, *snappedStart);
                auto newEnd = op.ends.value_or(false)
                                  ? snappedEnd
                                  : detail::addMidiTicks(newStart, n.length, here + "/grid");
                if (!newEnd) return newEnd.error();
                auto length = detail::addMidiTicks(*newEnd, -newStart, here + "/grid");
                if (!length) return length.error();
                if (*length < minLength) {
                    auto grid = f.ticksOf(*op.grid, here + "/grid");
                    if (!grid) return grid.error();
                    length = std::max(minLength, *grid);
                    newEnd = detail::addMidiTicks(newStart, *length, here + "/grid");
                    if (!newEnd) return newEnd.error();
                }
                n.start = newStart;
                n.length = *length;
                SUPPLE_TRY(noteEnd(n, here + "/grid"));
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::TransposeDegrees:
        case OpKind::SnapToScale:
        case OpKind::FitToChord: {
            const bool transpose = op.op == OpKind::TransposeDegrees;
            const bool fit = op.op == OpKind::FitToChord;
            if (transpose && !op.degrees)
                return Error("midi.degrees", "transpose_degrees needs degrees", here + "/degrees",
                             R"({"op":"transpose_degrees","notes":[0,1],"degrees":2})");
            if (!fit && op.chord)
                return Error("midi.args", "chord is not used by this op", here + "/chord",
                             "only fit_to_chord takes a chord");
            if (fit && (op.key || op.mode || op.degrees))
                return Error("midi.args", "fit_to_chord takes only notes? and chord?", here,
                             "remove key, mode and degrees");
            if (!transpose && op.degrees)
                return Error("midi.args", "degrees is only for transpose_degrees",
                             here + "/degrees", "remove degrees");
            if (op.mode && op.op != OpKind::SnapToScale)
                return Error("midi.args", "mode is only for snap_to_scale", here + "/mode",
                             "remove mode");
            std::optional<harmony::Key> key;
            std::optional<harmony::Chord> chord;
            if (op.key) {
                auto parsed = detail::parseKeyText(*op.key, here + "/key");
                if (!parsed) return std::move(parsed).error();
                key = *parsed;
            }
            if (op.chord) {
                auto parsed = harmony::parseChord(*op.chord);
                if (!parsed) {
                    Error e = parsed.error();
                    e.path = here + "/chord";
                    return e;
                }
                chord = *parsed;
            }
            auto sel = selection(!transpose);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                // The note as the earlier ops of this edit left it.
                const bool needKey = !fit && !key, needChord = fit && !chord;
                detail::NoteContext context;
                if (f.clip && (needKey || needChord)) {
                    // A clip reads its own context at the clip beat the note sounds at,
                    // never the project's (ADR-0098; supersedes ADR-0095 amendment C-e).
                    const model::Clip* held = session.clips.find(owner);
                    const model::Take* take =
                        held && held->activeTake ? session.takes.find(*held->activeTake) : nullptr;
                    if (!held || !take)
                        return Error("midi.no_key", "the clip is missing from the model",
                                     here + "/notes", "reload the project");
                    auto read =
                        detail::clipNoteContext(*held, *take, static_cast<int>(f.ppq), n.start,
                                                needKey, needChord, here + "/notes");
                    if (!read) return std::move(read).error();
                    context = *read;
                } else if ((needKey || needChord) && followingTake(session, owner)) {
                    // A following take reads its own context at the note's context beat
                    // (ADR-0098, following is playback).
                    auto read = detail::takeNoteContext(session, owner, n.start, needKey, needChord,
                                                        here + "/notes");
                    if (!read) return std::move(read).error();
                    context = *read;
                } else if (needKey || needChord) {
                    auto read = detail::noteContext(session, source, n.start,
                                                    f.itemPosition + f.beatOf(n.start), needKey,
                                                    needChord, here + "/notes");
                    if (!read) return std::move(read).error();
                    context = *read;
                }
                std::optional<int> pitch;
                if (transpose) {
                    pitch = harmony::transposeDegrees(n.pitch, key ? *key : *context.key,
                                                      static_cast<int>(*op.degrees));
                } else if (fit) {
                    const auto classes =
                        harmony::chordPitchClasses(chord ? *chord : *context.chord);
                    pitch =
                        harmony::snapToPitchClasses(n.pitch, classes, harmony::SnapMode::Nearest);
                } else {
                    const auto classes = harmony::keyPitchClasses(key ? *key : *context.key);
                    pitch = harmony::snapToPitchClasses(
                        n.pitch, classes, op.mode.value_or(harmony::SnapMode::Nearest));
                }
                if (!pitch)
                    return Error("midi.pitch_range",
                                 "a note would leave the MIDI range 0-127 or has no such pitch "
                                 "in that direction",
                                 here + "/notes",
                                 transpose
                                     ? "use fewer degrees, or leave the highest and lowest "
                                       "notes out of notes"
                                     : "leave the note out of notes, or snap in another mode");
                if (*pitch != n.pitch) {
                    n.pitch = static_cast<std::uint8_t>(*pitch);
                    notes[i].touched = true;
                }
            }
            return {};
        }
        case OpKind::CcSet: {
            if (!op.number || !op.points)
                return Error("midi.cc_args", "cc.set needs number and points", here,
                             R"({"op":"cc.set","number":1,"points":[{"beat":0,"value":64}]})");
            const auto channel = static_cast<std::uint8_t>(op.channel.value_or(1) - 1);
            for (const CcPoint& p : *op.points) {
                midi::Control c;
                c.kind = midi::ControlKind::CC;
                auto tick = f.tickOf(p.beat, here + "/points");
                if (!tick) return tick.error();
                c.tick = *tick;
                c.channel = channel;
                c.number = static_cast<std::uint8_t>(*op.number);
                c.value = static_cast<std::int32_t>(std::clamp<std::int64_t>(p.value, 0, 127));
                if (c.tick < 0)
                    return Error("midi.before_source", "a point is before the item's MIDI", here,
                                 "use a later beat position or smaller negative delta so the note "
                                 "or CC point is at or after the take start_offset");
                std::erase_if(seq.controls, [&](const midi::Control& x) {
                    return x.kind == c.kind && x.channel == c.channel && x.number == c.number &&
                           x.tick == c.tick;
                });
                seq.controls.push_back(c);
            }
            return {};
        }
        case OpKind::CcClear: {
            if (!op.number)
                return Error("midi.cc_args", "cc.clear needs number", here + "/number",
                             "provide number as the MIDI CC number from 0 to 127");
            auto from = op.from ? f.tickOf(*op.from, here + "/from")
                                : Result<std::int64_t>{std::numeric_limits<std::int64_t>::min()};
            if (!from) return from.error();
            auto to = op.to ? f.tickOf(*op.to, here + "/to")
                            : Result<std::int64_t>{std::numeric_limits<std::int64_t>::max()};
            if (!to) return to.error();
            std::erase_if(seq.controls, [&](const midi::Control& x) {
                return x.kind == midi::ControlKind::CC && x.number == *op.number &&
                       x.tick >= *from && x.tick < *to &&
                       (!op.channel || x.channel == *op.channel - 1);
            });
            return {};
        }
    }
    return {};
}

}  // namespace

Status detail::mergeMidi(const model::Session& s, model::EditTxn& txn, const std::vector<Id>& ids) {
    auto refuse = [](std::string message) {
        return Error("item.consolidate_midi", std::move(message), "/items",
                     "give MIDI items on one track, or one item with several MIDI takes");
    };
    if (ids.empty()) return refuse("no items");
    // What is merged: each item's heard take, or one item's every take.
    std::vector<std::pair<Id, Id>> parts;  // item, take
    for (const Id id : ids) {
        const model::Item* it = s.items.find(id);
        if (!it) return refuse("no item " + id.str());
        SUPPLE_TRY(checkUnlocked(s, id, "/items"));
        if (it->track != s.items.find(ids.front())->track)
            return refuse("the items are on different tracks");
        const std::vector<Id> takes =
            ids.size() == 1
                ? it->takes
                : (it->activeTake ? std::vector<Id>{*it->activeTake} : std::vector<Id>{});
        if (takes.empty()) return refuse(id.str() + " has no take");
        for (const Id take : takes) {
            const model::Take* tk = s.takes.find(take);
            if (!tk || !tk->source || tk->source->kind != model::Kind::Midi ||
                !s.midi.find(*tk->source))
                return refuse(id.str() + " is not MIDI");
            parts.emplace_back(id, take);
        }
    }
    if (parts.size() < 2) return refuse("one item with one take: nothing to merge");
    // The earliest item is kept; the merged MIDI starts there.
    Id base = ids.front(), last = ids.front();
    double start = 1e300, end = -1e300;
    for (const Id id : ids) {
        const model::Item& it = *s.items.find(id);
        const double from = model::itemSpan(s, it).startBeat;
        const double to = model::itemEndBeat(s, it);
        if (from < start) {
            start = from;
            base = id;
        }
        if (to > end) {
            end = to;
            last = id;
        }
    }
    // Following (ADR-0098, following is playback): what sounds is preserved, and checked. Takes
    // that all follow in one mode merge into a take that follows in it, with the context of each
    // source carried over onto the result's source beats; mixed or different modes are refused.
    const auto followRefusal = [](std::string message) {
        return Error("item.follow_merge", std::move(message), "/items",
                     "take.bake the following takes first, then consolidate");
    };
    const bool lone = ids.size() == 1;
    std::size_t following = 0;
    for (const auto& [id, take] : parts)
        if (s.takes.find(take)->follow != model::ClipFollow::None) ++following;
    const model::ClipFollow mode = s.takes.find(parts.front().second)->follow;
    if (following != 0) {
        if (following != parts.size())
            return followRefusal(
                "some of the takes follow the project's context and some do not, "
                "so merging would change how some notes sound");
        for (const auto& [id, take] : parts)
            if (s.takes.find(take)->follow != mode)
                return followRefusal(
                    id.str() + " follows by " +
                    (s.takes.find(take)->follow == model::ClipFollow::Chords ? "chords" : "key") +
                    " and the others by " + (mode == model::ClipFollow::Chords ? "chords" : "key"));
    }
    // The result's context, in the result's source beats. A note of the first source that is
    // chased at its start keeps its true start, before the result's start: `leadIn` beats.
    double leadIn = 0.0;
    std::vector<model::ContextPoint> mergedContext;
    std::vector<model::SoundingNote> baseHeard;
    if (following != 0) {
        constexpr double kEdge = 1e-9;
        // The first item's heard notes (a lone item's: its active take's), the chased ones
        // included, keep their true starts in the result's source.
        baseHeard = model::soundingNotes(s, base, -1e18, 1e18);
        for (const auto& h : baseHeard) leadIn = std::max(leadIn, start - h.songBeat);
        leadIn = std::ceil(std::max(0.0, leadIn - kEdge) * 960.0) / 960.0;
        // Each part's context over its span (the first from its lead-in), onto the result axis.
        std::vector<std::pair<double, std::pair<Id, Id>>> order;
        for (const auto& part : parts)
            order.emplace_back(model::itemSpan(s, *s.items.find(part.first)).startBeat, part);
        std::stable_sort(order.begin(), order.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<model::ContextPoint> first;  // a lone item's layered takes must all agree
        bool seen = false;
        for (const auto& [itemStart, part] : order) {
            const model::Item& it = *s.items.find(part.first);
            const model::Take& tk = *s.takes.find(part.second);
            const double rate = tk.playrate > 0 ? tk.playrate : 1.0;
            // A lone item's takes are layered on one axis, so each reads from the lead-in.
            const bool lead = lone || part.first == base;
            const double u0 = tk.contextPhase + tk.startOffset - (lead ? leadIn * rate : 0.0);
            const double u1 =
                tk.contextPhase + tk.startOffset + model::itemLengthBeats(s, it) * rate;
            // Result source beat of the re-based beat 0: the lead-in for the first source (it
            // starts there), else the lead-in plus the source's distance from the result start.
            const double origin = lead ? 0.0 : leadIn + (itemStart - start);
            bool cut = false;
            std::vector<model::ContextPoint> points = model::contextOver(tk, u0, u1, &cut);
            if (cut)
                return followRefusal(part.second.str() +
                                     " has a context too long to carry into the result "
                                     "(more points than a take holds)");
            for (model::ContextPoint& p : points)
                p.beat = std::round((origin + p.beat / rate) * 960.0) / 960.0;
            if (lone) {
                const auto key = [](const model::ContextPoint& p) {
                    return std::tie(p.beat, p.type, p.tonic, p.scale, p.symbol);
                };
                const bool same =
                    seen &&
                    std::equal(points.begin(), points.end(), first.begin(), first.end(),
                               [&](const auto& a, const auto& b) { return key(a) == key(b); });
                if (seen && !same)
                    return followRefusal(part.second.str() +
                                         " has a different context from the item's other takes "
                                         "over its span, so layering them would change how "
                                         "some notes sound");
                if (!seen) first = points;
                seen = true;
                continue;
            }
            // A later source's point at a beat replaces an earlier one's of its type there.
            for (model::ContextPoint& p : points) {
                std::erase_if(mergedContext, [&](const model::ContextPoint& q) {
                    return q.beat == p.beat && q.type == p.type;
                });
                mergedContext.push_back(std::move(p));
            }
        }
        if (lone) mergedContext = std::move(first);
        std::stable_sort(mergedContext.begin(), mergedContext.end(),
                         [](const model::ContextPoint& a, const model::ContextPoint& b) {
                             return a.beat != b.beat ? a.beat < b.beat : a.type < b.type;
                         });
    }

    midi::Sequence merged;
    merged.ppq = 960;
    const Id baseActive = s.items.find(base)->activeTake ? *s.items.find(base)->activeTake
                                                         : s.items.find(base)->takes.front();
    for (const auto& [id, take] : parts) {
        const model::Item& it = *s.items.find(id);
        const model::Take& tk = *s.takes.find(take);
        const double shift = model::itemSpan(s, it).startBeat - start + leadIn;
        if (following != 0 && id == base && take == baseActive) {
            // The first source's notes at their true starts (before the result's start when
            // chased); its controls and sysex as heardMidi places them.
            midi::Sequence scratch;
            scratch.ppq = merged.ppq;
            detail::heardMidi(s, it, tk, s.midi.find(*tk.source)->sequence, shift, scratch);
            merged.controls.insert(merged.controls.end(), scratch.controls.begin(),
                                   scratch.controls.end());
            merged.sysex.insert(merged.sysex.end(), scratch.sysex.begin(), scratch.sysex.end());
            const midi::Sequence& written = s.midi.find(*tk.source)->sequence;
            for (const auto& h : baseHeard) {
                midi::Note note = written.notes[h.note];
                note.start = std::llround((h.songBeat - start + leadIn) * merged.ppq);
                note.length =
                    std::max<std::int64_t>(0, std::llround((h.endBeat - h.songBeat) * merged.ppq));
                merged.notes.push_back(note);
            }
            continue;
        }
        detail::heardMidi(s, it, tk, s.midi.find(*tk.source)->sequence, shift, merged);
    }
    merged.length = std::llround((end - start + leadIn) * merged.ppq);
    merged.canonicalize();
    const model::Item& kept = *s.items.find(base);
    const Id heard = kept.activeTake ? *kept.activeTake : kept.takes.front();
    model::Take take = *s.takes.find(heard);
    take.source = txn.create(model::MidiSource{std::move(merged), false});
    take.startOffset = leadIn;
    take.playrate = 1;
    // The result's context is the sources' as carried over (none when none follow, since the
    // notes now sit at places the old context did not describe).
    take.context = std::move(mergedContext);
    take.contextPeriod.reset();
    take.contextPhase = 0.0;
    take.follow = following != 0 ? mode : model::ClipFollow::None;
    model::fitTakeContext(take, model::TakeHome::Item);
    take.stretchMarkers.clear();
    take.transients.reset();
    take.warpPeers.clear();
    auto copied = copyTake(s, txn, heard, std::move(take));
    if (!copied) return std::move(copied).error();
    model::Item item = kept;
    item.takes = {*copied};
    item.activeTake = *copied;
    item.loop = false;
    item.fadeOut = s.items.find(last)->fadeOut;
    model::placeItem(s, item, start, end - start);
    for (const Id id : ids) {
        for (const Id t : s.items.find(id)->takes) SUPPLE_TRY(removeTakeObjects(txn, s, t));
        if (id != base) txn.remove<model::Item>(id);
    }
    txn.put<model::Item>(base, std::move(item));
    if (following == 0) return {};
    // What sounds is preserved: every heard note's start, channel and sounding pitch before (the
    // heard take of each merged item) and after (the result item). A lone item's layered takes
    // add notes by design, so each of the heard take's must be among the result's.
    struct Heard {
        std::int64_t tick;
        int channel, pitch;
        Id item;
        double beat;
        bool operator<(const Heard& o) const {
            return std::tie(tick, channel, pitch) < std::tie(o.tick, o.channel, o.pitch);
        }
    };
    const auto collect = [](const model::Session& at, Id which) {
        std::vector<Heard> out;
        const double itemStart = model::itemSpan(at, *at.items.find(which)).startBeat;
        for (const model::SoundingNote& h : model::soundingNotes(at, which, -1e18, 1e18)) {
            const double beat = std::max(h.songBeat, itemStart);
            out.push_back({std::llround(beat * 960.0), h.channel + 1,
                           h.channel == 9 ? h.written : h.sounds.pitch, which, beat});
        }
        return out;
    };
    std::vector<Heard> before, after = collect(txn.preview(), base);
    if (lone) {
        before = collect(s, base);
    } else {
        for (const auto& part : parts) {
            auto one = collect(s, part.first);
            before.insert(before.end(), one.begin(), one.end());
        }
    }
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::vector<Heard> missing;
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::back_inserter(missing));
    if (!missing.empty())
        return followRefusal("merging would change how the note at beat " +
                             json::formatDouble(missing.front().beat) + " of " +
                             missing.front().item.str() + " sounds");
    if (!lone && before.size() != after.size()) {
        std::vector<Heard> extra;
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                            std::back_inserter(extra));
        if (!extra.empty())
            return followRefusal("merging would add a sounding note at beat " +
                                 json::formatDouble(extra.front().beat) + " of " + base.str());
    }
    return {};
}

void detail::heardMidi(const model::Session& s, const model::Item& it, const model::Take& take,
                       const midi::Sequence& seq, double shift, midi::Sequence& out) {
    const model::ItemSpan span = model::itemSpan(s, it);
    const double itemBeats = s.timeMap->beatAt(span.endSec) - span.startBeat;
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    const double loopBeats = seq.ppq > 0 ? static_cast<double>(seq.length) / seq.ppq : 0.0;
    const bool looping = it.loop && loopBeats > 0;
    const model::SourceBeatMapping mapping{take.startOffset, rate, loopBeats};
    const int reps = looping
                         ? 1 + static_cast<int>(std::floor(
                                   (itemBeats * rate + mapping.sourceOffset(looping)) / loopBeats))
                         : 1;
    auto tick = [&](double beat) { return std::llround((beat + shift) * out.ppq); };
    auto beat = [&](std::int64_t t, int pass) {
        return mapping.itemBeat(static_cast<double>(t) / seq.ppq, pass, looping);
    };
    for (int k = looping ? -1 : 0; k < reps; ++k) {
        for (const midi::Note& n : seq.notes) {
            if (looping && n.start >= seq.length) continue;
            double on = beat(n.start, k);
            const double off = std::min(itemBeats, beat(n.start + n.length, k));
            if (on >= itemBeats) continue;
            if (on < 0) {
                if (off <= 0) continue;
                on = 0;  // held into the item's start
            }
            midi::Note note = n;
            note.start = tick(on);
            note.length = std::max<std::int64_t>(0, tick(off) - note.start);
            out.notes.push_back(note);
        }
        for (const midi::Control& c : seq.controls) {
            const double at = beat(c.tick, k);
            if (at < 0 || at >= itemBeats) continue;
            midi::Control control = c;
            control.tick = tick(at);
            out.controls.push_back(control);
        }
        for (const midi::Sysex& x : seq.sysex) {
            const double at = beat(x.tick, k);
            if (at < 0 || at >= itemBeats) continue;
            midi::Sysex sysex = x;
            sysex.tick = tick(at);
            out.sysex.push_back(std::move(sysex));
        }
    }
}

void registerMidiCommands(cmd::Registry& registry) {
    {
        Command<NotesArgs, json::Value> c;
        c.name = "midi.notes";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List a MIDI item's or clip's notes and CC points (beats from its start).";
        c.description =
            "Notes are in a fixed order (by start, then pitch); midi.edit refers to them by their "
            "position in this list. start/length/beat are in beats from the item's start; channel "
            "is 1-16 (omitted for 1). pooled: other items share this MIDI, and edits change them "
            "too. For writing whole phrases, midi.write with notation is usually better. Errors: "
            "midi.sounding_clip (sounding on a clip: what it sounds as depends on where it is "
            "launched).";
        c.tags = {"midi"};
        c.errors = {{"midi.sounding_clip", "sounding was asked of a clip",
                     "launch.state says how a launched clip follows; lay the clip on the "
                     "timeline (item.add {clip}) to read what sounds"}};
        c.args = schema::object<NotesArgs>("midi_notes_args")
                     .field("item", &NotesArgs::item, ownerIdArg())
                     .field("sounding", &NotesArgs::sounding,
                            schema::boolean().doc("Add each note's sounding pitch as `sounds` "
                                                  "(a following take's notes as they play; the "
                                                  "first pass of a looped item, and "
                                                  "differs_by_pass when later passes differ). "
                                                  "`pitch` stays the written one. A clip is "
                                                  "refused: midi.sounding_clip."))
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.examples = {
            {"Notes of a MIDI item", j(R"({"item": "it_1"})"),
             j(R"({"item": "it_1", "ppq": 960, "length": "<any>", "position": "<any>", "notes": "<any>", "cc": "<any>"})"),
             "song_with_item"},
            {"A following take's notes with the pitches they sound as",
             j(R"({"item": "it_1", "sounding": true})"),
             j(R"({"item": "it_1", "ppq": 960, "length": "<any>", "position": "<any>", "notes": "<any>", "cc": "<any>"})"),
             "song_following"}};
        c.run = [](const NotesArgs& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = ctx.service<EngineApp>().view();
            auto t = midiTarget(s, a.item);
            if (!t) return std::move(t).error();
            if (a.sounding && s.clips.find(t->item))
                return Error("midi.sounding_clip",
                             a.item +
                                 " is a clip: what it sounds as depends on where it is "
                                 "launched",
                             "/sounding",
                             "launch.state says how a launched clip follows; lay it on the "
                             "timeline (item.add {clip}) to read what sounds");
            return describe(s, *t, a.sounding);
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<EditArgs, json::Value> c;
        c.name = "midi.edit";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary =
            "Edit a MIDI item's notes and CCs: move, resize, quantize, fit to key or chord.";
        c.description =
            "ops run in order and are one undo step. notes: indices into midi.notes' list as it "
            "was before this command (every op refers to that list, so later ops are not "
            "affected by earlier removals). Beats are from the item's start. add {pitch, start, "
            "length, velocity?, channel?}; remove {notes}; move {notes, beats?, semitones?}; "
            "resize {notes, length? | length_delta? | start_delta?}; velocity {notes, velocity? "
            "and/or velocity_delta?}; quantize {notes? (all), grid (beats, on the project's grid, "
            "at "
            "the beat the note plays at), strength? 0-1, ends?}; cc.set {number, points [{beat, "
            "value}], channel?}; cc.clear "
            "{number, from?, to?}. Scale ops read the context track (key.set, chord.set) at "
            "the project beat each note starts, in the order of the ops: transpose_degrees "
            "{notes, degrees, key?} moves notes by scale degrees (a pitch outside the scale "
            "first goes down to the scale); snap_to_scale {notes? (all), mode? nearest|down|up, "
            "key?} moves notes outside the scale to it (nearest: a tie goes down); fit_to_chord "
            "{notes? (all), chord?} moves each note to the nearest tone of the chord (a tie "
            "goes down; a slash bass counts). key is \"Eb major\", chord is \"Cm7\"; given, they "
            "replace the context. A note that is looped or shared by another item in a "
            "different key or chord is refused with midi.context_varies; none holding is "
            "midi.no_key or midi.no_chord. On a clip they read the clip's own context (the key "
            "and chords it was written against, clip.set {context}) at the clip beat each note "
            "sounds, never the project's. On an item whose take follows (take.set {follow}) they "
            "read the take's own context at each note's context beat, and act on the notes as "
            "written: the answer says acts_on: \"written\" when some heard note sounds "
            "otherwise (take.bake writes what sounds). Returns the new indices of the notes the "
            "ops added "
            "or changed (to keep a selection).";
        c.tags = {"midi"};
        auto point = schema::object<CcPoint>("cc_point")
                         .field("beat", &CcPoint::beat, schema::number())
                         .field("value", &CcPoint::value, schema::integer().range(0, 127))
                         .build();
        auto opSchema =
            schema::object<MidiOp>("midi_op")
                .field("op", &MidiOp::op,
                       schema::enumeration<OpKind>({{OpKind::Add, "add"},
                                                    {OpKind::Remove, "remove"},
                                                    {OpKind::Move, "move"},
                                                    {OpKind::Resize, "resize"},
                                                    {OpKind::Velocity, "velocity"},
                                                    {OpKind::Quantize, "quantize"},
                                                    {OpKind::CcSet, "cc.set"},
                                                    {OpKind::CcClear, "cc.clear"},
                                                    {OpKind::TransposeDegrees, "transpose_degrees"},
                                                    {OpKind::SnapToScale, "snap_to_scale"},
                                                    {OpKind::FitToChord, "fit_to_chord"}}))
                .field("notes", &MidiOp::notes, schema::arrayOf(schema::integer().min(0)))
                .field("pitch", &MidiOp::pitch, schema::integer().range(0, 127))
                .field("start", &MidiOp::start, schema::number())
                .field("length", &MidiOp::length, schema::number().exclusiveMin(0))
                .field("velocity", &MidiOp::velocity, schema::integer().range(1, 127))
                .field("channel", &MidiOp::channel, schema::integer().range(1, 16))
                .field("beats", &MidiOp::beats, schema::number())
                .field("semitones", &MidiOp::semitones, schema::integer().range(-127, 127))
                .field("length_delta", &MidiOp::lengthDelta, schema::number())
                .field("start_delta", &MidiOp::startDelta, schema::number())
                .field("velocity_delta", &MidiOp::velocityDelta, schema::integer().range(-127, 127))
                .field("grid", &MidiOp::grid, schema::number().exclusiveMin(0))
                .field("strength", &MidiOp::strength, schema::number().range(0, 1))
                .field("ends", &MidiOp::ends, schema::boolean())
                .field("number", &MidiOp::number, schema::integer().range(0, 127))
                .field("points", &MidiOp::points, schema::arrayOf(point))
                .field("from", &MidiOp::from, schema::number())
                .field("to", &MidiOp::to, schema::number())
                .field("degrees", &MidiOp::degrees, schema::integer().range(-127, 127))
                .field(
                    "mode", &MidiOp::mode,
                    schema::enumeration<harmony::SnapMode>({{harmony::SnapMode::Nearest, "nearest"},
                                                            {harmony::SnapMode::Down, "down"},
                                                            {harmony::SnapMode::Up, "up"}}))
                .field("key", &MidiOp::key, schema::string())
                .field("chord", &MidiOp::chord, schema::string())
                .build();
        c.args = schema::object<EditArgs>("midi_edit_args")
                     .field("item", &EditArgs::item, ownerIdArg())
                     .field("ops", &EditArgs::ops, schema::arrayOf(opSchema))
                     .build();
        c.result = schema::any();
        c.examples = {
            {"Add a note and move the first one up an octave",
             j(R"({"item": "it_1", "ops": [{"op": "add", "pitch": 64, "start": 0.5, "length": 0.5}, {"op": "move", "notes": [0], "semitones": 12}]})"),
             j(R"({"notes": "<any>"})"), "song_with_item"},
            {"Move the first note up two degrees of C major",
             j(R"({"item": "it_1", "ops": [{"op": "transpose_degrees", "notes": [0], "degrees": 2, "key": "C major"}]})"),
             j(R"({"notes": "<any>"})"), "song_with_item"}};
        c.run = [](const EditArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& app = ctx.service<EngineApp>();
            const model::Session s = app.view();
            auto t = midiTarget(s, a.item);
            if (!t) return std::move(t).error();
            SUPPLE_TRY(checkUnlocked(s, t->item));
            midi::Sequence seq = t->midi->sequence;
            std::vector<Working> notes;
            notes.reserve(seq.notes.size());
            for (const midi::Note& n : seq.notes) notes.push_back({n, true, false});
            const std::size_t original = notes.size();
            for (std::size_t i = 0; i < a.ops.size(); ++i)
                SUPPLE_TRY(
                    applyOp(a.ops[i], i, notes, original, seq, t->frame, s, t->source, t->item));
            seq.notes.clear();
            std::vector<midi::Note> touched;
            for (const Working& w : notes)
                if (w.alive) {
                    seq.notes.push_back(w.note);
                    if (w.touched) touched.push_back(w.note);
                    auto end = noteEnd(w.note, "/ops");
                    if (!end) return end.error();
                    seq.length = std::max(seq.length, *end);
                }
            seq.canonicalize();
            json::Value selected = json::Value::array();
            std::set<std::size_t> seen;
            for (const midi::Note& n : touched) {
                const auto it = std::find(seq.notes.begin(), seq.notes.end(), n);
                if (it != seq.notes.end())
                    seen.insert(static_cast<std::size_t>(it - seq.notes.begin()));
            }
            for (std::size_t i : seen) selected.push_back(static_cast<std::int64_t>(i));
            model::MidiSource src = *t->midi;
            src.sequence = std::move(seq);
            app.txn().put<model::MidiSource>(t->source, std::move(src));
            json::Value out = json::Value::object();
            out["notes"] = std::move(selected);
            // The pitch-reading ops act on the notes as written; on a following take, say so
            // when some heard note sounds otherwise (what the user hears is not what was edited).
            if (followingTake(s, t->item)) {
                bool reads = false;
                for (const MidiOp& op : a.ops)
                    reads |= op.op == OpKind::TransposeDegrees || op.op == OpKind::SnapToScale ||
                             op.op == OpKind::FitToChord;
                if (reads)
                    for (const auto& h : model::soundingNotes(s, t->item, -1e18, 1e18))
                        if (h.channel != 9 && h.sounds.pitch != h.written) {
                            out["acts_on"] = "written";
                            break;
                        }
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }
}

}  // namespace supple::engine
