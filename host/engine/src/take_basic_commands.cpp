// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Take commands: take.add, take.activate, take.set and take.list.
#include "extra_command_helpers.h"

namespace supple::engine {

namespace {

struct TakeActivateArgs {
    std::string item;
    std::optional<std::string> take;
    std::optional<std::int64_t> index;  // 0-based, in the item's take order
};

struct ItemQuery {
    std::string item;
};

struct TakeAddArgs {
    std::string item;
    std::optional<std::string> from;
};

struct TakeSetArgs {
    std::string take;
    std::optional<double> gainDb, pan, startOffset, playrate, pitch, rebaseSeconds;
    std::optional<model::ChannelMode> channelMode;
    std::optional<std::string> name, pitchMode;
    std::optional<bool> preservePitch;
};

struct TakeEntry {
    std::string id, name, source;
    bool active = false;
    double gainDb = 0, pan = 0, startOffset = 0;
    double playrate = 1, pitch = 0;
    bool preservePitch = true;
    std::string pitchMode;
    json::Value markers = json::Value::array(), hits = json::Value(), peers = json::Value::array();
    bool hitsStale = false;
};

struct TakeList {
    std::vector<TakeEntry> takes;
};

}  // namespace

void registerTakeBasicCommands(cmd::Registry& registry) {
    [&] {
        Command<TakeAddArgs, IdResult> c;
        c.name = "take.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a take to an item, optionally copied from another take of that item.";
        c.tags = {"take"};
        c.args = schema::object<TakeAddArgs>("take_add_args")
                     .field("item", &TakeAddArgs::item, idSchema("it"))
                     .field("from", &TakeAddArgs::from, idSchema("tk"))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"Copy a take", j(R"({"item":"it_1","from":"tk_1"})"), j(R"({"id":"tk_2"})"),
                       "song_with_item"}};
        c.run = [](const TakeAddArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            model::Take copy;
            if (a.from) {
                auto source = require<model::Take>(s, *a.from, "/from");
                if (!source) return std::move(source).error();
                const auto& takes = s.items.find(*item)->takes;
                if (std::find(takes.begin(), takes.end(), *source) == takes.end())
                    return Error(
                        "take.not_in_item", *a.from + " does not belong to " + a.item, "/from",
                        "choose a from take in the destination item; take.list lists its takes");
                auto takeSource = dangling<model::Take>(s, *source, "/from");
                if (!takeSource) return std::move(takeSource).error();
                copy = **takeSource;
                unpoolCopiedSource(copy, s, app(ctx).txn());
                copy.chain.clear();
                for (Id fx : (*takeSource)->chain) {
                    auto chained = dangling<model::FxInstance>(s, fx, "/takes");
                    if (!chained) return std::move(chained).error();
                    copy.chain.push_back(app(ctx).txn().create(**chained));
                }
            }
            auto& txn = app(ctx).txn();
            Id fresh = txn.create(std::move(copy));
            txn.modify<model::Item>(*item, [&](model::Item& it) {
                it.takes.push_back(fresh);
                if (!it.activeTake) it.activeTake = fresh;
            });
            return IdResult{fresh.str()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeActivateArgs, Empty> c;
        c.name = "take.activate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Choose the take heard from an item.";
        c.description =
            "take (its id) or index (0-based in the item's take order — the same position "
            "after item.split, which copies the takes in order: comping splits an item and "
            "picks a take by index on the piece).";
        c.tags = {"take"};
        c.args = schema::object<TakeActivateArgs>("take_activate_args")
                     .field("item", &TakeActivateArgs::item, idSchema("it"))
                     .field("take", &TakeActivateArgs::take, idSchema("tk"))
                     .field("index", &TakeActivateArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Activate", j(R"({"item":"it_1","take":"tk_1"})"), j("{}"), "song_with_item"}};
        c.run = [](const TakeActivateArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            const auto& takes = s.items.find(*item)->takes;
            if (a.take.has_value() == a.index.has_value())
                return Error("take.choice", "give take or index", "", "take: \"tk_2\" or index: 1");
            std::optional<Id> take;
            if (a.index) {
                if (*a.index >= static_cast<std::int64_t>(takes.size()))
                    return Error("take.index",
                                 a.item + " has " + std::to_string(takes.size()) + " takes",
                                 "/index", "use take.list for this item");
                take = takes[static_cast<std::size_t>(*a.index)];
            } else {
                auto found = require<model::Take>(s, *a.take, "/take");
                if (!found) return std::move(found).error();
                take = *found;
                if (std::find(takes.begin(), takes.end(), *take) == takes.end())
                    return Error("take.not_in_item", *a.take + " does not belong to " + a.item,
                                 "/take", "use take.list for this item");
            }
            app(ctx).txn().modify<model::Item>(*item,
                                               [&](model::Item& it) { it.activeTake = *take; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeSetArgs, Empty> c;
        c.name = "take.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set take gain, pan, channel mode, name, source offset, or playrate and pitch.";
        c.tags = {"take"};
        c.args =
            schema::object<TakeSetArgs>("take_set_args")
                .field("take", &TakeSetArgs::take, idSchema("tk"))
                .field("gain_db", &TakeSetArgs::gainDb,
                       schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                .field("pan", &TakeSetArgs::pan,
                       schema::number().range(-100, 100).unit(units::Unit::percent))
                .field("channel_mode", &TakeSetArgs::channelMode,
                       schema::enumeration<model::ChannelMode>(
                           {{model::ChannelMode::Normal, "normal"},
                            {model::ChannelMode::ReverseStereo, "reverse_stereo"},
                            {model::ChannelMode::MonoMix, "mono_mix"},
                            {model::ChannelMode::Left, "left"},
                            {model::ChannelMode::Right, "right"}}))
                .field("name", &TakeSetArgs::name, schema::string())
                .field("rebase_seconds", &TakeSetArgs::rebaseSeconds,
                       schema::number().doc("Rebase an audio take after trimming its item start; "
                                            "preserves the warp map."))
                .field("start_offset", &TakeSetArgs::startOffset,
                       schema::number().doc(
                           "Source position at the item start; negative for a late first pass."))
                .field("playrate", &TakeSetArgs::playrate,
                       schema::number()
                           .range(0.01, 100)
                           .unit(units::Unit::x)
                           .doc("Playback speed: 2 plays the source twice as fast. Its length in "
                                "seconds is the item's, so the take runs out sooner or loops."))
                .field("preserve_pitch", &TakeSetArgs::preservePitch,
                       schema::boolean().doc(
                           "Keep the source's pitch when the playrate changes (the default). "
                           "With pitch_mode \"resample\" the pitch follows the speed."))
                .field("pitch", &TakeSetArgs::pitch,
                       schema::number()
                           .range(-48, 48)
                           .unit(units::Unit::st)
                           .doc("Pitch offset in semitones. Stretched when the pitch is preserved, "
                                "otherwise a further speed factor of 2^(pitch/12)."))
                .field("pitch_mode", &TakeSetArgs::pitchMode,
                       schema::enumeration<std::string>(
                           {{"resample", "resample"}, {"signalsmith", "signalsmith"}})
                           .doc("Time-stretch algorithm: \"signalsmith\" (pitch-preserving, "
                                "the default) or \"resample\" (varispeed)."))
                .build();
        c.result = emptySchema();
        c.examples = {
            {"Lower a take", j(R"({"take":"tk_1","gain_db":"-3 dB"})"), j("{}"), "song_with_item"},
            {"Twice as fast, pitch and all",
             j(R"({"take":"tk_1","playrate":2,"preserve_pitch":false})"), j("{}"),
             "song_with_item"},
            {"Half speed, pitch kept", j(R"({"take":"tk_1","playrate":0.5})"), j("{}"),
             "song_with_item"},
            {"An octave up, same tempo", j(R"({"take":"tk_1","pitch":12})"), j("{}"),
             "song_with_item"}};
        c.run = [](const TakeSetArgs& a, Context& ctx) -> Result<Empty> {
            auto id = require<model::Take>(app(ctx).view(), a.take, "/take");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(checkUnlockedTake(app(ctx).view(), *id, "/take"));
            if (a.rebaseSeconds && a.startOffset)
                return Error("take.offset", "choose one source-position edit", "/rebase_seconds",
                             "supply rebase_seconds or start_offset, not both");
            const auto* original = app(ctx).view().takes.find(*id);
            const bool validWarp = !original->stretchMarkers.empty() &&
                                   static_cast<bool>(model::validateWarpMap(*original));
            app(ctx).txn().modify<model::Take>(*id, [&](model::Take& tk) {
                if (a.gainDb) tk.gainDb = *a.gainDb;
                if (a.pan) tk.pan = *a.pan;
                if (a.channelMode) tk.channelMode = *a.channelMode;
                if (a.name) tk.name = *a.name;
                if (a.rebaseSeconds) model::rebaseAudioTake(tk, *a.rebaseSeconds);
                if (a.startOffset) model::slipAudioTake(tk, *a.startOffset - tk.startOffset);
                if (a.playrate) model::rateAudioTake(tk, *a.playrate);
                if (a.preservePitch) tk.preservePitch = *a.preservePitch;
                if (a.pitch) tk.pitch = *a.pitch;
                if (a.pitchMode) tk.pitchMode = *a.pitchMode;
            });
            if (validWarp)
                SUPPLE_TRY(model::validateWarpMap(*app(ctx).view().takes.find(*id), "/take"));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemQuery, TakeList> c;
        c.name = "take.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the takes of an item and identify its active take.";
        c.tags = {"take"};
        c.args = schema::object<ItemQuery>("take_list_args")
                     .field("item", &ItemQuery::item, idSchema("it"))
                     .build();
        auto entry =
            schema::object<TakeEntry>("take_entry")
                .field("id", &TakeEntry::id, schema::string())
                .field("name", &TakeEntry::name, schema::string())
                .field("source", &TakeEntry::source, schema::string())
                .field("active", &TakeEntry::active, schema::boolean())
                .field("gain_db", &TakeEntry::gainDb, schema::number())
                .field("pan", &TakeEntry::pan, schema::number())
                .field("start_offset", &TakeEntry::startOffset, schema::number())
                .field("playrate", &TakeEntry::playrate, schema::number().unit(units::Unit::x))
                .field("preserve_pitch", &TakeEntry::preservePitch, schema::boolean())
                .field("pitch", &TakeEntry::pitch, schema::number().unit(units::Unit::st))
                .field("pitch_mode", &TakeEntry::pitchMode, schema::string())
                .field("stretch_markers", &TakeEntry::markers, schema::any())
                .field("transients", &TakeEntry::hits, schema::any())
                .field("warp_peers", &TakeEntry::peers, schema::any())
                .field("hits_stale", &TakeEntry::hitsStale, schema::boolean())
                .build();
        c.result = schema::object<TakeList>("take_list")
                       .field("takes", &TakeList::takes, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"List", j(R"({"item":"it_1"})"), j(R"({"takes":"<any>"})"), "song_with_item"}};
        c.run = [](const ItemQuery& a, Context& ctx) -> Result<TakeList> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            TakeList out;
            const auto& it = *s.items.find(*id);
            for (Id takeId : it.takes) {
                auto found = dangling<model::Take>(s, takeId, "/item");
                if (!found) return std::move(found).error();
                const auto& tk = **found;
                out.takes.push_back({takeId.str(), tk.name, tk.source ? tk.source->str() : "",
                                     it.activeTake == takeId, tk.gainDb, tk.pan, tk.startOffset,
                                     tk.playrate, tk.pitch, tk.preservePitch, tk.pitchMode});
                auto encoded = model::toJson(tk);
                auto& listedTake = out.takes.back();
                if (const auto* v = encoded.find("stretch_markers")) listedTake.markers = *v;
                if (const auto* v = encoded.find("transients")) listedTake.hits = *v;
                if (const auto* v = encoded.find("warp_peers")) listedTake.peers = *v;
                const auto* md = tk.source ? s.media.find(*tk.source) : nullptr;
                listedTake.hitsStale =
                    tk.transients && (!md || md->hash != tk.transients->sourceHash);
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
