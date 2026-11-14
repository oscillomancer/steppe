// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// Types and helpers shared by the command files split from the former
// extra_commands.cpp; Empty and the using-declarations come from
// command_helpers.h (routes, rulers, plug-ins, item edits, takes, FX patches).
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

#include "command_helpers.h"
#include "command_validation.h"
#include "supple/engine/app.h"
#include "supple/model/audio_warp.h"
#include "supple/model/env_shape.h"
#include "supple/model/numeric.h"
#include "supple/model/schemas.h"
#include "supple/time/timeline.h"

namespace supple::engine {

inline EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
inline json::Value j(std::string_view s) { return json::parse(s).value(); }

template <class T>
Result<Id> require(const model::Session& s, const std::string& text, std::string path) {
    auto id = Id::parse(text);
    if (id && id->kind == model::Traits<T>::kind && model::Traits<T>::map(s).contains(*id))
        return *id;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown", "no " + text,
                 std::move(path), "use a live id from the corresponding list command");
}

inline auto idSchema(const char* prefix) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$", "object id");
}

// A model entry the session itself refers to (an item's takes, a take's chain):
// an id the model hands back must resolve, and a dangling one is a structured
// error rather than a crash.
template <class T>
Result<const T*> dangling(const model::Session& s, Id id, std::string path) {
    const T* found = model::Traits<T>::map(s).find(id);
    if (found) return found;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown",
                 id.str() + " is missing from the model", std::move(path),
                 "the session refers to it; reload the project or fix the file");
}

struct IdResult {
    std::string id;
};

inline auto idResultSchema() {
    return schema::object<IdResult>("created_id")
        .field("id", &IdResult::id, schema::string())
        .build();
}

inline std::optional<std::vector<Id>> routeCycle(const model::Session& s, Id from, Id to,
                                                 std::optional<Id> ignored = {}) {
    if (from == to) return std::vector<Id>{from, to};
    std::map<Id, std::vector<Id>> edges;
    for (const auto& e : s.tracks)
        if (e.value->parent && e.value->parentSend.enabled) edges[e.id].push_back(*e.value->parent);
    // Racks are nodes too (ADR-0062): a send reaches an In module's rack, an
    // Out module's rack feeds its channel.
    auto rackOf = [&](Id module) -> std::optional<Id> {
        const model::RackModule* m = s.modules.find(module);
        return m ? std::optional<Id>(m->rack) : std::nullopt;
    };
    for (const auto& e : s.routes) {
        if (e.id == ignored) continue;
        const auto source =
            e.value->from.module ? rackOf(*e.value->from.module) : e.value->from.track;
        const auto target = e.value->to.module ? rackOf(*e.value->to.module)
                            : e.value->to.fx   ? model::trackOfFx(s, *e.value->to.fx)
                                               : e.value->to.track;
        if (source && target) edges[*source].push_back(*target);
        if (source && e.value->midi && e.value->to.track)
            for (Id module : model::midiFollowers(s, *e.value->to.track, source))
                edges[*source].push_back(s.modules.find(module)->rack);
    }
    std::set<Id> seen;
    std::vector<Id> path{to};
    std::function<bool(Id)> walk = [&](Id cur) {
        if (cur == from) return true;
        if (!seen.insert(cur).second) return false;
        for (Id next : edges[cur]) {
            path.push_back(next);
            if (walk(next)) return true;
            path.pop_back();
        }
        return false;
    };
    if (!walk(to)) return {};
    path.push_back(to);
    return path;
}

// A cycle's path by name, a rack marked as one: "Keys -> Synth (rack) ->
// Synth". With ids, each name is followed by its id instead, "Keys (tr_2) ->
// Synth (rk_1)", for a message an agent acts on.
inline std::string cycleText(const model::Session& s, const std::vector<Id>& path, bool ids) {
    std::string out;
    for (Id at : path) {
        const bool rack = at.kind == model::Kind::Rack;
        out += out.empty() ? "" : " -> ";
        out += rack ? model::rackLabel(s, at) : model::trackLabel(s, at);
        if (ids || rack) out += " (" + (ids ? at.str() : std::string("rack")) + ")";
    }
    return out;
}

}  // namespace supple::engine
