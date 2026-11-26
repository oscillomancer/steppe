// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "mod_source.h"

#include <algorithm>
#include <functional>

#include "app_actions.h"
#include "rack_scene.h"
#include "supple/model/freeze.h"
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
// where two read the same, what tells them apart, so no two rows are alike:
// the Mod Out input (" · Mod 2", two LFOs in one rack, one of them Random
// Steps), then the rack's place among racks of that name (" · rack 2").
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
    std::vector<std::string> out;
    for (model::Id route : routes) out.push_back(modRouteLabel(s, catalog, route));
    // Adds `suffix(i)` to each label that another one still reads as.
    auto separate = [&](const std::function<std::string(std::size_t)>& suffix) {
        const std::vector<std::string> before = out;
        for (std::size_t i = 0; i < out.size(); ++i)
            if (std::count(before.begin(), before.end(), before[i]) > 1) out[i] += suffix(i);
    };
    // Two sources that read the same in one rack: their Mod Out inputs.
    separate([&](std::size_t i) {
        const std::string& input = s.modulations.find(routes[i])->source.input;  // "in_2"
        const auto under = input.rfind('_');
        return " \u00b7 Mod " + (under == std::string::npos ? input : input.substr(under + 1));
    });
    // In two racks that read the same (a name given twice, or two unnamed
    // racks on one track): each rack's place among the racks so named.
    separate([&](std::size_t i) {
        const auto view = modSourceView(s, routes[i]);
        if (!view) return std::string();
        const std::string label = model::rackLabel(s, view->rack);
        std::size_t place = 0;
        for (const auto& r : s.racks) {
            if (model::rackLabel(s, r.id) == label) ++place;
            if (r.id == view->rack) break;
        }
        return " \u00b7 rack " + std::to_string(place);
    });
    // Whatever is left (no rack to tell apart): the row's place.
    separate([](std::size_t i) { return " \u00b7 " + std::to_string(i + 1); });
    return out;
}

}  // namespace

std::vector<model::Id> modRoutesOfTake(const model::Session& s, model::Id take) {
    std::vector<model::Id> out;
    if (const model::Take* t = s.takes.find(take))
        for (model::Id fx : t->chain)
            for (model::Id route : model::modulationsInto(s, fx)) out.push_back(route);
    std::sort(out.begin(), out.end());
    return out;
}

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

std::string removeRefusalWords(const model::Session& s, model::Id route, const Error& e) {
    if (e.code == "track.frozen") {
        const auto frozen = model::frozenOwner(s, route);
        return (frozen ? model::trackLabel(s, *frozen) : std::string("Its track")) +
               " is frozen: unfreeze it to change its modulation";
    }
    if (e.code == "mr.unknown" && e.path == "/args/modulation")
        return "That modulation is already gone";
    return "The modulation could not be removed";
}

std::vector<ui::MenuItem> modRemoveItems(AppActions& actions, const model::Session& s,
                                         const rack::Catalog* catalog,
                                         const model::EnvTarget& target) {
    std::vector<ui::MenuItem> items;
    AppActions* a = &actions;
    const auto routes = views::routesOn(s, target);
    const auto labels = menuLabels(s, catalog, routes);
    for (std::size_t i = 0; i < routes.size(); ++i) {
        ui::MenuItem m;
        m.label = "Remove modulation: " + labels[i];
        // A route in a frozen track's print stays with it (the engine
        // refuses it with track.frozen); a fader's route acts after the print.
        m.enabled = !model::frozenOwner(s, routes[i]);
        m.run = [a, route = routes[i]] {
            json::Value args = json::Value::object();
            args["modulation"] = route.str();
            const auto r = a->dispatcher().execute({"modulation.remove", std::move(args), kOrigin});
            if (!r && a->onError) a->onError(removeRefusalWords(a->app().head(), route, r.error()));
        };
        items.push_back(std::move(m));
    }
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
