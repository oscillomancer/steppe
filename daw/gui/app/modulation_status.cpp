// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "modulation_status.h"

#include <utility>

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

}  // namespace

std::map<model::Id, std::string> inertRoutes(cmd::Dispatcher& dispatcher) {
    std::map<model::Id, std::string> out;
    auto r = dispatcher.execute({"modulation.list", json::Value::object(), kOrigin});
    if (!r) return out;
    const json::Value* list = r->find("modulations");
    if (!list || !list->isArray()) return out;
    for (const json::Value& row : list->asArray()) {
        const json::Value* id = row.find("modulation");
        const json::Value* inert = row.find("inert");
        if (!id || !id->isString() || !inert || !inert->isString()) continue;
        if (auto parsed = model::Id::parse(id->asString())) out[*parsed] = inert->asString();
    }
    return out;
}

std::string inertWords(const std::string& reason) {
    if (reason == "source") return "nothing is cabled into its source";
    if (reason == "envelope") return "the envelope that carries it is off";
    if (reason == "latency") return "off since the plug-in's latency changed";
    return "the parameter cannot take it";
}

std::string takeIdleWords(const model::Session& s, model::Id fx) {
    const auto owner = model::takeFxOwner(s, fx);
    const model::Item* item =
        owner && owner->owner.kind == model::Kind::Item ? s.items.find(owner->owner) : nullptr;
    if (item && item->mute && item->activeTake == owner->take) return "its item is muted";
    if (owner && owner->owner.kind == model::Kind::Clip) return "another take of its clip plays";
    return "another take of its item plays";
}

std::optional<ui::MenuItem> inertItem(cmd::Dispatcher& dispatcher, const model::Session& s,
                                      const model::EnvTarget& target) {
    std::size_t routes = 0;
    std::optional<std::string> reason;
    std::map<model::Id, std::string> inert;
    bool listed = false;
    for (const auto& m : s.modulations) {
        if (!m.value->enabled || !(m.value->target == target)) continue;
        if (!listed) {
            inert = inertRoutes(dispatcher);
            listed = true;
        }
        ++routes;
        if (auto it = inert.find(m.id); it != inert.end() && !reason) reason = it->second;
    }
    if (!reason) return std::nullopt;
    ui::MenuItem m;
    m.label = (routes == 1 ? "Modulation idle: " : "A modulation is idle: ") +
              (*reason == "take" ? takeIdleWords(s, target.object) : inertWords(*reason));
    m.enabled = false;
    return m;
}

std::map<model::Id, double> LiveModulation::read(cmd::Dispatcher& dispatcher,
                                                 const engine::EngineApp& app) {
    std::map<model::Id, double> live = app.modulationOffsets();
    if (live.empty()) return live;  // nothing plays: every route is idle
    std::vector<model::Id> reported;
    reported.reserve(live.size());
    for (const auto& [id, offset] : live) reported.push_back(id);
    const model::Session& s = app.head();
    if (s.version != version_ || app.projectGeneration() != generation_ || reported != reported_) {
        inert_ = inertRoutes(dispatcher);
        version_ = s.version;
        generation_ = app.projectGeneration();
        reported_ = std::move(reported);
    }
    for (const auto& [id, reason] : inert_) live.erase(id);
    return live;
}

}  // namespace supple::gui
