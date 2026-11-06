// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "command_validation.h"
#include "fx_presets.h"
#include "rack_clip.h"
#include "rack_runtime.h"

namespace supple::engine {
namespace {
using model::Id;
struct CopyArgs {
    std::vector<std::string> modules;
};
struct PasteArgs {
    std::string rack;
    std::optional<double> x, y;
};
json::Value j(std::string_view s) { return json::parse(s).value(); }
auto idSchema(const char* prefix) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$", "an object id");
}
template <class A>
void common(cmd::Command<A, json::Value>& c, std::string name, cmd::Kind kind,
            std::string summary) {
    c.name = std::move(name);
    c.kind = kind;
    c.summary = std::move(summary);
    c.exposure = cmd::Exposure::McpTool;
    c.tags = {"rack", "modular"};
    c.result = schema::any();
}
}  // namespace

void registerRackClipCommands(cmd::Registry& registry) {
    {
        cmd::Command<CopyArgs, json::Value> c;
        common(c, "module.copy", cmd::Kind::Action,
               "Copy modules, their current plug-in settings and internal cables.");
        c.description =
            "Replace this engine's clipboard, independent of the project and undo. "
            "Module order is retained; cables to unselected modules are omitted.";
        c.args =
            schema::object<CopyArgs>("module_copy_args")
                .field("modules", &CopyArgs::modules,
                       schema::arrayOf(idSchema("mo")).minItems(1).maxItems(model::kMaxRackModules))
                .build();
        c.errors = {{"module.unknown", "a selected module no longer exists", "use rack.get ids"},
                    {"module.copy", "a module occurs twice", "select each module once"}};
        c.examples = {{"Copy a VCA and its Out, with their cable",
                       j(R"({"modules":["mo_3","mo_1"]})"), j(R"({"modules":2,"cables":1})"),
                       "song_with_rack"}};
        c.run = [](const CopyArgs& a, cmd::Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            auto clip = std::make_shared<RackClip>();
            std::set<Id> selected;
            model::EditTxn captured(s, 0);  // temporary snapshot, never installed in the app
            std::vector<Id> effects;
            double left = model::kMaxRackCoordinate, top = model::kMaxRackCoordinate;
            for (std::size_t i = 0; i < a.modules.size(); ++i) {
                const std::string path = "/modules/" + std::to_string(i);
                auto id = Id::parse(a.modules[i]);
                const auto* m = id ? s.modules.find(*id) : nullptr;
                if (!m)
                    return Error("module.unknown", "no module " + a.modules[i], path,
                                 "use module ids from rack.get");
                if (!selected.insert(*id).second)
                    return Error("module.copy", "module selected twice", path,
                                 "select each module once");
                clip->modules.push_back({*id, *m});
                left = std::min(left, m->x);
                top = std::min(top, m->y);
                if (m->fx) {
                    auto fx = *s.fx.find(*m->fx);
                    std::shared_ptr<const model::Blob> blob;
                    SUPPLE_TRY(detail::captureLiveState(app, *m->fx, fx, blob));
                    captured.modify<model::FxInstance>(*m->fx, [&](auto& saved) { saved = fx; });
                    if (blob) captured.putBlob(*fx.state, std::move(blob));
                    effects.push_back(*m->fx);
                    selected.insert(*m->fx);
                }
            }
            auto encoded = detail::encodeEffects(app, captured.preview(), effects);
            if (!encoded) return std::move(encoded).error();
            clip->effects = std::move(*encoded);
            for (auto& m : clip->modules) {
                m.module.x -= left;
                m.module.y -= top;
            }
            for (const auto& e : s.cables)
                if (selected.contains(e.value->from.object) &&
                    selected.contains(e.value->to.object))
                    clip->cables.push_back(*e.value);
            auto result = json::Value::object();
            result["modules"] = static_cast<int>(clip->modules.size());
            result["cables"] = static_cast<int>(clip->cables.size());
            app.setRackClipboard(std::move(clip));
            return result;
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<PasteArgs, json::Value> c;
        common(c, "rack.paste", cmd::Kind::Edit,
               "Paste the module clipboard with its plug-in settings as one undo step.");
        c.description =
            "Place the clip's top-left at x/y; omitted coordinates use free space "
            "right of existing modules. No routes or automation are copied.";
        auto coordinate =
            schema::number().range(-model::kMaxRackCoordinate, model::kMaxRackCoordinate);
        c.args = schema::object<PasteArgs>("rack_paste_args")
                     .field("rack", &PasteArgs::rack, idSchema("rk"))
                     .field("x", &PasteArgs::x, coordinate)
                     .field("y", &PasteArgs::y, coordinate)
                     .build();
        c.errors = {
            {"rack.unknown", "no such destination rack", "use rack.list"},
            {"rack.clip_empty", "no modules have been copied", "call module.copy first"},
            {"rack.unknown_type", "a copied module type is unavailable", "restore the module type"},
            {"chain.plugin_missing", "a copied plug-in is unavailable",
             "install the copied plug-in"},
            {"track.unknown", "a copied Notes source was removed",
             "restore the source track or copy again"},
            {"rack.limit", "the destination would exceed its limits",
             "remove modules or paste into another rack"},
            {"rack.position", "the clip extends beyond the coordinate range",
             "choose a position closer to the origin"}};
        c.examples = {{"Paste the copied VCA and Out", j(R"({"rack":"rk_1","x":500,"y":40})"),
                       j(R"({"modules":["mo_4","mo_5"],"cables":["cb_2"]})"),
                       "song_with_rack_clip"}};
        c.run = [](const PasteArgs& a, cmd::Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            auto rack = Id::parse(a.rack);
            if (!rack || !s.racks.contains(*rack))
                return Error("rack.unknown", "no rack " + a.rack, "/rack", "use a rack.list id");
            const auto clip = app.rackClipboard();
            if (!clip || clip->modules.empty())
                return Error("rack.clip_empty", "the module clipboard is empty", "/rack",
                             "copy modules with module.copy first");
            const auto& catalog = *app.instances().racks().catalog();
            std::size_t count = 0, ins = 0, outs = 0, cables = 0;
            double right = 0;
            for (const auto& m : s.modules)
                if (m.value->rack == *rack) {
                    ++count;
                    ins += m.value->type == model::kInModule;
                    outs += m.value->type == model::kOutModule;
                    const auto* type = catalog.find(m.value->type);
                    right = std::max(right, m.value->x + (type ? type->type->layout().width : 120));
                }
            for (const auto& cb : s.cables) {
                const auto* from = s.modules.find(cb.value->from.object);
                if (from && from->rack == *rack) ++cables;
            }
            const double x = a.x.value_or(count ? right + 16 : 40), y = a.y.value_or(40);
            for (std::size_t i = 0; i < clip->modules.size(); ++i) {
                const auto& m = clip->modules[i].module;
                const std::string path = "/modules/" + std::to_string(i);
                if (!catalog.find(m.type))
                    return Error("rack.unknown_type", "no module type " + m.type, path + "/type",
                                 "restore this type or copy available modules again");
                if (m.source && !s.tracks.contains(*m.source))
                    return Error("track.unknown", "no source track " + m.source->str(),
                                 path + "/source", "restore the Notes source track or copy again");
                if (std::abs(x + m.x) > model::kMaxRackCoordinate ||
                    std::abs(y + m.y) > model::kMaxRackCoordinate)
                    return Error("rack.position", "the pasted clip exceeds the coordinate range",
                                 "/x", "choose x/y closer to the origin");
                ins += m.type == model::kInModule;
                outs += m.type == model::kOutModule;
            }
            if (count + clip->modules.size() > model::kMaxRackModules ||
                cables + clip->cables.size() > model::kMaxRackCables || ins > model::kMaxRackIns ||
                outs > model::kMaxRackOuts)
                return Error("rack.limit", "the pasted clip exceeds the rack's limits", "/rack",
                             "remove modules or paste into another rack");
            // Validate every plug-in, patch and opaque state before creating anything.
            auto effects = detail::prepareEffects(app, clip->effects, "/modules/plugins");
            if (!effects) return std::move(effects).error();
            std::map<Id, Id> ids;
            auto result = json::Value::object();
            result["modules"] = json::Value::array();
            result["cables"] = json::Value::array();
            std::size_t fx = 0;
            auto& t = app.txn();
            for (const auto& original : clip->modules) {
                auto m = original.module;
                m.rack = *rack;
                m.x += x;
                m.y += y;
                if (m.fx) {
                    const Id made = detail::installEffect(app, (*effects)[fx++]);
                    ids[*m.fx] = made;
                    m.fx = made;
                }
                const Id id = t.create(std::move(m));
                ids[original.original] = id;
                result["modules"].asArray().push_back(id.str());
            }
            for (auto cb : clip->cables) {
                cb.from.object = ids.at(cb.from.object);
                cb.to.object = ids.at(cb.to.object);
                result["cables"].asArray().push_back(t.create(std::move(cb)).str());
            }
            return result;
        };
        addChecked(registry, std::move(c));
    }
}
}  // namespace supple::engine
