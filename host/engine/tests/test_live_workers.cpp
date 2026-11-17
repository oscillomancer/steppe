// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The live pool (ADR-0024): exact-once dependencies and claim/close lifetime.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <semaphore>
#include <thread>
#include <vector>

#include "live_workers.h"
#include "supple/diagctx/table.h"

using namespace supple::engine::detail;

namespace {
// Registered threads of `role` in this process's execution table, by worker number.
std::vector<unsigned> registered(supple::diagctx::ThreadRole role) {
    std::vector<unsigned> numbers;
    for (const auto& slot : supple::diagctx::table().threads) {
        const auto id = slot.id.load(std::memory_order_acquire);
        if (id == 0 || id == supple::diagctx::kClaiming) continue;
        const auto info = supple::diagctx::slotInfo(slot.info.load(std::memory_order_relaxed));
        if (info.role == role) numbers.push_back(info.ordinal);
    }
    std::sort(numbers.begin(), numbers.end());
    return numbers;
}
// Threads register as they start, a moment after the constructor returns: wait on that
// state (no clock) until `count` have.
bool awaitRegistered(supple::diagctx::ThreadRole role, std::size_t count) {
    for (long spin = 0; spin < 200'000'000L; ++spin) {
        if (registered(role).size() == count) return true;
        std::this_thread::yield();
    }
    return false;
}
}  // namespace

namespace {

struct Run {
    std::vector<std::vector<std::uint32_t>> deps;
    std::unique_ptr<std::atomic<int>[]> done;  // times each task ran
    std::atomic<int> violations{0};
    int spin = 0;
};

void task(void* ctx, std::uint32_t t) SUPPLE_RT {
    auto* r = static_cast<Run*>(ctx);
    for (std::uint32_t d : r->deps[t])
        if (r->done[d].load(std::memory_order_acquire) != 1) r->violations.fetch_add(1);
    volatile int sink = 0;
    for (int i = 0; i < r->spin; ++i) sink = sink + i;  // overlap tasks a little
    r->done[t].fetch_add(1, std::memory_order_release);
}

}  // namespace

TEST_CASE("live pool: runs every task once, after its dependencies") {
    std::mt19937 rng(7);
    for (unsigned helpers : {0u, 1u, 3u, 7u}) {
        LiveWorkers workers(helpers);
        CHECK(workers.helpers() == helpers);
        for (int graph = 0; graph < 40; ++graph) {
            Run r;
            const std::uint32_t n = 1 + rng() % 60;
            r.deps.resize(n);
            for (std::uint32_t i = 1; i < n; ++i)
                for (std::uint32_t k = rng() % 4; k > 0; --k) r.deps[i].push_back(rng() % i);
            r.spin = static_cast<int>(rng() % 2000);
            TaskGraph g;
            g.build(r.deps);
            for (int rep = 0; rep < 5; ++rep) {  // a graph is reused block after block
                r.done = std::make_unique<std::atomic<int>[]>(n);
                workers.run(g, &task, &r);
                for (std::uint32_t i = 0; i < n; ++i) REQUIRE(r.done[i].load() == 1);
            }
            CHECK(r.violations.load() == 0);
        }
    }
}

TEST_CASE("live pool: an empty graph and a chain") {
    LiveWorkers workers(3);
    TaskGraph empty;
    empty.build({});
    workers.run(empty, &task, nullptr);  // returns at once
    Run r;
    r.deps = {{}, {0}, {1}, {2}, {3}};
    TaskGraph g;
    g.build(r.deps);
    r.done = std::make_unique<std::atomic<int>[]>(5);
    workers.run(g, &task, &r);
    CHECK(r.violations.load() == 0);
    for (int i = 0; i < 5; ++i) CHECK(r.done[i].load() == 1);
}

TEST_CASE("live pool: preempted claim and departure retain graph and context") {
    for (auto pause : {LiveWorkers::Event::Claimed, LiveWorkers::Event::Leave}) {
        struct Gate {
            LiveWorkers::Event pause;
            std::atomic<bool> claimed{false}, held{false}, joining{false};
            std::binary_semaphore entered{0}, resume{0};
        } gate{pause};
        LiveWorkers pool(7, {[](void* p, LiveWorkers::Event e) {
                                 auto& x = *static_cast<Gate*>(p);
                                 if (e == x.pause && !x.claimed.exchange(true)) {
                                     x.held = true;
                                     x.entered.release();
                                     x.resume.acquire();
                                 }
                                 // Force an actual claim before the caller can
                                 // complete this otherwise tiny graph.
                                 if (e == LiveWorkers::Event::Published)
                                     while (!x.held.load()) supple::rt::relax();
                                 if (e == LiveWorkers::Event::JoinWait) x.joining.store(true);
                             },
                             &gate});
        Run r;
        r.deps = {{}, {}, {0, 1}};
        r.done = std::make_unique<std::atomic<int>[]>(3);
        auto graph = std::make_unique<TaskGraph>();
        graph->build(r.deps);
        std::atomic<bool> returned{false};
        std::thread submit([&] {
            pool.run(*graph, task, &r);
            returned = true;
        });
        gate.entered.acquire();
        while (!gate.joining.load()) std::this_thread::yield();
        CHECK_FALSE(returned.load());
        gate.resume.release();
        submit.join();
        CHECK(returned.load());
        for (unsigned i = 0; i < 3; ++i) CHECK(r.done[i] == 1);
        graph.reset();
        for (unsigned width : {8u, 1u, 2u, 16u, 4u}) {
            Run next;
            next.deps.resize(width);
            next.done = std::make_unique<std::atomic<int>[]>(width);
            TaskGraph replacement;
            replacement.build(next.deps);
            pool.run(replacement, task, &next);
            for (unsigned i = 0; i < width; ++i) CHECK(next.done[i] == 1);
        }
    }
}

TEST_CASE("live pool: a helper delayed before claiming cannot retain a closed job") {
    struct Gate {
        std::atomic<bool> first{true};
        std::atomic<unsigned> enteredJobs{0}, missedJobs{0};
        std::binary_semaphore entered{0}, resume{0}, parked{0};
    } gate;
    LiveWorkers pool(1, {[](void* p, LiveWorkers::Event e) {
                             auto& x = *static_cast<Gate*>(p);
                             if (e == LiveWorkers::Event::Join && x.first.exchange(false)) {
                                 x.entered.release();
                                 x.resume.acquire();
                             }
                             if (e == LiveWorkers::Event::Entered) ++x.enteredJobs;
                             if (e == LiveWorkers::Event::Missed) ++x.missedJobs;
                             if (e == LiveWorkers::Event::Idle && !x.first.load())
                                 x.parked.release();
                         },
                         &gate});
    auto r = std::make_unique<Run>();
    r->deps = {{}, {}, {0, 1}};
    r->done = std::make_unique<std::atomic<int>[]>(3);
    auto graph = std::make_unique<TaskGraph>();
    graph->build(r->deps);
    std::thread submit([&] { pool.run(*graph, task, r.get()); });
    gate.entered.acquire();
    // No clock threshold: joining the caller before releasing the helper
    // proves it can return while that helper is preempted before its claim.
    submit.join();
    for (unsigned i = 0; i < 3; ++i) CHECK(r->done[i] == 1);
    graph.reset();
    r.reset();
    gate.resume.release();
    gate.parked.acquire();  // late claim/check/decrement has finished
    CHECK(gate.enteredJobs == 0);
    CHECK(gate.missedJobs == 1);
}

TEST_CASE("live pool: an old wake may claim the next open job") {
    struct Gate {
        std::atomic<bool> first{true}, nextPublished{false}, nextEntered{false};
        unsigned publications = 0;  // caller only
        std::binary_semaphore entered{0}, resume{0};
    } gate;
    LiveWorkers pool(1, {[](void* p, LiveWorkers::Event e) {
                             auto& x = *static_cast<Gate*>(p);
                             if (e == LiveWorkers::Event::Join && x.first.exchange(false)) {
                                 x.entered.release();
                                 x.resume.acquire();
                             }
                             if (e == LiveWorkers::Event::Entered) x.nextEntered = true;
                             if (e == LiveWorkers::Event::Published && ++x.publications == 2) {
                                 x.nextPublished = true;
                                 while (!x.nextEntered.load()) supple::rt::relax();
                             }
                         },
                         &gate});
    {
        TaskGraph graph;
        graph.build({{}, {}});
        pool.run(graph, [](void*, std::uint32_t) {}, nullptr);
        gate.entered.acquire();
    }  // retire the first graph before permitting its wake to claim anything
    Run next;
    next.deps = {{}, {}, {0, 1}};
    next.done = std::make_unique<std::atomic<int>[]>(3);
    TaskGraph replacement;
    replacement.build(next.deps);
    std::thread submit([&] { pool.run(replacement, task, &next); });
    // Wait for publication, not elapsed time, before releasing the old wake.
    // The callback's Published hook waits for the helper to enter this job.
    while (!gate.nextPublished.load()) std::this_thread::yield();
    gate.resume.release();
    submit.join();
    CHECK(gate.nextEntered.load());
    CHECK(next.violations == 0);
    for (unsigned i = 0; i < 3; ++i) CHECK(next.done[i] == 1);
}

TEST_CASE("live pool: publication before parking is not lost; chains wake nobody") {
    struct Gate {
        std::atomic<bool> first{true};
        std::atomic<unsigned> joins{0}, calls{0};
        std::binary_semaphore entered{0}, resume{0}, parked{0};
    } gate;
    LiveWorkers pool(1, {[](void* p, LiveWorkers::Event e) {
                             auto& x = *static_cast<Gate*>(p);
                             if (e == LiveWorkers::Event::Idle && x.first.exchange(false)) {
                                 x.entered.release();
                                 x.resume.acquire();
                             }
                             if (e == LiveWorkers::Event::Join) ++x.joins;
                             if (e == LiveWorkers::Event::Idle && x.joins.load() != 0)
                                 x.parked.release();
                         },
                         &gate});
    gate.entered.acquire();
    TaskGraph chain;
    chain.build({{}, {0}, {1}});
    auto count = [](void* p, std::uint32_t) { ++static_cast<Gate*>(p)->calls; };
    pool.run(chain, count, &gate);
    CHECK(gate.calls == 3);
    CHECK(gate.joins == 0);
    TaskGraph parallel;
    parallel.build({{}, {}});
    std::thread submit([&] { pool.run(parallel, count, &gate); });
    submit.join();
    // Publication already happened; the helper hasn't entered futex wait yet.
    gate.resume.release();
    gate.parked.acquire();
    CHECK(gate.joins == 1);
    CHECK(gate.calls == 5);
}

TEST_CASE("live pool: a delayed node publishes state once before its dependent") {
    struct State {
        std::atomic<bool> rootEntered{false}, helperJoined{false}, resume{false};
        int rootState = 0, dependentState = 0;
        std::atomic<unsigned> roots{0};
    } state;
    LiveWorkers pool(1, {[](void* p, LiveWorkers::Event e) {
                             if (e == LiveWorkers::Event::Entered)
                                 static_cast<State*>(p)->helperJoined = true;
                         },
                         &state});
    TaskGraph graph;
    graph.build({{}, {}, {0, 1}});
    std::atomic<bool> returned{false};
    std::thread submit([&] {
        pool.run(
            graph,
            [](void* p, std::uint32_t taskIndex) SUPPLE_RT {
                auto& s = *static_cast<State*>(p);
                if (taskIndex == 0) {
                    s.rootEntered = true;
                    while (!s.resume.load()) supple::rt::relax();
                    s.rootState = 123;
                }
                if (taskIndex < 2)
                    ++s.roots;
                else
                    s.dependentState = s.rootState + static_cast<int>(s.roots.load());
            },
            &state);
        returned = true;
    });
    while (!state.rootEntered.load() || !state.helperJoined.load()) std::this_thread::yield();
    CHECK_FALSE(returned.load());
    state.resume = true;
    submit.join();
    CHECK(state.roots == 2);
    CHECK(state.dependentState == 125);
}

TEST_CASE("live pool: each helper registers as a numbered audio thread and releases its slot") {
    using supple::diagctx::ThreadRole;
    constexpr auto role = ThreadRole::AudioLive;
    CHECK(registered(role).empty());
    {
        LiveWorkers workers(3);
        REQUIRE(awaitRegistered(role, 3));
        CHECK(registered(role) == std::vector<unsigned>{1, 2, 3});
    }
    CHECK(registered(role).empty());  // the pool joined its threads, so their slots are free
}
