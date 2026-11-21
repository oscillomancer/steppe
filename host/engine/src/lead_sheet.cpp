// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "lead_sheet.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "supple/harmony/harmony.h"
#include "supple/notation/notation.h"

namespace supple::engine::detail {
namespace {

constexpr int kPpq = 960;

double gridBeat(double beat) { return std::round(beat * kPpq) / kPpq; }
std::int64_t ticksBetween(double from, double to) {
    return static_cast<std::int64_t>(std::llround((to - from) * kPpq));
}

std::string scaleList() {
    std::string out;
    for (auto scale : harmony::allScales()) {
        if (!out.empty()) out += ", ";
        out += harmony::scaleName(scale);
    }
    return out;
}

Error syntax(int bar, const std::string& token, const std::string& why, const std::string& hint) {
    return Error("context.syntax", "bar " + std::to_string(bar) + ": '" + token + "' " + why,
                 "/text", hint);
}

}  // namespace

namespace {

std::string printBars(const std::vector<const model::ContextPoint*>& points,
                      const time::TimeMap& map, int firstBar, int lastBar) {
    struct At {
        const model::ContextPoint* key = nullptr;
        const model::ContextPoint* chord = nullptr;
    };
    std::map<std::int64_t, At> byTick;  // every point by its tick, to look a bar up fast
    for (const model::ContextPoint* p : points) {
        At& at = byTick[static_cast<std::int64_t>(std::llround(p->beat * kPpq))];
        (p->type == model::ContextType::Key ? at.key : at.chord) = p;
    }
    std::string out;
    for (int bar = firstBar; bar <= lastBar; ++bar) {
        const double b0 = gridBeat(map.beatOfBar(bar));
        const double b1 = gridBeat(map.beatOfBar(bar + 1));
        const auto t0 = static_cast<std::int64_t>(std::llround(b0 * kPpq));
        const auto t1 = static_cast<std::int64_t>(std::llround(b1 * kPpq));
        std::vector<std::int64_t> cuts{t0};
        for (auto it = byTick.upper_bound(t0); it != byTick.end() && it->first < t1; ++it)
            cuts.push_back(it->first);
        cuts.push_back(t1);
        out += std::to_string(bar) + ":";
        for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
            const auto found = byTick.find(cuts[i]);
            const At at = found == byTick.end() || (i == 0 && cuts[i] != t0) ? At{} : found->second;
            if (at.key) out += " key " + at.key->tonic + " " + at.key->scale;
            out += ' ';
            if (!at.chord)
                out += "~";
            else if (at.chord->symbol.empty())
                out += "r";
            else
                out += "{" + at.chord->symbol + "}";
            out += ":" + notation::durationText(cuts[i + 1] - cuts[i], kPpq);
        }
        out += '\n';
    }
    return out;
}

}  // namespace

std::string printLeadSheet(const model::Session& s, int firstBar, int lastBar) {
    std::vector<const model::ContextPoint*> points;
    for (const auto& e : s.context) points.push_back(e.value.get());
    return printBars(points, *s.timeMap, firstBar, lastBar);
}

std::string printLeadSheetPoints(const std::vector<model::ContextPoint>& points,
                                 const time::TimeMap& map, int firstBar, int lastBar) {
    std::vector<const model::ContextPoint*> refs;
    for (const model::ContextPoint& p : points) refs.push_back(&p);
    return printBars(refs, map, firstBar, lastBar);
}

Result<LeadSheet> parseLeadSheet(const model::Session& s, std::string_view text) {
    return parseLeadSheet(*s.timeMap, text);
}

Result<LeadSheet> parseLeadSheet(const time::TimeMap& map, std::string_view text) {
    if (text.size() > kLeadSheetMaxBytes)
        return Error("context.too_long", "the lead sheet is over 1 MiB", "/text",
                     "write it in parts: each context.write covers the bars it names");
    LeadSheet sheet;
    int previous = 0;
    std::size_t lineNumber = 0;
    std::size_t pos = 0;
    // One bar of a statement: `tokens[0]` is its `N:` (or empty after a `|`), the items follow.
    // Bars run in order; the first one a sheet names sets where it starts.
    const auto readBar = [&](int bar, const std::vector<std::string>& tokens,
                             std::size_t lineNo) -> std::optional<Error> {
        if (bar < 1)
            return Error("context.syntax",
                         "line " + std::to_string(lineNo) + ": bars start at 1", "/text",
                         "the first project bar is 1");
        if (sheet.points.empty() && previous == 0) {
            sheet.firstBar = bar;
        } else if (bar != previous + 1) {
            return Error(
                "context.bar_order",
                "bar " + std::to_string(bar) + " follows bar " + std::to_string(previous), "/text",
                "bars run in order without gaps: the next is " + std::to_string(previous + 1));
        }
        if (bar - sheet.firstBar >= kLeadSheetMaxBars)
            return Error(
                "context.too_long",
                "a lead sheet covers at most " + std::to_string(kLeadSheetMaxBars) + " bars",
                "/text", "write it in parts");
        previous = bar;
        sheet.lastBar = bar;
        const double b0 = gridBeat(map.beatOfBar(bar));
        const double b1 = gridBeat(map.beatOfBar(bar + 1));
        const std::int64_t barTicks = ticksBetween(b0, b1);
        std::int64_t cursor = 0;
        bool keyHere = false;
        for (std::size_t t = 1; t < tokens.size(); ++t) {
            const std::string& token = tokens[t];
            if (token == "key") {
                if (t + 2 >= tokens.size())
                    return syntax(bar, token, "needs a tonic and a scale",
                                  "write key Eb major (scales: " + scaleList() + ")");
                const std::string& tonicText = tokens[t + 1];
                const std::string& scaleText = tokens[t + 2];
                t += 2;
                const auto tonic = harmony::parsePitchClass(tonicText);
                if (!tonic)
                    return syntax(bar, tonicText, "is not a pitch class",
                                  "use a letter and at most one # or b: C, Eb, F#");
                const auto scale = harmony::parseScale(scaleText);
                if (!scale)
                    return syntax(bar, scaleText, "is not a scale", "scales: " + scaleList());
                if (!harmony::keySupported({*tonic, *scale}))
                    return syntax(bar, tonicText + " " + scaleText, "needs a double accidental",
                                  "use the enharmonic key (Ab major for G# major)");
                if (keyHere)
                    return syntax(bar, token, "a second key at the same place",
                                  "one key per beat: move the second after a duration");
                keyHere = true;
                LeadPoint p;
                p.type = model::ContextType::Key;
                p.beat = gridBeat(b0 + static_cast<double>(cursor) / kPpq);
                p.tonic = harmony::printPitchClass(*tonic);
                p.scale = std::string(harmony::scaleName(*scale));
                sheet.points.push_back(std::move(p));
                continue;
            }
            const auto colon = token.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == token.size())
                return syntax(bar, token, "is not a chord, hold or rest",
                              "write {Cm7}:h for a chord, ~:h to hold, r:h for no chord");
            const std::string what = token.substr(0, colon);
            const auto ticks =
                notation::parseDuration(std::string_view(token).substr(colon + 1), kPpq);
            if (!ticks)
                return syntax(bar, token.substr(colon + 1), "is not a duration",
                              "use w, h, q, e, s, t, x, dots, sums such as h+q, or 480t");
            LeadPoint p;
            p.beat = gridBeat(b0 + static_cast<double>(cursor) / kPpq);
            if (what == "~") {
                // a hold: nothing at the cursor
            } else if (what == "r") {
                p.symbol.clear();
                sheet.points.push_back(p);
            } else if (what.size() >= 3 && what.front() == '{' && what.back() == '}') {
                auto chord = harmony::parseChord(std::string_view(what).substr(1, what.size() - 2));
                if (!chord) {
                    Error e = chord.error();
                    e.path = "/text";
                    e.message = "bar " + std::to_string(bar) + ": " + e.message;
                    return e;
                }
                p.symbol = harmony::printChord(*chord);
                sheet.points.push_back(p);
            } else {
                return syntax(bar, token, "is not a chord, hold or rest",
                              "write {Cm7}:h for a chord, ~:h to hold, r:h for no chord");
            }
            cursor += *ticks;
            keyHere = false;
            if (cursor > barTicks) break;  // reported below
        }
        if (cursor != barTicks)
            return Error("context.bar_length",
                         "bar " + std::to_string(bar) + " holds " + std::to_string(cursor) +
                             " ticks of durations, the bar is " + std::to_string(barTicks) +
                             " (960 to a beat)",
                         "/text",
                         "make the durations fill the bar exactly: " +
                             notation::durationText(barTicks, kPpq) + " in all");
        return std::nullopt;
    };
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        // A comment starts at a `#` that begins a word: F# and {F#7#9} are not comments.
        for (std::size_t i = 0; i < line.size(); ++i)
            if (line[i] == '#' &&
                (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) {
                line = line.substr(0, i);
                break;
            }
        // A `;` outside braces ends a statement, as a newline does (notation's separator).
        std::vector<std::string_view> statements;
        for (std::size_t i = 0, from = 0, depth = 0; i <= line.size(); ++i) {
            if (i < line.size() && line[i] == '{') ++depth;
            if (i < line.size() && line[i] == '}' && depth > 0) --depth;
            if (i == line.size() || (line[i] == ';' && depth == 0)) {
                statements.push_back(line.substr(from, i - from));
                from = i + 1;
            }
        }
        for (const std::string_view statement : statements) {
            std::vector<std::string> tokens;
            for (std::size_t i = 0; i < statement.size();) {
                while (i < statement.size() &&
                       std::isspace(static_cast<unsigned char>(statement[i])))
                    ++i;
                std::size_t j = i;
                while (j < statement.size() &&
                       !std::isspace(static_cast<unsigned char>(statement[j])))
                    ++j;
                if (j > i) tokens.emplace_back(statement.substr(i, j - i));
                i = j;
            }
            if (tokens.empty()) continue;
            // The bar number: `N:` at the start of the statement.
            const auto isBarNumber = [](const std::string& token) {
                return token.size() >= 2 && token.size() <= 7 && token.back() == ':' &&
                       std::all_of(token.begin(), token.end() - 1, [](char c) {
                           return std::isdigit(static_cast<unsigned char>(c));
                       });
            };
            if (!isBarNumber(tokens.front()))
                return Error(
                    "context.syntax",
                    "line " + std::to_string(lineNumber) + ": expected a bar number like '3:'",
                    "/text",
                    "start each line (or each part after a ;) with the project bar and "
                    "a colon: 1: {Cm7}:w");
            int bar = std::stoi(tokens.front().substr(0, tokens.front().size() - 1));
            // `|` ends the bar and goes on with the next one; a number after it is accepted when
            // it names that bar (`1: ... | 2: ...`), as in notation.
            std::vector<std::string> items{tokens.front()};
            for (std::size_t t = 1; t <= tokens.size(); ++t) {
                if (t < tokens.size() && tokens[t] != "|") {
                    items.push_back(tokens[t]);
                    continue;
                }
                if (const auto e = readBar(bar, items, lineNumber)) return *e;
                items.assign(1, std::string());
                ++bar;
                if (t + 1 < tokens.size() && isBarNumber(tokens[t + 1])) {
                    const int written = std::stoi(tokens[t + 1]);
                    if (written != bar)
                        return Error("context.bar_order",
                                     "bar " + std::to_string(written) + " follows a | where bar " +
                                         std::to_string(bar) + " comes next",
                                     "/text",
                                     "drop the number after | (bar " + std::to_string(bar) +
                                         " continues there), or start the part with " +
                                         std::to_string(written) + ":");
                    ++t;  // the number names the bar the | starts
                }
            }
        }
    }
    if (previous == 0)
        return Error("context.empty", "the lead sheet names no bars", "/text",
                     "start with a line like 1: key C major {C}:w");
    sheet.fromBeat = gridBeat(map.beatOfBar(sheet.firstBar));
    sheet.toBeat = gridBeat(map.beatOfBar(sheet.lastBar + 1));
    // Keys before chords at one beat; the order a diff applies them in.
    std::stable_sort(
        sheet.points.begin(), sheet.points.end(), [](const LeadPoint& a, const LeadPoint& b) {
            if (a.beat != b.beat) return a.beat < b.beat;
            return a.type == model::ContextType::Key && b.type != model::ContextType::Key;
        });
    // Two chord points at one beat (a zero-length hold cannot happen: durations are positive).
    return sheet;
}

}  // namespace supple::engine::detail
