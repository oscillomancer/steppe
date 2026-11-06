// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Automation lanes for plugin parameters (P5.14): a parameter's range, curve
// and units come from the effect's description (fx.describe). Cached per
// effect and parameter; a parameter the plugin does not describe is
// remembered as having no lane. A rack module's knobs (ADR-0062) take theirs
// from the module type.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/views/envelope_lane.h"

namespace supple::gui {

class PluginScales {
  public:
    explicit PluginScales(cmd::Dispatcher& dispatcher, engine::EngineApp* app = nullptr)
        : dispatcher_(dispatcher), app_(app) {}
    std::optional<views::EnvScale> operator()(const model::Envelope& env);

  private:
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp* app_;
    std::map<std::pair<model::Id, std::string>, std::shared_ptr<const params::DescribedParam>>
        cache_;
    // A module knob's, with the catalog generation it was made from.
    std::map<std::pair<model::Id, std::string>,
             std::pair<std::uint64_t, std::shared_ptr<const params::DescribedParam>>>
        moduleCache_;
};

}  // namespace supple::gui
