// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "take_follow.h"

#include <algorithm>
#include <set>

#include "clip_context_edit.h"
#include "supple/model/context.h"
#include "supple/model/follow.h"
#include "supple/time/timeline.h"
#include "supple/views/lanes.h"
#include "supple/views/launcher.h"

namespace supple::gui {

namespace {

const model::Take* heardTake(const model::Session& s, model::Id item) {
    const model::Item* it = s.items.find(item);
    return it && it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
}

}  // namespace

std::string takeLeadSheet(const model::Session& s, const model::Item& item, const model::Take& take,
                          const std::vector<model::ContextPoint>& points) {
    // take.set's bars: the meter at the item's position, bar 1 at context
    // beat 0 (the engine's takeBarMap).
    const time::TimeMap& tm = *s.timeMap;
    const int bar = tm.barBeat(std::max(0.0, model::itemSpan(s, item).startBeat)).bar;
    const time::MeterChange m = tm.meterAt(bar);
    const time::TimeMap map = time::TimeMap::constant(120.0, m.numerator, m.denominator);
    return clipLeadSheet(points, map, take.contextPeriod.value_or(0.0));
}

std::vector<model::ContextPoint> songContextUnder(const model::Session& s, model::Id itemId) {
    std::vector<model::ContextPoint> out;
    const model::Item* item = s.items.find(itemId);
    const model::Take* take = heardTake(s, itemId);
    if (!item || !take || !model::takeIsMidi(*take)) return out;
    const model::ItemSpan span = model::itemSpan(s, *item);
    const double start = span.startBeat;
    const double end = start + model::itemLengthBeats(s, *item);
    const model::ContextView& view = *s.contextView;
    auto take_ = [&](const std::vector<model::ContextView::Entry>& entries) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const model::ContextView::Entry& e = entries[i];
            if (e.beat >= end - model::kPlaceSlack) break;
            // The point that holds at the item's start, carried to it; the
            // ones before it are not heard.
            const bool next = i + 1 < entries.size();
            if (e.beat < start - model::kPlaceSlack && next &&
                entries[i + 1].beat <= start + model::kPlaceSlack)
                continue;
            const model::ContextPoint* p = s.context.find(e.id);
            const auto place =
                p ? model::placeAt(s, itemId, std::max(start, e.beat)) : std::nullopt;
            if (!place) continue;
            model::ContextPoint q = *p;
            q.beat = std::max(0.0, place->contextBeat);
            out.push_back(std::move(q));
        }
    };
    take_(view.keys);
    take_(view.chords);
    std::sort(out.begin(), out.end(),
              [](const model::ContextPoint& a, const model::ContextPoint& b) {
                  return a.beat != b.beat ? a.beat < b.beat : a.type < b.type;
              });
    return out;
}

std::optional<std::string> followRefusal(const model::Session& s, model::Id item) {
    const model::Take* take = heardTake(s, item);
    if (!take || !model::takeIsMidi(*take)) return std::string("a MIDI take follows");
    if (take->context.empty()) {
        const auto song = songContextUnder(s, item);
        const bool key = std::any_of(song.begin(), song.end(), [](const model::ContextPoint& p) {
            return p.type == model::ContextType::Key && model::parseKey(p);
        });
        if (!key) return std::string("no key in the song here");
        return std::nullopt;
    }
    if (!model::takeCanFollow(*take)) return std::string("needs a key in its context");
    return std::nullopt;
}

Result<json::Value> takeFollowArgs(const model::Session& s, model::Id itemId,
                                   model::ClipFollow follow) {
    const model::Item* item = s.items.find(itemId);
    const model::Take* take = heardTake(s, itemId);
    if (!item || !item->activeTake || !take)
        return Error("gui.follow", "the item has no take to follow", "/take",
                     "choose an item with a MIDI take");
    json::Value args = json::Value::object();
    args["take"] = item->activeTake->str();
    args["follow"] = follow == model::ClipFollow::Key      ? "key"
                     : follow == model::ClipFollow::Chords ? "chords"
                                                           : "none";
    if (follow != model::ClipFollow::None && take->context.empty())
        args["context"] = takeLeadSheet(s, *item, *take, songContextUnder(s, itemId));
    return args;
}

std::vector<model::Id> bakeable(const model::Session& s, const std::vector<model::Id>& items) {
    std::vector<model::Id> out;
    for (model::Id id : items)
        if (const model::Take* take = heardTake(s, id);
            take && take->follow != model::ClipFollow::None)
            out.push_back(id);
    return out;
}

Result<std::string> bakeItems(cmd::Dispatcher& dispatcher, const cmd::Origin& origin,
                              const model::Session& before, const std::vector<model::Id>& items) {
    const std::vector<model::Id> ids = bakeable(before, items);
    if (ids.empty())
        return Error("take.not_following", "nothing to bake: no selected item's take follows",
                     "/take", "set Follow to Key or Chords first, or there is nothing to bake");
    // What is heard now, for the words: read before the session moves on.
    std::string name;
    std::size_t notes = 0;
    int passes = 0;
    {
        const model::Item* item = before.items.find(ids.front());
        name = item ? views::Lanes::itemLabel(before, *item) : std::string("The item");
        std::set<std::uint32_t> heard;
        for (const model::SoundingNote& n :
             model::soundingNotes(before, ids.front(), -1e18, 1e18)) {
            heard.insert(n.note);
            passes = std::max(passes, n.pass + 1);
        }
        notes = heard.size();
    }
    std::vector<std::string> takes;
    for (model::Id id : ids) takes.push_back(before.items.find(id)->activeTake->str());
    bool unrolled = false;
    auto r = dispatcher.transact("Bake into notes", origin, [&]() -> Result<json::Value> {
        json::Value last = json::Value::object();
        for (const std::string& take : takes) {
            json::Value args = json::Value::object();
            args["take"] = take;
            auto one = dispatcher.execute({"take.bake", std::move(args), origin});
            if (!one) return one;
            if (const json::Value* u = one->find("unrolled"); u && u->isBool() && u->asBool())
                unrolled = true;
            last = std::move(*one);
        }
        return last;
    });
    if (!r) return std::move(r).error();
    if (ids.size() > 1)
        return std::to_string(ids.size()) + " items baked: their notes written as they sound";
    return views::bakeNotice(name, notes, passes, unrolled);
}

}  // namespace supple::gui
