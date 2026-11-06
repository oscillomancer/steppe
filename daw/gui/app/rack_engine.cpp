// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_engine.h"

#include "supple/engine/racks.h"

namespace supple::gui::racks {

std::shared_ptr<const rack::Catalog> catalog(engine::EngineApp& app) { return app.modules(); }

void setCatalog(engine::EngineApp& app, std::shared_ptr<const rack::Catalog> catalog) {
    app.setModules(std::move(catalog));
}

rack::Instance* instance(engine::EngineApp& app, model::Id module) {
    return app.moduleInstance(module);
}

std::shared_ptr<const rack::ModuleType> pluginType() { return engine::pluginModuleType(); }

double paramValue(const sd::ParamModel& param, const json::Value* value) {
    return engine::moduleParamValue(param, value);
}

json::Value paramJson(const sd::ParamModel& param, double physical) {
    return engine::moduleParamJson(param, physical);
}

}  // namespace supple::gui::racks
