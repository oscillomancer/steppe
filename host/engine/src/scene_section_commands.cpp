// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Scene <-> section (ADR-0097, step L4a): scene.insert, scene.capture and
// arrangement.build. They move material between the clip launcher and the
// timeline and touch the model only. Each is one undo step, and a refusal
// changes nothing.
#include <algorithm>
#include <cmath>
#include <set>

#include "clip_launcher.h"
#include "command_helpers.h"
#include "supple/model/freeze.h"
#include "supple/model/launcher.h"

namespace supple::engine {
namespace {

constexpr double kEps = 1e-6;  // beats
constexpr std::size_t kMaxSequence = 256;
constexpr std::int64_t kMaxBars = 100000;

struct LaidClip {
    std::string clip, item, track;
};
struct Segment {
    std::string scene, region;
    double start = 0, end = 0;
    std::vector<LaidClip> items;
};

struct SceneInsertArgs {
    std::string scene;
    std::optional<double> at;
    std::optional<std::int64_t> bar;
    std::int64_t bars = 0;
    std::optional<std::string> name;
    bool pooled = false;
};
struct SceneInsertResult {
    std::vector<LaidClip> items;
    std::string region;
    double start = 0, end = 0;
};
struct SceneCaptureArgs {
    std::string region;
    std::optional<std::string> name;
    bool pooled = false;
};
struct SceneCaptureResult {
    std::string scene;
    std::vector<LaidClip> clips;
};
struct SequenceEntry {
    std::string scene;
    std::int64_t bars = 0;
};
struct BuildArgs {
    std::vector<SequenceEntry> sequence;
    std::optional<double> at;
    std::optional<std::int64_t> bar;
    bool pooled = false;
};
struct BuildResult {
    std::vector<Segment> segments;
};

schema::Schema<LaidClip> laidSchema(const char* second) {
    return schema::object<LaidClip>("laid_clip")
        .field("clip", &LaidClip::clip, schema::string())
        .field(second, &LaidClip::item, schema::string())
        .field("track", &LaidClip::track, schema::string())
        .build();
}

// The bar a start falls on, which must begin it (scene.at otherwise).
Result<int> startBar(const model::Session& s, std::optional<double> at,
                     std::optional<std::int64_t> bar, const std::string& base) {
    if (bar && at)
        return Error("scene.at", "give at or bar, not both", base + "/at",
                     "bar is a 1-based bar number, at a position in beats on a bar line");
    if (!bar && !at)
        return Error("scene.at", "say where the span starts", base + "/bar",
                     "give bar (1-based) or at (beats, on a bar line)");
    if (bar) {
        if (*bar < 1 || *bar > kMaxBars)
            return Error("scene.at", "bar " + std::to_string(*bar) + " is out of range",
                         base + "/bar", "give a bar from 1 to " + std::to_string(kMaxBars));
        return static_cast<int>(*bar);
    }
    if (!(std::isfinite(*at) && *at >= 0))
        return Error("scene.at", "at must be 0 or more", base + "/at",
                     "give a position in beats on a bar line");
    const int onBar = s.timeMap->barBeat(*at).bar;
    if (std::abs(s.timeMap->beatOfBar(onBar) - *at) > kEps)
        return Error("scene.at",
                     "at " + json::formatDouble(*at) + " is not on a bar line (bar " +
                         std::to_string(onBar) + " starts at beat " +
                         json::formatDouble(s.timeMap->beatOfBar(onBar)) + ")",
                     base + "/at",
                     "give bar " + std::to_string(onBar) + " (or at " +
                         json::formatDouble(s.timeMap->beatOfBar(onBar)) +
                         "): a span of whole bars starts on a bar line");
    return onBar;
}

// The clips of `scene`, in the track tree's display order.
std::vector<std::pair<model::Id, const model::Clip*>> clipsOf(const model::Session& s,
                                                              model::Id scene) {
    std::vector<std::pair<model::Id, const model::Clip*>> out;
    for (model::Id track : tracksInDisplayOrder(s))
        if (const auto clip = model::clipAt(s, track, scene))
            out.emplace_back(*clip, s.clips.find(*clip));
    return out;
}

std::string barLabel(const model::Session& s, double beat) {
    return "bar " + std::to_string(s.timeMap->barBeat(beat).bar);
}

// scene.insert for one scene: its clips as items over `bars` bars from
// `firstBar`, and a section over them. `base` is the path of the scene
// argument's owner.
Result<Segment> insertScene(model::EditTxn& t, const model::Session& s, model::Id scene,
                            int firstBar, std::int64_t bars, const std::optional<std::string>& name,
                            bool pooled, const std::string& base) {
    if (bars < 1 || bars > kMaxBars)
        return Error("scene.bars", "bars must be from 1 to " + std::to_string(kMaxBars),
                     base + "/bars", "give the number of whole bars the scene lasts");
    const auto map = s.timeMap;
    const double start = map->beatOfBar(firstBar);
    const double end = map->beatOfBar(static_cast<int>(firstBar + bars));
    SUPPLE_TRY(model::checkedBeat(s, end, base + "/bars"));
    const auto clips = clipsOf(s, scene);
    const model::Scene& sc = *s.scenes.find(scene);
    if (clips.empty())
        return Error("scene.empty", scene.str() + " has no clips to lay out", base + "/scene",
                     "add clips to the scene (clip.add) first");
    // Frozen tracks and items already there refuse, naming them all.
    std::string occupied;
    for (const auto& [clipId, clip] : clips) {
        (void)clipId;
        if (const auto owner = model::frozenOwner(s, clip->track))
            return model::frozenError(s, *owner, base + "/scene");
        for (const auto& e : s.items) {
            if (e.value->track != clip->track) continue;
            const double from = model::itemSpan(s, *e.value).startBeat;
            const double to = model::itemEndBeat(s, *e.value);
            if (from < end - kEps && to > start + kEps)
                occupied += (occupied.empty() ? "" : ", ") + e.id.str() + " on " +
                            clip->track.str() + " (" + barLabel(s, from) + " to " +
                            barLabel(s, to) + ")";
        }
    }
    if (!occupied.empty())
        return Error("scene.insert.occupied",
                     "the span (" + barLabel(s, start) + " to " + barLabel(s, end) +
                         ") already holds " + occupied,
                     base + "/bar",
                     "move or remove those items first (item.move, item.remove), or insert the "
                     "scene at another bar");
    Segment seg;
    seg.scene = scene.str();
    seg.start = start;
    seg.end = end;
    for (const auto& [clipId, clip] : clips) {
        auto item = layClipOnTimeline(t, s, *clip, start, end, pooled, base + "/scene");
        if (!item) return std::move(item).error();
        seg.items.push_back({clipId.str(), item->str(), clip->track.str()});
    }
    model::Region region;
    region.start = start;
    region.end = end;
    region.name = name.value_or(sc.name);
    region.color = sc.color;
    region.section = true;
    seg.region = t.create(std::move(region)).str();
    return seg;
}

}  // namespace

void registerSceneSectionCommands(cmd::Registry& registry) {
    [&] {
        Command<SceneInsertArgs, SceneInsertResult> c;
        c.name = "scene.insert";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Lay a scene's clips on the timeline as items, and mark the span a section.";
        c.description =
            "The span starts at bar (1-based) or at (beats, on a bar line) and lasts bars whole "
            "bars. Each clip becomes an item on its track at the start, as item.add {clip} "
            "makes it: a once clip lasts its length or the span's if shorter; a loop clip lasts "
            "the span, as one looping item when it repeats within it, which needs its period to be "
            "its content: loop off with its length (as clip.add makes it), or loop on over the "
            "whole content with a length of whole repeats (scene.insert.period otherwise). Takes "
            "are copied, MIDI with them unless pooled. An item already in "
            "the span on a track that receives a clip is refused (scene.insert.occupied), and "
            "an empty scene (scene.empty). A section named after the scene (name overrides) "
            "is added over the span. One undo step.";
        c.tags = {"clip launcher", "scene", "arrangement"};
        c.args = schema::object<SceneInsertArgs>("scene_insert_args")
                     .field("scene", &SceneInsertArgs::scene, idArg("sc", "Scene"))
                     .field("at", &SceneInsertArgs::at,
                            schema::number().min(0).doc("Start in beats, on a bar line."))
                     .field("bar", &SceneInsertArgs::bar,
                            schema::integer().range(1, kMaxBars).doc("Start bar, 1-based."))
                     .field("bars", &SceneInsertArgs::bars, schema::integer().min(1))
                     .field("name", &SceneInsertArgs::name, schema::string())
                     .field("pooled", &SceneInsertArgs::pooled, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result =
            schema::object<SceneInsertResult>("scene_insert")
                .field("items", &SceneInsertResult::items, schema::arrayOf(laidSchema("item")))
                .field("region", &SceneInsertResult::region, schema::string())
                .field("start", &SceneInsertResult::start,
                       schema::number().doc("The span's start, in beats."))
                .field("end", &SceneInsertResult::end,
                       schema::number().doc("The span's end, in beats."))
                .build();
        c.errors = {
            {"scene.at", "at is not on a bar line, or neither or both of at and bar",
             "give bar, or an at on a bar line"},
            {"scene.bars", "bars is out of range", "give a whole number of bars from 1"},
            {"scene.empty", "the scene has no clips", "add clips with clip.add"},
            {"scene.insert.occupied", "an item is already in the span on a track that gets a clip",
             "move or remove it, or choose another bar"},
            {"scene.insert.period", "a repeating clip no single looping item reproduces",
             "loop the clip over its whole content, or ask for no more bars than it is long"}};
        c.examples = {{"A verse at bar 5", j(R"({"scene": "sc_1", "bar": 5, "bars": 4})"),
                       j(R"({"items": "<any>", "region": "<any>", "start": 16.0, "end": 32.0})"),
                       "song_with_clip"}};
        c.run = [](const SceneInsertArgs& a, Context& ctx) -> Result<SceneInsertResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto scene = existing<model::Scene>(s, a.scene, "/scene");
            if (!scene) return std::move(scene).error();
            auto bar = startBar(s, a.at, a.bar, "");
            if (!bar) return std::move(bar).error();
            model::EditTxn& t = app.txn();
            auto seg = insertScene(t, s, *scene, *bar, a.bars, a.name, a.pooled, "");
            if (!seg) return std::move(seg).error();
            SceneInsertResult r;
            r.items = std::move(seg->items);
            r.region = seg->region;
            r.start = seg->start;
            r.end = seg->end;
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<SceneCaptureArgs, SceneCaptureResult> c;
        c.name = "scene.capture";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Make a scene from the items inside a section: one clip per track.";
        c.description =
            "For each channel the items overlapping the region must be one item lying wholly "
            "inside it: several are scene.capture.items, one that starts before or ends after "
            "it scene.capture.edge, one that starts after its start scene.capture.offset (a "
            "clip begins at its own start); a region holding no item is scene.capture.empty. "
            "Items on a master are skipped with a warning. Each item becomes a clip as clip.add "
            "{item} makes it (MIDI copied unless pooled; a warped take is clip.unsupported): "
            "loop mode when it fills the region, once when it is shorter. The scene is added "
            "after the last, named after the region unless name is given, with its colour. The "
            "region and the items are unchanged. One undo step.";
        c.tags = {"clip launcher", "scene", "arrangement"};
        c.args = schema::object<SceneCaptureArgs>("scene_capture_args")
                     .field("region", &SceneCaptureArgs::region, idArg("rg", "Region"))
                     .field("name", &SceneCaptureArgs::name, schema::string())
                     .field("pooled", &SceneCaptureArgs::pooled, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<SceneCaptureResult>("scene_capture")
                       .field("scene", &SceneCaptureResult::scene, schema::string())
                       .field("clips", &SceneCaptureResult::clips,
                              schema::arrayOf(laidSchema("source_item")))
                       .build();
        c.errors = {
            {"scene.capture.empty", "the region holds no item", "choose a section with items"},
            {"scene.capture.items", "several items of one track inside the region",
             "consolidate them (item.consolidate) or capture a smaller region"},
            {"scene.capture.edge", "an item starts before or ends after the region",
             "split it at the region's edge (item.split) or widen the region"},
            {"scene.capture.offset", "an item starts after the region's start",
             "move it to the start of the region (item.move)"},
            {"clip.unsupported", "a warped take", "reset its warp first"},
            {"scene.limit", "the project has 1024 scenes", "remove a scene first"},
            {"clip.limit", "the project has 16384 clips", "remove clips first"}};
        c.examples = {{"Capture the verse", j(R"({"region": "rg_1"})"),
                       j(R"({"scene": "<any>", "clips": "<any>"})"), "song_with_section"}};
        c.run = [](const SceneCaptureArgs& a, Context& ctx) -> Result<SceneCaptureResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto rid = existing<model::Region>(s, a.region, "/region");
            if (!rid) return std::move(rid).error();
            const model::Region& region = *s.regions.find(*rid);
            const auto map = s.timeMap;
            const double rs =
                region.timebase == model::Timebase::Time ? map->beatAt(region.start) : region.start;
            const double re =
                region.timebase == model::Timebase::Time ? map->beatAt(region.end) : region.end;
            struct Pick {
                model::Id track, item;
            };
            std::vector<Pick> picks;
            std::vector<std::string> skipped;
            for (model::Id track : tracksInDisplayOrder(s)) {
                const model::Track& tr = *s.tracks.find(track);
                std::vector<model::Id> inside;
                for (const auto& e : s.items) {
                    if (e.value->track != track) continue;
                    const double from = model::itemSpan(s, *e.value).startBeat;
                    const double to = model::itemEndBeat(s, *e.value);
                    if (!(from < re - kEps && to > rs + kEps)) continue;
                    if (tr.type == model::StripType::Master) {
                        skipped.push_back(e.id.str());
                        continue;
                    }
                    if (from < rs - kEps || to > re + kEps)
                        return Error("scene.capture.edge",
                                     e.id.str() + " on " + track.str() + " runs from " +
                                         barLabel(s, from) + " to " + barLabel(s, to) +
                                         ", past the " + (from < rs - kEps ? "start" : "end") +
                                         " of the section (" + barLabel(s, rs) + " to " +
                                         barLabel(s, re) + ")",
                                     "/region",
                                     "split it at the section's edge (item.split), or capture a "
                                     "region that holds it whole");
                    inside.push_back(e.id);
                }
                if (inside.size() > 1) {
                    std::string names;
                    for (model::Id id : inside) names += (names.empty() ? "" : ", ") + id.str();
                    return Error("scene.capture.items",
                                 track.str() + " has " + std::to_string(inside.size()) +
                                     " items inside the section (" + names +
                                     "), and a clip holds one",
                                 "/region",
                                 "consolidate them into one item (item.consolidate), or capture "
                                 "a smaller region");
                }
                if (inside.size() == 1) {
                    const model::Item& only = *s.items.find(inside.front());
                    const double from = model::itemSpan(s, only).startBeat;
                    if (from > rs + kEps)
                        return Error("scene.capture.offset",
                                     inside.front().str() + " on " + track.str() + " starts at " +
                                         barLabel(s, from) + ", after the section's start (" +
                                         barLabel(s, rs) + "), and a clip begins at its own start",
                                     "/region",
                                     "move it to the section's start (item.move) so the clip "
                                     "keeps its timing, or capture a region that starts where "
                                     "it does");
                }
                if (!inside.empty()) picks.push_back({track, inside.front()});
            }
            if (picks.empty())
                return Error("scene.capture.empty",
                             a.region + " (" + barLabel(s, rs) + " to " + barLabel(s, re) +
                                 ") holds no item on a channel",
                             "/region", "choose a section that has items, or add some first");
            if (s.scenes.size() >= model::kMaxScenes)
                return Error("scene.limit",
                             "the project has " + std::to_string(model::kMaxScenes) + " scenes", {},
                             "remove a scene first (scene.remove)");
            if (s.clips.size() + picks.size() > model::kMaxClips)
                return Error("clip.limit",
                             "the project would have more than " +
                                 std::to_string(model::kMaxClips) + " clips",
                             {}, "remove clips first (clip.remove)");
            model::EditTxn& t = app.txn();
            model::Scene scene;
            scene.name = a.name.value_or(region.name);
            scene.color = region.color;
            scene.order = static_cast<int>(s.scenes.size());
            const model::Id sceneId = t.create(std::move(scene));
            SceneCaptureResult r;
            r.scene = sceneId.str();
            bool audio = false;
            for (const Pick& pick : picks) {
                auto made = clipFromItem(t, s, pick.item, a.pooled, "/region");
                if (!made) return std::move(made).error();
                made->track = pick.track;
                made->scene = sceneId;
                const double length = model::itemLengthBeats(s, *s.items.find(pick.item));
                made->mode =
                    length < (re - rs) - kEps ? model::ClipMode::Once : model::ClipMode::Loop;
                audio |= made->kind == model::ClipKind::Audio;
                const Id clip = t.create(std::move(*made));
                r.clips.push_back({clip.str(), pick.item.str(), pick.track.str()});
            }
            if (audio) SUPPLE_TRY(checkClipMemory(app, t.preview(), "/region"));
            if (!skipped.empty()) {
                std::string names;
                for (const auto& n : skipped) names += (names.empty() ? "" : ", ") + n;
                ctx.warn("scene.capture.skipped",
                         "items on a master are not captured (clips live on channels): " + names,
                         "/region");
            }
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<BuildArgs, BuildResult> c;
        c.name = "arrangement.build";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Lay a song out from scenes: each for some bars, one after the other.";
        c.description =
            "scene.insert for each entry of sequence in turn, each starting where the last ended, "
            "from bar or at (default bar 1), with a section for each. A scene may appear more "
            "than once. Every entry's refusal (scene.empty, scene.insert.period, "
            "scene.insert.occupied) names its index in path, and changes nothing; an empty "
            "sequence or one of more than 256 entries is scene.sequence. One undo step.";
        c.tags = {"clip launcher", "scene", "arrangement"};
        auto entry = schema::object<SequenceEntry>("sequence_entry")
                         .field("scene", &SequenceEntry::scene, idArg("sc", "Scene"))
                         .field("bars", &SequenceEntry::bars, schema::integer().min(1))
                         .build();
        c.args = schema::object<BuildArgs>("arrangement_build_args")
                     .field("sequence", &BuildArgs::sequence, schema::arrayOf(entry))
                     .field("at", &BuildArgs::at,
                            schema::number().min(0).doc("Start in beats, on a bar line."))
                     .field("bar", &BuildArgs::bar,
                            schema::integer().range(1, kMaxBars).doc("Start bar, 1-based [1]."))
                     .field("pooled", &BuildArgs::pooled, schema::boolean())
                     .defaultValue(false)
                     .build();
        auto segment = schema::object<Segment>("arrangement_segment")
                           .field("scene", &Segment::scene, schema::string())
                           .field("region", &Segment::region, schema::string())
                           .field("start", &Segment::start, schema::number())
                           .field("end", &Segment::end, schema::number())
                           .field("items", &Segment::items, schema::arrayOf(laidSchema("item")))
                           .build();
        c.result = schema::object<BuildResult>("arrangement_build")
                       .field("segments", &BuildResult::segments, schema::arrayOf(segment))
                       .build();
        c.errors = {
            {"scene.sequence", "the sequence is empty or longer than 256 entries",
             "give 1 to 256 entries of {scene, bars}"},
            {"scene.empty", "a scene in the sequence has no clips", "add clips with clip.add"},
            {"scene.insert.occupied", "items are already in an entry's span",
             "move or remove them, or start at another bar"},
            {"scene.insert.period", "a repeating clip no single looping item reproduces",
             "loop the clip over its whole content, or ask for fewer bars"}};
        c.examples = {{"A verse for four bars from bar 5",
                       j(R"({"sequence": [{"scene": "sc_1", "bars": 4}], "bar": 5})"),
                       j(R"({"segments": "<any>"})"), "song_with_clip"}};
        c.run = [](const BuildArgs& a, Context& ctx) -> Result<BuildResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            if (a.sequence.empty() || a.sequence.size() > kMaxSequence)
                return Error("scene.sequence",
                             "the sequence has " + std::to_string(a.sequence.size()) + " entries",
                             "/sequence", "give 1 to 256 entries of {scene, bars}");
            std::optional<std::int64_t> bar = a.bar;
            if (!a.at && !bar) bar = 1;
            auto first = startBar(s, a.at, bar, "");
            if (!first) return std::move(first).error();
            std::vector<model::Id> scenes;
            for (std::size_t i = 0; i < a.sequence.size(); ++i) {
                const std::string at = "/sequence/" + std::to_string(i);
                auto scene = existing<model::Scene>(s, a.sequence[i].scene, at + "/scene");
                if (!scene) return std::move(scene).error();
                if (a.sequence[i].bars < 1 || a.sequence[i].bars > kMaxBars)
                    return Error("scene.bars", "bars must be from 1 to " + std::to_string(kMaxBars),
                                 at + "/bars", "give the number of whole bars the scene lasts");
                scenes.push_back(*scene);
            }
            model::EditTxn& t = app.txn();
            BuildResult r;
            std::int64_t bars = 0;
            for (std::size_t i = 0; i < a.sequence.size(); ++i) {
                const std::string at = "/sequence/" + std::to_string(i);
                const std::int64_t where = *first + bars;
                if (where > kMaxBars)
                    return Error("scene.bars",
                                 "the sequence runs past bar " + std::to_string(kMaxBars),
                                 at + "/bars", "give fewer bars");
                auto seg = insertScene(t, s, scenes[i], static_cast<int>(where), a.sequence[i].bars,
                                       std::nullopt, a.pooled, at);
                if (!seg) return std::move(seg).error();
                r.segments.push_back(std::move(*seg));
                bars += a.sequence[i].bars;
            }
            return r;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
