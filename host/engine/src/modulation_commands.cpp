// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modulation routes (ADR-0096, session-model "Modulation"): modulation.add,
// .set, .remove, .list and .telemetry, and modulate, the whole gesture. A route takes a rack's Mod
// Out input to a parameter outside the rack: a CLAP parameter flagged modulatable, of an FX in a
// track's chain or input chain; a track's volume, pan or width; or the level of an audio send
// between strips or a rack send (M2).
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
    // A plug-in target's canonical reference and display name
    // (session-model "Plug-in parameter references").
    std::optional<std::string> param, param_name;
};

struct ModListArgs {
    std::optional<std::string> track, fx, rack;
};

// A cable into a route's Mod Out input: what the route carries.
struct FedBy {
    std::string module, output, type;
};
struct ModEntry {
    std::string modulation;
    SourceArg source;
    TargetArg target;
    double depth = 0;
    bool enabled = true, lane = true;
    std::string range;  // bipolar (-1..1) or unipolar (0..1)
    std::vector<FedBy> fedBy;
    std::optional<std::string> inert;       // why the plan cannot apply it: source, target, latency
    std::optional<std::string> param_name;  // a plug-in target's display name
};

struct ModList {
    std::vector<ModEntry> modulations;
};

struct ModTelemetryArgs {
    std::optional<std::string> track;
};

struct ModReading {
    std::string modulation;
    std::optional<double> offset;
    std::optional<double> low, high;  // none while nothing feeds its Mod Out input
};

struct ModTelemetry {
    std::vector<ModReading> modulations;
};

// modulate's source: a kind of modulator, or a Mod Out input already patched.
struct ModulateSource {
    std::string kind;  // lfo, follower, random, macro; "" for `input`
    std::optional<SourceArg> input;
};

struct ModulateArgs {
    TargetArg target;
    ModulateSource source;
    std::optional<double> depth;
    std::optional<std::string> from, rack;
};

struct Modulated {
    std::string modulation;
    std::optional<std::string> param, param_name;  // a plug-in target's, as modulation.add
    std::optional<std::string> rack, module, mod_out, input, send, channel;
    std::optional<std::string> knob;  // a macro's: the knob to move
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
// (bindModulation): not read-only or a bypass; modulatable, or automatable
// and reached through its envelope (`envelope`, ADR-0096 step M4; a VST3's
// always is).
struct PluginParam {
    host::ParamInfo info;
    bool envelope = false;
    std::string canonical;  // what the route stores (param_ref.h)
};
std::string pluginName(EngineApp& app, const model::FxInstance& fx) {
    auto info = app.findPlugin(fx.plugin.id);
    return info && !info->name.empty() ? info->name : fx.plugin.id;
}
// `ambiguity`: Refuse for a reference the caller gives now; First for one a
// route already stores (an older file's name binds as the plan binds it).
Result<PluginParam> modulatableParam(EngineApp& app, const model::FxInstance& fx,
                                     const std::string& key,
                                     detail::Ambiguity ambiguity = detail::Ambiguity::First) {
    auto params = app.fxParams(fx);
    if (!params) return std::move(params).error();
    auto schema = app.fxSchema(fx);
    const json::Value* described = schema && schema->isObject() ? &*schema : nullptr;
    auto target = detail::resolvePluginMod(*params, described, key, fx.plugin.format == "vst3",
                                           true, ambiguity, pluginName(app, fx));
    if (!target) return std::move(target).error();
    for (const host::ParamInfo& p : *params)
        if (p.id == target->param) {
            if (p.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_BYPASS))
                return Error("modulation.target", p.name + " is read-only or a bypass", "",
                             "fx.describe marks the parameters a route can take (modulatable)");
            return PluginParam{p, target->envelope, target->canonical};
        }
    return Error("modulation.target", "the plug-in has no parameter " + key, "",
                 "fx.describe lists its parameters");
}

// Whether an active envelope on `fxId` plays parameter `clapId`: the lane
// that carries a route through the envelope (M4).
bool activeEnvelopeOn(EngineApp& app, const model::Session& s, Id fxId, const model::FxInstance& fx,
                      std::uint32_t clapId) {
    auto params = app.fxParams(fx);
    if (!params) return false;
    auto schema = app.fxSchema(fx);
    const json::Value* described = schema && schema->isObject() ? &*schema : nullptr;
    for (const auto& e : s.envelopes) {
        const model::Envelope& env = *e.value;
        if (!env.active || env.target.object != fxId) continue;
        auto t = detail::resolvePluginMod(*params, described, env.target.param,
                                          fx.plugin.format == "vst3", true);
        if (t && t->param == clapId) return true;
    }
    return false;
}

// A parameter's current value in its envelope's unit (M4, modulate's
// one-point envelope): physical for a semantic key, the plain CLAP value
// otherwise. A running instance says it (a VST3's normalized value
// converted); without one, the model: the patch's value for a semantic key,
// else the saved value, else the parameter's default.
Result<double> envelopeValueNow(EngineApp& app, Id fxId, const model::FxInstance& fx,
                                const std::string& key, const host::ParamInfo& info) {
    std::optional<double> clap;
    if (host::Instance* in = app.instances().find(fxId)) {
        auto value = in->paramValue(info.id);
        if (!value) return std::move(value).error();
        clap = *value;
        if (in->paramsNormalized()) {
            auto plain = in->plainParam(info.id, *clap);
            if (!plain) return std::move(plain).error();
            clap = *plain;
        }
    }
    std::unique_ptr<params::DescribedParam> semantic;
    if (auto schema = app.fxSchema(fx); schema && schema->isObject())
        if (const auto* ps = schema->find("params"); ps && ps->isObject())
            if (const auto* entry = ps->find(key))
                if (auto d = params::fromDescription(key, *entry); d) semantic = std::move(*d);
    if (!clap && semantic && fx.patch)
        if (const auto* ps = fx.patch->find("params"); ps && ps->isObject())
            if (const auto* v = ps->find(key); v && v->isNumber()) return v->asDouble();
    if (!clap) {
        const auto saved = fx.params.find(info.id);
        clap = saved != fx.params.end() ? saved->second : info.def;
    }
    return semantic ? params::fromClap(semantic->spec, *clap) : *clap;
}

schema::Schema<ModulateSource> modulateSourceSchema() {
    auto exported =
        json::parse(
            R"({"oneOf":[{"type":"string","enum":["lfo","follower","random","macro"]},{"type":"object","properties":{"module":{"type":"string","pattern":"^mo_[0-9]+$"},"input":{"type":"string","pattern":"^in_([1-9]|1[0-6])$"}},"required":["module","input"],"additionalProperties":false}]})")
            .value();
    return schema::Schema<ModulateSource>(
        "modulate_source",
        [](const json::Value& v, ModulateSource& out, schema::DecodeCtx& ctx,
           const std::string& path) {
            if (v.isString()) {
                const std::string& k = v.asString();
                if (k == "lfo" || k == "follower" || k == "random" || k == "macro") {
                    out.kind = k;
                    return true;
                }
            } else if (v.isObject() && v.size() == 2) {
                const json::Value* module = v.find("module");
                const json::Value* input = v.find("input");
                const auto id =
                    module && module->isString() ? Id::parse(module->asString()) : std::nullopt;
                if (id && id->kind == model::Kind::Module && input && input->isString()) {
                    const std::string& in = input->asString();
                    int k = 0;
                    if (in.size() > 3 && in.size() <= 5 && in.rfind("in_", 0) == 0 &&
                        in[3] != '0' && std::all_of(in.begin() + 3, in.end(), [](char c) {
                            return c >= '0' && c <= '9';
                        }))
                        k = std::stoi(in.substr(3));
                    if (k >= 1 && k <= 16) {
                        out.input = SourceArg{module->asString(), in};
                        return true;
                    }
                }
            }
            ctx.error("schema.type", "expected lfo, follower, random, macro or {module, input}",
                      path,
                      "name a kind of modulator, or a Mod Out input: {\"module\": \"mo_N\", "
                      "\"input\": \"in_1\"}");
            return false;
        },
        [](const ModulateSource& s) {
            if (!s.input) return json::Value(s.kind);
            json::Value o = json::Value::object();
            o["module"] = json::Value(s.input->module);
            o["input"] = json::Value(s.input->input);
            return o;
        },
        exported);
}

// The track a target lives on: a rack send's is its rack's home channel.
std::optional<Id> homeTrackOf(const model::Session& s, const model::EnvTarget& target) {
    auto node = model::modulationTargetNode(s, target);
    if (node && node->kind == model::Kind::Rack)
        if (const model::Rack* r = s.racks.find(*node)) return r->track;
    return node;
}

// Whether `to` is reachable from `from` through the signal graph and the
// modulation routes (each an edge from its rack to its target's node).
bool reaches(const model::Session& s, Id from, Id to) {
    std::map<Id, std::vector<Id>> edges = signalEdges(s);
    for (const auto& e : s.modulations) {
        const model::RackModule* m = s.modules.find(e.value->source.module);
        const auto node = model::modulationTargetNode(s, e.value->target);
        if (m && node) edges[m->rack].push_back(*node);
    }
    std::set<Id> seen;
    std::function<bool(Id)> walk = [&](Id at) {
        if (at == to) return true;
        if (!seen.insert(at).second) return false;
        for (Id next : edges[at])
            if (walk(next)) return true;
        return false;
    };
    return walk(from);
}

// The route as it would be, checked against what the model cannot know (the
// instance's parameters) and what the user can fix with a better call.
// `self`: the route being set (skipped by the duplicate check).
// A plug-in target's reference is resolved and `m` gets its canonical
// spelling; `paramName` receives the display name.
Status checkRoute(EngineApp& app, const model::Session& s, model::Modulation& m,
                  std::optional<Id> self, detail::Ambiguity ambiguity,
                  std::string* paramName = nullptr) {
    const model::RackModule* module = s.modules.find(m.source.module);
    if (!module || module->type != model::kModOutModule)
        return refused("modulation.source", m.source.module.str() + " is not a Mod Out module",
                       "/source/module",
                       "add one with module.add {rack, type: \"mod_out\"}, or use rack.get to "
                       "find a rack's Mod Out");
    const Id target = m.target.object;
    const std::string param = m.target.param;
    std::optional<std::uint32_t> paramId;  // a plug-in target's, for the duplicate check
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
        auto found = modulatableParam(app, *fx, param, ambiguity);
        if (!found) {
            Error e = std::move(found).error();
            e.code = "modulation.target";
            e.path = "/target/param";
            return e;
        }
        m.target.param = found->canonical;
        paramId = found->info.id;
        if (paramName) *paramName = found->info.name;
        if (found->envelope && !activeEnvelopeOn(app, s, target, *fx, found->info.id))
            return refused("modulation.envelope",
                           found->info.name +
                               " is not modulatable: a route reaches it through an active "
                               "envelope, and it has none",
                           "/target/param",
                           "add one with env.add {fx: \"" + target.str() + "\", param: \"" + param +
                               "\", points: [{bar: 1, value: ...}]}, or use modulate, "
                               "which adds it");
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
    // One parameter whatever the spellings (review round 72 D1): an older
    // route's stored name resolves to the same parameter.
    const auto sameTarget = [&](const model::EnvTarget& other) {
        if (other == m.target) return true;
        if (!paramId || other.object != m.target.object) return false;
        const model::FxInstance* fx = s.fx.find(other.object);
        if (!fx) return false;
        auto found = modulatableParam(app, *fx, other.param);
        return found && found->info.id == *paramId;
    };
    for (const auto& e : s.modulations)
        if (e.id != self && e.value->source == m.source && sameTarget(e.value->target))
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
// The cables into a route's Mod Out input, with each source module's type.
std::vector<FedBy> fedBy(const model::Session& s, const model::ModSource& source) {
    std::vector<FedBy> out;
    for (const auto& c : s.cables) {
        const model::Cable& cable = *c.value;
        if (cable.to.object != source.module || cable.to.key != source.input) continue;
        const model::RackModule* from = s.modules.find(cable.from.object);
        out.push_back({cable.from.object.str(), cable.from.key, from ? from->type : ""});
    }
    return out;
}

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
        c.summary = "Route a rack's Mod Out to a plug-in parameter or mixer level.";
        c.description =
            "depth is the offset at a signal of 1: for a plug-in parameter a fraction of its "
            "CLAP range (-1..1). A parameter fx.describe marks modulatable takes the offset "
            "directly; one marked modulatable_by_envelope (automatable, continuous, not "
            "modulatable, or any VST3's) takes it through its active envelope, which must exist "
            "(modulation.envelope; modulate adds one), and for a VST3 depth is a fraction of "
            "its normalized range; "
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
                            schema::boolean().doc("Show the target's automation lane in the "
                                                  "timeline while the route exists (default "
                                                  "true). It never changes the sound."))
                     .build();
        c.result = schema::object<ModAdded>("modulation_added")
                       .field("modulation", &ModAdded::modulation, schema::string())
                       .field("param", &ModAdded::param,
                              schema::string().doc("A plug-in target's canonical reference: "
                                                   "its semantic key or decimal id."))
                       .field("param_name", &ModAdded::param_name,
                              schema::string().doc("A plug-in target's display name."))
                       .build();
        c.examples = {{"Add", j(R"({"source": {"module": "mo_2", "input": "in_1"},
                             "target": {"fx": "fx_1", "param": "gain"}, "depth": 0.25})"),
                       j(R"({"modulation": "mr_1", "param": "gain", "param_name": "<any>"})"),
                       "song_with_mod_out"}};
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
            std::string name;
            SUPPLE_TRY(checkRoute(app, s, m, std::nullopt, detail::Ambiguity::Refuse, &name));
            ModAdded out{"", std::nullopt, std::nullopt};
            if (m.target.object.kind == model::Kind::Fx) {
                out.param = m.target.param;
                out.param_name = name;
            }
            const Id id = app.txn().create(std::move(m));
            SUPPLE_TRY(checkCycle(app));
            out.modulation = id.str();
            return out;
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
                     .field("lane", &ModSetArgs::lane,
                            schema::boolean().doc("Show or hide the target's automation lane "
                                                  "for this route. It never changes the sound."))
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
            if (a.source || a.target || a.depth)
                SUPPLE_TRY(
                    checkRoute(app, s, m, *id,
                               a.target ? detail::Ambiguity::Refuse : detail::Ambiguity::First));
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
        c.summary = "List modulation routes by target or source rack.";
        c.description =
            "Each route with its source signal's range (bipolar -1..1, or unipolar 0..1 when "
            "every cable into the Mod Out input is unipolar, a gate or a phase), fed_by (each "
            "cable into that input: its module, output and module type) and, when the plan "
            "cannot apply it, inert: source (nothing is cabled into the input), target (the "
            "parameter cannot take it, or the target's track is frozen and its level is "
            "outside the print), envelope (a parameter reached through its envelope, which is "
            "gone or off) or latency (its "
            "plug-in changed latency while modulated; editing a route into it turns them back "
            "on).";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModListArgs>("modulation_list_args")
                     .field("track", &ModListArgs::track, idArg("tr", "Target track"))
                     .field("fx", &ModListArgs::fx, idArg("fx", "Target plug-in instance"))
                     .field("rack", &ModListArgs::rack, idArg("rk", "Source rack"))
                     .build();
        auto entry =
            schema::object<ModEntry>("modulation_entry")
                .field("modulation", &ModEntry::modulation, schema::string())
                .field("source", &ModEntry::source, sourceSchema())
                .field("target", &ModEntry::target, targetSchema())
                .field("depth", &ModEntry::depth, schema::number())
                .field("enabled", &ModEntry::enabled, schema::boolean())
                .field("lane", &ModEntry::lane,
                       schema::boolean().doc("The route asks for its target's lane to be shown "
                                             "(a display flag; the sound is the same either way)"))
                .field("range", &ModEntry::range, schema::string())
                .field("fed_by", &ModEntry::fedBy,
                       schema::arrayOf(schema::object<FedBy>("modulation_fed_by")
                                           .field("module", &FedBy::module, schema::string())
                                           .field("output", &FedBy::output, schema::string())
                                           .field("type", &FedBy::type, schema::string())
                                           .build()))
                .field("inert", &ModEntry::inert, schema::string())
                .field("param_name", &ModEntry::param_name,
                       schema::string().doc("A plug-in target's display name."))
                .build();
        c.result = schema::object<ModList>("modulation_list")
                       .field("modulations", &ModList::modulations, schema::arrayOf(entry))
                       .build();
        c.examples = {{"List", j("{}"), j(R"({"modulations": [{"modulation": "mr_1",
                   "source": {"module": "mo_2", "input": "in_1"},
                   "target": {"fx": "fx_1", "param": "gain"}, "depth": 0.25,
                   "enabled": true, "lane": true, "range": "bipolar", "fed_by": [],
                   "inert": "source", "param_name": "<any>"}]})"),
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
            // Why a plug-in target cannot take the route now: "" (it can),
            // "target", or "envelope" (M4: reached through an envelope it
            // lacks).
            std::map<std::pair<Id, std::string>, std::string> checked;
            const auto unplayable = [&](const model::EnvTarget& target) -> std::string {
                const auto key = std::make_pair(target.object, target.param);
                if (const auto it = checked.find(key); it != checked.end()) return it->second;
                const model::FxInstance* f = s.fx.find(target.object);
                std::string why;
                if (!f) {
                    why = "target";
                } else if (!model::frozenOwner(s, target.object)) {
                    auto found = modulatableParam(app, *f, target.param);
                    if (!found)
                        why = "target";
                    else if (found->envelope &&
                             !activeEnvelopeOn(app, s, target.object, *f, found->info.id))
                        why = "envelope";
                }
                checked.emplace(key, why);
                return why;
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
                row.fedBy = fedBy(s, m.source);
                if (m.target.object.kind != model::Kind::Fx) {
                    const auto node = model::modulationTargetNode(s, m.target);
                    const model::Track* t = node ? s.tracks.find(*node) : nullptr;
                    if (t && t->freeze) row.inert = "target";  // ADR-0096 "Freeze"
                } else {
                    if (const model::FxInstance* f = s.fx.find(m.target.object))
                        if (auto found = modulatableParam(app, *f, m.target.param))
                            row.param_name = found->info.name;
                    if (app.instances().modulationDisabled(m.target.object))
                        row.inert = "latency";
                    else if (const std::string why = unplayable(m.target); !why.empty())
                        row.inert = why;
                }
                // Nothing cabled into its Mod Out input: it carries nothing.
                if (!row.inert && row.fedBy.empty()) row.inert = "source";
                out.modulations.push_back(std::move(row));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ModulateArgs, Modulated> c;
        c.name = "modulate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;  // the one-step gesture is a core tool
        c.summary = "Modulate a parameter or mixer level: LFO, follower, random, macro.";
        c.description =
            "The whole gesture in one undo step. It takes the first rack homed on the target's "
            "track that holds a Mod Out, else the first rack named Modulators, else adds one on "
            "a new channel named Modulators (rack: choose one instead). It adds the source "
            "module (lfo: an LFO; random: an LFO's Random Steps preset; macro: a Macro, its "
            "first knob; follower: a Follower fed by an In module and a send from the track "
            "from), cables it into the first free Mod Out input (adding a Mod Out when none is "
            "free) and routes that input to the target. {module, input} routes a Mod Out input "
            "already patched. For a macro, knob names the knob to move. depth as "
            "modulation.add; default a quarter of a plug-in "
            "parameter's range, 6 dB, or 50 percent. A parameter reached through its envelope "
            "(fx.describe: modulatable_by_envelope) gets a one-point envelope at its current "
            "value first when it has none. A rack that would close a loop (it "
            "already drives the track a follower hears, or hears the target) is passed over "
            "for the next, or a new Modulators rack. Only a real cycle is refused "
            "(modulation.cycle): a follower on a track the target feeds, itself included. "
            "target: {fx, param} (a plug-in parameter by key, name or CLAP id; fx.describe "
            "full marks the modulatable ones), {track, param: volume|pan|width} or {route, "
            "param: volume}. The source module is left set to its defaults: change it with "
            "module.set {module, params} on the module this returns (an LFO's rate, shape or "
            "sync; a follower's gain and threshold; a macro's macro_1, which sits at 0 until "
            "moved, so a macro does nothing at first; module.describe {type} lists the knobs) "
            "and the route's depth with modulation.set.";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModulateArgs>("modulate_args")
                     .field("target", &ModulateArgs::target, targetSchema())
                     .field("source", &ModulateArgs::source, modulateSourceSchema())
                     .field("depth", &ModulateArgs::depth, schema::number())
                     .field("from", &ModulateArgs::from,
                            idArg("tr", "Track").doc("The track a follower listens to."))
                     .field("rack", &ModulateArgs::rack, idArg("rk", "Rack"))
                     .build();
        c.result = schema::object<Modulated>("modulated")
                       .field("modulation", &Modulated::modulation, schema::string())
                       .field("param", &Modulated::param,
                              schema::string().doc("A plug-in target's canonical reference."))
                       .field("param_name", &Modulated::param_name,
                              schema::string().doc("A plug-in target's display name."))
                       .field("rack", &Modulated::rack, schema::string())
                       .field("module", &Modulated::module, schema::string())
                       .field("mod_out", &Modulated::mod_out, schema::string())
                       .field("input", &Modulated::input, schema::string())
                       .field("send", &Modulated::send, schema::string())
                       .field("channel", &Modulated::channel,
                              schema::string().doc("The channel made for a new rack."))
                       .field("knob", &Modulated::knob,
                              schema::string().doc("A macro's knob to move, by key."))
                       .build();
        c.examples = {{"An LFO on a gain", j(R"({"target": {"fx": "fx_1", "param": "gain"},
                             "source": "lfo"})"),
                       j(R"({"modulation": "mr_1", "param": "gain", "param_name": "<any>",
                             "rack": "rk_1", "module": "mo_3", "mod_out": "mo_2",
                             "input": "in_1"})"),
                       "song_with_mod_out"}};
        c.errors = {{"modulation.cycle", "a follower on a track the target feeds",
                     "listen to another track, or modulate a buss it feeds"}};
        c.run = [](const ModulateArgs& a, Context& ctx) -> Result<Modulated> {
            EngineApp& app = engine::app(ctx);
            const auto nested = [&](const char* name, json::Value args,
                                    const std::string& at) -> Result<json::Value> {
                auto r = ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
                if (!r) {
                    Error error = std::move(r).error();
                    error.path.insert(0, at);
                    return error;
                }
                return r;
            };
            const int targets =
                int(bool(a.target.fx)) + int(bool(a.target.track)) + int(bool(a.target.route));
            if (targets != 1)
                return refused("modulation.target", "a target names one fx, track or route",
                               "/target",
                               "give {fx, param}, {track, param} or {route, param: volume}");
            const bool follower = a.source.kind == "follower";
            if (follower != bool(a.from))
                return refused("command.invalid_args",
                               follower ? "a follower listens to a track"
                                        : "only a follower "
                                          "takes from",
                               "/from",
                               follower ? "give from, the track whose level it follows"
                                        : "leave out from, or use source: follower");
            if (a.rack && a.source.input)
                return refused("command.invalid_args", "a Mod Out input names its rack", "/rack",
                               "leave out rack with source {module, input}");
            json::Value target = json::Value::object();
            if (a.target.fx) target["fx"] = json::Value(*a.target.fx);
            if (a.target.track) target["track"] = json::Value(*a.target.track);
            if (a.target.route) target["route"] = json::Value(*a.target.route);
            target["param"] = json::Value(a.target.param);
            const bool fxTarget = bool(a.target.fx);
            const double depth = a.depth.value_or(fxTarget                     ? 0.25
                                                  : a.target.param == "volume" ? 6.0
                                                                               : 50.0);
            // A parameter reached through its envelope gets one first, a
            // single point at its current value (ADR-0096 step M4).
            if (fxTarget) {
                const model::Session s0 = app.view();
                const auto fxId = Id::parse(*a.target.fx);
                const model::FxInstance* fx = fxId ? s0.fx.find(*fxId) : nullptr;
                if (fx)
                    if (auto found =
                            modulatableParam(app, *fx, a.target.param, detail::Ambiguity::Refuse);
                        found && found->envelope &&
                        !activeEnvelopeOn(app, s0, *fxId, *fx, found->info.id)) {
                        auto now = envelopeValueNow(app, *fxId, *fx, a.target.param, found->info);
                        if (!now) {
                            Error e = std::move(now).error();
                            e.path = "/target";
                            return e;
                        }
                        json::Value args = json::Value::object();
                        args["fx"] = json::Value(*a.target.fx);
                        args["param"] = json::Value(a.target.param);
                        json::Value point = json::Value::object();
                        point["bar"] = json::Value(1.0);
                        point["value"] = json::Value(*now);
                        args["points"] = json::Value::array();
                        args["points"].asArray().push_back(std::move(point));
                        auto added = nested("env.add", std::move(args), "/target");
                        if (!added) return std::move(added).error();
                    }
            }
            const auto route = [&](const std::string& module, const std::string& input,
                                   Modulated out) -> Result<Modulated> {
                json::Value args = json::Value::object();
                json::Value source = json::Value::object();
                source["module"] = json::Value(module);
                source["input"] = json::Value(input);
                args["source"] = json::Value(std::move(source));
                args["target"] = json::Value(target);
                args["depth"] = json::Value(depth);
                auto added = nested("modulation.add", std::move(args), "");
                if (!added) {
                    Error e = std::move(added).error();
                    if (e.code == "modulation.cycle" && follower) {
                        e.path = "/from";
                        e.hint = "the target feeds " + *a.from +
                                 ": listen to a track the target does not feed";
                    }
                    return e;
                }
                out.modulation = added->find("modulation")->asString();
                if (const json::Value* p = added->find("param")) out.param = p->asString();
                if (const json::Value* p = added->find("param_name"))
                    out.param_name = p->asString();
                out.mod_out = module;
                out.input = input;
                return out;
            };
            if (a.source.input) return route(a.source.input->module, a.source.input->input, {});

            const model::Session s = app.view();
            const model::EnvTarget wanted = targetOf(a.target);
            const auto home = homeTrackOf(s, wanted);
            const auto node = model::modulationTargetNode(s, wanted);
            std::optional<Id> heard;  // the track a follower listens to
            if (follower) {
                auto id = existing<model::Track>(s, *a.from, "/from");
                if (!id) return std::move(id).error();
                heard = *id;
                // A real cycle: the target already feeds what the follower
                // hears, whichever rack would hold it (ADR-0096, 2026-11-20).
                if ((home && *heard == *home) || (node && reaches(s, *node, *heard)))
                    return refused("modulation.cycle",
                                   home && *heard == *home
                                       ? "a follower on " + a.from.value() +
                                             " cannot modulate what plays on it"
                                       : "the target feeds " + a.from.value() +
                                             ", so following it would modulate itself",
                                   "/from",
                                   "listen to a track the target does not feed, or modulate a "
                                   "buss " +
                                       a.from.value() + " feeds");
            }
            // A rack that closes no loop: it does not already reach the
            // track a follower hears, and the target does not reach it.
            const auto loopless = [&](Id r) {
                return !(heard && reaches(s, r, *heard)) && !(node && reaches(s, *node, r));
            };
            // The rack: chosen, the home track's first with a Mod Out, the
            // first Modulators rack with one, or a new Modulators channel's;
            // one that would close a loop is passed over (a chosen one is
            // the caller's, and refused as before).
            Modulated out;
            std::optional<Id> rack;
            if (a.rack) {
                auto id = existing<model::Rack>(s, *a.rack, "/rack");
                if (!id) return std::move(id).error();
                rack = *id;
            }
            const auto hasModOut = [&](Id r) {
                for (const auto& m : s.modules)
                    if (m.value->rack == r && m.value->type == model::kModOutModule) return true;
                return false;
            };
            for (const auto& r : s.racks)
                if (!rack && home && r.value->track == *home && hasModOut(r.id) && loopless(r.id))
                    rack = r.id;
            for (const auto& r : s.racks)
                if (!rack && r.value->name == "Modulators" && hasModOut(r.id) && loopless(r.id))
                    rack = r.id;
            if (!rack) {
                json::Value args = json::Value::object();
                args["name"] = json::Value("Modulators");
                auto made = nested("rack.add", std::move(args), "");
                if (!made) return std::move(made).error();
                rack = *Id::parse(made->find("rack")->asString());
                out.channel = made->find("track")->asString();
                app.txn().modify<model::Rack>(*rack, [](model::Rack& r) { r.name = "Modulators"; });
            }
            out.rack = rack->str();
            // The first Mod Out input with no cable into it and no route from it.
            std::optional<std::pair<std::string, std::string>> free;
            {
                const model::Session now = app.view();
                for (const auto& m : now.modules) {
                    if (free || m.value->rack != *rack || m.value->type != model::kModOutModule)
                        continue;
                    for (int k = 1; k <= 16 && !free; ++k) {
                        const std::string key = "in_" + std::to_string(k);
                        bool taken = false;
                        for (const auto& cable : now.cables)
                            taken |= cable.value->to.object == m.id && cable.value->to.key == key;
                        for (const auto& mod : now.modulations)
                            taken |=
                                mod.value->source.module == m.id && mod.value->source.input == key;
                        if (!taken) free.emplace(m.id.str(), key);
                    }
                }
            }
            const auto addModule = [&](const char* type,
                                       std::optional<std::string> preset) -> Result<json::Value> {
                json::Value args = json::Value::object();
                args["rack"] = json::Value(rack->str());
                args["type"] = json::Value(type);
                if (preset) args["preset"] = json::Value(*preset);
                return nested("module.add", std::move(args), "/source");
            };
            if (!free) {
                auto modOut = addModule("mod_out", std::nullopt);
                if (!modOut) return std::move(modOut).error();
                free.emplace(modOut->find("module")->asString(), "in_1");
            }
            const std::string& kind = a.source.kind;
            auto module = addModule(
                kind == "follower" ? "follower"
                : kind == "macro"  ? "macro"
                                   : "lfo",
                kind == "random" ? std::optional<std::string>("Random Steps") : std::nullopt);
            if (!module) return std::move(module).error();
            out.module = module->find("module")->asString();
            const std::string output = module->find("outputs")->at(0).asString();
            if (kind == "macro")
                if (const auto* knobs = module->find("knobs");
                    knobs && knobs->isArray() && !knobs->asArray().empty())
                    out.knob = knobs->at(0).asString();
            const auto cable = [&](const std::string& from,
                                   const std::string& to) -> Result<json::Value> {
                json::Value args = json::Value::object();
                args["from"] = json::Value(from);
                args["to"] = json::Value(to);
                return nested("cable.add", std::move(args), "/source");
            };
            SUPPLE_TRY(cable(*out.module + "." + output, free->first + "." + free->second));
            if (follower) {
                auto in = addModule("in", std::nullopt);
                if (!in) return std::move(in).error();
                const std::string inId = in->find("module")->asString();
                SUPPLE_TRY(cable(inId + ".l", *out.module + ".in"));
                json::Value args = json::Value::object();
                args["from"] = json::Value(*a.from);
                json::Value to = json::Value::object();
                to["module"] = json::Value(inId);
                args["to"] = json::Value(std::move(to));
                auto send = nested("route.add", std::move(args), "");
                if (!send) {
                    Error e = std::move(send).error();
                    if (e.code == "route.cycle") {
                        e.code = "modulation.cycle";
                        e.path = "/from";
                        e.hint = "the rack feeds " + *a.from +
                                 ": choose another rack, or listen to another track";
                    }
                    return e;
                }
                out.send = send->find("id")->asString();
            }
            return route(free->first, free->second, std::move(out));
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ModTelemetryArgs, ModTelemetry> c;
        c.name = "modulation.telemetry";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Read each modulation route's live offset and span.";
        c.description =
            "offset: the route's target's current offset, the sum of every route on it, as the "
            "engine last applied it (a plug-in parameter's as a fraction of its range, a "
            "volume's in dB, a pan's or width's in percent); absent while no running plan "
            "applies the route. low and high: what this route alone can add, depth times its "
            "signal range (bipolar -1..1, unipolar 0..1), in the same unit; absent while "
            "nothing is cabled into its Mod Out input.";
        c.tags = {"modulation", "rack"};
        c.args = schema::object<ModTelemetryArgs>("modulation_telemetry_args")
                     .field("track", &ModTelemetryArgs::track, idArg("tr", "Target track"))
                     .build();
        auto entry = schema::object<ModReading>("modulation_reading")
                         .field("modulation", &ModReading::modulation, schema::string())
                         .field("offset", &ModReading::offset, schema::number())
                         .field("low", &ModReading::low, schema::number())
                         .field("high", &ModReading::high, schema::number())
                         .build();
        c.result = schema::object<ModTelemetry>("modulation_telemetry")
                       .field("modulations", &ModTelemetry::modulations, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Stopped, nothing cabled into the route's input yet", j("{}"),
                       j(R"({"modulations": [{"modulation": "mr_1"}]})"), "song_with_modulation"}};
        c.run = [](const ModTelemetryArgs& a, Context& ctx) -> Result<ModTelemetry> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            std::optional<Id> track;
            if (a.track) {
                auto id = existing<model::Track>(s, *a.track, "/track");
                if (!id) return std::move(id).error();
                track = *id;
            }
            const auto live = app.modulationOffsets();
            ModTelemetry out;
            for (const auto& e : s.modulations) {
                const model::Modulation& m = *e.value;
                if (track && model::modulationTargetNode(s, m.target) != track) continue;
                ModReading row;
                row.modulation = e.id.str();
                if (const auto it = live.find(e.id); it != live.end()) row.offset = it->second;
                // A Mod Out input nothing feeds carries nothing: no span.
                if (!fedBy(s, m.source).empty()) {
                    const bool unipolar = rangeOf(app, s, m.source) == "unipolar";
                    row.low = unipolar ? std::min(0.0, m.depth) : -std::abs(m.depth);
                    row.high = unipolar ? std::max(0.0, m.depth) : std::abs(m.depth);
                }
                out.modulations.push_back(std::move(row));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
