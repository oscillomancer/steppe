// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's model commands (ADR-0097, step L1): scene.* and
// clip.*. Launching (launch.*) is the engine's, step L2.
#include "clip_launcher.h"

#include <algorithm>

#include "clip_context.h"
#include "command_helpers.h"
#include "supple/model/env_shape.h"
#include "supple/model/follow.h"
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

SceneLaunch sceneLaunchRequests(const model::Session& s, Id scene, const std::vector<Id>& skipped) {
    SceneLaunch out;
    for (Id track : tracksInDisplayOrder(s))
        for (const auto& e : s.clips) {
            if (e.value->scene != scene || e.value->track != track) continue;
            if (s.tracks.find(track)->freeze)
                out.left.push_back({"track.frozen", track, e.id});
            else if (std::find(skipped.begin(), skipped.end(), e.id) != skipped.end())
                out.left.push_back({"launch.memory", track, e.id});
            else
                out.requests.push_back({track, LaunchLogEntry::Action::Clip, e.id, scene});
        }
    return out;
}

Error sceneHasNoClip(Id scene) {
    return Error("launch.scene", scene.str() + " has no clip that can play", "/scene",
                 "add clips to the scene with clip.add, or unfreeze their tracks");
}

Error trackFrozen(Id track, const std::string& path) {
    return Error("track.frozen", track.str() + " is frozen; it plays its print", path,
                 "track.unfreeze {track: \"" + track.str() + "\"} to launch its clips");
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

bool clipLaysAsOneItem(const model::Session& s, const model::Clip& clip, const model::Take& take) {
    const double P = clip.length;
    const double content = contentLength(s, take);
    const bool wholeLoop = clip.loop.on && std::abs(clip.loop.start) <= 1e-9 * P &&
                           std::abs(clip.loop.length - content) <= 1e-9 * std::max(1.0, P);
    // A clip with a fade shapes every pass, which one looped item cannot (its
    // fades are at its outer edges only): laid as copies (S3 step C, Q4 rule 2).
    const bool fades = clip.fadeIn.length > 0 || clip.fadeOut.length > 0;
    return clip.mode != model::ClipMode::Once && take.startOffset == 0 && !fades &&
           std::abs(P - content) <= 1e-9 * std::max(1.0, P) && (!clip.loop.on || wholeLoop);
}

Status checkClipTakeUnwarped(const model::Session& s, Id take, const std::string& path) {
    const model::Take* tk = s.takes.find(take);
    if (tk && (!tk->stretchMarkers.empty() || !tk->warpPeers.empty() || model::activeWarp(*tk)))
        return Error("clip.unsupported", take.str() + " is warped, and clips do not warp", path,
                     "reset its warp first (audio.warp.reset), or keep it on the timeline");
    return {};
}

namespace {

// Points from 0 on: what lay before goes, its value at 0 kept as a point.
void fromZero(std::vector<model::EnvPoint>& pts) {
    std::stable_sort(
        pts.begin(), pts.end(),
        [](const model::EnvPoint& a, const model::EnvPoint& b) { return a.pos < b.pos; });
    const auto first =
        std::find_if(pts.begin(), pts.end(), [](const model::EnvPoint& p) { return p.pos >= 0; });
    if (first == pts.begin()) return;
    model::EnvPoint at0 = *(first - 1);
    if (first != pts.end() && first->pos > 0) {
        const model::EnvPoint& a = *(first - 1);
        const model::EnvPoint& b = *first;
        const double k = (0 - a.pos) / (b.pos - a.pos);
        at0.value = a.value + (b.value - a.value) * model::shapeCurve(a.shape, a.tension, k);
    }
    at0.pos = 0;
    const bool keep = first == pts.end() || first->pos > 0;
    pts.erase(pts.begin(), first);
    if (keep) pts.insert(pts.begin(), at0);
}

}  // namespace

FxEnvelopeMap fxEnvelopesToClip(const model::Session& s, model::ClipKind kind, double startBeat,
                                double startSec) {
    const auto map = s.timeMap;
    return [map, kind, startBeat, startSec](model::Envelope& env) {
        const bool time = env.timebase == model::Timebase::Time;
        for (auto& p : env.points)
            p.pos = kind == model::ClipKind::Midi
                        ? (time ? map->beatAt(p.pos) : p.pos) - startBeat
                        : (time ? p.pos : map->secondsAt(p.pos)) - startSec;
        env.timebase =
            kind == model::ClipKind::Midi ? model::Timebase::Beats : model::Timebase::Time;
        fromZero(env.points);
    };
}

FxEnvelopeMap fxEnvelopesFromClip(const model::Session& s, model::ClipKind kind, double pos,
                                  double phase, double period, double length) {
    const auto map = s.timeMap;
    const double startSec = map->secondsAt(pos);
    return [map, kind, pos, phase, period, length, startSec](model::Envelope& env) {
        // A span that ends on a boundary takes the next pass's start too:
        // its FX hears what follows within its latency, as the clip did.
        const double reach = phase + length;
        const double tolerance = 1e-9 * std::max(1.0, period);
        if (period > 0 && !env.points.empty() && reach >= period - tolerance) {
            // The passes the item spans, in clip time from the first pass's start.
            std::vector<model::EnvPoint> one = env.points;
            std::stable_sort(
                one.begin(), one.end(),
                [](const model::EnvPoint& a, const model::EnvPoint& b) { return a.pos < b.pos; });
            // The curve's value at the period, approached from before it.
            double atEnd = one.back().value;
            for (std::size_t k = 0; k + 1 < one.size(); ++k)
                if (one[k].pos < period && one[k + 1].pos >= period) {
                    const double t = (period - one[k].pos) / (one[k + 1].pos - one[k].pos);
                    atEnd = one[k].value + (one[k + 1].value - one[k].value) *
                                               model::shapeCurve(one[k].shape, one[k].tension, t);
                }
            std::vector<model::EnvPoint> out;
            for (int k = 0; k * period <= reach + tolerance; ++k) {
                const double base = k * period;
                const bool last = (k + 1) * period > reach + tolerance;
                if (k > 0 && one.front().pos > 0) {
                    model::EnvPoint first = one.front();
                    first.pos = base;
                    out.push_back(first);
                }
                for (const auto& p : one) {
                    if (!last && p.pos >= period) break;
                    model::EnvPoint q = p;
                    q.pos += base;
                    out.push_back(q);
                }
                if (!last) {
                    model::EnvPoint held{base + period, atEnd, model::PointShape::Linear, 0.0};
                    out.push_back(held);
                }
            }
            env.points = std::move(out);
        }
        for (auto& p : env.points)
            p.pos = kind == model::ClipKind::Midi ? pos + p.pos - phase
                                                  : map->beatAt(startSec + p.pos - phase);
        env.timebase = model::Timebase::Beats;
        fromZero(env.points);
    };
}

FxEnvelopeMap takeEnvelopesFromClip(const model::Session& s, model::ClipKind kind, double pos,
                                    double phase, double period, double length) {
    FxEnvelopeMap timeline = fxEnvelopesFromClip(s, kind, pos, phase, period, length);
    return [timeline, pos](model::Envelope& env) {
        const model::Timebase kept = env.timebase;
        timeline(env);  // timeline beats, pass after pass
        for (auto& p : env.points) p.pos -= pos;
        fromZero(env.points);  // from the item's start
        env.timebase = kept;
    };
}

void convertTakeFxEnvelopes(model::EditTxn& t, const model::Session& s, Id take,
                            const FxEnvelopeMap& map) {
    const model::Take* tk = s.takes.find(take);
    if (!tk) return;
    for (const auto& e : s.envelopes)
        if (std::find(tk->chain.begin(), tk->chain.end(), e.value->target.object) !=
            tk->chain.end())
            t.modify<model::Envelope>(e.id, [&](model::Envelope& env) { map(env); });
}

Result<std::vector<Id>> copyTakesFor(model::EditTxn& t, const model::Session& s,
                                     const std::vector<Id>& takes, bool pooled,
                                     const std::function<double(double)>& envelopePos,
                                     const std::string& path, const FxEnvelopeMap& fxEnvelope,
                                     const FxEnvelopeMap& takeEnvelope) {
    std::vector<Id> out;
    for (Id old : takes) {
        const model::Take* tk = s.takes.find(old);
        if (!tk)
            return Error("item.take", old.str() + " is missing", path,
                         "use a current take id from take.list");
        if (envelopePos || fxEnvelope || takeEnvelope) {
            // Its own envelopes are measured from its owner's start, and so
            // are its FX's on a clip (C2): none of them can map automation
            // items.
            for (const auto& e : s.envelopes)
                if (((envelopePos || takeEnvelope) && e.value->target.object == old) ||
                    (fxEnvelope && std::find(tk->chain.begin(), tk->chain.end(),
                                             e.value->target.object) != tk->chain.end()))
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
        if (takeEnvelope) {
            const model::Session now = t.preview();
            for (const auto& e : now.envelopes)
                if (e.value->target.object == *fresh)
                    t.modify<model::Envelope>(e.id,
                                              [&](model::Envelope& env) { takeEnvelope(env); });
        } else if (envelopePos) {
            convertTakeEnvelopes(t, t.preview(), *fresh, envelopePos);
        }
        if (fxEnvelope) convertTakeFxEnvelopes(t, t.preview(), *fresh, fxEnvelope);
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

std::optional<model::ClipKind> clipKindOfFx(const model::Session& s, Id fx) {
    if (fx.kind != model::Kind::Fx) return std::nullopt;
    for (const auto& e : s.takes)
        if (std::find(e.value->chain.begin(), e.value->chain.end(), fx) != e.value->chain.end())
            return clipKindOfTake(s, e.id);
    return std::nullopt;
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
    // One period from its start: the clip's fades at its edges (S3 step C).
    item.fadeIn = clip.fadeIn;
    item.fadeOut = clip.fadeOut;
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
    out.fxEnvelope = fxEnvelopesFromClip(s, clip.kind, pos);
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
    if (r.clipsSkipped.empty()) return {};
    auto n = [](double x) { return json::formatDouble(std::ceil(x)); };
    return Error("clip.memory",
                 "the clips' audio would not all fit beside the arrangement's in the " +
                     n(kPreparedSampleBudget) + "-sample budget: the arrangement holds " +
                     n(r.arrangement) + " and the clips would hold " + n(r.clips),
                 path,
                 "use shorter audio or fewer distinct files in clips, or lay clips on the "
                 "timeline (item.add {clip}) and remove them");
}

Result<model::Clip> clipFromItem(model::EditTxn& t, const model::Session& s, Id itemId, bool pooled,
                                 const std::string& path, bool* truncated) {
    const model::Item& it = *s.items.find(itemId);
    model::Clip clip;
    std::optional<model::ClipKind> kind;
    for (Id tk : it.takes) {
        SUPPLE_TRY(checkClipTakeUnwarped(s, tk, path));
        const model::Take* take = s.takes.find(tk);
        const auto k = take ? model::clipKindOf(*take) : std::nullopt;
        if (k && kind && *k != *kind)
            return Error("clip.mixed", itemId.str() + " holds MIDI and audio takes", path,
                         "a clip holds one kind: remove the other takes from the item, or "
                         "copy them into clips of their own");
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
    // Its take FX's envelopes go into clip time (ADR-0097, amended, C2).
    auto copied = copyTakesFor(t, s, it.takes, pooled, envelopePos, path,
                               fxEnvelopesToClip(s, clip.kind, span.startBeat, span.startSec));
    if (!copied) return std::move(copied).error();
    clip.takes = *copied;
    // A clip's takes hold none of the four context members: the clip's context and follow are the
    // clip's (ADR-0098, following is playback). An item whose active take follows gives the clip
    // that take's context over the span copied, re-based and in clip beats, and its follow; the
    // key and the chord in force at the span's start are carried in at 0, so the clip has a key.
    for (const Id tk : clip.takes)
        t.modify<model::Take>(
            tk, [](model::Take& k) { model::fitTakeContext(k, model::TakeHome::Clip); });
    if (clip.kind == model::ClipKind::Midi && it.activeTake)
        if (const model::Take* active = s.takes.find(*it.activeTake);
            active && active->follow != model::ClipFollow::None) {
            const double rate = active->playrate > 0 ? active->playrate : 1.0;
            const double u0 = active->contextPhase + active->startOffset;
            bool cut = false;
            clip.context = model::contextOver(*active, u0, u0 + clip.length * rate, &cut);
            if (truncated) *truncated = cut;
            for (model::ContextPoint& p : clip.context) p.beat /= rate;
            clip.follow = active->follow;
        }
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
    // An audio item's fades become the clip's, shaping each pass (S3 step C);
    // a MIDI item's play nothing, and a MIDI clip holds none.
    if (clip.kind == model::ClipKind::Audio) {
        clip.fadeIn = it.fadeIn;
        clip.fadeOut = it.fadeOut;
    }
    // An item's name is its active take's.
    if (it.activeTake)
        if (const model::Take* active = s.takes.find(*it.activeTake)) clip.name = active->name;
    return clip;
}

void nameItemFromClip(model::EditTxn& t, const model::Clip& clip, const model::Item& item) {
    if (clip.name.empty() || !item.activeTake) return;
    t.modify<model::Take>(*item.activeTake, [&](model::Take& k) { k.name = clip.name; });
}

Result<std::vector<Id>> layClipOnTimeline(model::EditTxn& t, const model::Session& s,
                                          const model::Clip& clip, double startBeat, double endBeat,
                                          bool pooled, const std::string& path) {
    const std::string name = "a clip of " + s.tracks.find(clip.track)->name;
    if (clip.takes.empty() || !clip.activeTake)
        return Error("item.source", name + " has no take to lay on the timeline", path,
                     "give the clip a take first (take.add), or remove it");
    SUPPLE_TRY(checkItemTrack(s, clip.track, clip.takes, path));
    const model::Take* active = s.takes.find(*clip.activeTake);
    if (!active)
        return Error("item.take", clip.activeTake->str() + " is missing", path,
                     "use a current take id from take.list");
    auto shaped = itemFromClip(s, clip, *active, clip.track, startBeat, path);
    if (!shaped) return std::move(shaped).error();
    model::Item item = std::move(shaped->item);
    const auto map = s.timeMap;
    const double startSec = map->secondsAt(startBeat);
    const bool audio = clip.kind == model::ClipKind::Audio;
    const double span = audio ? map->secondsAt(endBeat) - startSec : endBeat - startBeat;
    const double tolerance = 1e-9 * std::max(1.0, span);
    // An audio clip with a fade that repeats within the span: one item per
    // pass, each faded as the pass is (S3 step C, Q4 rule 2). A pass the
    // span's end cuts inside its fade-out plays that fade-out to the pass's
    // end, past the span's end, as launch.commit lays a clip still playing at
    // a capture's end (review round 135); cut before its fade-out it ends at
    // the cut, keeping its fade-in. Never a hard cut through a fade.
    const bool fades = audio && (clip.fadeIn.length > 0 || clip.fadeOut.length > 0);
    const auto playsOut = [&](double secs) {
        return secs < clip.length - tolerance && clip.fadeOut.length > 0 &&
               secs > clip.length - clip.fadeOut.length + tolerance;
    };
    if (fades && clip.mode == model::ClipMode::Loop && clip.length < span - tolerance) {
        std::vector<Id> laid;
        for (std::int64_t k = 0; double(k) * clip.length < span - tolerance; ++k) {
            const double from = startSec + double(k) * clip.length;
            double secs = std::min(clip.length, span - double(k) * clip.length);
            if (playsOut(secs)) secs = clip.length;
            const double beat = map->beatAt(from);
            auto pass = itemFromClip(s, clip, *active, clip.track, beat, path);
            if (!pass) return std::move(pass).error();
            model::Item one = std::move(pass->item);
            if (secs < clip.length - tolerance) {
                SUPPLE_TRY(model::checkedBeat(s, map->beatAt(from + secs), path));
                model::placeItemSeconds(s, one, from, from + secs);
                one.fadeOut = {};
            }
            auto copied = copyTakesFor(t, s, clip.takes, pooled, pass->envelopePos, path,
                                       fxEnvelopesFromClip(s, clip.kind, beat, 0, clip.length,
                                                           std::min(secs, clip.length)));
            if (!copied) return std::move(copied).error();
            one.takes = *copied;
            for (std::size_t i = 0; i < clip.takes.size(); ++i)
                if (clip.activeTake == clip.takes[i]) one.activeTake = one.takes[i];
            nameItemFromClip(t, clip, one);
            laid.push_back(t.create(std::move(one)));
        }
        return laid;
    }
    // How long the item lasts, in the clip's unit (ADR-0097 "Scene <-> section").
    double length = clip.length;
    if (clip.mode == model::ClipMode::Once) {
        length = std::min(clip.length, span);
        if (audio && playsOut(length)) length = clip.length;
    } else if (clip.length >= span - tolerance) {
        length = audio && playsOut(span) ? clip.length : span;
    } else {
        // The clip repeats its period; one looping item reproduces that only when the
        // period is the content (loop off, length = content) or whole repeats of it (loop
        // on over the whole content).
        const double content = contentLength(s, *active);
        const double times = content > 0 ? clip.length / content : 0;
        const bool whole = times >= 1 - 1e-9 && std::abs(times - std::round(times)) < 1e-9;
        const bool repeats = item.loop ? whole : std::abs(times - 1) < 1e-9;
        if (!repeats)
            return Error("scene.insert.period",
                         "the clip of " + s.tracks.find(clip.track)->name + " repeats every " +
                             json::formatDouble(clip.length) + (audio ? " s" : " beats") +
                             " but its content is " + json::formatDouble(content) +
                             (audio ? " s" : " beats") +
                             ", which no single looping item "
                             "reproduces",
                         path,
                         "set the clip's length to its content (clip.set), or its loop on over "
                         "the whole content with a length of whole repeats, or ask for no more "
                         "bars than the clip is long");
        item.loop = true;
        length = span;
    }
    if (std::abs(length - clip.length) > tolerance) {
        // Cut short by the span before its fade-out: it ends at the cut.
        if (length < clip.length) item.fadeOut = {};
        if (audio) {
            SUPPLE_TRY(model::checkedBeat(s, map->beatAt(startSec + length), path));
            model::placeItemSeconds(s, item, startSec, startSec + length);
        } else {
            SUPPLE_TRY(model::checkedBeat(s, startBeat + length, path));
            model::placeItem(s, item, startBeat, length);
        }
    }
    // Its take FX's envelopes, in clip time, onto the timeline pass after
    // pass over the item (ADR-0097, amended, C2).
    // Over more than one period, its take envelopes repeat as its FX's do.
    const bool passes = item.loop && length > clip.length + tolerance;
    auto copied =
        copyTakesFor(t, s, clip.takes, pooled, shaped->envelopePos, path,
                     fxEnvelopesFromClip(s, clip.kind, startBeat, 0, clip.length, length),
                     passes ? takeEnvelopesFromClip(s, clip.kind, startBeat, 0, clip.length, length)
                            : FxEnvelopeMap{});
    if (!copied) return std::move(copied).error();
    item.takes = *copied;
    for (std::size_t k = 0; k < clip.takes.size(); ++k)
        if (clip.activeTake == clip.takes[k]) item.activeTake = item.takes[k];
    nameItemFromClip(t, clip, item);
    return std::vector<Id>{t.create(std::move(item))};
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

// A place in the project to read the context from (clip.add, clip.set): a bar, or beats.
struct ContextAtArg {
    std::optional<std::int64_t> bar;
    std::optional<double> at;
};
struct ClipAddArgs {
    std::string track, scene;
    std::optional<std::string> notation, audioFile, midiFile, item, name;
    bool pooled = false;
    std::optional<std::string> context;
    std::optional<ContextAtArg> contextAt;
    std::optional<model::ClipFollow> follow;
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
    std::optional<double> gainDb, length, fadeIn, fadeOut;
    std::optional<model::FadeShape> fadeInShape, fadeOutShape;
    std::optional<LoopArg> loop;
    std::optional<model::ClipMode> mode;
    std::optional<json::Value> quantize;  // null: the project's
    std::optional<std::string> context;
    std::optional<ContextAtArg> contextAt;
    std::optional<model::ClipFollow> follow;
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

schema::Schema<model::ClipFollow> clipFollowArgSchema() {
    return schema::enumeration<model::ClipFollow>({{model::ClipFollow::None, "none"},
                                                   {model::ClipFollow::Key, "key"},
                                                   {model::ClipFollow::Chords, "chords"}})
        .doc(
            "How the clip's notes are mapped when it is laid on the timeline (ADR-0098): "
            "none (default), key or chords. Needs a MIDI clip with a key at its start.");
}

schema::Schema<ContextAtArg> contextAtSchema() {
    return schema::object<ContextAtArg>("context_at")
        .field("bar", &ContextAtArg::bar, schema::integer().range(1, 1000000))
        .field("at", &ContextAtArg::at, schema::number().min(0))
        .build()
        .doc(
            "A place in the project, by bar or in beats: the clip takes the key and chords "
            "holding there, and the changes up to its length after it.");
}

schema::Schema<ClipAddArgs> clipAddArgsSchema() {
    return schema::object<ClipAddArgs>("clip_add_args")
        .field("track", &ClipAddArgs::track, idArg("tr", "Track"))
        .field("scene", &ClipAddArgs::scene, idArg("sc", "Scene"))
        .field("notation", &ClipAddArgs::notation, schema::string())
        .field("audio_file", &ClipAddArgs::audioFile, schema::string())
        .field("midi_file", &ClipAddArgs::midiFile, schema::string())
        .field("item", &ClipAddArgs::item, idArg("it", "Item"))
        .field("pooled", &ClipAddArgs::pooled, schema::boolean())
        .defaultValue(false)
        .field("name", &ClipAddArgs::name, schema::string())
        .field("context", &ClipAddArgs::context,
               schema::string().doc("The key and chords the clip was written against, as a lead "
                                    "sheet in clip bars (bar 1 is the clip's start): "
                                    "\"1: key D minor {Dm}:w\". \"\" is none."))
        .field("context_at", &ClipAddArgs::contextAt, contextAtSchema())
        .field("follow", &ClipAddArgs::follow, clipFollowArgSchema())
        .build();
}

schema::Schema<ClipSetArgs> clipSetArgsSchema() {
    auto loop = schema::object<LoopArg>("clip_loop_arg")
                    .field("on", &LoopArg::on, schema::boolean())
                    .field("start", &LoopArg::start, schema::number().min(0))
                    .field("length", &LoopArg::length, schema::number())
                    .build();
    return schema::object<ClipSetArgs>("clip_set_args")
        .field("clip", &ClipSetArgs::clip, idArg("cl", "Clip"))
        .field("name", &ClipSetArgs::name, schema::string())
        .field("color", &ClipSetArgs::color, schema::string())
        .field("mute", &ClipSetArgs::mute, schema::boolean())
        .field("gain_db", &ClipSetArgs::gainDb, schema::number().range(model::kSilenceDb, 24))
        .field("fade_in", &ClipSetArgs::fadeIn,
               schema::number().min(0).doc("Seconds of take time, from each pass's start."))
        .field("fade_in_shape", &ClipSetArgs::fadeInShape, fadeShapeSchema())
        .field("fade_out", &ClipSetArgs::fadeOut,
               schema::number().min(0).doc("Seconds of take time, to each pass's end."))
        .field("fade_out_shape", &ClipSetArgs::fadeOutShape, fadeShapeSchema())
        .field("length", &ClipSetArgs::length, schema::number())
        .field("loop", &ClipSetArgs::loop, loop)
        .field("mode", &ClipSetArgs::mode, clipModeSchema())
        .field("quantize", &ClipSetArgs::quantize,
               clearable("clearable_launch_quantize", model::launchQuantizeSchema()))
        .field("context", &ClipSetArgs::context,
               schema::string().doc("As clip.add's: the lead sheet in clip bars; \"\" clears the "
                                    "context."))
        .field("context_at", &ClipSetArgs::contextAt, contextAtSchema())
        .field("follow", &ClipSetArgs::follow, clipFollowArgSchema())
        .build();
}

// The beat a context_at names: a bar of the project, or beats; exactly one.
Result<double> contextPlace(const model::Session& s, const ContextAtArg& at,
                            const std::string& path) {
    if (at.bar.has_value() == at.at.has_value())
        return Error("clip.context_args", "context_at takes bar or at, one of them", path,
                     "e.g. {\"bar\": 9}, or {\"at\": 32} in beats");
    return at.bar ? s.timeMap->beatOfBar(static_cast<int>(*at.bar)) : *at.at;
}

// The context a command states for a clip of `kind` and `length`: its `context` text, the
// project's context under `at`, or (clip.add {item}) under the item. `text` is what
// history records for the resolved arguments. Nothing stated: nullopt.
struct Stated {
    std::optional<std::vector<model::ContextPoint>> points;
    std::string text;
};
Result<Stated> statedContext(Context& ctx, const model::Session& s, model::ClipKind kind,
                             double length, const std::optional<std::string>& text,
                             const std::optional<ContextAtArg>& at,
                             std::optional<std::pair<double, double>> underItem) {
    if (text && at)
        return Error("clip.context_args", "give context or context_at, not both", "/context_at",
                     "context is the lead sheet text; context_at reads the project's context");
    Stated out;
    const bool audio = kind == model::ClipKind::Audio;
    if (text) {
        auto points = detail::clipContextFromText(s, *text, kind, length, "/context");
        if (!points) return std::move(points).error();
        out.points = std::move(*points);
    } else if (at || underItem) {
        double from = 0, span = length;
        if (at) {
            auto place = contextPlace(s, *at, "/context_at");
            if (!place) return std::move(place).error();
            from = *place;
        } else {
            from = underItem->first;
            span = underItem->second;
        }
        auto taken = detail::contextUnder(s, from, span, audio);
        for (const auto& n : detail::unreadableNotes(taken, at ? "/context_at" : "/item"))
            ctx.warn(n.code, n.message, n.path);
        if (at && !taken.key)
            ctx.warn("context.no_key",
                     "no key holds at " + std::string("bar ") +
                         std::to_string(s.timeMap->barBeat(from).bar) +
                         ": the clip has no key and cannot follow",
                     "/context_at");
        out.points = std::move(taken.points);
    } else {
        return out;
    }
    model::Clip shape;  // the text history records is of the points as the clip keeps them
    shape.kind = kind;
    shape.length = length;
    shape.context = *out.points;
    model::fitClipContext(shape);
    out.text = detail::clipContextText(s, shape);
    return out;
}

void warnNotes(Context& ctx, const std::vector<detail::ClipNote>& notes) {
    for (const auto& n : notes) ctx.warn(n.code, n.message, n.path);
}
void warnFit(Context& ctx, const std::vector<model::ClipContextNote>& notes) {
    for (const auto& n : notes)
        ctx.warn(n.code, n.message, n.code == "clip.follow_reset" ? "/follow" : "/context");
}

// `follow` as given for a clip with this context: the refusals of clip.add and clip.set.
Status checkFollow(const model::Clip& clip, model::ClipFollow follow) {
    if (follow == model::ClipFollow::None) return {};
    if (clip.kind == model::ClipKind::Audio)
        return Error("clip.follow_audio", "an audio clip cannot follow the context", "/follow",
                     "follow applies to MIDI clips; leave it out for audio");
    if (!model::keyAt(clip, 0))
        return Error("clip.no_key", "follow needs a key at the clip's start", "/follow",
                     "give the clip a context with a key at its start: clip.set {context: \"1: key "
                     "D minor ~:w\"} or {context_at}");
    return {};
}

auto sceneIdArg() { return idArg("sc", "Scene"); }
auto clipIdArg() { return idArg("cl", "Clip"); }

void registerSceneCommands(cmd::Registry& registry) {
    [&] {
        Command<SceneAddArgs, IdResult> c;
        c.name = "scene.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpExec;
        c.summary = "Add a scene (launcher column) at the end or an index.";
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
        c.summary = "List scenes in column order with clip counts.";
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
        c.summary = "Add a clip to a scene's cell: from notation, a file or an item.";
        c.description =
            "One source at most: notation (as item.add's), midi_file, audio_file, or item (a "
            "copy of that item's takes; pooled shares its MIDI instead). Without one, an empty "
            "MIDI clip of one bar. A MIDI clip is measured in beats, as long as its notes "
            "rounded up to whole bars; an audio clip in seconds of take time, as long as the "
            "file. From an item, the length, loop and take envelopes convert at the tempo where "
            "the item sits (ADR-0097); the clip is named as the item is (its active take's name) "
            "unless name is given. Clips live on channels, one per (track, scene). A clip keeps "
            "the key and chords it was written against (context): as a lead sheet in clip bars "
            "(\"1: key D minor {Dm}:w\"), read from the project at a place (context_at), under "
            "the item it copies, or from the {SYMBOL} chords of its notation (a stated context "
            "holding chords wins over them; clip.chords_ignored warns when they differ from it). "
            "follow (none, key or chords) "
            "needs a MIDI clip with a key at its start (clip.no_key, clip.follow_audio); laying "
            "the clip (item.add {clip}, scene.insert, arrangement.build) lays its notes as "
            "written, the take carrying the clip's context and the follow (their follow "
            "argument), so they sound as the key or chords under them say; a launched clip "
            "follows them live (ADR-0098).";
        c.tags = {"clip launcher", "clip"};
        c.args = clipAddArgsSchema();
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
                    {"clip.limit", "the project has 16384 clips", "remove clips first"},
                    {"clip.context_args", "context and context_at together",
                     "give one: the text, or a place to read"},
                    {"clip.context_syntax", "the context text does not read",
                     "write a lead sheet in clip bars: 1: key D minor {Dm}:w"},
                    {"clip.context_range", "a context point at or past the clip's end",
                     "move it earlier or lengthen the clip"},
                    {"clip.context_audio", "a chord, or a key after the start, on an audio clip",
                     "an audio clip's context is a key at the start of bar 1"},
                    {"clip.no_key", "follow without a key at the clip's start",
                     "give the clip a context with a key at its start"},
                    {"clip.follow_audio", "follow on an audio clip", "follow is for MIDI clips"}};
        c.examples = {
            {"A chorus bass line",
             j(R"({"track": "tr_1", "scene": "sc_2", "notation": "voice bass; 1: C2:h G2:h"})"),
             j(R"({"clip": "cl_2", "takes": ["tk_3"], "source": "mi_3", "kind": "midi", "length": 4.0})"),
             "song_with_clip"},
            {"A bass line written in D minor, following the key when laid",
             j(R"({"track": "tr_1", "scene": "sc_2", "notation": "voice bass; 1: D2:h A1:h", "context": "1: key D minor {Dm}:w", "follow": "key"})"),
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
            std::vector<notation::ChordSymbol> symbolsOf;        // the notation's {SYMBOL} chords
            std::optional<std::pair<double, double>> underItem;  // where an item's context is
            std::optional<std::vector<model::ContextPoint>> fromTake;  // a following take's
            model::ClipFollow itemFollow = model::ClipFollow::None;    // and its follow
            auto single = [&](model::Take take) {
                const Id tk = t.create(std::move(take));
                clip.takes = {tk};
                clip.activeTake = tk;
            };
            if (a.item) {
                auto item = existing<model::Item>(s, *a.item, "/item");
                if (!item) return std::move(item).error();
                bool cut = false;
                auto made = clipFromItem(t, s, *item, a.pooled, "/item", &cut);
                if (!made) return std::move(made).error();
                if (cut)
                    ctx.warn("clip.context_dropped",
                             "the item's context is longer than a clip holds: the clip has the "
                             "first points of it",
                             "/item");
                made->track = clip.track;
                made->scene = clip.scene;
                if (a.name) made->name = *a.name;  // else the item's
                clip = std::move(*made);
                const model::Item& it = *s.items.find(*item);
                // An item whose take follows keeps that take's context (clipFromItem); any
                // other, the project's under it.
                if (clip.follow != model::ClipFollow::None) {
                    fromTake = clip.context;
                    itemFollow = clip.follow;
                } else {
                    underItem = {model::itemSpan(s, it).startBeat, model::itemLengthBeats(s, it)};
                }
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
                    auto compiled = compileNotationFull(s, *a.notation, 0);
                    if (!compiled) return std::move(compiled).error();
                    source = std::move(compiled->source);
                    beats = compiled->beats;
                    symbolsOf = std::move(compiled->chords);
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
            // The context the clip was written against (ADR-0098).
            auto stated =
                statedContext(ctx, s, clip.kind, clip.length, a.context, a.contextAt, underItem);
            if (!stated) return std::move(stated).error();
            if (fromTake && !stated->points) stated->points = std::move(fromTake);
            std::vector<detail::ClipNote> notes;
            detail::SymbolChords symbols;
            if (!symbolsOf.empty())
                symbols =
                    detail::chordsFromSymbols(s, symbolsOf, s.project->ppq, 0.0, 1.0, clip.length);
            clip.context =
                detail::composeContext(std::move(stated->points), std::move(symbols), notes);
            // A following take's follow is the default only for the context it brought.
            const model::ClipFollow wanted =
                a.follow.value_or(a.context || a.contextAt ? model::ClipFollow::None : itemFollow);
            SUPPLE_TRY(checkFollow(clip, wanted));
            clip.follow = wanted;
            warnNotes(ctx, notes);
            warnFit(ctx, model::fitClipContext(clip));
            // History replays the context the user saw, not the place it was read from:
            // only context_at, which states a place for the context alone, records the text.
            // An item is taken by reference and re-read on replay, notes and context together.
            if (a.contextAt) {
                ClipAddArgs resolved = a;
                resolved.contextAt.reset();
                resolved.context = stated->text;
                ctx.setResolvedArgs(clipAddArgsSchema().encode(resolved));
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
        c.summary = "Set a clip's name, colour, mute, gain, fades, length, loop, mode or quantize.";
        c.description =
            "length and loop are in the clip's unit: beats for MIDI, seconds of take time for "
            "audio. loop: {on, start?, length?}, the window that repeats inside the clip from "
            "the take's start. mode loop repeats every length; once plays it once. quantize "
            "(bar, beat, 1/2 … 1/32, none) overrides the project's launch_quantize; null "
            "returns to it. context (a lead sheet in clip bars; \"\" clears it) or context_at "
            "(read from the project at a bar or beat) sets the key and chords the clip was "
            "written against, and follow (none, key, chords) how it is mapped when laid; "
            "shortening a clip drops the points past its end (clip.context_dropped), and a "
            "context left with no key at the start resets follow to none (clip.follow_reset). "
            "fade_in and fade_out (audio clips) shape each pass of the period, as an item's "
            "fades shape the item: in from the pass's start, out to its end.";
        c.tags = {"clip launcher", "clip"};
        c.args = clipSetArgsSchema();
        c.result = emptySchema();
        c.errors = {{"clip.length", "a length or loop length not above 0",
                     "give a positive length in the clip's unit"},
                    {"clip.context_args", "context and context_at together",
                     "give one: the text, or a place to read"},
                    {"clip.context_syntax", "the context text does not read",
                     "write a lead sheet in clip bars: 1: key D minor {Dm}:w"},
                    {"clip.context_range", "a context point at or past the clip's end",
                     "move it earlier or lengthen the clip"},
                    {"clip.context_audio", "a chord, or a key after the start, on an audio clip",
                     "an audio clip's context is a key at the start of bar 1"},
                    {"clip.no_key", "follow without a key at the clip's start",
                     "give the clip a context with a key at its start"},
                    {"clip.follow_audio", "follow on an audio clip", "follow is for MIDI clips"},
                    {"clip.unsupported", "a fade on a MIDI clip",
                     "MIDI clips play no fades, as MIDI items play none"}};
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
            if (clip.kind == model::ClipKind::Midi)
                for (const auto& [given, name] :
                     {std::pair{a.fadeIn.has_value() || a.fadeInShape.has_value(), "fade_in"},
                      std::pair{a.fadeOut.has_value() || a.fadeOutShape.has_value(), "fade_out"}})
                    if (given)
                        return Error("clip.unsupported", "a MIDI clip has no fades",
                                     std::string("/") + name,
                                     "MIDI clips play no fades, as MIDI items play none: fade "
                                     "an audio clip");
            if (a.fadeIn) clip.fadeIn.length = *a.fadeIn;
            if (a.fadeOut) clip.fadeOut.length = *a.fadeOut;
            if (a.fadeInShape) clip.fadeIn.shape = *a.fadeInShape;
            if (a.fadeOutShape) clip.fadeOut.shape = *a.fadeOutShape;
            if (a.mode) clip.mode = *a.mode;
            // The clip's context and follow (ADR-0098): stated against the clip as it will be.
            auto stated =
                statedContext(ctx, s, clip.kind, clip.length, a.context, a.contextAt, std::nullopt);
            if (!stated) return std::move(stated).error();
            if (stated->points) clip.context = std::move(*stated->points);
            if (a.follow) {
                SUPPLE_TRY(checkFollow(clip, *a.follow));
                clip.follow = *a.follow;
            }
            warnFit(ctx, model::fitClipContext(clip));
            if (a.contextAt) {
                ClipSetArgs resolved = a;
                resolved.contextAt.reset();
                resolved.context = stated->text;
                ctx.setResolvedArgs(clipSetArgsSchema().encode(resolved));
            }
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
        c.summary = "Move a clip to an empty cell in another scene, channel or both.";
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
            const auto key = detail::clipKeyWords(clip);
            std::string line = "    " + tr.str() + (track.name.empty() ? "" : " " + track.name) +
                               ": " + clipLabel(s, *cl) + (key ? " in " + *key : "") + " ";
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
            if (clip.gainDb != 0.0) line += " · " + number(clip.gainDb) + " dB";
            if (clip.fadeIn.length > 0) line += " · fade in " + number(clip.fadeIn.length) + " s";
            if (clip.fadeOut.length > 0)
                line += " · fade out " + number(clip.fadeOut.length) + " s";
            if (clip.mute) line += " · muted";
            if (clip.follow != model::ClipFollow::None)
                line += clip.follow == model::ClipFollow::Key ? " · follows the key"
                                                              : " · follows the chords";
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
