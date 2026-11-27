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

Result<Engine::LaunchPlaced> Engine::launchInto(LaunchSlot& slot) {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan)
        return Error("launch.engine", "no project is loaded in the engine", "",
                     "start the engine with a project first");
    const LaunchRequest r = slot.request;
    if (auto refused = launchRefused(*plan, r)) return std::move(*refused);
    for (int attempt = 0;; ++attempt) {
        bool missed = false;
        auto placed = publishLaunch(std::span(&r, 1), slot.quantize, missed, &slot);
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

// A slot's lifetime (round 205): free, or applied, every executor done
// with it and its switch heard: in its epoch, once the callback has played
// past its key; after it, by its verdict. Its log entries are then settled
// as heard (publishLaunch), so none is judged against a reused slot; one
// not heard is voided and placed again instead (takeBackUnheard).
bool Engine::slotReusable(const LaunchSlot& s, std::uint64_t st, std::uint64_t current,
                          const detail::Plan* plan) const {
    if (status(st) == LaunchSlot::kFree) return true;
    const std::uint8_t need = s.required.load();
    if (status(st) != LaunchSlot::kApplied || (s.done.load() & need) != need) return false;
    const std::uint64_t e = s.epoch.load();
    if (e == current) return playedStream(e) > slotKey(s, plan);
    return e < current && s.verdict.load(std::memory_order_acquire) == 1;
}

bool Engine::replaced(const LaunchSlot& s) const {
    const auto it = launchNewest_.find(s.track.load());
    return it != launchNewest_.end() && s.order < it->second;
}

void Engine::freeLaunchSlot(LaunchSlot& s, std::uint64_t st) {
    if (s.state.compare_exchange_strong(st, next(st, LaunchSlot::kFree)) &&
        (status(st) == LaunchSlot::kPending || status(st) == LaunchSlot::kCommitted))
        launchesQueued_.fetch_sub(1);
}

Result<Engine::LaunchPlaced> Engine::publishLaunch(std::span<const LaunchRequest> requests,
                                                   model::LaunchQuantize quantize, bool& missed,
                                                   LaunchSlot* into) {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    auto placed = placeLaunch(requests, quantize);
    if (!placed) return placed;
    // Every ack in first, so each applied slot's log entries exist before a
    // slot is chosen (round 205 F2), and what the executors have judged
    // unheard taken back and voided (round 207 F1: with no service between,
    // a track's launches still do not pile up).
    if (readLaunchAcks()) sortLaunchLog();
    if (!into) takeBackUnheard();
    const std::uint64_t current = epoch_.load(std::memory_order_acquire);
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
        const auto spent = [&](const LaunchSlot& s, std::uint64_t st) {
            return slotReusable(s, st, current, plan);
        };
        LaunchSlot* slot = nullptr;
        std::uint64_t st = 0;
        if (into) {  // placed again: its own slot
            st = into->state.load();
            if (into->state.compare_exchange_strong(st, next(st, LaunchSlot::kFree))) slot = into;
        }
        for (int tries = 0; !into && !slot && tries < 16; ++tries) {
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
        if (slot && status(st) == LaunchSlot::kApplied) {
            // Heard (spent): its entries are judged, once.
            const int index = static_cast<int>(slot - &slots[0]);
            for (LogMeta& m : launchMeta_)
                if (m.slot == index && m.generation == (st >> 3)) m.settled = true;
        }
        if (!slot)
            return Error("launch.queue", "too many launches are waiting", "",
                         "wait for the queued launches to play");
        if (status(st) == LaunchSlot::kPending || status(st) == LaunchSlot::kCommitted)
            launchesQueued_.fetch_sub(1);
        if (!into) {
            // The track's newest; any other launch still waiting on it is
            // replaced, as its waiting slot is (round 207 F1).
            slot->order = ++launchOrderNext_;
            launchNewest_[r.track.n] = slot->order;
            for (LaunchSlot& other : slots) {
                if (&other == slot || other.track.load() != r.track.n) continue;
                const std::uint64_t now = other.state.load();
                const auto o = status(now);
                if (o == LaunchSlot::kPending || o == LaunchSlot::kMissed ||
                    o == LaunchSlot::kCommitted || o == LaunchSlot::kVoided)
                    freeLaunchSlot(other, now);
            }
        }
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
        slot->key.store(INT64_MIN, std::memory_order_relaxed);
        slot->keyAtSwitch.store(false, std::memory_order_relaxed);
        slot->verdict.store(0, std::memory_order_relaxed);
        slot->left.store(0, std::memory_order_relaxed);
        slot->anchorSample.store(placed->anchorSample, std::memory_order_relaxed);
        slot->songFrame.store(placed->songFrame, std::memory_order_relaxed);
        slot->anchorBeat.store(placed->anchorBeat, std::memory_order_relaxed);
        slot->playStart.store(!wantPlaying_.load(std::memory_order_acquire),
                              std::memory_order_relaxed);
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
            if (slot->playStart.load(std::memory_order_relaxed)) playStartMissed(0);
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
        // Waiting to switch: pending or committed, missed or voided (placed
        // again at the next play), or applied in an ended epoch past where
        // its hearing ended, to be taken back (ADR-0097, revision
        // 2026-11-26: launch.state says such a launch is queued).
        const auto st = status(s.state.load(std::memory_order_acquire));
        // (An applied launch an executor has yet to switch is not listed: the
        // renderer holds before a launch it has targets of, and the callback
        // plays only what it rendered (round 209), so none is heard first;
        // one on the callback's targets alone plays at once. Round 214.)
        if (st != LaunchSlot::kPending && st != LaunchSlot::kCommitted &&
            st != LaunchSlot::kMissed && st != LaunchSlot::kVoided && !unheardApplied(s))
            continue;
        if (replaced(s)) continue;  // not placed again (round 207 F1)
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

std::int64_t Engine::launchShift(const detail::Plan* plan, model::Id track) {
    if (!plan) return 0;
    for (const detail::Node& n : plan->nodes)
        if (n.id == track && !n.players.empty()) return n.timelineShift;
    return 0;
}

void Engine::noteHeardEnd(std::uint64_t left, std::uint64_t entered, std::int64_t end) SUPPLE_RT {
    // The epoch left, and each one skipped since (never played: nothing heard).
    for (std::uint64_t e = left; e < entered && e - left < heardEnds_.size(); ++e) {
        HeardEnd& h = heardEnds_[e % heardEnds_.size()];
        h.epoch.store(UINT64_MAX, std::memory_order_release);  // being written
        h.end.store(e == left ? end : 0, std::memory_order_release);
        h.epoch.store(e, std::memory_order_release);
    }
    heardEndsEntered_.store(entered, std::memory_order_release);
}

std::optional<std::int64_t> Engine::heardEnd(std::uint64_t epoch) const SUPPLE_RT {
    const HeardEnd& h = heardEnds_[epoch % heardEnds_.size()];
    if (h.epoch.load(std::memory_order_acquire) != epoch) return std::nullopt;
    const std::int64_t end = h.end.load(std::memory_order_acquire);
    if (h.epoch.load(std::memory_order_acquire) != epoch) return std::nullopt;  // rewritten
    return end;
}

void Engine::takeBackNodes(detail::Plan& p, int liveClass, std::uint64_t epoch,
                           std::int64_t end) SUPPLE_RT {
    for (detail::Node& n : p.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        if (n.launchUndoCount == 0) continue;
        // Each record judged once, here: by its slot's verdict, or, applied
        // as a relocation in this epoch, by its frame against the end.
        for (std::size_t i = 0; i < n.launchUndoCount; ++i) {
            detail::LaunchUndo& u = n.launchUndos[i];
            if (u.epoch != epoch) continue;
            // A slot reused since was spent heard (a launch's lifetime rule:
            // its slot is free again only once its switch is known heard), so
            // its record stays, whatever the end says (round 209: an end
            // unknown after more epochs than the ring holds took back a
            // heard switch, and the node played what the log did not).
            const LaunchSlot* slot = u.slot >= 0 && std::size_t(u.slot) < launchSlots_->size()
                                         ? &(*launchSlots_)[std::size_t(u.slot)]
                                         : nullptr;
            if (u.relocated || !slot)
                u.undo = u.key >= end;
            else if ((slot->state.load(std::memory_order_acquire) >> 3) != u.generation)
                u.undo = false;
            else
                u.undo = !slotHeard(*slot, &p, end);
        }
        bool inexact = false;
        if (detail::takeBackLaunches(n, epoch, end, &inexact) && inexact) {
            launchUndoLost_.fetch_add(1, std::memory_order_relaxed);
            notifyMain();
        }
    }
}

void Engine::markLeft(std::uint8_t mine, std::uint64_t left, std::uint64_t entered) SUPPLE_RT {
    bool marked = false;
    for (LaunchSlot& s : *launchSlots_) {
        if (status(s.state.load(std::memory_order_acquire)) != LaunchSlot::kApplied) continue;
        const std::uint64_t e = s.epoch.load(std::memory_order_relaxed);
        if (e >= left && e < std::max(entered, left + 1)) {
            s.left.fetch_or(mine, std::memory_order_acq_rel);
            marked = true;
        }
    }
    if (marked) notifyMain();
}

bool Engine::slotHeard(const LaunchSlot& s, const detail::Plan* plan,
                       std::int64_t end) const SUPPLE_RT {
    const std::uint8_t verdict = s.verdict.load(std::memory_order_acquire);
    if (verdict != 0) return verdict == 1;
    return slotKey(s, plan) < end;
}

void Engine::leaveLaunchEpoch(std::uint8_t mine, const detail::Plan* plan, std::uint64_t left,
                              std::int64_t end, bool judge, std::uint64_t entered) SUPPLE_RT {
    for (LaunchSlot& s : *launchSlots_) {
        if (status(s.state.load(std::memory_order_acquire)) != LaunchSlot::kApplied) continue;
        const std::uint64_t e = s.epoch.load(std::memory_order_relaxed);
        if (judge && e >= left && (e == left || e < entered)) {
            // The verdict, once (round 203 F1): by the end where the callback
            // played, unheard in an epoch it skipped.
            std::uint8_t none = 0;
            const std::uint8_t v = e == left && slotKey(s, plan) < end ? 1 : 2;
            (void)s.verdict.compare_exchange_strong(none, v, std::memory_order_acq_rel);
        }
        if (e != left) continue;
        if (!slotHeard(s, plan, end)) launchDone(s, mine);
    }
}

std::int64_t Engine::slotKey(const LaunchSlot& s, const detail::Plan* plan) SUPPLE_RT {
    if (const std::int64_t key = s.key.load(std::memory_order_acquire); key != INT64_MIN)
        return key;
    const model::Id track{model::Kind::Track,
                          static_cast<std::uint32_t>(s.track.load(std::memory_order_relaxed))};
    return s.target.load(std::memory_order_relaxed) + launchShift(plan, track);
}

bool Engine::settleLeave(detail::Plan& p, int liveClass, std::int64_t stream) SUPPLE_RT {
    // The test hook holds the leave late: the callback's end not known yet.
    if (rendererLeaveLateForTests_.load(std::memory_order_relaxed)) return false;
    const std::uint64_t left = launchLeavePending_;
    if (heardEndsEntered_.load(std::memory_order_acquire) <= left) return false;  // not yet
    // The slots' verdicts are in; the end serves the relocations' records.
    auto end = heardEnd(left);
    if (!end) {
        end = INT64_MIN;  // reused before read: unheard, counted
        launchUnsettled_.fetch_add(1, std::memory_order_relaxed);
    }
    launchLeavePending_ = UINT64_MAX;
    launchLeaveUndos_ = false;
    launchLeftEpoch_[0] = left;
    launchLeftEnd_[0] = *end;
    leaveLaunchEpoch(1, &p, left, *end);
    takeBackNodes(p, liveClass, left, *end);
    markLeft(1, left, left + 1);
    launchLateEnd_.store(stream, std::memory_order_release);
    // (No locate here: takeBackLaunches relocates each node it restores, and
    // a renderer settling before it rendered anything since its locate, as
    // at a play start it held for, has nothing else to clear; round 211 F2.)
    return true;
}

void Engine::passedPending(int path) SUPPLE_RT {
    launchPassedPending_[std::size_t(path)].fetch_add(1, std::memory_order_relaxed);
    launchUnsettled_.fetch_add(1, std::memory_order_relaxed);  // the capture is not as heard
    notifyMain();
}

void Engine::playStartMissed(int path) SUPPLE_RT {
    launchPlayStartMissed_[std::size_t(path)].fetch_add(1, std::memory_order_relaxed);
    launchUnsettled_.fetch_add(1, std::memory_order_relaxed);  // the capture is not as heard
    notifyMain();
}

bool Engine::launchHeldByLeave(detail::Plan& p, std::uint64_t epoch, std::int64_t stream,
                               std::int64_t end) SUPPLE_RT {
    if (launchLeavePending_ == UINT64_MAX || settleLeave(p, 0, stream)) return false;
    if (launchesQueued_.load() == 0) return false;
    for (const LaunchSlot& slot : *launchSlots_) {
        const std::uint64_t was = status(slot.state.load());
        if ((was != LaunchSlot::kPending && was != LaunchSlot::kCommitted &&
             was != LaunchSlot::kApplied) ||
            slot.epoch.load(std::memory_order_relaxed) != epoch ||
            (slot.done.load(std::memory_order_acquire) & 1) != 0)
            continue;
        const model::Id track{model::Kind::Track, static_cast<std::uint32_t>(
                                                      slot.track.load(std::memory_order_relaxed))};
        const model::Id clip{model::Kind::Clip,
                             static_cast<std::uint32_t>(slot.clip.load(std::memory_order_relaxed))};
        const auto kind = launchKind(
            static_cast<LaunchLogEntry::Action>(slot.action.load(std::memory_order_relaxed)));
        const std::int64_t target = slot.target.load(std::memory_order_relaxed);
        bool held = false;
        // Its first frame on this executor's targets, its early halves
        // included (as launchesAt finds it).
        forEachTarget(p, 0, track, [&](detail::Node& n, detail::NotesFeed* feed) {
            const int player =
                kind == detail::LaunchState::Kind::Clip ? detail::playerOf(n, clip) : -1;
            const std::int64_t at = target + n.timelineShift;
            const std::int64_t first =
                std::min(earlyFrame(at, feed ? 0 : detail::launchEarly(n, kind, player)),
                         earlyFrame(at, feed ? 0 : n.launchLead));
            if (first < end) held = true;
        });
        if (held) return true;
    }
    return false;
}

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
    // The renderer's leave of an epoch the callback had not left (its wait
    // timed out) waits for the callback's end: until then nothing applies
    // and nothing is taken back (round 201 F2), and such a block does not
    // count as run (what it passed it did not switch: round 207). Since
    // round 209 the renderer holds before the first frame of any launch of
    // its epoch it has targets of and has not switched (launchHeldByLeave),
    // and the callback, playing only what was rendered, cannot apply one
    // before it; a passed launch is counted (launchPassedPending_) and marks
    // the capture (round 214).
    if (renderer && launchLeavePending_ != UINT64_MAX && !settleLeave(p, liveClass, stream)) {
        // Rendering with switches the listener may not have heard still in
        // place: these frames are not as heard (round 209: marked here, as
        // they are rendered, not at the relocation).
        if (launchLeaveUndos_ && launchLateEpoch_.load(std::memory_order_relaxed) != epoch) {
            launchLateEnd_.store(INT64_MAX, std::memory_order_release);
            launchLateEpoch_.store(epoch, std::memory_order_release);
            launchUnsettled_.fetch_add(1, std::memory_order_relaxed);
            notifyMain();
        }
        // Passing a launch of this epoch it has not switched, on a target of
        // its own: these frames lack it (switched once settled, late), so a
        // capture over them is not as heard (round 207). Unreachable since
        // the hold (round 209): counted should a path reach it (round 214).
        if (launchesQueued_.load() != 0 && launchPassedEpoch_ != epoch)
            for (const LaunchSlot& slot : *launchSlots_) {
                const std::uint64_t was = status(slot.state.load());
                if ((was != LaunchSlot::kCommitted && was != LaunchSlot::kApplied) ||
                    slot.epoch.load(std::memory_order_relaxed) != epoch ||
                    (slot.done.load(std::memory_order_acquire) & mine) == mine)
                    continue;
                const model::Id track{
                    model::Kind::Track,
                    static_cast<std::uint32_t>(slot.track.load(std::memory_order_relaxed))};
                const std::int64_t target = slot.target.load(std::memory_order_relaxed);
                bool passing = false;
                forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed*) {
                    if (target + n.timelineShift < stream + k) passing = true;
                });
                if (!passing) continue;
                launchPassedEpoch_ = epoch;
                launchLateEnd_.store(INT64_MAX, std::memory_order_release);
                launchLateEpoch_.store(epoch, std::memory_order_release);
                passedPending(0);
                break;
            }
        return k;
    }
    const bool lastValid = launchLastValid_[c];
    const std::uint64_t lastEpoch = launchLastEpoch_[c];
    const std::int64_t lastStream = launchLastStream_[c];
    // A call again at the same stream (a block that did not advance) has
    // switched what was due there already.
    const bool repeat = lastValid && lastEpoch == epoch && lastStream == stream;
    launchLastValid_[c] = true;
    launchLastEpoch_[c] = epoch;
    launchLastStream_[c] = stream;
    // A new epoch for this executor: what it applied in the one it left and
    // the listener did not hear goes back (ADR-0097, dated 2026-11-26).
    // (Each executor takes back at its leave of an epoch: round 203 F1.)
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
        // Its switch frame, as heard (the track's channel's shift): what each
        // target records to take back an unheard switch (LaunchUndo::key),
        // set in the slot by the first executor to apply it and read by all
        // (round 201 F2).
        std::int64_t key = slotKey(slot, &p);
        const int slotIndex = static_cast<int>(&slot - &(*launchSlots_)[0]);
        // Switches this class's targets that `when` accepts. `relocated`: at
        // its first block after leaving the slot's epoch, as a relocation.
        const auto apply = [&](auto when, bool relocated) SUPPLE_RT {
            // The key as of the switch (round 203 F3): set again from the
            // shift this executor switches with; another found by the other
            // executor's switch marks the capture.
            if (!relocated && when(target + launchShift(&p, track))) {
                const std::int64_t atSwitch = target + launchShift(&p, track);
                if (!slot.keyAtSwitch.exchange(true, std::memory_order_acq_rel)) {
                    // An early half under another shift took it at a frame
                    // this switch does not match: not called heard exactly.
                    if (const std::int64_t early =
                            slot.key.exchange(atSwitch, std::memory_order_acq_rel);
                        early != INT64_MIN && early != atSwitch) {
                        launchUnsettled_.fetch_add(1, std::memory_order_relaxed);
                        notifyMain();
                    }
                } else if (slot.key.load(std::memory_order_acquire) != atSwitch) {
                    launchUnsettled_.fetch_add(1, std::memory_order_relaxed);
                    notifyMain();
                }
                key = slot.key.load(std::memory_order_acquire);
            }
            // A relocation's record is judged by where it applied in this
            // epoch (its first frame here).
            const std::int64_t recordKey = relocated ? stream : key;
            const auto note = [&](detail::Node& n, int feedIndex) SUPPLE_RT {
                detail::LaunchUndo* u =
                    detail::noteLaunchUndo(n, feedIndex, recordKey, epoch, slotIndex);
                if (u) {
                    u->relocated = relocated;
                    u->generation = slot.state.load(std::memory_order_acquire) >> 3;
                }
                return u;
            };
            forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed* feed) {
                if (!when(target + n.timelineShift)) return;
                if (feed) {
                    (void)note(n, static_cast<int>(feed - n.rack->notes.data()));
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
                detail::LaunchUndo* undo = note(n, -1);
                if (undo) undo->rotated = true;
                detail::applyLaunch(n, a, undo ? &undo->warmSwapped : nullptr);
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
                ack.key = key;
                ack.slot = slotIndex;
                ack.slotGeneration = slot.state.load(std::memory_order_acquire) >> 3;
                if ((renderer && dropRendererAcksForTests_.load(std::memory_order_relaxed)) ||
                    !(renderer ? rendererLaunchAcks_ : callbackLaunchAcks_)->push(ack)) {
                    launchAcksDropped_.fetch_add(1, std::memory_order_relaxed);
                    // Judged by the main thread once it knows whether the
                    // switch was heard (judgeAckDrops).
                    if (!launchAckDrops_[c]->push({slotEpoch, key}))
                        launchAckDropsUnknown_.fetch_add(1, std::memory_order_relaxed);
                }
                notifyMain();
            });
        };
        if (slotEpoch != epoch) {
            // Applied in an epoch this executor has left, or never ran: what
            // it had not switched there switches now.
            if (was == LaunchSlot::kApplied && slotEpoch < epoch) {
                // Unless its switch was not heard: then this executor took
                // back what it had applied (takeBackNodes), and the main
                // thread places it again (ADR-0097, dated 2026-11-26).
                // By its verdict (round 203 F1), which the callback wrote
                // as it left that epoch; the renderer, ahead of it, waits.
                const std::uint8_t verdict = slot.verdict.load(std::memory_order_acquire);
                if (verdict == 0 && renderer) continue;
                if (verdict != 1) {
                    launchDone(slot, mine);
                    continue;
                }
                const bool ran = lastValid && lastEpoch == slotEpoch;
                apply([&](std::int64_t at) { return !ran || at > lastStream; }, true);
                launchDone(slot, mine);
            }
            continue;  // pending or committed: the main thread places it again
        }
        bool any = false, passed = false, due = false, lacking = false, allPassed = true;
        std::int64_t first = INT64_MAX, after = INT64_MAX;
        forEachTarget(p, liveClass, track, [&](detail::Node& n, detail::NotesFeed* feed) {
            const std::int64_t at = target + n.timelineShift;
            if (at >= stream) allPassed = false;
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
        // Applied by the other executor while this one passed its frames
        // without looking (the renderer, its leave of the epoch before held
        // late until the callback's end is known): switched now, at this
        // block, as a relocation switches (playing by its phase from here,
        // as the other executor's targets do), so no target of it is left
        // behind (round 207). Unreachable since the hold (round 209):
        // counted, and the capture marked (round 214).
        if (was == LaunchSlot::kApplied && allPassed && !due) {
            apply([&](std::int64_t at) { return at < stream; }, true);
            launchDone(slot, mine);
            passedPending(1);
            continue;
        }
        if (was == LaunchSlot::kPending) {
            // Undecided at (or past) a frame of its: never taken there.
            if ((passed || due) &&
                slot.state.compare_exchange_strong(st, with(st, LaunchSlot::kMissed))) {
                launchesQueued_.fetch_sub(1, std::memory_order_acq_rel);
                if (slot.playStart.load(std::memory_order_relaxed)) playStartMissed(1);
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
                    if (passed && slot.playStart.load(std::memory_order_relaxed))
                        playStartMissed(2);
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
            // The first executor to apply it sets the key all record.
            std::int64_t unset = INT64_MIN;
            if (!slot.key.compare_exchange_strong(unset, key, std::memory_order_acq_rel))
                key = unset;
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
                // Taken back unheard: only what this early half sets.
                if (detail::LaunchUndo* undo =
                        detail::noteLaunchUndo(n, -1, key, epoch, slotIndex)) {
                    undo->early = true;
                    undo->generation = slot.state.load(std::memory_order_acquire) >> 3;
                }
                if (chain) {
                    detail::prelaunch(n, a);
                    n.launchNextKey = key;
                }
                if (warm) {
                    detail::prewarm(n, a, target);  // the bare target (Node::launchWarmAt)
                    n.launchWarmKey = key;
                }
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

void Engine::readHeardEnds() const {
    // Each epoch the callback has left, once (round 203 F1): its end as the
    // ring holds it, or none when the ring entry was reused before a service
    // read it.
    const std::uint64_t entered = heardEndsEntered_.load(std::memory_order_acquire);
    for (; judgedUpTo_ < entered; ++judgedUpTo_) judgedEnds_[judgedUpTo_] = heardEnd(judgedUpTo_);
}

std::optional<std::optional<std::int64_t>> Engine::judgedEnd(std::uint64_t epoch) const {
    readHeardEnds();
    const auto it = judgedEnds_.find(epoch);
    if (it == judgedEnds_.end()) return std::nullopt;  // not judged yet
    return it->second;
}

bool Engine::unheardApplied(const LaunchSlot& s) const {
    if (status(s.state.load()) != LaunchSlot::kApplied) return false;
    const std::uint64_t e = s.epoch.load();
    if (e >= epoch_.load(std::memory_order_acquire)) return false;  // its epoch runs on
    // The callback's verdict, once written; until then undecided.
    const std::uint8_t verdict = s.verdict.load(std::memory_order_acquire);
    return verdict != 1;
}

std::optional<bool> Engine::entryHeard(std::size_t i) const {
    const LogMeta& m = launchMeta_[i];
    // A slot's launch: the callback's verdict on the slot, while the slot
    // still holds it (it is not reused before its entries are judged:
    // launch's `spent`).
    if (m.slot >= 0) {
        const LaunchSlot& slot = (*launchSlots_)[std::size_t(m.slot)];
        if ((slot.state.load(std::memory_order_acquire) >> 3) == m.generation) {
            const std::uint8_t verdict = slot.verdict.load(std::memory_order_acquire);
            if (verdict == 0) return std::nullopt;
            return verdict == 1;
        }
    }
    // A return (or a slot reused, never expected): by its epoch's judged end;
    // one lost before any service read it is unheard, counted.
    const auto judged = judgedEnd(launchLog_[i].epoch);
    if (!judged) return std::nullopt;
    if (!*judged) {
        launchUnsettled_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return m.key < **judged;
}

void Engine::takeBackUnheard() {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    const std::uint64_t current = epoch_.load(std::memory_order_acquire);
    // The log: what an ended epoch logged at or past its heard end.
    std::vector<model::Id> returned;
    bool removed = false;
    for (std::size_t i = launchLog_.size(); i-- > 0;) {
        const LaunchLogEntry& e = launchLog_[i];
        // Judged once (round 203 F1): an entry found heard is never looked
        // at again.
        if (e.epoch >= current || launchMeta_[i].settled) continue;
        const std::optional<bool> heard = entryHeard(i);
        if (!heard) continue;  // not yet
        if (*heard) {
            launchMeta_[i].settled = true;
            continue;
        }
        if (launchMeta_[i].isReturn) returned.push_back(e.track);
        launchLog_.erase(launchLog_.begin() + static_cast<std::ptrdiff_t>(i));
        launchMeta_.erase(launchMeta_.begin() + static_cast<std::ptrdiff_t>(i));
        removed = true;
    }
    // A return comes from a plan, which the stop does not take back: the
    // track is on the arrangement when the transport goes on, so its return
    // is logged at the start of the epoch it goes on in (where an immediate
    // launch.arrangement lands then; a guess once that epoch has played), in
    // the track's own frames as its executor's ack there would be: the
    // stream less its timeline shift, so it reads as in effect from the
    // epoch's first frame.
    for (const model::Id track : returned) {
        const LaunchRequest back{track, LaunchLogEntry::Action::Arrangement, {}, {}};
        const auto placed = placeLaunch(std::span(&back, 1), model::LaunchQuantize::None);
        if (!placed) continue;
        LaunchLogEntry r;
        r.runningFrame = -launchShift(plan, track);
        r.track = track;
        r.action = LaunchLogEntry::Action::Arrangement;
        r.anchorSample = placed->anchorSample;
        r.anchorBeat = placed->anchorBeat;
        r.songFrame = placed->songFrame - placed->stream;
        r.epoch = placed->epoch;
        if (placed->stream != 0) noteReturnGuess(r.epoch, r.songFrame);
        launchLog_.push_back(r);
        launchMeta_.push_back({.isReturn = true, .key = 0});  // the epoch's first frame
    }
    if (removed) sortLaunchLog();
    // The judged ends no entry, dropped ack or capture still needs.
    std::uint64_t needed = std::min(judgedUpTo_, capture_.epoch);
    for (std::size_t i = 0; i < launchLog_.size(); ++i)
        if (!launchMeta_[i].settled) needed = std::min(needed, launchLog_[i].epoch);
    for (const AckDrop& d : launchAckDropsWaiting_) needed = std::min(needed, d.epoch);
    judgedEnds_.erase(judgedEnds_.begin(), judgedEnds_.lower_bound(needed));
    // The slots: voided once every executor is done with them (each took
    // back its own targets), then placed again in the epoch running now, as
    // a launch committed and not applied is (placeLaunchesAgain).
    for (LaunchSlot& s : *launchSlots_) {
        std::uint64_t st = s.state.load(std::memory_order_acquire);
        if (status(st) != LaunchSlot::kApplied || !unheardApplied(s)) continue;
        if (s.verdict.load(std::memory_order_acquire) == 0) continue;  // undecided yet
        const std::uint8_t need = s.required.load();
        if ((s.done.load(std::memory_order_acquire) & need) != need) continue;  // next service
        // Nor reused before every executor has left its epoch and taken
        // back by its verdict (a renderer's late leave settles later).
        if ((s.left.load(std::memory_order_acquire) & need) != need) continue;
        (void)s.state.compare_exchange_strong(st, with(st, LaunchSlot::kVoided));
    }
}

void Engine::judgeAckDrops() {
    // A verdict that came late or was taken as unheard (round 201 F2): the
    // capture there is not as heard (launch_unsettled).
    if (const auto unsettled = launchUnsettled_.load(std::memory_order_relaxed);
        unsettled != launchUnsettledSeen_) {
        launchUnsettledSeen_ = unsettled;
        if (capture_.any) capture_.facts.launchUnsettled = true;
    }
    for (auto& ring : launchAckDrops_) {
        AckDrop d;
        while (ring->pop(d)) launchAckDropsWaiting_.push_back(d);
    }
    const std::uint64_t current = epoch_.load(std::memory_order_acquire);
    const auto heard = [&](const AckDrop& d) -> std::optional<bool> {
        if (d.epoch == current) {  // heard once the callback passed it
            if (playedStream(current) > d.frame) return true;
            return std::nullopt;
        }
        const auto judged = judgedEnd(d.epoch);
        if (!judged) return std::nullopt;  // not yet
        if (!*judged) return true;         // lost: as heard (counted unlogged)
        return d.frame < **judged;
    };
    bool lost = false;
    std::erase_if(launchAckDropsWaiting_, [&](const AckDrop& d) {
        const auto h = heard(d);
        if (!h) return false;
        lost = lost || *h;
        return true;
    });
    if (const auto unknown = launchAckDropsUnknown_.load(std::memory_order_relaxed);
        unknown != launchAckDropsUnknownSeen_) {
        launchAckDropsUnknownSeen_ = unknown;
        lost = true;
    }
    if (!lost) return;
    launchAcksLost_ = true;
    if (capture_.on && !capture_.pending && !capture_.closed) capture_.facts.launchUnlogged = true;
    warnings_.push_back(
        "launch: the audio path's report of a launch was lost (its queue was full): the "
        "launch log misses a launch that was heard, so no capture from now on is as heard");
}

void Engine::serviceLaunches() {
    if (readLaunchAcks()) sortLaunchLog();  // the two executors' acks interleave
    // What an ended epoch applied and its listener did not hear (ADR-0097,
    // dated 2026-11-26).
    takeBackUnheard();
    serviceLaunchesRest();
}

bool Engine::readLaunchAcks() {
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
            launchMeta_.push_back(
                {.key = ack.key, .slot = ack.slot, .generation = ack.slotGeneration});
        }
    }
    if (added) launchAcksAdded_ = true;  // for the next service's check below
    return added;
}

void Engine::serviceLaunchesRest() {
    if (const auto n = launchTakeBackUnknown_.load(std::memory_order_relaxed);
        n != launchTakeBackUnknownSeen_) {
        launchTakeBackUnknownSeen_ = n;
        warnings_.push_back(
            "launch: an executor could not tell whether a launch was heard "
            "before a stop or locate, and kept it");
    }
    if (const auto n = launchUndoLost_.load(std::memory_order_relaxed); n != launchUndoLostSeen_) {
        launchUndoLostSeen_ = n;
        warnings_.push_back(
            "launch: more launches than a channel can take back were applied "
            "ahead of the listener before a stop or locate; the oldest stayed");
    }
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
    if (std::exchange(launchAcksAdded_, false) || publishedGen_ != launchCheckedGen_) {
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
    // A launch ack lost to a full ring: once its switch is known to have
    // been heard, the log misses a launch, which no render can replay; a
    // capture running, and every one placed from now on, is not as heard
    // (launch_unlogged). One for a launch not heard is nothing lost: that
    // launch is taken back (ADR-0097, dated 2026-11-26).
    launchAcksDropSeen_ = launchAcksDropped_.load(std::memory_order_relaxed);
    judgeAckDrops();
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
            launchMeta_.push_back(
                {.isReturn = true, .key = r.runningFrame + launchShift(plan, track)});
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
    // In the order they were placed (epoch, then switch frame): a later
    // launch taken back with an earlier one on its track replaces it again,
    // as it did when it was published (ADR-0097, dated 2026-11-26).
    std::array<LaunchSlot*, kLaunchSlots> order{};
    for (std::size_t i = 0; i < kLaunchSlots; ++i) order[i] = &(*launchSlots_)[i];
    std::stable_sort(order.begin(), order.end(), [](const LaunchSlot* a, const LaunchSlot* b) {
        const auto ea = a->epoch.load(), eb = b->epoch.load();
        return ea != eb ? ea < eb : a->target.load() < b->target.load();
    });
    for (LaunchSlot* slot : order) {
        LaunchSlot& s = *slot;
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
        // Replaced by a newer launch on its track: dropped (round 207 F1).
        if (replaced(s)) {
            freeLaunchSlot(s, st);
            continue;
        }
        const LaunchRequest r = s.request;
        auto placed = launchInto(s);
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
            // An ended epoch's launch its listener did not hear is queued
            // too: it is taken back and placed again (ADR-0097, dated
            // 2026-11-26).
            // By the entry's own key (round 203 F3); an ended epoch's entry
            // once judged heard stays so, and one whose end was lost is not.
            bool unheard = false;
            if (e.epoch == epoch) {
                unheard = launchMeta_[i].key > played;
            } else if (!launchMeta_[i].settled) {
                const std::optional<bool> verdict = entryHeard(i);
                unheard = verdict && !*verdict;
            }
            if (unheard) {
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
    std::int64_t stream = 0;
    bool fromPlayStart = true;
    if (clock.playing) {
        if (const auto heard = heardSong(clock.epoch)) {
            fromPlayStart = heard->stream == 0;
            stream = heard->stream;
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
    capture_.startStream = stream;
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
    // A launch ack dropped since the last service (round 178 F3), or one
    // whose switch is not known yet to have been heard (ADR-0097, dated
    // 2026-11-26): counted until it is judged.
    f.launchUnlogged = f.launchUnlogged ||
                       launchAcksDropped_.load(std::memory_order_relaxed) != launchAcksDropSeen_ ||
                       !launchAckDropsWaiting_.empty();
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
    // A launch it lists from an epoch that has ended while the callback has
    // not left it yet, at or past what was played there: whether it was heard
    // is not known, so the capture is not as heard (round 201,
    // launch_unsettled; its key carries the channel's shift, as the
    // take-back's does).
    if (capture_.epoch < epoch_.load(std::memory_order_acquire) && !judgedEnd(capture_.epoch)) {
        const std::int64_t played = playedStream(capture_.epoch);
        for (std::size_t i = 0; i < launchLog_.size(); ++i) {
            const LaunchLogEntry& e = launchLog_[i];
            if (e.epoch == capture_.epoch && e.songFrame < end && launchMeta_[i].key >= played)
                out.facts.launchUnsettled = true;
        }
    }
    // Over the frames a renderer rendered after a late leave, before it
    // settled, which may still hold an unheard switch (round 203 F2).
    if (capture_.epoch == launchLateEpoch_.load(std::memory_order_acquire) &&
        capture_.startStream < launchLateEnd_.load(std::memory_order_acquire))
        out.facts.launchUnsettled = true;
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
        ack.key = ack.stream + n.timelineShift;  // as heard (LaunchSlot::key)
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
        ack.key = ack.stream + n.timelineShift;  // as heard (LaunchSlot::key)
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
        ack.key = ack.stream + n.timelineShift;  // as heard (LaunchSlot::key)
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
        return launchMeta_[a].isReturn > launchMeta_[b].isReturn;
    });
    std::vector<LaunchLogEntry> log;
    std::vector<LogMeta> meta;
    log.reserve(order.size());
    meta.reserve(order.size());
    for (const std::size_t i : order) {
        log.push_back(launchLog_[i]);
        meta.push_back(launchMeta_[i]);
    }
    launchLog_ = std::move(log);
    launchMeta_ = std::move(meta);
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
    launchMeta_.push_back({.isReturn = true, .key = ack.key});
}

std::vector<model::Id> Engine::clipsSkipped() const {
    const detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (!plan || !plan->preparedSources) return {};
    return plan->preparedSources->clipsSkipped;
}

}  // namespace supple::engine
