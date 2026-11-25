// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The clip launcher's queue (ADR-0097, engine.md §1.4.2): the main thread
// places a launch at a quantize boundary in the epoch's stream; the
// executor that runs the track's node applies it there, at the block
// boundary where the node's song position reaches the anchor (the target
// plus the node's timeline shift), and acknowledges it for the log.
#include <algorithm>
#include <bit>
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

// What a launch's action does to a channel.
detail::LaunchState::Kind launchKind(LaunchLogEntry::Action action) {
    return action == LaunchLogEntry::Action::Clip   ? detail::LaunchState::Kind::Clip
           : action == LaunchLogEntry::Action::Stop ? detail::LaunchState::Kind::Stopped
                                                    : detail::LaunchState::Kind::Arrangement;
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
    if (!c.playing && playStart_) {
        // play() is starting a new epoch at the play position.
        c.epoch = playStart_->first;
        c.stream = 0;
        c.pos = playStart_->second;
        return true;
    }
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
                // (ADR-0097, amended 2026-11-20), and an audio clip on a
                // pitch-keeping plan its warm-up (S3 step D); nothing else.
                const auto kind = launchKind(r.action);
                const int player = n.id == r.track && kind == detail::LaunchState::Kind::Clip
                                       ? detail::playerOf(n, r.clip)
                                       : -1;
                floor = std::max(floor, at + detail::launchEarly(n, kind, player));
                // A warmed launch of the track already under way (applied
                // from its warm-up's first frame, so not replaced) switches
                // first: a later press lands after it, whatever its kind
                // (round 139 S1), and a warming one only once that warm-up
                // is done, reader 2 warming one launch at a time.
                const bool warms = detail::launchWarms(n, kind, player);
                if (n.id == r.track && n.launchWarm > 0)
                    for (const LaunchSlot& s : *launchSlots_) {
                        if (status(s.state.load()) != LaunchSlot::kApplied ||
                            s.track.load() != r.track.n || s.epoch.load() != clock.epoch ||
                            s.target.load() < clock.stream)
                            continue;
                        const auto was =
                            launchKind(static_cast<LaunchLogEntry::Action>(s.action.load()));
                        const int wasPlayer =
                            was == detail::LaunchState::Kind::Clip
                                ? detail::playerOf(n, {model::Kind::Clip,
                                                       static_cast<std::uint32_t>(s.clip.load())})
                                : -1;
                        if (detail::launchWarms(n, was, wasPlayer))
                            floor = std::max(floor, s.target.load() + (warms ? n.launchWarm : 1));
                    }
                // While a trajectory change is pending (S3 step D4): until the
                // callback, the slowest executor, has played its switch and
                // the crossfade, a warming launch switches at least the
                // crossfade plus its warm-up after the switch, so its warm-up
                // runs on the new trajectory alone (stream = device -
                // rateOrigin).
                if (warms && n.id == r.track && plan->rateEnabled &&
                    plan->rateSwitchDevice != INT64_MIN) {
                    const std::int64_t change = plan->rateSwitchDevice - plan->rateOrigin;
                    if (clock.stream < change + plan->smoothFrames)
                        floor = std::max(floor, change + plan->smoothFrames + n.launchWarm);
                }
            }
    // On a playrate plan the song follows the rate timeline (S3).
    if (clock.playing && plan->rateEnabled)
        return placeRateLaunch(*plan, clock.epoch, floor, quantize);
    const time::TimeMap& tm = *latest_->timeMap;
    const double rate = config_.sampleRate;
    const std::int64_t songFloor = songAfter(clock.loop, clock.pos, floor - clock.stream);
    const auto beatOf = [&](std::int64_t sample) { return tm.beatAt(double(sample) / rate); };
    const auto sampleOf = [&](double beat) { return time::samplesAt(tm.secondsAt(beat), rate); };
    LaunchPlaced out;
    out.epoch = clock.epoch;
    // The song's running count moves with the stream here (S3 B).
    const std::int64_t songBase = unitySongBase(clock);
    if (!clock.playing || quantize == model::LaunchQuantize::None) {
        out.stream = floor;
        out.anchorSample = songFloor;
        out.anchorBeat = beatOf(songFloor);
        out.songFrame = songBase + out.stream;
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
    out.songFrame = songBase + out.stream;
    return out;
}

Result<Engine::LaunchPlaced> Engine::placeRateLaunch(const detail::Plan& plan, std::uint64_t epoch,
                                                     std::int64_t floor,
                                                     model::LaunchQuantize quantize) const {
    // The running executor's rate clock, as prepareRatePlan reads it: a
    // copy of its timeline from there, with the controls that follow.
    const auto words = readRateClock();
    const detail::Plan* running = nullptr;
    for (const auto& candidate : plans_)
        if (candidate->generation == words[9]) running = candidate.get();
    // Not a relocation: the rate the boundary is reached by is changing
    // (review round 78 K1), or does not reach it within the search.
    const auto later = [] {
        return Error("launch.later", "the master playrate is changing", "",
                     "launch again in a moment");
    };
    // A rate control changed and has not reached the executors yet: the
    // trajectory to the boundary is not known (serviceLaunches retries).
    if (words[8] != epoch || !running || !running->rateEnabled ||
        running->requestedScalar != plan.requestedScalar ||
        running->timelines[0]->points() != plan.timelines[0]->points())
        return later();
    std::array<std::uint64_t, 6> packed{};
    std::copy_n(words.begin(), 6, packed.begin());
    detail::PlayrateTimeline::Snapshot seed;
    seed.clock = std::bit_cast<detail::PlayrateClock::State>(packed);
    seed.device = std::bit_cast<std::int64_t>(words[6]);
    seed.loopStart = std::bit_cast<double>(words[10]);
    seed.loopEnd = std::bit_cast<double>(words[11]);
    seed.looping = words[12] != 0;
    const std::int64_t toDevice = seed.device - std::bit_cast<std::int64_t>(words[7]);
    const std::size_t clockClass = options_.anticipative ? 1 : 0;
    // A small ring: the search walks forward, reading only recent frames.
    // (Its points are absolute already, and a live plan starts at 0.)
    constexpr std::size_t kRing = std::size_t{1} << 16;
    detail::PlayrateTimeline copy(kRing, seed.clock.target,
                                  running->timelines[clockClass]->points());
    copy.seed(seed);
    for (const auto& edit : running->scalarHistory)
        if (edit.device > seed.device) (void)copy.scalarEdits.push(edit);
    copy.inheritControls(*running->timelines[clockClass]);
    const std::int64_t first = floor + toDevice;
    copy.ensure(first);
    const double songFloor = copy.position(first);
    const time::TimeMap& tm = *latest_->timeMap;
    const double rate = config_.sampleRate;
    const auto beatOf = [&](double sample) { return tm.beatAt(sample / rate); };
    const auto sampleOf = [&](double beat) { return time::samplesAt(tm.secondsAt(beat), rate); };
    LaunchPlaced out;
    out.epoch = epoch;
    // Quantize none: the next whole song sample, at or after the floor, so
    // the render's first-frame rule (offline.cpp placeLaunches) switches on
    // the same stream frame as the executor; at most 1/rate device frames
    // after the floor (round 174 F1).
    const bool none = quantize == model::LaunchQuantize::None;
    double beat = none ? 0.0 : boundaryAt(tm, beatOf(songFloor), quantize);
    std::int64_t anchor = none ? static_cast<std::int64_t>(std::ceil(songFloor)) : sampleOf(beat);
    if (none) beat = beatOf(double(anchor));
    double distance = double(anchor) - songFloor;
    if (none && copy.looping() && songFloor < copy.loopEnd() && double(anchor) >= copy.loopEnd()) {
        anchor = static_cast<std::int64_t>(copy.loopStart());  // wrapped first
        beat = beatOf(double(anchor));
        distance = copy.loopEnd() - songFloor;
    } else if (!none && copy.looping() && songFloor < copy.loopEnd() &&
               double(anchor) >= copy.loopEnd()) {
        // The loop wraps first: the first boundary inside it after its start.
        const auto start = static_cast<std::int64_t>(copy.loopStart());
        beat = boundaryAt(tm, beatOf(double(start)), quantize);
        anchor = sampleOf(beat);
        if (double(anchor) >= copy.loopEnd()) {
            anchor = start;
            beat = beatOf(double(anchor));
        }
        distance = (copy.loopEnd() - songFloor) + double(anchor - start);
    }
    // The first device frame whose (unwrapped) song position reaches it:
    // without a loop the position is the unwrapped count, so the whole
    // sample itself, as the render compares it.
    const double reach = copy.looping() ? copy.phase(first) + distance : double(anchor);
    constexpr std::int64_t kChunk = kRing / 4;
    // At the slowest rate the song takes four frames a sample; a little more.
    const std::int64_t limit =
        first +
        static_cast<std::int64_t>(std::ceil(std::max(distance, 0.0) / model::kMinPlayrate)) +
        2 * kChunk;
    std::int64_t lo = first;
    for (;;) {
        const std::int64_t hi = lo + kChunk;
        copy.ensure(hi);
        if (copy.phase(hi) >= reach) {
            std::int64_t a = lo, b = hi;
            while (a < b) {
                const std::int64_t mid = a + (b - a) / 2;
                if (copy.phase(mid) < reach)
                    a = mid + 1;
                else
                    b = mid;
            }
            out.stream = floor + (a - first);
            break;
        }
        lo = hi + 1;
        if (lo > limit)  // a trajectory that does not advance
            return Error("launch.later",
                         "the master playrate does not reach the launch boundary yet", "",
                         "launch again in a moment");
    }
    out.anchorSample = anchor;
    out.anchorBeat = beat;
    out.songFrame = std::llround(reach);  // the boundary on the unwrapped count
    return out;
}

void Engine::voidRateLaunches(std::int64_t device) {
    const auto words = readRateClock();
    const std::int64_t toDevice =
        std::bit_cast<std::int64_t>(words[6]) - std::bit_cast<std::int64_t>(words[7]);
    const std::uint64_t epoch = epoch_.load(std::memory_order_acquire);
    for (LaunchSlot& s : *launchSlots_) {
        std::uint64_t st = s.state.load(std::memory_order_acquire);
        if (status(st) != LaunchSlot::kPending && status(st) != LaunchSlot::kCommitted) continue;
        if (s.epoch.load() != epoch || words[8] != epoch ||
            s.target.load(std::memory_order_relaxed) + toDevice < device)
            continue;
        if (s.state.compare_exchange_strong(st, with(st, LaunchSlot::kVoided)))
            launchesQueued_.fetch_sub(1);
    }
}

Result<Engine::LaunchPlaced> Engine::launch(std::span<const LaunchRequest> requests,
                                            model::LaunchQuantize quantize) {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan)
        return Error("launch.engine", "no project is loaded in the engine", "",
                     "start the engine with a project first");
    for (const LaunchRequest& r : requests)
        if (auto refused = launchRefused(*plan, r)) return std::move(*refused);
    // Placed, published, then decided. If every executor's horizon passed
    // it meanwhile (a placement on a playrate plan integrates the rate up to
    // the boundary), it is placed again at once, the slots reused.
    for (int attempt = 0;; ++attempt) {
        bool missed = false;
        auto placed = publishLaunch(requests, quantize, missed);
        if (!placed || !missed || attempt == 2) {
            if (placed) captureBeforeStretchCut();
            return placed;
        }
    }
}

std::optional<Error> Engine::launchRefused(const detail::Plan& plan, const LaunchRequest& r) const {
    // The scene (r.scene) is not read: EngineApp::launchRefusals answers a
    // scene from its clips' refusals, asked without it (round 149 S2).
    const bool clip = r.action == LaunchLogEntry::Action::Clip;
    // With the pitch kept, launched audio warms a stretch converter up
    // (S3 step D); one read through its take chain has no player yet.
    if (clip && std::find(plan.launchRefusedPlayrate.begin(), plan.launchRefusedPlayrate.end(),
                          r.clip) != plan.launchRefusedPlayrate.end())
        return Error("launch.playrate",
                     r.clip.str() +
                         " is an audio clip with take FX or envelopes, which cannot play "
                         "while the master playrate keeps the pitch",
                     "",
                     "turn off the master playrate's preserve pitch, or set it to 1 without "
                     "an envelope, or launch a copy of the clip without take FX");
    const auto& prepared = plan.preparedSources;
    if (clip && prepared &&
        std::find(prepared->clipsSkipped.begin(), prepared->clipsSkipped.end(), r.clip) !=
            prepared->clipsSkipped.end())
        return Error("launch.memory",
                     r.clip.str() + "'s audio did not fit the prepared-audio budget", "",
                     "use shorter audio or fewer distinct files in clips, or remove some "
                     "arrangement audio");
    const detail::Node* n = nodeOf(plan, r.track);
    if (!n || n->players.empty())
        return Error("launch.clip", r.track.str() + " has no clips to play", "",
                     "the track is frozen or has no clips");
    if (clip && detail::playerOf(*n, r.clip) < 0)
        return Error(
            "launch.clip",
            r.clip.str() + " is not one of " + r.track.str() + "'s clips in the running plan", "",
            "wait for the edit to reach the engine, then launch again");
    return std::nullopt;
}

std::optional<Error> Engine::launchRefusal(const LaunchRequest& request) const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan) return std::nullopt;
    return launchRefused(*plan, request);
}

Result<Engine::LaunchPlaced> Engine::publishLaunch(std::span<const LaunchRequest> requests,
                                                   model::LaunchQuantize quantize, bool& missed) {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
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
        slot->scene.store(r.scene ? r.scene->n : 0, std::memory_order_relaxed);
        slot->action.store(static_cast<std::uint8_t>(r.action), std::memory_order_relaxed);
        slot->done.store(0, std::memory_order_relaxed);
        slot->required.store(required, std::memory_order_relaxed);
        slot->epoch.store(placed->epoch, std::memory_order_relaxed);
        slot->target.store(placed->stream, std::memory_order_relaxed);
        slot->anchorSample.store(placed->anchorSample, std::memory_order_relaxed);
        slot->songFrame.store(placed->songFrame, std::memory_order_relaxed);
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
    missed = !published.empty();
    for (auto& [slot, st] : published) {
        const bool allow = launchHorizonsAllow(*plan, slot->request, placed->epoch, placed->stream);
        std::uint64_t expect = st;
        if (allow) {
            (void)slot->state.compare_exchange_strong(expect, with(st, LaunchSlot::kCommitted));
            missed = false;
        } else if (slot->state.compare_exchange_strong(expect, with(st, LaunchSlot::kMissed))) {
            launchesQueued_.fetch_sub(1);
        }
    }
    wakeRenderer();
    return placed;
}

bool Engine::launchHorizonsAllow(const detail::Plan& plan, const LaunchRequest& r,
                                 std::uint64_t epoch, std::int64_t target) const {
    const model::Id track = r.track;
    const auto kind = launchKind(r.action);
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
        const int player = n.id == track && kind == detail::LaunchState::Kind::Clip
                               ? detail::playerOf(n, r.clip)
                               : -1;
        if (e == epoch && end > earlyFrame(target, detail::launchEarly(n, kind, player)))
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
        p.placed.songFrame = s.songFrame.load(std::memory_order_relaxed);
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
        const std::uint64_t scene = slot.scene.load(std::memory_order_relaxed);
        const auto action =
            static_cast<LaunchLogEntry::Action>(slot.action.load(std::memory_order_relaxed));
        const detail::LaunchState::Kind kind = launchKind(action);
        const std::int64_t target = slot.target.load(std::memory_order_relaxed);
        const std::int64_t anchorSample = slot.anchorSample.load(std::memory_order_relaxed);
        const double anchorBeat = slot.anchorBeat.load(std::memory_order_relaxed);
        const std::int64_t songFrame = slot.songFrame.load(std::memory_order_relaxed);
        // Switches this class's targets that `when` accepts. `relocated`: at
        // its first block after leaving the slot's epoch, as a relocation.
        const auto apply = [&](auto when, bool relocated) SUPPLE_RT {
            forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed* feed) {
                if (!when(target + n.timelineShift)) return;
                if (feed) {
                    detail::applyFeedLaunch(*feed, kind, clip, anchorSample, anchorBeat);
                    if (relocated) {
                        feed->launch.unanchored = true;
                        feed->launchFresh = false;
                    }
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
                detail::applyLaunch(n, a);
                if (relocated) detail::launchRelocated(n);
                LaunchAck ack;
                ack.track = track.n;
                ack.clip = clip.n;
                ack.scene = scene;
                ack.epoch = slotEpoch;
                ack.action = static_cast<std::uint8_t>(action);
                ack.stream = target;
                ack.anchorSample = anchorSample;
                ack.anchorBeat = anchorBeat;
                ack.songFrame = songFrame;
                if (!(renderer ? rendererLaunchAcks_ : callbackLaunchAcks_)->push(ack))
                    launchAcksDropped_.fetch_add(1, std::memory_order_relaxed);
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
            // (prelaunch), and a pitch-keeping one its warm-up early
            // (prewarm, S3 step D): the first of them decides it.
            const int player =
                kind == detail::LaunchState::Kind::Clip ? detail::playerOf(n, clip) : -1;
            const std::int64_t early =
                earlyFrame(at, feed ? 0 : detail::launchEarly(n, kind, player));
            const std::int64_t chain = earlyFrame(at, feed ? 0 : n.launchLead);
            any = true;
            first = std::min(first, early);
            if (early < stream) passed = true;  // (consulted only while undecided)
            if (early == stream || chain == stream || at == stream) {
                due = true;
                lacking = lacking || (!feed && kind == detail::LaunchState::Kind::Clip &&
                                      detail::playerOf(n, clip) < 0);
            }
            for (const std::int64_t f : {early, chain, at})
                if (f > stream) after = std::min(after, f);
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
                if (feed) return;
                const std::int64_t at = target + n.timelineShift;
                detail::LaunchAction a;
                a.kind = kind;
                a.player = kind == detail::LaunchState::Kind::Clip ? detail::playerOf(n, clip) : -1;
                if (kind == detail::LaunchState::Kind::Clip && a.player < 0) return;
                const bool chain = n.launchLead > 0 && earlyFrame(at, n.launchLead) == stream;
                const bool warm = detail::launchWarms(n, kind, a.player) &&
                                  earlyFrame(at, n.launchWarm) == stream;
                if (!chain && !warm) return;
                a.anchorSample = anchorSample;
                a.anchorBeat = anchorBeat;
                if (chain) detail::prelaunch(n, a);
                if (warm) detail::prewarm(n, a, target);  // the bare target (Node::launchWarmAt)
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
            if (ack.returned) {  // where an executor returned the track
                logReturn(ack);
                added = true;
                continue;
            }
            added = true;
            LaunchLogEntry e;
            e.runningFrame = ack.stream;
            e.track = {model::Kind::Track, static_cast<std::uint32_t>(ack.track)};
            e.action = static_cast<LaunchLogEntry::Action>(ack.action);
            if (e.action == LaunchLogEntry::Action::Clip)
                e.clip = {model::Kind::Clip, static_cast<std::uint32_t>(ack.clip)};
            e.anchorSample = ack.anchorSample;
            e.anchorBeat = ack.anchorBeat;
            e.songFrame = ack.songFrame;
            e.epoch = ack.epoch;
            if (ack.scene != 0)
                e.scene = model::Id{model::Kind::Scene, static_cast<std::uint32_t>(ack.scene)};
            launchLog_.push_back(e);
            launchIsReturn_.push_back(0);
        }
    }
    if (added) sortLaunchLog();  // the two executors' acks interleave
    // Waiting for a track or clip the latest plan cannot play (frozen,
    // removed, an audio clip under a pitch-keeping playrate): dropped, with
    // a warning.
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    // A plan with other rate controls (S3): what was placed on the old
    // trajectory and switches after its handover is placed again. A
    // playrate that became active ends a running capture.
    if (plan && publishedGen_ != launchCheckedGen_) {
        LaunchRateControls controls;
        controls.enabled = plan->rateEnabled;
        if (plan->rateEnabled) {
            controls.sync = plan->syncRate;
            controls.scalar = plan->requestedScalar;
            for (const auto& pt : plan->timelines[0]->points()) {
                controls.points.push_back(pt.sample);
                controls.points.push_back(pt.value);
            }
        }
        if (controls != launchRateControls_) {
            launchRateControls_ = std::move(controls);
            const std::uint64_t epoch = epoch_.load(std::memory_order_acquire);
            for (LaunchSlot& s : *launchSlots_) {
                std::uint64_t st = s.state.load(std::memory_order_acquire);
                if ((status(st) == LaunchSlot::kPending || status(st) == LaunchSlot::kCommitted) &&
                    s.epoch.load() == epoch && s.target.load() >= plan->handAt &&
                    s.state.compare_exchange_strong(st, with(st, LaunchSlot::kVoided)))
                    launchesQueued_.fetch_sub(1);
            }
        }
    }
    captureEnding();
    // A master playrate that starts keeping the pitch while a launched audio
    // clip plays (S3 B, Q2): what was performed up to here stays committable.
    if (capture_.on && !capture_.pending)
        if (const auto clip = audioUnderStretch()) {
            (void)captureLaunches(false);
            warnings_.push_back("launch: the capture ended: " + clip->str() +
                                " is an audio clip, which a capture cannot write as heard while "
                                "the master playrate keeps the pitch");
        }
    // A track whose newest launch the latest plan cannot play goes back to
    // the arrangement in the executors as they adopt it (carryLaunch), which
    // they ack (logReturn). Under a playrate that is at the plan's handAt:
    // its song frame is placed now, before the song passes it.
    if (added || publishedGen_ != launchCheckedGen_) {
        launchCheckedGen_ = publishedGen_;
        if (plan && plan->rateEnabled)
            for (const model::Id track : tracksLosingLaunch(plan)) {
                const bool placedAlready = std::any_of(
                    returnPlaces_.begin(), returnPlaces_.end(), [&](const ReturnPlace& r) {
                        return r.track == track && r.generation == plan->generation;
                    });
                if (placedAlready) continue;
                // On the node's timeline, where its ack will be (round 176 F2).
                const detail::Node* n = nodeOf(*plan, track);
                const std::int64_t at = plan->handAt - (n ? n->timelineShift : 0);
                if (auto placed =
                        placeRateLaunch(*plan, plan->handEpoch, at, model::LaunchQuantize::None))
                    returnPlaces_.push_back(
                        {track, plan->generation, plan->handEpoch, at, *placed});
            }
    }
    // A launch ack lost to a full ring: the log misses a launch that was
    // heard, which no render can replay; a capture running, and every one
    // placed from now on, is not as heard (launch_unlogged).
    if (const auto dropped = launchAcksDropped_.load(std::memory_order_relaxed);
        dropped != launchAcksDropSeen_) {
        launchAcksDropSeen_ = dropped;
        launchAcksLost_ = true;
        if (capture_.on && !capture_.pending && !capture_.closed)
            capture_.facts.launchUnlogged = true;
        warnings_.push_back(
            "launch: the audio path's report of a launch was lost (its queue was full): the "
            "launch log misses a launch that was heard, so no capture from now on is as heard");
    }
    // A return ack lost to a full ring: each track the latest plan cannot
    // play is logged as returned where an immediate launch.arrangement
    // would land (the bound the control thread had before the acks), and a
    // capture running is not as heard (edited_during).
    if (const auto dropped = returnAcksDropped_.load(std::memory_order_relaxed);
        dropped != returnAcksDropSeen_) {
        returnAcksDropSeen_ = dropped;
        if (capture_.on && !capture_.pending && !capture_.closed) capture_.facts.edited = true;
        bool any = false;
        for (const model::Id track : tracksLosingLaunch(plan)) {
            const LaunchRequest back{track, LaunchLogEntry::Action::Arrangement, {}, {}};
            const auto placed = placeLaunch(std::span(&back, 1), model::LaunchQuantize::None);
            if (!placed) continue;
            LaunchLogEntry r;
            r.runningFrame = placed->stream;
            r.track = track;
            r.action = LaunchLogEntry::Action::Arrangement;
            r.anchorSample = placed->anchorSample;
            r.anchorBeat = placed->anchorBeat;
            r.songFrame = placed->songFrame;
            r.epoch = placed->epoch;
            launchLog_.push_back(r);
            launchIsReturn_.push_back(1);
            noteReturnGuess(r.epoch, r.songFrame);
            any = true;
        }
        if (any) sortLaunchLog();
        warnings_.push_back(
            "launch: the audio path's report of a return to the arrangement was lost (its queue "
            "was full): placed where an immediate launch.arrangement would land");
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
    placeLaunchesAgain();
    captureBeforeStretchCut();
}

void Engine::placeLaunchesAgain() {
    // Missed, or placed in an epoch that has ended: placed again, alone.
    const std::uint64_t epoch =
        playStart_ ? playStart_->first : epoch_.load(std::memory_order_acquire);
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
        // The newest heard entry (a return included); a newer one not heard
        // yet is what it has queued. A clip the plan lacks is not played: the
        // arrangement, until its executor's return is logged.
        const LaunchLogEntry* heard = nullptr;
        for (std::size_t i = launchLog_.size(); i-- > 0;) {
            const LaunchLogEntry& e = launchLog_[i];
            if (e.track != n.id) continue;
            if (e.epoch == epoch && e.runningFrame + n.timelineShift > played) {
                if (!tl.queued) {
                    LaunchPending q;
                    q.request = {e.track, e.action, e.clip, e.scene};
                    q.placed = {e.epoch, e.runningFrame, e.anchorSample, e.anchorBeat, e.songFrame};
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
        // What its clip follows by: the one it plays, else the one queued.
        const bool playsClip =
            tl.plays == TrackLaunch::Plays::Clip || tl.plays == TrackLaunch::Plays::Held;
        std::optional<model::Id> shown;
        if (playsClip)
            shown = tl.clip;
        else if (tl.queued && tl.queued->request.action == LaunchLogEntry::Action::Clip)
            shown = tl.queued->request.clip;
        if (const int k = shown ? detail::playerOf(n, *shown) : -1; k >= 0) {
            const detail::ClipPlayer& pl = n.players[static_cast<std::size_t>(k)];
            tl.followed = pl.followed == detail::FollowOutcome::Chords
                              ? TrackLaunch::Followed::Chords
                          : pl.followed == detail::FollowOutcome::Key ? TrackLaunch::Followed::Key
                                                                      : TrackLaunch::Followed::None;
            tl.followBudget = pl.followBudget;
        }
        out.push_back(std::move(tl));
    }
    return out;
}

std::optional<Engine::ClipFollowed> Engine::clipFollowed(model::Id track, model::Id clip) const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan) return std::nullopt;
    for (const detail::Node& n : plan->nodes) {
        if (n.id != track) continue;
        const int k = detail::playerOf(n, clip);
        if (k < 0) return std::nullopt;
        const detail::ClipPlayer& pl = n.players[static_cast<std::size_t>(k)];
        ClipFollowed out;
        out.followed = pl.followed == detail::FollowOutcome::Chords ? TrackLaunch::Followed::Chords
                       : pl.followed == detail::FollowOutcome::Key  ? TrackLaunch::Followed::Key
                                                                    : TrackLaunch::Followed::None;
        out.budget = pl.followBudget;
        return out;
    }
    return std::nullopt;
}

// --- Capturing a performance (ADR-0097 L4) -------------------------------------

// The song's running count (LaunchLogEntry::songFrame's unit) as `epoch`'s
// executor last published it with its heard position: the master playrate's
// timeline, or at unity the epoch's start plus its stream. Absent before the
// epoch has played.
std::optional<Engine::HeardSong> Engine::heardSong(std::uint64_t epoch) const {
    const auto words = readRateClock();
    if (words[8] != epoch || words[9] == 0) return std::nullopt;
    HeardSong out;
    out.song = std::bit_cast<double>(words[0]);  // PlayrateClock::State::timeline
    out.stream = std::bit_cast<std::int64_t>(words[7]);
    out.looping = words[12] != 0;
    out.loopStart = static_cast<std::int64_t>(std::bit_cast<double>(words[10]));
    out.loopEnd = static_cast<std::int64_t>(std::bit_cast<double>(words[11]));
    return out;
}

std::int64_t Engine::unitySongBase(const LaunchClock& clock) const {
    if (const auto heard = heardSong(clock.epoch))
        return static_cast<std::int64_t>(std::floor(heard->song)) - heard->stream;
    return clock.pos - clock.stream;  // nothing published yet: no wrap either
}

std::optional<model::Id> Engine::audioUnderStretch() const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !plan->rateStretch || !latest_) return std::nullopt;
    std::map<model::Id, std::size_t> last;
    for (std::size_t i = 0; i < launchLog_.size(); ++i) last[launchLog_[i].track] = i;
    for (const auto& [track, i] : last) {
        const LaunchLogEntry& e = launchLog_[i];
        if (e.action != LaunchLogEntry::Action::Clip) continue;  // returned: logged
        const model::Clip* clip = latest_->clips.find(e.clip);
        if (!clip || clip->kind == model::ClipKind::Midi) continue;
        // A clip played once sounds only in its pass from its anchor, which a
        // relocation keeps (round 113 T4, round 117 W3): finished once the
        // song heard now, in any epoch, is past that pass and no transport
        // loop brings it back. Its pass is the period the plan plays it by.
        if (clip->mode == model::ClipMode::Once)
            if (const auto heard = heardSong(readRateClock()[8])) {
                std::int64_t period = time::samplesAt(clip->length, config_.sampleRate);
                if (const detail::Node* n = nodeOf(*plan, track))
                    if (const int k = detail::playerOf(*n, e.clip); k >= 0)
                        period = n->players[static_cast<std::size_t>(k)].periodFrames;
                const auto song = static_cast<std::int64_t>(std::floor(heard->song));
                const bool loop = heard->looping && heard->loopEnd > heard->loopStart;
                const std::int64_t at =
                    loop && song >= heard->loopEnd
                        ? heard->loopStart +
                              (song - heard->loopStart) % (heard->loopEnd - heard->loopStart)
                        : song;
                const bool again = loop && e.anchorSample < heard->loopEnd &&
                                   e.anchorSample + period > heard->loopStart;
                if (!again && at >= e.anchorSample + period) continue;
            }
        return e.clip;
    }
    return std::nullopt;
}

bool Engine::cutsArrangementAudio(const LaunchRequest& r, std::int64_t anchor) const {
    if (!latest_) return false;
    // The stretch converter's reach either side of the switch: the history it
    // primes from and its lookahead (about 0.12 s measured at 0.75, round 113
    // T2). An item clear of it is neither read on past the cut nor primed
    // from. 0.25 s of song at most rate 1, scaled by the fastest master rate
    // the plan can play above 1: a reach in device time covers rate times as
    // much song (round 128 W1), and one in song time is covered either way.
    constexpr double kStretchReachSeconds = 0.25;
    double fastest = std::max(1.0, latest_->project->playrate.rate);
    if (const auto env = model::activePlayrateEnvelope(*latest_))
        for (const auto& pt : model::effectivePoints(*latest_, *env))
            fastest = std::max(fastest, pt.value);
    // A sync chase's scale is composed into the plan's scalar.
    if (const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire))
        fastest = std::max(fastest, plan->masterPlayrate.rate);
    fastest = std::min(fastest, model::kMaxPlayrate);
    const auto reach =
        static_cast<std::int64_t>(std::ceil(kStretchReachSeconds * fastest * config_.sampleRate));
    // The window around the switch on the timeline (`anchor`: its boundary,
    // not the unwrapped song frame, round 128 L1). Under a transport loop the
    // converter runs across the wrap, so a part of the window past the loop
    // end continues at its start, and a part before the start at its end.
    std::vector<std::pair<std::int64_t, std::int64_t>> windows{{anchor - reach, anchor + reach}};
    const std::int64_t loopStart = loopStart_.load(std::memory_order_relaxed);
    const std::int64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);
    if (loopOn_.load(std::memory_order_relaxed) && loopEnd > loopStart && anchor >= loopStart &&
        anchor <= loopEnd) {
        if (anchor + reach > loopEnd)
            windows.push_back({loopStart, loopStart + (anchor + reach - loopEnd)});
        if (anchor - reach < loopStart)
            windows.push_back({loopEnd - (loopStart - (anchor - reach)), loopEnd});
    }
    bool audio = false;
    for (const auto& e : latest_->items) {
        const model::Item& item = *e.value;
        if (item.track != r.track || !item.activeTake || item.mute) continue;
        const model::Take* take = latest_->takes.find(*item.activeTake);
        if (!take || !take->source || take->source->kind == model::Kind::Midi) continue;
        const model::ItemSpan span = model::itemSpan(*latest_, item);
        const double from = span.startSec * config_.sampleRate;
        const double to = span.endSec * config_.sampleRate;
        for (const auto& [a, b] : windows)
            if (from < double(b) && to > double(a)) audio = true;
        if (audio) break;
    }
    if (!audio) return false;
    // On the arrangement: no launch yet, or the newest returned it (a
    // plan's return included, once its executor acked it).
    bool arrangement = true;
    for (std::size_t i = launchLog_.size(); i-- > 0;)
        if (launchLog_[i].track == r.track) {
            arrangement = launchLog_[i].action == LaunchLogEntry::Action::Arrangement;
            break;
        }
    return arrangement != (r.action == LaunchLogEntry::Action::Arrangement);
}

void Engine::dropStaleCut() {
    // The switch that set an end ends it only while its launch stands: its
    // slot still holds it (the same generation, not voided or missed), or
    // it applied and is in the log. Replaced or voided before its boundary,
    // its end is dropped (round 117 W2).
    if (capture_.cutSlot == SIZE_MAX) return;
    const std::uint64_t st =
        (*launchSlots_)[capture_.cutSlot].state.load(std::memory_order_acquire);
    const std::uint64_t s = status(st);
    const bool holds =
        (st >> 3) == capture_.cutGeneration &&
        (s == LaunchSlot::kPending || s == LaunchSlot::kCommitted || s == LaunchSlot::kApplied);
    const bool logged =
        std::any_of(launchLog_.begin(), launchLog_.end(), [&](const LaunchLogEntry& e) {
            return e.epoch == capture_.epoch && e.track == capture_.cutTrack &&
                   e.songFrame == capture_.cutSong;
        });
    if (holds || logged) return;
    capture_.cutSlot = SIZE_MAX;
    settleCaptureEnd();
}

void Engine::captureBeforeStretchCut() {
    if (!capture_.on || capture_.pending || !latest_) return;
    auto& slots = *launchSlots_;
    dropStaleCut();
    // The stretch converter reads on past the switch live (its lookahead,
    // its history), where the commit cuts the item: up to 0.37 for tens of
    // ms in the test. Ended on the switch's boundary, the track never leaves
    // the arrangement within the capture, and what it holds is exact. The
    // earliest such switch queued sets it, with one warning.
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    const std::size_t before = capture_.cutSlot;
    const std::uint64_t beforeGeneration = capture_.cutGeneration;
    if (plan && plan->rateStretch)
        for (std::size_t i = 0; i < slots.size(); ++i) {
            LaunchSlot& s = slots[i];
            const std::uint64_t st = s.state.load(std::memory_order_acquire);
            if ((status(st) != LaunchSlot::kPending && status(st) != LaunchSlot::kCommitted) ||
                s.epoch.load() != capture_.epoch)
                continue;
            const std::int64_t song = s.songFrame.load();
            if (song <= capture_.startSong ||
                (capture_.cutSlot != SIZE_MAX && capture_.cutSong <= song))
                continue;
            // A launched audio clip reads through a stretch converter too,
            // whose history no committed item has (S3 step D, Q2 b).
            const model::Clip* launched = s.request.action == LaunchLogEntry::Action::Clip
                                              ? latest_->clips.find(s.request.clip)
                                              : nullptr;
            const bool audio = launched && launched->kind != model::ClipKind::Midi;
            if (!audio && !cutsArrangementAudio(s.request, s.anchorSample.load())) continue;
            capture_.cutAudio = audio;
            capture_.cutSlot = i;
            capture_.cutGeneration = st >> 3;
            capture_.cutTrack = s.request.track;
            capture_.cutSong = song;
            capture_.cutBeat =
                latest_->timeMap->beatAt(double(s.anchorSample.load()) / config_.sampleRate);
        }
    if (capture_.cutSlot != SIZE_MAX &&
        (capture_.cutSlot != before || capture_.cutGeneration != beforeGeneration))
        warnings_.push_back(
            "launch: the capture ends at " + capture_.cutTrack.str() + "'s switch: " +
            (capture_.cutAudio
                 ? std::string("it launches an audio clip, which a capture cannot write as heard "
                               "while the master playrate keeps the pitch")
                 : std::string("its arrangement plays audio there, which a capture cannot write as "
                               "heard where a switch cuts it while the master playrate keeps the "
                               "pitch")));
    settleCaptureEnd();
}

void Engine::settleCaptureEnd() {
    const bool cut = capture_.cutSlot != SIZE_MAX;
    capture_.ending = capture_.userEnding || cut;
    if (cut && (!capture_.userEnding || capture_.cutSong < capture_.userEndSong)) {
        capture_.endSong = capture_.cutSong;
        capture_.endBeat = capture_.cutBeat;
    } else if (capture_.userEnding) {
        capture_.endSong = capture_.userEndSong;
        capture_.endBeat = capture_.userEndBeat;
    }
}

std::int64_t Engine::captureHeard() const {
    const auto heard = heardSong(capture_.epoch);
    if (!heard) return capture_.startSong;
    return std::max(capture_.startSong, static_cast<std::int64_t>(std::floor(heard->song)));
}

void Engine::placeCapture() {
    if (!capture_.on || !capture_.pending) return;
    LaunchClock clock;
    if (!launchClock(clock)) return;   // relocating: the next service
    capture_.startSample = clock.pos;  // where it will begin, so far
    capture_.loop = clock.loop;
    std::int64_t song = clock.pos;  // at a play start, the epoch's first position
    bool fromPlayStart = true;
    if (clock.playing) {
        if (const auto heard = heardSong(clock.epoch)) {
            fromPlayStart = heard->stream == 0;
            // The position heard now, on the song's running count, and where
            // that is in the song (folded by the loop it runs under).
            song = static_cast<std::int64_t>(std::floor(heard->song));
            capture_.startSample = heard->looping && song >= heard->loopEnd
                                       ? heard->loopStart + (song - heard->loopStart) %
                                                                (heard->loopEnd - heard->loopStart)
                                       : song;
        } else if (clock.stream != 0) {
            return;  // the clock has not caught up with the epoch: the next service
        }
    } else if (clock.stream != 0) {
        return;  // stopped: it begins where playing starts
    }
    capture_.pending = false;
    capture_.placed = true;
    capture_.epoch = clock.epoch;
    capture_.startSong = song;
    capture_.version = latest_ ? latest_->version : 0;
    // As heard: where it began, and the counters now.
    capture_.facts = {};
    capture_.facts.fromPlayStart = fromPlayStart;
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    capture_.facts.liveInput = plan && plan->anyLive;
    capture_.facts.pluginFailed = pluginFailedNow();  // failed before it began (U1)
    capture_.facts.launchUnlogged = launchAcksLost_;
    // A guessed return still ahead: where it really was is not known.
    if (returnGuessEpoch_ == clock.epoch && returnGuessSong_ > song) capture_.facts.edited = true;
    capture_.misses = misses_.load(std::memory_order_relaxed);
    capture_.bridgeLate = bridgeLate_.load(std::memory_order_relaxed);
    capture_.xruns = backend_ ? backend_->status().xruns : 0;
    capture_.errors = processErrors_.load(std::memory_order_relaxed);
    capture_.closed = false;
}

Engine::CaptureFacts Engine::captureFacts() const {
    CaptureFacts f = capture_.facts;
    if (capture_.closed || capture_.pending) return f;
    f.dropout = f.dropout || misses_.load(std::memory_order_relaxed) != capture_.misses ||
                bridgeLate_.load(std::memory_order_relaxed) != capture_.bridgeLate ||
                (backend_ && backend_->status().xruns != capture_.xruns);
    f.pluginFailed = f.pluginFailed ||
                     processErrors_.load(std::memory_order_relaxed) != capture_.errors ||
                     pluginFailedNow();
    // A launch ack dropped since the last service (round 178 F3).
    f.launchUnlogged = f.launchUnlogged ||
                       launchAcksDropped_.load(std::memory_order_relaxed) != launchAcksDropSeen_;
    return f;
}

bool Engine::pluginFailedNow() const {
    // A failed bridged instance outputs silence and its process() answers
    // ok, so processErrors_ does not count it; nor a plug-in the pool
    // refused (round 174 U1). Either is not what an offline render's
    // healthy instance plays.
    if (!latest_) return false;
    for (const auto& [id, error] : plugins_.failedErrors())
        if (latest_->fx.find(id)) return true;
    return false;
}

void Engine::closeCapture() {
    if (capture_.pending || capture_.closed || !capture_.placed) return;
    capture_.facts = captureFacts();
    capture_.closed = true;
}

namespace {
bool playrateEnvelope(const model::Envelope& e) {
    return e.target.object == model::kMaster && e.target.param == "playrate";
}

// Whether the envelopes of two sessions that `pick` selects are the same.
template <class Pick>
bool sameEnvelopes(const model::Session& a, const model::Session& b, Pick pick) {
    if (a.envelopes.sameStorage(b.envelopes)) return true;
    std::size_t n = 0;
    for (const auto& e : a.envelopes) {
        if (!pick(*e.value)) continue;
        ++n;
        const model::Envelope* other = b.envelopes.find(e.id);
        if (!other || !(*other == *e.value)) return false;
    }
    for (const auto& e : b.envelopes)
        if (pick(*e.value)) {
            if (n == 0) return false;
            --n;
        }
    return n == 0;
}

// Whether two sessions are the same apart from the master playrate (its
// scalar, preserve pitch and envelopes). As rate_runtime.cpp's scalarOnly
// does, this names every Session member, so one added is a build error here
// until it says how it is compared; `context` and `contextView` are not
// compared: no plan reads them (ADR-0095).
bool sameApartFromPlayrate(const model::Session& a, const model::Session& b) {
    {
        [[maybe_unused]] const auto& [project, next, tempo, meters, timeMap, tracks, groups, items,
                                      takes, media, midi, fx, routes, envelopes, autoItems, markers,
                                      regions, racks, modules, cables, modulations, context,
                                      contextView, scenes, clips, blobs, extraSections, version] =
            a;
    }
    model::Project x = *a.project;
    x.playrate = b.project->playrate;
    return x == *b.project && a.tempo.sameStorage(b.tempo) && a.meters.sameStorage(b.meters) &&
           a.tracks.sameStorage(b.tracks) && a.groups.sameStorage(b.groups) &&
           a.items.sameStorage(b.items) && a.takes.sameStorage(b.takes) &&
           a.media.sameStorage(b.media) && a.midi.sameStorage(b.midi) && a.fx.sameStorage(b.fx) &&
           a.routes.sameStorage(b.routes) && a.autoItems.sameStorage(b.autoItems) &&
           a.markers.sameStorage(b.markers) && a.regions.sameStorage(b.regions) &&
           a.racks.sameStorage(b.racks) && a.modules.sameStorage(b.modules) &&
           a.cables.sameStorage(b.cables) && a.modulations.sameStorage(b.modulations) &&
           a.scenes.sameStorage(b.scenes) && a.clips.sameStorage(b.clips) && a.blobs == b.blobs &&
           a.extraSections == b.extraSections &&
           sameEnvelopes(a, b, [](const model::Envelope& e) { return !playrateEnvelope(e); });
}
}  // namespace

void Engine::captureEdit(const model::Session& before, const model::Session& after) {
    if (!capture_.on || capture_.pending || capture_.closed || &before == &after) return;
    if (!(before.project->playrate == after.project->playrate) ||
        !sameEnvelopes(before, after, playrateEnvelope))
        capture_.facts.rateChanged = true;
    if (!sameApartFromPlayrate(before, after)) capture_.facts.edited = true;
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (plan && plan->anyLive) capture_.facts.liveInput = true;
}

void Engine::captureRelocating() {
    captureEnding();  // a quantized end heard before the service: it ended there
    if (!capture_.on || capture_.pending) return;
    const std::int64_t heard = captureHeard();
    capture_.ending = capture_.userEnding = false;  // an end not reached: it ends here instead
    capture_.cutSlot = SIZE_MAX;
    if (heard > capture_.startSong) {  // it ran: running time ends here
        capture_.endSong = heard;
        capture_.on = false;
        closeCapture();
    } else {
        capture_.pending = true;  // nothing ran yet: it begins where the transport goes
    }
}

Status Engine::captureLaunches(bool on, std::optional<model::LaunchQuantize> quantize) {
    captureEnding();  // a quantized end heard before the service: it ended there
    if (on && capture_.on && capture_.userEnding) {  // on again before its end: it runs on
        capture_.userEnding = false;                 // (to a switch's end, if one is queued)
        settleCaptureEnd();
        return {};
    }
    if (on == capture_.on) return {};
    if (!on && quantize && *quantize != model::LaunchQuantize::None && !capture_.pending &&
        wantPlaying_.load(std::memory_order_acquire)) {
        // The next boundary, where a launch of every track with clips made
        // now would land: the same floor, so the two agree.
        const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
        std::vector<LaunchRequest> every;
        if (plan)
            for (const detail::Node& n : plan->nodes)
                if (!n.players.empty())
                    every.push_back({n.id, LaunchLogEntry::Action::Arrangement, {}, {}});
        auto placed = placeLaunch(every, *quantize);
        if (!placed) return std::move(placed).error();
        if (placed->epoch == capture_.epoch && placed->songFrame > capture_.startSong) {
            capture_.userEnding = true;
            capture_.userEndSong = placed->songFrame;
            // The beat of the sample it ends on, as the commit's `to` reads
            // it (the boundary's own beat when that is a whole sample).
            capture_.userEndBeat =
                latest_->timeMap->beatAt(double(placed->anchorSample) / config_.sampleRate);
            settleCaptureEnd();
            return {};
        }
        // A boundary in another epoch (a relocation is coming): it ends now.
    }
    if (on) {
        // Under a master playrate the capture runs in song time (S3 B); only
        // launched audio read while the pitch is kept cannot be committed as
        // heard (its stretch converter's history is not the timeline's).
        if (const auto clip = audioUnderStretch())
            return Error("launch.playrate",
                         clip->str() +
                             " is an audio clip playing while the master playrate keeps the "
                             "pitch, which a capture cannot write as heard",
                         "",
                         "stop it (launch.stop or launch.arrangement), or turn off the master "
                         "playrate's preserve pitch, then capture");
        capture_ = {};
        capture_.on = capture_.any = capture_.pending = true;
        placeCapture();
        return {};
    }
    capture_.endSong = capture_.pending ? capture_.startSong : captureHeard();
    closeCapture();
    capture_.pending = false;
    capture_.ending = capture_.userEnding = false;
    capture_.cutSlot = SIZE_MAX;
    capture_.on = false;
    return {};
}

void Engine::captureEnding() {
    if (capture_.on && !capture_.pending) dropStaleCut();
    if (!capture_.on || !capture_.ending || captureHeard() < capture_.endSong) return;
    capture_.ending = capture_.userEnding = false;  // heard its boundary: it ends there
    capture_.cutSlot = SIZE_MAX;
    capture_.on = false;
    closeCapture();
}

std::optional<Engine::LaunchCapture> Engine::launchCapture() {
    if (!capture_.any) return std::nullopt;
    serviceLaunches();  // what has applied so far is in the log
    placeCapture();
    LaunchCapture out;
    out.on = capture_.on;
    out.placed = capture_.placed;
    out.committedIn = capture_.committedIn;
    out.version = capture_.version;
    out.startSample = capture_.startSample;
    out.loop = capture_.loop;
    if (capture_.pending) return out;
    out.facts = captureFacts();
    // Running time on the song's count (S3 B): the same as stream frames at
    // unity, song samples under a master playrate.
    const std::int64_t start = capture_.startSong;
    const std::int64_t end = !capture_.on      ? capture_.endSong
                             : capture_.ending ? std::min(captureHeard(), capture_.endSong)
                                               : captureHeard();
    out.frames = end - start;
    if (capture_.on && capture_.ending) {
        out.endsFrame = capture_.endSong - start;
        out.endsBeat = capture_.endBeat;
    }
    // What each track played as the capture began: its newest entry by
    // then, a plan's return included (one after it, such as a clip removed
    // later, is a launch within).
    std::map<model::Id, std::size_t> before;
    for (std::size_t i = 0; i < launchLog_.size(); ++i) {
        const LaunchLogEntry& e = launchLog_[i];
        if (e.epoch < capture_.epoch || (e.epoch == capture_.epoch && e.songFrame <= start))
            before[e.track] = i;
    }
    for (const auto& [track, i] : before) {
        LaunchLogEntry first = launchLog_[i];
        if (first.action == LaunchLogEntry::Action::Arrangement) continue;
        first.runningFrame = 0;
        first.epoch = capture_.epoch;
        out.launches.push_back(first);
    }
    // Then each launch within it, in frame order: the log's, where a return
    // comes first at a frame it shares (a launch there came after it).
    const std::size_t firsts = out.launches.size();
    const auto within = [&](const LaunchLogEntry& e) {
        if (e.epoch != capture_.epoch || e.songFrame <= start || e.songFrame >= end) return;
        LaunchLogEntry in = e;
        in.runningFrame = e.songFrame - start;
        out.launches.push_back(in);
    };
    for (const LaunchLogEntry& e : launchLog_) within(e);
    std::stable_sort(out.launches.begin() + static_cast<std::ptrdiff_t>(firsts), out.launches.end(),
                     [](const LaunchLogEntry& a, const LaunchLogEntry& b) {
                         return a.runningFrame < b.runningFrame;
                     });
    return out;
}

void Engine::ackReturns(detail::Plan& next, int liveClass, bool renderer, std::uint64_t epoch,
                        std::int64_t stream) SUPPLE_RT {
    bool any = false;
    for (detail::Node& n : next.nodes) {
        if (!n.launchReturned || (liveClass >= 0 && n.live != (liveClass == 1))) continue;
        n.launchReturned = false;
        LaunchAck ack;
        ack.track = n.id.n;
        ack.epoch = epoch;
        ack.action = static_cast<std::uint8_t>(LaunchLogEntry::Action::Arrangement);
        // On the node's timeline, as a launch's target is (a latency-delayed
        // node switches its input timelineShift frames behind the stream:
        // round 176 F2).
        ack.stream = stream - n.timelineShift;
        ack.returned = true;
        ack.generation = next.generation;
        if (!(renderer ? rendererLaunchAcks_ : callbackLaunchAcks_)->push(ack))
            returnAcksDropped_.fetch_add(1, std::memory_order_relaxed);
        any = true;
    }
    if (any) notifyMain();
}

void Engine::ackClassMoves(const detail::Plan& old, const detail::Plan* skipped,
                           const detail::Plan& next, bool carried, std::uint64_t epoch,
                           std::int64_t stream) SUPPLE_RT {
    // `old` is the callback's previous plan: its live nodes are the
    // callback's own, and the renderer left its rendered ones (and those of
    // a skipped plan) when it adopted a newer plan, before this handoff was
    // published.
    bool any = false;
    for (const detail::Node& n : next.nodes) {
        const detail::Node* o = detail::findNode(old, n.id);
        if (!o) continue;
        const detail::Node* before = o;  // the state that restarts
        if (o->live) {
            if (n.live) continue;  // adoptLive carries the callback's own on
        } else {
            if (skipped) {
                before = detail::findNode(*skipped, n.id);
                if (!before || before->skipLive) continue;  // ackSkipped judged it
            }
            if (!n.live) continue;  // the renderer's, rendered throughout
            if (carried) continue;  // the renderer carried it (round 178 F1)
        }
        if (before->launch.kind == detail::LaunchState::Kind::Arrangement) continue;
        LaunchAck ack;
        ack.track = n.id.n;
        ack.epoch = epoch;
        ack.action = static_cast<std::uint8_t>(LaunchLogEntry::Action::Arrangement);
        ack.stream = stream - n.timelineShift;
        ack.returned = true;
        ack.generation = next.generation;
        if (!callbackLaunchAcks_->push(ack))
            returnAcksDropped_.fetch_add(1, std::memory_order_relaxed);
        any = true;
    }
    if (any) notifyMain();
}

void Engine::ackSkipped(detail::Plan& skipped) SUPPLE_RT {
    // Only skipLive is written here: the renderer may still read the rest
    // of the plan.
    bool any = false;
    for (detail::Node& n : skipped.nodes) {
        const detail::Node* o = livePlan_ ? detail::findNode(*livePlan_, n.id) : nullptr;
        const detail::Node* before = classBase_ ? detail::findNode(*classBase_, n.id) : o;
        const bool judged = classBase_ && before && before->skipLive;
        n.skipLive = n.live || judged;
        if (!o || o->live || !n.live || judged || !before) continue;
        if (before->launch.kind == detail::LaunchState::Kind::Arrangement) continue;
        LaunchAck ack;
        ack.track = n.id.n;
        ack.epoch = skipped.handEpoch;
        ack.action = static_cast<std::uint8_t>(LaunchLogEntry::Action::Arrangement);
        ack.stream = skipped.handAt - n.timelineShift;
        ack.returned = true;
        ack.generation = skipped.generation;
        if (!callbackLaunchAcks_->push(ack))
            returnAcksDropped_.fetch_add(1, std::memory_order_relaxed);
        any = true;
    }
    if (any) notifyMain();
}

void Engine::noteReturnGuess(std::uint64_t epoch, std::int64_t song) {
    if (epoch != returnGuessEpoch_) {
        returnGuessEpoch_ = epoch;
        returnGuessSong_ = song;
    } else {
        returnGuessSong_ = std::max(returnGuessSong_, song);
    }
}

void Engine::sortLaunchLog() {
    // By epoch and frame; at a shared frame a return first, else as drained.
    std::vector<std::size_t> order(launchLog_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const LaunchLogEntry &x = launchLog_[a], &y = launchLog_[b];
        if (x.epoch != y.epoch) return x.epoch < y.epoch;
        if (x.runningFrame != y.runningFrame) return x.runningFrame < y.runningFrame;
        return launchIsReturn_[a] > launchIsReturn_[b];
    });
    std::vector<LaunchLogEntry> log;
    std::vector<std::uint8_t> returns;
    log.reserve(order.size());
    returns.reserve(order.size());
    for (const std::size_t i : order) {
        log.push_back(launchLog_[i]);
        returns.push_back(launchIsReturn_[i]);
    }
    launchLog_ = std::move(log);
    launchIsReturn_ = std::move(returns);
}

std::vector<model::Id> Engine::tracksLosingLaunch(const detail::Plan* plan) const {
    std::map<model::Id, std::size_t> last;
    for (std::size_t i = 0; i < launchLog_.size(); ++i) last[launchLog_[i].track] = i;
    std::vector<model::Id> out;
    for (const auto& [track, i] : last) {
        const LaunchLogEntry& e = launchLog_[i];
        if (e.action == LaunchLogEntry::Action::Arrangement) continue;
        const detail::Node* n = plan ? nodeOf(*plan, track) : nullptr;
        if (!n || n->players.empty() ||
            (e.action == LaunchLogEntry::Action::Clip && detail::playerOf(*n, e.clip) < 0))
            out.push_back(track);
    }
    return out;
}

void Engine::logReturn(const LaunchAck& ack) {
    const model::Id track{model::Kind::Track, static_cast<std::uint32_t>(ack.track)};
    const ReturnKey key{track, ack.epoch, ack.generation};
    if (std::find(returnKeys_.begin(), returnKeys_.end(), key) != returnKeys_.end()) {
        ++returnAcksDuplicated_;  // never expected: the carries are disjoint by class
        return;
    }
    returnKeys_.push_back(key);
    LaunchLogEntry r;
    r.runningFrame = ack.stream;
    r.track = track;
    r.action = LaunchLogEntry::Action::Arrangement;
    r.epoch = ack.epoch;
    // Its plan: at unity the song moves with the stream; under a playrate
    // its handAt was placed when it was published (serviceLaunches).
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    for (const auto& candidate : plans_)
        if (candidate->generation == ack.generation) plan = candidate.get();
    const auto place = std::find_if(
        returnPlaces_.begin(), returnPlaces_.end(),
        [&](const ReturnPlace& p) { return p.track == track && p.generation == ack.generation; });
    LaunchClock clock;
    if (place != returnPlaces_.end() && place->epoch == ack.epoch && place->stream == ack.stream) {
        r.anchorSample = place->placed.anchorSample;
        r.anchorBeat = place->placed.anchorBeat;
        r.songFrame = place->placed.songFrame;
    } else if (plan && !plan->rateEnabled && latest_ && launchClock(clock) &&
               clock.epoch == ack.epoch) {
        r.songFrame = unitySongBase(clock) + ack.stream;
        // The song sample there: folded by the transport loop, as placeLaunch
        // folds an immediate launch's.
        const std::int64_t d = ack.stream - clock.stream;
        std::int64_t at = d >= 0 ? songAfter(clock.loop, clock.pos, d) : clock.pos + d;
        if (d < 0 && clock.loop.active() && clock.pos >= clock.loop.start &&
            clock.pos < clock.loop.end && at < clock.loop.start)
            at += clock.loop.end - clock.loop.start;
        r.anchorSample = std::max<std::int64_t>(at, 0);
        r.anchorBeat = latest_->timeMap->beatAt(double(r.anchorSample) / config_.sampleRate);
    } else {
        // Neither known (a playrate plan placed too late, or no clock in its
        // epoch): where an immediate launch.arrangement would land.
        const LaunchRequest back{track, LaunchLogEntry::Action::Arrangement, {}, {}};
        if (const auto placed = placeLaunch(std::span(&back, 1), model::LaunchQuantize::None)) {
            r.anchorSample = placed->anchorSample;
            r.anchorBeat = placed->anchorBeat;
            r.songFrame = placed->songFrame + (ack.stream - placed->stream);
        } else {
            r.songFrame = ack.stream;
        }
        noteReturnGuess(r.epoch, r.songFrame);
    }
    std::erase_if(returnPlaces_, [&](const ReturnPlace& p) {
        return p.track == track && p.generation <= ack.generation;
    });
    // A return comes from a plan compiled for an edit (or a change of the
    // prepared sources): a capture running is not as heard (edited_during).
    // The replay itself plays a logged return as heard (round 176 F3).
    if (capture_.on && !capture_.pending && !capture_.closed) capture_.facts.edited = true;
    launchLog_.push_back(r);
    launchIsReturn_.push_back(1);
}

std::vector<model::Id> Engine::clipsSkipped() const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !plan->preparedSources) return {};
    return plan->preparedSources->clipsSkipped;
}

}  // namespace supple::engine
