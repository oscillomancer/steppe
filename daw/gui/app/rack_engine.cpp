// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_engine.h"

#include <algorithm>
#include <cmath>

// TODO(rack): the engine's catalog and pool are internal; this reads them
// until supple/engine exposes them (asked of the engine's owner).
#include "rack_runtime.h"

namespace supple::gui::racks {

std::shared_ptr<const rack::Catalog> catalog(engine::EngineApp& app) {
    return app.instances().racks().catalog();
}

void setCatalog(engine::EngineApp& app, std::shared_ptr<const rack::Catalog> catalog) {
    app.instances().racks().setCatalog(std::move(catalog));
}

rack::Instance* instance(engine::EngineApp& app, model::Id module) {
    return app.instances().racks().find(module);
}

namespace {

sd::PortModel port(const char* key, const char* name, sd::PortModel::Signal signal, bool output,
                   const char* help) {
    sd::PortModel p;
    p.key = p.member = key;
    p.name = name;
    p.signal = signal;
    p.output = output;
    p.description = help;
    return p;
}

std::shared_ptr<const rack::ModuleType> makePlugin(bool instrument) {
    using S = sd::PortModel::Signal;
    sd::ModuleInfoModel info;
    info.id = std::string(model::kPluginModule);
    info.name = "Plugin";
    info.vendor = "Supple Audio";
    info.description = "A CLAP or VST3 plug-in in the patch; a click opens its editor.";
    info.tags = {"io", "plugin"};
    std::vector<sd::PortModel> ports = {
        port("in_l", "In L", S::Audio, false, "The plug-in's left input."),
        port("in_r", "In R", S::Audio, false, "Its right input."),
    };
    if (instrument) {
        ports.push_back(port("pitch", "Pitch", S::Pitch, false, "Notes' keys, 1 per octave."));
        ports.push_back(port("gate", "Gate", S::Gate, false, "A note sounds while high."));
        ports.push_back(port("velocity", "Velocity", S::Unipolar, false, "Its velocity."));
    }
    ports.push_back(port("out_l", "Out L", S::Audio, true, "The plug-in's left output."));
    ports.push_back(port("out_r", "Out R", S::Audio, true, "Its right output."));
    return rack::ModuleType::host(std::move(info), {}, std::move(ports));
}

}  // namespace

std::shared_ptr<const rack::ModuleType> pluginType(bool instrument) {
    static const auto effect = makePlugin(false), synth = makePlugin(true);
    return instrument ? synth : effect;
}

double paramValue(const sd::ParamModel& param, const json::Value* value) {
    return engine::detail::moduleParamValue(param, value);
}

json::Value paramJson(const sd::ParamModel& param, double physical) {
    return engine::detail::moduleParamJson(param, physical);
}

}  // namespace supple::gui::racks
