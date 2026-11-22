// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// take.comp and take.comp.get (session-model §1.5.1): a take stack comped into pieces, each
// playing one take, with a crossfade at every seam between takes. A comp is ordinary items: the
// seam is two items that overlap with a fade each, which both executors already sum.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>

#include "clip_launcher.h"
#include "command_helpers.h"
#include "command_validation.h"
#include "supple/model/audio_warp.h"
#include "supple/model/comp.h"
#include "supple/model/groups.h"
#include "supple/model/schemas.h"
#include "supple/time/timeline.h"

namespace supple::engine {
namespace {

struct SpanArg {
    double from = 0, to = 0;
    std::optional<std::string> take;
    std::optional<std::int64_t> index;
};

// The members are not called `item` and `bypassGroups` together: the command bus treats a
// command with both as a linked-edit command (item.set, item.split...), and this one is not.
struct CompArgs {
    std::optional<std::string> anyPiece;
    std::optional<std::vector<std::string>> pieces;
    std::vector<SpanArg> spans;
    double crossfade = 0.01;
    std::string shape = "cos";
    bool allowLinked = false;
};

struct WhichArgs {
    std::optional<std::string> anyPiece;
    std::optional<std::vector<std::string>> pieces;
};

struct PieceOut {
    std::string item;
    double from = 0, to = 0;
    std::string take;
    std::int64_t index = 0;
    std::string name;
    double fadeIn = 0, fadeOut = 0;
};
struct CompResult {
    std::vector<PieceOut> pieces;
};

schema::Schema<PieceOut> pieceSchema() {
    return schema::object<PieceOut>("comp_piece")
        .field("item", &PieceOut::item, schema::string())
        .field("from", &PieceOut::from, schema::number().unit(units::Unit::beats))
        .field("to", &PieceOut::to, schema::number().unit(units::Unit::beats))
        .field("take", &PieceOut::take, schema::string())
        .field("index", &PieceOut::index, schema::integer())
        .field("name", &PieceOut::name, schema::string())
        .field("fade_in", &PieceOut::fadeIn, schema::number())
        .field("fade_out", &PieceOut::fadeOut, schema::number())
        .build();
}
schema::Schema<CompResult> resultSchema() {
    return schema::object<CompResult>("comp_result")
        .field("pieces", &CompResult::pieces, schema::arrayOf(pieceSchema()))
        .build();
}

Result<std::pair<std::optional<Id>, std::vector<Id>>> whichPieces(
    const model::Session& s, const std::optional<std::string>& item,
    const std::optional<std::vector<std::string>>& items) {
    if (item.has_value() == items.has_value())
        return Error("comp.item", "give item or items, not both and not neither", "/item",
                     "item: any piece of the comp, or items: its pieces");
    const std::string instead =
        "a clip has no pieces to comp; lay it on the timeline first "
        "(item.add {clip})";
    std::pair<std::optional<Id>, std::vector<Id>> out;
    if (item) {
        SUPPLE_TRY(refuseClip(*item, "/item", instead));
        auto id = existing<model::Item>(s, *item, "/item");
        if (!id) return std::move(id).error();
        out.first = *id;
        return out;
    }
    if (items->empty())
        return Error("comp.item", "items is empty", "/items", "name at least one piece");
    for (std::size_t k = 0; k < items->size(); ++k) {
        const std::string path = "/items/" + std::to_string(k);
        SUPPLE_TRY(refuseClip((*items)[k], path, instead));
        auto id = existing<model::Item>(s, (*items)[k], path);
        if (!id) return std::move(id).error();
        out.second.push_back(*id);
    }
    return out;
}

std::size_t takeIndex(const model::Item& item, std::optional<Id> take) {
    if (!take) return 0;
    const auto at = std::find(item.takes.begin(), item.takes.end(), *take);
    return at == item.takes.end() ? 0 : static_cast<std::size_t>(at - item.takes.begin());
}

std::vector<PieceOut> describePieces(const model::Session& s,
                                     const std::vector<model::CompPiece>& run) {
    const double rate = s.project->sampleRate;
    std::vector<PieceOut> out;
    for (const auto& p : run) {
        const model::Item& item = *s.items.find(p.item);
        PieceOut o;
        o.item = p.item.str();
        o.from = s.timeMap->beatAt(static_cast<double>(p.coreStart) / rate);
        o.to = s.timeMap->beatAt(static_cast<double>(p.coreEnd) / rate);
        if (item.activeTake) {
            o.take = item.activeTake->str();
            o.index = static_cast<std::int64_t>(takeIndex(item, item.activeTake));
            if (const model::Take* take = s.takes.find(*item.activeTake)) o.name = take->name;
        }
        o.fadeIn = item.fadeIn.length;
        o.fadeOut = item.fadeOut.length;
        out.push_back(std::move(o));
    }
    return out;
}

// Whether the takes of a run can be extended for a crossfade, and why not.
std::string cannotExtend(const model::Session& s, const std::vector<model::CompPiece>& run) {
    std::set<Id> owned;
    bool midi = false, warped = false;
    for (const auto& p : run)
        for (Id id : s.items.find(p.item)->takes) {
            const model::Take* take = s.takes.find(id);
            if (!take) continue;
            owned.insert(id);
            owned.insert(take->chain.begin(), take->chain.end());
            if (!take->source || take->source->kind != model::Kind::Media) midi = true;
            if (!take->stretchMarkers.empty()) warped = true;
        }
    if (midi) return "a take is MIDI or empty";
    if (warped) return "a take has stretch markers";
    for (const auto& e : s.envelopes)
        if (owned.contains(e.value->target.object)) return "a take or its FX has envelopes";
    return {};
}

struct Painted {
    std::int64_t lo = 0, hi = 0;
    std::size_t index = 0;
};
struct Core {
    Id item;
    std::int64_t lo = 0, hi = 0;
    std::size_t index = 0;
};

bool near(double a, double b) { return std::fabs(a - b) <= 1e-9; }

}  // namespace

void registerCompCommands(cmd::Registry& registry) {
    [&] {
        Command<CompArgs, CompResult> c;
        c.name = "take.comp";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Comp an item's takes: each span plays one take, crossfaded at the seams.";
        c.description =
            "item (any piece, or an item with several takes to start one) or items (the pieces, in "
            "any "
            "order). Spans {from, to, take|index} (beats; index is the take's 0-based place in the "
            "item's "
            "take order) choose which take plays; what no span covers keeps its take. The item is "
            "split where the take changes into pieces that each keep all the takes, and a "
            "crossfade (seconds, default 0.01; shape cos is equal power, linear for "
            "phase-coherent material) is centred on every seam between different takes: the "
            "pieces overlap and fade, so nothing new reaches the audio path. Call it again on any "
            "piece (or items: the pieces) to move seams: the crossfades are rebuilt for the whole "
            "run, and the same call twice changes nothing. Audio only: a seam is a hard cut, with "
            "the warning comp.crossfade_skipped, when a take is MIDI or empty, warped or has "
            "envelopes. take.activate {index} on a piece changes its take and keeps the seams. "
            "One undo step. Returns the pieces in time order.";
        c.tags = {"take"};
        auto spanSchema = schema::object<SpanArg>("take_comp_span")
                              .field("from", &SpanArg::from, schema::number().min(0))
                              .field("to", &SpanArg::to, schema::number().min(0))
                              .field("take", &SpanArg::take, idArg("tk", "Take"))
                              .field("index", &SpanArg::index, schema::integer().min(0))
                              .build();
        c.args =
            schema::object<CompArgs>("take_comp_args")
                .field("item", &CompArgs::anyPiece, timelineItemArg())
                .field("items", &CompArgs::pieces,
                       schema::arrayOf(timelineItemArg()).maxItems(4096))
                .field("spans", &CompArgs::spans,
                       schema::arrayOf(spanSchema).minItems(1).maxItems(4096))
                .field("crossfade", &CompArgs::crossfade, schema::number().min(0))
                .defaultValue(0.01)
                .field("crossfade_shape", &CompArgs::shape,
                       schema::enumeration<std::string>({{"cos", "cos"}, {"linear", "linear"}}))
                .defaultValue(std::string("cos"))
                .field("bypass_groups", &CompArgs::allowLinked, schema::boolean())
                .defaultValue(false)
                .build();
        c.result = resultSchema();
        c.errors = {
            {"comp.item", "neither or both of item and items, or an empty items",
             "give item (any piece) or items (the pieces)"},
            {"comp.takes", "an item has fewer than two takes",
             "record several passes or take.add, or use take.activate"},
            {"comp.mismatch", "the pieces cannot be followed as one comp, or two could continue it",
             "name the pieces in items (take.comp.get lists a comp's pieces)"},
            {"comp.span", "a span is empty, out of order, overlapping or outside the comp",
             "give from before to, inside the comp, in time order without overlap"},
            {"comp.take", "a span has no or both of take and index, or one not in the item",
             "give take (an id of the item) or index (0..takes-1); take.list lists them"},
            {"comp.loop", "a looped item", "comp an item that does not loop"},
            {"comp.linked", "the item has linked peers",
             "set bypass_groups true to comp it alone, or take it out of its track group"},
            {"item.locked", "a piece is locked", "item.set {locked: false}"},
            {"item.kind", "a clip", "lay the clip on the timeline first (item.add {clip})"}};
        c.examples = {{"Take the second take for bars 2 to 3",
                       j(R"({"item":"it_1","crossfade":0,"spans":[{"from":1,"to":3,"index":1}]})"),
                       j(R"({"pieces":"<any>"})"), "song_with_takes"}};
        c.run = [](const CompArgs& a, Context& ctx) -> Result<CompResult> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            auto which = whichPieces(s, a.anyPiece, a.pieces);
            if (!which) return std::move(which).error();
            auto run = model::compRun(s, which->first, which->second);
            if (!run) return std::move(run).error();
            const std::string where = a.anyPiece ? "/item" : "/items";
            for (const auto& p : *run) {
                SUPPLE_TRY(checkUnlocked(s, p.item, where));
                if (!a.allowLinked && model::linkedItems(s, p.item).size() > 1)
                    return Error("comp.linked",
                                 p.item.str() + " is edited together with other items", where,
                                 "set bypass_groups true to comp it alone, or take it out of its "
                                 "track group");
            }
            const double rate = s.project->sampleRate;
            const time::TimeMap& tm = *s.timeMap;
            auto snap = [&](double beat) { return time::samplesAt(tm.secondsAt(beat), rate); };
            const std::int64_t runStart = run->front().coreStart, runEnd = run->back().coreEnd;
            const std::size_t takeCount = s.items.find(run->front().item)->takes.size();

            // The spans, in samples, each with the take index it plays.
            std::vector<Painted> spans;
            std::int64_t previousEnd = runStart;
            for (std::size_t i = 0; i < a.spans.size(); ++i) {
                const SpanArg& span = a.spans[i];
                const std::string at = "/spans/" + std::to_string(i);
                if (span.take.has_value() == span.index.has_value())
                    return Error("comp.take", "give take or index, exactly one", at,
                                 "take: \"tk_2\" (a take of the item) or index: 1");
                std::size_t index = 0;
                if (span.index) {
                    if (*span.index >= static_cast<std::int64_t>(takeCount))
                        return Error("comp.take",
                                     "the item has " + std::to_string(takeCount) + " takes",
                                     at + "/index",
                                     "use take.list for the item: indices 0.." +
                                         std::to_string(takeCount - 1));
                    index = static_cast<std::size_t>(*span.index);
                } else {
                    std::optional<std::size_t> found;
                    if (const auto id = Id::parse(*span.take))
                        for (const auto& p : *run) {
                            const auto& takes = s.items.find(p.item)->takes;
                            const auto hit = std::find(takes.begin(), takes.end(), *id);
                            if (hit != takes.end())
                                found = static_cast<std::size_t>(hit - takes.begin());
                        }
                    if (!found)
                        return Error("comp.take", *span.take + " is not a take of this comp",
                                     at + "/take",
                                     "use a take id from take.list of a piece, or index");
                    index = *found;
                }
                const std::int64_t lo = snap(span.from), hi = snap(span.to);
                if (hi <= lo)
                    return Error("comp.span", "the span is empty", at + "/to",
                                 "give from before to, at least a sample apart");
                if (lo < runStart)
                    return Error(
                        "comp.span", "the span starts before the comp", at + "/from",
                        "start at beat " +
                            std::to_string(tm.beatAt(static_cast<double>(runStart) / rate)) +
                            " or later");
                if (hi > runEnd)
                    return Error("comp.span", "the span ends after the comp", at + "/to",
                                 "end at beat " +
                                     std::to_string(tm.beatAt(static_cast<double>(runEnd) / rate)) +
                                     " or earlier");
                if (lo < previousEnd)
                    return Error("comp.span", "the span overlaps the one before it", at + "/from",
                                 "give the spans in time order, each starting where the last ended "
                                 "or later");
                previousEnd = hi;
                spans.push_back({lo, hi, index});
            }

            // What each core piece plays now, and where the take must change.
            std::vector<Core> cores;
            for (const auto& p : *run)
                cores.push_back(
                    {p.item, p.coreStart, p.coreEnd,
                     takeIndex(*s.items.find(p.item), s.items.find(p.item)->activeTake)});
            auto indexAt = [&](std::int64_t n) {
                for (const Painted& p : spans)
                    if (p.lo <= n && n < p.hi) return p.index;
                for (const Core& k : cores)
                    if (k.lo <= n && n < k.hi) return k.index;
                return std::size_t{0};
            };
            std::set<std::int64_t> breaks;
            auto isSeam = [&](std::int64_t n) {
                return std::any_of(cores.begin(), cores.end(),
                                   [&](const Core& k) { return k.lo == n; });
            };
            for (const Painted& p : spans)
                for (std::int64_t n : {p.lo, p.hi})
                    if (n > runStart && n < runEnd && !isSeam(n) && indexAt(n - 1) != indexAt(n))
                        breaks.insert(n);

            // The core, written: every crossfade taken out, so that a split sees plain pieces.
            auto& txn = engine.txn();
            std::map<Id, model::Item> originalItems;
            std::map<Id, model::Take> originalTakes;
            for (const auto& p : *run) {
                const model::Item& it = *s.items.find(p.item);
                originalItems[p.item] = it;
                for (Id t : it.takes) originalTakes[t] = *s.takes.find(t);
            }
            for (std::size_t k = 0; k + 1 < run->size(); ++k) {
                const auto& left = (*run)[k];
                const auto& right = (*run)[k + 1];
                const double seam = static_cast<double>(left.coreEnd) / rate;
                model::Item l = *txn.get<model::Item>(left.item);
                model::Item r = *txn.get<model::Item>(right.item);
                if (left.end != left.coreEnd) model::setItemEndSeconds(s, l, seam);
                if (right.start != right.coreStart) {
                    const double delta = seam - model::itemSpan(s, r).startSec;
                    for (Id t : r.takes)
                        txn.modify<model::Take>(
                            t, [&](model::Take& take) { model::rebaseAudioTake(take, delta); });
                    model::setItemStartSeconds(s, r, seam);
                }
                l.fadeOut = {};
                r.fadeIn = {};
                txn.put(left.item, std::move(l));
                txn.put(right.item, std::move(r));
            }

            // The splits, left to right, each through item.split (it copies every take).
            for (const std::int64_t at : breaks) {
                const auto hit = std::find_if(cores.begin(), cores.end(), [&](const Core& k) {
                    return k.lo < at && at < k.hi;
                });
                if (hit == cores.end()) continue;
                auto args = json::Value::object();
                args["item"] = hit->item.str();
                args["at"] = tm.beatAt(static_cast<double>(at) / rate);
                args["bypass_groups"] = true;
                auto split =
                    ctx.service<const cmd::Registry>().find("item.split")->invoke(args, ctx);
                if (!split) return std::move(split).error();
                const auto made = Id::parse(split->find("right")->asString());
                Core right{*made, at, hit->hi, hit->index};
                hit->hi = at;
                cores.insert(std::next(hit), right);
            }
            for (Core& k : cores) k.index = indexAt(k.lo);

            // The seams.
            const std::size_t n = cores.size();
            std::vector<Id> ids;
            for (const Core& k : cores) ids.push_back(k.item);
            std::vector<model::CompPiece> plain;
            for (const Core& k : cores) plain.push_back({k.item, k.lo, k.hi, k.lo, k.hi});
            const std::string refusal = cannotExtend(engine.view(), plain);
            const auto wanted = static_cast<std::int64_t>(std::llround(a.crossfade * rate / 2.0));
            std::vector<std::int64_t> half(n > 0 ? n - 1 : 0, 0);
            bool skipped = false, clamped = false;
            for (std::size_t k = 0; k + 1 < n; ++k) {
                if (cores[k].index == cores[k + 1].index || wanted == 0) continue;
                if (!refusal.empty()) {
                    skipped = true;
                    continue;
                }
                half[k] = std::min({wanted, (cores[k].hi - cores[k].lo) / 2,
                                    (cores[k + 1].hi - cores[k + 1].lo) / 2});
                clamped = clamped || half[k] < wanted;
            }
            if (skipped)
                ctx.warn("comp.crossfade_skipped",
                         "the seams are hard cuts: " + refusal +
                             ", and a crossfade would have to move it",
                         "/crossfade");
            if (clamped)
                ctx.warn("comp.crossfade_clamped",
                         "a crossfade is shorter than asked: the pieces beside the seam are short",
                         "/crossfade");
            const auto shape =
                a.shape == "linear" ? model::FadeShape::Linear : model::FadeShape::Cos;
            auto fade = [&](std::int64_t h) {
                model::Fade f;
                if (h > 0) {
                    f.length = static_cast<double>(2 * h) / rate;
                    f.shape = shape;
                }
                return f;
            };
            for (std::size_t i = 0; i < n; ++i) {
                model::Item item = *txn.get<model::Item>(cores[i].item);
                item.activeTake = item.takes[cores[i].index];
                const std::int64_t before = i > 0 ? half[i - 1] : 0;
                const std::int64_t after = i + 1 < n ? half[i] : 0;
                if (refusal.empty()) {
                    // Whole samples at both ends, the audio taken back or forward with them.
                    if (i > 0) {
                        const double start = static_cast<double>(cores[i].lo - before) / rate;
                        const double delta = start - model::itemSpan(s, item).startSec;
                        for (Id t : item.takes)
                            txn.modify<model::Take>(
                                t, [&](model::Take& take) { model::rebaseAudioTake(take, delta); });
                        model::setItemStartSeconds(s, item, start);
                    }
                    if (i + 1 < n)
                        model::setItemEndSeconds(s, item,
                                                 static_cast<double>(cores[i].hi + after) / rate);
                }
                if (i > 0) item.fadeIn = fade(before);
                if (i + 1 < n) item.fadeOut = fade(after);
                txn.put(cores[i].item, std::move(item));
            }

            // A value that is where it was, to a nanosecond, stays as stored: a second call with
            // the same arguments gives the same bytes.
            for (const auto& [id, was] : originalItems) {
                model::Item now = *txn.get<model::Item>(id);
                auto keep = [](double& value, double old) {
                    if (near(value, old)) value = old;
                };
                keep(now.position, was.position);
                keep(now.length, was.length);
                keep(now.fadeIn.length, was.fadeIn.length);
                keep(now.fadeOut.length, was.fadeOut.length);
                txn.put(id, std::move(now));
                for (Id t : was.takes) {
                    model::Take take = *txn.get<model::Take>(t);
                    keep(take.startOffset, originalTakes.at(t).startOffset);
                    txn.put(t, std::move(take));
                }
            }
            auto final = model::compRun(engine.view(), std::nullopt, ids);
            if (!final) return std::move(final).error();
            return CompResult{describePieces(engine.view(), *final)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<WhichArgs, CompResult> c;
        c.name = "take.comp.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List the pieces of a comp: spans, takes and fades.";
        c.description =
            "The pieces of the comp that item belongs to (or items), in time order: from and to "
            "are the seams in beats (crossfades taken out), take and index the take each plays, "
            "fade_in and fade_out the crossfades in seconds. An item with several takes and no "
            "seams is a comp of one piece.";
        c.tags = {"take"};
        c.args = schema::object<WhichArgs>("take_comp_get_args")
                     .field("item", &WhichArgs::anyPiece, timelineItemArg())
                     .field("items", &WhichArgs::pieces,
                            schema::arrayOf(timelineItemArg()).maxItems(4096))
                     .build();
        c.result = resultSchema();
        c.examples = {{"Read a comp", j(R"({"item":"it_1"})"), j(R"({"pieces":"<any>"})"),
                       "song_with_takes"}};
        c.run = [](const WhichArgs& a, Context& ctx) -> Result<CompResult> {
            const model::Session s = app(ctx).view();
            auto which = whichPieces(s, a.anyPiece, a.pieces);
            if (!which) return std::move(which).error();
            auto run = model::compRun(s, which->first, which->second);
            if (!run) return std::move(run).error();
            return CompResult{describePieces(s, *run)};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
