// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "clip_context.h"

#include <algorithm>
#include <cmath>

#include "lead_sheet.h"
#include "supple/model/context.h"
#include "supple/model/launcher.h"

namespace supple::engine::detail {
namespace {

std::string barWords(const model::Session& s, double beat) {
    return "bar " + std::to_string(s.timeMap->barBeat(std::max(0.0, beat)).bar);
}

void sortPoints(std::vector<model::ContextPoint>& points) {
    std::stable_sort(points.begin(), points.end(),
                     [](const model::ContextPoint& a, const model::ContextPoint& b) {
                         return a.beat != b.beat ? a.beat < b.beat : a.type < b.type;
                     });
}

}  // namespace

std::string clipContextText(const model::Session& s, const model::Clip& clip) {
    if (clip.context.empty()) return {};
    int last = 1;
    if (clip.kind == model::ClipKind::Midi)
        last = s.timeMap->barBeat(std::max(0.0, clip.length - 1.0 / 960.0)).bar;
    for (const model::ContextPoint& p : clip.context)
        last = std::max(last, s.timeMap->barBeat(p.beat).bar);
    return printLeadSheetPoints(clip.context, *s.timeMap, 1, last);
}

Result<std::vector<model::ContextPoint>> clipContextFromText(const model::Session& s,
                                                             const std::string& text,
                                                             model::ClipKind kind, double length,
                                                             const std::string& path) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::vector<model::ContextPoint>{};
    auto sheet = parseLeadSheet(s, text);
    if (!sheet) {
        Error e = std::move(sheet).error();
        const auto fix = [&](Error& x) {
            if (x.code == "context.syntax") x.code = "clip.context_syntax";
            x.path = path;
        };
        fix(e);
        for (auto& d : e.details) fix(d);
        return e;
    }
    std::vector<model::ContextPoint> points;
    std::size_t keys = 0, chords = 0;
    for (const LeadPoint& p : sheet->points) {
        const bool key = p.type == model::ContextType::Key;
        const std::string where = barWords(s, p.beat);
        if (kind == model::ClipKind::Audio && (!key || p.beat != 0))
            return Error("clip.context_audio",
                         where + ": an audio clip's context is a key at the start of bar 1", path,
                         "write key TONIC SCALE at the start of bar 1 and hold with ~:w; "
                         "chords and later keys are for MIDI clips");
        if (kind == model::ClipKind::Midi && p.beat >= length - 1e-9)
            return Error("clip.context_range",
                         where + ": the " + (key ? "key" : "chord") + " lies at or past the " +
                             "clip's end (" + json::formatDouble(length) + " beats)",
                         path,
                         "move it earlier, or lengthen the clip first (clip.set length); bars may "
                         "run past the clip's end, points may not");
        (key ? keys : chords)++;
        model::ContextPoint q;
        q.beat = p.beat;
        q.type = p.type;
        q.tonic = p.tonic;
        q.scale = p.scale;
        q.symbol = p.symbol;
        points.push_back(std::move(q));
    }
    if (keys > model::kMaxContextPoints || chords > model::kMaxContextPoints)
        return Error("context.limit",
                     "a clip holds at most " + std::to_string(model::kMaxContextPoints) +
                         " key points and as many chord points",
                     path, "write fewer changes");
    sortPoints(points);
    return points;
}

ContextTaken contextUnder(const model::Session& s, double fromBeat, double lengthBeats,
                          bool keyOnly) {
    ContextTaken out;
    // A point this build cannot read is not taken, and is named: `at` is its beat in the clip.
    struct Dropped {
        bool key;
        double at;
        std::string what;
    };
    std::vector<Dropped> dropped;
    const auto readable = [&](const model::ContextPoint& p, double at) {
        const bool key = p.type == model::ContextType::Key;
        if (key ? model::parseKey(p).has_value()
                : p.symbol.empty() || model::parseChordPoint(p).has_value())
            return true;
        dropped.push_back({key, at,
                           (key ? "key " + p.tonic + " " + p.scale : "chord " + p.symbol) + " in " +
                               barWords(s, p.beat)});
        return false;
    };
    if (const auto id = model::keyPointAt(s, fromBeat))
        if (const model::ContextPoint* p = s.context.find(*id)) {
            out.key = true;
            if (readable(*p, 0)) {
                model::ContextPoint q = *p;
                q.beat = 0;
                out.points.push_back(std::move(q));
            }
        }
    const auto message = [](const std::string& what, const char* then) {
        return "dropped " + what + ": this build cannot read it, so the clip does not take it" +
               then;
    };
    if (keyOnly) {
        for (const Dropped& d : dropped) out.unreadable.push_back(message(d.what, ""));
        return out;
    }
    if (const auto id = model::chordPointAt(s, fromBeat))
        if (const model::ContextPoint* p = s.context.find(*id); p && !p->symbol.empty())
            if (readable(*p, 0)) {
                model::ContextPoint q = *p;
                q.beat = 0;
                out.points.push_back(std::move(q));
            }
    for (const auto& e : s.context) {
        const double at = e.value->beat - fromBeat;
        if (at <= 1e-9 || at >= lengthBeats - 1e-9) continue;
        if (!readable(*e.value, at)) continue;
        model::ContextPoint q = *e.value;
        q.beat = at;
        out.points.push_back(std::move(q));
    }
    sortPoints(out.points);
    // Leaving a point out must not let the one before it carry on over its stretch: where the
    // project reads no chord, the clip says no chord; there is no "no key" to write, so the
    // answer says the earlier key holds.
    const auto before = [&](model::ContextType type, double at) {
        const model::ContextPoint* last = nullptr;
        for (const model::ContextPoint& p : out.points)
            if (p.type == type && p.beat < at - 1e-9) last = &p;
        return last;
    };
    std::vector<model::ContextPoint> none;
    for (const Dropped& d : dropped) {
        const char* then = "";
        if (d.key) {
            if (before(model::ContextType::Key, d.at))
                then = "; the clip's earlier key carries on over that stretch";
        } else if (const model::ContextPoint* last = before(model::ContextType::Chord, d.at);
                   last && !last->symbol.empty()) {
            model::ContextPoint gap;
            gap.beat = d.at;
            gap.type = model::ContextType::Chord;  // an empty symbol is "no chord from here"
            none.push_back(std::move(gap));
            then = "; the clip holds no chord from there";
        }
        out.unreadable.push_back(message(d.what, then));
    }
    if (!none.empty()) {
        for (auto& p : none) out.points.push_back(std::move(p));
        sortPoints(out.points);
    }
    return out;
}

std::vector<ClipNote> unreadableNotes(const ContextTaken& taken, const std::string& path) {
    std::vector<ClipNote> notes;
    for (const std::string& what : taken.unreadable)
        notes.push_back({"clip.context_dropped", what, path});
    return notes;
}

SymbolChords chordsFromSymbols(const model::Session& s,
                               const std::vector<notation::ChordSymbol>& symbols, int ppq,
                               double offset, double rate, double length) {
    SymbolChords out;
    if (ppq <= 0 || !(rate > 0)) return out;
    std::size_t beyond = 0, conflicts = 0;
    std::optional<double> firstBeyond, firstConflict;
    std::int64_t lastTick = -1;
    std::optional<harmony::Chord> lastChord;
    for (const notation::ChordSymbol& sym : symbols) {
        const double beat = (static_cast<double>(sym.tick) / ppq - offset) / rate;
        if (beat < -1e-9) continue;  // before the clip's start
        if (beat >= length - 1e-9) {
            if (beyond++ == 0) firstBeyond = beat;
            continue;
        }
        const auto tick = static_cast<std::int64_t>(std::llround(beat * 960.0));
        if (tick == lastTick) {  // the earlier-declared voice's symbol is already there
            if (!(lastChord && *lastChord == sym.chord)) {
                if (conflicts++ == 0) firstConflict = beat;
            }
            continue;
        }
        lastTick = tick;
        if (lastChord && *lastChord == sym.chord) continue;  // equal neighbours collapse
        lastChord = sym.chord;
        model::ContextPoint p;
        p.beat = beat;
        p.type = model::ContextType::Chord;
        p.symbol = harmony::printChord(sym.chord);
        out.points.push_back(std::move(p));
    }
    if (beyond)
        out.notes.push_back({"clip.chords_beyond_length",
                             std::to_string(beyond) + " chord symbol" + (beyond == 1 ? "" : "s") +
                                 " at or past the clip's end (" + json::formatDouble(length) +
                                 " beats) not recorded, the first in " + barWords(s, *firstBeyond),
                             "/notation"});
    if (conflicts)
        out.notes.push_back(
            {"clip.chord_conflict",
             std::to_string(conflicts) + " chord symbol" + (conflicts == 1 ? "" : "s") +
                 " of a later voice differ from the earlier voice's at the same place, the first "
                 "in " +
                 barWords(s, *firstConflict) + ": the earlier-declared voice's is recorded",
             "/notation"});
    return out;
}

std::vector<model::ContextPoint> composeContext(
    std::optional<std::vector<model::ContextPoint>> stated, SymbolChords symbols,
    std::vector<ClipNote>& notes) {
    for (auto& n : symbols.notes) notes.push_back(std::move(n));
    std::vector<model::ContextPoint> out =
        stated ? std::move(*stated) : std::vector<model::ContextPoint>{};
    const bool statedChords = std::any_of(out.begin(), out.end(), [](const model::ContextPoint& p) {
        return p.type == model::ContextType::Chord;
    });
    if (!symbols.points.empty()) {
        if (statedChords)
            notes.push_back({"clip.chords_ignored",
                             "the notation's chord symbols are ignored: the stated context holds "
                             "chords, which win",
                             "/context"});
        else
            for (auto& p : symbols.points) out.push_back(std::move(p));
    }
    sortPoints(out);
    return out;
}

std::optional<ClipNote> mismatchNote(const model::Session& s, model::Id clipId,
                                     const model::Clip& clip, double start, double length,
                                     double phase, const std::string& path) {
    if (clip.kind != model::ClipKind::Midi || !(clip.length > 0) || !(length > 0)) return {};
    if (model::keyPointAt(clip, 0) == nullptr && clip.context.empty()) return {};
    // Notes off channel 10 (stored 9): a drum clip, or an empty one, has no pitch to move.
    const model::Take* take = clip.activeTake ? s.takes.find(*clip.activeTake) : nullptr;
    const model::MidiSource* src = take && take->source && take->source->kind == model::Kind::Midi
                                       ? s.midi.find(*take->source)
                                       : nullptr;
    if (!src || std::none_of(src->sequence.notes.begin(), src->sequence.notes.end(),
                             [](const midi::Note& n) { return n.channel != 9; }))
        return {};
    // The beats at which either key can change, inside the span.
    const double end = start + length;
    std::vector<double> at{start};
    for (const auto& e : s.context)
        if (e.value->type == model::ContextType::Key && e.value->beat > start &&
            e.value->beat < end)
            at.push_back(e.value->beat);
    for (const model::ContextPoint& p : clip.context) {
        if (p.type != model::ContextType::Key) continue;
        // The first pass starts `phase` into the clip; later ones at its period.
        double first = p.beat - phase;
        if (first < 0) first += clip.length;
        for (int pass = 0; pass < 100000; ++pass) {
            const double b = start + first + pass * clip.length;
            if (b >= end - 1e-9) break;
            if (b > start) at.push_back(b);
        }
    }
    std::sort(at.begin(), at.end());
    for (const double b : at) {
        const double rel = b - start + phase;
        const double inClip = rel - std::floor(rel / clip.length + 1e-9) * clip.length;
        const model::ContextPoint* cp = model::keyPointAt(clip, std::max(0.0, inClip));
        const auto pid = model::keyPointAt(s, b);
        const model::ContextPoint* pp = pid ? s.context.find(*pid) : nullptr;
        const auto ck = cp ? model::parseKey(*cp) : std::nullopt;
        const auto pk = pp ? model::parseKey(*pp) : std::nullopt;
        if (!ck || !pk) continue;
        if (harmony::pitchClassNumber(ck->tonic) == harmony::pitchClassNumber(pk->tonic) &&
            ck->scale == pk->scale)
            continue;
        const std::string name =
            clip.name.empty() ? clipId.str() : clipId.str() + " \"" + clip.name + "\"";
        return ClipNote{"context.mismatch",
                        name + " was written in " + model::keyWords(*cp) + " and sits under " +
                            model::keyWords(*pp) + " from " + barWords(s, b) +
                            ": its notes are copied as they are",
                        path};
    }
    return {};
}

std::optional<std::string> clipKeyWords(const model::Clip& clip) {
    if (const model::ContextPoint* p = model::keyPointAt(clip, 0)) return model::keyWords(*p);
    return std::nullopt;
}

}  // namespace supple::engine::detail
