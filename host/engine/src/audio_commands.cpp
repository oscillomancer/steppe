// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <cmath>
#include <set>
#include "command_validation.h"
#include "supple/base/sha256.h"
#include "supple/model/audio_warp.h"
#include "supple/model/schemas.h"
#include "supple/model/take_owner.h"
#include "transients.h"

namespace supple::engine {
namespace {
using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;
template <class Args, class Output>
void addAudio(cmd::Registry& registry, Command<Args, Output> command) {
    command.tags = {"audio", "warp"};
    command.errors = {
        {"audio.*", "invalid, absent or stale source hits or incompatible items",
         "detect current audio hits; align item spans and use a finer grid or lower strength"},
        {"warp.*", "unsupported map, mode, bundle or preparation budget",
         "use nonlooped Signalsmith pitch preservation and ordered anchors; include all peers or "
         "detach"},
        {"item.locked", "a target item or warp peer is locked",
         "unlock the affected item with item.set"},
        {"take.unknown", "the take no longer exists", "choose a current take from take.list"}};
    addChecked(registry, std::move(command));
}
EngineApp& app(Context& c) { return c.service<EngineApp>(); }
json::Value j(std::string_view s) { return json::parse(s).value(); }
auto idSchema(const char* prefix) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$", "object id");
}
Result<Id> takeId(const model::Session& s, const std::string& text) {
    auto id = Id::parse(text);
    if (!id || !s.takes.contains(*id))
        return Error("take.unknown", "take does not exist", "/take", "use take.list");
    const auto* t = s.takes.find(*id);
    if (!t->source || !s.media.contains(*t->source))
        return Error("audio.source", "take has no audio source", "/take", "choose an audio take");
    SUPPLE_TRY(checkUnlockedTake(s, *id, "/take"));
    return *id;
}
const model::Item* owner(const model::Session& s, Id id) {
    const auto held = model::takeOwner(s, id);
    return held ? s.items.find(*held) : nullptr;
}
Status warpAllowed(const model::Session& s, Id id) {
    const auto* t = s.takes.find(id);
    const auto* it = owner(s, id);
    if (!t || !it || it->loop || !t->preservePitch || t->pitchMode != "signalsmith")
        return Error("warp.mode", "warping needs nonlooped pitch-preserving audio", "/take",
                     "disable item looping and set preserve_pitch true and pitch_mode signalsmith");
    return model::validateWarpMap(*t, "/take");
}
void anchors(model::Take& t, double duration) {
    if (t.stretchMarkers.empty())
        t.stretchMarkers = {{t.startOffset, 0, 0},
                            {t.startOffset + duration * t.playrate, duration, 0}};
}
struct HitSet {
    std::string take, sourceHash, algorithm = "manual";
    std::vector<double> positions;
    double sensitivity = .5;
};
struct Detect {
    std::vector<std::string> takes;
    double sensitivity = .5;
};
struct Marker {
    std::string take;
    std::optional<double> sourcePos;
    double itemPos = 0;
    std::uint64_t index = 0;
};
struct Items {
    std::vector<std::string> items;
};
struct Quantize {
    std::vector<std::string> items;
    double grid = .25, strength = 1, swing = 0, crossfade = .005;
    std::string mode = "warp", reference, hits = "reference";
};
struct Job {
    std::string job;
};
Result<std::vector<Id>> itemIds(const model::Session& s, const std::vector<std::string>& names) {
    std::vector<Id> ids;
    for (const auto& name : names) {
        auto id = Id::parse(name);
        if (!id || !s.items.contains(*id))
            return Error("item.unknown", "item does not exist", "/items",
                         "choose existing audio items");
        SUPPLE_TRY(checkUnlocked(s, *id, "/items"));
        const auto& it = *s.items.find(*id);
        if (!it.activeTake)
            return Error("audio.source", "item has no active take", "/items",
                         "activate an audio take");
        auto take = takeId(s, it.activeTake->str());
        if (!take) return take.error();
        if (std::find(ids.begin(), ids.end(), *id) != ids.end())
            return Error("audio.items", "duplicate item", "/items", "include each item once");
        ids.push_back(*id);
    }
    if (ids.empty())
        return Error("audio.items", "no items supplied", "/items",
                     "supply at least one audio item");
    return ids;
}
void detach(EngineApp& a, Id id) {
    const auto s = a.view();
    const auto* t = s.takes.find(id);
    if (!t) return;
    for (auto peer : t->warpPeers)
        if (s.takes.contains(peer))
            a.txn().modify<model::Take>(peer, [&](auto& p) {
                std::erase(p.warpPeers, id);
                if (p.warpPeers.size() < 2 || peer == id) p.warpPeers.clear();
            });
}
Result<json::Value> invoke(Context& ctx, const char* name, json::Value args) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
}
double nearest(double beat, double grid, double swing) {
    const auto base = std::floor(beat / (2 * grid)) * 2;
    double best = base * grid, distance = std::abs(beat - best);
    for (int i = -2; i <= 3; ++i) {
        const double k = base + i;
        const double p = k * grid + (std::fmod(std::abs(k), 2.) == 1 ? swing * grid / 2 : 0);
        const double d = std::abs(beat - p);
        if (d < distance || (d == distance && p < best)) {
            best = p;
            distance = d;
        }
    }
    return best;
}
}  // namespace
void registerAudioCommands(cmd::Registry& registry) {
    {
        Command<HitSet, json::Value> c;
        c.name = "take.hits.set";
        c.kind = Kind::Edit;
        c.summary = "Replace a take's transient candidates in original source seconds.";
        c.args =
            schema::object<HitSet>("take_hits_set")
                .field("take", &HitSet::take, idSchema("tk"))
                .field("positions", &HitSet::positions,
                       schema::arrayOf(schema::number().min(0)).maxItems(model::kMaxAudioMarkers))
                .field("source_hash", &HitSet::sourceHash, schema::string())
                .defaultValue(std::string())
                .field("algorithm", &HitSet::algorithm, schema::string())
                .defaultValue(std::string("manual"))
                .field("sensitivity", &HitSet::sensitivity, schema::number().range(0, 1))
                .defaultValue(.5)
                .build();
        c.result = schema::any();
        c.examples = {{"Mark attacks", j(R"({"take":"tk_1","positions":[0.1,0.3]})"),
                       j(R"("<any>")"), "song_with_audio"}};
        c.run = [](const HitSet& a, Context& ctx) -> Result<json::Value> {
            const auto s = app(ctx).view();
            auto id = takeId(s, a.take);
            if (!id) return id.error();
            const auto source = *s.takes.find(*id)->source;
            const auto& md = *s.media.find(source);
            if (!a.sourceHash.empty() && !md.hash.empty() && a.sourceHash != md.hash)
                return Error("audio.stale", "source identity changed", "/source_hash",
                             "reimport changed audio and detect hits again");
            for (double p : a.positions)
                if (p * md.sampleRate >= static_cast<double>(md.frames))
                    return Error("audio.hits", "hit is outside the source", "/positions",
                                 "use positions before the source end");
            const auto hash = a.sourceHash.empty() ? md.hash : a.sourceHash;
            app(ctx).txn().modify<model::Take>(*id, [&](auto& t) {
                t.transients = model::Transients{a.algorithm, hash, a.sensitivity, a.positions};
            });
            if (md.hash.empty() && !hash.empty())
                app(ctx).txn().modify<model::MediaSource>(source, [&](auto& m) { m.hash = hash; });
            auto out = json::Value::object();
            out["count"] = static_cast<std::uint64_t>(a.positions.size());
            return out;
        };
        addAudio(registry, std::move(c));
    }
    {
        Command<Detect, Job> c;
        c.name = "take.hits.detect";
        c.kind = Kind::Action;
        c.summary = "Detect transient candidates in audio takes, then commit one undoable edit.";
        c.args = schema::object<Detect>("take_hits_detect")
                     .field("takes", &Detect::takes,
                            schema::arrayOf(idSchema("tk")).minItems(1).maxItems(128))
                     .field("sensitivity", &Detect::sensitivity, schema::number().range(0, 1))
                     .defaultValue(.5)
                     .build();
        c.result =
            schema::object<Job>("audio_job").field("job", &Job::job, schema::string()).build();
        c.examples = {{"Detect attacks", j(R"({"takes":["tk_1"]})"), j(R"({"job":"<any>"})"),
                       "song_with_audio"}};
        c.run = [](const Detect& a, Context& ctx) -> Result<Job> {
            auto& host = app(ctx);
            const auto s = host.view();
            std::vector<Id> takes;
            for (const auto& name : a.takes) {
                auto id = takeId(s, name);
                if (!id) return id.error();
                takes.push_back(*id);
            }
            auto& jobs = ctx.service<cmd::JobManager>();
            auto& dispatcher = ctx.service<cmd::Dispatcher>();
            const auto id = jobs.create();
            const auto generation = host.projectGeneration();
            const auto branch = host.versions().branch();
            const auto directory = host.projectDir();
            const auto origin = ctx.origin();
            host.startJob([&host, &jobs, &dispatcher, s, takes, id, generation, branch, directory,
                           origin, sensitivity = a.sensitivity](StopToken stop) {
                auto results = json::Value::array();
                for (auto take : takes) {
                    if (stop.stop_requested() || jobs.cancelled(id)) return;
                    const auto& md = *s.media.find(*s.takes.find(take)->source);
                    std::filesystem::path path = md.path;
                    if (path.is_relative()) {
                        const auto media = directory / s.project->mediaDir / path;
                        std::error_code error;
                        path = std::filesystem::exists(media, error) ? media : directory / path;
                    }
                    auto hash = sha256File(path);
                    if (!hash) {
                        (void)jobs.fail(id, hash.error());
                        return;
                    }
                    auto audio = audio::readAudio(path);
                    if (!audio) {
                        (void)jobs.fail(id, audio.error());
                        return;
                    }
                    auto hits = detectTransients(*audio, sensitivity, [&] {
                        return stop.stop_requested() || jobs.cancelled(id);
                    });
                    if (!hits) {
                        if (!jobs.cancelled(id)) (void)jobs.fail(id, hits.error());
                        return;
                    }
                    auto after = sha256File(path);
                    if (!after || *after != *hash) {
                        (void)jobs.fail(
                            id, Error("audio.changed", "source changed during detection", {},
                                      "finish editing the file, then detect again"));
                        return;
                    }
                    auto args = json::Value::object();
                    args["take"] = take.str();
                    args["source_hash"] = *hash;
                    args["algorithm"] = "flux-v1";
                    args["sensitivity"] = sensitivity;
                    args["positions"] = json::Value::array();
                    for (double hit : *hits) args["positions"].push_back(hit);
                    results.push_back(std::move(args));
                }
                dispatcher.post(
                    "audio.detect.commit",
                    [&host, &jobs, &dispatcher, s, takes, id, generation, branch, origin,
                     results = std::move(results)]() -> Status {
                        if (jobs.cancelled(id)) return {};
                        auto stale = [&]() {
                            return jobs.fail(id, Error("audio.stale", "detection target changed",
                                                       {}, "detect the current takes again"));
                        };
                        if (host.projectGeneration() != generation ||
                            host.versions().branch() != branch)
                            return stale();
                        const auto current = host.view();
                        for (auto take : takes) {
                            const auto* now = current.takes.find(take);
                            const auto* before = s.takes.find(take);
                            if (!now || now->source != before->source ||
                                now->transients != before->transients)
                                return stale();
                            const auto* md = current.media.find(*now->source);
                            if (!md ||
                                model::toJson(*md) != model::toJson(*s.media.find(*before->source)))
                                return stale();
                        }
                        auto result = dispatcher.transact(
                            "Detect hits", origin, [&]() -> Result<json::Value> {
                                for (const auto& args : results.asArray()) {
                                    auto set = dispatcher.execute({"take.hits.set", args, origin});
                                    if (!set) return set.error();
                                }
                                auto out = json::Value::object();
                                out["takes"] = json::Value::array();
                                for (auto take : takes) out["takes"].push_back(take.str());
                                return out;
                            });
                        if (!result) return jobs.fail(id, result.error());
                        return jobs.complete(id, *result);
                    },
                    cmd::Dispatcher::PostKind::Edit);
            });
            return Job{id};
        };
        addAudio(registry, std::move(c));
    }
    for (const std::string operation : {"add", "move", "remove"}) {
        Command<Marker, json::Value> c;
        c.name = "take.stretch." + operation;
        c.kind = Kind::Edit;
        c.summary = operation == "add"    ? "Add a pitch-preserving audio stretch marker."
                    : operation == "move" ? "Move an audio stretch marker's item position."
                                          : "Remove an interior audio stretch marker.";
        if (operation == "add")
            c.description =
                "Omit source_pos to insert a point on the current source map at item_pos "
                "without changing its timing. An identical existing point is not duplicated. "
                "Warp peers receive the same source-relative point; ordinary edit groups stay "
                "local, as with other take edits. Requires nonlooped pitch-preserving audio.";
        auto args = schema::object<Marker>("stretch_marker_args")
                        .field("take", &Marker::take, idSchema("tk"));
        if (operation == "add")
            args.field("source_pos", &Marker::sourcePos, schema::number());
        else
            args.field("index", &Marker::index, schema::integerOf<std::uint64_t>());
        if (operation != "remove")
            args.field("item_pos", &Marker::itemPos, schema::number().min(0));
        c.args = args.build();
        c.result = schema::any();
        c.examples = {{"Edit an anchor",
                       operation == "add" ? j(R"({"take":"tk_1","source_pos":0.3,"item_pos":0.25})")
                       : operation == "move" ? j(R"({"take":"tk_1","index":1,"item_pos":0.26})")
                                             : j(R"({"take":"tk_1","index":1})"),
                       j(R"("<any>")"), operation == "add" ? "song_with_audio" : "song_with_warp"}};
        if (operation == "add")
            c.examples.push_back({"Insert an anchor without moving audio",
                                  j(R"({"take":"tk_1","item_pos":0.25})"), j(R"("<any>")"),
                                  "song_with_audio"});
        c.run = [operation](const Marker& a, Context& ctx) -> Result<json::Value> {
            auto& host = app(ctx);
            const auto s = host.view();
            auto id = takeId(s, a.take);
            if (!id) return id.error();
            SUPPLE_TRY(warpAllowed(s, *id));
            const auto& base = *s.takes.find(*id);
            const double sourcePos = a.sourcePos.value_or(model::audioSourceAt(base, a.itemPos));
            const auto peers = base.warpPeers.empty() ? std::vector<Id>{*id} : base.warpPeers;
            for (auto peer : peers) {
                SUPPLE_TRY(checkUnlockedTake(s, peer, "/take"));
                SUPPLE_TRY(warpAllowed(s, peer));
                auto t = *s.takes.find(peer);
                const auto span = model::itemSpan(s, *owner(s, peer));
                const double duration = span.endSec - span.startSec;
                anchors(t, duration);
                if (operation == "add") {
                    if (!(a.itemPos > 0 && a.itemPos < duration))
                        return Error("warp.boundary", "anchor must be inside the item", "/item_pos",
                                     "choose a position between item boundaries");
                    const double source = sourcePos + (t.startOffset - base.startOffset);
                    if (std::any_of(t.stretchMarkers.begin(), t.stretchMarkers.end(),
                                    [&](const auto& marker) {
                                        return marker.itemPos == a.itemPos &&
                                               marker.sourcePos == source;
                                    }))
                        continue;
                    t.stretchMarkers.push_back({source, a.itemPos, 0});
                    std::sort(t.stretchMarkers.begin(), t.stretchMarkers.end(),
                              [](auto x, auto y) { return x.itemPos < y.itemPos; });
                } else {
                    if (a.index == 0 || t.stretchMarkers.size() < 3 ||
                        a.index >= t.stretchMarkers.size() - 1)
                        return Error(
                            "warp.index", "not an interior anchor", "/index",
                            "use an interior index from take.list; reset removes all warping");
                    if (operation == "remove")
                        t.stretchMarkers.erase(t.stretchMarkers.begin() +
                                               static_cast<std::ptrdiff_t>(a.index));
                    else
                        t.stretchMarkers[static_cast<std::size_t>(a.index)].itemPos = a.itemPos;
                }
                SUPPLE_TRY(model::validateWarpMap(t, "/take"));
                host.txn().put(peer, std::move(t));
            }
            auto out = json::Value::object();
            out["markers"] = *model::toJson(*host.view().takes.find(*id)).find("stretch_markers");
            return out;
        };
        addAudio(registry, std::move(c));
    }
    for (const bool reset : {false, true}) {
        Command<Items, json::Value> c;
        c.name = reset ? "audio.warp.reset" : "audio.warp.detach";
        c.kind = Kind::Edit;
        c.summary = reset ? "Remove selected items' warping while keeping detected hits."
                          : "Detach selected items from jointly processed warp bundles.";
        c.args = schema::object<Items>("audio_items")
                     .field("items", &Items::items,
                            schema::arrayOf(idSchema("it")).minItems(1).maxItems(128))
                     .build();
        c.result = schema::any();
        c.examples = {{"Clear warp", j(R"({"items":["it_1"]})"), j("{}"), "song_with_audio"}};
        c.run = [reset](const Items& a, Context& ctx) -> Result<json::Value> {
            auto& host = app(ctx);
            const auto s = host.view();
            auto ids = itemIds(s, a.items);
            if (!ids) return ids.error();
            for (auto id : *ids) {
                const auto take = *s.items.find(id)->activeTake;
                for (auto peer : s.takes.find(take)->warpPeers)
                    SUPPLE_TRY(checkUnlockedTake(s, peer, "/items"));
                detach(host, take);
                if (reset)
                    host.txn().modify<model::Take>(take, [](auto& t) { t.stretchMarkers.clear(); });
            }
            return json::Value::object();
        };
        addAudio(registry, std::move(c));
    }
    {
        Command<Quantize, json::Value> c;
        c.name = "audio.quantize";
        c.kind = Kind::Edit;
        c.summary = "Quantize detected audio hits together using stretch markers or slices.";
        c.description =
            "Consumes stored source hits. Detect absent/stale hits with take.hits.detect. Aligned "
            "items share one timing map; warp mode jointly processes their channels. Slice mode "
            "requires unwarped input; strength 0 splits without moving hits.";
        c.args =
            schema::object<Quantize>("audio_quantize")
                .field("items", &Quantize::items,
                       schema::arrayOf(idSchema("it")).minItems(1).maxItems(128))
                .field("grid", &Quantize::grid, schema::number().range(1. / 1024, 1024))
                .field("strength", &Quantize::strength, schema::number().range(0, 1))
                .defaultValue(1.)
                .field("swing", &Quantize::swing, schema::number().range(0, 1))
                .defaultValue(0.)
                .field("crossfade", &Quantize::crossfade, schema::number().range(0, .05))
                .defaultValue(.005)
                .field("mode", &Quantize::mode,
                       schema::enumeration<std::string>({{"warp", "warp"}, {"slice", "slice"}}))
                .defaultValue(std::string("warp"))
                .field("reference", &Quantize::reference, schema::string())
                .defaultValue(std::string())
                .field("hits", &Quantize::hits,
                       schema::enumeration<std::string>(
                           {{"reference", "reference"}, {"merged", "merged"}}))
                .defaultValue(std::string("reference"))
                .build();
        c.result = schema::any();
        c.examples = {{"Warp to sixteenths", j(R"({"items":["it_1"],"grid":0.25})"),
                       j(R"("<any>")"), "song_with_hits"},
                      {"Split without moving",
                       j(R"({"items":["it_1"],"grid":0.25,"mode":"slice","strength":0})"),
                       j(R"("<any>")"), "song_with_hits"}};
        c.run = [](const Quantize& a, Context& ctx) -> Result<json::Value> {
            auto& host = app(ctx);
            const auto s = host.view();
            auto items = itemIds(s, a.items);
            if (!items) return items.error();
            Id reference = items->front();
            if (!a.reference.empty()) {
                auto id = Id::parse(a.reference);
                if (!id || std::find(items->begin(), items->end(), *id) == items->end())
                    return Error("audio.reference", "reference must be selected", "/reference",
                                 "choose an item from items");
                reference = *id;
            }
            const auto span = model::itemSpan(s, *s.items.find(reference));
            const double duration = span.endSec - span.startSec;
            const auto& base = *s.takes.find(*s.items.find(reference)->activeTake);
            std::vector<Id> peers;
            for (auto id : *items) {
                const auto& it = *s.items.find(id);
                const auto& t = *s.takes.find(*it.activeTake);
                const auto itemSpan = model::itemSpan(s, it);
                SUPPLE_TRY(warpAllowed(s, *it.activeTake));
                if (itemSpan.startSec != span.startSec || itemSpan.endSec != span.endSec ||
                    t.playrate != base.playrate || t.pitch != base.pitch)
                    return Error("audio.alignment",
                                 "selected audio must have aligned spans, rates and pitch",
                                 "/items",
                                 "align or trim the items and match take rates and pitch first");
                if (a.mode == "slice")
                    for (auto tk : it.takes)
                        if (!s.takes.find(tk)->stretchMarkers.empty())
                            return Error("audio.slice_warp", "slice requires unwarped takes",
                                         "/items", "remove warping before slicing");
                for (auto peer : t.warpPeers)
                    if (std::none_of(items->begin(), items->end(), [&](auto selected) {
                            return s.items.find(selected)->activeTake == peer;
                        }))
                        return Error("warp.peers", "selection omits linked warp peers", "/items",
                                     "select the complete bundle or detach first");
                peers.push_back(*it.activeTake);
            }
            std::sort(peers.begin(), peers.end());
            std::vector<double> hits;
            for (auto id : *items) {
                if (a.hits == "reference" && id != reference) continue;
                const auto& t = *s.takes.find(*s.items.find(id)->activeTake);
                const auto& md = *s.media.find(*t.source);
                if (!t.transients || t.transients->sourceHash != md.hash)
                    return Error("audio.hits_missing", "detected hits are absent or stale",
                                 "/items", "run take.hits.detect and wait for its result edit");
                for (double source : t.transients->positions) {
                    const double local = model::audioItemAt(t, source);
                    if (local > 0 && local < duration) hits.push_back(local);
                }
            }
            std::sort(hits.begin(), hits.end());
            std::vector<double> unique;
            for (double hit : hits)
                if (unique.empty() || hit - unique.back() > (a.hits == "merged" ? .005 : 1e-9))
                    unique.push_back(hit);
            hits = std::move(unique);
            if (hits.empty())
                return Error(
                    "audio.hits_empty", "no interior hits to quantize", "/items",
                    "detect hits at higher sensitivity or set candidates with take.hits.set");
            if (hits.size() + 2 > model::kMaxAudioMarkers)
                return Error("audio.hit_budget", "too many merged hits", "/items",
                             "use fewer items or lower sensitivity");
            std::vector<double> targets;
            double previous = 0;
            for (double hit : hits) {
                const double beat = s.timeMap->beatAt(span.startSec + hit);
                const double moved = beat + a.strength * (nearest(beat, a.grid, a.swing) - beat);
                const double target = s.timeMap->secondsAt(moved) - span.startSec;
                if (target <= previous + 1 / s.project->sampleRate || target >= duration)
                    return Error("audio.collision",
                                 "quantized hits collide or reach the item boundary", "/grid",
                                 "use a finer grid or lower strength");
                targets.push_back(target);
                previous = target;
            }
            auto out = json::Value::object();
            out["items"] = json::Value::array();
            out["movements"] = json::Value::array();
            for (std::size_t i = 0; i < hits.size(); ++i) {
                auto move = json::Value::object();
                move["from"] = hits[i];
                move["to"] = targets[i];
                out["movements"].push_back(move);
            }
            if (a.mode == "warp") {
                for (auto id : *items) {
                    const auto take = *s.items.find(id)->activeTake;
                    auto t = *s.takes.find(take);
                    t.stretchMarkers.clear();
                    const auto& original = *s.takes.find(take);
                    t.stretchMarkers.push_back({model::audioSourceAt(original, 0), 0, 0});
                    for (std::size_t i = 0; i < hits.size(); ++i)
                        t.stretchMarkers.push_back(
                            {model::audioSourceAt(original, hits[i]), targets[i], 0});
                    t.stretchMarkers.push_back(
                        {model::audioSourceAt(original, duration), duration, 0});
                    t.warpPeers = peers.size() > 1 ? peers : std::vector<Id>{};
                    SUPPLE_TRY(model::validateWarpMap(t, "/items"));
                    host.txn().put(take, std::move(t));
                    out["items"].push_back(id.str());
                }
            } else {
                std::vector<double> cuts{0}, moved{0};
                cuts.insert(cuts.end(), hits.begin(), hits.end());
                moved.insert(moved.end(), targets.begin(), targets.end());
                cuts.push_back(duration);
                for (auto original : *items) {
                    std::vector<Id> slices{original};
                    for (double hit : hits) {
                        auto args = json::Value::object();
                        args["item"] = slices.back().str();
                        args["at"] = s.timeMap->beatAt(span.startSec + hit);
                        auto split = invoke(ctx, "item.split", args);
                        if (!split) return split.error();
                        slices.push_back(*Id::parse(split->find("right")->asString()));
                    }
                    for (std::size_t i = 0; i < slices.size(); ++i) {
                        auto state = host.view();
                        auto item = *state.items.find(slices[i]);
                        const double pre =
                            i ? std::min({a.crossfade, (cuts[i] - cuts[i - 1]) / 2, moved[i]}) : 0;
                        const double start = span.startSec + moved[i] - pre;
                        const double naturalEnd = span.startSec + moved[i] + cuts[i + 1] - cuts[i];
                        const double end = i + 1 < slices.size()
                                               ? std::min(naturalEnd, span.startSec + moved[i + 1])
                                               : span.endSec;
                        if (!(end > start))
                            return Error("audio.slice_range", "slice movement removes its duration",
                                         "/strength", "lower quantization strength");
                        for (auto take : item.takes)
                            host.txn().modify<model::Take>(take, [&](auto& t) {
                                if (t.source && t.source->kind == model::Kind::Midi)
                                    t.startOffset +=
                                        (s.timeMap->beatAt(span.startSec + cuts[i] - pre) -
                                         s.timeMap->beatAt(span.startSec + cuts[i])) *
                                        t.playrate;
                                else
                                    model::rebaseAudioTake(t, -pre);
                            });
                        model::placeItemSeconds(state, item, start, end);
                        if (i) {
                            const double priorEnd =
                                span.startSec +
                                std::min(moved[i - 1] + cuts[i] - cuts[i - 1], moved[i]);
                            const double overlap = std::max(0., priorEnd - start);
                            item.fadeIn.length = overlap > 0 ? overlap : pre;
                            item.fadeIn.shape = model::FadeShape::Linear;
                        }
                        if (i + 1 < slices.size()) {
                            const double nextPre =
                                std::min({a.crossfade, (cuts[i + 1] - cuts[i]) / 2, moved[i + 1]});
                            const double overlap =
                                std::max(0., end - (span.startSec + moved[i + 1] - nextPre));
                            item.fadeOut.length =
                                overlap > 0 ? overlap : std::min(a.crossfade, (end - start) / 2);
                            item.fadeOut.shape = model::FadeShape::Linear;
                        }
                        host.txn().put(slices[i], std::move(item));
                        out["items"].push_back(slices[i].str());
                    }
                }
            }
            return out;
        };
        addAudio(registry, std::move(c));
    }
}
}  // namespace supple::engine
