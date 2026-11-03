// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/model/versions.h"

#include "supple/model/schemas.h"

#include <algorithm>
#include <optional>
#include <set>

namespace supple::model {

namespace {

bool sameMediaFile(const MediaSource& a, const MediaSource& b) {
    return a.path == b.path && a.format == b.format && a.sampleRate == b.sampleRate &&
           a.channels == b.channels && a.frames == b.frames;
}

// Coalesces two change lists (a then b) per object, like EditTxn does.
std::vector<Change> coalesce(const std::vector<Change>& a, const std::vector<Change>& b) {
    std::map<Id, Change::Op> m;
    for (const auto& list : {std::cref(a), std::cref(b)})
        for (const Change& c : list.get()) {
            auto it = m.find(c.id);
            if (it == m.end()) {
                m.emplace(c.id, c.op);
            } else if (it->second == Change::Op::Add && c.op == Change::Op::Remove) {
                m.erase(it);
            } else if (it->second != Change::Op::Add) {
                it->second = c.op;
            }
        }
    std::vector<Change> out;
    for (const auto& [id, op] : m) out.push_back({op, id});
    return out;
}

// The entry for `id` in whichever collection holds its kind.
struct EntryInfo {
    bool exists = false;
    std::uint64_t rev = 0;
};
template <class T>
EntryInfo info(const IdMap<T>& m, Id id) {
    const auto* e = m.entry(id);
    return e ? EntryInfo{true, e->rev} : EntryInfo{};
}
EntryInfo entryInfo(const Session& s, Id id) {
    switch (id.kind) {
        case Kind::Project:
            return {true, s.version};
        case Kind::Track:
            return info(s.tracks, id);
        case Kind::Item:
            return info(s.items, id);
        case Kind::Take:
            return info(s.takes, id);
        case Kind::Media:
            return info(s.media, id);
        case Kind::Midi:
            return info(s.midi, id);
        case Kind::Fx:
            return info(s.fx, id);
        case Kind::Route:
            return info(s.routes, id);
        case Kind::Envelope:
            return info(s.envelopes, id);
        case Kind::AutoItem:
            return info(s.autoItems, id);
        case Kind::Marker:
            return info(s.markers, id);
        case Kind::Region:
            return info(s.regions, id);
        case Kind::Group:
            return info(s.groups, id);
        case Kind::TempoPoint:
            return info(s.tempo, id);
        case Kind::Meter:
            return info(s.meters, id);
        default:
            return {};
    }
}

// The object's JSON form (null when absent), for field-level comparisons.
json::Value objectJson(const Session& s, Id id) {
    auto enc = [&](const auto& map) -> json::Value {
        const auto* v = map.find(id);
        return v ? toJson(*v) : json::Value();
    };
    switch (id.kind) {
        case Kind::Project:
            return toJson(*s.project);
        case Kind::Track:
            return enc(s.tracks);
        case Kind::Item:
            return enc(s.items);
        case Kind::Take:
            return enc(s.takes);
        case Kind::Media:
            return enc(s.media);
        case Kind::Midi:
            return enc(s.midi);
        case Kind::Fx:
            return enc(s.fx);
        case Kind::Route:
            return enc(s.routes);
        case Kind::Envelope:
            return enc(s.envelopes);
        case Kind::AutoItem:
            return enc(s.autoItems);
        case Kind::Marker:
            return enc(s.markers);
        case Kind::Region:
            return enc(s.regions);
        case Kind::Group:
            return enc(s.groups);
        case Kind::TempoPoint:
            return enc(s.tempo);
        case Kind::Meter:
            return enc(s.meters);
        default:
            return {};
    }
}

// The JSON Schema of an object's encoding: it marks the ID fields.
template <class T>
const json::Value& schemaFor(const IdMap<T>&) {
    static const json::Value schema = schemaOf<T>().jsonSchema();
    return schema;
}
const json::Value* objectSchema(const Session& s, Id id) {
    switch (id.kind) {
        case Kind::Project: {
            static const json::Value schema = schemaOf<Project>().jsonSchema();
            return &schema;
        }
        case Kind::Track:
            return &schemaFor(s.tracks);
        case Kind::Item:
            return &schemaFor(s.items);
        case Kind::Take:
            return &schemaFor(s.takes);
        case Kind::Media:
            return &schemaFor(s.media);
        case Kind::Midi:
            return &schemaFor(s.midi);
        case Kind::Fx:
            return &schemaFor(s.fx);
        case Kind::Route:
            return &schemaFor(s.routes);
        case Kind::Envelope:
            return &schemaFor(s.envelopes);
        case Kind::AutoItem:
            return &schemaFor(s.autoItems);
        case Kind::Marker:
            return &schemaFor(s.markers);
        case Kind::Region:
            return &schemaFor(s.regions);
        case Kind::Group:
            return &schemaFor(s.groups);
        case Kind::TempoPoint:
            return &schemaFor(s.tempo);
        case Kind::Meter:
            return &schemaFor(s.meters);
        default:
            return nullptr;
    }
}

// Structural lists that a replayed command recomputes on the target (a track
// added on each side appends to the parent's children on both): never a
// conflict by themselves.
bool structural(const std::string& field) {
    return field == "masters" || field == "children" || field == "chain" ||
           field == "input_chain" || field == "takes" || field == "items";
}

std::set<std::string> changedFields(const json::Value& a, const json::Value& b) {
    std::set<std::string> out;
    std::set<std::string> keys;
    if (a.isObject())
        for (const auto& m : a.asObject()) keys.insert(m.key);
    if (b.isObject())
        for (const auto& m : b.asObject()) keys.insert(m.key);
    for (const auto& k : keys) {
        const json::Value* x = a.isObject() ? a.find(k) : nullptr;
        const json::Value* y = b.isObject() ? b.find(k) : nullptr;
        if ((!x) != (!y) || (x && y && !(*x == *y))) out.insert(k);
    }
    return out;
}

std::set<std::string> changedFields(const Session& before, const Session& after, Id id) {
    return changedFields(objectJson(before, id), objectJson(after, id));
}

}  // namespace

namespace {

// "^tr_[0-9]+$" (one kind) or "^[a-z]{2}_[0-9]+$" (any kind).
bool idPattern(const json::Value* schema, const char* key) {
    const json::Value* p = schema && schema->isObject() ? schema->find(key) : nullptr;
    if (!p || !p->isString()) return false;
    const std::string& s = p->asString();
    constexpr std::string_view kTail = "_[0-9]+$";
    return s.size() > kTail.size() + 1 && s.front() == '^' &&
           std::string_view(s).substr(s.size() - kTail.size()) == kTail;
}

std::string remapped(const std::string& text, const std::map<Id, Id>& remap) {
    if (auto id = Id::parse(text))
        if (auto it = remap.find(*id); it != remap.end()) return it->second.str();
    return text;
}

// `inner` (A's IDs -> B's) followed by `outer` (B's IDs -> C's): A's IDs -> C's.
// An ID that `inner` keeps names the same object on B.
std::map<Id, Id> compose(const std::map<Id, Id>& inner, const std::map<Id, Id>& outer) {
    std::map<Id, Id> out;
    for (const auto& [a, b] : inner) {
        const auto c = outer.find(b);
        out.emplace(a, c == outer.end() ? b : c->second);
    }
    for (const auto& [b, c] : outer)
        if (!inner.count(b)) out.emplace(b, c);
    return out;
}

bool typeMatches(const json::Value& v, const json::Value& schema) {
    const json::Value* t = schema.find("type");
    if (!t || !t->isString()) return true;
    const std::string& s = t->asString();
    return (s == "string" && v.isString()) || (s == "object" && v.isObject()) ||
           (s == "array" && v.isArray()) || (s == "number" && v.isNumber()) ||
           (s == "integer" && v.isNumber()) || (s == "boolean" && v.isBool()) ||
           (s == "null" && v.isNull());
}

// A oneOf/anyOf alternative applies when its tag constants match (tagged
// variants) and its type matches.
bool alternativeMatches(const json::Value& v, const json::Value& alt) {
    if (!alt.isObject() || !typeMatches(v, alt)) return false;
    if (const json::Value* props = alt.find("properties");
        props && props->isObject() && v.isObject())
        for (const auto& p : props->asObject())
            if (const json::Value* c = p.value.isObject() ? p.value.find("const") : nullptr) {
                const json::Value* x = v.find(p.key);
                if (!x || !(*x == *c)) return false;
            }
    return true;
}

using Defs = std::map<std::string, const json::Value*>;

json::Value remapWalk(const json::Value& v, const json::Value* schema, Defs defs,
                      const std::map<Id, Id>& remap, int depth) {
    if (!schema || !schema->isObject() || depth > 64) return v;
    if (const json::Value* d = schema->find("$defs"); d && d->isObject())
        for (const auto& m : d->asObject()) defs[m.key] = &m.value;
    if (const json::Value* ref = schema->find("$ref"); ref && ref->isString()) {
        constexpr std::string_view kPrefix = "#/$defs/";
        const std::string& r = ref->asString();
        if (r.rfind(kPrefix, 0) == 0)
            if (auto it = defs.find(r.substr(kPrefix.size())); it != defs.end())
                return remapWalk(v, it->second, defs, remap, depth + 1);
        return v;
    }
    for (const char* k : {"oneOf", "anyOf"})
        if (const json::Value* alts = schema->find(k); alts && alts->isArray()) {
            for (const auto& alt : alts->asArray())
                if (alternativeMatches(v, alt)) return remapWalk(v, &alt, defs, remap, depth + 1);
            return v;
        }
    if (v.isString())
        return idPattern(schema, "pattern") ? json::Value(remapped(v.asString(), remap)) : v;
    if (v.isArray()) {
        const json::Value* items = schema->find("items");
        json::Value out = json::Value::array();
        for (const auto& x : v.asArray())
            out.push_back(remapWalk(x, items, defs, remap, depth + 1));
        return out;
    }
    if (v.isObject()) {
        const json::Value* props = schema->find("properties");
        const json::Value* extra = schema->find("additionalProperties");
        const bool idKeys = idPattern(schema->find("propertyNames"), "pattern");
        json::Value out = json::Value::object();
        for (const auto& m : v.asObject()) {
            const json::Value* sub = props && props->isObject() ? props->find(m.key) : nullptr;
            if (!sub) sub = extra;
            out[idKeys ? remapped(m.key, remap) : m.key] =
                remapWalk(m.value, sub, defs, remap, depth + 1);
        }
        return out;
    }
    return v;
}

}  // namespace

json::Value remapIds(const json::Value& args, const json::Value& schema,
                     const std::map<Id, Id>& remap) {
    if (remap.empty()) return args;
    return remapWalk(args, &schema, {}, remap, 0);
}

namespace {

// An object of `s` as JSON, its ID fields (not names or other text) rewritten
// through `remap`: a branch object as it reads with the target's IDs.
json::Value withTargetIds(const Session& s, Id id, const std::map<Id, Id>& remap) {
    const json::Value* schema = objectSchema(s, id);
    json::Value object = objectJson(s, id);
    return schema ? remapIds(object, *schema, remap) : object;
}

}  // namespace

VersionStore::VersionStore(Session initial) {
    const std::uint64_t id = initial.version;
    Version v;
    v.id = id;
    v.session = std::make_shared<const Session>(std::move(initial));
    v.txn.label = "initial";
    versions_.emplace(id, std::move(v));
    branches_.emplace("main", Branch{id, id, {}, versions_.at(id).session->next});
    next_ = id + 1;
}

const VersionStore::Branch* VersionStore::lookup(std::string_view name) const {
    auto it = branches_.find(name);
    return it == branches_.end() ? nullptr : &it->second;
}

Result<VersionStore::Branch*> VersionStore::find(std::string_view name) {
    auto it = branches_.find(name);
    if (it == branches_.end()) {
        std::string names;
        for (const auto& [n, b] : branches_) names += (names.empty() ? "" : ", ") + n;
        return Error("branch.unknown", "no branch named \"" + std::string(name) + "\"", {},
                     "branches: " + names);
    }
    return &it->second;
}

std::shared_ptr<const Session> VersionStore::head(std::string_view name) const {
    const Branch* b = lookup(name);
    if (!b) return nullptr;
    auto stored = versions_.at(b->head).session;
    if (stored->next == b->allocated) return stored;
    if (!b->view || b->view->version != stored->version || b->view->next != b->allocated) {
        auto out = std::make_shared<Session>(*stored);
        out->next = b->allocated;
        b->view = std::move(out);
    }
    return b->view;
}

bool VersionStore::updateMediaHash(Id id, const MediaSource& expected, std::string_view hash,
                                   std::string_view branch) {
    auto matches = [&](const MediaSource* media) {
        return media && sameMediaFile(*media, expected) && media->hash.empty();
    };
    const auto current = head(branch);
    const auto* present = current ? current->media.find(id) : nullptr;
    if (hash.empty() || !present || !sameMediaFile(*present, expected) ||
        (!present->hash.empty() && present->hash != hash))
        return false;
    bool used = false;
    for (const auto& item : current->items)
        for (Id takeId : item.value->takes)
            if (const auto* take = current->takes.find(takeId); take && take->source == id)
                used = true;
    if (!used) return false;
    auto updatedMedia = std::make_shared<MediaSource>(expected);
    updatedMedia->hash = hash;
    bool updated = false;
    for (auto& [versionId, version] : versions_) {
        const auto* entry = version.session->media.entry(id);
        if (!entry || !matches(entry->value.get())) continue;
        if (version.session.use_count() > 1) metadataLeases_[versionId].push_back(version.session);
        auto session = std::make_shared<Session>(*version.session);
        session->media = session->media.with(id, entry->rev, updatedMedia);
        version.session = std::move(session);
        updated = true;
    }
    for (auto& [name, branchState] : branches_) branchState.view.reset();
    return updated;
}

std::uint64_t VersionStore::headVersion(std::string_view name) const {
    const Branch* b = lookup(name);
    return b ? b->head : 0;
}

const Version* VersionStore::version(std::uint64_t id) const {
    auto it = versions_.find(id);
    return it == versions_.end() ? nullptr : &it->second;
}

void VersionStore::forEachVersion(const std::function<void(const Version&)>& visit) const {
    for (const auto& [id, v] : versions_) visit(v);
}

std::shared_ptr<const Session> VersionStore::pin(std::uint64_t id) const {
    const auto* found = version(id);
    return found ? found->session : nullptr;
}

void VersionStore::collectUnused() {
    std::vector<std::uint64_t> pending;
    for (const auto& [name, branch] : branches_) {
        pending.push_back(branch.head);
        pending.push_back(branch.base);
        pending.insert(pending.end(), branch.redo.begin(), branch.redo.end());
    }
    for (const auto& [id, version] : versions_)
        if (version.session.use_count() > 1) pending.push_back(id);
    for (auto& [id, leases] : metadataLeases_) {
        std::erase_if(leases, [](const auto& lease) { return lease.expired(); });
        if (!leases.empty()) pending.push_back(id);
    }
    std::erase_if(metadataLeases_, [](const auto& entry) { return entry.second.empty(); });
    std::set<std::uint64_t> reachable;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!reachable.insert(id).second) continue;
        if (const auto* found = version(id))
            pending.insert(pending.end(), found->parents.begin(), found->parents.end());
    }
    std::erase_if(versions_, [&](const auto& entry) { return !reachable.count(entry.first); });
}

namespace {
std::uint64_t jsonBytes(const json::Value& v) {
    std::uint64_t bytes = sizeof(v);
    if (v.isString()) bytes += v.asString().size();
    if (v.isArray())
        for (const auto& x : v.asArray()) bytes += jsonBytes(x);
    if (v.isObject())
        for (const auto& x : v.asObject()) bytes += x.key.size() + jsonBytes(x.value);
    return bytes;
}
template <class T>
std::uint64_t objectBytes(const T& v) {
    std::uint64_t bytes = sizeof(v);
    if constexpr (requires { v.name; }) bytes += v.name.size();
    if constexpr (requires { v.points; }) bytes += v.points.size() * sizeof(EnvPoint);
    if constexpr (requires { v.children; }) bytes += v.children.size() * sizeof(Id);
    if constexpr (requires { v.chain; }) bytes += v.chain.size() * sizeof(Id);
    if constexpr (requires { v.inputChain; }) bytes += v.inputChain.size() * sizeof(Id);
    if constexpr (requires { v.takes; }) bytes += v.takes.size() * sizeof(Id);
    if constexpr (requires { v.warpPeers; }) {
        bytes += v.warpPeers.size() * sizeof(Id) + v.stretchMarkers.size() * sizeof(StretchMarker);
        if (v.transients)
            bytes += v.transients->positions.size() * sizeof(double) +
                     v.transients->sourceHash.size() + v.transients->algorithm.size();
    }
    if constexpr (requires { v.patch; })
        if (v.patch) bytes += jsonBytes(*v.patch);
    return bytes;
}
std::uint64_t objectBytes(const MidiSource& v) {
    const auto& s = v.sequence;
    std::uint64_t bytes = sizeof(v) + s.notes.size() * sizeof(midi::Note) +
                          s.controls.size() * sizeof(midi::Control) +
                          s.sysex.size() * sizeof(midi::Sysex);
    for (const auto& event : s.sysex) bytes += event.bytes.size();
    return bytes;
}
std::uint64_t objectBytes(const Project& v) {
    return sizeof(v) + v.name.size() + v.title.size() + v.author.size() + v.notes.size() +
           v.mediaDir.size() + v.masters.size() * sizeof(Id) + jsonBytes(v.render);
}
// Pointer identity counts persistent containers and immutable payloads once.
// No MIDI events/blob bytes are traversed or serialized to estimate them.
struct RetainedSize {
    std::map<const void*, std::pair<std::size_t, std::uint64_t>> payloads;
    std::map<std::uint64_t, std::vector<const void*>> byVersion;
    std::map<const void*, std::vector<std::pair<const void*, std::uint64_t>>> containers;
    std::uint64_t bytes = 0;
    void add(std::uint64_t id, const void* key, std::uint64_t size) {
        auto& [count, stored] = payloads[key];
        if (count++ == 0) {
            stored = size;
            bytes += size;
        }
        byVersion[id].push_back(key);
    }
    template <class T>
    void map(std::uint64_t id, const IdMap<T>& objects) {
        if (objects.empty()) return;
        const void* storage = &*objects.begin();
        add(id, storage, objects.size() * sizeof(typename IdMap<T>::Entry));
        auto [cached, fresh] = containers.try_emplace(storage);
        if (fresh)
            for (const auto& e : objects)
                cached->second.emplace_back(e.value.get(), objectBytes(*e.value));
        for (const auto& [key, bytesHeld] : cached->second) add(id, key, bytesHeld);
    }
    void add(const Version& v) {
        const auto& s = *v.session;
        std::uint64_t metadata = sizeof(v) + sizeof(s) + v.changes.size() * sizeof(Change) +
                                 v.created.size() * sizeof(Id) + jsonBytes(s.extraSections) +
                                 v.txn.label.size() + v.txn.origin.size() +
                                 v.txn.coalesceKey.size();
        for (const auto& invocation : v.txn.invocations)
            metadata += sizeof(invocation) + invocation.command.size() + jsonBytes(invocation.args);
        for (const auto& [id, fields] : v.squashedFields) {
            metadata += sizeof(id) + sizeof(fields);
            for (const auto& field : fields) metadata += sizeof(field) + field.size();
        }
        metadata += v.mergeRemap.size() * sizeof(std::pair<const Id, Id>);
        add(v.id, &v, metadata);
        add(v.id, s.project.get(), objectBytes(*s.project));
        map(v.id, s.tempo);
        map(v.id, s.meters);
        map(v.id, s.tracks);
        map(v.id, s.items);
        map(v.id, s.takes);
        map(v.id, s.media);
        map(v.id, s.midi);
        map(v.id, s.fx);
        map(v.id, s.routes);
        map(v.id, s.envelopes);
        map(v.id, s.autoItems);
        map(v.id, s.markers);
        map(v.id, s.regions);
        map(v.id, s.groups);
        for (const auto& [hash, blob] : s.blobs)
            add(v.id, blob.get(), sizeof(Blob) + blob->data.size());
    }
    void drop(std::uint64_t id) {
        for (const void* key : byVersion.at(id)) {
            auto& [count, size] = payloads.at(key);
            if (--count == 0) bytes -= size;
        }
        byVersion.erase(id);
    }
};
}  // namespace

std::uint64_t VersionStore::retainedBytes() const {
    RetainedSize size;
    for (const auto& [id, v] : versions_) size.add(v);
    return size.bytes;
}

bool VersionStore::historyTruncated() const {
    const auto& b = branches_.at(current_);
    for (auto id : b.redo)
        if (versions_.at(id).squashedSteps) return true;
    for (auto id = b.head;;) {
        const auto& v = versions_.at(id);
        if (v.squashedSteps) return true;
        if (id == b.base || v.parents.empty()) return false;
        id = v.parents.front();
    }
}

void VersionStore::enforceBudget() {
    collectUnused();
    RetainedSize size;
    for (const auto& [id, v] : versions_) size.add(v);
    const auto budget =
        versions_.at(branches_.at(current_).head).session->project->historyBudgetBytes;
    if (size.bytes <= budget) return;
    std::set<std::uint64_t> protectedIds;
    std::map<std::uint64_t, std::vector<std::uint64_t>> children;
    for (const auto& [name, b] : branches_) {
        protectedIds.insert(b.head);
        protectedIds.insert(b.base);
        protectedIds.insert(b.redo.begin(), b.redo.end());
    }
    for (const auto& [id, v] : versions_) {
        if (v.session.use_count() > 1 || metadataLeases_.contains(id) || v.parents.size() != 1)
            protectedIds.insert(id);
        for (auto parent : v.parents) children[parent].push_back(id);
    }
    // Oldest first, independently across branch lineages. Forks, merge nodes,
    // redo steps and leases remain exact. The child retains the end snapshot
    // and the combined replay log of the earlier-history step.
    for (auto it = versions_.begin(); it != versions_.end() && size.bytes > budget;) {
        const auto id = it->first;
        auto& v = it->second;
        auto next = std::next(it);
        if (!protectedIds.contains(id) && children[id].size() == 1) {
            auto& child = versions_.at(children[id].front());
            if (child.parents.size() == 1 && child.session.use_count() == 1 &&
                !metadataLeases_.contains(child.id)) {
                size.drop(child.id);
                size.drop(id);
                auto remember = [&](const Version& step) {
                    for (const auto& [object, fields] : step.squashedFields)
                        child.squashedFields[object].insert(fields.begin(), fields.end());
                    const auto& before = *versions_.at(step.parents.front()).session;
                    for (const auto& change : step.changes) {
                        if (change.op != Change::Op::Update) continue;
                        auto fields = changedFields(before, *step.session, change.id);
                        child.squashedFields[change.id].insert(fields.begin(), fields.end());
                    }
                };
                remember(child);
                remember(v);
                child.parents = v.parents;
                auto inv = std::move(v.txn.invocations);
                inv.insert(inv.end(), child.txn.invocations.begin(), child.txn.invocations.end());
                child.txn.invocations = std::move(inv);
                child.changes = coalesce(v.changes, child.changes);
                auto created = std::move(v.created);
                created.insert(created.end(), child.created.begin(), child.created.end());
                child.created = std::move(created);
                child.squashedSteps += v.squashedSteps + 1;
                child.txn.label = "earlier history";
                child.txn.coalesceKey.clear();
                auto& siblings = children[v.parents.front()];
                std::replace(siblings.begin(), siblings.end(), id, child.id);
                size.add(child);
                versions_.erase(it);
            }
        }
        it = next;
    }
}

Result<std::uint64_t> VersionStore::commit(Session session, TxnRecord txn,
                                           std::vector<Change> changes, std::vector<Id> created,
                                           std::uint64_t timeMs) {
    if (session.version != next_)
        return Error("history.version", "the session was built as version " +
                                            std::to_string(session.version) + ", expected " +
                                            std::to_string(next_));
    Branch& b = branches_.at(current_);
    b.view.reset();
    for (std::size_t k = 0; k < kKindCount; ++k) {
        b.allocated.next[k] = std::max(b.allocated.next[k], session.next.next[k]);
        session.next.next[k] = b.allocated.next[k];
    }
    const Version& head = versions_.at(b.head);
    // A transaction may have opened before a metadata completion. Retain
    // that completion when committing its still-unhashed copy of the source.
    // Rebuild only when something needs it: unchanged maps keep their shared
    // storage (history size estimates and change detection rely on it).
    std::optional<IdMap<MediaSource>::Builder> media;
    if (!session.media.sameStorage(head.session->media))
        for (const auto& entry : session.media) {
            const auto* known = head.session->media.find(entry.id);
            if (entry.value->hash.empty() && known && !known->hash.empty() &&
                sameMediaFile(*entry.value, *known)) {
                if (!media) media.emplace(session.media);
                auto updated = std::make_shared<MediaSource>(*entry.value);
                updated->hash = known->hash;
                media->put(entry.id, entry.rev, std::move(updated));
            }
        }
    if (media) session.media = media->build();
    Version v;
    v.id = next_++;
    v.txn = std::move(txn);
    v.timeMs = timeMs;
    v.session = std::make_shared<const Session>(std::move(session));
    // The head may only be replaced when nothing else points at it: another
    // branch's head, base or redo stack, or a version built on it.
    auto referencedElsewhere = [&](std::uint64_t id) {
        for (const auto& [name, other] : branches_) {
            if (name == current_) continue;
            if (other.head == id || other.base == id ||
                std::find(other.redo.begin(), other.redo.end(), id) != other.redo.end())
                return true;
        }
        for (const auto& [vid, ver] : versions_)
            if (std::find(ver.parents.begin(), ver.parents.end(), id) != ver.parents.end())
                return true;
        return false;
    };
    const bool coalesceWithHead = !v.txn.coalesceKey.empty() && b.head != b.base &&
                                  b.redo.empty() && head.txn.coalesceKey == v.txn.coalesceKey &&
                                  !referencedElsewhere(b.head);
    if (coalesceWithHead) {
        // Replace the head: same parent, combined changes and invocations.
        v.parents = head.parents;
        v.changes = coalesce(head.changes, changes);
        v.created = head.created;
        v.created.insert(v.created.end(), created.begin(), created.end());
        std::vector<Invocation> inv = head.txn.invocations;
        inv.insert(inv.end(), v.txn.invocations.begin(), v.txn.invocations.end());
        v.txn.invocations = std::move(inv);
        b.head = v.id;
        versions_.emplace(v.id, std::move(v));
    } else {
        v.parents = {b.head};
        v.changes = std::move(changes);
        v.created = std::move(created);
        b.head = v.id;
        b.redo.clear();
        versions_.emplace(v.id, std::move(v));
    }
    enforceBudget();
    return b.head;
}

bool VersionStore::canUndo() const {
    const Branch& b = branches_.at(current_);
    return b.head != b.base;
}

bool VersionStore::canRedo() const { return !branches_.at(current_).redo.empty(); }

Status VersionStore::undo() {
    Branch& b = branches_.at(current_);
    b.view.reset();
    if (b.head == b.base)
        return Error("history.nothing_to_undo", "nothing to undo on branch " + current_);
    b.redo.push_back(b.head);
    b.head = versions_.at(b.head).parents.front();
    return okStatus();
}

Status VersionStore::redo() {
    Branch& b = branches_.at(current_);
    b.view.reset();
    if (b.redo.empty())
        return Error("history.nothing_to_redo", "nothing to redo on branch " + current_);
    b.head = b.redo.back();
    b.redo.pop_back();
    return okStatus();
}

std::vector<const Version*> VersionStore::history(std::size_t limit) const {
    std::vector<const Version*> out;
    const Branch& b = branches_.at(current_);
    std::uint64_t id = b.head;
    while (out.size() < limit) {
        const Version& v = versions_.at(id);
        out.push_back(&v);
        if (id == b.base || v.parents.empty()) break;
        id = v.parents.front();
    }
    return out;
}

Status VersionStore::createBranch(const std::string& name, std::string_view from) {
    if (name.empty() || name.size() > 100)
        return Error("branch.name", "branch names are 1-100 characters", "/name");
    if (branches_.count(name))
        return Error("branch.exists", "branch \"" + name + "\" already exists", "/name",
                     "pick another name or switch to it");
    auto src = find(from);
    if (!src) return std::move(src).error();
    const std::uint64_t at = (*src)->head;
    branches_.emplace(name, Branch{at, at, {}, (*src)->allocated});
    return okStatus();
}

Status VersionStore::createBranchAt(const std::string& name, std::uint64_t version) {
    if (name.empty() || name.size() > 100)
        return Error("branch.name", "branch names are 1-100 characters", "/name");
    if (branches_.count(name))
        return Error("branch.exists", "branch \"" + name + "\" already exists", "/name",
                     "pick another name or switch to it");
    const auto at = versions_.find(version);
    if (at == versions_.end() || !at->second.session)
        return Error("history.version", "no version " + std::to_string(version), "/version",
                     "history.list shows the versions of the current branch");
    // The current branch's history first, then any other branch's (a render
    // of another branch's head names that branch's version); the new branch
    // takes the ID counters of the lineage it starts from.
    const Branch* from = nullptr;
    if (const auto* current = lookup(current_); current && descends(current->head, version))
        from = current;
    for (const auto& [branchName, branch] : branches_)
        if (!from && descends(branch.head, version)) from = &branch;
    if (!from)
        return Error("history.version",
                     "version " + std::to_string(version) + " is in no branch's history",
                     "/version", "history.list shows the versions of the current branch");
    const Counters allocated = from->allocated;
    branches_.emplace(name, Branch{version, version, {}, allocated});
    return okStatus();
}

Status VersionStore::switchBranch(std::string_view name) {
    auto b = find(name);
    if (!b) return std::move(b).error();
    current_ = std::string(name);
    return okStatus();
}

Status VersionStore::deleteBranch(std::string_view name) {
    if (name == "main") return Error("branch.protected", "main cannot be deleted");
    if (name == current_)
        return Error("branch.current", "cannot delete the current branch", {}, "switch first");
    auto b = find(name);
    if (!b) return std::move(b).error();
    branches_.erase(branches_.find(name));
    enforceBudget();
    return okStatus();
}

std::vector<std::string> VersionStore::branches() const {
    std::vector<std::string> out;
    for (const auto& [n, b] : branches_) out.push_back(n);
    return out;
}

std::set<std::uint64_t> VersionStore::ancestors(std::uint64_t start) const {
    std::set<std::uint64_t> seen;
    std::vector<std::uint64_t> stack{start};
    while (!stack.empty()) {
        const std::uint64_t id = stack.back();
        stack.pop_back();
        if (!seen.insert(id).second) continue;
        auto it = versions_.find(id);
        if (it == versions_.end()) continue;
        for (std::uint64_t p : it->second.parents) stack.push_back(p);
    }
    return seen;
}

bool VersionStore::descends(std::uint64_t from, std::uint64_t ancestor) const {
    // First parents only: along them a lineage keeps its IDs, while a merge's
    // source parent has IDs of its own (parents are older, smaller IDs).
    for (std::uint64_t id = from; id >= ancestor;) {
        if (id == ancestor) return true;
        const auto it = versions_.find(id);
        if (it == versions_.end() || it->second.parents.empty()) return false;
        id = it->second.parents.front();
    }
    return false;
}

std::uint64_t VersionStore::forkPoint(std::uint64_t a, std::uint64_t b) const {
    const auto ofA = ancestors(a), ofB = ancestors(b);
    std::uint64_t best = 0;
    bool found = false;
    for (std::uint64_t id : ofA)
        if (ofB.count(id) && (!found || id > best)) {
            best = id;
            found = true;
        }
    return best;
}

MergeOutcome VersionStore::merge(std::string_view from, std::string_view into,
                                 const Replayer& replay, const ArgSchemas& schemas,
                                 std::uint64_t timeMs) {
    MergeOutcome out;
    const Branch* src = lookup(from);
    const Branch* dstc = lookup(into);
    if (!src || !dstc) {
        out.conflicts.push_back({Conflict::Kind::Invalid, 0, 0, std::nullopt,
                                 "unknown branch " + std::string(src ? into : from)});
        return out;
    }
    Branch& dst = branches_.find(into)->second;
    out.head = dst.head;
    const std::uint64_t fork = forkPoint(src->head, dst.head);
    if (fork == src->head) {
        out.upToDate = true;
        return out;
    }
    if (fork == dst.head) {
        dst.head = src->head;
        dst.view.reset();
        for (std::size_t k = 0; k < kKindCount; ++k)
            dst.allocated.next[k] = std::max(dst.allocated.next[k], src->allocated.next[k]);
        dst.redo.clear();
        out.head = dst.head;
        out.fastForward = true;
        enforceBudget();
        return out;
    }
    // The source's transactions since the fork, oldest first (first-parent chain).
    std::vector<const Version*> path;
    for (std::uint64_t id = src->head; id != fork;) {
        const Version& v = versions_.at(id);
        if (v.parents.empty()) {
            out.conflicts.push_back({Conflict::Kind::Invalid, 0, id, std::nullopt,
                                     "the fork point is not on the branch's first-parent history"});
            return out;
        }
        path.push_back(&v);
        id = v.parents.front();
    }
    std::reverse(path.begin(), path.end());

    // The source versions the target already holds: on its own first-parent
    // history, or replayed there by earlier merges (also through merges of
    // merges), each with the remap from that version's IDs to the target's.
    // Replay continues after the newest one on the path, or after the fork.
    std::map<std::uint64_t, std::map<Id, Id>> held;
    std::set<std::uint64_t> replayed;  // held through a replay, not on the target itself
    for (std::uint64_t id = dst.head;;) {
        const Version& v = versions_.at(id);
        held.try_emplace(id);
        std::map<Id, Id> remap = v.mergeRemap;
        for (std::uint64_t of = v.replayOf; of;) {
            const auto it = versions_.find(of);
            if (it == versions_.end()) break;
            if (held.try_emplace(of, remap).second) replayed.insert(of);
            remap = compose(it->second.mergeRemap, remap);
            of = it->second.replayOf;
        }
        if (v.parents.empty()) break;
        id = v.parents.front();
    }
    // A version also counts as held when a replayed step the target holds
    // descends from it on the same lineage: that replay applied it too, even
    // if budget squashing has since combined its own step into a later one.
    auto heldRemap = [&](std::uint64_t source) -> const std::map<Id, Id>* {
        if (const auto found = held.find(source); found != held.end()) return &found->second;
        for (auto it = held.rbegin(); it != held.rend(); ++it)
            if (it->first > source && replayed.count(it->first) && descends(it->first, source))
                return &it->second;
        return nullptr;
    };
    std::uint64_t base = fork;
    const std::map<Id, Id>* seed = nullptr;
    for (std::size_t k = path.size(); k-- > 0 && !seed;)
        if ((seed = heldRemap(path[k]->id))) {
            base = path[k]->id;
            path.erase(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(k) + 1);
        }
    if (!seed) seed = heldRemap(fork);
    const Session& forkState = *versions_.at(base).session;
    if (seed) {
        // Whole, including objects since removed: an older branch may still
        // hold one. An entry another lineage made for an ID this branch
        // allocates again is replaced when the replay creates that object.
        out.remap = *seed;
    } else {
        // The fork reached the target only through history that is gone:
        // safe if no merge on the way gave any object another ID.
        for (std::uint64_t id : ancestors(dst.head))
            if (const Version* v = version(id); v && !v->mergeRemap.empty()) {
                out.conflicts.push_back(
                    {Conflict::Kind::Invalid, 0, src->head, std::nullopt,
                     "cannot match " + std::string(from) + "'s objects on " + std::string(into) +
                         ": the history of its earlier merge is no longer kept; continue on a "
                         "new branch from " +
                         std::string(into)});
                return out;
            }
    }
    if (path.empty()) {
        out.upToDate = true;  // every step is already on the target
        return out;
    }
    const Session& targetHead = *versions_.at(dst.head).session;
    auto onTargetId = [&](Id id) {
        const auto found = out.remap.find(id);
        return found == out.remap.end() ? id : found->second;
    };
    std::shared_ptr<const Session> state = head(into);
    std::vector<Version> staged;
    std::uint64_t nextId = next_;
    std::uint64_t parent = dst.head;
    for (std::size_t step = 0; step < path.size(); ++step) {
        const Version& v = *path[step];
        // Concurrent edits: objects this transaction changed that the target
        // also changed since the fork — in the same (non-structural) field — or
        // removed; or objects the transaction removes that the target changed.
        const Session& before = *versions_.at(v.parents.front()).session;
        for (const Change& c : v.changes) {
            if (c.op == Change::Op::Add) continue;
            const auto atFork = entryInfo(forkState, c.id);
            if (!atFork.exists) continue;  // created on the branch itself
            const Id target = onTargetId(c.id);
            const auto onTarget = entryInfo(targetHead, target);
            if (target == c.id && onTarget.exists && onTarget.rev == atFork.rev)
                continue;  // untouched on target
            std::string why;
            const auto theirs = onTarget.exists
                                    ? changedFields(withTargetIds(forkState, c.id, out.remap),
                                                    objectJson(targetHead, target))
                                    : std::set<std::string>{};
            if (!onTarget.exists) {
                why = " was removed on " + std::string(into) + " since the fork";
            } else if (c.op == Change::Op::Remove) {
                if (theirs.empty()) continue;  // as at the fork
                why = " was changed on " + std::string(into) + " since the fork";
            } else {
                auto mine = changedFields(before, *v.session, c.id);
                if (auto touched = v.squashedFields.find(c.id); touched != v.squashedFields.end())
                    mine.insert(touched->second.begin(), touched->second.end());
                std::string both;
                for (const auto& f : mine)
                    if (!structural(f) && theirs.count(f)) {
                        const auto mineValue = objectJson(*v.session, c.id);
                        const auto theirsValue = objectJson(targetHead, target);
                        const auto* x = mineValue.find(f);
                        const auto* y = theirsValue.find(f);
                        if (c.id != kProject || !x || !y || *x != *y)
                            both += (both.empty() ? "" : ", ") + f;
                    }
                if (both.empty()) continue;
                why = ": both branches changed " + both;
            }
            out.conflicts.push_back(
                {Conflict::Kind::ConcurrentEdit, step, v.id, target, target.str() + why});
        }
        if (!out.conflicts.empty()) {
            out.head = dst.head;
            return out;
        }
        std::vector<Invocation> inv;
        Replayed result{*state, {}, {}};
        for (const auto& i : v.txn.invocations) {
            const json::Value* schema = schemas ? schemas(i.command) : nullptr;
            Invocation resolved{i.command, schema ? remapIds(i.args, *schema, out.remap) : i.args};
            auto next = replay(result.session, {resolved}, nextId);
            if (!next || result.created.size() + next->created.size() > v.created.size()) {
                out.conflicts.push_back({Conflict::Kind::Invalid, step, v.id, std::nullopt,
                                         next ? "replay created more objects than the original"
                                              : next.error().toString()});
                return out;
            }
            for (Id fresh : next->created) {
                const Id original = v.created[result.created.size()];
                if (original != fresh)
                    out.remap[original] = fresh;
                else
                    out.remap.erase(original);
                result.created.push_back(fresh);
            }
            result.changes = coalesce(result.changes, next->changes);
            result.session = std::move(next->session);
            inv.push_back(std::move(resolved));
        }
        if (result.created.size() != v.created.size()) {
            out.conflicts.push_back({Conflict::Kind::Invalid, step, v.id, std::nullopt,
                                     "replay creation count differs from the original"});
            return out;
        }
        Version nv;
        nv.id = nextId++;
        nv.parents = {parent};
        nv.txn = v.txn;
        nv.txn.coalesceKey.clear();  // a later edit never folds into a merged step
        nv.squashedSteps = v.squashedSteps;
        for (const auto& [object, fields] : v.squashedFields) {
            const auto remappedId = out.remap.find(object);
            nv.squashedFields[remappedId == out.remap.end() ? object : remappedId->second] = fields;
        }
        nv.txn.invocations = std::move(inv);
        nv.replayOf = v.id;
        nv.mergeRemap = out.remap;
        nv.changes = std::move(result.changes);
        nv.created = std::move(result.created);
        nv.timeMs = timeMs;
        nv.session = std::make_shared<const Session>(std::move(result.session));
        state = nv.session;
        parent = nv.id;
        staged.push_back(std::move(nv));
    }
    // Success: the last step also records the source head as a parent, so a
    // later merge of the same branch finds nothing new.
    staged.back().parents.push_back(src->head);
    for (auto& nv : staged) {
        const std::uint64_t id = nv.id;
        versions_.emplace(id, std::move(nv));
    }
    next_ = nextId;
    dst.head = parent;
    for (std::size_t k = 0; k < kKindCount; ++k)
        dst.allocated.next[k] = std::max(dst.allocated.next[k], state->next.next[k]);
    dst.redo.clear();
    out.head = dst.head;
    dst.view.reset();
    state.reset();
    enforceBudget();
    return out;
}

}  // namespace supple::model
