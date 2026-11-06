// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "control_map.h"
#include <supple/base/paths.h>

#include <algorithm>
#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/model/schemas.h"

namespace supple::gui {
namespace {

constexpr std::string_view kSchema = "supple.controls/1.0";

// One binding: {action, midi, mode, soft_takeover}, the last two omitted at
// their defaults. Members it does not know are left alone.
const schema::Schema<ActionControl>& bindingSchema() {
    static const schema::Schema<ActionControl> s = [] {
        auto b = schema::object<ActionControl>("action_control");
        b.unknownMembers(schema::UnknownMembers::Ignore);
        b.field("action", &ActionControl::action, schema::string().minLength(1));
        return model::controlBindingFields(b).build();
    }();
    return s;
}

// The same control, wherever it is heard from (the device is part of it).
bool sameControl(const model::MidiControl& a, const model::MidiControl& b) {
    return a.kind == b.kind && a.channel == b.channel && a.number == b.number &&
           a.device == b.device;
}

}  // namespace

std::filesystem::path ControlMap::defaultFile(std::string_view slug) {
    if (const auto root = paths::config(); !root.empty()) return root / slug / "controls.json";
    return {};
}

Result<ControlMap> ControlMap::load(const std::filesystem::path& file) {
    ControlMap out;
    if (file.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec) return out;
    auto text = readTextFile(file);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != kSchema)
        return Error("controls.schema", file.string() + " is not a supple.controls/1.0 document",
                     "/schema", "move the file aside; the app writes a new one");
    const json::Value* list = doc->find("bindings");
    if (!list) return out;
    if (!list->isArray())
        return Error("controls.bindings", "bindings must be an array", "/bindings",
                     "use [{action, midi, mode, soft_takeover}, …]");
    for (std::size_t i = 0; i < list->asArray().size(); ++i) {
        auto binding = bindingSchema().decode(list->asArray()[i]);
        if (!binding) {  // the member's path, within the file
            Error e = std::move(binding).error();
            const std::string at = "/bindings/" + std::to_string(i);
            e.path = at + e.path;
            for (Error& detail : e.details) detail.path = at + detail.path;
            return e;
        }
        out.bind(std::move(*binding));
    }
    return out;
}

Status ControlMap::save(const std::filesystem::path& file) const {
    json::Value doc = json::Value::object();
    doc["schema"] = std::string(kSchema);
    json::Value list = json::Value::array();
    std::vector<ActionControl> sorted = bindings_;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.action < b.action; });
    for (const ActionControl& b : sorted) list.push_back(bindingSchema().encode(b));
    doc["bindings"] = std::move(list);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

std::string ControlMap::bind(ActionControl binding) {
    std::string had;
    for (const ActionControl& b : bindings_)
        if (sameControl(b.control, binding.control) && b.action != binding.action) had = b.action;
    std::erase_if(bindings_,
                  [&](const ActionControl& b) { return sameControl(b.control, binding.control); });
    bindings_.push_back(std::move(binding));
    return had;
}

void ControlMap::unbindAction(std::string_view action) {
    std::erase_if(bindings_, [&](const ActionControl& b) { return b.action == action; });
}

std::vector<ActionControl> ControlMap::bindingsFor(std::string_view action) const {
    std::vector<ActionControl> out;
    for (const ActionControl& b : bindings_)
        if (b.action == action) out.push_back(b);
    return out;
}

}  // namespace supple::gui
