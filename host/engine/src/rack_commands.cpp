// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modular racks' commands (ADR-0062, rack.md §7): racks, modules and cables.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <set>

#include "command_validation.h"
#include "rack_runtime.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;
namespace sd = script::detail;

json::Value j(std::string_view text) { return json::parse(text).value(); }
EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
auto idOf(const char* prefix, const char* what) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$",
                                    std::string(what) + " id");
}
auto endSchema() {
    return schema::string().pattern(std::string(model::kCableEndPattern),
                                    "a module's port or knob: \"mo_3.out\"");
}

// --- lookups -------------------------------------------------------------------------

template <class T>
std::string some(const model::IdMap<T>& map, const std::function<bool(const T&)>& keep = {}) {
    std::string out;
    int n = 0;
    for (const auto& e : map) {
        if (keep && !keep(*e.value)) continue;
        if (n++ == 8) return out + ", …";
        out += (out.empty() ? "" : ", ") + e.id.str();
    }
    return out.empty() ? "none" : out;
}

Result<Id> rackId(const model::Session& s, const std::string& text, const char* path = "/rack") {
    auto id = Id::parse(text);
    if (id && id->kind == model::Kind::Rack && s.racks.contains(*id)) return *id;
    return Error("rack.unknown", "no rack " + text, path,
                 "racks: " + some(s.racks) + " (rack.list; rack.add makes one)");
}

Result<Id> moduleId(const model::Session& s, const std::string& text,
                    const char* path = "/module") {
    auto id = Id::parse(text);
    if (id && id->kind == model::Kind::Module && s.modules.contains(*id)) return *id;
    return Error("module.unknown", "no module " + text, path,
                 "modules: " + some(s.modules) + " (rack.get lists a rack's)");
}

Result<Id> cableId(const model::Session& s, const std::string& text, const char* path = "/cable") {
    auto id = Id::parse(text);
    if (id && id->kind == model::Kind::Cable && s.cables.contains(*id)) return *id;
    return Error("cable.unknown", "no cable " + text, path,
                 "cables: " + some(s.cables) + " (rack.get lists a rack's)");
}

const rack::Catalog& catalog(EngineApp& a) { return *a.instances().racks().catalog(); }

// A type by id, or by its short name ("vco", "out", "VCO").
const rack::Catalog::Entry* findType(const rack::Catalog& c, std::string_view name) {
    if (const auto* e = c.find(name)) return e;
    const std::string stock = "com.suppleaudio.rack." + std::string(name);
    if (const auto* e = c.find(stock)) return e;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const rack::Catalog::Entry* found = nullptr;
    for (const auto* e : c.list()) {
        std::string n = e->type->info().name;
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const std::string& id = e->type->id();
        const auto dot = id.rfind('.');
        if (n == lower || (dot != std::string::npos && id.substr(dot + 1) == lower)) {
            if (found) return nullptr;  // ambiguous
            found = e;
        }
    }
    return found;
}

Result<std::shared_ptr<const rack::ModuleType>> typeOf(EngineApp& a, std::string_view name,
                                                       const char* path) {
    if (const auto* e = findType(catalog(a), name)) return e->type;
    std::string names;
    int n = 0;
    for (const auto* e : catalog(a).list()) {
        if (n++ == 12) {
            names += ", …";
            break;
        }
        const std::string& id = e->type->id();
        const auto dot = id.rfind('.');
        names += (names.empty() ? "" : ", ") + id.substr(dot == std::string::npos ? 0 : dot + 1);
    }
    return Error("rack.unknown_type", "no module type " + std::string(name), path,
                 "types: " + names + " (module.list)");
}

// A module's type, if its catalog knows it.
std::shared_ptr<const rack::ModuleType> typeOfModule(EngineApp& a, const model::RackModule& m) {
    const auto* e = catalog(a).find(m.type);
    return e ? e->type : nullptr;
}

std::string keysOf(const rack::ModuleType& t, bool inputs, bool outputs, bool params) {
    std::string out;
    auto add = [&](const std::string& k) { out += (out.empty() ? "" : ", ") + k; };
    if (inputs)
        for (int k : t.inputs()) add(t.ports()[static_cast<std::size_t>(k)].key);
    if (outputs)
        for (int k : t.outputs()) add(t.ports()[static_cast<std::size_t>(k)].key);
    if (params)
        for (const auto& p : t.params())
            if (!p.output) add(p.key);
    return out.empty() ? "none" : out;
}

std::string_view signalName(sd::PortModel::Signal s) {
    switch (s) {
        case sd::PortModel::Signal::Audio:
            return "audio";
        case sd::PortModel::Signal::Pitch:
            return "pitch";
        case sd::PortModel::Signal::Gate:
            return "gate";
        case sd::PortModel::Signal::Cv:
            return "cv";
        case sd::PortModel::Signal::Unipolar:
            return "unipolar";
        case sd::PortModel::Signal::Phase:
            return "phase";
    }
    return "audio";
}

std::string_view unitName(sd::ParamModel::Unit u) {
    using U = sd::ParamModel::Unit;
    switch (u) {
        case U::Decibels:
            return "dB";
        case U::Hertz:
            return "Hz";
        case U::Milliseconds:
            return "ms";
        case U::Percent:
            return "%";
        case U::Ratio:
            return "ratio";
        case U::Semitones:
            return "st";
        case U::Cents:
            return "ct";
        case U::Octaves:
            return "oct";
        case U::Degrees:
            return "deg";
        case U::Bpm:
            return "bpm";
        case U::Samples:
            return "samples";
        case U::Meters:
            return "m";
        case U::None:
            break;
    }
    return "";
}

std::string_view kindName(sd::ParamModel::Kind k) {
    using K = sd::ParamModel::Kind;
    return k == K::Choice    ? "choice"
           : k == K::Toggle  ? "toggle"
           : k == K::Integer ? "integer"
                             : "number";
}

// The params a command gives, checked against the type, merged over `base`
// and written canonically: only values that differ from the defaults.
Result<json::Value> mergeParams(const rack::ModuleType& t, const json::Value& base,
                                const json::Value& given, const std::string& path) {
    if (!given.isObject())
        return Error("rack.param", "params is an object of knob values", path,
                     "e.g. {\"cutoff\": 800, \"shape\": \"saw\"}");
    json::Value out = json::Value::object();
    std::map<std::string, double> values;
    for (const auto& p : t.params())
        values[p.key] = detail::moduleParamValue(p, base.isObject() ? base.find(p.key) : nullptr);
    for (const auto& m : given.asObject()) {
        const int index = t.param(m.key);
        const std::string at = json::pointerJoin(path, m.key);
        if (index < 0 || t.params()[static_cast<std::size_t>(index)].output)
            return Error("rack.param", t.info().name + " has no knob " + m.key, at,
                         t.info().name + " has " + keysOf(t, false, false, true));
        const sd::ParamModel& p = t.params()[static_cast<std::size_t>(index)];
        if (p.kind == sd::ParamModel::Kind::Choice && m.value.isString()) {
            bool known = false;
            std::string keys;
            for (std::size_t k = 0; k < p.choices.size(); ++k) {
                known |=
                    sd::choiceKey(p, k) == m.value.asString() || p.choices[k] == m.value.asString();
                keys += (keys.empty() ? "" : ", ") + sd::choiceKey(p, k);
            }
            if (!known)
                return Error("rack.param", m.value.asString() + " is not a choice of " + m.key, at,
                             m.key + " is one of " + keys);
        } else if (p.kind == sd::ParamModel::Kind::Toggle && !m.value.isBool() &&
                   !m.value.isNumber()) {
            return Error("rack.param", m.key + " is a switch", at, "give true or false");
        } else if (p.kind != sd::ParamModel::Kind::Choice &&
                   p.kind != sd::ParamModel::Kind::Toggle &&
                   (!m.value.isNumber() || !std::isfinite(m.value.asDouble()))) {
            return Error("rack.param", m.key + " is a number", at,
                         m.key + " runs " + json::writeCompact(json::Value(p.min)) + " to " +
                             json::writeCompact(json::Value(p.max)) + " " +
                             std::string(unitName(p.unit)));
        }
        values[m.key] = detail::moduleParamValue(p, &m.value);
    }
    for (const auto& p : t.params())
        if (!p.output && values[p.key] != p.def)
            out[p.key] = detail::moduleParamJson(p, values[p.key]);
    return out;
}

// What a module's face measures (its type's layout), or a small default.
// A factory preset's knobs (rack.md §7), as params: its values, the others
// at their defaults.
Result<json::Value> presetParams(const rack::ModuleType& t, const std::string& name,
                                 const char* path) {
    std::string names;
    if (t.code())
        for (const auto& patch : t.code()->factoryPatches()) {
            if (patch.name == name) {
                json::Value out = json::Value::object();
                for (const auto& [id, value] : patch.values)
                    for (const auto& p : t.params())
                        if (p.id == id && !p.output && value != p.def)
                            out[p.key] = detail::moduleParamJson(p, value);
                return out;
            }
            names += (names.empty() ? "" : ", ") + patch.name;
        }
    return Error("rack.preset", t.id() + " has no preset \"" + name + "\"", path,
                 names.empty() ? "it has no presets" : "presets: " + names);
}

std::pair<double, double> sizeOf(const rack::ModuleType* t) {
    if (!t) return {120, 120};
    return {t->layout().width, t->layout().height};
}

// A free place in `rack` for a module `width` wide: right of the others.
std::pair<double, double> freePlace(EngineApp& a, const model::Session& s, Id rack) {
    double right = 0;
    bool any = false;
    for (const auto& e : s.modules) {
        if (e.value->rack != rack) continue;
        const auto type = typeOfModule(a, *e.value);
        right = std::max(right, e.value->x + sizeOf(type.get()).first);
        any = true;
    }
    return {any ? right + 16 : 40, 40};
}

// --- removal -------------------------------------------------------------------------

// A module and what hangs on it: its cables, envelopes on its knobs, an Out's
// routes (a channel taking it loses its input), an In's sends, a plugin
// module's plug-in and its envelopes.
void removeModule(model::EditTxn& t, const model::Session& s, Id module) {
    const model::RackModule& m = *s.modules.find(module);
    for (const auto& e : s.cables)
        if (e.value->from.object == module || e.value->to.object == module ||
            (m.fx && e.value->to.object == *m.fx))
            t.remove<model::Cable>(e.id);
    for (const auto& e : s.envelopes)
        if (e.value->target.object == module || (m.fx && e.value->target.object == *m.fx))
            t.remove<model::Envelope>(e.id);
    for (const auto& e : s.routes) {
        if (e.value->from.module != module && e.value->to.module != module) continue;
        for (const auto& tr : s.tracks)
            if (tr.value->record.input == e.id)
                t.modify<model::Track>(tr.id, [](model::Track& x) { x.record.input.reset(); });
        for (const auto& env : s.envelopes)
            if (env.value->target.object == e.id) t.remove<model::Envelope>(env.id);
        t.remove<model::Route>(e.id);
    }
    t.remove<model::RackModule>(module);
    if (m.fx) t.remove<model::FxInstance>(*m.fx);
}

}  // namespace

// Exposed to the module library's commands (module_commands.cpp).
json::Value describeModuleType(const rack::ModuleType& t) {
    json::Value out = json::Value::object();
    out["type"] = t.id();
    out["name"] = t.info().name;
    out["origin"] = std::string(rack::originName(t.origin()));
    out["vendor"] = t.info().vendor;
    out["version"] = t.info().version;
    out["description"] = t.info().description;
    if (!t.path().empty()) out["path"] = t.path();
    if (t.code() && !t.code()->factoryPatches().empty()) {
        json::Value presets = json::Value::array();
        for (const auto& patch : t.code()->factoryPatches()) presets.push_back(patch.name);
        out["presets"] = std::move(presets);
    }
    json::Value tags = json::Value::array();
    for (const auto& tag : t.info().tags)
        if (!tag.empty()) tags.push_back(tag);
    out["tags"] = std::move(tags);
    out["width"] = t.layout().width;
    out["height"] = t.layout().height;
    json::Value knobs = json::Value::array();
    for (const auto& p : t.params()) {
        if (p.output) continue;
        json::Value k = json::Value::object();
        k["key"] = p.key;
        k["name"] = p.name;
        k["kind"] = std::string(kindName(p.kind));
        if (!unitName(p.unit).empty()) k["unit"] = std::string(unitName(p.unit));
        if (p.kind != sd::ParamModel::Kind::Choice && p.kind != sd::ParamModel::Kind::Toggle) {
            k["min"] = p.min;
            k["max"] = p.max;
        }
        k["default"] = detail::moduleParamJson(p, p.def);
        if (p.kind == sd::ParamModel::Kind::Choice) {
            json::Value choices = json::Value::array();
            for (std::size_t i = 0; i < p.choices.size(); ++i)
                choices.push_back(sd::choiceKey(p, i));
            k["choices"] = std::move(choices);
        }
        if (!p.group.empty()) k["group"] = p.group;
        if (!p.description.empty()) k["help"] = p.description;
        knobs.push_back(std::move(k));
    }
    out["knobs"] = std::move(knobs);
    for (const bool output : {false, true}) {
        json::Value ports = json::Value::array();
        for (int k : output ? t.outputs() : t.inputs()) {
            const sd::PortModel& p = t.ports()[static_cast<std::size_t>(k)];
            json::Value x = json::Value::object();
            x["key"] = p.key;
            x["name"] = p.name;
            x["signal"] = std::string(signalName(p.signal));
            if (!p.description.empty()) x["help"] = p.description;
            if (!output && p.normal != 0) x["normal"] = p.normal;
            if (!p.group.empty()) x["group"] = p.group;
            ports.push_back(std::move(x));
        }
        out[output ? "outputs" : "inputs"] = std::move(ports);
    }
    return out;
}

Result<std::shared_ptr<const rack::ModuleType>> moduleTypeOf(EngineApp& a, std::string_view name,
                                                             const char* path) {
    return typeOf(a, name, path);
}

// Exposed to track.remove (commands.cpp): a track's racks go with it.
void removeRacksOn(model::EditTxn& t, const model::Session& s, Id track) {
    for (const auto& r : s.racks) {
        if (r.value->track != track) continue;
        for (const auto& m : s.modules)
            if (m.value->rack == r.id) removeModule(t, s, m.id);
        t.remove<model::Rack>(r.id);
    }
}

namespace {

// Whether each cable closes a loop (it then carries the previous frame):
// inside a strongly connected set of modules, from a module at or after the
// one it reaches, in id order (rack.md §4.3).
std::map<Id, bool> feedbackOf(const model::Session& s, Id rack) {
    std::vector<Id> nodes;
    std::map<Id, int> index;
    for (const auto& e : s.modules)
        if (e.value->rack == rack) {
            index[e.id] = static_cast<int>(nodes.size());
            nodes.push_back(e.id);
        }
    std::vector<std::vector<int>> next(nodes.size());
    std::vector<std::pair<Id, std::pair<int, int>>> edges;
    for (const auto& e : s.cables) {
        auto a = index.find(e.value->from.object), b = index.find(e.value->to.object);
        if (a == index.end() || b == index.end()) continue;
        next[static_cast<std::size_t>(a->second)].push_back(b->second);
        edges.push_back({e.id, {a->second, b->second}});
    }
    // Tarjan, iteratively bounded by the module count.
    const int n = static_cast<int>(nodes.size());
    std::vector<int> order(nodes.size(), -1), low(nodes.size(), 0), comp(nodes.size(), -1);
    std::vector<char> on(nodes.size(), 0);
    std::vector<int> stack;
    int counter = 0, comps = 0;
    std::function<void(int)> visit = [&](int v) {
        order[static_cast<std::size_t>(v)] = low[static_cast<std::size_t>(v)] = counter++;
        stack.push_back(v);
        on[static_cast<std::size_t>(v)] = 1;
        for (int w : next[static_cast<std::size_t>(v)]) {
            if (order[static_cast<std::size_t>(w)] < 0) {
                visit(w);
                low[static_cast<std::size_t>(v)] =
                    std::min(low[static_cast<std::size_t>(v)], low[static_cast<std::size_t>(w)]);
            } else if (on[static_cast<std::size_t>(w)]) {
                low[static_cast<std::size_t>(v)] =
                    std::min(low[static_cast<std::size_t>(v)], order[static_cast<std::size_t>(w)]);
            }
        }
        if (low[static_cast<std::size_t>(v)] == order[static_cast<std::size_t>(v)]) {
            int w = -1;
            do {
                w = stack.back();
                stack.pop_back();
                on[static_cast<std::size_t>(w)] = 0;
                comp[static_cast<std::size_t>(w)] = comps;
            } while (w != v);
            ++comps;
        }
    };
    for (int v = 0; v < n; ++v)
        if (order[static_cast<std::size_t>(v)] < 0) visit(v);
    std::map<int, int> sizes;
    for (int c : comp) ++sizes[c];
    std::map<Id, bool> out;
    for (const auto& [cable, ends] : edges) {
        const auto [a, b] = ends;
        const bool loop = comp[static_cast<std::size_t>(a)] == comp[static_cast<std::size_t>(b)] &&
                          (sizes[comp[static_cast<std::size_t>(a)]] > 1 || a == b);
        out[cable] = loop && a >= b;
    }
    return out;
}

// --- results -------------------------------------------------------------------------

json::Value moduleEntry(EngineApp& a, const model::Session& s, Id id, bool full) {
    const model::RackModule& m = *s.modules.find(id);
    const auto type = typeOfModule(a, m);
    json::Value e = json::Value::object();
    e["module"] = id.str();
    e["type"] = m.type;
    e["name"] = type ? type->info().name : m.type;
    const std::string label = model::moduleLabel(s, id);
    if (!label.empty()) e["label"] = label;
    e["x"] = m.x;
    e["y"] = m.y;
    const auto [w, h] = sizeOf(type.get());
    e["width"] = w;
    e["height"] = h;
    if (m.params.isObject() && m.params.size()) e["params"] = m.params;
    if (m.bypass) e["bypass"] = true;
    if (m.source) e["source"] = m.source->str();
    if (m.device) e["device"] = *m.device;
    if (m.fx) {
        e["fx"] = m.fx->str();
        if (const model::FxInstance* fx = s.fx.find(*m.fx)) e["plugin"] = fx->plugin.name;
    }
    if (!type) e["problem"] = "its type " + m.type + " is not installed: it is silent";
    if (full && type) {
        json::Value ins = json::Value::array(), outs = json::Value::array(),
                    knobs = json::Value::array();
        for (int k : type->inputs()) ins.push_back(type->ports()[static_cast<std::size_t>(k)].key);
        for (int k : type->outputs())
            outs.push_back(type->ports()[static_cast<std::size_t>(k)].key);
        for (const auto& p : type->params())
            if (!p.output) knobs.push_back(p.key);
        e["inputs"] = std::move(ins);
        e["outputs"] = std::move(outs);
        e["knobs"] = std::move(knobs);
    }
    return e;
}

json::Value outsOf(const model::Session& s, Id rack) {
    json::Value outs = json::Value::array();
    for (const auto& e : s.modules) {
        if (e.value->rack != rack || e.value->type != model::kOutModule) continue;
        json::Value o = json::Value::object();
        o["module"] = e.id.str();
        o["label"] = model::moduleLabel(s, e.id);
        json::Value feeds = json::Value::array();
        for (const auto& r : s.routes)
            if (r.value->from.module == e.id && r.value->to.track)
                feeds.push_back(r.value->to.track->str());
        o["channels"] = std::move(feeds);
        outs.push_back(std::move(o));
    }
    return outs;
}

// --- argument structs ----------------------------------------------------------------

struct RackAdd {
    std::optional<std::string> track, name;
};
struct RackRef {
    std::string rack;
};
struct RackSet {
    std::string rack;
    std::optional<std::string> name, track;
};
struct RackGet {
    std::string rack;
    std::optional<std::string> detail;
};
struct ModuleList {
    std::optional<std::string> query, tag;
};
struct ModuleDescribe {
    std::optional<std::string> type, module;
};
struct ModuleAdd {
    std::string rack;
    std::optional<std::string> type, label, preset;
    std::optional<double> x, y;
    std::optional<json::Value> params;
    std::optional<std::string> source, device, plugin;
};
struct ModuleRef {
    std::string module;
};
struct ModuleSet {
    std::string module;
    std::optional<json::Value> params;
    std::optional<std::string> label, preset;
    std::optional<double> x, y;
    std::optional<bool> bypass;
    std::optional<std::string> source, device;
};
struct ModuleDuplicate {
    std::vector<std::string> modules;
    std::optional<double> dx, dy;
    std::optional<bool> cables;
};
struct ModuleRandomize {
    std::string module;
    std::optional<double> amount;
    std::optional<std::int64_t> seed;
};
struct CableAdd {
    std::string from, to;
    std::optional<double> amount;
    std::optional<std::string> color;
};
struct CableRemove {
    std::optional<std::string> cable, from, to;
};
struct CableSet {
    std::string cable;
    std::optional<double> amount;
    std::optional<std::string> color;
};

template <class A>
void common(Command<A, json::Value>& c, std::string name, Kind kind, std::string summary) {
    c.name = std::move(name);
    c.kind = kind;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = std::move(summary);
    c.tags = {"rack", "modular"};
    c.result = schema::any();
}

auto coordinate() {
    return schema::number().range(-model::kMaxRackCoordinate, model::kMaxRackCoordinate);
}

Status checkColor(const std::optional<std::string>& color, const char* path) {
    if (color && !color->empty() && !model::validTrackColor(*color))
        return Error("cable.color", "unknown colour \"" + *color + "\"", path,
                     "red, orange, yellow, green, teal, blue, violet, grey or \"#rrggbb\"");
    return {};
}

// The default movement of a cable into `p` (rack.md §3.4): an octave for a
// frequency, half the range for anything else.
double defaultAmount(const sd::ParamModel& p) {
    if (p.unit == sd::ParamModel::Unit::Hertz) return 1.0;
    return (p.max - p.min) / 2;
}

}  // namespace

void registerRackCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- rack.add
    [&] {
        Command<RackAdd, json::Value> c;
        common(c, "rack.add", Kind::Edit, "Add a modular rack on a channel, its Out the input.");
        c.description =
            "A rack is a patching space homed on a channel (track, or a new channel named "
            "after name). It starts with an Out module, made the channel's input: what the "
            "patch sends to Out plays on the channel, before its inserts and Console 9 stage. "
            "The channel's MIDI items play into Notes modules. Add modules with module.add and "
            "connect them with cable.add.";
        c.args = schema::object<RackAdd>("rack_add_args")
                     .field("track", &RackAdd::track, idOf("tr", "Channel"))
                     .field("name", &RackAdd::name, schema::string())
                     .build();
        c.examples = {{"A rack on the bass channel", j(R"({"track": "tr_1"})"),
                       j(R"({"rack": "rk_1", "track": "tr_1", "out": "mo_1"})"), "song"},
                      {"A rack on a new channel", j(R"({"name": "Drones"})"),
                       j(R"({"rack": "rk_1", "track": "tr_2", "out": "mo_1"})"), "song"}};
        c.errors = {{"rack.home", "a rack lives on a channel", "give a channel's id"},
                    {"rack.limit", "too many racks", "remove a rack first"}};
        c.run = [](const RackAdd& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            if (s.racks.size() >= model::kMaxRacks)
                return Error("rack.limit", "a project holds at most 64 racks", "",
                             "remove a rack (rack.remove) first");
            model::EditTxn& t = e.txn();
            Id track;
            if (a.track) {
                auto id = Id::parse(*a.track);
                const model::Track* tr = id ? s.tracks.find(*id) : nullptr;
                if (!tr)
                    return Error("track.unknown", "no track " + *a.track, "/track",
                                 "give a channel's id (track.list)");
                if (tr->type != model::StripType::Channel)
                    return Error("rack.home", *a.track + " is not a channel", "/track",
                                 "a rack's Out feeds a channel: pick a channel or leave out track");
                track = *id;
            } else {
                model::Track tr;
                tr.name = a.name.value_or(model::nextTrackName(s, model::StripType::Channel));
                tr.parent = model::kMaster;
                track = t.create(tr);
                t.modify<model::Track>(model::kMaster,
                                       [&](model::Track& m) { m.children.push_back(track); });
            }
            const Id rack = t.create(model::Rack{track, a.track ? a.name.value_or("") : ""});
            model::RackModule out;
            out.rack = rack;
            out.type = std::string(model::kOutModule);
            out.version = "1.0.0";
            out.x = 640;
            out.y = 40;
            const Id outId = t.create(out);
            const model::Track& home = *t.get<model::Track>(track);
            if (home.record.input && t.get<model::Route>(*home.record.input)) {
                t.modify<model::Route>(*home.record.input, [&](model::Route& r) {
                    r.from = model::RouteEnd{};
                    r.from.module = outId;
                    r.midi.reset();
                });
            } else {
                model::Route r;
                r.from.module = outId;
                r.to.track = track;
                const Id route = t.create(r);
                t.modify<model::Track>(track, [&](model::Track& x) { x.record.input = route; });
            }
            json::Value out_ = json::Value::object();
            out_["rack"] = rack.str();
            out_["track"] = track.str();
            out_["out"] = outId.str();
            return out_;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.remove
    [&] {
        Command<RackRef, json::Value> c;
        common(c, "rack.remove", Kind::Edit, "Remove a rack with its modules and cables.");
        c.description =
            "Channels that took one of its Out modules as input lose it; sends into its In "
            "modules go; plugin modules' plug-ins go with them.";
        c.args = schema::object<RackRef>("rack_ref")
                     .field("rack", &RackRef::rack, idOf("rk", "Rack"))
                     .build();
        c.examples = {{"Remove the rack", j(R"({"rack": "rk_1"})"), j("{}"), "song_with_rack"}};
        c.run = [](const RackRef& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            auto id = rackId(s, a.rack);
            if (!id) return std::move(id).error();
            model::EditTxn& t = app(ctx).txn();
            for (const auto& m : s.modules)
                if (m.value->rack == *id) removeModule(t, s, m.id);
            t.remove<model::Rack>(*id);
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.set
    [&] {
        Command<RackSet, json::Value> c;
        common(c, "rack.set", Kind::Edit, "Rename a rack or move it to another channel.");
        c.description =
            "name \"\" shows the home channel's name. track moves its home (its Out modules "
            "keep feeding the channels they feed; Notes modules without a source follow the "
            "new home's MIDI).";
        c.args = schema::object<RackSet>("rack_set_args")
                     .field("rack", &RackSet::rack, idOf("rk", "Rack"))
                     .field("name", &RackSet::name, schema::string())
                     .field("track", &RackSet::track, idOf("tr", "Channel"))
                     .build();
        c.examples = {
            {"Name it", j(R"({"rack": "rk_1", "name": "Bells"})"), j("{}"), "song_with_rack"}};
        c.run = [](const RackSet& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            auto id = rackId(s, a.rack);
            if (!id) return std::move(id).error();
            std::optional<Id> track;
            if (a.track) {
                auto t = Id::parse(*a.track);
                const model::Track* tr = t ? s.tracks.find(*t) : nullptr;
                if (!tr || tr->type != model::StripType::Channel)
                    return Error("rack.home", *a.track + " is not a channel", "/track",
                                 "a rack lives on a channel (track.list)");
                track = *t;
            }
            app(ctx).txn().modify<model::Rack>(*id, [&](model::Rack& r) {
                if (a.name) r.name = *a.name;
                if (track) r.track = *track;
            });
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.list
    [&] {
        Command<cmd::NoArgs, json::Value> c;
        common(c, "rack.list", Kind::Query, "List racks: home channels, module counts, Outs.");
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        c.examples = {{"One rack", j("{}"),
                       j(R"({"racks": [{"rack": "rk_1", "name": "Bass", "track": "tr_1",
                             "modules": 3, "cables": 1, "outs": [{"module": "mo_1",
                             "label": "Out 1", "channels": ["tr_1"]}]}]})"),
                       "song_with_rack"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            json::Value racks = json::Value::array();
            for (const auto& r : s.racks) {
                json::Value e = json::Value::object();
                e["rack"] = r.id.str();
                e["name"] = model::rackLabel(s, r.id);
                e["track"] = r.value->track.str();
                std::int64_t modules = 0, cables = 0;
                for (const auto& m : s.modules) modules += m.value->rack == r.id;
                for (const auto& cb : s.cables)
                    cables += model::rackOfEnd(s, cb.value->from) == r.id;
                e["modules"] = modules;
                e["cables"] = cables;
                e["outs"] = outsOf(s, r.id);
                racks.push_back(std::move(e));
            }
            json::Value out = json::Value::object();
            out["racks"] = std::move(racks);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.get
    [&] {
        Command<RackGet, json::Value> c;
        common(c, "rack.get", Kind::Query, "A rack's modules, cables, Out modules and problems.");
        c.description =
            "Modules with their type, label, place and size and the knobs that differ from "
            "their defaults (detail full: also their input, output and knob keys); cables with "
            "feedback (true: it closes a loop and carries the previous frame); Out modules and "
            "the channels they feed; problems (missing types, cables that do not fit).";
        c.args = schema::object<RackGet>("rack_get_args")
                     .field("rack", &RackGet::rack, idOf("rk", "Rack"))
                     .field("detail", &RackGet::detail,
                            schema::string().pattern("^(brief|full)$", "brief or full"))
                     .build();
        c.examples = {{"What the rack holds", j(R"({"rack": "rk_1"})"), j(R"({"rack": "rk_1",
                         "name": "Bass", "track": "tr_1", "modules": "<any>",
                         "cables": [{"cable": "cb_1", "from": "mo_3.out", "to": "mo_1.l",
                         "feedback": false}], "outs": "<any>", "problems": []})"),
                       "song_with_rack"}};
        c.run = [](const RackGet& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            auto id = rackId(s, a.rack);
            if (!id) return std::move(id).error();
            const bool full = a.detail && *a.detail == "full";
            json::Value out = json::Value::object();
            out["rack"] = id->str();
            out["name"] = model::rackLabel(s, *id);
            out["track"] = s.racks.find(*id)->track.str();
            json::Value modules = json::Value::array(), problems = json::Value::array();
            for (const auto& m : s.modules) {
                if (m.value->rack != *id) continue;
                modules.push_back(moduleEntry(e, s, m.id, full));
                if (!typeOfModule(e, *m.value))
                    problems.push_back(m.id.str() + ": type " + m.value->type +
                                       " is not installed");
            }
            const auto feedback = feedbackOf(s, *id);
            json::Value cables = json::Value::array();
            for (const auto& cb : s.cables) {
                if (model::rackOfEnd(s, cb.value->from) != *id) continue;
                json::Value x = json::Value::object();
                x["cable"] = cb.id.str();
                x["from"] = cb.value->from.str();
                x["to"] = cb.value->to.str();
                if (cb.value->amount) x["amount"] = *cb.value->amount;
                if (!cb.value->color.empty()) x["color"] = cb.value->color;
                auto fb = feedback.find(cb.id);
                x["feedback"] = fb != feedback.end() && fb->second;
                cables.push_back(std::move(x));
            }
            out["modules"] = std::move(modules);
            out["cables"] = std::move(cables);
            out["outs"] = outsOf(s, *id);
            out["problems"] = std::move(problems);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.list
    [&] {
        Command<ModuleList, json::Value> c;
        common(c, "module.list", Kind::Query, "List module types: id, name, tags, what they do.");
        c.description =
            "Host modules (Out, In, Notes, Transport) first, then stock, the user's library and "
            "the project's. query matches names, ids, tags and descriptions; tag one tag.";
        c.args = schema::object<ModuleList>("module_list_args")
                     .field("query", &ModuleList::query, schema::string())
                     .field("tag", &ModuleList::tag, schema::string())
                     .build();
        c.examples = {{"Amplifiers", j(R"({"tag": "amplifier"})"),
                       j(R"({"types": [{"type": "com.suppleaudio.rack.vca", "name": "VCA",
                             "tags": ["amplifier", "utility"], "origin": "stock",
                             "description": "<any>"}]})"),
                       "empty"}};
        c.run = [](const ModuleList& a, Context& ctx) -> Result<json::Value> {
            auto lower = [](std::string x) {
                std::transform(x.begin(), x.end(), x.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return x;
            };
            const std::string q = lower(a.query.value_or(""));
            json::Value types = json::Value::array();
            for (const auto* entry : catalog(app(ctx)).list()) {
                const rack::ModuleType& t = *entry->type;
                json::Value tags = json::Value::array();
                std::string text = lower(t.info().name + " " + t.id() + " " + t.info().description);
                bool tagged = !a.tag;
                for (const auto& tag : t.info().tags) {
                    if (tag.empty()) continue;
                    tags.push_back(tag);
                    text += " " + lower(tag);
                    tagged |= a.tag && lower(*a.tag) == lower(tag);
                }
                if (!tagged || (!q.empty() && text.find(q) == std::string::npos)) continue;
                json::Value e = json::Value::object();
                e["type"] = t.id();
                e["name"] = t.info().name;
                e["tags"] = std::move(tags);
                e["origin"] = std::string(rack::originName(t.origin()));
                e["description"] = t.info().description;
                types.push_back(std::move(e));
            }
            json::Value out = json::Value::object();
            out["types"] = std::move(types);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.describe
    [&] {
        Command<ModuleDescribe, json::Value> c;
        common(c, "module.describe", Kind::Query,
               "A module type's knobs, inputs and outputs, with units and help.");
        c.description =
            "type (an id or a short name, \"vca\") or module (a module's type). Knobs: key, "
            "name, kind (number, integer, choice, toggle), unit, min, max, default, choices "
            "(keys), group, help. Ports: key, name, signal (audio, pitch in octaves from C4, "
            "gate, cv -1..1, unipolar 0..1, phase 0..1), help, and an input's normal (its value "
            "with nothing connected). A cable into a knob moves it by amount per unit of "
            "signal: octaves for a frequency, the knob's own unit otherwise.";
        c.args = schema::object<ModuleDescribe>("module_describe_args")
                     .field("type", &ModuleDescribe::type, schema::string())
                     .field("module", &ModuleDescribe::module, idOf("mo", "Module"))
                     .build();
        c.examples = {{"The VCA", j(R"({"type": "vca"})"),
                       j(R"({"type": "com.suppleaudio.rack.vca", "name": "VCA", "origin": "stock",
                             "vendor": "Supple Audio", "version": "1.0.0", "description": "<any>",
                             "tags": ["amplifier", "utility"], "width": "<any>", "height": "<any>",
                             "knobs": "<any>", "inputs": "<any>", "outputs": "<any>"})"),
                       "empty"}};
        c.run = [](const ModuleDescribe& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            std::shared_ptr<const rack::ModuleType> type;
            if (a.module) {
                const model::Session s = e.view();
                auto id = moduleId(s, *a.module);
                if (!id) return std::move(id).error();
                type = typeOfModule(e, *s.modules.find(*id));
                if (!type)
                    return Error("rack.unknown_type",
                                 "the type of " + *a.module + " is not installed", "/module",
                                 "module.list shows what is");
            } else if (a.type) {
                auto t = typeOf(e, *a.type, "/type");
                if (!t) return std::move(t).error();
                type = *t;
            } else {
                return Error("module.describe", "give type or module", "",
                             "e.g. {\"type\": \"vca\"}");
            }
            return describeModuleType(*type);
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.add
    [&] {
        Command<ModuleAdd, json::Value> c;
        common(c, "module.add", Kind::Edit, "Add a module to a rack: a type with knob values.");
        c.description =
            "type: an id or a short name (\"vca\", \"notes\"; module.list). x, y: its top-left "
            "corner in the rack (default: right of the others). params: knob values by key in "
            "the knob's units (a choice by its key, a switch as a bool). Notes: source (the "
            "track whose MIDI it plays; default the rack's channel) and device (a live MIDI "
            "input it always plays; \"\" any). plugin (instead of type): a plug-in id "
            "(fx.scan) for a plugin module, which shows the plug-in's name and opens its "
            "editor; its ports are in_l, in_r, out_l, out_r and, for instruments, pitch, gate "
            "and velocity. preset: a factory preset (module.describe), params on top. Returns "
            "its size and keys.";
        c.args = schema::object<ModuleAdd>("module_add_args")
                     .field("rack", &ModuleAdd::rack, idOf("rk", "Rack"))
                     .field("type", &ModuleAdd::type, schema::string())
                     .field("label", &ModuleAdd::label, schema::string())
                     .field("preset", &ModuleAdd::preset, schema::string().minLength(1))
                     .field("x", &ModuleAdd::x, coordinate())
                     .field("y", &ModuleAdd::y, coordinate())
                     .field("params", &ModuleAdd::params, schema::any())
                     .field("source", &ModuleAdd::source, idOf("tr", "Track"))
                     .field("device", &ModuleAdd::device, schema::string())
                     .field("plugin", &ModuleAdd::plugin, schema::string())
                     .build();
        c.examples = {
            {"A VCA, half open", j(R"({"rack": "rk_1", "type": "vca", "params": {"level": -6}})"),
             j(R"({"module": "mo_4", "type": "com.suppleaudio.rack.vca", "x": "<any>",
                             "y": 40.0, "width": "<any>", "height": "<any>",
                             "inputs": ["in", "cv"], "outputs": ["out"],
                             "knobs": ["level", "response"]})"),
             "song_with_rack"},
            {"An instrument plug-in, played by cables from Notes",
             j(R"({"rack": "rk_1", "plugin": "com.suppleaudio.test.sine"})"),
             j(R"({"module": "mo_4", "type": "com.suppleaudio.rack.plugin",
                             "x": "<any>", "y": "<any>", "width": "<any>", "height": "<any>",
                             "inputs": ["in_l", "in_r", "pitch", "gate", "velocity"],
                             "outputs": ["out_l", "out_r"], "knobs": [], "fx": "fx_1",
                             "plugin": "Test Sine"})"),
             "song_with_rack"}};
        c.errors = {{"rack.unknown_type", "no such module type", "module.list"},
                    {"rack.param", "a knob or value the type does not have", "module.describe"},
                    {"rack.limit", "too many modules", "remove modules first"}};
        c.run = [](const ModuleAdd& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            auto rack = rackId(s, a.rack);
            if (!rack) return std::move(rack).error();
            std::size_t count = 0, outs = 0, ins = 0;
            for (const auto& m : s.modules)
                if (m.value->rack == *rack) {
                    ++count;
                    outs += m.value->type == model::kOutModule;
                    ins += m.value->type == model::kInModule;
                }
            if (count >= model::kMaxRackModules)
                return Error("rack.limit", "a rack holds at most 512 modules", "/rack",
                             "remove modules (module.remove) or use another rack");
            model::RackModule m;
            m.rack = *rack;
            std::shared_ptr<const rack::ModuleType> type;
            // A plugin module: plugin, with no type or the plugin type.
            const bool pluginType =
                a.type && (*a.type == "plugin" || *a.type == model::kPluginModule);
            if (a.plugin ? a.type && !pluginType : !a.type || pluginType)
                return Error("command.invalid_args",
                             a.plugin ? "give a type or a plugin, not both"
                                      : "a module needs a type, or a plug-in to host",
                             a.plugin || !a.type ? "/type" : "/plugin",
                             "give type (module.list) for a module, or plugin (an fx.scan id) "
                             "for a plug-in");
            if (a.plugin) {
                m.type = std::string(model::kPluginModule);
            } else {
                auto t = typeOf(e, *a.type, "/type");
                if (!t) return std::move(t).error();
                type = *t;
                m.type = type->id();
                m.version = type->info().version;
            }
            if (m.type == model::kOutModule && outs >= model::kMaxRackOuts)
                return Error("rack.limit", "a rack holds at most 32 Out modules", "/type",
                             "mix into fewer Outs (a Mixer module)");
            if (m.type == model::kInModule && ins >= model::kMaxRackIns)
                return Error("rack.limit", "a rack holds at most 32 In modules", "/type",
                             "send more tracks into one In");
            if ((a.source || a.device) && m.type != model::kNotesModule)
                return Error("rack.param", "source and device are a Notes module's", "/source",
                             "add a Notes module (type notes) to play a track's MIDI");
            if (a.source) {
                auto src = Id::parse(*a.source);
                if (!src || !s.tracks.contains(*src))
                    return Error("track.unknown", "no track " + *a.source, "/source",
                                 "give the track whose MIDI it plays (track.list)");
                m.source = *src;
            }
            m.device = a.device;
            if (a.label) m.label = *a.label;
            if (a.params || a.preset) {
                if (!type)
                    return Error("rack.param", "a plugin module's knobs are its plug-in's",
                                 a.params ? "/params" : "/preset",
                                 "set them with fx.set, or fx.preset.load, on its fx");
                json::Value base = json::Value::object();
                if (a.preset) {
                    auto preset = presetParams(*type, *a.preset, "/preset");
                    if (!preset) return std::move(preset).error();
                    base = std::move(*preset);
                }
                auto params = a.params ? mergeParams(*type, base, *a.params, "/params")
                                       : Result<json::Value>(std::move(base));
                if (!params) return std::move(params).error();
                m.params = std::move(*params);
            }
            const auto place = freePlace(e, s, *rack);
            m.x = a.x.value_or(place.first);
            m.y = a.y.value_or(place.second);
            model::EditTxn& t = e.txn();
            if (a.plugin) {
                model::FxInstance fx;
                fx.plugin.id = *a.plugin;
                auto info = e.findPlugin(*a.plugin);
                if (!info)
                    return Error("fx.unknown_plugin", "no plug-in " + *a.plugin, "/plugin",
                                 "give a plug-in id from fx.scan");
                fx.plugin.name = info->name;
                fx.plugin.vendor = info->vendor;
                fx.plugin.version = info->version;
                m.fx = t.create(fx);
                m.version = "1.0.0";
            }
            const Id id = t.create(m);
            json::Value out = moduleEntry(e, t.preview(), id, true);
            json::Value result = json::Value::object();
            for (const char* k : {"module", "type", "x", "y", "width", "height", "inputs",
                                  "outputs", "knobs", "fx", "plugin", "problem"})
                if (const auto* v = out.find(k)) result[k] = *v;
            return result;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.remove
    [&] {
        Command<ModuleRef, json::Value> c;
        common(c, "module.remove", Kind::Edit, "Remove a module with its cables.");
        c.description =
            "Also its knob envelopes; an Out module's channels lose it as input; an In "
            "module's sends go; a plugin module's plug-in goes.";
        c.args = schema::object<ModuleRef>("module_ref")
                     .field("module", &ModuleRef::module, idOf("mo", "Module"))
                     .build();
        c.examples = {{"Remove the VCA", j(R"({"module": "mo_3"})"), j("{}"), "song_with_rack"}};
        c.run = [](const ModuleRef& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            auto id = moduleId(s, a.module);
            if (!id) return std::move(id).error();
            removeModule(app(ctx).txn(), s, *id);
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.set
    [&] {
        Command<ModuleSet, json::Value> c;
        common(c, "module.set", Kind::Edit, "Set a module's knobs, label, place or bypass.");
        c.description =
            "params merge into its knobs (values in the knob's units; a choice by key; a "
            "default value is dropped from the file). A knob edit is heard at once, without a "
            "recompile. bypass passes each input to the output of its kind. label names it "
            "(an Out module's label is what channels' input menus list). preset: a factory "
            "preset by name (module.describe lists them): every knob to it, then params on top.";
        c.args = schema::object<ModuleSet>("module_set_args")
                     .field("module", &ModuleSet::module, idOf("mo", "Module"))
                     .field("params", &ModuleSet::params, schema::any())
                     .field("label", &ModuleSet::label, schema::string())
                     .field("preset", &ModuleSet::preset, schema::string().minLength(1))
                     .field("x", &ModuleSet::x, coordinate())
                     .field("y", &ModuleSet::y, coordinate())
                     .field("bypass", &ModuleSet::bypass, schema::boolean())
                     .field("source", &ModuleSet::source, idOf("tr", "Track"))
                     .field("device", &ModuleSet::device, schema::string())
                     .build();
        c.examples = {{"Turn the VCA down", j(R"({"module": "mo_3", "params": {"level": -12}})"),
                       j("{}"), "song_with_rack"},
                      {"Name the Out", j(R"({"module": "mo_1", "label": "Bells"})"), j("{}"),
                       "song_with_rack"}};
        c.run = [](const ModuleSet& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            auto id = moduleId(s, a.module);
            if (!id) return std::move(id).error();
            model::RackModule m = *s.modules.find(*id);
            if (a.params || a.preset) {
                const char* path = a.params ? "/params" : "/preset";
                const auto type = m.fx ? nullptr : typeOfModule(e, m);
                if (!type)
                    return Error(
                        "rack.param",
                        m.fx ? "a plugin module's knobs are its plug-in's"
                             : "its type " + m.type + " is not installed",
                        path,
                        m.fx ? "use fx.set or fx.preset.load on " + m.fx->str() : "module.list");
                json::Value base = m.params;
                if (a.preset) {  // every knob: the preset's, else its default
                    auto preset = presetParams(*type, *a.preset, "/preset");
                    if (!preset) return std::move(preset).error();
                    base = std::move(*preset);
                }
                auto params = a.params ? mergeParams(*type, base, *a.params, "/params")
                                       : Result<json::Value>(std::move(base));
                if (!params) return std::move(params).error();
                m.params = std::move(*params);
            }
            if ((a.source || a.device) && m.type != model::kNotesModule)
                return Error("rack.param", "source and device are a Notes module's", "/source",
                             "only Notes modules play a track's MIDI");
            if (a.source) {
                auto src = Id::parse(*a.source);
                if (!src || !s.tracks.contains(*src))
                    return Error("track.unknown", "no track " + *a.source, "/source", "track.list");
                m.source = *src;
            }
            if (a.device) m.device = *a.device;
            if (a.label) m.label = *a.label;
            if (a.x) m.x = *a.x;
            if (a.y) m.y = *a.y;
            if (a.bypass) m.bypass = *a.bypass;
            e.txn().put<model::RackModule>(*id, std::move(m));
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.duplicate
    [&] {
        Command<ModuleDuplicate, json::Value> c;
        common(c, "module.duplicate", Kind::Edit,
               "Duplicate modules, with the cables between them.");
        c.description =
            "The copies sit dx, dy from the originals (default 0, 40 below the lowest). "
            "cables (default true): cables among the given modules are copied too. Plugin "
            "modules copy their plug-in with its state.";
        c.args =
            schema::object<ModuleDuplicate>("module_duplicate_args")
                .field("modules", &ModuleDuplicate::modules, schema::arrayOf(idOf("mo", "Module")))
                .field("dx", &ModuleDuplicate::dx, coordinate())
                .field("dy", &ModuleDuplicate::dy, coordinate())
                .field("cables", &ModuleDuplicate::cables, schema::boolean())
                .build();
        c.examples = {{"Copy the VCA", j(R"({"modules": ["mo_3"]})"),
                       j(R"({"modules": ["mo_4"], "cables": []})"), "song_with_rack"}};
        c.run = [](const ModuleDuplicate& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            std::vector<Id> ids;
            for (std::size_t k = 0; k < a.modules.size(); ++k) {
                const std::string path = "/modules/" + std::to_string(k);
                auto id = moduleId(s, a.modules[k], path.c_str());
                if (!id) return std::move(id).error();
                if (s.modules.find(*id)->type == model::kOutModule ||
                    s.modules.find(*id)->type == model::kInModule) {
                    // allowed: a second Out or In is a new one
                }
                ids.push_back(*id);
            }
            if (ids.empty()) return Error("module.duplicate", "give modules", "/modules", "");
            double low = -1e300;
            for (Id id : ids) {
                const auto& m = *s.modules.find(id);
                low = std::max(low, m.y + sizeOf(typeOfModule(e, m).get()).second);
            }
            const double dx = a.dx.value_or(0.0);
            double dy = 0;
            if (a.dy)
                dy = *a.dy;
            else {
                double top = 1e300;
                for (Id id : ids) top = std::min(top, s.modules.find(id)->y);
                dy = low - top + 40;
            }
            model::EditTxn& t = e.txn();
            std::map<Id, Id> copies;  // modules and their plug-ins (cables reach both)
            json::Value made = json::Value::array(), cables = json::Value::array();
            for (Id id : ids) {
                model::RackModule m = *s.modules.find(id);
                m.x = std::clamp(m.x + dx, -model::kMaxRackCoordinate, model::kMaxRackCoordinate);
                m.y = std::clamp(m.y + dy, -model::kMaxRackCoordinate, model::kMaxRackCoordinate);
                if (m.fx) {
                    const Id original = *m.fx;
                    m.fx = t.create(*s.fx.find(original));
                    copies[original] = *m.fx;
                }
                if (!m.label.empty() && m.type == model::kOutModule) m.label += " copy";
                copies[id] = t.create(m);
                made.push_back(copies[id].str());
            }
            if (a.cables.value_or(true))
                for (const auto& cb : s.cables) {
                    auto from = copies.find(cb.value->from.object);
                    auto to = copies.find(cb.value->to.object);
                    if (from == copies.end() || to == copies.end()) continue;
                    model::Cable copy = *cb.value;
                    copy.from.object = from->second;
                    copy.to.object = to->second;
                    cables.push_back(t.create(copy).str());
                }
            json::Value out = json::Value::object();
            out["modules"] = std::move(made);
            out["cables"] = std::move(cables);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- module.randomize
    [&] {
        Command<ModuleRandomize, json::Value> c;
        common(c, "module.randomize", Kind::Edit, "Move a module's knobs at random, by an amount.");
        c.description =
            "amount 0..1 (default 0.25): how far each knob may move, as a share of its range "
            "(choices change with that probability); switches stay. seed makes it repeatable. "
            "Returns the knobs it set.";
        c.args = schema::object<ModuleRandomize>("module_randomize_args")
                     .field("module", &ModuleRandomize::module, idOf("mo", "Module"))
                     .field("amount", &ModuleRandomize::amount, schema::number().range(0, 1))
                     .field("seed", &ModuleRandomize::seed, schema::integer())
                     .build();
        c.examples = {{"Nudge the VCA", j(R"({"module": "mo_3", "seed": 7})"),
                       j(R"({"params": "<any>"})"), "song_with_rack"}};
        c.run = [](const ModuleRandomize& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            auto id = moduleId(s, a.module);
            if (!id) return std::move(id).error();
            const model::RackModule& m = *s.modules.find(*id);
            const auto type = typeOfModule(e, m);
            if (!type)
                return Error("rack.param", "its knobs are not known", "/module",
                             m.fx ? "a plugin module's knobs are its plug-in's" : "module.list");
            const double amount = a.amount.value_or(0.25);
            std::mt19937_64 rng(static_cast<std::uint64_t>(
                a.seed.value_or(static_cast<std::int64_t>(s.version * 1315423911u + id->n))));
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            json::Value given = json::Value::object();
            for (const auto& p : type->params()) {
                if (p.output || p.kind == sd::ParamModel::Kind::Toggle) continue;
                const double now = detail::moduleParamValue(p, m.params.find(p.key));
                if (p.kind == sd::ParamModel::Kind::Choice) {
                    if (unit(rng) < amount && p.choices.size() > 1) {
                        const auto k =
                            static_cast<std::size_t>(unit(rng) * double(p.choices.size()));
                        given[p.key] = sd::choiceKey(p, std::min(k, p.choices.size() - 1));
                    }
                    continue;
                }
                double v = now + (unit(rng) * 2 - 1) * amount * (p.max - p.min);
                v = std::clamp(v, p.min, p.max);
                if (p.kind == sd::ParamModel::Kind::Integer) v = std::round(v);
                given[p.key] = v;
            }
            auto params = mergeParams(*type, m.params, given, "/params");
            if (!params) return std::move(params).error();
            e.txn().modify<model::RackModule>(*id,
                                              [&](model::RackModule& x) { x.params = *params; });
            json::Value out = json::Value::object();
            out["params"] = *params;
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- cable.add
    [&] {
        Command<CableAdd, json::Value> c;
        common(c, "cable.add", Kind::Edit, "Connect an output to an input or a knob.");
        c.description =
            "from: \"mo_N.<output>\"; to: \"mo_N.<input>\" or \"mo_N.<knob>\" (a modulation "
            "cable: amount is the knob's movement at a signal of 1, in octaves for a frequency, "
            "the knob's unit otherwise; default an octave or half the range). Several cables "
            "into one input sum; a mono cable reaches every voice of a poly module. feedback: "
            "it closes a loop, so it carries the previous frame (a one-sample delay).";
        c.args = schema::object<CableAdd>("cable_add_args")
                     .field("from", &CableAdd::from, endSchema())
                     .field("to", &CableAdd::to, endSchema())
                     .field("amount", &CableAdd::amount, schema::number())
                     .field("color", &CableAdd::color, schema::string())
                     .build();
        c.examples = {
            {"Notes' gate opens the VCA", j(R"({"from": "mo_2.gate", "to": "mo_3.cv"})"),
             j(R"({"cable": "cb_2", "feedback": false})"), "song_with_rack"},
            {"Modulate a knob", j(R"({"from": "mo_2.modwheel", "to": "mo_3.level", "amount": 12})"),
             j(R"({"cable": "cb_2", "feedback": false})"), "song_with_rack"}};
        c.errors = {{"rack.port", "no such output, input or knob", "rack.get detail full"},
                    {"cable.exists", "the two ends are joined already", "cable.set"},
                    {"cable.rack", "the ends are in different racks", "use In and Out modules"}};
        c.run = [](const CableAdd& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            SUPPLE_TRY(checkColor(a.color, "/color"));
            auto from = model::CableEnd::parse(a.from);
            auto to = model::CableEnd::parse(a.to);
            if (!from || from->object.kind != model::Kind::Module)
                return Error("rack.port", a.from + " is not a module's output", "/from",
                             "e.g. \"mo_3.out\"");
            if (!to)
                return Error("rack.port", a.to + " is not a module's input or knob", "/to",
                             "e.g. \"mo_4.in\" or \"mo_4.cutoff\"");
            auto fromId = moduleId(s, from->object.str(), "/from");
            if (!fromId) return std::move(fromId).error();
            const model::RackModule& src = *s.modules.find(*fromId);
            const auto srcType = typeOfModule(e, src);
            if (srcType && srcType->output(from->key) < 0)
                return Error(
                    "rack.port", from->object.str() + " has no output " + from->key, "/from",
                    from->object.str() + " has outputs " + keysOf(*srcType, false, true, false));
            model::Cable cable;
            cable.from = *from;
            cable.to = *to;
            if (a.color) cable.color = *a.color;
            if (to->object.kind == model::Kind::Fx) {
                const auto owner = model::moduleOfFx(s, to->object);
                if (!owner)
                    return Error("rack.port", to->object.str() + " is not a plugin module's", "/to",
                                 "cables reach the plug-ins of plugin modules");
                cable.amount = a.amount.value_or(0.5);
            } else {
                auto toId = moduleId(s, to->object.str(), "/to");
                if (!toId) return std::move(toId).error();
                const auto dstType = typeOfModule(e, *s.modules.find(*toId));
                if (dstType) {
                    if (dstType->input(to->key) >= 0) {
                        if (a.amount)
                            return Error(
                                "rack.port", "amount is a modulation cable's", "/amount",
                                to->key + " is an input; leave out amount, or reach a knob");
                    } else if (const int p = dstType->param(to->key); p >= 0) {
                        const sd::ParamModel& pm = dstType->params()[static_cast<std::size_t>(p)];
                        if (pm.kind == sd::ParamModel::Kind::Toggle || pm.output)
                            return Error(
                                "rack.port", to->key + " is a switch or a meter", "/to",
                                "cables move knobs: " + keysOf(*dstType, false, false, true));
                        cable.amount = a.amount.value_or(defaultAmount(pm));
                    } else {
                        return Error("rack.port",
                                     to->object.str() + " has no input or knob " + to->key, "/to",
                                     to->object.str() + " has inputs " +
                                         keysOf(*dstType, true, false, false) + "; knobs " +
                                         keysOf(*dstType, false, false, true));
                    }
                } else if (a.amount) {
                    cable.amount = *a.amount;
                }
            }
            if (model::rackOfEnd(s, cable.from) != model::rackOfEnd(s, cable.to))
                return Error("cable.rack", "the ends are in different racks", "/to",
                             "racks meet through tracks: an Out module feeds a channel, a send "
                             "reaches an In module");
            for (const auto& cb : s.cables)
                if (cb.value->from == cable.from && cb.value->to == cable.to)
                    return Error("cable.exists", cb.id.str() + " joins them already", "/to",
                                 "change it with cable.set {cable: \"" + cb.id.str() + "\"}");
            std::size_t inRack = 0;
            const auto rack = model::rackOfEnd(s, cable.from);
            for (const auto& cb : s.cables) inRack += model::rackOfEnd(s, cb.value->from) == rack;
            if (inRack >= model::kMaxRackCables)
                return Error("rack.limit", "a rack holds at most 4096 cables", "/from",
                             "remove cables first");
            const Id id = e.txn().create(cable);
            const auto preview = e.txn().preview();
            const auto feedback = feedbackOf(preview, *rack);
            json::Value out = json::Value::object();
            out["cable"] = id.str();
            out["feedback"] = feedback.count(id) && feedback.at(id);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- cable.remove
    [&] {
        Command<CableRemove, json::Value> c;
        common(c, "cable.remove", Kind::Edit, "Remove a cable, by id or by its two ends.");
        c.args = schema::object<CableRemove>("cable_remove_args")
                     .field("cable", &CableRemove::cable, idOf("cb", "Cable"))
                     .field("from", &CableRemove::from, endSchema())
                     .field("to", &CableRemove::to, endSchema())
                     .build();
        c.examples = {{"Unplug", j(R"({"cable": "cb_1"})"), j("{}"), "song_with_rack"},
                      {"Unplug by ends", j(R"({"from": "mo_3.out", "to": "mo_1.l"})"), j("{}"),
                       "song_with_rack"}};
        c.run = [](const CableRemove& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            std::optional<Id> id;
            if (a.cable) {
                auto c = cableId(s, *a.cable);
                if (!c) return std::move(c).error();
                id = *c;
            } else if (a.from && a.to) {
                for (const auto& cb : s.cables)
                    if (cb.value->from.str() == *a.from && cb.value->to.str() == *a.to) id = cb.id;
                if (!id)
                    return Error("cable.unknown", "no cable joins " + *a.from + " and " + *a.to,
                                 "/to", "rack.get lists the rack's cables");
            } else {
                return Error("cable.remove", "give cable, or from and to", "",
                             "e.g. {\"cable\": \"cb_1\"}");
            }
            app(ctx).txn().remove<model::Cable>(*id);
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- cable.set
    [&] {
        Command<CableSet, json::Value> c;
        common(c, "cable.set", Kind::Edit, "Set a modulation cable's amount or a cable's colour.");
        c.args = schema::object<CableSet>("cable_set_args")
                     .field("cable", &CableSet::cable, idOf("cb", "Cable"))
                     .field("amount", &CableSet::amount, schema::number())
                     .field("color", &CableSet::color, schema::string())
                     .build();
        c.examples = {
            {"Colour it", j(R"({"cable": "cb_1", "color": "violet"})"), j("{}"), "song_with_rack"}};
        c.run = [](const CableSet& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            auto id = cableId(s, a.cable);
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkColor(a.color, "/color"));
            model::Cable cable = *s.cables.find(*id);
            if (a.amount) {
                if (!cable.amount)
                    return Error("rack.port", "amount is a modulation cable's", "/amount",
                                 a.cable + " reaches an input, not a knob");
                cable.amount = *a.amount;
            }
            if (a.color) cable.color = *a.color;
            app(ctx).txn().put<model::Cable>(*id, std::move(cable));
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
