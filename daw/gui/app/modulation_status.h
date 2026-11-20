// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Inert modulation routes in the GUI (ADR-0096; gui.md §4.5): a route the
// plan cannot apply, as modulation.list reports it (`inert`: "source" when
// nothing is cabled into its Mod Out input, "envelope" when the envelope that
// carries it is off or gone, "target", "latency"), draws idle on the rings,
// bars and lanes, and the parameter's menu names the reason in a greyed row.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/model/session.h"
#include "supple/ui/popup.h"

namespace supple::gui {

// Every inert route with its reason, from modulation.list.
std::map<model::Id, std::string> inertRoutes(cmd::Dispatcher& dispatcher);

// The words for a reason, after "Modulation idle: ".
std::string inertWords(const std::string& reason);

// A greyed menu row naming why `target`'s routes are idle ("Modulation
// idle: nothing is cabled into its source"), or none when one plays.
std::optional<ui::MenuItem> inertItem(cmd::Dispatcher& dispatcher, const model::Session& s,
                                      const model::EnvTarget& target);

// The engine's live offsets as the controls draw them: an inert route's
// left out, so it draws idle as while stopped. modulation.list runs only
// while something plays, and again when the head's version, the project or
// the routes the engine reports change.
class LiveModulation {
  public:
    std::map<model::Id, double> read(cmd::Dispatcher& dispatcher, const engine::EngineApp& app);

  private:
    std::uint64_t version_ = ~std::uint64_t{0}, generation_ = ~std::uint64_t{0};
    std::vector<model::Id> reported_;
    std::map<model::Id, std::string> inert_;
};

}  // namespace supple::gui
