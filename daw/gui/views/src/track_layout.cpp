// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/track_layout.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <utility>

#include "supple/views/envelope_lane.h"
#include "supple/views/session_changes.h"

namespace supple::views {
namespace {

float clampHeight(float h) {
    return std::clamp(h, TrackLayout::kMinHeight, TrackLayout::kMaxHeight);
}

}  // namespace

std::vector<model::Id> displayChildren(const model::Session& session, const model::Track& parent) {
    std::vector<model::Id> out;
    out.reserve(parent.children.size());
    for (model::StripType type :
         {model::StripType::Channel, model::StripType::Buss, model::StripType::Master})
        for (model::Id id : parent.children)
            if (const model::Track* t = session.tracks.find(id); t && t->type == type)
                out.push_back(id);
    return out;
}

std::vector<model::Id> stripOrder(const model::Session& session) {
    std::vector<model::Id> order;
    std::function<void(model::Id)> walk = [&](model::Id id) {
        const model::Track* t = session.tracks.find(id);
        if (!t) return;
        order.push_back(id);
        for (model::Id child : displayChildren(session, *t)) walk(child);
    };
    for (model::Id m : session.project->masters) walk(m);
    std::vector<model::Id> grouped;
    grouped.reserve(order.size());
    for (model::StripType type :
         {model::StripType::Channel, model::StripType::Buss, model::StripType::Master})
        for (model::Id id : order)
            if (session.tracks.find(id)->type == type) grouped.push_back(id);
    return grouped;
}

std::set<model::Id> masterLanes(const model::Session& session) {
    std::set<model::Id> out;
    for (model::Id m : session.project->masters)
        if (const model::Track* t = session.tracks.find(m); t && t->record.armed) out.insert(m);
    for (const auto& e : session.items)
        if (const model::Track* t = session.tracks.find(e.value->track);
            t && t->type == model::StripType::Master)
            out.insert(e.value->track);
    return out;
}

std::optional<std::size_t> reorderIndex(const model::Session& session, model::Id track,
                                        std::optional<model::Id> before) {
    const model::Track* t = session.tracks.find(track);
    if (!t || t->type == model::StripType::Master || !t->parent) return std::nullopt;
    const model::Track* parent = session.tracks.find(*t->parent);
    if (!parent || before == track) return std::nullopt;
    std::vector<model::Id> rest = parent->children;
    std::erase(rest, track);
    std::optional<std::size_t> at;
    if (before) {
        const model::Track* b = session.tracks.find(*before);
        if (!b || b->type != t->type || b->parent != t->parent) return std::nullopt;
        if (const auto found = std::find(rest.begin(), rest.end(), *before); found != rest.end())
            at = static_cast<std::size_t>(found - rest.begin());
    } else {  // after the last sibling of this type
        for (std::size_t i = 0; i < rest.size(); ++i)
            if (const model::Track* s = session.tracks.find(rest[i]); s && s->type == t->type)
                at = i + 1;
    }
    if (!at) return std::nullopt;
    // The siblings of this type, before and after: the same order is no move.
    auto sameType = [&](const std::vector<model::Id>& ids) {
        std::vector<model::Id> out;
        for (model::Id id : ids)
            if (const model::Track* s = session.tracks.find(id); s && s->type == t->type)
                out.push_back(id);
        return out;
    };
    std::vector<model::Id> after = rest;
    after.insert(after.begin() + static_cast<std::ptrdiff_t>(*at), track);
    if (sameType(after) == sameType(parent->children)) return std::nullopt;
    return at;
}

std::vector<model::Id> reorderGroup(const model::Session& session, model::Id track,
                                    const std::function<bool(model::Id)>& selected) {
    const model::Track* t = session.tracks.find(track);
    const model::Track* parent = t && t->parent ? session.tracks.find(*t->parent) : nullptr;
    if (!parent || !selected || !selected(track)) return {track};
    std::vector<model::Id> group;
    for (model::Id id : parent->children)
        if (const model::Track* s = session.tracks.find(id);
            s && s->type == t->type && (id == track || selected(id)))
            group.push_back(id);
    return group;
}

std::vector<TrackMove> reorderMoves(const model::Session& session, model::Id track,
                                    const std::function<bool(model::Id)>& selected,
                                    std::optional<model::Id> before) {
    const model::Track* t = session.tracks.find(track);
    if (!t || t->type == model::StripType::Master || !t->parent) return {};
    const model::Track* parent = session.tracks.find(*t->parent);
    if (!parent) return {};
    const std::vector<model::Id> group = reorderGroup(session, track, selected);
    auto inGroup = [&](model::Id id) {
        return std::find(group.begin(), group.end(), id) != group.end();
    };
    if (before && inGroup(*before)) return {};
    std::vector<model::Id> rest;
    for (model::Id id : parent->children)
        if (!inGroup(id)) rest.push_back(id);
    std::optional<std::size_t> at;
    if (before) {
        const model::Track* b = session.tracks.find(*before);
        if (!b || b->type != t->type || b->parent != t->parent) return {};
        if (const auto found = std::find(rest.begin(), rest.end(), *before); found != rest.end())
            at = static_cast<std::size_t>(found - rest.begin());
    } else {  // after the last sibling of this type
        for (std::size_t i = 0; i < rest.size(); ++i)
            if (const model::Track* s = session.tracks.find(rest[i]); s && s->type == t->type)
                at = i + 1;
    }
    if (!at) return {};
    std::vector<model::Id> order = rest;
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(*at), group.begin(), group.end());
    auto sameType = [&](const std::vector<model::Id>& ids) {
        std::vector<model::Id> out;
        for (model::Id id : ids)
            if (const model::Track* s = session.tracks.find(id); s && s->type == t->type)
                out.push_back(id);
        return out;
    };
    if (sameType(order) == sameType(parent->children)) return {};  // the same order: no move
    // Last first, each in front of what follows it in the new order: the
    // tracks after it are already where they end up.
    std::vector<TrackMove> moves;
    std::vector<model::Id> now = parent->children;
    for (auto g = group.rbegin(); g != group.rend(); ++g) {
        const auto next = std::find(order.begin(), order.end(), *g) + 1;
        std::erase(now, *g);
        const auto to = next == order.end() ? now.end() : std::find(now.begin(), now.end(), *next);
        const auto index = static_cast<std::size_t>(to - now.begin());
        now.insert(now.begin() + static_cast<std::ptrdiff_t>(index), *g);
        moves.push_back({*g, index});
    }
    return moves;
}

bool TrackLayout::show(const model::Session& session, const std::vector<model::Change>* changes,
                       std::uint64_t generation) {
    const bool replaced = generation != generation_;
    if (!replaced && session.version == indexed_) return false;
    if (replaced) changes = nullptr;
    generation_ = generation;
    bool geometry = replaced || indexed_ == ~std::uint64_t{0} ||
                    session.project->masters != session_.project->masters ||
                    !session.groups.sameStorage(session_.groups);
    for (auto id : changedObjects(session_.tracks, session.tracks, changes)) {
        const auto* before = session_.tracks.find(id);
        const auto* after = session.tracks.find(id);
        geometry |= !before || !after || before->type != after->type ||
                    before->parent != after->parent || before->children != after->children ||
                    before->hiddenTcp != after->hiddenTcp ||
                    before->folderCompact != after->folderCompact ||
                    before->chain != after->chain || before->inputChain != after->inputChain ||
                    (after->type == model::StripType::Master &&
                     before->record.armed != after->record.armed);
    }
    // A master's lane comes and goes with its items (ADR-0057).
    if (!geometry) {
        auto onMaster = [](const model::Session& in, const model::Item* it) {
            const model::Track* t = it ? in.tracks.find(it->track) : nullptr;
            return t && t->type == model::StripType::Master;
        };
        for (auto id : changedObjects(session_.items, session.items, changes))
            if (onMaster(session_, session_.items.find(id)) ||
                onMaster(session, session.items.find(id))) {
                geometry |= masterLanes(session) != masterLanes_;
                break;
            }
    }
    for (auto id : changedObjects(session_.envelopes, session.envelopes, changes)) {
        const auto* before = session_.envelopes.find(id);
        const auto* after = session.envelopes.find(id);
        auto lane = [](const model::Envelope* env) {
            return env && env->visible &&
                   (env->target.object.kind == model::Kind::Track ||
                    env->target.object.kind == model::Kind::Group ||
                    env->target.object.kind == model::Kind::Fx ||
                    env->target.object.kind == model::Kind::Route);
        };
        geometry |= (lane(after) && scaleOf(*after).has_value()) != scaledEnvs_.contains(id);
        geometry |=
            lane(before) != lane(after) || (lane(before) && lane(after) &&
                                            (before->target.object != after->target.object ||
                                             before->target.param != after->target.param));
    }
    // Changing the plug-in can change whether its parameter lanes have scales.
    for (auto id : changedObjects(session_.fx, session.fx, changes)) {
        const auto* before = session_.fx.find(id);
        const auto* after = session.fx.find(id);
        geometry |= !before || !after || before->plugin.id != after->plugin.id ||
                    before->plugin.format != after->plugin.format ||
                    before->plugin.version != after->plugin.version;
    }
    // A send that leaves another strip now takes its lanes there.
    for (auto id : changedObjects(session_.routes, session.routes, changes)) {
        const auto* before = session_.routes.find(id);
        const auto* after = session.routes.find(id);
        geometry |= !before || !after || before->from.track != after->from.track;
    }
    // An expanded item that changed may hold another number of takes now.
    bool heights = false;
    if (!expanded_.empty())
        for (auto id : changedObjects(session_.items, session.items, changes))
            heights |= expanded_.contains(id);
    session_ = session;
    indexed_ = session.version;
    if (!geometry) {
        if (!heights) return false;
        pruneExpanded();
        rebuildGeometry();
        return true;
    }
    ++rebuilds_;
    rows_.clear();
    trackRows_.clear();
    envRows_.clear();
    scaledEnvs_.clear();
    std::map<model::Id, model::Id> owners;
    for (const auto& track : session_.tracks) {
        for (auto fx : track.value->chain) owners[fx] = track.id;
        for (auto fx : track.value->inputChain) owners[fx] = track.id;
    }
    // A send's lanes are under the strip it leaves (hardware inputs have none).
    for (const auto& route : session_.routes)
        if (route.value->from.track) owners[route.id] = *route.value->from.track;
    std::map<model::Id, std::vector<std::pair<int, model::Id>>> lanes;
    for (const auto& envelope : session_.envelopes) {
        if (!envelope.value->visible || !scaleOf(*envelope.value)) continue;
        if (envelope.value->target.object.kind == model::Kind::Track ||
            envelope.value->target.object.kind == model::Kind::Group ||
            envelope.value->target.object.kind == model::Kind::Fx ||
            envelope.value->target.object.kind == model::Kind::Route)
            scaledEnvs_.insert(envelope.id);
        auto owner = envelope.value->target.object;
        if (owner.kind == model::Kind::Fx || owner.kind == model::Kind::Route) {
            const auto found = owners.find(owner);
            if (found == owners.end()) continue;
            owner = found->second;
        }
        if (owner.kind == model::Kind::Track || owner.kind == model::Kind::Group)
            lanes[owner].emplace_back(envOrder(*envelope.value), envelope.id);
    }
    for (auto& [track, entries] : lanes) std::sort(entries.begin(), entries.end());
    masterLanes_ = masterLanes(session_);
    std::function<void(model::Id, int)> visit = [&](model::Id id, int depth) {
        const model::Track* t = session_.tracks.find(id);
        if (!t) return;
        const bool master = t->type == model::StripType::Master;
        // A master has a row only as its lane: armed or holding items (ADR-0057).
        const bool row = (!master || masterLanes_.contains(id)) && !t->hiddenTcp;
        if (row) {
            trackRows_[id] = rows_.size();
            rows_.push_back({id, t->type, depth, std::nullopt});
        }
        // Master strip rows are otherwise hidden, but their visible automation
        // lanes precede the master's children, including the global playrate lane.
        if (row || master)
            if (const auto found = lanes.find(id); found != lanes.end())
                for (const auto& lane : found->second) {
                    envRows_[lane.second] = rows_.size();
                    rows_.push_back({id, t->type, depth, lane.second});
                }
        if (t->folderCompact == 2) return;  // a collapsed folder hides its children
        // A master's children stay at its depth, lane or not.
        for (model::Id c : displayChildren(session_, *t))
            visit(c, row && !master ? depth + 1 : depth);
    };
    for (const auto& g : session_.groups)
        if (g.value->vca)
            for (const auto& lane : lanes[g.id]) {
                envRows_[lane.second] = rows_.size();
                rows_.push_back({g.id, model::StripType::Buss, 0, lane.second});
            }
    for (model::Id master : session_.project->masters) visit(master, 0);
    // Drop overrides for tracks that are gone (they would never be read again).
    for (auto it = heights_.begin(); it != heights_.end();)
        it = session_.tracks.contains(it->first) ? std::next(it) : heights_.erase(it);
    pruneExpanded();
    rebuildGeometry();
    return true;
}

void TrackLayout::setTakesExpanded(model::Id item, bool on) {
    const model::Item* it = session_.items.find(item);
    if (on && (!it || it->takes.size() < 2)) return;
    if (on ? !expanded_.insert(item).second : expanded_.erase(item) == 0) return;
    rebuildGeometry();
}

void TrackLayout::clearTakesExpanded() {
    if (expanded_.empty()) return;
    expanded_.clear();
    rebuildGeometry();
}

bool TrackLayout::pruneExpanded() {
    const auto before = expanded_.size();
    std::erase_if(expanded_, [&](model::Id id) {
        const model::Item* it = session_.items.find(id);
        return !it || it->takes.size() < 2;
    });
    return expanded_.size() != before;
}

std::optional<std::size_t> TrackLayout::indexOf(model::Id track) const {
    const auto found = trackRows_.find(track);
    return found == trackRows_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<std::size_t> TrackLayout::indexOfEnv(model::Id env) const {
    const auto found = envRows_.find(env);
    return found == envRows_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<EnvScale> TrackLayout::scaleOf(const model::Envelope& env) const {
    if (auto mixer = envScale(env)) return mixer;
    if (env.target.object.kind == model::Kind::Fx && pluginScales) return pluginScales(env);
    return std::nullopt;
}

float TrackLayout::envHeight() const { return clampHeight(std::max(28.0f, globalHeight_ * 0.6f)); }

void TrackLayout::setGlobalHeight(float h) {
    const float clamped = clampHeight(h);
    if (clamped == globalHeight_) return;
    globalHeight_ = clamped;
    rebuildGeometry();
}

void TrackLayout::adjustAllTrackHeights(float delta) {
    if (delta == 0 || !std::isfinite(delta)) return;
    const float global = clampHeight(globalHeight_ + delta);
    const auto expanded = expandedTakeCounts();
    auto heights = heights_;
    for (const auto& entry : session_.tracks) {
        const auto& track = *entry.value;
        if (track.type == model::StripType::Master && !indexOf(entry.id)) continue;
        float share = delta;
        if (const auto found = expanded.find(entry.id); found != expanded.end())
            share /= static_cast<float>(found->second);
        const float height = clampHeight(baseHeight(entry.id) + share);
        const float inherited = track.folderCompact == 1 ? clampHeight(global * 0.5f) : global;
        // Retain explicit overrides; default rows can keep following global
        // zoom. Compact and expanded rows need an override for an equal step.
        if (hasTrackHeight(entry.id) || height != inherited) heights[entry.id] = height;
    }
    if (global == globalHeight_ && heights == heights_) return;
    globalHeight_ = global;
    heights_ = std::move(heights);
    rebuildGeometry();
}

void TrackLayout::setTrackHeight(model::Id track, float h) {
    if (h <= 0)
        heights_.erase(track);
    else
        heights_[track] = clampHeight(h);
    rebuildGeometry();
}

void TrackLayout::clearTrackHeights() {
    heights_.clear();
    rebuildGeometry();
}

float TrackLayout::baseHeight(model::Id track) const {
    if (auto it = heights_.find(track); it != heights_.end()) return it->second;
    const model::Track* t = session_.tracks.find(track);
    if (t && t->folderCompact == 1) return clampHeight(globalHeight_ * 0.5f);
    return globalHeight_;
}

std::map<model::Id, std::size_t> TrackLayout::expandedTakeCounts() const {
    std::map<model::Id, std::size_t> counts;
    for (model::Id id : expanded_)
        if (const model::Item* it = session_.items.find(id))
            counts[it->track] = std::max(counts[it->track], it->takes.size());
    return counts;
}

void TrackLayout::rebuildGeometry() {
    rowHeights_.clear();
    yOffsets_.clear();
    rowHeights_.reserve(rows_.size());
    yOffsets_.reserve(rows_.size());
    // Rows holding an expanded item: a lane per take, each as tall as the row.
    const auto spread = expandedTakeCounts();
    float y = 0;
    for (const TrackRow& r : rows_) {
        float h = r.env ? envHeight() : baseHeight(r.track);
        if (!r.env)
            if (const auto found = spread.find(r.track); found != spread.end())
                h = std::max(h, kTakeBar + static_cast<float>(found->second) * h);
        yOffsets_.push_back(y);
        rowHeights_.push_back(h);
        y += h;
    }
    contentHeight_ = y;
    if (onChanged) onChanged();
}

}  // namespace supple::views
