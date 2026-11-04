// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// MIDI learn (ADR-0027): plug-in parameters driven by a controller. The
// bindings live on the effect (fx `learn`); fx.learn.set / remove / list edit
// and read them; EngineApp::pollControl moves the parameters.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>

#include "command_validation.h"
#include "supple/engine/app.h"
#include "supple/engine/midi_control.h"
#include "supple/model/schemas.h"
#include "supple/params/params.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

constexpr std::uint64_t kRestMs = 300;      // a plain parameter's control at rest: commit
constexpr std::uint64_t kGestureMs = 1000;  // a pause this long starts a new undo step
constexpr double kFineStep = 1.0 / 128;     // one encoder step of a continuous parameter

EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
json::Value j(std::string_view s) { return json::parse(s).value(); }

std::optional<std::uint32_t> numericId(std::string_view text) {
    std::uint32_t id = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), id);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) return std::nullopt;
    return id;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// The parameter a binding moves, and where it stands (normalized).
struct Target {
    std::shared_ptr<params::DescribedParam> semantic;  // else a plain parameter
    std::uint32_t id = 0;
    double min = 0, max = 1;
    bool stepped = false;
    double current = 0, step = kFineStep;
    double quantize(double n) const {
        if (!stepped || step >= 1.0) return stepped ? std::round(n) : n;
        return std::clamp(std::round(n / step) * step, 0.0, 1.0);
    }
};

double semanticStep(const params::ParamSpec& spec) {
    switch (spec.type) {
        case params::Type::Bool:
            return 1.0;
        case params::Type::Enum:
        case params::Type::Int: {
            const double span = spec.max - spec.min;
            return span > 0 && span <= 127 ? 1.0 / span : kFineStep;
        }
        case params::Type::Float:
            return kFineStep;
    }
    return kFineStep;
}

struct LearnArgs {
    std::string fx, param;
    model::MidiControl midi;
    std::optional<model::ControlMode> mode;
    std::optional<bool> softTakeover, whenSelected, whenFocused, whenVisible;
};
struct LearnResult {
    std::string param, control;
};
struct LearnRef {
    std::string fx, param;
};
struct Empty {};
struct LearnListArgs {
    std::optional<std::string> fx;
};
struct LearnList {
    json::Value learn = json::Value::array();
};

// A binding's parameter, as the effect names it: a semantic key, or a plain
// parameter's id (given by id or by name).
Result<std::string> resolveParam(EngineApp& a, const model::FxInstance& fx,
                                 const std::string& param) {
    auto schema = a.fxSchema(fx);
    if (!schema) return std::move(schema).error().at("/fx");
    if (schema->isObject()) {
        const json::Value* ps = schema->find("params");
        if (ps && ps->isObject() && ps->find(param)) return param;
        return Error("fx.param_unknown", "no parameter " + param, "/param",
                     "use a parameter key from fx.describe {full: true}");
    }
    auto list = a.fxParams(fx);
    if (!list) return std::move(list).error().at("/fx");
    const auto byId = numericId(param);
    const host::ParamInfo* found = nullptr;
    for (const host::ParamInfo& p : *list)
        if ((byId && p.id == *byId) || (!byId && iequals(p.name, param))) {
            if (found)
                return Error("fx.param_ambiguous", "parameter name is ambiguous", "/param",
                             "use the numeric id from fx.describe full");
            found = &p;
        }
    if (!found)
        return Error("fx.param_unknown", "unknown parameter " + param, "/param",
                     "use a parameter name or id from fx.describe full");
    if (found->flags & CLAP_PARAM_IS_READONLY)
        return Error("fx.param_readonly", "parameter is read-only", "/param",
                     "choose a writable parameter");
    return std::to_string(found->id);
}

json::Value learnJson(Id fx, const model::ParamLearn& l) {
    json::Value v = model::paramLearnSchema().encode(l);
    v["fx"] = fx.str();
    v["control"] = describe(l.control);
    return v;
}

}  // namespace

bool EngineApp::hasLearn() const {
    const model::Session s = head();
    for (const auto& e : s.fx)
        if (!e.value->learn.empty()) return true;
    for (const auto& e : s.tracks)
        if (!e.value->learn.empty()) return true;
    for (const auto& e : s.routes)
        if (!e.value->learn.empty()) return true;
    return false;
}

void EngineApp::pollControl() {
    if (!txn_) commitLearned(false);
    if (!engine_) return;
    controlTaken_.clear();
    engine_->takeControlMidi(controlTaken_);
    if (controlTaken_.empty()) return;
    const std::vector<std::string> ports = engine_->midiPorts();
    std::vector<ControlEvent> events;
    events.reserve(controlTaken_.size());
    for (const ControlMidiEvent& e : controlTaken_)
        events.push_back({e, e.port < ports.size() ? ports[e.port] : std::string()});
    if (onControl) onControl(events);
    // A caller's open transaction is not ours to add to; the messages go.
    if (learning_ || txn_) return;
    applyLearn(events);
}

void EngineApp::applyLearn(const std::vector<ControlEvent>& events) {
    const model::Session s = head();
    for (const auto& entry : s.fx) {
        for (const model::ParamLearn& learn : entry.value->learn) {
            const bool heard = std::any_of(events.begin(), events.end(), [&](const auto& e) {
                return matches(learn.control, e.midi, e.device);
            });
            if (!heard) continue;
            if ((learn.whenSelected || learn.whenFocused || learn.whenVisible) &&
                !(learnGate && learnGate(entry.id, learn)))
                continue;
            // Every message for it, in order: relative steps add up, the last
            // absolute position wins.
            Takeover& takeover = learnState_[{entry.id, learn.param}].takeover;
            const auto set = moveParam(entry.id, learn.param, [&](double current, double step) {
                std::optional<double> next;
                for (const ControlEvent& e : events) {
                    if (!matches(learn.control, e.midi, e.device)) continue;
                    const ControlReading r = read(e.midi, learn.mode);
                    if (auto v =
                            nextValue(current, r, learn.mode, learn.softTakeover, step, takeover)) {
                        current = *v;
                        takeover.set = current;
                        next = current;
                    }
                }
                return next;
            });
            if (set) takeover.set = *set;
        }
    }
    applyControlLearn(events);  // tracks' and sends' own controls (ADR-0058)
}

std::optional<double> EngineApp::moveParam(
    model::Id id, const std::string& param,
    const std::function<std::optional<double>(double current, double step)>& next) {
    const model::FxInstance* found = head().fx.find(id);
    if (!found || model::frozenOwner(head(), id)) return std::nullopt;
    const model::FxInstance fx = *found;
    // Where the parameter stands now.
    Target target;
    host::Instance* live = pool_ ? pool_->find(id) : nullptr;
    if (live && live->failed()) live = nullptr;
    auto schema = fxSchema(fx);
    if (!schema) return std::nullopt;
    if (schema->isObject()) {
        const json::Value* ps = schema->find("params");
        const json::Value* d = ps && ps->isObject() ? ps->find(param) : nullptr;
        if (!d) return std::nullopt;
        auto described = params::fromDescription(param, *d);
        if (!described) return std::nullopt;
        target.semantic = std::move(*described);
        const params::ParamSpec& spec = target.semantic->spec;
        double physical = spec.def;
        if (fx.patch)
            if (const json::Value* p = fx.patch->find("params"); p && p->isObject())
                if (const json::Value* v = p->find(param))
                    if (auto parsed = params::fromJson(spec, *v)) physical = *parsed;
        target.current = params::toNormalized(spec, physical);
        target.step = semanticStep(spec);
        target.stepped = spec.type != params::Type::Float;
    } else {
        const auto pid = numericId(param);
        if (!pid) return std::nullopt;
        auto list = live ? Result<std::vector<host::ParamInfo>>(live->params()) : fxParams(fx);
        if (!list) return std::nullopt;
        const auto info =
            std::find_if(list->begin(), list->end(), [&](const auto& p) { return p.id == *pid; });
        if (info == list->end() || info->max <= info->min || (info->flags & CLAP_PARAM_IS_READONLY))
            return std::nullopt;
        target.id = *pid;
        target.min = info->min;
        target.max = info->max;
        target.stepped = (info->flags & CLAP_PARAM_IS_STEPPED) != 0;
        if (target.stepped) target.step = 1.0 / std::max(1.0, info->max - info->min);
        double value = info->def;
        if (auto pending = learnCommits_.find({id, *pid}); pending != learnCommits_.end())
            value = pending->second.value;
        else if (live) {
            if (auto v = live->paramValue(*pid)) value = *v;
        } else if (auto v = fx.params.find(*pid); v != fx.params.end())
            value = v->second;
        target.current = std::clamp((value - info->min) / (info->max - info->min), 0.0, 1.0);
    }
    auto moved = next(target.current, target.step);
    if (!moved) return std::nullopt;
    const double to = target.quantize(std::clamp(*moved, 0.0, 1.0));
    if (std::abs(to - target.current) < 1e-12) return std::nullopt;
    const std::uint64_t now = serviceClockMs();
    LearnState& state = learnState_[{id, param}];
    if (now > state.lastMs + kGestureMs) ++state.serial;
    state.lastMs = now;
    if (target.semantic) {
        const params::ParamSpec& spec = target.semantic->spec;
        const double physical = params::clampPhysical(spec, params::fromNormalized(spec, to));
        json::Value values = json::Value::object();
        values[param] = params::toJson(spec, physical);
        json::Value args = json::Value::object();
        args["fx"] = id.str();
        args["values"] = std::move(values);
        const cmd::Origin origin{
            cmd::Origin::Type::Internal, "midi", "",
            "midi:" + id.str() + ":" + param + ":" + std::to_string(state.serial)};
        if (!commands_) return std::nullopt;
        if (auto set = commands_->execute({"fx.param.set", std::move(args), origin}); !set) {
            emit("engine.error", pluginErrorJson(set.error()));
            return std::nullopt;
        }
        return to;
    }
    double value = target.min + to * (target.max - target.min);
    if (target.stepped) value = std::round(value);
    LearnCommit& commit = learnCommits_[{id, target.id}];
    commit.value = value;
    commit.lastMs = now;
    commit.live = live != nullptr;
    mainWake_.notify();  // rest deadline was published above
    if (live)
        if (auto set = live->setParam(target.id, value); !set) {
            emit("engine.error", pluginErrorJson(set.error()));
            return std::nullopt;
        }
    return to;
}

std::optional<EngineApp::TouchedParam> EngineApp::lastTouched() const {
    if (touched_ && head().fx.contains(touched_->fx)) return touched_;
    return std::nullopt;
}

void EngineApp::touchParam(model::Id fx, std::string param) {
    touched_ = TouchedParam{fx, std::move(param), ++touchSerial_};
}

std::string EngineApp::paramName(model::Id id, const std::string& param) {
    const model::FxInstance* fx = head().fx.find(id);
    if (!fx) return param;
    if (auto schema = fxSchema(*fx); schema && schema->isObject()) {
        const json::Value* ps = schema->find("params");
        const json::Value* d = ps && ps->isObject() ? ps->find(param) : nullptr;
        const json::Value* name = d ? d->find("name") : nullptr;
        return name && name->isString() ? name->asString() : param;
    }
    if (const auto pid = numericId(param))
        if (auto list = fxParams(*fx))
            for (const host::ParamInfo& p : *list)
                if (p.id == *pid) return p.name;
    return param;
}

void EngineApp::commitLearned(bool all) {
    const std::uint64_t now = serviceClockMs();
    for (auto it = learnCommits_.begin(); it != learnCommits_.end();) {
        if (!all && now < it->second.lastMs + kRestMs) {
            ++it;
            continue;
        }
        const OutputKey key = it->first;
        const LearnCommit commit = it->second;
        it = learnCommits_.erase(it);
        if (!head().fx.contains(key.first) || model::frozenOwner(head(), key.first)) continue;
        host::Instance* live = pool_ ? pool_->find(key.first) : nullptr;
        if (commit.live && live && !live->failed() && engine_) {
            // As an edit in the plug-in's own editor: its state into the
            // project, the running instance left as it is.
            PluginOutput output;
            output.fx = key.first;
            output.event.paramId = key.second;
            output.event.value = commit.value;
            if (live->paramsNormalized()) {
                auto normalized = live->normalizedParam(key.second, commit.value);
                if (!normalized) continue;
                output.event.value = *normalized;
            }
            output.idle = true;
            OutputGesture gesture;
            gesture.latest = output;
            gesture.ended = true;
            committingLearn_ = true;
            auto st = commitOutputGesture(key, gesture);
            committingLearn_ = false;
            if (!st) emit("engine.error", pluginErrorJson(st.error()));
            continue;
        }
        if (!commands_) continue;
        json::Value values = json::Value::object();
        values[std::to_string(key.second)] = commit.value;
        json::Value args = json::Value::object();
        args["fx"] = key.first.str();
        args["values"] = std::move(values);
        auto set = commands_->execute(
            {"fx.param.set", std::move(args), {cmd::Origin::Type::Internal, "midi", ""}});
        if (!set) emit("engine.error", pluginErrorJson(set.error()));
    }
}

void registerLearnCommands(cmd::Registry& registry) {
    auto fxId = schema::string().pattern("^fx_[0-9]+$", "plugin instance id");
    {
        Command<LearnArgs, LearnResult> c;
        c.name = "fx.learn.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Let a MIDI controller (CC, note, pitch bend) move a plugin parameter.";
        c.description =
            "One binding per parameter (a new one replaces it). midi.device: a substring of the "
            "MIDI input's name (empty: any). mode for a CC: absolute, relative1 (127 = -1, 1 = "
            "+1), relative2 (63 = -1, 65 = +1), relative3 (65 = -1, 1 = +1) or toggle (above 0 "
            "flips it). soft_takeover: an absolute control acts once it reaches the value. "
            "when_selected / when_focused / when_visible: only while the effect's track is "
            "selected, or its window or editor has the focus or shows (the GUI).";
        c.tags = {"fx", "midi"};
        c.args = schema::object<LearnArgs>("fx_learn_set_args")
                     .field("fx", &LearnArgs::fx, fxId)
                     .field("param", &LearnArgs::param,
                            schema::string().minLength(1).doc(
                                "A semantic key, or a plain parameter's id or name (fx.describe)"))
                     .field("midi", &LearnArgs::midi, model::midiControlSchema())
                     .field("mode", &LearnArgs::mode, model::controlModeSchema())
                     .field("soft_takeover", &LearnArgs::softTakeover, schema::boolean())
                     .field("when_selected", &LearnArgs::whenSelected, schema::boolean())
                     .field("when_focused", &LearnArgs::whenFocused, schema::boolean())
                     .field("when_visible", &LearnArgs::whenVisible, schema::boolean())
                     .build();
        c.result = schema::object<LearnResult>("fx_learn_set_result")
                       .field("param", &LearnResult::param, schema::string())
                       .field("control", &LearnResult::control, schema::string())
                       .build();
        c.examples = {
            {"Gain on CC 21",
             j(R"({"fx":"fx_2","param":"gain","midi":{"kind":"cc","channel":1,"number":21}})"),
             j(R"({"param":"gain","control":"CC 21 · ch 1"})"), "song_with_fx"}};
        c.run = [](const LearnArgs& a, Context& ctx) -> Result<LearnResult> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            const auto id = Id::parse(a.fx);
            const model::FxInstance* fx = id ? s.fx.find(*id) : nullptr;
            if (!fx)
                return Error("fx.unknown", "no " + a.fx, "/fx",
                             "use an fx id from track.list or fx.add");
            auto param = resolveParam(engine, *fx, a.param);
            if (!param) return std::move(param).error();
            model::ParamLearn learn;
            learn.param = *param;
            learn.control = a.midi;
            if (learn.control.kind == model::ControlKind::PitchBend) learn.control.number = 0;
            learn.mode = a.mode.value_or(model::ControlMode::Absolute);
            learn.softTakeover = a.softTakeover.value_or(false);
            learn.whenSelected = a.whenSelected.value_or(false);
            learn.whenFocused = a.whenFocused.value_or(false);
            learn.whenVisible = a.whenVisible.value_or(false);
            engine.txn().modify<model::FxInstance>(*id, [&](model::FxInstance& f) {
                std::erase_if(f.learn, [&](const auto& l) { return l.param == learn.param; });
                f.learn.push_back(learn);
                std::sort(f.learn.begin(), f.learn.end(),
                          [](const auto& x, const auto& y) { return x.param < y.param; });
            });
            return LearnResult{*param, describe(learn.control)};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<LearnRef, Empty> c;
        c.name = "fx.learn.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a plugin parameter's MIDI controller binding.";
        c.tags = {"fx", "midi"};
        c.args = schema::object<LearnRef>("fx_learn_remove_args")
                     .field("fx", &LearnRef::fx, fxId)
                     .field("param", &LearnRef::param, schema::string().minLength(1))
                     .build();
        c.result = schema::object<Empty>("empty").build();
        c.examples = {
            {"Free the gain", j(R"({"fx":"fx_2","param":"gain"})"), j("{}"), "song_with_learn"}};
        c.run = [](const LearnRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            const auto id = Id::parse(a.fx);
            const model::FxInstance* fx = id ? s.fx.find(*id) : nullptr;
            if (!fx)
                return Error("fx.unknown", "no " + a.fx, "/fx",
                             "use an fx id from track.list or fx.add");
            auto param = resolveParam(engine, *fx, a.param);
            const std::string key = param ? *param : a.param;
            if (std::none_of(fx->learn.begin(), fx->learn.end(),
                             [&](const auto& l) { return l.param == key; }))
                return Error("fx.learn_unknown", a.param + " has no MIDI binding", "/param",
                             "use fx.learn.list for the bound parameters");
            engine.txn().modify<model::FxInstance>(*id, [&](model::FxInstance& f) {
                std::erase_if(f.learn, [&](const auto& l) { return l.param == key; });
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<LearnListArgs, LearnList> c;
        c.name = "fx.learn.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the MIDI controller bindings of plugin parameters.";
        c.description = "Every effect's, or one effect's (fx). control names the message in words.";
        c.tags = {"fx", "midi"};
        c.args = schema::object<LearnListArgs>("fx_learn_list_args")
                     .field("fx", &LearnListArgs::fx, fxId)
                     .build();
        c.result = schema::object<LearnList>("fx_learn_list")
                       .field("learn", &LearnList::learn, schema::any())
                       .build();
        c.examples = {
            {"Every binding", json::Value::object(),
             j(R"({"learn":[{"fx":"fx_2","param":"gain","midi":{"kind":"cc","channel":1,"number":21},"control":"CC 21 · ch 1"}]})"),
             "song_with_learn"}};
        c.run = [](const LearnListArgs& a, Context& ctx) -> Result<LearnList> {
            const model::Session s = app(ctx).view();
            LearnList out;
            if (a.fx) {
                const auto id = Id::parse(*a.fx);
                const model::FxInstance* fx = id ? s.fx.find(*id) : nullptr;
                if (!fx)
                    return Error("fx.unknown", "no " + *a.fx, "/fx",
                                 "use an fx id from track.list or fx.add");
                for (const auto& l : fx->learn) out.learn.push_back(learnJson(*id, l));
                return out;
            }
            for (const auto& e : s.fx)
                for (const auto& l : e.value->learn) out.learn.push_back(learnJson(e.id, l));
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace supple::engine
