// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Takes as material of their own (session-model §7): delete one, take one out
// of its item, spread an item's takes onto tracks or one after another, and
// render an item through its FX as a new audio take (MIDI through the track's
// instrument). Every command is one undo step.
#include <algorithm>
#include <cmath>
#include <set>

#include "command_validation.h"
#include "media_files.h"
#include "supple/audio/audio_file.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {

namespace fs = std::filesystem;
using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
json::Value j(std::string_view s) { return json::parse(s).value(); }

auto idSchema(const char* prefix) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$", "object id");
}

template <class T>
Result<Id> require(const model::Session& s, const std::string& text, std::string path) {
    auto id = Id::parse(text);
    if (id && id->kind == model::Traits<T>::kind && model::Traits<T>::map(s).contains(*id))
        return *id;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown", "no " + text,
                 std::move(path), "use a live id from the corresponding list command");
}

Result<json::Value> invoke(const char* name, json::Value args, Context& ctx) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
}

// The item holding `take`.
Result<Id> itemOfTake(const model::Session& s, Id take, const std::string& path) {
    for (const auto& e : s.items)
        if (std::find(e.value->takes.begin(), e.value->takes.end(), take) != e.value->takes.end())
            return e.id;
    return Error("take.orphan", take.str() + " belongs to no item", path,
                 "use a take from take.list");
}

// A track that may hold an item with `takes` (ADR-0057: channels, or masters for audio).
Result<Id> itemTrack(const model::Session& s, const std::string& text, const std::string& path,
                     const std::vector<Id>& takes) {
    auto track = require<model::Track>(s, text, path);
    if (!track) return track;
    SUPPLE_TRY(checkItemTrack(s, *track, takes, path));
    return track;
}

// `item` without `take`; the take heard next is the one after it, else the
// one before.
void dropTake(model::Item& item, Id take) {
    const auto at = std::find(item.takes.begin(), item.takes.end(), take);
    if (at == item.takes.end()) return;
    const auto index = static_cast<std::size_t>(at - item.takes.begin());
    item.takes.erase(at);
    if (item.activeTake == take)
        item.activeTake =
            item.takes.empty()
                ? std::nullopt
                : std::optional<Id>(item.takes[std::min(index, item.takes.size() - 1)]);
}

// A one-take item like `like` (fades, gain, timebase, colour…) on `track` at
// `startBeat`, holding `take`.
model::Item itemWith(const model::Session& s, const model::Item& like, Id take, Id track,
                     double startBeat) {
    model::Item copy = like;
    copy.takes = {take};
    copy.activeTake = take;
    copy.group.reset();
    copy.lane.reset();
    model::moveItem(s, copy, track, startBeat);
    return copy;
}

std::string takeName(const model::Session& s, Id take) {
    const model::Take* tk = s.takes.find(take);
    return tk ? tk->name : std::string();
}

struct TakeRef {
    std::string take;
};
struct TakeRemoveResult {
    std::string item;
    bool itemRemoved = false;
};
struct TakeExtractArgs {
    std::string take, track;
    std::optional<double> position;
};
struct ItemResult {
    std::string item;
};
struct ExplodeArgs {
    std::string item, mode = "tracks";
};
struct ExplodeResult {
    std::vector<std::string> items, tracks;
};
struct ApplyFxArgs {
    std::string item, mode = "take";
};
struct ApplyFxResult {
    std::string take, source, path;
};

// The rendered file: <media>/<name>.wav, or <name>-2.wav… when taken.
fs::path freshFile(const fs::path& dir, std::string name) {
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '/' || c == '\\'; }, '-');
    if (name.empty()) name = "render";
    std::error_code ec;
    for (int n = 1;; ++n) {
        const fs::path p = dir / (n == 1 ? name + ".wav" : name + "-" + std::to_string(n) + ".wav");
        if (!fs::exists(p, ec)) return p;
    }
}

}  // namespace

Status removeTakeObjects(model::EditTxn& t, const model::Session& s, Id take) {
    const model::Take* tk = s.takes.find(take);
    std::set<Id> gone{take};
    if (tk) {
        for (Id peer : tk->warpPeers) SUPPLE_TRY(checkUnlockedTake(s, peer, "/take"));
        for (Id peer : tk->warpPeers)
            if (peer != take && s.takes.contains(peer))
                t.modify<model::Take>(peer, [&](auto& p) {
                    std::erase(p.warpPeers, take);
                    if (p.warpPeers.size() < 2) p.warpPeers.clear();
                });
        for (Id f : tk->chain) {
            t.remove<model::FxInstance>(f);
            gone.insert(f);
        }
        if (tk->source && tk->source->kind == model::Kind::Midi) {
            const model::MidiSource* m = s.midi.find(*tk->source);
            bool shared = false;
            for (const auto& other : s.takes)
                if (other.id != take && other.value->source == tk->source) shared = true;
            if (m && !m->pooled && !shared) t.remove<model::MidiSource>(*tk->source);
        }
    }
    std::set<Id> removedEnvelopes;
    for (const auto& e : s.envelopes)
        if (gone.count(e.value->target.object)) {
            t.remove<model::Envelope>(e.id);
            removedEnvelopes.insert(e.id);
        }
    for (const auto& e : s.autoItems)
        if (removedEnvelopes.contains(e.value->envelope)) t.remove<model::AutomationItem>(e.id);
    t.remove<model::Take>(take);
    return {};
}

void registerTakeCommands(cmd::Registry& registry) {
    {
        Command<TakeRef, TakeRemoveResult> c;
        c.name = "take.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a take from its item (the item goes with its last take).";
        c.description =
            "The take's FX, envelopes and unshared MIDI go with it. When it was the heard take, "
            "the next take is heard (else the previous one). item_removed: it was the item's only "
            "take, so the item was removed too.";
        c.tags = {"take"};
        c.args = schema::object<TakeRef>("take_remove_args")
                     .field("take", &TakeRef::take, idSchema("tk"))
                     .build();
        c.result = schema::object<TakeRemoveResult>("take_remove_result")
                       .field("item", &TakeRemoveResult::item, schema::string())
                       .field("item_removed", &TakeRemoveResult::itemRemoved, schema::boolean())
                       .build();
        c.examples = {{"Delete the second take", j(R"({"take":"tk_2"})"),
                       j(R"({"item":"it_1","item_removed":false})"), "song_with_takes"}};
        c.run = [](const TakeRef& a, Context& ctx) -> Result<TakeRemoveResult> {
            const model::Session s = app(ctx).view();
            auto take = require<model::Take>(s, a.take, "/take");
            if (!take) return std::move(take).error();
            auto item = itemOfTake(s, *take, "/take");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item, "/take"));
            if (s.items.find(*item)->takes.size() == 1) {
                json::Value args = json::Value::object();
                args["item"] = item->str();
                SUPPLE_TRY(invoke("item.remove", std::move(args), ctx));
                return TakeRemoveResult{item->str(), true};
            }
            model::EditTxn& t = app(ctx).txn();
            SUPPLE_TRY(removeTakeObjects(t, s, *take));
            t.modify<model::Item>(*item, [&](model::Item& it) { dropTake(it, *take); });
            return TakeRemoveResult{item->str(), false};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<TakeExtractArgs, ItemResult> c;
        c.name = "take.extract";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Take a take out of its item into an item of its own on any channel.";
        c.description =
            "The new item copies the old one's length, fades, gain and timebase and starts at "
            "position (beats; default: where the item starts). The take keeps its FX and "
            "envelopes. The only take of an item moves the item itself.";
        c.tags = {"take", "item"};
        c.args = schema::object<TakeExtractArgs>("take_extract_args")
                     .field("take", &TakeExtractArgs::take, idSchema("tk"))
                     .field("track", &TakeExtractArgs::track, idSchema("tr"))
                     .field("position", &TakeExtractArgs::position,
                            schema::number().min(0).doc("Beats."))
                     .build();
        c.result = schema::object<ItemResult>("take_extract_result")
                       .field("item", &ItemResult::item, schema::string())
                       .build();
        c.examples = {{"Move take 2 to beat 8", j(R"({"take":"tk_2","track":"tr_1","position":8})"),
                       j(R"({"item":"it_2"})"), "song_with_takes"}};
        c.run = [](const TakeExtractArgs& a, Context& ctx) -> Result<ItemResult> {
            const model::Session s = app(ctx).view();
            auto take = require<model::Take>(s, a.take, "/take");
            if (!take) return std::move(take).error();
            auto item = itemOfTake(s, *take, "/take");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item, "/take"));
            auto track = itemTrack(s, a.track, "/track", {*take});
            if (!track) return std::move(track).error();
            const model::Item& from = *s.items.find(*item);
            const double at = a.position.value_or(model::itemSpan(s, from).startBeat);
            SUPPLE_TRY(model::checkedBeat(s, at, "/position"));
            model::EditTxn& t = app(ctx).txn();
            if (from.takes.size() == 1) {
                t.modify<model::Item>(*item, [&](model::Item& it) {
                    it.group.reset();
                    model::moveItem(s, it, *track, at);
                });
                return ItemResult{item->str()};
            }
            const Id made = t.create(itemWith(s, from, *take, *track, at));
            t.modify<model::Item>(*item, [&](model::Item& it) { dropTake(it, *take); });
            return ItemResult{made.str()};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<ExplodeArgs, ExplodeResult> c;
        c.name = "item.explode-takes";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Spread an item's takes onto new tracks, or one after another on its track.";
        c.description =
            "In recording order. mode tracks: the first take stays on the item's track and each "
            "other take gets a new channel right below it (named after the take) at the same "
            "time. mode serial: the takes follow each other on the item's own track, each as "
            "long as the item. Every take becomes a one-take item with its FX and envelopes.";
        c.tags = {"take", "item"};
        c.args = schema::object<ExplodeArgs>("item_explode_takes_args")
                     .field("item", &ExplodeArgs::item, idSchema("it"))
                     .field("mode", &ExplodeArgs::mode,
                            schema::string().pattern("^(tracks|serial)$", "tracks or serial"))
                     .defaultValue(std::string("tracks"))
                     .build();
        c.result = schema::object<ExplodeResult>("item_explode_takes_result")
                       .field("items", &ExplodeResult::items, schema::arrayOf(schema::string()))
                       .field("tracks", &ExplodeResult::tracks, schema::arrayOf(schema::string()))
                       .build();
        c.examples = {{"Takes onto tracks", j(R"({"item":"it_1"})"),
                       j(R"({"items":["it_1","<any>","<any>"],"tracks":["<any>","<any>"]})"),
                       "song_with_takes"},
                      {"Takes one after another", j(R"({"item":"it_1","mode":"serial"})"),
                       j(R"({"items":["it_1","<any>","<any>"],"tracks":[]})"), "song_with_takes"}};
        c.run = [](const ExplodeArgs& a, Context& ctx) -> Result<ExplodeResult> {
            const model::Session s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            const model::Item from = *s.items.find(*item);
            if (from.takes.size() < 2)
                return Error("take.single", a.item + " has only one take", "/item",
                             "record or add more takes first (take.add)");
            const model::Track& home = *s.tracks.find(from.track);
            const double start = model::itemSpan(s, from).startBeat;
            const double length = model::itemEndBeat(s, from) - start;
            ExplodeResult out;
            out.items.push_back(item->str());
            std::int64_t index = 0;
            if (home.parent) {
                const auto& siblings = s.tracks.find(*home.parent)->children;
                index = static_cast<std::int64_t>(
                    std::find(siblings.begin(), siblings.end(), from.track) - siblings.begin());
            }
            for (std::size_t k = 1; k < from.takes.size(); ++k) {
                const Id take = from.takes[k];
                Id track = from.track;
                double at = start + static_cast<double>(k) * length;
                if (a.mode == "tracks") {
                    json::Value add = json::Value::object();
                    std::string name = takeName(s, take);
                    if (name.empty()) name = home.name + " take " + std::to_string(k + 1);
                    add["name"] = name;
                    add["channels"] = static_cast<std::int64_t>(home.channels);
                    if (home.parent) add["parent"] = home.parent->str();
                    add["index"] = index + static_cast<std::int64_t>(k);
                    auto added = invoke("track.add", std::move(add), ctx);
                    if (!added) return std::move(added).error();
                    track = *Id::parse((*added)["track"].asString());
                    out.tracks.push_back(track.str());
                    at = start;
                }
                const model::Session now = app(ctx).view();
                const Id made = app(ctx).txn().create(itemWith(now, from, take, track, at));
                out.items.push_back(made.str());
            }
            app(ctx).txn().modify<model::Item>(*item, [&](model::Item& it) {
                it.takes = {from.takes.front()};
                it.activeTake = from.takes.front();
            });
            return out;
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<ApplyFxArgs, ApplyFxResult> c;
        c.name = "item.apply-fx";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Render an item through its take and track FX into a new audio take.";
        c.description =
            "Renders the heard take alone (other items on the track silent) through its take FX "
            "and the track's main chain, before the fader and pan, over the item's span: MIDI "
            "becomes audio through the track's instrument. The file goes to the project's media "
            "folder. mode take: a new take, heard, beside the old one (MIDI stays a take). mode "
            "replace: the new take takes the old one's place. The item's fades and gain still "
            "apply when it plays; the track FX still process the result, so bypass or move them "
            "if they should not apply twice.";
        c.tags = {"take", "item", "render"};
        c.args = schema::object<ApplyFxArgs>("item_apply_fx_args")
                     .field("item", &ApplyFxArgs::item, idSchema("it"))
                     .field("mode", &ApplyFxArgs::mode,
                            schema::string().pattern("^(take|replace)$", "take or replace"))
                     .defaultValue(std::string("take"))
                     .build();
        c.result = schema::object<ApplyFxResult>("item_apply_fx_result")
                       .field("take", &ApplyFxResult::take, schema::string())
                       .field("source", &ApplyFxResult::source, schema::string())
                       .field("path", &ApplyFxResult::path, schema::string())
                       .build();
        c.examples = {{"MIDI through the instrument, as a new take", j(R"({"item":"it_1"})"),
                       j(R"({"take":"<any>","source":"<any>","path":"<any>"})"), "song_with_fx"}};
        c.run = [](const ApplyFxArgs& a, Context& ctx) -> Result<ApplyFxResult> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            const model::Item& it = *s.items.find(*item);
            const model::Take* heard = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!heard || !heard->source)
                return Error("take.empty", a.item + " has no take with material", "/item",
                             "record or import into it first");
            // The item alone, as its take and FX shape it: other items on the
            // track silent, its own fades and gain left for playback.
            model::EditTxn render(s, s.version);
            for (const auto& e : s.items)
                if (e.value->track == it.track && e.id != *item)
                    render.modify<model::Item>(e.id, [](model::Item& x) { x.mute = true; });
            render.modify<model::Item>(*item, [](model::Item& x) {
                x.mute = false;
                x.gainDb = 0;
                x.fadeIn.length = 0;
                x.fadeOut.length = 0;
            });
            auto alone = render.commit();
            if (!alone) return std::move(alone).error();
            const model::ItemSpan span = model::itemSpan(s, it);
            OfflineSettings settings;
            settings.applyMasterPlayrate = false;
            settings.track = it.track;
            settings.tap = model::Tap::PreFader;
            settings.startSeconds = span.startSec;
            settings.endSeconds = span.endSec;
            auto rendered = engine.renderSession(*alone, settings);
            if (!rendered) return std::move(rendered).error();
            for (const auto& w : rendered->warnings) ctx.warn("render.warning", w);
            float peak = 0;
            for (const auto& ch : rendered->audio.channels)
                for (float v : ch) peak = std::max(peak, std::fabs(v));
            if (peak == 0)
                ctx.warn("render.silent",
                         "the render is silent: a MIDI take needs an instrument on its track");
            // Into the project's media folder (a headless unsaved project has
            // none of its own: <workdir>/media, named absolutely).
            const bool named = !engine.projectDir().empty();
            const fs::path dir = engine.mediaDir();
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path file =
                freshFile(dir, (heard->name.empty() ? std::string("item") : heard->name) + " fx");
            SUPPLE_TRY(audio::writeWav(file, rendered->audio));
            auto hash = sha256File(file);
            if (!hash) return std::move(hash).error();
            model::MediaSource md;
            md.path = named ? file.filename().string() : file.string();
            md.hash = *hash;
            md.format = "wav";
            md.sampleRate = rendered->audio.sampleRate;
            md.channels = static_cast<int>(rendered->audio.channels.size());
            md.frames = static_cast<std::int64_t>(rendered->audio.frames());
            model::EditTxn& t = engine.txn();
            const Id source = t.create(std::move(md));
            model::Take take;
            take.source = source;
            take.name = (heard->name.empty() ? std::string("Take") : heard->name) + " (FX)";
            const Id made = t.create(std::move(take));
            const Id old = *it.activeTake;
            if (a.mode == "replace") SUPPLE_TRY(removeTakeObjects(t, s, old));
            t.modify<model::Item>(*item, [&](model::Item& x) {
                auto at = std::find(x.takes.begin(), x.takes.end(), old);
                if (a.mode == "replace" && at != x.takes.end())
                    *at = made;
                else
                    x.takes.push_back(made);
                x.activeTake = made;
            });
            return ApplyFxResult{made.str(), source.str(), file.string()};
        };
        addChecked(registry, std::move(c));
    }
}

}  // namespace supple::engine
