// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes (ADR-0096, session-model "Modulation"): modulation.add,
// .set, .remove and .list. A route takes a rack's Mod Out input to a
// parameter outside the rack: a CLAP parameter flagged modulatable, of an FX
// in a track's chain or input chain; a track's volume, pan or width; or the
// level of an audio send between strips or a rack send (M2).
#include "command_helpers.h"
#include "rack_runtime.h"
#include "supple/model/freeze.h"

namespace supple::engine {

namespace {

struct SourceArg {
    std::string module, input;
};

struct TargetArg {
    std::optional<std::string> fx, track, route;
    std::string param;
};

struct ModAddArgs {
    SourceArg source;
    TargetArg target;
    double depth = 0;
    std::optional<bool> enabled, lane;
};

struct ModSetArgs {
    std::string modulation;
    std::optional<SourceArg> source;
    std::optional<TargetArg> target;
    std::optional<double> depth;
    std::optional<bool> enabled, lane;
};

struct ModRef {
    std::string modulation;
};

struct ModAdded {
    std::string modulation;
};

struct ModListArgs {
    std::optional<std::string> track, fx, rack;
};

struct ModEntry {
    std::string modulation;
    SourceArg source;
    TargetArg target;
    double depth = 0;
    bool enabled = true, lane = true;
    std::string range;                 // bipolar (-1..1) or unipolar (0..1)
    std::optional<std::string> inert;  // why the plan cannot apply it: target, latency
};

struct ModList {
    std::vector<ModEntry> modulations;
};

schema::Schema<SourceArg> sourceSchema() {
    return schema::object<SourceArg>("modulation_source")
        .field("module", &SourceArg::module, idArg("mo", "Mod Out module"))
        .field("input", &SourceArg::input,
               schema::string()
                   .pattern("^in_([1-9]|1[0-6])$", "a Mod Out input")
                   .doc("One of the Mod Out's inputs, in_1 .. in_16."))
        .build();
}

schema::Schema<TargetArg> targetSchema() {
    return schema::object<TargetArg>("modulation_target")
        .field("fx", &TargetArg::fx, idArg("fx", "Plugin instance"))
        .field("track", &TargetArg::track,
               idArg("tr", "Track").doc("A track's volume, pan or width."))
        .field("route", &TargetArg::route, idArg("sd", "Route").doc("A send's volume."))
        .field("param", &TargetArg::param,
               schema::string().minLength(1).doc(
                   "A plug-in parameter's key, name or decimal CLAP id (fx.describe marks "
                   "the modulatable ones); volume, pan or width for a track; volume for a "
                   "route."))
        .build();
}

model::EnvTarget targetOf(const TargetArg& t) {
    const std::string& id = t.fx ? *t.fx : t.track ? *t.track : *t.route;
    return {*Id::parse(id), t.param};
}

TargetArg targetArg(const model::EnvTarget& t) {
    TargetArg out;
    out.param = t.param;
    (t.object.kind == model::Kind::Fx      ? out.fx
     : t.object.kind == model::Kind::Track ? out.track
                                           : out.route) = t.object.str();
    return out;
}

Error refused(std::string code, std::string message, std::string path, std::string hint) {
    return Error(std::move(code), std::move(message), std::move(path), std::move(hint));
}

// The parameter a route into `fx` drives, found as the plan binds it
// (bindModulation): modulatable, not read-only or a bypass, not VST3.
Result<host::ParamInfo> modulatableParam(EngineApp& app, const model::FxInstance& fx,
                                         const std::string& key) {
    auto params = app.fxParams(fx);
    if (!params) return std::move(params).error();
    auto schema = app.fxSchema(fx);
    const json::Value* described = schema && schema->isObject() ? &*schema : nullptr;
    auto target = detail::resolvePluginMod(*params, described, key, fx.plugin.format == "vst3");
    if (!target) return std::move(target).error();
    for (const host::ParamInfo& p : *params)
        if (p.id == target->param) {
            if (p.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_BYPASS))
                return Error("modulation.target", p.name + " is read-only or a bypass", "",
                             "fx.describe marks the parameters a route can take (modulatable)");
            return p;
        }
    return Error("modulation.target", "the plug-in has no parameter " + key, "",
                 "fx.describe lists its parameters");
}

// The route as it would be, checked against what the model cannot know (the
// instance's parameters) and what the user can fix with a better call.
// `self`: the route being set (skipped by the duplicate check).
Status checkRoute(EngineApp& app, const model::Session& s, const model::Modulation& m,
                  std::optional<Id> self) {
    const model::RackModule* module = s.modules.find(m.source.module);
    if (!module || module->type != model::kModOutModule)
        return refused("modulation.source", m.source.module.str() + " is not a Mod Out module",
                       "/source/module",
                       "add one with module.add {rack, type: \"mod_out\"}, or use rack.get to "
                       "find a rack's Mod Out");
    const Id target = m.target.object;
    const std::string& param = m.target.param;
    // Mixer targets (M2): a track's volume, pan or width; a send's volume.
    if (target.kind == model::Kind::Track) {
        const model::Track* track = s.tracks.find(target);
        if (!track)
            return refused("modulation.target", "no " + target.str(), "/target/track",
                           "track.list names the tracks");
        if (param != "volume" && param != "pan" && param != "width")
            return refused("modulation.target", "a track's " + param + " cannot be modulated",
                           "/target/param",
                           "modulate volume, pan or width; switches such as mute and solo "
                           "cannot be modulated");
        if (param == "width" && track->type == model::StripType::Master)
            return refused("modulation.target", "a master has no width", "/target/param",
                           "modulate the master's volume or pan");
        if (track->freeze) return model::frozenError(s, target, "/target");
    } else if (target.kind == model::Kind::Route) {
        const model::Route* route = s.routes.find(target);
        if (!route)
            return refused("modulation.target", "no " + target.str(), "/target/route",
                           "route.list names the routes");
        const bool send = !route->midi && ((route->from.track && route->to.track) ||
                                           model::rackSendHome(s, *route).has_value());
        if (!send)
            return refused("modulation.target",
                           target.str() + " is not an audio send between strips or a rack send",
                           "/target/route",
                           "modulate the level of an audio send to a buss or master");
        if (param != "volume")
            return refused("modulation.target", "a send's " + param + " cannot be modulated",
                           "/target/param", "modulate the send's volume");
        if (auto owner = model::frozenOwner(s, target))
            return model::frozenError(s, *owner, "/target");
    } else {
        const model::FxInstance* fx = s.fx.find(target);
        if (!fx)
            return refused("modulation.target", "no " + target.str(), "/target/fx",
                           "fx.list names the plug-ins on each track");
        if (!model::trackOfChainFx(s, target))
            return refused("modulation.target",
                           target.str() + " is not in a track's chain or input chain", "/target/fx",
                           "modulate an FX on a track; take FX and a rack's plug-ins come later");
        if (auto owner = model::frozenOwner(s, target))
            return model::frozenError(s, *owner, "/target");
        if (auto found = modulatableParam(app, *fx, param); !found) {
            Error e = std::move(found).error();
            e.code = "modulation.target";
            e.path = "/target/param";
            return e;
        }
    }
    // The depth is the offset at a signal of 1, in the target's unit.
    const bool fxTarget = target.kind == model::Kind::Fx;
    const double bound = fxTarget ? 1.0 : param == "volume" ? 48.0 : 200.0;
    if (!std::isfinite(m.depth) || std::abs(m.depth) > bound)
        return refused("modulation.depth",
                       fxTarget            ? "a plug-in parameter's depth is -1..1"
                       : param == "volume" ? "a volume's depth is -48..48 dB"
                                           : "a pan or width's depth is -200..200 percent",
                       "/depth",
                       fxTarget ? "give the offset at a signal of 1 as a fraction of the "
                                  "parameter's range, -1..1"
                                : "give the offset at a signal of 1 in the target's unit (dB "
                                  "for volume, percent for pan and width)");
    for (const auto& e : s.modulations)
        if (e.id != self && e.value->source == m.source && e.value->target == m.target)
            return refused("modulation.duplicate",
                           e.id.str() + " already takes " + m.source.module.str() + "." +
                               m.source.input + " to " + target.str() + " " + m.target.param,
                           "/target",
                           "change " + e.id.str() + "'s depth with modulation.set instead");
    return {};
}

// After the edit: a cycle is the model's to find; say it at the target.
Status checkCycle(EngineApp& app) {
    Status ok = model::checkInvariants(app.txn().preview());
    if (ok || ok.error().code != "modulation.cycle") return ok;
    Error e = ok.error();
    e.path = "/target";
    return e;
}

// The signal range a Mod Out input carries: unipolar when every cable into
// it comes from a unipolar, gate or phase output, bipolar otherwise.
std::string rangeOf(EngineApp& app, const model::Session& s, const model::ModSource& source) {
    const rack::Catalog& catalog = *app.instances().racks().catalog();
    bool any = false;
    for (const auto& c : s.cables) {
        const model::Cable& cable = *c.value;
        if (cable.to.object != source.module || cable.to.key != source.input) continue;
        any = true;
        const model::RackModule* from = s.modules.find(cable.from.object);
        const auto* entry = from ? catalog.find(from->type) : nullptr;
        if (!entry) return "bipolar";
        bool unipolar = false;
        for (const auto& port : entry->type->ports())
            if (port.output && port.key == cable.from.key)
                unipolar = port.signal == rack::sd::PortModel::Signal::Unipolar ||
                           port.signal == rack::sd::PortModel::Signal::Gate ||
                           port.signal == rack::sd::PortModel::Signal::Phase;
        if (!unipolar) return "bipolar";
    }
    return any ? "unipolar" : "bipolar";
}

}  // namespace

void registerModulationCommands(cmd::Registry& registry) {
    [&] {
        Command<ModAddArgs, ModAdded> c;
        c.name = "modulation.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Route a rack's Mod Out input to a plug-in parameter or a mixer level.";
        c.description =
            "depth is the offset at a signal of 1: for a plug-in parameter a fraction of its "
            "CLAP range (-1..1), and the parameter must be marked modulatable in fx.describe; "
            "for a track's or send's volume in dB (-48..48); for a track's pan or width in "
            "percent (-200..200). The offset adds to the value automation or the fader gives, "
            "and the sum stays in the target's range.";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModAddArgs>("modulation_add_args")
                     .field("source", &ModAddArgs::source, sourceSchema())
                     .field("target", &ModAddArgs::target, targetSchema())
                     .field("depth", &ModAddArgs::depth, schema::number())
                     .field("enabled", &ModAddArgs::enabled, schema::boolean())
                     .field("lane", &ModAddArgs::lane,
                            schema::boolean().doc("Show the target's lane (default true)."))
                     .build();
        c.result = schema::object<ModAdded>("modulation_added")
                       .field("modulation", &ModAdded::modulation, schema::string())
                       .build();
        c.examples = {{"Add", j(R"({"source": {"module": "mo_2", "input": "in_1"},
                             "target": {"fx": "fx_1", "param": "gain"}, "depth": 0.25})"),
                       j(R"({"modulation": "mr_1"})"), "song_with_mod_out"}};
        c.run = [](const ModAddArgs& a, Context& ctx) -> Result<ModAdded> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            const int targets =
                int(bool(a.target.fx)) + int(bool(a.target.track)) + int(bool(a.target.route));
            if (targets != 1)
                return refused("modulation.target", "a target names one fx, track or route",
                               "/target",
                               "give {fx, param}, e.g. {\"fx\": \"fx_1\", "
                               "\"param\": \"cutoff\"}");
            if (s.modulations.size() >= model::kMaxModulations)
                return refused("modulation.limit",
                               "a project holds at most " + std::to_string(model::kMaxModulations) +
                                   " modulation routes",
                               "", "remove routes you no longer need with modulation.remove");
            model::Modulation m;
            m.source = {*Id::parse(a.source.module), a.source.input};
            m.target = targetOf(a.target);
            m.depth = a.depth;
            m.enabled = a.enabled.value_or(true);
            m.lane = a.lane.value_or(true);
            SUPPLE_TRY(checkRoute(app, s, m, std::nullopt));
            const Id id = app.txn().create(std::move(m));
            SUPPLE_TRY(checkCycle(app));
            return ModAdded{id.str()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ModSetArgs, Empty> c;
        c.name = "modulation.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Change a modulation route's source, target, depth, enabled or lane.";
        c.description =
            "Omitted fields keep their values. A depth drag sends Origin.gesture, so the drag "
            "is one undo step.";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModSetArgs>("modulation_set_args")
                     .field("modulation", &ModSetArgs::modulation, idArg("mr", "Modulation route"))
                     .field("source", &ModSetArgs::source, sourceSchema())
                     .field("target", &ModSetArgs::target, targetSchema())
                     .field("depth", &ModSetArgs::depth, schema::number())
                     .field("enabled", &ModSetArgs::enabled, schema::boolean())
                     .field("lane", &ModSetArgs::lane, schema::boolean())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Deepen", j(R"({"modulation": "mr_1", "depth": 0.5})"), j("{}"),
                       "song_with_modulation"}};
        c.run = [](const ModSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Modulation>(s, a.modulation, "/modulation");
            if (!id) return std::move(id).error();
            model::Modulation m = *s.modulations.find(*id);
            if (a.source) m.source = {*Id::parse(a.source->module), a.source->input};
            if (a.target) {
                const int targets = int(bool(a.target->fx)) + int(bool(a.target->track)) +
                                    int(bool(a.target->route));
                if (targets != 1)
                    return refused("modulation.target", "a target names one fx, track or route",
                                   "/target", "give {fx, param}");
                m.target = targetOf(*a.target);
            }
            if (a.depth) m.depth = *a.depth;
            if (a.enabled) m.enabled = *a.enabled;
            if (a.lane) m.lane = *a.lane;
            if (a.source || a.target || a.depth) SUPPLE_TRY(checkRoute(app, s, m, *id));
            app.txn().put<model::Modulation>(*id, std::move(m));
            SUPPLE_TRY(checkCycle(app));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ModRef, Empty> c;
        c.name = "modulation.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove a modulation route.";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModRef>("modulation_ref")
                     .field("modulation", &ModRef::modulation, idArg("mr", "Modulation route"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"modulation": "mr_1"})"), j("{}"), "song_with_modulation"}};
        c.run = [](const ModRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            auto id = existing<model::Modulation>(app.view(), a.modulation, "/modulation");
            if (!id) return std::move(id).error();
            app.txn().remove<model::Modulation>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ModListArgs, ModList> c;
        c.name = "modulation.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List modulation routes, by target track, FX or source rack.";
        c.description =
            "Each route with its source signal's range (bipolar -1..1, or unipolar 0..1 when "
            "every cable into the Mod Out input is unipolar, a gate or a phase) and, when the "
            "plan cannot apply it, inert: target (the parameter cannot take it, or the "
            "target's track is frozen and its level is outside the print) or latency (its "
            "plug-in changed latency while modulated; editing a route into it turns them back "
            "on).";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModListArgs>("modulation_list_args")
                     .field("track", &ModListArgs::track, idArg("tr", "Target track"))
                     .field("fx", &ModListArgs::fx, idArg("fx", "Target plug-in instance"))
                     .field("rack", &ModListArgs::rack, idArg("rk", "Source rack"))
                     .build();
        auto entry = schema::object<ModEntry>("modulation_entry")
                         .field("modulation", &ModEntry::modulation, schema::string())
                         .field("source", &ModEntry::source, sourceSchema())
                         .field("target", &ModEntry::target, targetSchema())
                         .field("depth", &ModEntry::depth, schema::number())
                         .field("enabled", &ModEntry::enabled, schema::boolean())
                         .field("lane", &ModEntry::lane, schema::boolean())
                         .field("range", &ModEntry::range, schema::string())
                         .field("inert", &ModEntry::inert, schema::string())
                         .build();
        c.result = schema::object<ModList>("modulation_list")
                       .field("modulations", &ModList::modulations, schema::arrayOf(entry))
                       .build();
        c.examples = {{"List", j("{}"), j(R"({"modulations": [{"modulation": "mr_1",
                   "source": {"module": "mo_2", "input": "in_1"},
                   "target": {"fx": "fx_1", "param": "gain"}, "depth": 0.25,
                   "enabled": true, "lane": true, "range": "bipolar"}]})"),
                       "song_with_modulation"}};
        c.run = [](const ModListArgs& a, Context& ctx) -> Result<ModList> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            std::optional<Id> track, fx, rack;
            if (a.track) {
                auto id = existing<model::Track>(s, *a.track, "/track");
                if (!id) return std::move(id).error();
                track = *id;
            }
            if (a.fx) {
                auto id = existing<model::FxInstance>(s, *a.fx, "/fx");
                if (!id) return std::move(id).error();
                fx = *id;
            }
            if (a.rack) {
                auto id = existing<model::Rack>(s, *a.rack, "/rack");
                if (!id) return std::move(id).error();
                rack = *id;
            }
            // Each target once: a plug-in without a live instance is asked
            // through a scratch one. A frozen target plays its print, which
            // holds the route.
            std::map<std::pair<Id, std::string>, bool> checked;
            const auto playable = [&](const model::EnvTarget& target) {
                const auto key = std::make_pair(target.object, target.param);
                if (const auto it = checked.find(key); it != checked.end()) return it->second;
                const model::FxInstance* f = s.fx.find(target.object);
                const bool ok = f && (model::frozenOwner(s, target.object) ||
                                      modulatableParam(app, *f, target.param));
                checked.emplace(key, ok);
                return ok;
            };
            ModList out;
            for (const auto& e : s.modulations) {
                const model::Modulation& m = *e.value;
                const model::RackModule* module = s.modules.find(m.source.module);
                if (track && model::modulationTargetNode(s, m.target) != track) continue;
                if (fx && m.target.object != *fx) continue;
                if (rack && (!module || module->rack != *rack)) continue;
                ModEntry row;
                row.modulation = e.id.str();
                row.source = {m.source.module.str(), m.source.input};
                row.target = targetArg(m.target);
                row.depth = m.depth;
                row.enabled = m.enabled;
                row.lane = m.lane;
                row.range = rangeOf(app, s, m.source);
                if (m.target.object.kind != model::Kind::Fx) {
                    const auto node = model::modulationTargetNode(s, m.target);
                    const model::Track* t = node ? s.tracks.find(*node) : nullptr;
                    if (t && t->freeze) row.inert = "target";  // ADR-0096 "Freeze"
                } else if (app.instances().modulationDisabled(m.target.object)) {
                    row.inert = "latency";
                } else if (!playable(m.target)) {
                    row.inert = "target";
                }
                out.modulations.push_back(std::move(row));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
