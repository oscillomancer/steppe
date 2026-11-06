// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A plugin module's plug-in parameters as cables reach them (rack.md §6, §8):
// what the rack window lists when a cable is dropped on a plugin module, and
// the names its parameter cables show. A cable names a parameter by its
// semantic key, its CLAP id or its name; the window makes cables by the
// semantic key when the plug-in has one, else by the id.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "supple/engine/app.h"

namespace supple::gui::racks {

struct PluginParam {
    std::string key;    // what a new cable names: the semantic key, else the CLAP id
    std::string name;   // as the plug-in shows it
    std::string group;  // a semantic parameter's group ("" for none)
    std::uint32_t id = 0;
    bool modulatable = false;  // takes CLAP modulation (never for a VST3 plug-in)
};

struct PluginParams {
    std::vector<PluginParam> params;  // the plug-in's visible parameters, in its order
    // Why nothing can be modulated: a VST3 plug-in, none modulatable, or the
    // plug-in could not be asked. "" when some can.
    std::string note;
    // The parameter a cable's key names: a semantic key, an id, or a name
    // (case-insensitive), as the engine resolves it. Null: none.
    const PluginParam* find(const std::string& key) const;
    // A cable end's name for people: the parameter's name, else the key.
    std::string nameOf(const std::string& key) const;
};

// Asks the plug-in (a scratch instance, as fx.describe does).
PluginParams pluginParams(engine::EngineApp& app, const model::FxInstance& fx);

}  // namespace supple::gui::racks
