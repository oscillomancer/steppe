// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Scene <-> section (ADR-0097, step L4a): scene.insert, scene.capture and
// arrangement.build. They move material between the clip launcher and the
// timeline and touch the model only. Each is one undo step, and a refusal
// changes nothing.
#include <algorithm>
#include <cmath>
#include <set>

#include "clip_context.h"
#include "clip_follow.h"
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
    std::optional<detail::FollowedInfo> followed;  // scene.insert, arrangement.build
};
// What write_context did for a section (session-model "Writing the context from scenes").
struct ContextWritten {
    bool written = false;
    std::int64_t keys = 0, chords = 0;
    std::optional<std::string> reason;  // present, disagree, holds, no_context
    std::vector<std::string> clips;     // disagree: the two clips
    std::optional<std::int64_t> bar;    // disagree: the project bar
    std::optional<std::string> message;
};
struct Segment {
    std::string scene, region;
    double start = 0, end = 0;
    std::vector<LaidClip> items;
    std::optional<ContextWritten> context;
};

struct SceneInsertArgs {
    std::string scene;
    std::optional<double> at;
    std::optional<std::int64_t> bar;
    std::int64_t bars = 0;
    std::optional<std::string> name;
    bool pooled = false;
    std::optional<model::ClipFollow> follow;
    bool writeContext = false;
};
struct SceneInsertResult {
    std::vector<LaidClip> items;
    std::string region;
    double start = 0, end = 0;
    std::optional<ContextWritten> context;
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
    std::optional<model::ClipFollow> follow;
    bool writeContext = false;
};
struct BuildResult {
    std::vector<Segment> segments;
};

schema::Schema<LaidClip> laidSchema(const char* second) {
    auto laid = schema::object<LaidClip>(std::string(second) == "item" ? "laid_item" : "laid_clip")
                    .field("clip", &LaidClip::clip, schema::string())
                    .field(second, &LaidClip::item, schema::string())
                    .field("track", &LaidClip::track, schema::string());
    if (std::string(second) == "item")
        laid.field("followed", &LaidClip::followed, detail::followedSchema());
    return laid.build();
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

schema::Schema<ContextWritten> contextWrittenSchema() {
    return schema::object<ContextWritten>("context_written")
        .field("written", &ContextWritten::written, schema::boolean())
        .field("keys", &ContextWritten::keys,
               schema::integer().doc("Key points written under the section."))
        .field("chords", &ContextWritten::chords,
               schema::integer().doc("Chord points written under the section."))
        .field("reason", &ContextWritten::reason,
               schema::enumeration<std::string>({{"present", "present"},
                                                 {"disagree", "disagree"},
                                                 {"holds", "holds"},
                                                 {"no_context", "no_context"}})
                   .doc("Why nothing was written: the context track already has points under "
                        "the section, the clips disagree, it already says the same, or no clip "
                        "has a context."))
        .field("clips", &ContextWritten::clips, schema::arrayOf(schema::string()))
        .field("bar", &ContextWritten::bar, schema::integer())
        .field("message", &ContextWritten::message, schema::string())
        .build();
}

constexpr const char* kWriteContextDoc =
    "Write the scene's clips' own key and chords onto the context track under each section that "
    "has none (shifted to the section, repeating with a loop clip), in the same undo step, "
    "before the clips are laid. Left alone: a section with points under it; not written: one "
    "whose clips disagree. The answer's context says which (default false).";

constexpr double kGrid = 960.0;
double onGrid(double beat) { return std::round(beat * kGrid) / kGrid; }

// The same key or chord: points are canonical, so the strings say it.
bool samePoint(const model::ContextPoint& a, const model::ContextPoint& b) {
    return a.type == b.type &&
           (a.type == model::ContextType::Key ? a.tonic == b.tonic && a.scale == b.scale
                                              : a.symbol == b.symbol);
}

// What write_context decided for one section: the points to write (none unless
// note.written) and the answer.
struct ContextPlan {
    ContextWritten note;
    std::vector<model::ContextPoint> points;
};

// Plans the context written under [start, end) for a scene's `clips`. `written` holds the
// points this call has already planned for earlier sections.
Result<ContextPlan> planSectionContext(
    const model::Session& s, const std::vector<std::pair<model::Id, const model::Clip*>>& clips,
    double start, double end, const std::vector<model::ContextPoint>& written) {
    ContextPlan plan;
    ContextWritten& note = plan.note;
    // Points already under the section are left alone.
    std::size_t present = 0;
    for (const auto& e : s.context)
        if (e.value->beat > start - kEps && e.value->beat < end - kEps) ++present;
    if (present) {
        note.reason = "present";
        note.message = "the context track already has " + std::to_string(present) +
                       (present == 1 ? " point" : " points") + " under this section: left alone";
        return plan;
    }
    // The clips' points on the timeline, with the clip that says each.
    struct Event {
        std::int64_t tick;
        model::ContextPoint point;
        model::Id clip;
    };
    std::vector<Event> events;
    for (const auto& [clipId, clip] : clips) {
        if (clip->context.empty()) continue;
        const bool midi = clip->kind == model::ClipKind::Midi && clip->length > 0;
        for (int pass = 0; pass < 100000; ++pass) {
            const double origin = start + pass * (midi ? clip->length : 0.0);
            if (origin >= end - kEps) break;
            for (const model::ContextPoint& p : clip->context) {
                const double at = onGrid(origin + p.beat);
                if (at >= end - kEps) continue;
                model::ContextPoint q = p;
                q.beat = at;
                events.push_back({std::llround(at * kGrid), std::move(q), clipId});
            }
            // An audio clip holds a key at its start only, and a once clip plays one pass.
            if (!midi || clip->mode == model::ClipMode::Once) break;
        }
    }
    if (events.empty()) {
        note.reason = "no_context";
        note.message = "no clip of the scene has a key or chords";
        return plan;
    }
    // An empty chord ("no chord from here") says nothing, so a real chord of another clip at its
    // beat wins and the two do not disagree: they sort after the real ones.
    const auto saysNothing = [](const model::ContextPoint& p) {
        return p.type == model::ContextType::Chord && p.symbol.empty();
    };
    std::stable_sort(events.begin(), events.end(), [&](const Event& a, const Event& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.point.type != b.point.type) return a.point.type < b.point.type;
        return !saysNothing(a.point) && saysNothing(b.point);
    });
    // Two clips saying different things of one type at one beat disagree.
    for (std::size_t i = 0; i < events.size(); ++i)
        for (std::size_t k = i + 1; k < events.size() && events[k].tick == events[i].tick; ++k)
            if (events[k].point.type == events[i].point.type && !saysNothing(events[i].point) &&
                !saysNothing(events[k].point) && !samePoint(events[i].point, events[k].point) &&
                events[k].clip != events[i].clip) {
                note.reason = "disagree";
                note.clips = {events[i].clip.str(), events[k].clip.str()};
                note.bar = s.timeMap->barBeat(events[i].point.beat).bar;
                note.message =
                    events[i].clip.str() + " and " + events[k].clip.str() +
                    " were written over different " +
                    (events[i].point.type == model::ContextType::Key ? "keys" : "chords") +
                    " at bar " + std::to_string(*note.bar) + ": nothing written for this section";
                return plan;
            }
    // What holds now: the context track at the start, then the points planned so far.
    const auto heldIn = [&](model::ContextType type) -> std::optional<model::ContextPoint> {
        std::optional<model::ContextPoint> held;
        const auto id = type == model::ContextType::Key ? model::keyPointAt(s, start)
                                                        : model::chordPointAt(s, start);
        if (id)
            if (const auto* p = s.context.find(*id)) held = *p;
        for (const auto& w : written)
            if (w.type == type && w.beat <= start + kEps && (!held || w.beat >= held->beat))
                held = w;
        return held;
    };
    std::optional<model::ContextPoint> heldKey = heldIn(model::ContextType::Key),
                                       heldChord = heldIn(model::ContextType::Chord);
    for (const Event& e : events) {
        auto& held = e.point.type == model::ContextType::Key ? heldKey : heldChord;
        const bool noChord = e.point.type == model::ContextType::Chord && e.point.symbol.empty() &&
                             (!held || held->symbol.empty());
        if (noChord || (held && samePoint(*held, e.point))) continue;
        if (!plan.points.empty() && plan.points.back().beat == e.point.beat &&
            plan.points.back().type == e.point.type)
            continue;  // the same point from another clip
        plan.points.push_back(e.point);
        held = e.point;
        (e.point.type == model::ContextType::Key ? note.keys : note.chords) += 1;
    }
    std::size_t keys = note.keys, chords = note.chords;
    for (const auto& e : s.context) (e.value->type == model::ContextType::Key ? keys : chords) += 1;
    for (const auto& w : written) (w.type == model::ContextType::Key ? keys : chords) += 1;
    if (keys > model::kMaxContextPoints || chords > model::kMaxContextPoints)
        return Error("context.limit",
                     "a project holds at most " + std::to_string(model::kMaxContextPoints) +
                         " key points and as many chord points",
                     "/write_context", "write_context less, or remove points you no longer need");
    if (plan.points.empty()) {
        note.reason = "holds";
        note.message = "the context track already says what the clips say";
        return plan;
    }
    note.written = true;
    return plan;
}

// The warning for a section that was asked for and not written for a reason the caller
// should know of.
void warnNotWritten(Context& ctx, const ContextWritten& note, const std::string& scene,
                    const std::string& path) {
    if (note.written || !note.reason || *note.reason == "no_context" || *note.reason == "holds")
        return;
    ctx.warn("context.not_written", scene + ": " + note.message.value_or(""), path);
}

std::string barLabel(const model::Session& s, double beat) {
    return "bar " + std::to_string(s.timeMap->barBeat(beat).bar);
}

// scene.insert for one scene: its clips as items over `bars` bars from
// `firstBar`, and a section over them. `base` is the path of the scene
// argument's owner.
Result<Segment> insertScene(Context& ctx, model::EditTxn& t, const model::Session& s,
                            model::Id scene, int firstBar, std::int64_t bars,
                            const std::optional<std::string>& name, bool pooled,
                            std::optional<model::ClipFollow> follow, const std::string& base) {
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
        // The laid takes carry the clip's context and follow (ADR-0098): the notes sound as the
        // context under the item says when asked to; otherwise as written, and context.mismatch
        // says when the clip's key is not the project's.
        std::vector<detail::ClipNote> notes;
        auto followed =
            detail::followPlacedItem(s, t, clipId, *clip, *item, follow, base + "/scene", notes);
        if (!followed) return std::move(followed).error();
        for (const auto& n : notes) ctx.warn(n.code, n.message, n.path);
        seg.items.push_back({clipId.str(), item->str(), clip->track.str(), std::move(*followed)});
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
        c.summary = "Lay a scene's clips on the timeline as items and a section.";
        c.description =
            "The span starts at bar (1-based) or at (beats, on a bar line) and lasts bars whole "
            "bars. Each clip becomes an item on its track at the start, as item.add {clip} "
            "makes it: a once clip lasts its length or the span's if shorter; a loop clip lasts "
            "the span, as one looping item when it repeats within it, which needs its period to be "
            "its content: loop off with its length (as clip.add makes it), or loop on over the "
            "whole content with a length of whole repeats (scene.insert.period otherwise). Takes "
            "are copied, MIDI with them unless pooled; an item takes its clip's name (its active "
            "take's) when the clip has one. follow (none, key or chords; default: each clip's own) "
            "is set on each laid MIDI take with the clip's context, so its notes sound as the key "
            "or chords under the item say, mapped from those they were written against "
            "(ADR-0098, \"following is playback\"): the notes stay as written, a context edit "
            "moves "
            "them, and take.bake writes the sounding pitches. Each item's followed says how they "
            "sound, by bar, and copied why a clip that cannot follow was left as it is; pooled "
            "may follow too. With none the notes sound as written, and context.mismatch warns "
            "once for each item whose clip's key "
            "(or, in the same key, chord) is not the project's under it. An item already in "
            "the span on a track that receives a clip is refused (scene.insert.occupied), and "
            "an empty scene (scene.empty). A section named after the scene (name overrides) "
            "is added over the span. write_context writes the clips' own key and chords onto the "
            "context track under the span first (context in the answer says what happened). One "
            "undo step.";
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
                     .field("follow", &SceneInsertArgs::follow, detail::followArgSchema())
                     .field("write_context", &SceneInsertArgs::writeContext,
                            schema::boolean().doc(kWriteContextDoc))
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
                .field("context", &SceneInsertResult::context, contextWrittenSchema())
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
            std::optional<ContextWritten> note;
            std::optional<model::Session> after;  // the session with the context written
            if (a.writeContext && a.bars >= 1 && a.bars <= kMaxBars) {
                const double start = s.timeMap->beatOfBar(*bar);
                const double end = s.timeMap->beatOfBar(static_cast<int>(*bar + a.bars));
                auto plan = planSectionContext(s, clipsOf(s, *scene), start, end, {});
                if (!plan) return std::move(plan).error();
                for (auto& p : plan->points) t.create(std::move(p));
                warnNotWritten(ctx, plan->note, a.scene, "/scene");
                note = std::move(plan->note);
                if (note->written) after = app.view();
            }
            auto seg = insertScene(ctx, t, after ? *after : s, *scene, *bar, a.bars, a.name,
                                   a.pooled, a.follow, "");
            if (!seg) return std::move(seg).error();
            SceneInsertResult r;
            r.items = std::move(seg->items);
            r.region = seg->region;
            r.start = seg->start;
            r.end = seg->end;
            r.context = std::move(note);
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<SceneCaptureArgs, SceneCaptureResult> c;
        c.name = "scene.capture";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Make a scene from a section's items, one clip per track.";
        c.description =
            "For each channel the items overlapping the region must be one item lying wholly "
            "inside it: several are scene.capture.items, one that starts before or ends after "
            "it scene.capture.edge, one that starts after its start scene.capture.offset (a "
            "clip begins at its own start); a region holding no item is scene.capture.empty. "
            "Items on a master are skipped with a warning. Each item becomes a clip as clip.add "
            "{item} makes it (MIDI copied unless pooled; a warped take is clip.unsupported): "
            "loop mode when it fills the region, once when it is shorter, named as the item is "
            "(its active take's name). The scene is added "
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
                // The clip remembers the key and chords the item sat under (ADR-0098); an
                // audio clip takes the key only.
                // An item whose take follows keeps that take's context and follow (clipFromItem
                // took them); any other takes the project's under it.
                if (made->follow == model::ClipFollow::None) {
                    auto taken = detail::contextUnder(
                        s, model::itemSpan(s, *s.items.find(pick.item)).startBeat, length,
                        made->kind == model::ClipKind::Audio);
                    for (const auto& n : detail::unreadableNotes(taken, "/region"))
                        ctx.warn(n.code, n.message, n.path);
                    made->context = std::move(taken.points);
                }
                model::fitClipContext(*made);
                const Id clip = t.create(std::move(*made));
                r.clips.push_back({clip.str(), pick.item.str(), pick.track.str(), std::nullopt});
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
        c.summary = "Lay a song out from scenes, each for some bars, in order.";
        c.description =
            "scene.insert for each entry of sequence in turn, each starting where the last ended, "
            "from bar or at (default bar 1), with a section for each. A scene may appear more "
            "than once. Every entry's refusal (scene.empty, scene.insert.period, "
            "scene.insert.occupied) names its index in path, and changes nothing; an empty "
            "sequence or one of more than 256 entries is scene.sequence. follow, pooled and each "
            "item's followed and context.mismatch are as scene.insert's. write_context writes each "
            "section's harmony from its scene's clips where the context track has none: a section "
            "with points under it is left alone, one whose clips disagree gets nothing, and each "
            "segment's context says which. One undo step.";
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
                     .field("follow", &BuildArgs::follow, detail::followArgSchema())
                     .field("write_context", &BuildArgs::writeContext,
                            schema::boolean().doc(kWriteContextDoc))
                     .defaultValue(false)
                     .build();
        auto segment = schema::object<Segment>("arrangement_segment")
                           .field("scene", &Segment::scene, schema::string())
                           .field("region", &Segment::region, schema::string())
                           .field("start", &Segment::start, schema::number())
                           .field("end", &Segment::end, schema::number())
                           .field("items", &Segment::items, schema::arrayOf(laidSchema("item")))
                           .field("context", &Segment::context, contextWrittenSchema())
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
            // write_context: each section's harmony first, so the clips that follow find it.
            std::vector<std::optional<ContextWritten>> notes(a.sequence.size());
            std::optional<model::Session> after;
            if (a.writeContext) {
                std::vector<model::ContextPoint> written;
                std::int64_t at = *first;
                bool any = false;
                for (std::size_t i = 0; i < a.sequence.size(); ++i) {
                    if (at + a.sequence[i].bars > kMaxBars) break;  // the laying refuses it below
                    const double start = s.timeMap->beatOfBar(static_cast<int>(at));
                    const double end =
                        s.timeMap->beatOfBar(static_cast<int>(at + a.sequence[i].bars));
                    auto plan = planSectionContext(s, clipsOf(s, scenes[i]), start, end, written);
                    if (!plan) {
                        Error e = std::move(plan).error();
                        e.path = "/sequence/" + std::to_string(i) + e.path;
                        return e;
                    }
                    for (auto& p : plan->points) {
                        written.push_back(p);
                        t.create(std::move(p));
                        any = true;
                    }
                    warnNotWritten(ctx, plan->note, a.sequence[i].scene,
                                   "/sequence/" + std::to_string(i) + "/scene");
                    notes[i] = std::move(plan->note);
                    at += a.sequence[i].bars;
                }
                if (any) after = app.view();
            }
            const model::Session& placed = after ? *after : s;
            BuildResult r;
            std::int64_t bars = 0;
            for (std::size_t i = 0; i < a.sequence.size(); ++i) {
                const std::string at = "/sequence/" + std::to_string(i);
                const std::int64_t where = *first + bars;
                if (where > kMaxBars)
                    return Error("scene.bars",
                                 "the sequence runs past bar " + std::to_string(kMaxBars),
                                 at + "/bars", "give fewer bars");
                auto seg = insertScene(ctx, t, placed, scenes[i], static_cast<int>(where),
                                       a.sequence[i].bars, std::nullopt, a.pooled, a.follow, at);
                if (!seg) return std::move(seg).error();
                seg->context = std::move(notes[i]);
                r.segments.push_back(std::move(*seg));
                bars += a.sequence[i].bars;
            }
            return r;
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace supple::engine
