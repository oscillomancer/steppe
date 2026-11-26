// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "mod_source.h"

#include <algorithm>

#include "app_actions.h"
#include "rack_scene.h"
#include "supple/ui/root.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/object_name.h"

namespace supple::gui {

namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

// The cables into the route's Mod Out input, in id order.
std::vector<model::Id> feeders(const model::Session& s, const model::Modulation& m) {
    std::vector<std::pair<model::Id, model::Id>> found;  // cable, module
    for (const auto& c : s.cables)
        if (c.value->to.object == m.source.module && c.value->to.key == m.source.input)
            found.emplace_back(c.id, c.value->from.object);
    std::sort(found.begin(), found.end());
    std::vector<model::Id> out;
    for (const auto& [cable, module] : found) out.push_back(module);
    return out;
}

// Each route's label in a menu that lists `routes`: modRouteLabel, and
// where two read the same (two LFOs in one rack, one of them Random Steps),
// each of those also names its Mod Out input, " · Mod 2".
std::vector<std::string> menuLabels(const model::Session& s, const rack::Catalog* catalog,
                                    const std::vector<model::Id>& routes);

}  // namespace

std::string modSourceName(const model::Session& s, const rack::Catalog* catalog, model::Id route) {
    const model::Modulation* m = s.modulations.find(route);
    if (!m) return {};
    const auto from = feeders(s, *m);
    if (from.empty()) {
        const std::string& input = m->source.input;  // "in_3"
        const auto under = input.rfind('_');
        return "Mod " + (under == std::string::npos ? input : input.substr(under + 1));
    }
    if (catalog) return moduleTitle(s, *catalog, from.front());
    const model::RackModule* mod = s.modules.find(from.front());
    if (!mod) return views::objectName(s, from.front());
    if (!mod->label.empty()) return mod->label;
    const auto dot = mod->type.rfind('.');
    return mod->type.substr(dot == std::string::npos ? 0 : dot + 1);
}

std::string modRouteLabel(const model::Session& s, const rack::Catalog* catalog, model::Id route) {
    const auto view = modSourceView(s, route);
    const std::string name = modSourceName(s, catalog, route);
    return view ? name + " (" + model::rackLabel(s, view->rack) + ")" : name;
}

std::optional<ModSourceView> modSourceView(const model::Session& s, model::Id route) {
    const model::Modulation* m = s.modulations.find(route);
    if (!m) return std::nullopt;
    const model::RackModule* out = s.modules.find(m->source.module);
    if (!out) return std::nullopt;
    ModSourceView view{out->rack, {m->source.module}};
    for (model::Id module : feeders(s, *m)) view.modules.insert(module);
    return view;
}

std::vector<model::Id> modRoutesOfTrack(const model::Session& s, model::Id track) {
    std::vector<model::Id> out;
    for (const auto& m : s.modulations) {
        const model::Id object = m.value->target.object;
        const model::Route* send =
            object.kind == model::Kind::Route ? s.routes.find(object) : nullptr;
        if (object == track ||
            (object.kind == model::Kind::Fx && model::trackOfChainFx(s, object) == track) ||
            (send && send->from.track == track))
            out.push_back(m.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

std::vector<std::string> menuLabels(const model::Session& s, const rack::Catalog* catalog,
                                    const std::vector<model::Id>& routes) {
    std::vector<std::string> labels;
    for (model::Id route : routes) labels.push_back(modRouteLabel(s, catalog, route));
    std::vector<std::string> out = labels;
    for (std::size_t i = 0; i < routes.size(); ++i) {
        if (std::count(labels.begin(), labels.end(), labels[i]) < 2) continue;
        const std::string& input = s.modulations.find(routes[i])->source.input;  // "in_2"
        const auto under = input.rfind('_');
        out[i] += " \u00b7 Mod " + (under == std::string::npos ? input : input.substr(under + 1));
    }
    return out;
}

}  // namespace

void showModSource(AppActions& actions, model::Id route) {
    const actions::Action* action = actions.registry().find("ui.modulation.source");
    if (!action) return;
    json::Value args = json::Value::object();
    args["modulation"] = route.str();
    if (auto r = actions.run(*action, args); !r && actions.onError)
        actions.onError(r.error().message);
}

std::vector<ui::MenuItem> modSourceItems(AppActions& actions, const model::Session& s,
                                         const rack::Catalog* catalog,
                                         const model::EnvTarget& target) {
    std::vector<ui::MenuItem> items;
    const auto routes = views::routesOn(s, target);
    const auto labels = menuLabels(s, catalog, routes);
    AppActions* a = &actions;
    for (std::size_t i = 0; i < routes.size(); ++i)
        items.push_back({routes.size() == 1 ? std::string("Show modulation source")
                                            : "Show modulation source: " + labels[i],
                         "", [a, route = routes[i]] { showModSource(*a, route); }});
    return items;
}

std::vector<ui::MenuItem> modRemoveItems(AppActions& actions, const model::Session& s,
                                         const rack::Catalog* catalog,
                                         const model::EnvTarget& target) {
    std::vector<ui::MenuItem> items;
    AppActions* a = &actions;
    const auto routes = views::routesOn(s, target);
    const auto labels = menuLabels(s, catalog, routes);
    for (std::size_t i = 0; i < routes.size(); ++i)
        items.push_back({"Remove modulation: " + labels[i], "", [a, route = routes[i]] {
                             json::Value args = json::Value::object();
                             args["modulation"] = route.str();
                             const auto r = a->dispatcher().execute(
                                 {"modulation.remove", std::move(args), kOrigin});
                             if (!r && a->onError) a->onError(r.error().message);
                         }});
    return items;
}

std::vector<ui::MenuItem> modRouteItems(AppActions& actions, const model::Session& s,
                                        const rack::Catalog* catalog,
                                        const model::EnvTarget& target) {
    auto items = modSourceItems(actions, s, catalog, target);
    for (auto& item : modRemoveItems(actions, s, catalog, target)) items.push_back(std::move(item));
    return items;
}

void openModSource(ui::Root& root, AppActions& actions, const model::Session& s,
                   const rack::Catalog* catalog, const model::EnvTarget& target,
                   const ui::Rect& anchor) {
    const auto routes = views::routesOn(s, target);
    if (routes.empty()) return;
    if (routes.size() == 1) {
        showModSource(actions, routes.front());
        return;
    }
    std::vector<ui::MenuItem> items;
    AppActions* a = &actions;
    const auto labels = menuLabels(s, catalog, routes);
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const model::Id route = routes[i];
        const model::Modulation* m = s.modulations.find(route);
        std::string label = labels[i];
        if (m && !m->enabled) label += " (off)";
        items.push_back({label, "", [a, route] { showModSource(*a, route); }});
    }
    ui::Root* r = &root;
    root.openPopup(
        ui::makeMenu("modulation.source.menu", std::move(items), [r] { r->closePopup(); }), anchor,
        ui::Placement::Below);
}

}  // namespace supple::gui
