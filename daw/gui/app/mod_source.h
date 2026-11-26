// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A modulation route's source, as the GUI names and shows it (ADR-0096;
// gui.md §2): the module cabled into the route's Mod Out input. A lane's
// "mod", a line of the track inspector and a control's parameter menu lead
// there; each runs the app action ui.modulation.source {modulation}, which
// opens the rack window on it (RackWindow::showModulationSource).
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "supple/model/session.h"
#include "supple/ui/geometry.h"
#include "supple/ui/popup.h"

namespace supple::rack {
class Catalog;
}
namespace supple::ui {
class Root;
}

namespace supple::gui {

class AppActions;

// What a route's source is called: the module cabled into its Mod Out input
// (the first cable by id) as the rack window titles it, else "Mod N" (the
// input's number), as an unpatched input is named.
std::string modSourceName(const model::Session& s, const rack::Catalog* catalog, model::Id route);
// With its rack: "LFO (Modulators)".
std::string modRouteLabel(const model::Session& s, const rack::Catalog* catalog, model::Id route);

// Where the source is: the rack of the route's Mod Out, and the modules to
// pick there (the Mod Out and every module cabled into the route's input).
struct ModSourceView {
    model::Id rack;
    std::set<model::Id> modules;
};
std::optional<ModSourceView> modSourceView(const model::Session& s, model::Id route);

// The routes that move something of `track`: its own controls, its effects'
// parameters (chain and input chain) and its sends', in id order.
std::vector<model::Id> modRoutesOfTrack(const model::Session& s, model::Id track);

// The routes into the FX of `take`'s chain (ADR-0096 M5), in id order.
std::vector<model::Id> modRoutesOfTake(const model::Session& s, model::Id take);

// Runs ui.modulation.source for `route`; its error goes to actions.onError.
void showModSource(AppActions& actions, model::Id route);

// A control's parameter menu rows (none without a route on `target`):
// "Show modulation source" for one route; for several, one row each,
// "Show modulation source: LFO (Modulators)". In every menu of routes no two
// rows read the same: the Mod Out input tells two same-named sources apart
// ("LFO (Modulators) · Mod 2"), then the rack's place among same-named racks.
std::vector<ui::MenuItem> modSourceItems(AppActions& actions, const model::Session& s,
                                         const rack::Catalog* catalog,
                                         const model::EnvTarget& target);

// "Remove modulation: LFO (Modulators)": one row per route on `target`, by
// source, each one modulation.remove (one undo step), greyed for a route in a
// frozen track's print; its refusal goes to actions.onError in
// removeRefusalWords. None without a route.
std::vector<ui::MenuItem> modRemoveItems(AppActions& actions, const model::Session& s,
                                         const rack::Catalog* catalog,
                                         const model::EnvTarget& target);
// A refused remove of `route` in the GUI's own words, by code and path,
// with no ids: track.frozen names the track; mr.unknown at /args/modulation (it
// went another way while the menu was open) says it is already gone.
std::string removeRefusalWords(const model::Session& s, model::Id route, const Error& e);
// What a parameter menu lists for `target`'s routes, in every menu alike:
// the source rows, then the remove rows.
std::vector<ui::MenuItem> modRouteItems(AppActions& actions, const model::Session& s,
                                        const rack::Catalog* catalog,
                                        const model::EnvTarget& target);

// One route on `target`: its source at once; several: a menu of them at
// `anchor` (window coordinates); none: nothing.
void openModSource(ui::Root& root, AppActions& actions, const model::Session& s,
                   const rack::Catalog* catalog, const model::EnvTarget& target,
                   const ui::Rect& anchor);

}  // namespace supple::gui
