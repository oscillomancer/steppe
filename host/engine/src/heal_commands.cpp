// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "command_validation.h"
#include "supple/model/item_edits.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {
struct Args {
    std::vector<model::Id> items;
};
}  // namespace
void registerHealCommands(cmd::Registry& registry) {
    cmd::Command<Args, json::Value> c;
    c.name = "item.heal";
    c.kind = cmd::Kind::Edit;
    c.exposure = cmd::Exposure::McpExec;
    c.summary = "Heal compatible consecutive audio pieces without rendering.";
    c.description =
        "One chain per track, complete linked/warp selection, matching stored audio/take/FX "
        "properties and continuous source maps. Accepts touching split pieces and unchanged linear "
        "Slice-at-hits overlaps up to 50 ms. Keep all left takes, outer fades and full warp maps; "
        "remove interior slice fades. Nonlinear envelope seams that cannot be recovered refuse. "
        "Stored FX states must match exactly; no live plugin queries or audio rendering. One undo "
        "step.";
    c.args = schema::object<Args>("item_heal_args")
                 .field("items", &Args::items, schema::arrayOf(model::idSchema()).minItems(2))
                 .build();
    c.result = schema::any();
    c.errors = {{"item.heal_*", "selection cannot be restored as a continuous source",
                 "select unchanged split pieces or use item.consolidate instead"},
                {"item.selection", "a linked or warp peer is unselected",
                 "include every affected peer or suspend the edit link"}};
    c.examples = {{"Heal an audio split", *json::parse(R"({"items":["it_1","it_2"]})"),
                   *json::parse(R"({"items":["it_1"]})"), "song_split_audio"}};
    c.run = [](const Args& a, cmd::Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        auto groups = model::healingGroups(app.view(), a.items);
        if (!groups) return groups.error();
        auto out = json::Value::object();
        out["items"] = json::Value::array();
        for (const auto& group : *groups) {
            for (std::size_t i = 1; i < group.items.size(); ++i) {
                const auto s = app.view();
                for (auto take : s.items.find(group.items[i])->takes)
                    SUPPLE_TRY(removeTakeObjects(app.txn(), app.view(), take));
                app.txn().remove<model::Item>(group.items[i]);
            }
            app.txn().put(group.items.front(), group.item);
            for (const auto& [id, env] : group.envelopes) app.txn().put(id, env);
            out["items"].push_back(group.items.front().str());
        }
        return out;
    };
    addChecked(registry, std::move(c));
}
}  // namespace supple::engine
