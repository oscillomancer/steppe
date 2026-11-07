// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <memory>

#include "supple/engine/app.h"
#include "supple/model/freeze.h"
#include "supple/model/groups.h"
#include "supple/model/numeric.h"

namespace supple::engine {
// Omission leaves a field unchanged; explicit null clears it. Preserve null
// inside the optional rather than letting the normal optional decoder erase it.
inline schema::Schema<std::optional<json::Value>> clearableString(
    schema::Schema<std::string> inner) {
    auto ex = json::Value::object();
    ex["anyOf"] = json::Value::array();
    ex["anyOf"].push_back(inner.jsonSchema());
    auto null = json::Value::object();
    null["type"] = "null";
    ex["anyOf"].push_back(std::move(null));
    return {"clearable_string",
            [inner](const json::Value& v, std::optional<json::Value>& out, schema::DecodeCtx& ctx,
                    const std::string& path) {
                if (!v.isNull()) {
                    std::string value;
                    if (!inner.decodeInto(v, value, ctx, path)) return false;
                }
                out = v;
                return true;
            },
            [](const std::optional<json::Value>& v) { return v.value_or(json::Value()); }, ex};
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
    for (const auto& e : s.items)
        if (std::find(e.value->takes.begin(), e.value->takes.end(), take) != e.value->takes.end())
            return checkUnlocked(s, e.id, std::move(path));
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

// Validate derived edit values before the invocation succeeds, so a caught Lua
// error restores this invocation's savepoint even inside a larger transaction.
template <class Args, class Output>
void addChecked(cmd::Registry& registry, cmd::Command<Args, Output> command) {
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
            if (name != "track.unfreeze" && name != "track.freeze.commit" &&
                name != "track.remove" && name != "track.duplicate") {
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
                                    f.key != "mute" && f.key != "solo" && f.key != "name" &&
                                    f.key != "color" && f.key != "notes" &&
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
