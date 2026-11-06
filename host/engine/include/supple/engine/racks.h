// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modular racks for the app (ADR-0062, rack.md): what a rack window needs
// beyond EngineApp::modules() (the catalog) and EngineApp::moduleInstance()
// (a running module, for its telemetry).
#pragma once

#include <memory>
#include <string>

#include "supple/json/value.h"
#include "supple/rack/module_type.h"

namespace supple::engine {

// A module knob's value from its JSON in the session (a number, a choice's
// key, a bool; rack.md §2.2), in the physical units Instance::setParam
// takes; its default when absent or not one of its values.
double moduleParamValue(const script::detail::ParamModel& param, const json::Value* value);
// The JSON a physical value is written as (module.set takes it).
json::Value moduleParamJson(const script::detail::ParamModel& param, double physical);
// The plugin module's type (com.suppleaudio.rack.plugin): in_l, in_r, pitch,
// gate, velocity in; out_l, out_r out. Its knobs are its plug-in's.
std::shared_ptr<const rack::ModuleType> pluginModuleType();
// Every built-in module type (host and stock) as a Markdown reference: ports,
// knobs with their ranges, units and defaults, factory presets. The engine
// CLI prints it (`modules --markdown`) into skill/generated/rack-modules.md.
std::string moduleReference();

}  // namespace supple::engine
