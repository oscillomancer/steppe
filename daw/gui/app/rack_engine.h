// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// What the rack window reads of the engine (ADR-0062): the module types it
// knows, the live instance behind a module (for telemetry), and a module's
// knob values from the session, in the physical units the faces show.
#pragma once

#include <memory>

#include "supple/engine/app.h"
#include "supple/rack/instance.h"
#include "supple/rack/module_type.h"

namespace supple::gui::racks {

namespace sd = script::detail;

// The catalog racks use: the host's, stock, library and project types.
std::shared_ptr<const rack::Catalog> catalog(engine::EngineApp& app);
// Replaces it (tests: module types of their own). Null: the built-in one.
void setCatalog(engine::EngineApp& app, std::shared_ptr<const rack::Catalog> catalog);
// The running instance of a module (null: not in a running plan).
rack::Instance* instance(engine::EngineApp& app, model::Id module);
// A plugin module's description (rack.md §5): audio in and out, and for an
// instrument the note inputs. Its parameters are the plug-in's.
std::shared_ptr<const rack::ModuleType> pluginType(bool instrument);
// A knob's value from the module's params (a number, a choice's key, a
// bool), physical; the default when absent or not one of its values.
double paramValue(const sd::ParamModel& param, const json::Value* value);
// The JSON a physical value is written as (rack.md §2.2).
json::Value paramJson(const sd::ParamModel& param, double physical);

}  // namespace supple::gui::racks
