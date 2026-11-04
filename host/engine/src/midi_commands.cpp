// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "command_validation.h"
#include "heard_midi.h"
#include "midi_ticks.h"
// Note-level MIDI editing (the piano roll's commands, and an agent's when
// notation is the wrong tool): midi.notes reads an item's notes and CCs in
// beats from the item's start, midi.edit applies ordered operations to them.
#include <algorithm>
#include <cmath>
#include <set>

#include "supple/engine/app.h"
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

enum class OpKind : std::uint8_t { Add, Remove, Move, Resize, Velocity, Quantize, CcSet, CcClear };

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
};

struct EditArgs {
    std::string item;
    std::vector<MidiOp> ops;
};

struct Frame {  // item beats <-> source ticks
    double ppq = 960, offset = 0, rate = 1, itemPosition = 0;
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

Result<Target> midiTarget(const model::Session& s, const std::string& text) {
    auto id = Id::parse(text);
    const model::Item* it = id && id->kind == model::Kind::Item ? s.items.find(*id) : nullptr;
    if (!it)
        return Error("it.unknown", "no " + text, "/item",
                     "use an item id from track.list or item.add");
    const model::Take* take = it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
    if (!take || !take->source || take->source->kind != model::Kind::Midi)
        return Error("midi.not_midi", text + " has no MIDI take", "/item",
                     "use item.add with notation to create a MIDI item");
    const model::MidiSource* midi = s.midi.find(*take->source);
    if (!midi)
        return Error("mi.unknown", take->source->str() + " is missing from the model", "/item",
                     "load a project containing the item's MIDI source, or recreate the item with "
                     "item.add notation");
    Target t{*id, *it->activeTake, *take->source, midi, {}};
    t.frame.ppq = midi->sequence.ppq;
    t.frame.offset = take->startOffset;
    t.frame.rate = take->playrate > 0 ? take->playrate : 1.0;
    t.frame.itemPosition = model::itemSpan(s, *it).startBeat;
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

Result<json::Value> describe(const model::Session& s, const Target& t) {
    const midi::Sequence& seq = t.midi->sequence;
    json::Value out = json::Value::object();
    out["item"] = t.item.str();
    out["ppq"] = static_cast<std::int64_t>(seq.ppq);
    out["length"] = roundBeats(model::itemLengthBeats(s, *s.items.find(t.item)));
    out["position"] = roundBeats(t.frame.itemPosition);
    if (t.midi->pooled) out["pooled"] = true;
    json::Value notes = json::Value::array();
    for (const midi::Note& n : seq.notes) notes.push_back(noteJson(n, t.frame));
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
               midi::Sequence& seq, const Frame& f) {
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
            // The project's grid: snap project beats, not item beats.
            auto snap = [&](double itemBeat) {
                const double project = f.itemPosition + itemBeat;
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
    midi::Sequence merged;
    merged.ppq = 960;
    for (const auto& [id, take] : parts) {
        const model::Item& it = *s.items.find(id);
        const model::Take& tk = *s.takes.find(take);
        detail::heardMidi(s, it, tk, s.midi.find(*tk.source)->sequence,
                          model::itemSpan(s, it).startBeat - start, merged);
    }
    merged.length = std::llround((end - start) * merged.ppq);
    merged.canonicalize();
    const model::Item& kept = *s.items.find(base);
    const Id heard = kept.activeTake ? *kept.activeTake : kept.takes.front();
    model::Take take = *s.takes.find(heard);
    take.source = txn.create(model::MidiSource{std::move(merged), false});
    take.startOffset = 0;
    take.playrate = 1;
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
        Command<ItemArgs, json::Value> c;
        c.name = "midi.notes";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List a MIDI item's notes and CC points (beats from the item's start).";
        c.description =
            "Notes are in a fixed order (by start, then pitch); midi.edit refers to them by their "
            "position in this list. start/length/beat are in beats from the item's start; channel "
            "is 1-16 (omitted for 1). pooled: other items share this MIDI, and edits change them "
            "too. For writing whole phrases, midi.write with notation is usually better.";
        c.tags = {"midi"};
        c.args =
            schema::object<ItemArgs>("midi_notes_args")
                .field("item", &ItemArgs::item, schema::string().pattern("^it_[0-9]+$", "item id"))
                .build();
        c.result = schema::any();
        c.examples = {
            {"Notes of a MIDI item", j(R"({"item": "it_1"})"),
             j(R"({"item": "it_1", "ppq": 960, "length": "<any>", "position": "<any>", "notes": "<any>", "cc": "<any>"})"),
             "song_with_item"}};
        c.run = [](const ItemArgs& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = ctx.service<EngineApp>().view();
            auto t = midiTarget(s, a.item);
            if (!t) return std::move(t).error();
            return describe(s, *t);
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<EditArgs, json::Value> c;
        c.name = "midi.edit";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Edit a MIDI item's notes and CCs: add, remove, move, resize, velocity, quantize.";
        c.description =
            "ops run in order and are one undo step. notes: indices into midi.notes' list as it "
            "was before this command (every op refers to that list, so later ops are not "
            "affected by earlier removals). Beats are from the item's start. add {pitch, start, "
            "length, velocity?, channel?}; remove {notes}; move {notes, beats?, semitones?}; "
            "resize {notes, length? | length_delta? | start_delta?}; velocity {notes, velocity? "
            "and/or velocity_delta?}; quantize {notes? (all), grid (beats, on the project's grid), "
            "strength? 0-1, ends?}; cc.set {number, points [{beat, value}], channel?}; cc.clear "
            "{number, from?, to?}. Returns the new indices of the notes the ops added or changed "
            "(to keep a selection).";
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
                                                    {OpKind::CcClear, "cc.clear"}}))
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
                .build();
        c.args =
            schema::object<EditArgs>("midi_edit_args")
                .field("item", &EditArgs::item, schema::string().pattern("^it_[0-9]+$", "item id"))
                .field("ops", &EditArgs::ops, schema::arrayOf(opSchema))
                .build();
        c.result = schema::any();
        c.examples = {
            {"Add a note and move the first one up an octave",
             j(R"({"item": "it_1", "ops": [{"op": "add", "pitch": 64, "start": 0.5, "length": 0.5}, {"op": "move", "notes": [0], "semitones": 12}]})"),
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
                SUPPLE_TRY(applyOp(a.ops[i], i, notes, original, seq, t->frame));
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
            return out;
        };
        addChecked(registry, std::move(c));
    }
}

}  // namespace supple::engine
