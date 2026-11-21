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

// Runs ui.modulation.source for `route`; its error goes to actions.onError.
void showModSource(AppActions& actions, model::Id route);

// A control's parameter menu rows (none without a route on `target`):
// "Show modulation source" for one route; for several, one row each,
// "Show modulation source: LFO (Modulators)".
std::vector<ui::MenuItem> modSourceItems(AppActions& actions, const model::Session& s,
                                         const rack::Catalog* catalog,
                                         const model::EnvTarget& target);

// One route on `target`: its source at once; several: a menu of them at
// `anchor` (window coordinates); none: nothing.
void openModSource(ui::Root& root, AppActions& actions, const model::Session& s,
                   const rack::Catalog* catalog, const model::EnvTarget& target,
                   const ui::Rect& anchor);

}  // namespace supple::gui
