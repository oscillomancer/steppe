// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "rack_controls.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "rack_scene.h"
#include "supple/engine/midi_control.h"

namespace supple::gui::racks {
namespace {

// "Cutoff" reads "cutoff" after a module's name; "FM Depth" stays.
std::string after(std::string name) {
    if (name.size() >= 1 && std::isupper(static_cast<unsigned char>(name[0])) &&
        (name.size() == 1 || !std::isupper(static_cast<unsigned char>(name[1]))))
        name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
    return name;
}

}  // namespace

std::vector<MacroGroup> macroGroups(
    const model::Session& s, const rack::Catalog& catalog, model::Id rack,
    const std::function<std::string(model::Id fx, const std::string& key)>& pluginParamName) {
    std::vector<MacroGroup> out;
    const rack::Catalog::Entry* macro = catalog.find(kMacroModule);
    if (!macro) return out;
    const rack::ModuleType& type = *macro->type;
    // A cable's far end, for people: "Filter cutoff", "VCA in".
    auto target = [&](const model::CableEnd& to) {
        model::Id owner = to.object;
        std::string what = to.key;
        if (to.object.kind == model::Kind::Fx) {
            for (const auto& m : s.modules)
                if (m.value->fx == to.object) owner = m.id;
            // As the plug-in shows it ("Mod Rate"): its own words, cased its way.
            if (pluginParamName)
                return moduleTitle(s, catalog, owner) + " " + pluginParamName(to.object, to.key);
        } else if (const model::RackModule* m = s.modules.find(to.object)) {
            if (const rack::Catalog::Entry* t = catalog.find(m->type)) {
                const rack::ModuleType& tt = *t->type;
                if (const int p = tt.param(to.key); p >= 0)
                    what = tt.params()[static_cast<std::size_t>(p)].name;
                else if (const int k = tt.input(to.key); k >= 0)
                    what = tt.input(k).name;
            }
        }
        return moduleTitle(s, catalog, owner) + " " + after(what);
    };
    for (const auto& e : s.modules) {
        const model::RackModule& m = *e.value;
        if (m.rack != rack || m.type != kMacroModule) continue;
        MacroGroup g;
        g.module = e.id;
        g.title = moduleTitle(s, catalog, e.id);
        for (int i = 0; i < static_cast<int>(type.params().size()); ++i) {
            const std::string& key = type.params()[static_cast<std::size_t>(i)].key;
            if (!key.starts_with("macro_")) continue;
            const std::string port = "out_" + key.substr(6);
            std::vector<const model::Cable*> cables;
            for (const auto& c : s.cables)
                if (c.value->from.object == e.id && c.value->from.key == port)
                    cables.push_back(c.value.get());
            if (cables.empty()) continue;
            MacroKnob k;
            k.param = i;
            k.key = key;
            k.name = target(cables.front()->to);
            if (cables.size() > 1) k.name += " +" + std::to_string(cables.size() - 1);
            g.knobs.push_back(std::move(k));
        }
        if (!g.knobs.empty()) out.push_back(std::move(g));
    }
    return out;
}

std::vector<model::Id> racksFeeding(const model::Session& s, model::Id track) {
    std::vector<model::Id> out;
    for (const auto& r : s.routes) {
        if (!r.value->from.module || r.value->to.track != track) continue;
        const model::RackModule* m = s.modules.find(*r.value->from.module);
        if (m && std::find(out.begin(), out.end(), m->rack) == out.end()) out.push_back(m->rack);
    }
    return out;
}

std::vector<ui::MenuItem> knobMenu(const model::Session& s, model::Id module,
                                   const sd::ParamModel& p, double current, KnobMenuHooks hooks) {
    std::vector<ui::MenuItem> items;
    items.push_back({"Default (" + paramText(p, p.def) + ")", "Double-click",
                     [set = hooks.set, def = p.def] { set(def); }});
    // Automation: on the rack's home channel, as a lane of its own.
    std::optional<model::Id> env;
    bool shown = false;
    for (const auto& e : s.envelopes)
        if (e.value->target.object == module && e.value->target.param == p.key) {
            env = e.id;
            shown = e.value->visible;
        }
    if (p.automatable && p.kind != sd::ParamModel::Kind::Toggle) {
        if (!env) {
            items.push_back(
                {"Automate", "",
                 [exec = hooks.exec, module, key = p.key, value = paramJson(p, current)] {
                     json::Value point = json::Value::object();
                     point["beat"] = 0.0;
                     point["value"] = value;
                     json::Value points = json::Value::array();
                     points.push_back(std::move(point));
                     json::Value args = json::Value::object();
                     args["module"] = module.str();
                     args["param"] = key;
                     args["points"] = std::move(points);
                     exec("env.add", std::move(args));
                 }});
        } else {
            items.push_back({shown ? "Hide automation lane" : "Show automation lane", "",
                             [exec = hooks.exec, id = *env, shown] {
                                 json::Value args = json::Value::object();
                                 args["env"] = id.str();
                                 args["visible"] = !shown;
                                 exec("env.set", std::move(args));
                             }});
        }
    }
    // MIDI learn: the dialog listens; module.learn.set binds (one per knob).
    const model::RackModule* rm = s.modules.find(module);
    const model::ParamLearn* bound = nullptr;
    if (rm)
        for (const auto& l : rm->learn)
            if (l.param == p.key) bound = &l;
    if (rm && rm->type != model::kPluginModule) {
        items.push_back({bound ? "MIDI: " + engine::describe(bound->control) + "…" : "MIDI learn…",
                         "", hooks.learn});
        if (bound)
            items.push_back({"Remove MIDI learn", "", [exec = hooks.exec, module, key = p.key] {
                                 json::Value args = json::Value::object();
                                 args["module"] = module.str();
                                 args["param"] = key;
                                 exec("module.learn.remove", std::move(args));
                             }});
    }
    if (p.kind == sd::ParamModel::Kind::Choice) {
        items.push_back({"", "", {}, true, true});
        for (std::size_t k = 0; k < p.choices.size(); ++k)
            items.push_back({p.choices[k], "",
                             [set = hooks.set, k] { set(static_cast<double>(k)); }, true, false,
                             std::lround(current) == static_cast<long>(k)});
    }
    return items;
}

LearnRequest knobLearn(const model::Session& s, model::Id module, const std::string& title,
                       const sd::ParamModel& p,
                       std::function<void(const std::string& command, json::Value args)> exec) {
    LearnRequest request;
    const model::RackModule* rm = s.modules.find(module);
    request.title =
        (rm ? model::rackLabel(s, rm->rack) + " » " : std::string()) + title + " · " + p.name;
    const bool toggle = p.kind == sd::ParamModel::Kind::Toggle;
    request.takeover = !toggle;
    request.mode = toggle ? model::ControlMode::Toggle : model::ControlMode::Absolute;
    if (rm)
        for (const auto& l : rm->learn)
            if (l.param == p.key)
                request.current = LearnChoice(static_cast<const model::ControlBinding&>(l));
    request.onOk = [exec, module, key = p.key](const LearnChoice& choice) {
        json::Value args = learnArgs(choice);
        args["module"] = module.str();
        args["param"] = key;
        exec("module.learn.set", std::move(args));
    };
    request.onRemove = [exec, module, key = p.key] {
        json::Value args = json::Value::object();
        args["module"] = module.str();
        args["param"] = key;
        exec("module.learn.remove", std::move(args));
    };
    return request;
}

// --- shared watches ----------------------------------------------------------------

namespace {
// Every view's subscriptions, by app (tests run several apps).
std::map<const engine::EngineApp*, std::vector<const Watchers*>>& registry() {
    static std::map<const engine::EngineApp*, std::vector<const Watchers*>> views;
    return views;
}
bool wanted(const engine::EngineApp& app, model::Id id) {
    for (const Watchers* w : registry()[&app])
        if (std::find(w->modules().begin(), w->modules().end(), id) != w->modules().end())
            return true;
    return false;
}
}  // namespace

Watchers::Watchers(engine::EngineApp& app) : app_(app) { registry()[&app_].push_back(this); }

Watchers::~Watchers() {
    set({});
    auto& views = registry()[&app_];
    std::erase(views, this);
    if (views.empty()) registry().erase(&app_);
}

void Watchers::set(std::vector<model::Id> modules) {
    std::vector<model::Id> before = std::exchange(mine_, std::move(modules));
    for (model::Id id : mine_)
        if (rack::Instance* in = app_.moduleInstance(id)) in->watch(true);
    for (model::Id id : before)
        if (!wanted(app_, id))
            if (rack::Instance* in = app_.moduleInstance(id)) in->watch(false);
}

}  // namespace supple::gui::racks
