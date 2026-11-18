// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "migration.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace supple::model {
namespace {
bool nonempty(const json::Value* v) { return v && v->isArray() && v->size() != 0; }
std::string text(const json::Value* v) { return v && v->isString() ? v->asString() : ""; }
void erase(json::Value& v, std::string_view key) {
    if (v.isObject()) v.asObject().erase(key);
}
bool flag(const json::Value* v) { return v && v->isBool() && v->asBool(); }
}  // namespace

Result<json::Value> migrateProject1(json::Value doc, std::vector<schema::Warning>* warnings) {
    auto warn = [&](std::string message, std::string path) {
        if (warnings)
            warnings->push_back({"project.migration", std::move(message), std::move(path)});
    };
    const auto* originalTracks = doc.find("tracks");
    const auto* originalProject = doc.find("project");
    if (!originalTracks || !originalTracks->isObject() || !originalProject ||
        !originalProject->isObject())
        return Error("project.migration", "1.x needs project and tracks objects", "",
                     "restore the project settings and track map before migrating");
    if (const auto* counters = originalProject->find("next_ids"); counters && !counters->isObject())
        return Error("project.migration", "next_ids must be an object", "/project/next_ids");
    json::Value tracks = *originalTracks;
    json::Value project = *originalProject;
    // 1.x predates master playrate. Its similarly named extensions cannot
    // override the new factory defaults during migration.
    project["playrate"] = toJson(Playrate{});
    json::Value routes = doc.find("routes") ? *doc.find("routes") : json::Value::object();
    json::Value items = doc.find("items") ? *doc.find("items") : json::Value::object();
    json::Value envelopes = doc.find("envelopes") ? *doc.find("envelopes") : json::Value::object();
    std::map<Id, StripType> types;
    std::uint64_t next = 1;
    if (const auto* counters = project.find("next_ids"))
        if (const auto* n = counters->find("tr"); n && n->isInt() && n->asInt() > 0)
            next = static_cast<std::uint64_t>(n->asInt());
    for (const auto& e : tracks.asObject()) {
        auto id = Id::parse(e.key);
        if (!id || id->kind != Kind::Track || !e.value.isObject())
            return Error("project.migration", "invalid legacy track", "/tracks/" + e.key,
                         "use a track id and object");
        next = std::max(next, std::uint64_t(id->n) + 1);
        types[*id] = *id == kMaster                       ? StripType::Master
                     : nonempty(e.value.find("children")) ? StripType::Buss
                                                          : StripType::Channel;
    }
    if (routes.isObject())
        for (const auto& e : routes.asObject()) {
            const auto* from = e.value.find("from");
            const auto* to = e.value.find("to");
            // Hardware record inputs stay on channels; only strip receives sum audio.
            if (!from || !to || !from->find("track") || to->find("fx") ||
                (e.value.find("midi") && !e.value.find("midi")->isNull()))
                continue;
            auto id = Id::parse(text(to->find("track")));
            if (id && types.contains(*id) && *id != kMaster) types[*id] = StripType::Buss;
        }
    if (next > std::numeric_limits<std::uint32_t>::max())
        return Error("project.migration", "track counter is out of range", "/project/next_ids/tr");
    std::map<std::string, std::string> movedInputs;
    for (const auto& [id, type] : types) {
        const std::string name = id.str();
        json::Value tr = *tracks.find(name);
        tr["type"] = stripTypeName(type);
        if (type == StripType::Master) tr["parent"] = nullptr;
        for (const auto& [key, def] : std::initializer_list<std::pair<const char*, json::Value>>{
                 {"width", 100.0}, {"trim", 0.0}, {"trim_db", 0.0}}) {
            if (const auto* value = tr.find(key)) {
                if (*value != def)
                    warn("dropped track " + std::string(key) + "; use a utility insert",
                         "/tracks/" + name + "/" + key);
                erase(tr, key);
            }
        }
        bool hasItems = false;
        if (items.isObject())
            for (const auto& e : items.asObject()) hasItems |= text(e.value.find("track")) == name;
        const auto* record = tr.find("record");
        bool hasRecord =
            record &&
            (flag(record->find("armed")) || !text(record->find("input")).empty() ||
             (!text(record->find("monitor")).empty() && text(record->find("monitor")) != "off") ||
             (!text(record->find("mode")).empty() && text(record->find("mode")) != "input"));
        if (type != StripType::Channel &&
            (hasItems || hasRecord || nonempty(tr.find("input_chain")))) {
            if (next >= std::numeric_limits<std::uint32_t>::max())
                return Error("project.migration", "no track ids left for a child channel",
                             "/project/next_ids/tr");
            const std::string childId = Id{Kind::Track, static_cast<std::uint32_t>(next++)}.str();
            json::Value child = json::Value::object();
            child["name"] = text(tr.find("name")) + " items";
            child["parent"] = name;
            child["type"] = "channel";
            for (const char* key : {"record", "input_chain", "timebase"})
                if (const auto* v = tr.find(key)) child[key] = *v;
            // Stereo sum strips; preserve mono recording/items on the new channel.
            if (const auto* v = tr.find("channels")) child["channels"] = *v;
            erase(tr, "record");
            erase(tr, "input_chain");
            if (items.isObject())
                for (auto& e : items.asObject())
                    if (text(e.value.find("track")) == name) e.value["track"] = childId;
            movedInputs[name] = childId;
            json::Value children =
                tr.find("children") ? *tr.find("children") : json::Value::array();
            if (!children.isArray())
                return Error("project.migration", "children must be an array",
                             "/tracks/" + name + "/children");
            children.asArray().insert(children.asArray().begin(), childId);
            tr["children"] = std::move(children);
            tracks[childId] = std::move(child);
            warn("moved items and record/input FX to first child " + childId, "/tracks/" + name);
        }
        if (type != StripType::Channel) tr["channels"] = 2;
        tracks[name] = std::move(tr);
    }
    std::set<std::string> droppedRoutes;
    if (routes.isObject()) {
        std::vector<std::string> ids;
        for (const auto& e : routes.asObject()) ids.push_back(e.key);
        for (const auto& id : ids) {
            auto r = *routes.find(id);
            const auto* from = r.find("from");
            const auto* to = r.find("to");
            if (!from || !to) continue;  // schema validation reports malformed endpoints
            const std::string source = text(from->find("track"));
            std::string target = text(to->find("track"));
            const bool midi =
                (r.find("midi") && !r.find("midi")->isNull()) || from->find("midi_device");
            if (movedInputs.contains(target) && (from->find("hw_channels") || midi)) {
                r["to"]["track"] = movedInputs.at(target);
                target = movedInputs.at(target);
            }
            const auto* src = tracks.find(source);
            const auto* dst = tracks.find(target);
            const std::string srcType = src ? text(src->find("type")) : "";
            const std::string dstType = dst ? text(dst->find("type")) : "";
            bool invalid = false;
            if (!midi) {
                if (to->find("fx"))
                    invalid = source.empty();
                else if (to->find("hw_channels"))
                    invalid = srcType != "master";
                else if (!target.empty())
                    invalid = srcType == "master" ||
                              (dstType == "channel" ? !from->find("hw_channels") : source.empty());
            }
            if (invalid) {
                erase(routes, id);
                droppedRoutes.insert(id);
                warn("dropped route incompatible with track types", "/routes/" + id);
            } else {
                // The legacy default was numeric centre, not follow.
                if (!r.find("pan")) r["pan"] = 0.0;
                routes[id] = std::move(r);
            }
        }
    }
    std::set<std::string> droppedEnvelopes;
    if (envelopes.isObject()) {
        std::vector<std::string> ids;
        for (const auto& e : envelopes.asObject()) ids.push_back(e.key);
        for (const auto& id : ids) {
            const auto& e = *envelopes.find(id);
            const auto* target = e.find("target");
            if (!target) continue;
            const std::string object = text(target->find("object"));
            const std::string param = text(target->find("param"));
            if (droppedRoutes.contains(object) ||
                (object.starts_with("tr_") &&
                 (param == "trim" || param == "width" || param == "phase_invert"))) {
                erase(envelopes, id);
                droppedEnvelopes.insert(id);
                warn("dropped envelope on a removed track parameter or route", "/envelopes/" + id);
            }
        }
    }
    if (auto* automation = doc.asObject().find("automation_items");
        automation && automation->isObject()) {
        std::vector<std::string> ids;
        for (const auto& e : automation->asObject())
            if (droppedEnvelopes.contains(text(e.value.find("envelope")))) ids.push_back(e.key);
        for (const auto& id : ids) {
            erase(*automation, id);
            warn("dropped automation item on a removed envelope", "/automation_items/" + id);
        }
    }
    for (const char* key : {"pan_law", "pan_law_db"}) {
        if (project.find(key))
            warn("dropped project pan law; tracks use Console 9's law",
                 "/project/" + std::string(key));
        erase(project, key);
    }
    project["console9"] = true;
    project["masters"] = json::Array{json::Value("tr_0")};
    project["next_ids"]["tr"] = next;
    if (auto* render = project.asObject().find("render"); render && render->isObject())
        if (auto* matrix = render->asObject().find("matrix"); matrix && matrix->isObject())
            for (auto& row : matrix->asObject())
                if (row.value.isArray())
                    for (auto& cell : row.value.asArray())
                        if (text(&cell) == "master") cell = "tr_0";
    warn("Console 9 enabled: levels now follow Console 9 staging", "/project/console9");
    doc["schema"] = kProjectSchema;
    doc["project"] = std::move(project);
    doc["tracks"] = std::move(tracks);
    doc["items"] = std::move(items);
    doc["routes"] = std::move(routes);
    doc["envelopes"] = std::move(envelopes);
    return doc;
}
}  // namespace supple::model
