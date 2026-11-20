// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Takes as material of their own (session-model §7): delete one, take one out
// of its item, spread an item's takes onto tracks or one after another, and
// render an item through its FX as a new audio take (MIDI through the track's
// instrument). Every command is one undo step.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

#include "clip_launcher.h"
#include "command_helpers.h"
#include "command_validation.h"
#include "file_platform.h"
#include "media_files.h"
#include "recording.h"
#include "supple/audio/audio_file.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"
#include "supple/model/take_owner.h"

namespace supple::engine {
namespace {

Result<json::Value> invoke(const char* name, json::Value args, Context& ctx) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
}

// The item or clip holding `take` (model::takeOwner).
Result<Id> ownerOfTake(const model::Session& s, Id take, const std::string& path) {
    if (const auto owner = model::takeOwner(s, take)) return *owner;
    return Error("take.orphan", take.str() + " belongs to no item or clip", path,
                 "use a take from take.list");
}

// A clip whose audio changed must still fit beside the arrangement.
Status checkOwnerMemory(EngineApp& app, Id owner, const std::string& path) {
    if (owner.kind != model::Kind::Clip) return {};
    return checkClipMemory(app, app.view(), path);
}

// A track that may hold an item with `takes` (ADR-0057: channels, or masters for audio).
Result<Id> itemTrack(const model::Session& s, const std::string& text, const std::string& path,
                     const std::vector<Id>& takes) {
    auto track = existing<model::Track>(s, text, path);
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
    std::string take;
    std::optional<std::string> track;
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
        // Not even a dangling link: writing through it would land elsewhere.
        if (!fs::exists(fs::symlink_status(p, ec))) return p;
    }
}

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

// --- record.commit: a recording's commit, replayable (ADR-0076) -------------------

struct RecSegmentArg {
    std::int64_t fileFrame = 0, timeline = 0;
};
struct RecMidiArg {
    std::int64_t timeline = 0, run = 0;
    std::vector<std::int64_t> bytes;
    std::optional<std::string> route;
    std::int64_t voice = -1;
};
struct RecTakeArg {
    std::string track, file;
    std::string hash;
    std::int64_t startSample = 0, frames = 0;
    std::vector<RecSegmentArg> segments;
    std::int64_t loopStart = 0, loopEnd = 0;
    std::int64_t overflowFrames = 0;
    double sampleRate = 0;
    bool midi = false;
    std::vector<RecMidiArg> events;
};
struct RecPunchArg {
    bool enabled = false;
    double start = 0, end = 0;
};
struct RecordCommitArgs {
    std::vector<RecTakeArg> takes;
    RecPunchArg punch;
};

schema::Schema<RecordCommitArgs> recordCommitSchema() {
    // The bounds of a MIDI journal (ADR-0080): a replayed record.commit is
    // read from a project's history, the same file boundary.
    constexpr double limit = static_cast<double>(detail::kRecordingClockLimit);
    const auto clock = schema::integer().range(-limit, limit);
    const auto length = schema::integer().range(0, limit);
    auto segment = schema::object<RecSegmentArg>("record_commit_segment")
                       .field("file_frame", &RecSegmentArg::fileFrame, length)
                       .field("timeline", &RecSegmentArg::timeline, clock)
                       .build();
    auto event =
        schema::object<RecMidiArg>("record_commit_event")
            .field("timeline", &RecMidiArg::timeline, clock)
            .field("run", &RecMidiArg::run, schema::integer().range(0, UINT32_MAX))
            .field("bytes", &RecMidiArg::bytes, schema::arrayOf(schema::integer().min(0).max(255)))
            .field("route", &RecMidiArg::route, idArg("sd", "Route"))
            .field("voice", &RecMidiArg::voice, schema::integer())
            .build();
    auto take = schema::object<RecTakeArg>("record_commit_take")
                    .field("track", &RecTakeArg::track, idArg("tr", "Track"))
                    .field("file", &RecTakeArg::file,
                           schema::string().doc("Relative to the media folder when inside it"))
                    .field("hash", &RecTakeArg::hash, schema::string())
                    .field("start_sample", &RecTakeArg::startSample, clock)
                    .field("frames", &RecTakeArg::frames, length)
                    .field("segments", &RecTakeArg::segments, schema::arrayOf(segment))
                    .field("loop_start", &RecTakeArg::loopStart, length)
                    .field("loop_end", &RecTakeArg::loopEnd, length)
                    .field("overflow_frames", &RecTakeArg::overflowFrames, length)
                    .field("sample_rate", &RecTakeArg::sampleRate, schema::number().exclusiveMin(0))
                    .field("midi", &RecTakeArg::midi, schema::boolean())
                    .field("events", &RecTakeArg::events, schema::arrayOf(event))
                    .build();
    return schema::object<RecordCommitArgs>("record_commit_args")
        .field("takes", &RecordCommitArgs::takes, schema::arrayOf(take))
        .field("punch", &RecordCommitArgs::punch,
               schema::object<RecPunchArg>("record_commit_punch")
                   .field("enabled", &RecPunchArg::enabled, schema::boolean())
                   .field("start", &RecPunchArg::start, schema::number())
                   .field("end", &RecPunchArg::end, schema::number())
                   .build())
        .build();
}

// take.extract from a clip: the take moves onto the timeline as an item at
// `position` (required), shaped by the conversion rule, its own envelopes
// converted in place; the clip goes with its last take.
Result<ItemResult> extractFromClip(EngineApp& app, const model::Session& s,
                                   const TakeExtractArgs& a, Id take, Id clipId) {
    if (!a.position)
        return Error("take.position", "a clip's take needs a position on the timeline", "/position",
                     "give position in beats, e.g. {\"position\": 16}");
    const model::Clip& clip = *s.clips.find(clipId);
    auto track = itemTrack(s, a.track.value_or(clip.track.str()), "/track", {take});
    if (!track) return std::move(track).error();
    SUPPLE_TRY(model::checkedBeat(s, *a.position, "/position"));
    auto shaped = itemFromClip(s, clip, *s.takes.find(take), *track, *a.position, "/take");
    if (!shaped) return std::move(shaped).error();
    model::EditTxn& t = app.txn();
    if (shaped->envelopePos) convertTakeEnvelopes(t, s, take, shaped->envelopePos);
    convertTakeFxEnvelopes(t, s, take, shaped->fxEnvelope);  // onto the timeline (C2)
    shaped->item.takes = {take};
    shaped->item.activeTake = take;
    if (clip.takes.size() == 1) {
        t.remove<model::Clip>(clipId);
    } else {
        model::Item scratch;  // dropTake's rule for the heard take
        scratch.takes = clip.takes;
        scratch.activeTake = clip.activeTake;
        dropTake(scratch, take);
        t.modify<model::Clip>(clipId, [&](model::Clip& c) {
            c.takes = scratch.takes;
            c.activeTake = scratch.activeTake;
        });
    }
    return ItemResult{t.create(std::move(shaped->item)).str()};
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
    [&] {
        Command<TakeAddArgs, IdResult> c;
        c.name = "take.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Add a take to an item or clip, optionally copied from one of its takes.";
        c.tags = {"take"};
        c.args = schema::object<TakeAddArgs>("take_add_args")
                     .field("item", &TakeAddArgs::item, ownerIdArg())
                     .field("from", &TakeAddArgs::from, idArg("tk", "Take"))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"Copy a take", j(R"({"item":"it_1","from":"tk_1"})"), j(R"({"id":"tk_2"})"),
                       "song_with_item"}};
        c.run = [](const TakeAddArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            auto item = existingOwner(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            model::Take copy;
            if (a.from) {
                auto source = existing<model::Take>(s, *a.from, "/from");
                if (!source) return std::move(source).error();
                const auto& takes = *model::ownerTakes(s, *item);
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
            modifyOwnerTakes(txn, *item, [&](std::vector<Id>& takes, std::optional<Id>& active) {
                takes.push_back(fresh);
                if (!active) active = fresh;
            });
            SUPPLE_TRY(checkOwnerMemory(app(ctx), *item, "/from"));
            return IdResult{fresh.str()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeActivateArgs, Empty> c;
        c.name = "take.activate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Choose the take heard from an item or clip.";
        c.description =
            "take (its id) or index (0-based in the item's take order — the same position "
            "after item.split, which copies the takes in order: comping splits an item and "
            "picks a take by index on the piece).";
        c.tags = {"take"};
        c.args = schema::object<TakeActivateArgs>("take_activate_args")
                     .field("item", &TakeActivateArgs::item, ownerIdArg())
                     .field("take", &TakeActivateArgs::take, idArg("tk", "Take"))
                     .field("index", &TakeActivateArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Activate", j(R"({"item":"it_1","take":"tk_1"})"), j("{}"), "song_with_item"}};
        c.run = [](const TakeActivateArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto item = existingOwner(s, a.item, "/item");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item));
            const auto& takes = *model::ownerTakes(s, *item);
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
                auto found = existing<model::Take>(s, *a.take, "/take");
                if (!found) return std::move(found).error();
                take = *found;
                if (std::find(takes.begin(), takes.end(), *take) == takes.end())
                    return Error("take.not_in_item", *a.take + " does not belong to " + a.item,
                                 "/take", "use take.list for this item");
            }
            modifyOwnerTakes(app(ctx).txn(), *item,
                             [&](std::vector<Id>&, std::optional<Id>& active) { active = *take; });
            SUPPLE_TRY(checkOwnerMemory(app(ctx), *item, a.take ? "/take" : "/index"));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<TakeSetArgs, Empty> c;
        c.name = "take.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Set take gain, pan, channel mode, name, source offset, or playrate and pitch.";
        c.tags = {"take"};
        c.args =
            schema::object<TakeSetArgs>("take_set_args")
                .field("take", &TakeSetArgs::take, idArg("tk", "Take"))
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
            auto id = existing<model::Take>(app(ctx).view(), a.take, "/take");
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
            if (const auto owner = model::takeOwner(app(ctx).view(), *id))
                SUPPLE_TRY(checkOwnerMemory(app(ctx), *owner, "/take"));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ItemQuery, TakeList> c;
        c.name = "take.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List the takes of an item or clip and identify its active take.";
        c.tags = {"take"};
        c.args = schema::object<ItemQuery>("take_list_args")
                     .field("item", &ItemQuery::item, ownerIdArg())
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
            auto id = existingOwner(s, a.item, "/item");
            if (!id) return std::move(id).error();
            TakeList out;
            const auto heard = ownerActiveTake(s, *id);
            for (Id takeId : *model::ownerTakes(s, *id)) {
                auto found = dangling<model::Take>(s, takeId, "/item");
                if (!found) return std::move(found).error();
                const auto& tk = **found;
                out.takes.push_back({takeId.str(), tk.name, tk.source ? tk.source->str() : "",
                                     heard == takeId, tk.gainDb, tk.pan, tk.startOffset,
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
    {
        Command<TakeRef, TakeRemoveResult> c;
        c.name = "take.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Delete a take from its item or clip (which goes with its last take).";
        c.description =
            "The take's FX, envelopes and unshared MIDI go with it. When it was the heard take, "
            "the next take is heard (else the previous one). item: the item or clip that held "
            "it; item_removed: it was the only take, so that item or clip was removed too.";
        c.tags = {"take"};
        c.args = schema::object<TakeRef>("take_remove_args")
                     .field("take", &TakeRef::take, idArg("tk", "Take"))
                     .build();
        c.result = schema::object<TakeRemoveResult>("take_remove_result")
                       .field("item", &TakeRemoveResult::item, schema::string())
                       .field("item_removed", &TakeRemoveResult::itemRemoved, schema::boolean())
                       .build();
        c.examples = {{"Delete the second take", j(R"({"take":"tk_2"})"),
                       j(R"({"item":"it_1","item_removed":false})"), "song_with_takes"}};
        c.run = [](const TakeRef& a, Context& ctx) -> Result<TakeRemoveResult> {
            const model::Session s = app(ctx).view();
            auto take = existing<model::Take>(s, a.take, "/take");
            if (!take) return std::move(take).error();
            auto item = ownerOfTake(s, *take, "/take");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item, "/take"));
            if (model::ownerTakes(s, *item)->size() == 1) {
                if (item->kind == model::Kind::Clip) {
                    SUPPLE_TRY(removeClipObjects(app(ctx).txn(), s, *item));
                    return TakeRemoveResult{item->str(), true};
                }
                json::Value args = json::Value::object();
                args["item"] = item->str();
                SUPPLE_TRY(invoke("item.remove", std::move(args), ctx));
                return TakeRemoveResult{item->str(), true};
            }
            model::EditTxn& t = app(ctx).txn();
            SUPPLE_TRY(removeTakeObjects(t, s, *take));
            modifyOwnerTakes(t, *item, [&](std::vector<Id>& takes, std::optional<Id>& active) {
                model::Item scratch;  // dropTake's rule for the heard take
                scratch.takes = takes;
                scratch.activeTake = active;
                dropTake(scratch, *take);
                takes = scratch.takes;
                active = scratch.activeTake;
            });
            return TakeRemoveResult{item->str(), false};
        };
        addChecked(registry, std::move(c));
    }
    {
        Command<TakeExtractArgs, ItemResult> c;
        c.name = "take.extract";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Take a take out of its item or clip into an item of its own on any channel.";
        c.description =
            "The new item copies the old one's length, fades, gain and timebase and starts at "
            "position (beats; default: where the item starts). The take keeps its FX and "
            "envelopes. The only take of an item moves the item itself. From a clip, position "
            "is required (a clip has no place in time), track defaults to the clip's, the item "
            "is as long as the clip, and its loop and take envelopes convert at the tempo where "
            "it lands (ADR-0097); the clip goes with its last take.";
        c.tags = {"take", "item"};
        c.args = schema::object<TakeExtractArgs>("take_extract_args")
                     .field("take", &TakeExtractArgs::take, idArg("tk", "Take"))
                     .field("track", &TakeExtractArgs::track,
                            idArg("tr", "Track").doc("Default: the item's or clip's track."))
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
            auto take = existing<model::Take>(s, a.take, "/take");
            if (!take) return std::move(take).error();
            auto item = ownerOfTake(s, *take, "/take");
            if (!item) return std::move(item).error();
            SUPPLE_TRY(checkUnlocked(s, *item, "/take"));
            if (item->kind == model::Kind::Clip)
                return extractFromClip(app(ctx), s, a, *take, *item);
            auto track =
                itemTrack(s, a.track.value_or(s.items.find(*item)->track.str()), "/track", {*take});
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
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Spread an item's takes onto new tracks, or one after another on its track.";
        c.description =
            "In recording order. mode tracks: the first take stays on the item's track and each "
            "other take gets a new channel right below it (named after the take) at the same "
            "time. mode serial: the takes follow each other on the item's own track, each as "
            "long as the item. Every take becomes a one-take item with its FX and envelopes.";
        c.tags = {"take", "item"};
        c.args = schema::object<ExplodeArgs>("item_explode_takes_args")
                     .field("item", &ExplodeArgs::item, timelineItemArg())
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
            SUPPLE_TRY(refuseClip(a.item, "/item",
                                  "a clip's takes stay in the clip; take.extract "
                                  "lays one on the timeline"));
            const model::Session s = app(ctx).view();
            auto item = existing<model::Item>(s, a.item, "/item");
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
        c.exposure = cmd::Exposure::McpExec;
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
                     .field("item", &ApplyFxArgs::item, timelineItemArg())
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
            SUPPLE_TRY(refuseClip(a.item, "/item",
                                  "lay the clip on the timeline first "
                                  "(item.add {clip})"));
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            auto item = existing<model::Item>(s, a.item, "/item");
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
            // Written under a temporary name, then published only under a
            // free one: another writer may take the fresh name meanwhile.
            static std::atomic<unsigned> serial{0};
            const std::string stem =
                (heard->name.empty() ? std::string("item") : heard->name) + " fx";
            fs::path temp = dir / ("." + stem + "." + std::to_string(detail::files::processId()) +
                                   "." + std::to_string(serial.fetch_add(1)) + ".tmp");
            if (auto written = audio::writeWav(temp, rendered->audio); !written) {
                fs::remove(temp, ec);
                return std::move(written).error();
            }
            fs::path file;
            for (int attempt = 0;; ++attempt) {
                file = freshFile(dir, stem);
                if (detail::files::publishNew(temp, file) == 0) break;
                if (errno != EEXIST || attempt >= 100) {
                    const std::string reason = std::strerror(errno);
                    fs::remove(temp, ec);
                    return Error("item.apply_fx_write",
                                 "cannot publish the render as " + file.string() + ": " + reason,
                                 "/item", "check media-folder permissions and free space");
                }
            }
            fs::remove(temp, ec);  // the published name keeps the file
            engine.noteCreatedFile(file);
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
    {
        Command<RecordCommitArgs, json::Value> c;
        c.name = "record.commit";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::Internal;
        c.summary = "Record a recording's commit (or a recovery's) as a replayable edit.";
        c.description =
            "The takes as the device delivered them and the punch range then; placing them "
            "again builds the same items, takes and sources. Audio files must still be there.";
        c.args = recordCommitSchema();
        c.result = schema::any();
        c.run = [](const RecordCommitArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& engine = app(ctx);
            const model::Session s = engine.view();
            std::vector<RecordedTake> takes;
            for (std::size_t i = 0; i < a.takes.size(); ++i) {
                const RecTakeArg& t = a.takes[i];
                const std::string at = "/takes/" + std::to_string(i);
                auto track = existing<model::Track>(s, t.track, at + "/track");
                if (!track) return std::move(track).error();
                RecordedTake r;
                r.track = *track;
                if (!t.midi) {
                    const fs::path file(t.file);
                    r.file = file.is_relative() ? engine.mediaDir() / file : file;
                    std::error_code ec;
                    if (!fs::is_regular_file(r.file, ec))
                        return Error("record.missing", "the recorded file " + t.file + " is gone",
                                     at + "/file", "restore the file, then merge again");
                }
                r.hash = t.hash;
                r.startSample = t.startSample;
                r.frames = t.frames;
                for (const auto& g : t.segments) r.segments.push_back({g.fileFrame, g.timeline});
                // As a journal's header: a loop ends after it starts, or is
                // [0, 0] (none).
                if (t.loopEnd <= t.loopStart && (t.loopStart != 0 || t.loopEnd != 0))
                    return Error("record.loop", "the recorded loop ends before it starts",
                                 at + "/loop_end",
                                 "give loop_end after loop_start, or both 0 for no loop");
                // The punch, in this take's frames, within the same bound
                // as its clocks: placement converts and clips with it.
                const double limit = static_cast<double>(detail::kRecordingClockLimit);
                for (auto [seconds, name] :
                     {std::pair{a.punch.start, "start"}, std::pair{a.punch.end, "end"}})
                    if (!(std::fabs(seconds * t.sampleRate) <= limit))
                        return Error(
                            "record.punch",
                            std::string("the punch ") + name + " is beyond any recording's length",
                            std::string("/punch/") + name,
                            "give a punch within the recording's range, in seconds");
                r.loop.start = t.loopStart;
                r.loop.end = t.loopEnd;
                r.overflowFrames = static_cast<std::uint64_t>(t.overflowFrames);
                r.sampleRate = t.sampleRate;
                r.isMidi = t.midi;
                for (std::size_t k = 0; k < t.events.size(); ++k) {
                    const RecMidiArg& e = t.events[k];
                    const std::string event = at + "/events/" + std::to_string(k);
                    // A channel message as captured (ADR-0080): a status
                    // 0x80..0xEF, then two 7-bit data bytes.
                    if (e.bytes.size() != 3 || e.bytes[0] < 0x80 || e.bytes[0] > 0xef ||
                        e.bytes[1] > 0x7f || e.bytes[2] > 0x7f)
                        return Error(
                            "record.midi", "a recorded MIDI event is not a channel message",
                            event + "/bytes", "give [status 128..239, data 0..127, data 0..127]");
                    if (e.voice < INT32_MIN || e.voice > INT32_MAX)
                        return Error("record.midi", "a recorded MIDI voice is out of range",
                                     event + "/voice", "give a voice within 32 bits");
                    RecordedMidiEvent m;
                    m.timeline = e.timeline;
                    m.run = static_cast<std::uint32_t>(e.run);
                    for (std::size_t b = 0; b < 3; ++b)
                        m.bytes[b] = static_cast<std::uint8_t>(e.bytes[b]);
                    if (e.route) {
                        auto route = existing<model::Route>(s, *e.route, event + "/route");
                        if (!route) return std::move(route).error();
                        m.route = *route;
                    }
                    m.voice = static_cast<int>(e.voice);
                    r.midi.push_back(m);
                }
                takes.push_back(std::move(r));
            }
            if (a.punch.enabled && a.punch.end <= a.punch.start)
                return Error("record.punch", "the punch ends before it starts", "/punch/end",
                             "give punch.end after punch.start, or disable the punch");
            TransportRange punch;
            punch.enabled = a.punch.enabled;
            punch.start = a.punch.start;
            punch.end = a.punch.end;
            auto placed = engine.placeTakes(takes, punch, nullptr);
            if (!placed) return std::move(placed).error();
            return json::Value::object();
        };
        addChecked(registry, std::move(c));
    }
}

namespace detail {
json::Value recordCommitArgs(const std::vector<RecordedTake>& takes, const TransportRange& punch,
                             const fs::path& media) {
    RecordCommitArgs a;
    for (const RecordedTake& r : takes) {
        RecTakeArg t;
        t.track = r.track.str();
        std::error_code ec;
        const fs::path rel = media.empty() ? fs::path() : fs::relative(r.file, media, ec);
        // A MIDI take's file is its crash-recovery journal (ADR-0080): not
        // part of what a merge replays.
        if (!r.isMidi)
            t.file = !ec && !rel.empty() && *rel.begin() != ".." ? rel.generic_string()
                                                                 : r.file.string();
        t.hash = r.hash;
        t.startSample = r.startSample;
        t.frames = r.frames;
        for (const auto& g : r.segments) t.segments.push_back({g.fileFrame, g.timeline});
        t.loopStart = r.loop.start;
        t.loopEnd = r.loop.end;
        t.overflowFrames = static_cast<std::int64_t>(r.overflowFrames);
        t.sampleRate = r.sampleRate;
        t.midi = r.isMidi;
        for (const auto& m : r.midi) {
            RecMidiArg e;
            e.timeline = m.timeline;
            e.run = m.run;
            e.bytes = {m.bytes[0], m.bytes[1], m.bytes[2]};
            if (m.route != model::Id{}) e.route = m.route.str();
            e.voice = m.voice;
            t.events.push_back(std::move(e));
        }
        a.takes.push_back(std::move(t));
    }
    a.punch = {punch.enabled, punch.start, punch.end};
    return recordCommitSchema().encode(a);
}
}  // namespace detail

}  // namespace supple::engine
