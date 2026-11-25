// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The engine tests' friend probe (Engine declares `friend struct EngineTestAccess`). One
// definition for every file of engine.tests, so the struct is identical wherever it is
// included (test_realtime.cpp, test_launch_access.cpp).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <supple/base/rt.h>
#include <supple/engine/realtime.h>

#include "live_workers.h"
#include "plan.h"
#include "recording.h"

namespace supple::engine {
// A main-thread transport write stalled halfway (preempted after the odd
// sequence store), and the executors' view of the epoch.
struct EngineTestAccess {
    static void outputConsume(Engine& e, std::uint64_t epoch, std::int64_t stream) SUPPLE_RT {
        e.rtEpoch_ = epoch;
        e.rtStream_.store(stream);
        e.rtPosEpoch_.store(epoch);
        e.wakeOutputConsumed();
    }
    static std::uint64_t outputSeen(const Engine& e) { return e.outputWakeSeen_; }
    static std::uint64_t outputSerial(const Engine& e) { return e.outputWakeSerial_.load(); }
    static void liveHooks(Engine& e, detail::LiveWorkers::Hooks hooks) {
        e.liveWorkersReady_.store(nullptr);
        e.liveWorkers_ = std::make_unique<detail::LiveWorkers>(3, hooks);
        e.liveWorkersReady_.store(e.liveWorkers_.get());
    }
    static std::size_t plans(const Engine& e) { return e.plans_.size(); }
    static void secondCompile(Engine& e, void (*change)(detail::Plan&)) {
        e.secondCompileForTests_ = change;
    }
    static void betweenCompiles(Engine& e, void (*hook)(void*), void* context) {
        e.betweenCompilesForTests_ = hook;
        e.betweenCompilesContext_ = context;
    }
    static std::uint32_t launchesQueued(const Engine& e) { return e.launchesQueued_.load(); }
    static void beforeLaunchCommit(Engine& e, void (*hook)(void*), void* context) {
        e.launchCommitForTests_ = hook;
        e.launchCommitContext_ = context;
    }
    // The next `attempts` prepareRatePlan attempts count as late: each
    // prepares again with the hand later, so S later; three fail, and the
    // recovery seek prepares the plan once more.
    static void lateAttempts(Engine& e, int attempts) { e.lateAttemptsForTests_ = attempts; }
    // The newest prepared trajectory change's switch S as a stream frame
    // (INT64_MIN: none, or a seek). Control thread.
    static std::int64_t preparedSwitch(const Engine& e) {
        const detail::Plan* p = e.ratePreparedTail_;
        if (!p || p->rateSwitchDevice == INT64_MIN) return INT64_MIN;
        return p->rateSwitchDevice - p->rateOrigin;
    }
    static void afterCollect(Engine& e, void (*hook)(void*), void* context) {
        e.afterCollectForTests_ = hook;
        e.afterCollectContext_ = context;
    }
    // Every instance a kept plan's stages name (pointers only: never used).
    static std::vector<host::Instance*> planInstances(const Engine& e) {
        std::vector<host::Instance*> out;
        for (const auto& plan : e.plans_)
            for (const auto& node : plan->nodes)
                detail::forEachStage(node, [&](const detail::Stage& st) {
                    if (st.instance) out.push_back(st.instance);
                });
        return out;
    }
    static std::uint64_t beginWrite(Engine& e) {
        const std::uint64_t seq = e.xportSeq_.load();
        e.xportSeq_.store(seq + 1);
        return seq;
    }
    static void loopAndEpoch(Engine& e, std::int64_t start, std::int64_t end) {
        e.loopStart_.store(start);
        e.loopEnd_.store(end);
        e.loopOn_.store(true);
        e.epoch_.fetch_add(1);
    }
    static void endWrite(Engine& e, std::uint64_t seq) { e.xportSeq_.store(seq + 2); }
    static std::uint64_t epoch(const Engine& e) { return e.epoch_.load(); }
    static std::uint64_t rtEpoch(const Engine& e) { return e.rtEpoch_; }
    static LoopRange rtLoop(const Engine& e) { return e.loopC_; }
    static std::uint64_t rendererEpoch(const Engine& e) { return e.epochDone_.load(); }
    static LoopRange rendererLoop(const Engine& e) { return e.loopR_; }  // after rendererEpoch
    static const detail::Plan& latestPlan(const Engine& e) { return *e.latestPlan_.load(); }
    // Every ring of every plan an executor may still use.
    // How far the renderer is ahead of what the callback played.
    static std::int64_t lead(const Engine& e) {
        return e.renderedStream_.load() - e.playedStream(e.epochDone_.load());
    }
    static std::int64_t aheadFrames(const Engine& e) { return e.aheadFrames_; }
    // The renderer waits for the callback to free room (a ring or its lead).
    static bool rendererWaitsForRoom(const Engine& e) {
        return e.rendererWakeAt_.load() != INT64_MAX;
    }
    static std::size_t pooledRings(const Engine& e) { return e.ringPool_.size(); }
    static std::uint64_t callbackGeneration(const Engine& e) { return e.callbackGen_.load(); }
    // Whether the plan the callback plays is still held (manual backends).
    static bool livePlanKept(const Engine& e) {
        return !e.livePlan_ || std::any_of(e.plans_.begin(), e.plans_.end(),
                                           [&](const auto& p) { return p.get() == e.livePlan_; });
    }
    // Whether every plan an executor may still be using (from the oldest
    // generation either has acknowledged) keeps all its rings.
    static bool ringsInUseKept(const Engine& e) {
        const std::uint64_t oldest = std::min(e.stoppedGen_.load(), e.callbackGen_.load());
        for (const auto& p : e.plans_)
            if (p->generation >= oldest)
                for (const auto* rings : {&p->frontierRings, &p->recordRings})
                    for (const auto& ring : *rings)
                        if (!ring) return false;
        return true;
    }
    // The frontier rings of the plan the callback plays (manual backends run
    // the callback on the test's thread).
    static std::vector<const detail::ChunkRing*> liveRings(const Engine& e) {
        std::vector<const detail::ChunkRing*> out;
        if (e.livePlan_)
            for (const auto& ring : e.livePlan_->frontierRings) out.push_back(ring.get());
        return out;
    }
    static std::vector<const detail::ChunkRing*> planRings(const Engine& e) {
        std::vector<const detail::ChunkRing*> out;
        for (const auto& plan : e.plans_) {
            for (const auto& ring : plan->frontierRings) out.push_back(ring.get());
            for (const auto& ring : plan->recordRings) out.push_back(ring.get());
        }
        return out;
    }
    static auto deadline(std::chrono::steady_clock::time_point now,
                         std::chrono::steady_clock::time_point next, bool idleWork,
                         std::chrono::steady_clock::duration safety = std::chrono::seconds(1)) {
        return Engine::rendererDeadline(now, next, idleWork, safety);
    }
    static auto stoppedDeadline(std::chrono::steady_clock::time_point next,
                                std::chrono::steady_clock::time_point last, bool requested) {
        return Engine::stoppedRenderDeadline(next, last, requested);
    }
    static auto safety(const Engine& e) { return e.rendererSafetyTimeout(); }
    static void playing(Engine& e, bool playing) { e.playing_ = playing; }
    static void rendererState(Engine& e, detail::Plan& plan, std::int64_t stream,
                              std::int64_t ahead) {
        e.plan_ = &plan;
        e.playing_ = true;
        e.streamLocal_ = stream;
        e.aheadFrames_ = ahead;
        e.ring_ = std::make_unique<detail::ChunkRing>(4, 64);
    }
    static detail::ChunkRing& ring(Engine& e) { return *e.ring_; }
    static std::int64_t target(const Engine& e, std::uint64_t epoch = 0) {
        return e.rendererSpaceTarget(epoch);
    }
    static bool room(const Engine& e, std::uint64_t epoch = 0) { return e.rendererHasRoom(epoch); }
    static void arm(Engine& e, std::int64_t target) { e.rendererWakeAt_.store(target); }
    static void consume(Engine& e, std::int64_t stream) SUPPLE_RT {
        e.rtStream_.store(stream);
        e.rtPosEpoch_.store(0);
        e.wakeRendererConsumed();
    }
    static auto event(const Engine& e) { return e.rendererEvent_.load(); }
    static detail::Recorder& recorder(Engine& e) { return *e.recorderOwned_; }
    // The latest plan's handover (a playrate plan's, prepared on the main
    // thread) and the returns to the arrangement the engine recorded.
    static std::int64_t latestHandAt(const Engine& e) { return e.latestPlan_.load()->handAt; }
    // An ack drained late, as the callback's can be behind the renderer's
    // (only between manual blocks: the callback is the ring's producer).
    static void drainedLate(Engine& e, const LaunchLogEntry& x) {
        Engine::LaunchAck a;
        a.track = x.track.n;
        a.clip = x.clip.n;
        a.epoch = x.epoch;
        a.action = static_cast<std::uint8_t>(x.action);
        a.stream = x.runningFrame;
        a.anchorSample = x.anchorSample;
        a.songFrame = x.songFrame;
        a.anchorBeat = x.anchorBeat;
        (void)e.callbackLaunchAcks_->push(a);
    }
    static std::vector<LaunchLogEntry> launchReturns(const Engine& e) {
        std::vector<LaunchLogEntry> out;
        for (const auto& r : e.launchReturns_) out.push_back(r.entry);
        return out;
    }
};
}  // namespace supple::engine
