// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Item edits: item.set, item.duplicate, item.paste and item.split.
#include "extra_command_helpers.h"

namespace supple::engine {

namespace {

// Search the same immutable source samples that playback reads. No decode,
// stretching or cache mutation belongs in this edit handler.
Result<double> priorZeroCrossing(EngineApp& owner, const model::Session& s, const model::Item& item,
                                 double cursor) {
    const auto* take = item.activeTake ? s.takes.find(*item.activeTake) : nullptr;
    if (!take || !take->source || take->source->kind != model::Kind::Media)
        return Error("item.split_not_audio", "zero-crossing split needs an active audio take",
                     "/item", "use snap none to split MIDI or empty items at the cursor");
    const auto span = model::itemSpan(s, item);
    if (!(cursor > span.startSec && cursor < span.endSec))
        return Error("item.split_range", "split must be inside the item", "/at",
                     "choose a beat strictly after the start and before the end");
    const auto& prepared = owner.media().sources();
    const Error pending("item.split_source_pending", "current source audio is not prepared",
                        "/item",
                        "start the engine (engine.start with backend dummy is sufficient), "
                        "wait for source preparation and retry");
    if (!prepared || prepared->session.project->mediaDir != s.project->mediaDir ||
        (owner.engine() && prepared->sampleRate != owner.engine()->config().sampleRate))
        return pending;
    const auto found = prepared->takes.find(*item.activeTake);
    if (found == prepared->takes.end()) return pending;
    const auto& source = found->second;
    // Per-take provenance lets a batch split several items from one snapshot.
    // Scalar variants do not depend on item placement or source offset.
    auto matches = [&](Id id) {
        const auto* now = s.takes.find(id);
        const auto* was = prepared->session.takes.find(id);
        if (!now || !was || now->source != was->source || now->playrate != was->playrate ||
            now->pitch != was->pitch || now->preservePitch != was->preservePitch ||
            now->pitchMode != was->pitchMode || now->stretchMarkers != was->stretchMarkers ||
            now->warpPeers != was->warpPeers)
            return false;
        const auto* media = now->source ? s.media.find(*now->source) : nullptr;
        const auto* oldMedia = was->source ? prepared->session.media.find(*was->source) : nullptr;
        return media && oldMedia && model::toJson(*media) == model::toJson(*oldMedia) &&
               (!source.itemLocal || now->startOffset == was->startOffset);
    };
    if (!matches(*item.activeTake)) return pending;
    if (source.itemLocal) {
        const model::Item* original = nullptr;
        for (const auto& entry : prepared->session.items)
            if (entry.value->activeTake == item.activeTake) original = entry.value.get();
        if (!original || item.loop != original->loop) return pending;
        const auto previous = model::itemSpan(prepared->session, *original);
        if (span.endSec - span.startSec != previous.endSec - previous.startSec) return pending;
        for (auto peer : take->warpPeers)
            if (!matches(peer)) return pending;
    } else if (model::activeWarp(*take) && !item.loop) {
        return pending;
    }
    if (source.error)
        return Error("item.split_source", source.error->message, "/item",
                     "relink or repair the audio source, then retry after source preparation");
    if (!source.data || !(prepared->sampleRate > 0)) return pending;
    const auto& audio = *source.data;
    const double rate = prepared->sampleRate;
    const auto frames = static_cast<std::int64_t>(audio.frames());
    if (audio.channels.empty() || frames == 0) return cursor;
    const MediaVariant variant{take->playrate, take->pitch, take->preservePitch, take->pitchMode};
    const auto offset =
        source.itemLocal ? 0 : time::samplesAt(take->startOffset, rate / variant.speed());
    const auto start = time::samplesAt(span.startSec, rate);
    const auto end = time::samplesAt(span.endSec, rate);
    const auto first =
        std::max(start + 1, static_cast<std::int64_t>(std::ceil((cursor - .1) * rate)));
    const auto last = std::min(end - 1, static_cast<std::int64_t>(std::floor(cursor * rate)));
    auto sample = [&](std::int64_t frame, std::size_t channel) {
        auto at = offset + frame - start;
        if (item.loop) at = ((at % frames) + frames) % frames;
        if (at < 0 || at >= frames) return 0.f;
        const auto n = static_cast<std::size_t>(at);
        float l = audio.channels[0][n];
        float r = audio.channels[audio.channelCount() > 1 ? 1 : 0][n];
        switch (take->channelMode) {
            case model::ChannelMode::Normal:
                break;
            case model::ChannelMode::ReverseStereo:
                std::swap(l, r);
                break;
            case model::ChannelMode::MonoMix:
                l = r = .5f * (l + r);
                break;
            case model::ChannelMode::Left:
                r = l;
                break;
            case model::ChannelMode::Right:
                l = r;
                break;
        }
        return channel == 0 ? l : r;
    };
    double energy[2]{};
    for (auto n = first; n <= last; ++n)
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double x = static_cast<double>(sample(n, ch));
            if (std::isfinite(x)) energy[ch] += x * x;
        }
    const std::size_t channel = energy[1] > energy[0] ? 1 : 0;
    // A fractional cursor in silence is already at zero; keep it exact.
    if (first <= last && sample(last, channel) == 0 && sample(last + 1, channel) == 0)
        return cursor;
    for (auto n = last; n >= first; --n) {
        const float x = sample(n, channel), previous = sample(n - 1, channel);
        const double at = static_cast<double>(n) / rate;
        if (at > span.startSec && at < span.endSec && at <= cursor && std::isfinite(x) &&
            std::isfinite(previous) &&
            (x == 0 || (previous < 0 && x > 0) || (previous > 0 && x < 0)))
            return at;
    }
    return cursor;
}

struct ItemSetArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> position, length, gainDb, fadeIn, fadeOut;
    std::optional<model::FadeShape> fadeInShape, fadeOutShape;
    std::optional<std::int64_t> bar;
    std::optional<bool> mute, loop, locked;
    std::optional<std::string> name, color;
    std::optional<model::ItemTimebase> timebase;
};

struct ItemDuplicateResult {
    std::string id;
    std::vector<std::string> linked;
};

struct ItemDuplicateArgs {
    bool bypassGroups = false;
    std::string item;
    std::optional<double> to;
};

struct PastePlacement {
    std::string item, track;
    double position = 0;
};

struct ItemPasteArgs {
    std::optional<std::int64_t> version;
    std::vector<PastePlacement> items;
};

struct ItemPasteResult {
    std::vector<std::string> items;
};

struct ItemSplitArgs {
    bool bypassGroups = false;
    std::string item;
    double at = 0;
    std::string snap = "none";
};

struct SplitResult {
    std::string left, right;
    std::vector<std::string> linkedLeft, linkedRight;
};

schema::Schema<model::FadeShape> fadeShapeSchema() {
    return schema::enumeration<model::FadeShape>({{model::FadeShape::Linear, "linear"},
                                                  {model::FadeShape::Slow, "slow"},
                                                  {model::FadeShape::Fast, "fast"},
                                                  {model::FadeShape::SlowFast, "slow_fast"},
                                                  {model::FadeShape::FastSlow, "fast_slow"},
                                                  {model::FadeShape::Cos, "cos"},
                                                  {model::FadeShape::Exp, "exp"}});
}

}  // namespace

void registerItemEditCommands(cmd::Registry& registry) {
    [&] {
        Command<ItemSetArgs, Empty> c;
        c.name = "item.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set an item's position, length, playback, fades, name, color, or lock.";
        c.description =
            "Positions and lengths are in beats; fades are in seconds. Name is the active take's "
            "name. A locked item refuses every edit except {\"locked\": false}. timebase: what "
            "the item follows when the tempo changes — beats (moves and stretches with it), "
            "time (stays at its seconds), beats_position (its start follows the beat, its "
            "length stays in seconds) or track (inherit); the item keeps its place now.";
        c.tags = {"item"};
        c.args = schema::object<ItemSetArgs>("item_set_args")
                     .field("bypass_groups", &ItemSetArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemSetArgs::item, idSchema("it"))
                     .field("position", &ItemSetArgs::position, schema::number().min(0))
                     .field("bar", &ItemSetArgs::bar,
                            schema::integer().range(1, std::numeric_limits<int>::max()))
                     .field("length", &ItemSetArgs::length, schema::number().min(0))
                     .field("mute", &ItemSetArgs::mute, schema::boolean())
                     .field("loop", &ItemSetArgs::loop, schema::boolean())
                     .field("locked", &ItemSetArgs::locked,
                            schema::boolean().doc("The arrange view marks locked items."))
                     .field("gain_db", &ItemSetArgs::gainDb,
                            schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                     .field("fade_in", &ItemSetArgs::fadeIn, schema::number().min(0))
                     .field("fade_in_shape", &ItemSetArgs::fadeInShape, fadeShapeSchema())
                     .field("fade_out_shape", &ItemSetArgs::fadeOutShape, fadeShapeSchema())
                     .field("fade_out", &ItemSetArgs::fadeOut, schema::number().min(0))
                     .field("name", &ItemSetArgs::name, schema::string())
                     .field("color", &ItemSetArgs::color, schema::string())
                     .field("timebase", &ItemSetArgs::timebase, model::itemTimebaseSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Lower an item", j(R"({"item":"it_1","gain_db":"-3 dB"})"), j("{}"), "song_with_item"},
            {"Keep an item at its time when the tempo changes",
             j(R"({"item":"it_1","timebase":"time"})"), j("{}"), "song_with_item"}};
        c.run = [](const ItemSetArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const auto& original = *s.items.find(*id);
            // A locked item refuses edits; unlocking it is the one exception.
            const bool unlocking = a.locked && !*a.locked;
            if (!unlocking) SUPPLE_TRY(checkUnlocked(s, *id));
            if (a.position && a.bar)
                return Error("item.position", "give position or bar, not both", "/position",
                             "remove one of them");
            if (a.length && !(*a.length > 0))
                return Error("item.length", "length must be greater than 0", "/length",
                             "give a positive length, or remove the item");
            model::Item it = original;
            if (a.timebase) {  // first: the rest is placed in the new domain
                const model::Timebase from = model::itemTimebase(s, it);
                it.timebase = *a.timebase;
                model::retimeItem(s, it, from, model::itemTimebase(s, it));
            }
            if (a.position || a.bar || a.length) {
                const double start = a.position ? *a.position
                                     : a.bar    ? s.timeMap->beatOfBar(static_cast<int>(*a.bar))
                                                : model::itemSpan(s, it).startBeat;
                if (a.length)
                    model::placeItem(s, it, start, *a.length);
                else
                    model::moveItem(s, it, it.track, start);
            }
            if (a.mute) it.mute = *a.mute;
            if (a.loop) it.loop = *a.loop;
            if (a.locked) it.locked = *a.locked;
            if (a.gainDb) it.gainDb = *a.gainDb;
            if (a.fadeIn) it.fadeIn.length = *a.fadeIn;
            if (a.fadeOut) it.fadeOut.length = *a.fadeOut;
            if (a.fadeInShape) it.fadeIn.shape = *a.fadeInShape;
            if (a.fadeOutShape) it.fadeOut.shape = *a.fadeOutShape;
            if (a.color) it.color = *a.color;
            if (a.name && !it.activeTake)
                return Error("item.no_take", "item has no active take to name", "/name",
                             "select an existing take with take.activate before setting name, or "
                             "add a take to the item");
            auto& t = app(ctx).txn();
            if (a.name)
                t.modify<model::Take>(*it.activeTake, [&](model::Take& tk) { tk.name = *a.name; });
            t.put<model::Item>(*id, std::move(it));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemDuplicateArgs, ItemDuplicateResult> c;
        c.name = "item.duplicate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Copy an item and its takes to a new beat position (default: immediately after).";
        c.tags = {"item"};
        c.args = schema::object<ItemDuplicateArgs>("item_duplicate_args")
                     .field("bypass_groups", &ItemDuplicateArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemDuplicateArgs::item, idSchema("it"))
                     .field("to", &ItemDuplicateArgs::to, schema::number().min(0))
                     .build();
        c.result =
            schema::object<ItemDuplicateResult>("item_duplicate_result")
                .field("id", &ItemDuplicateResult::id, schema::string())
                .field("linked", &ItemDuplicateResult::linked, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .build();
        c.examples = {{"Repeat", j(R"({"item":"it_1"})"), j(R"({"id":"it_2"})"), "song_with_item"}};
        c.run = [](const ItemDuplicateArgs& a, Context& ctx) -> Result<ItemDuplicateResult> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            model::Item copy = *s.items.find(*id);
            const double end = model::itemSpan(s, copy).startBeat + model::itemLengthBeats(s, copy);
            model::moveItem(s, copy, copy.track, a.to.value_or(end));
            copy.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldId : s.items.find(*id)->takes) {
                auto old = dangling<model::Take>(s, oldId, "/item");
                if (!old) return std::move(old).error();
                model::Take tk = **old;
                unpoolCopiedSource(tk, s, txn);
                auto cloned = copyTake(s, txn, oldId, std::move(tk));
                if (!cloned) return std::move(cloned).error();
                const Id fresh = *cloned;
                copy.takes.push_back(fresh);
                if (s.items.find(*id)->activeTake == oldId) copy.activeTake = fresh;
            }
            return ItemDuplicateResult{txn.create(std::move(copy)).str(), {}};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemPasteArgs, ItemPasteResult> c;
        c.name = "item.paste";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Copy items as they were in a version (a clipboard) to tracks and beats.";
        c.description =
            "version: where the items are read (default: now) — a GUI copy or cut remembers the "
            "version, so a paste reproduces the items as copied even after they were removed or "
            "edited. items: [{item, track, position}] (beats; channels only). Takes, fades, "
            "take FX and MIDI are copied; an audio source the project no longer has is restored.";
        c.tags = {"item"};
        auto placement = schema::object<PastePlacement>("paste_placement")
                             .field("item", &PastePlacement::item, idSchema("it"))
                             .field("track", &PastePlacement::track, idSchema("tr"))
                             .field("position", &PastePlacement::position, schema::number().min(0))
                             .build();
        c.args = schema::object<ItemPasteArgs>("item_paste_args")
                     .field("version", &ItemPasteArgs::version, schema::integer().min(1))
                     .field("items", &ItemPasteArgs::items, schema::arrayOf(placement))
                     .build();
        c.result = schema::object<ItemPasteResult>("item_paste_result")
                       .field("items", &ItemPasteResult::items, schema::arrayOf(schema::string()))
                       .build();
        c.examples = {{"Paste after itself",
                       j(R"({"items":[{"item":"it_1","track":"tr_1","position":16}]})"),
                       j(R"({"items":["it_2"]})"), "song_with_item"}};
        c.run = [](const ItemPasteArgs& a, Context& ctx) -> Result<ItemPasteResult> {
            EngineApp& eng = app(ctx);
            const auto now = eng.view();
            std::shared_ptr<const model::Session> from;
            if (a.version) {
                const model::Version* v =
                    eng.versions().version(static_cast<std::uint64_t>(*a.version));
                if (!v || !v->session)
                    return Error("history.version", "no version " + std::to_string(*a.version),
                                 "/version", "history.list shows the versions");
                from = v->session;
            } else {
                from = std::make_shared<const model::Session>(now);
            }
            const model::Session& s = *from;
            auto& txn = eng.txn();
            std::map<Id, std::vector<Id>> copiedTakes;
            std::map<Id, Id> restored;  // audio sources brought back, by their old id
            ItemPasteResult out;
            for (std::size_t i = 0; i < a.items.size(); ++i) {
                const PastePlacement& p = a.items[i];
                const std::string at = "/items/" + std::to_string(i);
                auto item = require<model::Item>(s, p.item, at + "/item");
                if (!item) return std::move(item).error();
                auto track = require<model::Track>(now, p.track, at + "/track");
                if (!track) return std::move(track).error();
                const model::Item& original = *s.items.find(*item);
                SUPPLE_TRY(checkItemTrack(now, *track, original.takes, at + "/track", &s));
                model::Item copy = original;
                // Its length as its timebase kept it in the copied version (beats
                // for beats, seconds otherwise), placed on today's tempo map.
                const model::ItemSpan span = model::itemSpan(s, original);
                const bool beats = model::itemTimebase(s, original) == model::Timebase::Beats;
                copy.track = *track;
                const double lengthBeats =
                    beats ? s.timeMap->beatAt(span.endSec) - span.startBeat
                          : now.timeMap->beatAt(now.timeMap->secondsAt(p.position) + span.endSec -
                                                span.startSec) -
                                p.position;
                model::placeItem(now, copy, p.position, lengthBeats);
                copy.takes.clear();
                copy.activeTake.reset();
                for (Id oldId : original.takes) {
                    auto old = dangling<model::Take>(s, oldId, at + "/item");
                    if (!old) return std::move(old).error();
                    model::Take tk = **old;
                    if (tk.source && tk.source->kind == model::Kind::Media &&
                        !now.media.find(*tk.source)) {
                        auto it = restored.find(*tk.source);
                        if (it == restored.end()) {
                            const model::MediaSource* md = s.media.find(*tk.source);
                            if (!md)
                                return Error("item.source", "the copied audio is gone", at,
                                             "recopy from a version containing the audio source, "
                                             "or reimport its audio_file");
                            it = restored.emplace(*tk.source, txn.create(*md)).first;
                        }
                        tk.source = it->second;
                    } else if (tk.source && tk.source->kind == model::Kind::Midi &&
                               !now.midi.find(*tk.source)) {
                        const model::MidiSource* mi = s.midi.find(*tk.source);
                        if (!mi)
                            return Error("item.source", "the copied MIDI is gone", at,
                                         "recopy from a version containing the MIDI source, or "
                                         "recreate the item with notation");
                        model::MidiSource fresh = *mi;
                        fresh.pooled = false;
                        tk.source = txn.create(std::move(fresh));
                    } else {
                        unpoolCopiedSource(tk, s, txn);
                    }
                    auto cloned = copyTake(s, txn, oldId, std::move(tk));
                    if (!cloned) return std::move(cloned).error();
                    const Id fresh = *cloned;
                    copiedTakes[oldId].push_back(fresh);
                    copy.takes.push_back(fresh);
                    if (original.activeTake == oldId) copy.activeTake = fresh;
                }
                out.items.push_back(txn.create(std::move(copy)).str());
            }
            for (const auto& [oldId, copies] : copiedTakes) {
                const auto& original = *s.takes.find(oldId);
                if (copies.size() != 1 || original.warpPeers.empty()) continue;
                std::vector<Id> peers;
                for (auto peer : original.warpPeers) {
                    const auto found = copiedTakes.find(peer);
                    if (found == copiedTakes.end() || found->second.size() != 1) {
                        peers.clear();
                        break;
                    }
                    peers.push_back(found->second.front());
                }
                if (peers.size() != original.warpPeers.size()) continue;
                std::sort(peers.begin(), peers.end());
                txn.modify<model::Take>(copies.front(),
                                        [&](auto& take) { take.warpPeers = peers; });
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemSplitArgs, SplitResult> c;
        c.name = "item.split";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Split an item at a beat, optionally at the prior audio zero crossing.";
        c.description =
            "snap defaults to none. zero_crossing searches the active take's prepared playback "
            "source up to 100 ms before at, before FX/fades/gain. After channel mode, use the "
            "louder stereo channel by window energy (left on ties), never a stereo sum unless "
            "MonoMix is selected. Choose the latest exact zero or sign-change sample boundary; "
            "never search forward or outside the item. Without a crossing, split at at. MIDI "
            "and empty takes refuse zero_crossing; the GUI skips them. Current prepared audio "
            "must be available; start the engine and retry after preparation. Linked items use "
            "the initiating item's resolved split time. History records that time for replay.";
        c.tags = {"item"};
        c.args = schema::object<ItemSplitArgs>("item_split_args")
                     .field("bypass_groups", &ItemSplitArgs::bypassGroups, schema::boolean())
                     .defaultValue(false)
                     .field("item", &ItemSplitArgs::item, idSchema("it"))
                     .field("at", &ItemSplitArgs::at, schema::number().min(0))
                     .field("snap", &ItemSplitArgs::snap,
                            schema::enumeration<std::string>(
                                {{"none", "none"}, {"zero_crossing", "zero_crossing"}}))
                     .defaultValue(std::string("none"))
                     .build();
        c.result =
            schema::object<SplitResult>("item_split_result")
                .field("left", &SplitResult::left, schema::string())
                .field("right", &SplitResult::right, schema::string())
                .field("linked_left", &SplitResult::linkedLeft, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .field("linked_right", &SplitResult::linkedRight, schema::arrayOf(schema::string()))
                .defaultValue(std::vector<std::string>{})
                .omitIfDefault()
                .build();
        c.errors = {{"item.split_range", "the cursor is outside the item's interior",
                     "choose a beat strictly after the start and before the end"},
                    {"item.split_not_audio", "zero_crossing needs an active audio take",
                     "use snap none to split MIDI or empty items at the cursor"},
                    {"item.split_source_pending", "current source audio is not prepared",
                     "start the engine, wait for source preparation and retry"},
                    {"item.split_source", "the audio source could not be prepared",
                     "relink or repair the audio source and retry"}};
        c.examples = {{"Split halfway", j(R"({"item":"it_1","at":2})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_item"},
                      {"Split at a prior audio zero crossing",
                       j(R"({"item":"it_1","at":2,"snap":"zero_crossing"})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_prepared_audio"}};
        c.resolve = [](ItemSplitArgs& a, Context& ctx) -> Status {
            if (a.snap != "zero_crossing") return {};
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return id.error();
            SUPPLE_TRY(checkUnlocked(s, *id));
            auto at =
                priorZeroCrossing(app(ctx), s, *s.items.find(*id), s.timeMap->secondsAt(a.at));
            if (!at) return at.error();
            a.at = s.timeMap->beatAt(*at);
            a.snap = "none";  // the bus logs the resolved beat for linked edits and replay
            return {};
        };
        c.run = [](const ItemSplitArgs& a, Context& ctx) -> Result<SplitResult> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& old = *s.items.find(*id);
            SUPPLE_TRY(checkUnlocked(s, *id));
            const auto* tr = s.tracks.find(old.track);
            model::Timebase tb = s.project->timebase;
            if (tr) {
                if (tr->timebase == model::TrackTimebase::Time) tb = model::Timebase::Time;
                if (tr->timebase == model::TrackTimebase::Beats) tb = model::Timebase::Beats;
                if (tr->timebase == model::TrackTimebase::BeatsPosition)
                    tb = model::Timebase::BeatsPosition;
            }
            if (old.timebase == model::ItemTimebase::Time) tb = model::Timebase::Time;
            if (old.timebase == model::ItemTimebase::Beats) tb = model::Timebase::Beats;
            if (old.timebase == model::ItemTimebase::BeatsPosition)
                tb = model::Timebase::BeatsPosition;
            const double splitSec = s.timeMap->secondsAt(a.at);
            const double startBeat =
                tb == model::Timebase::Time ? s.timeMap->beatAt(old.position) : old.position;
            const double startSec = s.timeMap->secondsAt(startBeat);
            const double endSec = tb == model::Timebase::Time ? old.position + old.length
                                  : tb == model::Timebase::Beats
                                      ? s.timeMap->secondsAt(old.position + old.length)
                                      : startSec + old.length;
            if (!(splitSec > startSec && splitSec < endSec))
                return Error("item.split_range", "split must be inside the item", "/at",
                             "choose a beat strictly after the start and before the end");
            model::Item left = old, right = old;
            if (tb == model::Timebase::Time) {
                left.length = splitSec - startSec;
                right.position = splitSec;
                right.length = endSec - splitSec;
            } else if (tb == model::Timebase::Beats) {
                left.length = a.at - old.position;
                right.position = a.at;
                right.length = old.position + old.length - a.at;
            } else {
                left.length = splitSec - startSec;
                right.position = a.at;
                right.length = endSec - splitSec;
            }
            left.fadeOut.length = 0;
            right.fadeIn.length = 0;
            right.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldTake : old.takes) {
                auto source = dangling<model::Take>(s, oldTake, "/item");
                if (!source) return std::move(source).error();
                model::Take tk = **source;
                const double delta = tk.source && tk.source->kind == model::Kind::Midi
                                         ? (a.at - startBeat) * tk.playrate
                                         : (splitSec - startSec) * tk.playrate;
                if (tk.source && tk.source->kind == model::Kind::Media)
                    model::rebaseAudioTake(tk, splitSec - startSec);
                else
                    tk.startOffset += delta;
                unpoolCopiedSource(tk, s, txn);
                auto cloned = copyTake(s, txn, oldTake, std::move(tk), a.at - startBeat);
                if (!cloned) return std::move(cloned).error();
                const Id fresh = *cloned;
                right.takes.push_back(fresh);
                if (old.activeTake == oldTake) right.activeTake = fresh;
            }
            txn.put<model::Item>(*id, std::move(left));
            Id fresh = txn.create(std::move(right));
            return SplitResult{id->str(), fresh.str(), {}, {}};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
