// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's queue (ADR-0097, engine.md §1.4.2): the main thread
// places a launch at a quantize boundary in the epoch's stream; the
// executor that runs the track's node applies it there, at the block
// boundary where the node's song position reaches the anchor (the target
// plus the node's timeline shift), and acknowledges it for the log.
#include <algorithm>
#include <cmath>

#include "plan.h"
#include "rack_runtime.h"
#include "supple/engine/realtime.h"

namespace supple::engine {

// A stream sample of the epoch and the song position there, from which any
// later stream sample's song position follows (unity plans; the loop folds).
struct Engine::LaunchClock {
    std::uint64_t epoch = 0;
    std::int64_t stream = 0, pos = 0;
    bool playing = false;
    LoopRange loop;
};

namespace {

constexpr std::uint64_t kStatusMask = 7;

std::uint64_t status(std::uint64_t state) { return state & kStatusMask; }
std::uint64_t with(std::uint64_t state, std::uint64_t s) { return (state & ~kStatusMask) | s; }
std::uint64_t next(std::uint64_t state, std::uint64_t s) { return ((state >> 3) + 1) << 3 | s; }

// The frame a target's clip chains take a launch at `at` with lead `lead`
// (prelaunch). One before the epoch's first frame (a launch placed while
// stopped targets the play start) is that first frame, where the offline
// replay applies it too: the chain warms up through its first frames, as
// when a swap raises the lead (ADR-0097, amended 2026-11-20). Else it would
// count as passed, and the launch would be placed again a bar later.
std::int64_t earlyFrame(std::int64_t at, std::int64_t lead) {
    return std::max<std::int64_t>(at - lead, 0);
}

// The song position `d` stream frames after `from`, the loop folding.
std::int64_t songAfter(const LoopRange& loop, std::int64_t from, std::int64_t d) {
    const std::int64_t x = from + d;
    if (loop.active() && from < loop.end && x >= loop.end)
        return loop.start + (x - loop.end) % (loop.end - loop.start);
    return x;
}

// The first quantize boundary at or after `beat`, in beats (quarter notes).
double boundaryAt(const time::TimeMap& tm, double beat, model::LaunchQuantize q) {
    constexpr double kEps = 1e-9;
    double step = 0;
    switch (q) {
        case model::LaunchQuantize::None:
            return beat;
        case model::LaunchQuantize::Bar: {
            const time::BarBeat at = tm.barBeat(beat);
            const double start = tm.beatOfBar(at.bar);
            return beat - start <= kEps ? start : tm.beatOfBar(at.bar + 1);
        }
        case model::LaunchQuantize::Beat:
        case model::LaunchQuantize::Quarter:
            step = 1;
            break;
        case model::LaunchQuantize::Half:
            step = 2;
            break;
        case model::LaunchQuantize::Eighth:
            step = 0.5;
            break;
        case model::LaunchQuantize::Sixteenth:
            step = 0.25;
            break;
        case model::LaunchQuantize::ThirtySecond:
            step = 0.125;
            break;
    }
    return std::ceil((beat - kEps) / step) * step;
}

const detail::Node* nodeOf(const detail::Plan& p, model::Id track) {
    for (const detail::Node& n : p.nodes)
        if (n.id == track) return &n;
    return nullptr;
}

}  // namespace

bool Engine::launchClock(LaunchClock& c) const {
    const TransportSnapshot t = transportSnapshot();
    c.epoch = t.epoch;
    c.loop = t.loop;
    c.playing = wantPlaying_.load(std::memory_order_acquire);
    if (!c.playing) {
        // Stopped: the stream holds still at the play position.
        c.stream = playedStream(c.epoch);
        c.pos = transport().sample;
        return true;
    }
    // The executor's last published position and its stream, read until
    // they belong together.
    for (int tries = 0; tries < 64; ++tries) {
        const auto e1 = rtPosEpoch_.load(std::memory_order_acquire);
        const auto s1 = rtStream_.load();
        const auto pos = rtPos_.load();
        const auto s2 = rtStream_.load();
        const auto e2 = rtPosEpoch_.load(std::memory_order_acquire);
        if (e1 != e2 || s1 != s2) continue;
        if (e1 != c.epoch) return false;  // not played in this epoch yet
        c.stream = s1;
        c.pos = pos;
        return true;
    }
    return false;
}

Result<Engine::LaunchPlaced> Engine::placeLaunch(std::span<const LaunchRequest> requests,
                                                 model::LaunchQuantize quantize) const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !latest_)
        return Error("launch.engine", "no project is loaded in the engine", "",
                     "start the engine with a project first");
    LaunchClock clock;
    if (!launchClock(clock))
        return Error("launch.later", "the transport is relocating", "", "launch again in a moment");
    // Each track's floor: its executor's position plus that executor's block.
    // A rack Notes feed following the track is a target too (ADR-0097).
    std::int64_t floor = clock.stream;
    if (clock.playing)
        for (const LaunchRequest& r : requests)
            for (const detail::Node& n : plan->nodes) {
                const bool target =
                    n.id == r.track ||
                    (n.rack && std::any_of(n.rack->notes.begin(), n.rack->notes.end(),
                                           [&](const detail::NotesFeed& f) {
                                               return f.follows && f.source == r.track;
                                           }));
                if (!target) continue;
                const bool rendered = options_.anticipative && !n.live;
                std::int64_t at = rendered ? std::max(renderedStream_.load(), clock.stream) +
                                                 std::max(options_.renderBlock, config_.blockSize)
                                           : clock.stream + config_.blockSize;
                // Never before where that executor's block may already reach.
                const LaunchHorizon& h = launchHorizon_[rendered ? 0 : 1];
                const std::int64_t end = h.end.load();
                if (h.epoch.load() == clock.epoch) at = std::max(at, end);
                // Its clip chains' input takes the launch its lead earlier
                // (ADR-0097, amended 2026-11-20).
                floor = std::max(floor, at + n.launchLead);
            }
    const time::TimeMap& tm = *latest_->timeMap;
    const double rate = config_.sampleRate;
    const std::int64_t songFloor = songAfter(clock.loop, clock.pos, floor - clock.stream);
    const auto beatOf = [&](std::int64_t sample) { return tm.beatAt(double(sample) / rate); };
    const auto sampleOf = [&](double beat) { return time::samplesAt(tm.secondsAt(beat), rate); };
    LaunchPlaced out;
    out.epoch = clock.epoch;
    if (!clock.playing || quantize == model::LaunchQuantize::None) {
        out.stream = floor;
        out.anchorSample = songFloor;
        out.anchorBeat = beatOf(songFloor);
        return out;
    }
    double beat = boundaryAt(tm, beatOf(songFloor), quantize);
    std::int64_t anchor = sampleOf(beat);
    std::int64_t distance = anchor - songFloor;
    const LoopRange& loop = clock.loop;
    if (loop.active() && songFloor < loop.end && anchor >= loop.end) {
        // The loop wraps first: the first boundary inside it after its start.
        beat = boundaryAt(tm, beatOf(loop.start), quantize);
        anchor = sampleOf(beat);
        if (anchor >= loop.end) {
            anchor = loop.start;
            beat = beatOf(anchor);
        }
        distance = (loop.end - songFloor) + (anchor - loop.start);
    }
    out.stream = floor + distance;
    out.anchorSample = anchor;
    out.anchorBeat = beat;
    return out;
}

Result<Engine::LaunchPlaced> Engine::launch(std::span<const LaunchRequest> requests,
                                            model::LaunchQuantize quantize) {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan)
        return Error("launch.engine", "no project is loaded in the engine", "",
                     "start the engine with a project first");
    if (plan->rateEnabled)  // staged (engine.md §1.4.2): no players compiled
        return Error("launch.playrate", "clips cannot play while a master playrate is active", "",
                     "set the master playrate to 1 and turn off its envelope, then launch");
    for (const LaunchRequest& r : requests) {
        const auto& prepared = plan->preparedSources;
        if (r.action == LaunchLogEntry::Action::Clip && prepared &&
            std::find(prepared->clipsSkipped.begin(), prepared->clipsSkipped.end(), r.clip) !=
                prepared->clipsSkipped.end())
            return Error("launch.memory",
                         r.clip.str() + "'s audio did not fit the prepared-audio budget", "",
                         "use shorter audio or fewer distinct files in clips, or remove some "
                         "arrangement audio");
        const detail::Node* n = nodeOf(*plan, r.track);
        if (!n || n->players.empty())
            return Error("launch.clip", r.track.str() + " has no clips to play", "",
                         "the track is frozen, has no clips, or a master playrate is active");
        if (r.action == LaunchLogEntry::Action::Clip && detail::playerOf(*n, r.clip) < 0)
            return Error(
                "launch.clip",
                r.clip.str() + " is not one of " + r.track.str() + "'s clips in the running plan",
                "", "wait for the edit to reach the engine, then launch again");
    }
    auto placed = placeLaunch(requests, quantize);
    if (!placed) return placed;
    auto& slots = *launchSlots_;
    const std::uint8_t required = options_.anticipative ? 3 : 2;
    std::vector<std::pair<LaunchSlot*, std::uint64_t>> published;
    for (const LaunchRequest& r : requests) {
        // The track's waiting slot, else a free one or one every executor is
        // done with. Taking it (a new generation) fails if an executor
        // decided it meanwhile: then look again.
        const auto waiting = [](std::uint64_t st) {
            const auto s = status(st);
            return s == LaunchSlot::kPending || s == LaunchSlot::kMissed ||
                   s == LaunchSlot::kCommitted || s == LaunchSlot::kVoided;
        };
        const auto spent = [](const LaunchSlot& s, std::uint64_t st) {
            if (status(st) == LaunchSlot::kFree) return true;
            const std::uint8_t need = s.required.load();
            return status(st) == LaunchSlot::kApplied && (s.done.load() & need) == need;
        };
        LaunchSlot* slot = nullptr;
        std::uint64_t st = 0;
        for (int tries = 0; !slot && tries < 16; ++tries) {
            LaunchSlot* pick = nullptr;
            for (LaunchSlot& s : slots) {
                const std::uint64_t now = s.state.load();
                if (waiting(now) && s.track.load() == r.track.n) {
                    pick = &s;
                    st = now;
                    break;
                }
            }
            for (LaunchSlot& s : slots) {
                if (pick) break;
                const std::uint64_t now = s.state.load();
                if (spent(s, now)) {
                    pick = &s;
                    st = now;
                }
            }
            if (!pick) break;
            if (pick->state.compare_exchange_strong(st, next(st, LaunchSlot::kFree))) slot = pick;
        }
        if (!slot)
            return Error("launch.queue", "too many launches are waiting", "",
                         "wait for the queued launches to play");
        if (status(st) == LaunchSlot::kPending || status(st) == LaunchSlot::kCommitted)
            launchesQueued_.fetch_sub(1);
        slot->request = r;
        slot->quantize = quantize;
        slot->track.store(r.track.n, std::memory_order_relaxed);
        slot->clip.store(r.clip.n, std::memory_order_relaxed);
        slot->action.store(static_cast<std::uint8_t>(r.action), std::memory_order_relaxed);
        slot->done.store(0, std::memory_order_relaxed);
        slot->required.store(required, std::memory_order_relaxed);
        slot->epoch.store(placed->epoch, std::memory_order_relaxed);
        slot->target.store(placed->stream, std::memory_order_relaxed);
        slot->anchorSample.store(placed->anchorSample, std::memory_order_relaxed);
        slot->anchorBeat.store(placed->anchorBeat, std::memory_order_relaxed);
        launchesQueued_.fetch_add(1);
        const std::uint64_t pending = next(slot->state.load(), LaunchSlot::kPending);
        slot->state.store(pending);  // sequentially consistent: before the horizons below
        published.push_back({slot, pending});
    }
    if (launchCommitForTests_) launchCommitForTests_(launchCommitContext_);
    // Decided once, for every target: committed if each executor running
    // one will read the slot before it passes its frame; otherwise missed,
    // and placed again by service().
    for (auto& [slot, st] : published) {
        const bool allow =
            launchHorizonsAllow(*plan, slot->request.track, placed->epoch, placed->stream);
        std::uint64_t expect = st;
        if (allow)
            (void)slot->state.compare_exchange_strong(expect, with(st, LaunchSlot::kCommitted));
        else if (slot->state.compare_exchange_strong(expect, with(st, LaunchSlot::kMissed)))
            launchesQueued_.fetch_sub(1);
    }
    wakeRenderer();
    return placed;
}

bool Engine::launchHorizonsAllow(const detail::Plan& plan, model::Id track, std::uint64_t epoch,
                                 std::int64_t target) const {
    for (const detail::Node& n : plan.nodes) {
        const bool owns = (n.id == track && !n.players.empty()) ||
                          (n.rack && std::any_of(n.rack->notes.begin(), n.rack->notes.end(),
                                                 [&](const detail::NotesFeed& f) {
                                                     return f.follows && f.source == track;
                                                 }));
        if (!owns) continue;
        const LaunchHorizon& h = launchHorizon_[options_.anticipative && !n.live ? 0 : 1];
        // The end, then the epoch: an executor stores them the other way.
        const std::int64_t end = h.end.load();
        const std::uint64_t e = h.epoch.load();
        if (e > epoch) return false;  // relocated already
        if (e == epoch && end > earlyFrame(target, n.launchLead))
            return false;  // its early half too
    }
    return true;
}

std::vector<Engine::LaunchPending> Engine::launchesPending() const {
    std::vector<LaunchPending> out;
    for (const LaunchSlot& s : *launchSlots_) {
        const auto st = status(s.state.load(std::memory_order_acquire));
        if (st != LaunchSlot::kPending && st != LaunchSlot::kCommitted) continue;
        LaunchPending p;
        p.request = s.request;
        p.placed.epoch = s.epoch.load(std::memory_order_relaxed);
        p.placed.stream = s.target.load(std::memory_order_relaxed);
        p.placed.anchorSample = s.anchorSample.load(std::memory_order_relaxed);
        p.placed.anchorBeat = s.anchorBeat.load(std::memory_order_relaxed);
        out.push_back(p);
    }
    return out;
}

namespace {

// A launch's targets in `p` that an executor class runs (liveClass as for
// wrapTo): its track's channel, when it has clips, and each rack Notes feed
// that follows the track (ADR-0097, dated revision 2026-11-20).
template <class F>
void forEachTarget(detail::Plan& p, int liveClass, model::Id track, F&& f) SUPPLE_RT {
    for (detail::Node& n : p.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        if (n.id == track && !n.players.empty()) f(n, static_cast<detail::NotesFeed*>(nullptr));
        if (n.rack)
            for (detail::NotesFeed& feed : n.rack->notes)
                if (feed.follows && feed.source == track) f(n, &feed);
    }
}

}  // namespace

void Engine::launchDone(LaunchSlot& slot, std::uint8_t mine) SUPPLE_RT {
    const std::uint8_t required = slot.required.load(std::memory_order_relaxed);
    const std::uint8_t was = slot.done.fetch_or(mine, std::memory_order_acq_rel);
    if ((was & required) != required && ((was | mine) & required) == required) {
        launchesQueued_.fetch_sub(1, std::memory_order_acq_rel);
        notifyMain();
    }
}

std::uint32_t Engine::launchesAt(detail::Plan& p, int liveClass, std::uint64_t epoch,
                                 std::int64_t stream, std::uint32_t k, bool renderer) SUPPLE_RT {
    const std::size_t c = renderer ? 0 : 1;
    const std::uint8_t mine = liveClass < 0 ? 3 : static_cast<std::uint8_t>(1u << c);
    // The horizon before the queue (sequentially consistent): the main
    // thread publishes a slot, then reads this; so either it sees how far
    // this block may go, or this executor sees the slot (LaunchSlot).
    launchHorizon_[c].epoch.store(epoch);
    launchHorizon_[c].end.store(stream + k);
    const bool lastValid = launchLastValid_[c];
    const std::uint64_t lastEpoch = launchLastEpoch_[c];
    const std::int64_t lastStream = launchLastStream_[c];
    // A call again at the same stream (a block that did not advance) has
    // switched what was due there already.
    const bool repeat = lastValid && lastEpoch == epoch && lastStream == stream;
    launchLastValid_[c] = true;
    launchLastEpoch_[c] = epoch;
    launchLastStream_[c] = stream;
    if (launchesQueued_.load() == 0) return k;
    std::int64_t soonest = INT64_MAX;
    for (LaunchSlot& slot : *launchSlots_) {
        std::uint64_t st = slot.state.load();
        const std::uint64_t was = status(st);
        if (was != LaunchSlot::kPending && was != LaunchSlot::kCommitted &&
            was != LaunchSlot::kApplied)
            continue;
        if (was == LaunchSlot::kApplied &&
            (slot.done.load(std::memory_order_acquire) & mine) == mine)
            continue;
        const std::uint64_t slotEpoch = slot.epoch.load(std::memory_order_relaxed);
        const model::Id track{model::Kind::Track, static_cast<std::uint32_t>(
                                                      slot.track.load(std::memory_order_relaxed))};
        const model::Id clip{model::Kind::Clip,
                             static_cast<std::uint32_t>(slot.clip.load(std::memory_order_relaxed))};
        const auto action =
            static_cast<LaunchLogEntry::Action>(slot.action.load(std::memory_order_relaxed));
        const detail::LaunchState::Kind kind =
            action == LaunchLogEntry::Action::Clip   ? detail::LaunchState::Kind::Clip
            : action == LaunchLogEntry::Action::Stop ? detail::LaunchState::Kind::Stopped
                                                     : detail::LaunchState::Kind::Arrangement;
        const std::int64_t target = slot.target.load(std::memory_order_relaxed);
        const std::int64_t anchorSample = slot.anchorSample.load(std::memory_order_relaxed);
        const double anchorBeat = slot.anchorBeat.load(std::memory_order_relaxed);
        // Switches this class's targets that `when` accepts. `relocated`: at
        // its first block after leaving the slot's epoch, as a relocation.
        const auto apply = [&](auto when, bool relocated) SUPPLE_RT {
            forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed* feed) {
                if (!when(target + n.timelineShift)) return;
                if (feed) {
                    detail::applyFeedLaunch(*feed, kind, clip, anchorSample, anchorBeat);
                    if (relocated) feed->launch.unanchored = true;
                    return;
                }
                detail::LaunchAction a;
                a.kind = kind;
                a.player = kind == detail::LaunchState::Kind::Clip ? detail::playerOf(n, clip) : -1;
                // A plan without the clip (only while an edit that removed it
                // reaches this executor): the other targets switch alone.
                if (kind == detail::LaunchState::Kind::Clip && a.player < 0) return;
                a.anchorSample = anchorSample;
                a.anchorBeat = anchorBeat;
                detail::applyLaunch(p, n, a);
                if (relocated) detail::launchRelocated(n);
                LaunchAck ack;
                ack.track = track.n;
                ack.clip = clip.n;
                ack.epoch = slotEpoch;
                ack.action = static_cast<std::uint8_t>(action);
                ack.stream = target;
                ack.anchorSample = anchorSample;
                ack.anchorBeat = anchorBeat;
                (void)(renderer ? rendererLaunchAcks_ : callbackLaunchAcks_)->push(ack);
                notifyMain();
            });
        };
        if (slotEpoch != epoch) {
            // Applied in an epoch this executor has left, or never ran: what
            // it had not switched there switches now.
            if (was == LaunchSlot::kApplied && slotEpoch < epoch) {
                const bool ran = lastValid && lastEpoch == slotEpoch;
                apply([&](std::int64_t at) { return !ran || at > lastStream; }, true);
                launchDone(slot, mine);
            }
            continue;  // pending or committed: the main thread places it again
        }
        bool any = false, passed = false, due = false, lacking = false;
        std::int64_t first = INT64_MAX, after = INT64_MAX;
        forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed* feed) {
            const std::int64_t at = target + n.timelineShift;
            // A channel with clip chains takes it its lead early as well
            // (prelaunch): its first frame, which decides it.
            const std::int64_t early = earlyFrame(at, feed ? 0 : n.launchLead);
            any = true;
            first = std::min(first, early);
            if (early < stream) passed = true;  // (consulted only while undecided)
            if (early == stream || at == stream) {
                due = true;
                lacking = lacking || (!feed && kind == detail::LaunchState::Kind::Clip &&
                                      detail::playerOf(n, clip) < 0);
            }
            if (early > stream)
                after = std::min(after, early);
            else if (at > stream)
                after = std::min(after, at);
        });
        if (!any) {  // nothing of it runs here
            if (was == LaunchSlot::kApplied) launchDone(slot, mine);
            continue;
        }
        if (was == LaunchSlot::kPending) {
            // Undecided at (or past) a frame of its: never taken there.
            if ((passed || due) &&
                slot.state.compare_exchange_strong(st, with(st, LaunchSlot::kMissed))) {
                launchesQueued_.fetch_sub(1, std::memory_order_acq_rel);
                notifyMain();
            }
            if (!passed && !due) soonest = std::min(soonest, first);
            continue;
        }
        if (was == LaunchSlot::kCommitted) {
            if (!passed && !due) {
                soonest = std::min(soonest, first);
                continue;
            }
            // Past a frame (the horizons rule it out) or the track's plan
            // lacks the clip: voided before anything switched, placed again.
            if (passed || lacking) {
                if (slot.state.compare_exchange_strong(st, with(st, LaunchSlot::kVoided))) {
                    launchesQueued_.fetch_sub(1, std::memory_order_acq_rel);
                    notifyMain();
                }
                continue;
            }
            // The first target to switch decides for all of them.
            if (!slot.state.compare_exchange_strong(st, with(st, LaunchSlot::kApplied)) &&
                status(st) != LaunchSlot::kApplied)
                continue;  // voided, or rewritten
        }
        if (due && !repeat) {
            forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed* feed) {
                if (feed || n.launchLead <= 0 ||
                    earlyFrame(target + n.timelineShift, n.launchLead) != stream)
                    return;
                detail::LaunchAction a;
                a.kind = kind;
                a.player = kind == detail::LaunchState::Kind::Clip ? detail::playerOf(n, clip) : -1;
                if (kind == detail::LaunchState::Kind::Clip && a.player < 0) return;
                a.anchorSample = anchorSample;
                a.anchorBeat = anchorBeat;
                detail::prelaunch(n, a);
            });
            apply([&](std::int64_t at) { return at == stream; }, false);
        }
        if (after != INT64_MAX)
            soonest = std::min(soonest, after);
        else
            launchDone(slot, mine);
    }
    if (soonest != INT64_MAX)
        k = static_cast<std::uint32_t>(std::min<std::int64_t>(k, soonest - stream));
    return k;
}

void Engine::serviceLaunches() {
    bool added = false;
    for (auto* ring : {rendererLaunchAcks_.get(), callbackLaunchAcks_.get()}) {
        LaunchAck ack;
        while (ring->pop(ack)) {
            added = true;
            LaunchLogEntry e;
            e.runningFrame = ack.stream;
            e.track = {model::Kind::Track, static_cast<std::uint32_t>(ack.track)};
            e.action = static_cast<LaunchLogEntry::Action>(ack.action);
            if (e.action == LaunchLogEntry::Action::Clip)
                e.clip = {model::Kind::Clip, static_cast<std::uint32_t>(ack.clip)};
            e.anchorSample = ack.anchorSample;
            e.anchorBeat = ack.anchorBeat;
            e.epoch = ack.epoch;
            launchLog_.push_back(e);
        }
    }
    if (added)  // the two executors' acks interleave
        std::stable_sort(launchLog_.begin(), launchLog_.end(),
                         [](const LaunchLogEntry& a, const LaunchLogEntry& b) {
                             return a.epoch != b.epoch ? a.epoch < b.epoch
                                                       : a.runningFrame < b.runningFrame;
                         });
    // Waiting for a track or clip the latest plan cannot play (frozen,
    // removed, a master playrate): dropped, with a warning.
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    // A track whose last launch's clip the latest plan lacks is back on the
    // arrangement (carryLaunch does the same in the executors).
    if (added || publishedGen_ != launchCheckedGen_) {
        launchCheckedGen_ = publishedGen_;
        std::map<model::Id, std::size_t> last;
        for (std::size_t i = 0; i < launchLog_.size(); ++i) last[launchLog_[i].track] = i;
        for (const auto& [track, i] : last) {
            const LaunchLogEntry& e = launchLog_[i];
            const auto reset = launchReset_.find(track);
            if (e.action != LaunchLogEntry::Action::Clip ||
                (reset != launchReset_.end() && reset->second > i))
                continue;
            const detail::Node* n = plan ? nodeOf(*plan, track) : nullptr;
            if (!n || n->players.empty() || detail::playerOf(*n, e.clip) < 0)
                launchReset_[track] = launchLog_.size();
        }
    }
    for (LaunchSlot& s : *launchSlots_) {
        std::uint64_t st = s.state.load(std::memory_order_acquire);
        if (status(st) != LaunchSlot::kPending && status(st) != LaunchSlot::kMissed &&
            status(st) != LaunchSlot::kCommitted && status(st) != LaunchSlot::kVoided)
            continue;
        const LaunchRequest& r = s.request;
        const detail::Node* n = plan ? nodeOf(*plan, r.track) : nullptr;
        const bool playable =
            n && !n->players.empty() &&
            (r.action != LaunchLogEntry::Action::Clip || detail::playerOf(*n, r.clip) >= 0);
        if (playable) continue;
        if (!s.state.compare_exchange_strong(st, next(st, LaunchSlot::kFree))) continue;  // applied
        if (status(st) == LaunchSlot::kPending || status(st) == LaunchSlot::kCommitted)
            launchesQueued_.fetch_sub(1);
        warnings_.push_back("launch: " + r.track.str() +
                            "'s queued launch was dropped: the edit left nothing to play");
    }
    // Missed, or placed in an epoch that has ended: placed again, alone.
    const std::uint64_t epoch = epoch_.load(std::memory_order_acquire);
    for (LaunchSlot& s : *launchSlots_) {
        std::uint64_t st = s.state.load(std::memory_order_acquire);
        // A committed launch of an ended epoch that nothing has switched yet
        // is voided here, against an executor applying it there.
        if (status(st) == LaunchSlot::kCommitted && s.epoch.load() != epoch &&
            s.state.compare_exchange_strong(st, with(st, LaunchSlot::kVoided))) {
            launchesQueued_.fetch_sub(1);
            st = with(st, LaunchSlot::kVoided);
        }
        const bool missed = status(st) == LaunchSlot::kMissed || status(st) == LaunchSlot::kVoided;
        const bool stale = status(st) == LaunchSlot::kPending && s.epoch.load() != epoch;
        if (!missed && !stale) continue;
        const LaunchRequest r = s.request;
        auto placed = launch(std::span<const LaunchRequest>(&r, 1), s.quantize);
        if (!placed && placed.error().code == "launch.later") continue;  // next service
        if (!placed) {
            // Its track can no longer play it: dropped.
            std::uint64_t now = s.state.load();
            if (s.state.compare_exchange_strong(now, next(now, LaunchSlot::kFree)) &&
                (status(now) == LaunchSlot::kPending || status(now) == LaunchSlot::kCommitted))
                launchesQueued_.fetch_sub(1);
            warnings_.push_back("launch: " + r.track.str() + ": " + placed.error().message);
        }
    }
}

std::vector<Engine::TrackLaunch> Engine::launchState() const {
    std::vector<TrackLaunch> out;
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !latest_) return out;
    const TransportState t = transport();
    const std::uint64_t epoch = epoch_.load(std::memory_order_acquire);
    const std::int64_t played = playedStream(epoch);
    const time::TimeMap& tm = *latest_->timeMap;
    const double rate = config_.sampleRate;
    const double beat = tm.beatAt(t.seconds);
    const auto pending = launchesPending();
    const auto mod = [](double x, double m) { return x - std::floor(x / m) * m; };
    for (const detail::Node& n : plan->nodes) {
        if (n.players.empty()) continue;
        TrackLaunch tl;
        tl.track = n.id;
        // The newest heard entry since the track's reset; a newer one not
        // heard yet is what it has queued.
        const auto reset = launchReset_.find(n.id);
        const std::size_t from = reset == launchReset_.end() ? 0 : reset->second;
        const LaunchLogEntry* heard = nullptr;
        for (std::size_t i = launchLog_.size(); i-- > from;) {
            const LaunchLogEntry& e = launchLog_[i];
            if (e.track != n.id) continue;
            if (e.epoch == epoch && e.runningFrame + n.timelineShift > played) {
                if (!tl.queued) {
                    LaunchPending q;
                    q.request = {e.track, e.action, e.clip};
                    q.placed = {e.epoch, e.runningFrame, e.anchorSample, e.anchorBeat};
                    tl.queued = q;
                }
                continue;
            }
            heard = &e;
            break;
        }
        for (const LaunchPending& q : pending)
            if (q.request.track == n.id) tl.queued = q;  // newer than any applied
        if (heard && heard->action == LaunchLogEntry::Action::Stop)
            tl.plays = TrackLaunch::Plays::Stopped;
        if (heard && heard->action == LaunchLogEntry::Action::Clip) {
            const int k = detail::playerOf(n, heard->clip);
            if (k >= 0) {
                const detail::ClipPlayer& pl = n.players[static_cast<std::size_t>(k)];
                tl.clip = heard->clip;
                tl.plays = TrackLaunch::Plays::Clip;
                const double since = pl.midi ? beat - heard->anchorBeat
                                             : double(t.sample - heard->anchorSample) / rate;
                const double period = pl.midi ? pl.periodBeats : double(pl.periodFrames) / rate;
                if (pl.once && (since < 0 || since >= period))
                    tl.plays = TrackLaunch::Plays::Held;
                else if (period > 0)
                    tl.position = mod(since, period);
            }
        }
        out.push_back(std::move(tl));
    }
    return out;
}

// --- Capturing a performance (ADR-0097 L4) -------------------------------------

std::int64_t Engine::captureHeard() const {
    if (rtPosEpoch_.load(std::memory_order_acquire) != capture_.epoch) return capture_.startFrame;
    return std::max(capture_.startFrame, rtStream_.load());
}

void Engine::placeCapture() {
    if (!capture_.on || !capture_.pending) return;
    LaunchClock clock;
    if (!launchClock(clock)) return;  // relocating: the next service
    capture_.pending = false;
    capture_.epoch = clock.epoch;
    capture_.startFrame = clock.stream;
    capture_.startSample = clock.pos;
    capture_.loop = clock.loop;
}

void Engine::captureRelocating() {
    if (!capture_.on || capture_.pending) return;
    const std::int64_t heard = captureHeard();
    if (heard > capture_.startFrame) {  // it ran: running time ends here
        capture_.endFrame = heard;
        capture_.on = false;
    } else {
        capture_.pending = true;  // nothing ran yet: it begins where the transport goes
    }
}

void Engine::captureLaunches(bool on) {
    if (on == capture_.on) return;
    if (on) {
        capture_ = {};
        capture_.on = capture_.any = capture_.pending = true;
        placeCapture();
        return;
    }
    capture_.endFrame = capture_.pending ? capture_.startFrame : captureHeard();
    capture_.pending = false;
    capture_.on = false;
}

std::optional<Engine::LaunchCapture> Engine::launchCapture() {
    if (!capture_.any) return std::nullopt;
    serviceLaunches();  // what has applied so far is in the log
    placeCapture();
    LaunchCapture out;
    out.on = capture_.on;
    out.startSample = capture_.startSample;
    out.loop = capture_.loop;
    if (capture_.pending) return out;
    const std::int64_t start = capture_.startFrame;
    const std::int64_t end = capture_.on ? captureHeard() : capture_.endFrame;
    out.frames = end - start;
    // What each track played as the capture began: its newest entry by
    // then, unless a plan has since returned it to the arrangement.
    std::map<model::Id, std::size_t> before;
    for (std::size_t i = 0; i < launchLog_.size(); ++i) {
        const LaunchLogEntry& e = launchLog_[i];
        if (e.epoch < capture_.epoch || (e.epoch == capture_.epoch && e.runningFrame <= start))
            before[e.track] = i;
    }
    for (const auto& [track, i] : before) {
        const auto reset = launchReset_.find(track);
        if (reset != launchReset_.end() && reset->second > i) continue;
        LaunchLogEntry first = launchLog_[i];
        if (first.action == LaunchLogEntry::Action::Arrangement) continue;
        first.runningFrame = 0;
        first.epoch = capture_.epoch;
        out.launches.push_back(first);
    }
    for (const LaunchLogEntry& e : launchLog_)
        if (e.epoch == capture_.epoch && e.runningFrame > start && e.runningFrame < end) {
            LaunchLogEntry within = e;
            within.runningFrame -= start;
            out.launches.push_back(within);
        }
    return out;
}

std::vector<model::Id> Engine::clipsSkipped() const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !plan->preparedSources) return {};
    return plan->preparedSources->clipsSkipped;
}

}  // namespace supple::engine
