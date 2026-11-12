// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Modular racks' commands (ADR-0062, rack.md §7): racks, modules and cables.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <set>

#include "command_validation.h"
#include "fx_presets.h"
#include "learn_binding.h"
#include "module_library.h"
#include "rack_runtime.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/engine/racks.h"
#include "supple/model/schemas.h"
#include "supple/rack/stock.h"

namespace supple::engine {

void registerRackPresetCommands(cmd::Registry& registry);
// The module library's (module_commands.cpp).
Result<json::Value> saveLibraryModule(EngineApp& e, const std::string& name,
                                      const std::string& source, const char* path);
std::string libraryNameFor(const std::string& id);
void registerModuleLearnCommands(cmd::Registry& registry);
void registerRackPatchCommand(cmd::Registry& registry);

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
auto notesSourceSchema() {
    return schema::Schema<std::string>(
        "notes_source",
        [](const json::Value& value, std::string& out, schema::DecodeCtx& ctx,
           const std::string& path) {
            const auto id = value.isString() ? Id::parse(value.asString()) : std::nullopt;
            if (!value.isString() ||
                (value.asString() != "none" && (!id || id->kind != model::Kind::Track))) {
                ctx.error("schema.type", "expected a Notes source track or none", path,
                          "use tr_N or none; null restores Home in module.set");
                return false;
            }
            out = value.asString();
            return true;
        },
        [](const std::string& value) { return json::Value(value); },
        j(R"({"oneOf":[{"type":"string","pattern":"^tr_[0-9]+$"},{"type":"string","enum":["none"]}]})"));
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
    if (name == "notes_out") return c.find(model::kNotesOutModule);
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
            if (!p.output && !p.hidden) add(p.key);
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
        if (index < 0 || t.params()[static_cast<std::size_t>(index)].output ||
            t.params()[static_cast<std::size_t>(index)].hidden)
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
        if (p.output || p.hidden) continue;
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

// The built-in types as Markdown (racks.h), from describeModuleType.
std::string moduleReference() {
    auto text = [](const json::Value& v) {
        return v.isString() ? v.asString() : json::writeCompact(v);
    };
    auto keyOf = [](const std::string& id) { return id.substr(id.rfind('.') + 1); };
    const auto catalog = detail::builtinModuleCatalog();
    std::vector<std::shared_ptr<const rack::ModuleType>> types;
    for (const auto* e : catalog->list()) types.push_back(e->type);
    std::sort(types.begin(), types.end(), [&](const auto& a, const auto& b) {
        const bool hostA = a->origin() == rack::Origin::Host,
                   hostB = b->origin() == rack::Origin::Host;
        return hostA != hostB ? hostA : keyOf(a->id()) < keyOf(b->id());
    });
    std::string md =
        "# Rack modules\n\n"
        "Generated by `<slug>-engine modules --markdown`; do not edit. Every built-in "
        "module type: the host modules, then the stock ones by key. `module.add` takes the "
        "key (`\"vca\"`) or the id; a cable end is `mo_N.<port>` or `mo_N.<knob>`. Knobs "
        "are in their units: a choice by its key, a switch as true or false. "
        "`module.describe` gives the same for any type, the library's too.\n\n";
    std::map<std::string, std::vector<std::string>> byTag;
    for (const auto& t : types)
        for (const auto& tag : t->info().tags)
            if (!tag.empty()) byTag[tag].push_back(keyOf(t->id()));
    md += "## By tag\n\n";
    for (const auto& [tag, keys] : byTag) {
        md += "- **" + tag + "**:";
        for (std::size_t i = 0; i < keys.size(); ++i) md += (i ? ", " : " ") + keys[i];
        md += "\n";
    }
    for (const auto& t : types) {
        json::Value d = describeModuleType(*t);
        md += "\n## " + keyOf(t->id()) + ": " + t->info().name + "\n\n";
        md += "`" + t->id() + "`";
        if (t->origin() == rack::Origin::Host) md += " (host)";
        md += "\n\n";
        if (!t->info().description.empty()) md += t->info().description + "\n\n";
        for (const char* side : {"inputs", "outputs"}) {
            const auto& ports = d[side].asArray();
            if (ports.empty()) continue;
            md += std::string(side[0] == 'i' ? "- In:\n" : "- Out:\n");
            for (const json::Value& p : ports) {
                md += "  - `" + p.find("key")->asString() + "` " + p.find("signal")->asString();
                if (const auto* n = p.find("normal")) md += " (unpatched " + text(*n) + ")";
                if (const auto* h = p.find("help")) md += ": " + h->asString();
                md += "\n";
            }
        }
        const auto& knobs = d["knobs"].asArray();
        if (!knobs.empty()) {
            md += "- Knobs:\n";
            for (const json::Value& k : knobs) {
                md += "  - `" + k.find("key")->asString() + "` ";
                if (const auto* c = k.find("choices")) {
                    std::string keys;
                    for (const auto& x : c->asArray())
                        keys += (keys.empty() ? "" : "|") + x.asString();
                    md += keys;
                } else if (k.find("kind")->asString() == "toggle") {
                    md += "true|false";
                } else {
                    md += text(*k.find("min")) + "..";
                    md += text(*k.find("max"));
                    if (const auto* u = k.find("unit")) md += " " + u->asString();
                }
                md += " = " + text(*k.find("default"));
                if (const auto* h = k.find("help")) md += ": " + h->asString();
                md += "\n";
            }
        }
        if (const auto* presets = d.find("presets")) {
            md += "- Presets:";
            const auto& names = presets->asArray();
            for (std::size_t i = 0; i < names.size(); ++i)
                md += (i ? ", " : " ") + names[i].asString();
            md += "\n";
        }
    }
    return md;
}

// Exposed to track.remove (commands.cpp): a track's racks go with it, except
// a rack that another channel hears (its Out as the channel's input, else a
// Note Out's MIDI). That rack moves home to the first such channel, keeping
// the name it showed.
void removeRacksOn(model::EditTxn& t, const model::Session& s, Id track) {
    for (const auto& r : s.racks) {
        if (r.value->track != track) continue;
        std::optional<Id> audio, midi;
        for (const auto& e : s.routes) {
            const model::RackModule* from =
                e.value->from.module ? s.modules.find(*e.value->from.module) : nullptr;
            // Only a channel can be home: a buss hears a rack send, not the rack.
            const model::Track* target =
                e.value->to.track ? s.tracks.find(*e.value->to.track) : nullptr;
            if (!from || from->rack != r.id || !target || *e.value->to.track == track ||
                target->type != model::StripType::Channel)
                continue;
            std::optional<Id>& to = e.value->midi ? midi : audio;
            if (!to) to = *e.value->to.track;
        }
        if (const std::optional<Id> home = audio ? audio : midi) {
            const std::string shown = model::rackLabel(s, r.id);
            t.modify<model::Rack>(r.id, [&](model::Rack& rack) {
                rack.track = *home;
                rack.name = shown;
            });
            continue;
        }
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
        auto destination = e.value->to.object;
        if (destination.kind == model::Kind::Fx) {
            auto owner = model::moduleOfFx(s, destination);
            if (!owner) continue;
            destination = *owner;
        }
        auto a = index.find(e.value->from.object), b = index.find(destination);
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
    if (m.sourceOff)
        e["source"] = "none";
    else if (m.source)
        e["source"] = m.source->str();
    if (!m.sourceSends) e["source_sends"] = false;
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
            if (!p.output && !p.hidden) knobs.push_back(p.key);
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
        json::Value feeds = json::Value::array(), sends = json::Value::array();
        for (const auto& r : s.routes)
            if (r.value->from.module == e.id && r.value->to.track) {
                if (model::rackSendHome(s, *r.value)) {  // ADR-0075
                    json::Value send = json::Value::object();
                    send["route"] = r.id.str();
                    send["to"] = r.value->to.track->str();
                    sends.push_back(std::move(send));
                } else {
                    feeds.push_back(r.value->to.track->str());
                }
            }
        o["channels"] = std::move(feeds);
        if (!sends.asArray().empty()) o["sends"] = std::move(sends);
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
    std::optional<bool> bypass, sourceSends;
    std::optional<json::Value> source, device;
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
                tr.record.monitor = e.defaultMonitor();
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
            if (home.record.input)
                if (const auto* previous = t.get<model::Route>(*home.record.input);
                    previous && previous->from.midiDevice)
                    t.modify<model::Track>(track, [&](auto& x) {
                        if (!x.record.midiInput) x.record.midiInput = home.record.input;
                        x.record.input.reset();
                    });
            const auto audioInput = t.get<model::Track>(track)->record.input;
            if (audioInput && t.get<model::Route>(*audioInput)) {
                t.modify<model::Route>(*audioInput, [&](model::Route& r) {
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
        c.description =
            "Each Out lists the channels it is the input of (channels) and, when it has any, "
            "its rack sends to busses and masters (sends: {route, to}, ADR-0075).";
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
            "the channels they feed (sends: their rack sends to busses and masters, ADR-0075, "
            "{route, to}); latency_samples for each Out and the rack node, and "
            "note_outs with their latencies (ADR-0064); problems (missing types, cables that do "
            "not fit). Brief/default latency reads prepared values without creating plug-ins; "
            "unprepared latency is null. detail full may probe unprepared plug-ins.";
        c.args = schema::object<RackGet>("rack_get_args")
                     .field("rack", &RackGet::rack, idOf("rk", "Rack"))
                     .field("detail", &RackGet::detail,
                            schema::string().pattern("^(brief|full)$", "brief or full"))
                     .build();
        c.examples = {{"What the rack holds", j(R"({"rack": "rk_1"})"), j(R"({"rack": "rk_1",
                         "name": "Bass", "track": "tr_1", "modules": "<any>",
                         "cables": [{"cable": "cb_1", "from": "mo_3.out", "to": "mo_1.l",
                         "feedback": false}], "outs": "<any>", "latency_samples": 0,
                         "note_outs": [], "problems": []})"),
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
            std::map<Id, std::int64_t> moduleLatencies;
            auto prepared = e.engine() ? e.engine()->rackLatencies(*id, s.version)
                                       : std::optional<RackLatencyInfo>{};
            bool available = true;
            if (!prepared)
                for (const auto& module : s.modules) {
                    if (module.value->rack != *id || !module.value->fx || module.value->bypass)
                        continue;
                    const auto* fx = s.fx.find(*module.value->fx);
                    if (!fx || fx->offline) continue;
                    if (auto* instance = e.instanceOf(*module.value->fx);
                        instance && instance->active()) {
                        moduleLatencies[module.id] =
                            e.instances().rememberedLatency(*module.value->fx);
                    } else if (full) {
                        auto latency = e.fxLatencySamples(*module.value->fx, *fx, true);
                        if (latency)
                            moduleLatencies[module.id] = *latency;
                        else {
                            available = false;
                            problems.push_back(module.id.str() + ": " + latency.error().toString());
                        }
                    } else
                        available = false;
                }
            const auto estimated = detail::rackLatencies(s, *id, moduleLatencies);
            const auto latencies =
                prepared ? detail::RackLatencies{prepared->latency, prepared->outputs} : estimated;
            out["latency_samples"] = available ? json::Value(latencies.latency) : json::Value();
            for (auto& output : out["outs"].asArray()) {
                const auto module = *Id::parse(output["module"].asString());
                auto found = latencies.outputs.find(module);
                if (found != latencies.outputs.end() && available)
                    output["latency_samples"] = found->second;
                else if (const auto fallback = estimated.outputs.find(module);
                         fallback != estimated.outputs.end() && available)
                    output["latency_samples"] = fallback->second;
                else
                    output["latency_samples"] = json::Value();
            }
            out["note_outs"] = json::Value::array();
            for (const auto& [module, latency] : latencies.outputs)
                if (s.modules.find(module) &&
                    s.modules.find(module)->type == model::kNotesOutModule) {
                    auto output = json::Value::object();
                    output["module"] = module.str();
                    output["latency_samples"] = available ? json::Value(latency) : json::Value();
                    out["note_outs"].push_back(std::move(output));
                }
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
        c.examples = {{"Find the VCA", j(R"({"query": "vca"})"),
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
                     .field("source", &ModuleAdd::source, notesSourceSchema())
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
            m.sourceOff = a.source == "none";
            if (a.source && !m.sourceOff) {
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
            "recompile. Notes source/device accept null to clear them (source returns to the home "
            "track; device to none). source: none disables the track feed while keeping routes and "
            "device. source_sends: false stops it hearing MIDI sends into its source track (its "
            "own sends and device still play). bypass passes each input to the output of its kind. "
            "label "
            "names it "
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
                     .field("source", &ModuleSet::source, clearableString(notesSourceSchema()))
                     .field("device", &ModuleSet::device, clearableString(schema::string()))
                     .field("source_sends", &ModuleSet::sourceSends, schema::boolean())
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
            if ((a.source || a.device || a.sourceSends) && m.type != model::kNotesModule)
                return Error("rack.param", "source, device and source_sends are a Notes module's",
                             a.source   ? "/source"
                             : a.device ? "/device"
                                        : "/source_sends",
                             "only Notes modules play a track's MIDI");
            if (a.sourceSends) m.sourceSends = *a.sourceSends;
            if (a.source) {
                m.source.reset();
                m.sourceOff = a.source->isString() && a.source->asString() == "none";
                if (!a.source->isNull() && !m.sourceOff) {
                    auto src = Id::parse(a.source->asString());
                    if (!src || !s.tracks.contains(*src))
                        return Error("track.unknown", "no track " + a.source->asString(), "/source",
                                     "track.list");
                    m.source = *src;
                }
            }
            if (a.device) {
                m.device.reset();
                if (!a.device->isNull()) m.device = a.device->asString();
            }
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
                if (p.output || p.hidden || p.kind == sd::ParamModel::Kind::Toggle) continue;
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
            "the knob's unit otherwise; default an octave or half the range), or a plugin "
            "module's plug-in parameter \"fx_N.<key>\" (CLAP modulation, amount a fraction "
            "of its range, default 0.5; it must be modulatable). A numeric CLAP id is also "
            "accepted (fx_3.12), with no leading zeros except 0; a numeric semantic key takes "
            "precedence. Several cables "
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
                SUPPLE_TRY(e.validatePluginMod(to->object, *s.fx.find(to->object), to->key, "/to"));
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
                    } else if (const int p = dstType->param(to->key);
                               p >= 0 && !dstType->params()[static_cast<std::size_t>(p)].hidden) {
                        const sd::ParamModel& pm = dstType->params()[static_cast<std::size_t>(p)];
                        if (pm.kind == sd::ParamModel::Kind::Toggle || pm.output || pm.hidden)
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
                auto found = cableId(s, *a.cable);
                if (!found) return std::move(found).error();
                id = *found;
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
    registerRackPresetCommands(registry);
    registerModuleLearnCommands(registry);
    registerRackPatchCommand(registry);
}

// --- rack presets (rack.md §7) --------------------------------------------------------
//
// A preset is a group of modules and the cables among them, saved apart from
// any project: supple.rack-preset/1.0, canonical JSON. Modules are by index
// with places relative to the group's top-left; a cable's ends name a module
// by index and a port, or (plugin: true) a parameter of its plug-in. A plugin
// module's plug-in is an FX chain entry (fx_presets.h). What belongs to a
// project stays out: a Notes module's source track, MIDI learn, envelopes,
// sends and the channels an Out feeds.

namespace {

namespace fs = std::filesystem;
constexpr std::string_view kRackPresetSchema = "supple.rack-preset/1.0";
constexpr std::size_t kMaxRackPreset = 64 * 1024 * 1024;

struct PresetSave {
    std::string rack, name;
    std::optional<std::vector<std::string>> modules;
    std::optional<std::string> description;
    std::optional<bool> replace;
};
struct PresetLoad {
    std::string rack;
    std::optional<std::string> name, path, io;
    std::optional<double> x, y;
};
struct PresetList {
    std::optional<std::string> query;
    std::optional<bool> factory;
};
struct PresetName {
    std::string name;
};

Status validPresetName(const std::string& name) {
    const bool bad = name.empty() || name.size() > 200 || name == "." || name == ".." ||
                     name.front() == ' ' || name.back() == ' ' || name.back() == '.' ||
                     std::any_of(name.begin(), name.end(), [](unsigned char c) {
                         return c < 32 || std::strchr("/\\:*?\"<>|", c) != nullptr;
                     });
    if (bad)
        return Error("rack.preset_name", "\"" + name + "\" is not a preset name", "/name",
                     "use 1-200 characters without / \\ : * ? \" < > | or control characters, "
                     "not ending in a space or a dot: \"Wobble Bass\"");
    return {};
}

Result<fs::path> presetPath(const EngineApp& a, const std::string& name) {
    SUPPLE_TRY(validPresetName(name));
    return a.rackPresetDir() / (name + ".json");
}

// The modules `ids` (one rack's) and the cables among them, as a document.
Result<json::Value> encodePreset(EngineApp& a, const model::Session& s, const std::vector<Id>& ids,
                                 const std::string& name, const std::string& description) {
    double left = model::kMaxRackCoordinate, top = model::kMaxRackCoordinate;
    for (Id id : ids) {
        left = std::min(left, s.modules.find(id)->x);
        top = std::min(top, s.modules.find(id)->y);
    }
    std::map<Id, std::size_t> index, plugin;  // a module's index, by its id or its plug-in's
    json::Value modules = json::Value::array(), sources = json::Value::object();
    for (std::size_t k = 0; k < ids.size(); ++k) {
        const model::RackModule& m = *s.modules.find(ids[k]);
        json::Value o = json::Value::object();
        o["type"] = m.type;
        if (!m.version.empty()) o["version"] = m.version;
        if (!m.label.empty()) o["label"] = m.label;
        o["x"] = m.x - left;
        o["y"] = m.y - top;
        if (m.params.isObject() && !m.params.asObject().empty()) o["params"] = m.params;
        if (m.bypass) o["bypass"] = true;
        if (m.device) o["device"] = *m.device;
        if (m.fx) {
            auto fx = detail::encodeEffects(a, s, {*m.fx});
            if (!fx) return std::move(fx).error().at("/modules/" + std::to_string(k));
            o["fx"] = std::move(fx->front());
            plugin[*m.fx] = k;
        }
        index[ids[k]] = k;
        modules.push_back(std::move(o));
        // A module of the user's (library or project): its source travels too.
        if (const auto* entry = catalog(a).find(m.type);
            entry &&
            (entry->type->origin() == rack::Origin::Library ||
             entry->type->origin() == rack::Origin::Project) &&
            !entry->type->source().empty() && !sources.find(m.type)) {
            json::Value src = json::Value::object();
            const std::string library = detail::moduleName(*entry->type);
            src["name"] = library.empty() ? libraryNameFor(m.type) : library;
            src["source"] = entry->type->source();
            sources[m.type] = std::move(src);
        }
    }
    json::Value cables = json::Value::array();
    for (const auto& e : s.cables) {
        const model::Cable& cb = *e.value;
        const auto from = index.find(cb.from.object);
        const bool toPlugin = cb.to.object.kind == model::Kind::Fx;
        const auto to = toPlugin ? plugin.find(cb.to.object) : index.find(cb.to.object);
        if (from == index.end() || to == (toPlugin ? plugin.end() : index.end())) continue;
        json::Value c = json::Value::object(), f = json::Value::object(), t = json::Value::object();
        f["module"] = static_cast<std::int64_t>(from->second);
        f["port"] = cb.from.key;
        t["module"] = static_cast<std::int64_t>(to->second);
        t["port"] = cb.to.key;
        if (toPlugin) t["plugin"] = true;
        c["from"] = std::move(f);
        c["to"] = std::move(t);
        if (cb.amount) c["amount"] = *cb.amount;
        if (!cb.color.empty()) c["color"] = cb.color;
        cables.push_back(std::move(c));
    }
    json::Value doc = json::Value::object();
    doc["schema"] = std::string(kRackPresetSchema);
    doc["name"] = name;
    if (!description.empty()) doc["description"] = description;
    doc["modules"] = std::move(modules);
    doc["cables"] = std::move(cables);
    if (!sources.asObject().empty()) doc["sources"] = std::move(sources);
    return doc;
}

// Reads a preset and checks its shape (not its types or plug-ins).
// `label` names it in errors (a file name).
Result<json::Value> parsePreset(std::string_view text, const std::string& label) {
    auto parsed = json::parse(text);
    if (!parsed) return std::move(parsed).error();
    json::Value& doc = *parsed;
    const std::string where = label + ": ";
    auto bad = [&](const std::string& at, const std::string& what) {
        return Error("rack.preset_invalid", where + what, at,
                     "the file is not a rack preset this version reads; save the patch again "
                     "with rack.preset.save");
    };
    auto isIndex = [](const json::Value* v, std::size_t n) {
        return v && v->isInt() && v->asInt() >= 0 && static_cast<std::size_t>(v->asInt()) < n;
    };
    if (!doc.isObject()) return bad("", "not a JSON object");
    const auto* schemaField = doc.find("schema");
    if (!schemaField || !schemaField->isString() || schemaField->asString() != kRackPresetSchema)
        return bad("/schema", "schema is not " + std::string(kRackPresetSchema));
    const auto* name = doc.find("name");
    if (!name || !name->isString() || name->asString().empty())
        return bad("/name", "it has no name");
    if (const auto* description = doc.find("description"); description && !description->isString())
        return bad("/description", "description is not a string");
    const auto* modules = doc.find("modules");
    if (!modules || !modules->isArray() || modules->asArray().empty())
        return bad("/modules", "it has no modules");
    const std::size_t n = modules->asArray().size();
    if (n > model::kMaxRackModules) return bad("/modules", "it has more modules than a rack holds");
    for (std::size_t k = 0; k < n; ++k) {
        const json::Value& m = modules->asArray()[k];
        const std::string at = "/modules/" + std::to_string(k);
        if (!m.isObject()) return bad(at, "a module is not an object");
        const auto* type = m.find("type");
        if (!type || !type->isString() || type->asString().empty())
            return bad(at + "/type", "a module has no type");
        for (const char* key : {"x", "y"})
            if (const auto* v = m.find(key);
                v && (!v->isNumber() || !std::isfinite(v->asDouble()) ||
                      std::abs(v->asDouble()) > model::kMaxRackCoordinate))
                return bad(at + "/" + key, std::string(key) + " is not a place in a rack");
        for (const char* key : {"label", "version", "device"})
            if (const auto* v = m.find(key); v && !v->isString())
                return bad(at + "/" + key, std::string(key) + " is not a string");
        if (const auto* v = m.find("params"); v && !v->isObject())
            return bad(at + "/params", "params is not an object");
        if (const auto* v = m.find("bypass"); v && !v->isBool())
            return bad(at + "/bypass", "bypass is not true or false");
        const bool plugin = type->asString() == model::kPluginModule;
        const auto* fx = m.find("fx");
        if (plugin != (fx != nullptr) || (fx && !fx->isObject()))
            return bad(at + "/fx", plugin ? "a plugin module has no plug-in"
                                          : "only a plugin module has a plug-in");
    }
    if (const auto* sources = doc.find("sources")) {
        if (!sources->isObject()) return bad("/sources", "sources is not an object");
        for (const auto& m : sources->asObject()) {
            const std::string at = json::pointerJoin("/sources", m.key);
            const json::Value* library = m.value.isObject() ? m.value.find("name") : nullptr;
            const json::Value* source = m.value.isObject() ? m.value.find("source") : nullptr;
            if (!library || !library->isString() ||
                !detail::validModuleName(library->asString(), {}) || !source ||
                !source->isString() || source->asString().empty() ||
                source->asString().size() > detail::kMaxModuleBytes)
                return bad(at, "a module's source is not {name, source}");
        }
    }
    const auto* cables = doc.find("cables");
    if (cables && !cables->isArray()) return bad("/cables", "cables is not an array");
    if (cables && cables->asArray().size() > model::kMaxRackCables)
        return bad("/cables", "it has more cables than a rack holds");
    for (std::size_t k = 0; cables && k < cables->asArray().size(); ++k) {
        const json::Value& c = cables->asArray()[k];
        const std::string at = "/cables/" + std::to_string(k);
        if (!c.isObject()) return bad(at, "a cable is not an object");
        for (const char* end : {"from", "to"}) {
            const auto* e = c.find(end);
            if (!e || !e->isObject() || !isIndex(e->find("module"), n) || !e->find("port") ||
                !e->find("port")->isString())
                return bad(at + "/" + end,
                           std::string("a cable's ") + end + " is not {module: an index, port}");
            if (const auto* p = e->find("plugin"); p && !p->isBool())
                return bad(at + "/" + end + "/plugin", "plugin is not true or false");
        }
        if (const auto* v = c.find("amount");
            v && (!v->isNumber() || !std::isfinite(v->asDouble())))
            return bad(at + "/amount", "amount is not a number");
        if (const auto* v = c.find("color"); v && !v->isString())
            return bad(at + "/color", "color is not a string");
    }
    return std::move(doc);
}

Result<json::Value> readPreset(const fs::path& file) {
    auto text = readTextFile(file, kMaxRackPreset);
    if (!text) return std::move(text).error();
    return parsePreset(*text, file.filename().string());
}

// The factory patches (modules/patches, compiled in), read once. One that
// does not read is left out; a test keeps every one readable.
struct FactoryPreset {
    std::string file;
    json::Value doc;
};
const std::vector<FactoryPreset>& factoryPresets() {
    static const std::vector<FactoryPreset> all = [] {
        std::vector<FactoryPreset> v;
        for (const rack::FactoryPatch& p : rack::factoryPatches())
            if (auto doc = parsePreset(p.text, std::string(p.file) + ".json"))
                v.push_back({std::string(p.file), std::move(*doc)});
        return v;
    }();
    return all;
}
const FactoryPreset* factoryPreset(const std::string& name) {
    for (const FactoryPreset& p : factoryPresets())
        if (p.doc.find("name")->asString() == name) return &p;
    return nullptr;
}

std::string stringOr(const json::Value& o, const char* key, std::string fallback = {}) {
    const auto* v = o.find(key);
    return v ? v->asString() : fallback;
}
double numberOr(const json::Value& o, const char* key, double fallback = 0) {
    const auto* v = o.find(key);
    return v ? v->asDouble() : fallback;
}

}  // namespace

void registerRackPresetCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- rack.preset.save
    [&] {
        Command<PresetSave, json::Value> c;
        common(c, "rack.preset.save", Kind::Action,
               "Save a rack's modules and cables as a preset.");
        c.description =
            "modules: which (default the whole rack); the cables among them come too. Plugin "
            "modules keep their plug-in's settings, and the source of your own (library) "
            "modules goes with them. A Notes module's source track, MIDI learn, envelopes, "
            "sends and the channels an Out feeds stay with the project. Saved by "
            "name in the user's rack presets; replace: true overwrites one.";
        c.args = schema::object<PresetSave>("rack_preset_save_args")
                     .field("rack", &PresetSave::rack, idOf("rk", "Rack"))
                     .field("name", &PresetSave::name, schema::string().minLength(1))
                     .field("modules", &PresetSave::modules, schema::arrayOf(idOf("mo", "Module")))
                     .field("description", &PresetSave::description, schema::string())
                     .field("replace", &PresetSave::replace, schema::boolean())
                     .build();
        c.examples = {{"Save the whole rack", j(R"({"rack": "rk_1", "name": "Gated VCA"})"),
                       j(R"({"path": "<any>", "modules": 3, "cables": 1})"), "song_with_rack"}};
        c.errors = {{"rack.preset_name", "not a usable name", "a name without path characters"},
                    {"rack.preset_exists", "a preset has the name", "replace: true or a new name"}};
        c.run = [](const PresetSave& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            auto path = presetPath(e, a.name);
            if (!path) return std::move(path).error();
            const model::Session s = e.view();
            auto rack = rackId(s, a.rack);
            if (!rack) return std::move(rack).error();
            std::vector<Id> ids;
            if (a.modules) {
                for (std::size_t k = 0; k < a.modules->size(); ++k) {
                    const std::string at = "/modules/" + std::to_string(k);
                    auto id = moduleId(s, (*a.modules)[k], at.c_str());
                    if (!id) return std::move(id).error();
                    if (s.modules.find(*id)->rack != *rack)
                        return Error("rack.preset", (*a.modules)[k] + " is not in " + a.rack, at,
                                     "rack.get " + a.rack + " lists its modules");
                    if (std::find(ids.begin(), ids.end(), *id) == ids.end()) ids.push_back(*id);
                }
            } else {
                for (const auto& m : s.modules)
                    if (m.value->rack == *rack) ids.push_back(m.id);
            }
            if (ids.empty())
                return Error("rack.preset", "nothing to save", a.modules ? "/modules" : "/rack",
                             "add modules first (module.add)");
            std::sort(ids.begin(), ids.end());
            auto doc = encodePreset(e, s, ids, a.name, a.description.value_or(""));
            if (!doc) return std::move(doc).error();
            const std::string text = json::writeCanonical(*doc) + "\n";
            if (text.size() > kMaxRackPreset)
                return Error("rack.preset", "the preset exceeds 64 MiB", "/modules",
                             "save fewer plugin modules");
            std::error_code ec;
            fs::create_directories(path->parent_path(), ec);
            if (auto written = detail::writePreset(*path, text, a.replace.value_or(false));
                !written) {
                Error error = written.error();
                if (error.code == "chain.exists") {
                    error.code = "rack.preset_exists";
                    error.message = "a rack preset named \"" + a.name + "\" exists";
                }
                return error;
            }
            json::Value out = json::Value::object();
            out["path"] = path->string();
            out["modules"] = static_cast<std::int64_t>(ids.size());
            out["cables"] = static_cast<std::int64_t>((*doc)["cables"].asArray().size());
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.preset.load
    [&] {
        Command<PresetLoad, json::Value> c;
        common(c, "rack.preset.load", Kind::Edit,
               "Add a saved preset's modules and cables to a rack.");
        c.description =
            "name (rack.preset.list: a saved preset, or else a factory patch) or path (a "
            "preset file). x, y: the group's top-left "
            "(default right of the rack's modules). io: join (default) gives the preset's Out "
            "and In modules' cables to the rack's own Outs and Ins in order, so a whole patch "
            "sounds at once (extra ones are added); add makes new ones. Notes modules play the "
            "rack's channel. Your own modules the preset carries are written into the library "
            "when it lacks them (installed lists them). Every embedded source must compile "
            "and declare its sources key as ModuleInfo.id before any module is installed. "
            "Knobs and cables the module types no "
            "longer have are left out with a warning (warnings); a module type or plug-in that "
            "is not installed is an error, and the rack does not change.";
        c.args = schema::object<PresetLoad>("rack_preset_load_args")
                     .field("rack", &PresetLoad::rack, idOf("rk", "Rack"))
                     .field("name", &PresetLoad::name, schema::string().minLength(1))
                     .field("path", &PresetLoad::path, schema::string().minLength(1))
                     .field("x", &PresetLoad::x, coordinate())
                     .field("y", &PresetLoad::y, coordinate())
                     .field("io", &PresetLoad::io,
                            schema::string().pattern("^(join|add)$", "join or add"))
                     .build();
        c.examples = {
            {"Load a saved patch into a new rack", j(R"({"rack": "rk_2", "name": "Gated VCA"})"),
             j(R"({"modules": ["mo_5", "mo_6"], "cables": ["cb_2"]})"), "song_with_rack_preset"}};
        c.errors = {{"rack.preset_source", "name and path both or neither", "give one"},
                    {"rack.unknown_type", "the preset needs a module type that is not installed",
                     "module.write it, or load another preset"},
                    {"chain.plugin_missing", "the preset needs a plug-in that is not installed",
                     "install it"},
                    {"rack.preset_module", "an embedded source does not compile or has another id",
                     "fix its source and ModuleInfo.id to match the sources entry"},
                    {"rack.preset_invalid", "not a rack preset", "save it again"}};
        c.run = [](const PresetLoad& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            if (a.name.has_value() == a.path.has_value())
                return Error("rack.preset_source", "give name or path, one of them", "/name",
                             "rack.preset.list shows the saved names");
            // A name is the user's preset, or else a factory patch's.
            Result<json::Value> read = json::Value();
            if (a.name) {
                auto p = presetPath(e, *a.name);
                if (!p) return std::move(p).error();
                std::error_code ec;
                if (fs::is_regular_file(*p, ec))
                    read = readPreset(*p);
                else if (const FactoryPreset* f = factoryPreset(*a.name))
                    read = f->doc;
                else
                    return Error("rack.preset", "no rack preset named \"" + *a.name + "\"", "/name",
                                 "rack.preset.list shows the saved and factory names");
            } else {
                read = readPreset(e.resolve(*a.path));
            }
            if (!read) return std::move(read).error();
            const json::Value& doc = *read;
            const model::Session s = e.view();
            auto rack = rackId(s, a.rack);
            if (!rack) return std::move(rack).error();
            const bool join = a.io.value_or("join") == "join";
            json::Value warnings = json::Value::array(), installed = json::Value::array();
            auto warn = [&](const char* code, const std::string& message, const std::string& at) {
                json::Value w = json::Value::object();
                w["code"] = code;
                w["message"] = message;
                w["path"] = at;
                warnings.push_back(std::move(w));
            };
            // Validate every source before installing any, including copies of
            // already installed types: each source must declare its map key.
            if (const json::Value* sources = doc.find("sources"))
                for (const auto& m : sources->asObject()) {
                    const std::string at = json::pointerJoin("/sources", m.key);
                    const auto compiled = detail::compileModule(m.value.find("source")->asString(),
                                                                rack::Origin::Library);
                    if (!compiled.type) {
                        std::string why;
                        for (const auto& d : compiled.diagnostics)
                            if (d.severity == "error") {
                                why = ": " + d.message;
                                break;
                            }
                        return Error("rack.preset_module",
                                     "the preset's " + m.key + " does not compile here" + why, at,
                                     "fix the embedded source; module.check {source: ...} "
                                     "shows its diagnostics");
                    }
                    if (compiled.type->id() != m.key)
                        return Error("rack.preset_module",
                                     "the source for " + m.key + " declares " + compiled.type->id(),
                                     at,
                                     "set ModuleInfo.id to " + m.key +
                                         ", or correct the preset's sources key and module types");
                }
            // The user's modules it brings: into the library when it lacks them.
            if (const json::Value* sources = doc.find("sources"))
                for (const auto& m : sources->asObject()) {
                    const std::string at = json::pointerJoin("/sources", m.key);
                    const std::string& source = m.value.find("source")->asString();
                    if (const auto* entry = catalog(e).find(m.key)) {
                        if (entry->type->source() != source)
                            warn("rack.preset_module",
                                 "the installed " + m.key +
                                     " differs from the preset's copy; the installed one plays",
                                 at);
                        continue;
                    }
                    auto destination = detail::availableModuleName(
                        e.moduleDir(), m.value.find("name")->asString());
                    if (!destination) return std::move(destination).error();
                    const std::string& name = *destination;
                    auto saved = saveLibraryModule(e, name, source, at.c_str());
                    if (!saved) return std::move(saved).error();
                    if (!(*saved)["ok"].asBool())
                        return Error("rack.preset_module",
                                     "the preset's " + m.key + " does not compile here", at,
                                     "it was saved to the library as " + name +
                                         "; module.check {name: \"" + name + "\"} shows why");
                    installed.push_back(name);
                }

            // The rack's own Outs and Ins, in order, for io: join; and its counts.
            std::vector<Id> ownOuts, ownIns;
            std::size_t count = 0, cableCount = 0;
            for (const auto& m : s.modules) {
                if (m.value->rack != *rack) continue;
                ++count;
                if (m.value->type == model::kOutModule) ownOuts.push_back(m.id);
                if (m.value->type == model::kInModule) ownIns.push_back(m.id);
            }
            for (const auto& cb : s.cables)
                cableCount += model::rackOfEnd(s, cb.value->from) == *rack;

            // Everything is checked before the model changes.
            const auto& entries = doc.find("modules")->asArray();
            const std::size_t n = entries.size();
            std::vector<model::RackModule> made(n);
            std::vector<std::shared_ptr<const rack::ModuleType>> types(n);
            std::vector<std::optional<detail::PreparedFx>> plugins(n);
            std::vector<std::optional<Id>> joined(n);  // io: join: the rack's own module
            std::size_t outs = 0, ins = 0, added = 0;
            for (std::size_t k = 0; k < n; ++k) {
                const json::Value& o = entries[k];
                const std::string at = "/modules/" + std::to_string(k);
                const std::string type = o.find("type")->asString();
                const bool isOut = type == model::kOutModule, isIn = type == model::kInModule;
                if (join && isOut && outs < ownOuts.size()) {
                    joined[k] = ownOuts[outs++];
                    continue;
                }
                if (join && isIn && ins < ownIns.size()) {
                    joined[k] = ownIns[ins++];
                    continue;
                }
                outs += isOut;
                ins += isIn;
                const auto* entry = catalog(e).find(type);
                if (!entry)
                    return Error("rack.unknown_type",
                                 "the preset needs module type " + type +
                                     (o.find("version") ? " " + o.find("version")->asString()
                                                        : std::string()) +
                                     ", which is not installed",
                                 at + "/type",
                                 "write it into the module library (module.write), or load "
                                 "another preset; module.list shows what is installed");
                types[k] = entry->type;
                model::RackModule& m = made[k];
                m.rack = *rack;
                m.type = type;
                m.version = entry->type->info().version;
                m.label = stringOr(o, "label");
                m.bypass = o.find("bypass") && o.find("bypass")->asBool();
                if (const auto* d = o.find("device")) m.device = d->asString();
                if (type == model::kPluginModule) {
                    auto ready = detail::prepareEffects(e, {*o.find("fx")}, at + "/fx");
                    if (!ready) return std::move(ready).error();
                    plugins[k] = std::move(ready->front());
                    m.version = "1.0.0";
                } else if (const auto* params = o.find("params")) {
                    // Knob by knob: one the type no longer has is left out.
                    json::Value merged = json::Value::object();
                    for (const auto& p : params->asObject()) {
                        json::Value one = json::Value::object();
                        one[p.key] = p.value;
                        auto next = mergeParams(*entry->type, merged, one, at + "/params");
                        if (!next) {
                            warn("rack.preset_knob",
                                 "module " + std::to_string(k) + " (" + entry->type->info().name +
                                     "): " + next.error().message + "; left at its default",
                                 at + "/params/" + p.key);
                            continue;
                        }
                        merged = std::move(*next);
                    }
                    m.params = std::move(merged);
                }
                ++added;
            }
            if (count + added > model::kMaxRackModules)
                return Error("rack.limit", "a rack holds at most 512 modules", "/rack",
                             "the preset adds " + std::to_string(added) + " to " +
                                 std::to_string(count) + "; remove modules or use another rack");
            if ((join ? std::max(outs, ownOuts.size()) : outs + ownOuts.size()) >
                model::kMaxRackOuts)
                return Error("rack.limit", "a rack holds at most 32 Out modules", "/io",
                             "use io: join, or another rack");
            if ((join ? std::max(ins, ownIns.size()) : ins + ownIns.size()) > model::kMaxRackIns)
                return Error("rack.limit", "a rack holds at most 32 In modules", "/io",
                             "use io: join, or another rack");

            // The group's top-left: given, else right of the rack's modules. A
            // rack that holds only its Outs and Ins takes the whole patch, in its
            // own layout: from those modules' corner, the joined ones moved too.
            const bool fresh = join && count == ownOuts.size() + ownIns.size();
            auto place = freePlace(e, s, *rack);
            if (fresh && count > 0) {
                place = {model::kMaxRackCoordinate, model::kMaxRackCoordinate};
                for (const auto& m : s.modules)
                    if (m.value->rack == *rack)
                        place = {std::min(place.first, m.value->x),
                                 std::min(place.second, m.value->y)};
            }
            const double x0 = a.x.value_or(place.first), y0 = a.y.value_or(place.second);
            for (std::size_t k = 0; k < n; ++k) {
                if (joined[k] && !fresh) continue;
                made[k].x = std::clamp(x0 + numberOr(entries[k], "x"), -model::kMaxRackCoordinate,
                                       model::kMaxRackCoordinate);
                made[k].y = std::clamp(y0 + numberOr(entries[k], "y"), -model::kMaxRackCoordinate,
                                       model::kMaxRackCoordinate);
            }

            // Cables whose ends the types still have.
            struct Planned {
                std::size_t from, to;
                std::string fromKey, toKey;
                bool plugin;
                std::optional<double> amount;
                std::string color;
            };
            std::vector<Planned> planned;
            EngineApp::PluginModValidation validation{s, {}};
            const json::Value* cables = doc.find("cables");
            for (std::size_t k = 0; cables && k < cables->asArray().size(); ++k) {
                const json::Value& entry = cables->asArray()[k];
                const std::string at = "/cables/" + std::to_string(k);
                const json::Value& f = *entry.find("from");
                const json::Value& t = *entry.find("to");
                Planned p{static_cast<std::size_t>(f.find("module")->asInt()),
                          static_cast<std::size_t>(t.find("module")->asInt()),
                          f.find("port")->asString(),
                          t.find("port")->asString(),
                          t.find("plugin") && t.find("plugin")->asBool(),
                          std::nullopt,
                          stringOr(entry, "color")};
                auto skip = [&](const std::string& why) {
                    warn("rack.preset_cable",
                         "cable " + std::to_string(k) + ": " + why + "; left out", at);
                };
                if (!p.color.empty() && !model::validTrackColor(p.color)) p.color.clear();
                // A joined module is the rack's own: its type is the catalog's.
                auto typeAt = [&](std::size_t i) -> std::shared_ptr<const rack::ModuleType> {
                    if (joined[i]) return typeOfModule(e, *s.modules.find(*joined[i]));
                    return types[i];
                };
                const auto src = typeAt(p.from), dst = typeAt(p.to);
                if (src && src->output(p.fromKey) < 0) {
                    skip(src->info().name + " has no output " + p.fromKey);
                    continue;
                }
                const auto* amount = entry.find("amount");
                if (p.plugin) {
                    if (!plugins[p.to]) {
                        skip("module " + std::to_string(p.to) + " is not a plugin module");
                        continue;
                    }
                    auto checked =
                        e.validatePluginMod({}, plugins[p.to]->fx, p.toKey, at + "/to/port",
                                            plugins[p.to]->blob, &validation);
                    if (!checked) {
                        skip(checked.error().message + ". " + checked.error().hint);
                        continue;
                    }
                    p.amount = amount ? amount->asDouble() : 0.5;
                } else if (dst && dst->input(p.toKey) >= 0) {
                    // an input: no amount
                } else if (const int q = dst ? dst->param(p.toKey) : -1;
                           q >= 0 && !dst->params()[static_cast<std::size_t>(q)].hidden) {
                    const sd::ParamModel& pm = dst->params()[static_cast<std::size_t>(q)];
                    if (pm.kind == sd::ParamModel::Kind::Toggle || pm.output || pm.hidden) {
                        skip(p.toKey + " is a switch or a meter");
                        continue;
                    }
                    p.amount = amount ? amount->asDouble() : defaultAmount(pm);
                } else if (dst) {
                    skip(dst->info().name + " has no input or knob " + p.toKey);
                    continue;
                }
                bool twice = false;
                for (const Planned& q : planned)
                    twice |= q.from == p.from && q.to == p.to && q.fromKey == p.fromKey &&
                             q.toKey == p.toKey && q.plugin == p.plugin;
                // Between two of the rack's own (joined) modules, it may be there already.
                if (joined[p.from] && joined[p.to])
                    for (const auto& cb : s.cables)
                        twice |= cb.value->from == model::CableEnd{*joined[p.from], p.fromKey} &&
                                 cb.value->to == model::CableEnd{*joined[p.to], p.toKey};
                if (!twice) planned.push_back(std::move(p));
            }
            if (cableCount + planned.size() > model::kMaxRackCables)
                return Error("rack.limit", "a rack holds at most 4096 cables", "/rack",
                             "remove cables first, or use another rack");

            model::EditTxn& t = e.txn();
            std::vector<Id> ids(n);
            json::Value modules = json::Value::array(), madeCables = json::Value::array();
            for (std::size_t k = 0; k < n; ++k) {
                if (joined[k]) {
                    ids[k] = *joined[k];
                    if (fresh) {
                        model::RackModule moved = *s.modules.find(*joined[k]);
                        moved.x = made[k].x;
                        moved.y = made[k].y;
                        t.put(*joined[k], std::move(moved));
                    }
                    continue;
                }
                if (plugins[k]) made[k].fx = detail::installEffect(e, *plugins[k]);
                ids[k] = t.create(made[k]);
                modules.push_back(ids[k].str());
            }
            for (const Planned& p : planned) {
                model::Cable cable;
                cable.from = {ids[p.from], p.fromKey};
                cable.to = {p.plugin ? *made[p.to].fx : ids[p.to], p.toKey};
                cable.amount = p.amount;
                cable.color = p.color;
                madeCables.push_back(t.create(cable).str());
            }
            json::Value out = json::Value::object();
            out["modules"] = std::move(modules);
            out["cables"] = std::move(madeCables);
            if (!installed.asArray().empty()) out["installed"] = std::move(installed);
            if (!warnings.asArray().empty()) out["warnings"] = std::move(warnings);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.preset.list
    [&] {
        Command<PresetList, json::Value> c;
        common(c, "rack.preset.list", Kind::Query,
               "List rack presets, saved and factory: names and contents.");
        c.description =
            "query: a part of the name or description. factory: true lists only the factory "
            "patches that ship with the app, false only the saved ones (default both). Each "
            "preset: name, path (a saved one's) or factory: true, description, its module and "
            "cable counts and its module types. A saved preset hides a factory patch of its "
            "name. Files that are not rack presets are skipped with a warning (warnings).";
        c.args = schema::object<PresetList>("rack_preset_list_args")
                     .field("query", &PresetList::query, schema::string())
                     .field("factory", &PresetList::factory, schema::boolean())
                     .build();
        c.examples = {{"The saved presets", j(R"({"factory": false})"),
                       j(R"({"presets": [{"name": "Gated VCA", "path": "<any>",
                             "modules": 3, "cables": 1,
                             "types": ["com.suppleaudio.rack.notes", "com.suppleaudio.rack.out",
                                       "com.suppleaudio.rack.vca"]}]})"),
                       "song_with_rack_preset"},
                      {"A factory patch", j(R"({"query": "Init Voice", "factory": true})"),
                       j(R"({"presets": [{"name": "Init Voice", "factory": true,
                             "description": "<any>", "modules": 6, "cables": 9,
                             "types": ["com.suppleaudio.rack.adsr", "com.suppleaudio.rack.filter",
                                       "com.suppleaudio.rack.notes", "com.suppleaudio.rack.out",
                                       "com.suppleaudio.rack.vca", "com.suppleaudio.rack.vco"]}]})"),
                       "song_with_rack"}};
        c.run = [](const PresetList& a, Context& ctx) -> Result<json::Value> {
            const fs::path dir = app(ctx).rackPresetDir();
            auto lower = [](std::string text) {
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return text;
            };
            const std::string query = lower(a.query.value_or(""));
            std::vector<std::pair<std::string, json::Value>> rows;
            json::Value warnings = json::Value::array();
            // A row for a preset document; nullopt when the query leaves it out.
            auto rowOf = [&](const json::Value& doc) -> std::optional<json::Value> {
                const std::string name = doc.find("name")->asString();
                const std::string description = stringOr(doc, "description");
                if (!query.empty() && lower(name).find(query) == std::string::npos &&
                    lower(description).find(query) == std::string::npos)
                    return std::nullopt;
                json::Value row = json::Value::object();
                row["name"] = name;
                if (!description.empty()) row["description"] = description;
                row["modules"] = static_cast<std::int64_t>(doc.find("modules")->asArray().size());
                const auto* cables = doc.find("cables");
                row["cables"] = static_cast<std::int64_t>(cables ? cables->asArray().size() : 0);
                std::set<std::string> kinds;
                for (const auto& m : doc.find("modules")->asArray())
                    kinds.insert(m.find("type")->asString());
                json::Value types = json::Value::array();
                for (const auto& k : kinds) types.push_back(k);
                row["types"] = std::move(types);
                return row;
            };
            std::set<std::string> saved;  // the saved presets' names
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code kind;
                if (!it->is_regular_file(kind) || it->path().extension() != ".json") continue;
                if (a.factory == true) {  // only to know which factory patches are hidden
                    saved.insert(it->path().stem().string());
                    continue;
                }
                auto doc = readPreset(it->path());
                if (!doc) {
                    json::Value w = json::Value::object();
                    w["code"] = doc.error().code;
                    w["message"] = doc.error().message;
                    w["path"] = it->path().string();
                    warnings.push_back(std::move(w));
                    continue;
                }
                // Loading by name reads <name>.json: that file is what hides a factory patch.
                saved.insert(it->path().stem().string());
                auto row = rowOf(*doc);
                if (!row) continue;
                (*row)["path"] = it->path().string();
                rows.emplace_back((*doc)["name"].asString() + '\0' + it->path().string(),
                                  std::move(*row));
            }
            for (const FactoryPreset& f : factoryPresets()) {
                if (a.factory == false) break;
                const std::string name = f.doc.find("name")->asString();
                if (saved.count(name) != 0) continue;
                auto row = rowOf(f.doc);
                if (!row) continue;
                (*row)["factory"] = true;
                rows.emplace_back(name + '\0', std::move(*row));
            }
            std::sort(rows.begin(), rows.end(),
                      [](const auto& x, const auto& y) { return x.first < y.first; });
            json::Value presets = json::Value::array();
            for (auto& r : rows) presets.push_back(std::move(r.second));
            json::Value out = json::Value::object();
            out["presets"] = std::move(presets);
            if (!warnings.asArray().empty()) out["warnings"] = std::move(warnings);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    // ---------------------------------------------------------------- rack.preset.delete
    [&] {
        Command<PresetName, json::Value> c;
        common(c, "rack.preset.delete", Kind::Action, "Delete a saved rack preset.");
        c.args = schema::object<PresetName>("rack_preset_delete_args")
                     .field("name", &PresetName::name, schema::string().minLength(1))
                     .build();
        c.examples = {{"Delete a preset", j(R"({"name": "Gated VCA"})"), j(R"({"path": "<any>"})"),
                       "song_with_rack_preset"}};
        c.errors = {{"rack.preset", "no saved preset of that name", "rack.preset.list"},
                    {"rack.preset_factory", "a factory patch cannot be deleted",
                     "save your own version under its name instead"}};
        c.run = [](const PresetName& a, Context& ctx) -> Result<json::Value> {
            auto path = presetPath(app(ctx), a.name);
            if (!path) return std::move(path).error();
            std::error_code ec;
            if (!fs::is_regular_file(fs::symlink_status(*path, ec))) {
                if (factoryPreset(a.name))
                    return Error("rack.preset_factory",
                                 "\"" + a.name + "\" is a factory patch, which cannot be deleted",
                                 "/name",
                                 "save a preset under its name to keep your own version in its "
                                 "place");
                return Error("rack.preset", "no rack preset named \"" + a.name + "\"", "/name",
                             "rack.preset.list shows the saved names");
            }
            if (!fs::remove(*path, ec) || ec)
                return Error("rack.preset", "cannot delete " + path->string() + ": " + ec.message(),
                             "/name", "check the folder's permissions");
            json::Value out = json::Value::object();
            out["path"] = path->string();
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

// --- MIDI learn on module knobs (rack.md §7) ------------------------------------------

namespace {

struct KnobLearnArgs : LearnBindingArgs {
    std::string module, param;
};
struct KnobLearnRef {
    std::string module, param;
};
struct KnobLearnList {
    std::optional<std::string> module;
};

// The module and its knob a binding names; refused for switches' meters and
// plugin modules (their knobs are their plug-in's: fx.learn.set).
Result<std::pair<Id, std::string>> learnTarget(EngineApp& e, const model::Session& s,
                                               const std::string& module,
                                               const std::string& param) {
    auto id = moduleId(s, module);
    if (!id) return std::move(id).error();
    const model::RackModule& m = *s.modules.find(*id);
    if (m.fx)
        return Error("rack.param", "a plugin module's knobs are its plug-in's", "/module",
                     "fx.learn.set {fx: \"" + m.fx->str() + "\"}");
    const auto type = typeOfModule(e, m);
    if (!type)
        return Error("rack.unknown_type", "its type " + m.type + " is not installed", "/module",
                     "module.list");
    const int index = type->param(param);
    if (index < 0 || type->params()[static_cast<std::size_t>(index)].output ||
        type->params()[static_cast<std::size_t>(index)].hidden)
        return Error("rack.param", type->info().name + " has no knob " + param, "/param",
                     type->info().name + " has " + keysOf(*type, false, false, true));
    return std::pair<Id, std::string>{*id, param};
}

json::Value knobLearnJson(Id module, const model::ParamLearn& l) {
    json::Value v = json::Value::object();
    v["module"] = module.str();
    v["param"] = l.param;
    v["midi"] = model::midiControlSchema().encode(l.control);
    v["control"] = describe(l.control);
    if (l.mode != model::ControlMode::Absolute)
        v["mode"] = model::controlModeSchema().encode(l.mode);
    if (l.softTakeover) v["soft_takeover"] = true;
    return v;
}

}  // namespace

void registerModuleLearnCommands(cmd::Registry& registry) {
    [&] {
        Command<KnobLearnArgs, json::Value> c;
        common(c, "module.learn.set", Kind::Edit, "Let a MIDI controller turn a module's knob.");
        c.description =
            "One binding per knob (a new one replaces it). midi.device: a substring of the MIDI "
            "input's name (empty: any). mode for a CC: absolute, relative1 (127 = -1, 1 = +1), "
            "relative2 (63 = -1, 65 = +1), relative3 (65 = -1, 1 = +1) or toggle. soft_takeover: "
            "an absolute control acts once it reaches the knob. A knob turns along its travel "
            "(a frequency's is logarithmic); each movement is one undo step. A plugin module's "
            "knobs are its plug-in's: fx.learn.set.";
        c.tags = {"rack", "modular", "midi"};
        auto args = schema::object<KnobLearnArgs>("module_learn_set_args");
        args.field("module", &KnobLearnArgs::module, idOf("mo", "Module"))
            .field("param", &KnobLearnArgs::param, schema::string().minLength(1));
        learnBindingArgFields(args);
        c.args = args.build();
        c.examples = {{"The VCA's level on CC 21", j(R"({"module": "mo_3", "param": "level",
                             "midi": {"kind": "cc", "channel": 1, "number": 21}})"),
                       j(R"({"param": "level", "control": "CC 21 · ch 1"})"), "song_with_rack"}};
        c.run = [](const KnobLearnArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            auto target = learnTarget(e, s, a.module, a.param);
            if (!target) return std::move(target).error();
            model::ParamLearn learn;
            static_cast<model::ControlBinding&>(learn) = a.binding();
            learn.param = target->second;
            e.txn().modify<model::RackModule>(target->first, [&](model::RackModule& m) {
                std::erase_if(m.learn, [&](const auto& l) { return l.param == learn.param; });
                m.learn.push_back(learn);
                std::sort(m.learn.begin(), m.learn.end(),
                          [](const auto& x, const auto& y) { return x.param < y.param; });
            });
            json::Value out = json::Value::object();
            out["param"] = learn.param;
            out["control"] = describe(learn.control);
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<KnobLearnRef, json::Value> c;
        common(c, "module.learn.remove", Kind::Edit, "Remove a module knob's MIDI controller.");
        c.tags = {"rack", "modular", "midi"};
        c.args = schema::object<KnobLearnRef>("module_learn_remove_args")
                     .field("module", &KnobLearnRef::module, idOf("mo", "Module"))
                     .field("param", &KnobLearnRef::param, schema::string().minLength(1))
                     .build();
        c.examples = {{"Free the level", j(R"({"module": "mo_3", "param": "level"})"), j("{}"),
                       "song_with_rack_learn"}};
        c.run = [](const KnobLearnRef& a, Context& ctx) -> Result<json::Value> {
            EngineApp& e = app(ctx);
            const model::Session s = e.view();
            auto id = moduleId(s, a.module);
            if (!id) return std::move(id).error();
            const auto& learn = s.modules.find(*id)->learn;
            if (std::none_of(learn.begin(), learn.end(),
                             [&](const auto& l) { return l.param == a.param; }))
                return Error("module.learn_unknown", a.param + " has no MIDI controller", "/param",
                             "module.learn.list shows the bound knobs");
            e.txn().modify<model::RackModule>(*id, [&](model::RackModule& m) {
                std::erase_if(m.learn, [&](const auto& l) { return l.param == a.param; });
            });
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<KnobLearnList, json::Value> c;
        common(c, "module.learn.list", Kind::Query, "List the MIDI controllers of module knobs.");
        c.description = "Every module's, or one module's. control names the message in words.";
        c.tags = {"rack", "modular", "midi"};
        c.args = schema::object<KnobLearnList>("module_learn_list_args")
                     .field("module", &KnobLearnList::module, idOf("mo", "Module"))
                     .build();
        c.examples = {{"Every binding", json::Value::object(),
                       j(R"({"learn": [{"module": "mo_3", "param": "level",
                             "midi": {"kind": "cc", "channel": 1, "number": 21},
                             "control": "CC 21 · ch 1"}]})"),
                       "song_with_rack_learn"}};
        c.run = [](const KnobLearnList& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = app(ctx).view();
            json::Value learn = json::Value::array();
            if (a.module) {
                auto id = moduleId(s, *a.module);
                if (!id) return std::move(id).error();
                for (const auto& l : s.modules.find(*id)->learn)
                    learn.push_back(knobLearnJson(*id, l));
            } else {
                for (const auto& m : s.modules)
                    for (const auto& l : m.value->learn) learn.push_back(knobLearnJson(m.id, l));
            }
            json::Value out = json::Value::object();
            out["learn"] = std::move(learn);
            return out;
        };
        registry.add(std::move(c));
    }();
}

// --- rack.patch: a whole patch in one call (rack.md §7) --------------------------------

namespace {

struct RackPatchArgs {
    std::string rack;
    json::Value modules = json::Value::object();
    std::optional<std::vector<json::Value>> cables;
    std::optional<double> x, y;
};

bool validLocalName(const std::string& name) {
    if (name.empty() || name.size() > 32 || name[0] < 'a' || name[0] > 'z') return false;
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }))
        return false;
    const auto id = Id::parse(name);  // "mo_3" names a module already there
    return !id;
}

Result<json::Value> invokeCommand(const char* name, json::Value args, Context& ctx,
                                  const std::string& at) {
    auto r = ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
    if (!r) {
        Error error = std::move(r).error();
        error.path.insert(0, at);
        return error;
    }
    return r;
}

// --- The automatic layout (rack.md §7.2) ---

constexpr double kLayoutGapX = 40, kLayoutGapY = 30, kLayoutBandGap = 60;
constexpr double kLayoutAspect = 1.6;  // width to height, about a window's

struct LayoutBox {
    double width = 120, height = 120;
};

// Places boxes joined by `edges` (from, to; indices into `boxes`) left to right
// by signal flow, top-left at x0, y0. Each box goes one column right of the
// longest chain feeding it, then as far right as its consumers allow, so a
// modulator sits just left of what it modulates. Within a column, boxes are
// ordered by the places of their neighbours (barycentres, swept both ways) so
// cables cross less. A column taller than a window-shaped patch would be is
// split into several, and a row of columns far wider than that wraps into
// bands, one under the other. Deterministic: ties keep the order of `boxes`.
std::vector<std::pair<double, double>> layoutBoxes(const std::vector<LayoutBox>& boxes,
                                                   const std::vector<std::pair<int, int>>& edges,
                                                   double x0, double y0) {
    const int n = static_cast<int>(boxes.size());
    std::vector<std::pair<double, double>> out(boxes.size(), {x0, y0});
    if (n == 0) return out;
    std::vector<std::vector<int>> outs(n), ins(n);
    for (const auto& [from, to] : edges) {
        if (from == to || from < 0 || to < 0 || from >= n || to >= n) continue;
        if (std::find(outs[from].begin(), outs[from].end(), to) != outs[from].end()) continue;
        outs[from].push_back(to);
        ins[to].push_back(from);
    }
    // Longest chain feeding each box; a feedback loop stops at n columns.
    std::vector<int> depth(n, 0);
    for (int pass = 0; pass < n; ++pass) {
        bool moved = false;
        for (int u = 0; u < n; ++u)
            for (int v : outs[u])
                if (depth[v] < depth[u] + 1 && depth[u] + 1 < n) {
                    depth[v] = depth[u] + 1;
                    moved = true;
                }
        if (!moved) break;
    }
    // Then right, up to one column left of its nearest consumer.
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth[a] > depth[b]; });
    for (int u : order) {
        int nearest = -1;
        for (int v : outs[u])
            if (depth[v] > depth[u]) nearest = nearest < 0 ? depth[v] : std::min(nearest, depth[v]);
        if (nearest > depth[u] + 1) depth[u] = nearest - 1;
    }
    // A last stage (fed, feeding nothing here) in the last column.
    int last = 0;
    for (int u = 0; u < n; ++u) last = std::max(last, depth[u]);
    for (int u = 0; u < n; ++u)
        if (outs[u].empty() && !ins[u].empty()) depth[u] = last;
    // Columns, without empty ones.
    std::map<int, std::vector<int>> byDepth;
    for (int i = 0; i < n; ++i) byDepth[depth[i]].push_back(i);
    std::vector<std::vector<int>> columns;
    for (auto& [d, members] : byDepth) columns.push_back(std::move(members));
    // Order within columns by the neighbours' places.
    std::vector<double> place(n, 0);
    auto rank = [&] {
        for (const auto& column : columns)
            for (std::size_t k = 0; k < column.size(); ++k)
                place[column[k]] = (k + 0.5) / static_cast<double>(column.size());
    };
    rank();
    for (int sweep = 0; sweep < 4; ++sweep) {
        const bool forward = sweep % 2 == 0;
        for (std::size_t c = 0; c < columns.size(); ++c) {
            auto& column = columns[forward ? c : columns.size() - 1 - c];
            std::vector<double> key(n, 0);
            for (int u : column) {
                const auto& near = forward ? ins[u] : outs[u];
                double sum = 0;
                for (int v : near) sum += place[v];
                key[u] = near.empty() ? place[u] : sum / static_cast<double>(near.size());
            }
            std::stable_sort(column.begin(), column.end(),
                             [&](int a, int b) { return key[a] < key[b]; });
            for (std::size_t k = 0; k < column.size(); ++k)
                place[column[k]] = (k + 0.5) / static_cast<double>(column.size());
        }
    }
    // Split tall columns into stacks no taller than `limit`, wrap the row of
    // stacks into bands no wider than `wide`, and place the boxes.
    struct Stack {
        std::vector<int> members;
        double width = 0, height = 0;
    };
    struct Shape {
        std::vector<std::pair<double, double>> at;
        double width = 0, height = 0;
        int bands = 0;
    };
    auto shape = [&](double limit, double wide) {
        std::vector<Stack> stacks;
        for (const auto& column : columns) {
            Stack s;
            for (int u : column) {
                const double h =
                    s.members.empty() ? boxes[u].height : s.height + kLayoutGapY + boxes[u].height;
                if (!s.members.empty() && h > limit) {
                    stacks.push_back(std::move(s));
                    s = Stack{};
                }
                s.height =
                    s.members.empty() ? boxes[u].height : s.height + kLayoutGapY + boxes[u].height;
                s.width = std::max(s.width, boxes[u].width);
                s.members.push_back(u);
            }
            stacks.push_back(std::move(s));
        }
        std::vector<std::vector<const Stack*>> bands(1);
        double row = 0;
        for (const Stack& s : stacks) {
            if (!bands.back().empty() && row + kLayoutGapX + s.width > wide) {
                bands.emplace_back();
                row = 0;
            }
            row += (bands.back().empty() ? 0 : kLayoutGapX) + s.width;
            bands.back().push_back(&s);
        }
        Shape made;
        made.at.assign(boxes.size(), {x0, y0});
        made.bands = static_cast<int>(bands.size());
        double top = y0;
        for (const auto& band : bands) {
            double bandHeight = 0;
            for (const Stack* s : band) bandHeight = std::max(bandHeight, s->height);
            double left = x0;
            for (const Stack* s : band) {
                double y = top + std::round((bandHeight - s->height) / 2);
                for (int u : s->members) {
                    made.at[u] = {left + std::round((s->width - boxes[u].width) / 2), y};
                    y += boxes[u].height + kLayoutGapY;
                }
                left += s->width + kLayoutGapX;
            }
            made.width = std::max(made.width, left - kLayoutGapX - x0);
            made.height = top + bandHeight - y0;
            top += bandHeight + kLayoutBandGap;
        }
        return made;
    };
    // Try the heights at which a column could split and a few band widths;
    // keep the shape nearest a window's, compact, with the fewest breaks in
    // the flow.
    double area = 0, tallest = 0;
    for (const auto& b : boxes) {
        area += (b.width + kLayoutGapX) * (b.height + kLayoutGapY);
        tallest = std::max(tallest, b.height);
    }
    std::set<double> limits = {tallest};
    for (const auto& column : columns) {
        double h = -kLayoutGapY;
        for (int u : column) limits.insert(std::max(tallest, h += kLayoutGapY + boxes[u].height));
    }
    Shape best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (double limit : limits) {
        const Shape one = shape(limit, std::numeric_limits<double>::infinity());
        for (int b = 1; b <= 6; ++b) {
            const Shape s = b == 1 ? one : shape(limit, std::ceil(one.width / b));
            if (b > 1 && s.bands == 1) continue;
            const double score =
                std::abs(std::log(s.width / std::max(s.height, 1.0) / kLayoutAspect)) +
                0.5 * std::log(s.width * s.height / std::max(area, 1.0)) + 0.3 * (s.bands - 1);
            if (score < bestScore - 1e-9) {
                bestScore = score;
                best = s;
            }
        }
    }
    return best.at;
}

double clampCoordinate(double v) {
    return std::clamp(v, -model::kMaxRackCoordinate, model::kMaxRackCoordinate);
}

// The label a patch's name gives its module (rack.md §7): none when the name
// is the type's ("vco"), the type's name and a number for the type's with a
// number ("vco2", "vco_2": "VCO 2"), else the name made readable ("bass_env":
// "Bass env", "osc1": "Osc 1"), a word that is a type's key spelled as that
// type's name ("lfo": "LFO").
std::string labelFromName(const rack::Catalog& catalog, const std::string& name,
                          const rack::ModuleType* type) {
    const auto keyOfId = [](const std::string& id) { return id.substr(id.rfind('.') + 1); };
    if (type) {
        const std::string key = keyOfId(type->id());
        if (name == key) return {};
        if (name.size() > key.size() && name.compare(0, key.size(), key) == 0) {
            std::string rest = name.substr(key.size());
            if (rest[0] == '_') rest.erase(0, 1);
            if (!rest.empty() &&
                std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return type->info().name + " " + rest;
        }
    }
    // Words: split at '_' and between letters and a trailing number.
    std::vector<std::string> words;
    std::string word;
    for (char c : name) {
        if (c == '_') {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
            continue;
        }
        const bool digit = c >= '0' && c <= '9';
        if (digit && !word.empty() && !(word.back() >= '0' && word.back() <= '9')) {
            words.push_back(std::move(word));
            word.clear();
        }
        word += c;
    }
    if (!word.empty()) words.push_back(std::move(word));
    std::string out;
    for (std::size_t k = 0; k < words.size(); ++k) {
        std::string w = words[k];
        const auto* e = findType(catalog, w);
        const std::string spelled = e ? e->type->info().name : std::string();
        if (e && keyOfId(e->type->id()) == w && spelled.find(' ') == std::string::npos) {
            std::string lower = spelled;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lower == w) w = spelled;
        }
        if (k == 0 && !w.empty() && w[0] >= 'a' && w[0] <= 'z') w[0] = static_cast<char>(w[0] - 32);
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

struct RackArrangeArgs {
    std::string rack;
    std::optional<std::vector<std::string>> modules;
    std::optional<double> x, y;
};

void registerRackArrangeCommand(cmd::Registry& registry) {
    Command<RackArrangeArgs, json::Value> c;
    common(c, "rack.arrange", Kind::Edit, "Lay out a rack's modules by signal flow.");
    c.description =
        "Moves the modules (default all the rack's) as rack.patch lays out new ones: left to "
        "right by signal flow along their cables, ordered within columns so cables cross less, "
        "wrapped to about a window's shape. From x, y (default the modules' top-left corner). "
        "One undo step; returns the new places.";
    c.args = schema::object<RackArrangeArgs>("rack_arrange_args")
                 .field("rack", &RackArrangeArgs::rack, idOf("rk", "Rack"))
                 .field("modules", &RackArrangeArgs::modules, schema::arrayOf(idOf("mo", "Module")))
                 .field("x", &RackArrangeArgs::x, coordinate())
                 .field("y", &RackArrangeArgs::y, coordinate())
                 .build();
    c.examples = {{"Tidy up a rack", j(R"({"rack": "rk_1"})"),
                   j(R"({"modules": {"mo_1": {"x": 328, "y": 138},
                   "mo_2": {"x": 40, "y": 40}, "mo_3": {"x": 184, "y": 115}}})"),
                   "song_with_rack"}};
    c.errors = {{"rack.module", "a module that is not in the rack", "name modules of this rack"}};
    c.run = [](const RackArrangeArgs& a, Context& ctx) -> Result<json::Value> {
        EngineApp& e = app(ctx);
        const model::Session s = e.view();
        auto rack = rackId(s, a.rack);
        if (!rack) return std::move(rack).error();
        std::vector<Id> ids;
        if (a.modules) {
            for (std::size_t k = 0; k < a.modules->size(); ++k) {
                const auto id = Id::parse((*a.modules)[k]);
                const model::RackModule* m = id ? s.modules.find(*id) : nullptr;
                if (!m || m->rack != *rack)
                    return Error("rack.module", (*a.modules)[k] + " is not a module of " + a.rack,
                                 "/modules/" + std::to_string(k),
                                 "rack.get lists the rack's modules");
                if (std::find(ids.begin(), ids.end(), *id) == ids.end()) ids.push_back(*id);
            }
        } else {
            for (const auto& m : s.modules)
                if (m.value->rack == *rack) ids.push_back(m.id);
        }
        std::vector<LayoutBox> boxes;
        std::map<Id, int> index;  // a module or its plug-in's fx
        double left = 0, top = 0;
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const model::RackModule& m = *s.modules.find(ids[k]);
            const auto [w, h] = sizeOf(typeOfModule(e, m).get());
            boxes.push_back({w, h});
            index[ids[k]] = static_cast<int>(k);
            if (m.fx) index[*m.fx] = static_cast<int>(k);
            left = k == 0 ? m.x : std::min(left, m.x);
            top = k == 0 ? m.y : std::min(top, m.y);
        }
        std::vector<std::pair<int, int>> edges;
        for (const auto& cable : s.cables) {
            const auto from = index.find(cable.value->from.object);
            const auto to = index.find(cable.value->to.object);
            if (from != index.end() && to != index.end())
                edges.emplace_back(from->second, to->second);
        }
        const auto places = layoutBoxes(boxes, edges, a.x.value_or(left), a.y.value_or(top));
        json::Value moved = json::Value::object();
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const model::RackModule& m = *s.modules.find(ids[k]);
            const double x = clampCoordinate(places[k].first),
                         y = clampCoordinate(places[k].second);
            json::Value at = json::Value::object();
            at["x"] = x;
            at["y"] = y;
            moved[ids[k].str()] = std::move(at);
            if (x == m.x && y == m.y) continue;
            json::Value args = json::Value::object();
            args["module"] = ids[k].str();
            args["x"] = x;
            args["y"] = y;
            auto set =
                invokeCommand("module.set", std::move(args), ctx, "/modules/" + std::to_string(k));
            if (!set) return std::move(set).error();
        }
        json::Value out = json::Value::object();
        out["modules"] = std::move(moved);
        return out;
    };
    addChecked(registry, std::move(c));
}

}  // namespace

void registerRackPatchCommand(cmd::Registry& registry) {
    registerRackArrangeCommand(registry);
    Command<RackPatchArgs, json::Value> c;
    common(c, "rack.patch", Kind::Edit, "Add named modules and the cables among them in one step.");
    c.description =
        "modules: {name: {type | plugin, params?, preset?, label?, x?, y?, source?, device?}} as "
        "module.add takes them; a name is lower-case letters, digits and '_'. cables: [{from, "
        "to, amount?, color?}] or [from, to, amount?], an end \"name.port\" or \"name.knob\" "
        "(a plugin module's knob is its plug-in's: \"synth.cutoff\"), or an existing module's "
        "\"mo_N.port\" (the rack's Out from rack.add). Modules without x, y are laid out left "
        "to right by signal flow from x, y (default right of the rack's modules), as "
        "rack.arrange does. A name labels its module unless label is given: \"kick\" is "
        "Kick, \"vco2\" is VCO 2, a type's own key (\"vco\") none. Everything is checked as "
        "module.add and cable.add check it; one error changes nothing. Returns the ids by name.";
    c.args = schema::object<RackPatchArgs>("rack_patch_args")
                 .field("rack", &RackPatchArgs::rack, idOf("rk", "Rack"))
                 .field("modules", &RackPatchArgs::modules, schema::any())
                 .field("cables", &RackPatchArgs::cables, schema::arrayOf(schema::any()))
                 .field("x", &RackPatchArgs::x, coordinate())
                 .field("y", &RackPatchArgs::y, coordinate())
                 .build();
    c.examples = {{"A tremolo into the Out",
                   j(R"({"rack": "rk_1", "modules": {"osc": {"type": "vco"},
               "lfo": {"type": "lfo", "params": {"rate": 5}}, "amp": {"type": "vca"}},
               "cables": [["osc.saw", "amp.in"], ["lfo.out", "amp.level", 6],
                          {"from": "amp.out", "to": "mo_1.l"}]})"),
                   j(R"({"modules": {"osc": "mo_4", "lfo": "mo_5", "amp": "mo_6"},
               "cables": ["cb_2", "cb_3", "cb_4"]})"),
                   "song_with_rack"}};
    c.errors = {{"rack.patch", "a name, an end or a field the patch cannot use",
                 "names: lower-case letters, digits and '_'; ends: name.port"}};
    c.run = [](const RackPatchArgs& a, Context& ctx) -> Result<json::Value> {
        EngineApp& e = app(ctx);
        const model::Session s = e.view();
        auto rack = rackId(s, a.rack);
        if (!rack) return std::move(rack).error();
        if (!a.modules.isObject())
            return Error("rack.patch", "modules is an object of modules by name", "/modules",
                         "{\"osc\": {\"type\": \"vco\"}}");
        static const std::set<std::string> fields = {"type", "plugin", "params", "preset", "label",
                                                     "x",    "y",      "source", "device"};
        // Names, and each module's size for the layout.
        std::vector<std::string> names;
        std::map<std::string, std::pair<double, double>> sizes;
        for (const auto& m : a.modules.asObject()) {
            const std::string at = json::pointerJoin("/modules", m.key);
            if (!validLocalName(m.key))
                return Error("rack.patch", "\"" + m.key + "\" is not a module name", at,
                             "use 1-32 lower-case letters, digits or '_', starting with a "
                             "letter, not an id: \"osc\", \"env_2\"");
            if (!m.value.isObject())
                return Error("rack.patch", m.key + " is not an object", at,
                             "{\"type\": \"vco\", \"params\": {...}}");
            for (const auto& f : m.value.asObject())
                if (!fields.count(f.key))
                    return Error("rack.patch", m.key + " has no field " + f.key,
                                 json::pointerJoin(at, f.key),
                                 "fields: type or plugin, params, preset, label, x, y, source, "
                                 "device");
            const json::Value* type = m.value.find("type");
            std::shared_ptr<const rack::ModuleType> t;
            if (m.value.find("plugin"))
                t = pluginModuleType();
            else if (type && type->isString())
                if (auto found = typeOf(e, type->asString(), (at + "/type").c_str())) t = *found;
            sizes[m.key] = sizeOf(t.get());
            names.push_back(m.key);
        }
        if (names.empty())
            return Error("rack.patch", "no modules", "/modules", "name at least one module");

        // Cables as [from, to, amount?, color?], their ends split.
        struct End {
            std::string name, key;
        };
        struct Wire {
            End from, to;
            std::optional<double> amount;
            std::optional<std::string> color;
        };
        std::vector<Wire> wires;
        auto splitEnd = [](const json::Value* v, End& out) {
            if (!v || !v->isString()) return false;
            const std::string& text = v->asString();
            const auto dot = text.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == text.size()) return false;
            out = {text.substr(0, dot), text.substr(dot + 1)};
            return true;
        };
        for (std::size_t k = 0; a.cables && k < a.cables->size(); ++k) {
            const json::Value& entry = (*a.cables)[k];
            const std::string at = "/cables/" + std::to_string(k);
            Wire w;
            const json::Value *from = nullptr, *to = nullptr, *amount = nullptr, *color = nullptr;
            if (entry.isArray()) {
                const auto& v = entry.asArray();
                if (v.size() >= 2) {
                    from = &v[0];
                    to = &v[1];
                }
                if (v.size() >= 3) amount = &v[2];
                if (v.size() >= 4) color = &v[3];
                if (v.size() > 4) from = nullptr;
            } else if (entry.isObject()) {
                from = entry.find("from");
                to = entry.find("to");
                amount = entry.find("amount");
                color = entry.find("color");
            }
            if (!splitEnd(from, w.from) || !splitEnd(to, w.to))
                return Error("rack.patch", "a cable is [from, to, amount?] or {from, to, ...}", at,
                             "ends are \"name.port\": [\"osc.saw\", \"amp.in\"]");
            if (amount && !amount->isNull()) {
                if (!amount->isNumber())
                    return Error("rack.patch", "amount is a number", at + "/amount",
                                 "the knob's movement at a signal of 1");
                w.amount = amount->asDouble();
            }
            if (color && !color->isNull()) {
                if (!color->isString())
                    return Error("rack.patch", "color is a string", at + "/color",
                                 "a track colour or \"#rrggbb\"");
                w.color = color->asString();
            }
            for (const End* end : {&w.from, &w.to})
                if (!sizes.count(end->name) && !Id::parse(end->name))
                    return Error("rack.patch", "no module named " + end->name, at,
                                 "name a module of this patch or give a module's id (mo_N)");
            wires.push_back(std::move(w));
        }

        // The layout (rack.md §7.2) of the modules without a place.
        std::vector<std::string> placed;
        std::map<std::string, int> slot;
        for (const auto& n : names) {
            const json::Value& spec = *a.modules.find(n);
            if (spec.find("x") && spec.find("y")) continue;
            slot[n] = static_cast<int>(placed.size());
            placed.push_back(n);
        }
        std::vector<LayoutBox> boxes;
        for (const auto& n : placed) boxes.push_back({sizes[n].first, sizes[n].second});
        std::vector<std::pair<int, int>> edges;
        for (const Wire& w : wires)
            if (slot.count(w.from.name) && slot.count(w.to.name))
                edges.emplace_back(slot[w.from.name], slot[w.to.name]);
        const auto place = freePlace(e, s, *rack);
        const auto places =
            layoutBoxes(boxes, edges, a.x.value_or(place.first), a.y.value_or(place.second));

        std::map<std::string, Id> made;
        std::map<std::string, std::string> fxOf;
        json::Value ids = json::Value::object();
        for (const auto& n : names) {
            const json::Value& spec = *a.modules.find(n);
            json::Value args = spec;
            args["rack"] = a.rack;
            if (const auto at = slot.find(n); at != slot.end()) {
                if (!spec.find("x")) args["x"] = clampCoordinate(places[at->second].first);
                if (!spec.find("y")) args["y"] = clampCoordinate(places[at->second].second);
            }
            if (!spec.find("label") && !spec.find("plugin")) {
                const json::Value* type = spec.find("type");
                const auto* entry =
                    type && type->isString() ? findType(catalog(e), type->asString()) : nullptr;
                if (std::string label =
                        labelFromName(catalog(e), n, entry ? entry->type.get() : nullptr);
                    !label.empty())
                    args["label"] = std::move(label);
            }
            auto added =
                invokeCommand("module.add", std::move(args), ctx, json::pointerJoin("/modules", n));
            if (!added) return std::move(added).error();
            made[n] = *Id::parse((*added)["module"].asString());
            if (const auto* fx = added->find("fx")) fxOf[n] = fx->asString();
            ids[n] = made[n].str();
        }
        static const std::set<std::string> pluginPorts = {"in_l",     "in_r",  "pitch", "gate",
                                                          "velocity", "out_l", "out_r"};
        auto endText = [&](const End& end, bool to) {
            if (auto it = made.find(end.name); it != made.end()) {
                if (to && fxOf.count(end.name) && !pluginPorts.count(end.key))
                    return fxOf[end.name] + "." + end.key;
                return it->second.str() + "." + end.key;
            }
            return end.name + "." + end.key;
        };
        json::Value cables = json::Value::array();
        for (std::size_t k = 0; k < wires.size(); ++k) {
            json::Value args = json::Value::object();
            args["from"] = endText(wires[k].from, false);
            args["to"] = endText(wires[k].to, true);
            if (wires[k].amount) args["amount"] = *wires[k].amount;
            if (wires[k].color) args["color"] = *wires[k].color;
            auto added =
                invokeCommand("cable.add", std::move(args), ctx, "/cables/" + std::to_string(k));
            if (!added) return std::move(added).error();
            cables.push_back((*added)["cable"].asString());
        }
        json::Value out = json::Value::object();
        out["modules"] = std::move(ids);
        out["cables"] = std::move(cables);
        return out;
    };
    addChecked(registry, std::move(c));
}

}  // namespace supple::engine
