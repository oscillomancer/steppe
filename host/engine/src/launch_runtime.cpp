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
    std::int64_t floor = clock.stream;
    if (clock.playing)
        for (const LaunchRequest& r : requests) {
            const detail::Node* n = nodeOf(*plan, r.track);
            const bool rendered = options_.anticipative && n && !n->live;
            const std::int64_t at = rendered ? std::max(renderedStream_.load(), clock.stream) +
                                                   std::max(options_.renderBlock, config_.blockSize)
                                             : clock.stream + config_.blockSize;
            floor = std::max(floor, at);
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
    for (const LaunchRequest& r : requests) {
        // The track's waiting slot, else a free or spent one.
        const auto waiting = [](std::uint64_t st) {
            return status(st) == LaunchSlot::kPending || status(st) == LaunchSlot::kMissed;
        };
        LaunchSlot* slot = nullptr;
        for (LaunchSlot& s : slots)
            if (waiting(s.state.load()) && s.track.load() == r.track.n) {
                slot = &s;
                break;
            }
        for (LaunchSlot& s : slots)
            if (!slot && !waiting(s.state.load())) slot = &s;
        if (!slot)
            return Error("launch.queue", "too many launches are waiting", "",
                         "wait for the queued launches to play");
        // Take it from the executors (a new generation), write, publish.
        std::uint64_t st = slot->state.load();
        while (!slot->state.compare_exchange_weak(st, next(st, LaunchSlot::kFree))) {
        }
        if (status(st) == LaunchSlot::kPending) launchesQueued_.fetch_sub(1);
        slot->request = r;
        slot->quantize = quantize;
        slot->track.store(r.track.n, std::memory_order_relaxed);
        slot->clip.store(r.clip.n, std::memory_order_relaxed);
        slot->action.store(static_cast<std::uint8_t>(r.action), std::memory_order_relaxed);
        slot->epoch.store(placed->epoch, std::memory_order_relaxed);
        slot->target.store(placed->stream, std::memory_order_relaxed);
        slot->anchorSample.store(placed->anchorSample, std::memory_order_relaxed);
        slot->anchorBeat.store(placed->anchorBeat, std::memory_order_relaxed);
        launchesQueued_.fetch_add(1);
        slot->state.store(next(slot->state.load(), LaunchSlot::kPending),
                          std::memory_order_release);
    }
    wakeRenderer();
    return placed;
}

std::vector<Engine::LaunchPending> Engine::launchesPending() const {
    std::vector<LaunchPending> out;
    for (const LaunchSlot& s : *launchSlots_) {
        if (status(s.state.load(std::memory_order_acquire)) != LaunchSlot::kPending) continue;
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

std::uint32_t Engine::launchesAt(detail::Plan& p, int liveClass, std::uint64_t epoch,
                                 std::int64_t stream, std::uint32_t k, bool renderer) SUPPLE_RT {
    if (launchesQueued_.load(std::memory_order_acquire) == 0) return k;
    std::int64_t soonest = INT64_MAX;
    for (LaunchSlot& slot : *launchSlots_) {
        std::uint64_t st = slot.state.load(std::memory_order_acquire);
        if (status(st) != LaunchSlot::kPending) continue;
        if (slot.epoch.load(std::memory_order_relaxed) != epoch) continue;  // re-placed by main
        const model::Id track{model::Kind::Track, static_cast<std::uint32_t>(
                                                      slot.track.load(std::memory_order_relaxed))};
        detail::Node* node = nullptr;
        for (detail::Node& n : p.nodes)
            if (n.id == track && (liveClass < 0 || n.live == (liveClass == 1))) node = &n;
        if (!node || node->players.empty()) continue;  // the other executor's, or not here
        const std::int64_t at = slot.target.load(std::memory_order_relaxed) + node->timelineShift;
        if (at < stream) {  // too late: the main thread places it again
            if (slot.state.compare_exchange_strong(st, with(st, LaunchSlot::kMissed))) {
                launchesQueued_.fetch_sub(1, std::memory_order_acq_rel);
                notifyMain();
            }
            continue;
        }
        if (at > stream) {
            soonest = std::min(soonest, at);
            continue;
        }
        detail::LaunchAction a;
        const auto action =
            static_cast<LaunchLogEntry::Action>(slot.action.load(std::memory_order_relaxed));
        a.kind = action == LaunchLogEntry::Action::Clip   ? detail::LaunchState::Kind::Clip
                 : action == LaunchLogEntry::Action::Stop ? detail::LaunchState::Kind::Stopped
                                                          : detail::LaunchState::Kind::Arrangement;
        const model::Id clip{model::Kind::Clip,
                             static_cast<std::uint32_t>(slot.clip.load(std::memory_order_relaxed))};
        a.player = a.kind == detail::LaunchState::Kind::Clip ? detail::playerOf(*node, clip) : -1;
        // A plan without the clip (this executor's is older or newer than
        // the main thread's): never applied here; service() drops it if
        // the latest plan lacks it too.
        if (a.kind == detail::LaunchState::Kind::Clip && a.player < 0) continue;
        a.anchorSample = slot.anchorSample.load(std::memory_order_relaxed);
        a.anchorBeat = slot.anchorBeat.load(std::memory_order_relaxed);
        const std::int64_t target = slot.target.load(std::memory_order_relaxed);
        // Applied once, by whichever executor gets here first with the slot
        // as it was read (the generation guards a rewrite).
        if (!slot.state.compare_exchange_strong(st, with(st, LaunchSlot::kApplied))) continue;
        launchesQueued_.fetch_sub(1, std::memory_order_acq_rel);
        detail::applyLaunch(p, *node, a);
        LaunchAck ack;
        ack.track = track.n;
        ack.clip = clip.n;
        ack.epoch = epoch;
        ack.action = static_cast<std::uint8_t>(action);
        ack.stream = target;
        ack.anchorSample = a.anchorSample;
        ack.anchorBeat = a.anchorBeat;
        (void)(renderer ? rendererLaunchAcks_ : callbackLaunchAcks_)->push(ack);
        notifyMain();
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
        if (status(st) != LaunchSlot::kPending && status(st) != LaunchSlot::kMissed) continue;
        const LaunchRequest& r = s.request;
        const detail::Node* n = plan ? nodeOf(*plan, r.track) : nullptr;
        const bool playable =
            n && !n->players.empty() &&
            (r.action != LaunchLogEntry::Action::Clip || detail::playerOf(*n, r.clip) >= 0);
        if (playable) continue;
        if (!s.state.compare_exchange_strong(st, next(st, LaunchSlot::kFree))) continue;  // applied
        if (status(st) == LaunchSlot::kPending) launchesQueued_.fetch_sub(1);
        warnings_.push_back("launch: " + r.track.str() +
                            "'s queued launch was dropped: the edit left nothing to play");
    }
    // Missed, or placed in an epoch that has ended: placed again, alone.
    const std::uint64_t epoch = epoch_.load(std::memory_order_acquire);
    for (LaunchSlot& s : *launchSlots_) {
        const std::uint64_t st = s.state.load(std::memory_order_acquire);
        const bool missed = status(st) == LaunchSlot::kMissed;
        const bool stale = status(st) == LaunchSlot::kPending && s.epoch.load() != epoch;
        if (!missed && !stale) continue;
        const LaunchRequest r = s.request;
        auto placed = launch(std::span<const LaunchRequest>(&r, 1), s.quantize);
        if (!placed && placed.error().code == "launch.later") continue;  // next service
        if (!placed) {
            // Its track can no longer play it: dropped.
            std::uint64_t now = s.state.load();
            if (s.state.compare_exchange_strong(now, next(now, LaunchSlot::kFree)) &&
                status(now) == LaunchSlot::kPending)
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

std::vector<model::Id> Engine::clipsSkipped() const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !plan->preparedSources) return {};
    return plan->preparedSources->clipsSkipped;
}

}  // namespace supple::engine
