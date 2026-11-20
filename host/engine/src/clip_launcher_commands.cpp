// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's model commands (ADR-0097, step L1): scene.* and
// clip.*. Launching (launch.*) is the engine's, step L2.
#include "clip_launcher.h"

#include "command_helpers.h"
#include "supple/model/launcher.h"
#include "supple/model/take_owner.h"

namespace supple::engine {

// --- shared ------------------------------------------------------------------------

Status removeClipObjects(model::EditTxn& t, const model::Session& s, Id clip) {
    const model::Clip* c = s.clips.find(clip);
    if (!c) return {};
    for (Id take : c->takes) SUPPLE_TRY(removeTakeObjects(t, s, take));
    t.remove<model::Clip>(clip);
    return {};
}

Result<std::vector<Id>> removeClipsWhere(model::EditTxn& t, const model::Session& s,
                                         const std::function<bool(const model::Clip&)>& which) {
    std::vector<Id> removed;
    for (const auto& e : s.clips)
        if (which(*e.value)) {
            SUPPLE_TRY(removeClipObjects(t, s, e.id));
            removed.push_back(e.id);
        }
    return removed;
}

void warnClipsRemoved(cmd::Context& ctx, const std::vector<Id>& clips, const std::string& path) {
    if (clips.empty()) return;
    std::string list;
    for (Id id : clips) list += (list.empty() ? "" : ", ") + id.str();
    ctx.warn("clip.removed",
             std::string(clips.size() == 1 ? "removed clip " : "removed clips ") + list +
                 " with their takes",
             path);
}

std::vector<Id> tracksInDisplayOrder(const model::Session& s) {
    std::vector<Id> out;
    std::function<void(Id)> walk = [&](Id id) {
        const model::Track* t = s.tracks.find(id);
        if (!t) return;
        out.push_back(id);
        for (Id child : t->children) walk(child);
    };
    for (Id master : s.project->masters) walk(master);
    return out;
}

double firstBarBeats(const model::Session& s) {
    const auto meter = s.timeMap->meterAt(1);
    return meter.numerator * 4.0 / meter.denominator;
}

double contentLength(const model::Session& s, const model::Take& take) {
    const double rate = take.playrate > 0 ? take.playrate : 1.0;
    if (!take.source) return 0;
    if (take.source->kind == model::Kind::Midi) {
        const model::MidiSource* m = s.midi.find(*take.source);
        if (!m || m->sequence.ppq <= 0) return 0;
        return static_cast<double>(m->sequence.length) / m->sequence.ppq / rate;
    }
    const model::MediaSource* md = s.media.find(*take.source);
    if (!md || md->sampleRate <= 0) return 0;
    return static_cast<double>(md->frames) / md->sampleRate / rate;
}

Status checkClipTakeUnwarped(const model::Session& s, Id take, const std::string& path) {
    const model::Take* tk = s.takes.find(take);
    if (tk && (!tk->stretchMarkers.empty() || !tk->warpPeers.empty() || model::activeWarp(*tk)))
        return Error("clip.unsupported", take.str() + " is warped, and clips do not warp", path,
                     "reset its warp first (audio.warp.reset), or keep it on the timeline");
    return {};
}

Result<std::vector<Id>> copyTakesFor(model::EditTxn& t, const model::Session& s,
                                     const std::vector<Id>& takes, bool pooled,
                                     const std::function<double(double)>& envelopePos,
                                     const std::string& path) {
    std::vector<Id> out;
    for (Id old : takes) {
        const model::Take* tk = s.takes.find(old);
        if (!tk)
            return Error("item.take", old.str() + " is missing", path,
                         "use a current take id from take.list");
        if (envelopePos) {
            // Only the take's own envelopes are measured from its owner's
            // start; take FX parameters follow the timeline.
            for (const auto& e : s.envelopes)
                if (e.value->target.object == old)
                    for (const auto& ai : s.autoItems)
                        if (ai.value->envelope == e.id)
                            return Error("clip.unsupported",
                                         e.id.str() + " on " + old.str() +
                                             " has automation items, which do not convert "
                                             "between beats and seconds",
                                         path,
                                         "remove its automation items (autoitem.remove) first");
        }
        model::Take copy = *tk;
        if (pooled && copy.source && copy.source->kind == model::Kind::Midi) {
            if (const model::MidiSource* m = s.midi.find(*copy.source); m && !m->pooled)
                t.modify<model::MidiSource>(*copy.source,
                                            [](model::MidiSource& src) { src.pooled = true; });
        } else {
            unpoolCopiedSource(copy, s, t);
        }
        auto fresh = copyTake(s, t, old, std::move(copy));
        if (!fresh) return std::move(fresh).error();
        if (envelopePos) convertTakeEnvelopes(t, t.preview(), *fresh, envelopePos);
        out.push_back(*fresh);
    }
    return out;
}

schema::Schema<std::string> ownerIdArg(const char* what) {
    return schema::string()
        .pattern("^(it|cl)_[0-9]+$", "item or clip id")
        .doc(std::string(what) + " id, e.g. \"it_1\" or \"cl_1\".");
}

schema::Schema<std::string> timelineItemArg() {
    return schema::string()
        .pattern("^(it|cl)_[0-9]+$", "item id")
        .doc("Item id, e.g. \"it_1\" (a clip's id is refused with item.kind).");
}

Result<Id> existingOwner(const model::Session& s, const std::string& text,
                         const std::string& path) {
    const auto id = Id::parse(text);
    if (id && id->kind == model::Kind::Clip) {
        auto clip = existing<model::Clip>(s, text, path);
        if (!clip) return std::move(clip).error();
        return *clip;
    }
    auto item = existing<model::Item>(s, text, path);
    if (!item) {
        Error e = std::move(item).error();
        e.code = "it.unknown";
        return e;
    }
    return *item;
}

Status refuseClip(const std::string& text, const std::string& path, const std::string& instead) {
    const auto id = Id::parse(text);
    if (id && id->kind == model::Kind::Clip)
        return Error("item.kind", text + " is a clip, and this command places items in time", path,
                     instead);
    return {};
}

Status refuseClipIds(const std::vector<Id>& ids, const std::string& path,
                     const std::string& instead) {
    for (std::size_t k = 0; k < ids.size(); ++k)
        SUPPLE_TRY(refuseClip(ids[k].str(), path + "/" + std::to_string(k), instead));
    return {};
}

std::optional<Id> ownerActiveTake(const model::Session& s, Id owner) {
    if (const model::Item* it = s.items.find(owner)) return it->activeTake;
    if (const model::Clip* c = s.clips.find(owner)) return c->activeTake;
    return std::nullopt;
}

void modifyOwnerTakes(model::EditTxn& t, Id owner,
                      const std::function<void(std::vector<Id>&, std::optional<Id>&)>& f) {
    if (owner.kind == model::Kind::Clip)
        t.modify<model::Clip>(owner, [&](model::Clip& c) { f(c.takes, c.activeTake); });
    else
        t.modify<model::Item>(owner, [&](model::Item& it) { f(it.takes, it.activeTake); });
}

double ownerStartBeat(const model::Session& s, Id owner) {
    if (const model::Item* it = s.items.find(owner)) return model::itemSpan(s, *it).startBeat;
    return 0;
}

std::optional<model::ClipKind> clipKindOfTake(const model::Session& s, Id take) {
    const auto owner = model::takeOwner(s, take);
    if (!owner || owner->kind != model::Kind::Clip) return std::nullopt;
    return s.clips.find(*owner)->kind;
}

Result<ClipItem> itemFromClip(const model::Session& s, const model::Clip& clip,
                              const model::Take& take, Id track, double pos,
                              const std::string& path) {
    ClipItem out;
    model::Item& item = out.item;
    item.track = track;
    item.gainDb = clip.gainDb;
    item.mute = clip.mute;
    item.color = clip.color;
    if (clip.loop.on) {
        const double content = contentLength(s, take);
        const double tolerance = 1e-9 * std::max(1.0, content);
        if (std::abs(clip.loop.start) > tolerance ||
            std::abs(clip.loop.length - content) > tolerance)
            return Error("clip.unsupported",
                         "the clip's loop window has no item equivalent: an item loops its whole "
                         "content",
                         path,
                         "set the clip's loop off, or to start 0 and length " +
                             json::formatDouble(content) + " (clip.set), first");
        item.loop = true;
    }
    const auto map = s.timeMap;
    const double startSec = map->secondsAt(pos);
    if (clip.kind == model::ClipKind::Audio) {
        SUPPLE_TRY(model::checkedBeat(s, map->beatAt(startSec + clip.length), path));
        // The seconds land exactly; the envelope points convert at the tempo
        // where the item lands.
        model::placeItemSeconds(s, item, startSec, startSec + clip.length);
        out.envelopePos = [map, startSec, pos](double secs) {
            return map->beatAt(startSec + secs) - pos;
        };
    } else {
        SUPPLE_TRY(model::checkedBeat(s, pos + clip.length, path));
        model::placeItem(s, item, pos, clip.length);  // in the track's timebase
    }
    return out;
}

void convertTakeEnvelopes(model::EditTxn& t, const model::Session& s, Id take,
                          const std::function<double(double)>& pos) {
    for (const auto& e : s.envelopes)
        if (e.value->target.object == take)
            t.modify<model::Envelope>(e.id, [&](model::Envelope& env) {
                for (auto& p : env.points) p.pos = pos(p.pos);
            });
}

Status checkClipMemory(EngineApp& app, const model::Session& after, const std::string& path) {
    const double rate =
        app.engine() ? app.engine()->config().sampleRate : after.project->sampleRate;
    const ResidentSamples r = residentSamples(after, app.projectDir(), rate);
    if (r.arrangement + r.clips <= kPreparedSampleBudget) return {};
    auto n = [](double x) { return json::formatDouble(std::ceil(x)); };
    return Error("clip.memory",
                 "the clips' audio would not all fit beside the arrangement's in the " +
                     n(kPreparedSampleBudget) + "-sample budget: the arrangement holds " +
                     n(r.arrangement) + " and the clips would hold " + n(r.clips),
                 path,
                 "use shorter audio or fewer distinct files in clips, or lay clips on the "
                 "timeline (item.add {clip}) and remove them");
}

namespace {

// --- helpers --------------------------------------------------------------------------

std::vector<Id> scenesInOrder(const model::Session& s) {
    std::vector<Id> out;
    for (const auto& e : s.scenes) out.push_back(e.id);
    std::stable_sort(out.begin(), out.end(),
                     [&](Id a, Id b) { return s.scenes.find(a)->order < s.scenes.find(b)->order; });
    return out;
}

// Gives the scenes in `ordered` the orders 0 .. n-1, touching only those
// whose order changes.
void renumber(model::EditTxn& t, const model::Session& s, const std::vector<Id>& ordered) {
    for (std::size_t k = 0; k < ordered.size(); ++k)
        if (s.scenes.find(ordered[k])->order != static_cast<int>(k))
            t.modify<model::Scene>(ordered[k],
                                   [k](model::Scene& sc) { sc.order = static_cast<int>(k); });
}

Result<Id> sceneArg(const model::Session& s, const std::string& text, const std::string& path,
                    const char* code = "sc.unknown") {
    auto id = existing<model::Scene>(s, text, path);
    if (id) return *id;
    Error e = std::move(id).error();
    e.code = code;
    if (s.scenes.empty()) e.hint = "there are no scenes; add one with scene.add";
    return e;
}

Result<Id> clipArg(const model::Session& s, const std::string& text, const std::string& path) {
    auto id = existing<model::Clip>(s, text, path);
    if (id) return *id;
    Error e = std::move(id).error();
    if (s.clips.empty()) e.hint = "there are no clips; add one with clip.add";
    return e;
}

Status checkClipTrack(const model::Session& s, Id track, const std::string& path) {
    const model::Track* tr = s.tracks.find(track);
    if (tr && tr->type != model::StripType::Channel)
        return Error("clip.track",
                     track.str() + " is a " + std::string(model::stripTypeName(tr->type)) +
                         "; clips live on channels",
                     path, "choose a channel; busses and masters hold no clips");
    return {};
}

Status checkCellFree(const model::Session& s, Id track, Id scene, const std::string& path,
                     std::optional<Id> except = std::nullopt) {
    if (const auto there = model::clipAt(s, track, scene); there && there != except)
        return Error("clip.occupied",
                     there->str() + " is already in " + track.str() + "'s " + scene.str(), path,
                     "choose an empty cell, or remove " + there->str() + " first (clip.remove)");
    return {};
}

Status checkClipLength(double length, const std::string& path) {
    if (!(std::isfinite(length) && length > 0))
        return Error("clip.length", "a clip's length must be above 0", path,
                     "give a positive length in the clip's unit (beats for MIDI, seconds for "
                     "audio)");
    return {};
}

double roundUpToBars(const model::Session& s, double beats) {
    const double bar = firstBarBeats(s);
    const double bars = std::max(1.0, std::ceil(beats / bar - 1e-9));
    return bars * bar;
}

// A number as describe writes it: whole numbers without a decimal point.
std::string number(double x) {
    if (std::isfinite(x) && x == std::round(x) && std::abs(x) < 1e15)
        return std::to_string(static_cast<long long>(x));
    return json::formatDouble(x);
}

std::string clipLabel(const model::Session& s, Id clip) {
    const model::Clip* c = s.clips.find(clip);
    return c && !c->name.empty() ? clip.str() + " \"" + c->name + "\"" : clip.str();
}

// --- arguments ------------------------------------------------------------------------

struct SceneAddArgs {
    std::optional<std::string> name, color;
    std::optional<std::int64_t> at;
};
struct SceneSetArgs {
    std::string scene;
    std::optional<std::string> name, color;
};
struct SceneRef {
    std::string scene;
};
struct SceneMoveArgs {
    std::string scene;
    std::int64_t to = 0;
};
struct SceneEntry {
    std::string id, name;
    std::optional<std::string> color;
    std::int64_t order = 0, clips = 0;
};
struct SceneList {
    std::vector<SceneEntry> scenes;
};

struct ClipAddArgs {
    std::string track, scene;
    std::optional<std::string> notation, audioFile, midiFile, item, name;
    bool pooled = false;
};
struct ClipAddResult {
    std::string clip;
    std::vector<std::string> takes;
    std::optional<std::string> source;
    std::string kind;
    double length = 0;
    std::optional<std::vector<std::string>> warnings;
};
struct LoopArg {
    bool on = false;
    std::optional<double> start, length;
};
struct ClipSetArgs {
    std::string clip;
    std::optional<std::string> name, color;
    std::optional<bool> mute;
    std::optional<double> gainDb, length;
    std::optional<LoopArg> loop;
    std::optional<model::ClipMode> mode;
    std::optional<json::Value> quantize;  // null: the project's
};
struct ClipRef {
    std::string clip;
};
struct ClipMoveArgs {
    std::string clip;
    std::optional<std::string> track, scene;
};
struct ClipDuplicateArgs {
    std::string clip;
    std::optional<std::string> track, scene;
    bool pooled = false;
};

schema::Schema<model::ClipMode> clipModeSchema() {
    return schema::enumeration<model::ClipMode>(
        {{model::ClipMode::Loop, "loop"}, {model::ClipMode::Once, "once"}});
}

auto sceneIdArg() { return idArg("sc", "Scene"); }
auto clipIdArg() { return idArg("cl", "Clip"); }

void registerSceneCommands(cmd::Registry& registry) {
    [&] {
        Command<SceneAddArgs, IdResult> c;
        c.name = "scene.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Add a scene, a column of the clip launcher, at the end or at an index.";
        c.description =
            "at is the column (0-based, default: after the last); the scenes from there on "
            "move one column right. A project holds at most 1024 scenes.";
        c.tags = {"clip launcher", "scene"};
        c.args = schema::object<SceneAddArgs>("scene_add_args")
                     .field("name", &SceneAddArgs::name, schema::string())
                     .field("color", &SceneAddArgs::color, schema::string())
                     .field("at", &SceneAddArgs::at,
                            schema::integer().range(0, model::kMaxScenes).doc("Column, 0-based."))
                     .build();
        c.result = idResultSchema();
        c.errors = {{"scene.limit", "the project has 1024 scenes", "remove a scene first"},
                    {"scene.index", "at is past the last column", "use 0 to the scene count"}};
        c.examples = {{"A verse", j(R"({"name": "Verse"})"), j(R"({"id": "sc_1"})"), "song"}};
        c.run = [](const SceneAddArgs& a, Context& ctx) -> Result<IdResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            if (s.scenes.size() >= model::kMaxScenes)
                return Error("scene.limit",
                             "the project has " + std::to_string(model::kMaxScenes) + " scenes", {},
                             "remove a scene first (scene.remove)");
            auto order = scenesInOrder(s);
            const auto at = static_cast<std::size_t>(a.at.value_or(std::int64_t(order.size())));
            if (at > order.size())
                return Error("scene.index", "at " + std::to_string(at) + " is past the last column",
                             "/at", "use 0 to " + std::to_string(order.size()));
            model::EditTxn& t = app.txn();
            model::Scene scene;
            scene.name = a.name.value_or("");
            scene.color = a.color.value_or("");
            scene.order = static_cast<int>(at);
            // Make room first, so no two scenes ever share an order.
            for (std::size_t k = at; k < order.size(); ++k)
                t.modify<model::Scene>(
                    order[k], [k](model::Scene& sc) { sc.order = static_cast<int>(k) + 1; });
            return IdResult{t.create(std::move(scene)).str()};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<SceneSetArgs, Empty> c;
        c.name = "scene.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Rename or recolour a scene.";
        c.tags = {"clip launcher", "scene"};
        c.args = schema::object<SceneSetArgs>("scene_set_args")
                     .field("scene", &SceneSetArgs::scene, sceneIdArg())
                     .field("name", &SceneSetArgs::name, schema::string())
                     .field("color", &SceneSetArgs::color, schema::string())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Rename", j(R"({"scene": "sc_1", "name": "Verse 1"})"), j("{}"), "song_with_clip"}};
        c.run = [](const SceneSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = sceneArg(s, a.scene, "/scene");
            if (!id) return std::move(id).error();
            app.txn().modify<model::Scene>(*id, [&](model::Scene& sc) {
                if (a.name) sc.name = *a.name;
                if (a.color) sc.color = *a.color;
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<SceneRef, Empty> c;
        c.name = "scene.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove a scene with its clips and their takes.";
        c.description =
            "The scenes after it move one column left. A clip.removed warning names the clips "
            "removed with it.";
        c.tags = {"clip launcher", "scene"};
        c.args = schema::object<SceneRef>("scene_ref")
                     .field("scene", &SceneRef::scene, sceneIdArg())
                     .build();
        c.result = emptySchema();
        c.examples = {{"An empty scene", j(R"({"scene": "sc_2"})"), j("{}"), "song_with_clip"}};
        c.run = [](const SceneRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = sceneArg(s, a.scene, "/scene");
            if (!id) return std::move(id).error();
            model::EditTxn& t = app.txn();
            auto removed =
                removeClipsWhere(t, s, [&](const model::Clip& clip) { return clip.scene == *id; });
            if (!removed) return std::move(removed).error();
            t.remove<model::Scene>(*id);
            auto order = scenesInOrder(s);
            std::erase(order, *id);
            renumber(t, s, order);
            warnClipsRemoved(ctx, *removed, "/scene");
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<SceneMoveArgs, Empty> c;
        c.name = "scene.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Move a scene to another column.";
        c.description = "to is the column it ends up in (0-based); the others close up around it.";
        c.tags = {"clip launcher", "scene"};
        c.args = schema::object<SceneMoveArgs>("scene_move_args")
                     .field("scene", &SceneMoveArgs::scene, sceneIdArg())
                     .field("to", &SceneMoveArgs::to,
                            schema::integer().range(0, model::kMaxScenes - 1).doc("Column."))
                     .build();
        c.result = emptySchema();
        c.errors = {{"scene.index", "to is past the last column", "use 0 to the count minus 1"}};
        c.examples = {
            {"Chorus first", j(R"({"scene": "sc_2", "to": 0})"), j("{}"), "song_with_clip"}};
        c.run = [](const SceneMoveArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = sceneArg(s, a.scene, "/scene");
            if (!id) return std::move(id).error();
            auto order = scenesInOrder(s);
            if (a.to < 0 || static_cast<std::size_t>(a.to) >= order.size())
                return Error("scene.index",
                             "to " + std::to_string(a.to) + " is past the last column", "/to",
                             "use 0 to " + std::to_string(order.size() - 1));
            std::erase(order, *id);
            order.insert(order.begin() + a.to, *id);
            renumber(app.txn(), s, order);
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<NoArgs, SceneList> c;
        c.name = "scene.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "List the scenes in column order with their clip counts.";
        c.tags = {"clip launcher", "scene"};
        c.args = schema::object<NoArgs>("no_args").build();
        auto entry = schema::object<SceneEntry>("scene_entry")
                         .field("id", &SceneEntry::id, schema::string())
                         .field("name", &SceneEntry::name, schema::string())
                         .field("color", &SceneEntry::color, schema::string())
                         .field("order", &SceneEntry::order, schema::integer())
                         .field("clips", &SceneEntry::clips, schema::integer())
                         .build();
        c.result = schema::object<SceneList>("scene_list")
                       .field("scenes", &SceneList::scenes, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Two scenes", j("{}"),
                       j(R"({"scenes": [{"id": "sc_1", "name": "Verse", "order": 0, "clips": 1},
                                         {"id": "sc_2", "name": "Chorus", "order": 1, "clips": 0}]})"),
                       "song_with_clip"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<SceneList> {
            const model::Session s = engine::app(ctx).view();
            SceneList out;
            for (Id id : scenesInOrder(s)) {
                const model::Scene& sc = *s.scenes.find(id);
                SceneEntry e{id.str(), sc.name, std::nullopt, sc.order, 0};
                if (!sc.color.empty()) e.color = sc.color;
                for (const auto& cl : s.clips)
                    if (cl.value->scene == id) ++e.clips;
                out.scenes.push_back(std::move(e));
            }
            return out;
        };
        addChecked(registry, std::move(c));
    }();
}

void registerClipCommands(cmd::Registry& registry) {
    [&] {
        Command<ClipAddArgs, ClipAddResult> c;
        c.name = "clip.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Add a clip to a channel's cell in a scene, from notation, a file or an item.";
        c.description =
            "One source at most: notation (as item.add's), midi_file, audio_file, or item (a "
            "copy of that item's takes; pooled shares its MIDI instead). Without one, an empty "
            "MIDI clip of one bar. A MIDI clip is measured in beats, as long as its notes "
            "rounded up to whole bars; an audio clip in seconds of take time, as long as the "
            "file. From an item, the length, loop and take envelopes convert at the tempo where "
            "the item sits (ADR-0097). Clips live on channels, one per (track, scene).";
        c.tags = {"clip launcher", "clip"};
        c.args = schema::object<ClipAddArgs>("clip_add_args")
                     .field("track", &ClipAddArgs::track, idArg("tr", "Track"))
                     .field("scene", &ClipAddArgs::scene, sceneIdArg())
                     .field("notation", &ClipAddArgs::notation, schema::string())
                     .field("audio_file", &ClipAddArgs::audioFile, schema::string())
                     .field("midi_file", &ClipAddArgs::midiFile, schema::string())
                     .field("item", &ClipAddArgs::item, idArg("it", "Item"))
                     .field("pooled", &ClipAddArgs::pooled, schema::boolean())
                     .defaultValue(false)
                     .field("name", &ClipAddArgs::name, schema::string())
                     .build();
        c.result =
            schema::object<ClipAddResult>("clip_add")
                .field("clip", &ClipAddResult::clip, schema::string())
                .field("takes", &ClipAddResult::takes, schema::arrayOf(schema::string()))
                .field("source", &ClipAddResult::source, schema::string())
                .field("kind", &ClipAddResult::kind, schema::string())
                .field("length", &ClipAddResult::length, schema::number())
                .field("warnings", &ClipAddResult::warnings, schema::arrayOf(schema::string()))
                .build();
        c.errors = {{"clip.track", "the track is not a channel", "put clips on a channel"},
                    {"clip.scene", "no such scene", "see scene.list, or add one with scene.add"},
                    {"clip.occupied", "the cell holds a clip", "choose an empty cell"},
                    {"clip.source", "more than one source",
                     "give one of notation, audio_file, "
                     "midi_file or item"},
                    {"clip.mixed", "the item holds MIDI and audio takes", "copy one kind"},
                    {"clip.unsupported", "a warped take", "reset its warp first"},
                    {"clip.memory", "the clips' audio would not fit", "use shorter audio"},
                    {"clip.limit", "the project has 16384 clips", "remove clips first"}};
        c.examples = {
            {"A chorus bass line",
             j(R"({"track": "tr_1", "scene": "sc_2", "notation": "voice bass; 1: C2:h G2:h"})"),
             j(R"({"clip": "cl_2", "takes": ["tk_3"], "source": "mi_3", "kind": "midi", "length": 4.0})"),
             "song_with_clip"}};
        c.run = [](const ClipAddArgs& a, Context& ctx) -> Result<ClipAddResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto track = existing<model::Track>(s, a.track, "/track");
            if (!track) return std::move(track).error();
            SUPPLE_TRY(checkClipTrack(s, *track, "/track"));
            auto scene = sceneArg(s, a.scene, "/scene", "clip.scene");
            if (!scene) return std::move(scene).error();
            SUPPLE_TRY(checkCellFree(s, *track, *scene, "/scene"));
            if (s.clips.size() >= model::kMaxClips)
                return Error("clip.limit",
                             "the project has " + std::to_string(model::kMaxClips) + " clips", {},
                             "remove clips first (clip.remove)");
            const int sources = (a.notation ? 1 : 0) + (a.audioFile ? 1 : 0) +
                                (a.midiFile ? 1 : 0) + (a.item ? 1 : 0);
            if (sources > 1)
                return Error("clip.source", "give one source at most", {},
                             "give one of notation, audio_file, midi_file or item");
            if (a.pooled && !a.item)
                return Error("clip.source", "pooled applies to a copy of an item", "/pooled",
                             "give item with pooled, or leave pooled out");
            model::EditTxn& t = app.txn();
            model::Clip clip;
            clip.track = *track;
            clip.scene = *scene;
            clip.name = a.name.value_or("");
            ClipAddResult r;
            auto single = [&](model::Take take) {
                const Id tk = t.create(std::move(take));
                clip.takes = {tk};
                clip.activeTake = tk;
            };
            if (a.item) {
                auto item = existing<model::Item>(s, *a.item, "/item");
                if (!item) return std::move(item).error();
                const model::Item& it = *s.items.find(*item);
                std::optional<model::ClipKind> kind;
                for (Id tk : it.takes) {
                    SUPPLE_TRY(checkClipTakeUnwarped(s, tk, "/item"));
                    const model::Take* take = s.takes.find(tk);
                    const auto k = take ? model::clipKindOf(*take) : std::nullopt;
                    if (k && kind && *k != *kind)
                        return Error("clip.mixed", *a.item + " holds MIDI and audio takes", "/item",
                                     "a clip holds one kind: remove the other takes from the "
                                     "item, or copy them into clips of their own");
                    if (k) kind = k;
                }
                clip.kind = kind.value_or(model::ClipKind::Midi);
                const auto span = model::itemSpan(s, it);
                std::function<double(double)> envelopePos;
                if (clip.kind == model::ClipKind::Midi) {
                    clip.length = model::itemLengthBeats(s, it);
                } else {
                    clip.length = span.endSec - span.startSec;
                    const auto map = s.timeMap;
                    envelopePos = [map, span](double beats) {
                        return map->secondsAt(span.startBeat + beats) - span.startSec;
                    };
                }
                auto copied = copyTakesFor(t, s, it.takes, a.pooled, envelopePos, "/item");
                if (!copied) return std::move(copied).error();
                clip.takes = *copied;
                for (std::size_t k = 0; k < it.takes.size(); ++k)
                    if (it.activeTake == it.takes[k]) clip.activeTake = clip.takes[k];
                if (!clip.activeTake && !clip.takes.empty()) clip.activeTake = clip.takes.front();
                if (it.loop && it.activeTake)
                    if (const model::Take* active = s.takes.find(*it.activeTake))
                        if (const double content = contentLength(s, *active); content > 0)
                            clip.loop = {true, 0.0, content};
                clip.mute = it.mute;
                clip.gainDb = it.gainDb;
                clip.color = it.color;
            } else if (a.audioFile) {
                auto imported = importAudioFile(app, t, s, *a.audioFile);
                if (!imported) return std::move(imported).error().at("/audio_file");
                clip.kind = model::ClipKind::Audio;
                clip.length = imported->seconds;
                SUPPLE_TRY(checkClipLength(clip.length, "/audio_file"));
                model::Take take;
                take.source = imported->source;
                single(std::move(take));
            } else {
                model::MidiSource source;
                double beats = 0;
                if (a.notation) {
                    auto compiled = compileNotation(s, *a.notation, 0);
                    if (!compiled) return std::move(compiled).error();
                    source = std::move(compiled->first);
                    beats = compiled->second;
                } else if (a.midiFile) {
                    auto file = readMidiFile(app, s, *a.midiFile, 0);
                    if (!file) return std::move(file).error().at("/midi_file");
                    source = std::move(file->source);
                    beats = file->beats;
                    if (!file->warnings.empty()) r.warnings = std::move(file->warnings);
                }
                clip.kind = model::ClipKind::Midi;
                clip.length = roundUpToBars(s, beats);
                if (!a.notation && !a.midiFile) {  // one empty bar
                    source.sequence.ppq = s.project->ppq;
                    source.sequence.length =
                        static_cast<std::int64_t>(std::llround(clip.length * s.project->ppq));
                }
                model::Take take;
                take.source = t.create(std::move(source));
                single(std::move(take));
            }
            const Id id = t.create(clip);
            if (clip.kind == model::ClipKind::Audio)
                SUPPLE_TRY(checkClipMemory(app, t.preview(), a.item ? "/item" : "/audio_file"));
            r.clip = id.str();
            for (Id tk : clip.takes) r.takes.push_back(tk.str());
            if (clip.activeTake)
                if (const model::Take* take = t.preview().takes.find(*clip.activeTake);
                    take && take->source)
                    r.source = take->source->str();
            r.kind = std::string(model::clipKindName(clip.kind));
            r.length = clip.length;
            return r;
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ClipSetArgs, Empty> c;
        c.name = "clip.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Set a clip's name, colour, mute, gain, length, loop, mode or quantize.";
        c.description =
            "length and loop are in the clip's unit: beats for MIDI, seconds of take time for "
            "audio. loop: {on, start?, length?}, the window that repeats inside the clip from "
            "the take's start. mode loop repeats every length; once plays it once. quantize "
            "(bar, beat, 1/2 … 1/32, none) overrides the project's launch_quantize; null "
            "returns to it.";
        c.tags = {"clip launcher", "clip"};
        auto loop = schema::object<LoopArg>("clip_loop_arg")
                        .field("on", &LoopArg::on, schema::boolean())
                        .field("start", &LoopArg::start, schema::number().min(0))
                        .field("length", &LoopArg::length, schema::number())
                        .build();
        c.args = schema::object<ClipSetArgs>("clip_set_args")
                     .field("clip", &ClipSetArgs::clip, clipIdArg())
                     .field("name", &ClipSetArgs::name, schema::string())
                     .field("color", &ClipSetArgs::color, schema::string())
                     .field("mute", &ClipSetArgs::mute, schema::boolean())
                     .field("gain_db", &ClipSetArgs::gainDb,
                            schema::number().range(model::kSilenceDb, 24))
                     .field("length", &ClipSetArgs::length, schema::number())
                     .field("loop", &ClipSetArgs::loop, loop)
                     .field("mode", &ClipSetArgs::mode, clipModeSchema())
                     .field("quantize", &ClipSetArgs::quantize,
                            clearable("clearable_launch_quantize", model::launchQuantizeSchema()))
                     .build();
        c.result = emptySchema();
        c.errors = {{"clip.length", "a length or loop length not above 0",
                     "give a positive length in the clip's unit"}};
        c.examples = {{"Play once, on the beat",
                       j(R"({"clip": "cl_1", "mode": "once", "quantize": "beat"})"), j("{}"),
                       "song_with_clip"}};
        c.run = [](const ClipSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = clipArg(s, a.clip, "/clip");
            if (!id) return std::move(id).error();
            model::Clip clip = *s.clips.find(*id);
            if (a.length) {
                SUPPLE_TRY(checkClipLength(*a.length, "/length"));
                clip.length = *a.length;
            }
            if (a.loop) {
                clip.loop.on = a.loop->on;
                if (a.loop->start) clip.loop.start = *a.loop->start;
                if (a.loop->length) clip.loop.length = *a.loop->length;
                if (clip.loop.on) SUPPLE_TRY(checkClipLength(clip.loop.length, "/loop/length"));
            }
            if (a.name) clip.name = *a.name;
            if (a.color) clip.color = *a.color;
            if (a.mute) clip.mute = *a.mute;
            if (a.gainDb) clip.gainDb = *a.gainDb;
            if (a.mode) clip.mode = *a.mode;
            if (a.quantize) {
                if (a.quantize->isNull())
                    clip.quantize.reset();
                else  // the schema has checked the name
                    for (const auto& [value, name] : model::kLaunchQuantizeNames)
                        if (a.quantize->isString() && a.quantize->asString() == name)
                            clip.quantize = value;
            }
            app.txn().modify<model::Clip>(*id, [&](model::Clip& c2) { c2 = clip; });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ClipRef, Empty> c;
        c.name = "clip.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Remove a clip with its takes (unpooled sources go too).";
        c.tags = {"clip launcher", "clip"};
        c.args =
            schema::object<ClipRef>("clip_ref").field("clip", &ClipRef::clip, clipIdArg()).build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"clip": "cl_1"})"), j("{}"), "song_with_clip"}};
        c.run = [](const ClipRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = clipArg(s, a.clip, "/clip");
            if (!id) return std::move(id).error();
            SUPPLE_TRY(removeClipObjects(app.txn(), s, *id));
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ClipMoveArgs, Empty> c;
        c.name = "clip.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Move a clip to an empty cell: another scene, another channel, or both.";
        c.tags = {"clip launcher", "clip"};
        c.args = schema::object<ClipMoveArgs>("clip_move_args")
                     .field("clip", &ClipMoveArgs::clip, clipIdArg())
                     .field("track", &ClipMoveArgs::track, idArg("tr", "Track"))
                     .field("scene", &ClipMoveArgs::scene, sceneIdArg())
                     .build();
        c.result = emptySchema();
        c.errors = {{"clip.occupied", "the cell holds a clip", "choose an empty cell"},
                    {"clip.track", "the track is not a channel", "choose a channel"}};
        c.examples = {{"To the chorus", j(R"({"clip": "cl_1", "scene": "sc_2"})"), j("{}"),
                       "song_with_clip"}};
        c.run = [](const ClipMoveArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = clipArg(s, a.clip, "/clip");
            if (!id) return std::move(id).error();
            const model::Clip& clip = *s.clips.find(*id);
            Id track = clip.track, scene = clip.scene;
            if (a.track) {
                auto tr = existing<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                SUPPLE_TRY(checkClipTrack(s, *tr, "/track"));
                track = *tr;
            }
            if (a.scene) {
                auto sc = sceneArg(s, *a.scene, "/scene", "clip.scene");
                if (!sc) return std::move(sc).error();
                scene = *sc;
            }
            SUPPLE_TRY(checkCellFree(s, track, scene, a.scene ? "/scene" : "/track", *id));
            app.txn().modify<model::Clip>(*id, [&](model::Clip& c2) {
                c2.track = track;
                c2.scene = scene;
            });
            return Empty{};
        };
        addChecked(registry, std::move(c));
    }();
    [&] {
        Command<ClipDuplicateArgs, IdResult> c;
        c.name = "clip.duplicate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Copy a clip and its takes to an empty cell.";
        c.description =
            "The copy's MIDI is its own unless pooled, which shares it (both clips then edit "
            "the same notes).";
        c.tags = {"clip launcher", "clip"};
        c.args = schema::object<ClipDuplicateArgs>("clip_duplicate_args")
                     .field("clip", &ClipDuplicateArgs::clip, clipIdArg())
                     .field("track", &ClipDuplicateArgs::track, idArg("tr", "Track"))
                     .field("scene", &ClipDuplicateArgs::scene, sceneIdArg())
                     .field("pooled", &ClipDuplicateArgs::pooled, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = idResultSchema();
        c.errors = {{"clip.occupied", "the cell holds a clip", "choose an empty cell"},
                    {"clip.limit", "the project has 16384 clips", "remove clips first"}};
        c.examples = {{"Into the chorus", j(R"({"clip": "cl_1", "scene": "sc_2"})"),
                       j(R"({"id": "cl_2"})"), "song_with_clip"}};
        c.run = [](const ClipDuplicateArgs& a, Context& ctx) -> Result<IdResult> {
            EngineApp& app = engine::app(ctx);
            const model::Session s = app.view();
            auto id = clipArg(s, a.clip, "/clip");
            if (!id) return std::move(id).error();
            model::Clip copy = *s.clips.find(*id);
            if (a.track) {
                auto tr = existing<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                SUPPLE_TRY(checkClipTrack(s, *tr, "/track"));
                copy.track = *tr;
            }
            if (a.scene) {
                auto sc = sceneArg(s, *a.scene, "/scene", "clip.scene");
                if (!sc) return std::move(sc).error();
                copy.scene = *sc;
            }
            SUPPLE_TRY(checkCellFree(s, copy.track, copy.scene, a.scene ? "/scene" : "/track"));
            if (s.clips.size() >= model::kMaxClips)
                return Error("clip.limit",
                             "the project has " + std::to_string(model::kMaxClips) + " clips", {},
                             "remove clips first (clip.remove)");
            model::EditTxn& t = app.txn();
            const model::Clip& original = *s.clips.find(*id);
            auto copied = copyTakesFor(t, s, original.takes, a.pooled, {}, "/clip");
            if (!copied) return std::move(copied).error();
            copy.takes = *copied;
            copy.activeTake.reset();
            for (std::size_t k = 0; k < original.takes.size(); ++k)
                if (original.activeTake == original.takes[k]) copy.activeTake = copy.takes[k];
            const Id fresh = t.create(std::move(copy));
            SUPPLE_TRY(checkClipMemory(app, t.preview(), "/clip"));
            return IdResult{fresh.str()};
        };
        addChecked(registry, std::move(c));
    }();
}

}  // namespace

// --- describe ------------------------------------------------------------------------

std::string describeScenes(const model::Session& s) {
    if (s.scenes.empty()) return {};
    const auto order = scenesInOrder(s);
    const auto tracks = tracksInDisplayOrder(s);
    std::string out = "Scenes (" + std::to_string(order.size()) + "):\n";
    for (Id sc : order) {
        const model::Scene& scene = *s.scenes.find(sc);
        out += "  " + sc.str() + (scene.name.empty() ? "" : " " + scene.name);
        bool any = false;
        std::string lines;
        for (Id tr : tracks) {
            const auto cl = model::clipAt(s, tr, sc);
            if (!cl) continue;
            any = true;
            const model::Clip& clip = *s.clips.find(*cl);
            const model::Track& track = *s.tracks.find(tr);
            std::string line = "    " + tr.str() + (track.name.empty() ? "" : " " + track.name) +
                               ": " + clipLabel(s, *cl) + " ";
            if (clip.kind == model::ClipKind::Midi) {
                line += number(clip.length) + " beats";
            } else {
                char secs[48];
                std::snprintf(secs, sizeof secs, "%.1f s", clip.length);
                line += secs;
            }
            if (clip.mode == model::ClipMode::Once) line += " once";
            if (clip.loop.on)
                line += " · loop " + number(clip.loop.start) + "–" +
                        number(clip.loop.start + clip.loop.length);
            const model::Take* take = clip.activeTake ? s.takes.find(*clip.activeTake) : nullptr;
            line += clip.kind == model::ClipKind::Midi ? " · MIDI" : " · audio";
            if (take && take->source) {
                if (take->source->kind == model::Kind::Midi) {
                    const model::MidiSource* m = s.midi.find(*take->source);
                    line += " " + take->source->str() + ", " +
                            std::to_string(m ? m->sequence.notes.size() : 0) + " notes";
                } else {
                    const model::MediaSource* m = s.media.find(*take->source);
                    line += " " + take->source->str() + " " +
                            (m ? fs::path(m->path).filename().string() : "");
                }
            } else {
                line += ", empty";
            }
            if (clip.takes.size() > 1) line += " · " + std::to_string(clip.takes.size()) + " takes";
            if (clip.quantize)
                line += " · quantize " + std::string(model::launchQuantizeName(*clip.quantize));
            if (clip.mute) line += " · muted";
            lines += line + "\n";
        }
        out += any ? "\n" + lines : " (no clips)\n";
    }
    return out;
}

void registerClipLauncherCommands(cmd::Registry& registry) {
    registerSceneCommands(registry);
    registerClipCommands(registry);
}

}  // namespace supple::engine
