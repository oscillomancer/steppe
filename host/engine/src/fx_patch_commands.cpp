// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// FX chain order, patches, parameter reads and meters: fx.move, fx.patch.*, fx.param.get and
// fx.meters.
#include "extra_command_helpers.h"

namespace supple::engine {

namespace {

struct FxMoveArgs {
    std::string fx;
    std::optional<std::string> track;
    std::int64_t index = 0;
};

struct FxPatchGetArgs {
    std::string fx;
    bool changedOnly = false;
};

struct FxPatchResult {
    json::Value patch;
};

struct FxParamGetArgs {
    std::string fx, param;
};

struct FxParamResult {
    json::Value value;
};

enum class MetersFrom : std::uint8_t { Live, Render };

struct FxMetersArgs {
    std::string fx;
    MetersFrom from = MetersFrom::Live;
};

struct FxMetersResult {
    std::string source;
    json::Value meters;
};

struct FxPatchSetArgs {
    std::string fx;
    std::optional<json::Value> patch;
    std::optional<std::string> name;
};

struct FxPatchListArgs {
    std::string fx;
};

struct FxPatchListResult {
    json::Value patches;
};

}  // namespace

void registerFxPatchCommands(cmd::Registry& registry) {
    [&] {
        Command<FxMoveArgs, Empty> c;
        c.name = "fx.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move a plugin within a track chain or to another track.";
        c.description =
            "Works on the track's chain, on its input chain (an input-chain effect stays on its "
            "track) and on a take's chain (take FX stay on their take: index only).";
        c.tags = {"fx"};
        c.args = schema::object<FxMoveArgs>("fx_move_args")
                     .field("fx", &FxMoveArgs::fx, idSchema("fx"))
                     .field("track", &FxMoveArgs::track, idSchema("tr"))
                     .field("index", &FxMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Move a gain first", j(R"({"fx":"fx_2","index":0})"), j("{}"), "song_with_fx"}};
        c.run = [](const FxMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            std::optional<Id> source;
            bool input = false;
            // The first chain that holds it: a track owns its FX, so a second
            // match would be a broken model, not a second place to remove from.
            for (const auto& e : s.tracks) {
                if (std::find(e.value->chain.begin(), e.value->chain.end(), *id) !=
                    e.value->chain.end()) {
                    source = e.id;
                    break;
                }
                if (std::find(e.value->inputChain.begin(), e.value->inputChain.end(), *id) !=
                    e.value->inputChain.end()) {
                    source = e.id;
                    input = true;
                    break;
                }
            }
            if (!source) {  // a take's chain: reordered in place, never moved out
                for (const auto& e : s.takes) {
                    auto chain = e.value->chain;
                    const auto at = std::find(chain.begin(), chain.end(), *id);
                    if (at == chain.end()) continue;
                    if (a.track)
                        return Error("fx.chain", "a take FX stays on its take", "/track",
                                     "give only index to reorder the take's chain");
                    chain.erase(at);
                    const std::size_t to =
                        std::min<std::size_t>(static_cast<std::size_t>(a.index), chain.size());
                    chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(to), *id);
                    app(ctx).txn().modify<model::Take>(e.id,
                                                       [&](model::Take& tk) { tk.chain = chain; });
                    return Empty{};
                }
                return Error("fx.chain", "this plugin is in no chain", "/fx",
                             "use an fx id returned by fx.add, fx.copy or fx.chain.load");
            }
            Id target = *source;
            if (a.track) {
                auto tr = require<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                target = *tr;
            }
            auto sourceChain = dangling<model::Track>(s, *source, "/fx");
            if (!sourceChain) return std::move(sourceChain).error();
            if (input) {
                if (target != *source)
                    return Error("fx.chain", "an input-chain effect stays on its track", "/track",
                                 "remove it and add it to the other track's input chain");
                std::vector<Id> chain = (*sourceChain)->inputChain;
                chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
                if (static_cast<std::size_t>(a.index) > chain.size())
                    return Error("fx.index", "index is past the end of the input chain", "/index",
                                 "use an index from 0 to " + std::to_string(chain.size()));
                chain.insert(chain.begin() + a.index, *id);
                app(ctx).txn().modify<model::Track>(
                    *source, [&](model::Track& tr) { tr.inputChain = chain; });
                return Empty{};
            }
            auto chain = (*sourceChain)->chain;
            chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
            std::vector<Id> targetChain = chain;
            if (target != *source) {
                auto destination = dangling<model::Track>(s, target, "/track");
                if (!destination) return std::move(destination).error();
                targetChain = (*destination)->chain;
            }
            if (static_cast<std::size_t>(a.index) > targetChain.size())
                return Error("fx.index", "index is past the end of the destination chain", "/index",
                             "use an index from 0 to " + std::to_string(targetChain.size()));
            targetChain.insert(targetChain.begin() + a.index, *id);
            auto& txn = app(ctx).txn();
            if (target != *source)
                txn.modify<model::Track>(*source, [&](model::Track& tr) { tr.chain = chain; });
            txn.modify<model::Track>(target, [&](model::Track& tr) { tr.chain = targetChain; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxPatchGetArgs, FxPatchResult> c;
        c.name = "fx.patch.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a semantic plugin's saved patch.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchGetArgs>("fx_patch_get_args")
                     .field("fx", &FxPatchGetArgs::fx, idSchema("fx"))
                     .field("changed_only", &FxPatchGetArgs::changedOnly, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<FxPatchResult>("fx_patch_result")
                       .field("patch", &FxPatchResult::patch, schema::any())
                       .build();
        c.examples = {
            {"Read gain", j(R"({"fx":"fx_2"})"), j(R"({"patch":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxPatchGetArgs& a, Context& ctx) -> Result<FxPatchResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const auto& fx = *s.fx.find(*id);
            auto effective = app(ctx).mergedPatch(fx, json::Value::object(), false);
            if (!effective) return std::move(effective).error();
            if (a.changedOnly) {
                model::FxInstance defaults = fx;
                defaults.patch.reset();
                auto baseline = app(ctx).mergedPatch(defaults, json::Value::object(), false);
                if (!baseline) return std::move(baseline).error();
                auto diff = json::createMergePatch(*baseline, *effective);
                if (!diff) return std::move(diff).error();
                return FxPatchResult{std::move(*diff)};
            }
            return FxPatchResult{std::move(*effective)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxPatchListArgs, FxPatchListResult> c;
        c.name = "fx.patch.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List an instance's read-only factory patches, by name.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchListArgs>("fx_patch_list_args")
                     .field("fx", &FxPatchListArgs::fx, idSchema("fx"))
                     .build();
        c.result = schema::object<FxPatchListResult>("fx_patch_list_result")
                       .field("patches", &FxPatchListResult::patches, schema::any())
                       .build();
        c.examples = {{"List factory patches", j(R"({"fx":"fx_2"})"), j(R"({"patches":[]})"),
                       "song_with_fx"}};
        c.run = [](const FxPatchListArgs& a, Context& ctx) -> Result<FxPatchListResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            auto patches = app(ctx).fxFactoryPatches(*id, *s.fx.find(*id));
            if (!patches) return std::move(patches).error();
            return FxPatchListResult{std::move(*patches)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxPatchSetArgs, FxPatchResult> c;
        c.name = "fx.patch.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace a semantic patch, or apply a factory patch by name.";
        c.description =
            "Give exactly one of patch or name. Factory names come from fx.patch.list; omitted "
            "parameters take defaults.";
        c.tags = {"fx", "patch"};
        c.args =
            schema::object<FxPatchSetArgs>("fx_patch_set_args")
                .field("fx", &FxPatchSetArgs::fx, idSchema("fx"))
                .field("patch", &FxPatchSetArgs::patch, schema::any())
                .field("name", &FxPatchSetArgs::name, schema::string().minLength(1).maxLength(64))
                .build();
        c.result = schema::object<FxPatchResult>("fx_patch_result")
                       .field("patch", &FxPatchResult::patch, schema::any())
                       .build();
        c.examples = {
            {"Replace gain",
             j(R"({"fx":"fx_2","patch":{"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}}})"),
             j(R"({"patch":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxPatchSetArgs& a, Context& ctx) -> Result<FxPatchResult> {
            if (a.patch.has_value() == a.name.has_value())
                return Error("fx.patch_args", "give exactly one of patch or name", "/patch",
                             "provide a complete patch, or a factory name from fx.patch.list");
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            json::Value document;
            if (a.patch)
                document = *a.patch;
            else {
                auto patches = app(ctx).fxFactoryPatches(*id, *s.fx.find(*id));
                if (!patches) return std::move(patches).error();
                std::string names;
                for (const auto& candidate : patches->asArray()) {
                    const auto& name = candidate.find("name")->asString();
                    if (name == *a.name) document = candidate;
                    names += (names.empty() ? "" : ", ") + name;
                }
                if (document.isNull())
                    return Error("fx.unknown_factory_patch",
                                 "no factory patch named '" + *a.name + "'", "/name",
                                 names.empty()
                                     ? "this plugin has no factory patches; provide patch instead"
                                     : "choose a name from fx.patch.list: " + names);
            }
            auto patch = app(ctx).mergedPatch(*s.fx.find(*id), document, true);
            if (!patch) {
                Error e = std::move(patch).error();
                e.path = "/patch" + e.path;
                return e;
            }
            app(ctx).txn().modify<model::FxInstance>(
                *id, [&](model::FxInstance& fx) { fx.patch = *patch; });
            return FxPatchResult{std::move(*patch)};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxParamGetArgs, FxParamResult> c;
        c.name = "fx.param.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read one saved plugin parameter by semantic key or numeric CLAP id.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxParamGetArgs>("fx_param_get_args")
                     .field("fx", &FxParamGetArgs::fx, idSchema("fx"))
                     .field("param", &FxParamGetArgs::param, schema::string().minLength(1))
                     .build();
        c.result = schema::object<FxParamResult>("fx_param_result")
                       .field("value", &FxParamResult::value, schema::any())
                       .build();
        c.examples = {{"Read gain", j(R"({"fx":"fx_2","param":"gain"})"), j(R"({"value":"<any>"})"),
                       "song_with_fx"}};
        c.run = [](const FxParamGetArgs& a, Context& ctx) -> Result<FxParamResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const auto& fx = *s.fx.find(*id);
            auto effective = app(ctx).mergedPatch(fx, json::Value::object(), false);
            if (!effective && effective.error().code != "fx.not_semantic")
                return std::move(effective).error();  // a broken patch is the cause
            const auto* params = effective ? effective->find("params") : nullptr;
            if (params && params->isObject())
                if (const auto* value = params->find(a.param)) return FxParamResult{*value};
            for (const auto& [key, value] : fx.params)
                if (std::to_string(key) == a.param) return FxParamResult{json::Value(value)};
            return Error("fx.param", "no saved parameter " + a.param, "/param",
                         "use fx.describe {full:true} for parameter names");
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<FxMetersArgs, FxMetersResult> c;
        c.name = "fx.meters";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a plugin's meters and readouts, playing now or as a render ended.";
        c.description =
            "The read-only parameters of a plugin: a semantic plugin's by patch key in physical "
            "units (an analyzer's loudness, a reverb's RT60, a gate's suggested threshold), any "
            "other plugin's by name as CLAP values. from live (the default): the playing "
            "instance's, which move while it processes (playback, monitoring); source none "
            "while no instance runs (start the engine). from render: as the last render.mix "
            "ended, which renders through instances of its own; source none until a render.mix "
            "has included this plugin.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxMetersArgs>("fx_meters_args")
                     .field("fx", &FxMetersArgs::fx, idSchema("fx"))
                     .field("from", &FxMetersArgs::from,
                            schema::enumeration<MetersFrom>(
                                {{MetersFrom::Live, "live"}, {MetersFrom::Render, "render"}})
                                .doc("live: the playing instance; render: the last render.mix"))
                     .defaultValue(MetersFrom::Live)
                     .build();
        c.result = schema::object<FxMetersResult>("fx_meters")
                       .field("source", &FxMetersResult::source,
                              schema::string().doc("live, render, or none (nothing to read yet)"))
                       .field("meters", &FxMetersResult::meters,
                              schema::any().doc("Read-only values by patch key (physical) or name"))
                       .build();
        c.examples = {{"Read meters", j(R"({"fx":"fx_2"})"),
                       j(R"({"source":"<any>","meters":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxMetersArgs& a, Context& ctx) -> Result<FxMetersResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            FxMetersResult r;
            r.source = "none";
            r.meters = json::Value::object();
            if (a.from == MetersFrom::Render) {
                if (const json::Value* rendered = app(ctx).renderMeters(*id)) {
                    r.source = "render";
                    r.meters = *rendered;
                }
                return r;
            }
            host::Instance* live = app(ctx).instanceOf(*id);
            if (!live || live->failed()) return r;
            auto readouts = instanceReadouts(*live);
            if (!readouts) return std::move(readouts).error();
            r.source = "live";
            r.meters = std::move(*readouts);
            return r;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
