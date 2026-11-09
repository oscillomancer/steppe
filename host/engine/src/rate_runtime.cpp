// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/realtime.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>

#include "plan.h"

namespace supple::engine {
namespace {
using Clock = detail::PlayrateClock;
using Timeline = detail::PlayrateTimeline;
static_assert(sizeof(Clock::State) == sizeof(std::array<std::uint64_t, 6>));
}  // namespace

std::array<std::uint64_t, 13> Engine::readRateClock() const {
    std::array<std::uint64_t, 13> words{};
    for (;;) {
        const auto seq = rateClockSequence_.load(std::memory_order_acquire);
        if (seq & 1) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = rateClockWords_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq == rateClockSequence_.load(std::memory_order_relaxed)) return words;
    }
}

void Engine::publishRateClock(detail::Plan& p, std::int64_t device, std::int64_t stream,
                              std::uint64_t epoch, bool live) SUPPLE_RT {
    if (p.rateEnabled && (p.timelines[0]->scalarLate.load(std::memory_order_acquire) ||
                          p.timelines[1]->scalarLate.load(std::memory_order_acquire)))
        notifyMain();
    Clock::State clock;
    auto looping = loopC_.active() && rateEpochStart_ < double(loopC_.end);
    if (p.rateEnabled) {
        const auto snapshot = p.timelines[live ? 1 : 0]->snapshot(device);
        clock = snapshot.clock;
        looping = snapshot.looping;
    } else
        clock.timeline = rateEpochStart_ + double(stream);
    const auto packed = std::bit_cast<std::array<std::uint64_t, 6>>(clock);
    std::array<std::uint64_t, 13> words{};
    std::copy(packed.begin(), packed.end(), words.begin());
    words[6] = std::bit_cast<std::uint64_t>(device);
    words[7] = std::bit_cast<std::uint64_t>(stream);
    words[8] = epoch;
    words[9] = p.generation;
    words[10] = std::bit_cast<std::uint64_t>(double(loopC_.start));
    words[11] = std::bit_cast<std::uint64_t>(double(loopC_.end));
    words[12] = looping ? 1 : 0;
    if (rateDeferredWake_.load(std::memory_order_acquire) &&
        (epoch != rateSafeEpoch_.load(std::memory_order_acquire) ||
         stream + (options_.anticipative
                       ? aheadFrames_
                       : static_cast<std::int64_t>(config_.sampleRate * options_.renderAheadMs /
                                                   1000.)) >=
             rateSafeStream_.load(std::memory_order_acquire))) {
        if (rateDeferredWake_.exchange(false)) notifyMain();
    }
    const auto seq = rateClockSequence_.load(std::memory_order_relaxed);
    rateClockSequence_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < words.size(); ++i)
        rateClockWords_[i].store(words[i], std::memory_order_relaxed);
    rateClockSequence_.store(seq + 2, std::memory_order_release);
}

namespace {
// Persistent collections make scalar-only detection independent of track count.
// The whole project compares equal apart from the scalar rate, so a field added
// to model::Project can never pass as a scalar edit. Keep the collections in
// step with model::Session.
bool scalarOnly(const model::Session& a, const model::Session& b) {
    model::Project x = *a.project;
    x.playrate.rate = b.project->playrate.rate;
    return x == *b.project && a.tempo.sameStorage(b.tempo) && a.meters.sameStorage(b.meters) &&
           a.tracks.sameStorage(b.tracks) && a.groups.sameStorage(b.groups) &&
           a.items.sameStorage(b.items) && a.takes.sameStorage(b.takes) &&
           a.media.sameStorage(b.media) && a.midi.sameStorage(b.midi) && a.fx.sameStorage(b.fx) &&
           a.routes.sameStorage(b.routes) && a.envelopes.sameStorage(b.envelopes) &&
           a.autoItems.sameStorage(b.autoItems) && a.markers.sameStorage(b.markers) &&
           a.regions.sameStorage(b.regions) && a.blobs == b.blobs &&
           a.extraSections == b.extraSections;
}
}  // namespace

bool Engine::loadScalarRate(const model::Session& session, const std::filesystem::path& dir) {
    auto* p = latestPlan_.load(std::memory_order_acquire);
    if (!backend_ || !latest_ || !p || !p->rateEnabled || rateSeekTarget_ || rateLoopTarget_ ||
        notificationReload_ || rateDeferred_ || dir != p->projectDir ||
        !scalarOnly(*latest_, session))
        return false;
    const auto value = session.project->playrate.rate;
    // An unchanged session can be an explicit maintenance reload. Only value
    // edits (or retrying a coalesced value) qualify for the control shortcut.
    if (!scalarPending_ && value == latest_->project->playrate.rate) return false;
    // A rate-enabled plan glides through exactly 1.0 like any other value (a
    // MIDI knob's centre lands there), on the converters it was compiled with.
    // A preserve-pitch plan compiled at exactly 1.0 reads its sources as tape
    // (exact at unity, plan.cpp): leaving 1.0 needs the stretch converter, so
    // that prepares a plan, as leaving the unity bypass does.
    if (value != 1 && p->masterPlayrate.preservePitch && p->masterPlayrate.rate == 1 &&
        p->timelines[0]->points().empty())
        return false;
    if (!std::isfinite(value) || value < .25 || value > 4) return false;
    if (value != p->requestedScalar) {
        // Keep controls newer than the consumed anchor for structural prediction.
        const auto words = readRateClock();
        const auto consumed = std::bit_cast<std::int64_t>(words[6]);
        if (words[9] == p->generation)
            std::erase_if(p->scalarHistory,
                          [&](const auto& old) { return old.device <= consumed; });
        // A bounded queue never loses the final request: defer/coalesce only
        // on saturation, and retry from service once output advances.
        if (p->timelines[0]->scalarEdits.free() == 0 || p->timelines[1]->scalarEdits.free() == 0 ||
            p->scalarHistory.size() == 1024) {
            latest_ = std::make_shared<const model::Session>(session);
            rateDeferredDir_ = dir;
            scalarPending_ = true;
            rateDeferredWake_.store(true);
            return true;
        }
        const auto sequence = p->scalarPublication->load(std::memory_order_relaxed) + 1;

        // History storage is reserved before plan publication. Only fixed
        // queue writes and the commit follow this frontier read.
        auto frontier =
            std::max({p->timelines[0]->frontier(), p->timelines[1]->frontier(), p->rateSafeDevice});
        // A renderer behind its render-ahead target may catch up past a
        // frontier read now before it takes the edit, which would arrive late
        // and force a recovery seek. Place the edit beyond everything it may
        // render ahead of what the callback last played: the same point as
        // the frontier when the renderer is caught up.
        if (options_.anticipative && words[8] == epoch_.load(std::memory_order_acquire))
            frontier = std::max(
                frontier, consumed + aheadFrames_ + static_cast<std::int64_t>(p->block) +
                              p->rateDeviceMargin + static_cast<std::int64_t>(p->rate * .15) + 128);
        // A callback can free a whole device block at once. Allow one such
        // refill burst (and at least two canonical quanta) before the edit.
        const auto headroom = std::max({p->block, config_.blockSize, 128u});
        const auto device = (frontier + headroom + 63) / 64 * 64;
        // Each move ramps over half as long again as the audio played since
        // the previous one (at least one device block, at most 300 ms), so the
        // steps of a mouse drag or a MIDI knob join into one continuous ramp
        // that follows the hand, instead of short jumps with holds between
        // them: the next move normally arrives mid-ramp and continues from
        // there. A move after a pause takes the longest ramp. Played audio,
        // unlike the render frontier, advances with the device. The rate then
        // follows that chain of ramps through a 150 ms low-pass, which rounds
        // off the corners where one ramp meets the next: smoothness matters
        // more here than response.
        const auto epoch = epoch_.load(std::memory_order_acquire);
        const auto heard = playedStream(epoch);
        const auto longest =
            std::max<std::int64_t>(detail::scalarRampFrames(p->rate), p->smoothFrames);
        const auto shortest = std::min<std::int64_t>(
            longest, std::max<std::int64_t>(p->smoothFrames, config_.blockSize));
        std::int64_t ramp = longest;
        if (scalarHeardEpoch_ == epoch && heard >= scalarHeard_)
            ramp = std::clamp((heard - scalarHeard_) * 3 / 2, shortest, longest);
        scalarHeard_ = heard;
        scalarHeardEpoch_ = epoch;
        const auto smoothing = detail::scalarSmoothingFrames(p->rate);
        const detail::ScalarRateEdit edit{device, value, sequence, static_cast<std::uint32_t>(ramp),
                                          smoothing};
        p->scalarHistory.push_back(edit);
        p->timelines[0]->scalarEdits.push(edit);
        p->timelines[1]->scalarEdits.push(edit);
        p->scalarPublication->store(sequence, std::memory_order_release);
        p->requestedScalar = value;
    }
    scalarPending_ = false;
    latest_ = std::make_shared<const model::Session>(session);
    wakeRenderer();
    return true;
}

Status Engine::prepareRatePlan(detail::Plan& p) {
    const auto transport = transportSnapshot();
    p.handEpoch = transport.epoch + (rateSeekTarget_ ? 1 : 0);
    p.rateSeekSequence = rateSeekTarget_ ? transport.seekSeq + 1 : 0;
    const auto words = readRateClock();
    Timeline::Snapshot seed;
    std::array<std::uint64_t, 6> packed{};
    std::copy_n(words.begin(), 6, packed.begin());
    seed.clock = words[9] == 0 ? Clock::State{} : std::bit_cast<Clock::State>(packed);
    seed.device = std::bit_cast<std::int64_t>(words[6]);
    seed.loopStart = std::bit_cast<double>(words[10]);
    seed.loopEnd = std::bit_cast<double>(words[11]);
    seed.looping = words[12] != 0;
    std::int64_t baseStream = std::bit_cast<std::int64_t>(words[7]);
    detail::Plan* previous = nullptr;
    for (const auto& candidate : plans_)
        if (candidate->generation == words[9]) previous = candidate.get();
    // Prepared trajectories form an ordered control history even before the
    // listener reaches them. Replacing pending plans would debounce a drag.
    if (ratePreparedTail_ && ratePreparedTail_->preparedRateBoundary.epoch == transport.epoch &&
        ratePreparedTail_->generation > words[9]) {
        previous = ratePreparedTail_;
        seed = previous->rateControl[0];
        baseStream = previous->preparedRateBoundary.stream;
    }
    p.rateFromGeneration = previous ? previous->generation : 0;
    p.rateFromUnity = !previous || !previous->rateEnabled;
    const bool playing = previous && wantPlaying_.load(std::memory_order_acquire);
    const LoopRange loop = rateLoopTarget_.value_or(transport.loop);
    const auto heard = playedStream(transport.epoch);
    const auto frontier = [&] {
        const auto consumed = playedStream(transport.epoch);
        return options_.anticipative &&
                       epochDone_.load(std::memory_order_acquire) == transport.epoch
                   ? std::max(consumed, renderedStream_.load(std::memory_order_acquire))
                   : consumed;
    };
    // Rate-enabled render passes stop on the 64-frame grid even when the
    // graph's allocated block is larger. Unity still uses full render blocks.
    const auto granularity =
        previous && previous->rateEnabled
            ? 64u
            : (options_.anticipative ? options_.renderBlock : config_.blockSize);
    std::int64_t handStream = heard;
    std::int64_t handDevice = seed.device;
    if (rateSeekTarget_) {
        handStream = 0;
        handDevice = static_cast<std::int64_t>(std::llround(*rateSeekTarget_));
    } else if (playing) {
        const auto lead =
            options_.anticipative
                ? aheadFrames_
                : static_cast<std::int64_t>(config_.sampleRate * options_.renderAheadMs / 1000.);
        const auto preparation = static_cast<std::int64_t>(std::ceil(
            (backend_->ownsThread() ? ratePreparationSeconds_ : 0) * config_.sampleRate * 2));
        handStream = std::max({heard + std::max<std::int64_t>(lead, 64) + 64,
                               frontier() + preparation + 2 * granularity + 64, baseStream + 64});
        handStream = (handStream + 63) / 64 * 64;
        handDevice = seed.device + handStream - baseStream;
    }
    p.handAt = handStream;
    Status result;
    try {
        if (rateSeekTarget_ || !playing) {
            const double timeline =
                rateSeekTarget_ ? double(*rateSeekTarget_) : seed.clock.timeline;
            result = detail::prepareRateSources(p, handDevice, timeline, &loop);
            p.rateOrigin = handDevice - handStream;
        } else {
            const auto clockClass = options_.anticipative ? 1u : 0u;
            const auto points = previous && previous->rateEnabled
                                    ? previous->timelines[clockClass]->points()
                                    : std::vector<detail::RatePoint>{};
            Timeline old(p.timelines[0]->capacity(), seed.clock.target, points);
            old.seed(seed);
            if (previous)
                for (const auto& edit : previous->scalarHistory)
                    if (edit.device > seed.device) old.scalarEdits.push(edit);
            if (previous && previous->rateEnabled)
                old.inheritControls(*previous->timelines[clockClass]);
            if (previous && previous->rateEnabled) {
                const auto past = seed.device - p.rateDeviceMargin - 64;
                if (!old.copyPast(*previous->timelines[clockClass], past, seed.device)) {
                    result = Error("playrate.history",
                                   "the changed graph needs unavailable clock history", {},
                                   "prepare a fresh seek at the consumed position");
                    return result;
                }
            }
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto began = std::chrono::steady_clock::now();
                old.ensure(handDevice + p.rateDeviceMargin +
                           static_cast<std::int64_t>(p.rate * .15) + p.block + 128);
                // The legacy unity executor wraps its device coordinate;
                // the rate executor keeps it monotonic from adoption on.
                if (attempt == 0 && (!previous || !previous->rateEnabled)) {
                    const auto wrapped =
                        static_cast<std::int64_t>(std::llround(old.position(handDevice)));
                    const auto offset = wrapped - handDevice;
                    handDevice = wrapped;
                    old.seed(seed, offset);
                    old.ensure(handDevice + p.rateDeviceMargin +
                               static_cast<std::int64_t>(p.rate * .15) + p.block + 128);
                }
                p.rateOrigin = handDevice - handStream;
                result = detail::configureRateTransition(p, previous, old, handDevice);
                const auto elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                ratePreparationSeconds_ = std::max(elapsed, ratePreparationSeconds_ * .9);
                if (!result || frontier() + granularity < handStream) break;
                // Priming must finish before its scheduled boundary. A busy
                // machine can need more than the ordinary render horizon;
                // predict farther ahead and prepare again while old audio runs.
                const auto lead = static_cast<std::int64_t>(std::ceil(
                                      (backend_->ownsThread() ? elapsed : 0) * p.rate * 2)) +
                                  2 * granularity + 128;
                const auto oldHand = handStream;
                handStream = (frontier() + lead + 63) / 64 * 64;
                handDevice += handStream - oldHand;
                p.handAt = handStream;
                if (attempt == 2)
                    result = Error("playrate.prepare_late",
                                   "source preparation could not reach a future boundary", {},
                                   "prepare a fresh seek at the consumed position");
            }
        }
        p.rateHandDevice = handDevice;
        p.preparedRateBoundary = {handStream, p.handEpoch};
    } catch (const std::exception& error) {
        return Error("playrate.prepare", error.what(), {},
                     "reduce source/FX count and retry the transport request");
    }
    return result;
}

}  // namespace supple::engine
