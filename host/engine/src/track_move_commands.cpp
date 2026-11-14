// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// track.move: reorder tracks and change folder nesting.
#include "extra_command_helpers.h"

namespace supple::engine {

namespace {

struct TrackMoveArgs {
    std::string track;
    std::optional<std::string> parent;
    std::optional<std::int64_t> index;
};

// Is `track` a descendant of `ancestor` in the folder nesting (children lists)?
// Moving a track under one of its own descendants would make the tree cyclic.
bool nestedUnder(const model::Session& s, Id ancestor, Id track) {
    std::vector<Id> stack{track};
    std::set<Id> seen;
    while (!stack.empty()) {
        const Id cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        if (cur == ancestor) return true;
        const model::Track* found = s.tracks.find(cur);
        if (!found) continue;
        for (Id child : found->children) stack.push_back(child);
    }
    return false;
}

}  // namespace

void registerTrackMoveCommands(cmd::Registry& registry) {
    [&] {
        Command<TrackMoveArgs, Empty> c;
        c.name = "track.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move a track under another track or reorder it among its siblings.";
        c.description =
            "parent defaults to the track's current parent (a pure reorder); index defaults to "
            "the end of the destination's children. A track cannot move under itself or one of "
            "its own descendants.";
        c.tags = {"track"};
        c.args = schema::object<TrackMoveArgs>("track_move_args")
                     .field("track", &TrackMoveArgs::track, idSchema("tr"))
                     .field("parent", &TrackMoveArgs::parent, idSchema("tr"))
                     .field("index", &TrackMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.errors = {{"tr.cycle", "the destination is inside the track", "choose another parent"}};
        c.examples = {{"Move a track to the master", j(R"({"track":"tr_1","parent":"tr_0"})"),
                       j("{}"), "song"}};
        c.run = [](const TrackMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Track>(s, a.track, "/track");
            if (!id) return std::move(id).error();
            if (*id == model::kMaster)
                return Error("track.strip_type", "tr_0 must remain the first root", "/track",
                             "reorder another master or move a channel/buss instead");
            const model::Track& track = *s.tracks.find(*id);
            if (track.type == model::StripType::Master) {
                if (a.parent)
                    return Error("track.strip_type", "masters have no parent", "/parent",
                                 "omit parent to reorder a master");
                auto p = app(ctx).txn().project();
                std::erase(p.masters, *id);
                auto at = a.index ? std::clamp<std::size_t>(*a.index, 1, p.masters.size())
                                  : p.masters.size();
                p.masters.insert(p.masters.begin() + static_cast<std::ptrdiff_t>(at), *id);
                app(ctx).txn().setProject(std::move(p));
                return Empty{};
            }
            const Id from = *track.parent;
            Id target = from;
            if (a.parent) {
                auto parent = require<model::Track>(s, *a.parent, "/parent");
                if (!parent) return std::move(parent).error();
                target = *parent;
            }
            if (target == *id)
                return Error("tr.cycle", "a track cannot be moved under itself", "/parent",
                             "choose another parent");
            if (target != model::kMaster && nestedUnder(s, *id, target))
                return Error("tr.cycle", "a track cannot be moved under one of its own children",
                             "/parent", "choose a track outside it");
            // The send graph: a folder send or a route that already reaches the
            // track would close a feedback loop (the same check route.add uses).
            if (target != from)
                if (auto cycle = routeCycle(s, *id, target)) {
                    return Error("tr.cycle",
                                 "the move creates a cycle: " + cycleText(s, *cycle, true),
                                 "/parent", "choose a destination that does not feed this track");
                }
            if (s.tracks.find(target)->type == model::StripType::Channel)
                return Error("track.strip_type", "a channel cannot have children", "/parent",
                             "add a buss and place the channels under it");
            model::EditTxn& t = app(ctx).txn();
            t.modify<model::Track>(from, [&](model::Track& p) {
                p.children.erase(std::remove(p.children.begin(), p.children.end(), *id),
                                 p.children.end());
            });
            t.modify<model::Track>(target, [&](model::Track& p) {
                const auto at = a.index ? std::min<std::size_t>(static_cast<std::size_t>(*a.index),
                                                                p.children.size())
                                        : p.children.size();
                p.children.insert(p.children.begin() + static_cast<std::ptrdiff_t>(at), *id);
            });
            t.modify<model::Track>(*id, [&](model::Track& tr) { tr.parent = target; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
