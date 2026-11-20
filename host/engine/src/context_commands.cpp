// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Context track commands (ADR-0095, session-model "Context track"): key and chord
// points, and what holds at a beat. Beats only; positions are rounded to 1/960 beat.
#include <cmath>

#include "command_helpers.h"
#include "supple/harmony/harmony.h"
#include "supple/model/context.h"

namespace supple::engine {

namespace {

using model::ContextPoint;
using model::ContextType;

// The grid the lead sheet and the notation use: the MIDI sequence's ppq.
double grid(double beat) { return std::round(beat * 960.0) / 960.0; }

struct KeySetArgs {
    double at = 0;
    std::optional<std::string> tonic, scale;
    std::optional<double> to;
};

struct ChordSetArgs {
    double at = 0;
    std::optional<std::string> symbol;
    std::optional<double> to;
};

struct AtArgs {
    double at = 0;
};

struct ContextAtArgs {
    std::optional<double> at;
    std::optional<std::int64_t> bar;
};

struct ContextGetArgs {
    std::optional<double> from, to;
};

struct KeyInfo {
    std::string id, tonic, scale;
    double fromBeat = 0;
    bool known = true;
};

struct ChordInfo {
    std::string id, symbol;
    double fromBeat = 0;
    bool known = true;
};

struct SectionInfo {
    std::string id, name;
    double start = 0, end = 0;
};

struct ContextAtResult {
    double beat = 0;
    std::int64_t bar = 1;
    double beatInBar = 1;
    std::optional<KeyInfo> key;
    std::optional<ChordInfo> chord;
    std::optional<SectionInfo> section;
};

struct PointRow {
    std::string id, type;
    double beat = 0;
    std::int64_t bar = 1;
    std::optional<std::string> tonic, scale, symbol;
};

struct ContextGetResult {
    std::vector<PointRow> points;
    std::optional<KeyInfo> key;  // what holds at `from`
    std::optional<ChordInfo> chord;
};

std::optional<Id> pointAt(const model::Session& s, ContextType type, double beat) {
    for (const auto& e : s.context)
        if (e.value->type == type && std::abs(e.value->beat - beat) < 1e-9) return e.id;
    return std::nullopt;
}

std::size_t countOf(const model::Session& s, ContextType type) {
    return static_cast<std::size_t>(std::count_if(
        s.context.begin(), s.context.end(), [&](const auto& e) { return e.value->type == type; }));
}

Error occupied(const char* what, double to) {
    return Error("context.occupied",
                 std::string("a ") + what + " point is already at beat " + std::to_string(to),
                 "/to", std::string("remove it first (") + what + ".remove) or pick another beat");
}

Error noPoint(const char* what, double at) {
    return Error("context.no_point",
                 std::string("no ") + what + " point at beat " + std::to_string(at), "/at",
                 std::string("use context.get to see where the points are"));
}

Error limit(const char* what) {
    return Error("context.limit",
                 std::string("a project holds at most ") +
                     std::to_string(model::kMaxContextPoints) + " " + what + " points",
                 "/at", std::string("remove points you no longer need"));
}

std::string scaleList() {
    std::string out;
    for (auto scale : harmony::allScales()) {
        if (!out.empty()) out += ", ";
        out += harmony::scaleName(scale);
    }
    return out;
}

// A tonic whose scale needs no double accidental and sounds the same, for the hint.
std::string enharmonic(const harmony::PitchClass& tonic, harmony::Scale scale) {
    for (char letter : std::string("CDEFGAB"))
        for (int accidental : {0, 1, -1}) {
            const harmony::PitchClass candidate{letter, accidental};
            if (harmony::pitchClassNumber(candidate) == harmony::pitchClassNumber(tonic) &&
                harmony::keySupported({candidate, scale}))
                return harmony::printPitchClass(candidate);
        }
    return {};
}

KeyInfo keyInfo(Id id, const ContextPoint& p) {
    return KeyInfo{id.str(), p.tonic, p.scale, p.beat, model::parseKey(p).has_value()};
}

ChordInfo chordInfo(Id id, const ContextPoint& p) {
    return ChordInfo{id.str(), p.symbol, p.beat, model::parseChordPoint(p).has_value()};
}

std::int64_t barOf(const model::Session& s, double beat) { return s.timeMap->barBeat(beat).bar; }

void fillHolding(const model::Session& s, double beat, std::optional<KeyInfo>& key,
                 std::optional<ChordInfo>& chord) {
    if (const auto id = model::keyPointAt(s, beat))
        if (const auto* p = s.context.find(*id)) key = keyInfo(*id, *p);
    if (const auto id = model::chordPointAt(s, beat))
        if (const auto* p = s.context.find(*id); p && !p->symbol.empty())
            chord = chordInfo(*id, *p);  // an empty symbol is "no chord from here"
}

auto keyInfoSchema() {
    return schema::object<KeyInfo>("context_key")
        .field("id", &KeyInfo::id, schema::string())
        .field("tonic", &KeyInfo::tonic, schema::string())
        .field("scale", &KeyInfo::scale, schema::string())
        .field("from_beat", &KeyInfo::fromBeat, schema::number())
        .field("known", &KeyInfo::known, schema::boolean())
        .build();
}

auto chordInfoSchema() {
    return schema::object<ChordInfo>("context_chord")
        .field("id", &ChordInfo::id, schema::string())
        .field("symbol", &ChordInfo::symbol, schema::string())
        .field("from_beat", &ChordInfo::fromBeat, schema::number())
        .field("known", &ChordInfo::known, schema::boolean())
        .build();
}

}  // namespace

void registerContextCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- key.set
    [&] {
        Command<KeySetArgs, IdResult> c;
        c.name = "key.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Set the key from a beat: tonic and scale, until the next key point.";
        c.description =
            "Adds a key point at `at` (project beats, rounded to 1/960) or changes the one "
            "there. `to` moves the point at `at` to another beat and keeps its id. tonic is a "
            "letter and at most one # or b (Eb, F#); scale is one of: " +
            scaleList() +
            ". A key that needs a double accidental (G# major) is refused: use the "
            "enharmonic one (Ab major). Notation, the piano roll and midi.edit read it.";
        c.tags = {"context"};
        c.args = schema::object<KeySetArgs>("key_set_args")
                     .field("at", &KeySetArgs::at, schema::number().min(0))
                     .field("tonic", &KeySetArgs::tonic, schema::string())
                     .field("scale", &KeySetArgs::scale, schema::string())
                     .field("to", &KeySetArgs::to, schema::number().min(0))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"E-flat major from the start", j(R"({"at":0,"tonic":"Eb","scale":"major"})"),
                       j(R"({"id":"cx_1"})"), "empty"}};
        c.run = [](const KeySetArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            model::EditTxn& txn = app(ctx).txn();
            const double at = grid(a.at);
            const auto existing = pointAt(s, ContextType::Key, at);
            if (a.to && !existing) return noPoint("key", at);
            ContextPoint p;
            p.type = ContextType::Key;
            p.beat = at;
            if (existing) p = *s.context.find(*existing);
            if (a.tonic) {
                const auto tonic = harmony::parsePitchClass(*a.tonic);
                if (!tonic)
                    return Error("key.tonic", "'" + *a.tonic + "' is not a pitch class", "/tonic",
                                 "use a letter and at most one # or b: C, Eb, F#");
                p.tonic = harmony::printPitchClass(*tonic);
            }
            if (a.scale) {
                const auto scale = harmony::parseScale(*a.scale);
                if (!scale)
                    return Error("scale.unknown", "'" + *a.scale + "' is not a scale", "/scale",
                                 "scales: " + scaleList());
                p.scale = std::string(harmony::scaleName(*scale));
            }
            if (p.tonic.empty() || p.scale.empty())
                return Error("key.args", "a new key point needs a tonic and a scale", "/tonic",
                             "give both, for example {\"tonic\":\"Eb\",\"scale\":\"major\"}");
            const auto key = model::parseKey(p);
            if (!key)  // an existing point from a later build, changed only in part
                return Error("scale.unknown", "'" + p.scale + "' is not a scale this build knows",
                             "/scale", "scales: " + scaleList());
            if (!harmony::keySupported(*key)) {
                const std::string other = enharmonic(key->tonic, key->scale);
                return Error("key.unsupported",
                             p.tonic + " " + p.scale + " needs a double accidental", "/tonic",
                             other.empty() ? "pick another tonic"
                                           : "use the enharmonic key: " + other + " " + p.scale);
            }
            if (a.to) {
                const double to = grid(*a.to);
                if (to != at && pointAt(s, ContextType::Key, to)) return occupied("key", to);
                p.beat = to;
                txn.put(*existing, std::move(p));
                return IdResult{existing->str()};
            }
            if (existing) {
                txn.put(*existing, std::move(p));
                return IdResult{existing->str()};
            }
            if (countOf(s, ContextType::Key) >= model::kMaxContextPoints) return limit("key");
            return IdResult{txn.create(std::move(p)).str()};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- key.remove
    [&] {
        Command<AtArgs, Empty> c;
        c.name = "key.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove the key point at a beat: the key before it continues.";
        c.tags = {"context"};
        c.args = schema::object<AtArgs>("key_remove_args")
                     .field("at", &AtArgs::at, schema::number().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Remove the key at the start", j(R"({"at":0})"), j("{}"), "song_with_context"}};
        c.run = [](const AtArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            const double at = grid(a.at);
            const auto id = pointAt(s, ContextType::Key, at);
            if (!id) return noPoint("key", at);
            app(ctx).txn().remove<ContextPoint>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- chord.set
    [&] {
        Command<ChordSetArgs, IdResult> c;
        c.name = "chord.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Set the chord from a beat, until the next chord point; \"\" ends it.";
        c.description =
            "Adds a chord point at `at` (project beats, rounded to 1/960) or changes the one "
            "there. `to` moves the point at `at` to another beat and keeps its id. symbol is "
            "the notation's chord grammar: a root, a quality and an optional /bass (Cm7, Bb, "
            "F#7#9, C/E); an empty symbol means no chord from here. The symbol is stored in "
            "canonical form (Cmin7 becomes Cm7).";
        c.tags = {"context"};
        c.args = schema::object<ChordSetArgs>("chord_set_args")
                     .field("at", &ChordSetArgs::at, schema::number().min(0))
                     .field("symbol", &ChordSetArgs::symbol, schema::string())
                     .field("to", &ChordSetArgs::to, schema::number().min(0))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"C minor seventh at bar 2", j(R"({"at":4,"symbol":"Cm7"})"),
                       j(R"({"id":"cx_1"})"), "empty"}};
        c.run = [](const ChordSetArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            model::EditTxn& txn = app(ctx).txn();
            const double at = grid(a.at);
            const auto existing = pointAt(s, ContextType::Chord, at);
            if (a.to && !existing) return noPoint("chord", at);
            ContextPoint p;
            p.type = ContextType::Chord;
            p.beat = at;
            if (existing) p = *s.context.find(*existing);
            if (a.symbol) {
                if (a.symbol->empty()) {
                    p.symbol.clear();
                } else {
                    auto chord = harmony::parseChord(*a.symbol);
                    if (!chord) {
                        Error e = chord.error();
                        e.path = "/symbol";
                        return e;
                    }
                    p.symbol = harmony::printChord(*chord);
                }
            } else if (!existing) {
                return Error("chord.args", "a new chord point needs a symbol", "/symbol",
                             "give one, for example {\"symbol\":\"Cm7\"}");
            }
            if (a.to) {
                const double to = grid(*a.to);
                if (to != at && pointAt(s, ContextType::Chord, to)) return occupied("chord", to);
                p.beat = to;
                txn.put(*existing, std::move(p));
                return IdResult{existing->str()};
            }
            if (existing) {
                txn.put(*existing, std::move(p));
                return IdResult{existing->str()};
            }
            if (countOf(s, ContextType::Chord) >= model::kMaxContextPoints) return limit("chord");
            return IdResult{txn.create(std::move(p)).str()};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- chord.remove
    [&] {
        Command<AtArgs, Empty> c;
        c.name = "chord.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove the chord point at a beat: the chord before it continues.";
        c.tags = {"context"};
        c.args = schema::object<AtArgs>("chord_remove_args")
                     .field("at", &AtArgs::at, schema::number().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Remove the chord at the start", j(R"({"at":0})"), j("{}"), "song_with_context"}};
        c.run = [](const AtArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            const double at = grid(a.at);
            const auto id = pointAt(s, ContextType::Chord, at);
            if (!id) return noPoint("chord", at);
            app(ctx).txn().remove<ContextPoint>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- context.at
    [&] {
        Command<ContextAtArgs, ContextAtResult> c;
        c.name = "context.at";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "What key, chord and section hold at a beat or bar.";
        c.description =
            "Give `at` (project beats) or `bar` (1-based). The key and chord are the last "
            "points at or before it; a section is a region with section true that covers "
            "it (the latest start wins). known false means the point is there but its scale "
            "or chord symbol is not one this build reads.";
        c.tags = {"context"};
        c.args = schema::object<ContextAtArgs>("context_at_args")
                     .field("at", &ContextAtArgs::at, schema::number().min(0))
                     .field("bar", &ContextAtArgs::bar, schema::integerOf<std::int64_t>().min(1))
                     .build();
        c.result = schema::object<ContextAtResult>("context_at")
                       .field("beat", &ContextAtResult::beat, schema::number())
                       .field("bar", &ContextAtResult::bar, schema::integer())
                       .field("beat_in_bar", &ContextAtResult::beatInBar, schema::number())
                       .field("key", &ContextAtResult::key, keyInfoSchema())
                       .field("chord", &ContextAtResult::chord, chordInfoSchema())
                       .field("section", &ContextAtResult::section,
                              schema::object<SectionInfo>("context_section")
                                  .field("id", &SectionInfo::id, schema::string())
                                  .field("name", &SectionInfo::name, schema::string())
                                  .field("start", &SectionInfo::start, schema::number())
                                  .field("end", &SectionInfo::end, schema::number())
                                  .build())
                       .build();
        c.examples = {{"At bar 2", j(R"({"bar":2})"), j(R"({"beat":4,"bar":2,"beat_in_bar":1,
                             "key":{"id":"cx_1","tonic":"Eb","scale":"major","from_beat":0,"known":true},
                             "chord":{"id":"cx_3","symbol":"Cm7","from_beat":4,"known":true},
                             "section":{"id":"rg_1","name":"Verse","start":0,"end":8}})"),
                       "song_with_context"}};
        c.run = [](const ContextAtArgs& a, Context& ctx) -> Result<ContextAtResult> {
            const auto s = app(ctx).view();
            if (!a.at == !a.bar)
                return Error("context.args", "give at or bar, one of them", "/at",
                             "for example {\"bar\":5} or {\"at\":16}");
            const double beat = a.at ? *a.at : s.timeMap->beatOfBar(static_cast<int>(*a.bar));
            ContextAtResult r;
            r.beat = beat;
            const auto bb = s.timeMap->barBeat(beat);
            r.bar = bb.bar;
            r.beatInBar = bb.beat + bb.fraction;
            fillHolding(s, beat, r.key, r.chord);
            if (const auto id = model::sectionAt(s, beat)) {
                const auto* region = s.regions.find(*id);
                const auto inBeats = [&](double pos) {
                    return region->timebase == model::Timebase::Time ? s.timeMap->beatAt(pos) : pos;
                };
                r.section = SectionInfo{id->str(), region->name, inBeats(region->start),
                                        inBeats(region->end)};
            }
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- context.get
    [&] {
        Command<ContextGetArgs, ContextGetResult> c;
        c.name = "context.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List the key and chord points in a range of beats.";
        c.description =
            "Points from `from` (default 0) to `to` (default: the end), by beat, with the "
            "key and chord that hold at `from`.";
        c.tags = {"context"};
        c.args = schema::object<ContextGetArgs>("context_get_args")
                     .field("from", &ContextGetArgs::from, schema::number().min(0))
                     .field("to", &ContextGetArgs::to, schema::number().min(0))
                     .build();
        c.result =
            schema::object<ContextGetResult>("context_get")
                .field("points", &ContextGetResult::points,
                       schema::arrayOf(schema::object<PointRow>("context_point_row")
                                           .field("id", &PointRow::id, schema::string())
                                           .field("type", &PointRow::type, schema::string())
                                           .field("beat", &PointRow::beat, schema::number())
                                           .field("bar", &PointRow::bar, schema::integer())
                                           .field("tonic", &PointRow::tonic, schema::string())
                                           .field("scale", &PointRow::scale, schema::string())
                                           .field("symbol", &PointRow::symbol, schema::string())
                                           .build()))
                .field("key", &ContextGetResult::key, keyInfoSchema())
                .field("chord", &ContextGetResult::chord, chordInfoSchema())
                .build();
        c.examples = {{"Nothing yet", j("{}"), j(R"({"points":[]})"), "empty"}};
        c.run = [](const ContextGetArgs& a, Context& ctx) -> Result<ContextGetResult> {
            const auto s = app(ctx).view();
            const double from = a.from.value_or(0);
            ContextGetResult out;
            std::vector<std::pair<double, Id>> rows;
            for (const auto& e : s.context) {
                const double beat = e.value->beat;
                if (beat < from || (a.to && beat > *a.to)) continue;
                rows.push_back({beat, e.id});
            }
            std::sort(rows.begin(), rows.end());
            for (const auto& [beat, id] : rows) {
                const ContextPoint& p = *s.context.find(id);
                PointRow row;
                row.id = id.str();
                row.type = p.type == ContextType::Key ? "key" : "chord";
                row.beat = beat;
                row.bar = barOf(s, beat);
                if (p.type == ContextType::Key) {
                    row.tonic = p.tonic;
                    row.scale = p.scale;
                } else {
                    row.symbol = p.symbol;
                }
                out.points.push_back(std::move(row));
            }
            fillHolding(s, from, out.key, out.chord);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
