// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include "command_validation.h"
#include "supple/model/groups.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {
using model::Id;
struct Empty {};
struct Ref {
    std::string group;
};
struct Add {
    std::string name;
    std::optional<std::string> color;
    std::optional<bool> active;
    std::optional<std::vector<Id>> members;
    std::optional<std::vector<std::string>> links;
};
struct Set {
    std::string group;
    std::optional<std::string> name, color;
    std::optional<bool> active;
    std::optional<std::vector<std::string>> links;
};
struct Members {
    std::string group;
    std::vector<Id> members;
};
struct VcaArgs {
    std::string group;
    std::optional<double> volume;
    std::optional<bool> mute, solo;
    std::optional<std::vector<Id>> followers;
};
struct VcaMembers {
    std::string group;
    std::optional<std::vector<Id>> tracks, vcas;
};
struct Entry {
    std::string group;
    model::TrackGroup state;
    std::vector<Id> controllers;
};
struct List {
    std::vector<Entry> groups;
};
struct Targets {
    std::optional<std::string> track, item;
    std::string attribute;
};
struct TargetResult {
    std::vector<Id> tracks, items;
};
struct Describe {
    std::string track;
    std::optional<double> at;
};
struct TrackResult {
    std::string track, name;
    model::StripType type;
    std::vector<Id> groups, vcas;
    double volume = 0, requested = 0, applied = 0, at = 0;
    bool mute = false, soloSilenced = false;
};
auto refSchema() {
    return schema::object<Ref>("group_ref")
        .field("group", &Ref::group, schema::string().pattern("^gr_[0-9]+$", "group id"))
        .build();
}
auto emptySchema() { return schema::object<Empty>("empty").build(); }
auto groupId() { return schema::string().pattern("^gr_[0-9]+$", "group id"); }
auto linksSchema() {
    return schema::arrayOf(schema::string().pattern(
        "^(volume|pan|mute|solo|armed|monitor|selection|edit)$", "linked attribute"));
}
auto entrySchema() {
    return schema::object<Entry>("group_entry")
        .field("group", &Entry::group, groupId())
        .field("state", &Entry::state, model::schemaOf<model::TrackGroup>())
        .field("controllers", &Entry::controllers, schema::arrayOf(model::idSchema()))
        .build();
}
Result<Id> group(const model::Session& s, const std::string& text, bool vca = false) {
    auto id = Id::parse(text);
    if (!id || !s.groups.contains(*id))
        return Error("group.unknown", "no group " + text, "/group", "use group.list or group.add");
    if (vca && !s.groups.find(*id)->vca)
        return Error("vca.missing", "group has no VCA", "/group", "add one with vca.add");
    return *id;
}
void canonical(std::vector<Id>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}
void removeVca(EngineApp& app, const model::Session& s, Id id) {
    for (const auto& e : s.envelopes)
        if (e.value->target.object == id) app.txn().remove<model::Envelope>(e.id);
    for (const auto& e : s.groups)
        if (e.value->vca)
            app.txn().modify<model::TrackGroup>(e.id,
                                                [&](auto& g) { std::erase(g.vca->followers, id); });
    app.txn().modify<model::TrackGroup>(id, [](auto& g) { g.vca.reset(); });
}
json::Value j(std::string_view text) { return json::parse(text).value(); }
template <class A, class R>
void common(cmd::Command<A, R>& c, std::string name, std::string summary) {
    c.name = std::move(name);
    c.summary = std::move(summary);
    c.kind = cmd::Kind::Edit;
    c.exposure = cmd::Exposure::McpExec;
    c.tags = {"group", "mixer"};
    c.errors = {{"group.unknown", "no such group", "use group.list"},
                {"group.member", "invalid track membership", "use each existing track once"},
                {"vca.cycle", "nested VCA control is cyclic", "remove the cycle's follower link"},
                {"vca.limit", "more than 32 controlling VCAs", "remove a control link"}};
}
}  // namespace
void registerGroupCommands(cmd::Registry& registry) {
    {
        cmd::Command<Add, Ref> c;
        common(c, "group.add", "Create a named track group with optional links and members.");
        c.args = schema::object<Add>("group_add_args")
                     .field("name", &Add::name, schema::string())
                     .field("color", &Add::color, schema::string())
                     .field("active", &Add::active, schema::boolean())
                     .field("members", &Add::members, schema::arrayOf(model::idSchema()))
                     .field("links", &Add::links, linksSchema())
                     .build();
        c.result = refSchema();
        c.examples = {{"Group the drums", j(R"({"name":"Drums","members":["tr_1"]})"),
                       j(R"({"group":"gr_1"})"), "song"}};
        c.run = [](const Add& a, cmd::Context& ctx) -> Result<Ref> {
            auto& app = ctx.service<EngineApp>();
            model::TrackGroup g;
            g.name = a.name;
            if (a.color) g.color = *a.color;
            if (a.active) g.active = *a.active;
            if (a.links) g.links = *a.links;
            if (a.members) g.members = *a.members;
            canonical(g.members);
            auto id = app.txn().create(g);
            SUPPLE_TRY(model::validateGroups(app.view()));
            return Ref{id.str()};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<Set, Empty> c;
        common(c, "group.set", "Change a group's name, color, links or active state.");
        c.args = schema::object<Set>("group_set_args")
                     .field("group", &Set::group, groupId())
                     .field("name", &Set::name, schema::string())
                     .field("color", &Set::color, schema::string())
                     .field("active", &Set::active, schema::boolean())
                     .field("links", &Set::links, linksSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Suspend linked edits", j(R"({"group":"gr_1","active":false})"), j("{}"),
                       "song_with_group"}};
        c.run = [](const Set& a, cmd::Context& ctx) -> Result<Empty> {
            auto& app = ctx.service<EngineApp>();
            auto id = group(app.view(), a.group);
            if (!id) return id.error();
            app.txn().modify<model::TrackGroup>(*id, [&](auto& g) {
                if (a.name) g.name = *a.name;
                if (a.color) g.color = *a.color;
                if (a.active) g.active = *a.active;
                if (a.links) g.links = *a.links;
            });
            SUPPLE_TRY(model::validateGroups(app.view()));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<Members, Empty> c;
        common(c, "group.members.set",
               "Replace a group's track membership; an empty array clears it.");
        c.args = schema::object<Members>("group_members_args")
                     .field("group", &Members::group, groupId())
                     .field("members", &Members::members, schema::arrayOf(model::idSchema()))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Assign a track", j(R"({"group":"gr_1","members":["tr_1"]})"), j("{}"),
                       "song_with_group"}};
        c.run = [](const Members& a, cmd::Context& ctx) -> Result<Empty> {
            auto& app = ctx.service<EngineApp>();
            auto id = group(app.view(), a.group);
            if (!id) return id.error();
            auto members = a.members;
            canonical(members);
            app.txn().modify<model::TrackGroup>(*id, [&](auto& g) { g.members = members; });
            SUPPLE_TRY(model::validateGroups(app.view()));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    for (bool vca : {false, true}) {
        cmd::Command<Ref, Empty> c;
        common(c, vca ? "vca.remove" : "group.remove",
               vca ? "Remove a VCA and its automation, keeping the track group."
                   : "Remove a track group and its VCA, keeping its tracks.");
        c.args = refSchema();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"group":"gr_1"})"), j("{}"), "song_with_vca"}};
        c.run = [vca](const Ref& a, cmd::Context& ctx) -> Result<Empty> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            auto id = group(s, a.group, vca);
            if (!id) return id.error();
            removeVca(app, s, *id);
            if (!vca) app.txn().remove<model::TrackGroup>(*id);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    for (bool create : {true, false}) {
        cmd::Command<VcaArgs, Empty> c;
        common(c, create ? "vca.add" : "vca.set",
               create ? "Add an audio-free VCA fader to a track group."
                      : "Set a VCA's level, mute or solo without moving member faders.");
        c.args = schema::object<VcaArgs>("vca_args")
                     .field("group", &VcaArgs::group, groupId())
                     .field("volume_db", &VcaArgs::volume,
                            schema::number().range(model::kSilenceDb, 12).unit(units::Unit::dB))
                     .field("mute", &VcaArgs::mute, schema::boolean())
                     .field("solo", &VcaArgs::solo, schema::boolean())
                     .field("followers", &VcaArgs::followers, schema::arrayOf(model::idSchema()))
                     .build();
        c.result = emptySchema();
        c.description =
            "VCA gain adds to member automation before the fader, capped at the Console 9 native "
            "top. Group suspension only suspends linked editing. Use vca.members.set to nest VCAs.";
        c.examples = {{"Lower the group", j(R"({"group":"gr_1","volume_db":-3})"), j("{}"),
                       create ? "song_with_group" : "song_with_vca"}};
        c.run = [create](const VcaArgs& a, cmd::Context& ctx) -> Result<Empty> {
            auto& app = ctx.service<EngineApp>();
            auto id = group(app.view(), a.group, !create);
            if (!id) return id.error();
            if (create && app.view().groups.find(*id)->vca)
                return Error("vca.exists", "group already has a VCA", "/group", "use vca.set");
            app.txn().modify<model::TrackGroup>(*id, [&](auto& g) {
                if (create) g.vca = model::Vca{};
                auto& v = *g.vca;
                if (a.volume) v.volumeDb = *a.volume;
                if (a.mute) v.mute = *a.mute;
                if (a.solo) v.solo = *a.solo;
                if (a.followers) {
                    v.followers = *a.followers;
                    canonical(v.followers);
                }
            });
            SUPPLE_TRY(model::validateGroups(app.view()));
            if (a.volume) app.recordAutomation(*id, "volume", *a.volume);
            if (a.mute) app.recordAutomation(*id, "mute", *a.mute ? 1 : 0);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<VcaMembers, Empty> c;
        common(c, "vca.members.set", "Replace the tracks or nested VCAs controlled by a VCA.");
        c.args = schema::object<VcaMembers>("vca_members_args")
                     .field("group", &VcaMembers::group, groupId())
                     .field("tracks", &VcaMembers::tracks, schema::arrayOf(model::idSchema()))
                     .field("vcas", &VcaMembers::vcas, schema::arrayOf(model::idSchema()))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Assign tracks", j(R"({"group":"gr_1","tracks":["tr_1"],"vcas":[]})"),
                       j("{}"), "song_with_vca"}};
        c.run = [](const VcaMembers& a, cmd::Context& ctx) -> Result<Empty> {
            auto& app = ctx.service<EngineApp>();
            auto id = group(app.view(), a.group, true);
            if (!id) return id.error();
            app.txn().modify<model::TrackGroup>(*id, [&](auto& g) {
                if (a.tracks) {
                    g.members = *a.tracks;
                    canonical(g.members);
                }
                if (a.vcas) {
                    g.vca->followers = *a.vcas;
                    canonical(g.vca->followers);
                }
            });
            SUPPLE_TRY(model::validateGroups(app.view()));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<cmd::NoArgs, List> c;
        common(c, "group.list", "List track groups, linked attributes and optional VCA controls.");
        c.kind = cmd::Kind::Query;
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        c.result = schema::object<List>("group_list")
                       .field("groups", &List::groups, schema::arrayOf(entrySchema()))
                       .build();
        c.examples = {{"List groups", j("{}"), j(R"({"groups":[]})"), "empty"}};
        c.run = [](const cmd::NoArgs&, cmd::Context& ctx) -> Result<List> {
            const auto s = ctx.service<EngineApp>().view();
            List out;
            for (const auto& e : s.groups)
                out.groups.push_back({e.id.str(), *e.value, model::vcaControllers(s, e.id)});
            return out;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<Ref, Entry> c;
        common(c, "group.describe", "Describe a track group's membership, links and VCA ancestry.");
        c.kind = cmd::Kind::Query;
        c.args = refSchema();
        c.result = entrySchema();
        c.examples = {{"Describe drums", j(R"({"group":"gr_1"})"),
                       j(R"({"group":"gr_1","state":"<any>","controllers":[]})"),
                       "song_with_group"}};
        c.run = [](const Ref& a, cmd::Context& ctx) -> Result<Entry> {
            const auto s = ctx.service<EngineApp>().view();
            auto id = group(s, a.group);
            if (!id) return id.error();
            return Entry{id->str(), *s.groups.find(*id), model::vcaControllers(s, *id)};
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<Targets, TargetResult> c;
        common(c, "group.targets",
               "Resolve linked tracks or matching items for an edit or selection.");
        c.kind = cmd::Kind::Query;
        c.args =
            schema::object<Targets>("group_targets_args")
                .field("track", &Targets::track,
                       schema::string().pattern("^tr_[0-9]+$", "track id"))
                .field("item", &Targets::item, schema::string().pattern("^it_[0-9]+$", "item id"))
                .field("attribute", &Targets::attribute,
                       schema::string().pattern(
                           "^(volume|pan|mute|solo|armed|monitor|selection|edit)$",
                           "linked attribute"))
                .build();
        c.result = schema::object<TargetResult>("group_targets")
                       .field("tracks", &TargetResult::tracks, schema::arrayOf(model::idSchema()))
                       .field("items", &TargetResult::items, schema::arrayOf(model::idSchema()))
                       .build();
        c.examples = {{"Resolve volume links", j(R"({"track":"tr_1","attribute":"volume"})"),
                       j(R"({"tracks":["tr_1"],"items":[]})"), "song_with_group"}};
        c.run = [](const Targets& a, cmd::Context& ctx) -> Result<TargetResult> {
            const auto s = ctx.service<EngineApp>().view();
            if (a.track.has_value() == a.item.has_value())
                return Error("group.target", "give one track or item", "", "omit the other target");
            auto id = Id::parse(a.track ? *a.track : *a.item);
            if (!id || (a.track ? !s.tracks.contains(*id) : !s.items.contains(*id)))
                return Error("group.target", "target does not exist", a.track ? "/track" : "/item",
                             "use track.list");
            TargetResult out;
            if (a.track)
                out.tracks = model::linkedTracks(s, *id, a.attribute);
            else {
                if (a.attribute != "edit")
                    return Error("group.attribute", "item targets use edit links", "/attribute",
                                 "use attribute edit");
                out.items = model::linkedItems(s, *id);
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }
    {
        cmd::Command<Describe, TrackResult> c;
        common(c, "track.describe",
               "Describe track groups and requested/applied effective gain at a time.");
        c.kind = cmd::Kind::Query;
        c.args = schema::object<Describe>("track_describe_args")
                     .field("track", &Describe::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("at", &Describe::at, schema::number().min(0))
                     .build();
        c.result =
            schema::object<TrackResult>("track_description")
                .field("track", &TrackResult::track, schema::string())
                .field("name", &TrackResult::name, schema::string())
                .field("type", &TrackResult::type, model::stripTypeSchema())
                .field("groups", &TrackResult::groups, schema::arrayOf(model::idSchema()))
                .field("vcas", &TrackResult::vcas, schema::arrayOf(model::idSchema()))
                .field("volume_db", &TrackResult::volume, schema::number())
                .field("requested_effective_volume_db", &TrackResult::requested, schema::number())
                .field("effective_volume_db", &TrackResult::applied, schema::number())
                .field("at", &TrackResult::at, schema::number())
                .field("effective_mute", &TrackResult::mute, schema::boolean())
                .field("solo_silenced", &TrackResult::soloSilenced, schema::boolean())
                .build();
        c.examples = {
            {"Inspect effective gain", j(R"({"track":"tr_1","at":0})"),
             j(R"({"track":"tr_1","name":"Bass","type":"channel","groups":[],"vcas":[],"volume_db":0.0,"requested_effective_volume_db":0.0,"effective_volume_db":0.0,"at":0.0,"effective_mute":false,"solo_silenced":false})"),
             "song"}};
        c.run = [](const Describe& a, cmd::Context& ctx) -> Result<TrackResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            auto id = Id::parse(a.track);
            const auto* t = id ? s.tracks.find(*id) : nullptr;
            if (!t) return Error("tr.unknown", "no such track", "/track", "use track.list");
            const double at = a.at.value_or(app.engine() ? app.engine()->transport().seconds : 0);
            auto level = model::effectiveTrackLevel(s, *id, at);
            return TrackResult{a.track,
                               t->name,
                               t->type,
                               model::trackGroups(s, *id),
                               model::vcaControllers(s, *id),
                               t->volumeDb,
                               level.requestedDb,
                               level.appliedDb,
                               at,
                               level.mute,
                               level.soloSilenced};
        };
        addChecked(registry, std::move(c));
    }
}
}  // namespace supple::engine
