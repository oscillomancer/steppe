// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/views/mouse_map.h"
#include <supple/base/paths.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/node.h"

namespace supple::views {
namespace {

constexpr unsigned kMods = ui::kShift | ui::kCtrl | ui::kAlt;

const std::vector<MouseMap::Context>& table() {
    static const std::vector<MouseMap::Context> t = {
        {"item.drag",
         "Drag an item",
         {"move", "move_free", "copy", "copy_free", "slip", "slip_free"}},
        {"item.edge", "Drag an item's edge", {"trim", "trim_free", "stretch", "stretch_free"}},
        {"item.click", "Click an item", {"select", "toggle", "extend"}},
        {"autoitem.edge",
         "Drag an automation item's edge",
         {"stretch", "stretch_free", "loop", "loop_free"}},
        {"fade", "Press a fade handle", {"drag", "drag_free", "cycle_shape"}},
        {"lane.drag", "Drag in empty lane space", {"select", "add"}},
        {"envelope", "Press an automation lane", {"edit", "edit_free", "toggle", "delete"}},
        {"ruler.marker", "Press a marker or region", {"move", "move_free", "remove"}},
        {"ruler.tempo", "Click a tempo point", {"none", "toggle_ramp"}},
        {"tcp.wheel",
         "Wheel over the track control panel",
         {"scroll", "resize_all", "scroll_time", "zoom"}},
        {"arrange.wheel",
         "Wheel over the arrangement (lanes and ruler)",
         {"zoom", "scroll_time", "scroll", "resize_all"}},
        {"midi.wheel",
         "Wheel over the MIDI editor",
         {"zoom", "scroll_time", "scroll_pitch", "zoom_rows"}},
    };
    return t;
}

struct Default {
    std::string_view context;
    unsigned mods;
    std::string_view behaviour;
};
constexpr std::array kDefaults = {
    Default{"item.drag", ui::kShift, "move_free"},
    Default{"item.drag", ui::kCtrl, "copy"},
    Default{"item.drag", ui::kCtrl | ui::kShift, "copy_free"},
    Default{"item.drag", ui::kAlt, "slip"},
    Default{"item.drag", ui::kAlt | ui::kShift, "slip_free"},
    Default{"item.edge", ui::kShift, "trim_free"},
    Default{"item.edge", ui::kAlt, "stretch"},
    Default{"item.edge", ui::kAlt | ui::kShift, "stretch_free"},
    Default{"item.click", ui::kCtrl, "toggle"},
    Default{"item.click", ui::kShift, "extend"},
    Default{"autoitem.edge", ui::kShift, "stretch_free"},
    Default{"autoitem.edge", ui::kAlt, "loop"},
    Default{"autoitem.edge", ui::kAlt | ui::kShift, "loop_free"},
    Default{"fade", ui::kShift, "drag_free"},
    Default{"fade", ui::kCtrl, "cycle_shape"},
    Default{"lane.drag", ui::kShift, "add"},
    Default{"lane.drag", ui::kCtrl, "add"},
    Default{"envelope", ui::kShift, "edit_free"},
    Default{"envelope", ui::kCtrl, "toggle"},
    Default{"envelope", ui::kAlt, "delete"},
    Default{"ruler.marker", ui::kShift, "move_free"},
    Default{"ruler.marker", ui::kAlt, "remove"},
    Default{"ruler.tempo", ui::kAlt, "toggle_ramp"},
    Default{"tcp.wheel", ui::kCtrl, "resize_all"},
    Default{"tcp.wheel", ui::kAlt, "resize_all"},
    Default{"arrange.wheel", ui::kShift, "scroll_time"},
    Default{"arrange.wheel", ui::kCtrl, "scroll"},
    Default{"arrange.wheel", ui::kAlt, "resize_all"},
    Default{"midi.wheel", ui::kShift, "scroll_time"},
    Default{"midi.wheel", ui::kCtrl, "scroll_pitch"},
    Default{"midi.wheel", ui::kAlt, "zoom_rows"},
};

constexpr std::array<unsigned, 8> kCombos = {0,
                                             ui::kShift,
                                             ui::kCtrl,
                                             ui::kAlt,
                                             ui::kCtrl | ui::kShift,
                                             ui::kAlt | ui::kShift,
                                             ui::kCtrl | ui::kAlt,
                                             ui::kCtrl | ui::kAlt | ui::kShift};

}  // namespace

std::span<const MouseMap::Context> MouseMap::contexts() { return table(); }

const MouseMap::Context* MouseMap::context(std::string_view id) {
    for (const Context& c : table())
        if (c.id == id) return &c;
    return nullptr;
}

std::span<const unsigned> MouseMap::combos() { return kCombos; }

std::string MouseMap::comboName(unsigned mods) {
    std::string out;
    auto add = [&](const char* name) {
        if (!out.empty()) out += '+';
        out += name;
    };
    if (mods & ui::kCtrl) add("ctrl");
    if (mods & ui::kAlt) add("alt");
    if (mods & ui::kShift) add("shift");
    return out;
}

std::optional<unsigned> MouseMap::comboFrom(std::string_view name) {
    for (unsigned c : kCombos)
        if (comboName(c) == name) return c;
    return std::nullopt;
}

MouseMap::MouseMap() {
    for (const Default& d : kDefaults) map_[std::string(d.context)][d.mods] = d.behaviour;
}

std::string_view MouseMap::behaviour(std::string_view context, unsigned mods) const {
    const Context* c = MouseMap::context(context);
    if (!c) return {};
    if (const auto it = map_.find(context); it != map_.end()) {
        if (const auto b = it->second.find(mods & kMods); b != it->second.end()) return b->second;
        if (const auto plain = it->second.find(0); plain != it->second.end()) return plain->second;
    }
    return c->behaviours.front();
}

bool MouseMap::bound(std::string_view context, unsigned mods) const {
    const auto it = map_.find(context);
    return it != map_.end() && it->second.contains(mods & kMods);
}

Status MouseMap::set(std::string_view context, unsigned mods, std::string_view behaviour) {
    const Context* c = MouseMap::context(context);
    if (!c) {
        std::string known;
        for (const Context& k : table()) known += (known.empty() ? "" : ", ") + std::string(k.id);
        return Error("mouse.context", "unknown mouse context \"" + std::string(context) + "\"",
                     "/contexts", "one of " + known);
    }
    if (behaviour.empty()) {
        map_[std::string(context)].erase(mods & kMods);
        return {};
    }
    if (std::find(c->behaviours.begin(), c->behaviours.end(), behaviour) == c->behaviours.end()) {
        std::string known;
        for (std::string_view b : c->behaviours)
            known += (known.empty() ? "" : ", ") + std::string(b);
        return Error(
            "mouse.behaviour",
            "\"" + std::string(behaviour) + "\" is not a " + std::string(context) + " behaviour",
            "/contexts/" + std::string(context), "one of " + known);
    }
    map_[std::string(context)][mods & kMods] = std::string(behaviour);
    return {};
}

json::Value MouseMap::toJson() const {
    const MouseMap defaults;
    json::Value doc = json::Value::object(), ctxs = json::Value::object();
    doc["schema"] = "supple.mouse/1.0";
    for (const Context& c : table()) {
        json::Value entry = json::Value::object();
        for (unsigned combo : kCombos) {
            const bool mine = bound(c.id, combo), theirs = defaults.bound(c.id, combo);
            const std::string_view b = behaviour(c.id, combo);
            if (mine == theirs && b == defaults.behaviour(c.id, combo)) continue;
            // "" in the file clears a default binding back to the plain press.
            entry[comboName(combo)] = mine ? std::string(b) : std::string();
        }
        if (!entry.asObject().empty()) ctxs[std::string(c.id)] = std::move(entry);
    }
    doc["contexts"] = std::move(ctxs);
    return doc;
}

Result<MouseMap> MouseMap::fromJson(const json::Value& doc) {
    const json::Value* schema = doc.isObject() ? doc.find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != "supple.mouse/1.0")
        return Error("mouse.schema", "not a supple.mouse/1.0 document", "/schema");
    MouseMap m;
    const json::Value* ctxs = doc.find("contexts");
    if (!ctxs) return m;
    if (!ctxs->isObject())
        return Error("mouse.contexts", "contexts maps a context to its combinations", "/contexts");
    for (const auto& [id, entry] : ctxs->asObject()) {
        // Retired: the wheel zoom's anchor now goes with the follow mode
        // (gui.md §4.3); an old file's setting is dropped.
        if (id == "wheel.zoom") continue;
        if (!entry.isObject())
            return Error("mouse.context", "a context maps combinations to behaviours",
                         "/contexts/" + id);
        for (const auto& [combo, value] : entry.asObject()) {
            const auto mods = comboFrom(combo);
            if (!mods)
                return Error("mouse.combo", "unknown modifier combination \"" + combo + "\"",
                             "/contexts/" + id + "/" + combo,
                             "\"\", shift, ctrl, alt, ctrl+shift, alt+shift, ctrl+alt or "
                             "ctrl+alt+shift");
            if (!value.isString())
                return Error("mouse.behaviour", "a behaviour is a string",
                             "/contexts/" + id + "/" + combo);
            // Retired proportional height zoom falls back to the current default.
            if (id == "tcp.wheel" && value.asString() == "zoom_height") continue;
            if (auto r = m.set(id, *mods, value.asString()); !r) {
                Error e = r.error();
                e.path = "/contexts/" + id + "/" + combo;
                return e;
            }
        }
    }
    return m;
}

Result<MouseMap> MouseMap::load(const std::filesystem::path& file) {
    if (file.empty()) return MouseMap{};
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec) return MouseMap{};
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    return fromJson(*doc);
}

Status MouseMap::save(const std::filesystem::path& file) const {
    if (file.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(toJson()) + "\n");
}

std::filesystem::path MouseMap::defaultFile(std::string_view slug) {
    if (const auto root = paths::config(); !root.empty()) return root / slug / "mouse.json";
    return {};
}

}  // namespace supple::views
