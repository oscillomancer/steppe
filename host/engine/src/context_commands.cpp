// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Context track commands (ADR-0095, session-model "Context track"): key and chord
// points, and what holds at a beat. Beats only; positions are rounded to 1/960 beat.
#include <cmath>

#include "command_helpers.h"
#include "lead_sheet.h"
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

struct WriteArgs {
    std::string text;
};

struct WriteResult {
    std::int64_t firstBar = 1, lastBar = 1, added = 0, replaced = 0, removed = 0, unchanged = 0;
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
    std::optional<std::string> text;  // the lead sheet over the range
    std::optional<KeyInfo> key;       // what holds at `from`
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

// The effects of key.set, chord.set and the removes, shared with context.write.
Result<IdResult> keySet(const model::Session& s, model::EditTxn& txn, const KeySetArgs& a) {
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
        return Error("scale.unknown", "'" + p.scale + "' is not a scale this build knows", "/scale",
                     "scales: " + scaleList());
    if (!harmony::keySupported(*key)) {
        const std::string other = enharmonic(key->tonic, key->scale);
        return Error("key.unsupported", p.tonic + " " + p.scale + " needs a double accidental",
                     "/tonic",
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
}

Result<IdResult> chordSet(const model::Session& s, model::EditTxn& txn, const ChordSetArgs& a) {
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
}

Result<Empty> removePoint(const model::Session& s, model::EditTxn& txn, ContextType type,
                          double at) {
    at = grid(at);
    const auto id = pointAt(s, type, at);
    if (!id) return noPoint(type == ContextType::Key ? "key" : "chord", at);
    txn.remove<ContextPoint>(*id);
    return Empty{};
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
        c.summary = "Set the key from a beat: tonic and scale.";
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
            return keySet(app(ctx).view(), app(ctx).txn(), a);
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- key.remove
    [&] {
        Command<AtArgs, Empty> c;
        c.name = "key.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove the key point at a beat.";
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
        c.summary = "Set the chord from a beat; \"\" ends it.";
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
            return chordSet(app(ctx).view(), app(ctx).txn(), a);
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- chord.remove
    [&] {
        Command<AtArgs, Empty> c;
        c.name = "chord.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove the chord point at a beat.";
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
            "key and chord that hold at `from`, and `text`: the same as a lead sheet in "
            "project bars, one line per bar, for context.write. In it `{Cm7}:h` is a chord "
            "point at the cursor, `r:h` a chord point with no chord, `~:h` a hold (no "
            "point), `key Eb major` a key point at the cursor; a bar's durations fill the "
            "bar. The bars run from the one holding `from` to the one holding `to` "
            "(exclusive when `to` is a bar line), or to the last point.";
        c.tags = {"context"};
        c.args = schema::object<ContextGetArgs>("context_get_args")
                     .field("from", &ContextGetArgs::from, schema::number().min(0))
                     .field("to", &ContextGetArgs::to, schema::number().min(0))
                     .build();
        c.result =
            schema::object<ContextGetResult>("context_get")
                .field("text", &ContextGetResult::text, schema::string())
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
            int firstBar = static_cast<int>(barOf(s, from)), lastBar = firstBar;
            bool any = a.to.has_value();
            if (a.to) {
                lastBar = static_cast<int>(barOf(s, *a.to));
                if (*a.to > from && grid(s.timeMap->beatOfBar(lastBar)) == grid(*a.to)) --lastBar;
            } else if (!rows.empty()) {
                any = true;
                lastBar = static_cast<int>(barOf(s, rows.back().first));
            }
            lastBar = std::max(lastBar, firstBar);
            if (any) {
                if (lastBar - firstBar >= detail::kLeadSheetMaxBars)
                    return Error("context.too_long",
                                 "the range covers more than " +
                                     std::to_string(detail::kLeadSheetMaxBars) + " bars",
                                 "/to", "ask for a narrower range with from and to");
                out.text = detail::printLeadSheet(s, firstBar, lastBar);
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- context.write
    [&] {
        Command<WriteArgs, WriteResult> c;
        c.name = "context.write";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Write key and chord points from a lead sheet.";
        c.description =
            "text is context.get's lead sheet: one line per project bar, 'N: ' then "
            "{SYMBOL}:DUR (a chord point at the cursor), r:DUR (a chord point with no "
            "chord), ~:DUR (hold: no point) and 'key TONIC SCALE' (a key point at the "
            "cursor, no time), where DUR is the notation's duration (q, h., h+q, 480t) and a "
            "bar's durations must fill it. Bars run in order from the first named to the "
            "last, one per line or, as in notation, after a ; ('1: ... ; 2: ...') or after a "
            "| token ('1: ... | 2: ...', the number after | optional). The write is a diff over "
            "that span: points equal in beat and content are "
            "left alone (ids kept), points in the span the text omits are removed, the rest "
            "are added or replaced. History records the key.set, chord.set and remove "
            "operations it came to, so branches that write different bars merge. Keys and "
            "chords both follow the text: to leave the key alone, include it. Errors name "
            "the bar; nothing is changed when one occurs.";
        c.tags = {"context"};
        c.args = schema::object<WriteArgs>("context_write_args")
                     .field("text", &WriteArgs::text, schema::string())
                     .build();
        c.result = schema::object<WriteResult>("context_write")
                       .field("first_bar", &WriteResult::firstBar, schema::integer())
                       .field("last_bar", &WriteResult::lastBar, schema::integer())
                       .field("added", &WriteResult::added, schema::integer())
                       .field("replaced", &WriteResult::replaced, schema::integer())
                       .field("removed", &WriteResult::removed, schema::integer())
                       .field("unchanged", &WriteResult::unchanged, schema::integer())
                       .build();
        c.examples = {{"Two bars of changes",
                       j(R"({"text":"1: key Eb major {Ebmaj7}:w\n2: {Cm7}:h {Fm7}:h\n"})"),
                       j(R"({"first_bar":1,"last_bar":2,"added":4,"replaced":0,"removed":0,
                             "unchanged":0})"),
                       "empty"}};
        c.run = [](const WriteArgs& a, Context& ctx) -> Result<WriteResult> {
            EngineApp& engine = app(ctx);
            const auto s = engine.view();
            auto sheet = detail::parseLeadSheet(s, a.text);
            if (!sheet) return std::move(sheet).error();
            auto tickOf = [](double beat) { return std::llround(beat * 960.0); };
            // What the span holds now, by type and tick.
            std::map<std::pair<ContextType, long long>, Id> existing;
            for (const auto& e : s.context)
                if (e.value->beat >= sheet->fromBeat - 1e-9 && e.value->beat < sheet->toBeat - 1e-9)
                    existing[{e.value->type, tickOf(e.value->beat)}] = e.id;
            WriteResult out;
            out.firstBar = sheet->firstBar;
            out.lastBar = sheet->lastBar;
            std::set<std::pair<ContextType, long long>> wanted;
            struct Step {
                const detail::LeadPoint* point;
                bool replace;
            };
            std::vector<Step> sets;
            for (const detail::LeadPoint& p : sheet->points) {
                const auto key = std::make_pair(p.type, tickOf(p.beat));
                wanted.insert(key);
                const auto found = existing.find(key);
                if (found == existing.end()) {
                    sets.push_back({&p, false});
                    continue;
                }
                const ContextPoint& old = *s.context.find(found->second);
                const bool same = p.type == ContextType::Key
                                      ? old.tonic == p.tonic && old.scale == p.scale
                                      : old.symbol == p.symbol;
                if (same)
                    ++out.unchanged;
                else
                    sets.push_back({&p, true});
            }
            // Removes first, by beat, then the sets; each is recorded as the command that
            // does it, so a merge replays them (and not this text).
            std::vector<std::pair<double, ContextType>> removes;
            for (const auto& [key, id] : existing)
                if (!wanted.contains(key)) removes.push_back({s.context.find(id)->beat, key.first});
            std::sort(removes.begin(), removes.end());
            for (const ContextType type : {ContextType::Key, ContextType::Chord}) {
                std::size_t total = countOf(s, type);
                for (const auto& r : removes) total -= r.second == type ? 1 : 0;
                for (const Step& step : sets) total += !step.replace && step.point->type == type;
                if (total > model::kMaxContextPoints)
                    return Error("context.limit",
                                 "the write would leave " + std::to_string(total) +
                                     (type == ContextType::Key ? " key" : " chord") +
                                     " points; a project holds at most " +
                                     std::to_string(model::kMaxContextPoints),
                                 "/text", "write fewer changes, or remove points first");
            }
            for (const auto& [beat, type] : removes) {
                SUPPLE_TRY(removePoint(s, engine.txn(), type, beat));
                json::Value args = json::Value::object();
                args["at"] = beat;
                engine.recordInvocation(type == ContextType::Key ? "key.remove" : "chord.remove",
                                        std::move(args));
                ++out.removed;
            }
            for (const Step& step : sets) {
                const detail::LeadPoint& p = *step.point;
                json::Value args = json::Value::object();
                args["at"] = p.beat;
                if (p.type == ContextType::Key) {
                    args["tonic"] = p.tonic;
                    args["scale"] = p.scale;
                    KeySetArgs set;
                    set.at = p.beat;
                    set.tonic = p.tonic;
                    set.scale = p.scale;
                    SUPPLE_TRY(keySet(s, engine.txn(), set));
                    engine.recordInvocation("key.set", std::move(args));
                } else {
                    args["symbol"] = p.symbol;
                    ChordSetArgs set;
                    set.at = p.beat;
                    set.symbol = p.symbol;
                    SUPPLE_TRY(chordSet(s, engine.txn(), set));
                    engine.recordInvocation("chord.set", std::move(args));
                }
                step.replace ? ++out.replaced : ++out.added;
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
