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
    double crossfade = model::kDefaultCompCrossfade;
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
struct SeamOut {
    std::int64_t seam = 0;
    double at = 0;
    std::string left, right;
};
// Another member of the named run's edit group: its comp, read like the named one.
struct MemberOut {
    std::string track;
    std::vector<PieceOut> pieces;
    double crossfade = model::kDefaultCompCrossfade;
    std::string shape = "cos";
    bool uniform = true;
    std::vector<SeamOut> differing;
};
// A group track the comp leaves alone: nothing over the run ("no_item"), or only one-take items,
// which have nothing to comp ("one_take").
struct SkippedOut {
    std::string track, reason;
};
struct CompResult {
    std::vector<PieceOut> pieces;
    double crossfade = model::kDefaultCompCrossfade;
    std::string shape = "cos";
    bool uniform = true;
    std::vector<SeamOut> differing;
    std::vector<MemberOut> members;
    std::vector<std::string> differingMembers;
    std::vector<SkippedOut> skippedTracks;
};

schema::Schema<PieceOut> pieceSchema() {
    return schema::object<PieceOut>("comp_piece")
        .field("item", &PieceOut::item, schema::string())
        .field("from", &PieceOut::from, schema::number().unit(units::Unit::beats))
        .field("to", &PieceOut::to, schema::number().unit(units::Unit::beats))
        .field("take", &PieceOut::take, schema::string())
        .field("index", &PieceOut::index, schema::integer())
        .field("name", &PieceOut::name, schema::string())
        .field("fade_in", &PieceOut::fadeIn, schema::number().doc("Seconds."))
        .field("fade_out", &PieceOut::fadeOut, schema::number().doc("Seconds."))
        .build();
}
schema::Schema<SeamOut> seamSchema() {
    return schema::object<SeamOut>("comp_seam")
        .field("seam", &SeamOut::seam, schema::integer().doc("0: between the first two pieces."))
        .field("at", &SeamOut::at, schema::number().unit(units::Unit::beats))
        .field("left", &SeamOut::left, schema::string())
        .field("right", &SeamOut::right, schema::string())
        .build();
}
schema::Schema<SkippedOut> skippedSchema() {
    return schema::object<SkippedOut>("comp_skipped")
        .field("track", &SkippedOut::track, schema::string())
        .field("reason", &SkippedOut::reason,
               schema::enumeration<std::string>({{"no_item", "no_item"}, {"one_take", "one_take"}})
                   .doc("no_item: the track has no item over the comp. one_take: its items over "
                        "the comp have one take each, so there is nothing to comp; they are left "
                        "as they are."))
        .build();
}

schema::Schema<MemberOut> memberSchema() {
    return schema::object<MemberOut>("comp_member")
        .field("track", &MemberOut::track, schema::string())
        .field("pieces", &MemberOut::pieces, schema::arrayOf(pieceSchema()))
        .field("crossfade", &MemberOut::crossfade, schema::number().doc("Seconds."))
        .field("crossfade_shape", &MemberOut::shape,
               schema::enumeration<std::string>({{"cos", "cos"}, {"linear", "linear"}}))
        .field("uniform", &MemberOut::uniform, schema::boolean())
        .field("differing_seams", &MemberOut::differing, schema::arrayOf(seamSchema()))
        .build();
}
schema::Schema<CompResult> resultSchema() {
    return schema::object<CompResult>("comp_result")
        .field("pieces", &CompResult::pieces, schema::arrayOf(pieceSchema()))
        .field("crossfade", &CompResult::crossfade, schema::number().doc("Seconds."))
        .field("crossfade_shape", &CompResult::shape,
               schema::enumeration<std::string>({{"cos", "cos"}, {"linear", "linear"}}))
        .field("uniform", &CompResult::uniform, schema::boolean())
        .field("differing_seams", &CompResult::differing, schema::arrayOf(seamSchema()))
        .field("members", &CompResult::members, schema::arrayOf(memberSchema()))
        .field("differing_members", &CompResult::differingMembers,
               schema::arrayOf(schema::string()))
        .field("skipped_tracks", &CompResult::skippedTracks, schema::arrayOf(skippedSchema()))
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

// The pieces and the crossfade the run reads back as (model::compCrossfade).
CompResult describe(const model::Session& s, const std::vector<model::CompPiece>& run) {
    const model::CompCrossfade crossfade = model::compCrossfade(s, run);
    CompResult out;
    out.pieces = describePieces(s, run);
    out.crossfade = crossfade.seconds;
    out.shape = crossfade.shape == model::FadeShape::Linear ? "linear" : "cos";
    const double rate = s.project->sampleRate;
    for (const std::size_t k : model::compDifferingSeams(s, run))
        out.differing.push_back({static_cast<std::int64_t>(k),
                                 s.timeMap->beatAt(static_cast<double>(run[k].coreEnd) / rate),
                                 run[k].item.str(), run[k + 1].item.str()});
    out.uniform = out.differing.empty();
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

// One run comped on its own: `who` is "" for the named run, "on <track>: " for a member of its
// edit group (it prefixes the warnings).
Result<CompResult> compOne(const CompArgs& a, Context& ctx, const std::string& who) {
    EngineApp& engine = app(ctx);
    const model::Session s = engine.view();
    auto which = whichPieces(s, a.anyPiece, a.pieces);
    if (!which) return std::move(which).error();
    auto run = model::compRun(s, which->first, which->second);
    if (!run) return std::move(run).error();
    for (const auto& p : *run) {
        // The path names the piece the caller gave: /item, or /items/k.
        std::string where = "/item";
        if (!a.anyPiece) {
            const auto at = std::find(which->second.begin(), which->second.end(), p.item);
            where = "/items/" + std::to_string(at - which->second.begin());
        }
        SUPPLE_TRY(checkUnlocked(s, p.item, where));
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
                return Error(
                    "comp.take", "the item has " + std::to_string(takeCount) + " takes",
                    at + "/index",
                    "use take.list for the item: indices 0.." + std::to_string(takeCount - 1));
            index = static_cast<std::size_t>(*span.index);
        } else {
            std::optional<std::size_t> found;
            if (const auto id = Id::parse(*span.take))
                for (const auto& p : *run) {
                    const auto& takes = s.items.find(p.item)->takes;
                    const auto hit = std::find(takes.begin(), takes.end(), *id);
                    if (hit != takes.end()) found = static_cast<std::size_t>(hit - takes.begin());
                }
            if (!found)
                return Error("comp.take", *span.take + " is not a take of this comp", at + "/take",
                             "use a take id from take.list of a piece, or index");
            index = *found;
        }
        const std::int64_t lo = snap(span.from), hi = snap(span.to);
        if (hi <= lo)
            return Error("comp.span", "the span is empty", at + "/to",
                         "give from before to, at least a sample apart");
        if (lo < runStart)
            return Error("comp.span", "the span starts before the comp", at + "/from",
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
        cores.push_back({p.item, p.coreStart, p.coreEnd,
                         takeIndex(*s.items.find(p.item), s.items.find(p.item)->activeTake)});
    // Over the pieces as they are now, whatever is joined or split below.
    const std::vector<Core> unjoined = cores;
    auto indexAt = [&](std::int64_t n) {
        for (const Painted& p : spans)
            if (p.lo <= n && n < p.hi) return p.index;
        for (const Core& k : unjoined)
            if (k.lo <= n && n < k.hi) return k.index;
        return std::size_t{0};
    };
    // Pieces that are one piece cut in two and will play the same take are joined first,
    // so that a moved seam leaves no leftover boundary to clamp its crossfade: the result
    // is what the same spans give from scratch.
    std::vector<std::vector<Id>> joins;
    {
        std::vector<Core> joined;
        for (const Core& k : cores) {
            const bool join = !joined.empty() && indexAt(k.lo - 1) == indexAt(k.lo) &&
                              model::joinablePieces(s, joins.back().back(), k.item);
            if (join) {
                joined.back().hi = k.hi;
                joins.back().push_back(k.item);
            } else {
                joined.push_back(k);
                joins.push_back({k.item});
            }
        }
        cores = std::move(joined);
    }
    std::set<std::int64_t> breaks;
    auto isSeam = [&](std::int64_t n) {
        return std::any_of(cores.begin(), cores.end(), [&](const Core& k) { return k.lo == n; });
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

    std::set<Id> removed;
    for (const auto& group : joins)
        for (std::size_t m = 1; m < group.size(); ++m) {
            model::Item l = *txn.get<model::Item>(group[0]);
            const model::Item r = *txn.get<model::Item>(group[m]);
            model::setItemEndToItemEnd(s, l, r);
            l.fadeOut = r.fadeOut;
            txn.put(group[0], std::move(l));
            for (Id t : r.takes) SUPPLE_TRY(removeTakeObjects(txn, engine.view(), t));
            txn.remove<model::Item>(group[m]);
            removed.insert(group[m]);
        }

    // The splits, left to right, each through item.split (it copies every take).
    for (const std::int64_t at : breaks) {
        const auto hit = std::find_if(cores.begin(), cores.end(),
                                      [&](const Core& k) { return k.lo < at && at < k.hi; });
        if (hit == cores.end()) continue;
        auto args = json::Value::object();
        args["item"] = hit->item.str();
        args["at"] = tm.beatAt(static_cast<double>(at) / rate);
        args["bypass_groups"] = true;
        auto split = ctx.service<const cmd::Registry>().find("item.split")->invoke(args, ctx);
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
    // A boundary between MIDI pieces that play one take and could not be joined (the
    // notes of one half were edited, say) stops a held note and strikes it again.
    std::string reattack;
    {
        const model::Session now = engine.view();
        auto isMidi = [&](Id item) {
            const model::Item* it = now.items.find(item);
            const model::Take* tk =
                it && it->activeTake ? now.takes.find(*it->activeTake) : nullptr;
            return tk && tk->source && tk->source->kind == model::Kind::Midi;
        };
        for (std::size_t k = 0; k + 1 < n; ++k)
            if (cores[k].index == cores[k + 1].index && isMidi(cores[k].item) &&
                isMidi(cores[k + 1].item)) {
                if (!reattack.empty()) reattack += ", ";
                reattack += std::to_string(tm.beatAt(static_cast<double>(cores[k].hi) / rate));
            }
    }
    if (!reattack.empty())
        ctx.warn("comp.midi_boundary",
                 who + "held notes sound again at the boundaries (beats " + reattack +
                     ") between pieces of one take that could not be joined: they "
                     "differ in their notes or settings",
                 "/spans");
    const auto wanted = static_cast<std::int64_t>(std::llround(a.crossfade * rate / 2.0));
    std::vector<std::int64_t> half(n > 0 ? n - 1 : 0, 0);
    bool skipped = false, clamped = false;
    for (std::size_t k = 0; k + 1 < n; ++k) {
        if (cores[k].index == cores[k + 1].index || wanted == 0) continue;
        if (!refusal.empty()) {
            skipped = true;
            continue;
        }
        half[k] = std::min(
            {wanted, (cores[k].hi - cores[k].lo) / 2, (cores[k + 1].hi - cores[k + 1].lo) / 2});
        clamped = clamped || half[k] < wanted;
    }
    if (skipped)
        ctx.warn(
            "comp.crossfade_skipped",
            who + "the seams are hard cuts: " + refusal + ", and a crossfade would have to move it",
            "/crossfade");
    if (clamped)
        ctx.warn("comp.crossfade_clamped",
                 who + "a crossfade is shorter than asked: the pieces beside the seam are short",
                 "/crossfade");
    const auto shape = a.shape == "linear" ? model::FadeShape::Linear : model::FadeShape::Cos;
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
                model::setItemEndSeconds(s, item, static_cast<double>(cores[i].hi + after) / rate);
        }
        if (i > 0) item.fadeIn = fade(before);
        if (i + 1 < n) item.fadeOut = fade(after);
        txn.put(cores[i].item, std::move(item));
    }

    // A value that is where it was, to a nanosecond, stays as stored: a second call with
    // the same arguments gives the same bytes.
    for (const auto& [id, was] : originalItems) {
        if (removed.contains(id)) continue;
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
    return describe(engine.view(), *final);
}

std::pair<std::int64_t, std::int64_t> itemSamples(const model::Session& s, const model::Item& it,
                                                  double rate) {
    const model::ItemSpan span = model::itemSpan(s, it);
    return {time::samplesAt(span.startSec, rate), time::samplesAt(span.endSec, rate)};
}

// "tr_2 (Second)": the id the commands take and the name the user sees.
std::string trackNamed(const model::Session& s, Id track) {
    return track.str() + " (" + model::trackLabel(s, track) + ")";
}

// The path that names each piece of the run as the caller gave it: /item, or /items/k.
std::vector<std::string> piecePaths(bool single, const std::vector<Id>& given,
                                    const std::vector<model::CompPiece>& run) {
    std::vector<std::string> out;
    for (const auto& p : run) {
        const auto at = std::find(given.begin(), given.end(), p.item);
        if (single)
            out.push_back("/item");
        else
            out.push_back(at == given.end() ? "/items"
                                            : "/items/" + std::to_string(at - given.begin()));
    }
    return out;
}

struct Member {
    Id track;
    std::vector<model::CompPiece> run;
};

// The other tracks of the run's edit group that hold items over the run, each as a run of its own,
// in track order. A member is a track whose items on the lead's lane over the run's extent form a
// run (compRun) with the same first and last core sample as the named run; how it is cut inside
// does not matter (take.comp rebuilds it from the spans, as for any run), so a member that is still
// whole beside a split lead counts, and so does one whose crossfades differ. A track with items
// over the run that do not form such a run (compRun refuses them, or they start or end elsewhere)
// is refused (comp.group_mismatch, comp.group_takes for too few takes); with `off`, those tracks go
// into it instead and the rest are returned (take.comp.get). A track with no item over the run is
// not a member and not an error, and neither is one whose items over the run all have a single take
// (nothing to comp): they go into `skipped` with their reason, and the reply names them
// (skipped_tracks).
Result<std::vector<Member>> groupMembers(const model::Session& s,
                                         const std::vector<model::CompPiece>& run,
                                         std::vector<Id>* off, std::vector<SkippedOut>* skipped,
                                         const std::vector<std::string>& paths) {
    const model::Item& leadItem = *s.items.find(run.front().item);
    const Id lead = leadItem.track;
    const double rate = s.project->sampleRate;
    const std::int64_t runStart = run.front().coreStart, runEnd = run.back().coreEnd;
    std::vector<Member> out;
    for (const Id track : model::linkedTracks(s, lead, "edit")) {
        if (track == lead) continue;
        std::vector<Id> items;
        for (const auto& e : s.items) {
            if (e.value->track != track || e.value->lane != leadItem.lane) continue;
            const auto [from, to] = itemSamples(s, *e.value, rate);
            if (from < runEnd && to > runStart) items.push_back(e.id);
        }
        if (items.empty()) {
            if (skipped) skipped->push_back({track.str(), "no_item"});
            continue;
        }
        // One take each: nothing to comp, and a bass or a DI beside takes of drums is ordinary.
        // The items stay as they are. (An item with several takes but fewer than a span plays is
        // comp.group_takes, below.)
        if (std::all_of(items.begin(), items.end(),
                        [&](Id id) { return s.items.find(id)->takes.size() < 2; })) {
            if (skipped) skipped->push_back({track.str(), "one_take"});
            continue;
        }
        const std::string who = trackNamed(s, track);
        // The lead's piece it overlaps most, for the error's path.
        std::size_t named = 0;
        {
            const auto [from, to] = itemSamples(s, *s.items.find(items.front()), rate);
            std::int64_t best = -1;
            for (std::size_t k = 0; k < run.size(); ++k) {
                const std::int64_t overlap =
                    std::min(to, run[k].coreEnd) - std::max(from, run[k].coreStart);
                if (overlap > best) {
                    best = overlap;
                    named = k;
                }
            }
        }
        std::optional<Error> problem;
        Member m{track, {}};
        auto theirs = model::compRun(s, std::nullopt, items);
        if (!theirs) {
            const Error& inner = theirs.error();
            if (inner.code == "comp.takes")
                problem = Error("comp.group_takes", who + " has an item with fewer than two takes",
                                paths[named], inner.hint + "; or comp it alone with bypass_groups");
            else
                problem = Error("comp.group_mismatch", who + ": " + inner.message, paths[named],
                                inner.hint + "; or comp it alone with bypass_groups");
        } else if (theirs->front().coreStart != runStart || theirs->back().coreEnd != runEnd) {
            problem = Error("comp.group_mismatch",
                            who +
                                "'s items do not cover the same stretch as the comp (they start "
                                "or end elsewhere)",
                            paths[named],
                            "make its items span the same stretch, or comp it alone with "
                            "bypass_groups");
        } else {
            m.run = std::move(*theirs);
        }
        if (problem) {
            if (!off) return *problem;
            off->push_back(track);
            continue;
        }
        out.push_back(std::move(m));
    }
    return out;
}

MemberOut memberOf(const model::Session& s, Id track, const CompResult& r) {
    MemberOut m;
    m.track = track.str();
    m.pieces = r.pieces;
    m.crossfade = r.crossfade;
    m.shape = r.shape;
    m.uniform = r.uniform;
    m.differing = r.differing;
    (void)s;
    return m;
}

// Members that read back differently from the named run: other spans, takes or crossfade.
// Then `uniform` of the whole group.
void finishGroup(CompResult& lead, std::vector<std::string> notLikeIt) {
    for (const MemberOut& m : lead.members) {
        bool same = m.pieces.size() == lead.pieces.size() && m.crossfade == lead.crossfade &&
                    m.shape == lead.shape;
        for (std::size_t k = 0; same && k < m.pieces.size(); ++k)
            same = m.pieces[k].from == lead.pieces[k].from && m.pieces[k].to == lead.pieces[k].to &&
                   m.pieces[k].index == lead.pieces[k].index;
        if (!same) notLikeIt.push_back(m.track);
    }
    std::sort(notLikeIt.begin(), notLikeIt.end());
    lead.differingMembers = std::move(notLikeIt);
    bool uniform = lead.uniform && lead.differingMembers.empty();
    for (const MemberOut& m : lead.members) uniform = uniform && m.uniform;
    lead.uniform = uniform;
}

// take.comp without bypass_groups: the named run and the run of every other member of its edit
// group, with the same spans and take indices. Everything is checked before anything is written.
Result<CompResult> compGroup(const CompArgs& a, Context& ctx) {
    const model::Session s = app(ctx).view();
    auto which = whichPieces(s, a.anyPiece, a.pieces);
    if (!which) return std::move(which).error();
    auto run = model::compRun(s, which->first, which->second);
    if (!run) return std::move(run).error();
    const auto paths = piecePaths(a.anyPiece.has_value(), which->second, *run);
    std::vector<SkippedOut> left;  // group tracks the comp leaves alone, named in the reply
    auto members = groupMembers(s, *run, nullptr, &left, paths);
    if (!members) return std::move(members).error();
    auto skipped = [&](CompResult& r) { r.skippedTracks = left; };
    if (members->empty()) {
        auto alone = compOne(a, ctx, "");
        if (alone) skipped(*alone);
        return alone;
    }
    // The take each span plays, as an index (every member plays the same index).
    const std::size_t leadTakes = s.items.find(run->front().item)->takes.size();
    std::vector<std::optional<std::int64_t>> indices;
    std::int64_t highest = -1;
    for (const SpanArg& span : a.spans) {
        std::optional<std::int64_t> index = span.index;
        if (!index && span.take)
            if (const auto id = Id::parse(*span.take))
                for (const auto& p : *run) {
                    const auto& takes = s.items.find(p.item)->takes;
                    const auto hit = std::find(takes.begin(), takes.end(), *id);
                    if (hit != takes.end()) index = hit - takes.begin();
                }
        indices.push_back(index);
        if (index && *index < static_cast<std::int64_t>(leadTakes))
            highest = std::max(highest, *index);
    }
    for (const Member& m : *members) {
        const std::size_t theirs = s.items.find(m.run.front().item)->takes.size();
        if (highest >= static_cast<std::int64_t>(theirs))
            return Error("comp.group_takes",
                         trackNamed(s, m.track) + " has " + std::to_string(theirs) +
                             " takes, and a span plays take index " + std::to_string(highest),
                         paths[0],
                         "comp it alone with bypass_groups, or give spans that use indices 0.." +
                             std::to_string(theirs - 1));
        for (const auto& p : m.run) SUPPLE_TRY(checkUnlocked(s, p.item, paths[0]));
    }
    auto lead = compOne(a, ctx, "");
    if (!lead) return lead;
    for (const Member& m : *members) {
        CompArgs each = a;
        each.anyPiece.reset();
        each.pieces = std::vector<std::string>();
        for (const auto& p : m.run) each.pieces->push_back(p.item.str());
        each.allowLinked = true;
        for (std::size_t k = 0; k < each.spans.size(); ++k) {
            each.spans[k].take.reset();
            each.spans[k].index = indices[k];
        }
        auto got = compOne(each, ctx, "on " + trackNamed(s, m.track) + ": ");
        if (!got) return got;
        lead->members.push_back(memberOf(s, m.track, *got));
    }
    finishGroup(*lead, {});
    skipped(*lead);
    return lead;
}

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
            "envelopes. Pieces of one take beside each other are joined (comp.midi_boundary warns "
            "of "
            "MIDI ones that cannot be, where a held note sounds again). "
            "take.activate {index} on a piece changes its take and keeps the seams. "
            "One undo step. Returns the pieces in time order. An item in an edit group (a "
            "multitrack recording, drums): without bypass_groups the same spans, take indices "
            "and crossfade apply to the run of every other track of the group that has items over "
            "the same stretch (however they are cut, however long their crossfades: a whole item "
            "beside a split lead is comped to the spans too); the result lists them under "
            "members, each with its track and pieces. A track whose items over the comp do not "
            "form a run starting and ending where it does (comp.group_mismatch, with the inner "
            "reason and hint), or that has several takes but lacks a take index a span plays "
            "(comp.group_takes), or is locked, refuses the whole call before anything changes. "
            "A track with no item over the comp, or whose items over it have one take each (a "
            "bass grouped with drums), is left alone and named in skipped_tracks with its reason "
            "(no_item, one_take). bypass_groups true comps only the named item.";
        c.tags = {"take"};
        auto spanSchema =
            schema::object<SpanArg>("take_comp_span")
                .field("from", &SpanArg::from, schema::number().min(0).unit(units::Unit::beats))
                .field("to", &SpanArg::to, schema::number().min(0).unit(units::Unit::beats))
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
                .field("crossfade", &CompArgs::crossfade, schema::number().min(0).doc("Seconds."))
                .defaultValue(model::kDefaultCompCrossfade)
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
            {"comp.group_mismatch",
             "a track of the edit group has items over the comp that do not form a run starting "
             "and ending where it does (the inner reason and hint are in the message)",
             "make its items span the same stretch, or comp it alone with bypass_groups"},
            {"comp.group_takes",
             "a track of the edit group has several takes but fewer than a span's index needs",
             "comp it alone with bypass_groups, or use indices it has"},
            {"item.locked", "a piece is locked", "item.set {locked: false}"},
            {"item.kind", "a clip", "lay the clip on the timeline first (item.add {clip})"}};
        c.examples = {{"Take the second take for bars 2 to 3",
                       j(R"({"item":"it_1","crossfade":0,"spans":[{"from":1,"to":3,"index":1}]})"),
                       j(R"({"pieces":"<any>","crossfade":0,"crossfade_shape":"cos","uniform":true,
                          "differing_seams":[],"members":[],"differing_members":[],
                          "skipped_tracks":[]})"),
                       "song_with_takes"}};
        c.run = [](const CompArgs& a, Context& ctx) -> Result<CompResult> {
            return a.allowLinked ? compOne(a, ctx, "") : compGroup(a, ctx);
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
            "fade_in and fade_out the crossfades in seconds. crossfade and crossfade_shape are the "
            "run's own: its longest seam fade (seconds) and that fade's shape, which a seam "
            "shortened by short pieces does not shrink, so take.comp with them rebuilds the same "
            "comp (the default 0.01 and cos for one piece, 0 for hard seams), as long as uniform "
            "is true. When a seam fade was set by hand to another length or shape, uniform is "
            "false and differing_seams lists the seams a take.comp with those crossfade values "
            "would rewrite (seam 0 is between the first two pieces; at in beats; left and right "
            "the items). For an item in an edit group, members reads every other track of the "
            "group that has items over the same stretch like the item's own comp, "
            "differing_members lists tracks whose comp reads differently or cannot follow, and "
            "skipped_tracks the group tracks it leaves alone, each with a reason: no_item, or "
            "one_take (they do not affect uniform, which is false while a member differs or "
            "is not uniform itself). An item with several "
            "takes and no seams is a comp of one piece.";
        c.tags = {"take"};
        c.args = schema::object<WhichArgs>("take_comp_get_args")
                     .field("item", &WhichArgs::anyPiece, timelineItemArg())
                     .field("items", &WhichArgs::pieces,
                            schema::arrayOf(timelineItemArg()).maxItems(4096))
                     .build();
        c.result = resultSchema();
        c.examples = {
            {"Read a comp", j(R"({"item":"it_1"})"),
             j(R"({"pieces":"<any>","crossfade":0.01,"crossfade_shape":"cos","uniform":true,
                          "differing_seams":[],"members":[],"differing_members":[],
                          "skipped_tracks":[]})"),
             "song_with_takes"}};
        c.run = [](const WhichArgs& a, Context& ctx) -> Result<CompResult> {
            const model::Session s = app(ctx).view();
            auto which = whichPieces(s, a.anyPiece, a.pieces);
            if (!which) return std::move(which).error();
            auto run = model::compRun(s, which->first, which->second);
            if (!run) return std::move(run).error();
            CompResult out = describe(s, *run);
            // The other members of its edit group, read the same way; one that does not line up
            // is listed in differing_members.
            std::vector<Id> off;
            std::vector<SkippedOut> left;
            auto members = groupMembers(s, *run, &off, &left,
                                        piecePaths(a.anyPiece.has_value(), which->second, *run));
            if (!members) return std::move(members).error();
            for (const Member& m : *members)
                out.members.push_back(memberOf(s, m.track, describe(s, m.run)));
            std::vector<std::string> notLikeIt;
            for (Id track : off) notLikeIt.push_back(track.str());
            out.skippedTracks = left;
            finishGroup(out, std::move(notLikeIt));
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
