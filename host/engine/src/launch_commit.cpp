// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// launch.commit's layout (ADR-0097 L4, engine.md §1.4.2). A launch at
// running frame f of a capture that began at song sample S lands at
// timeline sample S + f, whatever the transport loop did (L-f). What each
// clip played there is laid out as it was heard: its phase is the song
// position's (L-c), so a loop wrap restarts a piece at the phase the wrap
// gave it. The arrangement under a track's clip, stopped and held
// stretches is cleared, and the 5 ms switch crossfades become item fades.
#include "launch_commit.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include "clip_context.h"
#include "clip_launcher.h"
#include "command_validation.h"
#include "plan.h"
#include "supple/time/timeline.h"

namespace supple::engine {

namespace {

constexpr double kSampleLimit = 140737488355328.0;  // 2^47

// One stretch of a track: from a launch to the next, or the capture's end.
// `e` null: the arrangement.
struct Stretch {
    std::int64_t from = 0, to = 0;
    const CapturedLaunch* e = nullptr;
};

bool isAudioItem(const model::Session& s, const model::Item& item) {
    const model::Take* take = item.activeTake ? s.takes.find(*item.activeTake) : nullptr;
    return take && take->source && take->source->kind == model::Kind::Media;
}

double positiveMod(double x, double m) { return x - std::floor(x / m) * m; }

std::int64_t positiveMod(std::int64_t x, std::int64_t m) { return ((x % m) + m) % m; }

Result<json::Value> invokeNested(Context& ctx, const char* name, json::Value args) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
}

}  // namespace

schema::Schema<CapturedPerformance> capturedPerformanceSchema() {
    const auto sample = schema::integer().range(0, kSampleLimit);
    auto launch =
        schema::object<CapturedLaunch>("captured_launch")
            .field("frame", &CapturedLaunch::frame, sample)
            .field("track", &CapturedLaunch::track, idArg("tr", "Track"))
            .field("action", &CapturedLaunch::action,
                   schema::enumeration<std::string>(
                       {{"clip", "clip"}, {"arrangement", "arrangement"}, {"stop", "stop"}}))
            .field("clip", &CapturedLaunch::clip, idArg("cl", "Clip"))
            .field("anchor", &CapturedLaunch::anchor, sample)
            .field("anchor_beat", &CapturedLaunch::anchorBeat, schema::number().min(0))
            .build();
    auto loop = schema::object<CapturedLoop>("captured_loop")
                    .field("start", &CapturedLoop::start, sample)
                    .field("end", &CapturedLoop::end, sample)
                    .build();
    return schema::object<CapturedPerformance>("captured_performance")
        .field("sample_rate", &CapturedPerformance::sampleRate,
               schema::number().exclusiveMin(0).max(1536000))
        .field("start", &CapturedPerformance::start, sample)
        .field("frames", &CapturedPerformance::frames, sample)
        .field("loop", &CapturedPerformance::loop, loop)
        .field("launches", &CapturedPerformance::launches, schema::arrayOf(launch))
        .build();
}

Result<CommitDone> commitPerformance(Context& ctx, const CapturedPerformance& perf, bool pooled) {
    EngineApp& app = engine::app(ctx);
    const std::string at = "/performance";
    if (perf.frames <= 0)
        return Error("launch.capture", "the capture holds no running time", at + "/frames",
                     "launch.capture {on: true}, play and launch clips, then launch.commit");
    const double rate = perf.sampleRate;
    const auto map = app.view().timeMap;  // commits change no tempo
    const time::TimeMap& tm = *map;
    const std::int64_t F =
        std::max<std::int64_t>(1, time::samplesAt(detail::kLaunchFadeSeconds, rate));

    // The song position at running frame f, the loop folding (L-c), and
    // the first wrap after f.
    const bool looping =
        perf.loop && perf.loop->end > perf.loop->start && perf.start < perf.loop->end;
    const std::int64_t loopStart = looping ? perf.loop->start : 0;
    const std::int64_t loopLen = looping ? perf.loop->end - perf.loop->start : 0;
    const std::int64_t firstWrap = looping ? perf.loop->end - perf.start : INT64_MAX;
    const auto song = [&](std::int64_t f) {
        const std::int64_t x = perf.start + f;
        return looping && f >= firstWrap ? loopStart + (f - firstWrap) % loopLen : x;
    };
    const auto nextWrap = [&](std::int64_t f) -> std::int64_t {
        if (!looping) return INT64_MAX;
        if (f < firstWrap) return firstWrap;
        return firstWrap + ((f - firstWrap) / loopLen + 1) * loopLen;
    };
    const auto beatAt = [&](double sample) { return tm.beatAt(sample / rate); };
    // Where content heard at song beat `songBeat`, frame f, lands: the
    // song's own beat until the first wrap (exact), then the timeline's.
    const auto timelineBeat = [&](std::int64_t f, double songBeat, double sinceFrameBeat) {
        return f < firstWrap ? songBeat : beatAt(double(perf.start + f)) + sinceFrameBeat;
    };

    // The launches by track, checked against the session.
    std::map<Id, std::vector<const CapturedLaunch*>> byTrack;
    std::vector<Id> order;
    {
        const model::Session s = app.view();
        for (std::size_t i = 0; i < perf.launches.size(); ++i) {
            const CapturedLaunch& e = perf.launches[i];
            const std::string path = at + "/launches/" + std::to_string(i);
            const auto stale = [&](const std::string& what, const std::string& field) {
                return Error("launch.commit", what, path + field,
                             "the project changed since the performance was captured; undo "
                             "the change, or capture it again");
            };
            if (e.frame >= perf.frames)
                return Error("launch.commit", "a launch after the capture's end", path + "/frame",
                             "frames run from 0 to the capture's frames");
            const auto track = Id::parse(e.track);
            const model::Track* tr = track ? s.tracks.find(*track) : nullptr;
            if (!tr) return stale(e.track + " no longer exists", "/track");
            if (tr->type != model::StripType::Channel)
                return stale(e.track + " is no longer a channel", "/track");
            if (e.action == "clip") {
                const auto clip = e.clip ? Id::parse(*e.clip) : std::nullopt;
                const model::Clip* c = clip ? s.clips.find(*clip) : nullptr;
                if (!c) return stale(e.clip.value_or("its clip") + " no longer exists", "/clip");
                if (c->track != *track)
                    return stale(*e.clip + " is no longer " + e.track + "'s", "/clip");
                if (!c->activeTake || !s.takes.find(*c->activeTake))
                    return stale(*e.clip + " has no take to lay down", "/clip");
            }
            if (!byTrack.count(*track)) order.push_back(*track);
            byTrack[*track].push_back(&e);
        }
    }
    std::sort(order.begin(), order.end());

    CommitDone done;
    done.from = beatAt(double(perf.start));
    done.to = beatAt(double(perf.start + perf.frames));
    model::EditTxn& t = app.txn();

    // A section for each scene launched whole: at one running frame the
    // launches that hold (the last of a track's at a frame) start every clip
    // of the scene that could play (a frozen track's cannot). It runs until
    // one of those tracks is launched again, else the capture's end. A clip
    // launched alone, or some of a scene's, writes none. Its edges are where
    // the items land: the launch's own beat where it is exact, else the
    // timeline's position of the running frame.
    {
        const model::Session s = app.view();
        // Where running frame `frame` lands on the timeline, as the items do
        // (timelineBeat): the launch's exact beat before the first wrap, the
        // timeline's own position past it, never the folded song position.
        const auto beatOfEntry = [&](std::int64_t frame, const CapturedLaunch& e) {
            return frame < firstWrap && perf.start + frame == e.anchor
                       ? e.anchorBeat
                       : beatAt(double(perf.start + frame));
        };
        std::map<std::int64_t, std::vector<const CapturedLaunch*>> holding;  // by frame
        for (const auto& [track, launches] : byTrack) {
            std::map<std::int64_t, const CapturedLaunch*> last;  // the last at each frame
            for (const CapturedLaunch* e : launches)
                last[e->frame] = e;  // of one frame's launches the last given holds
            for (const auto& [frame, e] : last)
                if (e->action == "clip") holding[frame].push_back(e);
        }
        for (const auto& [frame, launched] : holding) {
            std::map<Id, std::vector<const CapturedLaunch*>> byScene;
            for (const CapturedLaunch* e : launched)
                byScene[s.clips.find(*Id::parse(*e->clip))->scene].push_back(e);
            for (const auto& [sceneId, started] : byScene) {
                const model::Scene* scene = s.scenes.find(sceneId);
                if (!scene) continue;
                std::size_t playable = 0;
                for (const auto& c : s.clips)
                    if (c.value->scene == sceneId) {
                        const model::Track* tr = s.tracks.find(c.value->track);
                        if (tr && !tr->freeze) ++playable;
                    }
                if (playable == 0 || started.size() != playable) continue;
                std::int64_t until = perf.frames;
                const CapturedLaunch* next = nullptr;
                for (const CapturedLaunch* e : started) {
                    for (const CapturedLaunch* later : byTrack[*Id::parse(e->track)])
                        if (later->frame > frame && later->frame < until) {
                            until = later->frame;
                            next = later;
                        }
                }
                model::Region region;
                region.start = beatOfEntry(frame, *started.front());
                region.end = next ? beatOfEntry(until, *next) : done.to;
                if (!(region.end > region.start)) continue;
                region.name = scene->name;
                region.color = scene->color;
                region.section = true;
                CommittedSection made{sceneId.str(), {}, region.start, region.end};
                // Committing again writes the same performance over itself: a section
                // already there, the same name over the same span, is kept, not doubled.
                const auto same = [&](const model::Region& r) {
                    return r.section && r.timebase == region.timebase && r.name == region.name &&
                           std::abs(r.start - region.start) <= 1e-9 &&
                           std::abs(r.end - region.end) <= 1e-9;
                };
                const model::Session now = t.preview();
                const auto twin = std::find_if(now.regions.begin(), now.regions.end(),
                                               [&](const auto& e) { return same(*e.value); });
                if (twin != now.regions.end())
                    made.region = twin->id.str();
                else
                    made.region = t.create(std::move(region)).str();
                done.sections.push_back(std::move(made));
            }
        }
    }

    for (const Id track : order) {
        auto& launches = byTrack[track];
        std::stable_sort(
            launches.begin(), launches.end(),
            [](const CapturedLaunch* a, const CapturedLaunch* b) { return a->frame < b->frame; });
        // The stretches, the arrangement first unless a launch is at 0; of
        // several launches at one frame the last holds.
        std::vector<Stretch> stretches;
        std::int64_t from = 0;
        const CapturedLaunch* state = nullptr;
        for (const CapturedLaunch* e : launches) {
            if (e->frame > from) stretches.push_back({from, e->frame, state});
            from = e->frame;
            state = e->action == "arrangement" ? nullptr : e;
        }
        stretches.push_back({from, perf.frames, state});
        if (std::none_of(stretches.begin(), stretches.end(),
                         [](const Stretch& x) { return x.e != nullptr; }))
            continue;
        done.tracks.push_back(track.str());

        // A once clip falls silent at the first wrap after its launch (held).
        const auto heldBy = [&](const Stretch& x, std::int64_t f) {
            if (!x.e || x.e->action != "clip") return false;
            const model::Clip* c = app.view().clips.find(*Id::parse(*x.e->clip));
            return c && c->mode == model::ClipMode::Once && nextWrap(x.from) <= f;
        };
        // Clears the arrangement under [a, b), each item cut by its kind:
        // audio from a + F (the arrangement fades out over F), with a fade
        // out at a and in at b where the switch crossfades; MIDI on [a, b).
        const auto clear = [&](std::int64_t a, std::int64_t b, bool fadeOut,
                               bool fadeIn) -> Status {
            std::vector<Id> items;
            for (const auto& e : app.view().items)
                if (e.value->track == track) items.push_back(e.id);
            for (const Id id : items) {
                const model::Session s = app.view();
                const model::Item* item = s.items.find(id);
                if (!item) continue;
                const bool audio = isAudioItem(s, *item);
                const double c0 = double(perf.start + a + (audio && fadeOut ? F : 0));
                const double c1 = double(perf.start + b);
                if (!(c1 > c0)) continue;
                const model::ItemSpan span = model::itemSpan(s, *item);
                const double is = span.startSec * rate, ie = span.endSec * rate;
                constexpr double kHalf = 0.5;
                if (ie <= c0 + kHalf || is >= c1 - kHalf) continue;
                const auto split = [&](Id it, double sample) -> Result<std::pair<Id, Id>> {
                    json::Value args = json::Value::object();
                    args["item"] = it.str();
                    args["at"] = beatAt(sample);
                    args["bypass_groups"] = true;
                    auto r = invokeNested(ctx, "item.split", args);
                    if (!r) return std::move(r).error();
                    return std::pair{*Id::parse(r->find("left")->asString()),
                                     *Id::parse(r->find("right")->asString())};
                };
                const auto remove = [&](Id it) -> Status {
                    const model::Session now = app.view();
                    SUPPLE_TRY(checkUnlocked(now, it));
                    for (Id tk : now.items.find(it)->takes)
                        SUPPLE_TRY(removeTakeObjects(t, now, tk));
                    t.remove<model::Item>(it);
                    return {};
                };
                const auto fade = [&](Id it, bool in) {
                    if (!audio || !(in ? fadeIn : fadeOut)) return;
                    t.modify<model::Item>(it, [&](model::Item& x) {
                        (in ? x.fadeIn : x.fadeOut) =
                            model::Fade{double(F) / rate, model::FadeShape::Linear, 0.0};
                    });
                };
                const bool before = is<c0 - kHalf, after = ie> c1 + kHalf;
                if (!before && !after) {
                    SUPPLE_TRY(remove(id));
                } else if (before && after) {
                    auto right = split(id, c1);
                    if (!right) return std::move(right).error();
                    auto middle = split(id, c0);
                    if (!middle) return std::move(middle).error();
                    SUPPLE_TRY(remove(middle->second));
                    fade(id, false);
                    fade(right->second, true);
                } else if (before) {
                    auto parts = split(id, c0);
                    if (!parts) return std::move(parts).error();
                    SUPPLE_TRY(remove(parts->second));
                    fade(id, false);
                } else {
                    auto parts = split(id, c1);
                    if (!parts) return std::move(parts).error();
                    SUPPLE_TRY(remove(parts->first));
                    fade(parts->second, true);
                }
            }
            return {};
        };
        // Each run of stretches away from the arrangement clears it.
        for (std::size_t i = 0; i < stretches.size();) {
            if (!stretches[i].e) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j + 1 < stretches.size() && stretches[j + 1].e) ++j;
            const Stretch& last = stretches[j];
            const bool fadeOut = stretches[i].from > 0;  // left the arrangement there
            const bool fadeIn =
                last.to < perf.frames && last.e->action == "clip" && !heldBy(last, last.to);
            SUPPLE_TRY(clear(stretches[i].from, last.to, fadeOut, fadeIn));
            i = j + 1;
        }

        // Each clip stretch, as copies of the clip's takes.
        for (std::size_t i = 0; i < stretches.size(); ++i) {
            const Stretch& x = stretches[i];
            if (!x.e || x.e->action != "clip") continue;
            const model::Session s = app.view();
            const Id clipId = *Id::parse(*x.e->clip);
            const model::Clip& clip = *s.clips.find(clipId);
            const model::Take& take = *s.takes.find(*clip.activeTake);
            const bool midi = clip.kind == model::ClipKind::Midi;
            const bool once = clip.mode == model::ClipMode::Once;
            const double P = clip.length;
            if (!(P > 0)) continue;
            const double content = contentLength(s, take);
            const bool wholeLoop = clip.loop.on && std::abs(clip.loop.start) <= 1e-9 * P &&
                                   std::abs(clip.loop.length - content) <= 1e-9 * std::max(1.0, P);
            // One looped item for the whole stretch when the clip's period is
            // its content's pass; else one item per period.
            const bool single = !once && take.startOffset == 0 &&
                                std::abs(P - content) <= 1e-9 * std::max(1.0, P) &&
                                (!clip.loop.on || wholeLoop);
            const Stretch* prev = i ? &stretches[i - 1] : nullptr;
            // The switch crossfades in from the arrangement or a clip.
            const bool fadeIn =
                prev && !(prev->e && prev->e->action == "stop") && !(prev && heldBy(*prev, x.from));
            const bool fadeOut = x.to < perf.frames && !heldBy(x, x.to);
            const double speed = take.playrate > 0 ? take.playrate : 1.0;
            const std::string path = at + "/launches";

            // One item: running frames [f0, f1), its content starting `phase`
            // into the period, at timeline beat `beat` (MIDI) or sample.
            const auto lay = [&](std::int64_t f0, std::int64_t f1, double beat0, double beat1,
                                 double phase, bool loopItem, bool firstOfStretch,
                                 bool lastOfStretch) -> Status {
                const model::Session now = app.view();
                const double pos = midi ? beat0 : beatAt(double(perf.start + f0));
                auto shaped = itemFromClip(now, clip, take, track, pos, path);
                if (!shaped) return std::move(shaped).error();
                model::Item item = std::move(shaped->item);
                std::function<double(double)> envelopePos;
                std::int64_t end = perf.start + f1;
                if (midi) {
                    model::placeItem(now, item, beat0, beat1 - beat0);
                    if (phase != 0) envelopePos = [phase](double b) { return b - phase; };
                } else {
                    if (lastOfStretch && fadeOut) end += F;
                    const double startSec = double(perf.start + f0) / rate;
                    model::placeItemSeconds(now, item, startSec, double(end) / rate);
                    const double phaseSec = phase / rate;
                    envelopePos = [map, startSec, pos, phaseSec](double secs) {
                        return map->beatAt(startSec + secs - phaseSec) - pos;
                    };
                    if (firstOfStretch && fadeIn)
                        item.fadeIn = model::Fade{double(F) / rate, model::FadeShape::Linear, 0.0};
                    if (lastOfStretch && fadeOut)
                        item.fadeOut = model::Fade{double(F) / rate, model::FadeShape::Linear, 0.0};
                }
                item.loop = loopItem || item.loop;
                // Its take FX's envelopes, in clip time, onto the timeline at
                // the phase heard, pass after pass over the item (C2).
                const double period = midi ? P : double(time::samplesAt(P, rate)) / rate;
                const double span = midi ? beat1 - beat0 : double(end - (perf.start + f0)) / rate;
                auto copied =
                    copyTakesFor(t, now, clip.takes, pooled, envelopePos, path,
                                 fxEnvelopesFromClip(now, clip.kind, pos,
                                                     midi ? phase : phase / rate, period, span));
                if (!copied) return std::move(copied).error();
                const double shift = midi ? phase * speed : phase / rate * speed;
                if (shift != 0)
                    for (const Id tk : *copied)
                        t.modify<model::Take>(tk, [&](model::Take& k) { k.startOffset += shift; });
                item.takes = *copied;
                for (std::size_t k = 0; k < clip.takes.size(); ++k)
                    if (clip.activeTake == clip.takes[k]) item.activeTake = item.takes[k];
                nameItemFromClip(t, clip, item);
                // The notes are copied as they were played (a launched clip is not followed
                // until X4): say when the clip's key is not the project's under the item.
                if (midi)
                    if (auto note = detail::mismatchNote(now, clipId, clip, beat0, beat1 - beat0,
                                                         phase, path))
                        ctx.warn(note->code, note->message, note->path);
                done.items.push_back(t.create(std::move(item)).str());
                return {};
            };

            // The stretch in spans of continuous song time, split at wraps.
            for (std::int64_t g = x.from; g < x.to;) {
                const std::int64_t h = std::min(x.to, nextWrap(g));
                const std::int64_t Sg = song(g);
                if (midi) {
                    const double Bg = Sg == x.e->anchor ? x.e->anchorBeat : beatAt(double(Sg));
                    const double q = Bg - x.e->anchorBeat;
                    // The span's end: the next launch's own beat where it is exact.
                    double Bh = beatAt(double(Sg + (h - g)));
                    if (h == x.to && i + 1 < stretches.size() && stretches[i + 1].e &&
                        stretches[i + 1].e->anchor == Sg + (h - g))
                        Bh = stretches[i + 1].e->anchorBeat;
                    double phase = positiveMod(q, P);
                    if (P - phase <= 1e-9) phase = 0;
                    double limit = Bh;
                    if (once) {
                        if (q < -1e-9 || q >= P - 1e-9) break;  // before or past its pass
                        limit = std::min(limit, x.e->anchorBeat + P);
                    }
                    // MIDI items take beats; their frames only matter for audio.
                    const double tb0 = timelineBeat(g, Bg, 0);
                    if (single) {
                        SUPPLE_TRY(lay(g, h, tb0, tb0 + (limit - Bg), phase, true, false, false));
                    } else {
                        for (double b = Bg, ph = phase; b < limit - 1e-9; ph = 0) {
                            const double e = std::min(limit, b + (P - ph));
                            const double tb = tb0 + (b - Bg);
                            SUPPLE_TRY(lay(g, h, tb, tb + (e - b), ph, wholeLoop, false, false));
                            b = e;
                            if (once) break;
                        }
                    }
                } else {
                    const std::int64_t Pf = time::samplesAt(P, rate);
                    if (Pf <= 0) break;
                    const std::int64_t q = Sg - x.e->anchor;
                    std::int64_t limit = h;
                    if (once) {
                        if (q < 0 || q >= Pf) break;
                        limit = std::min(h, g + (Pf - q));
                    }
                    const std::int64_t phase = positiveMod(q, Pf);
                    if (single) {
                        SUPPLE_TRY(
                            lay(g, limit, 0, 0, double(phase), true, g == x.from, limit == x.to));
                    } else {
                        for (std::int64_t f = g, ph = phase; f < limit; ph = 0) {
                            const std::int64_t e = std::min(limit, f + (Pf - ph));
                            SUPPLE_TRY(
                                lay(f, e, 0, 0, double(ph), wholeLoop, f == x.from, e == x.to));
                            f = e;
                            if (once) break;
                        }
                    }
                }
                if (once && h < x.to) break;  // held after the wrap
                g = h;
            }
        }
    }
    return done;
}

}  // namespace supple::engine
