// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A rack's Notes module following its source track's launches (ADR-0097,
// 2026-11-20), offline and live in both executors. Split from
// test_clip_player.cpp so each runs as its own ctest entry.
#include "clip_player_support.h"

using namespace supple;
using namespace supple::engine;
using namespace clip_player_test;

TEST_CASE("a rack's Notes module follows its source track's launches, offline") {
    Project p;
    rackFollows(p, false);
    const std::int64_t bar = 4 * kBeat;
    const std::vector<LaunchLogEntry> log{launch(bar, "cl_1"),
                                          back(2 * bar, LaunchLogEntry::Action::Stop),
                                          back(3 * bar, LaunchLogEntry::Action::Arrangement)};
    const auto g = gates(p, 8.0, log);
    const auto at = [&](double seconds) { return g[std::size_t(seconds * 48000)]; };
    CHECK(at(1.0) == 1.0f);   // the arrangement's G3
    CHECK(at(2.1) == 1.0f);   // the clip's first eighth note
    CHECK(at(2.35) == 0.0f);  // its rest: the arrangement's A3 passes unplayed
    CHECK(at(2.6) == 1.0f);   // its second eighth
    CHECK(at(3.5) == 0.0f);
    CHECK(at(4.5) == 0.0f);  // stopped: B3 passes unplayed
    CHECK(at(6.5) == 1.0f);  // the arrangement again, from C4 at the switch
    // Without the launches the rack plays the arrangement throughout.
    const auto plain = gates(p, 8.0, {});
    CHECK(plain[std::size_t(2.35 * 48000)] == 1.0f);
    CHECK(plain[std::size_t(4.5 * 48000)] == 1.0f);
}

TEST_CASE(
    "a rack's Notes module follows launches live as offline, in both executors, also "
    "with the rack and its source track on different executors" *
    doctest::description("rt")) {
    for (const bool device : {false, true}) {
        Project p;
        rackFollows(p, device);
        const model::Session s = p.app->head();
        {
            // With the device the rack is live and tr_1 is not: in the
            // anticipative engine their launches switch on two executors.
            InstancePool pool = makePool();
            auto plan = detail::compile(s, pool, 0, 128);
            REQUIRE_MESSAGE(plan, (plan ? std::string() : plan.error().toString()));
            for (const detail::Node& n : plan->nodes) {
                if (n.rack) CHECK(n.live == device);
                if (n.id == id("tr_1")) CHECK_FALSE(n.live);
            }
        }
        const std::int64_t frames = 48000 * 7 + 8192;
        // Without the launches the rack plays the arrangement: one render.
        const auto plain = gates(p, 7.0, {});
        std::vector<std::tuple<Loop, std::vector<LaunchLogEntry>, std::vector<float>>> replays;
        for (const Loop& loop : {Loop{}, kWrapBeforeAnchor})
            for (const std::uint32_t block : {128u, 512u})
                for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
                    // The wrap at one block size: the horizons' block rule is
                    // the unlooped run's.
                    if (loop && block != 128) continue;
                    INFO("looped " << bool(loop) << ", device " << device << ", block " << block
                                   << ", anticipative " << options.anticipative);
                    Engine::LaunchRequest stop = backTo("tr_1");
                    stop.action = LaunchLogEntry::Action::Stop;
                    const std::vector<Step> steps{{20, {request("tr_1", "cl_1")}},
                                                  {(96000 + 4000) / block, {stop}},
                                                  {(192000 + 4000) / block, {backTo("tr_1")}}};
                    const Live live =
                        playLive(s, p.app->projectDir(), options, block, frames, steps, loop);
                    REQUIRE(live.log.size() == 3);
                    // Looped, each later launch wraps first and lands on bar 2.
                    CHECK(live.log[0].anchorSample == 96000);
                    CHECK(live.log[1].anchorSample == (loop ? 96000 : 192000));
                    CHECK(live.log[2].anchorSample == (loop ? 96000 : 288000));
                    CHECK(live.log[2].runningFrame == 288000);
                    // A render is a pure function of the session, the log and
                    // the loop: one log, one replay (the runs mostly agree).
                    auto found = std::find_if(replays.begin(), replays.end(), [&](const auto& r) {
                        return std::get<0>(r) == loop && std::get<1>(r) == live.log;
                    });
                    if (found == replays.end()) {
                        InstancePool pool = makePool();
                        OfflineSettings settings;
                        settings.endSeconds = 7.0;
                        settings.projectDir = p.app->projectDir();
                        settings.launches = live.log;
                        settings.loop = loop;
                        auto offline = renderOffline(s, pool, settings);
                        REQUIRE(offline);
                        // The rack's switches changed the mix: equality says something.
                        CHECK(gates(p, 7.0, live.log) != plain);
                        found = replays.insert(replays.end(),
                                               {loop, live.log, offline->audio.channels[0]});
                    }
                    CHECK(maxDiff(live.left, live.latency, std::get<2>(*found), 48000 * 7) == 0.0);
                }
    }
}

TEST_CASE("a launch missed before it is decided lands on neither the track nor its rack" *
          doctest::description("rt")) {
    // The main thread stalls between publishing a launch and deciding it,
    // while the executors play past its bar. The executor that reaches a
    // frame of it undecided marks it missed: nothing switches there. Placed
    // again at once by launch() (ADR-0097, revision 2026-11-21), it
    // switches the track and the rack together on the next bar.
    for (const bool device : {false, true}) {
        Project p;
        rackFollows(p, device);
        const model::Session s = p.app->head();
        constexpr std::uint32_t kBlock = 128;
        const std::int64_t frames = 48000 * 7 + 8192;
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("device " << device << ", anticipative " << options.anticipative);
            InstancePool pool = makePool();
            MediaCache media;
            DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = frames});
            Engine engine(pool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = kBlock}));
            REQUIRE(engine.load(s, p.app->projectDir()));
            engine.play();
            struct Stall {
                Engine* engine;
                DummyBackend* backend;
                bool anticipative;
                std::int64_t cycles;
                bool done = false;  // the first attempt stalls, not the second
            } stall{&engine, &backend, options.anticipative, (96000 + 8192) / kBlock - 20};
            const auto run = [](Stall& st, std::int64_t cycles) {
                if (st.anticipative)
                    runAheadOnState(*st.engine, *st.backend, cycles, kBlock);
                else
                    st.backend->run(cycles);
            };
            run(stall, 20);
            launchCommitHookForTests(
                engine,
                [](void* context) {
                    auto& st = *static_cast<Stall*>(context);
                    if (st.done) return;
                    st.done = true;
                    if (st.anticipative)
                        runAheadOnState(*st.engine, *st.backend, st.cycles, kBlock);
                    else
                        st.backend->run(st.cycles);
                },
                &stall);
            const Engine::LaunchRequest r = request("tr_1", "cl_1");
            auto placed = engine.launch(std::span(&r, 1), model::LaunchQuantize::Bar);
            launchCommitHookForTests(engine, nullptr, nullptr);
            REQUIRE(placed);
            CHECK(placed->anchorSample == 192000);  // missed on bar 2: placed again, on bar 3
            engine.service();
            const auto pending = engine.launchesPending();
            REQUIRE(pending.size() == 1);
            CHECK(pending[0].placed.anchorSample == 192000);
            run(stall, frames / kBlock - 20 - stall.cycles);
            engine.service();
            engine.stop();
            CHECK(engine.processErrors() == 0);
            CHECK(engine.anticipationMisses() == 0);
            const auto log = engine.launchLog();
            REQUIRE(log.size() == 1);  // applied once, on bar 3
            CHECK(log[0].anchorSample == 192000);
            OfflineSettings settings;
            settings.endSeconds = 7.0;
            settings.projectDir = p.app->projectDir();
            settings.launches = log;
            InstancePool offlinePool = makePool();
            auto offline = renderOffline(s, offlinePool, settings);
            REQUIRE(offline);
            CHECK(maxDiff(backend.capture().channels[0], engine.outputLatency(),
                          offline->audio.channels[0], 48000 * 7) == 0.0);
        }
    }
}
