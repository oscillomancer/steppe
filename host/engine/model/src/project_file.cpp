// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/project_file.h"

#include <charconv>
#include <set>

#include "supple/base/sha256.h"
#include "supple/model/schemas.h"

namespace supple::model {

namespace {

constexpr std::string_view kSection = "branches";

// How deep a section's objects sit: members of these are objects by ID (or
// blobs by hash), of these one level further down; anything else is one value.
int objectDepth(std::string_view section) {
    static const std::set<std::string_view> keyed{
        "tracks",           "items",  "takes", "fx",    "routes",  "envelopes",
        "automation_items", "groups", "racks", "blobs", "modules", "cables"};
    static const std::set<std::string_view> nested{"tempo", "rulers", "sources"};
    if (keyed.contains(section)) return 1;
    if (nested.contains(section)) return 2;
    return 0;
}

void diffAt(const std::string& path, const json::Value* main, const json::Value* doc, int depth,
            json::Value& set, json::Value& remove) {
    if (!doc) {
        remove.push_back(path);
        return;
    }
    if (main && *main == *doc) return;
    if (!main || depth == 0 || !main->isObject() || !doc->isObject()) {
        set[path] = *doc;
        return;
    }
    for (const auto& m : main->asObject())
        diffAt(json::pointerJoin(path, m.key), &m.value, doc->find(m.key), depth - 1, set, remove);
    for (const auto& m : doc->asObject())
        if (!main->find(m.key))
            diffAt(json::pointerJoin(path, m.key), nullptr, &m.value, depth - 1, set, remove);
}

// The objects in which `doc` differs from `main`: {set: {pointer: value},
// remove: [pointer]}, each member omitted when empty.
json::Value stateDelta(const json::Value& main, const json::Value& doc) {
    json::Value set = json::Value::object(), remove = json::Value::array();
    for (const auto& m : main.asObject())
        if (m.key != "schema")
            diffAt(json::pointerJoin("", m.key), &m.value, doc.find(m.key), objectDepth(m.key), set,
                   remove);
    for (const auto& m : doc.asObject())
        if (m.key != "schema" && !main.find(m.key))
            diffAt(json::pointerJoin("", m.key), nullptr, &m.value, objectDepth(m.key), set,
                   remove);
    json::Value out = json::Value::object();
    if (set.size()) out["set"] = std::move(set);
    if (remove.size()) out["remove"] = std::move(remove);
    return out;
}

Result<json::Value> applyDelta(json::Value doc, const json::Value& delta, const std::string& path) {
    auto bad = [&](std::string message, std::string where) {
        return Error("history.saved", std::move(message), path + where,
                     "the branches section is damaged; main still opens without it");
    };
    if (!delta.isObject()) return bad("expected a state object", "");
    for (const auto& m : delta.asObject())
        if (m.key != "set" && m.key != "remove") return bad("unknown member", "/" + m.key);
    auto walk = [&](std::string_view pointer,
                    bool create) -> Result<std::pair<json::Value*, std::string>> {
        auto tokens = json::pointerSplit(pointer);
        if (!tokens || tokens->empty() || tokens->size() > 3)
            return Error("history.saved", "not an object pointer: " + std::string(pointer));
        json::Value* at = &doc;
        for (std::size_t i = 0; i + 1 < tokens->size(); ++i) {
            if (!at->isObject())
                return Error("history.saved", "no object at " + std::string(pointer));
            json::Value* next = at->asObject().find((*tokens)[i]);
            if (!next) {
                if (!create) return Error("history.saved", "nothing at " + std::string(pointer));
                next = &((*at)[(*tokens)[i]] = json::Value::object());
            }
            at = next;
        }
        if (!at->isObject()) return Error("history.saved", "no object at " + std::string(pointer));
        return std::pair{at, tokens->back()};
    };
    if (const auto* remove = delta.find("remove")) {
        if (!remove->isArray()) return bad("expected an array", "/remove");
        for (std::size_t i = 0; i < remove->size(); ++i) {
            const auto& pointer = remove->at(i);
            if (!pointer.isString())
                return bad("expected a pointer", "/remove/" + std::to_string(i));
            auto at = walk(pointer.asString(), false);
            if (!at || !at->first->asObject().erase(at->second))
                return bad("nothing to remove at " + pointer.asString(),
                           "/remove/" + std::to_string(i));
        }
    }
    if (const auto* set = delta.find("set")) {
        if (!set->isObject()) return bad("expected an object", "/set");
        for (const auto& m : set->asObject()) {
            auto at = walk(m.key, true);
            if (!at) return bad(at.error().message, json::pointerJoin("/set", m.key));
            (*at->first)[at->second] = m.value;
        }
    }
    return doc;
}

json::Value countersJson(const Counters& c) {
    json::Value out = json::Value::object();
    for (std::size_t k = 0; k < kKindCount; ++k)
        out[std::string(prefix(static_cast<Kind>(k)))] = c.next[k];
    return out;
}

const char* opName(Change::Op op) {
    switch (op) {
        case Change::Op::Add:
            return "add";
        case Change::Op::Remove:
            return "remove";
        case Change::Op::Update:
            break;
    }
    return "update";
}

json::Value idList(const std::vector<Id>& ids) {
    json::Value out = json::Value::array();
    for (Id id : ids) out.push_back(id.str());
    return out;
}

json::Value versionJson(const Version& v, std::uint64_t mainHead, const json::Value& mainDoc) {
    json::Value e = json::Value::object();
    json::Value parents = json::Value::array();
    for (auto p : v.parents) parents.push_back(p);
    e["parents"] = std::move(parents);
    e["label"] = v.txn.label;
    if (!v.txn.origin.empty()) e["origin"] = v.txn.origin;
    if (v.timeMs) e["time_ms"] = v.timeMs;
    if (!v.txn.invocations.empty()) {
        json::Value list = json::Value::array();
        for (const auto& i : v.txn.invocations) {
            json::Value inv = json::Value::object();
            inv["command"] = i.command;
            inv["args"] = i.args;
            list.push_back(std::move(inv));
        }
        e["invocations"] = std::move(list);
    }
    if (!v.changes.empty()) {
        json::Value list = json::Value::array();
        for (const auto& c : v.changes) {
            json::Value pair = json::Value::array();
            pair.push_back(opName(c.op));
            pair.push_back(c.id.str());
            list.push_back(std::move(pair));
        }
        e["changes"] = std::move(list);
    }
    if (!v.created.empty()) e["created"] = idList(v.created);
    if (v.squashedSteps) e["squashed_steps"] = v.squashedSteps;
    if (!v.squashedFields.empty()) {
        json::Value fields = json::Value::object();
        for (const auto& [id, names] : v.squashedFields) {
            json::Value list = json::Value::array();
            for (const auto& n : names) list.push_back(n);
            fields[id.str()] = std::move(list);
        }
        e["squashed_fields"] = std::move(fields);
    }
    if (v.replayOf) e["replay_of"] = v.replayOf;
    if (!v.mergeRemap.empty()) {
        json::Value remap = json::Value::object();
        for (const auto& [from, to] : v.mergeRemap) remap[from.str()] = to.str();
        e["merge_remap"] = std::move(remap);
    }
    if (v.id != mainHead) e["state"] = stateDelta(mainDoc, projectToJson(*v.session));
    return e;
}

// --- decoding ----------------------------------------------------------------------------

struct Reader {
    std::string base;  // the JSON pointer of what is read
    Error fail(std::string message, std::string_view where = {}) const {
        return Error("history.saved", std::move(message), base + std::string(where),
                     "the branches section is damaged; main still opens without it");
    }
};

Result<std::uint64_t> unsignedAt(const json::Value& v, const std::string& path) {
    if (!v.isInt() || v.asInt() < 0)
        return Error("history.saved", "expected a non-negative integer", path,
                     "the branches section is damaged; main still opens without it");
    return static_cast<std::uint64_t>(v.asInt());
}

Result<Id> idAt(const json::Value& v, const std::string& path) {
    auto id = v.isString() ? Id::parse(v.asString()) : std::nullopt;
    if (!id)
        return Error("history.saved", "expected an object id", path,
                     "the branches section is damaged; main still opens without it");
    return *id;
}

Result<std::vector<Id>> idsAt(const json::Value* v, const std::string& path) {
    std::vector<Id> out;
    if (!v) return out;
    if (!v->isArray())
        return Error("history.saved", "expected an array of ids", path,
                     "the branches section is damaged; main still opens without it");
    for (std::size_t i = 0; i < v->size(); ++i) {
        auto id = idAt(v->at(i), path + "/" + std::to_string(i));
        if (!id) return std::move(id).error();
        out.push_back(*id);
    }
    return out;
}

Result<Counters> countersAt(const json::Value* v, const std::string& path) {
    Counters c;
    if (!v || !v->isObject())
        return Error("history.saved", "expected next_ids", path,
                     "the branches section is damaged; main still opens without it");
    for (const auto& m : v->asObject()) {
        const auto kind = kindFromPrefix(m.key);
        auto n = unsignedAt(m.value, json::pointerJoin(path, m.key));
        if (!n) return std::move(n).error();
        if (!kind || *n > UINT32_MAX)
            return Error("history.saved", "not an id counter", json::pointerJoin(path, m.key),
                         "the branches section is damaged; main still opens without it");
        c[*kind] = static_cast<std::uint32_t>(*n);
    }
    return c;
}

// Objects equal to their first parent's share its entry and revision; the
// others take the version's own number. Merges compare revisions to tell an
// untouched object from a changed one, and files do not store them.
template <class T>
void shareRevisions(IdMap<T>& map, const IdMap<T>* parent, std::uint64_t rev) {
    typename IdMap<T>::Builder b(map);
    bool same = parent && parent->size() == map.size();
    for (const auto& e : map) {
        const auto* p = parent ? parent->entry(e.id) : nullptr;
        if (p && toJson(*p->value) == toJson(*e.value)) {
            b.put(e.id, p->rev, p->value);
        } else {
            b.put(e.id, rev, e.value);
            same = false;
        }
    }
    map = same ? *parent : b.build();
}

void shareRevisions(Session& s, const Session* p) {
    const auto rev = s.version;
    auto each = [&](auto member) {
        auto& map = s.*member;
        shareRevisions(map, p ? &(p->*member) : nullptr, rev);
    };
    each(&Session::tempo);
    each(&Session::meters);
    each(&Session::tracks);
    each(&Session::groups);
    each(&Session::items);
    each(&Session::takes);
    each(&Session::media);
    each(&Session::midi);
    each(&Session::fx);
    each(&Session::routes);
    each(&Session::envelopes);
    each(&Session::autoItems);
    each(&Session::markers);
    each(&Session::regions);
    each(&Session::racks);
    each(&Session::modules);
    each(&Session::cables);
    if (!p) return;
    if (toJson(*s.project) == toJson(*p->project)) s.project = p->project;
    if (s.tempo.sameStorage(p->tempo) && s.meters.sameStorage(p->meters)) s.timeMap = p->timeMap;
    for (auto& [hash, blob] : s.blobs)
        if (const auto found = p->blobs.find(hash); found != p->blobs.end()) blob = found->second;
}

Result<Version> versionFrom(const json::Value& e, std::uint64_t id, const Reader& r) {
    if (!e.isObject()) return r.fail("expected a version object");
    static const std::set<std::string_view> members{
        "parents", "label",          "origin",          "time_ms",   "invocations", "changes",
        "created", "squashed_steps", "squashed_fields", "replay_of", "merge_remap", "state"};
    for (const auto& m : e.asObject())
        if (!members.contains(m.key)) return r.fail("unknown member", "/" + m.key);
    Version v;
    v.id = id;
    const auto* parents = e.find("parents");
    if (!parents || !parents->isArray()) return r.fail("expected parents", "/parents");
    for (std::size_t i = 0; i < parents->size(); ++i) {
        auto p = unsignedAt(parents->at(i), r.base + "/parents/" + std::to_string(i));
        if (!p) return std::move(p).error();
        v.parents.push_back(*p);
    }
    auto text = [&](const char* key, std::string& out) -> Status {
        const auto* x = e.find(key);
        if (!x) return okStatus();
        if (!x->isString()) return r.fail("expected a string", std::string("/") + key);
        out = x->asString();
        return okStatus();
    };
    SUPPLE_TRY(text("label", v.txn.label));
    SUPPLE_TRY(text("origin", v.txn.origin));
    auto number = [&](const char* key, std::uint64_t& out) -> Status {
        const auto* x = e.find(key);
        if (!x) return okStatus();
        auto n = unsignedAt(*x, r.base + "/" + key);
        if (!n) return std::move(n).error();
        out = *n;
        return okStatus();
    };
    SUPPLE_TRY(number("time_ms", v.timeMs));
    SUPPLE_TRY(number("squashed_steps", v.squashedSteps));
    SUPPLE_TRY(number("replay_of", v.replayOf));
    if (const auto* list = e.find("invocations")) {
        if (!list->isArray()) return r.fail("expected an array", "/invocations");
        for (std::size_t i = 0; i < list->size(); ++i) {
            const auto& inv = list->at(i);
            const auto* command = inv.find("command");
            const auto* args = inv.find("args");
            if (!inv.isObject() || !command || !command->isString() || !args)
                return r.fail("expected {command, args}", "/invocations/" + std::to_string(i));
            v.txn.invocations.push_back({command->asString(), *args});
        }
    }
    if (const auto* list = e.find("changes")) {
        if (!list->isArray()) return r.fail("expected an array", "/changes");
        for (std::size_t i = 0; i < list->size(); ++i) {
            const auto& pair = list->at(i);
            const std::string where = "/changes/" + std::to_string(i);
            if (!pair.isArray() || pair.size() != 2 || !pair.at(0).isString())
                return r.fail("expected [op, id]", where);
            const auto& op = pair.at(0).asString();
            Change c{Change::Op::Update, {}};
            if (op == "add")
                c.op = Change::Op::Add;
            else if (op == "remove")
                c.op = Change::Op::Remove;
            else if (op != "update")
                return r.fail("op is add, remove or update", where + "/0");
            auto object = idAt(pair.at(1), r.base + where + "/1");
            if (!object) return std::move(object).error();
            c.id = *object;
            v.changes.push_back(c);
        }
    }
    auto created = idsAt(e.find("created"), r.base + "/created");
    if (!created) return std::move(created).error();
    v.created = std::move(*created);
    if (const auto* fields = e.find("squashed_fields")) {
        if (!fields->isObject()) return r.fail("expected an object", "/squashed_fields");
        for (const auto& m : fields->asObject()) {
            const std::string where = json::pointerJoin(r.base + "/squashed_fields", m.key);
            auto object = idAt(json::Value(m.key), where);
            if (!object) return std::move(object).error();
            if (!m.value.isArray()) return Reader{where}.fail("expected field names");
            auto& names = v.squashedFields[*object];
            for (std::size_t i = 0; i < m.value.size(); ++i) {
                if (!m.value.at(i).isString())
                    return Reader{where}.fail("expected a field name", "/" + std::to_string(i));
                names.insert(m.value.at(i).asString());
            }
        }
    }
    if (const auto* remap = e.find("merge_remap")) {
        if (!remap->isObject()) return r.fail("expected an object", "/merge_remap");
        for (const auto& m : remap->asObject()) {
            const std::string where = json::pointerJoin(r.base + "/merge_remap", m.key);
            auto from = idAt(json::Value(m.key), where);
            auto to = idAt(m.value, where);
            if (!from) return std::move(from).error();
            if (!to) return std::move(to).error();
            v.mergeRemap[*from] = *to;
        }
    }
    return v;
}

Result<VersionStore> restoreBranches(const json::Value& mainDoc, const json::Value& section,
                                     const Session& main) {
    const Reader r{"/branches"};
    if (!section.isObject()) return r.fail("expected an object");
    static const std::set<std::string_view> members{"current", "next_version", "main_hash", "refs",
                                                    "versions"};
    for (const auto& m : section.asObject())
        if (!members.contains(m.key)) return r.fail("unknown member", "/" + m.key);
    const auto* hash = section.find("main_hash");
    if (!hash || !hash->isString()) return r.fail("expected main_hash", "/main_hash");
    if (hash->asString() != "sha256:" + sha256Hex(json::writeCanonical(mainDoc)))
        return Error("history.stale",
                     "main was changed by a program that does not keep branches, so the saved "
                     "branches no longer fit it",
                     "/branches/main_hash", "main opens alone");
    SavedHistory saved;
    const auto* current = section.find("current");
    if (!current || !current->isString()) return r.fail("expected current", "/current");
    saved.current = current->asString();
    const auto* next = section.find("next_version");
    if (!next) return r.fail("expected next_version", "/next_version");
    auto nextVersion = unsignedAt(*next, "/branches/next_version");
    if (!nextVersion) return std::move(nextVersion).error();
    saved.next = *nextVersion;

    const auto* refs = section.find("refs");
    if (!refs || !refs->isObject()) return r.fail("expected refs", "/refs");
    for (const auto& m : refs->asObject()) {
        const Reader at{json::pointerJoin("/branches/refs", m.key)};
        if (!m.value.isObject()) return at.fail("expected a branch object");
        for (const auto& f : m.value.asObject())
            if (f.key != "head" && f.key != "base" && f.key != "redo" && f.key != "next_ids")
                return at.fail("unknown member", "/" + f.key);
        SavedBranch b;
        b.name = m.key;
        const auto* head = m.value.find("head");
        const auto* base = m.value.find("base");
        if (!head || !base) return at.fail("expected head and base");
        auto h = unsignedAt(*head, at.base + "/head");
        auto bs = unsignedAt(*base, at.base + "/base");
        if (!h) return std::move(h).error();
        if (!bs) return std::move(bs).error();
        b.head = *h;
        b.base = *bs;
        if (const auto* redo = m.value.find("redo")) {
            if (!redo->isArray()) return at.fail("expected an array", "/redo");
            for (std::size_t i = 0; i < redo->size(); ++i) {
                auto id = unsignedAt(redo->at(i), at.base + "/redo/" + std::to_string(i));
                if (!id) return std::move(id).error();
                b.redo.push_back(*id);
            }
        }
        auto counters = countersAt(m.value.find("next_ids"), at.base + "/next_ids");
        if (!counters) return std::move(counters).error();
        b.allocated = *counters;
        if (b.name == "main")
            saved.branches.insert(saved.branches.begin(), std::move(b));
        else
            saved.branches.push_back(std::move(b));
    }
    if (saved.branches.empty() || saved.branches.front().name != "main")
        return r.fail("there is no main branch", "/refs");
    const std::uint64_t mainHead = saved.branches.front().head;

    const auto* versions = section.find("versions");
    if (!versions || !versions->isObject()) return r.fail("expected versions", "/versions");
    for (const auto& m : versions->asObject()) {
        const Reader at{json::pointerJoin("/branches/versions", m.key)};
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(m.key.data(), m.key.data() + m.key.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != m.key.data() + m.key.size() ||
            std::to_string(id) != m.key)
            return at.fail("version keys are decimal numbers");
        auto v = versionFrom(m.value, id, at);
        if (!v) return std::move(v).error();
        const auto* state = m.value.find("state");
        Session session;
        if (id == mainHead) {
            if (state) return at.fail("main's head is the document; it has no state", "/state");
            session = main;
        } else {
            if (!state) return at.fail("expected state", "/state");
            auto doc = applyDelta(mainDoc, *state, at.base + "/state");
            if (!doc) return std::move(doc).error();
            auto decoded = projectFromJson(*doc);
            if (!decoded) {
                Error e = std::move(decoded).error();
                return at.fail("its state does not load: " + e.toString(), "/state");
            }
            session = std::move(*decoded);
        }
        session.version = id;
        v->session = std::make_shared<const Session>(std::move(session));
        saved.versions.emplace(id, std::move(*v));
    }
    if (!saved.versions.contains(mainHead))
        return r.fail("main's head is not among the versions", "/refs/main/head");
    // Oldest first, so each parent is shared before its children.
    for (auto& [id, v] : saved.versions) {
        Session s = *v.session;
        const Session* parent = nullptr;
        if (!v.parents.empty())
            if (const auto it = saved.versions.find(v.parents.front());
                it != saved.versions.end() && it->first < id)
                parent = it->second.session.get();
        shareRevisions(s, parent);
        v.session = std::make_shared<const Session>(std::move(s));
    }
    return VersionStore::restore(std::move(saved));
}

}  // namespace

json::Value projectFileToJson(const VersionStore& store, bool history) {
    const SavedHistory saved = store.saved(history);
    json::Value doc = projectToJson(*store.head("main"));
    const SavedBranch& main = saved.branches.front();
    if (saved.branches.size() == 1 && saved.versions.size() == 1 && main.redo.empty()) return doc;
    json::Value section = json::Value::object();
    section["current"] = saved.current;
    section["next_version"] = saved.next;
    section["main_hash"] = "sha256:" + sha256Hex(json::writeCanonical(doc));
    json::Value refs = json::Value::object();
    for (const auto& b : saved.branches) {
        json::Value ref = json::Value::object();
        ref["head"] = b.head;
        ref["base"] = b.base;
        if (!b.redo.empty()) {
            json::Value redo = json::Value::array();
            for (auto id : b.redo) redo.push_back(id);
            ref["redo"] = std::move(redo);
        }
        ref["next_ids"] = countersJson(b.allocated);
        refs[b.name] = std::move(ref);
    }
    section["refs"] = std::move(refs);
    json::Value versions = json::Value::object();
    for (const auto& [id, v] : saved.versions)
        versions[std::to_string(id)] = versionJson(v, main.head, doc);
    section["versions"] = std::move(versions);
    doc[kSection] = std::move(section);
    return doc;
}

std::string serializeProjectFile(const VersionStore& store, bool history) {
    return json::writeCanonical(projectFileToJson(store, history)) + "\n";
}

Result<OpenedProject> projectFileFromJson(const json::Value& input,
                                          std::vector<schema::Warning>* warnings) {
    if (!input.isObject()) return Error("project.type", "a project is a JSON object");
    json::Value doc = input;
    std::optional<json::Value> section;
    if (auto* found = doc.asObject().find(kSection)) {
        section = std::move(*found);
        doc.asObject().erase(kSection);
    }
    auto main = projectFromJson(doc, warnings);
    if (!main) return std::move(main).error();
    if (!section) return OpenedProject{VersionStore(std::move(*main)), std::nullopt};
    auto restored = restoreBranches(doc, *section, *main);
    if (!restored)
        return OpenedProject{VersionStore(std::move(*main)), std::move(restored).error()};
    return OpenedProject{std::move(*restored), std::nullopt};
}

Result<OpenedProject> parseProjectFile(std::string_view text,
                                       std::vector<schema::Warning>* warnings) {
    auto doc = json::parse(text);
    if (!doc) return std::move(doc).error();
    return projectFileFromJson(*doc, warnings);
}

}  // namespace supple::model
