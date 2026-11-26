// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "supple/engine/app.h"
#include "supple/model/freeze.h"
#include "supple/model/groups.h"
#include "supple/model/numeric.h"
#include "supple/model/take_owner.h"

namespace supple::engine {
// Omission leaves a field unchanged; explicit null clears it. Preserve null
// inside the optional rather than letting the normal optional decoder erase it.
// A value that is not null is checked against `inner`.
template <class T>
schema::Schema<std::optional<json::Value>> clearable(std::string name, schema::Schema<T> inner) {
    auto ex = json::Value::object();
    ex["anyOf"] = json::Value::array();
    ex["anyOf"].push_back(inner.jsonSchema());
    auto null = json::Value::object();
    null["type"] = "null";
    ex["anyOf"].push_back(std::move(null));
    return {std::move(name),
            [inner](const json::Value& v, std::optional<json::Value>& out, schema::DecodeCtx& ctx,
                    const std::string& path) {
                if (!v.isNull()) {
                    T value{};
                    if (!inner.decodeInto(v, value, ctx, path)) return false;
                }
                out = v;
                return true;
            },
            [](const std::optional<json::Value>& v) { return v.value_or(json::Value()); }, ex};
}
inline schema::Schema<std::optional<json::Value>> clearableString(
    schema::Schema<std::string> inner) {
    return clearable("clearable_string", std::move(inner));
}

// A locked item refuses every change except being unlocked (item.set
// {locked: false}); its takes and MIDI with it.
inline Status checkUnlocked(const model::Session& s, model::Id item, std::string path = "/item") {
    if (auto owner = model::frozenOwner(s, item)) return model::frozenError(s, *owner, path);
    if (const model::Item* it = s.items.find(item); it && it->locked)
        return Error("item.locked", item.str() + " is locked", std::move(path),
                     "unlock it with item.set {item: \"" + item.str() + "\", locked: false}");
    return {};
}
inline Status checkUnlockedTake(const model::Session& s, model::Id take, std::string path) {
    // A clip has no lock (ADR-0097); an item's take follows its item.
    if (const auto owner = model::takeOwner(s, take); owner && owner->kind == model::Kind::Item)
        return checkUnlocked(s, *owner, std::move(path));
    return {};
}

// Whether `track` may hold an item with these takes (ADR-0057): a channel any
// item, a master audio items only (its prints), a buss none. The takes are
// looked up in `from` (a copied version) when given.
inline Status checkItemTrack(const model::Session& s, model::Id track,
                             const std::vector<model::Id>& takes, std::string path,
                             const model::Session* from = nullptr) {
    const model::Track* t = s.tracks.find(track);
    if (!t || t->type == model::StripType::Channel) return {};
    if (t->type == model::StripType::Buss)
        return Error("track.strip_type", "only channels and masters hold items", std::move(path),
                     "put items on a child channel of the buss instead");
    for (model::Id id : takes)
        if (const model::Take* take = (from ? *from : s).takes.find(id);
            take && take->source && take->source->kind != model::Kind::Media)
            return Error("track.strip_type", "a master holds audio items only", std::move(path),
                         "put MIDI on a channel with an instrument; a master's items are "
                         "prints of its mix");
    return {};
}

// Removes a take with its FX, the envelopes on it and its FX, and its MIDI
// source when nothing else uses it (take.remove, item.remove; take_commands.cpp).
// The item's take list is the caller's to update.
Status removeTakeObjects(model::EditTxn& t, const model::Session& s, model::Id take);

namespace detail {
// The argument that names what a command copies, where a copy past the route
// limit is refused (ADR-0096 M5).
inline std::string copiedPath(std::string_view command) {
    if (command == "item.paste" || command == "item.consolidate" ||
        command == "item.consolidate.commit")
        return "/items";
    if (command == "item.add" || command == "clip.duplicate") return "/clip";
    if (command == "scene.capture") return "/region";
    if (command == "scene.insert") return "/scene";
    if (command == "arrangement.build") return "/sequence";
    if (command == "track.duplicate") return "/tracks";
    if (command == "launch.commit") return "/performance";
    if (command == "item.duplicate" || command == "item.split" || command == "clip.add")
        return "/item";
    if (command == "take.add") return "/from";
    return "";
}
// Every edit that can close a modulation cycle (review rounds 195 F2, 198
// F1, 199, 200): one row per command and argument, under the kind of edge
// the model's cycle check walks that it makes or redirects
// (model::CycleEdgeKind: the check adds every edge with its kind, and a test
// holds each kind to its rows and these rows to session-model.md's table).
// A row with a field applies when the edit gives that argument; one without
// applies to the command whatever it gives. The edit wrapper refuses and
// declares a row's edit (by: Wrapper). The modulation commands refuse and
// declare it in their own checks (Own). rack.patch adds its modules through
// module.add, which refuses at the module's entry (Nested); the wrapper
// declares it.
// Not rows: a new track (track.add, track.template.load) has no edge into
// it, and project.template.load replaces the whole project; a copy
// (track.duplicate, module.duplicate, the take copies) mirrors its source's
// edges, so only its copied modulation routes can close a cycle, and the
// copy settle drops those (modulation.not_copied, reason "cycle"); a remove
// that takes edges away cannot close one, but track.remove also leaves the
// Notes modules that played the track following their rack's channel; undo
// restores a state that held.
enum class CycleBy { Wrapper, Own, Nested };
struct CycleRow {
    model::CycleEdgeKind edge;
    std::string_view command;  // the command that makes or redirects it
    std::string_view field;    // the argument; "" for the command whatever it gives
    std::string_view path;     // where the refusal points
    std::string_view message;  // the registry's words for the cause
    std::string_view hint;
    CycleBy by = CycleBy::Wrapper;
};
using K = model::CycleEdgeKind;
inline constexpr std::array<CycleRow, 29> kCycleRows{{
    // A parent send: the child feeds its parent.
    {K::ParentSend, "track.move", "parent", "/parent",
     "the track's parent send would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or choose another parent"},
    {K::ParentSend, "track.set", "parent_send", "/parent_send",
     "the parent send would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or leave the parent send off"},
    // A route: its source feeds its destination (a track, a key's FX, a
    // rack's In; a rack's Out its channel).
    {K::Route, "route.add", "fx", "/fx", "the key route would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or key another plug-in"},
    {K::Route, "route.add", "to", "/to", "the send would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or send to another track"},
    {K::Route, "route.set", "to", "/to", "the send would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or keep the route's destination"},
    {K::Route, "route.set", "fx", "/fx", "the key route would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or keep the route's plug-in"},
    {K::Route, "route.set", "from", "/from", "the send would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or keep the route's source"},
    {K::Route, "route.set", "", "/route", "the route would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or leave the route as it was"},
    {K::Route, "track.set", "record_rack", "/record_rack",
     "the rack's Out as the channel's input would feed a rack what it modulates",
     "remove the modulation route first (modulation.remove), or pick another rack's Out"},
    // A MIDI send heard by a Notes module that follows the send's
    // destination (with its source's sends on): the send's source feeds
    // the module's rack.
    {K::NotesFollow, "route.add", "to", "/to",
     "the MIDI send would reach a Notes module in a rack that modulates its source",
     "remove the modulation route first (modulation.remove), or send to another track"},
    {K::NotesFollow, "route.set", "to", "/to",
     "the MIDI send would reach a Notes module in a rack that modulates its source",
     "remove the modulation route first (modulation.remove), or keep the route's destination"},
    {K::NotesFollow, "route.set", "from", "/from",
     "the MIDI send would reach a Notes module in a rack that modulates its source",
     "remove the modulation route first (modulation.remove), or keep the route's source"},
    {K::NotesFollow, "module.add", "source", "/source",
     "the Notes module would hear a MIDI send from what its rack modulates",
     "remove the modulation route first (modulation.remove), or choose another source"},
    {K::NotesFollow, "module.add", "", "/rack",
     "the Notes module, following its rack's channel, would hear a MIDI send from what its rack "
     "modulates",
     "remove the modulation route first (modulation.remove), or give the Notes module a source"},
    {K::NotesFollow, "module.set", "source", "/source",
     "the Notes module would hear a MIDI send from what its rack modulates",
     "remove the modulation route first (modulation.remove), or choose another source"},
    {K::NotesFollow, "module.set", "source_sends", "/source_sends",
     "the Notes module would hear a MIDI send from what its rack modulates",
     "remove the modulation route first (modulation.remove), or leave its source's sends off"},
    {K::NotesFollow, "rack.set", "track", "/track",
     "on that channel the rack's Notes module would hear what the rack modulates",
     "remove the modulation route first (modulation.remove), or keep the rack on its channel"},
    {K::NotesFollow, "track.remove", "", "/track",
     "the Notes modules that played the track would follow their rack's channel and hear what "
     "the rack modulates",
     "remove the modulation route first (modulation.remove), or give those Notes modules another "
     "source (module.set) first"},
    {K::NotesFollow, "rack.paste", "", "/rack",
     "a pasted Notes module would hear a MIDI send from what the rack modulates",
     "remove the modulation route (modulation.remove) or the MIDI send the Notes module would "
     "hear, or paste into another rack"},
    {K::NotesFollow, "rack.preset.load", "", "/rack",
     "the preset's Notes module, following the rack's channel, would hear a MIDI send from what "
     "the rack modulates",
     "remove the modulation route (modulation.remove) or the MIDI send into the rack's channel, "
     "or load the preset into another rack"},
    {K::NotesFollow, "rack.patch", "modules", "/modules",
     "a Notes module of the patch would hear a MIDI send from what the rack modulates",
     "refused as module.add refuses, at the module's entry (/modules/<name>/source or "
     "/modules/<name>/rack): remove the modulation route first (modulation.remove), or give "
     "the Notes module another source",
     CycleBy::Nested},
    // A modulation route: its source's rack feeds the node of its target.
    {K::Modulation, "modulation.add", "", "/target", "", "", CycleBy::Own},
    {K::Modulation, "modulation.set", "", "/target", "", "", CycleBy::Own},
    {K::Modulation, "modulate", "", "/target", "", "", CycleBy::Own},
    {K::Modulation, "item.move", "track", "/track",
     "the move puts a modulated take or FX where its rack hears it",
     "remove the route, or choose another track"},
    {K::Modulation, "clip.move", "track", "/track",
     "the move puts a modulated take or FX where its rack hears it",
     "remove the route, or choose another track"},
    {K::Modulation, "take.extract", "track", "/track",
     "the move puts a modulated take or FX where its rack hears it",
     "remove the route, or choose another track"},
    {K::Modulation, "fx.move", "track", "/track",
     "the move puts a modulated take or FX where its rack hears it",
     "remove the route, or choose another track"},
    {K::Modulation, "item.explode-takes", "", "/item",
     "the move puts a modulated take or FX where its rack hears it",
     "remove the route, or use mode serial"},
}};
// The row an edit's refusal takes, of its command's rows whose argument it
// gives (a parent send or a source's sends turned off, or a record rack
// cleared, makes no edge; a Notes source cleared does: the module then
// follows its rack's channel):
// the first whose kind of edge is one the edit added to the cycle the check
// reports (round 200 F3), else the first of them, else the command's row
// without an argument.
inline bool makesEdge(const CycleRow& r, const json::Value& fields) {
    const json::Value* v = fields.find(std::string(r.field));
    if (!v) return false;
    if (r.field == "record_rack") return !v->isNull();
    if (r.field == "parent_send" || r.field == "source_sends") return !v->isBool() || v->asBool();
    return true;
}
inline const CycleRow* cycleRow(std::string_view command, const json::Value& fields,
                                const std::vector<model::CycleEdgeKind>& added = {}) {
    const CycleRow* fallback = nullptr;
    const CycleRow* given = nullptr;
    for (const CycleRow& r : kCycleRows) {
        if (r.by != CycleBy::Wrapper || r.command != command) continue;
        if (r.field.empty()) {
            if (!fallback) fallback = &r;
        } else if (makesEdge(r, fields)) {
            if (std::find(added.begin(), added.end(), r.edge) != added.end()) return &r;
            if (!given) given = &r;
        }
    }
    return given ? given : fallback;
}
// The kinds of the edges on the cycle `after` reports that `before` lacks.
inline std::vector<model::CycleEdgeKind> addedCycleEdges(const model::Session& before,
                                                         const model::Session& after) {
    const auto old = model::cycleEdges(before);
    std::vector<model::CycleEdgeKind> out;
    for (const model::CycleEdge& e : model::reportedCycle(after))
        if (std::find(old.begin(), old.end(), e) == old.end()) out.push_back(e.kind);
    return out;
}
// What the registry declares for a command that has rows: its rows' causes
// and hints, each at its argument when it has more than one.
struct CycleRefusal {
    std::string message, hint;
};
inline std::optional<CycleRefusal> closesCycle(std::string_view command) {
    std::vector<const CycleRow*> rows;
    for (const CycleRow& r : kCycleRows)
        if (r.by != CycleBy::Own && r.command == command &&
            std::none_of(rows.begin(), rows.end(),
                         [&](const CycleRow* x) { return x->path == r.path && x->hint == r.hint; }))
            rows.push_back(&r);
    if (rows.empty()) return std::nullopt;
    if (rows.size() == 1)
        return CycleRefusal{std::string(rows[0]->message), std::string(rows[0]->hint)};
    CycleRefusal out{"the edit would feed a rack what it modulates", ""};
    for (const CycleRow* r : rows)
        out.hint += (out.hint.empty() ? "" : "; ") + std::string("at ") + std::string(r->path) +
                    ": " + std::string(r->hint);
    return out;
}
}  // namespace detail

// Validate derived edit values before the invocation succeeds, so a caught Lua
// error restores this invocation's savepoint even inside a larger transaction.
template <class Args, class Output>
void addChecked(cmd::Registry& registry, cmd::Command<Args, Output> command) {
    // What the wrapper below answers for a copy or a move (ADR-0096 M5),
    // declared here with it, so a registry walk finds each.
    const auto declare = [&](cmd::ErrorSpec spec) {
        if (std::none_of(command.errors.begin(), command.errors.end(),
                         [&](const cmd::ErrorSpec& e) { return e.code == spec.code; }))
            command.errors.push_back(std::move(spec));
    };
    if (!detail::copiedPath(command.name).empty())
        declare({"modulation.limit", "the copy's routes would pass the project's route limit",
                 "remove routes first (modulation.list), for instance the copied chain's"});
    if (const auto cycle = detail::closesCycle(command.name))
        declare({"modulation.cycle", cycle->message, cycle->hint});
    if (command.kind != cmd::Kind::Query) {
        auto original = std::move(command.run);
        auto encoded = command.args;
        const auto name = command.name;
        const bool edit = command.kind == cmd::Kind::Edit;
        command.run = [original = std::move(original), encoded, name, edit](
                          const Args& a, cmd::Context& ctx) -> Result<Output> {
            auto& app = ctx.service<EngineApp>();
            const auto before = app.view();
            const auto fields = encoded.encode(a);
            // Scenes and clips are outside a frozen track's print (ADR-0097):
            // they stay editable, and copying a frozen item into a clip
            // leaves it as it is (validateFrozenTransition still checks).
            // launch.* answer for a frozen track themselves (launch.stop and
            // launch.arrangement change nothing, launch.clip refuses).
            if (name != "track.unfreeze" && name != "track.freeze.commit" &&
                name != "track.remove" && name != "track.duplicate" && !name.starts_with("clip.") &&
                !name.starts_with("scene.") && !name.starts_with("launch.")) {
                for (const auto& field : fields.asObject()) {
                    if (!field.value.isString()) continue;
                    if (field.key != "fx" && field.key != "item" && field.key != "take" &&
                        field.key != "source" && field.key != "env" && field.key != "track")
                        continue;
                    auto id = model::Id::parse(field.value.asString());
                    if (!id) continue;
                    auto owner = model::frozenOwner(before, *id);
                    if (!owner) continue;
                    if (field.key == "track") {
                        if (name == "track.move" || name.starts_with("track.learn.")) continue;
                        if (name == "track.set") {
                            bool sound = false;
                            for (const auto& f : fields.asObject())
                                if (f.key != "track" && f.key != "volume_db" && f.key != "pan" &&
                                    f.key != "pan_mode" && f.key != "width" &&
                                    f.key != "pan_left" && f.key != "pan_right" &&
                                    f.key != "pan_law" && f.key != "mute" && f.key != "solo" &&
                                    f.key != "name" && f.key != "color" && f.key != "notes" &&
                                    f.key != "parent_send" && f.key != "locked" &&
                                    f.key != "hidden_tcp" && f.key != "hidden_mixer" &&
                                    f.key != "bypass_groups")
                                    sound = true;
                            if (!sound) continue;
                        }
                        if (name.starts_with("route.") || name.starts_with("env.")) continue;
                    }
                    return model::frozenError(before, *owner, "/" + field.key);
                }
            }
            auto result = original(a, ctx);
            if (!result) return result.error();
            // The routes this edit's copies made (ADR-0096 M5), settled now
            // that the copies are placed: one the destination cannot keep is
            // named, never dropped silently, and a copy past the route limit
            // is refused whole. Each original whose copy stays lives on, so a
            // cut (copy, then remove) warns nothing for it.
            std::vector<model::Id> livesOn;
            if (app.inTransaction()) {
                for (const RouteNotCopied& d : settleCopiedRoutes(app.txn(), false, &livesOn))
                    ctx.warn("modulation.not_copied", d.message, "/modulations/" + d.original.str(),
                             std::string(model::reasonWord(d.reason)));
                const std::size_t routes = app.txn().preview().modulations.size();
                if (routes > model::kMaxModulations && routes > before.modulations.size())
                    return Error("modulation.limit",
                                 "the copy would make " + std::to_string(routes) +
                                     " modulation routes; a project holds at most " +
                                     std::to_string(model::kMaxModulations),
                                 // launch.commit without `performance`
                                 // commits the last capture: the root.
                                 name == "launch.commit" && !fields.find("performance")
                                     ? std::string()
                                     : detail::copiedPath(name),
                                 "remove at least " +
                                     std::to_string(routes - model::kMaxModulations) +
                                     " routes first (modulation.list), for instance those of "
                                     "the copied chain, which the copy duplicates");
                const auto& joined = app.txn().routesLivingOn();
                livesOn.insert(livesOn.end(), joined.begin(), joined.end());
                // An edit that closes a cycle is refused at its argument,
                // never mended by dropping a route: it must not change what
                // sounds.
                if (const detail::CycleRow* row = detail::cycleRow(name, fields);
                    row && !before.modulations.empty()) {
                    const model::Session after = app.txn().preview();
                    Status ok = model::checkInvariants(after);
                    if (!ok && ok.error().code == "modulation.cycle") {
                        row =
                            detail::cycleRow(name, fields, detail::addedCycleEdges(before, after));
                        Error e = ok.error();
                        e.path = std::string(row->path);
                        e.hint = std::string(row->hint);
                        return e;
                    }
                }
            }
            // Routes whose source or target this edit took away go with it,
            // each named in a warning (ADR-0096, "Cascades"); a route's own
            // commands refuse instead.
            // The transaction removes those whose object it removes; a route
            // whose target stopped fitting (a send turned MIDI) goes here.
            if (edit && app.inTransaction() && !name.starts_with("modulation.")) {
                const auto removed = [&](model::Id route, const model::Modulation& m,
                                         const std::string& why, model::RouteWarningReason reason) {
                    ctx.warn("modulation.removed",
                             route.str() + " (" + m.source.module.str() + "." + m.source.input +
                                 " to " + m.target.object.str() + " " + m.target.param +
                                 ") was removed: " + why,
                             "/modulations/" + route.str(), std::string(model::reasonWord(reason)));
                };
                const model::Session after = app.view();
                for (const auto& e : before.modulations) {
                    if (after.modulations.contains(e.id)) continue;
                    if (std::find(livesOn.begin(), livesOn.end(), e.id) != livesOn.end())
                        continue;  // its copy or its twin carries it (M5)
                    const model::Modulation& m = *e.value;
                    if (after.modules.contains(m.source.module))
                        removed(e.id, m, "its target " + m.target.object.str() + " was removed",
                                model::RouteWarningReason::TargetRemoved);
                    else
                        removed(e.id, m, "its source " + m.source.module.str() + " was removed",
                                model::RouteWarningReason::SourceRemoved);
                }
                for (const model::StrandedModulation& d : model::strandedModulations(after)) {
                    removed(d.route, *after.modulations.find(d.route), d.why, d.reason());
                    app.txn().remove<model::Modulation>(d.route);
                }
            }
            if (edit) SUPPLE_TRY(model::validateFrozenTransition(before, app.view()));
            return result;
        };
    }
    if (command.kind == cmd::Kind::Edit) {
        auto run = std::move(command.run);
        // Shared, not copied: commands are copied with their closures, and a
        // schema copy is a deep copy of its JSON and field closures.
        auto args = std::make_shared<const decltype(command.args)>(command.args);
        const auto commandName = command.name;
        command.run = [run = std::move(run), args, commandName](
                          const Args& a, cmd::Context& ctx) -> Result<Output> {
            auto& owner = ctx.service<EngineApp>();
            const auto before = owner.view();
            std::vector<model::Id> linked;
            if constexpr (requires {
                              a.item;
                              a.bypassGroups;
                          }) {
                if (!a.bypassGroups &&
                    (commandName == "item.set" || commandName == "item.move" ||
                     commandName == "item.split" || commandName == "item.remove" ||
                     commandName == "item.duplicate")) {
                    if (auto id = model::Id::parse(a.item); id && before.items.contains(*id)) {
                        linked = model::linkedItems(before, *id);
                        std::erase(linked, *id);
                        if constexpr (requires { a.track; })
                            if (!linked.empty() && a.track &&
                                *a.track != before.items.find(*id)->track.str())
                                return Error("group.edit_destination",
                                             "linked items require explicit destination mapping",
                                             "/track",
                                             "move on the same track or set bypass_groups true");
                        if (commandName == "item.set") {
                            const auto fields = args->encode(a);
                            if (!(fields.find("position") || fields.find("bar") ||
                                  fields.find("length") || fields.find("mute") ||
                                  fields.find("fade_in") || fields.find("fade_out") ||
                                  fields.find("fade_in_shape") || fields.find("fade_out_shape")))
                                linked.clear();
                        }
                        if (commandName != "item.duplicate")
                            for (auto peer : linked) SUPPLE_TRY(checkUnlocked(before, peer));
                    }
                }
            }
            auto result = run(a, ctx);
            if (!result) return result.error();
            if constexpr (requires {
                              a.item;
                              a.bypassGroups;
                          }) {
                auto fields = args->encode(a);
                // An explicit same-track destination describes only the source;
                // each matching item keeps its own track.
                if (commandName == "item.move") fields.asObject().erase("track");
                if (commandName == "item.set") {
                    // Presentation/content fields belong to the explicit item only.
                    for (const char* key :
                         {"name", "color", "locked", "gain_db", "loop", "timebase"})
                        fields.asObject().erase(key);
                    const bool edits =
                        fields.find("position") || fields.find("bar") || fields.find("length") ||
                        fields.find("mute") || fields.find("fade_in") || fields.find("fade_out") ||
                        fields.find("fade_in_shape") || fields.find("fade_out_shape");
                    if (!edits) linked.clear();
                }
                for (auto peer : linked) {
                    fields["item"] = peer.str();
                    fields["bypass_groups"] = true;
                    auto each = args->decode(fields);
                    if (!each) return each.error();
                    auto changed = run(*each, ctx);
                    if (!changed) return changed.error();
                    if constexpr (requires {
                                      result->linkedLeft;
                                      result->linkedRight;
                                  }) {
                        result->linkedLeft.push_back(changed->left);
                        result->linkedRight.push_back(changed->right);
                    }
                    if constexpr (requires {
                                      result->linked;
                                      changed->id;
                                  })
                        result->linked.push_back(changed->id);
                }
            }
            auto& app = ctx.service<EngineApp>();
            auto session = app.view();
            if (auto* engine = app.engine();
                engine && engine->config().sampleRate > session.project->sampleRate) {
                auto project = *session.project;
                project.sampleRate = engine->config().sampleRate;
                session.project = std::make_shared<const model::Project>(std::move(project));
            }
            if (auto valid = model::validateNumeric(session); !valid) {
                Error error = valid.error();
                // When an argument directly sets this field, identify that argument.
                const auto slash = error.path.rfind('/');
                const auto field = error.path.substr(slash == std::string::npos ? 0 : slash + 1);
                if (args->encode(a).find(field)) error.path = "/" + field;
                return error;
            }
            SUPPLE_TRY(model::validateGroups(session));
            return result;
        };
    }
    registry.add(std::move(command));
}
}  // namespace supple::engine
