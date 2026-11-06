// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/app.h"
#include "supple/rack/instance.h"

namespace supple::engine {
void registerRackLevelCommands(cmd::Registry& registry) {
    struct Args {
        std::string rack;
    };
    cmd::Command<Args, json::Value> c;
    c.name = "rack.levels";
    c.kind = cmd::Kind::Query;
    c.exposure = cmd::Exposure::McpTool;
    c.tags = {"rack", "modular"};
    c.summary = "Read every module output's linear peak, without subscribing to telemetry.";
    c.description =
        "Returns {modules: {mo_N: {port: level}}}. Each level is the maximum "
        "absolute sample across all channels in the latest processed rack block "
        "(at most 32 frames), including unconnected outputs. Levels may exceed 1. "
        "Stopped, silent or uninstantiated modules read zero; unavailable types "
        "have no known ports. Anticipative levels can lead the heard audio.";
    c.args = schema::object<Args>("rack_levels_args")
                 .field("rack", &Args::rack, schema::string().pattern("^rk_[0-9]+$", "a rack id"))
                 .build();
    c.result = schema::any();
    c.errors = {{"rack.unknown", "no such rack", "use a rack.list id"}};
    c.examples = {
        {"Read output peaks while stopped", json::parse(R"({"rack":"rk_1"})").value(),
         json::parse(
             R"({"modules":{"mo_1":{},"mo_2":{"pitch":0.0,"gate":0.0,"velocity":0.0,"pressure":0.0,"retrigger":0.0,"modwheel":0.0,"bend":0.0},"mo_3":{"out":0.0}}})")
             .value(),
         "song_with_rack"}};
    c.run = [](const Args& a, cmd::Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        const auto s = app.view();
        const auto id = model::Id::parse(a.rack);
        if (!id || !s.racks.contains(*id))
            return Error("rack.unknown", "no rack " + a.rack, "/rack", "use a rack.list id");
        auto result = json::Value::object();
        result["modules"] = json::Value::object();
        for (const auto& m : s.modules) {
            if (m.value->rack != *id) continue;
            auto levels = json::Value::object();
            if (const auto* type = app.modules()->find(m.value->type))
                for (int output : type->type->outputs()) {
                    const auto& port = type->type->ports()[static_cast<std::size_t>(output)];
                    levels[port.key] = static_cast<double>(app.moduleOutputLevel(m.id, port.key));
                }
            result["modules"][m.id.str()] = std::move(levels);
        }
        return result;
    };
    registry.add(std::move(c));
}
}  // namespace supple::engine
